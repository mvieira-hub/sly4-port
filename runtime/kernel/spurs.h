// SPURS objects shared between the cellSpurs HLE (kernel/hle_cell_spurs.cpp) and the
// native replacements of known SPU tasks (kernel/spurs_tasks.cpp).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "cpu/ppu_context.h"
#include "memory/guest_memory.h"

namespace sly4 {

// CellSpursEventFlag: 16 bits.
struct SpursEventFlag {
    std::mutex m;
    std::condition_variable cv;
    uint16_t bits = 0;
    bool auto_clear = true;
    void set(uint16_t b);
};

// CellSpursLFQueue: fixed-size entries, `depth` of them at most.
struct SpursQueue {
    std::mutex m;
    std::condition_variable cv;
    uint32_t size = 0, depth = 0;
    std::deque<std::vector<uint8_t>> q;
    bool push(std::vector<uint8_t> e, bool blocking);
    bool pop(std::vector<uint8_t>& e, bool blocking);
};

std::shared_ptr<SpursEventFlag> spurs_event_flag(uint32_t addr);
std::shared_ptr<SpursQueue> spurs_queue(uint32_t addr);

struct SpursTaskInfo {
    uint32_t taskset, id, elf;
    uint32_t arg[4];  // CellSpursTaskArgument
};

// Starts the native version of the task if its SPU program is a known one; returns its
// name, or nullptr when the task cannot run (no SPU recompiler yet).
const char* start_native_spurs_task(GuestMemory& m, const SpursTaskInfo& task);

// --verify-zlib: every request to the real EDGE zlib task (LLE SPURS) is inflated natively
// too and compared once the requesting thread is back from its wait.
void zlib_verify_call(PPUContext& c, GuestMemory& m, const char* name);
void zlib_verify_summary();

}  // namespace sly4
