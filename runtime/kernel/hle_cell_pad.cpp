// cellPad (sys_io): one DualShock 3 connected on port 0. Its state comes from
// pad_state(), which the window's input will fill (phase 6); until then the controller
// is idle (sticks centred, nothing pressed).
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

constexpr uint32_t CELL_PAD_ERROR_ALREADY_INITIALIZED = 0x80121102, CELL_PAD_ERROR_UNINITIALIZED = 0x80121103,
                   CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121104, CELL_PAD_ERROR_NO_DEVICE = 0x80121107;
constexpr uint32_t kPorts = 7;
constexpr uint32_t kStatusConnected = 1, kStatusAssignChanges = 2;
// PS3 conformity, pressure-sensitive buttons, motion sensors, analog sticks, vibration.
constexpr uint32_t kCapabilities = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;

struct Pads {
    std::mutex lock;
    bool initialized = false;
    uint32_t max_connect = 0;
    uint32_t setting[kPorts] = {};
    bool reported = false;  // the connection was announced (ASSIGN_CHANGES once)
    PadState state;
};
Pads g;

void cellPadInit(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (g.initialized) {
        c.gpr[3] = CELL_PAD_ERROR_ALREADY_INITIALIZED;
        return;
    }
    g.initialized = true;
    g.max_connect = a32(c, 3);
    g.reported = false;
    c.gpr[3] = CELL_OK;
}
void cellPadEnd(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g.lock);
    c.gpr[3] = g.initialized ? CELL_OK : CELL_PAD_ERROR_UNINITIALIZED;
    g.initialized = false;
}

// cellPadGetInfo2(CellPadInfo2*): {u32 max_connect, now_connect, system_info,
// port_status[7], port_setting[7], device_capability[7], device_type[7]}.
void cellPadGetInfo2(PPUContext& c, GuestMemory& m) {
    const GuestAddr p = a32(c, 3);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_PAD_ERROR_UNINITIALIZED;
        return;
    }
    std::memset(m.host(p), 0, 12 + 4 * 4 * kPorts);
    m.write<uint32_t>(p, g.max_connect);
    m.write<uint32_t>(p + 4, 1);
    m.write<uint32_t>(p + 12, kStatusConnected | (g.reported ? 0 : kStatusAssignChanges));
    m.write<uint32_t>(p + 12 + 4 * kPorts, g.setting[0]);
    m.write<uint32_t>(p + 12 + 8 * kPorts, kCapabilities);
    m.write<uint32_t>(p + 12 + 12 * kPorts, 0);  // standard controller
    g.reported = true;
    c.gpr[3] = CELL_OK;
}

// cellPadGetData(port, CellPadData*): {s32 len, u16 button[64]}; 24 halfwords of data:
// [2], [3] digital buttons, [4]-[7] right x/y, left x/y, [8]-[19] pressure, [20]-[23]
// motion sensors.
void cellPadGetData(PPUContext& c, GuestMemory& m) {
    const uint32_t port = a32(c, 3);
    const GuestAddr d = a32(c, 4);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_PAD_ERROR_UNINITIALIZED;
        return;
    }
    if (port >= kPorts || !d) {
        c.gpr[3] = CELL_PAD_ERROR_INVALID_PARAMETER;
        return;
    }
    if (port != 0) {
        c.gpr[3] = CELL_PAD_ERROR_NO_DEVICE;
        return;
    }
    const PadState& s = g.state;
    std::memset(m.host(d), 0, 4 + 2 * 64);
    m.write<int32_t>(d, 24);
    auto b = [&](int i, uint16_t v) { m.write<uint16_t>(d + 4 + 2 * i, v); };
    b(2, s.digital1);
    b(3, s.digital2);
    b(4, s.right_x);
    b(5, s.right_y);
    b(6, s.left_x);
    b(7, s.left_y);
    for (int i = 0; i < 12; ++i) b(8 + i, s.pressure[i]);
    b(20, 512);  // sensors: x, y, z at rest (z feels gravity), gyro
    b(21, 399);
    b(22, 512);
    b(23, 512);
    c.gpr[3] = CELL_OK;
}

void cellPadSetPortSetting(PPUContext& c, GuestMemory&) {
    const uint32_t port = a32(c, 3);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_PAD_ERROR_UNINITIALIZED;
        return;
    }
    if (port >= kPorts) {
        c.gpr[3] = CELL_PAD_ERROR_INVALID_PARAMETER;
        return;
    }
    g.setting[port] = a32(c, 4);
    c.gpr[3] = CELL_OK;
}

// Vibration: nothing to drive yet.
void cellPadSetActDirect(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g.lock);
    c.gpr[3] = g.initialized ? CELL_OK : CELL_PAD_ERROR_UNINITIALIZED;
}

}  // namespace

void set_pad_state(const PadState& s) {
    std::lock_guard<std::mutex> lk(g.lock);
    g.state = s;
}

HleModule hle_sys_io() {
    return {"sys_io",
            {
                {"cellPadInit", cellPadInit},
                {"cellPadEnd", cellPadEnd},
                {"cellPadGetInfo2", cellPadGetInfo2},
                {"cellPadGetData", cellPadGetData},
                {"cellPadSetPortSetting", cellPadSetPortSetting},
                {"cellPadSetActDirect", cellPadSetActDirect},
            }};
}

}  // namespace sly4
