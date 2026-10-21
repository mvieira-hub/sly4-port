#include "check.h"

#include "core/endian.h"
#include "memory/guest_memory.h"

using namespace sly4;

TEST(endian_load_store) {
    uint8_t buf[8] = {};
    store_be<uint32_t>(buf, 0x11223344u);
    CHECK_EQ(buf[0], 0x11);
    CHECK_EQ(buf[3], 0x44);
    CHECK_EQ(load_be<uint32_t>(buf), 0x11223344u);
    store_be<float>(buf, 1.0f);  // IEEE 0x3F800000
    CHECK_EQ(buf[0], 0x3F);
    CHECK_EQ(buf[1], 0x80);
    CHECK_EQ(load_be<float>(buf), 1.0f);
    store_be<int16_t>(buf, -2);
    CHECK_EQ(buf[0], 0xFF);
    CHECK_EQ(buf[1], 0xFE);
    store_be<uint64_t>(buf, 0x0102030405060708ull);
    CHECK_EQ(buf[0], 0x01);
    CHECK_EQ(buf[7], 0x08);
    CHECK_EQ(load_be<double>(buf), std::bit_cast<double>(0x0102030405060708ull));
}

TEST(guest_memory_commit_and_access) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(!mem.is_committed(0x10000, 4));
    CHECK(mem.commit(0x10000, 0x1800, Protection::ReadWrite));  // widened to two pages
    CHECK(mem.is_committed(0x10000, 0x2000));
    CHECK(!mem.is_committed(0x10000, 0x2001));
    CHECK_EQ(mem.committed_bytes(), uint64_t(0x2000));
    CHECK_EQ(mem.read<uint32_t>(0x10000), 0u);  // fresh pages are zero

    mem.write<uint32_t>(0x10000, 0xDEADBEEF);
    CHECK_EQ(mem.host(0x10000)[0], 0xDE);
    CHECK_EQ(mem.read<uint32_t>(0x10000), 0xDEADBEEFu);
    mem.write<uint16_t>(0x10FFF, 0xABCD);  // unaligned, crosses a page boundary
    CHECK_EQ(mem.read<uint16_t>(0x10FFF), uint16_t(0xABCD));

    CHECK(mem.guest_of(mem.host(0x10010)).has_value());
    CHECK_EQ(*mem.guest_of(mem.host(0x10010)), 0x10010u);
    int local = 0;
    CHECK(!mem.guest_of(&local).has_value());

    mem.decommit(0x11000, 0x1000);
    CHECK(mem.is_committed(0x10000, 0x1000));
    CHECK(!mem.is_committed(0x11000, 4));
}

TEST(guest_memory_bounds_and_strings) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(!mem.commit(0xFFFFF000u, 0x2000, Protection::ReadWrite));  // would pass 4 GiB
    CHECK(mem.commit(0xFFFFF000u, 0x1000, Protection::ReadWrite));   // the very last page is fine
    mem.write<uint32_t>(0xFFFFFFFCu, 0x01020304);
    CHECK_EQ(mem.read<uint32_t>(0xFFFFFFFCu), 0x01020304u);

    CHECK(mem.commit(0x20000, 0x1000, Protection::ReadWrite));
    const char text[] = "cellGcmSys";
    mem.copy_to(0x20000, text, sizeof(text));
    CHECK_EQ(mem.read_cstring(0x20000), std::string("cellGcmSys"));
    // An unterminated string that runs into uncommitted memory stops at the page end.
    std::memset(mem.host(0x20FF0), 'x', 16);
    CHECK_EQ(mem.read_cstring(0x20FF0).size(), size_t(16));
}

TEST(guest_memory_vec128_is_byte_reversed) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(mem.commit(0x30000, 0x1000, Protection::ReadWrite));
    for (int i = 0; i < 16; ++i) mem.host(0x30000)[i] = static_cast<uint8_t>(i);
    Vec128 v = mem.read128(0x30000);
    CHECK_EQ(v.b[0], 15);
    CHECK_EQ(v.b[15], 0);
    // Host lane 0 (little-endian u32) holds guest word 3: bytes 12..15 = 0x0C0D0E0F.
    uint32_t lane0;
    std::memcpy(&lane0, v.b, 4);
    CHECK_EQ(lane0, 0x0C0D0E0Fu);
    mem.write128(0x30010, v);
    CHECK(std::memcmp(mem.host(0x30000), mem.host(0x30010), 16) == 0);
}

TEST(guest_memory_read_only_protection) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(mem.commit(0x40000, 0x1000, Protection::ReadWrite));
    mem.write<uint32_t>(0x40000, 0x12345678);
    CHECK(mem.protect(0x40000, 0x1000, Protection::ReadOnly));
    CHECK_EQ(mem.read<uint32_t>(0x40000), 0x12345678u);  // still readable
    CHECK(mem.protect(0x40000, 0x1000, Protection::ReadWrite));
    mem.write<uint32_t>(0x40000, 0);
    CHECK_EQ(mem.read<uint32_t>(0x40000), 0u);
}
