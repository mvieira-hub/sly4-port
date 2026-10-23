// Virtual file system: PS3 paths (/dev_bdvd, /app_home, /dev_hdd0, /dev_hdd1) mapped to
// folders on the PC, and the file descriptor table shared by cellFs and the sys_fs
// syscalls (801-818).
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "memory/guest_memory.h"

namespace sly4 {

class Vfs {
public:
    // `game_dir` is the PS3_GAME folder of the dump; `data_dir` holds hdd0/hdd1.
    void setup(const std::filesystem::path& game_dir, const std::filesystem::path& data_dir);
    void mount(const std::string& device, const std::filesystem::path& host);

    // Host path of a guest path; false if no device matches or the path climbs out of it.
    bool resolve(const std::string& guest, std::filesystem::path& host) const;
    const std::filesystem::path& data_dir() const { return data_dir_; }

private:
    mutable std::mutex lock_;  // mounts change at run time (cellSysCacheMount)
    std::vector<std::pair<std::string, std::filesystem::path>> mounts_;  // longest first
    std::filesystem::path data_dir_;
};
Vfs& vfs();

std::filesystem::path path_from_utf8(const std::string& s);
std::string path_to_utf8(const std::filesystem::path& p);

// ---------------------------------------------------------------- file API (CELL_FS codes)
namespace fs_flag {
constexpr uint32_t kRdonly = 0, kWronly = 1, kRdwr = 2, kAccmode = 3, kCreat = 0x40, kExcl = 0x80, kTrunc = 0x200,
                   kAppend = 0x400;
}

// CellFsStat (52 bytes, 4-byte packed) as the guest sees it.
struct FsStat {
    int32_t mode = 0;
    int64_t atime = 0, mtime = 0, ctime = 0;
    uint64_t size = 0, blksize = 4096;
    void write(GuestMemory& m, GuestAddr at) const;
};
constexpr uint32_t kFsStatSize = 52;
constexpr uint32_t kFsDirentSize = 258;  // CellFsDirent: type, namlen, name[256]

uint32_t fs_open(const std::string& path, uint32_t flags, uint32_t& fd);
uint32_t fs_close(uint32_t fd);
uint32_t fs_read(uint32_t fd, void* dst, uint64_t size, uint64_t& done);
uint32_t fs_write(uint32_t fd, const void* src, uint64_t size, uint64_t& done);
uint32_t fs_lseek(uint32_t fd, int64_t offset, uint32_t whence, uint64_t& pos);
uint32_t fs_read_at(uint32_t fd, uint64_t offset, void* dst, uint64_t size, uint64_t& done);  // keeps the position
uint32_t fs_fsync(uint32_t fd);
void fs_log_reads(bool on);  // --fs-reads: every read, with its position and size
uint32_t fs_stat(const std::string& path, FsStat& st);
uint32_t fs_fstat(uint32_t fd, FsStat& st);
uint32_t fs_opendir(const std::string& path, uint32_t& fd);
// Next entry; false at the end. type: 1 directory, 2 regular file.
uint32_t fs_readdir(uint32_t fd, std::string& name, uint8_t& type, FsStat* st, bool& got);
uint32_t fs_closedir(uint32_t fd);
uint32_t fs_mkdir(const std::string& path);
uint32_t fs_rmdir(const std::string& path);
uint32_t fs_unlink(const std::string& path);

}  // namespace sly4
