// RSX vertex and fragment programs -> SPIR-V (docs/GPU.md, 5d).
//
// Vertex programs (NV40 VP) are 128-bit instructions in the transform program memory;
// fragment programs (NV40 FP) are 128-bit instructions in guest memory, each 32-bit word
// with its halves swapped, and constants inline after the instruction that uses them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sly4::rsx {

// Shader interface shared by both translators and the Vulkan backend.
namespace shader_if {
// Vertex shader: inputs v[i] at location i (vec4, already converted to float).
// Uniform block (set 0, binding 0): vec4 c[468]; vec4 scale; vec4 offset; vec4 target
// (surface width, height). Outputs at these locations:
constexpr uint32_t kCol0 = 1, kCol1 = 2, kFog = 3, kTex0 = 4;  // TEX0..TEX9 at 4..13
constexpr uint32_t kVsConstants = 468;
// Fragment shader: uniform block (set 0, binding 1): vec4 fc[N] (the inline constants, in
// order) followed by vec4 alpha_ref; textures at binding 2 + unit.
constexpr uint32_t kFsUniformBinding = 1, kTextureBinding0 = 2;
}  // namespace shader_if

struct ShaderResult {
    bool ok = false;
    std::string error;
    std::vector<uint32_t> spirv;
};

// ---------------------------------------------------------------- vertex program
struct VertexProgramInfo {
    uint32_t instructions = 0;
    uint32_t inputs = 0;   // bitmask of v[i] read
    uint64_t hash = 0;     // of the instructions, for caching
};
// Decodes from `start` (instruction index) up to the instruction with END set.
VertexProgramInfo scan_vertex_program(const uint32_t* program, uint32_t start);
ShaderResult translate_vertex_program(const uint32_t* program, uint32_t start);

// ---------------------------------------------------------------- fragment program
// Texture dimensions the fragment program samples, per unit (2 = 2D, 3 = 3D, 6 = cube).
struct FragmentProgramInfo {
    uint32_t instructions = 0;    // 128-bit slots, constants included
    uint32_t size = 0;            // bytes
    std::vector<uint32_t> constant_offsets;  // byte offsets of the inline constants
    uint32_t textures = 0;        // bitmask of units sampled
    uint64_t hash = 0;            // of the code without the constant values
};
// `code` points at the guest bytes of the program (big-endian words); at most `max` bytes.
FragmentProgramInfo scan_fragment_program(const uint8_t* code, uint32_t max);

struct FragmentProgramKey {
    uint32_t shader_control = 0;  // SET_SHADER_CONTROL (32-bit exports, depth replace)
    uint32_t texture_dims[16] = {};
    uint32_t alpha_func = 0;      // 0 = no alpha test, else the GL compare function
    uint32_t shadow = 0;          // units holding depth textures sampled with a comparison
};
ShaderResult translate_fragment_program(const uint8_t* code, const FragmentProgramInfo& info,
                                        const FragmentProgramKey& key);

}  // namespace sly4::rsx
