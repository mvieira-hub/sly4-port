#include "ppu/ppu_decoder.h"

namespace sly4::ppu {

namespace {

struct OpInfo {
    const char* name;
    Form form;
};

constexpr OpInfo kOps[] = {
    {".long", Form::NONE},
#define X(e, name, f) {name, Form::f},
#include "ppu_ops.inc"
#undef X
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<size_t>(Op::count));

Op primary_d(uint32_t opcd) {
    switch (opcd) {
        case 2: return Op::tdi;      case 3: return Op::twi;      case 7: return Op::mulli;
        case 8: return Op::subfic;   case 10: return Op::cmpli;   case 11: return Op::cmpi;
        case 12: return Op::addic;   case 13: return Op::addic_;  case 14: return Op::addi;
        case 15: return Op::addis;   case 16: return Op::bc;      case 18: return Op::b;
        case 20: return Op::rlwimi;  case 21: return Op::rlwinm;  case 23: return Op::rlwnm;
        case 24: return Op::ori;     case 25: return Op::oris;    case 26: return Op::xori;
        case 27: return Op::xoris;   case 28: return Op::andi_;   case 29: return Op::andis_;
        case 32: return Op::lwz;     case 33: return Op::lwzu;    case 34: return Op::lbz;
        case 35: return Op::lbzu;    case 36: return Op::stw;     case 37: return Op::stwu;
        case 38: return Op::stb;     case 39: return Op::stbu;    case 40: return Op::lhz;
        case 41: return Op::lhzu;    case 42: return Op::lha;     case 43: return Op::lhau;
        case 44: return Op::sth;     case 45: return Op::sthu;    case 46: return Op::lmw;
        case 47: return Op::stmw;    case 48: return Op::lfs;     case 49: return Op::lfsu;
        case 50: return Op::lfd;     case 51: return Op::lfdu;    case 52: return Op::stfs;
        case 53: return Op::stfsu;   case 54: return Op::stfd;    case 55: return Op::stfdu;
        default: return Op::invalid;
    }
}

Op decode_vmx(uint32_t w) {
    switch (w & 0x3F) {  // VA-form, 6-bit extended opcode
        case 32: return Op::vmhaddshs;  case 33: return Op::vmhraddshs; case 34: return Op::vmladduhm;
        case 36: return Op::vmsumubm;   case 37: return Op::vmsummbm;   case 38: return Op::vmsumuhm;
        case 39: return Op::vmsumuhs;   case 40: return Op::vmsumshm;   case 41: return Op::vmsumshs;
        case 42: return Op::vsel;       case 43: return Op::vperm;      case 44: return Op::vsldoi;
        case 46: return Op::vmaddfp;    case 47: return Op::vnmsubfp;
        default: break;
    }
    if ((w & 0x3F) == 6) {  // VC-form, 10-bit extended opcode + Rc in bit 21
        switch (w & 0x3FF) {
            case 6: return Op::vcmpequb;   case 70: return Op::vcmpequh;  case 134: return Op::vcmpequw;
            case 198: return Op::vcmpeqfp; case 454: return Op::vcmpgefp; case 518: return Op::vcmpgtub;
            case 582: return Op::vcmpgtuh; case 646: return Op::vcmpgtuw; case 710: return Op::vcmpgtfp;
            case 774: return Op::vcmpgtsb; case 838: return Op::vcmpgtsh; case 902: return Op::vcmpgtsw;
            case 966: return Op::vcmpbfp;
            default: return Op::invalid;
        }
    }
    switch (w & 0x7FF) {  // VX-form, 11-bit extended opcode
        case 0: return Op::vaddubm;     case 2: return Op::vmaxub;      case 4: return Op::vrlb;
        case 8: return Op::vmuloub;     case 10: return Op::vaddfp;     case 12: return Op::vmrghb;
        case 14: return Op::vpkuhum;    case 64: return Op::vadduhm;    case 66: return Op::vmaxuh;
        case 68: return Op::vrlh;       case 72: return Op::vmulouh;    case 74: return Op::vsubfp;
        case 76: return Op::vmrghh;     case 78: return Op::vpkuwum;    case 128: return Op::vadduwm;
        case 130: return Op::vmaxuw;    case 132: return Op::vrlw;      case 140: return Op::vmrghw;
        case 142: return Op::vpkuhus;   case 206: return Op::vpkuwus;   case 258: return Op::vmaxsb;
        case 260: return Op::vslb;      case 264: return Op::vmulosb;   case 266: return Op::vrefp;
        case 268: return Op::vmrglb;    case 270: return Op::vpkshus;   case 322: return Op::vmaxsh;
        case 324: return Op::vslh;      case 328: return Op::vmulosh;   case 330: return Op::vrsqrtefp;
        case 332: return Op::vmrglh;    case 334: return Op::vpkswus;   case 384: return Op::vaddcuw;
        case 386: return Op::vmaxsw;    case 388: return Op::vslw;      case 394: return Op::vexptefp;
        case 396: return Op::vmrglw;    case 398: return Op::vpkshss;   case 452: return Op::vsl;
        case 458: return Op::vlogefp;   case 462: return Op::vpkswss;   case 512: return Op::vaddubs;
        case 514: return Op::vminub;    case 516: return Op::vsrb;      case 520: return Op::vmuleub;
        case 522: return Op::vrfin;     case 524: return Op::vspltb;    case 526: return Op::vupkhsb;
        case 576: return Op::vadduhs;   case 578: return Op::vminuh;    case 580: return Op::vsrh;
        case 584: return Op::vmuleuh;   case 586: return Op::vrfiz;     case 588: return Op::vsplth;
        case 590: return Op::vupkhsh;   case 640: return Op::vadduws;   case 642: return Op::vminuw;
        case 644: return Op::vsrw;      case 650: return Op::vrfip;     case 652: return Op::vspltw;
        case 654: return Op::vupklsb;   case 708: return Op::vsr;       case 714: return Op::vrfim;
        case 718: return Op::vupklsh;   case 768: return Op::vaddsbs;   case 770: return Op::vminsb;
        case 772: return Op::vsrab;     case 776: return Op::vmulesb;   case 778: return Op::vcfux;
        case 780: return Op::vspltisb;  case 782: return Op::vpkpx;     case 832: return Op::vaddshs;
        case 834: return Op::vminsh;    case 836: return Op::vsrah;     case 840: return Op::vmulesh;
        case 842: return Op::vcfsx;     case 844: return Op::vspltish;  case 846: return Op::vupkhpx;
        case 896: return Op::vaddsws;   case 898: return Op::vminsw;    case 900: return Op::vsraw;
        case 906: return Op::vctuxs;    case 908: return Op::vspltisw;  case 970: return Op::vctsxs;
        case 974: return Op::vupklpx;   case 1024: return Op::vsububm;  case 1026: return Op::vavgub;
        case 1028: return Op::vand;     case 1034: return Op::vmaxfp;   case 1036: return Op::vslo;
        case 1088: return Op::vsubuhm;  case 1090: return Op::vavguh;   case 1092: return Op::vandc;
        case 1098: return Op::vminfp;   case 1100: return Op::vsro;     case 1152: return Op::vsubuwm;
        case 1154: return Op::vavguw;   case 1156: return Op::vor;      case 1220: return Op::vxor;
        case 1282: return Op::vavgsb;   case 1284: return Op::vnor;     case 1346: return Op::vavgsh;
        case 1408: return Op::vsubcuw;  case 1410: return Op::vavgsw;   case 1536: return Op::vsububs;
        case 1540: return Op::mfvscr;   case 1544: return Op::vsum4ubs; case 1600: return Op::vsubuhs;
        case 1604: return Op::mtvscr;   case 1608: return Op::vsum4shs; case 1664: return Op::vsubuws;
        case 1672: return Op::vsum2sws; case 1792: return Op::vsubsbs;  case 1800: return Op::vsum4sbs;
        case 1856: return Op::vsubshs;  case 1920: return Op::vsubsws;  case 1928: return Op::vsumsws;
        default: return Op::invalid;
    }
}

Op decode_19(uint32_t w) {
    switch ((w >> 1) & 0x3FF) {
        case 0: return Op::mcrf;     case 16: return Op::bclr;    case 33: return Op::crnor;
        case 129: return Op::crandc; case 150: return Op::isync;  case 193: return Op::crxor;
        case 225: return Op::crnand; case 257: return Op::crand;  case 289: return Op::creqv;
        case 417: return Op::crorc;  case 449: return Op::cror;   case 528: return Op::bcctr;
        default: return Op::invalid;
    }
}

Op decode_30(uint32_t w) {
    switch ((w >> 2) & 7) {
        case 0: return Op::rldicl;
        case 1: return Op::rldicr;
        case 2: return Op::rldic;
        case 3: return Op::rldimi;
        case 4:
            if (((w >> 1) & 15) == 8) return Op::rldcl;
            if (((w >> 1) & 15) == 9) return Op::rldcr;
            return Op::invalid;
        default: return Op::invalid;
    }
}

Op decode_31(uint32_t w) {
    // XO-form arithmetic: 9-bit extended opcode, bit 21 is OE.
    switch ((w >> 1) & 0x1FF) {
        case 8: return Op::subfc;    case 10: return Op::addc;    case 40: return Op::subf;
        case 104: return Op::neg;    case 136: return Op::subfe;  case 138: return Op::adde;
        case 200: return Op::subfze; case 202: return Op::addze;  case 232: return Op::subfme;
        case 234: return Op::addme;  case 233: return Op::mulld;  case 235: return Op::mullw;
        case 266: return Op::add;    case 457: return Op::divdu;  case 459: return Op::divwu;
        case 489: return Op::divd;   case 491: return Op::divw;
        // high-word multiplies have no OE: bit 21 must be 0
        case 9: case 11: case 73: case 75:
            if ((w >> 10) & 1) return Op::invalid;
            switch ((w >> 1) & 0x1FF) {
                case 9: return Op::mulhdu;
                case 11: return Op::mulhwu;
                case 73: return Op::mulhd;
                default: return Op::mulhw;
            }
        default: break;
    }
    const uint32_t xo = (w >> 1) & 0x3FF;
    if ((xo >> 1) == 413) return Op::sradi;  // XS-form: bit 30 is sh[5]
    switch (xo) {
        case 0: return Op::cmp;       case 4: return Op::tw;        case 6: return Op::lvsl;
        case 7: return Op::lvebx;     case 20: return Op::lwarx;    case 21: return Op::ldx;
        case 23: return Op::lwzx;     case 24: return Op::slw;      case 26: return Op::cntlzw;
        case 27: return Op::sld;      case 28: return Op::and_;     case 32: return Op::cmpl;
        case 38: return Op::lvsr;     case 39: return Op::lvehx;    case 53: return Op::ldux;
        case 54: return Op::dcbst;    case 55: return Op::lwzux;    case 58: return Op::cntlzd;
        case 60: return Op::andc;     case 68: return Op::td;       case 71: return Op::lvewx;
        case 84: return Op::ldarx;    case 86: return Op::dcbf;     case 87: return Op::lbzx;
        case 103: return Op::lvx;     case 119: return Op::lbzux;   case 124: return Op::nor;
        case 135: return Op::stvebx;  case 149: return Op::stdx;    case 150: return Op::stwcx_;
        case 151: return Op::stwx;    case 167: return Op::stvehx;  case 181: return Op::stdux;
        case 183: return Op::stwux;   case 199: return Op::stvewx;  case 214: return Op::stdcx_;
        case 215: return Op::stbx;    case 231: return Op::stvx;    case 246: return Op::dcbtst;
        case 247: return Op::stbux;   case 278: return Op::dcbt;    case 279: return Op::lhzx;
        case 284: return Op::eqv;     case 311: return Op::lhzux;   case 316: return Op::xor_;
        case 339: return Op::mfspr;   case 341: return Op::lwax;    case 343: return Op::lhax;
        case 359: return Op::lvxl;    case 371: return Op::mftb;    case 373: return Op::lwaux;
        case 375: return Op::lhaux;   case 407: return Op::sthx;    case 412: return Op::orc;
        case 439: return Op::sthux;   case 444: return Op::or_;     case 467: return Op::mtspr;
        case 476: return Op::nand;    case 487: return Op::stvxl;   case 519: return Op::lvlx;
        case 534: return Op::lwbrx;   case 535: return Op::lfsx;    case 536: return Op::srw;
        case 539: return Op::srd;     case 551: return Op::lvrx;    case 567: return Op::lfsux;
        case 598: return Op::sync;    case 599: return Op::lfdx;    case 631: return Op::lfdux;
        case 647: return Op::stvlx;   case 662: return Op::stwbrx;  case 663: return Op::stfsx;
        case 679: return Op::stvrx;   case 695: return Op::stfsux;  case 727: return Op::stfdx;
        case 759: return Op::stfdux;  case 775: return Op::lvlxl;   case 790: return Op::lhbrx;
        case 792: return Op::sraw;    case 794: return Op::srad;    case 807: return Op::lvrxl;
        case 824: return Op::srawi;   case 854: return Op::eieio;   case 903: return Op::stvlxl;
        case 918: return Op::sthbrx;  case 922: return Op::extsh;   case 935: return Op::stvrxl;
        case 954: return Op::extsb;   case 982: return Op::icbi;    case 983: return Op::stfiwx;
        case 986: return Op::extsw;   case 1014: return Op::dcbz;
        case 19: return (w >> 20) & 1 ? Op::mfocrf : Op::mfcr;
        case 144: return (w >> 20) & 1 ? Op::mtocrf : Op::mtcrf;
        default: return Op::invalid;
    }
}

Op decode_59(uint32_t w) {
    switch ((w >> 1) & 31) {
        case 18: return Op::fdivs;   case 20: return Op::fsubs;   case 21: return Op::fadds;
        case 22: return Op::fsqrts;  case 24: return Op::fres;    case 25: return Op::fmuls;
        case 28: return Op::fmsubs;  case 29: return Op::fmadds;  case 30: return Op::fnmsubs;
        case 31: return Op::fnmadds;
        default: return Op::invalid;
    }
}

Op decode_63(uint32_t w) {
    switch ((w >> 1) & 31) {  // A-form first
        case 18: return Op::fdiv;    case 20: return Op::fsub;    case 21: return Op::fadd;
        case 22: return Op::fsqrt;   case 23: return Op::fsel;    case 25: return Op::fmul;
        case 26: return Op::frsqrte; case 28: return Op::fmsub;   case 29: return Op::fmadd;
        case 30: return Op::fnmsub;  case 31: return Op::fnmadd;
        default: break;
    }
    switch ((w >> 1) & 0x3FF) {
        case 0: return Op::fcmpu;    case 12: return Op::frsp;    case 14: return Op::fctiw;
        case 15: return Op::fctiwz;  case 32: return Op::fcmpo;   case 38: return Op::mtfsb1;
        case 40: return Op::fneg;    case 64: return Op::mcrfs;   case 70: return Op::mtfsb0;
        case 72: return Op::fmr;     case 134: return Op::mtfsfi; case 136: return Op::fnabs;
        case 264: return Op::fabs;   case 583: return Op::mffs;   case 711: return Op::mtfsf;
        case 814: return Op::fctid;  case 815: return Op::fctidz; case 846: return Op::fcfid;
        default: return Op::invalid;
    }
}

}  // namespace

const char* mnemonic(Op op) { return kOps[static_cast<size_t>(op)].name; }

const char* enum_name(Op op) {
    static constexpr const char* kEnumNames[] = {
        "invalid",
#define X(e, name, f) #e,
#include "ppu_ops.inc"
#undef X
    };
    return kEnumNames[static_cast<size_t>(op)];
}
Form form(Op op) { return kOps[static_cast<size_t>(op)].form; }

Insn decode(uint32_t w) {
    Insn in;
    in.w = w;
    const uint32_t opcd = w >> 26;
    switch (opcd) {
        case 0: in.op = ((w >> 1) & 0x3FF) == 256 ? Op::attn : Op::invalid; break;  // Cell attn: other bits ignored
        case 4: in.op = decode_vmx(w); break;
        case 17: in.op = (w & 2) ? Op::sc : Op::invalid; break;
        case 19: in.op = decode_19(w); break;
        case 30: in.op = decode_30(w); break;
        case 31: in.op = decode_31(w); break;
        case 58: in.op = (w & 3) == 0 ? Op::ld : (w & 3) == 1 ? Op::ldu : (w & 3) == 2 ? Op::lwa : Op::invalid; break;
        case 59: in.op = decode_59(w); break;
        case 62: in.op = (w & 3) == 0 ? Op::std : (w & 3) == 1 ? Op::stdu : Op::invalid; break;
        case 63: in.op = decode_63(w); break;
        default: in.op = primary_d(opcd); break;
    }
    return in;
}

}  // namespace sly4::ppu
