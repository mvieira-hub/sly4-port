// SPU integer, logical, shift/rotate, compare, mask and load/store instructions.
// Included by spu_semantics.h.
#pragma once

namespace sly4::spusem {

// ---------------------------------------------------------------- loads and stores
// Local store addresses wrap at 256 KiB and ignore the low 4 bits.
SPU_SEM(lqd, c.read_quad(RA.pref() + u32(in.i10() * 16), RT))
SPU_SEM(lqx, c.read_quad(RA.pref() + RB.pref(), RT))
SPU_SEM(lqa, c.read_quad(u32(in.i16() * 4), RT))
SPU_SEM(lqr, c.read_quad(c.pc + u32(in.i16() * 4), RT))
SPU_SEM(stqd, c.write_quad(RA.pref() + u32(in.i10() * 16), RT))
SPU_SEM(stqx, c.write_quad(RA.pref() + RB.pref(), RT))
SPU_SEM(stqa, c.write_quad(u32(in.i16() * 4), RT))
SPU_SEM(stqr, c.write_quad(c.pc + u32(in.i16() * 4), RT))

// Insertion controls: the "identity" shuffle pattern for inserting into the quadword
// with the element at (address & 15) replaced by the preferred-slot bytes.
SLY4_SPU_INLINE void insertion_control(SpuReg& r, u32 addr, int size) {
    for (int i = 0; i < 16; ++i) r.u8(i) = u8(0x10 + i);
    const u32 at = addr & 15 & ~u32(size - 1);
    const int first = size == 8 ? 0 : 4 - size;  // byte of the preferred slot the element starts at
    for (int k = 0; k < size; ++k) r.u8(int(at) + k) = u8(first + k);
}
SPU_SEM(cbd, SpuReg r; insertion_control(r, RA.pref() + u32(in.i7()), 1); RT = r)
SPU_SEM(chd, SpuReg r; insertion_control(r, RA.pref() + u32(in.i7()), 2); RT = r)
SPU_SEM(cwd, SpuReg r; insertion_control(r, RA.pref() + u32(in.i7()), 4); RT = r)
SPU_SEM(cdd, SpuReg r; insertion_control(r, RA.pref() + u32(in.i7()), 8); RT = r)
SPU_SEM(cbx, SpuReg r; insertion_control(r, RA.pref() + RB.pref(), 1); RT = r)
SPU_SEM(chx, SpuReg r; insertion_control(r, RA.pref() + RB.pref(), 2); RT = r)
SPU_SEM(cwx, SpuReg r; insertion_control(r, RA.pref() + RB.pref(), 4); RT = r)
SPU_SEM(cdx, SpuReg r; insertion_control(r, RA.pref() + RB.pref(), 8); RT = r)

// ---------------------------------------------------------------- immediates
SPU_WORDS(il, u32(in.i16()))
SPU_HALVES(ilh, in.u16())
SPU_WORDS(ilhu, in.u16() << 16)
SPU_WORDS(ila, in.u18())
SPU_SEM(iohl, for (int i = 0; i < 4; ++i) RT.u32(i) |= in.u16())
SPU_BYTES(fsmbi, (in.u16() >> (15 - i)) & 1 ? 0xFF : 0)

// ---------------------------------------------------------------- arithmetic
SPU_WORDS(a, RA.u32(i) + RB.u32(i))
SPU_HALVES(ah, RA.u16(i) + RB.u16(i))
SPU_WORDS(ai, RA.u32(i) + u32(in.i10()))
SPU_HALVES(ahi, RA.u16(i) + u16(in.i10()))
SPU_WORDS(sf, RB.u32(i) - RA.u32(i))
SPU_HALVES(sfh, RB.u16(i) - RA.u16(i))
SPU_WORDS(sfi, u32(in.i10()) - RA.u32(i))
SPU_HALVES(sfhi, u16(in.i10()) - RA.u16(i))
// Extended arithmetic: the carry / borrow comes in bit 0 of each RT word.
SPU_WORDS(addx, RA.u32(i) + RB.u32(i) + (RT.u32(i) & 1))
SPU_WORDS(sfx, RB.u32(i) - RA.u32(i) - ((RT.u32(i) & 1) ^ 1))
SPU_WORDS(cg, u32((u64(RA.u32(i)) + RB.u32(i)) >> 32))
SPU_WORDS(cgx, u32((u64(RA.u32(i)) + RB.u32(i) + (RT.u32(i) & 1)) >> 32))
SPU_WORDS(bg, RA.u32(i) <= RB.u32(i) ? 1u : 0u)  // 1 = no borrow from RB - RA
SPU_WORDS(bgx, (RT.u32(i) & 1) ? (RB.u32(i) >= RA.u32(i) ? 1u : 0u) : (RB.u32(i) > RA.u32(i) ? 1u : 0u))

// 16 x 16 multiplies on halfwords of each word (low = bits 0-15 of the word).
SLY4_SPU_INLINE s32 lo16s(u32 x) { return s16(x); }
SLY4_SPU_INLINE s32 hi16s(u32 x) { return s16(x >> 16); }
SPU_WORDS(mpy, u32(lo16s(RA.u32(i)) * lo16s(RB.u32(i))))
SPU_WORDS(mpyu, (RA.u32(i) & 0xFFFF) * (RB.u32(i) & 0xFFFF))
SPU_WORDS(mpyi, u32(lo16s(RA.u32(i)) * in.i10()))
SPU_WORDS(mpyui, (RA.u32(i) & 0xFFFF) * (u32(in.i10()) & 0xFFFF))
SPU_WORDS(mpyh, ((RA.u32(i) >> 16) * (RB.u32(i) & 0xFFFF)) << 16)
SPU_WORDS(mpys, u32((lo16s(RA.u32(i)) * lo16s(RB.u32(i))) >> 16))
SPU_WORDS(mpyhh, u32(hi16s(RA.u32(i)) * hi16s(RB.u32(i))))
SPU_WORDS(mpyhhu, (RA.u32(i) >> 16) * (RB.u32(i) >> 16))
SPU_WORDS(mpyhha, RT.u32(i) + u32(hi16s(RA.u32(i)) * hi16s(RB.u32(i))))
SPU_WORDS(mpyhhau, RT.u32(i) + (RA.u32(i) >> 16) * (RB.u32(i) >> 16))
// mpya is RRR: C = A * B + T.
SPU_SEM(mpya, SpuReg r; for (int i = 0; i < 4; ++i) r.u32(i) = u32(lo16s(RA.u32(i)) * lo16s(RB.u32(i))) + RT.u32(i);
        c.gpr[cc] = r)

SPU_WORDS(clz, u32(std::countl_zero(RA.u32(i))))
SPU_BYTES(cntb, std::popcount(RA.u8(i)))
SPU_BYTES(avgb, (u32(RA.u8(i)) + RB.u8(i) + 1) >> 1)
SPU_BYTES(absdb, RA.u8(i) > RB.u8(i) ? RA.u8(i) - RB.u8(i) : RB.u8(i) - RA.u8(i))
// sumb: each word gets the sum of RB's bytes in its upper half and RA's in the lower.
SPU_SEM(sumb, SpuReg r; for (int i = 0; i < 4; ++i) {
    u32 sa = 0, sb = 0;
    for (int k = 0; k < 4; ++k) {
        sa += RA.u8(4 * i + k);
        sb += RB.u8(4 * i + k);
    }
    r.u32(i) = (sb << 16) | (sa & 0xFFFF);
} RT = r)
SPU_HALVES(xsbh, s16(s8(RA.u16(i))))
SPU_WORDS(xshw, u32(s32(s16(RA.u32(i)))))
SPU_SEM(xswd, SpuReg r; for (int i = 0; i < 2; ++i) r.u64(i) = u64(s64(s32(RA.u64(i)))); RT = r)

// Form select masks from the low bits of the preferred word; gather the low bit of
// each element into the preferred word.
SPU_BYTES(fsmb, (RA.pref() >> (15 - i)) & 1 ? 0xFF : 0)
SPU_HALVES(fsmh, (RA.pref() >> (7 - i)) & 1 ? 0xFFFF : 0)
SPU_WORDS(fsm, (RA.pref() >> (3 - i)) & 1 ? 0xFFFFFFFFu : 0u)
SPU_SEM(gbb, u32 v = 0; for (int i = 0; i < 16; ++i) v = (v << 1) | (RA.u8(i) & 1); SpuReg r{}; r.u32(0) = v; RT = r)
SPU_SEM(gbh, u32 v = 0; for (int i = 0; i < 8; ++i) v = (v << 1) | (RA.u16(i) & 1); SpuReg r{}; r.u32(0) = v; RT = r)
SPU_SEM(gb, u32 v = 0; for (int i = 0; i < 4; ++i) v = (v << 1) | (RA.u32(i) & 1); SpuReg r{}; r.u32(0) = v; RT = r)

// ---------------------------------------------------------------- logical
SLY4_SPU_INLINE __m128i vnot(__m128i v) { return _mm_xor_si128(v, _mm_set1_epi32(-1)); }
SPU_SEM(and_, set_vq(RT, _mm_and_si128(vq(RA), vq(RB))))
SPU_SEM(andc, set_vq(RT, _mm_andnot_si128(vq(RB), vq(RA))))
SPU_SEM(nand, set_vq(RT, vnot(_mm_and_si128(vq(RA), vq(RB)))))
SPU_SEM(or_, set_vq(RT, _mm_or_si128(vq(RA), vq(RB))))
SPU_SEM(orc, set_vq(RT, _mm_or_si128(vq(RA), vnot(vq(RB)))))
SPU_SEM(nor, set_vq(RT, vnot(_mm_or_si128(vq(RA), vq(RB)))))
SPU_SEM(xor_, set_vq(RT, _mm_xor_si128(vq(RA), vq(RB))))
SPU_SEM(eqv, set_vq(RT, vnot(_mm_xor_si128(vq(RA), vq(RB)))))
SPU_BYTES(andbi, RA.u8(i) & u8(in.i10()))
SPU_HALVES(andhi, RA.u16(i) & u16(in.i10()))
SPU_WORDS(andi, RA.u32(i) & u32(in.i10()))
SPU_BYTES(orbi, RA.u8(i) | u8(in.i10()))
SPU_HALVES(orhi, RA.u16(i) | u16(in.i10()))
SPU_WORDS(ori, RA.u32(i) | u32(in.i10()))
SPU_BYTES(xorbi, RA.u8(i) ^ u8(in.i10()))
SPU_HALVES(xorhi, RA.u16(i) ^ u16(in.i10()))
SPU_WORDS(xori, RA.u32(i) ^ u32(in.i10()))
SPU_SEM(orx, SpuReg r{}; r.u32(0) = RA.u32(0) | RA.u32(1) | RA.u32(2) | RA.u32(3); RT = r)
// RRR: C = (A & ~T) | (B & T).
SPU_SEM(selb, SpuReg r; set_vq(r, _mm_or_si128(_mm_andnot_si128(vq(RT), vq(RA)), _mm_and_si128(vq(RB), vq(RT))));
        c.gpr[cc] = r)
// RRR: C = bytes of A:B picked by T, with the 0x80/0xC0/0xE0 constants.
// shufb, byte by byte: the reference (and the path without SSE4.1).
SLY4_SPU_INLINE SpuReg shufb_bytes(const SpuReg& a, const SpuReg& b, const SpuReg& sel) {
    SpuReg r;
    for (int i = 0; i < 16; ++i) {
        const u8 s = sel.u8(i);
        if ((s & 0xC0) == 0x80)
            r.u8(i) = 0x00;
        else if ((s & 0xE0) == 0xC0)
            r.u8(i) = 0xFF;
        else if ((s & 0xE0) == 0xE0)
            r.u8(i) = 0x80;
        else
            r.u8(i) = (s & 0x10) ? b.u8(s & 15) : a.u8(s & 15);
    }
    return r;
}

// The same with pshufb. The registers hold the bytes reversed (host byte k is SPU byte
// 15 - k), so SPU byte s & 15 is host byte (~s) & 15; selectors with the top bit set give
// 0x00, 0xFF or 0x80. It was the most frequent instruction in the menu's SPU jobs.
SLY4_SPU_INLINE SpuReg shufb(const SpuReg& a, const SpuReg& b, const SpuReg& sel) {
#ifdef __SSE4_1__
    __m128i va, vb, vs;
    std::memcpy(&va, a.b, 16);
    std::memcpy(&vb, b.b, 16);
    std::memcpy(&vs, sel.b, 16);
    const __m128i idx = _mm_andnot_si128(vs, _mm_set1_epi8(0x0F));
    const __m128i use_b = _mm_cmpeq_epi8(_mm_and_si128(vs, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(va, idx), _mm_shuffle_epi8(vb, idx), use_b);
    const __m128i top3 = _mm_and_si128(vs, _mm_set1_epi8(char(0xE0)));
    const __m128i ff = _mm_cmpeq_epi8(top3, _mm_set1_epi8(char(0xC0)));                       // 110xxxxx
    const __m128i x80 = _mm_and_si128(_mm_cmpeq_epi8(top3, _mm_set1_epi8(char(0xE0))), _mm_set1_epi8(char(0x80)));
    const __m128i special = _mm_or_si128(ff, x80);                                            // 10xxxxxx: 0
    r = _mm_blendv_epi8(r, special, vs);  // blendv takes the top bit of each selector byte
    SpuReg out;
    std::memcpy(out.b, &r, 16);
    return out;
#else
    return shufb_bytes(a, b, sel);
#endif
}

SPU_SEM(shufb, c.gpr[cc] = shufb(RA, RB, RT))

// ---------------------------------------------------------------- shifts and rotates
// Element shifts: left by the count, or right ("rotate and mask") by the negated count;
// a count of the element size or more gives 0 (or the sign for the arithmetic forms).
SLY4_SPU_INLINE u32 shl_w(u32 x, u32 n) { return n > 31 ? 0 : x << n; }
SLY4_SPU_INLINE u16 shl_h(u16 x, u32 n) { return n > 15 ? 0 : u16(x << n); }
SLY4_SPU_INLINE u32 shr_w(u32 x, u32 n) { return n > 31 ? 0 : x >> n; }
SLY4_SPU_INLINE u16 shr_h(u16 x, u32 n) { return n > 15 ? 0 : u16(x >> n); }
SLY4_SPU_INLINE u32 sar_w(u32 x, u32 n) { return u32(s32(x) >> (n > 31 ? 31 : n)); }
SLY4_SPU_INLINE u16 sar_h(u16 x, u32 n) { return u16(s16(x) >> (n > 15 ? 15 : n)); }

SPU_HALVES(shlh, shl_h(RA.u16(i), RB.u16(i) & 0x1F))
SPU_HALVES(shlhi, shl_h(RA.u16(i), u32(in.i7()) & 0x1F))
SPU_WORDS(shl, shl_w(RA.u32(i), RB.u32(i) & 0x3F))
SPU_WORDS(shli, shl_w(RA.u32(i), u32(in.i7()) & 0x3F))
SPU_HALVES(roth, rotl16(RA.u16(i), RB.u16(i) & 0xF))
SPU_HALVES(rothi, rotl16(RA.u16(i), u32(in.i7()) & 0xF))
SPU_WORDS(rot, rotl32(RA.u32(i), RB.u32(i) & 0x1F))
SPU_WORDS(roti, rotl32(RA.u32(i), u32(in.i7()) & 0x1F))
SPU_HALVES(rothm, shr_h(RA.u16(i), (0u - RB.u16(i)) & 0x1F))
SPU_HALVES(rothmi, shr_h(RA.u16(i), (0u - u32(in.i7())) & 0x1F))
SPU_WORDS(rotm, shr_w(RA.u32(i), (0u - RB.u32(i)) & 0x3F))
SPU_WORDS(rotmi, shr_w(RA.u32(i), (0u - u32(in.i7())) & 0x3F))
SPU_HALVES(rotmah, sar_h(RA.u16(i), (0u - RB.u16(i)) & 0x1F))
SPU_HALVES(rotmahi, sar_h(RA.u16(i), (0u - u32(in.i7())) & 0x1F))
SPU_WORDS(rotma, sar_w(RA.u32(i), (0u - RB.u32(i)) & 0x3F))
SPU_WORDS(rotmai, sar_w(RA.u32(i), (0u - u32(in.i7())) & 0x3F))

// Quadword shifts by bits (0-7) or bytes (0-31, >= 16 clears) from the preferred word.
SLY4_SPU_INLINE u128 qshl(u128 x, u32 bits) { return bits > 127 ? 0 : x << bits; }
SLY4_SPU_INLINE u128 qshr(u128 x, u32 bits) { return bits > 127 ? 0 : x >> bits; }
SPU_SEM(shlqbi, set_q(RT, qshl(q(RA), RB.pref() & 7)))
SPU_SEM(shlqbii, set_q(RT, qshl(q(RA), u32(in.i7()) & 7)))
// By bytes with pshufb: host byte k takes host byte k - n (left) or k + n (right); the
// masks are windows into these tables, 0x80 giving zero.
alignas(16) inline constexpr u8 kShiftWindow[48] = {
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
alignas(16) inline constexpr u8 kRotateWindow[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                                     0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
SLY4_SPU_INLINE void shuffle_bytes(SpuReg& rt, const SpuReg& ra, const u8* mask) {
#ifdef __SSSE3__
    set_vq(rt, _mm_shuffle_epi8(vq(ra), _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask))));
#else
    SpuReg r;
    for (int k = 0; k < 16; ++k) r.b[k] = (mask[k] & 0x80) ? 0 : ra.b[mask[k] & 15];
    rt = r;
#endif
}
SLY4_SPU_INLINE void qshl_bytes(SpuReg& rt, const SpuReg& ra, u32 n) { shuffle_bytes(rt, ra, kShiftWindow + 16 - (n > 16 ? 16 : n)); }
SLY4_SPU_INLINE void qshr_bytes(SpuReg& rt, const SpuReg& ra, u32 n) { shuffle_bytes(rt, ra, kShiftWindow + 16 + (n > 16 ? 16 : n)); }
SLY4_SPU_INLINE void qrotl_bytes(SpuReg& rt, const SpuReg& ra, u32 n) { shuffle_bytes(rt, ra, kRotateWindow + 16 - (n & 15)); }
SPU_SEM(shlqby, qshl_bytes(RT, RA, RB.pref() & 0x1F))
SPU_SEM(shlqbyi, qshl_bytes(RT, RA, u32(in.i7()) & 0x1F))
SPU_SEM(shlqbybi, qshl_bytes(RT, RA, (RB.pref() >> 3) & 0x1F))
SPU_SEM(rotqbi, set_q(RT, rotl128(q(RA), RB.pref() & 7)))
SPU_SEM(rotqbii, set_q(RT, rotl128(q(RA), u32(in.i7()) & 7)))
SPU_SEM(rotqby, qrotl_bytes(RT, RA, RB.pref() & 0xF))
SPU_SEM(rotqbyi, qrotl_bytes(RT, RA, u32(in.i7()) & 0xF))
SPU_SEM(rotqbybi, qrotl_bytes(RT, RA, (RB.pref() >> 3) & 0xF))
SPU_SEM(rotqmbi, set_q(RT, qshr(q(RA), (0u - RB.pref()) & 7)))
SPU_SEM(rotqmbii, set_q(RT, qshr(q(RA), (0u - u32(in.i7())) & 7)))
SPU_SEM(rotqmby, qshr_bytes(RT, RA, (0u - RB.pref()) & 0x1F))
SPU_SEM(rotqmbyi, qshr_bytes(RT, RA, (0u - u32(in.i7())) & 0x1F))
SPU_SEM(rotqmbybi, qshr_bytes(RT, RA, (0u - (RB.pref() >> 3)) & 0x1F))

// ---------------------------------------------------------------- compares
#define SPU_MASK(c_) ((c_) ? ~0u : 0u)
SPU_BYTES(ceqb, SPU_MASK(RA.u8(i) == RB.u8(i)))
SPU_BYTES(ceqbi, SPU_MASK(RA.u8(i) == u8(in.i10())))
SPU_HALVES(ceqh, SPU_MASK(RA.u16(i) == RB.u16(i)))
SPU_HALVES(ceqhi, SPU_MASK(RA.u16(i) == u16(in.i10())))
SPU_WORDS(ceq, SPU_MASK(RA.u32(i) == RB.u32(i)))
SPU_WORDS(ceqi, SPU_MASK(RA.u32(i) == u32(in.i10())))
SPU_BYTES(cgtb, SPU_MASK(s8(RA.u8(i)) > s8(RB.u8(i))))
SPU_BYTES(cgtbi, SPU_MASK(s8(RA.u8(i)) > s8(in.i10())))
SPU_HALVES(cgth, SPU_MASK(s16(RA.u16(i)) > s16(RB.u16(i))))
SPU_HALVES(cgthi, SPU_MASK(s16(RA.u16(i)) > s16(in.i10())))
SPU_WORDS(cgt, SPU_MASK(s32(RA.u32(i)) > s32(RB.u32(i))))
SPU_WORDS(cgti, SPU_MASK(s32(RA.u32(i)) > in.i10()))
SPU_BYTES(clgtb, SPU_MASK(RA.u8(i) > RB.u8(i)))
SPU_BYTES(clgtbi, SPU_MASK(RA.u8(i) > u8(in.i10())))
SPU_HALVES(clgth, SPU_MASK(RA.u16(i) > RB.u16(i)))
SPU_HALVES(clgthi, SPU_MASK(RA.u16(i) > u16(in.i10())))
SPU_WORDS(clgt, SPU_MASK(RA.u32(i) > RB.u32(i)))
SPU_WORDS(clgti, SPU_MASK(RA.u32(i) > u32(in.i10())))
#undef SPU_MASK

// Halt: stop the SPU when the preferred words compare true.
#define SPU_HALT(name, cond) \
    template <>              \
    SLY4_SPU_INLINE SpuStep op<Op::name>(SPUContext & c, Insn in) { \
        const u32 x = c.gpr[in.ra()].pref(), y = c.gpr[in.rb()].pref(); \
        (void)y; \
        return (cond) ? SpuStep::Halt : SpuStep::Ok; \
    }
SPU_HALT(heq, x == y)
SPU_HALT(heqi, x == u32(in.i10()))
SPU_HALT(hgt, s32(x) > s32(y))
SPU_HALT(hgti, s32(x) > in.i10())
SPU_HALT(hlgt, x > y)
SPU_HALT(hlgti, x > u32(in.i10()))
#undef SPU_HALT

}  // namespace sly4::spusem
