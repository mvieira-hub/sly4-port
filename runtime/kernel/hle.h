// HLE (high-level emulation) of the firmware libraries the game imports.
//
// The PS3 loader fills each import slot with the address of the export's function
// descriptor {function, toc}; the import stub loads it and branches there (bctr). Here
// every import gets a descriptor in a small guest area whose function address is a
// "thunk" address no code lives at. rt::call_indirect (generated code) and the
// interpreter recognise that range and call HleTable::call, which runs the native
// implementation, or, for what is not implemented yet, logs the call and returns
// CELL_OK in r3.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "cpu/ppu_context.h"
#include "memory/guest_memory.h"

namespace sly4 {

class GuestAllocator;
class NidNames;
struct LoadedImage;

// Arguments in r3..r10, result in r3 (set by the function).
using HleFn = void (*)(PPUContext&, GuestMemory&);

struct HleFunction {
    const char* name;  // the NID is derived from it
    HleFn fn;
};

struct HleModule {
    const char* name;
    std::vector<HleFunction> functions;
};

// Shared by HLE functions and syscalls: first call of each entry in order (the boot
// sequence), call counts, and the full trace when enabled.
class CallLog {
public:
    bool trace = false;
    // Returns true on the first call of `key` (an id unique per entry).
    bool first_call(uint32_t key);
    void record(const std::string& line);
    std::vector<std::string> sequence() const;

    // In-memory ring of the latest calls (thread, name, r3, r4, caller), printed with the
    // summary: shows the last steps before a hang without slowing the threads down.
    void ring(const char* thread, const char* name, uint64_t r3, uint64_t r4, uint32_t lr);
    void print_ring(size_t last) const;

private:
    mutable std::mutex lock_;
    std::unordered_set<uint32_t> seen_;
    std::vector<std::string> sequence_;

    struct RingEntry {
        const char* thread;
        const char* name;
        uint64_t r3, r4;
        uint32_t lr;
    };
    static constexpr size_t kRing = 4096;
    mutable std::mutex ring_lock_;
    RingEntry ring_[kRing] = {};
    size_t ring_next_ = 0;
};
CallLog& call_log();

class HleTable {
public:
    void add(const HleModule& module);

    // Writes descriptors for every import of `img` into a block taken from `alloc` and
    // points the import slots at them. Logs how many imports have an implementation.
    bool bind(GuestMemory& mem, GuestAllocator& alloc, const LoadedImage& img, const NidNames& names);

    uint32_t thunk_base() const { return thunk_base_; }
    uint32_t thunk_bytes() const { return thunk_count_ * 4; }
    void call(PPUContext& c, GuestMemory& m, uint32_t target);

    // LR of a thread's entry function: returning there ends the thread.
    uint32_t thread_return() const { return thunk_base_ + 4 * uint32_t(imports_.size()); }

    struct Stat {
        std::string module, name;
        uint64_t calls;
        bool implemented;
    };
    std::vector<Stat> stats() const;

private:
    struct Known {
        std::string module, name;
        HleFn fn;
    };
    struct Import {
        std::string module, name;
        uint32_t nid = 0;
        HleFn fn = nullptr;
        std::atomic<uint64_t> calls{0};
    };
    std::vector<Known> known_;
    std::deque<Import> imports_;
    uint32_t thunk_base_ = 0, thunk_count_ = 0;
};

// The table used by the running process (set by the kernel).
HleTable& hle();

// Modules (runtime/kernel/hle_*.cpp).
HleModule hle_sys_prx_for_user();
HleModule hle_sys_fs();
HleModule hle_cell_game();
HleModule hle_cell_sysutil();
HleModule hle_cell_sysutil_avconf_ext();
HleModule hle_cell_sysmodule();
HleModule hle_cell_rtc();
HleModule hle_cell_net_ctl();
HleModule hle_sys_net();
HleModule hle_cell_spurs();

// A string from the game's PARAM.SFO ("" if missing), e.g. "TITLE_ID".
std::string param_sfo_string(const char* key);

}  // namespace sly4
