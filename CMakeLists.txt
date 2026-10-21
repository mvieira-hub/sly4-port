cmake_minimum_required(VERSION 3.25)
project(sly4 VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "sly4 targets Windows; build with docker/build.ps1 (MinGW-w64 toolchain)")
endif()

# SDL3 (static) and the Vulkan headers come from the toolchain sysroot.
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)
find_path(VULKAN_HEADERS_DIR vulkan/vulkan.h REQUIRED)

# Vulkan structs are written as `VkFoo info{VK_STRUCTURE_TYPE_FOO}`: the remaining
# members are value-initialised on purpose, so the missing-initializer warning is noise.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_subdirectory(runtime)
add_subdirectory(app)
