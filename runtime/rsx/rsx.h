// The RSX, the PS3's GPU (docs/GPU.md). The game fills a FIFO in main memory with
// commands (method writes) and moves the control register's "put"; a host thread runs
// them: it follows jumps and calls, keeps every register the commands write (rsx_state.h),
// does the copies and synchronisation itself, and hands clears, draws and flips to a
// backend (Vulkan, or none).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "memory/guest_memory.h"

namespace sly4::rsx {

constexpr GuestAddr kLocalBase = 0xC0000000;
constexpr uint32_t kLocalSize = 0x0F900000;      // 249 MiB, as libgcm reports
constexpr uint32_t kReportsOffset = 0x0E000000;  // reports: local memory, or IO for main
constexpr uint32_t kLocationLocal = 0, kLocationMain = 1;

// ---------------------------------------------------------------- IO map (1 MiB pages)
std::mutex& io_lock();
bool io_to_ea(uint32_t offset, uint32_t& ea);
bool ea_to_offset(GuestAddr ea, uint32_t& offset);
void map_io(uint32_t ea_page, uint32_t io_page, uint32_t pages);  // with io_lock held
uint32_t unmap_io(uint32_t ea_page);                              // pages unmapped (0: none)
bool io_range_free(uint32_t io_page, uint32_t pages);

// Guest address of `offset` in a location (local memory, or main memory through IO);
// 0 if it is not mapped.
GuestAddr location_address(uint32_t location, uint32_t offset);

// ---------------------------------------------------------------- display
struct DisplayBuffer {
    uint32_t offset = 0, pitch = 0, width = 0, height = 0;  // offset in local memory
};
void set_display_buffer(uint32_t id, const DisplayBuffer& b);
DisplayBuffer display_buffer(uint32_t id);

// ---------------------------------------------------------------- the thread
// Starts the RSX on the FIFO behind `control` (put, get, ref) with the labels at `labels`.
void start(GuestMemory& mem, GuestAddr control, GuestAddr labels);
GuestAddr control_address();
GuestAddr labels_address();

// Development aids: the commands of frame `frame` (counted in flips) written to `path`,
// and counters for the end-of-run summary.
void set_capture(uint32_t frame, const std::string& path);
void summary();

}  // namespace sly4::rsx
