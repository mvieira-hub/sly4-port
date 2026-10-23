#include "cpu/guest_atomic.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace sly4 {

namespace {
constexpr size_t kStripes = 256;
std::mutex g_locks[kStripes];
std::atomic<LineWriteHook> g_hook{nullptr};
DebugWhoHook g_debug_who = nullptr;  // --watch-line
}  // namespace

std::mutex& guest_line_lock(GuestAddr ea) { return g_locks[(ea >> 7) % kStripes]; }

void set_line_write_hook(LineWriteHook h) { g_hook = h; }

void guest_line_written(GuestAddr ea) {
    if (LineWriteHook h = g_hook.load(std::memory_order_relaxed)) h(ea & ~127u);
}

void guest_load_reserve(GuestMemory& mem, GuestAddr ea, uint8_t* line) {
    const GuestAddr l = ea & ~127u;
    std::lock_guard<std::mutex> g(guest_line_lock(l));
    std::memcpy(line, mem.host(l), 128);
}

bool guest_store_conditional(GuestMemory& mem, GuestAddr ea, const uint8_t* line, uint64_t desired, int size) {
    const GuestAddr l = ea & ~127u;
    bool ok;
    {
        std::lock_guard<std::mutex> g(guest_line_lock(l));
        ok = std::memcmp(mem.host(l), line, 128) == 0;
        if (ok) {
            uint8_t before[128];
            const bool dbg = debug_line_hit(l, 128) != 0;
            if (dbg) std::memcpy(before, mem.host(l), 128);
            // The word itself with a compare-and-swap: a plain store to it after the compare
            // fails the store-conditional, as it would have killed the reservation.
            const uint8_t* seen = line + (ea & 127u);
            if (size == 4) {
                uint32_t expected, want = uint32_t(desired);
                std::memcpy(&expected, seen, 4);
                ok = __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(mem.host(ea)), &expected, want, false,
                                                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            } else {
                uint64_t expected;
                std::memcpy(&expected, seen, 8);
                ok = __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(mem.host(ea)), &expected, desired, false,
                                                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            }
            if (dbg && ok) {
                char who[96];
                const int n = std::snprintf(who, sizeof(who), "%s ", size == 4 ? "PPU stwcx" : "PPU stdcx");
                if (g_debug_who) g_debug_who(who + n, sizeof(who) - size_t(n));
                debug_line_log(mem, l, before, who, 0);
            }
        }
    }
    if (ok) guest_line_written(ea);
    return ok;
}

void guest_store_line_changes(uint8_t* line, const uint8_t* old, const uint8_t* now) {
    for (uint32_t i = 0; i < 128; i += 8) {
        uint64_t o, n;
        std::memcpy(&o, old + i, 8);
        std::memcpy(&n, now + i, 8);
        const uint64_t diff = o ^ n;
        if (!diff) continue;
        uint64_t mask = 0;  // the bytes that changed
        for (int b = 0; b < 64; b += 8)
            if ((diff >> b) & 0xFF) mask |= 0xFFull << b;
        auto* w = reinterpret_cast<uint64_t*>(line + i);
        uint64_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(w, &cur, (cur & ~mask) | (n & mask), true, __ATOMIC_SEQ_CST,
                                            __ATOMIC_RELAXED)) {
        }
    }
}

}  // namespace sly4

namespace sly4 {

namespace {
uint32_t g_debug_lines[kMaxDebugLines] = {};  // set before the game starts
int g_debug_count = 0;
}

void add_debug_line(GuestAddr line) {
    if (g_debug_count < kMaxDebugLines) g_debug_lines[g_debug_count++] = line & ~127u;
}
void set_debug_who_hook(DebugWhoHook h) { g_debug_who = h; }

int debug_lines_hit(GuestAddr ea, uint32_t size, GuestAddr (&out)[kMaxDebugLines]) {
    int n = 0;
    for (int i = 0; i < g_debug_count; ++i) {
        const uint32_t l = g_debug_lines[i];
        if (ea < l + 128 && l < ea + size) out[n++] = l;
    }
    return n;
}

GuestAddr debug_line_hit(GuestAddr ea, uint32_t size) {
    for (int i = 0; i < g_debug_count; ++i) {
        const uint32_t l = g_debug_lines[i];
        if (ea < l + 128 && l < ea + size) return l;
    }
    return 0;
}

void debug_line_log(GuestMemory& mem, GuestAddr line, const uint8_t* before, const char* who, uint32_t pc) {
    char changes[512];
    int n = 0;
    for (uint32_t w = 0; w < 32 && n < 480; ++w) {
        const uint32_t a = load_be<uint32_t>(before + 4 * w), b = mem.read<uint32_t>(line + 4 * w);
        if (a != b) n += std::snprintf(changes + n, sizeof(changes) - size_t(n), " +%02X:%08X->%08X", 4 * w, a, b);
    }
    changes[n] = 0;
    // Absolute steady-clock milliseconds: the same clock as the RSX's --rsx-timeline.
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    SLY4_INFO("%10.3f ms linha 0x%08X: %s (pc 0x%05X)%s", ms, line, who, pc, n ? changes : " sem mudança");
}

}  // namespace sly4
