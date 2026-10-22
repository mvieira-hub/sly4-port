#include "spu/spu_decoder.h"

#include <array>
#include <cstdio>

namespace sly4::spu {

namespace {

struct Info {
    const char* mnem;
    const char* enum_name;
    Form form;
    uint16_t opcode;
    uint8_t nargs;
    Arg args[4];
};

using enum Arg;
using enum Form;

template <class... A>
constexpr Info info(const char* mnem, const char* e, Form f, uint16_t opc, A... a) {
    return Info{mnem, e, f, opc, uint8_t(sizeof...(a)), {a...}};
}

const Info kInfo[] = {
    {"(invalid)", "invalid", RR, 0xFFFF, 0, {}},
#define X(e, mnem, form, opc, ...) info(mnem, #e, form, opc __VA_OPT__(, ) __VA_ARGS__),
#include "spu/spu_ops.inc"
#undef X
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == size_t(Op::count));

const Info& of(Op op) { return kInfo[size_t(op)]; }

// binutils' lookup: the longest opcode forms are tried first by masking the 11-bit
// prefix (RRR, then RI18/LBT, RI10, RI16, RI8) and only an entry of that form counts;
// otherwise the whole 11 bits select an RR/RI7/LBTI entry.
struct Table {
    std::array<Op, 2048> exact{};  // first table entry with this opcode, of any form
    std::array<Op, 2048> decode{};
    Table() {
        for (size_t i = size_t(Op::count) - 1; i >= 1; --i) exact[kInfo[i].opcode] = Op(i);  // first wins
        auto is = [&](uint32_t key, std::initializer_list<Form> forms) {
            const Op op = exact[key];
            if (op == Op::invalid) return Op::invalid;
            for (Form f : forms)
                if (of(op).form == f) return op;
            return Op::invalid;
        };
        for (uint32_t v = 0; v < 2048; ++v) {
            Op op;
            if ((op = is(v & 0x780, {RRR})) != Op::invalid ||
                (op = is(v & 0x7F0, {RI18, LBT})) != Op::invalid ||
                (op = is(v & 0x7F8, {RI10})) != Op::invalid || (op = is(v & 0x7FC, {RI16})) != Op::invalid ||
                (op = is(v & 0x7FE, {RI8})) != Op::invalid) {
                decode[v] = op;
            } else {
                decode[v] = exact[v];
            }
        }
    }
};

const Table& table() {
    static const Table t;
    return t;
}

}  // namespace

const char* mnemonic(Op op) { return of(op).mnem; }
const char* enum_name(Op op) { return of(op).enum_name; }
Form form(Op op) { return of(op).form; }

Insn decode(uint32_t w) { return Insn{table().decode[w >> 21], w}; }

bool may_leave(Op op) {
    switch (op) {
        case Op::br: case Op::bra: case Op::brsl: case Op::brasl: case Op::bi: case Op::bisl: case Op::iret:
        case Op::bisled: case Op::brz: case Op::brnz: case Op::brhz: case Op::brhnz: case Op::biz: case Op::binz:
        case Op::bihz: case Op::bihnz: case Op::stop: case Op::stopd: case Op::heq: case Op::heqi: case Op::hgt:
        case Op::hgti: case Op::hlgt: case Op::hlgti: case Op::rdch: case Op::wrch: case Op::invalid:
            return true;
        default:
            return false;
    }
}

bool ends_block(Op op) {
    switch (op) {
        case Op::br: case Op::bra: case Op::brsl: case Op::brasl: case Op::bi: case Op::bisl: case Op::iret:
        case Op::bisled: case Op::stop: case Op::stopd: case Op::invalid:
            return true;
        default:
            return false;
    }
}

bool direct_target(const Insn& in, uint32_t ip, uint32_t& target) {
    switch (in.op) {
        case Op::br: case Op::brsl: case Op::brz: case Op::brnz: case Op::brhz: case Op::brhnz:
            target = (ip + uint32_t(in.i16() * 4)) & 0x3FFFC;
            return true;
        case Op::bra: case Op::brasl:
            target = uint32_t(in.i16() * 4) & 0x3FFFC;
            return true;
        default:
            return false;
    }
}

std::string disasm(const Insn& in, uint32_t ip) {
    char buf[64];
    if (in.op == Op::invalid) {
        std::snprintf(buf, sizeof(buf), ".long 0x%x", in.w);
        return buf;
    }
    const Info& inf = of(in.op);
    std::string s = inf.mnem;
    switch (in.op) {  // flag suffixes of the indirect branches, sync and hbr
        case Op::bi: case Op::bisl: case Op::iret: case Op::bisled: case Op::bihnz:
        case Op::bihz: case Op::binz: case Op::biz: case Op::sync: case Op::hbr: {
            const uint32_t fb = in.fb();
            if (fb & 0x40) s += in.op == Op::sync ? "c" : "p";
            if (fb & 0x20) s += "d";
            if (fb & 0x10) s += "e";
            break;
        }
        default: break;
    }
    if (inf.nargs) s += '\t';
    int hex = 0;
    bool paren = false;
    auto addr = [&](uint32_t a) {
        std::snprintf(buf, sizeof(buf), "0x%x", a);
        s += buf;
    };
    for (int i = 0; i < inf.nargs; ++i) {
        const Arg arg = inf.args[i];
        if (arg != P && !paren && i > 0) s += ',';
        switch (arg) {
            case T: std::snprintf(buf, sizeof(buf), "$%u", in.rt()); s += buf; break;
            case A: std::snprintf(buf, sizeof(buf), "$%u", in.ra()); s += buf; break;
            case B: std::snprintf(buf, sizeof(buf), "$%u", in.rb()); s += buf; break;
            case C: std::snprintf(buf, sizeof(buf), "$%u", in.rc()); s += buf; break;
            case S: std::snprintf(buf, sizeof(buf), "$sp%u", in.ra()); s += buf; break;
            case H: std::snprintf(buf, sizeof(buf), "$ch%u", in.ra()); s += buf; break;
            case P: paren = true; s += '('; break;
            case U7A: std::snprintf(buf, sizeof(buf), "%d", 173 - int(in.u8())); s += buf; break;
            case U7B: std::snprintf(buf, sizeof(buf), "%d", 155 - int(in.u8())); s += buf; break;
            case I7: hex = in.i7(); std::snprintf(buf, sizeof(buf), "%d", hex); s += buf; break;
            case I10: hex = in.i10(); std::snprintf(buf, sizeof(buf), "%d", hex); s += buf; break;
            case S14: hex = in.i10() * 16; std::snprintf(buf, sizeof(buf), "%d", hex); s += buf; break;
            case S16: hex = in.i16(); std::snprintf(buf, sizeof(buf), "%d", hex); s += buf; break;
            case X16: hex = int(in.u16()); std::snprintf(buf, sizeof(buf), "%u", in.u16()); s += buf; break;
            case U18: hex = int(in.u18()); std::snprintf(buf, sizeof(buf), "%u", in.u18()); s += buf; break;
            case S11: addr(uint32_t(int64_t(ip) + in.i9a() * 4)); break;
            case S11I: addr(uint32_t(int64_t(ip) + in.i9b() * 4)); break;
            case R18: {
                const int v = in.i16() * 4;
                if (v == 0) {
                    s += '0';
                } else {
                    hex = int(ip) + v;
                    addr(uint32_t(hex) & 0x3FFFF);
                }
                break;
            }
            case S18: {
                const uint32_t v = in.u16() * 4;
                if (v == 0)
                    s += '0';
                else
                    addr(v);
                break;
            }
        }
        if (arg != P && paren) {
            s += ')';
            paren = false;
        }
    }
    if (hex > 16) {
        std::snprintf(buf, sizeof(buf), "\t# %x", hex);
        s += buf;
    }
    return s;
}

}  // namespace sly4::spu
