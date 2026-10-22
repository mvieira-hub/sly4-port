// cellGcmSys, the PS3's graphics library (docs/GPU.md, 5a).
//
// The game builds RSX command buffers itself (the inline part of libgcm is compiled
// into the EBOOT) in a FIFO in main memory mapped into the RSX's IO space, and moves the
// control register's "put" as it goes. This module sets that up as libgcm_sys does (and
// RPCS3's cellGcmSys.cpp): local memory at 0xC0000000, the IO map, the default command
// buffer in 32 KiB fragments with the callback that moves between them, labels, reports
// and the VBlank interrupt. The RSX itself is runtime/rsx.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

#include "core/host_sleep.h"
#include "core/log.h"
#include "kernel/guest_alloc.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "rsx/rsx.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

constexpr uint32_t CELL_GCM_ERROR_FAILURE = 0x802100FF, CELL_GCM_ERROR_INVALID_VALUE = 0x80210003,
                   CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004, CELL_GCM_ERROR_ADDRESS_OVERWRAP = 0x80210005;

using rsx::kLocalBase;
using rsx::kLocalSize;
using rsx::kReportsOffset;
constexpr uint32_t kFragment = 32 * 1024;  // default command buffer fragments
constexpr uint32_t kOldJump = 0x20000000;  // RSX jump command
constexpr uint32_t kFlipMethod = 0xFEAC;
constexpr uint32_t kPages = 4096;          // 1 MiB pages of the 4 GiB space

struct Tile {
    uint32_t location, offset, size, pitch, comp, base, bank;
    bool bound;
};
struct Gcm {
    std::mutex lock;
    bool initialized = false;
    // CellGcmConfig
    uint32_t io_address = 0, io_size = 0;
    // Guest structures: CellGcmContextData, CellGcmControl (put, get, ref), 256 labels.
    GuestAddr context = 0, control = 0, labels = 0;
    uint32_t buffer = 0, fragments = 0;  // default command buffer (ea) and its 32 KiB fragments
    Tile tiles[15] = {};
    bool zcull[8] = {};
    GuestAddr vblank_handler = 0;
    std::atomic<uint64_t> vblank_count{0};
};
Gcm g;

bool io_to_address(uint32_t offset, uint32_t& ea) { return rsx::io_to_ea(offset, ea); }
bool address_to_offset(uint32_t ea, uint32_t& offset) { return rsx::ea_to_offset(ea, offset); }

uint32_t control_get(GuestMemory& m) { return m.read<uint32_t>(g.control + 4); }

// ---------------------------------------------------------------- default command buffer
// Fragment n spans [buffer + 32K * n, buffer + 32K * (n + 1) - 4); the first one skips
// 4 KiB reserved by libgcm, and the last word of each is left for the jump.
void fragment_bounds(uint32_t n, uint32_t& begin, uint32_t& end) {
    begin = g.buffer + kFragment * n + (n == 0 ? 4096 : 0);
    end = g.buffer + kFragment * (n + 1) - 4;
}

// Is the RSX (get) somewhere other than the fragment [begin, end]?
bool get_outside(uint32_t get, uint32_t begin, uint32_t end) {
    uint32_t b = 0, e = 0;
    address_to_offset(begin, b);
    address_to_offset(end, e);
    return get < b || get > e;
}

// The context callback (CellGcmContextData::callback) the game calls when the current
// fragment is full: flushes it, jumps to the next fragment and waits until the RSX has
// left that one.
void gcm_callback(PPUContext& c, GuestMemory& m) {
    const GuestAddr ctx = a32(c, 3);
    const uint32_t current = m.read<uint32_t>(ctx + 8);
    uint32_t put = 0;
    address_to_offset(current, put);
    m.write<uint32_t>(g.control, put);
    const uint32_t n = ((current - g.buffer) / kFragment + 1) % g.fragments;
    uint32_t begin, end, target = 0;
    fragment_bounds(n, begin, end);
    address_to_offset(begin, target);
    m.write<uint32_t>(current, kOldJump | target);
    m.write<uint32_t>(ctx, begin);
    m.write<uint32_t>(ctx + 4, end);
    m.write<uint32_t>(ctx + 8, begin);
    while (!get_outside(control_get(m), begin, end) && !kernel().exited())
        host_sleep_us(100);
    c.gpr[3] = CELL_OK;
}

// VBlank at 60 Hz: the counter, and the game's handler on an interrupt thread.
void vblank_thread(PpuThread& t) {
    GuestMemory& m = kernel().mem();
    auto next = std::chrono::steady_clock::now();
    while (!kernel().exited()) {
        next += std::chrono::microseconds(16683);
        host_sleep_until(next);
        g.vblank_count.fetch_add(1);
        GuestAddr handler;
        {
            std::lock_guard<std::mutex> lk(g.lock);
            handler = g.vblank_handler;
        }
        if (handler) {
            t.ctx.gpr[3] = 1;  // head
            call_guest(t.ctx, m, handler);
        }
    }
}

// ---------------------------------------------------------------- functions
// _cellGcmInitBody(CellGcmContextData** context, u32 cmdSize, u32 ioSize, u32 ioAddress)
void _cellGcmInitBody(PPUContext& c, GuestMemory& m) {
    const uint32_t cmd_size = a32(c, 4), io_size = a32(c, 5), io_address = a32(c, 6);
    if ((io_address & 0xFFFFF) || (io_size & 0xFFFFF) || cmd_size < kFragment || cmd_size > io_size) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    std::lock_guard<std::mutex> lk(g.lock);
    if (g.initialized) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    if (!m.is_committed(kLocalBase, kLocalSize) && !m.commit(kLocalBase, kLocalSize, Protection::ReadWrite)) {
        SLY4_ERROR("cellGcm: não foi possível reservar a memória local (0x%08X, %u MiB)", kLocalBase, kLocalSize >> 20);
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    GuestAllocator& main = kernel().main_memory();
    g.context = main.alloc(0x100, 0x100);
    g.control = main.alloc(0x1000, 0x1000);
    g.labels = main.alloc(0x1000, 0x1000);
    const GuestAddr callback = hle().callback(m, "cellGcmCallback", gcm_callback);
    if (!g.context || !g.control || !g.labels || !callback) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    std::memset(m.host(g.control), 0, 0x1000);
    std::memset(m.host(g.labels), 0, 0x1000);
    g.control += 0x40;  // CellGcmControl sits 0x40 into the RSX DMA control page
    m.write<uint32_t>(g.control + 8, 0xFFFFFFFF);  // ref

    g.io_address = io_address;
    g.io_size = io_size;
    {
        std::lock_guard<std::mutex> io(rsx::io_lock());
        rsx::map_io(io_address >> 20, 0, io_size >> 20);
    }
    g.buffer = io_address;
    g.fragments = cmd_size / kFragment;
    uint32_t begin, end;
    fragment_bounds(0, begin, end);
    m.write<uint32_t>(g.context, begin);
    m.write<uint32_t>(g.context + 4, end);
    m.write<uint32_t>(g.context + 8, begin);
    m.write<uint32_t>(g.context + 12, callback);
    m.write<uint32_t>(a32(c, 3), g.context);
    g.initialized = true;

    rsx::start(m, g.control, g.labels);
    kernel().start_native_thread("_gcm_intr_thread", vblank_thread);
    SLY4_INFO("cellGcm: comandos em 0x%08X (%u fragmentos de 32 KiB), IO 0x%08X + %u MiB, memória local 0x%08X + %u MiB",
              g.buffer, g.fragments, io_address, io_size >> 20, kLocalBase, kLocalSize >> 20);
    c.gpr[3] = CELL_OK;
}

// cellGcmGetConfiguration(CellGcmConfig*): local and IO addresses and sizes, clocks.
void cellGcmGetConfiguration(PPUContext& c, GuestMemory& m) {
    const GuestAddr p = a32(c, 3);
    m.write<uint32_t>(p, kLocalBase);
    m.write<uint32_t>(p + 4, g.io_address);
    m.write<uint32_t>(p + 8, kLocalSize);
    m.write<uint32_t>(p + 12, g.io_size);
    m.write<uint32_t>(p + 16, 650000000);  // memory frequency
    m.write<uint32_t>(p + 20, 500000000);  // core frequency
}

void cellGcmGetControlRegister(PPUContext& c, GuestMemory&) { c.gpr[3] = g.control; }
void cellGcmGetLabelAddress(PPUContext& c, GuestMemory&) { c.gpr[3] = g.labels + 0x10 * (a32(c, 3) & 0xFF); }

// cellGcmGetReportDataAddressLocation(index, location): 0 local, 1 main.
void cellGcmGetReportDataAddressLocation(PPUContext& c, GuestMemory&) {
    const uint32_t index = a32(c, 3), location = a32(c, 4);
    uint32_t ea = 0;
    if (location == 0)
        ea = kLocalBase + kReportsOffset + 0x10 * index;
    else
        io_to_address(kReportsOffset + 0x10 * index, ea);
    c.gpr[3] = ea;
}
void cellGcmGetTimeStamp(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = m.read<uint64_t>(kLocalBase + kReportsOffset + 0x10 * (a32(c, 3) & 0xFFFF));
}

// cellGcmAddressToOffset(address, u32* offset)
void cellGcmAddressToOffset(PPUContext& c, GuestMemory& m) {
    uint32_t offset;
    std::lock_guard<std::mutex> lk(g.lock);
    if (!address_to_offset(a32(c, 3), offset)) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    m.write<uint32_t>(a32(c, 4), offset);
    c.gpr[3] = CELL_OK;
}

// cellGcmMapMainMemory(ea, size, u32* offset): maps at the first free IO range.
void cellGcmMapMainMemory(PPUContext& c, GuestMemory& m) {
    const uint32_t ea = a32(c, 3), size = a32(c, 4);
    if ((ea & 0xFFFFF) || (size & 0xFFFFF) || !size) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    std::lock_guard<std::mutex> lk(rsx::io_lock());
    const uint32_t pages = size >> 20;
    for (uint32_t io = 0; io + pages <= 512; ++io) {  // 512 MiB of IO space
        if (!rsx::io_range_free(io, pages)) continue;
        rsx::map_io(ea >> 20, io, pages);
        m.write<uint32_t>(a32(c, 5), io << 20);
        c.gpr[3] = CELL_OK;
        return;
    }
    c.gpr[3] = CELL_GCM_ERROR_FAILURE;
}

// cellGcmMapEaIoAddress(ea, io, size)
void cellGcmMapEaIoAddress(PPUContext& c, GuestMemory&) {
    const uint32_t ea = a32(c, 3), io = a32(c, 4), size = a32(c, 5);
    if ((ea & 0xFFFFF) || (io & 0xFFFFF) || (size & 0xFFFFF) || !size) {
        c.gpr[3] = CELL_GCM_ERROR_INVALID_ALIGNMENT;
        return;
    }
    std::lock_guard<std::mutex> lk(rsx::io_lock());
    if ((io >> 20) + (size >> 20) > kPages || (ea >> 20) + (size >> 20) > kPages) {
        c.gpr[3] = CELL_GCM_ERROR_ADDRESS_OVERWRAP;
        return;
    }
    rsx::map_io(ea >> 20, io >> 20, size >> 20);
    c.gpr[3] = CELL_OK;
}

void cellGcmUnmapEaIoAddress(PPUContext& c, GuestMemory&) {
    const uint32_t ea = a32(c, 3) >> 20;
    std::lock_guard<std::mutex> lk(rsx::io_lock());
    if (!rsx::unmap_io(ea)) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    c.gpr[3] = CELL_OK;
}

// cellGcmGetTiledPitchSize(size): the smallest pitch the tiling hardware supports that
// holds `size` bytes (0 if none).
void cellGcmGetTiledPitchSize(PPUContext& c, GuestMemory&) {
    static const uint32_t pitches[] = {0x0000, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00,
                                       0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000,
                                       0x2800, 0x3000, 0x3400, 0x3800, 0x4000, 0x5000, 0x6000, 0x6800, 0x7000,
                                       0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
    const uint32_t size = a32(c, 3);
    c.gpr[3] = 0;
    for (size_t i = 0; i + 1 < std::size(pitches); ++i)
        if (pitches[i] < size && size <= pitches[i + 1]) {
            c.gpr[3] = pitches[i + 1];
            return;
        }
}

// cellGcmSetTileInfo(index, location, offset, size, pitch, comp, base, bank)
void cellGcmSetTileInfo(PPUContext& c, GuestMemory&) {
    const uint32_t i = a32(c, 3);
    if (i >= 15 || a32(c, 4) > 1 || (a32(c, 5) & 0xFFFF) || (a32(c, 6) & 0xFFFF)) {
        c.gpr[3] = CELL_GCM_ERROR_INVALID_VALUE;
        return;
    }
    std::lock_guard<std::mutex> lk(g.lock);
    g.tiles[i] = {a32(c, 4), a32(c, 5), a32(c, 6), a32(c, 7), a32(c, 8), a32(c, 9), a32(c, 10), g.tiles[i].bound};
    c.gpr[3] = CELL_OK;
}
template <bool Bind>
void cellGcmBindTile(PPUContext& c, GuestMemory&) {
    const uint32_t i = a32(c, 3);
    if (i >= 15) {
        c.gpr[3] = CELL_GCM_ERROR_INVALID_VALUE;
        return;
    }
    std::lock_guard<std::mutex> lk(g.lock);
    g.tiles[i].bound = Bind;
    c.gpr[3] = CELL_OK;
}
// cellGcmBindZcull(index, offset, width, height, cullStart, zFormat, aaFormat, ...)
template <bool Bind>
void cellGcmBindZcull(PPUContext& c, GuestMemory&) {
    const uint32_t i = a32(c, 3);
    if (i >= 8) {
        c.gpr[3] = CELL_GCM_ERROR_INVALID_VALUE;
        return;
    }
    std::lock_guard<std::mutex> lk(g.lock);
    g.zcull[i] = Bind;
    c.gpr[3] = CELL_OK;
}

// cellGcmSetDisplayBuffer(id, offset, pitch, width, height)
void cellGcmSetDisplayBuffer(PPUContext& c, GuestMemory&) {
    const uint32_t id = a32(c, 3);
    if (id >= 8) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    rsx::set_display_buffer(id, {a32(c, 4), a32(c, 5), a32(c, 6), a32(c, 7)});
    SLY4_INFO("cellGcm: buffer de tela %u em 0x%08X, %ux%u, pitch %u", id, a32(c, 4), a32(c, 6), a32(c, 7), a32(c, 5));
    c.gpr[3] = CELL_OK;
}

void cellGcmSetVBlankHandler(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g.lock);
    g.vblank_handler = a32(c, 3);
}
void cellGcmGetVBlankCount(PPUContext& c, GuestMemory&) { c.gpr[3] = uint32_t(g.vblank_count.load()); }

// The flip command in the FIFO: (count 1 << 18) | 0xFEAC, then the buffer id. Writes a
// callback first if the fragment has no room for it.
uint32_t put_flip(PPUContext& c, GuestMemory& m, GuestAddr ctx, uint32_t id) {
    if (m.read<uint32_t>(ctx + 8) + 8 >= m.read<uint32_t>(ctx + 4)) {
        const uint64_t r3 = c.gpr[3], r4 = c.gpr[4];
        c.gpr[3] = ctx;
        c.gpr[4] = 8;
        call_guest(c, m, m.read<uint32_t>(ctx + 12));
        c.gpr[3] = r3;
        c.gpr[4] = r4;
    }
    const uint32_t cur = m.read<uint32_t>(ctx + 8);
    m.write<uint32_t>(cur, (1u << 18) | kFlipMethod);
    m.write<uint32_t>(cur + 4, id);
    m.write<uint32_t>(ctx + 8, cur + 8);
    return id;
}
// _cellGcmSetFlipCommand(context, id)
void _cellGcmSetFlipCommand(PPUContext& c, GuestMemory& m) {
    const uint32_t id = a32(c, 4);
    if (id > 7) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    put_flip(c, m, a32(c, 3), id);
    c.gpr[3] = CELL_OK;
}
// _cellGcmSetFlipCommandWithWaitLabel(context, id, label index, label value): the label
// is written right away, as RPCS3 does.
void _cellGcmSetFlipCommandWithWaitLabel(PPUContext& c, GuestMemory& m) {
    const uint32_t id = a32(c, 4), index = a32(c, 5), value = a32(c, 6);
    if (id > 7) {
        c.gpr[3] = CELL_GCM_ERROR_FAILURE;
        return;
    }
    put_flip(c, m, a32(c, 3), id);
    m.write<uint32_t>(g.labels + 0x10 * (index & 0xFF), value);
    c.gpr[3] = CELL_OK;
}

void cellGcmGetDefaultCommandWordSize(PPUContext& c, GuestMemory&) { c.gpr[3] = 0x400; }
void cellGcmGetDefaultSegmentWordSize(PPUContext& c, GuestMemory&) { c.gpr[3] = 0x100; }
void ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

}  // namespace

HleModule hle_cell_gcm_sys() {
    return {"cellGcmSys",
            {
                {"_cellGcmInitBody", _cellGcmInitBody},
                {"_cellGcmFunc15", ok},
                {"_cellGcmSetFlipCommand", _cellGcmSetFlipCommand},
                {"_cellGcmSetFlipCommandWithWaitLabel", _cellGcmSetFlipCommandWithWaitLabel},
                {"cellGcmInitDefaultFifoMode", ok},
                {"cellGcmSetDefaultFifoSize", ok},
                {"cellGcmGetDefaultCommandWordSize", cellGcmGetDefaultCommandWordSize},
                {"cellGcmGetDefaultSegmentWordSize", cellGcmGetDefaultSegmentWordSize},
                {"cellGcmGetConfiguration", cellGcmGetConfiguration},
                {"cellGcmGetControlRegister", cellGcmGetControlRegister},
                {"cellGcmGetLabelAddress", cellGcmGetLabelAddress},
                {"cellGcmGetReportDataAddressLocation", cellGcmGetReportDataAddressLocation},
                {"cellGcmGetTimeStamp", cellGcmGetTimeStamp},
                {"cellGcmAddressToOffset", cellGcmAddressToOffset},
                {"cellGcmMapMainMemory", cellGcmMapMainMemory},
                {"cellGcmMapEaIoAddress", cellGcmMapEaIoAddress},
                {"cellGcmUnmapEaIoAddress", cellGcmUnmapEaIoAddress},
                {"cellGcmGetTiledPitchSize", cellGcmGetTiledPitchSize},
                {"cellGcmSetTileInfo", cellGcmSetTileInfo},
                {"cellGcmBindTile", cellGcmBindTile<true>},
                {"cellGcmUnbindTile", cellGcmBindTile<false>},
                {"cellGcmBindZcull", cellGcmBindZcull<true>},
                {"cellGcmUnbindZcull", cellGcmBindZcull<false>},
                {"cellGcmSetDisplayBuffer", cellGcmSetDisplayBuffer},
                {"cellGcmSetFlipMode", ok},
                {"cellGcmResetFlipStatus", ok},
                {"cellGcmSetVBlankFrequency", ok},
                {"cellGcmSetVBlankHandler", cellGcmSetVBlankHandler},
                {"cellGcmGetVBlankCount", cellGcmGetVBlankCount},
            }};
}

// For the summary.
void gcm_summary() {
    if (!g.initialized) return;
    rsx::summary();
    SLY4_INFO("cellGcm: %" PRIu64 " VBlanks", g.vblank_count.load());
}

}  // namespace sly4
