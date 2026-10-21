// PPU decoder/disassembler: one real instruction word per form, with the text GNU
// objdump (-M cell,raw) prints for it at that address. The full-executable check is
// tools/ppc/compare_disasm.py; these keep the CI honest without the game.
#include "check.h"

#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"

using namespace sly4::ppu;

namespace {

struct Sample {
    uint32_t addr, word;
    const char* text;
};

// Taken from the EBOOT disassembly (objdump text, whitespace collapsed).
constexpr Sample kSamples[] = {
    {0x10230, 0x4e800020, "bclr 20,lt,0"},
    {0x10238, 0xf821ff11, "stdu r1,-240(r1)"},
    {0x1023c, 0x7c0802a6, "mfspr r0,8"},
    {0x10248, 0x607f0000, "ori r31,r3,0"},
    {0x10254, 0x3b400000, "addi r26,0,0"},
    {0x1025c, 0x2c030000, "cmpi cr0,0,r3,0"},
    {0x10290, 0x408101dc, "bc 4,gt,0x1046c"},
    {0x1029c, 0x7b7b0020, "rldicl r27,r27,0,32"},
    {0x102b0, 0x3c6000f6, "addis r3,0,246"},
    {0x102d8, 0x48722fdd, "bl 0x7332b4"},
    {0x10360, 0x5463103a, "rlwinm r3,r3,2,0,29"},
    {0x10470, 0x7c0803a6, "mtspr 8,r0"},
    {0x104b4, 0xbf800000, "stmw r28,0(0)"},
    {0x104c0, 0x00000000, ".long 0x0"},
    {0x105fc, 0xfc1d1800, "fcmpu cr0,f29,f3"},
    {0x10664, 0x4c423382, "cror eq,eq,4*cr1+eq"},
    {0x106c8, 0xec4218ba, "fmadds f2,f2,f2,f3"},
    {0x10780, 0x4e9e0421, "bcctrl 20,4*cr7+eq,0"},
    {0x113e4, 0xffffffff, "fnmadd. f31,f31,f31,f31"},
    {0x11448, 0x7c4030ce, "lvx v2,0,r6"},
    {0x1165c, 0x104418ae, "vmaddfp v2,v4,v2,v3"},
    {0x11708, 0x7c401c0e, "lvlx v2,0,r3"},
    {0x1186c, 0x1047112a, "vsel v2,v7,v2,v4"},
    {0x13e34, 0x3727c5ac, "addic. r25,r7,-14932"},
    {0x14c30, 0x78a6746c, "rldimi r6,r5,14,49"},
    {0x16678, 0x40490fdb, "bcla 2,4*cr2+gt,0xfd8"},
    {0x18db4, 0x40c90fdb, "bcla- 6,4*cr2+gt,0xfd8"},
    {0x18f94, 0x10882feb, "vperm v4,v8,v5,v31"},
    {0x191ac, 0x41a00000, "bt lt,0x191ac"},
    {0x196ac, 0x110b582c, "vsldoi v8,v11,v11,0"},
    {0x1a6b8, 0x7c7d0e70, "srawi r29,r3,1"},
    {0x1b49c, 0x41700000, "bdzt 4*cr4+lt,0x1b49c"},
    {0x1bfdc, 0x41c80000, "bc- 14,4*cr2+lt,0x1bfdc"},
    {0x29824, 0x42200000, "bdnz 0x29824"},
    {0x290c0, 0x40e00000, "bc+ 7,lt,0x290c0"},
    {0x67678, 0x4169999a, "bdzta 4*cr2+gt,0xffff9998"},
    {0x6e0a0, 0x7c6318f8, "not r3,r3"},
    {0xabfc0, 0x44000002, "sc 0"},
    {0xe0c20, 0x7cc82828, "lwarx r6,r8,r5,0"},
    {0xe0c2c, 0x7d28292d, "stwcx. r9,r8,r5"},
    {0xe5c84, 0x7ca62b78, "mr r6,r5"},
    {0x17b940, 0x10631d04, "vnot v3,v3"},
    {0x17b950, 0x10423c86, "vcmpequw. v2,v2,v7"},
    {0x5099f0, 0x7c00fa2c, "dcbt 0,r31,0"},
    {0x513f04, 0x7c88c50e, "stvlx v4,r8,r24"},
    {0x52f300, 0x7ce7fe76, "sradi r7,r7,63"},
    {0x52f624, 0x7c2004ac, "sync 1"},
    {0x531f64, 0x4b7fffff, "bla 0xff7ffffc"},
    {0x5cb078, 0x1048134a, "vcfsx v2,v2,8"},
    {0x5dcb50, 0x7d908120, "mtocrf 8,r12"},
    {0x5ed3c4, 0x7c180026, "mfocrf r0,128"},
    {0x5f1c38, 0x7c035051, "subf. r0,r3,r10"},
    {0x5f2990, 0x4e1c0000, "mcrf cr4,cr7"},
    {0x6062c8, 0x4ddd0020, "bclr- 14,4*cr7+gt,0"},
    {0x706830, 0xfdfe058e, "mtfsf 255,f0"},
    {0x714870, 0x00000200, "attn"},
    {0x714860, 0x00000a00, "attn"},
    {0x71c578, 0xe8e4fff9, "ldu r7,-8(r4)"},
    {0x7bb3d4, 0x100009ce, "vpkswss v0,v0,v1"},
    {0x7bb554, 0x1108034c, "vspltish v8,8"},
    {0x7bb7b8, 0xfd805834, "frsqrte f12,f11,0"},
    {0x88c5ac, 0x4f58c042, "crnot 4*cr6+eq,4*cr6+lt"},
    {0xab84b0, 0x106f030c, "vspltisb v3,15"},
    {0xab88c0, 0x13732b44, "vsrah v27,v19,v5"},
};

std::string collapse(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    return out;
}

}  // namespace

TEST(ppu_disasm_matches_objdump_samples) {
    for (const Sample& s : kSamples) {
        const std::string got = collapse(disasm(decode(s.word), s.addr));
        if (got != s.text) {
            ++::sly4::test::failures();
            std::printf("    FALHOU 0x%08x: esperado '%s', obtido '%s'\n", s.word, s.text, got.c_str());
        }
    }
}

TEST(ppu_decoder_fields) {
    const Insn in = decode(0xf821ff11);  // stdu r1,-240(r1)
    CHECK(in.op == Op::stdu);
    CHECK_EQ(in.rd(), 1u);
    CHECK_EQ(in.ra(), 1u);
    CHECK_EQ(in.ds(), -240);
    const Insn rl = decode(0x7b7b0020);  // rldicl r27,r27,0,32: mb is stored swapped
    CHECK(rl.op == Op::rldicl);
    CHECK_EQ(rl.mb64(), 32u);
    const Insn sr = decode(0x7ce7fe76);  // sradi r7,r7,63: sh[5] in bit 30
    CHECK(sr.op == Op::sradi);
    CHECK_EQ(sr.sh64(), 63u);
    const Insn b = decode(0x48722fdd);  // bl 0x7332b4 at 0x102d8
    CHECK(b.op == Op::b && b.lk() && !b.aa());
    CHECK_EQ(b.branch_target(0x102d8), 0x7332b4u);
    CHECK(decode(0x7c6c42e6).op == Op::mftb);  // Cell time base read (binutils rejects it with -M cell)
}

TEST(ppu_every_op_has_a_name) {
    for (uint16_t i = 0; i < static_cast<uint16_t>(Op::count); ++i) {
        const char* n = mnemonic(static_cast<Op>(i));
        CHECK(n != nullptr && n[0] != 0);
    }
}
