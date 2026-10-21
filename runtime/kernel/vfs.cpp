#include "kernel/vfs.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "kernel/kernel.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sly4 {

namespace fsys = std::filesystem;

namespace {

constexpr uint32_t CELL_EISDIR = 0x80010012, CELL_EACCES = 0x80010029, CELL_EBADF = 0x8001002A,
                   CELL_EIO = 0x8001002B, CELL_EMFILE = 0x8001002C, CELL_ENOTDIR = 0x8001002E,
                   CELL_ENOTEMPTY = 0x80010036;
constexpr int32_t kModeDir = 0040000 | 0777, kModeFile = 0100000 | 0666;

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

uint32_t from_win32(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME: return CELL_ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: return CELL_EACCES;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return CELL_EEXIST;
        case ERROR_DIR_NOT_EMPTY: return CELL_ENOTEMPTY;
        default: return CELL_EIO;
    }
}
uint32_t from_ec(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return CELL_ENOENT;
    if (ec == std::errc::file_exists) return CELL_EEXIST;
    if (ec == std::errc::directory_not_empty) return CELL_ENOTEMPTY;
    if (ec == std::errc::permission_denied) return CELL_EACCES;
    return CELL_EIO;
}

int64_t unix_seconds(fsys::file_time_type t) {
    using namespace std::chrono;
    return duration_cast<seconds>(file_clock::to_sys(t).time_since_epoch()).count();
}

struct Handle {
    std::mutex m;
    std::string guest;
    fsys::path host;
    HANDLE h = INVALID_HANDLE_VALUE;
    bool dir = false;
    std::vector<fsys::directory_entry> entries;  // directories: "." and ".." are synthesised
    size_t next = 0;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Handle>> g_files;
uint32_t g_next_fd = 3;

uint32_t add_handle(std::shared_ptr<Handle> h, uint32_t& fd) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_files.size() >= 1024) return CELL_EMFILE;
    while (g_files.count(g_next_fd)) ++g_next_fd;
    fd = g_next_fd++;
    g_files.emplace(fd, std::move(h));
    return CELL_OK;
}
std::shared_ptr<Handle> get_handle(uint32_t fd) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_files.find(fd);
    return it == g_files.end() ? nullptr : it->second;
}

void log_fs(const char* op, const std::string& path, uint32_t r) {
    if (r == CELL_OK)
        SLY4_INFO("[fs] %s %s", op, path.c_str());
    else
        SLY4_INFO("[fs] %s %s -> 0x%08X", op, path.c_str(), r);
}

bool resolve_or_log(const std::string& path, fsys::path& host) {
    if (vfs().resolve(path, host)) return true;
    SLY4_WARN("[fs] caminho fora do VFS: %s", path.c_str());
    return false;
}

void stat_of(const fsys::path& p, bool dir, FsStat& st) {
    std::error_code ec;
    st.mode = dir ? kModeDir : kModeFile;
    st.size = dir ? 0 : fsys::file_size(p, ec);
    const auto t = fsys::last_write_time(p, ec);
    st.atime = st.mtime = st.ctime = ec ? 0 : unix_seconds(t);
    st.blksize = 4096;
}

}  // namespace

// ---------------------------------------------------------------- paths
fsys::path path_from_utf8(const std::string& s) {
    return fsys::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string path_to_utf8(const fsys::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

Vfs& vfs() {
    static Vfs v;
    return v;
}

void Vfs::setup(const fsys::path& game_dir, const fsys::path& data_dir) {
    data_dir_ = data_dir;
    mount("/dev_bdvd", game_dir.parent_path());
    mount("/app_home", game_dir / "USRDIR");
    mount("/dev_hdd0", data_dir / "hdd0");
    mount("/dev_hdd1", data_dir / "hdd1");
    std::error_code ec;
    fsys::create_directories(data_dir / "hdd0" / "game", ec);
    fsys::create_directories(data_dir / "hdd1", ec);
    SLY4_INFO("VFS: /dev_bdvd = %s; /dev_hdd0 e /dev_hdd1 em %s", path_to_utf8(game_dir.parent_path()).c_str(),
              path_to_utf8(data_dir).c_str());
}

void Vfs::mount(const std::string& device, const fsys::path& host) {
    const std::string dev = lower(device);
    std::lock_guard<std::mutex> g(lock_);
    std::erase_if(mounts_, [&](const auto& m) { return m.first == dev; });
    mounts_.emplace_back(dev, host);
    std::sort(mounts_.begin(), mounts_.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

bool Vfs::resolve(const std::string& guest, fsys::path& host) const {
    // Split into components, dropping empty ones and "."; ".." may not leave the device.
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < guest.size()) {
        size_t j = guest.find('/', i);
        if (j == std::string::npos) j = guest.size();
        std::string part = guest.substr(i, j - i);
        if (part == "..") {
            if (parts.size() <= 1) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    if (parts.empty()) return false;
    const std::string device = "/" + lower(parts[0]);
    std::lock_guard<std::mutex> g(lock_);
    for (const auto& [dev, root] : mounts_) {
        if (dev != device) continue;
        host = root;
        for (size_t k = 1; k < parts.size(); ++k) host /= path_from_utf8(parts[k]);
        return true;
    }
    return false;
}

void FsStat::write(GuestMemory& m, GuestAddr at) const {
    m.write<int32_t>(at, mode);
    m.write<int32_t>(at + 4, 0);  // uid
    m.write<int32_t>(at + 8, 0);  // gid
    m.write<int64_t>(at + 12, atime);
    m.write<int64_t>(at + 20, mtime);
    m.write<int64_t>(at + 28, ctime);
    m.write<uint64_t>(at + 36, size);
    m.write<uint64_t>(at + 44, blksize);
}

// ---------------------------------------------------------------- files
uint32_t fs_open(const std::string& path, uint32_t flags, uint32_t& fd) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    if (fsys::is_directory(host, ec)) {
        log_fs("open", path, CELL_EISDIR);
        return CELL_EISDIR;
    }
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING;
    switch (flags & fs_flag::kAccmode) {
        case fs_flag::kWronly: access = GENERIC_WRITE; break;
        case fs_flag::kRdwr: access = GENERIC_READ | GENERIC_WRITE; break;
        default: break;
    }
    if (flags & fs_flag::kCreat)
        disposition = flags & fs_flag::kExcl ? CREATE_NEW : flags & fs_flag::kTrunc ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & fs_flag::kTrunc)
        disposition = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const uint32_t r = from_win32(GetLastError());
        log_fs("open", path, r);
        return r;
    }
    if (flags & fs_flag::kAppend) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(h, zero, nullptr, FILE_END);
    }
    auto handle = std::make_shared<Handle>();
    handle->guest = path;
    handle->host = host;
    handle->h = h;
    const uint32_t r = add_handle(handle, fd);
    log_fs("open", path, r);
    return r;
}

uint32_t fs_close(uint32_t fd) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_files.find(fd);
    if (it == g_files.end() || it->second->dir) return CELL_EBADF;
    g_files.erase(it);
    return CELL_OK;
}

uint32_t fs_read(uint32_t fd, void* dst, uint64_t size, uint64_t& done) {
    done = 0;
    auto h = get_handle(fd);
    if (!h || h->dir) return CELL_EBADF;
    std::lock_guard<std::mutex> lk(h->m);
    auto* p = static_cast<uint8_t*>(dst);
    while (done < size) {
        const DWORD chunk = DWORD(std::min<uint64_t>(size - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h->h, p + done, chunk, &got, nullptr)) return from_win32(GetLastError());
        done += got;
        if (got < chunk) break;
    }
    return CELL_OK;
}

uint32_t fs_read_at(uint32_t fd, uint64_t offset, void* dst, uint64_t size, uint64_t& done) {
    done = 0;
    auto h = get_handle(fd);
    if (!h || h->dir) return CELL_EBADF;
    std::lock_guard<std::mutex> lk(h->m);
    LARGE_INTEGER cur{}, zero{}, to{};
    SetFilePointerEx(h->h, zero, &cur, FILE_CURRENT);
    to.QuadPart = int64_t(offset);
    SetFilePointerEx(h->h, to, nullptr, FILE_BEGIN);
    auto* p = static_cast<uint8_t*>(dst);
    uint32_t r = CELL_OK;
    while (done < size) {
        const DWORD chunk = DWORD(std::min<uint64_t>(size - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h->h, p + done, chunk, &got, nullptr)) {
            r = from_win32(GetLastError());
            break;
        }
        done += got;
        if (got < chunk) break;
    }
    SetFilePointerEx(h->h, cur, nullptr, FILE_BEGIN);
    return r;
}

uint32_t fs_write(uint32_t fd, const void* src, uint64_t size, uint64_t& done) {
    done = 0;
    auto h = get_handle(fd);
    if (!h || h->dir) return CELL_EBADF;
    std::lock_guard<std::mutex> lk(h->m);
    const auto* p = static_cast<const uint8_t*>(src);
    while (done < size) {
        const DWORD chunk = DWORD(std::min<uint64_t>(size - done, 1u << 30));
        DWORD put = 0;
        if (!WriteFile(h->h, p + done, chunk, &put, nullptr)) return from_win32(GetLastError());
        done += put;
    }
    return CELL_OK;
}

uint32_t fs_lseek(uint32_t fd, int64_t offset, uint32_t whence, uint64_t& pos) {
    auto h = get_handle(fd);
    if (!h || h->dir) return CELL_EBADF;
    if (whence > 2) return CELL_EINVAL;
    std::lock_guard<std::mutex> lk(h->m);
    LARGE_INTEGER to{}, out{};
    to.QuadPart = offset;
    const DWORD method = whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(h->h, to, &out, method)) return CELL_EINVAL;
    pos = uint64_t(out.QuadPart);
    return CELL_OK;
}

uint32_t fs_fsync(uint32_t fd) {
    auto h = get_handle(fd);
    if (!h || h->dir) return CELL_EBADF;
    FlushFileBuffers(h->h);
    return CELL_OK;
}

uint32_t fs_stat(const std::string& path, FsStat& st) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    const auto s = fsys::status(host, ec);
    if (ec || !fsys::exists(s)) {
        log_fs("stat", path, CELL_ENOENT);
        return CELL_ENOENT;
    }
    stat_of(host, fsys::is_directory(s), st);
    log_fs("stat", path, CELL_OK);
    return CELL_OK;
}

uint32_t fs_fstat(uint32_t fd, FsStat& st) {
    auto h = get_handle(fd);
    if (!h) return CELL_EBADF;
    stat_of(h->host, h->dir, st);
    if (!h->dir) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h->h, &size)) st.size = uint64_t(size.QuadPart);
    }
    return CELL_OK;
}

// ---------------------------------------------------------------- directories
uint32_t fs_opendir(const std::string& path, uint32_t& fd) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    if (!fsys::exists(host, ec)) {
        log_fs("opendir", path, CELL_ENOENT);
        return CELL_ENOENT;
    }
    if (!fsys::is_directory(host, ec)) {
        log_fs("opendir", path, CELL_ENOTDIR);
        return CELL_ENOTDIR;
    }
    auto handle = std::make_shared<Handle>();
    handle->guest = path;
    handle->host = host;
    handle->dir = true;
    for (const auto& e : fsys::directory_iterator(host, ec)) handle->entries.push_back(e);
    std::sort(handle->entries.begin(), handle->entries.end(),
              [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
    const uint32_t r = add_handle(handle, fd);
    log_fs("opendir", path, r);
    return r;
}

uint32_t fs_readdir(uint32_t fd, std::string& name, uint8_t& type, FsStat* st, bool& got) {
    got = false;
    auto h = get_handle(fd);
    if (!h || !h->dir) return CELL_EBADF;
    std::lock_guard<std::mutex> lk(h->m);
    const size_t i = h->next;
    if (i >= h->entries.size() + 2) return CELL_OK;
    ++h->next;
    got = true;
    if (i < 2) {
        name = i == 0 ? "." : "..";
        type = 1;
        if (st) stat_of(h->host, true, *st);
        return CELL_OK;
    }
    const auto& e = h->entries[i - 2];
    std::error_code ec;
    const bool dir = e.is_directory(ec);
    name = path_to_utf8(e.path().filename());
    type = dir ? 1 : 2;
    if (st) stat_of(e.path(), dir, *st);
    return CELL_OK;
}

uint32_t fs_closedir(uint32_t fd) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_files.find(fd);
    if (it == g_files.end() || !it->second->dir) return CELL_EBADF;
    g_files.erase(it);
    return CELL_OK;
}

uint32_t fs_mkdir(const std::string& path) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    uint32_t r = CELL_OK;
    if (fsys::exists(host, ec))
        r = CELL_EEXIST;
    else if (!fsys::create_directory(host, ec))
        r = ec ? from_ec(ec) : CELL_EEXIST;
    log_fs("mkdir", path, r);
    return r;
}

uint32_t fs_rmdir(const std::string& path) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    uint32_t r = CELL_OK;
    if (!fsys::is_directory(host, ec))
        r = fsys::exists(host, ec) ? CELL_ENOTDIR : CELL_ENOENT;
    else if (!fsys::is_empty(host, ec))
        r = CELL_ENOTEMPTY;
    else if (!fsys::remove(host, ec))
        r = from_ec(ec);
    log_fs("rmdir", path, r);
    return r;
}

uint32_t fs_unlink(const std::string& path) {
    fsys::path host;
    if (!resolve_or_log(path, host)) return CELL_ENOENT;
    std::error_code ec;
    uint32_t r = CELL_OK;
    if (fsys::is_directory(host, ec))
        r = CELL_EISDIR;
    else if (!fsys::remove(host, ec))
        r = ec ? from_ec(ec) : CELL_ENOENT;
    log_fs("unlink", path, r);
    return r;
}

}  // namespace sly4
