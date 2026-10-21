#include "gpu/vk_functions.h"

#include <string_view>

namespace sly4::vk {

#define SLY4_VK_DEFINE(name) PFN_##name name = nullptr;
SLY4_VK_GLOBAL_FUNCS(SLY4_VK_DEFINE)
SLY4_VK_INSTANCE_FUNCS(SLY4_VK_DEFINE)
SLY4_VK_DEVICE_FUNCS(SLY4_VK_DEFINE)
#undef SLY4_VK_DEFINE

namespace {
PFN_vkGetInstanceProcAddr g_get_instance_proc_addr = nullptr;
}

bool load_global(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** missing) {
    g_get_instance_proc_addr = get_instance_proc_addr;
#define SLY4_VK_LOAD(name)                                                                    \
    name = reinterpret_cast<PFN_##name>(g_get_instance_proc_addr(VK_NULL_HANDLE, #name));     \
    if (!name && std::string_view(#name) != "vkEnumerateInstanceVersion") {                   \
        *missing = #name;                                                                     \
        return false;                                                                         \
    }
    SLY4_VK_GLOBAL_FUNCS(SLY4_VK_LOAD)
#undef SLY4_VK_LOAD
    return true;
}

bool load_instance(VkInstance instance, const char** missing) {
#define SLY4_VK_LOAD(name)                                                                \
    name = reinterpret_cast<PFN_##name>(g_get_instance_proc_addr(instance, #name));       \
    if (!name) {                                                                          \
        *missing = #name;                                                                 \
        return false;                                                                     \
    }
    SLY4_VK_INSTANCE_FUNCS(SLY4_VK_LOAD)
#undef SLY4_VK_LOAD
    return true;
}

bool load_device(VkDevice device, const char** missing) {
#define SLY4_VK_LOAD(name)                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
    if (!name) {                                                            \
        *missing = #name;                                                   \
        return false;                                                       \
    }
    SLY4_VK_DEVICE_FUNCS(SLY4_VK_LOAD)
#undef SLY4_VK_LOAD
    return true;
}

const char* result_name(VkResult r) {
    switch (r) {
#define SLY4_CASE(x) \
    case x:          \
        return #x;
        SLY4_CASE(VK_SUCCESS)
        SLY4_CASE(VK_NOT_READY)
        SLY4_CASE(VK_TIMEOUT)
        SLY4_CASE(VK_INCOMPLETE)
        SLY4_CASE(VK_SUBOPTIMAL_KHR)
        SLY4_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        SLY4_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        SLY4_CASE(VK_ERROR_INITIALIZATION_FAILED)
        SLY4_CASE(VK_ERROR_DEVICE_LOST)
        SLY4_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        SLY4_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        SLY4_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        SLY4_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        SLY4_CASE(VK_ERROR_SURFACE_LOST_KHR)
        SLY4_CASE(VK_ERROR_OUT_OF_DATE_KHR)
#undef SLY4_CASE
        default:
            return "VkResult desconhecido";
    }
}

}  // namespace sly4::vk
