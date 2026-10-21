# Cross-compile for Windows x86-64 with MinGW-w64 (POSIX threads flavour, GCC 14).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Libraries built for Windows (SDL3, Vulkan headers) live in this sysroot.
set(SLY4_SYSROOT /opt/mingw-sysroot CACHE PATH "Prefix with Windows libraries and headers")
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${SLY4_SYSROOT})
set(CMAKE_PREFIX_PATH ${SLY4_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
