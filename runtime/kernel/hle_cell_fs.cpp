// sys_fs (cellFs*) and the sys_fs syscalls 801-818, on the VFS (kernel/vfs.h).
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }
std::string path_arg(const PPUContext& c, GuestMemory& m, int n) { return m.read_cstring(a32(c, n), 1024); }

// ---------------------------------------------------------------- shared by cellFs and sc
void fs_open_(PPUContext& c, GuestMemory& m) {
    uint32_t fd = 0;
    const uint32_t r = fs_open(path_arg(c, m, 3), a32(c, 4), fd);
    if (r == CELL_OK) m.write<uint32_t>(a32(c, 5), fd);
    c.gpr[3] = r;
}
void fs_read_(PPUContext& c, GuestMemory& m) {
    uint64_t done = 0;
    const uint32_t r = fs_read(a32(c, 3), m.host(a32(c, 4)), c.gpr[5], done);
    if (a32(c, 6)) m.write<uint64_t>(a32(c, 6), done);
    c.gpr[3] = r;
}
void fs_write_(PPUContext& c, GuestMemory& m) {
    uint64_t done = 0;
    const uint32_t r = fs_write(a32(c, 3), m.host(a32(c, 4)), c.gpr[5], done);
    if (a32(c, 6)) m.write<uint64_t>(a32(c, 6), done);
    c.gpr[3] = r;
}
void fs_close_(PPUContext& c, GuestMemory&) { c.gpr[3] = fs_close(a32(c, 3)); }
void fs_lseek_(PPUContext& c, GuestMemory& m) {
    uint64_t pos = 0;
    const uint32_t r = fs_lseek(a32(c, 3), int64_t(c.gpr[4]), a32(c, 5), pos);
    if (r == CELL_OK && a32(c, 6)) m.write<uint64_t>(a32(c, 6), pos);
    c.gpr[3] = r;
}
void fs_stat_(PPUContext& c, GuestMemory& m) {
    FsStat st;
    const uint32_t r = fs_stat(path_arg(c, m, 3), st);
    if (r == CELL_OK) st.write(m, a32(c, 4));
    c.gpr[3] = r;
}
void fs_fstat_(PPUContext& c, GuestMemory& m) {
    FsStat st;
    const uint32_t r = fs_fstat(a32(c, 3), st);
    if (r == CELL_OK) st.write(m, a32(c, 4));
    c.gpr[3] = r;
}
void fs_opendir_(PPUContext& c, GuestMemory& m) {
    uint32_t fd = 0;
    const uint32_t r = fs_opendir(path_arg(c, m, 3), fd);
    if (r == CELL_OK) m.write<uint32_t>(a32(c, 4), fd);
    c.gpr[3] = r;
}
// CellFsDirent: d_type u8, d_namlen u8, d_name[256]; *nread = its size, or 0 at the end.
void fs_readdir_(PPUContext& c, GuestMemory& m) {
    std::string name;
    uint8_t type = 0;
    bool got = false;
    const uint32_t r = fs_readdir(a32(c, 3), name, type, nullptr, got);
    const uint32_t dir = a32(c, 4);
    if (r == CELL_OK && got) {
        name.resize(std::min<size_t>(name.size(), 255));
        std::memset(m.host(dir), 0, kFsDirentSize);
        m.write<uint8_t>(dir, type);
        m.write<uint8_t>(dir + 1, uint8_t(name.size()));
        m.copy_to(dir + 2, name.data(), name.size());
    }
    if (a32(c, 5)) m.write<uint64_t>(a32(c, 5), r == CELL_OK && got ? kFsDirentSize : 0);
    c.gpr[3] = r;
}
void fs_closedir_(PPUContext& c, GuestMemory&) { c.gpr[3] = fs_closedir(a32(c, 3)); }
void fs_mkdir_(PPUContext& c, GuestMemory& m) { c.gpr[3] = fs_mkdir(path_arg(c, m, 3)); }
void fs_rmdir_(PPUContext& c, GuestMemory& m) { c.gpr[3] = fs_rmdir(path_arg(c, m, 3)); }
void fs_unlink_(PPUContext& c, GuestMemory& m) { c.gpr[3] = fs_unlink(path_arg(c, m, 3)); }
void fs_fsync_(PPUContext& c, GuestMemory&) { c.gpr[3] = fs_fsync(a32(c, 3)); }

// sys_fs_open has an extra `mode` argument before arg/size; the rest is the same.
void sys_fs_open(PPUContext& c, GuestMemory& m) { fs_open_(c, m); }

// ---------------------------------------------------------------- cellFs only
// cellFsSdataOpen: SDATA (NPD-encrypted) files. Plain files are opened as they are.
void cellFsSdataOpen(PPUContext& c, GuestMemory& m) {
    SLY4_WARN("[fs] cellFsSdataOpen(%s): aberto como arquivo comum", path_arg(c, m, 3).c_str());
    fs_open_(c, m);
}
void cellFsSdataOpenByFd(PPUContext& c, GuestMemory&) {
    SLY4_WARN("[fs] cellFsSdataOpenByFd(%u) sem implementação", a32(c, 3));
    c.gpr[3] = CELL_ENOSYS;
}

// cellFsGetFreeSize(path, u32* block_size, u64* block_count): 40 GiB free in 4 KiB blocks.
void cellFsGetFreeSize(PPUContext& c, GuestMemory& m) {
    m.write<uint32_t>(a32(c, 4), 4096);
    m.write<uint64_t>(a32(c, 5), (40ull << 30) / 4096);
    c.gpr[3] = CELL_OK;
}

// cellFsGetDirectoryEntries(fd, CellFsDirectoryEntry* entries, u32 entries_size, u32* count):
// each entry is a CellFsStat followed by a CellFsDirent (52 + 258, padded to 312).
void cellFsGetDirectoryEntries(PPUContext& c, GuestMemory& m) {
    constexpr uint32_t kEntry = 312;
    const uint32_t fd = a32(c, 3), out = a32(c, 4), max = a32(c, 5) / kEntry;
    uint32_t n = 0, r = CELL_OK;
    while (n < max) {
        std::string name;
        uint8_t type = 0;
        FsStat st;
        bool got = false;
        r = fs_readdir(fd, name, type, &st, got);
        if (r != CELL_OK || !got) break;
        const GuestAddr e = out + n * kEntry;
        std::memset(m.host(e), 0, kEntry);
        st.write(m, e);
        name.resize(std::min<size_t>(name.size(), 255));
        m.write<uint8_t>(e + kFsStatSize, type);
        m.write<uint8_t>(e + kFsStatSize + 1, uint8_t(name.size()));
        m.copy_to(e + kFsStatSize + 2, name.data(), name.size());
        ++n;
    }
    m.write<uint32_t>(a32(c, 6), n);
    c.gpr[3] = r;
}

// ---------------------------------------------------------------- asynchronous I/O
// CellFsAio: fd u32 @0, offset u64 @8, buf u32 @16, size u64 @24, user_data u64 @32.
// Requests run in order on a system thread "FsAio", which calls the game's callback
// func(aio, error, id, transferred).
struct AioRequest {
    GuestAddr aio, func;
    int32_t id;
    bool write;
};
std::mutex g_aio_lock;
std::condition_variable g_aio_cv;
std::deque<AioRequest> g_aio_queue;
bool g_aio_started = false;
int32_t g_aio_next_id = 1;

void aio_worker(PpuThread& t) {
    GuestMemory& m = kernel().mem();
    for (;;) {
        AioRequest rq;
        {
            std::unique_lock<std::mutex> lk(g_aio_lock);
            g_aio_cv.wait(lk, [] { return !g_aio_queue.empty(); });
            rq = g_aio_queue.front();
            g_aio_queue.pop_front();
        }
        const uint32_t fd = m.read<uint32_t>(rq.aio);
        const uint64_t offset = m.read<uint64_t>(rq.aio + 8);
        const uint32_t buf = m.read<uint32_t>(rq.aio + 16);
        const uint64_t size = m.read<uint64_t>(rq.aio + 24);
        uint64_t done = 0;
        uint32_t r;
        if (rq.write) {
            uint64_t pos = 0;
            r = fs_lseek(fd, int64_t(offset), 0, pos);
            if (r == CELL_OK) r = fs_write(fd, m.host(buf), size, done);
        } else {
            r = fs_read_at(fd, offset, m.host(buf), size, done);
        }
        PPUContext& c = t.ctx;
        c.gpr[3] = rq.aio;
        c.gpr[4] = r;
        c.gpr[5] = uint32_t(rq.id);
        c.gpr[6] = done;
        if (rq.func) call_guest(c, m, rq.func);
    }
}

void aio_submit(PPUContext& c, GuestMemory& m, bool write) {
    AioRequest rq{a32(c, 3), a32(c, 5), 0, write};
    {
        std::lock_guard<std::mutex> lk(g_aio_lock);
        if (!g_aio_started) {
            g_aio_started = kernel().start_native_thread("FsAio", aio_worker);
            if (!g_aio_started) {
                c.gpr[3] = CELL_EAGAIN;
                return;
            }
        }
        rq.id = g_aio_next_id++;
        g_aio_queue.push_back(rq);
    }
    g_aio_cv.notify_one();
    m.write<int32_t>(a32(c, 4), rq.id);
    c.gpr[3] = CELL_OK;
}
void cellFsAioRead(PPUContext& c, GuestMemory& m) { aio_submit(c, m, false); }
void cellFsAioWrite(PPUContext& c, GuestMemory& m) { aio_submit(c, m, true); }
void cellFsAioCancel(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g_aio_lock);
    const int32_t id = int32_t(c.gpr[3]);
    for (auto it = g_aio_queue.begin(); it != g_aio_queue.end(); ++it)
        if (it->id == id) {
            g_aio_queue.erase(it);
            c.gpr[3] = CELL_OK;
            return;
        }
    c.gpr[3] = CELL_EINVAL;  // already done or unknown
}
void ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

}  // namespace

HleModule hle_sys_fs() {
    return {"sys_fs",
            {
                {"cellFsOpen", fs_open_},
                {"cellFsSdataOpen", cellFsSdataOpen},
                {"cellFsSdataOpenByFd", cellFsSdataOpenByFd},
                {"cellFsRead", fs_read_},
                {"cellFsWrite", fs_write_},
                {"cellFsClose", fs_close_},
                {"cellFsLseek", fs_lseek_},
                {"cellFsStat", fs_stat_},
                {"cellFsFstat", fs_fstat_},
                {"cellFsOpendir", fs_opendir_},
                {"cellFsReaddir", fs_readdir_},
                {"cellFsClosedir", fs_closedir_},
                {"cellFsMkdir", fs_mkdir_},
                {"cellFsRmdir", fs_rmdir_},
                {"cellFsUnlink", fs_unlink_},
                {"cellFsFsync", fs_fsync_},
                {"cellFsGetFreeSize", cellFsGetFreeSize},
                {"cellFsGetDirectoryEntries", cellFsGetDirectoryEntries},
                {"cellFsAioInit", ok},
                {"cellFsAioFinish", ok},
                {"cellFsAioRead", cellFsAioRead},
                {"cellFsAioWrite", cellFsAioWrite},
                {"cellFsAioCancel", cellFsAioCancel},
            }};
}

void register_fs_syscalls() {
    register_syscall(801, "sys_fs_open", sys_fs_open);
    register_syscall(802, "sys_fs_read", fs_read_);
    register_syscall(803, "sys_fs_write", fs_write_);
    register_syscall(804, "sys_fs_close", fs_close_);
    register_syscall(805, "sys_fs_opendir", fs_opendir_);
    register_syscall(806, "sys_fs_readdir", fs_readdir_);
    register_syscall(807, "sys_fs_closedir", fs_closedir_);
    register_syscall(808, "sys_fs_stat", fs_stat_);
    register_syscall(809, "sys_fs_fstat", fs_fstat_);
    register_syscall(811, "sys_fs_mkdir", fs_mkdir_);
    register_syscall(813, "sys_fs_rmdir", fs_rmdir_);
    register_syscall(814, "sys_fs_unlink", fs_unlink_);
    register_syscall(818, "sys_fs_lseek", fs_lseek_);
}

}  // namespace sly4
