// RSX state tracker (docs/GPU.md, 5b): every register the FIFO writes, plus the memories
// that are loaded through registers (vertex program, vertex constants), with decoders for
// what the backend needs.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "rsx/rsx_methods.h"

namespace sly4::rsx {

constexpr uint32_t kTransformProgramSize = 512;    // 128-bit instructions
constexpr uint32_t kTransformConstants = 468;      // vec4
constexpr uint32_t kVertexAttributes = 16;
constexpr uint32_t kTextureUnits = 16;

// Primitive types of SET_BEGIN_END.
enum class Primitive : uint32_t {
    None = 0, Points = 1, Lines = 2, LineLoop = 3, LineStrip = 4, Triangles = 5, TriangleStrip = 6,
    TriangleFan = 7, Quads = 8, QuadStrip = 9, Polygon = 10,
};

// Vertex attribute formats (SET_VERTEX_DATA_ARRAY_FORMAT bits 0-3).
enum class VertexType : uint32_t { S1 = 1, F = 2, SF = 3, UB = 4, S32K = 5, CMP = 6, UB256 = 7 };

struct VertexArray {
    VertexType type;
    uint32_t size;    // components, 0 = disabled
    uint32_t stride;
    uint32_t frequency;
    uint32_t offset;  // in its location
    uint32_t location;
};

struct Surface {
    uint32_t color_format, depth_format, type, antialias;  // SET_SURFACE_FORMAT fields
    uint32_t width, height, x, y;                          // clip
    uint32_t target;                                       // SET_SURFACE_COLOR_TARGET
    uint32_t color_offset[4], color_pitch[4], color_location[4];
    uint32_t depth_offset, depth_pitch, depth_location;
};

struct Texture {
    uint32_t offset, location, format, mipmaps, dimension, width, height, depth, pitch;
    uint32_t address, control0, control1, filter, border, control3;
    // format: the 8-bit CELL_GCM_TEXTURE_* value, with the LN (0x20) and UN (0x40) flags.
    bool enabled() const { return (control0 >> 31) & 1; }
    bool linear() const { return format & 0x20; }  // otherwise swizzled
    bool normalized() const { return !(format & 0x40); }
    uint32_t base_format() const { return format & ~0x60u; }
    bool cubemap = false;
};

class State {
public:
    uint32_t reg(uint32_t method) const { return regs_[(method & 0xFFFF) >> 2]; }
    float regf(uint32_t method) const {
        float f;
        std::memcpy(&f, &regs_[(method & 0xFFFF) >> 2], 4);
        return f;
    }
    void set(uint32_t method, uint32_t v) { regs_[(method & 0xFFFF) >> 2] = v; }

    // Memories written through registers.
    std::array<uint32_t, kTransformProgramSize * 4> transform_program{};
    uint32_t transform_program_load = 0;
    std::array<float, kTransformConstants * 4> transform_constants{};
    // Attribute values used when an attribute has no array (SET_VERTEX_DATA*_M).
    std::array<float, kVertexAttributes * 4> vertex_data{};

    // Decoded views.
    Surface surface() const;
    VertexArray vertex_array(uint32_t i) const;
    Texture texture(uint32_t i) const;
    uint32_t vertex_program_start() const { return reg(nv4097::SetTransformProgramStart); }
    // SET_SHADER_PROGRAM: offset of the fragment program and its location (1 local, 2 main).
    uint32_t fragment_program_offset() const { return reg(nv4097::SetShaderProgram) & ~3u; }
    uint32_t fragment_program_location() const { return (reg(nv4097::SetShaderProgram) & 3) - 1; }

private:
    std::array<uint32_t, 0x10000 / 4> regs_{};
};

// Location of a context DMA handle (SET_CONTEXT_DMA_*): local or main memory.
inline uint32_t dma_location(uint32_t handle) {
    return handle == 0xFEED0001 || handle == 0xBAD68000 ? 1u : 0u;
}

}  // namespace sly4::rsx
