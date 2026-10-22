// RSX vertex programs (NV40 VP) -> SPIR-V. Field layout as in RPCS3's RSXVertexProgram.h
// (checked against the game's own programs: see docs/GPU.md, 5d).
#include <cstdio>
#include <cstring>

#include "gpu/rsx_shader_util.h"
#include "gpu/rsx_shaders.h"

namespace sly4::rsx {

namespace {

using spv::Id;

constexpr uint32_t kVpSlots = 512;  // transform program memory, in instructions

struct Insn {
    uint32_t d0, d1, d2, d3;
    // D0
    uint32_t cond_swz(int i) const { return (d0 >> (8 - 2 * i)) & 3; }  // x at bits 8-9 ... w at 2-3
    uint32_t addr_swz() const { return d0 & 3; }
    uint32_t cond() const { return (d0 >> 10) & 7; }
    bool cond_test() const { return (d0 >> 13) & 1; }
    bool cond_update0() const { return (d0 >> 14) & 1; }
    uint32_t dst_tmp() const { return (d0 >> 15) & 0x3F; }
    bool src_abs(int i) const { return (d0 >> (21 + i)) & 1; }
    uint32_t addr_reg() const { return (d0 >> 24) & 1; }
    uint32_t cond_reg() const { return (d0 >> 25) & 1; }
    bool saturate() const { return (d0 >> 26) & 1; }
    bool cond_update1() const { return (d0 >> 29) & 1; }
    bool vec_result() const { return (d0 >> 30) & 1; }
    // D1
    uint32_t input_src() const { return (d1 >> 8) & 0xF; }
    uint32_t const_src() const { return (d1 >> 12) & 0x3FF; }
    uint32_t vec_op() const { return (d1 >> 22) & 0x1F; }
    uint32_t sca_op() const { return (d1 >> 27) & 0x1F; }
    // D3
    bool end() const { return d3 & 1; }
    bool index_const() const { return (d3 >> 1) & 1; }
    uint32_t dst() const { return (d3 >> 2) & 0x1F; }
    uint32_t sca_dst_tmp() const { return (d3 >> 7) & 0x3F; }
    bool vec_mask(int i) const { return (d3 >> (16 - i)) & 1; }  // x bit 16 ... w bit 13
    bool sca_mask(int i) const { return (d3 >> (20 - i)) & 1; }  // x bit 20 ... w bit 17
    // Sources (17 bits each).
    uint32_t src(int i) const {
        switch (i) {
            case 0: return (d2 >> 23) | ((d1 & 0xFF) << 9);
            case 1: return (d2 >> 6) & 0x1FFFF;
            default: return (d3 >> 21) | ((d2 & 0x3F) << 11);
        }
    }
};

enum VecOp : uint32_t {
    V_NOP = 0, V_MOV, V_MUL, V_ADD, V_MAD, V_DP3, V_DPH, V_DP4, V_DST, V_MIN, V_MAX, V_SLT, V_SGE, V_ARL, V_FRC,
    V_FLR, V_SEQ, V_SFL, V_SGT, V_SLE, V_SNE, V_STR, V_SSG, V_TXL = 0x19,
};
enum ScaOp : uint32_t {
    S_NOP = 0, S_MOV, S_RCP, S_RCC, S_RSQ, S_EXP, S_LOG, S_LIT, S_BRA, S_BRI, S_CAL, S_CLI, S_RET, S_LG2, S_EX2,
    S_SIN, S_COS, S_BRB, S_CLB, S_PSH, S_POP,
};

Insn fetch(const uint32_t* program, uint32_t i) {
    const uint32_t* p = program + 4 * (i % kVpSlots);
    return {p[0], p[1], p[2], p[3]};
}

}  // namespace

VertexProgramInfo scan_vertex_program(const uint32_t* program, uint32_t start) {
    VertexProgramInfo info;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = start; i < start + kVpSlots; ++i) {
        const Insn in = fetch(program, i);
        h = fnv1a64(&in, sizeof(in), h);
        ++info.instructions;
        for (int s = 0; s < 3; ++s)
            if ((in.src(s) & 3) == 2) info.inputs |= 1u << in.input_src();
        if (in.end()) break;
    }
    info.hash = h;
    return info;
}

ShaderResult translate_vertex_program(const uint32_t* program, uint32_t start) {
    ShaderResult r;
    spv::Builder b;
    Vec v(b);
    using namespace shader_if;

    // Uniforms.
    const Id arr = b.t_array(v.v4, kVsConstants);
    b.decorate(arr, spv::ArrayStride, {16});
    const Id ubo_t = b.t_struct({arr, v.v4, v.v4, v.v4});
    b.decorate(ubo_t, spv::Block);
    b.member_decorate(ubo_t, 0, spv::Offset, {0});
    b.member_decorate(ubo_t, 1, spv::Offset, {kVsConstants * 16});
    b.member_decorate(ubo_t, 2, spv::Offset, {kVsConstants * 16 + 16});
    b.member_decorate(ubo_t, 3, spv::Offset, {kVsConstants * 16 + 32});
    const Id ubo = b.variable(b.t_pointer(spv::Uniform, ubo_t), spv::Uniform);
    b.decorate(ubo, spv::DescriptorSet, {0});
    b.decorate(ubo, spv::Binding, {0});
    const Id pu4 = b.t_pointer(spv::Uniform, v.v4);

    // Inputs (the ones the program reads) and outputs.
    const VertexProgramInfo info = scan_vertex_program(program, start);
    std::vector<Id> interface;
    Id inputs[16] = {};
    const Id pi4 = b.t_pointer(spv::Input, v.v4);
    for (uint32_t i = 0; i < 16; ++i)
        if (info.inputs & (1u << i)) {
            inputs[i] = b.variable(pi4, spv::Input);
            b.decorate(inputs[i], spv::Location, {i});
            interface.push_back(inputs[i]);
        }
    const Id po4 = b.t_pointer(spv::Output, v.v4);
    const Id position = b.variable(po4, spv::Output);
    b.decorate(position, spv::BuiltIn, {spv::Position});
    interface.push_back(position);
    // VP output registers: 0 HPOS, 1 COL0, 2 COL1, 3 BFC0, 4 BFC1, 5 FOG, 6 PSZ, 7-14 TEX0-7,
    // 15 TEX8 (here TEX0-9 continue at 4 + n).
    Id varyings[16] = {};
    auto varying_location = [](uint32_t o) -> int {
        if (o == 1) return int(kCol0);
        if (o == 2) return int(kCol1);
        if (o == 5) return int(kFog);
        if (o >= 7 && o <= 15) return int(kTex0 + (o - 7));
        return -1;
    };
    for (uint32_t o = 1; o < 16; ++o) {
        const int loc = varying_location(o);
        if (loc < 0) continue;
        varyings[o] = b.variable(po4, spv::Output);
        b.decorate(varyings[o], spv::Location, {uint32_t(loc)});
        interface.push_back(varyings[o]);
    }

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    // Registers: temps r0-r63, outputs o0-o15, condition codes cc0/cc1, address regs a0/a1.
    Id temps[64], outs[16], cc[2], addr[2];
    for (Id& t : temps) t = b.local(v.pf4);
    for (Id& o : outs) o = b.local(v.pf4);
    for (Id& c : cc) c = b.local(v.pf4);
    for (Id& a : addr) a = b.local(v.pf4);  // kept as floats, floored on use
    for (Id t : temps) b.op_void(spv::OpStore, {t, v.zero()});
    for (Id o : outs) b.op_void(spv::OpStore, {o, b.c_vec4(0, 0, 0, 1)});
    for (Id c : cc) b.op_void(spv::OpStore, {c, v.zero()});
    for (Id a : addr) b.op_void(spv::OpStore, {a, v.zero()});

    auto constant = [&](const Insn& in) -> Id {
        Id index = b.c_int(int32_t(in.const_src()));
        if (in.index_const()) {
            const Id a = b.op(spv::OpLoad, v.v4, {addr[in.addr_reg()]});
            const Id ai = b.op(spv::OpConvertFToS, v.i32, {v.comp(a, in.addr_swz())});
            index = b.op(spv::OpIAdd, v.i32, {index, ai});
        }
        return b.op(spv::OpLoad, v.v4, {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(0), index})});
    };
    auto source = [&](const Insn& in, int s) -> Id {
        const uint32_t w = in.src(s);
        Id val;
        switch (w & 3) {
            case 1: val = b.op(spv::OpLoad, v.v4, {temps[(w >> 2) & 0x3F]}); break;
            case 2: val = inputs[in.input_src()] ? b.op(spv::OpLoad, v.v4, {inputs[in.input_src()]}) : v.zero(); break;
            case 3: val = constant(in); break;
            default: val = v.zero(); break;
        }
        val = v.swizzle(val, (w >> 14) & 3, (w >> 12) & 3, (w >> 10) & 3, (w >> 8) & 3);
        if (in.src_abs(s)) val = v.abs(val);
        if ((w >> 16) & 1) val = v.neg(val);
        return val;
    };

    for (uint32_t i = start; i < start + kVpSlots; ++i) {
        const Insn in = fetch(program, i);
        const uint32_t vop = in.vec_op(), sop = in.sca_op();
        Id vres = 0, sres = 0;
        auto s0 = [&] { return source(in, 0); };
        auto s1 = [&] { return source(in, 1); };
        auto s2 = [&] { return source(in, 2); };
        switch (vop) {
            case V_NOP: break;
            case V_MOV: vres = s0(); break;
            case V_MUL: vres = v.mul(s0(), s1()); break;
            case V_ADD: vres = v.add(s0(), s2()); break;
            case V_MAD: vres = v.add(v.mul(s0(), s1()), s2()); break;
            case V_DP3: vres = v.splat(v.dot3(s0(), s1())); break;
            case V_DPH: {
                const Id a = s0();
                vres = v.splat(v.dot(v.vec4(v.comp(a, 0), v.comp(a, 1), v.comp(a, 2), b.c_float(1)), s1()));
                break;
            }
            case V_DP4: vres = v.splat(v.dot(s0(), s1())); break;
            case V_DST: {
                const Id a = s0(), c = s1();
                vres = v.vec4(b.c_float(1), v.fmul(v.comp(a, 1), v.comp(c, 1)), v.comp(a, 2), v.comp(c, 3));
                break;
            }
            case V_MIN: vres = b.ext(v.v4, spv::FMin, {s0(), s1()}); break;
            case V_MAX: vres = b.ext(v.v4, spv::FMax, {s0(), s1()}); break;
            case V_SLT: vres = v.set_if(spv::OpFOrdLessThan, s0(), s1()); break;
            case V_SGE: vres = v.set_if(spv::OpFOrdGreaterThanEqual, s0(), s1()); break;
            case V_SEQ: vres = v.set_if(spv::OpFOrdEqual, s0(), s1()); break;
            case V_SGT: vres = v.set_if(spv::OpFOrdGreaterThan, s0(), s1()); break;
            case V_SLE: vres = v.set_if(spv::OpFOrdLessThanEqual, s0(), s1()); break;
            case V_SNE: vres = v.set_if(spv::OpFOrdNotEqual, s0(), s1()); break;
            case V_SFL: vres = v.zero(); break;
            case V_STR: vres = v.one(); break;
            case V_FRC: vres = b.ext(v.v4, spv::Fract, {s0()}); break;
            case V_FLR: vres = b.ext(v.v4, spv::Floor, {s0()}); break;
            case V_SSG: vres = b.ext(v.v4, spv::FSign, {s0()}); break;
            case V_ARL: vres = b.ext(v.v4, spv::Floor, {s0()}); break;
            default: {
                char e[64];
                std::snprintf(e, sizeof(e), "VP: operação vetorial 0x%X sem tradução (instrução %u)", vop, i - start);
                r.error = e;
                return r;
            }
        }
        switch (sop) {
            case S_NOP: break;
            case S_MOV: sres = v.comp(s2(), 0); break;
            case S_RCP: sres = v.fdiv(b.c_float(1), v.comp(s2(), 0)); break;
            case S_RCC: {
                const Id x = v.fdiv(b.c_float(1), v.comp(s2(), 0));
                sres = b.ext(v.f, spv::FClamp, {x, b.c_float(5.42101e-20f), b.c_float(1.884467e19f)});
                break;
            }
            case S_RSQ: sres = b.ext(v.f, spv::InverseSqrt, {b.ext(v.f, spv::FAbs, {v.comp(s2(), 0)})}); break;
            case S_EX2: sres = b.ext(v.f, spv::Exp2, {v.comp(s2(), 0)}); break;
            case S_LG2: sres = b.ext(v.f, spv::Log2, {v.comp(s2(), 0)}); break;
            case S_SIN: sres = b.ext(v.f, spv::Sin, {v.comp(s2(), 0)}); break;
            case S_COS: sres = b.ext(v.f, spv::Cos, {v.comp(s2(), 0)}); break;
            case S_EXP: {
                const Id x = v.comp(s2(), 0);
                const Id fl = b.ext(v.f, spv::Floor, {x});
                sres = v.vec4(b.ext(v.f, spv::Exp2, {fl}), b.op(spv::OpFSub, v.f, {x, fl}), b.ext(v.f, spv::Exp2, {x}),
                              b.c_float(1));
                break;
            }
            case S_LOG: {
                const Id x = b.ext(v.f, spv::FAbs, {v.comp(s2(), 0)});
                const Id l = b.ext(v.f, spv::Log2, {x});
                const Id fl = b.ext(v.f, spv::Floor, {l});
                sres = v.vec4(fl, v.fdiv(x, b.ext(v.f, spv::Exp2, {fl})), l, b.c_float(1));
                break;
            }
            case S_LIT: {
                const Id s = s2();
                const Id x = b.ext(v.f, spv::FMax, {v.comp(s, 0), b.c_float(0)});
                const Id y = b.ext(v.f, spv::FMax, {v.comp(s, 1), b.c_float(0)});
                const Id w = b.ext(v.f, spv::FClamp, {v.comp(s, 3), b.c_float(-128), b.c_float(128)});
                const Id lit = b.op(spv::OpFOrdGreaterThan, v.bl, {v.comp(s, 0), b.c_float(0)});
                const Id z = b.op(spv::OpSelect, v.f, {lit, b.ext(v.f, spv::Pow, {y, w}), b.c_float(0)});
                sres = v.vec4(b.c_float(1), x, z, b.c_float(1));
                break;
            }
            default: {
                char e[64];
                std::snprintf(e, sizeof(e), "VP: operação escalar 0x%X sem tradução (instrução %u)", sop, i - start);
                r.error = e;
                return r;
            }
        }
        // Scalar results are replicated (EXP/LOG/LIT already give a vector).
        Id svec = 0;
        if (sres) svec = (sop == S_EXP || sop == S_LOG || sop == S_LIT) ? sres : v.splat(sres);
        if (in.saturate()) {
            if (vres) vres = v.clamp01(vres);
            if (svec) svec = v.clamp01(svec);
        }
        // Conditional writes.
        Id cond = v.bmask(true, true, true, true);
        if (in.cond_test()) {
            const Id c = b.op(spv::OpLoad, v.v4, {cc[in.cond_reg()]});
            cond = v.cond_test(in.cond(), v.swizzle(c, in.cond_swz(0), in.cond_swz(1), in.cond_swz(2), in.cond_swz(3)));
        }
        auto write = [&](Id var, Id value, bool x, bool y, bool z, bool w) {
            if (!(x || y || z || w)) return;
            v.store_masked(var, value, v.band(v.bmask(x, y, z, w), cond));
        };
        if (vres) {
            const bool m[4] = {in.vec_mask(0), in.vec_mask(1), in.vec_mask(2), in.vec_mask(3)};
            if (vop == V_ARL) {
                write(addr[in.addr_reg()], vres, m[0], m[1], m[2], m[3]);
            } else {
                if (in.dst_tmp() != 0x3F) write(temps[in.dst_tmp()], vres, m[0], m[1], m[2], m[3]);
                if (in.vec_result() && in.dst() != 0x1F) write(outs[in.dst() & 15], vres, m[0], m[1], m[2], m[3]);
                if (in.cond_update0() && in.cond_update1()) b.op_void(spv::OpStore, {cc[in.cond_reg()], vres});
            }
        }
        if (svec) {
            const bool m[4] = {in.sca_mask(0), in.sca_mask(1), in.sca_mask(2), in.sca_mask(3)};
            if (in.sca_dst_tmp() != 0x3F) write(temps[in.sca_dst_tmp()], svec, m[0], m[1], m[2], m[3]);
            if (!in.vec_result() && in.dst() != 0x1F) write(outs[in.dst() & 15], svec, m[0], m[1], m[2], m[3]);
        }
        if (in.end()) break;
    }

    // Outputs. The RSX viewport transform (window = clip * scale + offset * w, in pixels,
    // origin top left) is done here; the Vulkan viewport then covers the whole target.
    for (uint32_t o = 1; o < 16; ++o)
        if (varyings[o]) b.op_void(spv::OpStore, {varyings[o], b.op(spv::OpLoad, v.v4, {outs[o]})});
    const Id pos = b.op(spv::OpLoad, v.v4, {outs[0]});
    const Id scale = b.op(spv::OpLoad, v.v4, {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(1)})});
    const Id offset = b.op(spv::OpLoad, v.v4, {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(2)})});
    const Id target = b.op(spv::OpLoad, v.v4, {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(3)})});
    const Id w = v.comp(pos, 3);
    const Id window = v.add(v.mul(pos, scale), b.op(spv::OpVectorTimesScalar, v.v4, {offset, w}));
    // Vulkan clip: x = window.x * 2 / width - w (same for y), z = window.z, w = w.
    const Id x = b.op(spv::OpFSub, v.f, {v.fdiv(v.fmul(v.comp(window, 0), b.c_float(2)), v.comp(target, 0)), w});
    const Id y = b.op(spv::OpFSub, v.f, {v.fdiv(v.fmul(v.comp(window, 1), b.c_float(2)), v.comp(target, 1)), w});
    b.op_void(spv::OpStore, {position, v.vec4(x, y, v.comp(window, 2), w)});
    b.op_void(spv::OpReturn, {});
    b.end_function();
    b.entry_point(spv::Vertex, fn, "main", interface);
    r.spirv = b.finish();
    r.ok = true;
    return r;
}

}  // namespace sly4::rsx
