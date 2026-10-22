#include "loader/spu_elf.h"

#include <algorithm>
#include <cstring>

#include "core/endian.h"

namespace sly4 {

bool parse_spu_elf(const uint8_t* d, size_t size, SpuImage& out, std::string& error) {
    out = {};
    if (size < 52 || std::memcmp(d, "\x7F" "ELF", 4) != 0 || d[4] != 1 || d[5] != 2) {
        error = "não é um ELF32 big-endian";
        return false;
    }
    if (load_be<uint16_t>(d + 18) != 0x17) {
        error = "não é um programa SPU (e_machine != 0x17)";
        return false;
    }
    out.entry = load_be<uint32_t>(d + 24);
    const uint32_t phoff = load_be<uint32_t>(d + 28), shoff = load_be<uint32_t>(d + 32);
    const uint32_t phentsize = load_be<uint16_t>(d + 42), phnum = load_be<uint16_t>(d + 44);
    const uint32_t shentsize = load_be<uint16_t>(d + 46), shnum = load_be<uint16_t>(d + 48);
    if (phentsize < 32 || uint64_t(phoff) + uint64_t(phnum) * phentsize > size) {
        error = "tabela de segmentos fora do arquivo";
        return false;
    }
    uint64_t end = std::max<uint64_t>(uint64_t(phoff) + phnum * phentsize, shoff ? uint64_t(shoff) + shnum * shentsize : 0);
    for (uint32_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = d + phoff + i * phentsize;
        if (load_be<uint32_t>(ph) != 1) continue;  // PT_LOAD
        SpuSegment s;
        s.offset = load_be<uint32_t>(ph + 4);
        s.vaddr = load_be<uint32_t>(ph + 8);
        s.file_size = load_be<uint32_t>(ph + 16);
        s.mem_size = load_be<uint32_t>(ph + 20);
        s.flags = load_be<uint32_t>(ph + 24);
        if (uint64_t(s.vaddr) + std::max(s.file_size, s.mem_size) > kSpuLocalStoreSize ||
            uint64_t(s.offset) + s.file_size > size) {
            error = "segmento fora do local store ou do arquivo";
            return false;
        }
        end = std::max<uint64_t>(end, uint64_t(s.offset) + s.file_size);
        out.segments.push_back(s);
    }
    out.size = uint32_t(end);
    return true;
}

void load_spu_segments(const uint8_t* data, const SpuImage& img, uint8_t* ls) {
    for (const SpuSegment& s : img.segments) {
        std::memcpy(ls + s.vaddr, data + s.offset, s.file_size);
        if (s.mem_size > s.file_size) std::memset(ls + s.vaddr + s.file_size, 0, s.mem_size - s.file_size);
    }
}

}  // namespace sly4
