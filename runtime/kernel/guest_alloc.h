// First-fit allocator over one area of the guest address space (main memory, user
// memory, stacks). Pages are committed on allocation; freed ranges go back to the
// free list but stay committed (they may share pages with live blocks).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

#include "memory/guest_memory.h"

namespace sly4 {

class GuestAllocator {
public:
    GuestAllocator(GuestMemory& mem, GuestAddr base, uint32_t size, const char* name);

    // Returns 0 when the area is full. `commit = false` leaves the pages reserved (the
    // caller commits what it needs, e.g. a stack above its guard page).
    GuestAddr alloc(uint32_t size, uint32_t align = 0x10, bool commit = true);
    bool free(GuestAddr addr);
    uint32_t size_of(GuestAddr addr) const;  // 0 if addr is not a live block

    GuestAddr base() const { return base_; }
    uint32_t capacity() const { return size_; }
    uint32_t used() const;
    bool contains(GuestAddr a) const { return a - base_ < size_; }

private:
    GuestMemory& mem_;
    GuestAddr base_;
    uint32_t size_;
    const char* name_;
    mutable std::mutex lock_;
    std::map<GuestAddr, uint32_t> free_;  // start -> size, coalesced
    std::map<GuestAddr, uint32_t> used_;  // start -> size
};

}  // namespace sly4
