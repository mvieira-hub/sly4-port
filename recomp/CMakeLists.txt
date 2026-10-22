# Static recompiler (phase 2). sly4_ppu holds the PPU decoder/disassembler, shared
# with the runtime's interpreter and the C++ emitter as they are written.
add_library(sly4_ppu STATIC
    ppu/ppu_decoder.cpp
    ppu/ppu_disasm.cpp
    spu/spu_decoder.cpp
)
target_include_directories(sly4_ppu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Control-flow analysis of the executable (functions, blocks, jump tables) and the
# PPU -> C++ emitter.
add_library(sly4_analysis STATIC analysis/program.cpp emit/ppu_emitter.cpp emit/spu_emitter.cpp)
target_link_libraries(sly4_analysis PUBLIC sly4_ppu sly4_runtime sly4_cpu)

add_executable(sly4_recomp tools/sly4_recomp.cpp tools/oracle.cpp tools/spu_oracle.cpp)
target_link_libraries(sly4_recomp PRIVATE sly4_analysis sly4_cpu)
target_link_options(sly4_recomp PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
set_target_properties(sly4_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
