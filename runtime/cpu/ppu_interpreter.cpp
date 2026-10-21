// Interpreter loop body: control flow, system instructions and dispatch to the shared
// per-instruction semantics (ppu_semantics.h).
#include "cpu/ppu_interpreter.h"

#include <chrono>

namespace sly4 {

using ppu::Insn;
using ppu::Op;

namespace sem {

uint64_t (*timebase_source)() = nullptr;

uint64_t timebase() {
    if (timebase_source) return timebase_source();
    constexpr uint64_t kTimebaseHz = 79'800'000;  // PS3 time base frequency
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const uint64_t ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return static_cast<uint64_t>(u128(ns) * kTimebaseHz / 1'000'000'000);
}

}  // namespace sem

namespace {

// Every op through its sem::op<> (control ops are handled before reaching here).
PpuStep dispatch(PPUContext& c, GuestMemory& mem, const Insn& in) {
    switch (in.op) {
#define X(e, name, form) \
    case Op::e: return sem::op<Op::e>(c, mem, in);
#include "ppu/ppu_ops.inc"
#undef X
        default: return PpuStep::Invalid;
    }
}

}  // namespace

PpuStep ppu_step(PPUContext& c, GuestMemory& mem) {
    return ppu_execute(c, mem, ppu::decode(mem.read<uint32_t>(c.cia)));
}

PpuStep ppu_execute(PPUContext& c, GuestMemory& mem, const Insn& in) {
    const uint32_t d = in.rd(), a = in.ra(), b = in.rb();
    switch (in.op) {
        // ---------------- control flow
        case Op::b: {
            const uint32_t target = in.branch_target(c.cia);
            if (in.lk()) c.lr = c.cia + 4;
            c.cia = target;
            return PpuStep::Ok;
        }
        case Op::bc: {
            const bool taken = sem::branch_taken(c, d, a, true);
            if (in.lk()) c.lr = c.cia + 4;
            c.cia = taken ? in.branch_target(c.cia) : c.cia + 4;
            return PpuStep::Ok;
        }
        case Op::bclr: {
            const uint32_t target = uint32_t(c.lr) & ~3u;
            const bool taken = sem::branch_taken(c, d, a, true);
            if (in.lk()) c.lr = c.cia + 4;
            c.cia = taken ? target : c.cia + 4;
            return PpuStep::Ok;
        }
        case Op::bcctr: {
            const uint32_t target = uint32_t(c.ctr) & ~3u;
            const bool taken = sem::branch_taken(c, d, a, false);
            if (in.lk()) c.lr = c.cia + 4;
            c.cia = taken ? target : c.cia + 4;
            return PpuStep::Ok;
        }
        // ---------------- system
        case Op::tw: if (sem::trap_taken(d, int32_t(c.gpr[a]), int32_t(c.gpr[b]))) return PpuStep::Trap; break;
        case Op::td: if (sem::trap_taken(d, int64_t(c.gpr[a]), int64_t(c.gpr[b]))) return PpuStep::Trap; break;
        case Op::twi: if (sem::trap_taken(d, int32_t(c.gpr[a]), in.simm())) return PpuStep::Trap; break;
        case Op::tdi: if (sem::trap_taken(d, int64_t(c.gpr[a]), in.simm())) return PpuStep::Trap; break;
        case Op::sc: c.cia += 4; return PpuStep::Syscall;
        case Op::attn: return PpuStep::Halt;
        case Op::invalid: return PpuStep::Invalid;
        default: {
            const PpuStep st = dispatch(c, mem, in);
            if (st != PpuStep::Ok) return st;
            break;
        }
    }
    c.cia += 4;
    return PpuStep::Ok;
}

}  // namespace sly4
