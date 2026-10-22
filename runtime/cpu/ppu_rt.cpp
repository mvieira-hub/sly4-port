#include "cpu/ppu_rt.h"
#include "cpu/ppu_rt_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "cpu/ppu_interpreter.h"

namespace sly4::rt {

namespace {

// Every generated table registered so far (the game's and each firmware module's),
// merged and sorted by address.
std::vector<FnEntry> g_functions;

constexpr uint64_t kCellEnosys = 0x80010003;

void fault(PPUContext& c, GuestMemory& m, const char* what) {
    if (hooks().fault) {
        hooks().fault(c, m, what);
        return;
    }
    SLY4_ERROR("PPU: %s em 0x%08X (LR 0x%08llX)", what, c.cia, static_cast<unsigned long long>(c.lr));
    std::abort();
}

}  // namespace

Hooks& hooks() {
    static Hooks h;
    return h;
}

// Adds a generated table (each generated module registers its own).
void set_function_table(const FnEntry* table, size_t count) {
    g_functions.insert(g_functions.end(), table, table + count);
    std::sort(g_functions.begin(), g_functions.end(), [](const FnEntry& a, const FnEntry& b) { return a.addr < b.addr; });
}

const FnEntry* function_table(size_t& count) {
    count = g_functions.size();
    return g_functions.data();
}

GenFn find_function(uint32_t addr) {
    auto it = std::lower_bound(g_functions.begin(), g_functions.end(), addr,
                               [](const FnEntry& e, uint32_t a) { return e.addr < a; });
    return it != g_functions.end() && it->addr == addr ? it->fn : nullptr;
}

static bool is_hle(uint32_t target) {
    const Hooks& h = hooks();
    return h.hle && target - h.hle_base < h.hle_size;
}

namespace {
LleTraceFn g_lle_trace = nullptr;
uint32_t g_lle_base = 0, g_lle_size = 0;
}  // namespace

void set_lle_trace(LleTraceFn fn, uint32_t base, uint32_t size) {
    g_lle_base = base;
    g_lle_size = size;
    g_lle_trace = fn;
}

void call_indirect(PPUContext& c, GuestMemory& m, uint32_t target) {
    if (g_lle_trace && target - g_lle_base < g_lle_size) g_lle_trace(c, target);
    if (GenFn fn = find_function(target)) {
        fn(c, m);
        return;
    }
    if (is_hle(target)) {
        hooks().hle(c, m, target);
        return;
    }
    interpret_until_return(c, m, target);
}

namespace {
std::atomic<uint64_t> g_interp_calls{0}, g_interp_steps{0};
}  // namespace

void interpreter_stats(uint64_t& calls, uint64_t& steps) {
    calls = g_interp_calls.load();
    steps = g_interp_steps.load();
}

void interpret_until_return(PPUContext& c, GuestMemory& m, uint32_t entry) {
    const uint32_t ret = uint32_t(c.lr) & ~3u;
    c.cia = entry;
    g_interp_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t steps = 0;
    struct Count {  // added once, on every way out
        uint64_t& n;
        ~Count() { g_interp_steps.fetch_add(n, std::memory_order_relaxed); }
    } count{steps};
    while (c.cia != ret) {
        if (is_hle(c.cia)) {  // reached by bctr/bctrl: run it and return to LR
            hooks().hle(c, m, c.cia);
            c.cia = uint32_t(c.lr) & ~3u;
            continue;
        }
        if (--c.budget < 0) {
            budget_exceeded(c, m);
            return;
        }
        ++steps;
        switch (ppu_step(c, m)) {
            case PpuStep::Ok: break;
            case PpuStep::Syscall: syscall(c, m); break;
            case PpuStep::Trap: trap(c, m); return;
            case PpuStep::Halt:
            case PpuStep::Invalid: invalid(c, m); return;
        }
    }
}

void syscall(PPUContext& c, GuestMemory& m) {
    if (hooks().syscall) {
        hooks().syscall(c, m);
        return;
    }
    SLY4_WARN("syscall %llu sem implementação (0x%08X)", static_cast<unsigned long long>(c.gpr[11]), c.cia - 4);
    c.gpr[3] = kCellEnosys;
}

void trap(PPUContext& c, GuestMemory& m) { fault(c, m, "armadilha (tw/td)"); }
void invalid(PPUContext& c, GuestMemory& m) { fault(c, m, "instrução inválida"); }
void bad_jump(PPUContext& c, GuestMemory& m, uint32_t from, uint32_t target) {
    c.cia = from;
    c.ctr = target;
    fault(c, m, "destino fora da tabela de salto");
}
void lr_mismatch(PPUContext& c, GuestMemory& m, uint32_t at, uint32_t expected) {
    if (hooks().lr_mismatch) {
        hooks().lr_mismatch(c, m, at, expected);
        return;
    }
    SLY4_WARN("blr em 0x%08X volta para 0x%08X, esperado 0x%08X", at, uint32_t(c.lr) & ~3u, expected);
}
void budget_exceeded(PPUContext& c, GuestMemory& m) { fault(c, m, "orçamento de execução esgotado"); }

}  // namespace sly4::rt
