// Reference PPU interpreter: executes one instruction at a time with the exact
// architectural semantics (64-bit mode, Cell PPU). It is the oracle for the C++
// emitter (differential tests) and the runtime's fallback for code the static
// analysis did not find. The per-instruction semantics live in ppu_semantics.h and
// are validated against real PowerPC execution under QEMU (tools/ppc/oracle).
#pragma once

#include <cstdint>

#include "cpu/ppu_context.h"
#include "cpu/ppu_semantics.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"

namespace sly4 {

// Executes `in`, which sits at c.cia.
PpuStep ppu_execute(PPUContext& c, GuestMemory& mem, const ppu::Insn& in);
// Fetches, decodes and executes the instruction at c.cia.
PpuStep ppu_step(PPUContext& c, GuestMemory& mem);

}  // namespace sly4
