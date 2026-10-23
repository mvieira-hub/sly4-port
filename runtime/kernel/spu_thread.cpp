#include "kernel/spu_thread.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <map>
#include <cstring>
#include <thread>

#include "core/host_sleep.h"
#include "core/log.h"
#include "cpu/guest_atomic.h"
#include "cpu/spu_blocks.h"
#include "kernel/kernel.h"
#include "loader/spu_elf.h"

namespace sly4 {

namespace {

constexpr uint64_t kSpuThreadEventUserKey = 0xFFFFFFFF53505501ull;   // SYS_SPU_THREAD_EVENT_USER_KEY
constexpr uint64_t kSpuGroupEventRunKey = 0xFFFFFFFF53505500ull;     // SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY
constexpr uint32_t kJoinGroupExit = 1, kJoinAllThreadsExit = 2;

// Bumped on every atomic write to a lock line (guest_line_written): SPUs waiting for a lost
// reservation look again.
std::atomic<uint32_t> g_line_epoch{0};
constexpr size_t kMaxSpuThreads = 64;
std::atomic<SpuThread*> g_waiters[kMaxSpuThreads];
std::atomic<size_t> g_waiter_count{0};

// Every reservation on the line is lost (as on the Cell, where any write kills it, even
// one that leaves the same bytes), and every SPU waiting for an event looks again.
void wake_spus(GuestAddr line) {
    g_line_epoch.fetch_add(1);
    const size_t n = g_waiter_count.load();
    for (size_t i = 0; i < n; ++i)
        if (SpuThread* t = g_waiters[i].load()) {
            if (t->ctx.reserved && t->ctx.reserve_ea == line) __atomic_store_n(&t->ctx.llr_lost, 1u, __ATOMIC_RELEASE);
            t->cv.notify_all();
        }
}

}  // namespace

// ---------------------------------------------------------------- PPU -> SPU
void SpuThread::push_in_mbox(uint32_t v) {
    {
        std::lock_guard<std::mutex> lk(m);
        if (in_mbox.size() >= 4) in_mbox.pop_front();
        in_mbox.push_back(v);
    }
    cv.notify_all();
}

void SpuThread::write_snr(int n, uint32_t v) {
    {
        std::lock_guard<std::mutex> lk(m);
        snr[n] = snr_or[n] && snr_valid[n] ? snr[n] | v : v;
        snr_valid[n] = true;
    }
    cv.notify_all();
}

bool SpuThread::pop_out_mbox(uint32_t& v) {
    std::lock_guard<std::mutex> lk(m);
    if (out_mbox.empty()) return false;
    v = out_mbox.front();
    out_mbox.pop_front();
    return true;
}

// Event conditions (with m held): inbound mailbox, signals, and a lost lock-line
// reservation (another writer changed the 128 bytes a getllar reserved; SPURS kernels
// sleep on this), which is found by comparing the line.
uint32_t SpuThread::pending_events() {
    uint32_t ev = 0;
    if (!in_mbox.empty()) ev |= spu_event::InMbox;
    if (snr_valid[0]) ev |= spu_event::Sn1;
    if (snr_valid[1]) ev |= spu_event::Sn2;
    if (ctx.reserved && (__atomic_exchange_n(&ctx.llr_lost, 0u, __ATOMIC_ACQ_REL) ||
                         std::memcmp(ctx.mem->host(ctx.reserve_ea), ctx.reserve_data, 128) != 0)) {
        ctx.reserved = false;
        ev |= spu_event::LlrLost;
    }
    return ev;
}

// ---------------------------------------------------------------- SPU channels
// Channel reads that waited: how many were woken up by a notification and how many only
// found their event after the 1 ms timeout (a write nobody announced), for the summary.
bool g_spu_timeline = false;
double steady_ms_now() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void spu_timeline(bool on) { g_spu_timeline = on; }
std::atomic<uint64_t> g_waits_woken{0}, g_waits_late{0}, g_late_events[3]{};  // [LLR, decrementer, other]

SpuChannelResult SpuThread::read(SPUContext& c, uint32_t ch, uint32_t& value) {
    std::unique_lock<std::mutex> lk(m);
    bool waited = false, late = false;
    auto found = [&](uint32_t ev) {
        if (!waited) return;
        if (g_spu_timeline && idle_since) {
            const double now = steady_ms_now();
            if (now - idle_since > 1.0)
                SLY4_INFO("%10.3f ms SPU%u: acordou após %.2f ms (pc 0x%05X, evento 0x%X)", now, id & 0xFF, now - idle_since,
                          c.pc, ev);
        }
        idle_since = 0;
        (late ? g_waits_late : g_waits_woken).fetch_add(1, std::memory_order_relaxed);
        if (late) g_late_events[(ev & spu_event::LlrLost) ? 0 : (ev & 0x20) ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
    };
    // Gives up after ~10 ms with Wait: the rdch is retried, and the run loop sees stops.
    for (int tries = 0; tries < 10; ++tries) {
        const uint32_t seen = g_line_epoch.load();
        if (stop_requested) return SpuChannelResult::Wait;
        switch (ch) {
            case spu_ch::RdInMbox:
                if (!in_mbox.empty()) {
                    found(spu_event::InMbox);
                    value = in_mbox.front();
                    in_mbox.pop_front();
                    return SpuChannelResult::Ok;
                }
                break;
            case spu_ch::RdSigNotify1:
            case spu_ch::RdSigNotify2: {
                const int n = ch == spu_ch::RdSigNotify1 ? 0 : 1;
                if (snr_valid[n]) {
                    found(0);
                    value = snr[n];
                    snr[n] = 0;
                    snr_valid[n] = false;
                    return SpuChannelResult::Ok;
                }
                break;
            }
            case spu_ch::RdEventStat:
                spu_check_decrementer(c);
                c.event_stat |= pending_events();
                if (c.event_stat & c.event_mask) {
                    found(c.event_stat & c.event_mask);
                    value = c.event_stat & c.event_mask;
                    return SpuChannelResult::Ok;
                }
                break;
            default:
                SLY4_WARN("SPU %08X: leitura do canal %u sem implementação (pc 0x%05X)", id, ch, c.pc);
                return SpuChannelResult::Invalid;
        }
        // Atomic writes to any line wake the SPUs (wake_spus); a line written while we looked
        // is looked at again at once.
        if (g_line_epoch.load() != seen) continue;
        waited = true;
        if (g_spu_timeline && !idle_since) idle_since = steady_ms_now();
        const auto w0 = std::chrono::steady_clock::now();
        late = cv.wait_for(lk, std::chrono::milliseconds(1)) == std::cv_status::timeout;
        waited_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count());
    }
    return SpuChannelResult::Wait;
}

SpuChannelResult SpuThread::write(SPUContext& c, uint32_t ch, uint32_t value) {
    switch (ch) {
        case spu_ch::WrOutMbox: {
            std::lock_guard<std::mutex> lk(m);
            out_mbox.clear();
            out_mbox.push_back(value);
            return SpuChannelResult::Ok;
        }
        case spu_ch::WrOutIntrMbox: {
            const uint32_t code = value >> 24;
            uint32_t data = 0;
            pop_out_mbox(data);
            if (code < 128) {  // sys_spu_thread_send_event (< 64) / throw_event
                const uint32_t spup = code & 63;
                auto q = ports[spup].lock();
                uint32_t r = CELL_ENOTCONN;
                if (q) r = q->send({kSpuThreadEventUserKey, id, (uint64_t(spup) << 32) | (value & 0xFFFFFF), data});
                if (code < 64) push_in_mbox(r);
                return SpuChannelResult::Ok;
            }
            if (code == 128 || code == 192) {  // sys_event_flag_set_bit (192: without a result)
                auto f = idm().get<EventFlag>(data);
                if (f) f->set(1ull << (value & 0xFFFFFF));
                if (code == 128) push_in_mbox(f ? CELL_OK : CELL_ESRCH);
                return SpuChannelResult::Ok;
            }
            SLY4_WARN("SPU %08X: SPU_WrOutIntrMbox desconhecido 0x%08X", id, value);
            return SpuChannelResult::Invalid;
        }
        default:
            SLY4_WARN("SPU %08X: escrita no canal %u sem implementação (0x%X, pc 0x%05X)", id, ch, value, c.pc);
            return SpuChannelResult::Invalid;
    }
}

uint32_t SpuThread::events(SPUContext&) {
    std::lock_guard<std::mutex> lk(m);
    return pending_events();
}

uint32_t SpuThread::count(SPUContext& c, uint32_t ch) {
    std::lock_guard<std::mutex> lk(m);
    switch (ch) {
        case spu_ch::RdInMbox: return uint32_t(in_mbox.size());
        case spu_ch::WrOutMbox: return out_mbox.empty() ? 1 : 0;
        case spu_ch::WrOutIntrMbox: return 1;
        case spu_ch::RdSigNotify1: return snr_valid[0] ? 1 : 0;
        case spu_ch::RdSigNotify2: return snr_valid[1] ? 1 : 0;
        case spu_ch::RdEventStat: spu_check_decrementer(c); c.event_stat |= pending_events(); return (c.event_stat & c.event_mask) ? 1 : 0;
        default: return 0;
    }
}

// ---------------------------------------------------------------- stop signals
bool SpuThread::handle_stop(uint32_t code) {
    uint32_t v = 0;
    switch (code) {
        case spu_stop::Yield:
            std::this_thread::yield();
            return true;
        case spu_stop::ThreadExit:
            pop_out_mbox(v);
            exit_status = v;
            return false;
        case spu_stop::GroupExit: {
            pop_out_mbox(v);
            {
                std::lock_guard<std::mutex> lk(group->m);
                group->exit_status = v;
                group->join_cause = kJoinGroupExit;
            }
            for (auto& t : group->threads) {
                if (!t || t.get() == this) continue;
                t->stop_requested = true;
                t->cv.notify_all();
            }
            return false;
        }
        case spu_stop::ReceiveEvent:
        case spu_stop::TryReceiveEvent: {
            uint32_t spuq = 0;
            pop_out_mbox(spuq);
            std::shared_ptr<EventQueue> q;
            for (auto& [n, w] : bound)
                if (n == spuq) q = w.lock();
            {
                std::lock_guard<std::mutex> lk(m);
                in_mbox.clear();
            }
            if (!q) {
                push_in_mbox(CELL_EINVAL);
                return true;
            }
            Event e{};
            for (;;) {
                const uint32_t r = q->receive(e, code == spu_stop::TryReceiveEvent ? 1 : 10000);
                if (r == CELL_OK) break;
                if (code == spu_stop::TryReceiveEvent) {
                    push_in_mbox(CELL_EBUSY);
                    return true;
                }
                if (r != CELL_ETIMEDOUT || stop_requested) {
                    push_in_mbox(r == CELL_ETIMEDOUT ? CELL_ECANCELED : r);
                    return !stop_requested;
                }
            }
            {
                std::lock_guard<std::mutex> lk(m);
                in_mbox.assign({CELL_OK, uint32_t(e.data1), uint32_t(e.data2), uint32_t(e.data3)});
            }
            cv.notify_all();
            return true;
        }
        default:
            SLY4_WARN("SPU %08X: stop 0x%X sem implementação (pc 0x%05X)", id, code, ctx.pc - 4);
            return false;
    }
}

void SpuThread::run() {
    running = true;
    name_host_thread("SPU" + std::to_string(id & 0xFF));  // --ppu-profile SPU1 ... SPU6
    while (!stop_requested) {
        uint64_t n = 0;
        const SpuStep st = spu_run_blocks(ctx, 100000, n);
        executed += n;
        if (st == SpuStep::Ok || st == SpuStep::Branch) continue;
        if (st == SpuStep::Wait) continue;  // a channel gave up because of stop_requested
        if (st == SpuStep::Stop) {
            if (!handle_stop(ctx.stop_code)) break;
            continue;
        }
        SLY4_ERROR("SPU %08X '%s': %s em 0x%05X", id, name.c_str(),
                   st == SpuStep::Halt ? "halt" : "instrução inválida", ctx.pc);
        break;
    }
    running = false;
    group->thread_finished();
}

void SpuThreadGroup::thread_finished() {
    std::shared_ptr<EventQueue> q;
    {
        std::lock_guard<std::mutex> lk(m);
        if (--running) return;
        if (!join_cause) join_cause = kJoinAllThreadsExit;
        started = false;
        q = run_queue.lock();
    }
    cv.notify_all();
    if (q) q->send({kSpuGroupEventRunKey, id, 0, 0});
}

// ---------------------------------------------------------------- images
// sys_spu_image {u32 type, u32 entry_point, u32 segs, s32 nsegs}; sys_spu_segment
// {s32 type (1 copy, 2 fill, 4 info), u32 ls, u32 size, u32 addr or fill value}.
uint32_t spu_image_import(GuestMemory& m, GuestAddr img, GuestAddr elf) {
    SpuImage info;
    std::string error;
    if (!parse_spu_elf(m.host(elf), 0x1000000, info, error)) {
        SLY4_ERROR("sys_spu_image_import(0x%08X): %s", elf, error.c_str());
        return CELL_ENOEXEC;
    }
    uint32_t nsegs = 0;
    for (const SpuSegment& s : info.segments) nsegs += 1 + (s.mem_size > s.file_size);
    const GuestAddr segs = kernel().main_memory().alloc(16 * std::max(nsegs, 1u), 16);
    if (!segs) return CELL_ENOMEM;
    uint32_t i = 0;
    for (const SpuSegment& s : info.segments) {
        const GuestAddr e = segs + 16 * i++;
        m.write<int32_t>(e, 1);
        m.write<uint32_t>(e + 4, s.vaddr);
        m.write<uint32_t>(e + 8, s.file_size);
        m.write<uint32_t>(e + 12, elf + s.offset);
        if (s.mem_size > s.file_size) {
            const GuestAddr z = segs + 16 * i++;
            m.write<int32_t>(z, 2);
            m.write<uint32_t>(z + 4, s.vaddr + s.file_size);
            m.write<uint32_t>(z + 8, s.mem_size - s.file_size);
            m.write<uint32_t>(z + 12, 0);
        }
    }
    m.write<uint32_t>(img, 0);  // SYS_SPU_IMAGE_TYPE_USER
    m.write<uint32_t>(img + 4, info.entry);
    m.write<uint32_t>(img + 8, segs);
    m.write<int32_t>(img + 12, int32_t(nsegs));
    return CELL_OK;
}

}  // namespace sly4

namespace sly4 {

namespace {
std::mutex g_threads_lock;
std::vector<std::shared_ptr<SpuThread>> g_threads;
}  // namespace

void add_spu_thread(const std::shared_ptr<SpuThread>& t) {
    std::lock_guard<std::mutex> g(g_threads_lock);
    g_threads.push_back(t);
    // Threads live for the whole run (the registry keeps them), so the pointer stays valid.
    const size_t i = g_waiter_count.load();
    if (i < kMaxSpuThreads) {
        g_waiters[i] = t.get();
        g_waiter_count = i + 1;
    }
    set_line_write_hook(wake_spus);
}

std::vector<std::shared_ptr<SpuThread>> spu_threads() {
    std::lock_guard<std::mutex> g(g_threads_lock);
    return g_threads;
}

}  // namespace sly4

// ---------------------------------------------------------------- PC profile
namespace sly4 {

namespace {
std::mutex g_profile_lock;
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_profile;  // (SPU id, pc) -> samples
std::atomic<bool> g_profiling{false};
}  // namespace

void spu_profile_start() {
    if (g_profiling.exchange(true)) return;
    std::thread([] {
        while (!kernel().exited()) {
            host_sleep_us(250);
            std::lock_guard<std::mutex> g(g_profile_lock);
            for (const auto& t : spu_threads())
                if (t->running) ++g_profile[{t->id, t->ctx.pc}];
        }
    }).detach();
}

void spu_profile_print() {
    if (const uint64_t late = g_waits_late.load())
        SLY4_INFO("SPU: %" PRIu64 " esperas de canal acordadas, %" PRIu64
                  " só vistas no timeout de 1 ms (reserva %" PRIu64 ", decrementador %" PRIu64 ", outras %" PRIu64 ")",
                  g_waits_woken.load(), late, g_late_events[0].load(), g_late_events[1].load(), g_late_events[2].load());
    if (!g_profiling) return;
    std::lock_guard<std::mutex> g(g_profile_lock);
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> by_spu;
    for (const auto& [key, n] : g_profile) by_spu[key.first].push_back({n, key.second});
    for (auto& [id, v] : by_spu) {
        std::sort(v.rbegin(), v.rend());
        uint64_t total = 0;
        for (const auto& [n, pc] : v) total += n;
        std::fprintf(stderr, "  perfil do SPU 0x%08X (%" PRIu64 " amostras):\n", id, total);
        std::shared_ptr<SpuThread> t;
        for (const auto& s : spu_threads())
            if (s->id == id) t = s;
        for (size_t i = 0; i < v.size() && i < 80; ++i)
            std::fprintf(stderr, "    %5.1f%%  %05X  %s\n", 100.0 * double(v[i].first) / double(total), v[i].second,
                         t ? spu::disasm(spu::decode(t->ctx.ls_u32(v[i].second)), v[i].second).c_str() : "");
    }
}

}  // namespace sly4
