// Text form of a decoded PPU instruction, in the syntax of GNU objdump
// "-M cell,raw" (binutils 2.35): base mnemonics, plus the few aliases objdump still
// prints in raw mode (mr, not, vnot, crnot, bdnz/bt/bf forms and +/- hints).
// Matching objdump exactly lets the decoder be checked against binutils on the whole
// executable (tools/ppc/compare_disasm.py).
#pragma once

#include <cstdint>
#include <string>

#include "ppu/ppu_decoder.h"

namespace sly4::ppu {

std::string disasm(const Insn& in, uint32_t addr);

}  // namespace sly4::ppu
