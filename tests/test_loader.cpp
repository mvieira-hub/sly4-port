// Loader tests. The EBOOT tests need the user's decrypted executable: they read the
// path from SLY4_EBOOT (default PS3_GAME/USRDIR/EBOOT.elf from the current folder)
// and are skipped when it is absent, as in CI.
#include <cstdlib>
#include <filesystem>

#include "check.h"

#include "loader/elf_loader.h"
#include "loader/nid.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace sly4;

namespace {

std::string eboot_path() {
    const char* env = std::getenv("SLY4_EBOOT");
    return env ? env : "PS3_GAME/USRDIR/EBOOT.elf";
}

std::string hex_digest(const std::array<uint8_t, 20>& d) {
    std::string s;
    char buf[3];
    for (uint8_t b : d) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

}  // namespace

TEST(sha1_known_vectors) {
    CHECK_EQ(hex_digest(sha1("abc", 3)), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hex_digest(sha1("", 0)), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    // FIPS 180 vector of 56 bytes: the padding spills into a second 64-byte block.
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(hex_digest(sha1(two_blocks.data(), two_blocks.size())),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST(nid_matches_eboot_imports) {
    // Pairs taken from the EBOOT import table (work/eboot/imports.csv).
    CHECK_EQ(nid_of("cellGcmGetTiledPitchSize"), 0x055BD74Du);
    CHECK_EQ(nid_of("_cellGcmInitBody"), 0x15BAE46Bu);
    NidNames names;
    names.add("cellGcmGetTiledPitchSize");
    CHECK(names.find(0x055BD74D) && *names.find(0x055BD74D) == "cellGcmGetTiledPitchSize");
    CHECK(names.find(0x12345678) == nullptr);
}

TEST(elf_rejects_non_elf) {
    GuestMemory mem;
    CHECK(mem.reserve());
    LoadedImage img;
    std::string error;
    CHECK(!load_elf("arquivo-que-nao-existe.elf", mem, img, error));
    CHECK(!error.empty());
}

TEST(eboot_loads_like_eboot_scan) {
    const std::string path = eboot_path();
    if (!std::filesystem::exists(path)) SKIP("EBOOT.elf não encontrado; defina SLY4_EBOOT");
    GuestMemory mem;
    CHECK(mem.reserve());
    LoadedImage img;
    std::string error;
    CHECK(load_elf(path, mem, img, error));
    if (!error.empty()) std::printf("    erro: %s\n", error.c_str());

    // Values established by tools/eboot_scan.py and the program header dump.
    CHECK_EQ(img.entry_descriptor, 0x00F3B2D8u);
    CHECK_EQ(img.toc, 0x00F50800u);
    CHECK(img.entry_function >= 0x10000 && img.entry_function < 0xF05168);
    CHECK_EQ(img.segments.size(), size_t(2));
    CHECK_EQ(img.image_end, 0x011F89B8u);
    CHECK_EQ(img.tls.image, 0x00F55680u);
    CHECK_EQ(img.tls.mem_size, 0x160u);
    CHECK(img.param.present);
    CHECK_EQ(img.param.sdk_version, 0x00410001u);
    CHECK_EQ(img.param.primary_prio, 1001);
    CHECK_EQ(img.param.primary_stack_size, 0x10000u);
    CHECK_EQ(img.libstub_start, 0x00ABFB10u);
    CHECK_EQ(img.libstub_end, 0x00ABFE80u);
    CHECK_EQ(img.imports.size(), size_t(20));
    CHECK_EQ(img.imported_function_count(), size_t(251));
    if (!img.imports.empty() && !img.imports[0].functions.empty()) {
        CHECK_EQ(img.imports[0].name, std::string("cellGcmSys"));
        CHECK_EQ(img.imports[0].functions[0].nid, 0x055BD74Du);
        CHECK_EQ(img.imports[0].functions[0].slot, 0x00F2D30Cu);
    }

    // .bss is zero, the code segment is read-only on the host, data is writable.
    CHECK_EQ(mem.read<uint32_t>(0x011F0000), 0u);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(mem.host(0x10000), &mbi, sizeof(mbi));
    CHECK_EQ(mbi.Protect, DWORD(PAGE_READONLY));
    VirtualQuery(mem.host(0xF10000), &mbi, sizeof(mbi));
    CHECK_EQ(mbi.Protect, DWORD(PAGE_READWRITE));
}
