// SPU floating point. Included by spu_semantics.h; see the note there on precision.
#pragma once

namespace sly4::spusem {

// ---------------------------------------------------------------- single precision
// 2^e as a double (|e| well inside the normal range here): multiplying by it is exact,
// like the std::ldexp it replaces, which was a library call (1.5% of an SPU thread).
SLY4_SPU_INLINE double pow2(int e) { return std::bit_cast<double>(u64(e + 1023) << 52); }

// The SPU's single precision has no infinities or NaNs: exponent 255 is one more binade
// (up to ~6.8e38, 0x7FFFFFFF being the largest value). Like RPCS3 by default ("approximate
// xfloat"), only the comparisons follow it (below); the arithmetic is the host's IEEE with
// one SPU rule, zero times anything is zero, and NaN results made the default NaN (which
// input NaN an x86 FMA passes on depends on the operand order the compiler picks: the
// interpreter and the generated code must agree bit for bit). Emulating the extended range
// in the arithmetic too (saturating at 0x7FFFFFFF) turned the overflows of the game's
// post-processing on the SPUs, black there, into white dots at the edge of characters.
SLY4_SPU_INLINE float fnan(float f) { return f != f ? std::bit_cast<float>(0x7FC00000u) : f; }
// Zero (or a denormal, zero on the SPU) times anything is zero: x when y is not zero.
SLY4_SPU_INLINE float nz(float x, float y) {
    return std::bit_cast<float>(std::bit_cast<u32>(x) & ((std::bit_cast<u32>(y) & 0x7F800000u) ? ~0u : 0x80000000u));
}

// Denormal operands and results are zero through DAZ/FTZ (SpuFloatMode).
#define SPU_FLOATS(name, expr) SPU_SEM(name, SpuReg r; for (int i = 3; i >= 0; --i) { \
    const float x = RA.f32(i), y = RB.f32(i); (void)x; (void)y; r.f32(i) = fnan(expr); } RT = r)
SPU_FLOATS(fa, x + y)
SPU_FLOATS(fs, x - y)
SPU_FLOATS(fm, nz(x, y) * nz(y, x))
#undef SPU_FLOATS

// frest/frsqest give an estimate in the form fi completes: the result carries the
// estimate's sign and exponent, a 13-bit base in mantissa bits 10..22 and a 10-bit step in
// bits 0..9 of a piecewise-linear 1/x (1/sqrt(x)) over 16 intervals of the operand's
// mantissa, and fi evaluates base - step * y with y the operand's low 19 mantissa bits
// (as RPCS3 does it). The base cannot hold 2.0, so the estimate is a little below the
// exact value even for powers of two: 1/1.0 comes out 1 - 2^-14. The game's MLAA on the
// SPUs counts on that: its "1.0" blend weight must stay under 0x10000 in 16.16, since the
// pixel weight is w + 1 of 8 bits, and an exact 1.0 made one of 257 (white dots at the
// edge of characters). The tables here are the chord of each interval (within 2^-10 of
// 1/x); the hardware's are a little tighter.
SLY4_SPU_INLINE u32 spu_estimate(u32 sign, int rexp, double at_lo, double at_hi) {
    if (rexp <= 0) return sign;  // underflow: zero
    if (rexp > 255) rexp = 255;
    const double base = at_lo - 1.0, step = at_lo - at_hi;
    const u32 b13 = base >= 1.0 ? 0x1FFFu : u32(base * 8192.0);
    const u32 s10 = std::min<u32>(0x3FF, u32(step * 8192.0 + 0.5));
    return sign | u32(rexp) << 23 | b13 << 10 | s10;
}
SLY4_SPU_INLINE u32 spu_frest(u32 a) {
    const u32 sign = a & 0x80000000u, exp = (a >> 23) & 0xFF, i = (a >> 19) & 15;
    if (exp == 0) return sign | 0x7F800000u;  // 1/0: the largest binade
    const double lo = 2.0 / (1.0 + i / 16.0), hi = 2.0 / (1.0 + (i + 1) / 16.0);
    return spu_estimate(sign, 253 - int(exp), lo, hi);
}
SLY4_SPU_INLINE u32 spu_frsqest(u32 a) {
    const u32 exp = (a >> 23) & 0xFF, i = (a >> 19) & 15;  // sign ignored: |x|
    if (exp == 0) return 0x7F800000u;
    const int e = int(exp) - 127;
    // x = 2^e (1 + f): even e gives 2^(-e/2) / sqrt(1 + f), odd e gives 2^(-(e-1)/2) / sqrt(2 (1 + f));
    // both scaled into [1, 2) for the result's exponent.
    const bool odd = e & 1;
    const double k = odd ? std::sqrt(2.0) : 2.0;
    const double lo = k / std::sqrt(1.0 + i / 16.0), hi = k / std::sqrt(1.0 + (i + 1) / 16.0);
    const int rexp = 127 - (odd ? (e - 1) / 2 : e / 2) - 1;
    return spu_estimate(0, rexp, lo, hi);
}
SLY4_SPU_INLINE float spu_fi(u32 a, u32 b) {
    const float base = std::bit_cast<float>((b & 0x007FFC00u) | 0x3F800000u);
    const float step = float(b & 0x3FFu) * float(pow2(-13));
    const float y = float(a & 0x7FFFFu) * float(pow2(-19));
    const u32 r = std::bit_cast<u32>(base - step * y);
    return ftz(std::bit_cast<float>((b & 0xFF800000u) | (r & 0x007FFFFFu)));
}
SPU_SEM(frest, SpuReg r; for (int i = 3; i >= 0; --i) r.u32(i) = spu_frest(std::bit_cast<u32>(ftz(RA.f32(i)))); RT = r)
SPU_SEM(frsqest, SpuReg r; for (int i = 3; i >= 0; --i) r.u32(i) = spu_frsqest(std::bit_cast<u32>(ftz(RA.f32(i)))); RT = r)
SPU_SEM(fi, SpuReg r; for (int i = 3; i >= 0; --i) r.f32(i) = spu_fi(std::bit_cast<u32>(ftz(RA.f32(i))), RB.u32(i)); RT = r)

// RRR fused forms: C = A * B (+/-) T.
#define SPU_FMA(name, expr) SPU_SEM(name, SpuReg r; for (int i = 3; i >= 0; --i) { \
    const float x = nz(RA.f32(i), RB.f32(i)), y = nz(RB.f32(i), RA.f32(i)), z = RT.f32(i); \
    r.f32(i) = fnan(expr); } c.gpr[cc] = r)
SPU_FMA(fma, std::fma(x, y, z))
SPU_FMA(fms, std::fma(x, y, -z))
SPU_FMA(fnms, -std::fma(x, y, -z))
#undef SPU_FMA

// Comparisons on the bits: sign and magnitude ordered as integers, zero and denormals
// (exponent 0) all equal to 0.
SLY4_SPU_INLINE s32 xf_key(u32 v) { return v & 0x7F800000u ? (v >> 31 ? -s32(v & 0x7FFFFFFFu) : s32(v)) : 0; }
SLY4_SPU_INLINE u32 xf_mag(u32 v) { return v & 0x7F800000u ? v & 0x7FFFFFFFu : 0; }
SPU_WORDS(fceq, xf_key(RA.u32(i)) == xf_key(RB.u32(i)) ? ~0u : 0u)
SPU_WORDS(fcgt, xf_key(RA.u32(i)) > xf_key(RB.u32(i)) ? ~0u : 0u)
SPU_WORDS(fcmeq, xf_mag(RA.u32(i)) == xf_mag(RB.u32(i)) ? ~0u : 0u)
SPU_WORDS(fcmgt, xf_mag(RA.u32(i)) > xf_mag(RB.u32(i)) ? ~0u : 0u)

// Conversions with a power-of-two scale: cflts/cfltu multiply by 2^(173 - u8) and
// saturate, csflt/cuflt divide by 2^(155 - u8).

SLY4_SPU_INLINE u32 to_s32_sat(double v) {
    if (std::isnan(v)) return v < 0 ? 0x80000000u : 0x7FFFFFFFu;
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return u32(s32(v));
}
SLY4_SPU_INLINE u32 to_u32_sat(double v) {
    if (std::isnan(v) || v <= 0) return 0;
    if (v >= 4294967295.0) return 0xFFFFFFFFu;
    return u32(v);
}
SPU_WORDS(cflts, to_s32_sat(double(ftz(RA.f32(i))) * pow2(173 - int(in.u8()))))
SPU_WORDS(cfltu, to_u32_sat(double(ftz(RA.f32(i))) * pow2(173 - int(in.u8()))))
SPU_SEM(csflt, SpuReg r; for (int i = 3; i >= 0; --i) r.f32(i) = ftz(float(double(s32(RA.u32(i))) * pow2(int(in.u8()) - 155))); RT = r)
SPU_SEM(cuflt, SpuReg r; for (int i = 3; i >= 0; --i) r.f32(i) = ftz(float(double(RA.u32(i)) * pow2(int(in.u8()) - 155))); RT = r)

// FPSCR: kept as written (exception flags are not modelled).
SPU_SEM(fscrrd, for (int i = 0; i < 4; ++i) RT.u32(i) = c.fpscr[i])
SPU_SEM(fscrwr, for (int i = 0; i < 4; ++i) c.fpscr[i] = RA.u32(i))

// ---------------------------------------------------------------- double precision
// IEEE, denormals included (IeeeDoubles).
#define SPU_DOUBLES(name, expr) SPU_SEM(name, IeeeDoubles ieee; SpuReg r; for (int i = 1; i >= 0; --i) { \
    const double x = ieee.in(RA.f64(i)), y = ieee.in(RB.f64(i)), z = ieee.in(RT.f64(i)); (void)z; \
    r.f64(i) = ieee.out(dnan(expr)); } RT = r)
SPU_DOUBLES(dfa, x + y)
SPU_DOUBLES(dfs, x - y)
SPU_DOUBLES(dfm, x * y)
SPU_DOUBLES(dfma, std::fma(x, y, z))
SPU_DOUBLES(dfms, std::fma(x, y, -z))
SPU_DOUBLES(dfnms, -std::fma(x, y, -z))
SPU_DOUBLES(dfnma, -std::fma(x, y, z))
#undef SPU_DOUBLES

// fesd: the single in each even word becomes a double; frds: the reverse (odd words 0).
SPU_SEM(fesd, SpuReg r; for (int i = 0; i < 2; ++i) r.f64(i) = double(ftz(RA.f32(2 * i))); RT = r)
SPU_SEM(frds, SpuReg r{}; for (int i = 0; i < 2; ++i) r.f32(2 * i) = ftz(float(RA.f64(i))); RT = r)

#define SPU_DCMP(name, cond) SPU_SEM(name, IeeeDoubles ieee; SpuReg r; for (int i = 1; i >= 0; --i) { \
    const double x = ieee.in(RA.f64(i)), y = ieee.in(RB.f64(i)); r.u64(i) = (cond) ? ~0ull : 0ull; } RT = r)
SPU_DCMP(dfceq, x == y)
SPU_DCMP(dfcgt, x > y)
SPU_DCMP(dfcmeq, std::fabs(x) == std::fabs(y))
SPU_DCMP(dfcmgt, std::fabs(x) > std::fabs(y))
#undef SPU_DCMP

// dftsv: test special value; I7 bits: 0x40 NaN, 0x20 +inf, 0x10 -inf, 0x08 +0, 0x04 -0,
// 0x02 +denormal, 0x01 -denormal. On the bits: DAZ would make a denormal compare as 0.
SLY4_SPU_INLINE bool dftsv_match(u64 v, u32 m) {
    const bool neg = v >> 63;
    const u64 e = (v >> 52) & 0x7FF, f = v & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) return f ? (m & 0x40) : (m & (neg ? 0x10 : 0x20));
    if (e == 0) return f ? (m & (neg ? 0x01 : 0x02)) : (m & (neg ? 0x04 : 0x08));
    return false;
}
SPU_SEM(dftsv, SpuReg r; for (int i = 0; i < 2; ++i) r.u64(i) = dftsv_match(RA.u64(i), u32(in.i7()) & 0x7F) ? ~0ull : 0ull; RT = r)

}  // namespace sly4::spusem
