#include "check.h"

#include "cpu/ppu_rt.h"
#include "kernel/guest_alloc.h"
#include "kernel/hle.h"
#include "loader/elf_loader.h"
#include "loader/nid.h"

using namespace sly4;

TEST(guest_alloc_first_fit_and_merge) {
    GuestMemory mem;
    CHECK(mem.reserve());
    GuestAllocator a(mem, 0x20000000, 0x100000, "teste");
    const GuestAddr x = a.alloc(0x100, 0x100);
    const GuestAddr y = a.alloc(0x1000, 0x1000);
    const GuestAddr z = a.alloc(0x10);
    CHECK_EQ(x, 0x20000000u);
    CHECK_EQ(y, 0x20001000u);           // aligned past x
    CHECK_EQ(z, 0x20000100u);           // first fit: the gap after x
    CHECK(mem.is_committed(y, 0x1000));  // committed and zeroed
    CHECK_EQ(mem.read<uint32_t>(y), 0u);
    CHECK_EQ(a.size_of(y), 0x1000u);
    CHECK_EQ(a.used(), 0x1110u);
    CHECK(a.free(x));
    CHECK(!a.free(x));  // double free is refused
    CHECK(a.free(z));
    CHECK_EQ(a.alloc(0x1000, 0x10), 0x20000000u);  // x, z and the gap merged back
    CHECK_EQ(a.alloc(0x100000), 0u);               // does not fit any more
    const GuestAddr r = a.alloc(0x2000, 0x1000, false);
    CHECK(r != 0);
    CHECK(!mem.is_committed(r, 1));  // reserved only
}

namespace {
void answer_42(PPUContext& c, GuestMemory&) { c.gpr[3] = 42; }
HleTable* g_table = nullptr;
void on_hle(PPUContext& c, GuestMemory& m, uint32_t target) { g_table->call(c, m, target); }
}  // namespace

// The import stub of a PS3 game (as in the EBOOT's .lib.stub) reaches the native
// function through the descriptor written into its slot, and returns to LR.
TEST(hle_import_stub_reaches_native_function) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(mem.commit(0x10000, 0x10000, Protection::ReadWrite));
    GuestAllocator area(mem, 0x30000, 0x10000, "hle");

    LoadedImage img;
    ImportedModule mod;
    mod.name = "testmod";
    mod.functions = {{nid_of("answer"), 0x18000}, {nid_of("missing"), 0x18004}};
    img.imports = {mod};

    HleTable table;
    table.add({"testmod", {{"answer", answer_42}}});
    NidNames names;
    names.add("missing");
    CHECK(table.bind(mem, area, img, names));
    g_table = &table;
    rt::Hooks& h = rt::hooks();
    h.hle = on_hle;
    h.hle_base = table.thunk_base();
    h.hle_size = table.thunk_bytes();

    // Stub for slot S: lis r12,S@ha; lwz r12,S@l(r12); std r2,40(r1); lwz r0,0(r12);
    // lwz r2,4(r12); mtctr r0; bctr
    auto stub = [&](GuestAddr at, uint32_t slot) {
        const uint32_t words[] = {0x3D800000 | ((slot + 0x8000) >> 16), 0x818C0000 | (slot & 0xFFFF), 0xF8410028,
                                  0x800C0000, 0x804C0004, 0x7C0903A6, 0x4E800420};
        for (uint32_t i = 0; i < 7; ++i) mem.write<uint32_t>(at + 4 * i, words[i]);
    };
    stub(0x10000, 0x18000);
    stub(0x10100, 0x18004);

    PPUContext c;
    c.gpr[1] = 0x1F000;
    c.lr = 0x100;
    c.gpr[3] = 7;
    rt::interpret_until_return(c, mem, 0x10000);
    CHECK_EQ(c.gpr[3], 42u);
    c.gpr[3] = 7;
    rt::interpret_until_return(c, mem, 0x10100);
    CHECK_EQ(c.gpr[3], 0u);  // not implemented: CELL_OK
    auto st = table.stats();
    CHECK_EQ(st.size(), size_t(2));
    CHECK(st[0].implemented && st[0].calls == 1);
    CHECK(!st[1].implemented && st[1].name == "missing");

    h.hle = nullptr;
    h.hle_base = h.hle_size = 0;
}
