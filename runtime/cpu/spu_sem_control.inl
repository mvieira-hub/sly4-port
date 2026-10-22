// SPU control flow, stop, hints, channels and SPRs. Included by spu_semantics.h.
#pragma once

namespace sly4::spusem {

// ---------------------------------------------------------------- branches
#define SPU_BRANCH(name, ...)                                    \
    template <>                                                  \
    inline SpuStep op<Op::name>(SPUContext & c, Insn in) {       \
        [[maybe_unused]] const u32 rel = branch_target(c.pc + u32(in.i16() * 4)); \
        [[maybe_unused]] const u32 abs = branch_target(u32(in.i16() * 4)); \
        [[maybe_unused]] const u32 ind = branch_target(c.gpr[in.ra()].pref()); \
        [[maybe_unused]] const SpuReg& RT = c.gpr[in.rt()];      \
        __VA_ARGS__;                                             \
    }

SPU_BRANCH(br, c.pc = rel; return SpuStep::Branch)
SPU_BRANCH(bra, c.pc = abs; return SpuStep::Branch)
SPU_BRANCH(brsl, set_link(c, in.rt()); c.pc = rel; return SpuStep::Branch)
SPU_BRANCH(brasl, set_link(c, in.rt()); c.pc = abs; return SpuStep::Branch)
// Indirect branches that are taken also apply their E (enable interrupts) and D
// (disable) flags.
inline SpuStep take_ind(SPUContext& c, Insn in, u32 target) {
    if (in.fb() & 0x10) c.irq = true;
    if (in.fb() & 0x20) c.irq = false;
    c.pc = target;
    return SpuStep::Branch;
}
SPU_BRANCH(bi, return take_ind(c, in, ind))
SPU_BRANCH(bisl, set_link(c, in.rt()); return take_ind(c, in, ind))
SPU_BRANCH(iret, return take_ind(c, in, c.srr0 & kSpuLsMask & ~3u))
// bisled: branch to RA if an event is pending, link either way.
SPU_BRANCH(bisled, const bool ev = (c.event_stat & c.event_mask) != 0; set_link(c, in.rt());
           if (ev) return take_ind(c, in, ind); c.pc = c.pc + 4; return SpuStep::Branch)

// Conditional branches test the preferred word (or its low halfword) of RT.
SPU_BRANCH(brz, if (RT.pref() == 0) { c.pc = rel; return SpuStep::Branch; } return SpuStep::Ok)
SPU_BRANCH(brnz, if (RT.pref() != 0) { c.pc = rel; return SpuStep::Branch; } return SpuStep::Ok)
SPU_BRANCH(brhz, if ((RT.pref() & 0xFFFF) == 0) { c.pc = rel; return SpuStep::Branch; } return SpuStep::Ok)
SPU_BRANCH(brhnz, if ((RT.pref() & 0xFFFF) != 0) { c.pc = rel; return SpuStep::Branch; } return SpuStep::Ok)
SPU_BRANCH(biz, if (RT.pref() == 0) return take_ind(c, in, ind); return SpuStep::Ok)
SPU_BRANCH(binz, if (RT.pref() != 0) return take_ind(c, in, ind); return SpuStep::Ok)
SPU_BRANCH(bihz, if ((RT.pref() & 0xFFFF) == 0) return take_ind(c, in, ind); return SpuStep::Ok)
SPU_BRANCH(bihnz, if ((RT.pref() & 0xFFFF) != 0) return take_ind(c, in, ind); return SpuStep::Ok)
#undef SPU_BRANCH

// ---------------------------------------------------------------- no-ops, hints, stop
SPU_SEM(nop, (void)0)
SPU_SEM(lnop, (void)0)
SPU_SEM(sync, (void)0)
SPU_SEM(dsync, (void)0)
SPU_SEM(hbr, (void)0)
SPU_SEM(hbra, (void)0)
SPU_SEM(hbrr, (void)0)

// stop: the 14-bit signal goes to whoever runs the SPU (thread group, SPURS...).
template <>
inline SpuStep op<Op::stop>(SPUContext& c, Insn in) {
    c.stop_code = in.u14();
    c.pc += 4;
    return SpuStep::Stop;
}
template <>
inline SpuStep op<Op::stopd>(SPUContext& c, Insn) {
    c.stop_code = 0x3FFF;  // debug stop
    c.pc += 4;
    return SpuStep::Stop;
}

// ---------------------------------------------------------------- channels and SPRs
template <>
inline SpuStep op<Op::rdch>(SPUContext& c, Insn in) {
    u32 v = 0;
    switch (spu_read_channel(c, in.ra(), v)) {
        case SpuChannelResult::Ok: break;
        case SpuChannelResult::Wait: return SpuStep::Wait;
        case SpuChannelResult::Invalid: return SpuStep::Invalid;
    }
    SpuReg r{};
    r.u32(0) = v;
    c.gpr[in.rt()] = r;
    return SpuStep::Ok;
}
template <>
inline SpuStep op<Op::rchcnt>(SPUContext& c, Insn in) {
    SpuReg r{};
    r.u32(0) = spu_channel_count(c, in.ra());
    c.gpr[in.rt()] = r;
    return SpuStep::Ok;
}
template <>
inline SpuStep op<Op::wrch>(SPUContext& c, Insn in) {
    switch (spu_write_channel(c, in.ra(), c.gpr[in.rt()].pref())) {
        case SpuChannelResult::Ok: return SpuStep::Ok;
        case SpuChannelResult::Wait: return SpuStep::Wait;
        case SpuChannelResult::Invalid: break;
    }
    return SpuStep::Invalid;
}
// No SPR is implemented on the SPU of the PS3's user programs: mfspr reads 0.
SPU_SEM(mfspr, SpuReg r{}; RT = r)
SPU_SEM(mtspr, (void)0)

}  // namespace sly4::spusem
