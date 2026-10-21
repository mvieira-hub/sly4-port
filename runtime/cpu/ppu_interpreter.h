// Reference PPU interpreter: executes one instruction at a time with the exact
// architectural semantics (64-bit mode, Cell PPU). It is the oracle for the C++
// emitter (differential tests) and the runtime's fallback for code the static
// analysis did not find. It is validated against real PowerPC execution under QEMU
// (tools/ppc/oracle).
#pragma once

#include <cstdint>

#include "cpu/ppu_context.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"

namespace sly4 {

enum class PpuStep : uint8_t {
    Ok,       // executed; c.cia is the next instruction (or the branch target)
    Syscall,  // sc: the kernel handles r11/r3..; c.cia already points past it
    Trap,     // tw/td/twi/tdi condition met (c.cia still on the trap)
    Halt,     // attn
    Invalid,  // not a valid instruction
};

// Executes `in`, which sits at c.cia.
PpuStep ppu_execute(PPUContext& c, GuestMemory& mem, const ppu::Insn& in);
// Fetches, decodes and executes the instruction at c.cia.
PpuStep ppu_step(PPUContext& c, GuestMemory& mem);

namespace ppu_interp {
// Floating point (ppu_interp_fpu.cpp) and VMX (ppu_interp_vmx.cpp); false = not theirs.
bool exec_fpu(PPUContext& c, GuestMemory& mem, const ppu::Insn& in);
bool exec_vmx(PPUContext& c, GuestMemory& mem, const ppu::Insn& in);
}  // namespace ppu_interp

}  // namespace sly4
