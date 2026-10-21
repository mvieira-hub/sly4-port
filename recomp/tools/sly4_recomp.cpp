// sly4_recomp.exe: static recompiler toolkit for the PS3 executable (phase 2).
//
//   sly4_recomp disasm --eboot <EBOOT.elf> [--start 0x10230] [--end 0xABD560] [--out file]
//       Decodes every word of the range and prints "addr:\tword\tinstruction" in the
//       syntax of objdump -M cell,raw, to be compared with binutils
//       (tools/ppc/compare_disasm.py). Default range: the EBOOT .text section.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "loader/elf_loader.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int usage() {
    std::fprintf(stderr,
                 "uso: sly4_recomp disasm --eboot <EBOOT.elf> [--start 0x10230] [--end 0xABD560] [--out arquivo]\n");
    return 2;
}

int cmd_disasm(int argc, char** argv) {
    std::string eboot, out;
    uint32_t start = 0x10230, end = 0xABD560;
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (std::strcmp(argv[i], "--eboot") == 0) eboot = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0) out = argv[++i];
        else if (std::strcmp(argv[i], "--start") == 0) start = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--end") == 0) end = std::strtoul(argv[++i], nullptr, 0);
        else return usage();
    }
    if (eboot.empty() || end <= start) return usage();

    sly4::GuestMemory mem;
    sly4::LoadedImage img;
    std::string error;
    if (!mem.reserve() || !sly4::load_elf(eboot, mem, img, error)) {
        SLY4_ERROR("falha ao carregar %s: %s", eboot.c_str(), error.c_str());
        return 3;
    }
    if (!mem.is_committed(start, end - start)) {
        SLY4_ERROR("faixa 0x%X-0x%X fora da imagem", start, end);
        return 3;
    }

    FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), "w");
    if (!f) {
        SLY4_ERROR("não foi possível criar %s", out.c_str());
        return 3;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    const auto t0 = std::chrono::steady_clock::now();
    size_t count = 0, invalid = 0;
    for (uint32_t a = start; a + 4 <= end; a += 4) {
        const uint32_t w = mem.read<uint32_t>(a);
        const sly4::ppu::Insn in = sly4::ppu::decode(w);
        invalid += in.op == sly4::ppu::Op::invalid;
        std::fprintf(f, "%x:\t%08x\t%s\n", a, w, sly4::ppu::disasm(in, a).c_str());
        ++count;
    }
    if (f != stdout) std::fclose(f);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%zu palavras decodificadas em %.2f s (%zu sem instrução válida)", count, s, invalid);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) return usage();
    if (std::strcmp(argv[1], "disasm") == 0) return cmd_disasm(argc, argv);
    return usage();
}
