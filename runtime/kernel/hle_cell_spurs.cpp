// cellSpurs, the part the PPU sees of the SPU task system: SPURS event flags and
// lock-free queues (LFQueue) as host objects, and tasks. There is no SPU yet (phase 4):
// a task whose SPU program is known is replaced by native code (kernel/spurs_tasks.h);
// other tasks are only logged.
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/spurs.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }
void ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

constexpr uint32_t CELL_SPURS_TASK_ERROR_AGAIN = 0x80410901, CELL_SPURS_TASK_ERROR_INVAL = 0x80410902,
                   CELL_SPURS_TASK_ERROR_SRCH = 0x80410905, CELL_SPURS_TASK_ERROR_BUSY = 0x8041090A;

template <class T>
struct Registry {
    std::mutex lock;
    std::map<uint32_t, std::shared_ptr<T>> objects;
    std::shared_ptr<T> create(uint32_t addr) {
        std::lock_guard<std::mutex> g(lock);
        return objects[addr] = std::make_shared<T>();
    }
    std::shared_ptr<T> find(uint32_t addr) {
        std::lock_guard<std::mutex> g(lock);
        auto it = objects.find(addr);
        return it == objects.end() ? nullptr : it->second;
    }
};
Registry<SpursEventFlag> g_flags;
Registry<SpursQueue> g_queues;

// ---------------------------------------------------------------- event flags
// _cellSpursEventFlagInitialize(spurs, taskset, CellSpursEventFlag*, clearMode, direction)
void cellSpursEventFlagInitialize(PPUContext& c, GuestMemory&) {
    auto f = g_flags.create(a32(c, 5));
    f->auto_clear = a32(c, 6) == 0;  // CELL_SPURS_EVENT_FLAG_CLEAR_AUTO
    c.gpr[3] = CELL_OK;
}
void cellSpursEventFlagClear(PPUContext& c, GuestMemory&) {
    auto f = g_flags.find(a32(c, 3));
    if (!f) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_SRCH;
        return;
    }
    std::lock_guard<std::mutex> lk(f->m);
    f->bits &= uint16_t(~c.gpr[4]);
    c.gpr[3] = CELL_OK;
}
void cellSpursEventFlagSet(PPUContext& c, GuestMemory&) {
    auto f = g_flags.find(a32(c, 3));
    if (!f) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_SRCH;
        return;
    }
    f->set(uint16_t(c.gpr[4]));
    c.gpr[3] = CELL_OK;
}
// cellSpursEventFlagWait(ef, u16* mask, mode): mode 0 = OR, 1 = AND; *mask gets the
// bits that satisfied the wait, which are cleared in auto-clear mode.
void event_flag_wait(PPUContext& c, GuestMemory& m, bool try_only) {
    auto f = g_flags.find(a32(c, 3));
    if (!f) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_SRCH;
        return;
    }
    const uint16_t mask = m.read<uint16_t>(a32(c, 4));
    const bool all = a32(c, 5) == 1;
    std::unique_lock<std::mutex> lk(f->m);
    auto met = [&] { return all ? (f->bits & mask) == mask : (f->bits & mask) != 0; };
    if (try_only && !met()) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_BUSY;
        return;
    }
    f->cv.wait(lk, met);
    const uint16_t got = f->bits & mask;
    if (f->auto_clear) f->bits &= uint16_t(~got);
    m.write<uint16_t>(a32(c, 4), got);
    c.gpr[3] = CELL_OK;
}
void cellSpursEventFlagWait(PPUContext& c, GuestMemory& m) { event_flag_wait(c, m, false); }
void cellSpursEventFlagTryWait(PPUContext& c, GuestMemory& m) { event_flag_wait(c, m, true); }

// ---------------------------------------------------------------- LFQueue
// _cellSpursLFQueueInitialize(taskset or spurs, CellSpursLFQueue*, buffer, size, depth, direction)
void cellSpursLFQueueInitialize(PPUContext& c, GuestMemory&) {
    const uint32_t size = a32(c, 6), depth = a32(c, 7);
    if (!size || !depth || size % 16) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_INVAL;
        return;
    }
    auto q = g_queues.create(a32(c, 4));
    q->size = size;
    q->depth = depth;
    c.gpr[3] = CELL_OK;
}
// _cellSpursLFQueuePushBody(queue, const void* buffer, isBlocking)
void cellSpursLFQueuePushBody(PPUContext& c, GuestMemory& m) {
    auto q = g_queues.find(a32(c, 3));
    if (!q) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_SRCH;
        return;
    }
    std::vector<uint8_t> e(q->size);
    m.copy_from(e.data(), a32(c, 4), q->size);
    if (call_log().trace) {
        std::string hex;
        char b[4];
        for (uint8_t x : e) std::snprintf(b, sizeof(b), "%02X", x), hex += b;
        SLY4_INFO("SPURS: LFQueue 0x%08X <- %s", a32(c, 3), hex.c_str());
        for (int k = 0; k < 2; ++k) {
            const uint32_t p = uint32_t(e[4 * k]) << 24 | e[4 * k + 1] << 16 | e[4 * k + 2] << 8 | e[4 * k + 3];
            if (!m.is_committed(p, 16)) continue;
            hex.clear();
            for (int i = 0; i < 16; ++i) std::snprintf(b, sizeof(b), "%02X", m.read<uint8_t>(p + i)), hex += b;
            SLY4_INFO("SPURS:   [%d] 0x%08X: %s", k, p, hex.c_str());
        }
    }
    c.gpr[3] = q->push(std::move(e), a32(c, 5) != 0) ? CELL_OK : CELL_SPURS_TASK_ERROR_AGAIN;
}
// _cellSpursLFQueuePopBody(queue, void* buffer, isBlocking)
void cellSpursLFQueuePopBody(PPUContext& c, GuestMemory& m) {
    auto q = g_queues.find(a32(c, 3));
    if (!q) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_SRCH;
        return;
    }
    std::vector<uint8_t> e;
    if (!q->pop(e, a32(c, 5) != 0)) {
        c.gpr[3] = CELL_SPURS_TASK_ERROR_AGAIN;
        return;
    }
    m.copy_to(a32(c, 4), e.data(), e.size());
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- tasks
// cellSpursCreateTask(taskset, u32* id, elf, context, size, lsPattern, CellSpursTaskArgument*)
std::mutex g_task_lock;
uint32_t g_next_task = 0;

void cellSpursCreateTask(PPUContext& c, GuestMemory& m) {
    const uint32_t elf = a32(c, 5), arg = a32(c, 9);
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(g_task_lock);
        id = g_next_task++;
    }
    m.write<uint32_t>(a32(c, 4), id);
    uint32_t words[4] = {};
    for (int i = 0; i < 4 && arg; ++i) words[i] = m.read<uint32_t>(arg + 4 * i);
    const SpursTaskInfo info{a32(c, 3), id, elf, {words[0], words[1], words[2], words[3]}};
    const char* native = start_native_spurs_task(m, info);
    SLY4_INFO("SPURS: tarefa %u do taskset 0x%08X, ELF SPU em 0x%08X (%s), argumento %08X %08X %08X %08X", id,
              info.taskset, elf, native ? native : "sem SPU: não executada", words[0], words[1], words[2], words[3]);
    c.gpr[3] = CELL_OK;
}

}  // namespace

// ---------------------------------------------------------------- shared with native tasks
void SpursEventFlag::set(uint16_t b) {
    std::lock_guard<std::mutex> lk(m);
    bits |= b;
    cv.notify_all();
}

bool SpursQueue::push(std::vector<uint8_t> e, bool blocking) {
    std::unique_lock<std::mutex> lk(m);
    if (q.size() >= depth) {
        if (!blocking) return false;
        cv.wait(lk, [&] { return q.size() < depth; });
    }
    q.push_back(std::move(e));
    cv.notify_all();
    return true;
}

bool SpursQueue::pop(std::vector<uint8_t>& e, bool blocking) {
    std::unique_lock<std::mutex> lk(m);
    if (q.empty()) {
        if (!blocking) return false;
        cv.wait(lk, [&] { return !q.empty(); });
    }
    e = std::move(q.front());
    q.pop_front();
    cv.notify_all();
    return true;
}

std::shared_ptr<SpursEventFlag> spurs_event_flag(uint32_t addr) { return g_flags.find(addr); }
std::shared_ptr<SpursQueue> spurs_queue(uint32_t addr) { return g_queues.find(addr); }

HleModule hle_cell_spurs() {
    return {"cellSpurs",
            {
                {"_cellSpursEventFlagInitialize", cellSpursEventFlagInitialize},
                {"cellSpursEventFlagAttachLv2EventQueue", ok},
                {"cellSpursEventFlagDetachLv2EventQueue", ok},
                {"cellSpursEventFlagClear", cellSpursEventFlagClear},
                {"cellSpursEventFlagSet", cellSpursEventFlagSet},
                {"cellSpursEventFlagWait", cellSpursEventFlagWait},
                {"cellSpursEventFlagTryWait", cellSpursEventFlagTryWait},
                {"_cellSpursLFQueueInitialize", cellSpursLFQueueInitialize},
                {"cellSpursLFQueueAttachLv2EventQueue", ok},
                {"cellSpursLFQueueDetachLv2EventQueue", ok},
                {"_cellSpursLFQueuePushBody", cellSpursLFQueuePushBody},
                {"_cellSpursLFQueuePopBody", cellSpursLFQueuePopBody},
                {"cellSpursCreateTask", cellSpursCreateTask},
            }};
}

}  // namespace sly4
