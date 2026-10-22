// SPU programs: ELF32 big-endian, machine 0x17 (SPU), loaded into a 256 KiB local
// store. The game embeds them in the EBOOT (tools/spu_extract.py finds 10) and the
// SPURS/SPU-thread HLE loads them from guest memory, so the loader reads from a buffer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sly4 {

constexpr uint32_t kSpuLocalStoreSize = 0x40000;

struct SpuSegment {
    uint32_t offset = 0;  // in the ELF
    uint32_t vaddr = 0;   // in local store
    uint32_t file_size = 0, mem_size = 0;
    uint32_t flags = 0;   // PF_X = 1, PF_W = 2, PF_R = 4
    bool executable() const { return flags & 1; }
};

struct SpuImage {
    uint32_t entry = 0;
    uint32_t size = 0;  // bytes of the ELF (end of its last segment or header table)
    std::vector<SpuSegment> segments;
};

// Parses the ELF at `data` (`size` bytes available). False with `error` set if it is
// not an SPU executable or a segment does not fit in local store.
bool parse_spu_elf(const uint8_t* data, size_t size, SpuImage& out, std::string& error);

// Copies the loadable segments into `ls` (kSpuLocalStoreSize bytes) and zeroes .bss.
void load_spu_segments(const uint8_t* data, const SpuImage& img, uint8_t* ls);

}  // namespace sly4
