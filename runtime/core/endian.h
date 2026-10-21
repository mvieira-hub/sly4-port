// Big-endian access for guest data. The PS3 (PPU, SPU, RSX) is big-endian and the
// host is little-endian x86-64, so every guest load/store swaps bytes.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sly4 {

static_assert(std::endian::native == std::endian::little, "the runtime assumes a little-endian host");

template <class T>
constexpr T byteswap(T v) noexcept {
    static_assert(std::is_integral_v<T>);
    if constexpr (sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
    } else {
        static_assert(sizeof(T) == 8);
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
    }
}

template <size_t N> struct UintOfSize;
template <> struct UintOfSize<1> { using type = uint8_t; };
template <> struct UintOfSize<2> { using type = uint16_t; };
template <> struct UintOfSize<4> { using type = uint32_t; };
template <> struct UintOfSize<8> { using type = uint64_t; };

// Reads a big-endian T (integer, float or double) from unaligned memory.
template <class T>
T load_be(const void* p) noexcept {
    using U = typename UintOfSize<sizeof(T)>::type;
    U u;
    std::memcpy(&u, p, sizeof(U));
    return std::bit_cast<T>(byteswap(u));
}

// Writes T as big-endian to unaligned memory.
template <class T>
void store_be(void* p, T v) noexcept {
    using U = typename UintOfSize<sizeof(T)>::type;
    U u = byteswap(std::bit_cast<U>(v));
    std::memcpy(p, &u, sizeof(U));
}

}  // namespace sly4
