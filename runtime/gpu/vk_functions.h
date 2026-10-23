// Vulkan entry points, loaded at run time from the driver's vulkan-1.dll through
// SDL (no Vulkan SDK or import library needed to build). VK_NO_PROTOTYPES is set by
// the build, so these pointers are the only declarations of the vk* functions.
#pragma once

#include <vulkan/vulkan.h>

namespace sly4::vk {

#define SLY4_VK_GLOBAL_FUNCS(X)              \
    X(vkCreateInstance)                      \
    X(vkEnumerateInstanceVersion)            \
    X(vkEnumerateInstanceLayerProperties)    \
    X(vkEnumerateInstanceExtensionProperties)

#define SLY4_VK_INSTANCE_FUNCS(X)                  \
    X(vkDestroyInstance)                           \
    X(vkEnumeratePhysicalDevices)                  \
    X(vkGetPhysicalDeviceProperties)               \
    X(vkGetPhysicalDeviceFeatures2)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkEnumerateDeviceExtensionProperties)        \
    X(vkCreateDevice)                              \
    X(vkGetDeviceProcAddr)                         \
    X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)   \
    X(vkGetPhysicalDeviceMemoryProperties)         \
    X(vkGetPhysicalDeviceFormatProperties)

#define SLY4_VK_DEVICE_FUNCS(X)      \
    X(vkDestroyDevice)               \
    X(vkGetDeviceQueue)              \
    X(vkDeviceWaitIdle)              \
    X(vkCreateSwapchainKHR)          \
    X(vkDestroySwapchainKHR)         \
    X(vkGetSwapchainImagesKHR)       \
    X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR)             \
    X(vkQueueSubmit2)                \
    X(vkCreateImageView)             \
    X(vkDestroyImageView)            \
    X(vkCreateCommandPool)           \
    X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers)      \
    X(vkResetCommandBuffer)          \
    X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer)            \
    X(vkCmdPipelineBarrier2)         \
    X(vkCmdBeginRendering)           \
    X(vkCmdEndRendering)             \
    X(vkCreateSemaphore)             \
    X(vkDestroySemaphore)            \
    X(vkCreateFence)                 \
    X(vkDestroyFence)                \
    X(vkWaitForFences)               \
    X(vkResetFences)                 \
    X(vkQueueWaitIdle)               \
    X(vkCreateBuffer)                \
    X(vkDestroyBuffer)               \
    X(vkCreateImage)                 \
    X(vkDestroyImage)                \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetImageMemoryRequirements)  \
    X(vkAllocateMemory)              \
    X(vkFreeMemory)                  \
    X(vkBindBufferMemory)            \
    X(vkBindImageMemory)             \
    X(vkMapMemory)                   \
    X(vkCreateShaderModule)          \
    X(vkDestroyShaderModule)         \
    X(vkCreatePipelineLayout)        \
    X(vkDestroyPipelineLayout)       \
    X(vkCreateGraphicsPipelines)     \
    X(vkDestroyPipeline)             \
    X(vkCreateDescriptorSetLayout)   \
    X(vkDestroyDescriptorSetLayout)  \
    X(vkCreateDescriptorPool)        \
    X(vkDestroyDescriptorPool)       \
    X(vkResetDescriptorPool)         \
    X(vkAllocateDescriptorSets)      \
    X(vkUpdateDescriptorSets)        \
    X(vkCreateSampler)               \
    X(vkDestroySampler)              \
    X(vkCmdBindPipeline)             \
    X(vkCmdBindDescriptorSets)       \
    X(vkCmdBindVertexBuffers)        \
    X(vkCmdBindIndexBuffer)          \
    X(vkCmdDraw)                     \
    X(vkCmdDrawIndexed)              \
    X(vkCmdSetViewport)              \
    X(vkCmdSetScissor)               \
    X(vkCmdCopyBufferToImage)        \
    X(vkCmdCopyImageToBuffer)        \
    X(vkCmdBlitImage)                \
    X(vkCmdCopyImage)                \
    X(vkCmdClearColorImage)          \
    X(vkCmdClearDepthStencilImage)   \
    X(vkCmdClearAttachments)         \
    X(vkCmdSetBlendConstants)        \
    X(vkCmdSetStencilReference)

#define SLY4_VK_DECLARE(name) extern PFN_##name name;
SLY4_VK_GLOBAL_FUNCS(SLY4_VK_DECLARE)
SLY4_VK_INSTANCE_FUNCS(SLY4_VK_DECLARE)
SLY4_VK_DEVICE_FUNCS(SLY4_VK_DECLARE)
#undef SLY4_VK_DECLARE

// Each returns false and names the first missing function in *missing.
bool load_global(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** missing);
bool load_instance(VkInstance instance, const char** missing);
bool load_device(VkDevice device, const char** missing);

const char* result_name(VkResult r);

}  // namespace sly4::vk
