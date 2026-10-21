// LV2 kernel of the native port (phase 3): process start, guest memory areas, PPU
// threads and the syscall dispatcher. Firmware libraries are in hle.h.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cpu/ppu_context.h"
#include "kernel/guest_alloc.h"
#include "memory/guest_memory.h"

namespace sly4 {

class NidNames;
struct LoadedImage;

// CELL error codes (r3 of syscalls and library calls).
constexpr uint32_t CELL_OK = 0;
constexpr uint32_t CELL_EAGAIN = 0x80010001, CELL_EINVAL = 0x80010002, CELL_ENOSYS = 0x80010003,
                   CELL_ENOMEM = 0x80010004, CELL_ESRCH = 0x80010005, CELL_ENOENT = 0x80010006,
                   CELL_EDEADLK = 0x80010008, CELL_EPERM = 0x80010009, CELL_EBUSY = 0x8001000A,
                   CELL_ETIMEDOUT = 0x8001000B, CELL_EFAULT = 0x8001000D, CELL_ESTAT = 0x8001000F;

// Guest address map (the same areas RPCS3 uses).
namespace layout {
constexpr GuestAddr kMainEnd = 0x20000000;  // main memory: from the end of the image
constexpr GuestAddr kUserBase = 0x20000000, kUserSize = 0x10000000;  // sys_memory_allocate
constexpr GuestAddr kStackBase = 0xD0000000, kStackSize = 0x10000000;
}  // namespace layout

// Thrown by HLE functions to unwind the guest stack of the calling thread.
struct ProcessExit {
    int status;
};
struct ThreadExit {
    uint64_t value;
};

struct PpuThread {
    uint64_t id = 0;
    std::string name;
    PPUContext ctx;
    GuestAddr stack_addr = 0;  // lowest committed byte
    uint32_t stack_size = 0;
    int32_t priority = 0;
    GuestAddr tls_block = 0;  // TLS slot (r13 - 0x7030), 0 before sys_initialize_tls
};

// The guest thread running on this host thread (nullptr on host-only threads).
PpuThread* current_thread();

struct KernelOptions {
    bool trace = false;  // log every HLE call and syscall, not only the first of each
};

class Kernel {
public:
    Kernel(GuestMemory& mem, const LoadedImage& img);
    ~Kernel();

    // Memory areas, import binding, syscall handler. False if something is missing.
    bool init(const NidNames& names, const KernelOptions& opt);

    // Runs the game's entry point on the calling host thread until the process exits
    // (sys_process_exit) or the main thread returns. Returns the exit status.
    int run_main();

    // Called at exit or on timeout: call counts and the boot sequence.
    void print_summary() const;

    GuestMemory& mem() { return mem_; }
    const LoadedImage& image() const { return img_; }
    GuestAllocator& main_memory() { return *main_; }
    GuestAllocator& user_memory() { return *user_; }
    GuestAllocator& stacks() { return *stacks_; }

    // Thread-local storage (sys_initialize_tls): the image the game passes in and the
    // per-thread blocks. Returns the new r13 for a thread.
    void set_tls_template(GuestAddr image, uint32_t file_size, uint32_t mem_size);
    uint64_t alloc_tls(PpuThread& t);

    // New guest stack with a no-access guard page below it.
    bool alloc_stack(PpuThread& t, uint32_t size);

    uint64_t next_thread_id() { return next_thread_id_.fetch_add(1); }

private:
    GuestMemory& mem_;
    const LoadedImage& img_;
    std::unique_ptr<GuestAllocator> main_, user_, stacks_;
    std::atomic<uint64_t> next_thread_id_{0x01000000};

    std::mutex tls_lock_;
    GuestAddr tls_image_ = 0;
    uint32_t tls_file_ = 0, tls_mem_ = 0;
};

Kernel& kernel();  // the running process (set by the Kernel constructor)

// Syscall dispatcher (kernel/syscalls.cpp), installed as rt::hooks().syscall.
void syscall_dispatch(PPUContext& c, GuestMemory& m);
const char* syscall_name(uint32_t n);  // nullptr if unknown
struct SyscallStat {
    uint32_t number;
    uint64_t calls;
    bool implemented;
};
std::vector<SyscallStat> syscall_stats();

}  // namespace sly4
