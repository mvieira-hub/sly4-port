// PPU floating point core (see ppu_fpu.h).
//
// Host arithmetic (SSE2) runs under the rounding mode from FPSCR[RN] and the IEEE flags
// it raises are translated to FPSCR bits. Single-precision results are computed in
// double with round-to-odd and then rounded once to single, which gives the correctly
// rounded single result even for full double inputs (no double rounding).
#include "cpu/ppu_fpu.h"

#include <cfenv>
#include <cmath>
#include <cstring>

// GCC has no FENV_ACCESS pragma; the library is built with -frounding-math instead.

namespace sly4::fp {

namespace {

using u64 = uint64_t;
using u32 = uint32_t;
using s64 = int64_t;

constexpr u64 kDefaultQNaN = 0x7FF8000000000000ull;
constexpr u64 kQuietBit = 0x0008000000000000ull;
constexpr u32 kFpscrReserved = 1u << 11;  // ISA bit 20: always reads 0

u64 bits(double d) { u64 u; std::memcpy(&u, &d, 8); return u; }
double dbl(u64 u) { double d; std::memcpy(&d, &u, 8); return d; }
u32 fbits(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }

bool is_nan(u64 x) { return (x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (x & 0x000FFFFFFFFFFFFFull); }
bool is_snan(u64 x) { return is_nan(x) && !(x & kQuietBit); }
bool is_inf(u64 x) { return (x & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
bool is_zero(u64 x) { return (x & 0x7FFFFFFFFFFFFFFFull) == 0; }
bool sign(u64 x) { return x >> 63; }

// FPRF (C + FPCC) for a double result, or for a single result (classified as single).
u32 fprf_of(u64 x, bool single) {
    if (is_nan(x)) return 0x11;
    const bool neg = sign(x);
    if (is_inf(x)) return neg ? 0x09 : 0x05;
    if (is_zero(x)) return neg ? 0x12 : 0x02;
    bool denorm;
    if (single) {
        const int e = int((x >> 52) & 0x7FF) - 1023;
        denorm = e < -126;
    } else {
        denorm = ((x >> 52) & 0x7FF) == 0;
    }
    if (denorm) return neg ? 0x18 : 0x14;
    return neg ? 0x08 : 0x04;
}

int host_round(u32 f) {
    switch (f & fpscr::RN) {
        case 0: return FE_TONEAREST;
        case 1: return FE_TOWARDZERO;
        case 2: return FE_UPWARD;
        default: return FE_DOWNWARD;
    }
}

// Sets exception bits; FX only when one of them goes from 0 to 1 (Power ISA).
void raise(PPUContext& c, u32 bits_to_set) {
    const u32 newly = bits_to_set & ~c.fpscr;
    c.fpscr |= bits_to_set;
    if (newly & (fpscr::OX | fpscr::UX | fpscr::ZX | fpscr::XX | fpscr::VX_ALL)) c.fpscr |= fpscr::FX;
}
void recompute_summary(PPUContext& c) {
    u32& f = c.fpscr;
    f = (f & fpscr::VX_ALL) ? f | fpscr::VX : f & ~fpscr::VX;
    const bool fex = ((f & fpscr::VX) && (f & fpscr::VE)) || ((f & fpscr::OX) && (f & fpscr::OE)) ||
                     ((f & fpscr::UX) && (f & fpscr::UE)) || ((f & fpscr::ZX) && (f & fpscr::ZE)) ||
                     ((f & fpscr::XX) && (f & fpscr::XE));
    f = fex ? f | fpscr::FEX : f & ~fpscr::FEX;
}
void set_fprf(PPUContext& c, u32 v) { c.fpscr = (c.fpscr & ~fpscr::FPRF) | (v << 12); }
void set_fi_fr(PPUContext& c, bool fi, bool fr) {
    c.fpscr = (c.fpscr & ~(fpscr::FI | fpscr::FR)) | (fi ? fpscr::FI : 0) | (fr ? fpscr::FR : 0);
    if (fi) raise(c, fpscr::XX);
}

// Round-to-odd helper: the double result r was computed toward zero; if it was
// inexact, force the last bit to 1 so a later rounding to single is exact.
double make_odd(double r, bool inexact) { return inexact ? dbl(bits(r) | 1) : r; }

struct HostResult {
    double value;
    int flags;
};

double host_op(Arith k, double a, double b, double cc) {
    switch (k) {
        case Arith::Add: return a + b;
        case Arith::Sub: return a - b;
        case Arith::Mul: return a * cc;
        case Arith::Div: return a / b;
        case Arith::Sqrt: return std::sqrt(b);
        case Arith::MAdd: case Arith::NMAdd: return std::fma(a, cc, b);
        default: return std::fma(a, cc, -b);  // MSub, NMSub
    }
}

HostResult run_host(Arith k, double a, double b, double cc, int mode) {
    const int old = std::fegetround();
    std::fesetround(mode);
    std::feclearexcept(FE_ALL_EXCEPT);
    volatile double va = a, vb = b, vc = cc;
    const double r = host_op(k, va, vb, vc);
    const int fl = std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetround(old);
    return {r, fl};
}

}  // namespace

uint64_t single_to_double(uint32_t w) {
    const u64 s = u64(w >> 31) << 63;
    const u32 exp = (w >> 23) & 0xFF, frac = w & 0x7FFFFF;
    if (exp == 0xFF) return s | 0x7FF0000000000000ull | (u64(frac) << 29);
    if (exp) return s | (u64(exp - 127 + 1023) << 52) | (u64(frac) << 29);
    if (!frac) return s;
    // single denormal: normalise
    int e = -126;
    u32 f = frac;
    while (!(f & 0x800000)) {
        f <<= 1;
        --e;
    }
    return s | (u64(e + 1023) << 52) | (u64(f & 0x7FFFFF) << 29);
}

uint32_t double_to_single_store(uint64_t x) {
    const u32 exp = (x >> 52) & 0x7FF;
    if (exp > 896 || (x & 0x7FFFFFFFFFFFFFFFull) == 0)
        return u32((x >> 32) & 0xC0000000) | u32((x >> 29) & 0x3FFFFFFF);
    if (exp >= 874) {  // becomes a single denormal (truncated)
        const u64 frac = (1ull << 52) | (x & 0x000FFFFFFFFFFFFFull);
        return u32((x >> 32) & 0x80000000) | u32(frac >> (926 - exp));
    }
    return u32((x >> 32) & 0x80000000);  // undefined in the ISA: too small, store signed zero
}

void arith(PPUContext& c, uint32_t frt, uint64_t A, uint64_t B, uint64_t C, Arith k, bool single) {
    const bool uses_a = k != Arith::Sqrt;
    const bool uses_b = k != Arith::Mul;
    const bool uses_c = k == Arith::Mul || k >= Arith::MAdd;

    // NaN operands: VXSNAN for signalling ones; result = first NaN of A, B, C, quieted.
    if ((uses_a && is_nan(A)) || (uses_b && is_nan(B)) || (uses_c && is_nan(C))) {
        if ((uses_a && is_snan(A)) || (uses_b && is_snan(B)) || (uses_c && is_snan(C))) raise(c, fpscr::VXSNAN);
        u64 res = (uses_a && is_nan(A)) ? A : (uses_b && is_nan(B)) ? B : C;
        res |= kQuietBit;
        if (single) res = single_to_double(fbits(static_cast<float>(dbl(res))) | 0x00400000);
        c.fpr[frt] = res;
        set_fi_fr(c, false, false);
        set_fprf(c, 0x11);
        recompute_summary(c);
        return;
    }

    // Invalid operations that produce the default QNaN.
    u32 invalid = 0;
    const double a = dbl(A), b = dbl(B), cc = dbl(C);
    switch (k) {
        case Arith::Add: if (is_inf(A) && is_inf(B) && sign(A) != sign(B)) invalid = fpscr::VXISI; break;
        case Arith::Sub: if (is_inf(A) && is_inf(B) && sign(A) == sign(B)) invalid = fpscr::VXISI; break;
        case Arith::Mul: if ((is_inf(A) && is_zero(C)) || (is_zero(A) && is_inf(C))) invalid = fpscr::VXIMZ; break;
        case Arith::Div:
            if (is_inf(A) && is_inf(B)) invalid = fpscr::VXIDI;
            else if (is_zero(A) && is_zero(B)) invalid = fpscr::VXZDZ;
            break;
        case Arith::Sqrt: if (sign(B) && !is_zero(B)) invalid = fpscr::VXSQRT; break;
        default:
            if ((is_inf(A) && is_zero(C)) || (is_zero(A) && is_inf(C))) invalid = fpscr::VXIMZ;
            else if (is_inf(A) || is_inf(C)) {
                const bool prod_neg = sign(A) != sign(C);
                const bool b_neg = (k == Arith::MSub || k == Arith::NMSub) ? !sign(B) : sign(B);
                if (is_inf(B) && prod_neg != b_neg) invalid = fpscr::VXISI;
            }
            break;
    }
    if (invalid) {
        raise(c, invalid);
        c.fpr[frt] = kDefaultQNaN;
        set_fi_fr(c, false, false);
        set_fprf(c, 0x11);
        recompute_summary(c);
        return;
    }

    const int mode = host_round(c.fpscr);
    double res;
    int flags;
    bool fr = false;
    // The result truncated toward zero: |exact| < 2^Emin exactly when |truncated| < 2^Emin,
    // which is how PowerPC detects underflow (tiny *before* rounding; x86 checks after).
    const HostResult z = run_host(k, a, b, cc, FE_TOWARDZERO);
    if (!single) {
        const HostResult h = mode == FE_TOWARDZERO ? z : run_host(k, a, b, cc, mode);
        res = h.value;
        flags = h.flags;
        if (flags & FE_INEXACT) fr = std::fabs(res) != std::fabs(z.value);
    } else {
        // Toward zero in double, made odd, then a single rounding to float. An exact zero
        // takes its sign from the real rounding mode (x - x is -0 when rounding down).
        double odd = make_odd(z.value, z.flags & FE_INEXACT);
        if (z.value == 0 && !(z.flags & FE_INEXACT)) odd = run_host(k, a, b, cc, mode).value;
        const int old = std::fegetround();
        std::feclearexcept(FE_ALL_EXCEPT);
        std::fesetround(mode);
        volatile double vo = odd;
        const float f = static_cast<float>(vo);
        flags = std::fetestexcept(FE_ALL_EXCEPT) | (z.flags & (FE_INEXACT | FE_DIVBYZERO));
        std::fesetround(FE_TOWARDZERO);
        const float fz = static_cast<float>(vo);
        std::fesetround(old);
        res = static_cast<double>(f);
        fr = std::fabs(f) != std::fabs(fz);
    }
    // Negated forms negate after rounding (IBM: "identical to fmadd with the result negated").
    if (k == Arith::NMAdd || k == Arith::NMSub) res = -res;

    const double min_normal = single ? 1.17549435082228750797e-38 : 2.2250738585072014e-308;
    // nonzero exact result (truncation can reach 0 only when inexact) below 2^Emin
    const bool tiny = (z.value != 0 || (z.flags & FE_INEXACT)) && std::fabs(z.value) < min_normal;
    u32 exc = 0;
    if (flags & FE_DIVBYZERO) exc |= fpscr::ZX;
    if (flags & FE_OVERFLOW) exc |= fpscr::OX;
    if (tiny && (flags & FE_INEXACT)) exc |= fpscr::UX;
    raise(c, exc);
    c.fpr[frt] = bits(res);
    set_fi_fr(c, flags & FE_INEXACT, fr && (flags & FE_INEXACT));
    set_fprf(c, fprf_of(bits(res), single));
    recompute_summary(c);
}

void frsqrte(PPUContext& c, uint32_t frt, uint64_t B) {
    if (is_nan(B)) {
        if (is_snan(B)) raise(c, fpscr::VXSNAN);
        c.fpr[frt] = B | kQuietBit;
    } else if (sign(B) && !is_zero(B)) {
        raise(c, fpscr::VXSQRT);
        c.fpr[frt] = kDefaultQNaN;
    } else if (is_zero(B)) {
        raise(c, fpscr::ZX);
        c.fpr[frt] = sign(B) ? 0xFFF0000000000000ull : 0x7FF0000000000000ull;
    } else {
        c.set_f(frt, 1.0 / std::sqrt(dbl(B)));  // an estimate on hardware
    }
    set_fprf(c, fprf_of(c.fpr[frt], false));
    recompute_summary(c);
}

void frsp(PPUContext& c, uint32_t frt, uint64_t B) {
    if (is_nan(B)) {
        if (is_snan(B)) raise(c, fpscr::VXSNAN);
        c.fpr[frt] = single_to_double(fbits(static_cast<float>(dbl(B | kQuietBit))) | 0x00400000);
        set_fi_fr(c, false, false);
        set_fprf(c, 0x11);
    } else {
        const int mode = host_round(c.fpscr);
        const int old = std::fegetround();
        std::feclearexcept(FE_ALL_EXCEPT);
        std::fesetround(mode);
        volatile double vb = dbl(B);
        const float f = static_cast<float>(vb);
        const int fl = std::fetestexcept(FE_ALL_EXCEPT);
        std::fesetround(FE_TOWARDZERO);
        const float fz = static_cast<float>(vb);
        std::fesetround(old);
        u32 exc = 0;
        if (fl & FE_OVERFLOW) exc |= fpscr::OX;
        // tiny before rounding (see arith): the input itself is the exact value
        const double mag = std::fabs(dbl(B));
        if (mag != 0 && mag < 1.17549435082228750797e-38 && (fl & FE_INEXACT)) exc |= fpscr::UX;
        raise(c, exc);
        c.set_f(frt, static_cast<double>(f));
        set_fi_fr(c, fl & FE_INEXACT, (fl & FE_INEXACT) && std::fabs(f) != std::fabs(fz));
        set_fprf(c, fprf_of(c.fpr[frt], true));
    }
    recompute_summary(c);
}

void fcfid(PPUContext& c, uint32_t frt, uint64_t B) {
    const int old = std::fegetround();
    std::feclearexcept(FE_ALL_EXCEPT);
    std::fesetround(host_round(c.fpscr));
    volatile s64 v = static_cast<s64>(B);
    const double r = static_cast<double>(v);
    const int fl = std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetround(FE_TOWARDZERO);
    const double rz = static_cast<double>(v);
    std::fesetround(old);
    c.set_f(frt, r);
    set_fi_fr(c, fl & FE_INEXACT, (fl & FE_INEXACT) && std::fabs(r) != std::fabs(rz));
    set_fprf(c, fprf_of(c.fpr[frt], false));
    recompute_summary(c);
}

void convert_to_int(PPUContext& c, uint32_t frt, uint64_t B, bool word, bool toward_zero) {
    const double b = dbl(B);
    const s64 lo = word ? INT32_MIN : INT64_MIN;
    const s64 hi = word ? INT32_MAX : INT64_MAX;
    u64 res;
    bool fi = false, fr = false;
    if (is_nan(B)) {
        raise(c, fpscr::VXCVI | (is_snan(B) ? fpscr::VXSNAN : 0));
        res = word ? 0x80000000u : 0x8000000000000000ull;
    } else {
        const int mode = toward_zero ? FE_TOWARDZERO : host_round(c.fpscr);
        const int old = std::fegetround();
        std::fesetround(mode);
        volatile double vb = b;
        const double rounded = std::nearbyint(vb);
        std::fesetround(old);
        if (rounded < double(lo) || rounded > double(hi) || (!word && rounded >= 9223372036854775808.0)) {
            raise(c, fpscr::VXCVI);
            res = u64(rounded < 0 ? lo : hi);
            if (word) res &= 0xFFFFFFFFu;
        } else {
            const s64 v = static_cast<s64>(rounded);
            res = word ? u64(u32(v)) : u64(v);
            fi = rounded != b;
            fr = fi && std::fabs(rounded) > std::fabs(b);
        }
    }
    c.fpr[frt] = res;
    set_fi_fr(c, fi, fr);
    recompute_summary(c);
}

void fcmp(PPUContext& c, uint32_t bf, uint64_t A, uint64_t B, bool ordered) {
    u32 fpcc;
    if (is_nan(A) || is_nan(B)) {
        fpcc = 1;
        if (is_snan(A) || is_snan(B)) raise(c, fpscr::VXSNAN);
        if (ordered) raise(c, fpscr::VXVC);
    } else {
        const double x = dbl(A), y = dbl(B);
        fpcc = x < y ? 8 : x > y ? 4 : 2;
    }
    c.fpscr = (c.fpscr & ~(0xFu << 12)) | (fpcc << 12);
    c.set_crf(bf, fpcc);
    recompute_summary(c);
}

void mtfsf(PPUContext& c, uint32_t flm, uint64_t B) {
    const u32 v = uint32_t(B);
    for (u32 i = 0; i < 8; ++i)
        if (flm & (0x80 >> i)) {
            const u32 m = 0xF0000000u >> (4 * i);
            c.fpscr = (c.fpscr & ~m) | (v & m);
        }
    c.fpscr &= ~kFpscrReserved;
    recompute_summary(c);
}

void mtfsfi(PPUContext& c, uint32_t bf, uint32_t imm) {
    const u32 m = 0xF0000000u >> (4 * bf);
    c.fpscr = ((c.fpscr & ~m) | ((imm << 28) >> (4 * bf))) & ~kFpscrReserved;
    recompute_summary(c);
}

void mtfsb0(PPUContext& c, uint32_t bt) {
    c.fpscr &= ~(0x80000000u >> bt);
    recompute_summary(c);
}

void mtfsb1(PPUContext& c, uint32_t bt) {
    raise(c, (0x80000000u >> bt) & ~kFpscrReserved);
    recompute_summary(c);
}

void mcrfs(PPUContext& c, uint32_t bf, uint32_t bfa) {
    const u32 shift = 28 - 4 * bfa;
    c.set_crf(bf, (c.fpscr >> shift) & 0xF);
    // exception bits copied out are cleared (FEX and VX are summaries)
    const u32 clear = (0xFu << shift) & (fpscr::FX | fpscr::OX | fpscr::UX | fpscr::ZX | fpscr::XX | fpscr::VX_ALL);
    c.fpscr &= ~clear;
    recompute_summary(c);
}

}  // namespace sly4::fp
