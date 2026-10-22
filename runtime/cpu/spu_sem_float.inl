// SPU floating point. Included by spu_semantics.h; see the note there on precision.
#pragma once

namespace sly4::spusem {

// ---------------------------------------------------------------- single precision
#define SPU_FLOATS(name, expr) SPU_SEM(name, SpuReg r; for (int i = 0; i < 4; ++i) { \
    const float x = ftz(RA.f32(i)), y = ftz(RB.f32(i)); (void)x; (void)y; r.f32(i) = ftz(expr); } RT = r)
SPU_FLOATS(fa, x + y)
SPU_FLOATS(fs, x - y)
SPU_FLOATS(fm, x * y)
SPU_FLOATS(frest, 1.0f / x)
SPU_FLOATS(frsqest, 1.0f / std::sqrt(std::fabs(x)))
SPU_FLOATS(fi, y)  // RB holds the estimate from frest/frsqest, already exact here
#undef SPU_FLOATS

// RRR fused forms: C = A * B (+/-) T.
#define SPU_FMA(name, expr) SPU_SEM(name, SpuReg r; for (int i = 0; i < 4; ++i) { \
    const float x = ftz(RA.f32(i)), y = ftz(RB.f32(i)), z = ftz(RT.f32(i)); r.f32(i) = ftz(expr); } c.gpr[cc] = r)
SPU_FMA(fma, std::fma(x, y, z))
SPU_FMA(fms, std::fma(x, y, -z))
SPU_FMA(fnms, -std::fma(x, y, -z))
#undef SPU_FMA

#define SPU_FCMP(name, cond) SPU_WORDS(name, [&] { const float x = ftz(RA.f32(i)), y = ftz(RB.f32(i)); \
    return (cond) ? ~0u : 0u; }())
SPU_FCMP(fceq, x == y)
SPU_FCMP(fcgt, x > y)
SPU_FCMP(fcmeq, std::fabs(x) == std::fabs(y))
SPU_FCMP(fcmgt, std::fabs(x) > std::fabs(y))
#undef SPU_FCMP

// Conversions with a power-of-two scale: cflts/cfltu multiply by 2^(173 - u8) and
// saturate, csflt/cuflt divide by 2^(155 - u8).
inline u32 to_s32_sat(double v) {
    if (std::isnan(v)) return v < 0 ? 0x80000000u : 0x7FFFFFFFu;
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return u32(s32(v));
}
inline u32 to_u32_sat(double v) {
    if (std::isnan(v) || v <= 0) return 0;
    if (v >= 4294967295.0) return 0xFFFFFFFFu;
    return u32(v);
}
SPU_WORDS(cflts, to_s32_sat(std::ldexp(double(ftz(RA.f32(i))), 173 - int(in.u8()))))
SPU_WORDS(cfltu, to_u32_sat(std::ldexp(double(ftz(RA.f32(i))), 173 - int(in.u8()))))
SPU_SEM(csflt, SpuReg r; for (int i = 0; i < 4; ++i) r.f32(i) = ftz(float(std::ldexp(double(s32(RA.u32(i))), int(in.u8()) - 155))); RT = r)
SPU_SEM(cuflt, SpuReg r; for (int i = 0; i < 4; ++i) r.f32(i) = ftz(float(std::ldexp(double(RA.u32(i)), int(in.u8()) - 155))); RT = r)

// FPSCR: kept as written (exception flags are not modelled).
SPU_SEM(fscrrd, for (int i = 0; i < 4; ++i) RT.u32(i) = c.fpscr[i])
SPU_SEM(fscrwr, for (int i = 0; i < 4; ++i) c.fpscr[i] = RA.u32(i))

// ---------------------------------------------------------------- double precision
#define SPU_DOUBLES(name, expr) SPU_SEM(name, SpuReg r; for (int i = 0; i < 2; ++i) { \
    const double x = RA.f64(i), y = RB.f64(i), z = RT.f64(i); (void)z; r.f64(i) = dnan(expr); } RT = r)
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

#define SPU_DCMP(name, cond) SPU_SEM(name, SpuReg r; for (int i = 0; i < 2; ++i) { \
    const double x = RA.f64(i), y = RB.f64(i); r.u64(i) = (cond) ? ~0ull : 0ull; } RT = r)
SPU_DCMP(dfceq, x == y)
SPU_DCMP(dfcgt, x > y)
SPU_DCMP(dfcmeq, std::fabs(x) == std::fabs(y))
SPU_DCMP(dfcmgt, std::fabs(x) > std::fabs(y))
#undef SPU_DCMP

// dftsv: test special value; I7 bits: 0x40 NaN, 0x20 +inf, 0x10 -inf, 0x08 +0, 0x04 -0,
// 0x02 +denormal, 0x01 -denormal.
inline bool dftsv_match(double v, u32 m) {
    const bool neg = std::signbit(v);
    switch (std::fpclassify(v)) {
        case FP_NAN: return m & 0x40;
        case FP_INFINITE: return m & (neg ? 0x10 : 0x20);
        case FP_ZERO: return m & (neg ? 0x04 : 0x08);
        case FP_SUBNORMAL: return m & (neg ? 0x01 : 0x02);
        default: return false;
    }
}
SPU_SEM(dftsv, SpuReg r; for (int i = 0; i < 2; ++i) r.u64(i) = dftsv_match(RA.f64(i), u32(in.i7()) & 0x7F) ? ~0ull : 0ull; RT = r)

}  // namespace sly4::spusem
