// System utilities: cellSysutil (callbacks, system parameters, video/audio output,
// cache, dialogs), cellSysutilAvconfExt, cellSysmodule, cellRtc, and the network/NP
// libraries in their "offline" state.
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }
void ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

// ---------------------------------------------------------------- cellSysutil
constexpr uint32_t CELL_SYSUTIL_ERROR_VALUE = 0x8002B102;

struct Callback {
    uint32_t func = 0, userdata = 0;
};
std::mutex g_cb_lock;
Callback g_callbacks[4];

void cellSysutilRegisterCallback(PPUContext& c, GuestMemory&) {
    const uint32_t slot = a32(c, 3);
    if (slot >= 4) {
        c.gpr[3] = CELL_SYSUTIL_ERROR_VALUE;
        return;
    }
    std::lock_guard<std::mutex> g(g_cb_lock);
    g_callbacks[slot] = {a32(c, 4), a32(c, 5)};
    c.gpr[3] = CELL_OK;
}
void cellSysutilUnregisterCallback(PPUContext& c, GuestMemory&) {
    const uint32_t slot = a32(c, 3);
    if (slot >= 4) {
        c.gpr[3] = CELL_SYSUTIL_ERROR_VALUE;
        return;
    }
    std::lock_guard<std::mutex> g(g_cb_lock);
    g_callbacks[slot] = {};
    c.gpr[3] = CELL_OK;
}
// No system events yet (XMB, exit request...): nothing to deliver.
void cellSysutilCheckCallback(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

void cellSysutilGetSystemParamInt(PPUContext& c, GuestMemory& m) {
    int32_t v;
    switch (a32(c, 3)) {
        case 0x111: v = 1; break;    // LANG: English (US)
        case 0x112: v = 1; break;    // ENTER_BUTTON_ASSIGN: cross
        case 0x114: v = 1; break;    // DATE_FORMAT: DD/MM/YYYY
        case 0x115: v = 1; break;    // TIME_FORMAT: 24 h
        case 0x116: v = 180; break;  // TIMEZONE (minutes), as RPCS3
        case 0x117: v = 0; break;    // SUMMERTIME
        case 0x121: v = 0; break;    // GAME_PARENTAL_LEVEL: off
        case 0x123: v = 0; break;    // GAME_PARENTAL_LEVEL0_RESTRICT
        case 0x141: v = 0; break;    // CURRENT_USER_HAS_NP_ACCOUNT
        case 0x151: v = 0; break;    // CAMERA_PLFREQ
        case 0x152: v = 1; break;    // PAD_RUMBLE: on
        case 0x153: v = 0; break;    // KEYBOARD_TYPE
        case 0x154: v = 0; break;    // JAPANESE_KEYBOARD_ENTRY_METHOD
        case 0x155: v = 0; break;    // CHINESE_KEYBOARD_ENTRY_METHOD
        case 0x156: v = 0; break;    // PAD_AUTOOFF
        case 0x157: v = 0; break;    // MAGNETOMETER
        default: c.gpr[3] = CELL_SYSUTIL_ERROR_VALUE; return;
    }
    m.write<int32_t>(a32(c, 4), v);
    c.gpr[3] = CELL_OK;
}

// Video output: one 1280x720 progressive 16:9 display at 59.94 Hz.
constexpr uint8_t kRes720 = 2;
void cellVideoOutGetState(PPUContext& c, GuestMemory& m) {
    const uint32_t out = a32(c, 3), state = a32(c, 5);
    if (out != 0) {  // only the primary output exists
        m.write<uint8_t>(state, 2);  // CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED
        c.gpr[3] = CELL_OK;
        return;
    }
    const uint8_t s[16] = {0, 1, 0, 0, 0, 0, 0, 0, kRes720, 1, 0, 2, 0, 0, 0, 1};  // state, RGB, mode, 59.94 Hz
    m.copy_to(state, s, sizeof(s));
    c.gpr[3] = CELL_OK;
}
bool resolution_size(uint32_t id, uint16_t& w, uint16_t& h) {
    switch (id) {
        case 1: w = 1920; h = 1080; return true;
        case 2: w = 1280; h = 720; return true;
        case 4: w = 720; h = 480; return true;
        case 5: w = 720; h = 576; return true;
        default: return false;
    }
}
void cellVideoOutGetResolution(PPUContext& c, GuestMemory& m) {
    uint16_t w, h;
    if (!resolution_size(a32(c, 3), w, h)) {
        c.gpr[3] = 0x8002B222;  // CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER
        return;
    }
    m.write<uint16_t>(a32(c, 4), w);
    m.write<uint16_t>(a32(c, 4) + 2, h);
    c.gpr[3] = CELL_OK;
}
void cellVideoOutGetResolutionAvailability(PPUContext& c, GuestMemory&) {
    uint16_t w, h;
    c.gpr[3] = a32(c, 3) == 0 && resolution_size(a32(c, 4), w, h) ? 1 : 0;
}
void cellVideoOutConfigure(PPUContext& c, GuestMemory& m) {
    SLY4_INFO("cellVideoOutConfigure: resolução %u, formato %u, pitch %u", m.read<uint8_t>(a32(c, 4)),
              m.read<uint8_t>(a32(c, 4) + 1), m.read<uint32_t>(a32(c, 4) + 12));
    c.gpr[3] = CELL_OK;
}
// cellVideoOutGetScreenSize(videoOut, float* inches): an ordinary TV.
void cellVideoOutGetScreenSize(PPUContext& c, GuestMemory& m) {
    m.write<float>(a32(c, 4), 40.0f);
    c.gpr[3] = CELL_OK;
}
// Audio output: stereo LPCM only (returns the number of channels).
void cellAudioOutGetSoundAvailability(PPUContext& c, GuestMemory&) {
    c.gpr[3] = a32(c, 3) == 0 && a32(c, 4) == 0 ? 2 : 0;  // primary output, CELL_AUDIO_OUT_CODING_TYPE_LPCM
}

// cellSysCacheMount(CellSysCacheParam*): cacheId[32] @0, getCachePath[1024] @32. As in
// RPCS3, /dev_hdd1 becomes hdd1/caches/<title id>_<cache id>/, emptied at each mount, and
// the call returns CELL_SYSCACHE_RET_OK_CLEARED (0).
void cellSysCacheMount(PPUContext& c, GuestMemory& m) {
    const uint32_t p = a32(c, 3);
    const std::string id = m.read_cstring(p, 32);
    const std::filesystem::path dir = vfs().data_dir() / "hdd1" / "caches" / path_from_utf8(param_sfo_string("TITLE_ID") + "_" + id);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    vfs().mount("/dev_hdd1", dir);
    const char path[] = "/dev_hdd1";
    m.copy_to(p + 32, path, sizeof(path));
    SLY4_INFO("cellSysCacheMount(\"%s\") -> %s = %s", id.c_str(), path, path_to_utf8(dir).c_str());
    c.gpr[3] = 0;
}

// Dialogs: logged; the game gets no button press yet.
void cellMsgDialogOpen2(PPUContext& c, GuestMemory& m) {
    SLY4_WARN("cellMsgDialogOpen2(tipo 0x%X): \"%s\"", a32(c, 3), m.read_cstring(a32(c, 4), 512).c_str());
    c.gpr[3] = CELL_OK;
}
void cellMsgDialogOpenErrorCode(PPUContext& c, GuestMemory&) {
    SLY4_WARN("cellMsgDialogOpenErrorCode(0x%08X)", a32(c, 3));
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- cellSysmodule
constexpr uint32_t CELL_SYSMODULE_ERROR_UNLOADED = 0x80012003;
std::mutex g_mod_lock;
std::set<uint32_t> g_modules;

void cellSysmoduleLoadModule(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> g(g_mod_lock);
    g_modules.insert(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
void cellSysmoduleUnloadModule(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> g(g_mod_lock);
    c.gpr[3] = g_modules.erase(a32(c, 3)) ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
void cellSysmoduleIsLoaded(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> g(g_mod_lock);
    c.gpr[3] = g_modules.count(a32(c, 3)) ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}

// ---------------------------------------------------------------- cellRtc
// CellRtcDateTime: year, month, day, hour, minute, second (u16), microsecond (u32).
void cellRtcGetCurrentClockLocalTime(PPUContext& c, GuestMemory& m) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    const uint32_t p = a32(c, 3);
    m.write<uint16_t>(p, uint16_t(tm.tm_year + 1900));
    m.write<uint16_t>(p + 2, uint16_t(tm.tm_mon + 1));
    m.write<uint16_t>(p + 4, uint16_t(tm.tm_mday));
    m.write<uint16_t>(p + 6, uint16_t(tm.tm_hour));
    m.write<uint16_t>(p + 8, uint16_t(tm.tm_min));
    m.write<uint16_t>(p + 10, uint16_t(tm.tm_sec));
    m.write<uint32_t>(p + 12, uint32_t(duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000));
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- network, offline
constexpr uint32_t CELL_NET_CTL_ERROR_NOT_CONNECTED = 0x80130106;
void cellNetCtlGetState(PPUContext& c, GuestMemory& m) {
    m.write<int32_t>(a32(c, 3), 0);  // CELL_NET_CTL_STATE_Disconnected
    c.gpr[3] = CELL_OK;
}
void cellNetCtlGetInfo(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_NET_CTL_ERROR_NOT_CONNECTED; }
// _sys_net_errno_loc(): address of this process's sys_net errno.
void sys_net_errno_loc(PPUContext& c, GuestMemory&) {
    static GuestAddr errno_addr = kernel().main_memory().alloc(4, 4);
    c.gpr[3] = errno_addr;
}

}  // namespace

HleModule hle_cell_sysutil() {
    return {"cellSysutil",
            {
                {"cellSysutilRegisterCallback", cellSysutilRegisterCallback},
                {"cellSysutilUnregisterCallback", cellSysutilUnregisterCallback},
                {"cellSysutilCheckCallback", cellSysutilCheckCallback},
                {"cellSysutilGetSystemParamInt", cellSysutilGetSystemParamInt},
                {"cellVideoOutGetState", cellVideoOutGetState},
                {"cellVideoOutGetResolution", cellVideoOutGetResolution},
                {"cellVideoOutGetResolutionAvailability", cellVideoOutGetResolutionAvailability},
                {"cellVideoOutConfigure", cellVideoOutConfigure},
                {"cellAudioOutGetSoundAvailability", cellAudioOutGetSoundAvailability},
                {"cellAudioOutConfigure", ok},
                {"cellSysCacheMount", cellSysCacheMount},
                {"cellMsgDialogOpen2", cellMsgDialogOpen2},
                {"cellMsgDialogOpenErrorCode", cellMsgDialogOpenErrorCode},
                {"cellMsgDialogClose", ok},
                {"cellMsgDialogAbort", ok},
            }};
}

HleModule hle_cell_sysutil_avconf_ext() {
    return {"cellSysutilAvconfExt",
            {
                {"cellVideoOutGetScreenSize", cellVideoOutGetScreenSize},
                {"cellVideoOutSetGamma", ok},
            }};
}

HleModule hle_cell_sysmodule() {
    return {"cellSysmodule",
            {
                {"cellSysmoduleInitialize", ok},
                {"cellSysmoduleLoadModule", cellSysmoduleLoadModule},
                {"cellSysmoduleUnloadModule", cellSysmoduleUnloadModule},
                {"cellSysmoduleIsLoaded", cellSysmoduleIsLoaded},
            }};
}

HleModule hle_cell_rtc() { return {"cellRtc", {{"cellRtcGetCurrentClockLocalTime", cellRtcGetCurrentClockLocalTime}}}; }

HleModule hle_cell_net_ctl() {
    return {"cellNetCtl",
            {
                {"cellNetCtlInit", ok},
                {"cellNetCtlTerm", ok},
                {"cellNetCtlGetState", cellNetCtlGetState},
                {"cellNetCtlGetInfo", cellNetCtlGetInfo},
            }};
}

HleModule hle_sys_net() {
    return {"sys_net",
            {
                {"sys_net_initialize_network_ex", ok},
                {"sys_net_finalize_network", ok},
                {"_sys_net_errno_loc", sys_net_errno_loc},
            }};
}

}  // namespace sly4
