// LV2 syscalls (sc with the number in r11, arguments in r3..r10, result in r3).
// Numbers and names follow RPCS3's lv2.cpp table. What is not implemented yet logs its
// first call and returns CELL_OK, so a run maps what the game needs.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <thread>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"

namespace sly4 {

namespace {

constexpr uint32_t kCount = 1024;

// ---------------------------------------------------------------- process
void sys_process_getpid(PPUContext& c, GuestMemory&) { c.gpr[3] = 0x01000500; }
void sys_process_exit_(PPUContext& c, GuestMemory&) { throw ProcessExit{int32_t(c.gpr[3])}; }
void sys_process_is_spu_lock_line_reservation_address(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

// ---------------------------------------------------------------- PPU threads
void sys_ppu_thread_yield(PPUContext& c, GuestMemory&) {
    std::this_thread::yield();
    c.gpr[3] = CELL_OK;
}
void sys_ppu_thread_join(PPUContext& c, GuestMemory& m) {
    uint64_t value = 0;
    const uint32_t r = kernel().join_thread(c.gpr[3], value);
    if (r == CELL_OK && uint32_t(c.gpr[4])) m.write<uint64_t>(uint32_t(c.gpr[4]), value);
    c.gpr[3] = r;
}
void sys_ppu_thread_detach(PPUContext& c, GuestMemory&) { c.gpr[3] = kernel().detach_thread(c.gpr[3]); }
void sys_ppu_thread_set_priority(PPUContext& c, GuestMemory&) {
    auto t = kernel().find_thread(c.gpr[3]);
    if (t) t->priority = int32_t(c.gpr[4]);
    c.gpr[3] = t ? CELL_OK : CELL_ESRCH;
}
void sys_ppu_thread_get_priority(PPUContext& c, GuestMemory& m) {
    auto t = kernel().find_thread(c.gpr[3]);
    if (t) m.write<int32_t>(uint32_t(c.gpr[4]), t->priority);
    c.gpr[3] = t ? CELL_OK : CELL_ESRCH;
}
void sys_ppu_thread_get_stack_information(PPUContext& c, GuestMemory& m) {
    PpuThread* t = current_thread();
    const uint32_t info = uint32_t(c.gpr[3]);
    m.write<uint32_t>(info, t ? t->stack_addr : 0);
    m.write<uint32_t>(info + 4, t ? t->stack_size : 0);
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- time
void sys_timer_usleep(PPUContext& c, GuestMemory&) {
    std::this_thread::sleep_for(std::chrono::microseconds(c.gpr[3]));
    c.gpr[3] = CELL_OK;
}
void sys_timer_sleep(PPUContext& c, GuestMemory&) {
    std::this_thread::sleep_for(std::chrono::seconds(uint32_t(c.gpr[3])));
    c.gpr[3] = CELL_OK;
}
void sys_time_get_timezone(PPUContext& c, GuestMemory& m) {
    m.write<int32_t>(uint32_t(c.gpr[3]), 0);  // minutes from UTC
    m.write<int32_t>(uint32_t(c.gpr[4]), 0);  // daylight saving
    c.gpr[3] = CELL_OK;
}
void sys_time_get_current_time(PPUContext& c, GuestMemory& m) {
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    m.write<int64_t>(uint32_t(c.gpr[3]), ns / 1'000'000'000);
    m.write<int64_t>(uint32_t(c.gpr[4]), ns % 1'000'000'000);
    c.gpr[3] = CELL_OK;
}
void sys_time_get_timebase_frequency(PPUContext& c, GuestMemory&) { c.gpr[3] = 79'800'000; }

// ---------------------------------------------------------------- TTY
void sys_tty_write(PPUContext& c, GuestMemory& m) {
    const uint32_t ch = uint32_t(c.gpr[3]), buf = uint32_t(c.gpr[4]), len = uint32_t(c.gpr[5]);
    std::string s(reinterpret_cast<const char*>(m.host(buf)), len);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::fprintf(stderr, "[tty%u] %s\n", ch, s.c_str());
    std::fflush(stderr);
    if (uint32_t pw = uint32_t(c.gpr[6])) m.write<uint32_t>(pw, len);
    c.gpr[3] = CELL_OK;
}

struct Entry {
    const char* name = nullptr;
    SyscallFn fn = nullptr;
};

struct Table {
    std::array<Entry, kCount> e{};
    std::array<std::atomic<uint64_t>, kCount> calls{};

    void name(uint32_t n, const char* s) { e[n].name = s; }
    void impl(uint32_t n, const char* s, SyscallFn f) { e[n] = {s, f}; }

    Table() {
        // Names of every syscall the game issues directly (and their close relatives).
        name(1, "sys_process_getpid");
        name(14, "sys_process_is_spu_lock_line_reservation_address");
        name(22, "_sys_process_exit");
        name(43, "sys_ppu_thread_yield");
        name(44, "sys_ppu_thread_join");
        name(45, "sys_ppu_thread_detach");
        name(47, "sys_ppu_thread_set_priority");
        name(48, "sys_ppu_thread_get_priority");
        name(49, "sys_ppu_thread_get_stack_information");
        name(52, "_sys_ppu_thread_create");
        name(53, "sys_ppu_thread_start");
        name(70, "sys_timer_create");
        name(71, "sys_timer_destroy");
        name(72, "sys_timer_get_information");
        name(73, "_sys_timer_start");
        name(74, "sys_timer_stop");
        name(75, "sys_timer_connect_event_queue");
        name(76, "sys_timer_disconnect_event_queue");
        name(82, "sys_event_flag_create");
        name(83, "sys_event_flag_destroy");
        name(85, "sys_event_flag_wait");
        name(86, "sys_event_flag_trywait");
        name(87, "sys_event_flag_set");
        name(90, "sys_semaphore_create");
        name(91, "sys_semaphore_destroy");
        name(92, "sys_semaphore_wait");
        name(93, "sys_semaphore_trywait");
        name(94, "sys_semaphore_post");
        name(95, "_sys_lwmutex_create");
        name(100, "sys_mutex_create");
        name(101, "sys_mutex_destroy");
        name(102, "sys_mutex_lock");
        name(103, "sys_mutex_trylock");
        name(104, "sys_mutex_unlock");
        name(105, "sys_cond_create");
        name(106, "sys_cond_destroy");
        name(107, "sys_cond_wait");
        name(108, "sys_cond_signal");
        name(118, "sys_event_flag_clear");
        name(120, "sys_rwlock_create");
        name(121, "sys_rwlock_destroy");
        name(122, "sys_rwlock_rlock");
        name(123, "sys_rwlock_tryrlock");
        name(124, "sys_rwlock_runlock");
        name(125, "sys_rwlock_wlock");
        name(126, "sys_rwlock_trywlock");
        name(127, "sys_rwlock_wunlock");
        name(128, "sys_event_queue_create");
        name(129, "sys_event_queue_destroy");
        name(130, "sys_event_queue_receive");
        name(131, "sys_event_queue_tryreceive");
        name(133, "sys_event_queue_drain");
        name(134, "sys_event_port_create");
        name(135, "sys_event_port_destroy");
        name(136, "sys_event_port_connect_local");
        name(137, "sys_event_port_disconnect");
        name(138, "sys_event_port_send");
        name(141, "sys_timer_usleep");
        name(142, "sys_timer_sleep");
        name(144, "sys_time_get_timezone");
        name(145, "sys_time_get_current_time");
        name(147, "sys_time_get_timebase_frequency");
        name(156, "sys_spu_image_open");
        name(169, "sys_spu_initialize");
        name(181, "sys_spu_thread_write_ls");
        name(182, "sys_spu_thread_read_ls");
        name(190, "sys_spu_thread_write_spu_mb");
        name(191, "sys_spu_thread_connect_event");
        name(192, "sys_spu_thread_disconnect_event");
        name(251, "sys_spu_thread_group_connect_event_all_threads");
        name(252, "sys_spu_thread_group_disconnect_event_all_threads");
        name(260, "sys_spu_image_open_by_fd");
        name(324, "sys_memory_container_create");
        name(330, "sys_mmapper_allocate_address");
        name(331, "sys_mmapper_free_address");
        name(332, "sys_mmapper_allocate_shared_memory");
        name(348, "sys_memory_allocate");
        name(349, "sys_memory_free");
        name(350, "sys_memory_allocate_from_container");
        name(351, "sys_memory_get_page_attribute");
        name(352, "sys_memory_get_user_memory_size");
        name(353, "sys_memory_get_user_memory_stat");
        name(402, "sys_tty_read");
        name(403, "sys_tty_write");
        name(801, "sys_fs_open");
        name(802, "sys_fs_read");
        name(803, "sys_fs_write");
        name(804, "sys_fs_close");
        name(805, "sys_fs_opendir");
        name(806, "sys_fs_readdir");
        name(807, "sys_fs_closedir");
        name(808, "sys_fs_stat");
        name(809, "sys_fs_fstat");
        name(811, "sys_fs_mkdir");
        name(812, "sys_fs_rename");
        name(813, "sys_fs_rmdir");
        name(817, "sys_fs_fcntl");
        name(818, "sys_fs_lseek");
        name(988, "sys_dbg_set_ppu_exception_handler");

        impl(1, "sys_process_getpid", sys_process_getpid);
        impl(14, "sys_process_is_spu_lock_line_reservation_address", sys_process_is_spu_lock_line_reservation_address);
        impl(22, "_sys_process_exit", sys_process_exit_);
        impl(43, "sys_ppu_thread_yield", sys_ppu_thread_yield);
        impl(44, "sys_ppu_thread_join", sys_ppu_thread_join);
        impl(45, "sys_ppu_thread_detach", sys_ppu_thread_detach);
        impl(47, "sys_ppu_thread_set_priority", sys_ppu_thread_set_priority);
        impl(48, "sys_ppu_thread_get_priority", sys_ppu_thread_get_priority);
        impl(49, "sys_ppu_thread_get_stack_information", sys_ppu_thread_get_stack_information);
        impl(141, "sys_timer_usleep", sys_timer_usleep);
        impl(142, "sys_timer_sleep", sys_timer_sleep);
        impl(144, "sys_time_get_timezone", sys_time_get_timezone);
        impl(145, "sys_time_get_current_time", sys_time_get_current_time);
        impl(147, "sys_time_get_timebase_frequency", sys_time_get_timebase_frequency);
        impl(403, "sys_tty_write", sys_tty_write);
    }
};

Table& table() {
    static Table t;
    return t;
}

}  // namespace

const char* syscall_name(uint32_t n) { return n < kCount ? table().e[n].name : nullptr; }

void register_syscall(uint32_t n, const char* name, SyscallFn fn) {
    if (n < kCount) table().impl(n, name, fn);
}

void syscall_dispatch(PPUContext& c, GuestMemory& m) {
    const uint64_t n = c.gpr[11];
    Table& t = table();
    const Entry* e = n < kCount ? &t.e[n] : nullptr;
    if (n < kCount) t.calls[n].fetch_add(1, std::memory_order_relaxed);
    if (PpuThread* th = current_thread()) {
        th->last_call = e && e->name ? e->name : "syscall ?";
        th->last_lr = c.cia - 4;
        th->last_r3 = c.gpr[3];
        th->last_r4 = c.gpr[4];
        call_log().ring(th->name.c_str(), th->last_call.load(), c.gpr[3], c.gpr[4], c.cia - 4);
    }
    CallLog& log = call_log();
    if (log.first_call(0x10000 + uint32_t(std::min<uint64_t>(n, kCount))) || log.trace) {
        char line[320];
        const PpuThread* th = current_thread();
        std::snprintf(line, sizeof(line),
                      "{%s} sc %" PRIu64 " %s(0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64 ") em 0x%08X%s",
                      th ? th->name.c_str() : "?", n, e && e->name ? e->name : "?", c.gpr[3], c.gpr[4], c.gpr[5],
                      c.gpr[6], c.cia - 4,
                      e && e->fn ? "" : "  [sem implementação: CELL_OK]");
        log.record(line);
    }
    if (e && e->fn)
        e->fn(c, m);
    else
        c.gpr[3] = CELL_OK;
}

std::vector<SyscallStat> syscall_stats() {
    std::vector<SyscallStat> out;
    Table& t = table();
    for (uint32_t n = 0; n < kCount; ++n)
        if (uint64_t k = t.calls[n].load()) out.push_back({n, k, t.e[n].fn != nullptr});
    return out;
}

}  // namespace sly4
