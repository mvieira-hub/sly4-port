// SPU instructions the EDGE zlib oracle (sly4_recomp spu-oracle) does not reach, checked
// against values worked out from the SPU ISA.
#include "check.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "cpu/spu_interpreter.h"
#include "cpu/spu_semantics.h"
#include "spu/spu_decoder.h"

using namespace sly4;

namespace {

struct Spu {
    std::unique_ptr<SpuLocalStore> ls = std::make_unique<SpuLocalStore>();
    SPUContext c{};
    Spu() { c.ls = ls->data; }
    void words(int r, uint32_t a, uint32_t b, uint32_t c_, uint32_t d) {
        c.gpr[r].u32(0) = a, c.gpr[r].u32(1) = b, c.gpr[r].u32(2) = c_, c.gpr[r].u32(3) = d;
    }
    void floats(int r, float a, float b, float c_, float d) {
        c.gpr[r].f32(0) = a, c.gpr[r].f32(1) = b, c.gpr[r].f32(2) = c_, c.gpr[r].f32(3) = d;
    }
    // The 11-bit opcode prefix of `op` (found by decoding), then its fields.
    SpuStep run(spu::Op op, uint32_t fields) {
        uint32_t opc = 0;
        for (uint32_t v = 0; v < 2048; ++v)
            if (spu::decode(v << 21).op == op) {
                opc = v;
                break;
            }
        spusem::SpuFloatMode fp;  // as SPU code runs (spu_run, spu_run_blocks)
        return spu_execute(c, spu::decode((opc << 21) | fields));
    }
    SpuStep rr(spu::Op op, int t, int a, int b) { return run(op, uint32_t(b) << 14 | uint32_t(a) << 7 | uint32_t(t)); }
    SpuStep rrr(spu::Op op, int dest, int a, int b, int t) {
        return run(op, uint32_t(dest) << 21 | uint32_t(b) << 14 | uint32_t(a) << 7 | uint32_t(t));
    }
    SpuStep ri7(spu::Op op, int t, int a, int i7) {
        return run(op, (uint32_t(i7) & 0x7F) << 14 | uint32_t(a) << 7 | uint32_t(t));
    }
    SpuStep ri8(spu::Op op, int t, int a, int u8) {
        return run(op, (uint32_t(u8) & 0xFF) << 14 | uint32_t(a) << 7 | uint32_t(t));
    }
    uint32_t w(int r, int i) const { return c.gpr[r].u32(i); }
    float f(int r, int i) const { return c.gpr[r].f32(i); }
    double d(int r, int i) const { return c.gpr[r].f64(i); }
};

}  // namespace

TEST(spu_multiply_and_count) {
    using spu::Op;
    Spu s;
    s.words(1, 0x0001FFFE, 0x00030000, 0x00004000, 0xFFFF0000);  // lows -2, 0, 0x4000, 0; highs 1, 3, 0, -1
    s.words(2, 0x00000003, 0x00000005, 0x00004000, 0x00020000);
    s.rr(Op::mpy, 3, 1, 2);
    CHECK_EQ(s.w(3, 0), 0xFFFFFFFAu);  // -2 * 3
    s.rr(Op::mpyu, 3, 1, 2);
    CHECK_EQ(s.w(3, 0), 0x2FFFAu);  // 0xFFFE * 3
    s.rr(Op::mpyh, 3, 1, 2);
    CHECK_EQ(s.w(3, 1), 0x000F0000u);  // (3 * 5) << 16
    s.rr(Op::mpys, 3, 1, 2);
    CHECK_EQ(s.w(3, 2), 0x1000u);  // (0x4000 * 0x4000) >> 16
    s.rr(Op::mpyhh, 3, 1, 2);
    CHECK_EQ(s.w(3, 3), 0xFFFFFFFEu);  // -1 * 2
    s.rr(Op::mpyhhu, 3, 1, 2);
    CHECK_EQ(s.w(3, 3), 0x1FFFEu);  // 0xFFFF * 2
    s.words(4, 0x00010000, 0, 0xFF, 0x80000000);
    s.rr(Op::clz, 5, 4, 0);
    CHECK(s.w(5, 0) == 15 && s.w(5, 1) == 32 && s.w(5, 2) == 24 && s.w(5, 3) == 0);
    s.rr(Op::cntb, 5, 4, 0);
    CHECK(s.w(5, 2) == 8 && s.w(5, 3) == 0x01000000);
}

TEST(spu_carry_borrow_and_bytes) {
    using spu::Op;
    Spu s;
    s.words(1, 0xFFFFFFFF, 3, 5, 0xFFFFFFFF);
    s.words(2, 1, 5, 3, 0);
    s.rr(Op::cg, 3, 1, 2);
    CHECK(s.w(3, 0) == 1 && s.w(3, 1) == 0);
    s.rr(Op::bg, 3, 1, 2);  // 1 when RA <= RB
    CHECK(s.w(3, 1) == 1 && s.w(3, 2) == 0);
    s.words(3, 1, 0, 1, 1);
    s.rr(Op::sfx, 3, 1, 2);  // RB - RA - (1 - carry)
    CHECK(s.w(3, 0) == 2 && s.w(3, 1) == 1 && s.w(3, 2) == 0xFFFFFFFE);
    s.words(3, 0, 0, 0, 1);
    s.rr(Op::cgx, 3, 1, 2);  // carry of RA + RB + carry
    CHECK(s.w(3, 3) == 1 && s.w(3, 0) == 1);
    s.words(3, 1, 0, 0, 0);
    s.words(1, 3, 5, 5, 5);
    s.words(2, 5, 5, 3, 5);
    s.rr(Op::bgx, 3, 1, 2);  // carry 1: RB >= RA; carry 0: RB > RA
    CHECK(s.w(3, 0) == 1 && s.w(3, 1) == 0 && s.w(3, 2) == 0 && s.w(3, 3) == 0);

    s.words(1, 0x01020304, 0x05090000, 0, 0);
    s.words(2, 0x0A0A0A0A, 0x09050000, 0, 0);
    s.rr(Op::sumb, 3, 1, 2);
    CHECK_EQ(s.w(3, 0), 0x0028000Au);  // RB bytes 40 | RA bytes 10
    s.rr(Op::avgb, 3, 1, 2);
    CHECK_EQ(s.w(3, 0), 0x06060707u);
    s.rr(Op::absdb, 3, 1, 2);
    CHECK_EQ(s.w(3, 1), 0x04040000u);
}

TEST(spu_masks_gather_and_rotates) {
    using spu::Op;
    Spu s;
    s.words(1, 0x00008001, 0, 0, 0);
    s.rr(Op::fsmb, 2, 1, 0);
    CHECK(s.w(2, 0) == 0xFF000000 && s.w(2, 1) == 0 && s.w(2, 3) == 0x000000FF);
    s.words(1, 0x0000000A, 0, 0, 0);
    s.rr(Op::fsmh, 2, 1, 0);  // bits 0b00001010 -> halfwords 4 and 6
    CHECK(s.w(2, 2) == 0xFFFF0000 && s.w(2, 3) == 0xFFFF0000 && s.w(2, 0) == 0);
    s.words(1, 1, 0, 3, 5);
    s.rr(Op::gb, 2, 1, 0);
    CHECK(s.w(2, 0) == 0xB && s.w(2, 1) == 0);
    s.words(1, 1, 2, 4, 8);
    s.rr(Op::orx, 2, 1, 0);
    CHECK(s.w(2, 0) == 15 && s.w(2, 3) == 0);

    s.words(1, 0x80000001, 0x80000000, 0x80000000, 0x12345678);
    s.words(2, 1, 0xFFFFFFFC, 0xFFFFFFFC, 33);
    s.rr(Op::rot, 3, 1, 2);
    CHECK_EQ(s.w(3, 0), 3u);
    s.rr(Op::rotm, 3, 1, 2);  // logical right by -RB
    CHECK(s.w(3, 1) == 0x08000000 && s.w(3, 2) == 0x08000000);
    s.rr(Op::rotma, 3, 1, 2);  // arithmetic right by -RB
    CHECK_EQ(s.w(3, 1), 0xF8000000u);
    s.rr(Op::shl, 3, 1, 2);  // 33 shifts everything out
    CHECK_EQ(s.w(3, 3), 0u);
    s.ri7(Op::rothi, 3, 1, 1);
    CHECK_EQ(s.w(3, 0), 0x00010002u);
    s.ri7(Op::rotmahi, 3, 1, -4);  // halfwords arithmetic right by 4
    CHECK_EQ(s.w(3, 1), 0xF8000000u);
    s.ri7(Op::rotqmbii, 3, 1, -3);  // quadword logical right by 3 bits
    CHECK_EQ(s.w(3, 0), 0x10000000u);
    s.words(4, 8, 0, 0, 0);
    s.ri7(Op::cdd, 5, 4, 0);  // insert a doubleword at byte 8
    CHECK(s.w(5, 0) == 0x10111213 && s.w(5, 2) == 0x00010203 && s.w(5, 3) == 0x04050607);
}

TEST(spu_shufb_selb) {
    using spu::Op;
    Spu s;
    s.words(1, 0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
    s.words(2, 0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F);
    s.words(3, 0x80C0E013, 0x0F1F0001, 0, 0);
    s.rrr(Op::shufb, 4, 1, 2, 3);
    CHECK(s.w(4, 0) == 0x00FF8013 && s.w(4, 1) == 0x0F1F0001);
    s.words(3, 0xFFFF0000, 0, 0, 0);
    s.rrr(Op::selb, 4, 1, 2, 3);  // bits of RB where the mask is set
    CHECK(s.w(4, 0) == 0x10110203 && s.w(4, 1) == 0x04050607);

    // The pshufb form against the byte-by-byte reference: random data and selectors
    // (every byte value, special ones included).
    uint64_t x = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    int same = 0;
    for (int k = 0; k < 20000; ++k) {
        SpuReg a, b, sel;
        for (int i = 0; i < 2; ++i) {
            a.d[i] = next();
            b.d[i] = next();
            sel.d[i] = next();
        }
        const SpuReg fast = spusem::shufb(a, b, sel), ref = spusem::shufb_bytes(a, b, sel);
        same += std::memcmp(fast.b, ref.b, 16) == 0;
    }
    CHECK_EQ(same, 20000);
}

TEST(spu_single_float) {
    using spu::Op;
    Spu s;
    s.floats(1, 1.5f, 3.0f, 2.0f, 1e-40f);  // the last one is a denormal: zero on the SPU
    s.floats(2, 2.25f, 0.5f, 3.0f, 1.0f);
    s.floats(3, 0, 0, 1.0f, 0);
    s.rr(Op::fa, 4, 1, 2);
    CHECK(s.f(4, 0) == 3.75f && s.f(4, 3) == 1.0f);
    s.rr(Op::fm, 4, 1, 2);
    CHECK(s.f(4, 1) == 1.5f && s.f(4, 3) == 0.0f);
    s.rrr(Op::fma, 4, 1, 2, 3);  // A * B + T
    CHECK_EQ(s.f(4, 2), 7.0f);
    s.rrr(Op::fms, 4, 1, 2, 3);  // A * B - T
    CHECK_EQ(s.f(4, 2), 5.0f);
    s.rrr(Op::fnms, 4, 1, 2, 3);  // T - A * B
    CHECK_EQ(s.f(4, 2), -5.0f);
    s.rr(Op::fcgt, 4, 1, 2);
    CHECK(s.w(4, 1) == 0xFFFFFFFF && s.w(4, 0) == 0);
    s.rr(Op::fcmgt, 4, 2, 1);
    CHECK(s.w(4, 3) == 0xFFFFFFFF);
    // A denormal result is zero too (with its sign). Exponent 255 is not infinity or NaN
    // but one more binade: 2^128 * 0 is 0, 1.0 * 0x7FC00007 is itself.
    s.floats(1, 1e-20f, -1e-20f, std::numeric_limits<float>::infinity(), 1.0f);
    s.floats(2, 1e-20f, 1e-20f, 0.0f, std::nanf("7"));
    s.rr(Op::fm, 4, 1, 2);
    CHECK(s.w(4, 0) == 0 && s.w(4, 1) == 0x80000000u && s.w(4, 2) == 0 && s.w(4, 3) == 0x7FC00007u);
    // Overflow saturates at the largest value instead of becoming infinity.
    s.words(1, 0x7F7FFFFFu, 0xFF000000u, 0x7F800000u, 0x7FFFFFFFu);  // 3.4e38, -1.7e38, 2^128, max
    s.words(2, 0x40000000u, 0x41000000u, 0x7F800000u, 0xBF800000u);  // 2, 8, 2^128, -1
    s.rr(Op::fm, 4, 1, 2);
    CHECK(s.w(4, 0) == 0x7FFFFFFFu && s.w(4, 1) == 0xFFFFFFFFu && s.w(4, 2) == 0x7FFFFFFFu && s.w(4, 3) == 0xFFFFFFFFu);
    s.rr(Op::fa, 4, 1, 1);  // 2 * 3.4e38 is exactly the largest value; -2^128 fits; 2^129 does not
    CHECK(s.w(4, 0) == 0x7FFFFFFFu && s.w(4, 1) == 0xFF800000u && s.w(4, 2) == 0x7FFFFFFFu && s.w(4, 3) == 0x7FFFFFFFu);
    s.ri8(Op::cflts, 4, 1, 173);  // exponent 255 saturates by its sign
    CHECK(s.w(4, 1) == 0x80000000u && s.w(4, 2) == 0x7FFFFFFFu);
    // Comparisons: 0x7FFFFFFF is greater than any other value; +0, -0 and denormals are equal.
    s.words(1, 0x7FFFFFFFu, 0x80000000u, 0x00000001u, 0xFF800000u);
    s.words(2, 0x7F7FFFFFu, 0x00000000u, 0x80000000u, 0xBF800000u);
    s.rr(Op::fcgt, 4, 1, 2);
    CHECK(s.w(4, 0) == ~0u && s.w(4, 1) == 0 && s.w(4, 2) == 0 && s.w(4, 3) == 0);
    s.rr(Op::fceq, 4, 1, 2);
    CHECK(s.w(4, 0) == 0 && s.w(4, 1) == ~0u && s.w(4, 2) == ~0u && s.w(4, 3) == 0);
    s.rr(Op::fcmgt, 4, 1, 2);
    CHECK(s.w(4, 0) == ~0u && s.w(4, 1) == 0 && s.w(4, 3) == ~0u);

    s.floats(1, 3.7f, 1.5f, 3e9f, -1.0f);
    s.ri8(Op::cflts, 4, 1, 173);  // scale 2^0, truncate, saturate
    CHECK(s.w(4, 0) == 3 && s.w(4, 1) == 1 && s.w(4, 2) == 0x7FFFFFFF && s.w(4, 3) == 0xFFFFFFFF);
    s.ri8(Op::cflts, 4, 1, 169);  // scale 2^4
    CHECK(s.w(4, 1) == 24);
    s.ri8(Op::cfltu, 4, 1, 173);
    CHECK(s.w(4, 3) == 0 && s.w(4, 2) == 3000000000u);
    s.words(1, 5, 32, 0xFFFFFFFF, 0);
    s.ri8(Op::csflt, 4, 1, 155);  // scale 2^0
    CHECK(s.f(4, 0) == 5.0f && s.f(4, 2) == -1.0f);
    s.ri8(Op::csflt, 4, 1, 151);  // divided by 2^4
    CHECK(s.f(4, 1) == 2.0f);
    s.ri8(Op::cuflt, 4, 1, 155);
    CHECK(s.f(4, 2) == 4294967296.0f);
}

TEST(spu_double_float) {
    using spu::Op;
    Spu s;
    s.c.gpr[1].f64(0) = 1.5, s.c.gpr[1].f64(1) = 2.0;
    s.c.gpr[2].f64(0) = 2.5, s.c.gpr[2].f64(1) = 3.0;
    s.c.gpr[3].f64(0) = 0, s.c.gpr[3].f64(1) = 1.0;
    s.rr(Op::dfa, 4, 1, 2);
    CHECK(s.d(4, 0) == 4.0 && s.d(4, 1) == 5.0);
    s.c.gpr[4] = s.c.gpr[3];
    s.rr(Op::dfma, 4, 1, 2);  // RT + RA * RB
    CHECK_EQ(s.d(4, 1), 7.0);
    s.c.gpr[4] = s.c.gpr[3];
    s.rr(Op::dfnms, 4, 1, 2);  // RT - RA * RB
    CHECK_EQ(s.d(4, 1), -5.0);
    s.floats(5, 1.5f, 0, -2.0f, 0);
    s.rr(Op::fesd, 6, 5, 0);
    CHECK(s.d(6, 0) == 1.5 && s.d(6, 1) == -2.0);
    s.rr(Op::frds, 7, 6, 0);
    CHECK(s.f(7, 0) == 1.5f && s.w(7, 1) == 0 && s.f(7, 2) == -2.0f);
    // Double precision keeps denormals, operands and results.
    s.c.gpr[1].f64(0) = 1e-300, s.c.gpr[2].f64(0) = 1e-10;
    s.rr(Op::dfm, 4, 1, 2);
    CHECK_EQ(s.d(4, 0), 1e-310);
    s.c.gpr[1].f64(0) = 1e-310, s.c.gpr[2].f64(0) = 1.0;
    s.rr(Op::dfceq, 4, 1, 3);  // 1e-310 == 0?
    CHECK_EQ(s.c.gpr[4].u64(0), 0ull);
    s.ri7(Op::dftsv, 4, 1, 0x02);  // +denormal
    CHECK_EQ(s.c.gpr[4].u64(0), ~0ull);
    s.c.gpr[8].f64(0) = std::nan("");
    s.c.gpr[8].f64(1) = -0.0;
    s.ri7(Op::dftsv, 9, 8, 0x44);  // NaN or -0
    CHECK(s.c.gpr[9].u64(0) == ~0ull && s.c.gpr[9].u64(1) == ~0ull);
    s.ri7(Op::dftsv, 9, 8, 0x08);  // +0 only
    CHECK(s.c.gpr[9].u64(1) == 0);
}
