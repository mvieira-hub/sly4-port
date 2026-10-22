// RSX: state decoding, shader translation and texture decoding (docs/GPU.md, phase 5).
#include "check.h"

#include <cstring>
#include <vector>

#include "core/endian.h"
#include "gpu/rsx_shaders.h"
#include "gpu/rsx_texture.h"
#include "rsx/rsx_state.h"

using namespace sly4;
using namespace sly4::rsx;

// The game's loading-screen vertex program (captured from its FIFO): MOV o[1] = v[3],
// MOV o[7] = v[8], then o[0].wzyx = DP4(v[0], c[3..0]).
static const uint32_t kLoadingVp[6 * 4] = {
    0x401F9C6C, 0x0040030D, 0x8106C083, 0x6041FF84, 0x401F9C6C, 0x00400808, 0x0106C083, 0x60419F9C,
    0x401F9C6C, 0x01C0300D, 0x8106C0C3, 0x60403F80, 0x401F9C6C, 0x01C0200D, 0x8106C0C3, 0x60405F80,
    0x401F9C6C, 0x01C0100D, 0x8106C0C3, 0x60409F80, 0x401F9C6C, 0x01C0000D, 0x8106C0C3, 0x60411F81,
};

TEST(rsx_vertex_program_translates) {
    std::vector<uint32_t> program(512 * 4, 0);
    std::memcpy(program.data(), kLoadingVp, sizeof(kLoadingVp));
    const VertexProgramInfo info = scan_vertex_program(program.data(), 0);
    CHECK_EQ(info.instructions, 6u);
    CHECK_EQ(info.inputs, (1u << 0) | (1u << 3) | (1u << 8));
    const ShaderResult r = translate_vertex_program(program.data(), 0);
    CHECK(r.ok);
    CHECK(r.spirv.size() > 5);
    CHECK_EQ(r.spirv[0], 0x07230203u);  // SPIR-V magic
}

// Fragment program words are stored big-endian with their halves swapped.
static void put_fp_word(uint8_t* p, uint32_t w) { store_be<uint32_t>(p, (w << 16) | (w >> 16)); }

TEST(rsx_fragment_program_with_constant) {
    // MOV R0, c (inline constant 0.25, 0.5, 0.75, 1), END.
    uint8_t code[32] = {};
    const uint32_t dst = 1 | (0xF << 9) | (1u << 24);  // end, R0, xyzw, MOV
    const uint32_t src0 = 2 | (0 << 9) | (1 << 11) | (2 << 13) | (3 << 15) | (7u << 18);  // constant, xyzw, always
    put_fp_word(code, dst);
    put_fp_word(code + 4, src0);
    put_fp_word(code + 8, 0);
    put_fp_word(code + 12, 0);
    const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &c[i], 4);
        put_fp_word(code + 16 + 4 * i, bits);
    }
    const FragmentProgramInfo info = scan_fragment_program(code, sizeof(code));
    CHECK_EQ(info.size, 32u);
    CHECK_EQ(info.constant_offsets.size(), size_t(1));
    CHECK_EQ(info.constant_offsets[0], 16u);
    FragmentProgramKey key;
    key.shader_control = 0x40;  // 32-bit exports: the color is R0
    const ShaderResult r = translate_fragment_program(code, info, key);
    CHECK(r.ok);
    CHECK_EQ(r.spirv[0], 0x07230203u);
}

TEST(rsx_texture_decode) {
    // 2x2 A8R8G8B8, linear, identity remap: bytes A R G B -> R G B A.
    const uint8_t argb[16] = {0xFF, 0x10, 0x20, 0x30, 0x80, 0x40, 0x50, 0x60,
                              0x00, 0x70, 0x80, 0x90, 0x7F, 0xA0, 0xB0, 0xC0};
    Texture t{};
    t.format = 0x85 | 0x20;  // A8R8G8B8, linear
    t.width = t.height = 2;
    t.pitch = 8;
    t.control1 = 0xAAE4;
    CHECK_EQ(texture_source_size(t), 16u);
    TextureData d = decode_texture(argb, t);
    CHECK(d.ok);
    const uint8_t want[16] = {0x10, 0x20, 0x30, 0xFF, 0x40, 0x50, 0x60, 0x80,
                              0x70, 0x80, 0x90, 0x00, 0xA0, 0xB0, 0xC0, 0x7F};
    CHECK(std::memcmp(d.bytes.data(), want, 16) == 0);
    // Swizzled 2x2: Morton order (0,0) (1,0) (0,1) (1,1) is the same as linear here; 4x2
    // differs: texel (2,0) is the 5th in memory.
    uint8_t sw[32];
    for (int i = 0; i < 8; ++i) store_be<uint32_t>(sw + 4 * i, 0xFF000000u | uint32_t(i));
    t.format = 0x85;  // swizzled
    t.width = 4;
    t.pitch = 0;
    d = decode_texture(sw, t);
    CHECK_EQ(d.bytes[(0 * 4 + 2) * 4 + 2], 4);  // (2,0) -> index 4, blue byte
    CHECK_EQ(d.bytes[(1 * 4 + 1) * 4 + 2], 3);  // (1,1) -> index 3
    // The remap can force channels: alpha to one, blue to zero.
    t.format = 0x85 | 0x20;
    t.width = 2;
    t.pitch = 8;
    t.control1 = (0xE4) | (1 << 8) | (2 << 10) | (2 << 12) | (0 << 14);  // A one, R/G remap, B zero
    d = decode_texture(argb, t);
    CHECK_EQ(d.bytes[3], 255);
    CHECK_EQ(d.bytes[2], 0);
    CHECK_EQ(d.bytes[0], 0x10);
}

TEST(rsx_state_decoders) {
    State s;
    s.set(nv4097::SetSurfaceFormat, 0x090A0148);
    s.set(nv4097::SetSurfaceClipHorizontal, 0x05000000);
    s.set(nv4097::SetSurfaceClipVertical, 0x02D00000);
    const Surface sf = s.surface();
    CHECK_EQ(sf.color_format, 8u);  // A8R8G8B8
    CHECK_EQ(sf.depth_format, 2u);  // Z24S8
    CHECK_EQ(sf.width, 1280u);
    CHECK_EQ(sf.height, 720u);
    s.set(nv4097::SetVertexDataArrayFormat + 4 * 3, 0x00001844);
    s.set(nv4097::SetVertexDataArrayOffset + 4 * 3, 0x8000000C);
    const VertexArray a = s.vertex_array(3);
    CHECK(a.type == VertexType::UB);
    CHECK_EQ(a.size, 4u);
    CHECK_EQ(a.stride, 0x18u);
    CHECK_EQ(a.offset, 0xCu);
    CHECK_EQ(a.location, 1u);
    s.set(nv4097::SetTextureFormat, 0x0001A529);
    s.set(nv4097::SetTextureImageRect, 0x00C00068);
    const Texture t = s.texture(0);
    CHECK_EQ(t.location, 0u);
    CHECK_EQ(t.base_format(), 0x85u);
    CHECK(t.linear());
    CHECK_EQ(t.width, 192u);
    CHECK_EQ(t.height, 104u);
}
