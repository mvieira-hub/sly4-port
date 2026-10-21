// Vulkan 1.3 presentation layer: instance, device, swapchain and a per-frame clear.
// This is the base the RSX backend (FIFO -> Vulkan) will draw into in phase 5.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/vk_functions.h"

struct SDL_Window;

namespace sly4 {

struct VulkanOptions {
    bool validation = false;  // enable VK_LAYER_KHRONOS_validation if it is installed
    bool vsync = true;        // FIFO present mode; off = MAILBOX/IMMEDIATE when available
};

class VulkanContext {
public:
    bool init(SDL_Window* window, const VulkanOptions& options);
    void shutdown();

    // Clears the next swapchain image to `rgba` (linear) and presents it.
    // Returns false only on an unrecoverable error (device lost...).
    bool draw_frame(const std::array<float, 4>& rgba);

    void notify_resized() { resize_pending_ = true; }

    const std::string& device_name() const { return device_name_; }
    uint32_t api_version() const { return api_version_; }
    VkFormat swapchain_format() const { return format_.format; }
    VkExtent2D swapchain_extent() const { return extent_; }
    VkPresentModeKHR present_mode() const { return present_mode_; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
    };

    bool create_instance();
    bool pick_physical_device();
    bool create_device();
    bool create_swapchain();
    void destroy_swapchain_resources();
    bool recreate_swapchain();
    bool create_frames();

    SDL_Window* window_ = nullptr;
    VulkanOptions options_{};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    std::string device_name_;
    uint32_t api_version_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    // One per swapchain image: a present may still wait on it when the frame slot is reused.
    std::vector<VkSemaphore> render_finished_;

    std::array<Frame, kFramesInFlight> frames_{};
    uint32_t frame_index_ = 0;
    bool resize_pending_ = false;
};

}  // namespace sly4
