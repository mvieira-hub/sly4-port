// LV2 SPU thread groups (sys_spu_thread_group_* / sys_spu_thread_*, docs/SPU.md 4d).
// Each SPU thread runs on its own host thread with the SPU interpreter; the SPU talks to
// the PPU through mailboxes, signal notification, event ports and "stop" signals, as
// RPCS3's SPUThread.cpp / sys_spu.cpp do.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cpu/spu_channels.h"
#include "cpu/spu_context.h"
#include "kernel/sync.h"

namespace sly4 {

struct SpuThreadGroup;

// Stop signals of the LV2 SPU thread ABI.
namespace spu_stop {
constexpr uint32_t Yield = 0x100, GroupExit = 0x101, ThreadExit = 0x102, ReceiveEvent = 0x110,
                   TryReceiveEvent = 0x111;
}

class SpuThread : public KernelObject, public SpuIo {
public:
    uint32_t id = 0;
    uint32_t index = 0;  // slot in the group
    SpuThreadGroup* group = nullptr;
    GuestAddr image = 0;  // sys_spu_image the group start deploys
    uint64_t args[4] = {};

    std::unique_ptr<SpuLocalStore> ls = std::make_unique<SpuLocalStore>();
    SPUContext ctx{};

    std::mutex m;
    std::condition_variable cv;
    std::deque<uint32_t> in_mbox;  // PPU -> SPU, up to 4
    std::deque<uint32_t> out_mbox;  // SPU -> PPU (stop protocols), 1
    uint32_t snr[2] = {};
    bool snr_valid[2] = {};
    bool snr_or[2] = {};  // OR mode instead of overwrite (sys_spu_thread_set_spu_cfg)
    std::array<std::weak_ptr<EventQueue>, 64> ports;  // SPU port -> event queue (send_event)
    std::vector<std::pair<uint32_t, std::weak_ptr<EventQueue>>> bound;  // spuq -> queue (receive_event)

    std::atomic<bool> running{false}, stop_requested{false};
    std::atomic<uint64_t> executed{0};  // SPU instructions run (for the summary)
    uint32_t exit_status = 0;

    // SpuIo: mailboxes, signals, events.
    SpuChannelResult read(SPUContext& c, uint32_t ch, uint32_t& value) override;
    SpuChannelResult write(SPUContext& c, uint32_t ch, uint32_t value) override;
    uint32_t count(SPUContext& c, uint32_t ch) override;
    uint32_t events(SPUContext& c) override;

    void push_in_mbox(uint32_t v);
    void write_snr(int n, uint32_t v);

    // Host thread body: runs until the thread or its group stops.
    void run();

private:
    bool handle_stop(uint32_t code);  // false: the thread is done
    bool pop_out_mbox(uint32_t& v);
    uint32_t pending_events();
};

struct SpuThreadGroup : KernelObject {
    uint32_t id = 0;
    int32_t priority = 0;
    uint32_t num = 0;
    std::vector<std::shared_ptr<SpuThread>> threads;

    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    uint32_t running = 0;
    uint32_t join_cause = 0, exit_status = 0;
    std::weak_ptr<EventQueue> run_queue;  // SYS_SPU_THREAD_GROUP_EVENT_RUN

    void thread_finished();
};

// sys_spu_image from an SPU ELF already in guest memory: writes the image struct at
// `img` and a segment table in main memory. Returns a CELL code.
uint32_t spu_image_import(GuestMemory& m, GuestAddr img, GuestAddr elf);

// Every SPU thread created so far, for the end-of-run summary.
std::vector<std::shared_ptr<SpuThread>> spu_threads();
void add_spu_thread(const std::shared_ptr<SpuThread>& t);

// Development aid (--spu-profile): samples every SPU's PC and prints where they spend
// their time with the summary.
void spu_profile_start();
void spu_profile_print();

}  // namespace sly4
