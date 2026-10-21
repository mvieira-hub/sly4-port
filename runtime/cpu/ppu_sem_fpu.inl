// Floating-point semantics: thin per-instruction wrappers over the FP core (ppu_fpu.h).
#pragma once

namespace sly4::sem {

// ---------------- loads / stores
SLY4_SEM(lfs, c.fpr[d] = fp::single_to_double(mem.read<uint32_t>(ea_d(c, in))))
SLY4_SEM(lfsx, c.fpr[d] = fp::single_to_double(mem.read<uint32_t>(ea_x(c, in))))
SLY4_SEM(lfsu, const u64 ea = r[a] + static_cast<s64>(in.simm());
         c.fpr[d] = fp::single_to_double(mem.read<uint32_t>(GuestAddr(ea))); r[a] = ea)
SLY4_SEM(lfsux, const u64 ea = r[a] + r[b]; c.fpr[d] = fp::single_to_double(mem.read<uint32_t>(GuestAddr(ea)));
         r[a] = ea)
SLY4_SEM(lfd, c.fpr[d] = mem.read<uint64_t>(ea_d(c, in)))
SLY4_SEM(lfdx, c.fpr[d] = mem.read<uint64_t>(ea_x(c, in)))
SLY4_SEM(lfdu, const u64 ea = r[a] + static_cast<s64>(in.simm()); c.fpr[d] = mem.read<uint64_t>(GuestAddr(ea));
         r[a] = ea)
SLY4_SEM(lfdux, const u64 ea = r[a] + r[b]; c.fpr[d] = mem.read<uint64_t>(GuestAddr(ea)); r[a] = ea)
SLY4_SEM(stfs, mem.write<uint32_t>(ea_d(c, in), fp::double_to_single_store(c.fpr[d])))
SLY4_SEM(stfsx, mem.write<uint32_t>(ea_x(c, in), fp::double_to_single_store(c.fpr[d])))
SLY4_SEM(stfsu, const u64 ea = r[a] + static_cast<s64>(in.simm());
         mem.write<uint32_t>(GuestAddr(ea), fp::double_to_single_store(c.fpr[d])); r[a] = ea)
SLY4_SEM(stfsux, const u64 ea = r[a] + r[b]; mem.write<uint32_t>(GuestAddr(ea), fp::double_to_single_store(c.fpr[d]));
         r[a] = ea)
SLY4_SEM(stfd, mem.write<uint64_t>(ea_d(c, in), c.fpr[d]))
SLY4_SEM(stfdx, mem.write<uint64_t>(ea_x(c, in), c.fpr[d]))
SLY4_SEM(stfdu, const u64 ea = r[a] + static_cast<s64>(in.simm()); mem.write<uint64_t>(GuestAddr(ea), c.fpr[d]);
         r[a] = ea)
SLY4_SEM(stfdux, const u64 ea = r[a] + r[b]; mem.write<uint64_t>(GuestAddr(ea), c.fpr[d]); r[a] = ea)
SLY4_SEM(stfiwx, mem.write<uint32_t>(ea_x(c, in), uint32_t(c.fpr[d])))

// ---------------- arithmetic (Rc=1 copies FPSCR[0:3] to CR1)
#define SLY4_SEM_ARITH(name, kind, single)                                                       \
    SLY4_SEM(name, fp::arith(c, d, c.fpr[a], c.fpr[b], c.fpr[in.rc()], fp::Arith::kind, single); \
             if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM_ARITH(fadd, Add, false)
SLY4_SEM_ARITH(fadds, Add, true)
SLY4_SEM_ARITH(fsub, Sub, false)
SLY4_SEM_ARITH(fsubs, Sub, true)
SLY4_SEM_ARITH(fmul, Mul, false)
SLY4_SEM_ARITH(fmuls, Mul, true)
SLY4_SEM_ARITH(fdiv, Div, false)
SLY4_SEM_ARITH(fdivs, Div, true)
SLY4_SEM_ARITH(fsqrt, Sqrt, false)
SLY4_SEM_ARITH(fsqrts, Sqrt, true)
SLY4_SEM_ARITH(fmadd, MAdd, false)
SLY4_SEM_ARITH(fmadds, MAdd, true)
SLY4_SEM_ARITH(fmsub, MSub, false)
SLY4_SEM_ARITH(fmsubs, MSub, true)
SLY4_SEM_ARITH(fnmadd, NMAdd, false)
SLY4_SEM_ARITH(fnmadds, NMAdd, true)
SLY4_SEM_ARITH(fnmsub, NMSub, false)
SLY4_SEM_ARITH(fnmsubs, NMSub, true)
#undef SLY4_SEM_ARITH
// An estimate on hardware; computed as 1/x correctly rounded to single.
SLY4_SEM(fres, fp::arith(c, d, 0x3FF0000000000000ull, c.fpr[b], 0, fp::Arith::Div, true);
         if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(frsqrte, fp::frsqrte(c, d, c.fpr[b]); if (in.rc_bit()) fp::set_cr1(c))

// ---------------- moves, select, conversions
SLY4_SEM(fsel, double av; std::memcpy(&av, &c.fpr[a], 8);
         const bool nan = (c.fpr[a] & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
                          (c.fpr[a] & 0x000FFFFFFFFFFFFFull);
         c.fpr[d] = (!nan && av >= 0.0) ? c.fpr[in.rc()] : c.fpr[b]; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fmr, c.fpr[d] = c.fpr[b]; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fneg, c.fpr[d] = c.fpr[b] ^ 0x8000000000000000ull; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fabs, c.fpr[d] = c.fpr[b] & 0x7FFFFFFFFFFFFFFFull; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fnabs, c.fpr[d] = c.fpr[b] | 0x8000000000000000ull; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(frsp, fp::frsp(c, d, c.fpr[b]); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fctiw, fp::convert_to_int(c, d, c.fpr[b], true, false); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fctiwz, fp::convert_to_int(c, d, c.fpr[b], true, true); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fctid, fp::convert_to_int(c, d, c.fpr[b], false, false); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fctidz, fp::convert_to_int(c, d, c.fpr[b], false, true); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(fcfid, fp::fcfid(c, d, c.fpr[b]); if (in.rc_bit()) fp::set_cr1(c))

// ---------------- compares and FPSCR
SLY4_SEM(fcmpu, fp::fcmp(c, in.bf(), c.fpr[a], c.fpr[b], false))
SLY4_SEM(fcmpo, fp::fcmp(c, in.bf(), c.fpr[a], c.fpr[b], true))
SLY4_SEM(mffs, c.fpr[d] = c.fpscr; if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(mtfsf, fp::mtfsf(c, in.flm(), c.fpr[b]); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(mtfsfi, fp::mtfsfi(c, in.bf(), (in.w >> 12) & 0xF); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(mtfsb0, fp::mtfsb0(c, d); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(mtfsb1, fp::mtfsb1(c, d); if (in.rc_bit()) fp::set_cr1(c))
SLY4_SEM(mcrfs, fp::mcrfs(c, in.bf(), in.bfa()))

}  // namespace sly4::sem
