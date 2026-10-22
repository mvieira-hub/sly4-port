// SPU (Cell Synergistic Processor Unit) instruction decoder: 32-bit word -> instruction
// + field accessors. Fields are named as binutils does: T is bits 0-6 (least
// significant), A 7-13, B 14-20, C 21-27. In RRR forms C is the destination and T the
// third source.
#pragma once

#include <cstdint>
#include <string>

namespace sly4::spu {

// Instruction formats, by opcode length: RRR 4 bits, RI18/LBT 7, RI10 8, RI16 9,
// RI8 10, RR/RI7/LBTI 11.
enum class Form : uint8_t { RRR, RI18, RI16, RI10, RI8, RI7, RR, LBT, LBTI };

// Operand kinds of the assembler syntax (binutils' A_* argument types).
enum class Arg : uint8_t {
    T, A, B, C,  // registers
    S, H,        // special purpose register / channel, in the A field
    P,           // "(": the next register is the base of a displacement
    I7,          // signed 7-bit immediate (shift counts, cbd offsets, dftsv mask)
    U7A, U7B,    // 173 - u8 / 155 - u8: scale of the float <-> int conversions
    I10,         // signed 10-bit immediate
    S14,         // signed 10-bit immediate * 16 (lqd/stqd displacement)
    S11, S11I,   // branch hint address: ip + 9-bit (split field) * 4
    S16, X16,    // 16-bit immediate, signed / unsigned
    R18,         // ip-relative 16-bit word offset (branches, lqr/stqr)
    S18,         // absolute 16-bit word address (bra, lqa/stqa)
    U18,         // 18-bit unsigned immediate (ila)
};

enum class Op : uint16_t {
    invalid,
#define X(e, mnem, form, opc, ...) e,
#include "spu_ops.inc"
#undef X
    count
};

const char* mnemonic(Op op);
const char* enum_name(Op op);
Form form(Op op);

struct Insn {
    Op op = Op::invalid;
    uint32_t w = 0;

    uint32_t rt() const { return w & 0x7F; }
    uint32_t ra() const { return (w >> 7) & 0x7F; }
    uint32_t rb() const { return (w >> 14) & 0x7F; }
    uint32_t rc() const { return (w >> 21) & 0x7F; }

    int32_t i7() const { return static_cast<int32_t>(w << 11) >> 25; }
    uint32_t u8() const { return (w >> 14) & 0xFF; }
    int32_t i10() const { return static_cast<int32_t>(w << 8) >> 22; }
    int32_t i16() const { return static_cast<int32_t>(w << 9) >> 16; }
    uint32_t u16() const { return (w >> 7) & 0xFFFF; }
    uint32_t u18() const { return (w >> 7) & 0x3FFFF; }
    uint32_t u14() const { return w & 0x3FFF; }  // stop signal
    // Branch hint: the hinted branch is ip + i9 * 4; hbra/hbrr split it as bits 23-24 + 0-6,
    // hbr as bits 14-15 + 0-6.
    int32_t i9a() const { return (static_cast<int32_t>(w << 7) >> 30) * 128 | static_cast<int32_t>(w & 0x7F); }
    int32_t i9b() const { return (static_cast<int32_t>(w << 16) >> 30) * 128 | static_cast<int32_t>(w & 0x7F); }
    // Interrupt / prediction flags of the indirect branches, sync and hbr (bits 14-20).
    uint32_t fb() const { return (w >> 14) & 0x7F; }
};

Insn decode(uint32_t w);

// Control flow, for the SPU recompiler and its block cache.
// may_leave: the instruction can return something other than "go on to the next word"
// (branches, stop, halts, channels that wait). ends_block: execution never goes on to
// the next word (unconditional branches, stop, words that are not instructions).
bool may_leave(Op op);
bool ends_block(Op op);
// Direct branch target (relative or absolute), or false for other instructions.
bool direct_target(const Insn& in, uint32_t ip, uint32_t& target);

// Text in the format of spu-elf-objdump: "mnemonic\toperands" plus "\t# hex" when the
// last immediate is above 16, or ".long 0x..." for words that are not instructions.
// `ip` is the local store address of the word.
std::string disasm(const Insn& in, uint32_t ip);

}  // namespace sly4::spu
