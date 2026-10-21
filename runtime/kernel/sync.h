// LV2 synchronisation objects on host primitives: mutex (also lwmutex), condition
// variable (also lwcond), rwlock, semaphore, event flag, event queue and port.
//
// Every object lives in the IdManager under the id the guest sees. Priorities and
// the FIFO/priority protocols are not modelled: the host scheduler decides who wakes.
// Methods return the CELL code for r3; timeouts are in microseconds, 0 = forever.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace sly4 {

struct KernelObject {
    virtual ~KernelObject() = default;
    std::string name;
};

class IdManager {
public:
    // Ids are `base | n` with n counting from 1, like RPCS3's per-type id ranges.
    uint32_t add(std::shared_ptr<KernelObject> obj, uint32_t base);
    template <class T>
    std::shared_ptr<T> get(uint32_t id) {
        std::lock_guard<std::mutex> g(lock_);
        auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : std::dynamic_pointer_cast<T>(it->second);
    }
    bool remove(uint32_t id);

private:
    std::mutex lock_;
    std::map<uint32_t, std::shared_ptr<KernelObject>> objects_;
    std::map<uint32_t, uint32_t> next_;  // base -> next n
};
IdManager& idm();

namespace idbase {
constexpr uint32_t kMutex = 0x85000000, kCond = 0x86000000, kRwlock = 0x88000000, kEventQueue = 0x8D000000,
                   kSemaphore = 0x96000000, kLwmutex = 0x95000000, kLwcond = 0x97000000, kEventFlag = 0x98000000,
                   kEventPort = 0x0E000000, kTimer = 0x11000000;
}

// Waits on cv until pred() or the timeout; false on timeout.
template <class Pred>
bool wait_for(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint64_t timeout_us, Pred pred) {
    if (!timeout_us) {
        cv.wait(lk, pred);
        return true;
    }
    return cv.wait_for(lk, std::chrono::microseconds(timeout_us), pred);
}

struct Mutex : KernelObject {
    std::mutex m;
    std::condition_variable cv;  // signalled when the mutex becomes free
    uint64_t owner = 0;
    uint32_t count = 0;  // recursion depth
    bool recursive = false;
    uint32_t cond_users = 0;

    uint32_t lock(uint64_t tid, uint64_t timeout_us);
    uint32_t trylock(uint64_t tid);
    uint32_t unlock(uint64_t tid);
};

struct Cond : KernelObject {
    std::shared_ptr<Mutex> mutex;
    std::condition_variable cv;
    uint32_t waiters = 0, signals = 0;

    // Releases the mutex entirely, waits for a signal, takes the mutex back.
    uint32_t wait(uint64_t tid, uint64_t timeout_us);
    uint32_t signal(bool all);
};

struct Rwlock : KernelObject {
    std::mutex m;
    std::condition_variable cv;
    uint32_t readers = 0;
    uint64_t writer = 0;

    uint32_t rlock(uint64_t timeout_us, bool try_only);
    uint32_t runlock();
    uint32_t wlock(uint64_t tid, uint64_t timeout_us, bool try_only);
    uint32_t wunlock(uint64_t tid);
};

struct Semaphore : KernelObject {
    std::mutex m;
    std::condition_variable cv;
    int32_t value = 0, max = 0;

    uint32_t wait(uint64_t timeout_us, bool try_only);
    uint32_t post(int32_t count);
};

namespace event_flag {
constexpr uint32_t kWaitAnd = 0x01, kWaitOr = 0x02, kClear = 0x10, kClearAll = 0x20;
}
struct EventFlag : KernelObject {
    std::mutex m;
    std::condition_variable cv;
    uint64_t bits = 0;

    // result: the bit pattern when the condition held (before clearing).
    uint32_t wait(uint64_t pattern, uint32_t mode, uint64_t& result, uint64_t timeout_us, bool try_only);
    void set(uint64_t pattern);
    void clear(uint64_t pattern);  // bits &= pattern, as LV2 does
};

struct Event {
    uint64_t source, data1, data2, data3;
};
struct EventQueue : KernelObject {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Event> events;
    uint32_t size = 127;
    uint64_t key = 0;
    bool destroyed = false;

    uint32_t send(const Event& e);  // CELL_EBUSY when full
    uint32_t receive(Event& e, uint64_t timeout_us);
};

struct EventPort : KernelObject {
    uint64_t name_value = 0;  // event source; 0 = derived from the id
    uint32_t id = 0;
    std::weak_ptr<EventQueue> queue;
};

}  // namespace sly4
