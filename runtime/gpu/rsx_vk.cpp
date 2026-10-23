#include "gpu/rsx_vk.h"

#include <algorithm>
#include <array>
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
// Whether the RSX winding is mirrored in Vulkan's framebuffer coordinates (see draw()).
constexpr bool kMirroredWinding = false;

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
    uint32_t width = 0, height = 0, layers = 1, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t hash = 0;          // textures: hash of the guest bytes uploaded
    uint64_t hashed_flip = ~0ull;  // textures: the frame the bytes were last hashed in
    GuestAddr src = 0;
    uint32_t src_size = 0;      // textures: the guest bytes they come from
    // Surfaces: where the up-to-date copy is. gpu_dirty: drawn since guest memory was last
    // written from it; cpu_dirty: guest memory written behind the GPU's back (2D copies).
    bool cpu_dirty = false, gpu_dirty = false;
    uint32_t pitch = 0, guest_format = 0;  // color surfaces: bytes per line, SET_SURFACE_FORMAT color
    uint64_t writes = 0;  // depth surfaces: draws and clears that wrote them (conversions follow it)
};

// Texture change detection: 8 bytes at a time (FNV-1a byte by byte was 40% of the RSX
// thread in the menu, with every texture hashed again on every draw).
uint64_t hash_bytes(const uint8_t* p, size_t n) {
    // Four independent lanes of 8 bytes per round (in the style of xxHash), so the
    // multiplies overlap; then the tail.
    constexpr uint64_t k1 = 0x9E3779B185EBCA87ull, k2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t acc[4] = {k1 ^ n, k2, n * k1, ~k2};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; ++l) {
            uint64_t v;
            std::memcpy(&v, p + i + 8 * l, 8);
            acc[l] = (acc[l] + v * k2);
            acc[l] = ((acc[l] << 31) | (acc[l] >> 33)) * k1;
        }
    uint64_t h = acc[0] ^ (acc[1] * k1) ^ (acc[2] * k2) ^ ((acc[3] << 17) | (acc[3] >> 47));
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// Byte offsets of R, G, B and A in a pixel of a 32-bit color surface in guest memory, or
// false for the formats that are not kept in sync with memory (16-bit, float, B8, G8B8).
bool surface_byte_order(uint32_t format, uint32_t (&o)[4]) {
    static constexpr uint32_t argb[4] = {1, 2, 3, 0}, abgr[4] = {3, 2, 1, 0};
    switch (format) {
        case 4: case 5: case 8: std::memcpy(o, argb, sizeof(o)); return true;     // X8R8G8B8, A8R8G8B8
        case 14: case 15: case 16: std::memcpy(o, abgr, sizeof(o)); return true;  // X8B8G8R8, A8B8G8R8
        default: return false;
    }
}

// A depth texture (DEPTH24_D8, DEPTH16 and their float forms) with a compare function in
// TEXTURE_ADDRESS (bits 28-30): a shadow map, sampled with a comparison.
bool shadow_texture(const Texture& t) {
    const uint32_t f = t.base_format();
    return f >= 0x90 && f <= 0x93 && ((t.address >> 28) & 7) != 0;
}

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
    uint32_t cull, front_face;  // VkCullModeFlags, VkFrontFace
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

void convert_attribute(const VertexArray& a, const uint8_t* p, float out[4]);

// Big-endian floats with N components, a whole array: the most common attribute, and the
// per-vertex switch of convert_attribute was 10% of the RSX thread in the menu.
template <uint32_t N>
void convert_floats(const uint8_t* p, uint32_t stride, uint32_t count, float* out) {
    for (uint32_t v = 0; v < count; ++v, p += stride, out += 4) {
        for (uint32_t i = 0; i < N; ++i) {
            const uint32_t b = load_be<uint32_t>(p + 4 * i);
            std::memcpy(&out[i], &b, 4);
        }
        for (uint32_t i = N; i < 4; ++i) out[i] = i == 3 ? 1.0f : 0.0f;
    }
}

// count vertices of one attribute array (stride bytes apart) to float4.
void convert_array(const VertexArray& a, const uint8_t* p, uint32_t stride, uint32_t count, float* out) {
    if (a.type == VertexType::F) {
        switch (std::min(a.size, 4u)) {
            case 1: return convert_floats<1>(p, stride, count, out);
            case 2: return convert_floats<2>(p, stride, count, out);
            case 3: return convert_floats<3>(p, stride, count, out);
            case 4: return convert_floats<4>(p, stride, count, out);
            default: break;
        }
    }
    for (uint32_t v = 0; v < count; ++v) convert_attribute(a, p + size_t(v) * stride, out + 4 * v);
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
    void flush(GuestMemory& mem, GuestAddr ea, uint32_t size) override;
    bool scaled_copy(GuestMemory& mem, const ScaledCopy& c) override;
    Image* surface_holding(GuestAddr ea, uint32_t pitch, uint32_t& x, uint32_t& y, GuestAddr& base);
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
    VkBuffer convert_ = VK_NULL_HANDLE;  // depth -> color conversions, on the GPU
    VkDeviceMemory convert_memory_ = VK_NULL_HANDLE;
    VkDeviceSize convert_size_ = 0;
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
    std::unique_ptr<Image> dummy2d_, dummy3d_, dummy_cube_, dummy_depth_;
    std::vector<std::unique_ptr<Image>> retired_;  // destroyed after the next submit
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint32_t> vp_inputs_;  // VP hash -> input mask
    std::unordered_set<uint64_t> failed_;                // programs that did not translate
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::map<std::array<uint32_t, 3>, VkSampler> samplers_;  // filter, address, LOD bits of CONTROL0

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
    uint64_t draws_ = 0, skipped_ = 0, texture_uploads_ = 0, pipelines_built_ = 0, flushes_ = 0, gpu_copies_ = 0, hashed_bytes_ = 0, converted_vertices_ = 0;

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& b, VkDeviceMemory& m, uint8_t** map,
                     bool cached = false);
    std::unique_ptr<Image> make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage,
                                      VkImageAspectFlags aspect, const VkComponentMapping& swizzle = {},
                                      VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D, uint32_t depth = 1,
                                      uint32_t layers = 1, uint32_t levels = 1);
    void destroy(Image& img);

    VkCommandBuffer cmd();
    void submit_and_wait();
    void transition(Image& img, VkImageLayout layout);
    VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align = 16);

    bool bind_targets(GuestMemory& mem, const State& s);
    bool ensure_readback(VkDeviceSize bytes);
    void flush_depth(GuestMemory& mem, GuestAddr addr, Image& img);
    // Guest memory -> a color surface written by the CPU (cpu_dirty).
    void upload_surface(GuestMemory& mem, GuestAddr ea, Image& img);
    static uint32_t surface_bytes(const Image& img) { return (img.pitch ? img.pitch : img.width * 4) * img.height; }
    void begin_rendering();
    void end_rendering();

    VkShaderModule module(uint64_t key, const std::vector<uint32_t>& spirv);
    Image* texture(GuestMemory& mem, const Texture& t, uint32_t dims);
    Image* depth_texture(const Texture& t);
    Image* depth_as_color(const Texture& t);
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
    for (Image* img : {dummy2d_.get(), dummy3d_.get(), dummy_cube_.get(), dummy_depth_.get()})
        if (img) destroy(*img);
    if (descriptor_pool_) vkDestroyDescriptorPool(dev_, descriptor_pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(dev_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (ring_) vkDestroyBuffer(dev_, ring_, nullptr);
    if (ring_memory_) vkFreeMemory(dev_, ring_memory_, nullptr);
    if (convert_) vkDestroyBuffer(dev_, convert_, nullptr);
    if (convert_memory_) vkFreeMemory(dev_, convert_memory_, nullptr);
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
                           uint8_t** map, bool cached) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (!ok(vkCreateBuffer(dev_, &bi, nullptr, &b), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    // What the CPU reads back wants cached memory: uncached, each byte read costs a trip over
    // the bus (reading a depth buffer back took ~0.2 s).
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.memoryTypeIndex = cached ? memory_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : UINT32_MAX;
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = memory_type(req.memoryTypeBits, host);
    if (ai.memoryTypeIndex == UINT32_MAX || !ok(vkAllocateMemory(dev_, &ai, nullptr, &m), "vkAllocateMemory"))
        return false;
    vkBindBufferMemory(dev_, b, m, 0);
    return ok(vkMapMemory(dev_, m, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(map)), "vkMapMemory");
}

std::unique_ptr<Image> Renderer::make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage,
                                            VkImageAspectFlags aspect, const VkComponentMapping& swizzle,
                                            VkImageViewType view_type, uint32_t depth, uint32_t layers,
                                            uint32_t levels) {
    auto img = std::make_unique<Image>();
    img->format = f;
    img->aspect = aspect;
    img->width = w;
    img->height = h;
    img->layers = layers;
    img->levels = levels;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, depth};
    ci.mipLevels = levels;
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
    vi.subresourceRange = {aspect, 0, levels, 0, layers};
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
    // A shadow map nobody drew: far away everywhere (depth 1).
    dummy_depth_ = make_image(1, 1, VK_FORMAT_D16_UNORM, tex_usage, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!dummy2d_ || !dummy3d_ || !dummy_cube_ || !dummy_depth_) return false;
    transition(*dummy_depth_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue far_depth{1.0f, 0};
    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), dummy_depth_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1,
                                &depth_range);
    transition(*dummy_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    b.subresourceRange = {img.aspect, 0, img.levels, 0, img.layers};
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

bool Renderer::bind_targets(GuestMemory& mem, const State& s) {
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
        if (color) {
            color->pitch = sf.color_pitch[index];
            color->guest_format = sf.color_format;
            if (color->cpu_dirty) upload_surface(mem, ea, *color);
        }
    }
    if (sf.depth_format) {
        const GuestAddr ea = location_address(sf.depth_location, sf.depth_offset);
        const VkFormat f = sf.depth_format == 1 ? VK_FORMAT_D16_UNORM : depth_format_;
        const VkImageAspectFlags aspect =
            f == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (ea && ea != (color ? location_address(sf.color_location[index], sf.color_offset[index]) : 0))
            depth = find(ea, f,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         aspect);
        if (depth) {
            depth->pitch = sf.depth_pitch;
            depth->guest_format = sf.depth_format;
        }
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
    if (color_) color_->gpu_dirty = true;
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
void Renderer::clear(GuestMemory& mem, const State& s, uint32_t mask) {
    if (!bind_targets(mem, s)) return;
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
    if (depth_ && (mask & 3)) {
        depth_->gpu_dirty = true;
        ++depth_->writes;
    }
    VkClearRect rect{};
    rect.rect = {{0, 0}, {target_w_, target_h_}};
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd(), n, att, 1, &rect);
}

void Renderer::invalidate(GuestAddr ea, uint32_t size) {
    for (auto& [addr, img] : surfaces_)
        if (!(img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && ea < addr + surface_bytes(*img) && addr < ea + size)
            img->cpu_dirty = true;
    for (auto& [key, img] : textures_)
        if (img && ea < img->src + img->src_size && img->src < ea + size) img->hashed_flip = ~0ull;
}

bool Renderer::ensure_readback(VkDeviceSize bytes) {
    if (readback_size_ >= bytes) return true;
    if (readback_) {
        submit_and_wait();
        vkDestroyBuffer(dev_, readback_, nullptr);
        vkFreeMemory(dev_, readback_memory_, nullptr);
        readback_ = VK_NULL_HANDLE;
        readback_size_ = 0;
    }
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_, readback_memory_, &readback_map_, true))
        return false;
    readback_size_ = bytes;
    return true;
}

// Surfaces the GPU drew in [ea, ea + size) are read back into guest memory.
void Renderer::flush(GuestMemory& mem, GuestAddr ea, uint32_t size) {
    for (auto& [addr, ptr] : surfaces_) {
        Image& img = *ptr;
        if (!img.gpu_dirty || !(ea < addr + surface_bytes(img) && addr < ea + size)) continue;
        img.gpu_dirty = false;
        ++flushes_;
        for (auto& [key, tex] : textures_)  // read from these bytes before: hashed again
            if (tex && addr < tex->src + tex->src_size && tex->src < addr + surface_bytes(img)) tex->hashed_flip = ~0ull;
        if (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
            flush_depth(mem, addr, img);
            continue;
        }
        uint32_t o[4];
        if (!surface_byte_order(img.guest_format, o) || img.format != VK_FORMAT_R8G8B8A8_UNORM ||
            img.pitch < img.width * 4)
            continue;
        end_rendering();
        if (!ensure_readback(VkDeviceSize(img.width) * img.height * 4)) return;
        transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {img.width, img.height, 1};
        vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
        submit_and_wait();
        for (uint32_t y = 0; y < img.height; ++y) {
            const uint8_t* q = readback_map_ + size_t(y) * img.width * 4;
            uint8_t* p = mem.host(addr + y * img.pitch);
            for (uint32_t x = 0; x < img.width; ++x, q += 4, p += 4)
                for (int c = 0; c < 4; ++c) p[o[c]] = q[c];
        }
    }
}

// A depth surface in guest memory as the RSX keeps it: big-endian 16-bit words (Z16) or
// 32-bit words with the depth in the upper 24 bits and the stencil in the lower 8 (Z24S8).
// The game reads them back as textures: the main depth as A8R8G8B8 for the menu's
// post-processing, the shadow maps as DEPTH16 and G8B8.
void Renderer::flush_depth(GuestMemory& mem, GuestAddr addr, Image& img) {
    const bool z16 = img.format == VK_FORMAT_D16_UNORM;
    const uint32_t bpp = z16 ? 2 : 4;
    if (img.pitch < img.width * bpp) return;
    end_rendering();
    const VkDeviceSize pixels = VkDeviceSize(img.width) * img.height;
    // Depth: 2 bytes a pixel (D16), 4 (D24 in the low 24 bits, or D32 float); stencil: 1.
    if (!ensure_readback(pixels * (z16 ? 2 : 5))) return;
    transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copies[2]{};
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copies[0].imageExtent = {img.width, img.height, 1};
    copies[1] = copies[0];
    copies[1].bufferOffset = pixels * 4;
    copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, z16 ? 1 : 2, copies);
    submit_and_wait();
    const bool d32f = img.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (uint32_t y = 0; y < img.height; ++y) {
        uint8_t* p = mem.host(addr + y * img.pitch);
        const size_t row = size_t(y) * img.width;
        if (z16) {
            const uint8_t* q = readback_map_ + row * 2;
            for (uint32_t x = 0; x < img.width; ++x) {
                uint16_t d;
                std::memcpy(&d, q + 2 * x, 2);
                store_be<uint16_t>(p + 2 * x, d);
            }
            continue;
        }
        const uint8_t* q = readback_map_ + row * 4;
        const uint8_t* st = readback_map_ + pixels * 4 + row;
        for (uint32_t x = 0; x < img.width; ++x) {
            uint32_t d;
            std::memcpy(&d, q + 4 * x, 4);
            if (d32f) {
                float f;
                std::memcpy(&f, &d, 4);
                d = uint32_t(std::clamp(f, 0.0f, 1.0f) * 16777215.0f + 0.5f);
            }
            store_be<uint32_t>(p + 4 * x, (d & 0xFFFFFF) << 8 | st[x]);
        }
    }
}

// The color surface holding ea, laid out with this pitch; x, y: where ea falls in it.
Image* Renderer::surface_holding(GuestAddr ea, uint32_t pitch, uint32_t& x, uint32_t& y, GuestAddr& base) {
    for (auto& [addr, img] : surfaces_) {
        if ((img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || img->pitch != pitch || ea < addr || ea >= addr + surface_bytes(*img))
            continue;
        const uint32_t off = ea - addr;
        if (off % 4) continue;
        x = off % pitch / 4;
        y = off / pitch;
        base = addr;
        return img.get();
    }
    return nullptr;
}

// NV3089 between color surfaces of the same format: a blit, without leaving the GPU. A
// destination that is no surface yet (the game saves the display buffer in main memory
// and copies it back later) gets one, a shadow of that memory filled from it first.
bool Renderer::scaled_copy(GuestMemory& mem, const ScaledCopy& c) {
    uint32_t sx, sy, dx, dy;
    GuestAddr sbase, dbase;
    Image* s = surface_holding(c.src, c.src_pitch, sx, sy, sbase);
    if (!s || s->format != VK_FORMAT_R8G8B8A8_UNORM || sx + c.src_w > s->width || sy + c.src_h > s->height) return false;
    Image* d = surface_holding(c.dst, c.dst_pitch, dx, dy, dbase);
    if (!d) {
        if (c.dst_pitch % 4 || c.dst_pitch / 4 > 4096 || c.dst_y + c.dst_h > 4096 || surfaces_.count(c.dst)) return false;
        auto img = make_image(c.dst_pitch / 4, c.dst_y + c.dst_h, s->format,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
        if (!img) return false;
        img->pitch = c.dst_pitch;
        img->guest_format = s->guest_format;
        img->cpu_dirty = true;  // its pixels come from memory before the copy lands
        d = img.get();
        dbase = c.dst;
        dx = dy = 0;
        surfaces_[c.dst] = std::move(img);
    }
    if (d == s || d->format != s->format || dx + c.dst_x + c.dst_w > d->width || dy + c.dst_y + c.dst_h > d->height)
        return false;
    if (s->cpu_dirty) upload_surface(mem, sbase, *s);
    if (d->cpu_dirty) upload_surface(mem, dbase, *d);
    end_rendering();
    transition(*s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {int32_t(sx), int32_t(sy), 0};
    blit.srcOffsets[1] = {int32_t(sx + c.src_w), int32_t(sy + c.src_h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {int32_t(dx + c.dst_x), int32_t(dy + c.dst_y), 0};
    blit.dstOffsets[1] = {int32_t(dx + c.dst_x + c.dst_w), int32_t(dy + c.dst_y + c.dst_h), 1};
    vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, c.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    d->gpu_dirty = true;
    ++gpu_copies_;
    return true;
}

void Renderer::upload_surface(GuestMemory& mem, GuestAddr ea, Image& img) {
    img.cpu_dirty = false;
    uint32_t o[4];
    if (!surface_byte_order(img.guest_format, o) || img.format != VK_FORMAT_R8G8B8A8_UNORM || img.pitch < img.width * 4)
        return;
    end_rendering();
    const VkDeviceSize at = ring_alloc(VkDeviceSize(img.width) * img.height * 4, 16);
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* p = mem.host(ea + y * img.pitch);
        uint8_t* q = ring_map_ + at + size_t(y) * img.width * 4;
        for (uint32_t x = 0; x < img.width; ++x, q += 4, p += 4)
            for (int c = 0; c < 4; ++c) q[c] = p[o[c]];
    }
    transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {img.width, img.height, 1};
    vkCmdCopyBufferToImage(cmd(), ring_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
    const bool shadow = shadow_texture(t);
    const std::array<uint32_t, 3> key{t.filter, t.address, (t.control0 & 0x7FFFFF80u) | (shadow ? 1u : 0u)};
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const uint32_t mag = (t.filter >> 24) & 7, min = (t.filter >> 16) & 7;
    ci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // MIN: 1 NEAREST, 2 LINEAR, 3 NEAREST_MIPMAP_NEAREST, 4 LINEAR_MIPMAP_NEAREST,
    // 5 NEAREST_MIPMAP_LINEAR, 6 LINEAR_MIPMAP_LINEAR; the first two sample level 0 only.
    ci.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = min >= 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = address_mode(t.address & 0xF);
    ci.addressModeV = address_mode((t.address >> 8) & 0xF);
    ci.addressModeW = address_mode((t.address >> 16) & 0xF);
    // Levels between CONTROL0's min LOD (bits 19-30) and max LOD (bits 7-18), and FILTER's
    // bias (bits 0-12, signed), all fixed point with 8 fraction bits, as RPCS3 reads them.
    // Most of the menu's textures have a max LOD of 0: their base level only (sampling the
    // smaller ones, which they do not fill, made the time tunnel blurred and dark).
    if (min >= 3) {
        ci.minLod = float((t.control0 >> 19) & 0xFFF) / 256.0f;
        ci.maxLod = std::max(ci.minLod, float((t.control0 >> 7) & 0xFFF) / 256.0f);
        ci.mipLodBias = float(int32_t(t.filter << 19) >> 19) / 256.0f;
    }
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (shadow) {
        // The RSX compares the texel with the reference, Vulkan the reference with the texel:
        // the function goes the other way round (RPCS3's get_compare_func(op, true)).
        static constexpr VkCompareOp kReversed[8] = {
            VK_COMPARE_OP_NEVER, VK_COMPARE_OP_GREATER,   VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_GREATER_OR_EQUAL,
            VK_COMPARE_OP_LESS,  VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
        ci.compareEnable = VK_TRUE;
        ci.compareOp = kReversed[(t.address >> 28) & 7];
    }
    VkSampler s = VK_NULL_HANDLE;
    ok(vkCreateSampler(dev_, &ci, nullptr, &s), "vkCreateSampler");
    samplers_[key] = s;
    return s;
}

// A shadow map: the D16 surface the game drew it into, sampled on the GPU, or a copy of the
// part the texture starts at (the menu's two 1024x1024 maps share one 1024x2048 surface).
Image* Renderer::depth_texture(const Texture& t) {
    const GuestAddr ea = location_address(t.location, t.offset);
    if (!ea || t.base_format() != 0x92) return nullptr;  // DEPTH16
    for (auto& [addr, ptr] : surfaces_) {
        Image& img = *ptr;
        if (img.format != VK_FORMAT_D16_UNORM || !img.pitch || ea < addr || ea >= addr + surface_bytes(img)) continue;
        const uint32_t off = ea - addr, y = off / img.pitch;
        if (off % img.pitch || t.width > img.width || y + t.height > img.height || &img == depth_) return nullptr;
        end_rendering();
        if (y == 0 && t.width == img.width && t.height == img.height) {
            transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return &img;
        }
        const TextureKey key{ea, t.format, t.width, t.height, t.pitch, t.control1 & 0xFFFF, t.dimension};
        auto& part = textures_[key];
        if (!part || part->format != img.format) {
            if (part) retired_.push_back(std::move(part));
            part = make_image(t.width, t.height, img.format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!part) return nullptr;
        }
        transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(*part, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.srcOffset = {0, int32_t(y), 0};
        region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.extent = {t.width, t.height, 1};
        vkCmdCopyImage(cmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, part->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition(*part, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        part->hash = 0;
        ++gpu_copies_;
        return part.get();
    }
    return nullptr;
}

// A depth surface read as color: the main depth (D24S8) as A8R8G8B8 for the menu's
// post-processing, a shadow map (D16) as G8B8. The depth goes through a buffer into an
// RGBA8 or RG8 image, and the view's swizzle puts each byte where decoding the PS3's memory
// layout would (big-endian words, depth in the upper 24 bits); the stencil byte, B of
// A8R8G8B8, reads as 0. Before, the depth went through guest memory and the CPU, with a
// wait for the GPU each time (menu ~42 -> ~24 frames/s).
Image* Renderer::depth_as_color(const Texture& t) {
    const uint32_t f = t.base_format();
    const GuestAddr ea = location_address(t.location, t.offset);
    if (!ea || (f != 0x85 && f != 0x8B)) return nullptr;  // A8R8G8B8, G8B8
    for (auto& [addr, ptr] : surfaces_) {
        Image& img = *ptr;
        if (!(img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || !img.pitch || ea < addr || ea >= addr + surface_bytes(img))
            continue;
        // Which component of the converted image holds A, R, G and B of the decoded texel.
        VkComponentSwizzle argb[4];
        VkFormat cf;
        if (f == 0x85 && img.format == VK_FORMAT_D24_UNORM_S8_UINT) {
            cf = VK_FORMAT_R8G8B8A8_UNORM;  // r = depth bits 0-7, g = 8-15, b = 16-23
            argb[0] = VK_COMPONENT_SWIZZLE_B, argb[1] = VK_COMPONENT_SWIZZLE_G, argb[2] = VK_COMPONENT_SWIZZLE_R,
            argb[3] = VK_COMPONENT_SWIZZLE_ZERO;
        } else if (f == 0x8B && img.format == VK_FORMAT_D16_UNORM) {
            cf = VK_FORMAT_R8G8_UNORM;  // r = depth bits 0-7, g = 8-15
            argb[0] = VK_COMPONENT_SWIZZLE_G, argb[1] = VK_COMPONENT_SWIZZLE_R, argb[2] = VK_COMPONENT_SWIZZLE_G,
            argb[3] = VK_COMPONENT_SWIZZLE_R;
        } else {
            return nullptr;
        }
        const uint32_t off = ea - addr, y = off / img.pitch;
        if (off % img.pitch || t.width > img.width || y + t.height > img.height) return nullptr;
        const uint32_t remap = t.control1 & 0xFFFF;
        auto map = [&](int out) {
            const uint32_t op = (remap >> (8 + 2 * out)) & 3;
            return op == 0 ? VK_COMPONENT_SWIZZLE_ZERO : op == 1 ? VK_COMPONENT_SWIZZLE_ONE : argb[(remap >> (2 * out)) & 3];
        };
        const VkComponentMapping swizzle{map(1), map(2), map(3), map(0)};
        const TextureKey key{ea, t.format, t.width, t.height, t.pitch, remap, t.dimension};
        auto& part = textures_[key];
        if (part && part->format == cf && part->src == addr && part->hash == img.writes + 1) return part.get();
        if (!part || part->format != cf) {
            if (part) retired_.push_back(std::move(part));
            part = make_image(t.width, t.height, cf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, swizzle);
            if (!part) return nullptr;
        }
        const VkDeviceSize bytes = VkDeviceSize(t.width) * t.height * (cf == VK_FORMAT_R8G8_UNORM ? 2 : 4);
        if (convert_size_ < bytes) {
            if (convert_) {
                submit_and_wait();
                vkDestroyBuffer(dev_, convert_, nullptr);
                vkFreeMemory(dev_, convert_memory_, nullptr);
                convert_ = VK_NULL_HANDLE;
                convert_memory_ = VK_NULL_HANDLE;
                convert_size_ = 0;
            }
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (!ok(vkCreateBuffer(dev_, &bi, nullptr, &convert_), "vkCreateBuffer")) return nullptr;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev_, convert_, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (ai.memoryTypeIndex == UINT32_MAX ||
                !ok(vkAllocateMemory(dev_, &ai, nullptr, &convert_memory_), "vkAllocateMemory"))
                return nullptr;
            vkBindBufferMemory(dev_, convert_, convert_memory_, 0);
            convert_size_ = bytes;
        }
        end_rendering();
        transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.imageOffset = {0, int32_t(y), 0};
        copy.imageExtent = {t.width, t.height, 1};
        vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, convert_, 1, &copy);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd(), &dep);
        transition(*part, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageOffset = {0, 0, 0};
        vkCmdCopyBufferToImage(cmd(), convert_, part->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(*part, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        part->src = addr;
        part->src_size = 0;  // not compared with memory: follows the surface's writes
        part->hash = img.writes + 1;
        ++gpu_copies_;
        return part.get();
    }
    return nullptr;
}

Image* Renderer::texture(GuestMemory& mem, const Texture& t, uint32_t dims) {
    if (!t.enabled() || !t.width || !t.height || dims != 2) return nullptr;
    const GuestAddr ea = location_address(t.location, t.offset);
    if (!ea) return nullptr;
    const TextureKey key{ea, t.format, t.width, t.height, t.pitch, t.control1 & 0xFFFF, t.dimension};
    // A texture that is a surface we rendered: sample the surface itself, or, when the
    // texture is a smaller rectangle at its start (a 704x396 image in a 1280x720 target),
    // a copy of that rectangle; sampling the whole surface would shrink the picture.
    if (auto it = surfaces_.find(ea); it != surfaces_.end() && !(it->second->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        Image* img = it->second.get();
        if (img != color_ && t.width <= img->width && t.height <= img->height) {
            if (img->cpu_dirty) upload_surface(mem, ea, *img);
            if (t.width == img->width && t.height == img->height) {
                if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                    end_rendering();
                    transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                return img;
            }
            auto& part = textures_[key];
            if (!part || part->format != img->format || part->width != t.width || part->height != t.height) {
                if (part) retired_.push_back(std::move(part));
                part = make_image(t.width, t.height, img->format,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                if (!part) return nullptr;
            }
            end_rendering();
            transition(*img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(*part, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {t.width, t.height, 1};
            vkCmdCopyImage(cmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, part->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(*part, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            part->hash = 0;  // a copy of the surface, not of memory: taken again on every use
            return part.get();
        }
    }
    if (Image* conv = depth_as_color(t)) return conv;
    const uint32_t size = texture_source_size(t);
    if (!size || size > 64u << 20) return nullptr;
    flush(mem, ea, size);  // a surface drawn at another address or size, read as plain memory
    auto& slot = textures_[key];
    // Hashed once per frame: the CPU rarely rewrites a texture in the middle of one, and the
    // RSX's own writes clear hashed_flip (invalidate).
    if (slot && slot->hashed_flip == flips_) return slot.get();
    const uint64_t hash = hash_bytes(mem.host(ea), size);
    hashed_bytes_ += size;
    if (slot && slot->hash == hash) {
        slot->hashed_flip = flips_;
        return slot.get();
    }
    const TextureData data = decode_texture(mem.host(ea), t);
    if (!slot || slot->format != data.format || slot->levels != data.levels) {
        if (slot) retired_.push_back(std::move(slot));
        slot = make_image(data.width, data.height, data.format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          data.swizzle, VK_IMAGE_VIEW_TYPE_2D, 1, 1, data.levels);
        if (!slot) return nullptr;
    }
    end_rendering();
    const VkDeviceSize at = ring_alloc(data.bytes.size(), 16);
    std::memcpy(ring_map_ + at, data.bytes.data(), data.bytes.size());
    transition(*slot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copies[16]{};
    for (uint32_t l = 0; l < data.levels; ++l) {
        copies[l].bufferOffset = at + data.level_offset[l];
        copies[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        copies[l].imageExtent = {std::max(1u, data.width >> l), std::max(1u, data.height >> l), 1};
    }
    vkCmdCopyBufferToImage(cmd(), ring_, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, data.levels, copies);
    transition(*slot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    slot->hash = hash;
    slot->hashed_flip = flips_;
    slot->src = ea;
    slot->src_size = size;
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
    rs.cullMode = k.cull;
    rs.frontFace = VkFrontFace(k.front_face);
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
    if (!bind_targets(mem, s)) return skip();

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
            if (fk.texture_dims[t] == 2 && shadow_texture(tex)) fk.shadow |= 1u << t;
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
        if ((fk.shadow >> t) & 1) {
            Image* img = depth_texture(tex);
            image_infos[t] = {sampler(tex), (img ? img : dummy_depth_.get())->view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            continue;
        }
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
            convert_array(a, mem.host(base + first * stride), stride, count, out);
            converted_vertices_ += count;
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
    const uint32_t branch_bits = s.reg(SetTransformBranchBits);  // read as bits by BRB/CLB
    std::memcpy(&extra[10], &branch_bits, 4);
    extra[11] = 0.0f;
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
    if (s.reg(SetCullFaceEnable) & 1) {
        switch (s.reg(SetCullFace)) {
            case 0x404: k.cull = VK_CULL_MODE_FRONT_BIT; break;           // GL_FRONT
            case 0x405: k.cull = VK_CULL_MODE_BACK_BIT; break;            // GL_BACK
            case 0x408: k.cull = VK_CULL_MODE_FRONT_AND_BACK; break;      // GL_FRONT_AND_BACK
            default: break;
        }
    }
    // SET_FRONT_FACE: 0x900 GL_CW, 0x901 GL_CCW, in window coordinates with y up (GL). Our
    // window y grows downwards (the VP's viewport transform), which mirrors the winding.
    k.front_face = (s.reg(SetFrontFace) == 0x900) != kMirroredWinding ? VK_FRONT_FACE_CLOCKWISE
                                                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
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

    // Record. Depth counts as drawn only when written: the menu samples its depth buffer
    // (as A8R8G8B8) while it stays bound, and each read would otherwise read it back again.
    begin_rendering();
    if (depth_ && ((k.depth_test && k.depth_write) || k.stencil[0])) {
        depth_->gpu_dirty = true;
        ++depth_->writes;
    }
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
    if (auto it = surfaces_.find(kLocalBase + db.offset); it != surfaces_.end()) {
        src = it->second.get();
        if (src->cpu_dirty) upload_surface(mem, it->first, *src);
    }
    present_from(src, mem, db);

    const bool dump = !opt_.dump_dir.empty() && opt_.dump_every && flips_ % opt_.dump_every == 0;
    VkDeviceSize bytes = VkDeviceSize(db.width) * db.height * 4;
    if (dump && ensure_readback(bytes)) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {db.width, db.height, 1};
        vkCmdCopyImageToBuffer(cmd(), front_source_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
    }
    submit_and_wait();
    if (dump && readback_size_ >= bytes) {
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
              " texturas enviadas, %zu superfícies, %" PRIu64 " leituras da GPU, %" PRIu64 " cópias na GPU; %.1f GiB de texturas conferidas, %" PRIu64 " vértices convertidos",
              draws_, skipped_, pipelines_.size(), modules_.size(), texture_uploads_, surfaces_.size(), flushes_, gpu_copies_,
              double(hashed_bytes_) / double(1u << 30), converted_vertices_);
}

}  // namespace

std::unique_ptr<VulkanRenderer> create_vulkan_renderer(VulkanContext& ctx, const VulkanRendererOptions& opt) {
    auto r = std::make_unique<Renderer>(ctx, opt);
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace sly4::rsx
