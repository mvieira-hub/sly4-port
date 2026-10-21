#include "loader/nid.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace sly4 {

namespace {

constexpr uint8_t kNidSuffix[16] = {0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                                    0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A};

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

}  // namespace

std::array<uint8_t, 20> sha1(const void* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> msg(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    const uint64_t bits = uint64_t(size) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &msg[chunk + i * 4];
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
    return out;
}

uint32_t nid_of(std::string_view name) {
    std::string buf(name);
    buf.append(reinterpret_cast<const char*>(kNidSuffix), sizeof(kNidSuffix));
    auto d = sha1(buf.data(), buf.size());
    return uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
}

bool NidNames::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string w;
        while (words >> w) names_.emplace(nid_of(w), w);
    }
    return true;
}

const std::string* NidNames::find(uint32_t nid) const {
    auto it = names_.find(nid);
    return it == names_.end() ? nullptr : &it->second;
}

}  // namespace sly4
