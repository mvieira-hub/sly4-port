// Integer, CR, rotate, load/store, SPR and system-control semantics (see ppu_semantics.h).
//
// Where the ISA leaves bits undefined (mulhw high word, divw on overflow, mfocrf other
// fields) the semantics follow RPCS3, whose behaviour was checked on real PS3s.
#pragma once

namespace sly4::sem {

// ---------------- D-form arithmetic and logical immediates
SLY4_SEM(addi, r[d] = (a ? r[a] : 0) + static_cast<s64>(in.simm()))
SLY4_SEM(addis, r[d] = (a ? r[a] : 0) + (static_cast<s64>(in.simm()) << 16))
SLY4_SEM(addic, bool ca; r[d] = add_carry(r[a], static_cast<s64>(in.simm()), false, ca); c.xer_ca = ca)
SLY4_SEM(addic_, bool ca; r[d] = add_carry(r[a], static_cast<s64>(in.simm()), false, ca); c.xer_ca = ca;
         set_cr0(c, r[d]))
SLY4_SEM(subfic, bool ca; r[d] = add_carry(~r[a], static_cast<s64>(in.simm()), true, ca); c.xer_ca = ca)
SLY4_SEM(mulli, r[d] = r[a] * static_cast<u64>(static_cast<s64>(in.simm())))
SLY4_SEM(ori, r[a] = r[d] | in.uimm())
SLY4_SEM(oris, r[a] = r[d] | (u64(in.uimm()) << 16))
SLY4_SEM(xori, r[a] = r[d] ^ in.uimm())
SLY4_SEM(xoris, r[a] = r[d] ^ (u64(in.uimm()) << 16))
SLY4_SEM(andi_, r[a] = r[d] & in.uimm(); set_cr0(c, r[a]))
SLY4_SEM(andis_, r[a] = r[d] & (u64(in.uimm()) << 16); set_cr0(c, r[a]))

// ---------------- compares
SLY4_SEM(cmpi, if (in.l_cmp()) compare_signed(c, in.bf(), s64(r[a]), in.simm());
         else compare_signed(c, in.bf(), s32(r[a]), in.simm()))
SLY4_SEM(cmpli, if (in.l_cmp()) compare_unsigned(c, in.bf(), r[a], in.uimm());
         else compare_unsigned(c, in.bf(), u32(r[a]), in.uimm()))
SLY4_SEM(cmp, if (in.l_cmp()) compare_signed(c, in.bf(), s64(r[a]), s64(r[b]));
         else compare_signed(c, in.bf(), s32(r[a]), s32(r[b])))
SLY4_SEM(cmpl, if (in.l_cmp()) compare_unsigned(c, in.bf(), r[a], r[b]);
         else compare_unsigned(c, in.bf(), u32(r[a]), u32(r[b])))

// ---------------- XO-form arithmetic
SLY4_SEM(add, const u64 res = r[a] + r[b]; xo_result(c, in, res, add_overflow(r[a], r[b], res)))
SLY4_SEM(subf, const u64 res = r[b] - r[a]; xo_result(c, in, res, add_overflow(~r[a], r[b], res)))
SLY4_SEM(addc, bool ca; const u64 res = add_carry(r[a], r[b], false, ca); const bool ov = add_overflow(r[a], r[b], res);
         c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(adde, bool ca; const u64 res = add_carry(r[a], r[b], c.xer_ca, ca);
         const bool ov = add_overflow(r[a], r[b], res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(subfc, bool ca; const u64 res = add_carry(~r[a], r[b], true, ca);
         const bool ov = add_overflow(~r[a], r[b], res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(subfe, bool ca; const u64 res = add_carry(~r[a], r[b], c.xer_ca, ca);
         const bool ov = add_overflow(~r[a], r[b], res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(addze, bool ca; const u64 res = add_carry(r[a], 0, c.xer_ca, ca); const bool ov = add_overflow(r[a], 0, res);
         c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(addme, bool ca; const u64 res = add_carry(r[a], ~0ull, c.xer_ca, ca);
         const bool ov = add_overflow(r[a], ~0ull, res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(subfze, bool ca; const u64 res = add_carry(~r[a], 0, c.xer_ca, ca);
         const bool ov = add_overflow(~r[a], 0, res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(subfme, bool ca; const u64 res = add_carry(~r[a], ~0ull, c.xer_ca, ca);
         const bool ov = add_overflow(~r[a], ~0ull, res); c.xer_ca = ca; xo_result(c, in, res, ov))
SLY4_SEM(neg, xo_result(c, in, 0 - r[a], r[a] == 0x8000000000000000ull))
SLY4_SEM(mullw, const s64 p = s64(s32(r[a])) * s32(r[b]); xo_result(c, in, u64(p), p != s64(s32(p))))
SLY4_SEM(mulld, const s128 p = s128(s64(r[a])) * s64(r[b]); xo_result(c, in, u64(p), p != s128(s64(p))))
SLY4_SEM(mulhw, xo_result(c, in, u64((s64(s32(r[a])) * s32(r[b])) >> 32), false, false))
SLY4_SEM(mulhwu, xo_result(c, in, (u64(u32(r[a])) * u32(r[b])) >> 32, false, false))
SLY4_SEM(mulhd, xo_result(c, in, u64((s128(s64(r[a])) * s64(r[b])) >> 64), false, false))
SLY4_SEM(mulhdu, xo_result(c, in, u64((u128(r[a]) * r[b]) >> 64), false, false))
SLY4_SEM(divw, const s32 x = s32(r[a]), y = s32(r[b]); const bool ov = y == 0 || (x == INT32_MIN && y == -1);
         xo_result(c, in, ov ? 0 : u64(u32(x / y)), ov))
SLY4_SEM(divwu, const u32 x = u32(r[a]), y = u32(r[b]); xo_result(c, in, y ? u64(x / y) : 0, y == 0))
SLY4_SEM(divd, const s64 x = s64(r[a]), y = s64(r[b]); const bool ov = y == 0 || (x == INT64_MIN && y == -1);
         xo_result(c, in, ov ? 0 : u64(x / y), ov))
SLY4_SEM(divdu, xo_result(c, in, r[b] ? r[a] / r[b] : 0, r[b] == 0))

// ---------------- logical, extend, count, shifts (result in rA)
SLY4_SEM(and_, x_result(c, in, r[d] & r[b]))
SLY4_SEM(andc, x_result(c, in, r[d] & ~r[b]))
SLY4_SEM(or_, x_result(c, in, r[d] | r[b]))
SLY4_SEM(orc, x_result(c, in, r[d] | ~r[b]))
SLY4_SEM(xor_, x_result(c, in, r[d] ^ r[b]))
SLY4_SEM(nand, x_result(c, in, ~(r[d] & r[b])))
SLY4_SEM(nor, x_result(c, in, ~(r[d] | r[b])))
SLY4_SEM(eqv, x_result(c, in, ~(r[d] ^ r[b])))
SLY4_SEM(extsb, x_result(c, in, u64(s64(int8_t(r[d])))))
SLY4_SEM(extsh, x_result(c, in, u64(s64(int16_t(r[d])))))
SLY4_SEM(extsw, x_result(c, in, u64(s64(s32(r[d])))))
SLY4_SEM(cntlzw, x_result(c, in, u32(r[d]) ? __builtin_clz(u32(r[d])) : 32))
SLY4_SEM(cntlzd, x_result(c, in, r[d] ? __builtin_clzll(r[d]) : 64))
SLY4_SEM(slw, const u32 n = r[b] & 0x3F; x_result(c, in, n < 32 ? u64(u32(r[d] << n)) : 0))
SLY4_SEM(srw, const u32 n = r[b] & 0x3F; x_result(c, in, n < 32 ? u64(u32(r[d]) >> n) : 0))
SLY4_SEM(sld, const u32 n = r[b] & 0x7F; x_result(c, in, n < 64 ? r[d] << n : 0))
SLY4_SEM(srd, const u32 n = r[b] & 0x7F; x_result(c, in, n < 64 ? r[d] >> n : 0))

inline void sraw_common(PPUContext& c, const Insn& in, u32 n) {
    const s32 v = s32(c.gpr[in.rd()]);
    if (n < 32) {
        c.xer_ca = v < 0 && n && (u32(v) & ((1u << n) - 1));
        x_result(c, in, u64(s64(v >> n)));
    } else {
        c.xer_ca = v < 0;
        x_result(c, in, v < 0 ? ~0ull : 0);
    }
}
inline void srad_common(PPUContext& c, const Insn& in, u32 n) {
    const s64 v = s64(c.gpr[in.rd()]);
    if (n < 64) {
        c.xer_ca = v < 0 && n && (u64(v) & ((1ull << n) - 1));
        x_result(c, in, u64(v >> n));
    } else {
        c.xer_ca = v < 0;
        x_result(c, in, v < 0 ? ~0ull : 0);
    }
}
SLY4_SEM(sraw, sraw_common(c, in, u32(r[b] & 0x3F)))
SLY4_SEM(srawi, sraw_common(c, in, in.sh()))
SLY4_SEM(srad, srad_common(c, in, u32(r[b] & 0x7F)))
SLY4_SEM(sradi, srad_common(c, in, in.sh64()))

// ---------------- rotates
SLY4_SEM(rlwinm, x_result(c, in, rotl32(r[d], in.sh()) & mask64(in.mb() + 32, in.me() + 32)))
SLY4_SEM(rlwnm, x_result(c, in, rotl32(r[d], r[b] & 31) & mask64(in.mb() + 32, in.me() + 32)))
SLY4_SEM(rlwimi, const u64 m = mask64(in.mb() + 32, in.me() + 32);
         x_result(c, in, (rotl32(r[d], in.sh()) & m) | (r[a] & ~m)))
SLY4_SEM(rldicl, x_result(c, in, rotl64(r[d], in.sh64()) & mask64(in.mb64(), 63)))
SLY4_SEM(rldicr, x_result(c, in, rotl64(r[d], in.sh64()) & mask64(0, in.mb64())))
SLY4_SEM(rldic, x_result(c, in, rotl64(r[d], in.sh64()) & mask64(in.mb64(), 63 - in.sh64())))
SLY4_SEM(rldimi, const u64 m = mask64(in.mb64(), 63 - in.sh64());
         x_result(c, in, (rotl64(r[d], in.sh64()) & m) | (r[a] & ~m)))
SLY4_SEM(rldcl, x_result(c, in, rotl64(r[d], r[b] & 63) & mask64(in.mb64(), 63)))
SLY4_SEM(rldcr, x_result(c, in, rotl64(r[d], r[b] & 63) & mask64(0, in.mb64())))

// ---------------- special registers and CR
template <>
inline PpuStep op<Op::mfspr>(PPUContext& c, GuestMemory&, Insn in) {
    u64& rd = c.gpr[in.rd()];
    switch (in.spr()) {
        case 1: rd = c.xer(); break;
        case 8: rd = c.lr; break;
        case 9: rd = c.ctr; break;
        case 256: rd = c.vrsave; break;
        case 268: rd = timebase(); break;
        case 269: rd = timebase() >> 32; break;
        default: return PpuStep::Invalid;
    }
    return PpuStep::Ok;
}
template <>
inline PpuStep op<Op::mftb>(PPUContext& c, GuestMemory& mem, Insn in) {
    return op<Op::mfspr>(c, mem, in);
}
template <>
inline PpuStep op<Op::mtspr>(PPUContext& c, GuestMemory&, Insn in) {
    const u64 v = c.gpr[in.rd()];
    switch (in.spr()) {
        case 1: c.set_xer(v); break;
        case 8: c.lr = v; break;
        case 9: c.ctr = v; break;
        case 256: c.vrsave = u32(v); break;
        default: return PpuStep::Invalid;
    }
    return PpuStep::Ok;
}
SLY4_SEM(mfcr, r[d] = c.cr)
template <>
inline PpuStep op<Op::mfocrf>(PPUContext& c, GuestMemory&, Insn in) {
    const u32 fxm = in.fxm();
    if (!fxm || (fxm & (fxm - 1))) return PpuStep::Invalid;  // exactly one field
    const u32 n = __builtin_clz(fxm) - 24;
    c.gpr[in.rd()] = u64(c.crf(n)) << (28 - 4 * n);
    return PpuStep::Ok;
}
SLY4_SEM(mtcrf, for (u32 n = 0; n < 8; ++n) if (in.fxm() & (0x80 >> n)) c.set_crf(n, u32(r[d] >> (28 - 4 * n))))
SLY4_SEM(mtocrf, for (u32 n = 0; n < 8; ++n) if (in.fxm() & (0x80 >> n)) c.set_crf(n, u32(r[d] >> (28 - 4 * n))))
SLY4_SEM(mcrf, c.set_crf(in.bf(), c.crf(in.bfa())))
SLY4_SEM(crand, c.set_crbit(d, c.crbit(a) && c.crbit(b)))
SLY4_SEM(crandc, c.set_crbit(d, c.crbit(a) && !c.crbit(b)))
SLY4_SEM(creqv, c.set_crbit(d, c.crbit(a) == c.crbit(b)))
SLY4_SEM(crnand, c.set_crbit(d, !(c.crbit(a) && c.crbit(b))))
SLY4_SEM(crnor, c.set_crbit(d, !(c.crbit(a) || c.crbit(b))))
SLY4_SEM(cror, c.set_crbit(d, c.crbit(a) || c.crbit(b)))
SLY4_SEM(crorc, c.set_crbit(d, c.crbit(a) || !c.crbit(b)))
SLY4_SEM(crxor, c.set_crbit(d, c.crbit(a) != c.crbit(b)))

// ---------------- integer loads/stores
SLY4_SEM(lbz, r[d] = mem.read<uint8_t>(ea_d(c, in)))
SLY4_SEM(lhz, r[d] = mem.read<uint16_t>(ea_d(c, in)))
SLY4_SEM(lha, r[d] = u64(s64(mem.read<int16_t>(ea_d(c, in)))))
SLY4_SEM(lwz, r[d] = mem.read<uint32_t>(ea_d(c, in)))
SLY4_SEM(ld, r[d] = mem.read<uint64_t>(ea_ds(c, in)))
SLY4_SEM(lwa, r[d] = u64(s64(mem.read<int32_t>(ea_ds(c, in)))))
SLY4_SEM(lbzu, const u64 ea = r[a] + static_cast<s64>(in.simm()); r[d] = mem.read<uint8_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(lhzu, const u64 ea = r[a] + static_cast<s64>(in.simm()); r[d] = mem.read<uint16_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(lhau, const u64 ea = r[a] + static_cast<s64>(in.simm()); r[d] = u64(s64(mem.read<int16_t>(GuestAddr(ea))));
         r[a] = ea)
SLY4_SEM(lwzu, const u64 ea = r[a] + static_cast<s64>(in.simm()); r[d] = mem.read<uint32_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(ldu, const u64 ea = r[a] + static_cast<s64>(in.ds()); r[d] = mem.read<uint64_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(stb, mem.write<uint8_t>(ea_d(c, in), uint8_t(r[d])))
SLY4_SEM(sth, mem.write<uint16_t>(ea_d(c, in), uint16_t(r[d])))
SLY4_SEM(stw, mem.write<uint32_t>(ea_d(c, in), uint32_t(r[d])))
SLY4_SEM(std, mem.write<uint64_t>(ea_ds(c, in), r[d]))
SLY4_SEM(stbu, const u64 ea = r[a] + static_cast<s64>(in.simm()); mem.write<uint8_t>(GuestAddr(ea), uint8_t(r[d]));
         r[a] = ea)
SLY4_SEM(sthu, const u64 ea = r[a] + static_cast<s64>(in.simm()); mem.write<uint16_t>(GuestAddr(ea), uint16_t(r[d]));
         r[a] = ea)
SLY4_SEM(stwu, const u64 ea = r[a] + static_cast<s64>(in.simm()); mem.write<uint32_t>(GuestAddr(ea), u32(r[d]));
         r[a] = ea)
SLY4_SEM(stdu, const u64 ea = r[a] + static_cast<s64>(in.ds()); mem.write<uint64_t>(GuestAddr(ea), r[d]); r[a] = ea)
SLY4_SEM(lmw, GuestAddr ea = ea_d(c, in); for (u32 i = d; i < 32; ++i, ea += 4) r[i] = mem.read<uint32_t>(ea))
SLY4_SEM(stmw, GuestAddr ea = ea_d(c, in); for (u32 i = d; i < 32; ++i, ea += 4) mem.write<uint32_t>(ea, u32(r[i])))
SLY4_SEM(lbzx, r[d] = mem.read<uint8_t>(ea_x(c, in)))
SLY4_SEM(lhzx, r[d] = mem.read<uint16_t>(ea_x(c, in)))
SLY4_SEM(lhax, r[d] = u64(s64(mem.read<int16_t>(ea_x(c, in)))))
SLY4_SEM(lwzx, r[d] = mem.read<uint32_t>(ea_x(c, in)))
SLY4_SEM(lwax, r[d] = u64(s64(mem.read<int32_t>(ea_x(c, in)))))
SLY4_SEM(ldx, r[d] = mem.read<uint64_t>(ea_x(c, in)))
SLY4_SEM(lbzux, const u64 ea = r[a] + r[b]; r[d] = mem.read<uint8_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(lhzux, const u64 ea = r[a] + r[b]; r[d] = mem.read<uint16_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(lhaux, const u64 ea = r[a] + r[b]; r[d] = u64(s64(mem.read<int16_t>(GuestAddr(ea)))); r[a] = ea)
SLY4_SEM(lwzux, const u64 ea = r[a] + r[b]; r[d] = mem.read<uint32_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(lwaux, const u64 ea = r[a] + r[b]; r[d] = u64(s64(mem.read<int32_t>(GuestAddr(ea)))); r[a] = ea)
SLY4_SEM(ldux, const u64 ea = r[a] + r[b]; r[d] = mem.read<uint64_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(stbx, mem.write<uint8_t>(ea_x(c, in), uint8_t(r[d])))
SLY4_SEM(sthx, mem.write<uint16_t>(ea_x(c, in), uint16_t(r[d])))
SLY4_SEM(stwx, mem.write<uint32_t>(ea_x(c, in), u32(r[d])))
SLY4_SEM(stdx, mem.write<uint64_t>(ea_x(c, in), r[d]))
SLY4_SEM(stbux, const u64 ea = r[a] + r[b]; mem.write<uint8_t>(GuestAddr(ea), uint8_t(r[d])); r[a] = ea)
SLY4_SEM(sthux, const u64 ea = r[a] + r[b]; mem.write<uint16_t>(GuestAddr(ea), uint16_t(r[d])); r[a] = ea)
SLY4_SEM(stwux, const u64 ea = r[a] + r[b]; mem.write<uint32_t>(GuestAddr(ea), u32(r[d])); r[a] = ea)
SLY4_SEM(stdux, const u64 ea = r[a] + r[b]; mem.write<uint64_t>(GuestAddr(ea), r[d]); r[a] = ea)
// byte-reversed: the guest value is little-endian in memory
SLY4_SEM(lhbrx, r[d] = byteswap(mem.read<uint16_t>(ea_x(c, in))))
SLY4_SEM(lwbrx, r[d] = byteswap(mem.read<uint32_t>(ea_x(c, in))))
SLY4_SEM(sthbrx, mem.write<uint16_t>(ea_x(c, in), byteswap(uint16_t(r[d]))))
SLY4_SEM(stwbrx, mem.write<uint32_t>(ea_x(c, in), byteswap(u32(r[d]))))
SLY4_SEM(lwarx, const GuestAddr ea = ea_x(c, in); r[d] = mem.read<uint32_t>(ea); c.reserved = true;
         c.reserve_addr = ea; c.reserve_value = r[d]; c.reserve_size = 4)
SLY4_SEM(ldarx, const GuestAddr ea = ea_x(c, in); r[d] = mem.read<uint64_t>(ea); c.reserved = true;
         c.reserve_addr = ea; c.reserve_value = r[d]; c.reserve_size = 8)
SLY4_SEM(stwcx_, const bool ok = store_conditional<uint32_t>(c, mem, ea_x(c, in), u32(r[d]));
         c.set_crf(0, (ok ? 2u : 0u) | (c.xer_so ? 1u : 0u)))
SLY4_SEM(stdcx_, const bool ok = store_conditional<uint64_t>(c, mem, ea_x(c, in), r[d]);
         c.set_crf(0, (ok ? 2u : 0u) | (c.xer_so ? 1u : 0u)))

// ---------------- cache and barriers
SLY4_SEM(dcbz, std::memset(mem.host(ea_x(c, in) & ~127u), 0, 128))  // Cell PPU cache line: 128 bytes
SLY4_SEM(sync, std::atomic_thread_fence(std::memory_order_seq_cst))
SLY4_SEM(eieio, )
SLY4_SEM(isync, )
SLY4_SEM(dcbt, )
SLY4_SEM(dcbtst, )
SLY4_SEM(dcbst, )
SLY4_SEM(dcbf, )
SLY4_SEM(icbi, )

// Control flow and system instructions are executed by the interpreter / emitter.
#define SLY4_SEM_CONTROL(name)                                                     \
    template <>                                                                    \
    inline PpuStep op<Op::name>(PPUContext&, GuestMemory&, Insn) {                \
        return PpuStep::Invalid;                                                   \
    }
SLY4_SEM_CONTROL(b)
SLY4_SEM_CONTROL(bc)
SLY4_SEM_CONTROL(bclr)
SLY4_SEM_CONTROL(bcctr)
SLY4_SEM_CONTROL(sc)
SLY4_SEM_CONTROL(attn)
SLY4_SEM_CONTROL(tw)
SLY4_SEM_CONTROL(td)
SLY4_SEM_CONTROL(twi)
SLY4_SEM_CONTROL(tdi)
#undef SLY4_SEM_CONTROL

}  // namespace sly4::sem
