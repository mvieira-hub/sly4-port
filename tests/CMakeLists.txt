add_executable(sly4_tests
    test_main.cpp
    test_guest_memory.cpp
    test_loader.cpp
    test_ppu_decoder.cpp
    test_analysis.cpp
    test_interpreter.cpp
    test_kernel.cpp
    test_spu.cpp
    test_spu_semantics.cpp
    test_spu_thread.cpp
    test_rsx.cpp
)
target_link_libraries(sly4_tests PRIVATE sly4_runtime sly4_ppu sly4_analysis sly4_cpu sly4_kernel)
target_link_options(sly4_tests PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
set_target_properties(sly4_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
