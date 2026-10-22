// Atomicity between the PPU's reservations (lwarx/stwcx., ldarx/stdcx.) and the SPU's
// 128-byte lock lines (getllar/putllc/putlluc). On the Cell both work on whole cache
// lines; here a PPU store-conditional is a compare-and-swap of 4 or 8 bytes and an SPU
// putllc a compare-and-copy of 128, so both take the same lock for the line, or a PPU
// update could land between the SPU's compare and its copy and be overwritten.
// Plain stores take no lock: an SPU putllc therefore writes only the bytes it changed,
// each 8-byte word with a compare-and-swap, so that a plain store to another part of the
// line in the same instant survives (Havok keeps a PPU sys_lwmutex in the same line as
// the cellSyncMutex its SPU tasks hammer; its owner field used to be lost).
#pragma once

#include <cstdint>
#include <mutex>

#include "memory/guest_memory.h"

namespace sly4 {

// The lock of the 128-byte line holding `ea` (striped: several lines share one).
std::mutex& guest_line_lock(GuestAddr ea);

// Called after every atomic write to a line (PPU store-conditional, SPU putllc/putlluc):
// wakes whoever waits for a lock-line reservation to be lost (SPU threads).
using LineWriteHook = void (*)(GuestAddr line);
void set_line_write_hook(LineWriteHook h);
void guest_line_written(GuestAddr ea);

// PPU load-and-reserve: copies the 128-byte line holding `ea` to `line` (consistently,
// under the line lock).
void guest_load_reserve(GuestMemory& mem, GuestAddr ea, uint8_t* line);
// PPU store-conditional: writes `desired` (raw big-endian bytes, `size` 4 or 8) at ea if
// the line still equals `line`, as the reservation saw it.
bool guest_store_conditional(GuestMemory& mem, GuestAddr ea, const uint8_t* line, uint64_t desired, int size);
// Writes to a 128-byte host line the bytes where `now` differs from `old` (what the
// reservation saw), leaving every other byte as it is in memory. Call with the line lock.
void guest_store_line_changes(uint8_t* line, const uint8_t* old, const uint8_t* now);

}  // namespace sly4

namespace sly4 {

// Debugging (--watch-line): every write to the watched 128-byte lines (up to 32), by anyone,
// is logged with the words that changed.
constexpr int kMaxDebugLines = 32;
void add_debug_line(GuestAddr line);
// The first watched line that [ea, ea + size) touches, or 0; debug_lines_hit lists them all.
GuestAddr debug_line_hit(GuestAddr ea, uint32_t size);
int debug_lines_hit(GuestAddr ea, uint32_t size, GuestAddr (&out)[kMaxDebugLines]);
// Names the current PPU thread in the log (set by the kernel).
using DebugWhoHook = void (*)(char* buf, size_t size);
void set_debug_who_hook(DebugWhoHook h);
void debug_line_log(GuestMemory& mem, GuestAddr line, const uint8_t* before, const char* who, uint32_t pc);

}  // namespace sly4
