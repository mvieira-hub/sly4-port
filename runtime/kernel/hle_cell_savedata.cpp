// cellSaveData (in cellSysutil): saves in /dev_hdd0/home/00000001/savedata/<dir>, each with
// a PARAM.SFO, driven by the game's callbacks as the system software does: a list or fixed
// callback picks the directory, the stat callback sees what is there and says what to
// write in PARAM.SFO, the file callback reads, writes and deletes files one at a time.
// Follows RPCS3's savedata_op (cellSaveData.cpp), without its dialogs: a list is answered
// with the game's own choice (focus, new data or the first fixed entry), confirmations
// with yes.
//
// Until this existed the calls returned CELL_OK without calling anyone, and the game took
// the untouched settings buffer as its saved settings: no language selection on the first
// run, and the music and sound volumes at zero.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"

namespace sly4 {

namespace {

namespace fsys = std::filesystem;

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

constexpr uint32_t CELL_SAVEDATA_ERROR_CBRESULT = 0x8002B401, CELL_SAVEDATA_ERROR_ACCESS_ERROR = 0x8002B402,
                   CELL_SAVEDATA_ERROR_PARAM = 0x8002B404, CELL_SAVEDATA_ERROR_BROKEN = 0x8002B406,
                   CELL_SAVEDATA_ERROR_FAILURE = 0x8002B407, CELL_SAVEDATA_ERROR_BUSY = 0x8002B408;
constexpr uint32_t CELL_CANCEL = 1;
constexpr int32_t CBRESULT_OK_LAST_NOCONFIRM = 2, CBRESULT_OK_LAST = 1, CBRESULT_OK_NEXT = 0,
                  CBRESULT_ERR_INVALID = -5;
constexpr uint32_t FOCUSPOS_DIRNAME = 0, FOCUSPOS_LISTHEAD = 1, FOCUSPOS_LISTTAIL = 2, FOCUSPOS_LATEST = 3,
                   FOCUSPOS_OLDEST = 4, FOCUSPOS_NEWDATA = 5;
constexpr uint32_t FILEOP_READ = 0, FILEOP_WRITE = 1, FILEOP_DELETE = 2, FILEOP_WRITE_NOTRUNC = 3;
constexpr uint32_t FILETYPE_SECUREFILE = 0, FILETYPE_NORMALFILE = 1, FILETYPE_ICON0 = 2, FILETYPE_ICON1 = 3,
                   FILETYPE_PIC1 = 4, FILETYPE_SND0 = 5;
constexpr uint32_t RECREATE_MASK = 0xFFFEFFFF, RECREATE_NO = 0, RECREATE_NO_NOBROKEN = 1, RECREATE_YES = 2,
                   RECREATE_YES_RESET_OWNER = 3;

enum class Op { AutoLoad, AutoSave, ListLoad, ListSave, FixedLoad, FixedSave };

// ---------------------------------------------------------------- guest structures
// Offsets in the structures the callbacks get (cellSaveData.h of the SDK, big-endian).
namespace off {
// CellSaveDataSetBuf
constexpr uint32_t buf_dir_list_max = 0, buf_file_list_max = 4, buf_size = 32, buf_ptr = 36;
// CellSaveDataSetList
constexpr uint32_t list_sort_type = 0, list_sort_order = 4, list_prefix = 8;
// CellSaveDataCBResult
constexpr uint32_t res_result = 0, res_userdata = 16, res_size = 20;
// CellSaveDataListGet
constexpr uint32_t lget_dir_num = 0, lget_dir_list_num = 4, lget_dir_list = 8, lget_size = 76;
// CellSaveDataListSet
constexpr uint32_t lset_focus_pos = 0, lset_focus_dir = 4, lset_fixed_num = 8, lset_fixed_list = 12,
                   lset_new_data = 16, lset_size = 24;
// CellSaveDataFixedSet
constexpr uint32_t fset_dir = 0, fset_size = 12;
// CellSaveDataDirList (an entry of a list)
constexpr uint32_t dirlist_name = 0, dirlist_param = 32, dirlist_size = 48;
// CellSaveDataStatGet
constexpr uint32_t sget_free_kb = 0, sget_is_new = 4, sget_atime = 8, sget_mtime = 16, sget_ctime = 24,
                   sget_dir_name = 32, sget_param = 64, sget_bind = 1616, sget_size_kb = 1620, sget_sys_size_kb = 1624,
                   sget_file_num = 1628, sget_file_list_num = 1632, sget_file_list = 1636, sget_size = 1704;
// CellSaveDataSystemFileParam
constexpr uint32_t sfp_title = 0, sfp_subtitle = 128, sfp_detail = 256, sfp_attribute = 1280, sfp_parental = 1284,
                   sfp_list_param = 1288, sfp_reserved = 1296, sfp_size = 1552;
// CellSaveDataStatSet
constexpr uint32_t sset_param = 0, sset_recreate = 4, sset_size = 12;
// CellSaveDataFileStat
constexpr uint32_t fstat_type = 0, fstat_size = 8, fstat_atime = 16, fstat_mtime = 24, fstat_ctime = 32,
                   fstat_name = 40, fstat_bytes = 56;
// CellSaveDataFileGet
constexpr uint32_t fget_exc_size = 0, fget_size = 68;
// CellSaveDataFileSet
constexpr uint32_t file_op = 0, file_type = 8, file_name = 28, file_offset = 32, file_size = 36, file_buf_size = 40,
                   file_buf = 44, file_set_size = 48;
// CellSaveDataListNewData
constexpr uint32_t new_dir = 4;
}  // namespace off

std::string guest_string(GuestMemory& m, GuestAddr a, size_t max) {
    if (!a) return {};
    const char* p = reinterpret_cast<const char*>(m.host(a));
    return std::string(p, strnlen(p, max));
}
void put_string(GuestMemory& m, GuestAddr a, const std::string& s, size_t size) {
    std::memset(m.host(a), 0, size);
    std::memcpy(m.host(a), s.data(), std::min(s.size(), size - 1));
}

// Directory names: 1-31 of A-Z, 0-9, '-' and '_'.
bool valid_dir_name(const std::string& s) {
    if (s.empty() || s.size() >= 32) return false;
    return std::all_of(s.begin(), s.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

// ---------------------------------------------------------------- PARAM.SFO
// Little-endian header, an index of {key offset u16, format u16, length u32, maximum u32,
// data offset u32}, the keys and the data. Formats: 0x0004 bytes, 0x0204 UTF-8 string with
// its terminator, 0x0404 32-bit integer.
struct SfoValue {
    uint16_t format = 0x0204;
    uint32_t max = 0;
    std::string bytes;  // strings and arrays
    uint32_t integer = 0;
};
using Sfo = std::map<std::string, SfoValue>;

uint32_t le32(const std::string& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
uint16_t le16(const std::string& b, size_t at) {
    uint16_t v;
    std::memcpy(&v, b.data() + at, 2);
    return v;
}

Sfo load_sfo(const fsys::path& p) {
    Sfo sfo;
    std::ifstream f(p, std::ios::binary);
    if (!f) return sfo;
    const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 20 || le32(b, 0) != 0x46535000) return sfo;
    const uint32_t keys = le32(b, 8), data = le32(b, 12), count = le32(b, 16);
    for (uint32_t i = 0; i < count && 20 + 16 * (i + 1) <= b.size(); ++i) {
        const size_t e = 20 + 16 * i;
        const size_t key_at = keys + le16(b, e);
        const uint16_t fmt = le16(b, e + 2);
        const uint32_t len = le32(b, e + 4), max = le32(b, e + 8), at = data + le32(b, e + 12);
        if (key_at >= b.size() || at + len > b.size()) break;
        SfoValue v;
        v.format = fmt;
        v.max = max;
        if (fmt == 0x0404)
            v.integer = le32(b, at);
        else
            v.bytes = b.substr(at, fmt == 0x0204 && len ? len - 1 : len);
        sfo[std::string(b.c_str() + key_at)] = v;
    }
    return sfo;
}

std::string save_sfo(const Sfo& sfo) {
    std::string keys, data, index;
    for (const auto& [k, v] : sfo) {  // std::map: keys in order, as the format wants
        const uint32_t len = v.format == 0x0404 ? 4 : uint32_t(v.bytes.size()) + (v.format == 0x0204 ? 1 : 0);
        const uint32_t max = v.format == 0x0404 ? 4 : std::max(v.max, (len + 3) & ~3u);
        auto put16 = [&](uint16_t x) { index.append(reinterpret_cast<const char*>(&x), 2); };
        auto put32 = [&](uint32_t x) { index.append(reinterpret_cast<const char*>(&x), 4); };
        put16(uint16_t(keys.size()));
        put16(v.format);
        put32(len);
        put32(max);
        put32(uint32_t(data.size()));
        keys += k;
        keys += '\0';
        std::string d = v.format == 0x0404 ? std::string(reinterpret_cast<const char*>(&v.integer), 4) : v.bytes;
        d.resize(max, '\0');
        data += d;
    }
    while (keys.size() % 4) keys += '\0';
    const uint32_t key_start = 20 + uint32_t(index.size()), data_start = key_start + uint32_t(keys.size());
    std::string out(20, '\0');
    const uint32_t head[5] = {0x46535000, 0x00000101, key_start, data_start, uint32_t(sfo.size())};
    std::memcpy(out.data(), head, 20);
    return out + index + keys + data;
}

SfoValue sfo_string(const std::string& s, uint32_t max) {
    SfoValue v;
    v.bytes = s.substr(0, max ? max - 1 : s.size());
    v.max = max;
    return v;
}
SfoValue sfo_int(uint32_t x) {
    SfoValue v;
    v.format = 0x0404;
    v.integer = x;
    v.max = 4;
    return v;
}
std::string sfo_get(const Sfo& sfo, const char* key) {
    const auto it = sfo.find(key);
    return it == sfo.end() ? std::string() : std::string(it->second.bytes.c_str());
}

// ---------------------------------------------------------------- files
std::string read_file(const fsys::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_file(const fsys::path& p, const std::string& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(b.data(), std::streamsize(b.size()));
    return bool(f);
}
int64_t unix_time(const fsys::path& p) {
    std::error_code ec;
    const auto t = fsys::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}
// System files, left out of the file list the game sees.
bool system_file(const std::string& name) { return name == "PARAM.SFO" || name == "PARAM.PFD"; }

struct Entry {
    std::string dir, list_param, subtitle;
    int64_t mtime = 0;
};

// ---------------------------------------------------------------- the operation
struct Context {
    GuestAddr base = 0;
    GuestAddr result, list_get, list_set, fixed_set, stat_get, stat_set, file_get, file_set;
};
std::mutex g_lock;
Context g_ctx;

const Context& context(GuestMemory& m) {
    if (!g_ctx.base) {
        g_ctx.base = kernel().main_memory().alloc(0x1000, 0x80);
        GuestAddr at = g_ctx.base;
        auto take = [&](uint32_t size) {
            const GuestAddr a = at;
            at += (size + 15) & ~15u;
            return a;
        };
        g_ctx.result = take(off::res_size);
        g_ctx.list_get = take(off::lget_size);
        g_ctx.list_set = take(off::lset_size);
        g_ctx.fixed_set = take(off::fset_size);
        g_ctx.stat_get = take(off::sget_size);
        g_ctx.stat_set = take(off::sset_size);
        g_ctx.file_get = take(off::fget_size);
        g_ctx.file_set = take(off::file_set_size);
    }
    std::memset(m.host(g_ctx.base), 0, 0x1000);
    return g_ctx;
}

const char* op_name(Op op) {
    switch (op) {
        case Op::AutoLoad: return "AutoLoad";
        case Op::AutoSave: return "AutoSave";
        case Op::ListLoad: return "ListLoad";
        case Op::ListSave: return "ListSave";
        case Op::FixedLoad: return "FixedLoad";
        default: return "FixedSave";
    }
}

struct Args {
    Op op;
    GuestAddr dir_name = 0, set_list = 0, set_buf = 0, func_list = 0, func_fixed = 0, func_stat = 0, func_file = 0,
              userdata = 0;
};

uint32_t savedata_op(PPUContext& c, GuestMemory& m, const Args& a) {
    std::unique_lock<std::mutex> lk(g_lock, std::try_to_lock);
    if (!lk) return CELL_SAVEDATA_ERROR_BUSY;
    if (!a.set_buf || !a.func_stat) return CELL_SAVEDATA_ERROR_PARAM;
    const Context& x = context(m);
    fsys::path base;
    if (!vfs().resolve("/dev_hdd0/home/00000001/savedata", base)) return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    std::error_code ec;
    fsys::create_directories(base, ec);
    m.write<uint32_t>(x.result + off::res_userdata, a.userdata);
    auto clear_result = [&] { std::memset(m.host(x.result), 0, off::res_userdata); };
    auto result = [&] { return m.read<int32_t>(x.result + off::res_result); };
    auto call = [&](GuestAddr fn, GuestAddr get, GuestAddr set) {
        clear_result();
        c.gpr[3] = x.result;
        c.gpr[4] = get;
        c.gpr[5] = set;
        call_guest(c, m, fn);
    };
    const uint32_t buf = m.read<uint32_t>(a.set_buf + off::buf_ptr), buf_size = m.read<uint32_t>(a.set_buf + off::buf_size);
    const uint32_t dir_list_max = m.read<uint32_t>(a.set_buf + off::buf_dir_list_max);
    const uint32_t file_list_max = m.read<uint32_t>(a.set_buf + off::buf_file_list_max);

    std::string dir;
    if (a.set_list) {
        // The saves whose names start with one of the prefixes ("A|B"), sorted as asked.
        const std::string prefixes = guest_string(m, m.read<uint32_t>(a.set_list + off::list_prefix), 256);
        std::vector<std::string> prefix_list;
        for (size_t i = 0; i <= prefixes.size();) {
            const size_t bar = std::min(prefixes.find('|', i), prefixes.size());
            prefix_list.push_back(prefixes.substr(i, bar - i));
            i = bar + 1;
        }
        std::vector<Entry> entries;
        uint32_t dir_num = 0;
        for (const auto& d : fsys::directory_iterator(base, ec)) {
            const std::string name = path_to_utf8(d.path().filename());
            if (!d.is_directory() || !valid_dir_name(name)) continue;
            if (!std::any_of(prefix_list.begin(), prefix_list.end(), [&](const std::string& p) { return name.rfind(p, 0) == 0; }))
                continue;
            if (!dir_num++) std::memset(m.host(buf), 0, buf_size);
            const Sfo sfo = load_sfo(d.path() / "PARAM.SFO");
            if (sfo.empty() || entries.size() >= dir_list_max) continue;
            entries.push_back({sfo_get(sfo, "SAVEDATA_DIRECTORY"), sfo_get(sfo, "SAVEDATA_LIST_PARAM"),
                               sfo_get(sfo, "SUB_TITLE"), unix_time(d.path())});
        }
        const bool by_subtitle = m.read<uint32_t>(a.set_list + off::list_sort_type) == 1;
        std::sort(entries.begin(), entries.end(), [&](const Entry& p, const Entry& q) {
            return by_subtitle ? (p.subtitle != q.subtitle ? p.subtitle < q.subtitle : p.mtime < q.mtime)
                               : (p.mtime != q.mtime ? p.mtime < q.mtime : p.subtitle < q.subtitle);
        });
        if (m.read<uint32_t>(a.set_list + off::list_sort_order) == 0) std::reverse(entries.begin(), entries.end());
        m.write<uint32_t>(x.list_get + off::lget_dir_num, dir_num);
        m.write<uint32_t>(x.list_get + off::lget_dir_list_num, uint32_t(entries.size()));
        m.write<uint32_t>(x.list_get + off::lget_dir_list, buf);
        for (size_t i = 0; i < entries.size(); ++i) {
            const GuestAddr e = buf + uint32_t(i) * off::dirlist_size;
            put_string(m, e + off::dirlist_name, entries[i].dir, 32);
            put_string(m, e + off::dirlist_param, entries[i].list_param, 8);
        }
        if (a.func_list) {
            m.write<uint32_t>(x.list_set + off::lset_focus_pos, FOCUSPOS_LISTHEAD);
            call(a.func_list, x.list_get, x.list_set);
            const int32_t r = result();
            if (r != CBRESULT_OK_NEXT)
                return r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM ? CELL_OK : CELL_SAVEDATA_ERROR_CBRESULT;
            // No dialog: the game's own choice. New data when it offers some and asks for it
            // (or there is nothing else), else the focused entry among the fixed ones.
            std::vector<std::string> fixed;
            const uint32_t fixed_num = m.read<uint32_t>(x.list_set + off::lset_fixed_num);
            const GuestAddr fixed_list = m.read<uint32_t>(x.list_set + off::lset_fixed_list);
            for (uint32_t i = 0; fixed_list && i < fixed_num && i < 2048; ++i)
                fixed.push_back(guest_string(m, fixed_list + i * off::dirlist_size, 32));
            const GuestAddr new_data = m.read<uint32_t>(x.list_set + off::lset_new_data);
            const uint32_t focus = m.read<uint32_t>(x.list_set + off::lset_focus_pos);
            const std::string focus_dir = guest_string(m, m.read<uint32_t>(x.list_set + off::lset_focus_dir), 32);
            if (new_data && (focus == FOCUSPOS_NEWDATA || fixed.empty())) {
                dir = guest_string(m, m.read<uint32_t>(new_data + off::new_dir), 32);
            } else if (focus == FOCUSPOS_DIRNAME && !focus_dir.empty()) {
                dir = focus_dir;
            } else if (!fixed.empty()) {
                dir = (focus == FOCUSPOS_LISTTAIL || focus == FOCUSPOS_OLDEST) ? fixed.back() : fixed.front();
            }
        }
        if (a.func_fixed) {
            call(a.func_fixed, x.list_get, x.fixed_set);
            const int32_t r = result();
            if (r != CBRESULT_OK_NEXT) return r == CBRESULT_OK_LAST ? CELL_OK : CELL_SAVEDATA_ERROR_CBRESULT;
            const GuestAddr name = m.read<uint32_t>(x.fixed_set + off::fset_dir);
            if (!name) return CELL_SAVEDATA_ERROR_PARAM;  // error 26
            dir = guest_string(m, name, 32);
        }
        if (dir_num) std::memset(m.host(buf), 0, buf_size);  // again, for the stat callback
    }
    if (a.dir_name) dir = guest_string(m, a.dir_name, 32);
    if (!valid_dir_name(dir)) {
        SLY4_WARN("cellSaveData: nome de diretório inválido '%s'", dir.c_str());
        return CELL_SAVEDATA_ERROR_BROKEN;
    }

    // ---- stat
    const fsys::path path = base / dir;
    Sfo sfo = load_sfo(path / "PARAM.SFO");
    const bool is_new = sfo.empty();
    const int64_t t = unix_time(path);
    m.write<int32_t>(x.stat_get + off::sget_free_kb, 40 * 1024 * 1024 - 256);
    m.write<uint32_t>(x.stat_get + off::sget_is_new, is_new ? 1 : 0);
    m.write<int64_t>(x.stat_get + off::sget_atime, t);
    m.write<int64_t>(x.stat_get + off::sget_mtime, t);
    m.write<int64_t>(x.stat_get + off::sget_ctime, t);
    put_string(m, x.stat_get + off::sget_dir_name, dir, 32);
    if (!is_new) {
        const GuestAddr p = x.stat_get + off::sget_param;
        put_string(m, p + off::sfp_title, sfo_get(sfo, "TITLE"), 128);
        put_string(m, p + off::sfp_subtitle, sfo_get(sfo, "SUB_TITLE"), 128);
        put_string(m, p + off::sfp_detail, sfo_get(sfo, "DETAIL"), 1024);
        m.write<uint32_t>(p + off::sfp_attribute, sfo.count("ATTRIBUTE") ? sfo["ATTRIBUTE"].integer : 0);
        m.write<uint32_t>(p + off::sfp_parental, sfo.count("PARENTAL_LEVEL") ? sfo["PARENTAL_LEVEL"].integer : 0);
        put_string(m, p + off::sfp_list_param, sfo_get(sfo, "SAVEDATA_LIST_PARAM"), 8);
        std::memset(m.host(buf), 0, buf_size);
    }
    std::vector<std::string> names;
    for (const auto& f : fsys::directory_iterator(path, ec)) {
        const std::string name = path_to_utf8(f.path().filename());
        if (f.is_regular_file() && !system_file(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    uint64_t bytes = 0;
    uint32_t listed = 0;
    for (const std::string& name : names) {
        const uint64_t size = fsys::file_size(path / name, ec);
        bytes += (size + 1023) & ~uint64_t(1023);
        if (listed >= file_list_max) continue;
        const GuestAddr e = buf + listed++ * off::fstat_bytes;
        const uint32_t type = name == "ICON0.PNG"   ? FILETYPE_ICON0
                              : name == "ICON1.PAM" ? FILETYPE_ICON1
                              : name == "PIC1.PNG"  ? FILETYPE_PIC1
                              : name == "SND0.AT3"  ? FILETYPE_SND0
                              : sfo.count("*" + name) && sfo["*" + name].integer ? FILETYPE_SECUREFILE
                                                                                   : FILETYPE_NORMALFILE;
        m.write<uint32_t>(e + off::fstat_type, type);
        m.write<uint64_t>(e + off::fstat_size, size);
        const int64_t ft = unix_time(path / name);
        m.write<int64_t>(e + off::fstat_atime, ft);
        m.write<int64_t>(e + off::fstat_mtime, ft);
        m.write<int64_t>(e + off::fstat_ctime, ft);
        put_string(m, e + off::fstat_name, name, 13);
    }
    m.write<uint32_t>(x.stat_get + off::sget_file_num, uint32_t(names.size()));
    m.write<uint32_t>(x.stat_get + off::sget_file_list_num, listed);
    m.write<uint32_t>(x.stat_get + off::sget_file_list, buf);
    m.write<int32_t>(x.stat_get + off::sget_sys_size_kb, 35);
    m.write<int32_t>(x.stat_get + off::sget_size_kb, is_new ? 0 : int32_t(bytes / 1024) + 35);
    call(a.func_stat, x.stat_get, x.stat_set);
    if (const int32_t r = result(); r != CBRESULT_OK_NEXT) {
        SLY4_INFO("cellSaveData: %s %s: funcStat devolveu %d", op_name(a.op), dir.c_str(), r);
        return r == CBRESULT_OK_LAST ? CELL_OK : CELL_SAVEDATA_ERROR_CBRESULT;
    }
    bool modified = false, recreated = false;
    const GuestAddr set_param = m.read<uint32_t>(x.stat_set + off::sset_param);
    if (set_param) {
        if (m.read<uint32_t>(set_param + off::sfp_attribute) > 1 || m.read<uint32_t>(set_param + off::sfp_parental) > 11)
            return CELL_SAVEDATA_ERROR_PARAM;  // errors 57, 58
        SfoValue account;
        account.format = 0x0004;
        account.bytes = "0000000000000000";
        account.max = 16;
        sfo["ACCOUNT_ID"] = account;
        sfo["ATTRIBUTE"] = sfo_int(m.read<uint32_t>(set_param + off::sfp_attribute));
        sfo["CATEGORY"] = sfo_string("SD", 4);
        sfo["DETAIL"] = sfo_string(guest_string(m, set_param + off::sfp_detail, 1024), 1024);
        sfo["PARENTAL_LEVEL"] = sfo_int(m.read<uint32_t>(set_param + off::sfp_parental));
        sfo["SAVEDATA_DIRECTORY"] = sfo_string(dir, 32);
        sfo["SAVEDATA_LIST_PARAM"] = sfo_string(guest_string(m, set_param + off::sfp_list_param, 8), 8);
        sfo["SUB_TITLE"] = sfo_string(guest_string(m, set_param + off::sfp_subtitle, 128), 128);
        sfo["TITLE"] = sfo_string(guest_string(m, set_param + off::sfp_title, 128), 128);
        modified = true;
    } else if (is_new) {
        // Nothing to load and nothing to create: what the system software answers too
        // ("sysutil savedata parameter error : 50"). The game then starts from defaults.
        SLY4_INFO("cellSaveData: %s %s: não existe (o jogo usa os padrões)", op_name(a.op), dir.c_str());
        return CELL_SAVEDATA_ERROR_PARAM;
    }
    switch (m.read<uint32_t>(x.stat_set + off::sset_recreate) & RECREATE_MASK) {
        case RECREATE_NO:
        case RECREATE_NO_NOBROKEN: break;
        case RECREATE_YES:
        case RECREATE_YES_RESET_OWNER:
            if (!set_param) return CELL_SAVEDATA_ERROR_PARAM;
            for (auto it = sfo.begin(); it != sfo.end();) it = it->first[0] == '*' ? sfo.erase(it) : std::next(it);
            recreated = modified = true;
            break;
        default: return CELL_SAVEDATA_ERROR_PARAM;  // error 48
    }

    // ---- files, in memory until the end
    std::map<std::string, std::string> files;
    if (!recreated)
        for (const std::string& name : names) files[name] = read_file(path / name);
    m.write<uint32_t>(x.file_get + off::fget_exc_size, 0);
    uint32_t status = CELL_OK;
    while (a.func_file) {
        std::memset(m.host(x.file_set), 0, off::file_set_size);
        call(a.func_file, x.file_get, x.file_set);
        const int32_t r = result();
        if (r != CBRESULT_OK_NEXT) {
            if (r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM) break;
            status = r < CBRESULT_ERR_INVALID || r > CBRESULT_OK_LAST_NOCONFIRM ? CELL_SAVEDATA_ERROR_PARAM
                                                                                : CELL_SAVEDATA_ERROR_CBRESULT;
            break;
        }
        const uint32_t type = m.read<uint32_t>(x.file_set + off::file_type);
        std::string name;
        switch (type) {
            case FILETYPE_SECUREFILE:
            case FILETYPE_NORMALFILE: name = guest_string(m, m.read<uint32_t>(x.file_set + off::file_name), 13); break;
            case FILETYPE_ICON0: name = "ICON0.PNG"; break;
            case FILETYPE_ICON1: name = "ICON1.PAM"; break;
            case FILETYPE_PIC1: name = "PIC1.PNG"; break;
            case FILETYPE_SND0: name = "SND0.AT3"; break;
            default: status = CELL_SAVEDATA_ERROR_PARAM; break;
        }
        if (status != CELL_OK) break;
        if (name.empty() || system_file(name) || name.find_first_of("/\\") != std::string::npos || name == "." ||
            name == "..") {
            status = CELL_SAVEDATA_ERROR_PARAM;
            break;
        }
        const uint32_t op = m.read<uint32_t>(x.file_set + off::file_op);
        const uint32_t offset = m.read<uint32_t>(x.file_set + off::file_offset);
        const uint32_t size = m.read<uint32_t>(x.file_set + off::file_size);
        const uint32_t buf_bytes = m.read<uint32_t>(x.file_set + off::file_buf_size);
        const GuestAddr data = m.read<uint32_t>(x.file_set + off::file_buf);
        if (op != FILEOP_DELETE && (buf_bytes < size || (!data && buf_bytes))) {
            status = CELL_SAVEDATA_ERROR_PARAM;  // errors 72, 73
            break;
        }
        uint32_t done = 0;
        if (op == FILEOP_READ) {
            const auto it = files.find(name);
            if (it == files.end() || it->second.size() <= offset) {
                SLY4_INFO("cellSaveData: %s %s: %s não existe", op_name(a.op), dir.c_str(), name.c_str());
                status = CELL_SAVEDATA_ERROR_FAILURE;
                break;
            }
            done = uint32_t(std::min<size_t>(size, it->second.size() - offset));
            std::memcpy(m.host(data), it->second.data() + offset, done);
        } else if (op == FILEOP_WRITE || op == FILEOP_WRITE_NOTRUNC) {
            std::string& f = files[name];
            if (f.size() < offset) f.resize(offset, '\0');
            if (op == FILEOP_WRITE) f.resize(offset);
            if (f.size() < offset + size) f.resize(offset + size, '\0');
            std::memcpy(f.data() + offset, m.host(data), size);
            done = size;
            sfo["*" + name] = sfo_int(type == FILETYPE_SECUREFILE ? 1 : 0);
            modified = true;
        } else if (op == FILEOP_DELETE) {
            if (!files.erase(name)) {
                status = CELL_SAVEDATA_ERROR_FAILURE;
                break;
            }
            sfo.erase("*" + name);
            modified = true;
        } else {
            status = CELL_SAVEDATA_ERROR_PARAM;  // error 60
            break;
        }
        m.write<uint32_t>(x.file_get + off::fget_exc_size, done);
    }

    // ---- the directory, replaced as a whole
    if (!sfo.empty() && modified) {
        const fsys::path work = base / (".working_" + dir), backup = base / (".backup_" + dir);
        fsys::remove_all(work, ec);
        fsys::create_directories(work, ec);
        bool ok = write_file(work / "PARAM.SFO", save_sfo(sfo));
        for (const auto& [name, bytes_] : files) ok = ok && write_file(work / name, bytes_);
        if (!ok) {
            SLY4_ERROR("cellSaveData: não foi possível gravar %s", path_to_utf8(work).c_str());
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        fsys::remove_all(backup, ec);
        if (fsys::exists(path)) fsys::rename(path, backup, ec);
        fsys::rename(work, path, ec);
        fsys::remove_all(backup, ec);
        SLY4_INFO("cellSaveData: %s %s: gravado (%zu arquivos)", op_name(a.op), dir.c_str(), files.size());
    } else if (status == CELL_OK) {
        SLY4_INFO("cellSaveData: %s %s: ok", op_name(a.op), dir.c_str());
    }
    return status;
}

// cellSaveDataAuto*2(version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata)
template <Op O>
void cellSaveDataAuto(PPUContext& c, GuestMemory& m) {
    Args a{O};
    a.dir_name = a32(c, 4);
    a.set_buf = a32(c, 6);
    a.func_stat = a32(c, 7);
    a.func_file = a32(c, 8);
    a.userdata = a32(c, 10);
    if (!a.dir_name) {
        c.gpr[3] = CELL_SAVEDATA_ERROR_PARAM;
        return;
    }
    c.gpr[3] = savedata_op(c, m, a);
}
// cellSaveDataList*2 / Fixed*2(version, setList, setBuf, funcList or funcFixed, funcStat,
// funcFile, container, userdata)
template <Op O>
void cellSaveDataListFixed(PPUContext& c, GuestMemory& m) {
    Args a{O};
    a.set_list = a32(c, 4);
    a.set_buf = a32(c, 5);
    (O == Op::ListLoad || O == Op::ListSave ? a.func_list : a.func_fixed) = a32(c, 6);
    a.func_stat = a32(c, 7);
    a.func_file = a32(c, 8);
    a.userdata = a32(c, 10);
    if (!a.set_list || (!a.func_list && !a.func_fixed)) {
        c.gpr[3] = CELL_SAVEDATA_ERROR_PARAM;
        return;
    }
    c.gpr[3] = savedata_op(c, m, a);
}
// cellSaveDataDelete2(container): the system's delete dialog; the user leaves it.
void cellSaveDataDelete(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_CANCEL; }

}  // namespace

HleModule hle_cell_savedata() {
    return {"cellSysutil",
            {
                {"cellSaveDataAutoLoad2", cellSaveDataAuto<Op::AutoLoad>},
                {"cellSaveDataAutoSave2", cellSaveDataAuto<Op::AutoSave>},
                {"cellSaveDataAutoLoad", cellSaveDataAuto<Op::AutoLoad>},
                {"cellSaveDataAutoSave", cellSaveDataAuto<Op::AutoSave>},
                {"cellSaveDataListLoad2", cellSaveDataListFixed<Op::ListLoad>},
                {"cellSaveDataListSave2", cellSaveDataListFixed<Op::ListSave>},
                {"cellSaveDataFixedLoad2", cellSaveDataListFixed<Op::FixedLoad>},
                {"cellSaveDataFixedSave2", cellSaveDataListFixed<Op::FixedSave>},
                {"cellSaveDataDelete2", cellSaveDataDelete},
            }};
}

}  // namespace sly4
