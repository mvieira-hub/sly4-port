#include "gpu/rsx_vk.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "core/endian.h"
#include "core/log.h"
#include "core/png.h"
#include "gpu/rsx_shader_util.h"
#include "gpu/rsx_shaders.h"
#include "gpu/rsx_texture.h"
#include "rsx/rsx.h"

namespace sly4::rsx {

namespace {

using namespace sly4::vk;
using namespace nv4097;

constexpr VkDeviceSize kRingSize = 96ull << 20;
constexpr uint32_t kMaxSets = 4096;
constexpr uint32_t kTextureUnits = 16;

bool ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    SLY4_ERROR("RSX/Vulkan: %s falhou: %s", what, result_name(r));
    return false;
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, layers = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t hash = 0;          // textures: hash of the guest bytes uploaded
    bool cpu_dirty = false;     // surfaces: guest memory written behind the GPU's back
};

// Everything a pipeline depends on besides the dynamic state.
struct PipelineKey {
    VkShaderModule vs, fs;
    uint32_t topology;
    uint32_t inputs, constant_inputs;
    VkFormat color, depth;
    uint32_t blend_enable, blend_sf, blend_df, blend_eq, color_mask;
    uint32_t depth_test, depth_write, depth_func;
    uint32_t stencil[8];
    uint32_t restart;
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const { return size_t(fnv1a64(&k, sizeof(k))); }
};

struct TextureKey {
    GuestAddr ea;
    uint32_t format, width, height, pitch, remap, dimension;
    bool operator<(const TextureKey& o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
};

VkBlendFactor blend_factor(uint32_t gl) {
    switch (gl) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint32_t gl) {
    switch (gl) {
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp stencil_op(uint32_t gl) {
    switch (gl) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
VkCompareOp compare_op(uint32_t gl) { return VkCompareOp(gl >= 0x200 && gl <= 0x207 ? gl - 0x200 : 7); }

VkSamplerAddressMode address_mode(uint32_t m) {
    switch (m) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6: case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = float(m) / 16777216.0f;
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else v = (1.0f + float(m) / 1024.0f) * float(1u << e) / 32768.0f;
    return s ? -v : v;
}

// Bytes one attribute takes in guest memory (inline arrays pad each to 4 bytes).
uint32_t attribute_bytes(const VertexArray& a) {
    switch (a.type) {
        case VertexType::F: return 4 * a.size;
        case VertexType::S1: case VertexType::SF: case VertexType::S32K: return 2 * a.size;
        case VertexType::UB: case VertexType::UB256: return a.size;
        case VertexType::CMP: return 4;
        default: return 4 * a.size;
    }
}

// One attribute of one vertex to float4 (missing components 0, 0, 0, 1).
void convert_attribute(const VertexArray& a, const uint8_t* p, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const uint32_t n = std::min(a.size, 4u);
    switch (a.type) {
        case VertexType::F:
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t b = load_be<uint32_t>(p + 4 * i);
                std::memcpy(&out[i], &b, 4);
            }
            break;
        case VertexType::S1:
            for (uint32_t i = 0; i < n; ++i) out[i] = std::max(-1.0f, float(int16_t(load_be<uint16_t>(p + 2 * i))) / 32767.0f);
            break;
        case VertexType::SF:
            for (uint32_t i = 0; i < n; ++i) out[i] = half_to_float(load_be<uint16_t>(p + 2 * i));
            break;
        case VertexType::S32K:
            for (uint32_t i = 0; i < n; ++i) out[i] = float(int16_t(load_be<uint16_t>(p + 2 * i)));
            break;
        case VertexType::UB:
            for (uint32_t i = 0; i < n; ++i) out[i] = float(p[i]) / 255.0f;
            break;
        case VertexType::UB256:
            for (uint32_t i = 0; i < n; ++i) out[i] = float(p[i]);
            break;
        case VertexType::CMP: {
            const uint32_t v = load_be<uint32_t>(p);
            auto sext = [](uint32_t x, int bits) { return int32_t(x << (32 - bits)) >> (32 - bits); };
            out[0] = float(sext(v & 0x7FF, 11)) / 1023.0f;
            out[1] = float(sext((v >> 11) & 0x7FF, 11)) / 1023.0f;
            out[2] = float(sext(v >> 22, 10)) / 511.0f;
            break;
        }
        default: break;
    }
}

class Renderer final : public VulkanRenderer {
public:
    Renderer(VulkanContext& ctx, const VulkanRendererOptions& opt) : ctx_(ctx), opt_(opt), dev_(ctx.device()) {}
    ~Renderer() override;
    bool init();

    void clear(GuestMemory& mem, const State& s, uint32_t mask) override;
    void draw(GuestMemory& mem, const State& s, const DrawCommand& d) override;
    void flip(GuestMemory& mem, const State& s, uint32_t buffer) override;
    void sync(GuestMemory&) override { submit_and_wait(); }
    void invalidate(GuestAddr ea, uint32_t size) override;
    VulkanContext::Source front() override {
        std::lock_guard<std::mutex> lk(front_lock_);
        return front_source_;
    }
    void summary() override;

private:
    VulkanContext& ctx_;
    VulkanRendererOptions opt_;
    VkDevice dev_;
    VkPhysicalDeviceMemoryProperties memprops_{};
    VkFormat depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool recording_ = false;

    VkBuffer ring_ = VK_NULL_HANDLE;
    VkDeviceMemory ring_memory_ = VK_NULL_HANDLE;
    uint8_t* ring_map_ = nullptr;
    VkDeviceSize ring_at_ = 0;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    uint8_t* readback_map_ = nullptr;
    VkDeviceSize readback_size_ = 0;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    uint32_t sets_used_ = 0;

    std::map<GuestAddr, std::unique_ptr<Image>> surfaces_;
    std::map<TextureKey, std::unique_ptr<Image>> textures_;
    std::unique_ptr<Image> dummy2d_, dummy3d_, dummy_cube_;
    std::vector<std::unique_ptr<Image>> retired_;  // destroyed after the next submit
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint32_t> vp_inputs_;  // VP hash -> input mask
    std::unordered_set<uint64_t> failed_;                // programs that did not translate
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;

    // Current render targets.
    Image* color_ = nullptr;
    Image* depth_ = nullptr;
    uint32_t target_w_ = 0, target_h_ = 0;
    bool rendering_ = false;

    // The front image the window shows.
    std::mutex front_lock_;
    std::vector<std::unique_ptr<Image>> fronts_;  // never destroyed while running (the window may read them)
    VulkanContext::Source front_source_{};
    uint64_t flips_ = 0;

    // Counters.
    uint64_t draws_ = 0, skipped_ = 0, texture_uploads_ = 0, pipelines_built_ = 0;

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& b, VkDeviceMemory& m, uint8_t** map);
    std::unique_ptr<Image> make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage,
                                      VkImageAspectFlags aspect, const VkComponentMapping& swizzle = {},
                                      VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D, uint32_t depth = 1,
                                      uint32_t layers = 1);
    void destroy(Image& img);

    VkCommandBuffer cmd();
    void submit_and_wait();
    void transition(Image& img, VkImageLayout layout);
    VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align = 16);

    bool bind_targets(const State& s);
    void begin_rendering();
    void end_rendering();

    VkShaderModule module(uint64_t key, const std::vector<uint32_t>& spirv);
    Image* texture(GuestMemory& mem, const Texture& t, uint32_t dims);
    VkSampler sampler(const Texture& t);
    VkPipeline pipeline(const PipelineKey& key);
    void present_from(Image* src, GuestMemory& mem, const DisplayBuffer& db);
};

Renderer::~Renderer() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (auto& [k, p] : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, m] : modules_) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto& [k, s] : samplers_) vkDestroySampler(dev_, s, nullptr);
    for (auto& [k, img] : surfaces_) destroy(*img);
    for (auto& [k, img] : textures_) destroy(*img);
    for (auto& img : retired_) destroy(*img);
    for (auto& img : fronts_) destroy(*img);
    for (Image* img : {dummy2d_.get(), dummy3d_.get(), dummy_cube_.get()})
        if (img) destroy(*img);
    if (descriptor_pool_) vkDestroyDescriptorPool(dev_, descriptor_pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(dev_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (ring_) vkDestroyBuffer(dev_, ring_, nullptr);
    if (ring_memory_) vkFreeMemory(dev_, ring_memory_, nullptr);
    if (readback_) vkDestroyBuffer(dev_, readback_, nullptr);
    if (readback_memory_) vkFreeMemory(dev_, readback_memory_, nullptr);
    if (fence_) vkDestroyFence(dev_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
}

uint32_t Renderer::memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool Renderer::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& b, VkDeviceMemory& m,
                           uint8_t** map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (!ok(vkCreateBuffer(dev_, &bi, nullptr, &b), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !ok(vkAllocateMemory(dev_, &ai, nullptr, &m), "vkAllocateMemory"))
        return false;
    vkBindBufferMemory(dev_, b, m, 0);
    return ok(vkMapMemory(dev_, m, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(map)), "vkMapMemory");
}

std::unique_ptr<Image> Renderer::make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage,
                                            VkImageAspectFlags aspect, const VkComponentMapping& swizzle,
                                            VkImageViewType view_type, uint32_t depth, uint32_t layers) {
    auto img = std::make_unique<Image>();
    img->format = f;
    img->aspect = aspect;
    img->width = w;
    img->height = h;
    img->layers = layers;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, depth};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (!ok(vkCreateImage(dev_, &ci, nullptr, &img->image), "vkCreateImage")) return nullptr;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    if (!ok(vkAllocateMemory(dev_, &ai, nullptr, &img->memory), "vkAllocateMemory")) return nullptr;
    vkBindImageMemory(dev_, img->image, img->memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = view_type;
    vi.format = f;
    vi.components = swizzle;
    vi.subresourceRange = {aspect, 0, 1, 0, layers};
    if (!ok(vkCreateImageView(dev_, &vi, nullptr, &img->view), "vkCreateImageView")) return nullptr;
    return img;
}

void Renderer::destroy(Image& img) {
    if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev_, img.memory, nullptr);
    img = Image{};
}

bool Renderer::init() {
    vkGetPhysicalDeviceMemoryProperties(ctx_.physical_device(), &memprops_);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(ctx_.physical_device(), VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_.queue_family();
    if (!ok(vkCreateCommandPool(dev_, &pci, nullptr, &pool_), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (!ok(vkAllocateCommandBuffers(dev_, &cai, &cmd_), "vkAllocateCommandBuffers")) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!ok(vkCreateFence(dev_, &fi, nullptr, &fence_), "vkCreateFence")) return false;

    if (!make_buffer(kRingSize,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     ring_, ring_memory_, &ring_map_))
        return false;

    VkDescriptorSetLayoutBinding bindings[2 + kTextureUnits]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {shader_if::kFsUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    for (uint32_t t = 0; t < kTextureUnits; ++t)
        bindings[2 + t] = {shader_if::kTextureBinding0 + t, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                           VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2 + kTextureUnits;
    li.pBindings = bindings;
    if (!ok(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout"))
        return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    if (!ok(vkCreatePipelineLayout(dev_, &pli, nullptr, &pipeline_layout_), "vkCreatePipelineLayout")) return false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kMaxSets},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextureUnits * kMaxSets}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kMaxSets;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (!ok(vkCreateDescriptorPool(dev_, &dpi, nullptr, &descriptor_pool_), "vkCreateDescriptorPool")) return false;

    // 1x1 textures for units a program samples but the game left empty.
    const VkImageUsageFlags tex_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummy2d_ = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, tex_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy3d_ = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, tex_usage, VK_IMAGE_ASPECT_COLOR_BIT, {},
                          VK_IMAGE_VIEW_TYPE_3D);
    dummy_cube_ = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, tex_usage, VK_IMAGE_ASPECT_COLOR_BIT, {},
                             VK_IMAGE_VIEW_TYPE_CUBE, 1, 6);
    if (!dummy2d_ || !dummy3d_ || !dummy_cube_) return false;
    for (Image* img : {dummy2d_.get(), dummy3d_.get(), dummy_cube_.get()}) {
        transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img->layers};
        vkCmdClearColorImage(cmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    submit_and_wait();
    SLY4_INFO("RSX/Vulkan: renderizador pronto (profundidade %s)",
              depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    return true;
}

// ---------------------------------------------------------------- commands
VkCommandBuffer Renderer::cmd() {
    if (!recording_) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &bi);
        recording_ = true;
    }
    return cmd_;
}

void Renderer::submit_and_wait() {
    end_rendering();
    if (recording_) {
        vkEndCommandBuffer(cmd_);
        recording_ = false;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd_;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        {
            std::lock_guard<std::mutex> lk(ctx_.queue_lock());
            ok(vkQueueSubmit2(ctx_.queue(), 1, &si, fence_), "vkQueueSubmit2");
        }
        vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(dev_, 1, &fence_);
        vkResetCommandBuffer(cmd_, 0);
    }
    ring_at_ = 0;
    if (sets_used_) {
        vkResetDescriptorPool(dev_, descriptor_pool_, 0);
        sets_used_ = 0;
    }
    for (auto& img : retired_) destroy(*img);
    retired_.clear();
}

void Renderer::transition(Image& img, VkImageLayout layout) {
    if (img.layout == layout) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, 1, 0, img.layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd(), &dep);
    img.layout = layout;
}

VkDeviceSize Renderer::ring_alloc(VkDeviceSize size, VkDeviceSize align) {
    VkDeviceSize at = (ring_at_ + align - 1) & ~(align - 1);
    if (at + size > kRingSize) {
        submit_and_wait();  // everything recorded so far is done with the ring
        at = 0;
    }
    ring_at_ = at + size;
    return at;
}

// ---------------------------------------------------------------- targets
VkFormat surface_color_format(uint32_t f) {
    switch (f) {
        case 3: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 9: return VK_FORMAT_R8_UNORM;
        case 10: return VK_FORMAT_R8G8_UNORM;
        case 11: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 12: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 13: return VK_FORMAT_R32_SFLOAT;
        default: return VK_FORMAT_R8G8B8A8_UNORM;  // the 8-bit-per-channel ones
    }
}

bool Renderer::bind_targets(const State& s) {
    const Surface sf = s.surface();
    const uint32_t w = sf.x + sf.width, h = sf.y + sf.height;
    if (!w || !h || w > 4096 || h > 4096) return false;
    Image* color = nullptr;
    Image* depth = nullptr;
    // SET_SURFACE_COLOR_TARGET: 0 none, 1 A, 2 B, 0x13 A+B, ... (only the first is drawn to).
    const uint32_t t = sf.target & 0x1F;
    const int index = t == 0 ? -1 : t == 2 ? 1 : 0;
    auto find = [&](GuestAddr ea, VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect) -> Image* {
        auto it = surfaces_.find(ea);
        if (it != surfaces_.end() && it->second->format == f && it->second->width == w && it->second->height == h)
            return it->second.get();
        if (it != surfaces_.end()) {
            if (it->second.get() == color_ || it->second.get() == depth_) end_rendering();
            if (color_ == it->second.get()) color_ = nullptr;
            if (depth_ == it->second.get()) depth_ = nullptr;
            retired_.push_back(std::move(it->second));
            surfaces_.erase(it);
        }
        auto img = make_image(w, h, f, usage, aspect);
        if (!img) return nullptr;
        Image* p = img.get();
        surfaces_[ea] = std::move(img);
        return p;
    };
    if (index >= 0) {
        const GuestAddr ea = location_address(sf.color_location[index], sf.color_offset[index]);
        if (ea)
            color = find(ea, surface_color_format(sf.color_format),
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (sf.depth_format) {
        const GuestAddr ea = location_address(sf.depth_location, sf.depth_offset);
        const VkFormat f = sf.depth_format == 1 ? VK_FORMAT_D16_UNORM : depth_format_;
        const VkImageAspectFlags aspect =
            f == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (ea && ea != (color ? location_address(sf.color_location[index], sf.color_offset[index]) : 0))
            depth = find(ea, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, aspect);
    }
    if (!color && !depth) return false;
    if (rendering_ && (color != color_ || depth != depth_)) end_rendering();
    color_ = color;
    depth_ = depth;
    target_w_ = w;
    target_h_ = h;
    return true;
}

void Renderer::begin_rendering() {
    if (rendering_) return;
    if (color_) transition(*color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth_) transition(*depth_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = color_ ? color_->view : VK_NULL_HANDLE;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth_ ? depth_->view : VK_NULL_HANDLE;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {target_w_, target_h_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = color_ ? 1 : 0;
    ri.pColorAttachments = color_ ? &ca : nullptr;
    ri.pDepthAttachment = depth_ ? &da : nullptr;
    ri.pStencilAttachment = depth_ && (depth_->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &da : nullptr;
    vkCmdBeginRendering(cmd(), &ri);
    rendering_ = true;
}

void Renderer::end_rendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cmd_);
    rendering_ = false;
}

// ---------------------------------------------------------------- clear
void Renderer::clear(GuestMemory&, const State& s, uint32_t mask) {
    if (!bind_targets(s)) return;
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (color_ && (mask & 0xF0)) {
        const uint32_t v = s.reg(SetColorClearValue);
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        a.clearValue.color.float32[0] = float((v >> 16) & 0xFF) / 255.0f;
        a.clearValue.color.float32[1] = float((v >> 8) & 0xFF) / 255.0f;
        a.clearValue.color.float32[2] = float(v & 0xFF) / 255.0f;
        a.clearValue.color.float32[3] = float(v >> 24) / 255.0f;
        att[n++] = a;
    }
    if (depth_ && (mask & 3)) {
        const uint32_t v = s.reg(SetZstencilClearValue);
        VkClearAttachment a{};
        if (mask & 1) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((mask & 2) && (depth_->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (depth_->format == VK_FORMAT_D16_UNORM) {
            a.clearValue.depthStencil = {float(v & 0xFFFF) / 65535.0f, 0};
        } else {
            a.clearValue.depthStencil = {float(v >> 8) / 16777215.0f, v & 0xFF};
        }
        if (a.aspectMask) att[n++] = a;
    }
    if (!n) return;
    begin_rendering();
    VkClearRect rect{};
    rect.rect = {{0, 0}, {target_w_, target_h_}};
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd(), n, att, 1, &rect);
}

void Renderer::invalidate(GuestAddr ea, uint32_t size) {
    for (auto& [addr, img] : surfaces_) {
        const uint32_t bytes = img->width * img->height * 4;
        if (ea < addr + bytes && addr < ea + size) img->cpu_dirty = true;
    }
}

// ---------------------------------------------------------------- programs and textures
VkShaderModule Renderer::module(uint64_t key, const std::vector<uint32_t>& spirv) {
    if (auto it = modules_.find(key); it != modules_.end()) return it->second;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (!ok(vkCreateShaderModule(dev_, &ci, nullptr, &m), "vkCreateShaderModule")) m = VK_NULL_HANDLE;
    modules_[key] = m;
    return m;
}

VkSampler Renderer::sampler(const Texture& t) {
    const uint64_t key = (uint64_t(t.filter) << 32) | t.address;
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const uint32_t mag = (t.filter >> 24) & 7, min = (t.filter >> 16) & 7;
    ci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = address_mode(t.address & 0xF);
    ci.addressModeV = address_mode((t.address >> 8) & 0xF);
    ci.addressModeW = address_mode((t.address >> 16) & 0xF);
    ci.maxLod = 0.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    ok(vkCreateSampler(dev_, &ci, nullptr, &s), "vkCreateSampler");
    samplers_[key] = s;
    return s;
}

Image* Renderer::texture(GuestMemory& mem, const Texture& t, uint32_t dims) {
    if (!t.enabled() || !t.width || !t.height || dims != 2) return nullptr;
    const GuestAddr ea = location_address(t.location, t.offset);
    if (!ea) return nullptr;
    // A texture that is a surface we rendered: sample the surface itself.
    if (auto it = surfaces_.find(ea); it != surfaces_.end() && !(it->second->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        Image* img = it->second.get();
        if (img != color_) {
            if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                end_rendering();
                transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            return img;
        }
    }
    const TextureKey key{ea, t.format, t.width, t.height, t.pitch, t.control1 & 0xFFFF, t.dimension};
    const uint32_t size = texture_source_size(t);
    if (!size || size > 64u << 20) return nullptr;
    const uint64_t hash = fnv1a64(mem.host(ea), size);
    auto& slot = textures_[key];
    if (slot && slot->hash == hash) return slot.get();
    const TextureData data = decode_texture(mem.host(ea), t);
    if (!slot || slot->format != data.format) {
        if (slot) retired_.push_back(std::move(slot));
        slot = make_image(data.width, data.height, data.format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          data.swizzle);
        if (!slot) return nullptr;
    }
    end_rendering();
    const VkDeviceSize at = ring_alloc(data.bytes.size(), 16);
    std::memcpy(ring_map_ + at, data.bytes.data(), data.bytes.size());
    transition(*slot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {data.width, data.height, 1};
    vkCmdCopyBufferToImage(cmd(), ring_, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(*slot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    slot->hash = hash;
    ++texture_uploads_;
    return slot.get();
}

VkPipeline Renderer::pipeline(const PipelineKey& k) {
    if (auto it = pipelines_.find(k); it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, k.vs, "main",
                 nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k.fs,
                 "main", nullptr};
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(k.inputs & (1u << i))) continue;
        binds.push_back({i, (k.constant_inputs & (1u << i)) ? 0u : 16u, VK_VERTEX_INPUT_RATE_VERTEX});
        attrs.push_back({i, i, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VkPrimitiveTopology(k.topology);
    ia.primitiveRestartEnable = k.restart && (k.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                                              k.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                                              k.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depth != VK_FORMAT_UNDEFINED && k.depth_test;
    ds.depthWriteEnable = k.depth != VK_FORMAT_UNDEFINED && k.depth_test && k.depth_write;
    ds.depthCompareOp = compare_op(k.depth_func);
    ds.stencilTestEnable = k.depth != VK_FORMAT_UNDEFINED && k.depth != VK_FORMAT_D16_UNORM && k.stencil[0];
    VkStencilOpState so{};
    so.failOp = stencil_op(k.stencil[4]);
    so.depthFailOp = stencil_op(k.stencil[5]);
    so.passOp = stencil_op(k.stencil[6]);
    so.compareOp = compare_op(k.stencil[1]);
    so.compareMask = k.stencil[2] & 0xFF;
    so.writeMask = k.stencil[3] & 0xFF;
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.blend_enable;
    cb.srcColorBlendFactor = blend_factor(k.blend_sf & 0xFFFF);
    cb.srcAlphaBlendFactor = blend_factor(k.blend_sf >> 16);
    cb.dstColorBlendFactor = blend_factor(k.blend_df & 0xFFFF);
    cb.dstAlphaBlendFactor = blend_factor(k.blend_df >> 16);
    cb.colorBlendOp = blend_op(k.blend_eq & 0xFFFF);
    cb.alphaBlendOp = blend_op(k.blend_eq >> 16);
    // SET_COLOR_MASK: B bit 0, G bit 8, R bit 16, A bit 24.
    cb.colorWriteMask = ((k.color_mask >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) |
                        ((k.color_mask >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (k.color_mask & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) |
                        ((k.color_mask >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    cbs.pAttachments = &cb;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = k.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    pr.pColorAttachmentFormats = &k.color;
    pr.depthAttachmentFormat = k.depth;
    pr.stencilAttachmentFormat = (k.depth != VK_FORMAT_UNDEFINED && k.depth != VK_FORMAT_D16_UNORM) ? k.depth
                                                                                                   : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &pr;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = pipeline_layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (!ok(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines"))
        p = VK_NULL_HANDLE;
    pipelines_[k] = p;
    ++pipelines_built_;
    return p;
}

// ---------------------------------------------------------------- draw
void Renderer::draw(GuestMemory& mem, const State& s, const DrawCommand& d) {
    ++draws_;
    auto skip = [&] { ++skipped_; };
    if (!bind_targets(s)) return skip();

    // Vertex program.
    const uint32_t vp_start = s.vertex_program_start();
    const VertexProgramInfo vpi = scan_vertex_program(s.transform_program.data(), vp_start);
    const uint64_t vs_key = vpi.hash ^ 0x5653;
    if (failed_.count(vs_key)) return skip();
    VkShaderModule vs = VK_NULL_HANDLE;
    if (auto it = modules_.find(vs_key); it != modules_.end()) {
        vs = it->second;
    } else {
        const ShaderResult r = translate_vertex_program(s.transform_program.data(), vp_start);
        if (!r.ok) {
            SLY4_WARN("RSX: %s", r.error.c_str());
            failed_.insert(vs_key);
            return skip();
        }
        vs = module(vs_key, r.spirv);
    }
    if (!vs) return skip();

    // Fragment program (read from guest memory every draw: its constants change in place).
    const GuestAddr fp_ea = location_address(s.fragment_program_location(), s.fragment_program_offset());
    if (!fp_ea) return skip();
    const uint8_t* fp = mem.host(fp_ea);
    const FragmentProgramInfo fpi = scan_fragment_program(fp, 32768);
    FragmentProgramKey fk{};
    fk.shader_control = s.reg(SetShaderControl);
    if (s.reg(SetAlphaTestEnable)) fk.alpha_func = s.reg(SetAlphaFunc);
    for (uint32_t t = 0; t < 16; ++t)
        if (fpi.textures & (1u << t)) {
            const Texture tex = s.texture(t);
            fk.texture_dims[t] = tex.cubemap ? 6 : tex.dimension == 3 ? 3 : 2;
        }
    const uint64_t fs_key = fnv1a64(&fk, sizeof(fk), fpi.hash);
    if (failed_.count(fs_key)) return skip();
    VkShaderModule fs = VK_NULL_HANDLE;
    if (auto it = modules_.find(fs_key); it != modules_.end()) {
        fs = it->second;
    } else {
        const ShaderResult r = translate_fragment_program(fp, fpi, fk);
        if (!r.ok) {
            SLY4_WARN("RSX: %s", r.error.c_str());
            failed_.insert(fs_key);
            return skip();
        }
        fs = module(fs_key, r.spirv);
    }
    if (!fs) return skip();

    // Textures (uploads happen outside of rendering).
    VkDescriptorImageInfo image_infos[kTextureUnits]{};
    for (uint32_t t = 0; t < kTextureUnits; ++t) {
        if (!(fpi.textures & (1u << t))) continue;
        const Texture tex = s.texture(t);
        Image* img = texture(mem, tex, fk.texture_dims[t]);
        if (!img) img = fk.texture_dims[t] == 6 ? dummy_cube_.get() : fk.texture_dims[t] == 3 ? dummy3d_.get()
                                                                                                : dummy2d_.get();
        image_infos[t] = {sampler(tex), img->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    // Vertex data: every input as float4 (constant inputs: one value, stride 0).
    const uint32_t base_offset = s.reg(SetVertexDataBaseOffset), base_index = s.reg(SetVertexDataBaseIndex);
    std::vector<uint32_t> indices;  // when the draw needs an index buffer
    uint32_t first = 0, count = 0;
    bool indexed = false;
    Primitive prim = d.primitive;
    if (d.kind == DrawCommand::Kind::Indexed) {
        const GuestAddr ib = location_address(d.index_location, d.index_offset);
        if (!ib) return skip();
        const bool restart = s.reg(SetRestartIndexEnable) & 1;
        const uint32_t restart_index = s.reg(SetRestartIndex);
        uint32_t lo = UINT32_MAX, hi = 0;
        for (const auto& [f, n] : d.ranges)
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t idx = d.index_16bit ? load_be<uint16_t>(mem.host(ib + (f + i) * 2))
                                                   : load_be<uint32_t>(mem.host(ib + (f + i) * 4));
                if (restart && idx == restart_index) {
                    indices.push_back(UINT32_MAX);
                    continue;
                }
                indices.push_back(idx + base_index);
                lo = std::min(lo, idx + base_index);
                hi = std::max(hi, idx + base_index);
            }
        if (lo > hi || hi - lo > (1u << 22)) return skip();
        for (uint32_t& i : indices)
            if (i != UINT32_MAX) i -= lo;
        first = lo;
        count = hi - lo + 1;
        indexed = true;
    } else if (d.kind == DrawCommand::Kind::Arrays) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (const auto& [f, n] : d.ranges) {
            lo = std::min(lo, f);
            hi = std::max(hi, f + n);
        }
        if (lo >= hi) return skip();
        first = lo;
        count = hi - lo;
        if (d.ranges.size() > 1) {  // several ranges: draw them through indices
            for (const auto& [f, n] : d.ranges)
                for (uint32_t i = 0; i < n; ++i) indices.push_back(f + i - lo);
            indexed = true;
        }
    }

    // Inline arrays: the enabled arrays' formats, interleaved, from the FIFO data.
    uint32_t inline_stride = 0;
    if (d.kind == DrawCommand::Kind::Inline) {
        for (uint32_t i = 0; i < 16; ++i) {
            const VertexArray a = s.vertex_array(i);
            if (a.size) inline_stride += (attribute_bytes(a) + 3) & ~3u;
        }
        if (!inline_stride) return skip();
        first = 0;
        count = uint32_t(d.inline_data.size() * 4 / inline_stride);
        if (!count) return skip();
    }

    uint32_t constant_inputs = 0;
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    uint32_t nbind = 0, inline_at = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const VertexArray a = s.vertex_array(i);
        const bool used = vpi.inputs & (1u << i);
        if (d.kind == DrawCommand::Kind::Inline && a.size) {
            if (used) {
                const VkDeviceSize at = ring_alloc(VkDeviceSize(count) * 16, 16);
                auto* out = reinterpret_cast<float*>(ring_map_ + at);
                for (uint32_t v = 0; v < count; ++v) {
                    uint8_t tmp[16];
                    for (uint32_t w = 0; w < 4; ++w) {
                        const size_t word = (size_t(v) * inline_stride + inline_at) / 4 + w;
                        store_be<uint32_t>(tmp + 4 * w, word < d.inline_data.size() ? d.inline_data[word] : 0);
                    }
                    convert_attribute(a, tmp, out + 4 * v);
                }
                vbufs[nbind] = ring_;
                voffs[nbind++] = at;
            }
            inline_at += (attribute_bytes(a) + 3) & ~3u;
            continue;
        }
        if (!used) continue;
        if (a.size && d.kind != DrawCommand::Kind::Inline) {
            const GuestAddr base = location_address(a.location, a.offset + base_offset);
            if (!base) return skip();
            const VkDeviceSize at = ring_alloc(VkDeviceSize(count) * 16, 16);
            auto* out = reinterpret_cast<float*>(ring_map_ + at);
            const uint32_t stride = a.stride;
            for (uint32_t v = 0; v < count; ++v)
                convert_attribute(a, mem.host(base + (first + v) * stride), out + 4 * v);
            vbufs[nbind] = ring_;
            voffs[nbind++] = at;
        } else {
            const VkDeviceSize at = ring_alloc(16, 16);
            std::memcpy(ring_map_ + at, &s.vertex_data[i * 4], 16);
            vbufs[nbind] = ring_;
            voffs[nbind++] = at;
            constant_inputs |= 1u << i;
        }
    }

    // Primitives Vulkan does not have become index lists.
    auto expand = [&](auto&& gen) {
        std::vector<uint32_t> src = indexed ? indices : std::vector<uint32_t>();
        if (!indexed)
            for (uint32_t i = 0; i < count; ++i) src.push_back(i);
        indices.clear();
        gen(src);
        indexed = true;
    };
    VkPrimitiveTopology topo;
    switch (prim) {
        case Primitive::Points: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case Primitive::Lines: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case Primitive::LineStrip: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case Primitive::LineLoop:
            topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            expand([&](const std::vector<uint32_t>& src) {
                indices = src;
                if (!src.empty()) indices.push_back(src[0]);
            });
            break;
        case Primitive::Triangles: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case Primitive::TriangleStrip: case Primitive::QuadStrip: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case Primitive::TriangleFan: case Primitive::Polygon: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        case Primitive::Quads:
            topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            expand([&](const std::vector<uint32_t>& src) {
                for (size_t q = 0; q + 3 < src.size(); q += 4)
                    for (size_t k : {0, 1, 2, 0, 2, 3}) indices.push_back(src[q + k]);
            });
            break;
        default: return skip();
    }

    // Uniforms.
    const VkDeviceSize vs_size = (shader_if::kVsConstants + 3) * 16;
    const VkDeviceSize vs_at = ring_alloc(vs_size, 256);
    std::memcpy(ring_map_ + vs_at, s.transform_constants.data(), shader_if::kVsConstants * 16);
    float* extra = reinterpret_cast<float*>(ring_map_ + vs_at + shader_if::kVsConstants * 16);
    for (int i = 0; i < 4; ++i) {
        extra[i] = s.regf(SetViewportScale + 4 * i);
        extra[4 + i] = s.regf(SetViewportOffset + 4 * i);
    }
    extra[8] = float(target_w_);
    extra[9] = float(target_h_);
    extra[10] = extra[11] = 0.0f;
    const uint32_t nconst = uint32_t(fpi.constant_offsets.size());
    const VkDeviceSize fs_size = VkDeviceSize((nconst ? nconst : 1) + 1) * 16;
    const VkDeviceSize fs_at = ring_alloc(fs_size, 256);
    auto* fc = reinterpret_cast<uint32_t*>(ring_map_ + fs_at);
    for (uint32_t c = 0; c < nconst; ++c)
        for (uint32_t w = 0; w < 4; ++w) {
            const uint32_t raw = load_be<uint32_t>(fp + fpi.constant_offsets[c] + 4 * w);
            fc[c * 4 + w] = (raw << 16) | (raw >> 16);
        }
    float* alpha = reinterpret_cast<float*>(fc + (nconst ? nconst : 1) * 4);
    alpha[0] = float(s.reg(SetAlphaRef) & 0xFF) / 255.0f;
    alpha[1] = alpha[2] = alpha[3] = 0.0f;
    VkDeviceSize ib_at = 0;
    if (indexed) {
        ib_at = ring_alloc(indices.size() * 4, 16);
        std::memcpy(ring_map_ + ib_at, indices.data(), indices.size() * 4);
    }

    // Pipeline.
    PipelineKey k;
    std::memset(&k, 0, sizeof(k));  // compared and hashed as bytes, padding included
    k.vs = vs;
    k.fs = fs;
    k.topology = topo;
    k.inputs = vpi.inputs;
    k.constant_inputs = constant_inputs;
    k.color = color_ ? color_->format : VK_FORMAT_UNDEFINED;
    k.depth = depth_ ? depth_->format : VK_FORMAT_UNDEFINED;
    k.blend_enable = s.reg(SetBlendEnable) & 1;
    if (k.blend_enable) {
        k.blend_sf = s.reg(SetBlendFuncSfactor);
        k.blend_df = s.reg(SetBlendFuncDfactor);
        k.blend_eq = s.reg(SetBlendEquation);
    }
    k.color_mask = s.reg(SetColorMask);
    k.depth_test = s.reg(SetDepthTestEnable) & 1;
    k.depth_write = s.reg(SetDepthMask) & 1;
    k.depth_func = s.reg(SetDepthFunc);
    k.stencil[0] = s.reg(SetStencilTestEnable) & 1;
    if (k.stencil[0]) {
        k.stencil[1] = s.reg(SetStencilFunc);
        k.stencil[2] = s.reg(SetStencilFuncMask);
        k.stencil[3] = s.reg(SetStencilMask);
        k.stencil[4] = s.reg(SetStencilOpFail);
        k.stencil[5] = s.reg(SetStencilOpZfail);
        k.stencil[6] = s.reg(SetStencilOpZpass);
    }
    k.restart = indexed;
    const VkPipeline p = pipeline(k);
    if (!p) return skip();

    // Descriptors.
    if (sets_used_ >= kMaxSets) submit_and_wait();
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptor_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!ok(vkAllocateDescriptorSets(dev_, &dai, &set), "vkAllocateDescriptorSets")) return skip();
    ++sets_used_;
    VkDescriptorBufferInfo bufs[2] = {{ring_, vs_at, vs_size}, {ring_, fs_at, fs_size}};
    VkWriteDescriptorSet writes[2 + kTextureUnits]{};
    uint32_t nw = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        VkWriteDescriptorSet& w = writes[nw++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = i == 0 ? 0 : shader_if::kFsUniformBinding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufs[i];
    }
    for (uint32_t t = 0; t < kTextureUnits; ++t) {
        if (!(fpi.textures & (1u << t))) continue;
        VkWriteDescriptorSet& w = writes[nw++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = shader_if::kTextureBinding0 + t;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image_infos[t];
    }
    vkUpdateDescriptorSets(dev_, nw, writes, 0, nullptr);

    // Record.
    begin_rendering();
    VkCommandBuffer c = cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    const VkViewport viewport{0, 0, float(target_w_), float(target_h_), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &viewport);
    const uint32_t sh = s.reg(SetScissorHorizontal), sv = s.reg(SetScissorVertical);
    int32_t sx = int32_t(sh & 0xFFFF), sy = int32_t(sv & 0xFFFF);
    uint32_t sw = sh >> 16, shh = sv >> 16;
    if (!sw || !shh) {
        sx = sy = 0;
        sw = target_w_;
        shh = target_h_;
    }
    sw = std::min(sw, target_w_ > uint32_t(sx) ? target_w_ - uint32_t(sx) : 0);
    shh = std::min(shh, target_h_ > uint32_t(sy) ? target_h_ - uint32_t(sy) : 0);
    const VkRect2D scissor{{sx, sy}, {sw, shh}};
    vkCmdSetScissor(c, 0, 1, &scissor);
    const uint32_t bc = s.reg(SetBlendColor);
    const float blend_constants[4] = {float((bc >> 16) & 0xFF) / 255.0f, float((bc >> 8) & 0xFF) / 255.0f,
                                      float(bc & 0xFF) / 255.0f, float(bc >> 24) / 255.0f};
    vkCmdSetBlendConstants(c, blend_constants);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, s.reg(SetStencilFuncRef) & 0xFF);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
    if (nbind) {
        // Bindings are the input indices; bind them one by one (they are not contiguous).
        uint32_t n = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            if (!(vpi.inputs & (1u << i))) continue;
            vkCmdBindVertexBuffers(c, i, 1, &vbufs[n], &voffs[n]);
            ++n;
        }
    }
    if (indexed) {
        vkCmdBindIndexBuffer(c, ring_, ib_at, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, uint32_t(indices.size()), 1, 0, 0, 0);
    } else {
        vkCmdDraw(c, count, 1, 0, 0);
    }
}

// ---------------------------------------------------------------- flip
void Renderer::present_from(Image* src, GuestMemory& mem, const DisplayBuffer& db) {
    const uint32_t w = db.width, h = db.height;
    std::lock_guard<std::mutex> lk(front_lock_);
    Image* front = fronts_.empty() ? nullptr : fronts_.back().get();
    if (!front || front->width != w || front->height != h) {
        auto img = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
        if (!img) return;
        front = img.get();
        fronts_.push_back(std::move(img));
    }
    transition(*front, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src) {
        transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(std::min(w, src->width)), int32_t(std::min(h, src->height)), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(std::min(w, src->width)), int32_t(std::min(h, src->height)), 1};
        vkCmdBlitImage(cmd(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, front->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    } else {
        // Nothing rendered there: the display buffer as the CPU left it (A8R8G8B8).
        const VkDeviceSize at = ring_alloc(VkDeviceSize(w) * h * 4, 16);
        const uint8_t* p = mem.host(kLocalBase + db.offset);
        uint8_t* o = ring_map_ + at;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* px = p + size_t(y) * db.pitch + x * 4;
                uint8_t* q = o + (size_t(y) * w + x) * 4;
                q[0] = px[1];
                q[1] = px[2];
                q[2] = px[3];
                q[3] = 255;
            }
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd(), ring_, front->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    transition(*front, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    front_source_ = {front->image, w, h};
}

void Renderer::flip(GuestMemory& mem, const State&, uint32_t buffer) {
    ++flips_;
    end_rendering();
    const DisplayBuffer db = display_buffer(buffer);
    if (!db.width || !db.height) return;
    Image* src = nullptr;
    if (auto it = surfaces_.find(kLocalBase + db.offset); it != surfaces_.end() && !it->second->cpu_dirty)
        src = it->second.get();
    present_from(src, mem, db);

    const bool dump = !opt_.dump_dir.empty() && opt_.dump_every && flips_ % opt_.dump_every == 0;
    VkDeviceSize bytes = VkDeviceSize(db.width) * db.height * 4;
    if (dump) {
        if (readback_size_ < bytes) {
            if (readback_) {
                submit_and_wait();
                vkDestroyBuffer(dev_, readback_, nullptr);
                vkFreeMemory(dev_, readback_memory_, nullptr);
            }
            make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_, readback_memory_, &readback_map_);
            readback_size_ = bytes;
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {db.width, db.height, 1};
        vkCmdCopyImageToBuffer(cmd(), front_source_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
    }
    submit_and_wait();
    if (dump) {
        std::error_code ec;
        std::filesystem::create_directories(opt_.dump_dir, ec);
        char name[64];
        std::snprintf(name, sizeof(name), "/frame_%06" PRIu64 ".png", flips_);
        std::vector<uint8_t> px(readback_map_, readback_map_ + bytes);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        write_png(opt_.dump_dir + name, px.data(), db.width, db.height);
    }
}

void Renderer::summary() {
    SLY4_INFO("RSX/Vulkan: %" PRIu64 " draws (%" PRIu64 " não desenhados), %zu pipelines, %zu programas, %" PRIu64
              " texturas enviadas, %zu superfícies",
              draws_, skipped_, pipelines_.size(), modules_.size(), texture_uploads_, surfaces_.size());
}

}  // namespace

std::unique_ptr<VulkanRenderer> create_vulkan_renderer(VulkanContext& ctx, const VulkanRendererOptions& opt) {
    auto r = std::make_unique<Renderer>(ctx, opt);
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace sly4::rsx
