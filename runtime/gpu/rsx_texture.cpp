#include "gpu/rsx_texture.h"

#include <algorithm>
#include <cstring>

#include "core/endian.h"

namespace sly4::rsx {

namespace {

// CELL_GCM_TEXTURE_* base formats (without the LN/UN flags).
enum : uint32_t {
    B8 = 0x81, A1R5G5B5 = 0x82, A4R4G4B4 = 0x83, R5G6B5 = 0x84, A8R8G8B8 = 0x85, DXT1 = 0x86, DXT23 = 0x87,
    DXT45 = 0x88, G8B8 = 0x8B, R6G5B5 = 0x8F, X16 = 0x94, Y16_X16 = 0x95, R5G5B5A1 = 0x97,
    W16Z16Y16X16_FLOAT = 0x9A, W32Z32Y32X32_FLOAT = 0x9B, X32_FLOAT = 0x9C, D1R5G5B5 = 0x9D, D8R8G8B8 = 0x9E,
    Y16_X16_FLOAT = 0x9F,
};

uint32_t texel_bytes(uint32_t f) {
    switch (f) {
        case B8: return 1;
        case A1R5G5B5: case A4R4G4B4: case R5G6B5: case G8B8: case R6G5B5: case X16: case R5G5B5A1:
        case D1R5G5B5: return 2;
        case A8R8G8B8: case Y16_X16: case X32_FLOAT: case D8R8G8B8: case Y16_X16_FLOAT: return 4;
        case W16Z16Y16X16_FLOAT: return 8;
        case W32Z32Y32X32_FLOAT: return 16;
        default: return 0;
    }
}
bool compressed(uint32_t f) { return f == DXT1 || f == DXT23 || f == DXT45; }
uint32_t block_bytes(uint32_t f) { return f == DXT1 ? 8 : 16; }

// Offset of texel (x, y) in a swizzled (Morton order, x bit first) texture.
uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t log2w, uint32_t log2h) {
    uint32_t offset = 0, shift = 0;
    while (log2w || log2h) {
        if (log2w) {
            offset |= (x & 1) << shift++;
            x >>= 1;
            --log2w;
        }
        if (log2h) {
            offset |= (y & 1) << shift++;
            y >>= 1;
            --log2h;
        }
    }
    return offset;
}
uint32_t log2_of(uint32_t v) {
    uint32_t l = 0;
    while ((1u << l) < v) ++l;
    return l;
}

uint8_t expand(uint32_t v, uint32_t bits) { return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }
uint8_t unorm8(float f) { return uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }
float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = float(m) / 16777216.0f;
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else v = (1.0f + float(m) / 1024.0f) * float(1u << e) / 32768.0f;
    return s ? -v : v;
}

// One texel as A, R, G, B (8 bits each).
void decode_texel(uint32_t f, const uint8_t* p, uint8_t argb[4]) {
    switch (f) {
        case B8: argb[0] = argb[1] = argb[2] = argb[3] = p[0]; break;
        case G8B8: argb[0] = p[0]; argb[1] = p[1]; argb[2] = p[0]; argb[3] = p[1]; break;
        case A8R8G8B8: std::memcpy(argb, p, 4); break;
        case D8R8G8B8: argb[0] = 255; argb[1] = p[1]; argb[2] = p[2]; argb[3] = p[3]; break;
        case R5G6B5: {
            const uint32_t v = load_be<uint16_t>(p);
            argb[0] = 255; argb[1] = expand(v >> 11, 5); argb[2] = expand((v >> 5) & 0x3F, 6); argb[3] = expand(v & 0x1F, 5);
            break;
        }
        case R6G5B5: {
            const uint32_t v = load_be<uint16_t>(p);
            argb[0] = 255; argb[1] = expand(v >> 10, 6); argb[2] = expand((v >> 5) & 0x1F, 5); argb[3] = expand(v & 0x1F, 5);
            break;
        }
        case A1R5G5B5: case D1R5G5B5: {
            const uint32_t v = load_be<uint16_t>(p);
            argb[0] = (f == D1R5G5B5 || (v >> 15)) ? 255 : 0;
            argb[1] = expand((v >> 10) & 0x1F, 5); argb[2] = expand((v >> 5) & 0x1F, 5); argb[3] = expand(v & 0x1F, 5);
            break;
        }
        case R5G5B5A1: {
            const uint32_t v = load_be<uint16_t>(p);
            argb[0] = (v & 1) ? 255 : 0;
            argb[1] = expand(v >> 11, 5); argb[2] = expand((v >> 6) & 0x1F, 5); argb[3] = expand((v >> 1) & 0x1F, 5);
            break;
        }
        case A4R4G4B4: {
            const uint32_t v = load_be<uint16_t>(p);
            argb[0] = expand(v >> 12, 4); argb[1] = expand((v >> 8) & 0xF, 4); argb[2] = expand((v >> 4) & 0xF, 4);
            argb[3] = expand(v & 0xF, 4);
            break;
        }
        case X16: argb[0] = argb[1] = argb[2] = argb[3] = p[0]; break;  // high byte
        case Y16_X16: argb[0] = argb[1] = p[0]; argb[2] = argb[3] = p[2]; break;
        case X32_FLOAT: {
            float v;
            const uint32_t bits = load_be<uint32_t>(p);
            std::memcpy(&v, &bits, 4);
            argb[0] = argb[1] = argb[2] = argb[3] = unorm8(v);
            break;
        }
        case Y16_X16_FLOAT:
            argb[0] = argb[1] = unorm8(half_to_float(load_be<uint16_t>(p)));
            argb[2] = argb[3] = unorm8(half_to_float(load_be<uint16_t>(p + 2)));
            break;
        case W16Z16Y16X16_FLOAT:  // X, Y, Z, W in memory -> R, G, B, A
            argb[1] = unorm8(half_to_float(load_be<uint16_t>(p)));
            argb[2] = unorm8(half_to_float(load_be<uint16_t>(p + 2)));
            argb[3] = unorm8(half_to_float(load_be<uint16_t>(p + 4)));
            argb[0] = unorm8(half_to_float(load_be<uint16_t>(p + 6)));
            break;
        case W32Z32Y32X32_FLOAT:
            for (int i = 0; i < 4; ++i) {
                float v;
                const uint32_t bits = load_be<uint32_t>(p + 4 * i);
                std::memcpy(&v, &bits, 4);
                argb[i == 3 ? 0 : i + 1] = unorm8(v);
            }
            break;
        default: argb[0] = 255; argb[1] = 255; argb[2] = 0; argb[3] = 255; break;  // magenta: unknown
    }
}

// Where level `l` sits in guest memory: the levels follow each other. Compressed and linear
// levels keep the row pitch of the texture (or are packed when it has none); swizzled
// levels are each a swizzled power-of-two image.
struct LevelLayout {
    uint32_t width, height, pitch, size;  // pitch: bytes per row (per block row when compressed)
};
LevelLayout level_layout(const Texture& t, uint32_t l) {
    const uint32_t f = t.base_format();
    LevelLayout g;
    g.width = std::max(1u, t.width >> l);
    g.height = std::max(1u, t.height >> l);
    if (compressed(f)) {
        const uint32_t row = ((g.width + 3) / 4) * block_bytes(f), rows = (g.height + 3) / 4;
        g.pitch = t.linear() && t.pitch ? t.pitch : row;
        g.size = g.pitch * (rows - 1) + row;
        if (t.linear() && t.pitch) g.size = g.pitch * rows;
        return g;
    }
    const uint32_t bpp = texel_bytes(f);
    if (t.linear() && t.pitch) {
        g.pitch = t.pitch;
        g.size = t.pitch * g.height;
    } else if (t.linear()) {
        g.pitch = g.width * bpp;
        g.size = g.pitch * g.height;
    } else {
        g.pitch = 0;
        g.size = (1u << log2_of(g.width)) * (1u << log2_of(g.height)) * bpp;
    }
    return g;
}

}  // namespace

uint32_t texture_levels(const Texture& t) {
    uint32_t most = 1;
    for (uint32_t s = std::max(t.width, t.height); s > 1; s >>= 1) ++most;
    return std::clamp(t.mipmaps, 1u, std::min(most, 13u));
}

uint32_t texture_source_size(const Texture& t) {
    uint32_t size = 0;
    for (uint32_t l = 0, n = texture_levels(t); l < n; ++l) size += level_layout(t, l).size;
    return size;
}

namespace {

// Decodes one uncompressed level into tightly packed RGBA8.
void decode_level(const uint8_t* src, const Texture& t, uint32_t l, const uint32_t (&src_of)[4],
                  const uint32_t (&op_of)[4], uint8_t* dst) {
    const uint32_t f = t.base_format(), bpp = texel_bytes(f);
    const LevelLayout g = level_layout(t, l);
    const uint32_t lw = log2_of(g.width), lh = log2_of(g.height);
    uint8_t argb[4], out[4];
    for (uint32_t y = 0; y < g.height; ++y)
        for (uint32_t x = 0; x < g.width; ++x) {
            const uint8_t* p = g.pitch ? src + size_t(y) * g.pitch + x * bpp : src + size_t(swizzle_index(x, y, lw, lh)) * bpp;
            decode_texel(f, p, argb);
            for (int i = 0; i < 4; ++i) out[i] = op_of[i] == 0 ? 0 : op_of[i] == 1 ? 255 : argb[src_of[i]];
            uint8_t* o = dst + (size_t(y) * g.width + x) * 4;
            o[0] = out[1];  // R
            o[1] = out[2];  // G
            o[2] = out[3];  // B
            o[3] = out[0];  // A
        }
}

}  // namespace

TextureData decode_texture(const uint8_t* src, const Texture& t) {
    TextureData d;
    const uint32_t f = t.base_format();
    d.width = std::max(1u, t.width);
    d.height = std::max(1u, t.height);
    d.levels = texture_levels(t);
    // The remap: per output channel (A, R, G, B) a source channel and an operation.
    const uint32_t remap = t.control1 & 0xFFFF;
    uint32_t src_of[4], op_of[4];
    for (int i = 0; i < 4; ++i) {
        src_of[i] = (remap >> (2 * i)) & 3;
        op_of[i] = (remap >> (8 + 2 * i)) & 3;
    }
    if (compressed(f)) {
        d.format = f == DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : f == DXT23 ? VK_FORMAT_BC2_UNORM_BLOCK
                                                                         : VK_FORMAT_BC3_UNORM_BLOCK;
        // Each level's blocks, rows packed (a linear texture's pitch may pad them).
        for (uint32_t l = 0, at = 0; l < d.levels; ++l) {
            const LevelLayout g = level_layout(t, l);
            const uint32_t row = ((g.width + 3) / 4) * block_bytes(f), rows = (g.height + 3) / 4;
            d.level_offset.push_back(d.bytes.size());
            for (uint32_t r = 0; r < rows; ++r)
                d.bytes.insert(d.bytes.end(), src + at + size_t(r) * g.pitch, src + at + size_t(r) * g.pitch + row);
            at += g.size;
        }
        // Channel X of the data is component: A, R, G, B -> a, r, g, b.
        const VkComponentSwizzle from[4] = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                            VK_COMPONENT_SWIZZLE_B};
        auto map = [&](int out) {
            return op_of[out] == 0 ? VK_COMPONENT_SWIZZLE_ZERO
                                   : op_of[out] == 1 ? VK_COMPONENT_SWIZZLE_ONE : from[src_of[out]];
        };
        d.swizzle = {map(1), map(2), map(3), map(0)};
        d.ok = true;
        return d;
    }
    const uint32_t bpp = texel_bytes(f);
    if (!bpp) {
        d.levels = 1;
        d.level_offset = {0};
        d.bytes.assign(size_t(d.width) * d.height * 4, 0);
        for (size_t i = 0; i < d.bytes.size(); i += 4) d.bytes[i] = d.bytes[i + 2] = d.bytes[i + 3] = 255;
        d.ok = false;
        return d;
    }
    for (uint32_t l = 0, at = 0; l < d.levels; ++l) {
        const LevelLayout g = level_layout(t, l);
        d.level_offset.push_back(d.bytes.size());
        d.bytes.resize(d.bytes.size() + size_t(g.width) * g.height * 4);
        decode_level(src + at, t, l, src_of, op_of, d.bytes.data() + d.level_offset.back());
        at += g.size;
    }
    d.ok = true;
    return d;
}

}  // namespace sly4::rsx
