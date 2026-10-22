#include "rsx/rsx_state.h"

namespace sly4::rsx {

using namespace nv4097;

Surface State::surface() const {
    Surface s{};
    const uint32_t f = reg(SetSurfaceFormat);
    s.color_format = f & 0x1F;
    s.depth_format = (f >> 5) & 0x7;
    s.type = (f >> 8) & 0xF;  // 1 linear (pitch), 2 swizzled
    s.antialias = (f >> 12) & 0xF;
    s.x = reg(SetSurfaceClipHorizontal) & 0xFFFF;
    s.width = reg(SetSurfaceClipHorizontal) >> 16;
    s.y = reg(SetSurfaceClipVertical) & 0xFFFF;
    s.height = reg(SetSurfaceClipVertical) >> 16;
    s.target = reg(SetSurfaceColorTarget);
    const uint32_t offsets[4] = {SetSurfaceColorAOffset, SetSurfaceColorBOffset, SetSurfaceColorCOffset,
                                 SetSurfaceColorDOffset};
    const uint32_t pitches[4] = {SetSurfacePitchA, SetSurfacePitchB, SetSurfacePitchC, SetSurfacePitchD};
    const uint32_t dmas[4] = {SetContextDmaColorA, SetContextDmaColorB, SetContextDmaColorC, SetContextDmaColorD};
    for (int i = 0; i < 4; ++i) {
        s.color_offset[i] = reg(offsets[i]);
        s.color_pitch[i] = reg(pitches[i]);
        s.color_location[i] = dma_location(reg(dmas[i]));
    }
    s.depth_offset = reg(SetSurfaceZetaOffset);
    s.depth_pitch = reg(SetSurfacePitchZ);
    s.depth_location = dma_location(reg(SetContextDmaZeta));
    return s;
}

VertexArray State::vertex_array(uint32_t i) const {
    const uint32_t f = reg(SetVertexDataArrayFormat + 4 * i), o = reg(SetVertexDataArrayOffset + 4 * i);
    return {VertexType(f & 0xF), (f >> 4) & 0xF, (f >> 8) & 0xFF, f >> 16, o & 0x7FFFFFFF, o >> 31};
}

Texture State::texture(uint32_t i) const {
    const uint32_t base = SetTextureOffset + 0x20 * i;
    Texture t{};
    t.offset = reg(base);
    const uint32_t f = reg(base + 4);
    t.location = (f & 3) - 1;
    t.cubemap = (f >> 2) & 1;
    t.dimension = (f >> 4) & 0xF;
    t.format = (f >> 8) & 0xFF;
    t.mipmaps = (f >> 16) & 0xFFFF;
    t.address = reg(base + 8);
    t.control0 = reg(base + 0xC);
    t.control1 = reg(base + 0x10);
    t.filter = reg(base + 0x14);
    t.width = reg(base + 0x18) >> 16;
    t.height = reg(base + 0x18) & 0xFFFF;
    t.border = reg(base + 0x1C);
    t.control3 = reg(SetTextureControl3 + 4 * i);
    t.pitch = t.control3 & 0xFFFFF;
    t.depth = t.control3 >> 20;
    return t;
}

}  // namespace sly4::rsx
