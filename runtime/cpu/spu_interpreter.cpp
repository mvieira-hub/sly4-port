#include "cpu/spu_interpreter.h"

namespace sly4 {

using spu::Insn;
using spu::Op;

SpuStep spu_execute(SPUContext& c, const Insn& in) {
    SpuStep st;
    switch (in.op) {
#define X(e, mnem, form, opc, ...) \
    case Op::e: st = spusem::op<Op::e>(c, in); break;
#include "spu/spu_ops.inc"
#undef X
        default: return SpuStep::Invalid;
    }
    if (st == SpuStep::Ok) c.pc = (c.pc + 4) & kSpuLsMask;
    return st;
}

SpuStep spu_step(SPUContext& c) { return spu_execute(c, spu::decode(c.ls_u32(c.pc))); }

SpuStep spu_run(SPUContext& c, uint64_t max_steps, uint64_t& executed) {
    for (uint64_t n = 0; n < max_steps; ++n) {
        const SpuStep st = spu_step(c);
        if (st == SpuStep::Branch) spu_check_interrupt(c);
        if (st != SpuStep::Ok && st != SpuStep::Branch) {
            executed += n + (st == SpuStep::Stop || st == SpuStep::Halt ? 1 : 0);
            return st;
        }
    }
    executed += max_steps;
    return SpuStep::Ok;
}

}  // namespace sly4
