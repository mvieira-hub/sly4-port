// PPU interpreter: what the QEMU oracle (tools/ppc/oracle) cannot cover - branches,
// reservations, traps/system, lmw/stmw and the Cell-only lvlx/lvrx/stvlx/stvrx - plus
// a few spot checks that keep the CI honest without QEMU.
#include "check.h"

#include "cpu/ppu_interpreter.h"

using namespace sly4;
using ppu::Op;

namespace {

constexpr uint32_t kCode = 0x10000, kData = 0x20000;

uint32_t x31(uint32_t d, uint32_t a, uint32_t b, uint32_t xo, uint32_t rc = 0) {
    return 31u << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc;
}
uint32_t bc(uint32_t bo, uint32_t bi, int32_t disp, bool lk = false) {
    return 16u << 26 | bo << 21 | bi << 16 | (uint32_t(disp) & 0xFFFC) | lk;
}
uint32_t xl19(uint32_t bo, uint32_t bi, uint32_t xo, bool lk) { return 19u << 26 | bo << 21 | bi << 16 | xo << 1 | lk; }

struct Cpu {
    GuestMemory mem;
    PPUContext c;
    Cpu() {
        mem.reserve();
        mem.commit(kCode, 0x1000, Protection::ReadWrite);
        mem.commit(kData, 0x1000, Protection::ReadWrite);
        c.cia = kCode;
    }
    PpuStep run(uint32_t word) {
        mem.write<uint32_t>(c.cia, word);
        return ppu_step(c, mem);
    }
    // guest byte i of a vector register
    uint8_t vbyte(int r, int i) const { return c.vr[r].b[15 - i]; }
};

}  // namespace

TEST(interp_branches_ctr_lr) {
    Cpu p;
    // bdnz +8 with CTR = 2: taken, CTR = 1
    p.c.ctr = 2;
    CHECK(p.run(bc(16, 0, 8)) == PpuStep::Ok);
    CHECK_EQ(p.c.ctr, uint64_t(1));
    CHECK_EQ(p.c.cia, kCode + 8);
    // bdnz with CTR = 1: CTR becomes 0, not taken
    p.c.cia = kCode;
    p.run(bc(16, 0, 8));
    CHECK_EQ(p.c.ctr, uint64_t(0));
    CHECK_EQ(p.c.cia, kCode + 4);
    // bt eq (cr0) with link: LR set even though the branch is not taken
    p.c.cia = kCode;
    p.c.cr = 0;
    p.run(bc(12, 2, 0x40, true));
    CHECK_EQ(p.c.cia, kCode + 4);
    CHECK_EQ(p.c.lr, uint64_t(kCode + 4));
    p.c.cia = kCode;
    p.c.set_crbit(2, true);
    p.run(bc(12, 2, 0x40));
    CHECK_EQ(p.c.cia, kCode + 0x40);
    // blr / bctrl
    p.c.cia = kCode;
    p.c.lr = 0x10203;  // low bits ignored
    p.run(xl19(20, 0, 16, false));
    CHECK_EQ(p.c.cia, 0x10200u);
    p.c.cia = kCode;
    p.c.ctr = 0x10800;
    p.run(xl19(20, 0, 528, true));
    CHECK_EQ(p.c.cia, 0x10800u);
    CHECK_EQ(p.c.lr, uint64_t(kCode + 4));
    // absolute b: target is the sign-extended LI
    p.c.cia = kCode;
    p.run(18u << 26 | 0x00000100 | 2);  // ba 0x100
    CHECK_EQ(p.c.cia, 0x100u);
}

TEST(interp_reservation) {
    Cpu p;
    p.mem.write<uint32_t>(kData, 7);
    p.c.gpr[4] = kData;
    p.run(x31(3, 0, 4, 20));  // lwarx r3,0,r4
    CHECK_EQ(p.c.gpr[3], uint64_t(7));
    p.c.gpr[5] = 9;
    p.run(x31(5, 0, 4, 150, 1));  // stwcx. r5,0,r4 -> success
    CHECK_EQ(p.c.crf(0) & 2, 2u);
    CHECK_EQ(p.mem.read<uint32_t>(kData), 9u);
    p.run(x31(5, 0, 4, 150, 1));  // no reservation left -> fails
    CHECK_EQ(p.c.crf(0) & 2, 0u);
    // reservation broken by another writer changing the word
    p.run(x31(3, 0, 4, 20));
    p.mem.write<uint32_t>(kData, 1234);
    p.c.gpr[5] = 1;
    p.run(x31(5, 0, 4, 150, 1));
    CHECK_EQ(p.c.crf(0) & 2, 0u);
    CHECK_EQ(p.mem.read<uint32_t>(kData), 1234u);
}

TEST(interp_traps_system) {
    Cpu p;
    p.c.gpr[3] = 5;
    CHECK(p.run(x31(4, 3, 3, 4)) == PpuStep::Trap);  // tw 4 (eq),r3,r3
    CHECK_EQ(p.c.cia, kCode);                         // stays on the trap
    p.c.gpr[4] = 6;
    CHECK(p.run(x31(4, 3, 4, 4)) == PpuStep::Ok);     // 5 != 6: no trap
    p.c.cia = kCode;
    CHECK(p.run(0x44000002) == PpuStep::Syscall);     // sc
    CHECK_EQ(p.c.cia, kCode + 4);
    CHECK(p.run(0x00000200) == PpuStep::Halt);        // attn
    CHECK(p.run(0x00000000) == PpuStep::Invalid);
}

TEST(interp_lmw_stmw_spr) {
    Cpu p;
    for (uint32_t i = 0; i < 4; ++i) p.mem.write<uint32_t>(kData + 4 * i, 0x100 + i);
    p.c.gpr[3] = kData;
    p.run(46u << 26 | 28u << 21 | 3u << 16);  // lmw r28,0(r3)
    CHECK_EQ(p.c.gpr[28], uint64_t(0x100));
    CHECK_EQ(p.c.gpr[31], uint64_t(0x103));
    p.c.gpr[30] = 0xFFFFFFFF12345678ull;  // stmw stores the low words
    p.run(47u << 26 | 30u << 21 | 3u << 16 | 0x40);  // stmw r30,64(r3)
    CHECK_EQ(p.mem.read<uint32_t>(kData + 0x40), 0x12345678u);
    CHECK_EQ(p.mem.read<uint32_t>(kData + 0x44), 0x103u);
    p.c.gpr[7] = 0xABCD;
    p.run(x31(7, 8, 0, 467));  // mtspr 8 (LR),r7
    CHECK_EQ(p.c.lr, uint64_t(0xABCD));
    p.run(x31(9, 8, 0, 339));  // mfspr r9,8
    CHECK_EQ(p.c.gpr[9], uint64_t(0xABCD));
}

TEST(interp_cell_lvlx_lvrx_stvlx_stvrx) {
    Cpu p;
    for (uint32_t i = 0; i < 32; ++i) p.mem.write<uint8_t>(kData + i, uint8_t(i));
    p.c.gpr[4] = kData + 5;
    p.c.gpr[0] = 0;
    // lvlx v1,0,r4: bytes from EA to the end of the block, left-justified, zero fill
    p.run(x31(1, 0, 4, 519));
    for (int i = 0; i < 11; ++i) CHECK_EQ(p.vbyte(1, i), uint8_t(5 + i));
    for (int i = 11; i < 16; ++i) CHECK_EQ(p.vbyte(1, i), uint8_t(0));
    // lvrx v2,0,r4: bytes from the block start to EA-1, right-justified
    p.run(x31(2, 0, 4, 551));
    for (int i = 0; i < 11; ++i) CHECK_EQ(p.vbyte(2, i), uint8_t(0));
    for (int i = 11; i < 16; ++i) CHECK_EQ(p.vbyte(2, i), uint8_t(i - 11));
    // lvrx of an aligned address loads nothing
    p.c.gpr[5] = kData + 16;
    p.run(x31(3, 0, 5, 551));
    for (int i = 0; i < 16; ++i) CHECK_EQ(p.vbyte(3, i), uint8_t(0));
    // stvlx / stvrx write the matching halves back
    for (int i = 0; i < 16; ++i) p.c.vr[4].b[15 - i] = uint8_t(0xA0 + i);
    p.run(x31(4, 0, 4, 647));  // stvlx v4,0,r4
    for (int i = 0; i < 11; ++i) CHECK_EQ(p.mem.read<uint8_t>(kData + 5 + i), uint8_t(0xA0 + i));
    CHECK_EQ(p.mem.read<uint8_t>(kData + 4), uint8_t(4));  // untouched before EA
    p.run(x31(4, 0, 4, 679));  // stvrx v4,0,r4
    for (int i = 0; i < 5; ++i) CHECK_EQ(p.mem.read<uint8_t>(kData + i), uint8_t(0xAB + i));
    CHECK_EQ(p.mem.read<uint8_t>(kData + 16), uint8_t(16));  // next block untouched
}

TEST(interp_spot_checks) {
    Cpu p;
    // addc with carry out, then adde consumes it
    p.c.gpr[3] = ~0ull;
    p.c.gpr[4] = 1;
    p.run(x31(5, 3, 4, 10));  // addc r5,r3,r4
    CHECK_EQ(p.c.gpr[5], uint64_t(0));
    CHECK(p.c.xer_ca);
    p.run(x31(6, 0, 0, 138));  // adde r6,r0,r0 = 0 + 0 + CA
    CHECK_EQ(p.c.gpr[6], uint64_t(1));
    // rlwinm r7,r8,8,24,31 (extract the high byte of the low word)
    p.c.gpr[8] = 0x12345678;
    p.run(21u << 26 | 8u << 21 | 7u << 16 | 8u << 11 | 24u << 6 | 31u << 1);
    CHECK_EQ(p.c.gpr[7], uint64_t(0x12));
    // lfs / stfs keep a single SNaN intact (no quieting, no rounding)
    p.mem.write<uint32_t>(kData, 0x7F800001);
    p.c.gpr[9] = kData;
    p.run(48u << 26 | 1u << 21 | 9u << 16);  // lfs f1,0(r9)
    CHECK_EQ(p.c.fpr[1], 0x7FF0000020000000ull);
    p.run(52u << 26 | 1u << 21 | 9u << 16 | 8);  // stfs f1,8(r9)
    CHECK_EQ(p.mem.read<uint32_t>(kData + 8), 0x7F800001u);
    // dcbz zeroes the whole 128-byte Cell cache line
    for (uint32_t i = 0; i < 256; i += 4) p.mem.write<uint32_t>(kData + i, 0xFFFFFFFF);
    p.c.gpr[10] = kData + 0x90;
    p.run(x31(0, 0, 10, 1014));  // dcbz 0,r10
    CHECK_EQ(p.mem.read<uint32_t>(kData + 0x80), 0u);
    CHECK_EQ(p.mem.read<uint32_t>(kData + 0xFC), 0u);
    CHECK_EQ(p.mem.read<uint32_t>(kData + 0x7C), 0xFFFFFFFFu);
}
