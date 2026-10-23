// LV2 SPU threads (kernel/spu_thread.h): a small SPU program talks to the PPU side
// through the mailboxes and leaves with a stop signal; lock-line reservations report
// when another writer changes the line.
#include "check.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "core/endian.h"
#include "cpu/guest_atomic.h"
#include "cpu/ppu_rt.h"
#include "cpu/spu_blocks.h"
#include "cpu/spu_channels.h"
#include "cpu/spu_interpreter.h"
#include "kernel/spu_thread.h"
#include "memory/guest_memory.h"

using namespace sly4;

namespace {

// SPU encodings: 11-bit opcode prefix (recomp/spu/spu_ops.inc) and the RR fields.
constexpr uint32_t rdch(int rt, int ch) { return 0x00Du << 21 | uint32_t(ch) << 7 | uint32_t(rt); }
constexpr uint32_t wrch(int ch, int rt) { return 0x10Du << 21 | uint32_t(ch) << 7 | uint32_t(rt); }
constexpr uint32_t stop(uint32_t code) { return code & 0x3FFF; }

std::shared_ptr<SpuThread> make_thread(GuestMemory& m, SpuThreadGroup& g) {
    auto t = std::make_shared<SpuThread>();
    t->group = &g;
    t->ctx.ls = t->ls->data;
    t->ctx.mem = &m;
    t->ctx.io = t.get();
    g.threads.push_back(t);
    return t;
}

void gen_a(PPUContext&, GuestMemory&) {}
void gen_b(PPUContext&, GuestMemory&) {}

}  // namespace

TEST(spu_thread_mailboxes_and_exit) {
    GuestMemory m;
    CHECK(m.reserve());
    SpuThreadGroup g;
    auto t = make_thread(m, g);
    // Reads the inbound mailbox, sends it back through the outbound one, then leaves:
    // sys_spu_thread_exit takes its status from the outbound mailbox.
    const uint32_t prog[] = {rdch(4, spu_ch::RdInMbox), wrch(spu_ch::WrOutMbox, 4), stop(spu_stop::ThreadExit)};
    for (uint32_t i = 0; i < 3; ++i) store_be<uint32_t>(t->ls->data + 4 * i, prog[i]);
    g.started = true;
    g.running = 1;
    std::thread runner([t] { t->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the SPU waits on the mailbox
    CHECK(t->running.load());
    t->push_in_mbox(0xC0FFEE);
    runner.join();
    CHECK_EQ(t->exit_status, 0xC0FFEEu);
    CHECK(!g.started);
    CHECK_EQ(g.join_cause, 2u);  // SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT
    CHECK(t->executed.load() >= 3u);
}

TEST(spu_thread_lock_line_lost_event) {
    GuestMemory m;
    CHECK(m.reserve());
    CHECK(m.commit(0x10000000, 0x10000, Protection::ReadWrite));
    SpuThreadGroup g;
    auto t = make_thread(m, g);
    SPUContext& c = t->ctx;
    // getllar of the line at 0x10000080 into local store 0x100.
    c.mfc.lsa = 0x100;
    c.mfc.eal = 0x10000080;
    CHECK(spu_mfc_command(c, 0xD0) == SpuChannelResult::Ok);
    CHECK(c.reserved);
    c.event_mask = spu_event::LlrLost;
    CHECK_EQ(spu_channel_count(c, spu_ch::RdEventStat), 0u);  // nobody wrote the line yet
    m.write<uint32_t>(0x100000A0, 1);                           // another writer
    CHECK_EQ(spu_channel_count(c, spu_ch::RdEventStat), 1u);
    uint32_t v = 0;
    CHECK(spu_read_channel(c, spu_ch::RdEventStat, v) == SpuChannelResult::Ok);
    CHECK_EQ(v, spu_event::LlrLost);
    CHECK(!c.reserved);
    // putllc of a line whose reservation was lost fails.
    CHECK(spu_mfc_command(c, 0xB4) == SpuChannelResult::Ok);
    CHECK_EQ(c.atomic_stat, 1u);
}

// A putllc writes only the bytes the SPU changed: a plain store to another part of the
// line, landing between the compare and the write, survives (Havok's PPU lwmutex owner,
// in the same line as the cellSyncMutex its SPU tasks update).
TEST(spu_putllc_keeps_other_stores) {
    alignas(128) uint8_t memory[128] = {}, old[128] = {}, now[128] = {};
    store_be<uint32_t>(memory + 0x30, 0xFFFFFFFF);
    std::memcpy(old, memory, 128);
    std::memcpy(now, old, 128);
    store_be<uint16_t>(now + 0x22, 1);               // the SPU: acq++
    store_be<uint32_t>(memory + 0x30, 0x01000000);  // meanwhile the PPU: owner = main_thread
    guest_store_line_changes(memory, old, now);
    CHECK_EQ(load_be<uint16_t>(memory + 0x22), 1u);
    CHECK_EQ(load_be<uint32_t>(memory + 0x30), 0x01000000u);
    // Bytes of a changed word that the SPU left alone are kept too.
    store_be<uint16_t>(memory + 0x20, 7);
    std::memcpy(old, now, 128);
    store_be<uint16_t>(now + 0x22, 2);
    guest_store_line_changes(memory, old, now);
    CHECK_EQ(load_be<uint32_t>(memory + 0x20), 0x00070002u);
}

// A compiled block found to match is trusted until its local-store page is written: by an
// SPU store or a DMA get (epochs), after which it is compared again.
SpuStep test_block_fn(SPUContext&, uint64_t&) { return SpuStep::Ok; }

TEST(spu_block_cache_follows_local_store_writes) {
    static const uint8_t code[8] = {0x40, 0x80, 0x00, 0x03, 0x40, 0x80, 0x00, 0x84};  // il $3,0; il $4,1
    static const SpuBlockEntry entry{0x3F000, 8, code, test_block_fn};
    spu_register_blocks(&entry, 1);
    auto ls = std::make_unique<SpuLocalStore>();
    SPUContext c;
    c.ls = ls->data;
    std::memcpy(ls->data + 0x3F000, code, 8);
    c.ls_rewritten();
    c.pc = 0x3F000;
    CHECK(spu_find_block(c) == test_block_fn);
    CHECK(spu_find_block(c) == test_block_fn);  // from the cache
    SpuReg r{};  // an SPU store over the code: the block no longer matches
    c.write_quad(0x3F000, r);
    CHECK(spu_find_block(c) == nullptr);
    std::memcpy(ls->data + 0x3F000, code, 8);  // back, as a DMA get would put it
    c.ls_written(0x3F000, 8);
    CHECK(spu_find_block(c) == test_block_fn);
}

// Firmware modules register their own generated function table next to the game's.
TEST(function_tables_merge) {
    const rt::FnEntry b[] = {{0xFFF00010, gen_b}};
    const rt::FnEntry a[] = {{0xFFF00000, gen_a}, {0xFFF00020, gen_a}};
    rt::set_function_table(b, 1);
    rt::set_function_table(a, 2);
    CHECK(rt::find_function(0xFFF00000) == gen_a);
    CHECK(rt::find_function(0xFFF00010) == gen_b);
    CHECK(rt::find_function(0xFFF00020) == gen_a);
    CHECK(rt::find_function(0xFFF00008) == nullptr);
}

// A DMA list element with the stall-and-notify flag stops the list after its transfer
// and raises the list stall event; the acknowledgement lets the list go on.
TEST(spu_dma_list_stall_and_notify) {
    GuestMemory m;
    CHECK(m.reserve());
    CHECK(m.commit(0x10000000, 0x10000, Protection::ReadWrite));
    m.write<uint32_t>(0x10000000, 0x11111111);
    m.write<uint32_t>(0x10000100, 0x22222222);
    auto ls = std::make_unique<SpuLocalStore>();
    SPUContext c{};
    c.ls = ls->data;
    c.mem = &m;
    // List at LS 0x200: {stall | 16 bytes, 0x10000000}, {16 bytes, 0x10000100}.
    store_be<uint32_t>(ls->data + 0x200, 0x80000010);
    store_be<uint32_t>(ls->data + 0x204, 0x10000000);
    store_be<uint32_t>(ls->data + 0x208, 0x00000010);
    store_be<uint32_t>(ls->data + 0x20C, 0x10000100);
    c.mfc = {0x1000, 0, 0x200, 16, 5};  // lsa, eah, list address, list size, tag 5
    CHECK(spu_mfc_command(c, 0x44) == SpuChannelResult::Ok);  // getl
    CHECK_EQ(c.ls_u32(0x1000), 0x11111111u);
    CHECK_EQ(c.ls_u32(0x1010), 0u);  // not yet
    CHECK_EQ(c.event_stat & spu_event::ListStall, spu_event::ListStall);
    CHECK_EQ(spu_channel_count(c, spu_ch::MFC_RdListStallStat), 1u);
    uint32_t v = 0;
    CHECK(spu_read_channel(c, spu_ch::MFC_RdListStallStat, v) == SpuChannelResult::Ok);
    CHECK_EQ(v, 1u << 5);
    CHECK(spu_write_channel(c, spu_ch::MFC_WrListStallAck, 5) == SpuChannelResult::Ok);
    CHECK_EQ(c.ls_u32(0x1010), 0x22222222u);
    CHECK(!c.stalled[5].active);
}

// With interrupts enabled (bie) and an event in the mask, the SPU branches to 0 with
// the interrupted PC in SRR0 and interrupts off; iret with E returns and enables them.
TEST(spu_interrupts) {
    auto ls = std::make_unique<SpuLocalStore>();
    SPUContext c{};
    c.ls = ls->data;
    c.pc = 0x300;
    c.event_mask = spu_event::ListStall;
    CHECK(!spu_check_interrupt(c));  // interrupts are off
    c.irq = true;
    CHECK(!spu_check_interrupt(c));  // nothing pending
    c.event_stat = spu_event::ListStall;
    CHECK(spu_check_interrupt(c));
    CHECK_EQ(c.pc, 0u);
    CHECK_EQ(c.srr0, 0x300u);
    CHECK(!c.irq);
    // irete $0 (iret with the E flag) at 0: back to 0x300 with interrupts on.
    store_be<uint32_t>(ls->data, 0x35440000);
    CHECK(spu_step(c) == SpuStep::Branch);
    CHECK_EQ(c.pc, 0x300u);
    CHECK(c.irq);
}
