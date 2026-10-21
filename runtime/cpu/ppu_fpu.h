// PPU floating point core, shared by the interpreter and the recompiled code: PowerPC
// NaN rules, full FPSCR updates, conversions and the bit-exact lfs/stfs formats.
// The per-instruction wrappers live in ppu_sem_fpu.inl.
#pragma once

#include <cstdint>

#include "cpu/ppu_context.h"

namespace sly4::fp {

enum class Arith { Add, Sub, Mul, Div, Sqrt, MAdd, MSub, NMAdd, NMSub };

// fadd/fsub/fmul/fdiv/fsqrt/fmadd/... (single or double); A, B, C as in the A-form
// (fmul uses A*C, fsqrt only B). Writes FRT and updates the FPSCR.
void arith(PPUContext& c, uint32_t frt, uint64_t A, uint64_t B, uint64_t C, Arith k, bool single);
void frsqrte(PPUContext& c, uint32_t frt, uint64_t B);
void frsp(PPUContext& c, uint32_t frt, uint64_t B);
void fcfid(PPUContext& c, uint32_t frt, uint64_t B);
void convert_to_int(PPUContext& c, uint32_t frt, uint64_t B, bool word, bool toward_zero);  // fcti[w|d][z]
void fcmp(PPUContext& c, uint32_t bf, uint64_t A, uint64_t B, bool ordered);
void mtfsf(PPUContext& c, uint32_t flm, uint64_t B);
void mtfsfi(PPUContext& c, uint32_t bf, uint32_t imm);
void mtfsb0(PPUContext& c, uint32_t bt);
void mtfsb1(PPUContext& c, uint32_t bt);
void mcrfs(PPUContext& c, uint32_t bf, uint32_t bfa);

// lfs / stfs conversions as the ISA defines them (bit exact, SNaN preserved, no rounding).
uint64_t single_to_double(uint32_t w);
uint32_t double_to_single_store(uint64_t x);

// Rc=1 on FP instructions: CR1 = FPSCR[FX FEX VX OX].
inline void set_cr1(PPUContext& c) { c.set_crf(1, c.fpscr >> 28); }

}  // namespace sly4::fp
