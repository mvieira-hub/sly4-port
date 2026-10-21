// LV2 kernel of the native port (phase 3): process start, guest memory areas, PPU
// threads and the syscall dispatcher. Firmware libraries are in hle.h.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
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
                   CELL_ETIMEDOUT = 0x8001000B, CELL_EFAULT = 0x8001000D, CELL_ESTAT = 0x8001000F,
                   CELL_EALIGN = 0x80010010,
                   CELL_ECANCELED = 0x80010013, CELL_EEXIST = 0x80010014, CELL_EISCONN = 0x80010015,
                   CELL_ENOTCONN = 0x80010016;

// Guest address map (the same areas RPCS3 uses).
namespace layout {
constexpr GuestAddr kMainEnd = 0x20000000;  // main memory: from the end of the image
constexpr GuestAddr kUserBase = 0x20000000, kUserSize = 0x10000000;  // sys_memory_allocate
constexpr GuestAddr kMmapperBase = 0x30000000, kMmapperSize = 0x90000000;  // sys_mmapper ranges
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
    GuestAddr entry = 0;       // function address (from the entry descriptor)
    GuestAddr stack_addr = 0;  // lowest committed byte
    uint32_t stack_size = 0;
    int32_t priority = 0;
    GuestAddr tls_block = 0;  // TLS slot (r13 - 0x7030), 0 before sys_initialize_tls

    // Exit status for sys_ppu_thread_join.
    std::mutex m;
    std::condition_variable cv;
    bool joinable = false, done = false, joined = false;
    uint64_t exit_value = 0;
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

    // Process exit, from any guest thread; the host's main loop watches exited().
    void request_exit(int status);
    bool exited() const { return exited_.load(); }
    int exit_status() const { return exit_status_.load(); }

    // sys_ppu_thread_create: a thread that will run `entry_opd` with r3 = arg; returns
    // nullptr (and the CELL code in `error`) on failure. start_thread runs it on its own
    // host thread.
    std::shared_ptr<PpuThread> create_thread(GuestAddr entry_opd, uint64_t arg, int32_t prio, uint32_t stack_size,
                                             bool joinable, std::string name, uint32_t& error);
    bool start_thread(const std::shared_ptr<PpuThread>& t);
    std::shared_ptr<PpuThread> find_thread(uint64_t id);
    uint32_t join_thread(uint64_t id, uint64_t& value);
    uint32_t detach_thread(uint64_t id);

    // Called at exit or on timeout: call counts and the boot sequence.
    void print_summary() const;

    GuestMemory& mem() { return mem_; }
    const LoadedImage& image() const { return img_; }
    GuestAllocator& main_memory() { return *main_; }
    GuestAllocator& user_memory() { return *user_; }
    GuestAllocator& stacks() { return *stacks_; }
    GuestAllocator& mmapper_area() { return *mmapper_; }

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
    std::unique_ptr<GuestAllocator> main_, user_, stacks_, mmapper_;
    std::atomic<uint64_t> next_thread_id_{0x01000000};

    std::mutex tls_lock_;
    GuestAddr tls_image_ = 0;
    uint32_t tls_file_ = 0, tls_mem_ = 0;

    std::atomic<bool> exited_{false};
    std::atomic<int> exit_status_{0};
    std::mutex threads_lock_;
    std::map<uint64_t, std::shared_ptr<PpuThread>> threads_;

    // Runs a thread's entry function and marks it done (exit value in t.exit_value).
    // True if the entry function returned, false on sys_ppu_thread_exit; ProcessExit
    // propagates.
    bool run_entry(PpuThread& t);

public:
    void thread_entry_point(const std::shared_ptr<PpuThread>& t);  // host side of start_thread
};

Kernel& kernel();  // the running process (set by the Kernel constructor)

// System time in microseconds since the process started (sys_time_get_system_time,
// timer expiries).
uint64_t guest_system_time();

// Calls the guest function of descriptor `opd` from native code with the arguments
// already in r3..r10; returns when it does. LR, r1 and r2 are preserved for the caller.
void call_guest(PPUContext& c, GuestMemory& m, GuestAddr opd);

// Syscall dispatcher (kernel/syscalls.cpp), installed as rt::hooks().syscall.
using SyscallFn = void (*)(PPUContext&, GuestMemory&);
void register_syscall(uint32_t n, const char* name, SyscallFn fn);
void register_sync_syscalls();    // kernel/syscalls_sync.cpp
void register_memory_syscalls();  // kernel/memory.cpp

// sys_mmapper memory blocks, also used by the sysPrxForUser wrappers (kernel/memory.cpp).
uint32_t mmapper_allocate_memory(uint64_t size, uint64_t flags, uint32_t& mem_id);
uint32_t mmapper_free_memory(uint32_t mem_id);
uint32_t mmapper_map_memory(GuestAddr addr, uint32_t mem_id);
uint32_t mmapper_unmap_memory(GuestAddr addr, uint32_t& mem_id);
void syscall_dispatch(PPUContext& c, GuestMemory& m);
const char* syscall_name(uint32_t n);  // nullptr if unknown
struct SyscallStat {
    uint32_t number;
    uint64_t calls;
    bool implemented;
};
std::vector<SyscallStat> syscall_stats();

}  // namespace sly4
