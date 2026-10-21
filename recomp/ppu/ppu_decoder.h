// PPU instruction decoder: 32-bit word -> base instruction + field accessors.
// Bit numbering in comments follows the Power ISA (bit 0 = most significant).
#pragma once

#include <cstdint>

namespace sly4::ppu {

enum class Form : uint8_t {
    NONE,
    D_RT_RA0_SI, D_RT_RA_SI, D_RA_RS_UI, D_CMPI, D_CMPLI, D_TRAPI,
    D_MEM_R, DS_MEM_R, D_MEM_F,
    I_BRANCH, B_BRANCH, XL_BRANCH, SC,
    XL_MCRF, XL_CR3,
    M_IMM, M_REG, MD_IMM, MDS_REG,
    X_CMP, X_TRAP, XO_3, XO_2, X_LOG3, X_LOG2, X_SHIFTI, XS_SHIFTI,
    X_MFSPR, X_MTSPR, X_MFCR, X_MFOCRF, X_MTCRF, X_SYNC,
    X_MEM_R, X_MEM_RES, X_MEM_F, X_MEM_V, X_CACHE, X_DCBT,
    A_FRT_FRA_FRB, A_FRT_FRA_FRC, A_FRT_FRA_FRC_FRB, A_FRT_FRB, A_FRSQRTE,
    X_FRT_FRB, X_FCMP, X_MFFS, X_MTFSF, X_MTFSB, X_MTFSFI,
    VA_ABC, VA_ACB, VA_SLDOI, VC, VX_ABD, VX_BD, VX_UIMM, VX_SIMM, VX_D, VX_B,
};

enum class Op : uint16_t {
    invalid,
#define X(e, name, form) e,
#include "ppu_ops.inc"
#undef X
    count
};

const char* mnemonic(Op op);
Form form(Op op);

struct Insn {
    Op op = Op::invalid;
    uint32_t w = 0;

    // Register / field operands (Power ISA bit positions)
    uint32_t rd() const { return (w >> 21) & 31; }   // RT/RS/FRT/FRS/VD/VS/BO/TO, bits 6-10
    uint32_t ra() const { return (w >> 16) & 31; }   // RA/FRA/VA/BI, bits 11-15
    uint32_t rb() const { return (w >> 11) & 31; }   // RB/FRB/VB, bits 16-20
    uint32_t rc() const { return (w >> 6) & 31; }    // FRC/VC, bits 21-25
    bool rc_bit() const { return w & 1; }            // Rc ('.') in X/XO/A/M/MD forms
    bool oe() const { return (w >> 10) & 1; }        // OE in XO form
    bool lk() const { return w & 1; }                // LK in branches
    bool aa() const { return (w >> 1) & 1; }         // AA in I/B branches

    int32_t simm() const { return static_cast<int16_t>(w & 0xFFFF); }
    uint32_t uimm() const { return w & 0xFFFF; }
    int32_t ds() const { return static_cast<int16_t>(w & 0xFFFC); }
    int32_t bd() const { return static_cast<int16_t>(w & 0xFFFC); }
    int32_t li() const { return static_cast<int32_t>((w & 0x03FFFFFC) << 6) >> 6; }

    uint32_t bf() const { return (w >> 23) & 7; }     // CR field (cmp, mcrf, fcmp)
    uint32_t bfa() const { return (w >> 18) & 7; }
    uint32_t l_cmp() const { return (w >> 21) & 1; }  // 64-bit compare
    uint32_t sh() const { return (w >> 11) & 31; }
    uint32_t mb() const { return (w >> 6) & 31; }
    uint32_t me() const { return (w >> 1) & 31; }
    uint32_t sh64() const { return ((w >> 11) & 31) | ((w >> 1) & 1) << 5; }  // MD/XS: sh[0:4] || sh[5]
    uint32_t mb64() const { return ((w >> 6) & 31) | ((w >> 5) & 1) << 5; }   // MD: mb[0:4] || mb[5], stored swapped
    uint32_t spr() const { return ((w >> 16) & 31) | ((w >> 11) & 31) << 5; }
    uint32_t fxm() const { return (w >> 12) & 0xFF; }
    uint32_t flm() const { return (w >> 17) & 0xFF; }
    uint32_t bh() const { return (w >> 11) & 3; }
    uint32_t lev() const { return (w >> 5) & 0x7F; }
    uint32_t sync_l() const { return (w >> 21) & 3; }
    uint32_t vuimm() const { return (w >> 16) & 31; }
    int32_t vsimm() const { return static_cast<int32_t>(((w >> 16) & 31) << 27) >> 27; }
    uint32_t vshb() const { return (w >> 6) & 15; }
    bool vc_rc() const { return (w >> 10) & 1; }       // '.' of VMX compares

    // Branch target for b/bc at `addr` (32-bit address space).
    uint32_t branch_target(uint32_t addr) const {
        int32_t off = op == Op::b ? li() : bd();
        return aa() ? static_cast<uint32_t>(off) : addr + static_cast<uint32_t>(off);
    }
};

Insn decode(uint32_t w);

}  // namespace sly4::ppu
