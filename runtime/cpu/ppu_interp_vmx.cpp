// PPU VMX (AltiVec) + Cell lvlx/lvrx family.
//
// Every operation works on arrays in guest element order (element 0 first), converted
// from/to the byte-reversed Vec128 register layout. Floating point follows the VMX rules:
// round to nearest, no exceptions, NaN results are the first NaN operand made quiet
// (default 0x7FC00000), and with VSCR[NJ] (the PS3 default) denormal inputs and results
// are flushed to zero.
#include <cmath>
#include <cstring>
#include <limits>

#include "cpu/ppu_interpreter.h"

namespace sly4::ppu_interp {

using ppu::Insn;
using ppu::Op;

namespace {

using u8 = uint8_t;
using s8 = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;

template <class T>
struct Lanes {
    static constexpr int N = 16 / sizeof(T);
    T v[N];
    T& operator[](int i) { return v[i]; }
    const T& operator[](int i) const { return v[i]; }
};

// Guest element i lives in host lane N-1-i (see Vec128).
template <class T>
Lanes<T> get(const Vec128& x) {
    Lanes<T> r;
    for (int i = 0; i < Lanes<T>::N; ++i) std::memcpy(&r[i], x.b + (Lanes<T>::N - 1 - i) * sizeof(T), sizeof(T));
    return r;
}
template <class T>
Vec128 put(const Lanes<T>& l) {
    Vec128 x;
    for (int i = 0; i < Lanes<T>::N; ++i) std::memcpy(x.b + (Lanes<T>::N - 1 - i) * sizeof(T), &l[i], sizeof(T));
    return x;
}

template <class T, class F>
Vec128 map2(const Vec128& a, const Vec128& b, F f) {
    const Lanes<T> x = get<T>(a), y = get<T>(b);
    Lanes<T> r;
    for (int i = 0; i < Lanes<T>::N; ++i) r[i] = f(x[i], y[i]);
    return put(r);
}

template <class T>
T saturate(s64 v, bool& sat) {
    constexpr s64 lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    if (v < lo) { sat = true; return T(lo); }
    if (v > hi) { sat = true; return T(hi); }
    return T(v);
}

u32 fb(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }
float bf(u32 u) { float f; std::memcpy(&f, &u, 4); return f; }
bool fnan(float f) { return std::isnan(f); }
constexpr u32 kDefaultNaN = 0x7FC00000;

struct VmxFloat {
    bool nj;
    float in(float x) const {  // flush denormal input
        return (nj && std::fpclassify(x) == FP_SUBNORMAL) ? std::copysign(0.0f, x) : x;
    }
    float out(float x) const {
        return (nj && std::fpclassify(x) == FP_SUBNORMAL) ? std::copysign(0.0f, x) : x;
    }
    static float quiet(float x) { return bf(fb(x) | 0x00400000); }
};

// Saturating float -> integer used by vctsxs/vctuxs.
template <class T>
T float_to_int_sat(float x, u32 scale, bool& sat) {
    if (std::isnan(x)) return 0;
    const double v = std::trunc(double(x) * std::ldexp(1.0, int(scale)));
    constexpr double lo = double(std::numeric_limits<T>::min()), hi = double(std::numeric_limits<T>::max());
    if (v < lo) { sat = true; return std::numeric_limits<T>::min(); }
    if (v > hi) { sat = true; return std::numeric_limits<T>::max(); }
    return T(v);
}

}  // namespace

bool exec_vmx(PPUContext& c, GuestMemory& mem, const Insn& in) {
    const u32 d = in.rd(), a = in.ra(), b = in.rb(), cc = in.rc();
    Vec128* const v = c.vr;
    const Vec128 A = v[a], B = v[b], C = v[cc];
    const VmxFloat F{(c.vscr & vscr::NJ) != 0};
    bool sat = false;
    auto ea = [&] { return GuestAddr((a ? c.gpr[a] : 0) + c.gpr[b]); };

    // Float lane-wise helpers with NaN propagation (first NaN of the operands, quieted).
    auto fmap2 = [&](auto f) {
        return map2<u32>(A, B, [&](u32 x, u32 y) {
            const float p = F.in(bf(x)), q = F.in(bf(y));
            if (fnan(p)) return fb(VmxFloat::quiet(p));
            if (fnan(q)) return fb(VmxFloat::quiet(q));
            const float r = f(p, q);
            return fnan(r) ? kDefaultNaN : fb(F.out(r));
        });
    };
    auto fmap1 = [&](auto f) {
        Lanes<u32> x = get<u32>(B);
        for (int i = 0; i < 4; ++i) {
            const float p = F.in(bf(x[i]));
            if (fnan(p)) { x[i] = fb(VmxFloat::quiet(p)); continue; }
            const float r = f(p);
            x[i] = fnan(r) ? kDefaultNaN : fb(F.out(r));
        }
        return put(x);
    };
    // Compare: all-ones / all-zeros per element; Rc sets CR6.
    auto vcmp = [&](const Vec128& res, u32 elem_size) {
        v[d] = res;
        if (in.vc_rc()) {
            bool all = true, none = true;
            for (int i = 0; i < 16; i += elem_size) {
                if (res.b[i]) none = false;
                else all = false;
            }
            c.set_crf(6, (all ? 8u : 0u) | (none ? 2u : 0u));
        }
    };

    switch (in.op) {
        // ---------------- loads / stores
        case Op::lvx: case Op::lvxl: case Op::lvebx: case Op::lvehx: case Op::lvewx:
            // element loads read the whole aligned quadword, as the hardware does
            v[d] = mem.read128(ea() & ~15u);
            return true;
        case Op::stvx: case Op::stvxl: mem.write128(ea() & ~15u, v[d]); return true;
        case Op::stvebx: { const GuestAddr e = ea(); mem.write<u8>(e, get<u8>(v[d])[e & 15]); return true; }
        case Op::stvehx: { const GuestAddr e = ea() & ~1u; mem.write<u16>(e, get<u16>(v[d])[(e & 15) / 2]); return true; }
        case Op::stvewx: { const GuestAddr e = ea() & ~3u; mem.write<u32>(e, get<u32>(v[d])[(e & 15) / 4]); return true; }
        case Op::lvsl: case Op::lvsr: {
            const u32 sh = ea() & 15;
            Lanes<u8> r;
            for (int i = 0; i < 16; ++i) r[i] = u8(in.op == Op::lvsl ? sh + i : 16 - sh + i);
            v[d] = put(r);
            return true;
        }
        case Op::lvlx: case Op::lvlxl: {  // bytes from EA to the end of its block, left-aligned
            const GuestAddr e = ea(), off = e & 15;
            Lanes<u8> r{};
            for (u32 i = 0; i < 16 - off; ++i) r[i] = mem.read<u8>(e + i);
            v[d] = put(r);
            return true;
        }
        case Op::lvrx: case Op::lvrxl: {  // bytes from the block start up to EA, right-aligned
            const GuestAddr e = ea(), off = e & 15, blk = e & ~15u;
            Lanes<u8> r{};
            for (u32 i = 0; i < off; ++i) r[16 - off + i] = mem.read<u8>(blk + i);
            v[d] = put(r);
            return true;
        }
        case Op::stvlx: case Op::stvlxl: {
            const GuestAddr e = ea(), off = e & 15;
            const Lanes<u8> s = get<u8>(v[d]);
            for (u32 i = 0; i < 16 - off; ++i) mem.write<u8>(e + i, s[i]);
            return true;
        }
        case Op::stvrx: case Op::stvrxl: {
            const GuestAddr e = ea(), off = e & 15, blk = e & ~15u;
            const Lanes<u8> s = get<u8>(v[d]);
            for (u32 i = 0; i < off; ++i) mem.write<u8>(blk + i, s[16 - off + i]);
            return true;
        }

        // ---------------- VSCR
        case Op::mfvscr: { Lanes<u32> r{}; r[3] = c.vscr; v[d] = put(r); return true; }
        case Op::mtvscr: c.vscr = get<u32>(B)[3] & (vscr::NJ | vscr::SAT); return true;

        // ---------------- logical
        case Op::vand: case Op::vandc: case Op::vor: case Op::vxor: case Op::vnor: {
            Vec128 r;
            for (int i = 0; i < 16; ++i) {
                const u8 x = A.b[i], y = B.b[i];
                r.b[i] = in.op == Op::vand ? u8(x & y) : in.op == Op::vandc ? u8(x & ~y) : in.op == Op::vor ? u8(x | y)
                       : in.op == Op::vxor ? u8(x ^ y) : u8(~(x | y));
            }
            v[d] = r;
            return true;
        }
        case Op::vsel: {
            Vec128 r;
            for (int i = 0; i < 16; ++i) r.b[i] = u8((B.b[i] & C.b[i]) | (A.b[i] & ~C.b[i]));
            v[d] = r;
            return true;
        }
        case Op::vperm: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B), s = get<u8>(C);
            Lanes<u8> r;
            for (int i = 0; i < 16; ++i) {
                const u32 k = s[i] & 31;
                r[i] = k < 16 ? x[k] : y[k - 16];
            }
            v[d] = put(r);
            return true;
        }
        case Op::vsldoi: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B);
            Lanes<u8> r;
            const u32 sh = in.vshb();
            for (u32 i = 0; i < 16; ++i) r[i] = i + sh < 16 ? x[i + sh] : y[i + sh - 16];
            v[d] = put(r);
            return true;
        }

        // ---------------- integer add/sub (modulo, carry, saturating)
        case Op::vaddubm: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x + y); }); return true;
        case Op::vadduhm: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x + y); }); return true;
        case Op::vadduwm: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x + y; }); return true;
        case Op::vsububm: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x - y); }); return true;
        case Op::vsubuhm: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x - y); }); return true;
        case Op::vsubuwm: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x - y; }); return true;
        case Op::vaddcuw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((u64(x) + y) >> 32); }); return true;
        case Op::vsubcuw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32(x >= y); }); return true;
        case Op::vaddubs: v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return saturate<u8>(s64(x) + y, sat); }); break;
        case Op::vadduhs: v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return saturate<u16>(s64(x) + y, sat); }); break;
        case Op::vadduws: v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return saturate<u32>(s64(x) + y, sat); }); break;
        case Op::vaddsbs: v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return u8(saturate<s8>(s64(s8(x)) + s8(y), sat)); }); break;
        case Op::vaddshs: v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return u16(saturate<s16>(s64(s16(x)) + s16(y), sat)); }); break;
        case Op::vaddsws: v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return u32(saturate<s32>(s64(s32(x)) + s32(y), sat)); }); break;
        case Op::vsububs: v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return saturate<u8>(s64(x) - y, sat); }); break;
        case Op::vsubuhs: v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return saturate<u16>(s64(x) - y, sat); }); break;
        case Op::vsubuws: v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return saturate<u32>(s64(x) - y, sat); }); break;
        case Op::vsubsbs: v[d] = map2<u8>(A, B, [&](u8 x, u8 y) { return u8(saturate<s8>(s64(s8(x)) - s8(y), sat)); }); break;
        case Op::vsubshs: v[d] = map2<u16>(A, B, [&](u16 x, u16 y) { return u16(saturate<s16>(s64(s16(x)) - s16(y), sat)); }); break;
        case Op::vsubsws: v[d] = map2<u32>(A, B, [&](u32 x, u32 y) { return u32(saturate<s32>(s64(s32(x)) - s32(y), sat)); }); break;

        // ---------------- max / min / average
        case Op::vmaxub: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return x > y ? x : y; }); return true;
        case Op::vmaxuh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return x > y ? x : y; }); return true;
        case Op::vmaxuw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x > y ? x : y; }); return true;
        case Op::vmaxsb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return s8(x) > s8(y) ? x : y; }); return true;
        case Op::vmaxsh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return s16(x) > s16(y) ? x : y; }); return true;
        case Op::vmaxsw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) > s32(y) ? x : y; }); return true;
        case Op::vminub: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return x < y ? x : y; }); return true;
        case Op::vminuh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return x < y ? x : y; }); return true;
        case Op::vminuw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x < y ? x : y; }); return true;
        case Op::vminsb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return s8(x) < s8(y) ? x : y; }); return true;
        case Op::vminsh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return s16(x) < s16(y) ? x : y; }); return true;
        case Op::vminsw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) < s32(y) ? x : y; }); return true;
        case Op::vavgub: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8((u32(x) + y + 1) >> 1); }); return true;
        case Op::vavguh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16((u32(x) + y + 1) >> 1); }); return true;
        case Op::vavguw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((u64(x) + y + 1) >> 1); }); return true;
        case Op::vavgsb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8((s32(s8(x)) + s8(y) + 1) >> 1); }); return true;
        case Op::vavgsh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16((s32(s16(x)) + s16(y) + 1) >> 1); }); return true;
        case Op::vavgsw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32((s64(s32(x)) + s32(y) + 1) >> 1); }); return true;

        // ---------------- shifts and rotates (element-wise, count modulo element width)
        case Op::vrlb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { const u32 n = y & 7; return u8((x << n) | (x >> ((8 - n) & 7))); }); return true;
        case Op::vrlh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { const u32 n = y & 15; return u16((x << n) | (x >> ((16 - n) & 15))); }); return true;
        case Op::vrlw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { const u32 n = y & 31; return n ? (x << n) | (x >> (32 - n)) : x; }); return true;
        case Op::vslb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x << (y & 7)); }); return true;
        case Op::vslh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x << (y & 15)); }); return true;
        case Op::vslw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x << (y & 31); }); return true;
        case Op::vsrb: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(x >> (y & 7)); }); return true;
        case Op::vsrh: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(x >> (y & 15)); }); return true;
        case Op::vsrw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return x >> (y & 31); }); return true;
        case Op::vsrab: v[d] = map2<u8>(A, B, [](u8 x, u8 y) { return u8(s8(x) >> (y & 7)); }); return true;
        case Op::vsrah: v[d] = map2<u16>(A, B, [](u16 x, u16 y) { return u16(s16(x) >> (y & 15)); }); return true;
        case Op::vsraw: v[d] = map2<u32>(A, B, [](u32 x, u32 y) { return u32(s32(x) >> (y & 31)); }); return true;
        case Op::vsl: case Op::vsr: {  // whole-register shift by 0..7 bits (vB's last byte)
            const u32 n = get<u8>(B)[15] & 7;
            const Lanes<u8> x = get<u8>(A);
            Lanes<u8> r;
            for (int i = 0; i < 16; ++i) {
                if (in.op == Op::vsl) r[i] = u8((x[i] << n) | (i < 15 && n ? x[i + 1] >> (8 - n) : 0));
                else r[i] = u8((x[i] >> n) | (i > 0 && n ? x[i - 1] << (8 - n) : 0));
            }
            v[d] = put(r);
            return true;
        }
        case Op::vslo: case Op::vsro: {  // whole-register shift by octets
            const u32 n = (get<u8>(B)[15] >> 3) & 15;
            const Lanes<u8> x = get<u8>(A);
            Lanes<u8> r{};
            for (u32 i = 0; i < 16; ++i) {
                if (in.op == Op::vslo) r[i] = i + n < 16 ? x[i + n] : 0;
                else r[i] = i >= n ? x[i - n] : 0;
            }
            v[d] = put(r);
            return true;
        }

        // ---------------- merge, splat, pack, unpack
        case Op::vmrghb: case Op::vmrglb: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B);
            const int base = in.op == Op::vmrghb ? 0 : 8;
            Lanes<u8> r;
            for (int i = 0; i < 8; ++i) { r[2 * i] = x[base + i]; r[2 * i + 1] = y[base + i]; }
            v[d] = put(r);
            return true;
        }
        case Op::vmrghh: case Op::vmrglh: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
            const int base = in.op == Op::vmrghh ? 0 : 4;
            Lanes<u16> r;
            for (int i = 0; i < 4; ++i) { r[2 * i] = x[base + i]; r[2 * i + 1] = y[base + i]; }
            v[d] = put(r);
            return true;
        }
        case Op::vmrghw: case Op::vmrglw: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
            const int base = in.op == Op::vmrghw ? 0 : 2;
            Lanes<u32> r;
            for (int i = 0; i < 2; ++i) { r[2 * i] = x[base + i]; r[2 * i + 1] = y[base + i]; }
            v[d] = put(r);
            return true;
        }
        case Op::vspltb: { const u8 e = get<u8>(B)[in.vuimm() & 15]; Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = e; v[d] = put(r); return true; }
        case Op::vsplth: { const u16 e = get<u16>(B)[in.vuimm() & 7]; Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = e; v[d] = put(r); return true; }
        case Op::vspltw: { const u32 e = get<u32>(B)[in.vuimm() & 3]; Lanes<u32> r; for (int i = 0; i < 4; ++i) r[i] = e; v[d] = put(r); return true; }
        case Op::vspltisb: { Lanes<u8> r; for (int i = 0; i < 16; ++i) r[i] = u8(in.vsimm()); v[d] = put(r); return true; }
        case Op::vspltish: { Lanes<u16> r; for (int i = 0; i < 8; ++i) r[i] = u16(in.vsimm()); v[d] = put(r); return true; }
        case Op::vspltisw: { Lanes<u32> r; for (int i = 0; i < 4; ++i) r[i] = u32(in.vsimm()); v[d] = put(r); return true; }
        case Op::vpkuhum: case Op::vpkuhus: case Op::vpkshus: case Op::vpkshss: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
            Lanes<u8> r;
            for (int i = 0; i < 16; ++i) {
                const u16 e = i < 8 ? x[i] : y[i - 8];
                switch (in.op) {
                    case Op::vpkuhum: r[i] = u8(e); break;
                    case Op::vpkuhus: r[i] = saturate<u8>(e, sat); break;
                    case Op::vpkshus: r[i] = saturate<u8>(s16(e), sat); break;
                    default: r[i] = u8(saturate<s8>(s16(e), sat)); break;
                }
            }
            v[d] = put(r);
            break;
        }
        case Op::vpkuwum: case Op::vpkuwus: case Op::vpkswus: case Op::vpkswss: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) {
                const u32 e = i < 4 ? x[i] : y[i - 4];
                switch (in.op) {
                    case Op::vpkuwum: r[i] = u16(e); break;
                    case Op::vpkuwus: r[i] = saturate<u16>(e, sat); break;
                    case Op::vpkswus: r[i] = saturate<u16>(s32(e), sat); break;
                    default: r[i] = u16(saturate<s16>(s32(e), sat)); break;
                }
            }
            v[d] = put(r);
            break;
        }
        case Op::vpkpx: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) {
                const u32 e = i < 4 ? x[i] : y[i - 4];
                r[i] = u16(((e >> 9) & 0x8000) | ((e >> 9) & 0x7C00) | ((e >> 6) & 0x03E0) | ((e >> 3) & 0x001F));
            }
            v[d] = put(r);
            return true;
        }
        case Op::vupkhsb: case Op::vupklsb: {
            const Lanes<u8> x = get<u8>(B);
            const int base = in.op == Op::vupkhsb ? 0 : 8;
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) r[i] = u16(s16(s8(x[base + i])));
            v[d] = put(r);
            return true;
        }
        case Op::vupkhsh: case Op::vupklsh: {
            const Lanes<u16> x = get<u16>(B);
            const int base = in.op == Op::vupkhsh ? 0 : 4;
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[base + i])));
            v[d] = put(r);
            return true;
        }
        case Op::vupkhpx: case Op::vupklpx: {
            const Lanes<u16> x = get<u16>(B);
            const int base = in.op == Op::vupkhpx ? 0 : 4;
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) {
                const u16 p = x[base + i];
                r[i] = ((p & 0x8000) ? 0xFF000000u : 0) | (u32((p >> 10) & 31) << 16) | (u32((p >> 5) & 31) << 8) |
                       u32(p & 31);
            }
            v[d] = put(r);
            return true;
        }

        // ---------------- multiply / multiply-sum / sum
        case Op::vmuleub: case Op::vmuloub: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B);
            const int odd = in.op == Op::vmuloub;
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) r[i] = u16(u32(x[2 * i + odd]) * y[2 * i + odd]);
            v[d] = put(r);
            return true;
        }
        case Op::vmulesb: case Op::vmulosb: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B);
            const int odd = in.op == Op::vmulosb;
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) r[i] = u16(s32(s8(x[2 * i + odd])) * s8(y[2 * i + odd]));
            v[d] = put(r);
            return true;
        }
        case Op::vmuleuh: case Op::vmulouh: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
            const int odd = in.op == Op::vmulouh;
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) r[i] = u32(x[2 * i + odd]) * y[2 * i + odd];
            v[d] = put(r);
            return true;
        }
        case Op::vmulesh: case Op::vmulosh: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
            const int odd = in.op == Op::vmulosh;
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) r[i] = u32(s32(s16(x[2 * i + odd])) * s16(y[2 * i + odd]));
            v[d] = put(r);
            return true;
        }
        case Op::vmhaddshs: case Op::vmhraddshs: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B), z = get<u16>(C);
            const s32 round = in.op == Op::vmhraddshs ? 0x4000 : 0;
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i)
                r[i] = u16(saturate<s16>(((s32(s16(x[i])) * s16(y[i]) + round) >> 15) + s16(z[i]), sat));
            v[d] = put(r);
            break;
        }
        case Op::vmladduhm: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B), z = get<u16>(C);
            Lanes<u16> r;
            for (int i = 0; i < 8; ++i) r[i] = u16(u32(x[i]) * y[i] + z[i]);
            v[d] = put(r);
            return true;
        }
        case Op::vmsumubm: case Op::vmsummbm: {
            const Lanes<u8> x = get<u8>(A), y = get<u8>(B);
            const Lanes<u32> z = get<u32>(C);
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) {
                s64 s = z[i];
                for (int k = 0; k < 4; ++k)
                    s += in.op == Op::vmsumubm ? s64(x[4 * i + k]) * y[4 * i + k] : s64(s8(x[4 * i + k])) * y[4 * i + k];
                r[i] = u32(s);
            }
            v[d] = put(r);
            return true;
        }
        case Op::vmsumuhm: case Op::vmsumuhs: case Op::vmsumshm: case Op::vmsumshs: {
            const Lanes<u16> x = get<u16>(A), y = get<u16>(B);
            const Lanes<u32> z = get<u32>(C);
            const bool sgn = in.op == Op::vmsumshm || in.op == Op::vmsumshs;
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) {
                s64 s = sgn ? s64(s32(z[i])) : s64(z[i]);
                for (int k = 0; k < 2; ++k)
                    s += sgn ? s64(s16(x[2 * i + k])) * s16(y[2 * i + k]) : s64(x[2 * i + k]) * y[2 * i + k];
                switch (in.op) {
                    case Op::vmsumuhs: r[i] = saturate<u32>(s, sat); break;
                    case Op::vmsumshs: r[i] = u32(saturate<s32>(s, sat)); break;
                    default: r[i] = u32(s); break;
                }
            }
            v[d] = put(r);
            break;
        }
        case Op::vsum4ubs: case Op::vsum4sbs: case Op::vsum4shs: {
            const Lanes<u32> y = get<u32>(B);
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) {
                s64 s;
                if (in.op == Op::vsum4ubs) {
                    const Lanes<u8> x = get<u8>(A);
                    s = s64(y[i]);
                    for (int k = 0; k < 4; ++k) s += x[4 * i + k];
                    r[i] = saturate<u32>(s, sat);
                } else if (in.op == Op::vsum4sbs) {
                    const Lanes<u8> x = get<u8>(A);
                    s = s32(y[i]);
                    for (int k = 0; k < 4; ++k) s += s8(x[4 * i + k]);
                    r[i] = u32(saturate<s32>(s, sat));
                } else {
                    const Lanes<u16> x = get<u16>(A);
                    s = s32(y[i]) + s64(s16(x[2 * i])) + s16(x[2 * i + 1]);
                    r[i] = u32(saturate<s32>(s, sat));
                }
            }
            v[d] = put(r);
            break;
        }
        case Op::vsum2sws: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
            Lanes<u32> r{};
            for (int i = 0; i < 2; ++i)
                r[2 * i + 1] = u32(saturate<s32>(s64(s32(x[2 * i])) + s32(x[2 * i + 1]) + s32(y[2 * i + 1]), sat));
            v[d] = put(r);
            break;
        }
        case Op::vsumsws: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B);
            Lanes<u32> r{};
            s64 s = s32(y[3]);
            for (int i = 0; i < 4; ++i) s += s32(x[i]);
            r[3] = u32(saturate<s32>(s, sat));
            v[d] = put(r);
            break;
        }

        // ---------------- integer compares
        case Op::vcmpequb: vcmp(map2<u8>(A, B, [](u8 x, u8 y) { return u8(x == y ? 0xFF : 0); }), 1); return true;
        case Op::vcmpequh: vcmp(map2<u16>(A, B, [](u16 x, u16 y) { return u16(x == y ? 0xFFFF : 0); }), 2); return true;
        case Op::vcmpequw: vcmp(map2<u32>(A, B, [](u32 x, u32 y) { return x == y ? ~0u : 0u; }), 4); return true;
        case Op::vcmpgtub: vcmp(map2<u8>(A, B, [](u8 x, u8 y) { return u8(x > y ? 0xFF : 0); }), 1); return true;
        case Op::vcmpgtuh: vcmp(map2<u16>(A, B, [](u16 x, u16 y) { return u16(x > y ? 0xFFFF : 0); }), 2); return true;
        case Op::vcmpgtuw: vcmp(map2<u32>(A, B, [](u32 x, u32 y) { return x > y ? ~0u : 0u; }), 4); return true;
        case Op::vcmpgtsb: vcmp(map2<u8>(A, B, [](u8 x, u8 y) { return u8(s8(x) > s8(y) ? 0xFF : 0); }), 1); return true;
        case Op::vcmpgtsh: vcmp(map2<u16>(A, B, [](u16 x, u16 y) { return u16(s16(x) > s16(y) ? 0xFFFF : 0); }), 2); return true;
        case Op::vcmpgtsw: vcmp(map2<u32>(A, B, [](u32 x, u32 y) { return s32(x) > s32(y) ? ~0u : 0u; }), 4); return true;

        // ---------------- floating point
        case Op::vaddfp: v[d] = fmap2([](float x, float y) { return x + y; }); return true;
        case Op::vsubfp: v[d] = fmap2([](float x, float y) { return x - y; }); return true;
        case Op::vmaxfp: v[d] = fmap2([](float x, float y) { return (x > y || (x == 0 && y == 0 && !std::signbit(x))) ? x : y; }); return true;
        case Op::vminfp: v[d] = fmap2([](float x, float y) { return (x < y || (x == 0 && y == 0 && std::signbit(x))) ? x : y; }); return true;
        case Op::vmaddfp: case Op::vnmsubfp: {
            const Lanes<u32> x = get<u32>(A), y = get<u32>(B), z = get<u32>(C);
            Lanes<u32> r;
            for (int i = 0; i < 4; ++i) {
                const float p = F.in(bf(x[i])), q = F.in(bf(y[i])), s = F.in(bf(z[i]));
                if (fnan(p) || fnan(q) || fnan(s)) {
                    r[i] = fb(VmxFloat::quiet(fnan(p) ? p : fnan(q) ? q : s));
                    continue;
                }
                float res = in.op == Op::vmaddfp ? std::fmaf(p, s, q) : -std::fmaf(p, s, -q);
                r[i] = fnan(res) ? kDefaultNaN : fb(F.out(res));
            }
            v[d] = put(r);
            return true;
        }
        case Op::vrefp: v[d] = fmap1([](float x) { return 1.0f / x; }); return true;
        case Op::vrsqrtefp: v[d] = fmap1([](float x) { return 1.0f / std::sqrt(x); }); return true;
        case Op::vexptefp: v[d] = fmap1([](float x) { return std::exp2(x); }); return true;
        case Op::vlogefp: v[d] = fmap1([](float x) { return std::log2(x); }); return true;
        case Op::vrfin: v[d] = fmap1([](float x) { return std::nearbyint(x); }); return true;
        case Op::vrfiz: v[d] = fmap1([](float x) { return std::trunc(x); }); return true;
        case Op::vrfip: v[d] = fmap1([](float x) { return std::ceil(x); }); return true;
        case Op::vrfim: v[d] = fmap1([](float x) { return std::floor(x); }); return true;
        case Op::vcfux: case Op::vcfsx: {
            Lanes<u32> x = get<u32>(B);
            for (int i = 0; i < 4; ++i) {
                const double val = in.op == Op::vcfux ? double(x[i]) : double(s32(x[i]));
                x[i] = fb(float(std::ldexp(val, -int(in.vuimm()))));
            }
            v[d] = put(x);
            return true;
        }
        case Op::vctuxs: case Op::vctsxs: {
            Lanes<u32> x = get<u32>(B);
            for (int i = 0; i < 4; ++i) {
                const float f = F.in(bf(x[i]));
                x[i] = in.op == Op::vctuxs ? float_to_int_sat<u32>(f, in.vuimm(), sat)
                                           : u32(float_to_int_sat<s32>(f, in.vuimm(), sat));
            }
            v[d] = put(x);
            break;
        }
        case Op::vcmpeqfp: case Op::vcmpgefp: case Op::vcmpgtfp: {
            const Op op = in.op;
            vcmp(map2<u32>(A, B, [&](u32 x, u32 y) {
                     const float p = F.in(bf(x)), q = F.in(bf(y));
                     const bool t = op == Op::vcmpeqfp ? p == q : op == Op::vcmpgefp ? p >= q : p > q;
                     return t ? ~0u : 0u;
                 }), 4);
            return true;
        }
        case Op::vcmpbfp: {
            const Vec128 res = map2<u32>(A, B, [&](u32 x, u32 y) {
                const float p = F.in(bf(x)), q = F.in(bf(y));
                return (p <= q ? 0u : 0x80000000u) | (p >= -q ? 0u : 0x40000000u);
            });
            v[d] = res;
            if (in.vc_rc()) {
                bool all_in = true;
                for (u32 w : get<u32>(res).v) all_in = all_in && w == 0;
                c.set_crf(6, all_in ? 2u : 0u);
            }
            return true;
        }
        default:
            return false;
    }
    if (sat) c.vscr |= vscr::SAT;
    return true;
}

}  // namespace sly4::ppu_interp
