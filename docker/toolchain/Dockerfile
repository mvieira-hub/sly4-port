# Toolchain for the native port: cross-compiles Windows x86-64 executables on Linux.
#   MinGW-w64 GCC 14 (C++20, POSIX threads), CMake, Ninja
#   Vulkan headers (from Debian) and SDL3 built as a static Windows library,
#   both installed in /opt/mingw-sysroot.
# Build: docker build -t sly4-mingw:1 -f docker/toolchain/Dockerfile .
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix mingw-w64-tools \
        cmake ninja-build make ca-certificates wget libvulkan-dev \
    && rm -rf /var/lib/apt/lists/*

COPY cmake/mingw-w64-x86_64.cmake /opt/cmake/mingw-w64-x86_64.cmake

# Vulkan headers only (the loader, vulkan-1.dll, comes with the GPU driver on Windows).
RUN mkdir -p /opt/mingw-sysroot/include \
    && cp -r /usr/include/vulkan /usr/include/vk_video /opt/mingw-sysroot/include/ \
    && test -f /opt/mingw-sysroot/include/vulkan/vulkan_core.h

ARG SDL_VERSION=3.2.0
WORKDIR /build
RUN wget -q -O sdl.tar.gz https://github.com/libsdl-org/SDL/archive/refs/tags/release-${SDL_VERSION}.tar.gz \
    && tar xf sdl.tar.gz \
    && cmake -S SDL-release-${SDL_VERSION} -B sdl-build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/cmake/mingw-w64-x86_64.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/mingw-sysroot \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build sdl-build \
    && cmake --install sdl-build \
    && cd / && rm -rf /build

WORKDIR /src
