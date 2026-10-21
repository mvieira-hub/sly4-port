#include "cpu/ppu_rt.h"

#include <algorithm>
#include <cstdlib>

#include "core/log.h"
#include "cpu/ppu_interpreter.h"

namespace sly4::rt {

namespace {

const FnEntry* g_table = nullptr;
size_t g_count = 0;

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

void set_function_table(const FnEntry* table, size_t count) {
    g_table = table;
    g_count = count;
}

const FnEntry* function_table(size_t& count) {
    count = g_count;
    return g_table;
}

GenFn find_function(uint32_t addr) {
    const FnEntry* end = g_table + g_count;
    const FnEntry* it = std::lower_bound(g_table, end, addr, [](const FnEntry& e, uint32_t a) { return e.addr < a; });
    return it != end && it->addr == addr ? it->fn : nullptr;
}

void call_indirect(PPUContext& c, GuestMemory& m, uint32_t target) {
    if (GenFn fn = find_function(target)) {
        fn(c, m);
        return;
    }
    interpret_until_return(c, m, target);
}

void interpret_until_return(PPUContext& c, GuestMemory& m, uint32_t entry) {
    const uint32_t ret = uint32_t(c.lr) & ~3u;
    c.cia = entry;
    while (c.cia != ret) {
        if (--c.budget < 0) {
            budget_exceeded(c, m);
            return;
        }
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
