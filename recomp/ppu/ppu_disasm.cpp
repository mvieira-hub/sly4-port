#include "ppu/ppu_disasm.h"

#include <cstdarg>
#include <cstdio>

namespace sly4::ppu {

namespace {

std::string fmt(const char* f, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// Condition register bit: lt/gt/eq/so of CR0, "4*crN+xx" for the other fields.
std::string crbit(uint32_t b) {
    static const char* names[] = {"lt", "gt", "eq", "so"};
    if (b < 4) return names[b];
    return fmt("4*cr%u+%s", b / 4, names[b % 4]);
}

std::string ra0(uint32_t ra) { return ra ? fmt("r%u", ra) : std::string("0"); }

// objdump's choice of mnemonic for conditional branches in raw mode:
// - BO values whose "at" hint would be the reserved 0b01 are printed with the old
//   aliases (bdnzf, bdzf, bf, bdnzt, bdzt, bt, bdnz, bdz), BI as a CR bit;
// - BO with at = 0b10 / 0b11 get a '-' / '+' hint suffix;
// - everything else prints as bc/bclr/bcctr with BO,BI numeric/named.
struct BranchName {
    const char* alias = nullptr;  // nullptr: plain base mnemonic
    bool uses_bi = true;
    char hint = 0;
};

BranchName branch_name(uint32_t bo) {
    switch (bo) {
        case 1: return {"bdnzf"};
        case 3: return {"bdzf"};
        case 5: return {"bf"};
        case 9: return {"bdnzt"};
        case 11: return {"bdzt"};
        case 13: return {"bt"};
        case 17: return {"bdnz", false};
        case 19: return {"bdz", false};
        case 6: case 14: case 24: case 26: return {nullptr, true, '-'};
        case 7: case 15: case 25: case 27: return {nullptr, true, '+'};
        default: return {};
    }
}

std::string op_name(const Insn& in) {
    std::string n = mnemonic(in.op);
    switch (form(in.op)) {
        case Form::XO_3:
        case Form::XO_2:
            if (in.oe() && in.op != Op::mulhw && in.op != Op::mulhwu && in.op != Op::mulhd && in.op != Op::mulhdu)
                n += 'o';
            if (in.rc_bit()) n += '.';
            break;
        case Form::X_LOG3: case Form::X_LOG2: case Form::X_SHIFTI: case Form::XS_SHIFTI:
        case Form::M_IMM: case Form::M_REG: case Form::MD_IMM: case Form::MDS_REG:
        case Form::A_FRT_FRA_FRB: case Form::A_FRT_FRA_FRC: case Form::A_FRT_FRA_FRC_FRB:
        case Form::A_FRT_FRB: case Form::A_FRSQRTE: case Form::X_FRT_FRB: case Form::X_MFFS:
        case Form::X_MTFSF: case Form::X_MTFSB: case Form::X_MTFSFI:
            if (in.rc_bit()) n += '.';
            break;
        case Form::VC:
            if (in.vc_rc()) n += '.';
            break;
        default:
            break;
    }
    return n;
}

std::string with_operands(const std::string& name, const std::string& ops) {
    if (ops.empty()) return name;
    std::string s = name;
    s.resize(name.size() < 7 ? 8 : name.size() + 1, ' ');  // objdump pads the mnemonic to 7 + 1
    return s + ops;
}

std::string branch(const Insn& in, uint32_t addr) {
    const uint32_t bo = in.rd(), bi = in.ra();
    const BranchName bn = branch_name(bo);
    std::string base = in.op == Op::bc ? "bc" : in.op == Op::bclr ? "bclr" : "bcctr";
    std::string ops;
    if (in.op == Op::bc) {
        const std::string target = fmt("0x%x", in.branch_target(addr));
        if (bn.alias) {
            base = bn.alias;
            ops = bn.uses_bi ? crbit(bi) + "," + target : target;
        } else {
            ops = fmt("%u,", bo) + crbit(bi) + "," + target;
        }
        if (in.lk()) base += 'l';
        if (in.aa()) base += 'a';
    } else {
        ops = fmt("%u,", bo) + crbit(bi) + fmt(",%u", in.bh());
        if (in.lk()) base += 'l';
    }
    if (!bn.alias && bn.hint) base += bn.hint;
    return with_operands(base, ops);
}

}  // namespace

std::string disasm(const Insn& in, uint32_t addr) {
    if (in.op == Op::invalid) return fmt(".long 0x%x", in.w);
    const std::string n = op_name(in);
    const uint32_t d = in.rd(), a = in.ra(), b = in.rb(), c = in.rc();
    switch (form(in.op)) {
        case Form::NONE: return n;
        case Form::D_RT_RA0_SI: return with_operands(n, fmt("r%u,", d) + ra0(a) + fmt(",%d", in.simm()));
        case Form::D_RT_RA_SI: return with_operands(n, fmt("r%u,r%u,%d", d, a, in.simm()));
        case Form::D_RA_RS_UI: return with_operands(n, fmt("r%u,r%u,%u", a, d, in.uimm()));
        case Form::D_CMPI: return with_operands(n, fmt("cr%u,%u,r%u,%d", in.bf(), in.l_cmp(), a, in.simm()));
        case Form::D_CMPLI: return with_operands(n, fmt("cr%u,%u,r%u,%u", in.bf(), in.l_cmp(), a, in.uimm()));
        case Form::D_TRAPI: return with_operands(n, fmt("%u,r%u,%d", d, a, in.simm()));
        case Form::D_MEM_R: return with_operands(n, fmt("r%u,%d(", d, in.simm()) + ra0(a) + ")");
        case Form::DS_MEM_R: return with_operands(n, fmt("r%u,%d(", d, in.ds()) + ra0(a) + ")");
        case Form::D_MEM_F: return with_operands(n, fmt("f%u,%d(", d, in.simm()) + ra0(a) + ")");

        case Form::I_BRANCH: {
            std::string base = "b";
            if (in.lk()) base += 'l';
            if (in.aa()) base += 'a';
            return with_operands(base, fmt("0x%x", in.branch_target(addr)));
        }
        case Form::B_BRANCH:
        case Form::XL_BRANCH: return branch(in, addr);
        case Form::SC: return with_operands(n, fmt("%u", in.lev()));

        case Form::XL_MCRF: return with_operands(n, fmt("cr%u,cr%u", in.bf(), in.bfa()));
        case Form::XL_CR3:
            if (in.op == Op::crnor && a == b) return with_operands("crnot", crbit(d) + "," + crbit(a));
            return with_operands(n, crbit(d) + "," + crbit(a) + "," + crbit(b));

        case Form::M_IMM: return with_operands(n, fmt("r%u,r%u,%u,%u,%u", a, d, in.sh(), in.mb(), in.me()));
        case Form::M_REG: return with_operands(n, fmt("r%u,r%u,r%u,%u,%u", a, d, b, in.mb(), in.me()));
        case Form::MD_IMM: return with_operands(n, fmt("r%u,r%u,%u,%u", a, d, in.sh64(), in.mb64()));
        case Form::MDS_REG: return with_operands(n, fmt("r%u,r%u,r%u,%u", a, d, b, in.mb64()));

        case Form::X_CMP: return with_operands(n, fmt("cr%u,%u,r%u,r%u", in.bf(), in.l_cmp(), a, b));
        case Form::X_TRAP: return with_operands(n, fmt("%u,r%u,r%u", d, a, b));
        case Form::XO_3: return with_operands(n, fmt("r%u,r%u,r%u", d, a, b));
        case Form::XO_2: return with_operands(n, fmt("r%u,r%u", d, a));
        case Form::X_LOG3:
            if (in.op == Op::or_ && d == b && !in.rc_bit()) return with_operands("mr", fmt("r%u,r%u", a, d));
            if (in.op == Op::nor && d == b && !in.rc_bit()) return with_operands("not", fmt("r%u,r%u", a, d));
            return with_operands(n, fmt("r%u,r%u,r%u", a, d, b));
        case Form::X_LOG2: return with_operands(n, fmt("r%u,r%u", a, d));
        case Form::X_SHIFTI: return with_operands(n, fmt("r%u,r%u,%u", a, d, in.sh()));
        case Form::XS_SHIFTI: return with_operands(n, fmt("r%u,r%u,%u", a, d, in.sh64()));
        case Form::X_MFSPR: return with_operands(n, fmt("r%u,%u", d, in.spr()));
        case Form::X_MTSPR: return with_operands(n, fmt("%u,r%u", in.spr(), d));
        case Form::X_MFCR: return with_operands(n, fmt("r%u", d));
        case Form::X_MFOCRF: return with_operands(n, fmt("r%u,%u", d, in.fxm()));
        case Form::X_MTCRF: return with_operands(n, fmt("%u,r%u", in.fxm(), d));
        case Form::X_SYNC: return with_operands(n, fmt("%u", in.sync_l()));

        case Form::X_MEM_R: return with_operands(n, fmt("r%u,", d) + ra0(a) + fmt(",r%u", b));
        case Form::X_MEM_RES: return with_operands(n, fmt("r%u,", d) + ra0(a) + fmt(",r%u,%u", b, in.w & 1));
        case Form::X_MEM_F: return with_operands(n, fmt("f%u,", d) + ra0(a) + fmt(",r%u", b));
        case Form::X_MEM_V: return with_operands(n, fmt("v%u,", d) + ra0(a) + fmt(",r%u", b));
        case Form::X_CACHE: return with_operands(n, ra0(a) + fmt(",r%u", b));
        case Form::X_DCBT: return with_operands(n, ra0(a) + fmt(",r%u,%u", b, d));

        case Form::A_FRT_FRA_FRB: return with_operands(n, fmt("f%u,f%u,f%u", d, a, b));
        case Form::A_FRT_FRA_FRC: return with_operands(n, fmt("f%u,f%u,f%u", d, a, c));
        case Form::A_FRT_FRA_FRC_FRB: return with_operands(n, fmt("f%u,f%u,f%u,f%u", d, a, c, b));
        case Form::A_FRT_FRB: return with_operands(n, fmt("f%u,f%u", d, b));
        case Form::A_FRSQRTE: return with_operands(n, fmt("f%u,f%u,%u", d, b, a));
        case Form::X_FRT_FRB: return with_operands(n, fmt("f%u,f%u", d, b));
        case Form::X_FCMP: return with_operands(n, fmt("cr%u,f%u,f%u", in.bf(), a, b));
        case Form::X_MFFS: return with_operands(n, fmt("f%u", d));
        case Form::X_MTFSF: return with_operands(n, fmt("%u,f%u", in.flm(), b));
        case Form::X_MTFSB: return with_operands(n, fmt("%u", d));
        case Form::X_MTFSFI: return with_operands(n, fmt("cr%u,%u", in.bf(), (in.w >> 12) & 15));

        case Form::VA_ABC: return with_operands(n, fmt("v%u,v%u,v%u,v%u", d, a, b, c));
        case Form::VA_ACB: return with_operands(n, fmt("v%u,v%u,v%u,v%u", d, a, c, b));
        case Form::VA_SLDOI: return with_operands(n, fmt("v%u,v%u,v%u,%u", d, a, b, in.vshb()));
        case Form::VC: return with_operands(n, fmt("v%u,v%u,v%u", d, a, b));
        case Form::VX_ABD:
            if (in.op == Op::vnor && a == b) return with_operands("vnot", fmt("v%u,v%u", d, a));
            return with_operands(n, fmt("v%u,v%u,v%u", d, a, b));
        case Form::VX_BD: return with_operands(n, fmt("v%u,v%u", d, b));
        case Form::VX_UIMM: return with_operands(n, fmt("v%u,v%u,%u", d, b, in.vuimm()));
        case Form::VX_SIMM: return with_operands(n, fmt("v%u,%d", d, in.vsimm()));
        case Form::VX_D: return with_operands(n, fmt("v%u", d));
        case Form::VX_B: return with_operands(n, fmt("v%u", b));
    }
    return n;
}

}  // namespace sly4::ppu
