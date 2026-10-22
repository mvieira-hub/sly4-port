// SPU reference interpreter: fetch from local store, decode, run the shared semantics.
#pragma once

#include <cstdint>

#include "cpu/spu_semantics.h"

namespace sly4 {

// Executes one decoded instruction at c.pc (advancing it unless the instruction
// branched or has to wait on a channel).
SpuStep spu_execute(SPUContext& c, const spu::Insn& in);

// Fetches and executes the instruction at c.pc.
SpuStep spu_step(SPUContext& c);

// Runs until something other than Ok/Branch happens or `max_steps` instructions ran.
// Returns that step (Ok when the budget ran out) and adds the count to `executed`.
SpuStep spu_run(SPUContext& c, uint64_t max_steps, uint64_t& executed);

}  // namespace sly4
