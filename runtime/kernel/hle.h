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
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
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

    // --call-times: wall time spent inside each HLE function and syscall, per thread, to
    // see what threads wait on.
    bool timing = false;
    void add_time(const char* thread, const char* name, uint64_t ns);
    // --thread-timeline NAME: every call of that thread with the absolute time; repeated
    // calls from the same site (polling loops) are folded into one line.
    std::string timeline_thread;
    void timeline(const char* thread, const char* what, std::chrono::steady_clock::time_point t0);
    void print_times() const;

private:
    struct Timed {
        uint64_t calls = 0, ns = 0;
    };
    mutable std::mutex time_lock_;
    std::map<std::pair<std::string, std::string>, Timed> times_;
    std::chrono::steady_clock::time_point time_start_ = std::chrono::steady_clock::now();

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
    // Several images (the game and the firmware modules it runs): an import that one of
    // them exports is linked to that export directly; the rest get HLE thunks.
    bool bind(GuestMemory& mem, GuestAllocator& alloc, const std::vector<const LoadedImage*>& images,
              const NidNames& names);

    uint32_t thunk_base() const { return thunk_base_; }
    uint32_t thunk_bytes() const { return thunk_count_ * 4; }
    void call(PPUContext& c, GuestMemory& m, uint32_t target);

    // LR of a thread's entry function: returning there ends the thread.
    uint32_t thread_return() const { return thunk_base_ + 4 * uint32_t(imports_.size()); }

    // Descriptor of a native function the guest can call through a pointer (callbacks
    // the firmware hands to the game, such as the cellGcm context callback). Valid after
    // bind(); the same function gets the same descriptor.
    GuestAddr callback(GuestMemory& mem, const char* name, HleFn fn);
    // Logs a call to a firmware export that runs as it is (first call, ring, trace).
    void trace_lle(PPUContext& c, uint32_t target);
    // Development aid: called with the name of every library call (HLE: "name", LLE:
    // "module.name") before it runs.
    using CallHook = void (*)(PPUContext& c, GuestMemory& m, const char* name);
    void set_call_hook(CallHook h) { call_hook_ = h; }

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
    struct LleExport {
        std::string name;  // "module.name"
        std::atomic<uint64_t> calls{0};
    };
    std::unordered_map<uint32_t, LleExport> lle_names_;  // by export entry
    CallHook call_hook_ = nullptr;
    GuestMemory* mem_ = nullptr;
    uint32_t thunk_base_ = 0, thunk_count_ = 0;
    static constexpr uint32_t kCallbacks = 32;
    std::mutex callbacks_lock_;
    std::vector<HleFunction> callbacks_;
    uint32_t callback_first_ = 0;  // thunk index of the first callback
    GuestAddr callback_desc_ = 0;
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
HleModule hle_sys_prx_for_user_spu();   // kernel/syscalls_spu.cpp
HleModule hle_sys_prx_for_user_libc();  // kernel/hle_libc.cpp
HleModule hle_cell_gcm_sys();           // kernel/hle_cell_gcm.cpp
HleModule hle_cell_audio();             // kernel/hle_cell_audio.cpp
HleModule hle_sys_io();                 // kernel/hle_cell_pad.cpp

// Controller on port 0 as cellPadGetData reports it: digital buttons (CELL_PAD_CTRL_*
// bits of words 2 and 3), sticks (0x00-0xFF, 0x80 centred) and button pressures.
struct PadState {
    uint16_t digital1 = 0, digital2 = 0;
    uint16_t right_x = 0x80, right_y = 0x80, left_x = 0x80, left_y = 0x80;
    uint16_t pressure[12] = {};
};
void set_pad_state(const PadState& s);
void audio_summary();                   // audio counters for the end-of-run summary
void gcm_summary();                     // RSX counters for the end-of-run summary

// A string from the game's PARAM.SFO ("" if missing), e.g. "TITLE_ID".
std::string param_sfo_string(const char* key);

}  // namespace sly4
