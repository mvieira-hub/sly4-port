// Semantics of every PPU instruction, one inline function per instruction:
//
//     sem::op<ppu::Op::addi>(c, mem, insn)
//
// Shared by the interpreter (which dispatches on the decoded op) and by the recompiled
// C++ (which calls the right op<> with a constant instruction word, so the compiler
// specialises and folds the operand extraction). Validated against QEMU by
// "sly4_recomp oracle-check" (docs/RECOMP.md, 2c).
//
// Control flow (b, bc, bclr, bcctr), sc, attn and traps are not here: the interpreter
// and the emitter handle them, using the helpers below (branch_taken, trap_taken).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/endian.h"
#include "cpu/ppu_context.h"
#include "cpu/guest_atomic.h"
#include "cpu/ppu_fpu.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"

namespace sly4 {

enum class PpuStep : uint8_t {
    Ok,       // executed; c.cia is the next instruction (or the branch target)
    Syscall,  // sc: the kernel handles r11/r3..; c.cia already points past it
    Trap,     // tw/td/twi/tdi condition met (c.cia still on the trap)
    Halt,     // attn
    Invalid,  // not a valid instruction / operand
};

namespace sem {

using ppu::Insn;
using ppu::Op;
using u64 = uint64_t;
using s64 = int64_t;
using u32 = uint32_t;
using s32 = int32_t;
using u128 = unsigned __int128;
using s128 = __int128;

// ---------------------------------------------------------------- shared helpers
inline u64 rotl64(u64 x, unsigned n) {
    n &= 63;
    return n ? (x << n) | (x >> (64 - n)) : x;
}
// 32-bit rotate as the ISA defines it: the low word duplicated, then rotated as 64 bits.
inline u64 rotl32(u64 x, unsigned n) { return rotl64((x & 0xFFFFFFFF) | (x << 32), n & 31); }
// MASK(mb, me) in ISA bit numbering (bit 0 = MSB), inclusive, wrapping when mb > me.
inline u64 mask64(unsigned mb, unsigned me) {
    const u64 m1 = ~0ull >> mb, m2 = ~0ull << (63 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}
inline u64 add_carry(u64 a, u64 b, bool cin, bool& cout) {
    const u128 s = u128(a) + b + cin;
    cout = (s >> 64) != 0;
    return static_cast<u64>(s);
}
inline bool add_overflow(u64 a, u64 b, u64 r) { return ((a ^ r) & (b ^ r)) >> 63; }

inline void set_cr0(PPUContext& c, u64 r) {
    const s64 v = static_cast<s64>(r);
    c.set_crf(0, (v < 0 ? 8u : v > 0 ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
inline void compare_signed(PPUContext& c, u32 bf, s64 a, s64 b) {
    c.set_crf(bf, (a < b ? 8u : a > b ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
inline void compare_unsigned(PPUContext& c, u32 bf, u64 a, u64 b) {
    c.set_crf(bf, (a < b ? 8u : a > b ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
inline void set_ov(PPUContext& c, bool ov) {
    c.xer_ov = ov;
    if (ov) c.xer_so = true;
}
// XO-form result in rD with OE/Rc; X-form logical result in rA with Rc.
inline void xo_result(PPUContext& c, const Insn& in, u64 res, bool ov, bool has_oe = true) {
    c.gpr[in.rd()] = res;
    if (has_oe && in.oe()) set_ov(c, ov);
    if (in.rc_bit()) set_cr0(c, res);
}
inline void x_result(PPUContext& c, const Insn& in, u64 res) {
    c.gpr[in.ra()] = res;
    if (in.rc_bit()) set_cr0(c, res);
}

inline GuestAddr ea_d(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + static_cast<s64>(in.simm()));
}
inline GuestAddr ea_ds(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + static_cast<s64>(in.ds()));
}
inline GuestAddr ea_x(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + c.gpr[in.rb()]);
}

// Conditional branch condition (BO/BI), updating CTR when BO[2] = 0 and allowed.
inline bool branch_taken(PPUContext& c, u32 bo, u32 bi, bool allow_ctr) {
    bool ctr_ok = true;
    if (allow_ctr && !(bo & 4)) {
        --c.ctr;
        ctr_ok = (c.ctr != 0) != ((bo & 2) != 0);
    }
    const bool cond_ok = (bo & 16) || (c.crbit(bi) == ((bo & 8) != 0));
    return ctr_ok && cond_ok;
}
inline bool trap_taken(u32 to, s64 a, s64 b) {
    return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) || ((to & 2) && u64(a) < u64(b)) ||
           ((to & 1) && u64(a) > u64(b));
}

template <class T>
inline bool store_conditional(PPUContext& c, GuestMemory& mem, GuestAddr ea, T value) {
    const bool ok = c.reserved && c.reserve_addr == ea && c.reserve_size == sizeof(T);
    c.reserved = false;
    if (!ok) return false;
    // The whole reserved line must be unchanged, checked under the lock the SPU's
    // lock-line commands take too (cpu/guest_atomic.h).
    return guest_store_conditional(mem, ea, c.reserve_line, uint64_t(byteswap(value)), int(sizeof(T)));
}

uint64_t timebase();  // PS3 time base (79.8 MHz), ppu_interpreter.cpp
// Replaces the time base source (deterministic tests); nullptr = host clock.
extern uint64_t (*timebase_source)();

// ---------------------------------------------------------------- per-instruction semantics
template <Op O>
inline PpuStep op(PPUContext& c, GuestMemory& mem, Insn in);

// SLY4_SEM(name, statements...): defines op<Op::name>; d/a/b are the main register fields
// and r the GPR file. Statements may `return PpuStep::Invalid`.
#define SLY4_SEM(name, ...)                                                                       \
    template <>                                                                                   \
    inline PpuStep op<Op::name>(PPUContext & c, GuestMemory & mem, Insn in) {                    \
        [[maybe_unused]] const u32 d = in.rd(), a = in.ra(), b = in.rb();                        \
        [[maybe_unused]] u64* const r = c.gpr;                                                   \
        (void)mem;                                                                                \
        __VA_ARGS__;                                                                              \
        return PpuStep::Ok;                                                                       \
    }

}  // namespace sem
}  // namespace sly4

#include "cpu/ppu_sem_int.inl"
#include "cpu/ppu_sem_fpu.inl"
#include "cpu/ppu_sem_vmx.inl"
