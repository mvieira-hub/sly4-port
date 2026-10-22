#include "check.h"

#include "core/inflate.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "cpu/ppu_rt.h"
#include "kernel/guest_alloc.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "kernel/vfs.h"
#include "loader/elf_loader.h"
#include "loader/nid.h"

using namespace sly4;

TEST(guest_alloc_first_fit_and_merge) {
    GuestMemory mem;
    CHECK(mem.reserve());
    GuestAllocator a(mem, 0x20000000, 0x100000, "teste");
    const GuestAddr x = a.alloc(0x100, 0x100);
    const GuestAddr y = a.alloc(0x1000, 0x1000);
    const GuestAddr z = a.alloc(0x10);
    CHECK_EQ(x, 0x20000000u);
    CHECK_EQ(y, 0x20001000u);           // aligned past x
    CHECK_EQ(z, 0x20000100u);           // first fit: the gap after x
    CHECK(mem.is_committed(y, 0x1000));  // committed and zeroed
    CHECK_EQ(mem.read<uint32_t>(y), 0u);
    CHECK_EQ(a.size_of(y), 0x1000u);
    CHECK_EQ(a.used(), 0x1110u);
    CHECK(a.free(x));
    CHECK(!a.free(x));  // double free is refused
    CHECK(a.free(z));
    CHECK_EQ(a.alloc(0x1000, 0x10), 0x20000000u);  // x, z and the gap merged back
    CHECK_EQ(a.alloc(0x100000), 0u);               // does not fit any more
    const GuestAddr r = a.alloc(0x2000, 0x1000, false);
    CHECK(r != 0);
    CHECK(!mem.is_committed(r, 1));  // reserved only
}

namespace {
void answer_42(PPUContext& c, GuestMemory&) { c.gpr[3] = 42; }
HleTable* g_table = nullptr;
void on_hle(PPUContext& c, GuestMemory& m, uint32_t target) { g_table->call(c, m, target); }
}  // namespace

// The import stub of a PS3 game (as in the EBOOT's .lib.stub) reaches the native
// function through the descriptor written into its slot, and returns to LR.
TEST(hle_import_stub_reaches_native_function) {
    GuestMemory mem;
    CHECK(mem.reserve());
    CHECK(mem.commit(0x10000, 0x10000, Protection::ReadWrite));
    GuestAllocator area(mem, 0x30000, 0x10000, "hle");

    LoadedImage img;
    ImportedModule mod;
    mod.name = "testmod";
    mod.functions = {{nid_of("answer"), 0x18000}, {nid_of("missing"), 0x18004}};
    img.imports = {mod};

    HleTable table;
    table.add({"testmod", {{"answer", answer_42}}});
    NidNames names;
    names.add("missing");
    CHECK(table.bind(mem, area, img, names));
    g_table = &table;
    rt::Hooks& h = rt::hooks();
    h.hle = on_hle;
    h.hle_base = table.thunk_base();
    h.hle_size = table.thunk_bytes();

    // Stub for slot S: lis r12,S@ha; lwz r12,S@l(r12); std r2,40(r1); lwz r0,0(r12);
    // lwz r2,4(r12); mtctr r0; bctr
    auto stub = [&](GuestAddr at, uint32_t slot) {
        const uint32_t words[] = {0x3D800000 | ((slot + 0x8000) >> 16), 0x818C0000 | (slot & 0xFFFF), 0xF8410028,
                                  0x800C0000, 0x804C0004, 0x7C0903A6, 0x4E800420};
        for (uint32_t i = 0; i < 7; ++i) mem.write<uint32_t>(at + 4 * i, words[i]);
    };
    stub(0x10000, 0x18000);
    stub(0x10100, 0x18004);

    PPUContext c;
    c.gpr[1] = 0x1F000;
    c.lr = 0x100;
    c.gpr[3] = 7;
    rt::interpret_until_return(c, mem, 0x10000);
    CHECK_EQ(c.gpr[3], 42u);
    c.gpr[3] = 7;
    rt::interpret_until_return(c, mem, 0x10100);
    CHECK_EQ(c.gpr[3], 0u);  // not implemented: CELL_OK
    auto st = table.stats();
    CHECK_EQ(st.size(), size_t(2));
    CHECK(st[0].implemented && st[0].calls == 1);
    CHECK(!st[1].implemented && st[1].name == "missing");

    h.hle = nullptr;
    h.hle_base = h.hle_size = 0;
}

// LV2 synchronisation objects with real host threads.
TEST(sync_mutex_cond_semantics) {
    auto mx = std::make_shared<Mutex>();
    CHECK_EQ(mx->lock(1, 0), CELL_OK);
    CHECK_EQ(mx->lock(1, 0), CELL_EDEADLK);  // not recursive
    CHECK_EQ(mx->trylock(2), CELL_EBUSY);
    CHECK_EQ(mx->unlock(2), CELL_EPERM);
    CHECK_EQ(mx->lock(2, 1000), CELL_ETIMEDOUT);
    CHECK_EQ(mx->unlock(1), CELL_OK);
    mx->recursive = true;
    CHECK_EQ(mx->lock(1, 0), CELL_OK);
    CHECK_EQ(mx->lock(1, 0), CELL_OK);
    CHECK_EQ(mx->count, 2u);

    // A waiter gives the (recursively held) mutex away and gets it back with its depth.
    Cond cv;
    cv.mutex = mx;
    std::atomic<int> stage{0};
    std::thread other([&] {
        while (stage.load() == 0) std::this_thread::yield();
        CHECK_EQ(mx->lock(2, 0), CELL_OK);  // possible only while thread 1 waits
        stage = 2;
        cv.signal(false);
        CHECK_EQ(mx->unlock(2), CELL_OK);
    });
    stage = 1;
    CHECK_EQ(cv.wait(1, 0), CELL_OK);
    CHECK_EQ(stage.load(), 2);
    CHECK_EQ(mx->owner, 1u);
    CHECK_EQ(mx->count, 2u);
    other.join();
    CHECK_EQ(cv.wait(1, 1000), CELL_ETIMEDOUT);  // nobody signals
    CHECK_EQ(mx->owner, 1u);
}

TEST(sync_event_queue_flag_semaphore) {
    EventQueue q;
    q.size = 2;
    CHECK_EQ(q.send({1, 2, 3, 4}), CELL_OK);
    CHECK_EQ(q.send({5, 6, 7, 8}), CELL_OK);
    CHECK_EQ(q.send({9, 9, 9, 9}), CELL_EBUSY);  // full
    Event e{};
    CHECK_EQ(q.receive(e, 0), CELL_OK);
    CHECK_EQ(e.source, 1u);
    CHECK_EQ(e.data3, 4u);
    CHECK_EQ(q.receive(e, 0), CELL_OK);
    CHECK_EQ(e.data1, 6u);
    CHECK_EQ(q.receive(e, 1000), CELL_ETIMEDOUT);
    std::thread sender([&] { q.send({10, 0, 0, 0}); });
    CHECK_EQ(q.receive(e, 0), CELL_OK);  // blocks until the other thread sends
    CHECK_EQ(e.source, 10u);
    sender.join();

    EventFlag f;
    uint64_t result = 0;
    CHECK_EQ(f.wait(0x3, event_flag::kWaitAnd, result, 0, true), CELL_EBUSY);
    f.set(0x1);
    CHECK_EQ(f.wait(0x3, event_flag::kWaitOr | event_flag::kClear, result, 0, true), CELL_OK);
    CHECK_EQ(result, 0x1u);
    CHECK_EQ(f.bits, 0u);
    std::thread setter([&] { f.set(0x6); });
    CHECK_EQ(f.wait(0x6, event_flag::kWaitAnd, result, 0, false), CELL_OK);
    setter.join();
    f.clear(0x2);  // bits &= 0x2
    CHECK_EQ(f.bits, 0x2u);

    Semaphore s;
    s.value = 1;
    s.max = 2;
    CHECK_EQ(s.wait(0, true), CELL_OK);
    CHECK_EQ(s.wait(0, true), CELL_EBUSY);
    CHECK_EQ(s.post(3), CELL_EBUSY);  // above max
    CHECK_EQ(s.post(2), CELL_OK);
    CHECK_EQ(s.value, 2);
}

// VFS paths and the file API on a temporary folder laid out like a dump.
TEST(vfs_paths_and_files) {
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / "sly4_vfs_test";
    std::error_code ec;
    fsys::remove_all(root, ec);
    fsys::create_directories(root / "dump" / "PS3_GAME" / "USRDIR" / "sub", ec);
    {
        std::ofstream f(root / "dump" / "PS3_GAME" / "USRDIR" / "data.bin", std::ios::binary);
        f << "0123456789";
    }
    Vfs& v = vfs();
    v.setup(root / "dump" / "PS3_GAME", root / "data");

    fsys::path host;
    CHECK(v.resolve("/dev_bdvd/PS3_GAME/USRDIR/data.bin", host));
    CHECK(host == root / "dump" / "PS3_GAME" / "USRDIR" / "data.bin");
    CHECK(v.resolve("/app_home//./sub/../data.bin", host));  // same file through /app_home
    CHECK(host == root / "dump" / "PS3_GAME" / "USRDIR" / "data.bin");
    CHECK(v.resolve("/DEV_HDD0/game", host));                // device names ignore case
    CHECK(host == root / "data" / "hdd0" / "game");
    CHECK(!v.resolve("/dev_bdvd/../etc", host));             // cannot climb out of a device
    CHECK(!v.resolve("/dev_flash/x", host));                 // not mounted

    uint32_t fd = 0;
    CHECK_EQ(fs_open("/app_home/data.bin", fs_flag::kRdonly, fd), CELL_OK);
    char buf[16] = {};
    uint64_t done = 0, pos = 0;
    CHECK_EQ(fs_lseek(fd, 4, 0, pos), CELL_OK);
    CHECK_EQ(fs_read(fd, buf, 16, done), CELL_OK);
    CHECK_EQ(done, uint64_t(6));
    CHECK(std::string(buf) == "456789");
    CHECK_EQ(fs_read_at(fd, 1, buf, 2, done), CELL_OK);  // does not move the position
    CHECK(buf[0] == '1' && buf[1] == '2');
    CHECK_EQ(fs_read(fd, buf, 4, done), CELL_OK);
    CHECK_EQ(done, uint64_t(0));  // still at the end
    FsStat st;
    CHECK_EQ(fs_fstat(fd, st), CELL_OK);
    CHECK_EQ(st.size, uint64_t(10));
    CHECK_EQ(st.mode, 0100666);
    CHECK_EQ(fs_close(fd), CELL_OK);
    CHECK_EQ(fs_close(fd), 0x8001002Au);  // EBADF
    CHECK_EQ(fs_open("/app_home/missing", fs_flag::kRdonly, fd), CELL_ENOENT);

    CHECK_EQ(fs_mkdir("/dev_hdd0/game/TEST"), CELL_OK);
    CHECK_EQ(fs_mkdir("/dev_hdd0/game/TEST"), CELL_EEXIST);
    CHECK_EQ(fs_open("/dev_hdd0/game/TEST/save.dat", fs_flag::kWronly | fs_flag::kCreat | fs_flag::kTrunc, fd), CELL_OK);
    CHECK_EQ(fs_write(fd, "abc", 3, done), CELL_OK);
    CHECK_EQ(fs_close(fd), CELL_OK);
    CHECK_EQ(fs_stat("/dev_hdd0/game/TEST/save.dat", st), CELL_OK);
    CHECK_EQ(st.size, uint64_t(3));

    CHECK_EQ(fs_opendir("/dev_hdd0/game/TEST", fd), CELL_OK);
    std::vector<std::string> names;
    for (;;) {
        std::string name;
        uint8_t type = 0;
        bool got = false;
        CHECK_EQ(fs_readdir(fd, name, type, nullptr, got), CELL_OK);
        if (!got) break;
        names.push_back(name + (type == 1 ? "/" : ""));
    }
    CHECK_EQ(fs_closedir(fd), CELL_OK);
    CHECK_EQ(names.size(), size_t(3));
    CHECK(names.size() == 3 && names[0] == "./" && names[1] == "../" && names[2] == "save.dat");
    CHECK_EQ(fs_rmdir("/dev_hdd0/game/TEST"), 0x80010036u);  // ENOTEMPTY
    CHECK_EQ(fs_unlink("/dev_hdd0/game/TEST/save.dat"), CELL_OK);
    CHECK_EQ(fs_rmdir("/dev_hdd0/game/TEST"), CELL_OK);
    fsys::remove_all(root, ec);
}

// signal_all must wake every thread that was waiting, even if one of them wakes first
// and waits again at once (it must not take the signal of the one that has not run yet).
// Unlocking a mutex someone waits for gives it to that thread: the old owner cannot
// take it back at once (the Bink sound thread starved the render thread that way).
TEST(sync_mutex_hands_off_to_waiter) {
    auto mx = std::make_shared<Mutex>();
    CHECK_EQ(mx->lock(1, 0), CELL_OK);
    std::atomic<bool> got{false};
    std::thread other([&] {
        CHECK_EQ(mx->lock(2, 0), CELL_OK);
        got = true;
        CHECK_EQ(mx->unlock(2), CELL_OK);
    });
    for (;;) {
        {
            std::lock_guard<std::mutex> g(mx->m);
            if (!mx->queue.empty()) break;
        }
        std::this_thread::yield();
    }
    CHECK_EQ(mx->unlock(1), CELL_OK);
    CHECK_EQ(mx->trylock(1), CELL_EBUSY);  // already thread 2's, even before it runs
    other.join();
    CHECK(got.load());
    CHECK_EQ(mx->trylock(1), CELL_OK);
}

TEST(sync_cond_signal_all_is_not_stolen) {
    auto mx = std::make_shared<Mutex>();
    Cond cv;
    cv.mutex = mx;
    std::atomic<uint32_t> b_result{0xFFFFFFFF}, a_second{0xFFFFFFFF};
    std::thread b([&] {
        mx->lock(2, 0);
        b_result = cv.wait(2, 2'000'000);
        mx->unlock(2);
    });
    std::thread a([&] {
        mx->lock(1, 0);
        cv.wait(1, 2'000'000);
        a_second = cv.wait(1, 200'000);  // nobody signals again: must time out
        mx->unlock(1);
    });
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mx->m);
            if (cv.waiters == 2) break;
        }
        std::this_thread::yield();
    }
    cv.signal(true);
    a.join();
    b.join();
    CHECK_EQ(b_result.load(), CELL_OK);
    CHECK_EQ(a_second.load(), CELL_ETIMEDOUT);
}

// Raw DEFLATE: a stored block, and a fixed-Huffman block with a back reference
// ("abc" then length 9 at distance 3), encoded by hand from RFC 1951.
TEST(inflate_raw_blocks) {
    uint8_t out[32];
    size_t n = 0, used = 0;
    const uint8_t stored[] = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    CHECK(inflate_raw(stored, sizeof(stored), out, sizeof(out), n, used) == InflateResult::Ok);
    CHECK(n == 5 && std::memcmp(out, "hello", 5) == 0);
    CHECK_EQ(used, sizeof(stored));

    const uint8_t fixed[] = {0x4B, 0x4C, 0x4A, 0x86, 0x23, 0x00};
    CHECK(inflate_raw(fixed, sizeof(fixed), out, sizeof(out), n, used) == InflateResult::Ok);
    CHECK(n == 12 && std::memcmp(out, "abcabcabcabc", 12) == 0);
    CHECK_EQ(used, sizeof(fixed));

    CHECK(inflate_raw(fixed, sizeof(fixed), out, 11, n, used) == InflateResult::OutputTooSmall);
    CHECK(inflate_raw(fixed, 4, out, sizeof(out), n, used) == InflateResult::InputTruncated);
    const uint8_t reserved[] = {0x07};  // BTYPE 3 does not exist
    CHECK(inflate_raw(reserved, 1, out, sizeof(out), n, used) == InflateResult::BadData);
}
