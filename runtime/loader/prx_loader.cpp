// PRX modules of the firmware (libsre.prx...), decrypted beforehand (rpcs3 --decrypt).
// Format as RPCS3's ppu_load_prx reads it (Emu/Cell/PPUModule.cpp).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/endian.h"
#include "loader/elf_loader.h"

namespace sly4 {

namespace {

constexpr uint16_t ET_SCE_PPURELEXEC = 0xFFA4;
constexpr uint16_t EM_PPC64 = 21;
constexpr uint32_t PT_LOAD = 1, PT_SCE_PPURELA = 0x700000A4;
constexpr uint32_t PF_X = 1, PF_W = 2;

struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, file_size, mem_size;
};

// Special exports (attribute 0x8000): module management functions.
constexpr uint32_t kNidModuleStart = 0xBC9A0086, kNidModuleStop = 0xAB779874;

}  // namespace

bool load_prx(const std::string& path, GuestMemory& mem, GuestAddr base, LoadedImage& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "não foi possível abrir " + path;
        return false;
    }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 64 || d[0] != 0x7F || d[1] != 'E' || d[4] != 2 || d[5] != 2) {
        error = "não é um ELF64 big-endian";
        return false;
    }
    if (load_be<uint16_t>(&d[16]) != ET_SCE_PPURELEXEC || load_be<uint16_t>(&d[18]) != EM_PPC64) {
        error = "não é um módulo PRX do PPU (tipo 0xFFA4)";
        return false;
    }
    const uint64_t phoff = load_be<uint64_t>(&d[32]);
    const uint16_t phentsize = load_be<uint16_t>(&d[54]), phnum = load_be<uint16_t>(&d[56]);
    std::vector<Phdr> ph;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t o = phoff + uint64_t(i) * phentsize;
        if (o + 56 > d.size()) {
            error = "cabeçalho de programa fora do arquivo";
            return false;
        }
        const uint8_t* p = &d[o];
        ph.push_back({load_be<uint32_t>(p), load_be<uint32_t>(p + 4), load_be<uint64_t>(p + 8), load_be<uint64_t>(p + 16),
                      load_be<uint64_t>(p + 24), load_be<uint64_t>(p + 32), load_be<uint64_t>(p + 40)});
    }

    out = {};
    std::vector<GuestAddr> seg_addr;  // per PT_LOAD, in order (relocations index them)
    for (const Phdr& p : ph) {
        if (p.type != PT_LOAD) continue;
        const GuestAddr at = base + GuestAddr(p.vaddr);
        if (p.offset + p.file_size > d.size() || !mem.commit(at, uint32_t(std::max(p.mem_size, uint64_t(1))), Protection::ReadWrite)) {
            error = "segmento fora do arquivo ou da memória";
            return false;
        }
        mem.copy_to(at, &d[p.offset], p.file_size);
        seg_addr.push_back(at);
        out.segments.push_back({at, uint32_t(p.file_size), uint32_t(p.mem_size), p.flags});
        ElfSection s;
        s.addr = at;
        s.size = uint32_t(p.flags & PF_X ? p.file_size : p.mem_size);
        s.type = 1;
        s.flags = 2 | (p.flags & PF_X ? 4 : 0) | (p.flags & PF_W ? 1 : 0);
        out.sections.push_back(s);
        out.image_end = std::max<GuestAddr>(out.image_end, at + GuestAddr(p.mem_size));
    }
    if (seg_addr.empty()) {
        error = "módulo sem segmentos";
        return false;
    }

    // SCE relocations: 24-byte entries {offset u64, u16, index_value u8, index_addr u8,
    // type u32, ptr u64}; the target is segment[index_addr] + offset, the value
    // segment[index_value] + ptr (index_value 0xFF: ptr is absolute).
    std::vector<std::pair<GuestAddr, uint64_t>> addr32;  // (where, value) of ADDR32 relocations
    for (const Phdr& p : ph) {
        if (p.type != PT_SCE_PPURELA) continue;
        for (uint64_t o = p.offset; o + 24 <= p.offset + p.file_size && o + 24 <= d.size(); o += 24) {
            const uint8_t* r = &d[o];
            const uint64_t offset = load_be<uint64_t>(r);
            const uint8_t index_value = r[10], index_addr = r[11];
            const uint32_t type = load_be<uint32_t>(r + 12);
            const uint64_t ptr = load_be<uint64_t>(r + 16);
            if (index_addr >= seg_addr.size() || (index_value != 0xFF && index_value >= seg_addr.size())) {
                error = "relocação com segmento inválido";
                return false;
            }
            const GuestAddr raddr = seg_addr[index_addr] + GuestAddr(offset);
            const uint64_t rdata = (index_value == 0xFF ? 0 : seg_addr[index_value]) + ptr;
            switch (type) {
                case 1:  // ADDR32
                    mem.write<uint32_t>(raddr, uint32_t(rdata));
                    addr32.push_back({raddr, rdata});
                    break;
                case 4: mem.write<uint16_t>(raddr, uint16_t(rdata)); break;           // ADDR16_LO
                case 5: mem.write<uint16_t>(raddr, uint16_t(rdata >> 16)); break;     // ADDR16_HI
                case 6: mem.write<uint16_t>(raddr, uint16_t((rdata >> 16) + ((rdata & 0x8000) ? 1 : 0))); break;  // HA
                case 10: {  // REL24: bits 6-29 of a branch
                    const uint32_t w = mem.read<uint32_t>(raddr);
                    mem.write<uint32_t>(raddr, (w & 0xFC000003) | ((uint32_t(rdata - raddr) >> 2) & 0xFFFFFF) << 2);
                    break;
                }
                case 11: {  // REL14: bits 16-29 of a conditional branch
                    const uint32_t w = mem.read<uint32_t>(raddr);
                    mem.write<uint32_t>(raddr, (w & 0xFFFF0003) | ((uint32_t(rdata - raddr) >> 2) & 0x3FFF) << 2);
                    break;
                }
                case 38: mem.write<uint64_t>(raddr, rdata); break;           // ADDR64
                case 44: mem.write<uint64_t>(raddr, rdata - raddr); break;   // REL64
                case 57: {  // ADDR16_LO_DS: low 16 bits, the 2 lowest kept
                    const uint16_t h = mem.read<uint16_t>(raddr);
                    mem.write<uint16_t>(raddr, uint16_t((h & 3) | (uint16_t(rdata) & ~3u)));
                    break;
                }
                default: {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "tipo de relocação %u sem suporte", type);
                    error = buf;
                    return false;
                }
            }
        }
    }

    // Library info: first segment + (p_paddr - p_offset) of the first program header.
    // {u16 attributes, u8 version[2], char name[28], u32 toc, exports start/end, imports start/end}
    if (!ph[0].paddr) {
        error = "módulo sem informação de biblioteca";
        return false;
    }
    const GuestAddr info = seg_addr[0] + GuestAddr(ph[0].paddr - ph[0].offset);
    out.module_name = mem.read_cstring(info + 4, 28);
    out.toc = mem.read<uint32_t>(info + 32);
    const GuestAddr exp_start = mem.read<uint32_t>(info + 36), exp_end = mem.read<uint32_t>(info + 40);
    const GuestAddr imp_start = mem.read<uint32_t>(info + 44), imp_end = mem.read<uint32_t>(info + 48);

    // Exports: records of (u8 size) bytes; +4 attributes, +6 functions, +8 variables,
    // +16 name, +20 NIDs, +24 descriptor addresses (functions first).
    for (GuestAddr a = exp_start; a + 28 <= exp_end;) {
        const uint8_t size = mem.read<uint8_t>(a);
        if (size < 28) {
            error = "registro de exportação inválido";
            return false;
        }
        const uint16_t attr = mem.read<uint16_t>(a + 4), nfunc = mem.read<uint16_t>(a + 6);
        const GuestAddr name = mem.read<uint32_t>(a + 16), nids = mem.read<uint32_t>(a + 20),
                        addrs = mem.read<uint32_t>(a + 24);
        if (attr & 0x8000) {  // module management: module_start, module_stop...
            for (uint16_t i = 0; i < nfunc; ++i) {
                const uint32_t nid = mem.read<uint32_t>(nids + 4u * i);
                const GuestAddr opd = mem.read<uint32_t>(addrs + 4u * i);
                if (nid == kNidModuleStart) out.module_start = opd;
                if (nid == kNidModuleStop) out.module_stop = opd;
            }
        } else {
            ExportedLibrary lib;
            lib.name = name ? mem.read_cstring(name, 128) : "";
            for (uint16_t i = 0; i < nfunc; ++i)
                lib.functions.push_back({mem.read<uint32_t>(nids + 4u * i), mem.read<uint32_t>(addrs + 4u * i)});
            out.exports.push_back(std::move(lib));
        }
        a += size;
    }
    if (!read_import_table(mem, imp_start, imp_end, out.imports, error)) return false;

    // Function entries: every descriptor {code address, module TOC} built by an ADDR32
    // relocation outside the code, plus the exports and module_start/stop.
    auto in_code = [&](uint64_t v) {
        for (const ElfSection& s : out.sections)
            if ((s.flags & 4) && v >= s.addr && v - s.addr < s.size) return true;
        return false;
    };
    for (const auto& [where, value] : addr32)
        if (!in_code(where) && in_code(value) && mem.read<uint32_t>(where + 4) == out.toc)
            out.function_entries.push_back(GuestAddr(value));
    for (const ExportedLibrary& lib : out.exports)
        for (const auto& [nid, opd] : lib.functions) out.function_entries.push_back(mem.read<uint32_t>(opd));
    for (GuestAddr opd : {out.module_start, out.module_stop})
        if (opd) out.function_entries.push_back(mem.read<uint32_t>(opd));
    std::sort(out.function_entries.begin(), out.function_entries.end());
    out.function_entries.erase(std::unique(out.function_entries.begin(), out.function_entries.end()),
                               out.function_entries.end());
    out.entry_descriptor = out.module_start;
    out.entry_function = out.module_start ? mem.read<uint32_t>(out.module_start) : 0;
    return true;
}

}  // namespace sly4
