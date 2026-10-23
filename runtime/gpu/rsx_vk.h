// The RSX renderer on Vulkan (docs/GPU.md, 5c-5e): RSX surfaces become Vulkan images,
// programs are translated to SPIR-V, vertex data and textures are converted from guest
// memory, and at each flip the display buffer is copied to a "front" image that the
// window shows (or that is written to a PNG file).
#pragma once

#include <memory>
#include <string>

#include "gpu/vulkan_context.h"
#include "rsx/rsx_backend.h"

namespace sly4::rsx {

struct VulkanRendererOptions {
    std::string dump_dir;  // write the front image of every `dump_every` flips here as PNG
    uint32_t dump_every = 0;
    uint32_t dump_first = 0;  // and only from this flip on
};

class VulkanRenderer : public Backend {
public:
    virtual ~VulkanRenderer() = default;
    // The image to show (TRANSFER_SRC_OPTIMAL), updated at each flip.
    virtual VulkanContext::Source front() = 0;
    virtual void summary() = 0;
};

std::unique_ptr<VulkanRenderer> create_vulkan_renderer(VulkanContext& ctx, const VulkanRendererOptions& opt);

}  // namespace sly4::rsx
