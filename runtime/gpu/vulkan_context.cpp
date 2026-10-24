#include "gpu/vulkan_context.h"

#include <algorithm>
#include <cstring>

// vulkan.h (through vk_functions.h) must come first: SDL_vulkan.h only declares its own
// minimal Vulkan types when the real header is absent.
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>

#include "core/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace sly4::vk;

namespace sly4 {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    SLY4_ERROR("%s falhou: %s (%d)", what, result_name(r), static_cast<int>(r));
    return false;
}

std::string version_string(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

bool has_extension(VkPhysicalDevice dev, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, exts.data());
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

}  // namespace

bool VulkanContext::init(SDL_Window* window, const VulkanOptions& options) {
    window_ = window;
    options_ = options;
    const char* missing = nullptr;

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) {
        SLY4_ERROR("vulkan-1.dll não encontrado (driver da GPU sem Vulkan?): %s", SDL_GetError());
        return false;
    }
    if (!load_global(gipa, &missing)) {
        SLY4_ERROR("função global do Vulkan ausente: %s", missing);
        return false;
    }
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader_version);
    SLY4_INFO("loader Vulkan %s", version_string(loader_version).c_str());
    if (loader_version < VK_API_VERSION_1_3) {
        SLY4_ERROR("é preciso Vulkan 1.3; atualize o driver da GPU");
        return false;
    }

    if (!create_instance()) return false;
    if (!load_instance(instance_, &missing)) {
        SLY4_ERROR("função de instância do Vulkan ausente: %s", missing);
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        SLY4_ERROR("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    if (!pick_physical_device()) return false;
    if (!create_device()) return false;
    if (!load_device(device_, &missing)) {
        SLY4_ERROR("função de dispositivo do Vulkan ausente: %s", missing);
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    if (!create_swapchain()) return false;
    if (!create_frames()) return false;
    return true;
}

bool VulkanContext::init_headless(const VulkanOptions& options) {
    headless_ = true;
    options_ = options;
    const char* missing = nullptr;
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                          reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")))
                    : nullptr;
    if (!gipa) {
        SLY4_ERROR("vulkan-1.dll não encontrado (driver da GPU sem Vulkan?)");
        return false;
    }
    if (!load_global(gipa, &missing)) {
        SLY4_ERROR("função global do Vulkan ausente: %s", missing);
        return false;
    }
    if (!create_instance()) return false;
    if (!load_instance(instance_, &missing)) {
        SLY4_ERROR("função de instância do Vulkan ausente: %s", missing);
        return false;
    }
    if (!pick_physical_device() || !create_device()) return false;
    if (!load_device(device_, &missing)) {
        SLY4_ERROR("função de dispositivo do Vulkan ausente: %s", missing);
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    SLY4_INFO("Vulkan sem janela em %s", device_name_.c_str());
    return true;
}

bool VulkanContext::create_instance() {
    std::vector<const char*> extensions;
    if (!headless_) {
        Uint32 sdl_count = 0;
        const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        if (!sdl_exts) {
            SLY4_ERROR("SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
            return false;
        }
        extensions.assign(sdl_exts, sdl_exts + sdl_count);
    } else {
        // Not used without a window, but the surface entry points are loaded with the rest.
        extensions = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    }

    std::vector<const char*> layers;
    if (options_.validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        bool found = std::any_of(props.begin(), props.end(), [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, kValidationLayer) == 0;
        });
        if (found) {
            layers.push_back(kValidationLayer);
            SLY4_INFO("camada de validação ativada");
        } else {
            SLY4_WARN("%s não instalada (vem com o Vulkan SDK); seguindo sem validação", kValidationLayer);
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Sly Cooper: Thieves in Time (port nativo)";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 1);
    app.pEngineName = "sly4-runtime";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 1);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    return check(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
}

bool VulkanContext::pick_physical_device() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());

    int best_score = -1;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(dev, &f2);

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, queues.data());
        int family = -1;
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = headless_ ? VK_TRUE : VK_FALSE;
            if (!headless_) vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present);
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                family = static_cast<int>(i);
                break;
            }
        }

        const bool ok = props.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering && f13.synchronization2 &&
                        family >= 0 && has_extension(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        int score = !ok ? -1 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        SLY4_INFO("GPU: %s (Vulkan %s)%s", props.deviceName, version_string(props.apiVersion).c_str(),
                  ok ? "" : " - não atende aos requisitos");
        if (score > best_score) {
            best_score = score;
            physical_ = dev;
            queue_family_ = static_cast<uint32_t>(family);
            device_name_ = props.deviceName;
            api_version_ = props.apiVersion;
        }
    }
    if (best_score < 0) {
        SLY4_ERROR("nenhuma GPU com Vulkan 1.3, dynamic rendering, synchronization2 e swapchain");
        return false;
    }
    SLY4_INFO("usando: %s", device_name_.c_str());
    return true;
}

bool VulkanContext::create_device() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f13;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;  // also headless: the swapchain entry points are loaded with the rest
    ci.ppEnabledExtensionNames = extensions;
    return check(vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice");
}

bool VulkanContext::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) return true;  // minimized: try again on resize

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    format_ = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                              : formats[0];
    for (const auto& f : formats) {
        // UNORM: the PS3 already outputs gamma-encoded values.
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format_ = f;
            break;
        }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
    // MAILBOX: no tearing, and a present never waits for the display (with FIFO on a 144 Hz
    // monitor it did, holding the queue the renderer shares). FIFO is always there.
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR wanted[2] = {options_.vsync ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR,
                                        VK_PRESENT_MODE_MAILBOX_KHR};
    for (VkPresentModeKHR want : wanted) {
        if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
            present_mode_ = want;
            break;
        }
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }
    }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = image_count;
    ci.imageFormat = format_.format;
    ci.imageColorSpace = format_.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = present_mode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (!check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());

    views_.resize(n);
    render_finished_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(device_, &vi, nullptr, &views_[i]), "vkCreateImageView")) return false;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!check(vkCreateSemaphore(device_, &si, nullptr, &render_finished_[i]), "vkCreateSemaphore")) return false;
    }
    return true;
}

void VulkanContext::destroy_swapchain_resources() {
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    views_.clear();
    render_finished_.clear();
    images_.clear();
}

bool VulkanContext::recreate_swapchain() {
    vkDeviceWaitIdle(device_);
    destroy_swapchain_resources();
    resize_pending_ = false;
    return create_swapchain();
}

bool VulkanContext::create_frames() {
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family_;
        if (!check(vkCreateCommandPool(device_, &pci, nullptr, &f.pool), "vkCreateCommandPool")) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device_, &ai, &f.cmd), "vkAllocateCommandBuffers")) return false;
        VkFenceCreateInfo ai_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!check(vkCreateFence(device_, &ai_fence, nullptr, &f.acquired), "vkCreateFence")) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateFence(device_, &fi, nullptr, &f.in_flight), "vkCreateFence")) return false;
    }
    return true;
}

bool VulkanContext::draw_frame(const std::array<float, 4>& rgba, const Source& source) {
    if (headless_) return true;
    if (resize_pending_ || !swapchain_ || images_.empty()) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w == 0 || h == 0) return true;  // minimized
        if (!recreate_swapchain()) return false;
        if (images_.empty()) return true;
    }

    Frame& f = frames_[frame_index_];
    if (!check(vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return false;

    // The image is waited for here, on the CPU, not by a semaphore in the submit: the queue is
    // shared with the renderer, and a submit waiting for the display held the game's work
    // behind it (up to a refresh per present).
    uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, VK_NULL_HANDLE, f.acquired, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resize_pending_ = true;
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return check(r, "vkAcquireNextImageKHR");
    if (r == VK_SUBOPTIMAL_KHR) resize_pending_ = true;
    if (!check(vkWaitForFences(device_, 1, &f.acquired, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return false;
    vkResetFences(device_, 1, &f.acquired);

    vkResetFences(device_, 1, &f.in_flight);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    // The swapchain image: cleared, then the source (if any) scaled into it keeping its
    // aspect ratio. Earlier writes to the source by the renderer were submitted to this
    // same queue, so a barrier is enough to wait for them.
    VkImageMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = images_[image];
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[1] = barriers[0];
    barriers[1].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[1].oldLayout = barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1].image = source.image;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = source.image ? 2 : 1;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(f.cmd, &dep);

    VkClearColorValue clear{};
    std::memcpy(clear.float32, source.image ? std::array<float, 4>{0, 0, 0, 1}.data() : rgba.data(), sizeof(float) * 4);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(f.cmd, images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    if (source.image && source.width && source.height) {
        const double sx = double(extent_.width) / source.width, sy = double(extent_.height) / source.height;
        const double s = std::min(sx, sy);
        const int32_t w = int32_t(source.width * s), h = int32_t(source.height * s);
        const int32_t x = (int32_t(extent_.width) - w) / 2, y = (int32_t(extent_.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(f.cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[image],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    VkImageMemoryBarrier2 to_present = barriers[0];
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(f.cmd, &dep);
    vkEndCommandBuffer(f.cmd);

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_finished_[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    {
        std::lock_guard<std::mutex> lk(queue_lock_);
        if (!check(vkQueueSubmit2(queue_, 1, &si, f.in_flight), "vkQueueSubmit2")) return false;
        r = vkQueuePresentKHR(queue_, &pi);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        resize_pending_ = true;
    } else if (!check(r, "vkQueuePresentKHR")) {
        return false;
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return true;
}

void VulkanContext::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        for (Frame& f : frames_) {
            if (f.in_flight) vkDestroyFence(device_, f.in_flight, nullptr);
            if (f.acquired) vkDestroyFence(device_, f.acquired, nullptr);
            if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
            f = Frame{};
        }
        destroy_swapchain_resources();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace sly4
