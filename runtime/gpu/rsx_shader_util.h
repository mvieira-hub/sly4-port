// Vector helpers shared by the RSX shader translators (on top of spirv.h).
#pragma once

#include "gpu/spirv.h"

namespace sly4::rsx {

struct Vec {
    spv::Builder& b;
    spv::Id f, v4, bl, b4, i32, pf4;  // float, vec4, bool, bvec4, int, Function vec4*

    explicit Vec(spv::Builder& builder)
        : b(builder),
          f(b.t_float()),
          v4(b.t_vec4()),
          bl(b.t_bool()),
          b4(b.t_bvec4()),
          i32(b.t_int()),
          pf4(b.t_pointer(spv::Function, b.t_vec4())) {}

    spv::Id swizzle(spv::Id v, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
        if (x == 0 && y == 1 && z == 2 && w == 3) return v;
        return b.op(spv::OpVectorShuffle, v4, {v, v, x, y, z, w});
    }
    spv::Id splat(spv::Id s) { return b.op(spv::OpCompositeConstruct, v4, {s, s, s, s}); }
    spv::Id comp(spv::Id v, uint32_t i) { return b.op(spv::OpCompositeExtract, f, {v, i}); }
    spv::Id vec4(spv::Id x, spv::Id y, spv::Id z, spv::Id w) {
        return b.op(spv::OpCompositeConstruct, v4, {x, y, z, w});
    }
    spv::Id neg(spv::Id v) { return b.op(spv::OpFNegate, v4, {v}); }
    spv::Id abs(spv::Id v) { return b.ext(v4, spv::FAbs, {v}); }
    spv::Id add(spv::Id a, spv::Id c) { return b.op(spv::OpFAdd, v4, {a, c}); }
    spv::Id sub(spv::Id a, spv::Id c) { return b.op(spv::OpFSub, v4, {a, c}); }
    spv::Id mul(spv::Id a, spv::Id c) { return b.op(spv::OpFMul, v4, {a, c}); }
    spv::Id div(spv::Id a, spv::Id c) { return b.op(spv::OpFDiv, v4, {a, c}); }
    spv::Id fadd(spv::Id a, spv::Id c) { return b.op(spv::OpFAdd, f, {a, c}); }
    spv::Id fmul(spv::Id a, spv::Id c) { return b.op(spv::OpFMul, f, {a, c}); }
    spv::Id fdiv(spv::Id a, spv::Id c) { return b.op(spv::OpFDiv, f, {a, c}); }
    spv::Id dot(spv::Id a, spv::Id c) { return b.op(spv::OpDot, f, {a, c}); }
    spv::Id dot3(spv::Id a, spv::Id c) {
        const spv::Id v3 = b.t_vec(f, 3);
        return b.op(spv::OpDot, f, {b.op(spv::OpVectorShuffle, v3, {a, a, 0, 1, 2}),
                                    b.op(spv::OpVectorShuffle, v3, {c, c, 0, 1, 2})});
    }
    spv::Id zero() { return b.c_vec4(0, 0, 0, 0); }
    spv::Id one() { return b.c_vec4(1, 1, 1, 1); }
    // 1.0 where the comparison holds, else 0.0 (component-wise).
    spv::Id set_if(spv::Op cmp, spv::Id a, spv::Id c) {
        return b.op(spv::OpSelect, v4, {b.op(cmp, b4, {a, c}), one(), zero()});
    }
    spv::Id clamp01(spv::Id v) { return b.ext(v4, spv::FClamp, {v, zero(), one()}); }
    spv::Id bmask(bool x, bool y, bool z, bool w) {
        return b.c_composite(b4, {b.c_bool(x), b.c_bool(y), b.c_bool(z), b.c_bool(w)});
    }
    // Stores `value` into `var` only in the components where `mask` (bvec4) is true.
    void store_masked(spv::Id var, spv::Id value, spv::Id mask) {
        const spv::Id old = b.op(spv::OpLoad, v4, {var});
        b.op_void(spv::OpStore, {var, b.op(spv::OpSelect, v4, {mask, value, old})});
    }
    spv::Id band(spv::Id a, spv::Id c) { return b.op(spv::OpLogicalAnd, b4, {a, c}); }
    // A condition-code test (RSX COND_* values 0-7: FL, LT, EQ, LE, GT, NE, GE, TR) on
    // the components of `cc`, as a bvec4.
    spv::Id cond_test(uint32_t cond, spv::Id cc) {
        switch (cond & 7) {
            case 0: return bmask(false, false, false, false);
            case 1: return b.op(spv::OpFOrdLessThan, b4, {cc, zero()});
            case 2: return b.op(spv::OpFOrdEqual, b4, {cc, zero()});
            case 3: return b.op(spv::OpFOrdLessThanEqual, b4, {cc, zero()});
            case 4: return b.op(spv::OpFOrdGreaterThan, b4, {cc, zero()});
            case 5: return b.op(spv::OpFOrdNotEqual, b4, {cc, zero()});
            case 6: return b.op(spv::OpFOrdGreaterThanEqual, b4, {cc, zero()});
            default: return bmask(true, true, true, true);
        }
    }
};

inline uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

}  // namespace sly4::rsx
