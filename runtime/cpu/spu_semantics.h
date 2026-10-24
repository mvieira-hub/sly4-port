// Semantics of every SPU instruction, one inline function per instruction:
//
//     spusem::op<spu::Op::a>(c, insn)
//
// Shared by the SPU interpreter and (phase 4c) the recompiled C++. Control flow sets
// c.pc and returns Branch; everything else returns Ok and the caller advances the PC.
// Channels (rdch/wrch/rchcnt) go through spu_channels.h.
//
// Single-precision float follows the SPU where RPCS3 does by default: denormal inputs and
// results are zero, comparisons read exponent 255 as a number (no NaNs on the SPU) and zero
// times anything is zero; the rest is the host's IEEE, rounding to nearest (see
// spu_sem_float.inl). frest/frsqest give the SPU's piecewise-linear estimate, in the form fi
// completes (a little below the exact value, even for powers of two). Double precision is
// IEEE, as on the SPU. SPU code runs inside a SpuFloatMode (spu_run, spu_run_blocks): the
// host's own DAZ/FTZ do the denormals.
//
// Element-wise loops go from the last guest element to the first: that is the host's
// memory order (registers are reversed), so GCC vectorizes them without reversing the
// lanes before and after each instruction.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <immintrin.h>

#include "cpu/spu_channels.h"
#include "cpu/spu_context.h"
#include "spu/spu_decoder.h"

namespace sly4 {

enum class SpuStep : uint8_t {
    Ok,       // executed; the caller adds 4 to the PC
    Branch,   // c.pc holds the next instruction
    Stop,     // stop/stopd: c.pc on the next instruction, the signal in c.stop_code
    Halt,     // heq/hgt/hlgt condition met
    Invalid,  // not an instruction
    Wait,     // a channel cannot be read/written yet; retry the same instruction
};

// The semantics and their helpers are forced inline: in the generated blocks each
// instruction then compiles with its fields as constants. Left to itself GCC called them
// out of line in the big blocks, and those calls were 44% of an SPU thread in the menu.
#define SLY4_SPU_INLINE inline __attribute__((always_inline))

namespace spusem {

using spu::Insn;
using spu::Op;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using u128 = unsigned __int128;

// The 128-bit guest value of a register (guest byte 0 = most significant): with the
// reversed layout this is just the host little-endian integer.
SLY4_SPU_INLINE u128 q(const SpuReg& r) {
    u128 v;
    std::memcpy(&v, r.b, 16);
    return v;
}
SLY4_SPU_INLINE void set_q(SpuReg& r, u128 v) { std::memcpy(r.b, &v, 16); }
// The same in an SSE register: GCC does u128 logic and shifts in pairs of general
// registers, moving every operand out of and back into the vector unit.
SLY4_SPU_INLINE __m128i vq(const SpuReg& r) {
    __m128i v;
    std::memcpy(&v, r.b, 16);
    return v;
}
SLY4_SPU_INLINE void set_vq(SpuReg& r, __m128i v) { std::memcpy(r.b, &v, 16); }

SLY4_SPU_INLINE u32 rotl32(u32 x, u32 n) { return n ? (x << n) | (x >> (32 - n)) : x; }
SLY4_SPU_INLINE u16 rotl16(u16 x, u32 n) { return n ? u16((x << n) | (x >> (16 - n))) : x; }
SLY4_SPU_INLINE u128 rotl128(u128 x, u32 n) { return n ? (x << n) | (x >> (128 - n)) : x; }

// Denormals are zero on the SPU (single precision): MXCSR's DAZ (denormal operands are
// zero) and FTZ (denormal results are zero) do that on the host. Around all SPU code;
// before, filtering every operand and result in software made fm/fma ~25 host
// instructions each. The SPU's single precision rounds toward zero; like RPCS3, this
// rounds to nearest.
class SpuFloatMode {
public:
    static constexpr unsigned kDazFtz = 0x8040;
    SpuFloatMode() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | kDazFtz); }
    ~SpuFloatMode() { _mm_setcsr(saved_); }
    SpuFloatMode(const SpuFloatMode&) = delete;
    SpuFloatMode& operator=(const SpuFloatMode&) = delete;

private:
    unsigned saved_;
};

// Double precision keeps its denormals: DAZ/FTZ are off for its instructions. Operands
// pass through in() after the switch and results through out() before the switch back
// (empty volatile asm, kept in order with ldmxcsr), so the arithmetic stays inside.
class IeeeDoubles {
public:
    IeeeDoubles() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ & ~SpuFloatMode::kDazFtz); }
    ~IeeeDoubles() { _mm_setcsr(saved_); }
    IeeeDoubles(const IeeeDoubles&) = delete;
    IeeeDoubles& operator=(const IeeeDoubles&) = delete;
    static double in(double v) {
        asm volatile("" : "+x"(v));
        return v;
    }
    static double out(double v) { return in(v); }

private:
    unsigned saved_;
};

// Denormals as zero in software, for values that do not go through a float operation.
SLY4_SPU_INLINE float ftz(float f) {
    // On the bits, without branches, so the 4-lane loops of the float instructions can be
    // vectorized: exponent 0 (denormal or zero) keeps only the sign, NaN becomes the default.
    u32 b = std::bit_cast<u32>(f);
    const u32 e = b & 0x7F800000u, m = b & 0x007FFFFFu;
    b = e == 0 ? (b & 0x80000000u) : b;
    b = (e == 0x7F800000u && m) ? 0x7FC00000u : b;
    return std::bit_cast<float>(b);
}
// Double precision is IEEE on the SPU; NaN results are made the default NaN: which input
// NaN an x86 FMA passes on depends on the operand order the compiler picks, and the
// interpreter and the generated code must agree bit for bit.
SLY4_SPU_INLINE double dnan(double d) { return std::isnan(d) ? std::bit_cast<double>(0x7FF8000000000000ull) : d; }

SLY4_SPU_INLINE u32 branch_target(u32 a) { return a & kSpuLsMask & ~3u; }
SLY4_SPU_INLINE void set_link(SPUContext& c, u32 rt) {
    SpuReg r{};
    r.u32(0) = (c.pc + 4) & kSpuLsMask;
    c.gpr[rt] = r;
}

template <Op O>
inline SpuStep op(SPUContext& c, Insn in);


// SPU_SEM(name, statements...): defines op<Op::name>. t/a/b/cc are the register fields
// T, A, B and C (binutils naming); RT/RA/RB/RC the registers themselves.
#define SPU_SEM(name, ...)                                                        \
    template <>                                                                   \
    SLY4_SPU_INLINE SpuStep op<Op::name>(SPUContext & c, Insn in) {                        \
        [[maybe_unused]] const u32 t = in.rt(), a = in.ra(), b = in.rb(), cc = in.rc(); \
        [[maybe_unused]] SpuReg& RT = c.gpr[t];                                   \
        [[maybe_unused]] const SpuReg RA = c.gpr[a], RB = c.gpr[b], RC = c.gpr[cc];       \
        __VA_ARGS__;                                                              \
        return SpuStep::Ok;                                                       \
    }

// Element-wise forms: r.<lane accessor>(i) = expression of i.
#define SPU_WORDS(name, ...) SPU_SEM(name, SpuReg r; for (int i = 3; i >= 0; --i) r.u32(i) = (__VA_ARGS__); RT = r)
#define SPU_HALVES(name, ...) SPU_SEM(name, SpuReg r; for (int i = 7; i >= 0; --i) r.u16(i) = u16(__VA_ARGS__); RT = r)
#define SPU_BYTES(name, ...) SPU_SEM(name, SpuReg r; for (int i = 15; i >= 0; --i) r.u8(i) = u8(__VA_ARGS__); RT = r)

}  // namespace spusem
}  // namespace sly4

#include "cpu/spu_sem_int.inl"
#include "cpu/spu_sem_float.inl"
#include "cpu/spu_sem_control.inl"
