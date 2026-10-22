// Recompiled SPU code (docs/SPU.md, 4e).
//
// SPU programs are not fixed: the SPURS kernel, its tasks and jobs are copied into local
// store by DMA at run time, often at the same addresses. So compiled code is kept per
// block, and a block runs only when the local store holds exactly the code it was
// generated from. A block is the straight run of words from an entry address up to and
// including the first one after which execution never goes on (unconditional branch,
// stop, a word that is not an instruction); conditional branches inside it leave it
// only when taken.
//
// The blocks come from the SPU cache: while the interpreter runs, the code of every
// block it enters can be recorded, and "sly4_recomp spu-emit" turns the cache (and SPU
// ELFs given to it) into C++.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cpu/spu_semantics.h"

namespace sly4 {

// Runs the block from c.pc; adds the instructions it ran to `executed` and returns as
// the interpreter would after its last one (Branch with c.pc set when it leaves).
using SpuBlockFn = SpuStep (*)(SPUContext& c, uint64_t& executed);

// For generated blocks: an instruction that left the straight line after `before`
// instructions of the block; counts as spu_run does (not the one that waits).
inline SpuStep spu_block_leave(SpuStep st, uint64_t& executed, uint64_t before) {
    executed += before + (st != SpuStep::Wait && st != SpuStep::Invalid ? 1 : 0);
    return st;
}

struct SpuBlockEntry {
    uint32_t pc;
    uint32_t size;        // bytes of code
    const uint8_t* code;  // as it sits in local store (big-endian words)
    SpuBlockFn fn;
};

constexpr uint32_t kSpuMaxBlockWords = 1024;

// Size in bytes of the block that starts at `pc` in this local store.
uint32_t spu_block_size(const uint8_t* ls, uint32_t pc);

// Compiled blocks (generated/spu); registered before any SPU runs.
void spu_register_blocks(const SpuBlockEntry* table, size_t count);
size_t spu_block_count();
const SpuBlockEntry* spu_block_table(size_t& count);
// The compiled block for c.pc whose code matches the local store, or nullptr.
SpuBlockFn spu_find_block(const SPUContext& c);

// Same contract as spu_run, with compiled blocks where they exist (and the interpreter
// elsewhere). With the SPU cache on, interpreted blocks are recorded.
SpuStep spu_run_blocks(SPUContext& c, uint64_t max_steps, uint64_t& executed);

// ---------------------------------------------------------------- SPU cache
struct SpuCachedBlock {
    uint32_t pc = 0;
    std::vector<uint8_t> code;
};

void spu_cache_enable(bool on);
size_t spu_cache_size();
// Reads a cache file (missing file: empty, true).
bool spu_cache_read(const std::string& path, std::vector<SpuCachedBlock>& out);
// Writes what was recorded, together with what the file already had.
bool spu_cache_save(const std::string& path);

}  // namespace sly4
