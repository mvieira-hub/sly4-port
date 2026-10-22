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
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;    // level 0, rows tightly packed
};

// Bytes of guest memory level 0 of `t` occupies (for change detection).
uint32_t texture_source_size(const Texture& t);
// Decodes level 0 of `t` from `src` (texture_source_size(t) bytes).
TextureData decode_texture(const uint8_t* src, const Texture& t);

}  // namespace sly4::rsx
