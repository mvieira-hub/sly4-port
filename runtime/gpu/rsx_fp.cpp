// RSX fragment programs (NV40 FP) -> SPIR-V. Field layout as in RPCS3's
// RSXFragmentProgram.h. Each 32-bit word is stored big-endian with its two halves
// swapped; a source of type "constant" takes the next 16 bytes as an inline vec4.
#include <cstdio>
#include <cstring>

#include "core/endian.h"
#include "gpu/rsx_shader_util.h"
#include "gpu/rsx_shaders.h"

namespace sly4::rsx {

namespace {

using spv::Id;

uint32_t word(const uint8_t* p) {
    const uint32_t w = load_be<uint32_t>(p);
    return (w << 16) | (w >> 16);
}

struct Insn {
    uint32_t dst, s0, s1, s2;
    bool end() const { return dst & 1; }
    uint32_t dest_reg() const { return (dst >> 1) & 0x3F; }
    bool dst_fp16() const { return (dst >> 7) & 1; }
    bool set_cond() const { return (dst >> 8) & 1; }
    bool mask(int i) const { return (dst >> (9 + i)) & 1; }
    uint32_t attr() const { return (dst >> 13) & 0xF; }
    uint32_t tex() const { return (dst >> 17) & 0xF; }
    uint32_t opcode() const { return ((dst >> 24) & 0x3F) | (((s1 >> 31) & 1) << 6); }
    bool no_dest() const { return (dst >> 30) & 1; }
    bool saturate() const { return (dst >> 31) & 1; }
    // Condition (in SRC0).
    bool exec_lt() const { return (s0 >> 18) & 1; }
    bool exec_eq() const { return (s0 >> 19) & 1; }
    bool exec_gt() const { return (s0 >> 20) & 1; }
    uint32_t cond_swz(int i) const { return (s0 >> (21 + 2 * i)) & 3; }
    uint32_t cond_mod_reg() const { return (s0 >> 30) & 1; }
    uint32_t cond_reg() const { return (s0 >> 31) & 1; }
    uint32_t scale() const { return (s1 >> 28) & 7; }
    uint32_t src(int i) const { return i == 0 ? s0 : i == 1 ? s1 : s2; }
    bool src_abs(int i) const { return i == 0 ? (s0 >> 29) & 1 : (src(i) >> 18) & 1; }
    bool uses_constant() const {
        for (int i = 0; i < 3; ++i)
            if ((src(i) & 3) == 2) return true;
        return false;
    }
};

Insn fetch(const uint8_t* p) { return {word(p), word(p + 4), word(p + 8), word(p + 12)}; }

enum FpOp : uint32_t {
    NOP = 0x00, MOV, MUL, ADD, MAD, DP3, DP4, DST, MIN, MAX, SLT, SGE, SLE, SGT, SNE, SEQ, FRC, FLR, KIL, PK4,
    UP4, DDX, DDY, TEX, TXP, TXD, RCP, RSQ, EX2, LG2, LIT, LRP, STR, SFL, COS, SIN, PK2, UP2, POW, PKB, UPB,
    PK16, UP16, BEM, PKG, UPG, DP2A, TXL, TXB = 0x31, TEXBEM = 0x33, TXPBEM, BEMLUM, REFL, TIMESWTEX, DP2,
    NRM, DIV, DIVSQ, LIF, FENCT, FENCB, BRK = 0x40, CAL, IFE, LOOP, REP, RET,
};

}  // namespace

FragmentProgramInfo scan_fragment_program(const uint8_t* code, uint32_t max) {
    FragmentProgramInfo info;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t at = 0;
    while (at + 16 <= max) {
        const Insn in = fetch(code + at);
        h = fnv1a64(&in, sizeof(in), h);
        const uint32_t op = in.opcode();
        if (op == TEX || op == TXP || op == TXD || op == TXL || op == TXB) info.textures |= 1u << in.tex();
        at += 16;
        ++info.instructions;
        if (in.uses_constant() && at + 16 <= max) {
            info.constant_offsets.push_back(at);
            at += 16;
            ++info.instructions;
        }
        if (in.end()) break;
    }
    info.size = at;
    info.hash = h;
    return info;
}

ShaderResult translate_fragment_program(const uint8_t* code, const FragmentProgramInfo& info,
                                        const FragmentProgramKey& key) {
    ShaderResult r;
    spv::Builder b;
    Vec v(b);
    using namespace shader_if;
    std::vector<Id> interface;

    // Uniforms: the inline constants (in order) and the alpha test reference.
    const uint32_t nconst = uint32_t(info.constant_offsets.size());
    const Id arr = b.t_array(v.v4, nconst ? nconst : 1);
    b.decorate(arr, spv::ArrayStride, {16});
    const Id ubo_t = b.t_struct({arr, v.v4});
    b.decorate(ubo_t, spv::Block);
    b.member_decorate(ubo_t, 0, spv::Offset, {0});
    b.member_decorate(ubo_t, 1, spv::Offset, {(nconst ? nconst : 1) * 16});
    const Id ubo = b.variable(b.t_pointer(spv::Uniform, ubo_t), spv::Uniform);
    b.decorate(ubo, spv::DescriptorSet, {0});
    b.decorate(ubo, spv::Binding, {kFsUniformBinding});
    const Id pu4 = b.t_pointer(spv::Uniform, v.v4);

    // Inputs (created when first read) and textures.
    const Id pi4 = b.t_pointer(spv::Input, v.v4);
    Id inputs[16] = {};
    Id frag_coord = 0, front_facing = 0;
    auto input = [&](uint32_t a) -> Id {
        if (a == 0) {
            if (!frag_coord) {
                frag_coord = b.variable(pi4, spv::Input);
                b.decorate(frag_coord, spv::BuiltIn, {spv::FragCoord});
                interface.push_back(frag_coord);
            }
            return b.op(spv::OpLoad, v.v4, {frag_coord});
        }
        if (a == 14) {  // facing: +1 front, -1 back
            if (!front_facing) {
                front_facing = b.variable(b.t_pointer(spv::Input, v.bl), spv::Input);
                b.decorate(front_facing, spv::BuiltIn, {spv::FrontFacing});
                interface.push_back(front_facing);
            }
            const Id f = b.op(spv::OpSelect, v.f, {b.op(spv::OpLoad, v.bl, {front_facing}), b.c_float(1), b.c_float(-1)});
            return v.splat(f);
        }
        if (a > 13) return v.zero();
        if (!inputs[a]) {
            inputs[a] = b.variable(pi4, spv::Input);
            const uint32_t loc = a == 1 ? kCol0 : a == 2 ? kCol1 : a == 3 ? kFog : kTex0 + (a - 4);
            b.decorate(inputs[a], spv::Location, {loc});
            interface.push_back(inputs[a]);
        }
        return b.op(spv::OpLoad, v.v4, {inputs[a]});
    };
    Id samplers[16] = {};
    Id sampler_types[16] = {};
    for (uint32_t t = 0; t < 16; ++t) {
        if (!(info.textures & (1u << t))) continue;
        const uint32_t d = key.texture_dims[t];
        const spv::Dim dim = d == 3 ? spv::Dim3D : d == 6 ? spv::DimCube : spv::Dim2D;
        sampler_types[t] = b.t_sampled_image(dim, (key.shadow >> t) & 1);
        samplers[t] = b.variable(b.t_pointer(spv::UniformConstant, sampler_types[t]), spv::UniformConstant);
        b.decorate(samplers[t], spv::DescriptorSet, {0});
        b.decorate(samplers[t], spv::Binding, {kTextureBinding0 + t});
    }

    // Outputs.
    const Id po4 = b.t_pointer(spv::Output, v.v4);
    const Id out_color = b.variable(po4, spv::Output);
    b.decorate(out_color, spv::Location, {0});
    interface.push_back(out_color);
    const bool depth_export = (key.shader_control & 0xE) != 0;
    Id out_depth = 0;
    if (depth_export) {
        out_depth = b.variable(b.t_pointer(spv::Output, v.f), spv::Output);
        b.decorate(out_depth, spv::BuiltIn, {spv::FragDepth});
        interface.push_back(out_depth);
    }

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    Id rregs[64], hregs[64], cc[2];
    for (Id& x : rregs) x = b.local(v.pf4);
    for (Id& x : hregs) x = b.local(v.pf4);
    for (Id& x : cc) x = b.local(v.pf4);
    for (Id x : rregs) b.op_void(spv::OpStore, {x, v.zero()});
    for (Id x : hregs) b.op_void(spv::OpStore, {x, v.zero()});
    for (Id x : cc) b.op_void(spv::OpStore, {x, v.zero()});

    // if (cond) discard; - OpKill ends its block, so it gets one of its own.
    auto discard_if = [&](Id cond) {
        const Id kill_l = b.fresh_id(), merge_l = b.fresh_id();
        b.op_void(spv::OpSelectionMerge, {merge_l, 0});
        b.op_void(spv::OpBranchConditional, {cond, kill_l, merge_l});
        b.op_void(spv::OpLabel, {kill_l});
        b.op_void(spv::OpKill, {});
        b.op_void(spv::OpLabel, {merge_l});
    };

    uint32_t at = 0, const_index = 0;
    while (at + 16 <= info.size) {
        const Insn in = fetch(code + at);
        at += 16;
        Id constant = 0;
        if (in.uses_constant()) {
            constant = b.op(spv::OpLoad, v.v4,
                            {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(0), b.c_int(int32_t(const_index++))})});
            at += 16;
        }
        auto source = [&](int i) -> Id {
            const uint32_t s = in.src(i);
            Id val;
            switch (s & 3) {
                case 0: {
                    const uint32_t reg = (s >> 2) & 0x3F;
                    val = b.op(spv::OpLoad, v.v4, {((s >> 8) & 1) ? hregs[reg] : rregs[reg]});
                    break;
                }
                case 1: val = input(in.attr()); break;
                case 2: val = constant; break;
                default: val = v.zero(); break;
            }
            val = v.swizzle(val, (s >> 9) & 3, (s >> 11) & 3, (s >> 13) & 3, (s >> 15) & 3);
            if (in.src_abs(i)) val = v.abs(val);
            if ((s >> 17) & 1) val = v.neg(val);
            return val;
        };
        auto s0 = [&] { return source(0); };
        auto s1 = [&] { return source(1); };
        auto s2 = [&] { return source(2); };
        auto x0 = [&] { return v.comp(s0(), 0); };

        // The execution condition, from the condition code register.
        Id cond = v.bmask(true, true, true, true);
        const bool lt = in.exec_lt(), eq = in.exec_eq(), gt = in.exec_gt();
        if (!(lt && eq && gt)) {
            const Id c = v.swizzle(b.op(spv::OpLoad, v.v4, {cc[in.cond_reg()]}), in.cond_swz(0), in.cond_swz(1),
                                   in.cond_swz(2), in.cond_swz(3));
            Id t = v.bmask(false, false, false, false);
            auto orr = [&](Id a, Id c2) { return b.op(spv::OpLogicalOr, v.b4, {a, c2}); };
            if (lt) t = orr(t, b.op(spv::OpFOrdLessThan, v.b4, {c, v.zero()}));
            if (eq) t = orr(t, b.op(spv::OpFOrdEqual, v.b4, {c, v.zero()}));
            if (gt) t = orr(t, b.op(spv::OpFOrdGreaterThan, v.b4, {c, v.zero()}));
            cond = t;
        }

        Id res = 0;
        const uint32_t op = in.opcode();
        auto sample = [&](uint32_t kind) -> Id {
            const uint32_t t = in.tex();
            if (!samplers[t]) return v.zero();
            const Id smp = b.op(spv::OpLoad, sampler_types[t], {samplers[t]});
            const uint32_t d = key.texture_dims[t];
            const uint32_t n = (d == 3 || d == 6) ? 3 : 2;
            const Id coords = s0();
            const Id ct = b.t_vec(v.f, n);
            // A shadow map: the hardware compares z (z / w for TXP) with the texel, as RPCS3
            // does, and every component gets the result.
            if ((key.shadow >> t) & 1) {
                const Id xy = b.op(spv::OpVectorShuffle, b.t_vec(v.f, 2), {coords, coords, 0, 1});
                const Id r = kind == TXP ? b.op(spv::OpImageSampleProjDrefImplicitLod, v.f,
                                                {smp, b.op(spv::OpVectorShuffle, b.t_vec(v.f, 3), {coords, coords, 0, 1, 3}),
                                                 v.comp(coords, 2)})
                                         : b.op(spv::OpImageSampleDrefImplicitLod, v.f, {smp, xy, v.comp(coords, 2)});
                return v.splat(r);
            }
            auto take = [&](Id c, uint32_t a, uint32_t b2, uint32_t c3) {
                return n == 2 ? b.op(spv::OpVectorShuffle, ct, {c, c, a, b2})
                              : b.op(spv::OpVectorShuffle, ct, {c, c, a, b2, c3});
            };
            switch (kind) {
                case TXP:
                    if (n == 2)
                        return b.op(spv::OpImageSampleProjImplicitLod, v.v4,
                                    {smp, b.op(spv::OpVectorShuffle, b.t_vec(v.f, 3), {coords, coords, 0, 1, 3})});
                    return b.op(spv::OpImageSampleImplicitLod, v.v4,
                                {smp, take(v.div(coords, v.splat(v.comp(coords, 3))), 0, 1, 2)});
                case TXL:
                    return b.op(spv::OpImageSampleExplicitLod, v.v4,
                                {smp, take(coords, 0, 1, 2), spv::Lod, v.comp(s1(), 0)});
                case TXB:
                    return b.op(spv::OpImageSampleImplicitLod, v.v4,
                                {smp, take(coords, 0, 1, 2), spv::Bias, v.comp(s1(), 0)});
                case TXD:
                    return b.op(spv::OpImageSampleExplicitLod, v.v4,
                                {smp, take(coords, 0, 1, 2), spv::Grad, take(s1(), 0, 1, 2), take(s2(), 0, 1, 2)});
                default: return b.op(spv::OpImageSampleImplicitLod, v.v4, {smp, take(coords, 0, 1, 2)});
            }
        };
        const Id u32 = b.t_uint();
        auto bits_of = [&](Id f) { return b.op(spv::OpBitcast, u32, {f}); };
        auto float_of = [&](Id u) { return b.op(spv::OpBitcast, v.f, {u}); };
        switch (op) {
            case NOP: break;
            case FENCT: case FENCB: break;  // texture/branch fences: ordering only, nothing to compute
            case MOV: res = s0(); break;
            case MUL: res = v.mul(s0(), s1()); break;
            case ADD: res = v.add(s0(), s1()); break;
            case MAD: res = v.add(v.mul(s0(), s1()), s2()); break;
            case DP3: res = v.splat(v.dot3(s0(), s1())); break;
            case DP4: res = v.splat(v.dot(s0(), s1())); break;
            case DP2: {
                const Id a = s0(), c = s1();
                res = v.splat(v.fadd(v.fmul(v.comp(a, 0), v.comp(c, 0)), v.fmul(v.comp(a, 1), v.comp(c, 1))));
                break;
            }
            case DP2A: {
                const Id a = s0(), c = s1();
                res = v.splat(v.fadd(v.fadd(v.fmul(v.comp(a, 0), v.comp(c, 0)), v.fmul(v.comp(a, 1), v.comp(c, 1))),
                                     v.comp(s2(), 0)));
                break;
            }
            case DST: {
                const Id a = s0(), c = s1();
                res = v.vec4(b.c_float(1), v.fmul(v.comp(a, 1), v.comp(c, 1)), v.comp(a, 2), v.comp(c, 3));
                break;
            }
            case MIN: res = b.ext(v.v4, spv::FMin, {s0(), s1()}); break;
            case MAX: res = b.ext(v.v4, spv::FMax, {s0(), s1()}); break;
            case SLT: res = v.set_if(spv::OpFOrdLessThan, s0(), s1()); break;
            case SGE: res = v.set_if(spv::OpFOrdGreaterThanEqual, s0(), s1()); break;
            case SLE: res = v.set_if(spv::OpFOrdLessThanEqual, s0(), s1()); break;
            case SGT: res = v.set_if(spv::OpFOrdGreaterThan, s0(), s1()); break;
            case SNE: res = v.set_if(spv::OpFOrdNotEqual, s0(), s1()); break;
            case SEQ: res = v.set_if(spv::OpFOrdEqual, s0(), s1()); break;
            case SFL: res = v.zero(); break;
            case STR: res = v.one(); break;
            case FRC: res = b.ext(v.v4, spv::Fract, {s0()}); break;
            case FLR: res = b.ext(v.v4, spv::Floor, {s0()}); break;
            case DDX: res = b.op(spv::OpDPdx, v.v4, {s0()}); break;
            case DDY: res = b.op(spv::OpDPdy, v.v4, {s0()}); break;
            case TEX: case TXP: case TXD: case TXL: case TXB: res = sample(op); break;
            case RCP: res = v.splat(v.fdiv(b.c_float(1), x0())); break;
            case RSQ: res = v.splat(b.ext(v.f, spv::InverseSqrt, {b.ext(v.f, spv::FAbs, {x0()})})); break;
            case EX2: res = v.splat(b.ext(v.f, spv::Exp2, {x0()})); break;
            case LG2: res = v.splat(b.ext(v.f, spv::Log2, {x0()})); break;
            case COS: res = v.splat(b.ext(v.f, spv::Cos, {x0()})); break;
            case SIN: res = v.splat(b.ext(v.f, spv::Sin, {x0()})); break;
            case POW: res = v.splat(b.ext(v.f, spv::Pow, {x0(), v.comp(s1(), 0)})); break;
            case DIV: res = v.div(s0(), v.splat(v.comp(s1(), 0))); break;
            case DIVSQ: res = v.mul(s0(), v.splat(b.ext(v.f, spv::InverseSqrt, {v.comp(s1(), 0)}))); break;
            case LRP: res = b.ext(v.v4, spv::FMix, {s2(), s1(), s0()}); break;
            case NRM: {
                const Id a = s0();
                const Id v3 = b.t_vec(v.f, 3);
                const Id n = b.ext(v3, spv::Normalize, {b.op(spv::OpVectorShuffle, v3, {a, a, 0, 1, 2})});
                res = b.op(spv::OpVectorShuffle, v.v4, {n, a, 0, 1, 2, 6});
                break;
            }
            case LIT: {
                const Id s = s0();
                const Id x = b.ext(v.f, spv::FMax, {v.comp(s, 0), b.c_float(0)});
                const Id y = b.ext(v.f, spv::FMax, {v.comp(s, 1), b.c_float(0)});
                const Id w = b.ext(v.f, spv::FClamp, {v.comp(s, 3), b.c_float(-128), b.c_float(128)});
                const Id lit = b.op(spv::OpFOrdGreaterThan, v.bl, {v.comp(s, 0), b.c_float(0)});
                res = v.vec4(b.c_float(1), x, b.op(spv::OpSelect, v.f, {lit, b.ext(v.f, spv::Pow, {y, w}), b.c_float(0)}),
                             b.c_float(1));
                break;
            }
            case PK2: res = v.splat(float_of(b.ext(u32, spv::Glsl(58), {b.op(spv::OpVectorShuffle, b.t_vec(v.f, 2), {s0(), s0(), 0, 1})}))); break;
            case UP2: {
                const Id h = b.ext(b.t_vec(v.f, 2), spv::Glsl(62), {bits_of(x0())});
                res = b.op(spv::OpVectorShuffle, v.v4, {h, h, 0, 1, 0, 1});
                break;
            }
            case PK4: res = v.splat(float_of(b.ext(u32, spv::Glsl(54), {s0()}))); break;
            case UP4: res = b.ext(v.v4, spv::Glsl(63), {bits_of(x0())}); break;
            case PKB: res = v.splat(float_of(b.ext(u32, spv::Glsl(55), {s0()}))); break;
            case UPB: res = b.ext(v.v4, spv::Glsl(64), {bits_of(x0())}); break;
            case PK16: res = v.splat(float_of(b.ext(u32, spv::Glsl(56), {b.op(spv::OpVectorShuffle, b.t_vec(v.f, 2), {s0(), s0(), 0, 1})}))); break;
            case UP16: {
                const Id h = b.ext(b.t_vec(v.f, 2), spv::Glsl(60), {bits_of(x0())});
                res = b.op(spv::OpVectorShuffle, v.v4, {h, h, 0, 1, 0, 1});
                break;
            }
            case KIL:  // discard where the condition holds (any component)
                discard_if(b.op(spv::OpAny, v.bl, {cond}));
                break;
            default: {
                char e[64];
                std::snprintf(e, sizeof(e), "FP: opcode 0x%X sem tradução", op);
                r.error = e;
                return r;
            }
        }
        if (res) {
            // Scale, saturation, then the masked/conditional writes.
            switch (in.scale()) {
                case 1: res = v.mul(res, b.c_vec4(2, 2, 2, 2)); break;
                case 2: res = v.mul(res, b.c_vec4(4, 4, 4, 4)); break;
                case 3: res = v.mul(res, b.c_vec4(8, 8, 8, 8)); break;
                case 5: res = v.mul(res, b.c_vec4(0.5f, 0.5f, 0.5f, 0.5f)); break;
                case 6: res = v.mul(res, b.c_vec4(0.25f, 0.25f, 0.25f, 0.25f)); break;
                case 7: res = v.mul(res, b.c_vec4(0.125f, 0.125f, 0.125f, 0.125f)); break;
                default: break;
            }
            if (in.saturate()) res = v.clamp01(res);
            const Id mask = v.band(v.bmask(in.mask(0), in.mask(1), in.mask(2), in.mask(3)), cond);
            if (!in.no_dest()) v.store_masked(in.dst_fp16() ? hregs[in.dest_reg()] : rregs[in.dest_reg()], res, mask);
            if (in.set_cond()) v.store_masked(cc[in.cond_mod_reg()], res, mask);
        }
        if (in.end()) break;
    }

    // Color output: R0 with 32-bit exports, else H0; depth from R1.z.
    Id color = b.op(spv::OpLoad, v.v4, {(key.shader_control & 0x40) ? rregs[0] : hregs[0]});
    if (key.alpha_func && key.alpha_func != 0x207) {
        const Id ref = v.comp(b.op(spv::OpLoad, v.v4, {b.op(spv::OpAccessChain, pu4, {ubo, b.c_int(1)})}), 0);
        const Id a = v.comp(color, 3);
        Id pass;
        switch (key.alpha_func) {
            case 0x200: pass = b.c_bool(false); break;
            case 0x201: pass = b.op(spv::OpFOrdLessThan, v.bl, {a, ref}); break;
            case 0x202: pass = b.op(spv::OpFOrdEqual, v.bl, {a, ref}); break;
            case 0x203: pass = b.op(spv::OpFOrdLessThanEqual, v.bl, {a, ref}); break;
            case 0x204: pass = b.op(spv::OpFOrdGreaterThan, v.bl, {a, ref}); break;
            case 0x205: pass = b.op(spv::OpFOrdNotEqual, v.bl, {a, ref}); break;
            default: pass = b.op(spv::OpFOrdGreaterThanEqual, v.bl, {a, ref}); break;
        }
        discard_if(b.op(spv::OpLogicalNot, v.bl, {pass}));
    }
    b.op_void(spv::OpStore, {out_color, color});
    if (out_depth) b.op_void(spv::OpStore, {out_depth, v.comp(b.op(spv::OpLoad, v.v4, {rregs[1]}), 2)});
    b.op_void(spv::OpReturn, {});
    b.end_function();
    b.entry_point(spv::Fragment, fn, "main", interface);
    b.execution_mode(fn, spv::OriginUpperLeft);
    if (out_depth) b.execution_mode(fn, spv::DepthReplacing);
    r.spirv = b.finish();
    r.ok = true;
    return r;
}

}  // namespace sly4::rsx
