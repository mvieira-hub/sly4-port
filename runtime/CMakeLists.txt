# Native runtime: everything that replaces the PS3 (memory, kernel, HLE modules, GPU...).
add_library(sly4_runtime STATIC
    core/png.cpp
    gpu/rsx_fp.cpp
    gpu/rsx_texture.cpp
    gpu/rsx_vp.cpp
    gpu/spirv.cpp
    gpu/vk_functions.cpp
    gpu/vulkan_context.cpp
    core/host_sleep.cpp
    core/inflate.cpp
    loader/elf_loader.cpp
    loader/image_report.cpp
    loader/nid.cpp
    loader/prx_loader.cpp
    loader/spu_elf.cpp
    memory/guest_memory.cpp
)
target_include_directories(sly4_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${VULKAN_HEADERS_DIR})
target_compile_definitions(sly4_runtime PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(sly4_runtime PUBLIC SDL3::SDL3-static)

# PPU reference interpreter (phase 2c): oracle for the C++ emitter and runtime fallback.
add_library(sly4_cpu STATIC
    cpu/guest_atomic.cpp
    cpu/ppu_interpreter.cpp
    cpu/ppu_fpu.cpp
    cpu/ppu_rt.cpp
    cpu/spu_blocks.cpp
    cpu/spu_channels.cpp
    cpu/spu_interpreter.cpp
)
target_link_libraries(sly4_cpu PUBLIC sly4_runtime sly4_ppu)
# FP results depend on the rounding mode set at run time and on IEEE flags. -mfma makes
# std::fma/fmaf the hardware fused multiply-add (MinGW's library versions round twice,
# which breaks fmadd/vmaddfp by one ulp); x86 CPUs have had FMA3 since 2013.
# -fsignaling-nans keeps x * 1.0 a multiply, as in the generated SPU code (CMakeLists.txt).
target_compile_options(sly4_cpu PRIVATE -frounding-math -fno-fast-math -ffp-contract=off -fsignaling-nans -mfma)

# LV2 kernel and HLE firmware libraries (phase 3).
add_library(sly4_kernel STATIC
    kernel/guest_alloc.cpp
    kernel/hle.cpp
    kernel/hle_cell_audio.cpp
    kernel/hle_cell_fs.cpp
    kernel/hle_cell_game.cpp
    kernel/hle_cell_pad.cpp
    kernel/hle_cell_gcm.cpp
    kernel/hle_cell_spurs.cpp
    kernel/hle_libc.cpp
    kernel/hle_sys_prx_for_user.cpp
    kernel/hle_system.cpp
    kernel/kernel.cpp
    kernel/spurs_tasks.cpp
    kernel/memory.cpp
    kernel/spu_thread.cpp
    kernel/sync.cpp
    kernel/syscalls.cpp
    kernel/syscalls_spu.cpp
    kernel/syscalls_sync.cpp
    kernel/vfs.cpp
    gpu/rsx_vk.cpp
    rsx/rsx_core.cpp
    rsx/rsx_methods.cpp
    rsx/rsx_state.cpp
)
target_link_libraries(sly4_kernel PUBLIC sly4_cpu)
