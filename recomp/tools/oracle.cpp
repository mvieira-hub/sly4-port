// Differential testing of the PPU interpreter against real PowerPC execution (QEMU).
//
//   sly4_recomp oracle-gen   --eboot EBOOT.elf --out-dir <dir> [--per-op 256] [--seed 1]
//   (docker) tools/ppc/oracle/run.sh builds <dir>/harness.S and runs it under qemu-ppc64
//   sly4_recomp oracle-check --dir <dir>
//
// oracle-gen takes one real instance of every testable instruction from the EBOOT,
// randomises its operand fields and the input register state, and writes cases.bin
// (one record per case) plus harness.S (load state; instruction; store state, per case).
// oracle-check runs the same cases through the interpreter and compares every register.
#include "oracle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "analysis/program.h"
#include "core/endian.h"
#include "core/log.h"
#include "cpu/ppu_interpreter.h"
#include "loader/elf_loader.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"

namespace sly4::oracle {

namespace {

using ppu::Form;
using ppu::Insn;
using ppu::Op;

// Record layout (big-endian, 16-byte aligned, shared with harness.S). kMem holds the
// 256-byte memory window that load/store cases address; harness_main.c maps it at
// kWindow and the checker commits the same guest address.
constexpr uint32_t kRec = 1344;
constexpr uint32_t kGpr = 0, kFpr = 256, kVr = 512, kCr = 1024, kXer = 1032, kFpscr = 1040, kCtr = 1048,
                   kVscr = 1056, kInsn = 1072, kMem = 1088, kMemSize = 256;
constexpr uint32_t kWindow = 0x20000000;

constexpr uint32_t kForbiddenGpr = (1u << 1) | (1u << 2) | (1u << 13) | (1u << 30) | (1u << 31);
constexpr uint32_t kHarnessGprs[] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                     23, 24, 25, 26, 27, 28, 29};

enum class K { Gpr, Fpr, Vr, Imm };
struct Field {
    uint8_t start, len;  // ISA bit numbering
    K kind;
};

void set_field(uint32_t& w, const Field& f, uint32_t v) {
    const uint32_t shift = 32 - f.start - f.len, mask = ((1u << f.len) - 1) << shift;
    w = (w & ~mask) | ((v << shift) & mask);
}
uint32_t get_field(uint32_t w, const Field& f) { return (w >> (32 - f.start - f.len)) & ((1u << f.len) - 1); }

constexpr Field RT{6, 5, K::Gpr}, RA{11, 5, K::Gpr}, RB{16, 5, K::Gpr};
constexpr Field FT{6, 5, K::Fpr}, FA{11, 5, K::Fpr}, FB{16, 5, K::Fpr}, FC{21, 5, K::Fpr};
constexpr Field VD{6, 5, K::Vr}, VA{11, 5, K::Vr}, VB{16, 5, K::Vr}, VC{21, 5, K::Vr};
constexpr Field IMM16{16, 16, K::Imm}, RC{31, 1, K::Imm}, OE{21, 1, K::Imm};

bool any_of_op(Op op, std::initializer_list<Op> ops) { return std::find(ops.begin(), ops.end(), op) != ops.end(); }

// Operand fields that may be randomised for each testable form; nullopt-like empty
// vector with ok=false means "not testable here" (memory, branches, system).
bool fields_for(Op op, std::vector<Field>& out) {
    out.clear();
    switch (ppu::form(op)) {
        case Form::D_RT_RA0_SI: case Form::D_RT_RA_SI: out = {RT, RA, IMM16}; return true;
        case Form::D_RA_RS_UI: out = {RT, RA, IMM16}; return true;
        case Form::D_CMPI: case Form::D_CMPLI: out = {{6, 3, K::Imm}, {10, 1, K::Imm}, RA, IMM16}; return true;
        case Form::M_IMM: out = {RT, RA, {16, 5, K::Imm}, {21, 5, K::Imm}, {26, 5, K::Imm}, RC}; return true;
        case Form::M_REG: out = {RT, RA, RB, {21, 5, K::Imm}, {26, 5, K::Imm}, RC}; return true;
        case Form::MD_IMM: out = {RT, RA, {16, 5, K::Imm}, {21, 6, K::Imm}, {30, 1, K::Imm}, RC}; return true;
        case Form::MDS_REG: out = {RT, RA, RB, {21, 6, K::Imm}, RC}; return true;
        case Form::X_CMP: out = {{6, 3, K::Imm}, {10, 1, K::Imm}, RA, RB}; return true;
        case Form::XO_3:
            out = {RT, RA, RB, RC};
            if (op != Op::mulhw && op != Op::mulhwu && op != Op::mulhd && op != Op::mulhdu) out.push_back(OE);
            return true;
        case Form::XO_2: out = {RT, RA, OE, RC}; return true;
        case Form::X_LOG3: out = {RT, RA, RB, RC}; return true;
        case Form::X_LOG2: out = {RT, RA, RC}; return true;
        case Form::X_SHIFTI: out = {RT, RA, {16, 5, K::Imm}, RC}; return true;
        case Form::XS_SHIFTI: out = {RT, RA, {16, 5, K::Imm}, {30, 1, K::Imm}, RC}; return true;
        case Form::X_MFSPR: if (op == Op::mftb) return false; out = {RT}; return true;
        case Form::X_MTSPR: out = {RT}; return true;
        case Form::X_MFCR: case Form::X_MFOCRF: out = {RT}; return true;
        case Form::X_MTCRF: out = {RT, {12, 8, K::Imm}}; return true;
        case Form::XL_CR3: out = {{6, 5, K::Imm}, {11, 5, K::Imm}, {16, 5, K::Imm}}; return true;
        case Form::XL_MCRF: out = {{6, 3, K::Imm}, {11, 3, K::Imm}}; return true;
        case Form::A_FRT_FRA_FRB: out = {FT, FA, FB, RC}; return true;
        case Form::A_FRT_FRA_FRC: out = {FT, FA, FC, RC}; return true;
        case Form::A_FRT_FRA_FRC_FRB: out = {FT, FA, FB, FC, RC}; return true;
        case Form::A_FRT_FRB: case Form::A_FRSQRTE: case Form::X_FRT_FRB: out = {FT, FB, RC}; return true;
        case Form::X_FCMP: out = {{6, 3, K::Imm}, FA, FB}; return true;
        case Form::X_MFFS: out = {FT, RC}; return true;
        case Form::X_MTFSF: out = {{7, 8, K::Imm}, FB, RC}; return true;
        case Form::X_MTFSB: out = {{6, 5, K::Imm}, RC}; return true;
        case Form::X_MTFSFI: out = {{6, 3, K::Imm}, {16, 4, K::Imm}, RC}; return true;
        case Form::VA_ABC: case Form::VA_ACB: out = {VD, VA, VB, VC}; return true;
        case Form::VA_SLDOI: out = {VD, VA, VB, {22, 4, K::Imm}}; return true;
        case Form::VC: out = {VD, VA, VB, {21, 1, K::Imm}}; return true;
        case Form::VX_ABD: out = {VD, VA, VB}; return true;
        case Form::VX_BD: out = {VD, VB}; return true;
        case Form::VX_UIMM: {
            const uint8_t bits = op == Op::vspltb ? 4 : op == Op::vsplth ? 3 : op == Op::vspltw ? 2 : 5;
            out = {VD, VB, {uint8_t(16 - bits), bits, K::Imm}};
            return true;
        }
        case Form::VX_SIMM: out = {VD, {11, 5, K::Imm}}; return true;
        case Form::VX_D: out = {VD}; return true;
        case Form::VX_B: out = {VB}; return true;
        // Memory: the base register is pointed into the window by the generator. Not here:
        // lmw/stmw (they reach r30/r31, the harness pointers), reservations, and the Cell
        // lvlx/lvrx family, which the 970 does not have (covered by unit tests instead).
        case Form::D_MEM_R:
            if (op == Op::lmw || op == Op::stmw) return false;
            out = {RT, RA, IMM16};
            return true;
        case Form::DS_MEM_R: out = {RT, RA, {16, 14, K::Imm}}; return true;
        case Form::D_MEM_F: out = {FT, RA, IMM16}; return true;
        case Form::X_MEM_R:
            if (op == Op::stwcx_ || op == Op::stdcx_) return false;
            out = {RT, RA, RB};
            return true;
        case Form::X_MEM_F: out = {FT, RA, RB}; return true;
        case Form::X_MEM_V:
            if (any_of_op(op, {Op::lvlx, Op::lvrx, Op::lvlxl, Op::lvrxl, Op::stvlx, Op::stvrx, Op::stvlxl,
                               Op::stvrxl}))
                return false;
            out = {VD, RA, RB};
            return true;
        case Form::X_CACHE:
            if (op != Op::dcbz) return false;
            out = {RA, RB};
            return true;
        default: return false;
    }
}

bool is_memory(Op op) {
    switch (ppu::form(op)) {
        case Form::D_MEM_R: case Form::DS_MEM_R: case Form::D_MEM_F: case Form::X_MEM_R: case Form::X_MEM_F:
        case Form::X_CACHE: return true;
        case Form::X_MEM_V: return op != Op::lvsl && op != Op::lvsr;
        default: return false;
    }
}
bool is_update(Op op) {
    return any_of_op(op, {Op::lwzu, Op::lbzu, Op::lhzu, Op::lhau, Op::ldu, Op::lfsu, Op::lfdu, Op::stwu, Op::stbu,
                          Op::sthu, Op::stdu, Op::stfsu, Op::stfdu, Op::lwzux, Op::lbzux, Op::lhzux, Op::lhaux,
                          Op::lwaux, Op::ldux, Op::lfsux, Op::lfdux, Op::stwux, Op::stbux, Op::sthux, Op::stdux,
                          Op::stfsux, Op::stfdux});
}
bool is_load(Op op) {
    const char* n = ppu::mnemonic(op);
    return n[0] == 'l';
}

// ---------------------------------------------------------------- random values
struct Gen {
    std::mt19937_64 rng;
    explicit Gen(uint64_t seed) : rng(seed) {}
    uint64_t u64() { return rng(); }
    uint32_t below(uint32_t n) { return uint32_t(rng() % n); }

    uint64_t gpr() {
        switch (below(12)) {
            case 0: return 0;
            case 1: return 1;
            case 2: return ~0ull;
            case 3: return below(256);
            case 4: return 0x7FFFFFFF;
            case 5: return 0x80000000;
            case 6: return 0xFFFFFFFF;
            case 7: return 0x8000000000000000ull;
            case 8: return 0x7FFFFFFFFFFFFFFFull;
            case 9: return uint64_t(int64_t(int32_t(rng())));
            case 10: return rng() & 0xFFFFFFFF;
            default: return rng();
        }
    }
    uint64_t dbl_bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
    uint32_t flt_bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
    float any_float() {
        static const uint32_t specials[] = {0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001,
                                            0xFFC12345, 0x00000001, 0x807FFFFF, 0x00800000, 0x3F800000, 0xBF800000,
                                            0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000};
        float f;
        switch (below(6)) {
            case 0: { const uint32_t u = specials[below(16)]; std::memcpy(&f, &u, 4); return f; }
            case 1: return float(int32_t(below(2000)) - 1000);
            case 2: return float(int32_t(below(2000)) - 1000) / 64.0f;
            default: { const uint32_t u = uint32_t(rng()); std::memcpy(&f, &u, 4); return f; }
        }
    }
    uint64_t fpr() {
        static const uint64_t specials[] = {
            0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
            0x7FF8000000000000ull, 0x7FF4000000000001ull, 0xFFF8000000000123ull, 0x0000000000000001ull,
            0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
            0x7FEFFFFFFFFFFFFFull, 0x41DFFFFFFFC00000ull, 0xC1E0000000000000ull, 0x41E0000000000000ull,
            0x43E0000000000000ull, 0x3FE0000000000000ull, 0x380FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull};
        switch (below(8)) {
            case 0: case 1: return specials[below(20)];
            case 2: { const float f = any_float(); return dbl_bits(double(f)); }  // single-representable
            case 3: return dbl_bits(double(int64_t(below(4000)) - 2000) / 8.0);
            case 4: return dbl_bits(std::ldexp(double(int64_t(rng() >> 11)), int(below(120)) - 60));
            default: return rng();
        }
    }
    void vr(uint8_t out[16]) {
        switch (below(4)) {
            case 0: case 1:
                for (int i = 0; i < 4; ++i) {
                    const uint32_t u = flt_bits(any_float());
                    store_be<uint32_t>(out + 4 * i, u);
                }
                break;
            case 2:
                for (int i = 0; i < 8; ++i) {
                    static const uint16_t sp[] = {0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x00FF, 0xFF00, 0x7F7F};
                    store_be<uint16_t>(out + 2 * i, below(2) ? sp[below(8)] : uint16_t(rng()));
                }
                break;
            default:
                for (int i = 0; i < 16; ++i) out[i] = uint8_t(rng());
                break;
        }
    }
    uint32_t fpscr() {
        // enables and NI off (no traps); bit 20 (0x800) is reserved and always reads 0
        uint32_t f = uint32_t(rng()) & ~0xFCu & ~0x800u;
        if (below(2)) f &= ~(fpscr::FX | fpscr::OX | fpscr::UX | fpscr::ZX | fpscr::XX | fpscr::VX_ALL);
        f = (f & fpscr::VX_ALL) ? f | fpscr::VX : f & ~fpscr::VX;
        f &= ~fpscr::FEX;  // no enabled exceptions, so FEX is 0
        return f;
    }
};

uint32_t mutate(Gen& g, uint32_t sample, Op op, const std::vector<Field>& fields) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        uint32_t w = sample;
        for (const Field& f : fields) {
            uint32_t v;
            switch (f.kind) {
                case K::Gpr: v = kHarnessGprs[g.below(sizeof(kHarnessGprs) / 4)]; break;
                default: v = uint32_t(g.u64()); break;
            }
            set_field(w, f, v);
        }
        if (op == Op::mfspr || op == Op::mtspr) {  // only XER (1) and CTR (9)
            const uint32_t spr = g.below(2) ? 1 : 9;
            w = (w & ~(0x3FFu << 11)) | ((spr & 31) << 16) | ((spr >> 5) << 11);
        }
        if (op == Op::mfocrf || op == Op::mtocrf) {  // exactly one CR field
            w = (w & ~(0xFFu << 12)) | ((0x80u >> g.below(8)) << 12);
        }
        // Never enable FP exceptions (VE..XE, FPSCR bits 24-28) nor touch the FEX/VX
        // summaries: the game runs with exceptions off, and enabled ones would trap.
        if (op == Op::mtfsb1 || op == Op::mtfsb0) {
            const uint32_t bt = (w >> 21) & 31;
            if ((bt >= 24 && bt <= 29) || bt == 1 || bt == 2) continue;
        }
        if (op == Op::mtfsfi) {
            const uint32_t bf = (w >> 23) & 7, imm = (w >> 12) & 15;
            if ((bf == 6 && imm) || (bf == 7 && (imm & 0xC))) continue;
        }
        if (is_memory(op)) {
            // small displacement so the access stays inside the window (DS: multiple of 4)
            const int32_t disp = int32_t(g.below(97)) - 48;
            if (ppu::form(op) == Form::D_MEM_R || ppu::form(op) == Form::D_MEM_F) w = (w & ~0xFFFFu) | (disp & 0xFFFF);
            if (ppu::form(op) == Form::DS_MEM_R) w = (w & ~0xFFFCu) | (disp & 0xFFFC);
            const uint32_t rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31;
            if (ra == 0) continue;                                    // base register needed
            if (is_update(op) && is_load(op) && ra == rd) continue;   // invalid form
            const Form f = ppu::form(op);
            const bool indexed = f == Form::X_MEM_R || f == Form::X_MEM_F || f == Form::X_MEM_V || f == Form::X_CACHE;
            if (indexed && ra == rb) continue;                        // base and index must differ
            const bool gpr_src = f == Form::D_MEM_R || f == Form::DS_MEM_R || f == Form::X_MEM_R;
            if (gpr_src && !is_load(op) && (rd == ra || (indexed && rd == rb))) continue;  // keep the pointer intact
        }
        const Insn in = ppu::decode(w);
        if (in.op != op) continue;
        bool ok = true;
        for (const Field& f : fields)
            if (f.kind == K::Gpr && (kForbiddenGpr >> get_field(w, f)) & 1) ok = false;
        if (ok) return w;
    }
    return 0;
}

// ---------------------------------------------------------------- harness.S
void write_harness(const std::filesystem::path& path, const std::vector<uint32_t>& insns) {
    std::ofstream s(path);
    s << "# generated by sly4_recomp oracle-gen; do not edit\n"
         "    .machine ppc64\n    .machine altivec\n"
         "    .section \".opd\",\"aw\"\n    .align 3\n    .globl run_all\n"
         "run_all:\n    .quad .L.run_all,.TOC.@tocbase,0\n    .text\n    .align 4\n"
         ".L.run_all:\n    mflr 0\n    std 0,16(1)\n    stdu 1,-1024(1)\n";
    for (int r = 14; r <= 31; ++r) s << "    std " << r << "," << 112 + 8 * (r - 14) << "(1)\n";
    for (int f = 14; f <= 31; ++f) s << "    stfd " << f << "," << 256 + 8 * (f - 14) << "(1)\n";
    for (int v = 20; v <= 31; ++v) s << "    li 0," << 400 + 16 * (v - 20) << "\n    stvx " << v << ",1,0\n";
    s << "    mfcr 0\n    stw 0,600(1)\n    mfspr 0,256\n    stw 0,604(1)\n    li 0,-1\n    mtspr 256,0\n"
         "    mr 30,3\n    mr 31,4\n";
    for (uint32_t w : insns) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08x", w);
        s << "    bl .L.load_state\n    .long " << buf << "\n    bl .L.store_state\n    addi 30,30," << kRec
          << "\n    addi 31,31," << kRec << "\n";
    }
    s << "    li 0,0\n    std 0,608(1)\n    lfd 0,608(1)\n    mtfsf 255,0\n";
    for (int r = 14; r <= 31; ++r) s << "    ld " << r << "," << 112 + 8 * (r - 14) << "(1)\n";
    for (int f = 14; f <= 31; ++f) s << "    lfd " << f << "," << 256 + 8 * (f - 14) << "(1)\n";
    for (int v = 20; v <= 31; ++v) s << "    li 0," << 400 + 16 * (v - 20) << "\n    lvx " << v << ",1,0\n";
    s << "    lwz 0,600(1)\n    mtcrf 255,0\n    lwz 0,604(1)\n    mtspr 256,0\n"
         "    addi 1,1,1024\n    ld 0,16(1)\n    mtlr 0\n    blr\n";

    // load_state: the memory window and the special registers first (r0/r3/f0/v0 as
    // temporaries), then every register.
    s << ".L.load_state:\n    lis 3," << (kWindow >> 16) << "\n";
    for (uint32_t k = 0; k < kMemSize; k += 8) s << "    ld 0," << kMem + k << "(30)\n    std 0," << k << "(3)\n";
    s << "    lwz 0," << kCr << "(30)\n    mtcrf 255,0\n"
      << "    ld 0," << kXer << "(30)\n    mtspr 1,0\n"
      << "    ld 0," << kCtr << "(30)\n    mtspr 9,0\n"
      << "    lfd 0," << kFpscr << "(30)\n    mtfsf 255,0\n"
      << "    li 0," << kVscr << "\n    lvx 0,30,0\n    mtvscr 0\n";
    for (int v = 0; v < 32; ++v) s << "    li 0," << kVr + 16 * v << "\n    lvx " << v << ",30,0\n";
    for (int f = 0; f < 32; ++f) s << "    lfd " << f << "," << kFpr + 8 * f << "(30)\n";
    for (uint32_t r : kHarnessGprs)
        if (r) s << "    ld " << r << "," << kGpr + 8 * r << "(30)\n";
    s << "    ld 0," << kGpr << "(30)\n    blr\n";

    s << ".L.store_state:\n";
    for (uint32_t r : kHarnessGprs) s << "    std " << r << "," << kGpr + 8 * r << "(31)\n";
    for (int f = 0; f < 32; ++f) s << "    stfd " << f << "," << kFpr + 8 * f << "(31)\n";
    for (int v = 0; v < 32; ++v) s << "    li 0," << kVr + 16 * v << "\n    stvx " << v << ",31,0\n";
    s << "    mfcr 0\n    stw 0," << kCr << "(31)\n"
      << "    mfspr 0,1\n    std 0," << kXer << "(31)\n"
      << "    mfspr 0,9\n    std 0," << kCtr << "(31)\n"
      << "    mffs 0\n    stfd 0," << kFpscr << "(31)\n"
      << "    mfvscr 0\n    li 0," << kVscr << "\n    stvx 0,31,0\n"
      << "    lis 3," << (kWindow >> 16) << "\n";
    for (uint32_t k = 0; k < kMemSize; k += 8) s << "    ld 0," << k << "(3)\n    std 0," << kMem + k << "(31)\n";
    s << "    blr\n";
}

// ---------------------------------------------------------------- state <-> record
void to_context(const uint8_t* rec, PPUContext& c) {
    for (int i = 0; i < 32; ++i) c.gpr[i] = load_be<uint64_t>(rec + kGpr + 8 * i);
    for (int i = 0; i < 32; ++i) c.fpr[i] = load_be<uint64_t>(rec + kFpr + 8 * i);
    for (int i = 0; i < 32; ++i)
        for (int k = 0; k < 16; ++k) c.vr[i].b[15 - k] = rec[kVr + 16 * i + k];
    c.cr = load_be<uint32_t>(rec + kCr);
    c.set_xer(load_be<uint64_t>(rec + kXer));
    c.ctr = load_be<uint64_t>(rec + kCtr);
    c.fpscr = uint32_t(load_be<uint64_t>(rec + kFpscr));
    c.vscr = load_be<uint32_t>(rec + kVscr + 12) & (vscr::NJ | vscr::SAT);
}

bool any_of(Op op, std::initializer_list<Op> ops) { return std::find(ops.begin(), ops.end(), op) != ops.end(); }

bool close_rel(double x, double y, double tol) {
    if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
    if (std::isinf(x) || std::isinf(y) || x == 0 || y == 0) return x == y;
    return std::fabs(x - y) <= tol * std::fabs(y);
}

// Differences that are explained, not bugs: places where QEMU departs from the Power ISA
// / IBM hardware, results the ISA leaves undefined (the interpreter follows RPCS3, which
// was checked on real PS3s), and hardware estimates. Each one copies the interpreter's
// value into `want` for the affected field and is counted under its reason.
void accept_known(const PPUContext& in, const PPUContext& got, PPUContext& want, const Insn& insn,
                  std::map<std::string, uint32_t>& accepted) {
    const Op op = insn.op;
    const Form f = ppu::form(op);
    const bool fp_rc = f == Form::A_FRT_FRA_FRB || f == Form::A_FRT_FRA_FRC || f == Form::A_FRT_FRA_FRC_FRB ||
                       f == Form::A_FRT_FRB || f == Form::A_FRSQRTE || f == Form::X_FRT_FRB || f == Form::X_MFFS ||
                       f == Form::X_MTFSF || f == Form::X_MTFSB || f == Form::X_MTFSFI;
    const uint32_t kCr1 = 0x0F000000, kCr0 = 0xF0000000 & ~0x10000000;  // CR0 without SO
    const uint32_t kExc = fpscr::OX | fpscr::UX | fpscr::ZX | fpscr::XX | fpscr::VX_ALL;
    auto copy_cr = [&](uint32_t mask) { want.cr = (want.cr & ~mask) | (got.cr & mask); };
    auto copy_fpscr = [&](uint32_t mask) { want.fpscr = (want.fpscr & ~mask) | (got.fpscr & mask); };
    const uint32_t d = insn.rd();

    // QEMU sets FX on every exception; the ISA only on a 0 -> 1 transition of an exception bit.
    if ((want.fpscr & fpscr::FX) && !(in.fpscr & fpscr::FX) && !(got.fpscr & fpscr::FX) &&
        !(want.fpscr & ~in.fpscr & kExc)) {
        want.fpscr &= ~fpscr::FX;
        if (fp_rc && insn.rc_bit()) copy_cr(0x08000000);
        accepted["QEMU liga FX sem transicao 0->1 (ISA: so na transicao)"]++;
    }
    // fcmpu/fcmpo alter only FPCC/FX/VX bits; QEMU also clears FI.
    if (any_of(op, {Op::fcmpu, Op::fcmpo}) && ((want.fpscr ^ got.fpscr) & fpscr::FI)) {
        copy_fpscr(fpscr::FI);
        accepted["QEMU zera FI em fcmpu/fcmpo"]++;
    }
    // Single-precision results that are single denormals are classified as denormal
    // (as the IBM-derived Microwatt FPU does); QEMU classifies them with the double range.
    if (any_of(op, {Op::fadds, Op::fsubs, Op::fmuls, Op::fdivs, Op::fsqrts, Op::fmadds, Op::fmsubs, Op::fnmadds,
                    Op::fnmsubs, Op::fres, Op::frsp})) {
        const uint32_t cg = (got.fpscr >> 12) & 0x1F, cw = (want.fpscr >> 12) & 0x1F;
        if ((cg == 0x14 && cw == 0x04) || (cg == 0x18 && cw == 0x08)) {
            copy_fpscr(fpscr::FPRF);
            accepted["FPRF de denormal de precisao simples (QEMU usa a faixa double)"]++;
        }
    }
    // fnmadd/fnmsub round, then negate (IBM docs); QEMU negates before rounding, which
    // only matters under the directed rounding modes.
    if (any_of(op, {Op::fnmadd, Op::fnmadds, Op::fnmsub, Op::fnmsubs}) && (in.fpscr & 3) &&
        (want.fpr[d] != got.fpr[d] || want.fpscr != got.fpscr)) {
        want.fpr[d] = got.fpr[d];
        want.fpscr = got.fpscr;
        if (insn.rc_bit()) copy_cr(kCr1);
        accepted["fnmadd/fnmsub com arredondamento direcionado (QEMU nega antes de arredondar)"]++;
    }
    // Estimates: hardware returns an approximation (about 12 bits); compare with tolerance.
    if (any_of(op, {Op::fres, Op::frsqrte})) {
        if (close_rel(got.f(d), want.f(d), 1.0 / 4096)) {
            want.fpr[d] = got.fpr[d];
            copy_fpscr(fpscr::FX | fpscr::XX | fpscr::FI);
            if (insn.rc_bit()) copy_cr(kCr1);
            accepted["estimativa de ponto flutuante dentro de 2^-12"]++;
        }
    }
    if (any_of(op, {Op::vrefp, Op::vrsqrtefp, Op::vexptefp, Op::vlogefp})) {
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            float x, y;
            std::memcpy(&x, got.vr[d].b + 4 * k, 4);
            std::memcpy(&y, want.vr[d].b + 4 * k, 4);
            ok = ok && close_rel(x, y, 1.0 / 2048);
        }
        if (ok && std::memcmp(want.vr[d].b, got.vr[d].b, 16)) {
            want.vr[d] = got.vr[d];
            accepted["estimativa VMX dentro de 2^-11"]++;
        }
    }
    // Undefined by the ISA: high word of mulhw/mulhwu (and CR0 derived from it).
    if (any_of(op, {Op::mulhw, Op::mulhwu}) && uint32_t(want.gpr[d]) == uint32_t(got.gpr[d]) &&
        want.gpr[d] != got.gpr[d]) {
        want.gpr[d] = got.gpr[d];
        if (insn.rc_bit()) copy_cr(kCr0);
        accepted["palavra alta de mulhw/mulhwu (indefinida)"]++;
    }
    // Undefined by the ISA: quotient on division by zero / overflow.
    {
        const uint64_t a = in.gpr[insn.ra()], b = in.gpr[insn.rb()];
        bool undefined = false;
        if (op == Op::divw) undefined = int32_t(b) == 0 || (int32_t(a) == INT32_MIN && int32_t(b) == -1);
        if (op == Op::divwu) undefined = uint32_t(b) == 0;
        if (op == Op::divd) undefined = b == 0 || (int64_t(a) == INT64_MIN && int64_t(b) == -1);
        if (op == Op::divdu) undefined = b == 0;
        if (undefined && (want.gpr[d] != got.gpr[d] || ((want.cr ^ got.cr) & kCr0))) {
            want.gpr[d] = got.gpr[d];
            if (insn.rc_bit()) copy_cr(kCr0);
            accepted["quociente de divisao por zero/estouro (indefinido)"]++;
        }
    }
    // lvebx/lvehx/lvewx: only the addressed element is defined. The interpreter loads the
    // whole aligned quadword (as RPCS3 does); QEMU keeps the other elements.
    if (any_of(op, {Op::lvebx, Op::lvehx, Op::lvewx})) {
        const uint32_t ea = uint32_t((insn.ra() ? in.gpr[insn.ra()] : 0) + in.gpr[insn.rb()]);
        const uint32_t size = op == Op::lvebx ? 1 : op == Op::lvehx ? 2 : 4;
        const uint32_t first = (ea & 15) & ~(size - 1);  // guest byte index of the element
        bool same = true;
        for (uint32_t k = first; k < first + size; ++k) same = same && want.vr[d].b[15 - k] == got.vr[d].b[15 - k];
        if (same && std::memcmp(want.vr[d].b, got.vr[d].b, 16)) {
            want.vr[d] = got.vr[d];
            accepted["lvebx/lvehx/lvewx: elementos nao enderecados (indefinidos)"]++;
        }
    }
    // Undefined by the ISA: high word of fctiw/fctiwz, FPRF of every fcti*.
    if (any_of(op, {Op::fctiw, Op::fctiwz, Op::fctid, Op::fctidz})) {
        bool changed = false;
        if (any_of(op, {Op::fctiw, Op::fctiwz}) && uint32_t(want.fpr[d]) == uint32_t(got.fpr[d]) &&
            want.fpr[d] != got.fpr[d]) {
            want.fpr[d] = got.fpr[d];
            changed = true;
        }
        if ((want.fpscr ^ got.fpscr) & fpscr::FPRF) {
            copy_fpscr(fpscr::FPRF);
            changed = true;
        }
        if (changed) accepted["fcti*: palavra alta / FPRF (indefinidos)"]++;
    }
}

}  // namespace

int cmd_gen(int argc, char** argv) {
    std::string eboot, out_dir;
    uint32_t per_op = 256;
    uint64_t seed = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--eboot")) eboot = argv[i + 1];
        else if (!std::strcmp(argv[i], "--out-dir")) out_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--per-op")) per_op = std::strtoul(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 0);
    }
    if (eboot.empty() || out_dir.empty()) {
        std::fprintf(stderr, "uso: sly4_recomp oracle-gen --eboot EBOOT.elf --out-dir pasta [--per-op 256] [--seed 1]\n");
        return 2;
    }
    GuestMemory mem;
    LoadedImage img;
    std::string error;
    if (!mem.reserve() || !load_elf(eboot, mem, img, error)) {
        SLY4_ERROR("%s", error.c_str());
        return 3;
    }
    // One real instance of every testable instruction, taken only from words the flow
    // analysis proved to be code (data in .text can decode with reserved fields set).
    const recomp::Program prog = recomp::analyze(mem, img);
    std::map<Op, uint32_t> samples;
    std::vector<Field> fields;
    for (const auto& s : img.sections) {
        if (!s.executable()) continue;
        for (uint32_t a = s.addr; a + 4 <= s.addr + s.size; a += 4) {
            if (prog.class_of(a) != 1) continue;
            const uint32_t w = mem.read<uint32_t>(a);
            const Op op = ppu::decode(w).op;
            if (op != Op::invalid && !samples.count(op) && fields_for(op, fields)) samples[op] = w;
        }
    }
    Gen g(seed);
    std::vector<uint32_t> insns;
    std::vector<uint8_t> records;
    for (const auto& [op, sample] : samples) {
        fields_for(op, fields);
        for (uint32_t k = 0; k < per_op; ++k) {
            const uint32_t w = mutate(g, sample, op, fields);
            if (!w) break;
            std::vector<uint8_t> rec(kRec, 0);
            for (int i = 0; i < 32; ++i) store_be<uint64_t>(&rec[kGpr + 8 * i], g.gpr());
            for (int i = 0; i < 32; ++i) store_be<uint64_t>(&rec[kFpr + 8 * i], g.fpr());
            for (int i = 0; i < 32; ++i) g.vr(&rec[kVr + 16 * i]);
            store_be<uint32_t>(&rec[kCr], uint32_t(g.u64()));
            store_be<uint64_t>(&rec[kXer], g.u64() & 0xE0000000ull);
            store_be<uint64_t>(&rec[kCtr], g.gpr());
            store_be<uint64_t>(&rec[kFpscr], g.fpscr());
            store_be<uint32_t>(&rec[kVscr + 12], (g.below(4) ? vscr::NJ : 0) | (g.below(2) ? vscr::SAT : 0));
            store_be<uint32_t>(&rec[kInsn], w);
            if (op == Op::mtfsf) {  // the source FPR must not carry enable bits (see mutate)
                const uint32_t fb = (w >> 11) & 31;
                const uint64_t vfb = load_be<uint64_t>(&rec[kFpr + 8 * fb]) & ~0xFCull;
                store_be<uint64_t>(&rec[kFpr + 8 * fb], vfb);
            }
            // memory window: random bytes with single-precision specials sprinkled in
            for (uint32_t k = 0; k < kMemSize; k += 4)
                store_be<uint32_t>(&rec[kMem + k], g.below(3) ? uint32_t(g.u64()) : g.flt_bits(g.any_float()));
            if (is_memory(op)) {
                const uint32_t ra = (w >> 16) & 31, rb = (w >> 11) & 31;
                store_be<uint64_t>(&rec[kGpr + 8 * ra], kWindow + 64 + g.below(64));
                const Form f = ppu::form(op);
                if (f == Form::X_MEM_R || f == Form::X_MEM_F || f == Form::X_MEM_V || f == Form::X_CACHE)
                    store_be<uint64_t>(&rec[kGpr + 8 * rb], uint64_t(int64_t(g.below(65)) - 32));
            }
            records.insert(records.end(), rec.begin(), rec.end());
            insns.push_back(w);
        }
    }
    std::filesystem::create_directories(out_dir);
    std::ofstream(std::filesystem::path(out_dir) / "cases.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(records.data()), std::streamsize(records.size()));
    write_harness(std::filesystem::path(out_dir) / "harness.S", insns);
    SLY4_INFO("%zu instruções testáveis, %zu casos gravados em %s", samples.size(), insns.size(), out_dir.c_str());
    return 0;
}

int cmd_check(int argc, char** argv) {
    std::string dir;
    int examples = 2;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--dir")) dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--examples")) examples = std::atoi(argv[i + 1]);
    }
    if (dir.empty()) {
        std::fprintf(stderr, "uso: sly4_recomp oracle-check --dir pasta [--examples 2]\n");
        return 2;
    }
    auto read_all = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::vector<uint8_t> in = read_all(std::filesystem::path(dir) / "cases.bin");
    const std::vector<uint8_t> out = read_all(std::filesystem::path(dir) / "results.bin");
    if (in.empty() || in.size() != out.size() || in.size() % kRec) {
        SLY4_ERROR("cases.bin/results.bin ausentes ou de tamanhos diferentes em %s", dir.c_str());
        return 3;
    }
    GuestMemory mem;
    mem.reserve();
    mem.commit(kWindow, 0x10000, Protection::ReadWrite);

    struct Stat {
        uint32_t cases = 0, bad = 0;
        std::map<std::string, uint32_t> parts;
        std::vector<std::string> ex;
    };
    std::map<Op, Stat> stats;
    std::map<std::string, uint32_t> accepted;
    const size_t n = in.size() / kRec;
    size_t bad_total = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* rin = &in[i * kRec];
        const uint8_t* rout = &out[i * kRec];
        const Insn insn = ppu::decode(load_be<uint32_t>(rin + kInsn));
        PPUContext got, want, input;
        to_context(rin, got);
        to_context(rin, input);
        to_context(rout, want);
        got.cia = 0x10000;
        mem.copy_to(kWindow, rin + kMem, kMemSize);
        const PpuStep st = ppu_execute(got, mem, insn);
        accept_known(input, got, want, insn, accepted);
        Stat& s = stats[insn.op];
        ++s.cases;
        std::vector<std::string> diffs;
        char buf[200];
        if (st != PpuStep::Ok) diffs.push_back("passo != Ok");
        for (uint32_t r : kHarnessGprs)
            if (got.gpr[r] != want.gpr[r]) {
                std::snprintf(buf, sizeof(buf), "r%u qemu=%016llx nosso=%016llx", r,
                              (unsigned long long)want.gpr[r], (unsigned long long)got.gpr[r]);
                diffs.push_back(buf);
            }
        for (int r = 0; r < 32; ++r)
            if (got.fpr[r] != want.fpr[r]) {
                std::snprintf(buf, sizeof(buf), "f%d qemu=%016llx nosso=%016llx", r,
                              (unsigned long long)want.fpr[r], (unsigned long long)got.fpr[r]);
                diffs.push_back(buf);
            }
        for (int r = 0; r < 32; ++r)
            if (std::memcmp(got.vr[r].b, want.vr[r].b, 16)) {
                std::string a, b;
                for (int k = 15; k >= 0; --k) {
                    std::snprintf(buf, sizeof(buf), "%02x", want.vr[r].b[k]);
                    a += buf;
                    std::snprintf(buf, sizeof(buf), "%02x", got.vr[r].b[k]);
                    b += buf;
                }
                diffs.push_back("v" + std::to_string(r) + " qemu=" + a + " nosso=" + b);
            }
        if (got.cr != want.cr) {
            std::snprintf(buf, sizeof(buf), "cr qemu=%08x nosso=%08x", want.cr, got.cr);
            diffs.push_back(buf);
        }
        if ((got.xer() & 0xE0000000) != (want.xer() & 0xE0000000)) {
            std::snprintf(buf, sizeof(buf), "xer qemu=%08llx nosso=%08llx", (unsigned long long)want.xer(),
                          (unsigned long long)got.xer());
            diffs.push_back(buf);
        }
        if (got.ctr != want.ctr) diffs.push_back("ctr");
        // dcbz: the Cell zeroes its 128-byte cache line; QEMU's 970 runs in the 32-byte
        // compatibility mode. Accept when QEMU's zeroed block lies inside ours and the rest
        // of the window is untouched on both sides.
        bool mem_accepted = false;
        if (insn.op == Op::dcbz) {
            const uint32_t ea = uint32_t((insn.ra() ? input.gpr[insn.ra()] : 0) + input.gpr[insn.rb()]) - kWindow;
            const uint32_t l32 = ea & ~31u, l128 = ea & ~127u;
            bool ok = true;
            for (uint32_t k = 0; k < kMemSize && ok; ++k) {
                const bool in32 = k >= l32 && k < l32 + 32, in128 = k >= l128 && k < l128 + 128;
                const uint8_t ours = mem.read<uint8_t>(kWindow + k), theirs = rout[kMem + k], orig = rin[kMem + k];
                ok = (in32 ? theirs == 0 : theirs == orig) && (in128 ? ours == 0 : ours == orig);
            }
            if (ok) {
                mem_accepted = true;
                accepted["dcbz: Cell zera 128 bytes, o 970 do QEMU 32"]++;
            }
        }
        for (uint32_t k = 0; k < kMemSize && !mem_accepted; ++k) {
            const uint8_t ours = mem.read<uint8_t>(kWindow + k), theirs = rout[kMem + k];
            if (ours != theirs) {
                std::snprintf(buf, sizeof(buf), "mem +0x%02x qemu=%02x nosso=%02x", k, theirs, ours);
                diffs.push_back(buf);
                break;
            }
        }
        // FPSCR[FR] is not modelled by QEMU (it keeps or clears it arbitrarily), so it is
        // not compared; the interpreter computes it as the hardware does.
        if ((got.fpscr & ~fpscr::FR) != (want.fpscr & ~fpscr::FR)) {
            std::snprintf(buf, sizeof(buf), "fpscr qemu=%08x nosso=%08x", want.fpscr, got.fpscr);
            diffs.push_back(buf);
        }
        if (got.vscr != want.vscr) {
            std::snprintf(buf, sizeof(buf), "vscr qemu=%08x nosso=%08x", want.vscr, got.vscr);
            diffs.push_back(buf);
        }
        if (diffs.empty()) continue;
        ++s.bad;
        ++bad_total;
        for (const auto& dd : diffs) {
            const std::string tok = dd.substr(0, dd.find(' '));
            const bool reg = tok.size() > 1 && std::isdigit(static_cast<unsigned char>(tok[1]));
            s.parts[reg && tok[0] == 'r' ? "gpr" : reg && tok[0] == 'f' ? "fpr" : reg && tok[0] == 'v' ? "vr" : tok]++;
        }
        if (int(s.ex.size()) < examples) {
            std::string e = "      " + ppu::disasm(insn, 0x10000) + ":";
            for (size_t k = 0; k < diffs.size() && k < 4; ++k) e += "\n        " + diffs[k];
            // inputs of the operands, for reproduction
            std::snprintf(buf, sizeof(buf), "\n        entrada: fpscr=%08x vscr=%08x cr=%08x xer=%08llx",
                          uint32_t(load_be<uint64_t>(rin + kFpscr)),
                          load_be<uint32_t>(rin + kVscr + 12), load_be<uint32_t>(rin + kCr),
                          (unsigned long long)load_be<uint64_t>(rin + kXer));
            e += buf;
            s.ex.push_back(e);
        }
    }
    size_t ops_bad = 0;
    for (const auto& [op, s] : stats) ops_bad += s.bad != 0;
    SLY4_INFO("%zu casos, %zu instruções; casos divergentes: %zu; instruções com divergência: %zu", n, stats.size(),
              bad_total, ops_bad);
    for (const auto& [why, count] : accepted) std::printf("  aceito (%u casos): %s\n", count, why.c_str());
    std::vector<std::pair<Op, const Stat*>> order;
    for (const auto& [op, s] : stats)
        if (s.bad) order.emplace_back(op, &s);
    std::sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.second->bad > y.second->bad; });
    for (const auto& [op, s] : order) {
        std::string parts;
        for (const auto& [k, v] : s->parts) parts += " " + k + "=" + std::to_string(v);
        std::printf("  %-10s %4u/%-4u%s\n", ppu::mnemonic(op), s->bad, s->cases, parts.c_str());
        for (const auto& e : s->ex) std::printf("%s\n", e.c_str());
    }
    std::fflush(stdout);
    return bad_total ? 1 : 0;
}

}  // namespace sly4::oracle
