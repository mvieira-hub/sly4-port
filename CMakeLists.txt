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
add_subdirectory(recomp)

# Recompiled game code: generated locally by "sly4_recomp emit" from the user's own
# EBOOT.elf into generated/ (ignored by git: it is derived from the game). Built only
# when present; sly4.exe then runs the game, otherwise it stops after loading.
# SLY4_GEN_BUDGET adds the block budget used by the differential test.
set(SLY4_GEN_DIR "${CMAKE_SOURCE_DIR}/generated/ppu" CACHE PATH "Directory written by sly4_recomp emit")
option(SLY4_GEN_BUDGET "Budget checks in the generated code (differential tests)" ON)
if(EXISTS "${SLY4_GEN_DIR}/sources.cmake")
    include("${SLY4_GEN_DIR}/sources.cmake")
    add_library(sly4_gen STATIC ${SLY4_GEN_SOURCES})
    target_include_directories(sly4_gen PUBLIC ${SLY4_GEN_DIR})
    target_link_libraries(sly4_gen PUBLIC sly4_cpu)
    # Same FP rules as the interpreter's inline semantics (VMX float uses fmaf); -O1
    # keeps the build of millions of generated lines reasonable for now.
    target_compile_options(sly4_gen PRIVATE -O1 -Wno-unused-label -frounding-math -fno-fast-math -ffp-contract=off -mfma)
    if(SLY4_GEN_BUDGET)
        target_compile_definitions(sly4_gen PUBLIC SLY4_GEN_BUDGET)
    endif()
    add_executable(sly4_gendiff tests/gendiff/gendiff.cpp)
    target_link_libraries(sly4_gendiff PRIVATE sly4_gen)
    target_compile_options(sly4_gendiff PRIVATE -frounding-math -mfma)
    target_link_options(sly4_gendiff PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
    set_target_properties(sly4_gendiff PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

add_subdirectory(app)
add_subdirectory(tests)
