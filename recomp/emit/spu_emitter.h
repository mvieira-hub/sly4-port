// SPU -> C++ (docs/SPU.md, 4e): each block of the SPU cache becomes a function that runs
// its instructions straight through the shared semantics (spusem::op<Op::X>), with the
// fields of each instruction known at compile time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpu/spu_blocks.h"

namespace sly4::recomp {

struct SpuEmitStats {
    size_t blocks = 0, instructions = 0, files = 0;
};

// Writes spu_NNNN.cpp, table.cpp (register_spu_blocks()) and sources.cmake to out_dir.
bool emit_spu_blocks(const std::vector<SpuCachedBlock>& blocks, const std::string& out_dir, SpuEmitStats& stats,
                     std::string& error);

// Blocks of an SPU program found statically: its entry, direct branch targets, return
// points of calls and the words after each unconditional branch.
void spu_static_blocks(const uint8_t* ls, const std::vector<std::pair<uint32_t, uint32_t>>& code_ranges,
                       uint32_t entry, std::vector<SpuCachedBlock>& out);

}  // namespace sly4::recomp
