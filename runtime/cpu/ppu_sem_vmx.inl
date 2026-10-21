// VMX (AltiVec) + Cell lvlx/lvrx family semantics (see ppu_semantics.h).
//
// Every operation works on arrays in guest element order (element 0 first), converted
// from/to the byte-reversed Vec128 register layout. Floating point follows the VMX rules:
// round to nearest, no exceptions, NaN results are the first NaN operand made quiet
// (default 0x7FC00000), and with VSCR[NJ] (the PS3 default) denormal inputs and results
// are flushed to zero.
#pragma once

#include <cmath>
#include <limits>

namespace sly4::sem {

namespace vmx {

using u8 = uint8_t;
using s8 = int8_t;
using u16 = uint16_t;
using s16 = int16_t;

template <class T>
struct Lanes {
    static constexpr int N = 16 / sizeof(T);
    T v[N];
    T& operator[](int i) { return v[i]; }
    const T& operator[](int i) const { return v[i]; }
};

// Guest element i lives in host lane N-1-i (see Vec128).
template <class T>
inline Lanes<T> get(const Vec128& x) {
    Lanes<T> r;
    for (int i = 0; i < Lanes<T>::N; ++i) std::memcpy(&r[i], x.b + (Lanes<T>::N - 1 - i) * sizeof(T), sizeof(T));
    return r;
}
template <class T>
inline Vec128 put(const Lanes<T>& l) {
    Vec128 x;
    for (int i = 0; i < Lanes<T>::N; ++i) std::memcpy(x.b + (Lanes<T>::N - 1 - i) * sizeof(T), &l[i], sizeof(T));
    return x;
}
template <class T, class F>
inline Vec128 map2(const Vec128& a, const Vec128& b, F f) {
    const Lanes<T> x = get<T>(a), y = get<T>(b);
    Lanes<T> r;
    for (int i = 0; i < Lanes<T>::N; ++i) r[i] = f(x[i], y[i]);
    return put(r);
}
template <class T>
inline T saturate(s64 v, bool& sat) {
    constexpr s64 lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    if (v < lo) { sat = true; return T(lo); }
    if (v > hi) { sat = true; return T(hi); }
    return T(v);
}

inline u32 fb(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }
inline float bf(u32 u) { float f; std::memcpy(&f, &u, 4); return f; }
constexpr u32 kDefaultNaN = 0x7FC00000;

struct Flt {
    bool nj;
    float in(float x) const { return (nj && std::fpclassify(x) == FP_SUBNORMAL) ? std::copysign(0.0f, x) : x; }
    float out(float x) const { return (nj && std::fpclassify(x) == FP_SUBNORMAL) ? std::copysign(0.0f, x) : x; }
    static float quiet(float x) { return bf(fb(x) | 0x00400000); }
};

// Lane-wise float ops with NaN propagation (first NaN of the operands, quieted).
template <class F>
inline Vec128 fmap2(const PPUContext& c, const Vec128& A, const Vec128& B, F f) {
    const Flt fl{(c.vscr & vscr::NJ) != 0};
    return map2<u32>(A, B, [&](u32 x, u32 y) {
        const float p = fl.in(bf(x)), q = fl.in(bf(y));
        if (std::isnan(p)) return fb(Flt::quiet(p));
        if (std::isnan(q)) return fb(Flt::quiet(q));
        const float r = f(p, q);
        return std::isnan(r) ? kDefaultNaN : fb(fl.out(r));
    });
}
template <class F>
inline Vec128 fmap1(const PPUContext& c, const Vec128& B, F f) {
    const Flt fl{(c.vscr & vscr::NJ) != 0};
    Lanes<u32> x = get<u32>(B);
    for (int i = 0; i < 4; ++i) {
        const float p = fl.in(bf(x[i]));
        if (std::isnan(p)) { x[i] = fb(Flt::quiet(p)); continue; }
        const float r = f(p);
        x[i] = std::isnan(r) ? kDefaultNaN : fb(fl.out(r));
    }
    return put(x);
}
// Compare result to vD; Rc sets CR6 (all true: 8, all false: 2).
inline void compare(PPUContext& c, const Insn& in, const Vec128& res, u32 elem_size) {
    c.vr[in.rd()] = res;
    if (in.vc_rc()) {
        bool all = true, none = true;
        for (u32 i = 0; i < 16; i += elem_size) {
            if (res.b[i]) none = false;
            else all = false;
        }
        c.set_crf(6, (all ? 8u : 0u) | (none ? 2u : 0u));
    }
}
template <class T>
inline T float_to_int_sat(float x, u32 scale, bool& sat) {
    if (std::isnan(x)) return 0;
    const double v = std::trunc(double(x) * std::ldexp(1.0, int(scale)));
    constexpr double lo = double(std::numeric_limits<T>::min()), hi = double(std::numeric_limits<T>::max());
    if (v < lo) { sat = true; return std::numeric_limits<T>::min(); }
    if (v > hi) { sat = true; return std::numeric_limits<T>::max(); }
    return T(v);
}
inline GuestAddr ea(const PPUContext& c, const Insn& in) { return ea_x(c, in); }

}  // namespace vmx

// SLY4_VSEM(name, statements...): like SLY4_SEM, with A/B/C = vA/vB/vC values, v = the
// VR file, and `sat` merged into VSCR[SAT] at the end.
#define SLY4_VSEM(name, ...)                                                                     \
    template <>                                                                                  \
    inline PpuStep op<Op::name>(PPUContext & c, GuestMemory & mem, Insn in) {                   \
        using namespace vmx;                                                                     \
        [[maybe_unused]] const u32 d = in.rd(), a = in.ra(), b = in.rb();                       \
        [[maybe_unused]] Vec128* const v = c.vr;                                                \
        [[maybe_unused]] const Vec128 A = v[a], B = v[b], C = v[in.rc()];                       \
        [[maybe_unused]] bool sat = false;                                                      \
        (void)mem;                                                                               \
        __VA_ARGS__;                                                                             \
        if (sat) c.vscr |= vscr::SAT;                                                            \
        return PpuStep::Ok;                                                                      \
    }

// ---------------- loads / stores
// element loads read the whole aligned quadword, as the hardware does (RPCS3)
SLY4_VSEM(lvx, v[d] = mem.read128(ea(c, in) & ~15u))
SLY4_VSEM(lvxl, v[d] = mem.read128(ea(c, in) & ~15u))
SLY4_VSEM(lvebx, v[d] = mem.read128(ea(c, in) & ~15u))
SLY4_VSEM(lvehx, v[d] = mem.read128(ea(c, in) & ~15u))
SLY4_VSEM(lvewx, v[d] = mem.read128(ea(c, in) & ~15u))
SLY4_VSEM(stvx, mem.write128(ea(c, in) & ~15u, v[d]))
SLY4_VSEM(stvxl, mem.write128(ea(c, in) & ~15u, v[d]))
SLY4_VSEM(stvebx, const GuestAddr e = ea(c, in); mem.write<u8>(e, get<u8>(v[d])[e & 15]))
SLY4_VSEM(stvehx, const GuestAddr e = ea(c, in) & ~1u; mem.write<u16>(e, get<u16>(v[d])[(e & 15) / 2]))
SLY4_VSEM(stvewx, const GuestAddr e = ea(c, in) & ~3u; mem.write<u32>(e, get<u32>(v[d])[(e & 15) / 4]))
SLY4_VSEM(lvsl, const u32 sh = ea(c, in) & 15; Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = u8(sh + i);
          v[d] = put(r))
SLY4_VSEM(lvsr, const u32 sh = ea(c, in) & 15; Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = u8(16 - sh + i);
          v[d] = put(r))
// Cell: bytes from EA to the end of its block, left-justified / from the block start up
// to EA, right-justified; zero fill.
#define SLY4_LVLX(name)                                                                          \
    SLY4_VSEM(name, const GuestAddr e = ea(c, in), off = e & 15; Lanes<u8> r{};                 \
              for (u32 i = 0; i < 16 - off; ++i) r[i] = mem.read<u8>(e + i); v[d] = put(r))
#define SLY4_LVRX(name)                                                                          \
    SLY4_VSEM(name, const GuestAddr e = ea(c, in), off = e & 15, blk = e & ~15u; Lanes<u8> r{};  \
              for (u32 i = 0; i < off; ++i) r[16 - off + i] = mem.read<u8>(blk + i); v[d] = put(r))
#define SLY4_STVLX(name)                                                                         \
    SLY4_VSEM(name, const GuestAddr e = ea(c, in), off = e & 15; const Lanes<u8> s = get<u8>(v[d]); \
              for (u32 i = 0; i < 16 - off; ++i) mem.write<u8>(e + i, s[i]))
#define SLY4_STVRX(name)                                                                         \
    SLY4_VSEM(name, const GuestAddr e = ea(c, in), off = e & 15, blk = e & ~15u;                \
              const Lanes<u8> s = get<u8>(v[d]);                                                 \
              for (u32 i = 0; i < off; ++i) mem.write<u8>(blk + i, s[16 - off + i]))
SLY4_LVLX(lvlx)
SLY4_LVLX(lvlxl)
SLY4_LVRX(lvrx)
SLY4_LVRX(lvrxl)
SLY4_STVLX(stvlx)
SLY4_STVLX(stvlxl)
SLY4_STVRX(stvrx)
SLY4_STVRX(stvrxl)
#undef SLY4_LVLX
#undef SLY4_LVRX
#undef SLY4_STVLX
#undef SLY4_STVRX

// ---------------- VSCR
SLY4_VSEM(mfvscr, Lanes<u32> r{}; r[3] = c.vscr; v[d] = put(r))
SLY4_VSEM(mtvscr, c.vscr = get<u32>(B)[3] & (vscr::NJ | vscr::SAT))

// ---------------- logical, select, permute
SLY4_VSEM(vand, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8(A.b[i] & B.b[i]); v[d] = r)
SLY4_VSEM(vandc, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8(A.b[i] & ~B.b[i]); v[d] = r)
SLY4_VSEM(vor, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8(A.b[i] | B.b[i]); v[d] = r)
SLY4_VSEM(vxor, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8(A.b[i] ^ B.b[i]); v[d] = r)
SLY4_VSEM(vnor, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8(~(A.b[i] | B.b[i])); v[d] = r)
SLY4_VSEM(vsel, Vec128 r; for (int i = 0; i < 16; ++i) r.b[i] = u8((B.b[i] & C.b[i]) | (A.b[i] & ~C.b[i])); v[d] = r)
SLY4_VSEM(vperm, const Lanes<u8> x = get<u8>(A), y = get<u8>(B), s = get<u8>(C); Lanes<u8> r;
          for (int i = 0; i < 16; ++i) { const u32 k = s[i] & 31; r[i] = k < 16 ? x[k] : y[k - 16]; } v[d] = put(r))
SLY4_VSEM(vsldoi, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); Lanes<u8> r; const u32 sh = in.vshb();
          for (u32 i = 0; i < 16; ++i) r[i] = i + sh < 16 ? x[i + sh] : y[i + sh - 16]; v[d] = put(r))

// ---------------- integer add/sub (modulo, carry, saturating)
SLY4_VSEM(vaddubm, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x + y); }))
SLY4_VSEM(vadduhm, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x + y); }))
SLY4_VSEM(vadduwm, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x + y; }))
SLY4_VSEM(vsububm, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x - y); }))
SLY4_VSEM(vsubuhm, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x - y); }))
SLY4_VSEM(vsubuwm, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x - y; }))
SLY4_VSEM(vaddcuw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((u64(x) + y) >> 32); }))
SLY4_VSEM(vsubcuw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32(x >= y); }))
SLY4_VSEM(vaddubs, v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return saturate<u8>(s64(x) + y, sat); }))
SLY4_VSEM(vadduhs, v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return saturate<u16>(s64(x) + y, sat); }))
SLY4_VSEM(vadduws, v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return saturate<u32>(s64(x) + y, sat); }))
SLY4_VSEM(vaddsbs, v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return u8(saturate<s8>(s64(s8(x)) + s8(y), sat)); }))
SLY4_VSEM(vaddshs, v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return u16(saturate<s16>(s64(s16(x)) + s16(y), sat)); }))
SLY4_VSEM(vaddsws, v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return u32(saturate<s32>(s64(s32(x)) + s32(y), sat)); }))
SLY4_VSEM(vsububs, v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return saturate<u8>(s64(x) - y, sat); }))
SLY4_VSEM(vsubuhs, v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return saturate<u16>(s64(x) - y, sat); }))
SLY4_VSEM(vsubuws, v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return saturate<u32>(s64(x) - y, sat); }))
SLY4_VSEM(vsubsbs, v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return u8(saturate<s8>(s64(s8(x)) - s8(y), sat)); }))
SLY4_VSEM(vsubshs, v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return u16(saturate<s16>(s64(s16(x)) - s16(y), sat)); }))
SLY4_VSEM(vsubsws, v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return u32(saturate<s32>(s64(s32(x)) - s32(y), sat)); }))

// ---------------- max / min / average
SLY4_VSEM(vmaxub, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return x > y ? x : y; }))
SLY4_VSEM(vmaxuh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return x > y ? x : y; }))
SLY4_VSEM(vmaxuw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x > y ? x : y; }))
SLY4_VSEM(vmaxsb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return s8(x) > s8(y) ? x : y; }))
SLY4_VSEM(vmaxsh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return s16(x) > s16(y) ? x : y; }))
SLY4_VSEM(vmaxsw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) > s32(y) ? x : y; }))
SLY4_VSEM(vminub, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return x < y ? x : y; }))
SLY4_VSEM(vminuh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return x < y ? x : y; }))
SLY4_VSEM(vminuw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x < y ? x : y; }))
SLY4_VSEM(vminsb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return s8(x) < s8(y) ? x : y; }))
SLY4_VSEM(vminsh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return s16(x) < s16(y) ? x : y; }))
SLY4_VSEM(vminsw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) < s32(y) ? x : y; }))
SLY4_VSEM(vavgub, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8((u32(x) + y + 1) >> 1); }))
SLY4_VSEM(vavguh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16((u32(x) + y + 1) >> 1); }))
SLY4_VSEM(vavguw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((u64(x) + y + 1) >> 1); }))
SLY4_VSEM(vavgsb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8((s32(s8(x)) + s8(y) + 1) >> 1); }))
SLY4_VSEM(vavgsh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16((s32(s16(x)) + s16(y) + 1) >> 1); }))
SLY4_VSEM(vavgsw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((s64(s32(x)) + s32(y) + 1) >> 1); }))

// ---------------- shifts and rotates (element-wise, count modulo element width)
SLY4_VSEM(vrlb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { const u32 n = y & 7; return u8((x << n) | (x >> ((8 - n) & 7))); }))
SLY4_VSEM(vrlh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { const u32 n = y & 15; return u16((x << n) | (x >> ((16 - n) & 15))); }))
SLY4_VSEM(vrlw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { const u32 n = y & 31; return n ? (x << n) | (x >> (32 - n)) : x; }))
SLY4_VSEM(vslb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x << (y & 7)); }))
SLY4_VSEM(vslh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x << (y & 15)); }))
SLY4_VSEM(vslw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x << (y & 31); }))
SLY4_VSEM(vsrb, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x >> (y & 7)); }))
SLY4_VSEM(vsrh, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x >> (y & 15)); }))
SLY4_VSEM(vsrw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x >> (y & 31); }))
SLY4_VSEM(vsrab, v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(s8(x) >> (y & 7)); }))
SLY4_VSEM(vsrah, v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(s16(x) >> (y & 15)); }))
SLY4_VSEM(vsraw, v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32(s32(x) >> (y & 31)); }))
// whole-register shifts: by 0..7 bits (vsl/vsr) or by octets (vslo/vsro), from vB's last byte
SLY4_VSEM(vsl, const u32 n = get<u8>(B)[15] & 7; const Lanes<u8> x = get<u8>(A); Lanes<u8> r;
          for (int i = 0; i < 16; ++i) r[i] = u8((x[i] << n) | (i < 15 && n ? x[i + 1] >> (8 - n) : 0)); v[d] = put(r))
SLY4_VSEM(vsr, const u32 n = get<u8>(B)[15] & 7; const Lanes<u8> x = get<u8>(A); Lanes<u8> r;
          for (int i = 0; i < 16; ++i) r[i] = u8((x[i] >> n) | (i > 0 && n ? x[i - 1] << (8 - n) : 0)); v[d] = put(r))
SLY4_VSEM(vslo, const u32 n = (get<u8>(B)[15] >> 3) & 15; const Lanes<u8> x = get<u8>(A); Lanes<u8> r{};
          for (u32 i = 0; i < 16; ++i) r[i] = i + n < 16 ? x[i + n] : 0; v[d] = put(r))
SLY4_VSEM(vsro, const u32 n = (get<u8>(B)[15] >> 3) & 15; const Lanes<u8> x = get<u8>(A); Lanes<u8> r{};
          for (u32 i = 0; i < 16; ++i) r[i] = i >= n ? x[i - n] : 0; v[d] = put(r))

// ---------------- merge, splat, pack, unpack
template <class T>
inline Vec128 merge(const Vec128& A, const Vec128& B, bool high) {
    using namespace vmx;
    const Lanes<T> x = get<T>(A), y = get<T>(B);
    constexpr int H = Lanes<T>::N / 2;
    const int base = high ? 0 : H;
    Lanes<T> r;
    for (int i = 0; i < H; ++i) {
        r[2 * i] = x[base + i];
        r[2 * i + 1] = y[base + i];
    }
    return put(r);
}
SLY4_VSEM(vmrghb, v[d] = merge<u8>(A, B, true))
SLY4_VSEM(vmrglb, v[d] = merge<u8>(A, B, false))
SLY4_VSEM(vmrghh, v[d] = merge<u16>(A, B, true))
SLY4_VSEM(vmrglh, v[d] = merge<u16>(A, B, false))
SLY4_VSEM(vmrghw, v[d] = merge<u32>(A, B, true))
SLY4_VSEM(vmrglw, v[d] = merge<u32>(A, B, false))
SLY4_VSEM(vspltb, const u8 e = get<u8>(B)[in.vuimm() & 15]; Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = e; v[d] = put(r))
SLY4_VSEM(vsplth, const u16 e = get<u16>(B)[in.vuimm() & 7]; Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = e; v[d] = put(r))
SLY4_VSEM(vspltw, const u32 e = get<u32>(B)[in.vuimm() & 3]; Lanes<u32> r; for (int i = 0; i < 4; ++i) r[i] = e; v[d] = put(r))
SLY4_VSEM(vspltisb, Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = u8(in.vsimm()); v[d] = put(r))
SLY4_VSEM(vspltish, Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = u16(in.vsimm()); v[d] = put(r))
SLY4_VSEM(vspltisw, Lanes<u32> r; for (int i = 0; i < 4; ++i) r[i] = u32(in.vsimm()); v[d] = put(r))

// pack halfwords -> bytes: kind 0 modulo, 1 unsigned sat, 2 signed->unsigned sat, 3 signed sat
inline Vec128 pack16(const Vec128& A, const Vec128& B, int kind, bool& sat) {
    using namespace vmx;
    const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
    Lanes<u8> r;
    for (int i = 0; i < 16; ++i) {
        const u16 e = i < 8 ? x[i] : y[i - 8];
        r[i] = kind == 0 ? u8(e) : kind == 1 ? saturate<u8>(e, sat) : kind == 2 ? saturate<u8>(s16(e), sat)
                                                                          : u8(saturate<s8>(s16(e), sat));
    }
    return put(r);
}
inline Vec128 pack32(const Vec128& A, const Vec128& B, int kind, bool& sat) {
    using namespace vmx;
    const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
    Lanes<u16> r;
    for (int i = 0; i < 8; ++i) {
        const u32 e = i < 4 ? x[i] : y[i - 4];
        r[i] = kind == 0 ? u16(e) : kind == 1 ? saturate<u16>(e, sat) : kind == 2 ? saturate<u16>(s32(e), sat)
                                                                            : u16(saturate<s16>(s32(e), sat));
    }
    return put(r);
}
SLY4_VSEM(vpkuhum, v[d] = pack16(A, B, 0, sat))
SLY4_VSEM(vpkuhus, v[d] = pack16(A, B, 1, sat))
SLY4_VSEM(vpkshus, v[d] = pack16(A, B, 2, sat))
SLY4_VSEM(vpkshss, v[d] = pack16(A, B, 3, sat))
SLY4_VSEM(vpkuwum, v[d] = pack32(A, B, 0, sat))
SLY4_VSEM(vpkuwus, v[d] = pack32(A, B, 1, sat))
SLY4_VSEM(vpkswus, v[d] = pack32(A, B, 2, sat))
SLY4_VSEM(vpkswss, v[d] = pack32(A, B, 3, sat))
SLY4_VSEM(vpkpx, const Lanes<u32> x = get<u32>(A), y = get<u32>(B); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) {
              const u32 e = i < 4 ? x[i] : y[i - 4];
              r[i] = u16(((e >> 9) & 0x8000) | ((e >> 9) & 0x7C00) | ((e >> 6) & 0x03E0) | ((e >> 3) & 0x001F));
          } v[d] = put(r))
SLY4_VSEM(vupkhsb, const Lanes<u8> x = get<u8>(B); Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = u16(s16(s8(x[i])));
          v[d] = put(r))
SLY4_VSEM(vupklsb, const Lanes<u8> x = get<u8>(B); Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = u16(s16(s8(x[8 + i])));
          v[d] = put(r))
SLY4_VSEM(vupkhsh, const Lanes<u16> x = get<u16>(B); Lanes<u32> r; for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[i])));
          v[d] = put(r))
SLY4_VSEM(vupklsh, const Lanes<u16> x = get<u16>(B); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[4 + i]))); v[d] = put(r))
inline Vec128 unpack_pixel(const Vec128& B, int base) {
    using namespace vmx;
    const Lanes<u16> x = get<u16>(B);
    Lanes<u32> r;
    for (int i = 0; i < 4; ++i) {
        const u16 p = x[base + i];
        r[i] = ((p & 0x8000) ? 0xFF000000u : 0) | (u32((p >> 10) & 31) << 16) | (u32((p >> 5) & 31) << 8) | u32(p & 31);
    }
    return put(r);
}
SLY4_VSEM(vupkhpx, v[d] = unpack_pixel(B, 0))
SLY4_VSEM(vupklpx, v[d] = unpack_pixel(B, 4))

// ---------------- multiply / multiply-sum / sum
SLY4_VSEM(vmuleub, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(u32(x[2 * i]) * y[2 * i]); v[d] = put(r))
SLY4_VSEM(vmuloub, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(u32(x[2 * i + 1]) * y[2 * i + 1]); v[d] = put(r))
SLY4_VSEM(vmulesb, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(s32(s8(x[2 * i])) * s8(y[2 * i])); v[d] = put(r))
SLY4_VSEM(vmulosb, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(s32(s8(x[2 * i + 1])) * s8(y[2 * i + 1])); v[d] = put(r))
SLY4_VSEM(vmuleuh, const Lanes<u16> x = get<u16>(A), y = get<u16>(B); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(x[2 * i]) * y[2 * i]; v[d] = put(r))
SLY4_VSEM(vmulouh, const Lanes<u16> x = get<u16>(A), y = get<u16>(B); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(x[2 * i + 1]) * y[2 * i + 1]; v[d] = put(r))
SLY4_VSEM(vmulesh, const Lanes<u16> x = get<u16>(A), y = get<u16>(B); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[2 * i])) * s16(y[2 * i])); v[d] = put(r))
SLY4_VSEM(vmulosh, const Lanes<u16> x = get<u16>(A), y = get<u16>(B); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[2 * i + 1])) * s16(y[2 * i + 1])); v[d] = put(r))
SLY4_VSEM(vmhaddshs, const Lanes<u16> x = get<u16>(A), y = get<u16>(B), z = get<u16>(C); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(saturate<s16>(((s32(s16(x[i])) * s16(y[i])) >> 15) + s16(z[i]), sat));
          v[d] = put(r))
SLY4_VSEM(vmhraddshs, const Lanes<u16> x = get<u16>(A), y = get<u16>(B), z = get<u16>(C); Lanes<u16> r;
          for (int i = 0; i < 8; ++i)
              r[i] = u16(saturate<s16>(((s32(s16(x[i])) * s16(y[i]) + 0x4000) >> 15) + s16(z[i]), sat));
          v[d] = put(r))
SLY4_VSEM(vmladduhm, const Lanes<u16> x = get<u16>(A), y = get<u16>(B), z = get<u16>(C); Lanes<u16> r;
          for (int i = 0; i < 8; ++i) r[i] = u16(u32(x[i]) * y[i] + z[i]); v[d] = put(r))
SLY4_VSEM(vmsumubm, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); const Lanes<u32> z = get<u32>(C); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) { s64 s = z[i]; for (int k = 0; k < 4; ++k) s += s64(x[4 * i + k]) * y[4 * i + k];
                                        r[i] = u32(s); } v[d] = put(r))
SLY4_VSEM(vmsummbm, const Lanes<u8> x = get<u8>(A), y = get<u8>(B); const Lanes<u32> z = get<u32>(C); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) { s64 s = z[i]; for (int k = 0; k < 4; ++k) s += s64(s8(x[4 * i + k])) * y[4 * i + k];
                                        r[i] = u32(s); } v[d] = put(r))
// halfword multiply-sum: kind 0 uhm, 1 uhs, 2 shm, 3 shs
inline Vec128 msum16(const Vec128& A, const Vec128& B, const Vec128& C, int kind, bool& sat) {
    using namespace vmx;
    const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
    const Lanes<u32> z = get<u32>(C);
    const bool sgn = kind >= 2;
    Lanes<u32> r;
    for (int i = 0; i < 4; ++i) {
        s64 s = sgn ? s64(s32(z[i])) : s64(z[i]);
        for (int k = 0; k < 2; ++k)
            s += sgn ? s64(s16(x[2 * i + k])) * s16(y[2 * i + k]) : s64(x[2 * i + k]) * y[2 * i + k];
        r[i] = kind == 1 ? saturate<u32>(s, sat) : kind == 3 ? u32(saturate<s32>(s, sat)) : u32(s);
    }
    return put(r);
}
SLY4_VSEM(vmsumuhm, v[d] = msum16(A, B, C, 0, sat))
SLY4_VSEM(vmsumuhs, v[d] = msum16(A, B, C, 1, sat))
SLY4_VSEM(vmsumshm, v[d] = msum16(A, B, C, 2, sat))
SLY4_VSEM(vmsumshs, v[d] = msum16(A, B, C, 3, sat))
SLY4_VSEM(vsum4ubs, const Lanes<u32> y = get<u32>(B); const Lanes<u8> x = get<u8>(A); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) { s64 s = s64(y[i]); for (int k = 0; k < 4; ++k) s += x[4 * i + k];
                                        r[i] = saturate<u32>(s, sat); } v[d] = put(r))
SLY4_VSEM(vsum4sbs, const Lanes<u32> y = get<u32>(B); const Lanes<u8> x = get<u8>(A); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) { s64 s = s32(y[i]); for (int k = 0; k < 4; ++k) s += s8(x[4 * i + k]);
                                        r[i] = u32(saturate<s32>(s, sat)); } v[d] = put(r))
SLY4_VSEM(vsum4shs, const Lanes<u32> y = get<u32>(B); const Lanes<u16> x = get<u16>(A); Lanes<u32> r;
          for (int i = 0; i < 4; ++i) r[i] = u32(saturate<s32>(s32(y[i]) + s64(s16(x[2 * i])) + s16(x[2 * i + 1]), sat));
          v[d] = put(r))
SLY4_VSEM(vsum2sws, const Lanes<u32> x = get<u32>(A), y = get<u32>(B); Lanes<u32> r{};
          for (int i = 0; i < 2; ++i)
              r[2 * i + 1] = u32(saturate<s32>(s64(s32(x[2 * i])) + s32(x[2 * i + 1]) + s32(y[2 * i + 1]), sat));
          v[d] = put(r))
SLY4_VSEM(vsumsws, const Lanes<u32> x = get<u32>(A), y = get<u32>(B); Lanes<u32> r{}; s64 s = s32(y[3]);
          for (int i = 0; i < 4; ++i) s += s32(x[i]); r[3] = u32(saturate<s32>(s, sat)); v[d] = put(r))

// ---------------- integer compares
SLY4_VSEM(vcmpequb, compare(c, in, map2<u8>(A, B, [](u8 x, u8 y) { return u8(x == y ? 0xFF : 0); }), 1))
SLY4_VSEM(vcmpequh, compare(c, in, map2<u16>(A, B, [](u16 x, u16 y) { return u16(x == y ? 0xFFFF : 0); }), 2))
SLY4_VSEM(vcmpequw, compare(c, in, map2<u32>(A, B, [](u32 x, u32 y) { return x == y ? ~0u : 0u; }), 4))
SLY4_VSEM(vcmpgtub, compare(c, in, map2<u8>(A, B, [](u8 x, u8 y) { return u8(x > y ? 0xFF : 0); }), 1))
SLY4_VSEM(vcmpgtuh, compare(c, in, map2<u16>(A, B, [](u16 x, u16 y) { return u16(x > y ? 0xFFFF : 0); }), 2))
SLY4_VSEM(vcmpgtuw, compare(c, in, map2<u32>(A, B, [](u32 x, u32 y) { return x > y ? ~0u : 0u; }), 4))
SLY4_VSEM(vcmpgtsb, compare(c, in, map2<u8>(A, B, [](u8 x, u8 y) { return u8(s8(x) > s8(y) ? 0xFF : 0); }), 1))
SLY4_VSEM(vcmpgtsh, compare(c, in, map2<u16>(A, B, [](u16 x, u16 y) { return u16(s16(x) > s16(y) ? 0xFFFF : 0); }), 2))
SLY4_VSEM(vcmpgtsw, compare(c, in, map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) > s32(y) ? ~0u : 0u; }), 4))

// ---------------- floating point
SLY4_VSEM(vaddfp, v[d] = fmap2(c, A, B, [](float x, float y) { return x + y; }))
SLY4_VSEM(vsubfp, v[d] = fmap2(c, A, B, [](float x, float y) { return x - y; }))
SLY4_VSEM(vmaxfp, v[d] = fmap2(c, A, B, [](float x, float y) {
              return (x > y || (x == 0 && y == 0 && !std::signbit(x))) ? x : y; }))
SLY4_VSEM(vminfp, v[d] = fmap2(c, A, B, [](float x, float y) {
              return (x < y || (x == 0 && y == 0 && std::signbit(x))) ? x : y; }))
inline Vec128 madd(const PPUContext& c, const Vec128& A, const Vec128& B, const Vec128& C, bool negate_sub) {
    using namespace vmx;
    const Flt fl{(c.vscr & vscr::NJ) != 0};
    const Lanes<u32> x = get<u32>(A), y = get<u32>(B), z = get<u32>(C);
    Lanes<u32> r;
    for (int i = 0; i < 4; ++i) {
        const float p = fl.in(bf(x[i])), q = fl.in(bf(y[i])), s = fl.in(bf(z[i]));
        if (std::isnan(p) || std::isnan(q) || std::isnan(s)) {
            r[i] = fb(Flt::quiet(std::isnan(p) ? p : std::isnan(q) ? q : s));
            continue;
        }
        const float res = negate_sub ? -std::fmaf(p, s, -q) : std::fmaf(p, s, q);
        r[i] = std::isnan(res) ? kDefaultNaN : fb(fl.out(res));
    }
    return put(r);
}
SLY4_VSEM(vmaddfp, v[d] = madd(c, A, B, C, false))
SLY4_VSEM(vnmsubfp, v[d] = madd(c, A, B, C, true))
SLY4_VSEM(vrefp, v[d] = fmap1(c, B, [](float x) { return 1.0f / x; }))
SLY4_VSEM(vrsqrtefp, v[d] = fmap1(c, B, [](float x) { return 1.0f / std::sqrt(x); }))
SLY4_VSEM(vexptefp, v[d] = fmap1(c, B, [](float x) { return std::exp2(x); }))
SLY4_VSEM(vlogefp, v[d] = fmap1(c, B, [](float x) { return std::log2(x); }))
SLY4_VSEM(vrfin, v[d] = fmap1(c, B, [](float x) { return std::nearbyint(x); }))
SLY4_VSEM(vrfiz, v[d] = fmap1(c, B, [](float x) { return std::trunc(x); }))
SLY4_VSEM(vrfip, v[d] = fmap1(c, B, [](float x) { return std::ceil(x); }))
SLY4_VSEM(vrfim, v[d] = fmap1(c, B, [](float x) { return std::floor(x); }))
SLY4_VSEM(vcfux, Lanes<u32> x = get<u32>(B);
          for (int i = 0; i < 4; ++i) x[i] = fb(float(std::ldexp(double(x[i]), -int(in.vuimm())))); v[d] = put(x))
SLY4_VSEM(vcfsx, Lanes<u32> x = get<u32>(B);
          for (int i = 0; i < 4; ++i) x[i] = fb(float(std::ldexp(double(s32(x[i])), -int(in.vuimm())))); v[d] = put(x))
SLY4_VSEM(vctuxs, const Flt fl{(c.vscr & vscr::NJ) != 0}; Lanes<u32> x = get<u32>(B);
          for (int i = 0; i < 4; ++i) x[i] = float_to_int_sat<u32>(fl.in(bf(x[i])), in.vuimm(), sat); v[d] = put(x))
SLY4_VSEM(vctsxs, const Flt fl{(c.vscr & vscr::NJ) != 0}; Lanes<u32> x = get<u32>(B);
          for (int i = 0; i < 4; ++i) x[i] = u32(float_to_int_sat<s32>(fl.in(bf(x[i])), in.vuimm(), sat)); v[d] = put(x))
inline Vec128 fcompare(const PPUContext& c, const Vec128& A, const Vec128& B, int kind) {
    using namespace vmx;
    const Flt fl{(c.vscr & vscr::NJ) != 0};
    return map2<u32>(A, B, [&](u32 x, u32 y) {
        const float p = fl.in(bf(x)), q = fl.in(bf(y));
        const bool t = kind == 0 ? p == q : kind == 1 ? p >= q : p > q;
        return t ? ~0u : 0u;
    });
}
SLY4_VSEM(vcmpeqfp, compare(c, in, fcompare(c, A, B, 0), 4))
SLY4_VSEM(vcmpgefp, compare(c, in, fcompare(c, A, B, 1), 4))
SLY4_VSEM(vcmpgtfp, compare(c, in, fcompare(c, A, B, 2), 4))
SLY4_VSEM(vcmpbfp, const Flt fl{(c.vscr & vscr::NJ) != 0};
          const Vec128 res = map2<u32>(A, B, [&](u32 x, u32 y) {
              const float p = fl.in(bf(x)), q = fl.in(bf(y));
              return (p <= q ? 0u : 0x80000000u) | (p >= -q ? 0u : 0x40000000u);
          });
          v[d] = res;
          if (in.vc_rc()) { bool all_in = true; for (u32 w : get<u32>(res).v) all_in = all_in && w == 0;
                            c.set_crf(6, all_in ? 2u : 0u); })

#undef SLY4_VSEM

}  // namespace sly4::sem
