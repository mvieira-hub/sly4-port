add_executable(sly4_tests
    test_main.cpp
    test_guest_memory.cpp
    test_loader.cpp
)
target_link_libraries(sly4_tests PRIVATE sly4_runtime)
target_link_options(sly4_tests PRIVATE -static -static-libgcc -static-libstdc++ -mconsole)
set_target_properties(sly4_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
