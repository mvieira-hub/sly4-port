// RSX textures in guest memory -> data a Vulkan image can take (docs/GPU.md, 5c).
#pragma once

#include <cstdint>
#include <vector>

#include "gpu/vk_functions.h"
#include "memory/guest_memory.h"
#include "rsx/rsx_state.h"

namespace sly4::rsx {

struct TextureData {
    bool ok = false;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkComponentMapping swizzle{};  // identity unless the data could not be remapped on the CPU
    uint32_t width = 0, height = 0;   // level 0
    uint32_t levels = 1;
    std::vector<uint8_t> bytes;       // every level, rows tightly packed
    std::vector<size_t> level_offset;  // where each level starts in bytes
};

// The mipmap levels the texture has (at least 1, at most down to 1x1).
uint32_t texture_levels(const Texture& t);
// Bytes of guest memory all levels of `t` occupy (for change detection).
uint32_t texture_source_size(const Texture& t);
// Decodes `t` from `src` (texture_source_size(t) bytes).
TextureData decode_texture(const uint8_t* src, const Texture& t);

}  // namespace sly4::rsx
