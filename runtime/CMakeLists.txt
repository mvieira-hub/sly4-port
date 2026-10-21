# Native runtime: everything that replaces the PS3 (memory, kernel, HLE modules, GPU...).
# Phase 0 only has the Vulkan presentation layer.
add_library(sly4_runtime STATIC
    gpu/vk_functions.cpp
    gpu/vulkan_context.cpp
)
target_include_directories(sly4_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${VULKAN_HEADERS_DIR})
target_compile_definitions(sly4_runtime PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(sly4_runtime PUBLIC SDL3::SDL3-static)
