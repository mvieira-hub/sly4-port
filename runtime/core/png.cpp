#include "core/png.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace sly4 {

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t t = i;
            for (int k = 0; k < 8; ++k) t = t & 1 ? 0xEDB88320u ^ (t >> 1) : t >> 1;
            table[i] = t;
        }
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    // Length, type, data, CRC of type + data.
    const uint32_t n = uint32_t(data.size());
    uint8_t head[8] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n),
                       uint8_t(type[0]), uint8_t(type[1]), uint8_t(type[2]), uint8_t(type[3])};
    std::fwrite(head, 1, 8, f);
    if (n) std::fwrite(data.data(), 1, n, f);
    const uint32_t crc = crc32(data.data(), n, crc32(head + 4, 4));
    const uint8_t tail[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    std::fwrite(tail, 1, 4, f);
}

}  // namespace

bool write_png(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bits, RGBA, deflate, no filter, no interlace
    chunk(f, "IHDR", ihdr);

    // Raw scanlines (filter byte 0), in stored deflate blocks of up to 65535 bytes.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (width * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size() || raw.empty();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (raw.empty()) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return std::fclose(f) == 0;
}

}  // namespace sly4
