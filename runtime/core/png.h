// Minimal PNG writer (RGBA8, uncompressed deflate blocks): frame dumps of the renderer.
#pragma once

#include <cstdint>
#include <string>

namespace sly4 {

// `rgba` is width * height * 4 bytes, rows top to bottom. False if the file cannot be written.
bool write_png(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace sly4
