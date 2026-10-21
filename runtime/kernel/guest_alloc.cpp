#include "kernel/guest_alloc.h"

#include <cstring>
#include <iterator>

#include "core/log.h"

namespace sly4 {

GuestAllocator::GuestAllocator(GuestMemory& mem, GuestAddr base, uint32_t size, const char* name)
    : mem_(mem), base_(base), size_(size), name_(name) {
    free_.emplace(base, size);
}

GuestAddr GuestAllocator::alloc(uint32_t size, uint32_t align, bool commit) {
    if (size == 0 || align == 0 || (align & (align - 1))) return 0;
    std::lock_guard<std::mutex> g(lock_);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const uint64_t start = it->first, end = start + it->second;
        const uint64_t a = (start + align - 1) & ~uint64_t(align - 1);
        if (a + size > end) continue;
        free_.erase(it);
        if (a > start) free_.emplace(GuestAddr(start), uint32_t(a - start));
        if (a + size < end) free_.emplace(GuestAddr(a + size), uint32_t(end - a - size));
        used_.emplace(GuestAddr(a), size);
        if (commit && !mem_.commit(GuestAddr(a), size, Protection::ReadWrite)) return 0;
        if (commit) std::memset(mem_.host(GuestAddr(a)), 0, size);
        return GuestAddr(a);
    }
    SLY4_WARN("memória esgotada na área %s: pedido de 0x%X bytes", name_, size);
    return 0;
}

bool GuestAllocator::free(GuestAddr addr) {
    std::lock_guard<std::mutex> g(lock_);
    auto u = used_.find(addr);
    if (u == used_.end()) return false;
    GuestAddr start = addr;
    uint64_t end = uint64_t(addr) + u->second;
    used_.erase(u);
    // Merge with the free neighbours.
    auto next = free_.lower_bound(start);
    if (next != free_.end() && next->first == end) {
        end += next->second;
        next = free_.erase(next);
    }
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (uint64_t(prev->first) + prev->second == start) {
            start = prev->first;
            free_.erase(prev);
        }
    }
    free_.emplace(start, uint32_t(end - start));
    return true;
}

uint32_t GuestAllocator::size_of(GuestAddr addr) const {
    std::lock_guard<std::mutex> g(lock_);
    auto u = used_.find(addr);
    return u == used_.end() ? 0 : u->second;
}

uint32_t GuestAllocator::used() const {
    std::lock_guard<std::mutex> g(lock_);
    uint64_t total = 0;
    for (const auto& [a, s] : used_) total += s;
    return uint32_t(total);
}

}  // namespace sly4
