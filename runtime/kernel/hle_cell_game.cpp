// cellGame: boot check, content permission and PARAM.SFO values, for a disc game
// (what RPCS3's cellGame.cpp does for category "DG").
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"

namespace sly4 {

namespace {

constexpr uint32_t kGameTypeDisc = 1;
constexpr int32_t kSizeKbNotCalc = -1;
constexpr uint32_t CELL_GAME_RET_NONE = 2;
constexpr uint32_t CELL_GAME_ERROR_PARAM = 0x8002CB07, CELL_GAME_ERROR_NOTSUPPORTED = 0x8002CB21,
                   CELL_GAME_ERROR_INVALID_ID = 0x8002CB25, CELL_GAME_ERROR_NOTPATCH = 0x8002CB27;

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

// PARAM.SFO: little-endian header and index; values are UTF-8 strings or 32-bit integers.
struct Sfo {
    std::map<std::string, std::string> strings;
    std::map<std::string, int32_t> ints;
};

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size() ? b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24 : 0;
}

const Sfo& param_sfo() {
    static Sfo sfo;
    static std::once_flag once;
    std::call_once(once, [] {
        std::filesystem::path host;
        if (!vfs().resolve("/dev_bdvd/PS3_GAME/PARAM.SFO", host)) return;
        std::ifstream f(host, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (b.size() < 20 || std::memcmp(b.data(), "\0PSF", 4) != 0) {
            SLY4_WARN("PARAM.SFO não encontrado ou inválido em %s", path_to_utf8(host).c_str());
            return;
        }
        const uint32_t keys = le32(b, 8), data = le32(b, 12), count = le32(b, 16);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = 20 + 16 * size_t(i);
            if (e + 16 > b.size()) break;
            const uint32_t key_off = b[e] | b[e + 1] << 8, fmt = b[e + 2] | b[e + 3] << 8;
            const uint32_t len = le32(b, e + 4), off = le32(b, e + 12);
            if (keys + key_off >= b.size() || data + off + len > b.size()) continue;
            const std::string key(reinterpret_cast<const char*>(&b[keys + key_off]));
            if (fmt == 0x0404)
                sfo.ints[key] = int32_t(le32(b, data + off));
            else
                sfo.strings[key] = std::string(reinterpret_cast<const char*>(&b[data + off]),
                                               strnlen(reinterpret_cast<const char*>(&b[data + off]), len));
        }
        SLY4_INFO("PARAM.SFO: %s (%s), versão %s, categoria %s", sfo.strings["TITLE"].c_str(),
                  sfo.strings["TITLE_ID"].c_str(), sfo.strings["APP_VER"].c_str(), sfo.strings["CATEGORY"].c_str());
    });
    return sfo;
}

void write_string(GuestMemory& m, GuestAddr at, uint32_t capacity, const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), capacity - 1);
    m.copy_to(at, s.data(), n);
    m.write<uint8_t>(at + uint32_t(n), 0);
}

// CellGameContentSize: hddFreeSizeKB, sizeKB, sysSizeKB (s32 each).
void write_size(GuestMemory& m, GuestAddr size, int32_t sys_kb) {
    if (!size) return;
    m.write<int32_t>(size, 40 * 1024 * 1024 - 256);
    m.write<int32_t>(size + 4, kSizeKbNotCalc);
    m.write<int32_t>(size + 8, sys_kb);
}

// cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size, char dirName[32])
void cellGameBootCheck(PPUContext& c, GuestMemory& m) {
    if (!a32(c, 3) || !a32(c, 4)) {
        c.gpr[3] = CELL_GAME_ERROR_PARAM;
        return;
    }
    param_sfo();
    m.write<uint32_t>(a32(c, 3), kGameTypeDisc);
    m.write<uint32_t>(a32(c, 4), 0);
    write_size(m, a32(c, 5), 4);
    c.gpr[3] = CELL_OK;
}

void cellGamePatchCheck(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_GAME_ERROR_NOTPATCH; }

// cellGameDataCheck(type, dirName, size): the disc's own data, or game data on the HDD.
std::string g_permit_dir = "/dev_bdvd/PS3_GAME";
void cellGameDataCheck(PPUContext& c, GuestMemory& m) {
    const uint32_t type = a32(c, 3);
    if (type == kGameTypeDisc) {
        g_permit_dir = "/dev_bdvd/PS3_GAME";
        write_size(m, a32(c, 5), 0);
        c.gpr[3] = CELL_OK;
        return;
    }
    if (!a32(c, 4)) {
        c.gpr[3] = CELL_GAME_ERROR_PARAM;
        return;
    }
    const std::string dir = m.read_cstring(a32(c, 4), 32);
    g_permit_dir = "/dev_hdd0/game/" + dir;
    FsStat st;
    write_size(m, a32(c, 5), 0);
    c.gpr[3] = fs_stat(g_permit_dir, st) == CELL_OK ? CELL_OK : CELL_GAME_RET_NONE;
}

// cellGameContentPermit(char contentInfoPath[128], char usrdirPath[128])
void cellGameContentPermit(PPUContext& c, GuestMemory& m) {
    if (a32(c, 3)) write_string(m, a32(c, 3), 128, g_permit_dir);
    if (a32(c, 4)) write_string(m, a32(c, 4), 128, g_permit_dir + "/USRDIR");
    c.gpr[3] = CELL_OK;
}

void cellGameGetParamInt(PPUContext& c, GuestMemory& m) {
    const char* key = nullptr;
    switch (a32(c, 3)) {
        case 102: key = "PARENTAL_LEVEL"; break;
        case 103: key = "RESOLUTION"; break;
        case 104: key = "SOUND_FORMAT"; break;
        default: c.gpr[3] = CELL_GAME_ERROR_INVALID_ID; return;
    }
    const auto& ints = param_sfo().ints;
    auto it = ints.find(key);
    m.write<int32_t>(a32(c, 4), it == ints.end() ? 0 : it->second);
    c.gpr[3] = CELL_OK;
}

// cellGameGetParamString(id, char* buf, u32 bufsize): a disc game may read the titles,
// the title id and the application version.
void cellGameGetParamString(PPUContext& c, GuestMemory& m) {
    const uint32_t id = a32(c, 3), buf = a32(c, 4), size = a32(c, 5);
    if (!buf || !size) {
        c.gpr[3] = CELL_GAME_ERROR_PARAM;
        return;
    }
    const char* key = nullptr;
    if (id == 0 || id == 1)
        key = "TITLE";
    else if (id == 100)
        key = "TITLE_ID";
    else if (id == 105)
        key = "APP_VER";
    else if (id < 100 || id == 101) {
        c.gpr[3] = CELL_GAME_ERROR_NOTSUPPORTED;
        return;
    } else {
        c.gpr[3] = CELL_GAME_ERROR_INVALID_ID;
        return;
    }
    const auto& strings = param_sfo().strings;
    auto it = strings.find(key);
    write_string(m, buf, size, it == strings.end() ? "" : it->second);
    c.gpr[3] = CELL_OK;
}

void cellGameContentErrorDialog(PPUContext& c, GuestMemory&) {
    SLY4_WARN("cellGameContentErrorDialog(tipo %u, faltam %u KB)", a32(c, 3), a32(c, 4));
    c.gpr[3] = CELL_OK;
}

}  // namespace

std::string param_sfo_string(const char* key) {
    const auto& s = param_sfo().strings;
    auto it = s.find(key);
    return it == s.end() ? "" : it->second;
}

HleModule hle_cell_game() {
    return {"cellGame",
            {
                {"cellGameBootCheck", cellGameBootCheck},
                {"cellGamePatchCheck", cellGamePatchCheck},
                {"cellGameDataCheck", cellGameDataCheck},
                {"cellGameContentPermit", cellGameContentPermit},
                {"cellGameGetParamInt", cellGameGetParamInt},
                {"cellGameGetParamString", cellGameGetParamString},
                {"cellGameContentErrorDialog", cellGameContentErrorDialog},
            }};
}

}  // namespace sly4
