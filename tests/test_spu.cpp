#include "check.h"

#include <string>

#include "spu/spu_decoder.h"

using namespace sly4;

// Words from the game's SPU programs with spu-elf-objdump's text for them.
TEST(spu_disasm_matches_objdump_samples) {
    struct Case {
        uint32_t w, ip;
        const char* text;
    };
    const Case cases[] = {
        {0x1CEC0081, 0x3000, "ai\t$1,$1,-80"},
        {0x40800010, 0x3000, "il\t$16,0"},
        {0x34002804, 0x3000, "lqd\t$4,0($80)"},
        {0x24004080, 0x3000, "stqd\t$0,16($1)"},
        {0x42804009, 0x3000, "ila\t$9,65664\t# 10080"},
        {0xB142C58C, 0x3000, "shufb\t$10,$11,$11,$12"},
        {0x3E800087, 0x3000, "cbd\t$7,0($1)"},
        {0x35080100, 0x3000, "bid\t$2"},
        {0x35773663, 0x3000, "bisledpe\t$99,$108"},
        {0x21A00813, 0x3000, "wrch\t$ch16,$19"},
        {0x01A006D0, 0x3000, "rdch\t$80,$ch13"},
        {0x762B4A13, 0x3000, "cflts\t$19,$20,0"},
        {0x32000100, 0x3000, "br\t0x3008\t# 3008"},
        {0x00000000, 0x3000, "stop"},
        {0x40200000 | 127, 0x3000, "nop\t$127"},
        {0x01020304, 0x3000, ".long 0x1020304"},
    };
    for (const Case& c : cases) {
        const std::string got = spu::disasm(spu::decode(c.w), c.ip);
        if (got != c.text) std::printf("    %08x: [%s] esperado [%s]\n", c.w, got.c_str(), c.text);
        CHECK(got == c.text);
    }
}

// Every opcode in the table decodes back to its own instruction (no shadowing by a
// shorter-opcode form).
TEST(spu_every_op_decodes_to_itself) {
    for (uint16_t i = 1; i < uint16_t(spu::Op::count); ++i) {
        const spu::Op op = spu::Op(i);
        bool found = false;
        for (uint32_t v = 0; v < 2048 && !found; ++v) found = spu::decode(v << 21).op == op;
        if (!found) std::printf("    %s nunca é decodificada\n", spu::mnemonic(op));
        CHECK(found);
    }
}
