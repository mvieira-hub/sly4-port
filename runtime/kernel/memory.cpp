// LV2 memory: sys_memory (user memory in 64 KiB / 1 MiB pages) and sys_mmapper (address
// ranges reserved first, memory blocks mapped into them later).
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"

namespace sly4 {

namespace {

constexpr uint32_t kUserMemoryTotal = 0x0D500000;  // 213 MiB, what a game sees on the PS3
constexpr uint64_t kPage1M = 0x400, kPage64K = 0x200;
constexpr uint32_t kMemoryId = 0x08000000;  // sys_mmapper memory ids

uint32_t page_of(uint64_t flags) { return (flags & 0xF00) == kPage64K ? 0x10000 : 0x100000; }

// A block from sys_mmapper_allocate_memory; it takes guest pages only while mapped.
struct MemoryBlock : KernelObject {
    uint32_t size = 0;
    GuestAddr mapped_at = 0;
};

std::mutex g_lock;
uint64_t g_mapped = 0;  // bytes of mmapper blocks, counted against user memory

uint32_t user_memory_used() {
    std::lock_guard<std::mutex> g(g_lock);
    return kernel().user_memory().used() + uint32_t(g_mapped);
}

// ---------------------------------------------------------------- sys_memory
void sys_memory_allocate(PPUContext& c, GuestMemory& m) {
    const uint32_t size = uint32_t(c.gpr[3]);
    const uint32_t align = page_of(c.gpr[4]);
    if (!size || size % align) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    if (user_memory_used() + uint64_t(size) > kUserMemoryTotal) {
        c.gpr[3] = CELL_ENOMEM;
        return;
    }
    const GuestAddr a = kernel().user_memory().alloc(size, align);
    if (!a) {
        c.gpr[3] = CELL_ENOMEM;
        return;
    }
    m.write<uint32_t>(uint32_t(c.gpr[5]), a);
    c.gpr[3] = CELL_OK;
}
void sys_memory_free(PPUContext& c, GuestMemory&) {
    c.gpr[3] = kernel().user_memory().free(uint32_t(c.gpr[3])) ? CELL_OK : CELL_EINVAL;
}
void sys_memory_get_page_attribute(PPUContext& c, GuestMemory& m) {
    const uint32_t addr = uint32_t(c.gpr[3]), out = uint32_t(c.gpr[4]);
    if (!m.is_committed(addr, 1)) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    m.write<uint64_t>(out, 0x40000);  // SYS_MEMORY_PROT_READ_WRITE
    m.write<uint64_t>(out + 8, 0xF);  // SYS_MEMORY_ACCESS_RIGHT_ANY
    m.write<uint32_t>(out + 16, kernel().user_memory().contains(addr) ? 0x100000 : 0x1000);
    c.gpr[3] = CELL_OK;
}
void sys_memory_get_user_memory_size(PPUContext& c, GuestMemory& m) {
    const uint32_t info = uint32_t(c.gpr[3]);
    const uint32_t used = user_memory_used();
    m.write<uint32_t>(info, kUserMemoryTotal);
    m.write<uint32_t>(info + 4, used < kUserMemoryTotal ? kUserMemoryTotal - used : 0);
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- sys_mmapper
void sys_mmapper_allocate_address(PPUContext& c, GuestMemory& m) {
    const uint64_t size = c.gpr[3], align = c.gpr[5];
    if (!size || size % 0x10000000 || (align && align % 0x10000000)) {  // 256 MiB units
        c.gpr[3] = CELL_EALIGN;
        return;
    }
    const GuestAddr a = kernel().mmapper_area().alloc(uint32_t(size), align ? uint32_t(align) : 0x10000000, false);
    if (!a) {
        c.gpr[3] = CELL_ENOMEM;
        return;
    }
    m.write<uint32_t>(uint32_t(c.gpr[6]), a);
    c.gpr[3] = CELL_OK;
}
void sys_mmapper_free_address(PPUContext& c, GuestMemory&) {
    c.gpr[3] = kernel().mmapper_area().free(uint32_t(c.gpr[3])) ? CELL_OK : CELL_EINVAL;
}

}  // namespace

uint32_t mmapper_allocate_memory(uint64_t size, uint64_t flags, uint32_t& mem_id) {
    const uint32_t page = page_of(flags);
    if (!size || size % page) return CELL_EALIGN;
    if (user_memory_used() + size > kUserMemoryTotal) return CELL_ENOMEM;
    auto b = std::make_shared<MemoryBlock>();
    b->size = uint32_t(size);
    mem_id = idm().add(b, kMemoryId);
    std::lock_guard<std::mutex> g(g_lock);
    g_mapped += size;
    return CELL_OK;
}

uint32_t mmapper_free_memory(uint32_t mem_id) {
    auto b = idm().get<MemoryBlock>(mem_id);
    if (!b) return CELL_ESRCH;
    if (b->mapped_at) return CELL_EBUSY;
    idm().remove(mem_id);
    std::lock_guard<std::mutex> g(g_lock);
    g_mapped -= b->size;
    return CELL_OK;
}

uint32_t mmapper_map_memory(GuestAddr addr, uint32_t mem_id) {
    auto b = idm().get<MemoryBlock>(mem_id);
    if (!b) return CELL_ESRCH;
    if (b->mapped_at) return CELL_EBUSY;
    if (!kernel().mmapper_area().contains(addr)) return CELL_EINVAL;
    if (!kernel().mem().commit(addr, b->size, Protection::ReadWrite)) return CELL_ENOMEM;
    std::memset(kernel().mem().host(addr), 0, b->size);
    b->mapped_at = addr;
    return CELL_OK;
}

uint32_t mmapper_unmap_memory(GuestAddr addr, uint32_t& mem_id) {
    // Linear search is fine: a game maps a handful of blocks.
    for (uint32_t n = 1; n < 0x10000; ++n) {
        auto b = idm().get<MemoryBlock>(kMemoryId | n);
        if (!b) continue;
        if (b->mapped_at == addr) {
            kernel().mem().decommit(addr, b->size);
            b->mapped_at = 0;
            mem_id = kMemoryId | n;
            return CELL_OK;
        }
    }
    return CELL_EINVAL;
}

void register_memory_syscalls() {
    register_syscall(330, "sys_mmapper_allocate_address", sys_mmapper_allocate_address);
    register_syscall(331, "sys_mmapper_free_address", sys_mmapper_free_address);
    register_syscall(348, "sys_memory_allocate", sys_memory_allocate);
    register_syscall(349, "sys_memory_free", sys_memory_free);
    register_syscall(351, "sys_memory_get_page_attribute", sys_memory_get_page_attribute);
    register_syscall(352, "sys_memory_get_user_memory_size", sys_memory_get_user_memory_size);
}

}  // namespace sly4
