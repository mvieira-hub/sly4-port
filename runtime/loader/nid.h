// PS3 function NIDs: imports are referenced by a 32-bit hash of the function name,
// the first 4 bytes (little-endian) of SHA-1(name + fixed suffix).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sly4 {

std::array<uint8_t, 20> sha1(const void* data, size_t size);

uint32_t nid_of(std::string_view name);

// NID -> name, built from a list of candidate names (tools/data/ps3_nid_candidates.txt:
// names separated by whitespace, '#' comments). Unknown NIDs simply have no name.
class NidNames {
public:
    bool load_file(const std::string& path);
    void add(std::string_view name) { names_.emplace(nid_of(name), std::string(name)); }
    const std::string* find(uint32_t nid) const;
    size_t size() const { return names_.size(); }

private:
    std::unordered_map<uint32_t, std::string> names_;
};

}  // namespace sly4
