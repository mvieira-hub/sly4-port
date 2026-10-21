# Static recompiler (phase 2). sly4_ppu holds the PPU decoder/disassembler, shared
# with the runtime's interpreter and the C++ emitter as they are written.
add_library(sly4_ppu STATIC
    ppu/ppu_decoder.cpp
    ppu/ppu_disasm.cpp
)
target_include_directories(sly4_ppu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(sly4_recomp tools/sly4_recomp.cpp)
target_link_libraries(sly4_recomp PRIVATE sly4_ppu sly4_runtime)
target_link_options(sly4_recomp PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
set_target_properties(sly4_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
