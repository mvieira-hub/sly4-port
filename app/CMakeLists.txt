add_executable(sly4 main.cpp)
target_link_libraries(sly4 PRIVATE sly4_runtime sly4_kernel)
if(TARGET sly4_gen)
    target_link_libraries(sly4 PRIVATE sly4_gen)
    target_compile_definitions(sly4 PRIVATE SLY4_HAS_GEN)
endif()
# One self-contained .exe: no libstdc++/libgcc/winpthread DLLs next to it.
target_link_options(sly4 PRIVATE -static -static-libgcc -static-libstdc++)
# Console subsystem while the port is in development: logs go to the terminal.
target_link_options(sly4 PRIVATE -mconsole)
set_target_properties(sly4 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
