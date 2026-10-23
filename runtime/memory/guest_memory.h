// The PS3 address space inside the host process.
//
// PS3 pointers are 32 bits, so 4 GiB are reserved in one contiguous block and a guest
// address is simply an offset from base(). Regions are committed on demand (ELF
// segments now; user memory, stacks, RSX memory later); everything else stays
// reserved with no access, so a stray guest access faults and the fault handler
// reports it as a guest address.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <cstring>
#include <optional>
#include <string>

#include "core/endian.h"

namespace sly4 {

using GuestAddr = uint32_t;

// Host protection of committed guest pages. Guest code is never executed natively
// (it is recompiled), so there is no execute flag; read-only catches writes to code.
enum class Protection { NoAccess, ReadOnly, ReadWrite };

// A VMX/SPU register as it sits in host memory. Guest bytes are stored fully
// reversed (byte 15 of the guest vector is b[0]), so a little-endian lane i of the
// host register holds guest element (N - 1 - i). The recompiler (phase 2) builds on
// this convention.
struct alignas(16) Vec128 {
    uint8_t b[16];
};

class GuestMemory {
public:
    static constexpr uint64_t kSize = 1ull << 32;
    static constexpr uint32_t kPageSize = 0x1000;

    GuestMemory() = default;
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    ~GuestMemory() { release(); }

    bool reserve();
    void release();
    bool reserved() const { return base_ != nullptr; }

    // Page-granular: the range is widened to whole 4 KiB pages. New pages are zeroed.
    bool commit(GuestAddr addr, uint32_t size, Protection prot);
    bool protect(GuestAddr addr, uint32_t size, Protection prot);
    void decommit(GuestAddr addr, uint32_t size);
    bool is_committed(GuestAddr addr, uint32_t size) const;
    uint64_t committed_bytes() const;

    uint8_t* base() const { return base_; }
    uint8_t* host(GuestAddr addr) const { return base_ + addr; }
    // Guest address of a host pointer inside the reservation, if it is one.
    std::optional<GuestAddr> guest_of(const void* p) const;

    template <class T>
    T read(GuestAddr addr) const {
        return load_be<T>(host(addr));
    }
    template <class T>
    void write(GuestAddr addr, T v) {
        store_be<T>(host(addr), v);
    }
    Vec128 read128(GuestAddr addr) const;
    void write128(GuestAddr addr, const Vec128& v);

    void copy_to(GuestAddr dst, const void* src, size_t size) { std::memcpy(host(dst), src, size); }
    void copy_from(void* dst, GuestAddr src, size_t size) const { std::memcpy(dst, host(src), size); }
    // NUL-terminated string at addr, at most max_len bytes (stops at uncommitted memory).
    std::string read_cstring(GuestAddr addr, size_t max_len = 4096) const;

private:
    // Which pages are committed is asked to the OS (VirtualQuery), not tracked here.
    uint8_t* base_ = nullptr;
};

// Installs a vectored exception handler that logs access violations inside the
// guest reservation as guest addresses (then lets the crash continue normally).
void install_guest_fault_handler(const GuestMemory& mem);

// Debugging (--watch-write): every write to [addr, addr + size), inside one page, calls
// `hook` with the address and the host instruction, on the writing thread, before it
// happens. The page must be committed; false otherwise.
using WriteWatchHook = void (*)(GuestAddr at, const void* host_pc);
bool watch_writes(GuestAddr addr, uint32_t size, WriteWatchHook hook);

// Writes to memory the GPU drew and has not written back yet (the RSX renderer's
// surfaces): the range's pages are kept read-only, and the first write by anyone else
// (PPU, SPU DMA, HLE) ends the tracking and records the range, for the renderer to drop
// its copy instead of writing it back over the new data later (RPCS3: invalidate on
// write). The game reuses that memory for other things: Bink's video planes landed on a
// surface drawn seconds before.
void track_writes(GuestAddr addr, uint32_t size);
void untrack_writes(GuestAddr addr, uint32_t size);    // the renderer writes it back itself
void before_host_write(GuestAddr addr, uint32_t size);  // kernel-mode writes (ReadFile)
void before_host_write(const void* host, uint64_t size);  // the same, by host pointer (none if not guest memory)
bool take_written(std::vector<std::pair<GuestAddr, uint32_t>>& out);  // ranges written since the last call
// While alive, this thread's writes to tracked pages pass without ending any tracking.
struct HostWriteScope {
    HostWriteScope();
    ~HostWriteScope();
};

}  // namespace sly4
