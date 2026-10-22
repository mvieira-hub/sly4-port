// Semantics of every SPU instruction, one inline function per instruction:
//
//     spusem::op<spu::Op::a>(c, insn)
//
// Shared by the SPU interpreter and (phase 4c) the recompiled C++. Control flow sets
// c.pc and returns Branch; everything else returns Ok and the caller advances the PC.
// Channels (rdch/wrch/rchcnt) go through spu_channels.h.
//
// Single-precision float follows what the SPU does where it is cheap to do so
// (denormal inputs and results are zero) and the host otherwise (round to nearest,
// IEEE infinities), as RPCS3 does by default; frest/frsqest return the exact value and
// fi passes it through. Double precision is IEEE, as on the SPU.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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
inline u128 q(const SpuReg& r) {
    u128 v;
    std::memcpy(&v, r.b, 16);
    return v;
}
inline void set_q(SpuReg& r, u128 v) { std::memcpy(r.b, &v, 16); }

inline u32 rotl32(u32 x, u32 n) { return n ? (x << n) | (x >> (32 - n)) : x; }
inline u16 rotl16(u16 x, u32 n) { return n ? u16((x << n) | (x >> (16 - n))) : x; }
inline u128 rotl128(u128 x, u32 n) { return n ? (x << n) | (x >> (128 - n)) : x; }

// Denormals are zero on the SPU (single precision). The SPU has no single-precision
// NaN either; on the host one comes out of invalid operations, and which input NaN an
// x86 FMA passes on depends on the operand order the compiler picks. So every NaN is
// made the same one, and the interpreter and the generated code agree bit for bit.
inline float ftz(float f) {
    switch (std::fpclassify(f)) {
        case FP_SUBNORMAL: return std::copysign(0.0f, f);
        case FP_NAN: return std::bit_cast<float>(0x7FC00000u);
        default: return f;
    }
}
// Double precision is IEEE on the SPU; NaN results are made the default NaN, for the
// same reason.
inline double dnan(double d) { return std::isnan(d) ? std::bit_cast<double>(0x7FF8000000000000ull) : d; }

inline u32 branch_target(u32 a) { return a & kSpuLsMask & ~3u; }
inline void set_link(SPUContext& c, u32 rt) {
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
    inline SpuStep op<Op::name>(SPUContext & c, Insn in) {                        \
        [[maybe_unused]] const u32 t = in.rt(), a = in.ra(), b = in.rb(), cc = in.rc(); \
        [[maybe_unused]] SpuReg& RT = c.gpr[t];                                   \
        [[maybe_unused]] const SpuReg RA = c.gpr[a], RB = c.gpr[b], RC = c.gpr[cc];       \
        __VA_ARGS__;                                                              \
        return SpuStep::Ok;                                                       \
    }

// Element-wise forms: r.<lane accessor>(i) = expression of i.
#define SPU_WORDS(name, ...) SPU_SEM(name, SpuReg r; for (int i = 0; i < 4; ++i) r.u32(i) = (__VA_ARGS__); RT = r)
#define SPU_HALVES(name, ...) SPU_SEM(name, SpuReg r; for (int i = 0; i < 8; ++i) r.u16(i) = u16(__VA_ARGS__); RT = r)
#define SPU_BYTES(name, ...) SPU_SEM(name, SpuReg r; for (int i = 0; i < 16; ++i) r.u8(i) = u8(__VA_ARGS__); RT = r)

}  // namespace spusem
}  // namespace sly4

#include "cpu/spu_sem_int.inl"
#include "cpu/spu_sem_float.inl"
#include "cpu/spu_sem_control.inl"
