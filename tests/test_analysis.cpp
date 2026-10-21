// Control-flow analysis on a hand-assembled program: a switch through a jump table
// (the compiler's idiom), a call, a tail call and float constants left in .text.
#include "check.h"

#include "analysis/program.h"

using namespace sly4;
using namespace sly4::recomp;

namespace {

constexpr uint32_t kCode = 0x10000, kData = 0x20000, kToc = 0x20000;

uint32_t addi(uint32_t rt, uint32_t ra, int32_t si) { return 14u << 26 | rt << 21 | ra << 16 | (si & 0xFFFF); }
uint32_t b(uint32_t from, uint32_t to, bool link = false) { return 18u << 26 | ((to - from) & 0x03FFFFFC) | link; }
uint32_t bc(uint32_t bo, uint32_t bi, uint32_t from, uint32_t to) {
    return 16u << 26 | bo << 21 | bi << 16 | ((to - from) & 0xFFFC);
}
uint32_t cmpli_cr7(uint32_t ra, uint32_t ui) { return 10u << 26 | 7u << 23 | ra << 16 | ui; }
uint32_t lwz(uint32_t rt, uint32_t ra, int32_t d) { return 32u << 26 | rt << 21 | ra << 16 | (d & 0xFFFF); }
uint32_t x31(uint32_t d, uint32_t a, uint32_t bb, uint32_t xo) { return 31u << 26 | d << 21 | a << 16 | bb << 11 | xo << 1; }
uint32_t rldic(uint32_t ra, uint32_t rs, uint32_t sh, uint32_t mb) {
    return 30u << 26 | rs << 21 | ra << 16 | (sh & 31) << 11 | (mb & 31) << 6 | (mb >> 5) << 5 | 2u << 2 | (sh >> 5) << 1;
}
constexpr uint32_t kBlr = 0x4E800020, kBctr = 0x4E800420;
constexpr uint32_t kMtctrR0 = 31u << 26 | 0u << 21 | 9u << 16 | 467u << 1;  // mtspr 9,r0

struct Fixture {
    GuestMemory mem;
    LoadedImage img;

    Fixture() {
        mem.reserve();
        mem.commit(kCode, 0x1000, Protection::ReadWrite);
        mem.commit(kData, 0x1000, Protection::ReadWrite);
        const uint32_t A = kCode, B = kCode + 0x60, C = kCode + 0x80, table = A + 0x24;
        const uint32_t code[] = {
            /* 00 */ cmpli_cr7(3, 2),                  // 3 cases
            /* 04 */ bc(12, 29, A + 0x04, A + 0x40),   // bgt cr7 -> default
            /* 08 */ lwz(11, 2, 0),                    // table base from the TOC
            /* 0c */ rldic(9, 3, 2, 30),               // index * 4
            /* 10 */ x31(0, 9, 11, 23),                // lwzx r0,r9,r11
            /* 14 */ x31(0, 0, 0, 986),                // extsw r0,r0
            /* 18 */ x31(0, 0, 11, 266),               // add r0,r0,r11
            /* 1c */ kMtctrR0,
            /* 20 */ kBctr,
            /* 24 */ 0x30 - 0x24, 0x38 - 0x24, 0x40 - 0x24,  // table: offsets from its base
            /* 30 */ b(A + 0x30, B, true),             // case 0: call B
            /* 34 */ b(A + 0x34, A + 0x40),
            /* 38 */ addi(3, 0, 1),                    // case 1
            /* 3c */ b(A + 0x3c, A + 0x40),
            /* 40 */ kBlr,                             // case 2 / default
            /* 44 */ 0x3F800000, 0x42C80000,           // 1.0f, 100.0f: data that decodes
        };
        for (size_t i = 0; i < sizeof(code) / 4; ++i) mem.write<uint32_t>(A + 4 * uint32_t(i), code[i]);
        mem.write<uint32_t>(B, b(B, C));  // B: tail call to C
        mem.write<uint32_t>(C, kBlr);
        mem.write<uint32_t>(kToc, table);
        // .opd: A and C are address-taken; B is only reached by the call.
        mem.write<uint32_t>(kData + 0x10, A);
        mem.write<uint32_t>(kData + 0x14, kToc);
        mem.write<uint32_t>(kData + 0x18, C);
        mem.write<uint32_t>(kData + 0x1C, kToc);
        img.toc = kToc;
        img.entry_function = A;
        img.opd_start = kData + 0x10;
        img.opd_end = kData + 0x20;
        img.sections.push_back({kCode, 0x100, 1, 6});
        img.sections.push_back({kData, 0x100, 1, 3});
    }
};

}  // namespace

TEST(analysis_switch_call_tailcall_and_data) {
    Fixture fx;
    const Program p = analyze(fx.mem, fx.img);
    CHECK_EQ(p.functions.size(), size_t(3));
    CHECK_EQ(p.stats.invalid_reached, 0u);

    const auto a = p.functions.find(kCode);
    CHECK(a != p.functions.end());
    if (a != p.functions.end()) {
        const Function& f = a->second;
        CHECK(f.from_opd);
        CHECK_EQ(f.tables.size(), size_t(1));
        if (!f.tables.empty()) {
            const JumpTable& jt = f.tables[0];
            CHECK(jt.bounded);
            CHECK(jt.toc_base);
            CHECK_EQ(jt.base, kCode + 0x24);
            CHECK_EQ(jt.count, 3u);
            CHECK_EQ(jt.targets.size(), size_t(3));
            if (jt.targets.size() == 3) {
                CHECK_EQ(jt.targets[0], kCode + 0x30);
                CHECK_EQ(jt.targets[1], kCode + 0x38);
                CHECK_EQ(jt.targets[2], kCode + 0x40);
            }
        }
        CHECK_EQ(f.calls.size(), size_t(1));
        CHECK_EQ(f.hi, kCode + 0x44);  // the floats after the blr are not part of it
    }
    const auto bfn = p.functions.find(kCode + 0x60);
    CHECK(bfn != p.functions.end() && !bfn->second.from_opd);
    if (bfn != p.functions.end()) {
        CHECK_EQ(bfn->second.tail_calls.size(), size_t(1));
        CHECK(!bfn->second.blocks.empty() && bfn->second.blocks[0].term == Term::TailCall);
    }
    CHECK_EQ(p.class_of(kCode + 0x20), uint8_t(1));  // bctr: code
    CHECK_EQ(p.class_of(kCode + 0x28), uint8_t(2));  // table entry
    CHECK_EQ(p.class_of(kCode + 0x44), uint8_t(0));  // 1.0f: not reached
    CHECK_EQ(p.class_of(kCode + 0x48), uint8_t(0));
}
