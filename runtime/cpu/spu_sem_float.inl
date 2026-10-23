// SPU floating point. Included by spu_semantics.h; see the note there on precision.
#pragma once

namespace sly4::spusem {

// ---------------------------------------------------------------- single precision
// 2^e as a double (|e| well inside the normal range here): multiplying by it is exact,
// like the std::ldexp it replaces, which was a library call (1.5% of an SPU thread).
SLY4_SPU_INLINE double pow2(int e) { return std::bit_cast<double>(u64(e + 1023) << 52); }

// The SPU's single precision has no infinities or NaNs: exponent 255 is one more binade
// (up to ~6.8e38, 0x7FFFFFFF being the largest value) and results that overflow saturate
// there. Game code relies on it, e.g. 0x7FFFFFFF as the starting "best distance" of a
// nearest-pair search (a NaN on the host: no distance compared below it).
// The arithmetic runs on host floats, and only lanes that came out infinite or NaN (some
// operand had exponent 255, or the result overflowed) are redone exactly in double.
SLY4_SPU_INLINE double xf_in(u32 v) {
    const u32 e = (v >> 23) & 0xFF;
    const double m = e ? double(0x800000u | (v & 0x7FFFFFu)) * pow2(int(e) - 150) : 0.0;
    return v >> 31 ? -m : m;
}
SLY4_SPU_INLINE u32 xf_out(double d) {
    const u32 sign = std::signbit(d) ? 0x80000000u : 0;
    const double m = std::fabs(d);
    if (m >= 0x1p127) {  // halved into the host's range, then the exponent put back
        const u32 h = std::bit_cast<u32>(float(m * 0.5));
        return sign | (h >= 0x7F800000u ? 0x7FFFFFFFu : h + 0x00800000u);
    }
    const u32 f = std::bit_cast<u32>(float(m));
    return sign | (f & 0x7F800000u ? f : 0);
}
SLY4_SPU_INLINE bool xf_special(const SpuReg& r) {
    bool any = false;
    for (int i = 0; i < 4; ++i) any |= (r.w[i] & 0x7F800000u) == 0x7F800000u;
    return any;
}

// Denormal operands and results are zero through DAZ/FTZ (SpuFloatMode).
#define SPU_FLOATS(name, expr) SPU_SEM(name, SpuReg r; for (int i = 3; i >= 0; --i) { \
    const float x = RA.f32(i), y = RB.f32(i); (void)x; (void)y; r.f32(i) = expr; } \
    if (xf_special(r)) [[unlikely]] for (int i = 3; i >= 0; --i) if ((r.u32(i) & 0x7F800000u) == 0x7F800000u) { \
        const double x = xf_in(RA.u32(i)), y = xf_in(RB.u32(i)); (void)y; r.u32(i) = xf_out(expr); } \
    RT = r)
SPU_FLOATS(fa, x + y)
SPU_FLOATS(fs, x - y)
SPU_FLOATS(fm, x * y)
SPU_FLOATS(frest, 1.0f / x)
SPU_FLOATS(frsqest, 1.0f / std::sqrt(std::fabs(x)))
#undef SPU_FLOATS
SPU_WORDS(fi, RB.u32(i) & 0x7F800000u ? RB.u32(i) : RB.u32(i) & 0x80000000u)  // RB holds the exact estimate

// RRR fused forms: C = A * B (+/-) T.
#define SPU_FMA(name, expr) SPU_SEM(name, SpuReg r; for (int i = 3; i >= 0; --i) { \
    const float x = RA.f32(i), y = RB.f32(i), z = RT.f32(i); r.f32(i) = expr; } \
    if (xf_special(r)) [[unlikely]] for (int i = 3; i >= 0; --i) if ((r.u32(i) & 0x7F800000u) == 0x7F800000u) { \
        const double x = xf_in(RA.u32(i)), y = xf_in(RB.u32(i)), z = xf_in(RT.u32(i)); r.u32(i) = xf_out(expr); } \
    c.gpr[cc] = r)
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
SPU_WORDS(cflts, to_s32_sat(xf_in(RA.u32(i)) * pow2(173 - int(in.u8()))))
SPU_WORDS(cfltu, to_u32_sat(xf_in(RA.u32(i)) * pow2(173 - int(in.u8()))))
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
