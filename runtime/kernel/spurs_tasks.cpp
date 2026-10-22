// Native replacements for SPU tasks of the game whose behaviour is known, identified by
// a hash of their SPU ELF.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/inflate.h"
#include "core/log.h"
#include "kernel/spurs.h"

namespace sly4 {

namespace {

// Size of an SPU ELF image in guest memory: the end of its section or program headers.
uint32_t elf_size(const GuestMemory& m, uint32_t elf) {
    if (m.read<uint32_t>(elf) != 0x7F454C46) return 0;
    const uint32_t phoff = m.read<uint32_t>(elf + 0x1C), shoff = m.read<uint32_t>(elf + 0x20);
    const uint32_t phnum = m.read<uint16_t>(elf + 0x2C), shnum = m.read<uint16_t>(elf + 0x30);
    uint32_t end = std::max(phoff + phnum * 32, shoff + shnum * 40);
    for (uint32_t i = 0; i < phnum; ++i) {
        const uint32_t ph = elf + phoff + i * 32;
        end = std::max(end, m.read<uint32_t>(ph + 4) + m.read<uint32_t>(ph + 16));  // p_offset + p_filesz
    }
    return end;
}

uint64_t fnv1a(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// ---------------------------------------------------------------- EDGE zlib inflate task
// edgezlib_inflate_task (Sony EDGE 1.2.3, zlib for SPU). Its argument word 0 is the
// LFQueue of requests; each 32-byte element is
//   +0  src (raw DEFLATE, zlib header already skipped)   +4  dst
//   +8  compressed size                                   +12 bytes to deliver
//   +16 work-to-do counter | flags                        +20 CellSpursEventFlag*
//   +24 event flag bits  +26 skip at the start  +28 skip at the end   (u16 each)
// The stream inflates to skip_begin + deliver + skip_end bytes; only the window
// [skip_begin, skip_begin + deliver) goes to dst. With flag bit 0 the task then
// decrements the counter (otherwise it stores the delivered size there), and finally
// sets the event flag bits.
constexpr uint64_t kEdgeZlibInflateTask = 0xB28A89DF3EB9235Cull;

void decrement_be32(GuestMemory& m, uint32_t ea) {
    auto* p = reinterpret_cast<uint32_t*>(m.host(ea));
    uint32_t old = __atomic_load_n(p, __ATOMIC_SEQ_CST), next;
    do {
        const uint32_t v = __builtin_bswap32(old);
        if (v == 0) {
            SLY4_ERROR("EDGE zlib: contador de trabalho em 0x%08X já está em zero", ea);
            return;
        }
        next = __builtin_bswap32(v - 1);
    } while (!__atomic_compare_exchange_n(p, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
}

void edge_zlib_worker(GuestMemory* mp, std::shared_ptr<SpursQueue> queue) {
    GuestMemory& m = *mp;
    std::vector<uint8_t> e, block;
    uint64_t done = 0;
    for (;;) {
        queue->pop(e, true);
        auto be32 = [&](int o) { return uint32_t(e[o]) << 24 | e[o + 1] << 16 | e[o + 2] << 8 | e[o + 3]; };
        auto be16 = [&](int o) { return uint32_t(e[o]) << 8 | e[o + 1]; };
        const uint32_t src = be32(0), dst = be32(4), src_size = be32(8), deliver = be32(12);
        const uint32_t counter = be32(16), flag = be32(20), bits = be16(24);
        const uint32_t skip_begin = be16(26), skip_end = be16(28);
        const size_t expected = size_t(skip_begin) + deliver + skip_end;
        block.resize(expected);
        size_t out = 0, used = 0;
        const InflateResult r = inflate_raw(m.host(src), src_size, block.data(), expected, out, used);
        if (r != InflateResult::Ok || out != expected) {
            SLY4_ERROR("EDGE zlib: 0x%08X (+%u) -> %zu de %zu bytes esperados: %s", src, src_size, out, expected,
                       inflate_result_name(r));
        }
        m.copy_to(dst, block.data() + std::min<size_t>(skip_begin, out),
                  std::min<size_t>(deliver, out > skip_begin ? out - skip_begin : 0));
        if (counter & 1)
            decrement_be32(m, counter & ~3u);
        else if (counter & ~3u)
            m.write<uint32_t>(counter & ~3u, deliver);
        if (flag)
            if (auto f = spurs_event_flag(flag)) f->set(uint16_t(bits));
        if (++done == 1 || done % 1000 == 0)
            SLY4_INFO("EDGE zlib: %" PRIu64 " blocos descomprimidos (último: %u -> %zu bytes)", done, src_size, out);
    }
}

}  // namespace

const char* start_native_spurs_task(GuestMemory& m, const SpursTaskInfo& task) {
    const uint32_t size = elf_size(m, task.elf);
    const uint64_t hash = size ? fnv1a(m.host(task.elf), size) : 0;
    if (hash == kEdgeZlibInflateTask) {
        auto queue = spurs_queue(task.arg[0]);
        if (!queue) return nullptr;
        std::thread(edge_zlib_worker, &m, queue).detach();
        return "EDGE zlib inflate, nativo";
    }
    SLY4_INFO("SPURS: ELF SPU 0x%08X desconhecido (%u bytes, hash %016" PRIX64 ")", task.elf, size, hash);
    return nullptr;
}

}  // namespace sly4
