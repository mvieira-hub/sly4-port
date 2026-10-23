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
# Guest registers are unions read and written through references to different members
# (SpuReg::u32() and f32() on the same quadword). With strict aliasing GCC may assume they
# do not overlap: once the SPU semantics were inlined into the blocks, an fnms read a
# register from before the iohl that completed it.
add_compile_options(-fno-strict-aliasing)

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

# Firmware modules run as they are (docs/SPU.md, 4c): generated/fw_<name> from
# "sly4_recomp emit --prx ... --name <name>". Each becomes sly4_gen_<name>, and
# gen_firmware.cpp registers all of them (an empty function when there are none).
file(GLOB SLY4_FW_GEN_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/generated/fw_*")
set(SLY4_FW_MODULES "")
foreach(dir ${SLY4_FW_GEN_DIRS})
    if(EXISTS "${dir}/sources.cmake")
        get_filename_component(mod "${dir}" NAME)
        string(REGEX REPLACE "^fw_" "" mod "${mod}")
        include("${dir}/sources.cmake")
        add_library(sly4_gen_${mod} STATIC ${SLY4_GEN_SOURCES})
        target_include_directories(sly4_gen_${mod} PRIVATE "${dir}")
        target_link_libraries(sly4_gen_${mod} PUBLIC sly4_cpu)
        target_compile_options(sly4_gen_${mod} PRIVATE -O1 -Wno-unused-label -frounding-math -fno-fast-math -ffp-contract=off -mfma)
        list(APPEND SLY4_FW_MODULES ${mod})
    endif()
endforeach()
# Recompiled SPU blocks (docs/SPU.md, 4e): generated/spu from "sly4_recomp spu-emit".
# sly4_spudiff compares them with the interpreter.
set(_spu_decl "")
set(_spu_call "")
set(SLY4_SPU_GEN_DIR "${CMAKE_SOURCE_DIR}/generated/spu")
if(EXISTS "${SLY4_SPU_GEN_DIR}/sources.cmake")
    include("${SLY4_SPU_GEN_DIR}/sources.cmake")
    add_library(sly4_gen_spu STATIC ${SLY4_GEN_SOURCES})
    target_link_libraries(sly4_gen_spu PUBLIC sly4_cpu)
    # -fsignaling-nans: without it GCC turns x * 1.0 into x when the 1.0 is a constant of
    # the block (ilhu), and the multiply that zeroes a denormal operand (DAZ) disappears.
    target_compile_options(sly4_gen_spu PRIVATE -O2 -frounding-math -fno-fast-math -ffp-contract=off -fsignaling-nans -mfma)
    # With the semantics forced inline each file needs a few GiB to compile: at most 4 at
    # a time (Ninja pool), or the compiler runs out of the container's memory.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS sly4_spu_gen=4)
    set_property(TARGET sly4_gen_spu PROPERTY JOB_POOL_COMPILE sly4_spu_gen)
    set(_spu_decl "void register_spu_blocks();\n")
    set(_spu_call "    register_spu_blocks();\n")
    add_executable(sly4_spudiff tests/spudiff/spudiff.cpp recomp/tools/spu_oracle.cpp)
    target_include_directories(sly4_spudiff PRIVATE recomp/tools)
    target_compile_definitions(sly4_spudiff PRIVATE SLY4_SPU_ORACLE_BLOCKS)
    target_link_libraries(sly4_spudiff PRIVATE sly4_gen_spu)
    target_compile_options(sly4_spudiff PRIVATE -frounding-math -mfma)
    target_link_options(sly4_spudiff PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
    set_target_properties(sly4_spudiff PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
set(_fw_decl "")
set(_fw_call "")
foreach(mod ${SLY4_FW_MODULES})
    string(APPEND _fw_decl "void register_functions_${mod}();\n")
    string(APPEND _fw_call "    register_functions_${mod}();\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/gen_firmware.cpp.in"
    "// Generated by CMake: registers the recompiled firmware modules and SPU blocks.\n"
    "namespace sly4::gen {\n${_fw_decl}${_spu_decl}"
    "void register_firmware_functions() {\n${_fw_call}}\n"
    "void register_spu_code() {\n${_spu_call}}\n}  // namespace sly4::gen\n")
configure_file("${CMAKE_BINARY_DIR}/gen_firmware.cpp.in" "${CMAKE_BINARY_DIR}/gen_firmware.cpp" COPYONLY)

add_subdirectory(app)
add_subdirectory(tests)
