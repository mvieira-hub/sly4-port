// Integer, condition register, rotate, load/store, branch and system instructions.
// FPU and VMX live in ppu_interp_fpu.cpp / ppu_interp_vmx.cpp.
//
// Where the ISA leaves bits undefined (mulhw high word, divw on overflow, mfocrf other
// fields) the interpreter follows RPCS3, whose behaviour was checked on real PS3s.
#include "cpu/ppu_interpreter.h"

#include <atomic>
#include <chrono>

namespace sly4 {

using ppu::Insn;
using ppu::Op;

namespace {

using u64 = uint64_t;
using s64 = int64_t;
using u32 = uint32_t;
using s32 = int32_t;
using u128 = unsigned __int128;
using s128 = __int128;

constexpr u64 kTimebaseHz = 79'800'000;  // PS3 time base frequency

u64 rotl64(u64 x, unsigned n) {
    n &= 63;
    return n ? (x << n) | (x >> (64 - n)) : x;
}
// 32-bit rotate as the ISA defines it: the low word duplicated, then rotated as 64 bits.
u64 rotl32(u64 x, unsigned n) { return rotl64((x & 0xFFFFFFFF) | (x << 32), n & 31); }
// MASK(mb, me) in ISA bit numbering (bit 0 = MSB), inclusive, wrapping when mb > me.
u64 mask64(unsigned mb, unsigned me) {
    const u64 m1 = ~0ull >> mb, m2 = ~0ull << (63 - me);
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

u64 add_carry(u64 a, u64 b, bool cin, bool& cout) {
    const u128 s = u128(a) + b + cin;
    cout = (s >> 64) != 0;
    return static_cast<u64>(s);
}
bool add_overflow(u64 a, u64 b, u64 r) { return ((a ^ r) & (b ^ r)) >> 63; }

void set_cr0(PPUContext& c, u64 r) {
    const s64 v = static_cast<s64>(r);
    c.set_crf(0, (v < 0 ? 8u : v > 0 ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
void compare_signed(PPUContext& c, u32 bf, s64 a, s64 b) {
    c.set_crf(bf, (a < b ? 8u : a > b ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
void compare_unsigned(PPUContext& c, u32 bf, u64 a, u64 b) {
    c.set_crf(bf, (a < b ? 8u : a > b ? 4u : 2u) | (c.xer_so ? 1u : 0u));
}
void set_ov(PPUContext& c, bool ov) {
    c.xer_ov = ov;
    if (ov) c.xer_so = true;
}

u64 timebase() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const u64 ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return static_cast<u64>(u128(ns) * kTimebaseHz / 1'000'000'000);
}

bool trap_taken(u32 to, s64 a, s64 b) {
    return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
           ((to & 2) && u64(a) < u64(b)) || ((to & 1) && u64(a) > u64(b));
}

// Conditional branch condition (BO/BI), updating CTR when BO[2] = 0.
bool branch_taken(PPUContext& c, u32 bo, u32 bi, bool allow_ctr) {
    bool ctr_ok = true;
    if (allow_ctr && !(bo & 4)) {
        --c.ctr;
        ctr_ok = (c.ctr != 0) != ((bo & 2) != 0);
    }
    const bool cond_ok = (bo & 16) || (c.crbit(bi) == ((bo & 8) != 0));
    return ctr_ok && cond_ok;
}

GuestAddr ea_d(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + static_cast<s64>(in.simm()));
}
GuestAddr ea_ds(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + static_cast<s64>(in.ds()));
}
GuestAddr ea_x(const PPUContext& c, const Insn& in) {
    return static_cast<GuestAddr>((in.ra() ? c.gpr[in.ra()] : 0) + c.gpr[in.rb()]);
}

template <class T>
bool store_conditional(PPUContext& c, GuestMemory& mem, GuestAddr ea, T value) {
    const bool ok = c.reserved && c.reserve_addr == ea && c.reserve_size == sizeof(T);
    c.reserved = false;
    if (!ok) return false;
    // Compare-and-swap on the raw big-endian bytes the reservation saw.
    T expected = byteswap(static_cast<T>(c.reserve_value));
    const T desired = byteswap(value);
    auto* p = reinterpret_cast<T*>(mem.host(ea));
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

}  // namespace

PpuStep ppu_step(PPUContext& c, GuestMemory& mem) {
    return ppu_execute(c, mem, ppu::decode(mem.read<uint32_t>(c.cia)));
}

PpuStep ppu_execute(PPUContext& c, GuestMemory& mem, const Insn& in) {
    const u32 d = in.rd(), a = in.ra(), b = in.rb();
    u64* const r = c.gpr;
    u32 next = c.cia + 4;

    // XO-form arithmetic: compute, then OE/Rc.
    auto finish_xo = [&](u64 res, bool ov, bool has_oe = true) {
        r[d] = res;
        if (has_oe && in.oe()) set_ov(c, ov);
        if (in.rc_bit()) set_cr0(c, res);
    };
    // Logical/shift X-form: result in rA.
    auto finish_x = [&](u64 res) {
        r[a] = res;
        if (in.rc_bit()) set_cr0(c, res);
    };

    switch (in.op) {
        // ---------------- D-form arithmetic
        case Op::addi: r[d] = (a ? r[a] : 0) + static_cast<s64>(in.simm()); break;
        case Op::addis: r[d] = (a ? r[a] : 0) + (static_cast<s64>(in.simm()) << 16); break;
        case Op::addic:
        case Op::addic_: {
            bool ca;
            r[d] = add_carry(r[a], static_cast<s64>(in.simm()), false, ca);
            c.xer_ca = ca;
            if (in.op == Op::addic_) set_cr0(c, r[d]);
            break;
        }
        case Op::subfic: {
            bool ca;
            r[d] = add_carry(~r[a], static_cast<s64>(in.simm()), true, ca);
            c.xer_ca = ca;
            break;
        }
        case Op::mulli: r[d] = r[a] * static_cast<u64>(static_cast<s64>(in.simm())); break;
        case Op::ori: r[a] = r[d] | in.uimm(); break;
        case Op::oris: r[a] = r[d] | (u64(in.uimm()) << 16); break;
        case Op::xori: r[a] = r[d] ^ in.uimm(); break;
        case Op::xoris: r[a] = r[d] ^ (u64(in.uimm()) << 16); break;
        case Op::andi_: r[a] = r[d] & in.uimm(); set_cr0(c, r[a]); break;
        case Op::andis_: r[a] = r[d] & (u64(in.uimm()) << 16); set_cr0(c, r[a]); break;

        // ---------------- compares and traps
        case Op::cmpi:
            if (in.l_cmp()) compare_signed(c, in.bf(), s64(r[a]), in.simm());
            else compare_signed(c, in.bf(), s32(r[a]), in.simm());
            break;
        case Op::cmpli:
            if (in.l_cmp()) compare_unsigned(c, in.bf(), r[a], in.uimm());
            else compare_unsigned(c, in.bf(), u32(r[a]), in.uimm());
            break;
        case Op::cmp:
            if (in.l_cmp()) compare_signed(c, in.bf(), s64(r[a]), s64(r[b]));
            else compare_signed(c, in.bf(), s32(r[a]), s32(r[b]));
            break;
        case Op::cmpl:
            if (in.l_cmp()) compare_unsigned(c, in.bf(), r[a], r[b]);
            else compare_unsigned(c, in.bf(), u32(r[a]), u32(r[b]));
            break;
        case Op::tw: if (trap_taken(d, s32(r[a]), s32(r[b]))) return PpuStep::Trap; break;
        case Op::td: if (trap_taken(d, s64(r[a]), s64(r[b]))) return PpuStep::Trap; break;
        case Op::twi: if (trap_taken(d, s32(r[a]), in.simm())) return PpuStep::Trap; break;
        case Op::tdi: if (trap_taken(d, s64(r[a]), in.simm())) return PpuStep::Trap; break;

        // ---------------- XO-form arithmetic
        case Op::add: { const u64 res = r[a] + r[b]; finish_xo(res, add_overflow(r[a], r[b], res)); break; }
        case Op::addc: case Op::adde: case Op::subfc: case Op::subfe: {
            const bool sub = in.op == Op::subfc || in.op == Op::subfe;
            const u64 x = sub ? ~r[a] : r[a];
            const bool cin = (in.op == Op::adde || in.op == Op::subfe) ? c.xer_ca : sub;
            bool ca;
            const u64 res = add_carry(x, r[b], cin, ca);
            const bool ov = add_overflow(x, r[b], res);
            c.xer_ca = ca;
            finish_xo(res, ov);
            break;
        }
        case Op::addze: case Op::addme: case Op::subfze: case Op::subfme: {
            const bool sub = in.op == Op::subfze || in.op == Op::subfme;
            const u64 x = sub ? ~r[a] : r[a];
            const u64 y = (in.op == Op::addme || in.op == Op::subfme) ? ~0ull : 0;
            bool ca;
            const u64 res = add_carry(x, y, c.xer_ca, ca);
            const bool ov = add_overflow(x, y, res);
            c.xer_ca = ca;
            finish_xo(res, ov);
            break;
        }
        case Op::subf: { const u64 res = r[b] - r[a]; finish_xo(res, add_overflow(~r[a], r[b], res)); break; }
        case Op::neg: finish_xo(0 - r[a], r[a] == 0x8000000000000000ull); break;
        case Op::mullw: {
            const s64 p = s64(s32(r[a])) * s32(r[b]);
            finish_xo(u64(p), p != s64(s32(p)));
            break;
        }
        case Op::mulld: {
            const s128 p = s128(s64(r[a])) * s64(r[b]);
            finish_xo(u64(p), p != s128(s64(p)));
            break;
        }
        case Op::mulhw: finish_xo(u64((s64(s32(r[a])) * s32(r[b])) >> 32), false, false); break;
        case Op::mulhwu: finish_xo((u64(u32(r[a])) * u32(r[b])) >> 32, false, false); break;
        case Op::mulhd: finish_xo(u64((s128(s64(r[a])) * s64(r[b])) >> 64), false, false); break;
        case Op::mulhdu: finish_xo(u64((u128(r[a]) * r[b]) >> 64), false, false); break;
        case Op::divw: {
            const s32 x = s32(r[a]), y = s32(r[b]);
            const bool ov = y == 0 || (x == INT32_MIN && y == -1);
            finish_xo(ov ? 0 : u64(u32(x / y)), ov);
            break;
        }
        case Op::divwu: {
            const u32 x = u32(r[a]), y = u32(r[b]);
            finish_xo(y ? u64(x / y) : 0, y == 0);
            break;
        }
        case Op::divd: {
            const s64 x = s64(r[a]), y = s64(r[b]);
            const bool ov = y == 0 || (x == INT64_MIN && y == -1);
            finish_xo(ov ? 0 : u64(x / y), ov);
            break;
        }
        case Op::divdu: finish_xo(r[b] ? r[a] / r[b] : 0, r[b] == 0); break;

        // ---------------- logical, extend, count, shifts (result in rA)
        case Op::and_: finish_x(r[d] & r[b]); break;
        case Op::andc: finish_x(r[d] & ~r[b]); break;
        case Op::or_: finish_x(r[d] | r[b]); break;
        case Op::orc: finish_x(r[d] | ~r[b]); break;
        case Op::xor_: finish_x(r[d] ^ r[b]); break;
        case Op::nand: finish_x(~(r[d] & r[b])); break;
        case Op::nor: finish_x(~(r[d] | r[b])); break;
        case Op::eqv: finish_x(~(r[d] ^ r[b])); break;
        case Op::extsb: finish_x(u64(s64(int8_t(r[d])))); break;
        case Op::extsh: finish_x(u64(s64(int16_t(r[d])))); break;
        case Op::extsw: finish_x(u64(s64(s32(r[d])))); break;
        case Op::cntlzw: finish_x(u32(r[d]) ? __builtin_clz(u32(r[d])) : 32); break;
        case Op::cntlzd: finish_x(r[d] ? __builtin_clzll(r[d]) : 64); break;
        case Op::slw: { const u32 n = r[b] & 0x3F; finish_x(n < 32 ? u64(u32(r[d] << n)) : 0); break; }
        case Op::srw: { const u32 n = r[b] & 0x3F; finish_x(n < 32 ? u64(u32(r[d]) >> n) : 0); break; }
        case Op::sld: { const u32 n = r[b] & 0x7F; finish_x(n < 64 ? r[d] << n : 0); break; }
        case Op::srd: { const u32 n = r[b] & 0x7F; finish_x(n < 64 ? r[d] >> n : 0); break; }
        case Op::sraw: case Op::srawi: {
            const u32 n = in.op == Op::srawi ? in.sh() : u32(r[b] & 0x3F);
            const s32 v = s32(r[d]);
            if (n < 32) {
                c.xer_ca = v < 0 && n && (u32(v) & ((1u << n) - 1));
                finish_x(u64(s64(v >> n)));
            } else {
                c.xer_ca = v < 0;
                finish_x(v < 0 ? ~0ull : 0);
            }
            break;
        }
        case Op::srad: case Op::sradi: {
            const u32 n = in.op == Op::sradi ? in.sh64() : u32(r[b] & 0x7F);
            const s64 v = s64(r[d]);
            if (n < 64) {
                c.xer_ca = v < 0 && n && (u64(v) & ((1ull << n) - 1));
                finish_x(u64(v >> n));
            } else {
                c.xer_ca = v < 0;
                finish_x(v < 0 ? ~0ull : 0);
            }
            break;
        }

        // ---------------- rotates
        case Op::rlwinm: finish_x(rotl32(r[d], in.sh()) & mask64(in.mb() + 32, in.me() + 32)); break;
        case Op::rlwnm: finish_x(rotl32(r[d], r[b] & 31) & mask64(in.mb() + 32, in.me() + 32)); break;
        case Op::rlwimi: {
            const u64 m = mask64(in.mb() + 32, in.me() + 32);
            finish_x((rotl32(r[d], in.sh()) & m) | (r[a] & ~m));
            break;
        }
        case Op::rldicl: finish_x(rotl64(r[d], in.sh64()) & mask64(in.mb64(), 63)); break;
        case Op::rldicr: finish_x(rotl64(r[d], in.sh64()) & mask64(0, in.mb64())); break;
        case Op::rldic: finish_x(rotl64(r[d], in.sh64()) & mask64(in.mb64(), 63 - in.sh64())); break;
        case Op::rldimi: {
            const u64 m = mask64(in.mb64(), 63 - in.sh64());
            finish_x((rotl64(r[d], in.sh64()) & m) | (r[a] & ~m));
            break;
        }
        case Op::rldcl: finish_x(rotl64(r[d], r[b] & 63) & mask64(in.mb64(), 63)); break;
        case Op::rldcr: finish_x(rotl64(r[d], r[b] & 63) & mask64(0, in.mb64())); break;

        // ---------------- special registers and CR
        case Op::mfspr:
        case Op::mftb:
            switch (in.spr()) {
                case 1: r[d] = c.xer(); break;
                case 8: r[d] = c.lr; break;
                case 9: r[d] = c.ctr; break;
                case 256: r[d] = c.vrsave; break;
                case 268: r[d] = timebase(); break;
                case 269: r[d] = timebase() >> 32; break;
                default: return PpuStep::Invalid;
            }
            break;
        case Op::mtspr:
            switch (in.spr()) {
                case 1: c.set_xer(r[d]); break;
                case 8: c.lr = r[d]; break;
                case 9: c.ctr = r[d]; break;
                case 256: c.vrsave = u32(r[d]); break;
                default: return PpuStep::Invalid;
            }
            break;
        case Op::mfcr: r[d] = c.cr; break;
        case Op::mfocrf: {
            const u32 fxm = in.fxm();
            if (!fxm || (fxm & (fxm - 1))) return PpuStep::Invalid;  // exactly one field
            const u32 n = __builtin_clz(fxm) - 24;
            r[d] = u64(c.crf(n)) << (28 - 4 * n);
            break;
        }
        case Op::mtcrf:
        case Op::mtocrf:
            for (u32 n = 0; n < 8; ++n)
                if (in.fxm() & (0x80 >> n)) c.set_crf(n, u32(r[d] >> (28 - 4 * n)));
            break;
        case Op::mcrf: c.set_crf(in.bf(), c.crf(in.bfa())); break;
        case Op::crand: c.set_crbit(d, c.crbit(a) && c.crbit(b)); break;
        case Op::crandc: c.set_crbit(d, c.crbit(a) && !c.crbit(b)); break;
        case Op::creqv: c.set_crbit(d, c.crbit(a) == c.crbit(b)); break;
        case Op::crnand: c.set_crbit(d, !(c.crbit(a) && c.crbit(b))); break;
        case Op::crnor: c.set_crbit(d, !(c.crbit(a) || c.crbit(b))); break;
        case Op::cror: c.set_crbit(d, c.crbit(a) || c.crbit(b)); break;
        case Op::crorc: c.set_crbit(d, c.crbit(a) || !c.crbit(b)); break;
        case Op::crxor: c.set_crbit(d, c.crbit(a) != c.crbit(b)); break;

        // ---------------- branches
        case Op::b: {
            const u32 target = in.branch_target(c.cia);
            if (in.lk()) c.lr = c.cia + 4;
            next = target;
            break;
        }
        case Op::bc: {
            const bool taken = branch_taken(c, d, a, true);
            if (in.lk()) c.lr = c.cia + 4;
            if (taken) next = in.branch_target(c.cia);
            break;
        }
        case Op::bclr: {
            const u32 target = u32(c.lr) & ~3u;
            const bool taken = branch_taken(c, d, a, true);
            if (in.lk()) c.lr = c.cia + 4;
            if (taken) next = target;
            break;
        }
        case Op::bcctr: {
            const u32 target = u32(c.ctr) & ~3u;
            const bool taken = branch_taken(c, d, a, false);
            if (in.lk()) c.lr = c.cia + 4;
            if (taken) next = target;
            break;
        }

        // ---------------- integer loads/stores
        case Op::lbz: r[d] = mem.read<uint8_t>(ea_d(c, in)); break;
        case Op::lhz: r[d] = mem.read<uint16_t>(ea_d(c, in)); break;
        case Op::lha: r[d] = u64(s64(mem.read<int16_t>(ea_d(c, in)))); break;
        case Op::lwz: r[d] = mem.read<uint32_t>(ea_d(c, in)); break;
        case Op::ld: r[d] = mem.read<uint64_t>(ea_ds(c, in)); break;
        case Op::lwa: r[d] = u64(s64(mem.read<int32_t>(ea_ds(c, in)))); break;
        case Op::lbzu: case Op::lhzu: case Op::lhau: case Op::lwzu: {
            const u64 ea = r[a] + static_cast<s64>(in.simm());
            const GuestAddr g = GuestAddr(ea);
            r[d] = in.op == Op::lbzu ? mem.read<uint8_t>(g)
                 : in.op == Op::lhzu ? mem.read<uint16_t>(g)
                 : in.op == Op::lhau ? u64(s64(mem.read<int16_t>(g)))
                                     : mem.read<uint32_t>(g);
            r[a] = ea;
            break;
        }
        case Op::ldu: { const u64 ea = r[a] + static_cast<s64>(in.ds()); r[d] = mem.read<uint64_t>(GuestAddr(ea)); r[a] = ea; break; }
        case Op::stb: mem.write<uint8_t>(ea_d(c, in), uint8_t(r[d])); break;
        case Op::sth: mem.write<uint16_t>(ea_d(c, in), uint16_t(r[d])); break;
        case Op::stw: mem.write<uint32_t>(ea_d(c, in), uint32_t(r[d])); break;
        case Op::std: mem.write<uint64_t>(ea_ds(c, in), r[d]); break;
        case Op::stbu: case Op::sthu: case Op::stwu: {
            const u64 ea = r[a] + static_cast<s64>(in.simm());
            const GuestAddr g = GuestAddr(ea);
            if (in.op == Op::stbu) mem.write<uint8_t>(g, uint8_t(r[d]));
            else if (in.op == Op::sthu) mem.write<uint16_t>(g, uint16_t(r[d]));
            else mem.write<uint32_t>(g, uint32_t(r[d]));
            r[a] = ea;
            break;
        }
        case Op::stdu: { const u64 ea = r[a] + static_cast<s64>(in.ds()); mem.write<uint64_t>(GuestAddr(ea), r[d]); r[a] = ea; break; }
        case Op::lmw: { GuestAddr ea = ea_d(c, in); for (u32 i = d; i < 32; ++i, ea += 4) r[i] = mem.read<uint32_t>(ea); break; }
        case Op::stmw: { GuestAddr ea = ea_d(c, in); for (u32 i = d; i < 32; ++i, ea += 4) mem.write<uint32_t>(ea, u32(r[i])); break; }
        case Op::lbzx: r[d] = mem.read<uint8_t>(ea_x(c, in)); break;
        case Op::lhzx: r[d] = mem.read<uint16_t>(ea_x(c, in)); break;
        case Op::lhax: r[d] = u64(s64(mem.read<int16_t>(ea_x(c, in)))); break;
        case Op::lwzx: r[d] = mem.read<uint32_t>(ea_x(c, in)); break;
        case Op::lwax: r[d] = u64(s64(mem.read<int32_t>(ea_x(c, in)))); break;
        case Op::ldx: r[d] = mem.read<uint64_t>(ea_x(c, in)); break;
        case Op::lbzux: case Op::lhzux: case Op::lhaux: case Op::lwzux: case Op::lwaux: case Op::ldux: {
            const u64 ea = r[a] + r[b];
            const GuestAddr g = GuestAddr(ea);
            switch (in.op) {
                case Op::lbzux: r[d] = mem.read<uint8_t>(g); break;
                case Op::lhzux: r[d] = mem.read<uint16_t>(g); break;
                case Op::lhaux: r[d] = u64(s64(mem.read<int16_t>(g))); break;
                case Op::lwzux: r[d] = mem.read<uint32_t>(g); break;
                case Op::lwaux: r[d] = u64(s64(mem.read<int32_t>(g))); break;
                default: r[d] = mem.read<uint64_t>(g); break;
            }
            r[a] = ea;
            break;
        }
        case Op::stbx: mem.write<uint8_t>(ea_x(c, in), uint8_t(r[d])); break;
        case Op::sthx: mem.write<uint16_t>(ea_x(c, in), uint16_t(r[d])); break;
        case Op::stwx: mem.write<uint32_t>(ea_x(c, in), u32(r[d])); break;
        case Op::stdx: mem.write<uint64_t>(ea_x(c, in), r[d]); break;
        case Op::stbux: case Op::sthux: case Op::stwux: case Op::stdux: {
            const u64 ea = r[a] + r[b];
            const GuestAddr g = GuestAddr(ea);
            switch (in.op) {
                case Op::stbux: mem.write<uint8_t>(g, uint8_t(r[d])); break;
                case Op::sthux: mem.write<uint16_t>(g, uint16_t(r[d])); break;
                case Op::stwux: mem.write<uint32_t>(g, u32(r[d])); break;
                default: mem.write<uint64_t>(g, r[d]); break;
            }
            r[a] = ea;
            break;
        }
        // byte-reversed: the guest value is little-endian in memory
        case Op::lhbrx: r[d] = byteswap(mem.read<uint16_t>(ea_x(c, in))); break;
        case Op::lwbrx: r[d] = byteswap(mem.read<uint32_t>(ea_x(c, in))); break;
        case Op::sthbrx: mem.write<uint16_t>(ea_x(c, in), byteswap(uint16_t(r[d]))); break;
        case Op::stwbrx: mem.write<uint32_t>(ea_x(c, in), byteswap(u32(r[d]))); break;
        case Op::lwarx: case Op::ldarx: {
            const GuestAddr ea = ea_x(c, in);
            const bool w = in.op == Op::lwarx;
            r[d] = w ? u64(mem.read<uint32_t>(ea)) : mem.read<uint64_t>(ea);
            c.reserved = true;
            c.reserve_addr = ea;
            c.reserve_value = r[d];
            c.reserve_size = w ? 4 : 8;
            break;
        }
        case Op::stwcx_: case Op::stdcx_: {
            const GuestAddr ea = ea_x(c, in);
            const bool ok = in.op == Op::stwcx_ ? store_conditional<uint32_t>(c, mem, ea, u32(r[d]))
                                                : store_conditional<uint64_t>(c, mem, ea, r[d]);
            c.set_crf(0, (ok ? 2u : 0u) | (c.xer_so ? 1u : 0u));
            break;
        }

        // ---------------- cache, barriers, system
        case Op::dcbz: {
            const GuestAddr line = ea_x(c, in) & ~127u;  // Cell PPU cache line: 128 bytes
            std::memset(mem.host(line), 0, 128);
            break;
        }
        case Op::sync: std::atomic_thread_fence(std::memory_order_seq_cst); break;
        case Op::eieio: case Op::isync: case Op::dcbt: case Op::dcbtst: case Op::dcbst: case Op::dcbf: case Op::icbi:
            break;
        case Op::sc: c.cia = next; return PpuStep::Syscall;
        case Op::attn: return PpuStep::Halt;
        case Op::invalid: return PpuStep::Invalid;

        default:
            if (ppu_interp::exec_fpu(c, mem, in) || ppu_interp::exec_vmx(c, mem, in)) break;
            return PpuStep::Invalid;
    }
    c.cia = next;
    return PpuStep::Ok;
}

}  // namespace sly4
