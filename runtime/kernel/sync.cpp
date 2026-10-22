#include "kernel/sync.h"

#include "kernel/kernel.h"

namespace sly4 {

uint32_t IdManager::add(std::shared_ptr<KernelObject> obj, uint32_t base) {
    std::lock_guard<std::mutex> g(lock_);
    uint32_t& n = next_[base];
    const uint32_t id = base | ++n;
    objects_.emplace(id, std::move(obj));
    return id;
}

bool IdManager::remove(uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    return objects_.erase(id) != 0;
}

IdManager& idm() {
    static IdManager m;
    return m;
}

// ---------------------------------------------------------------- mutex
uint32_t Mutex::lock(uint64_t tid, uint64_t timeout_us) {
    std::unique_lock<std::mutex> lk(m);
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        ++count;
        return CELL_OK;
    }
    if (!wait_for(cv, lk, timeout_us, [&] { return owner == 0; })) return CELL_ETIMEDOUT;
    owner = tid;
    count = 1;
    return CELL_OK;
}

uint32_t Mutex::trylock(uint64_t tid) {
    std::lock_guard<std::mutex> lk(m);
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        ++count;
        return CELL_OK;
    }
    if (owner) return CELL_EBUSY;
    owner = tid;
    count = 1;
    return CELL_OK;
}

uint32_t Mutex::unlock(uint64_t tid) {
    std::lock_guard<std::mutex> lk(m);
    if (owner != tid) return CELL_EPERM;
    if (--count == 0) {
        owner = 0;
        cv.notify_one();
    }
    return CELL_OK;
}

// ---------------------------------------------------------------- condition variable
uint32_t Cond::wait(uint64_t tid, uint64_t timeout_us) {
    Mutex& mx = *mutex;
    std::unique_lock<std::mutex> lk(mx.m);
    if (mx.owner != tid) return CELL_EPERM;
    const uint32_t saved = mx.count;
    mx.owner = 0;
    mx.count = 0;
    mx.cv.notify_one();
    ++waiters;
    const uint64_t ticket = ++issued;
    bool signalled = wait_for(cv, lk, timeout_us, [&] { return released >= ticket; });
    if (!signalled) {
        signalled = released >= ticket;  // released just as the wait timed out
        if (!signalled) abandoned.insert(ticket);
    }
    --waiters;
    mx.cv.wait(lk, [&] { return mx.owner == 0; });
    mx.owner = tid;
    mx.count = saved;
    return signalled ? CELL_OK : CELL_ETIMEDOUT;
}

uint32_t Cond::signal(bool all) {
    std::lock_guard<std::mutex> lk(mutex->m);
    if (all) {
        released = issued;
        abandoned.clear();
    } else {
        // The oldest ticket still waiting; tickets of timed-out waits are skipped.
        while (released < issued && abandoned.erase(++released)) {
        }
    }
    cv.notify_all();  // each waiter checks its own ticket
    return CELL_OK;
}

// ---------------------------------------------------------------- rwlock
uint32_t Rwlock::rlock(uint64_t timeout_us, bool try_only) {
    std::unique_lock<std::mutex> lk(m);
    auto free = [&] { return writer == 0; };
    if (try_only) {
        if (!free()) return CELL_EBUSY;
    } else if (!wait_for(cv, lk, timeout_us, free)) {
        return CELL_ETIMEDOUT;
    }
    ++readers;
    return CELL_OK;
}

uint32_t Rwlock::runlock() {
    std::lock_guard<std::mutex> lk(m);
    if (!readers) return CELL_EPERM;
    if (--readers == 0) cv.notify_all();
    return CELL_OK;
}

uint32_t Rwlock::wlock(uint64_t tid, uint64_t timeout_us, bool try_only) {
    std::unique_lock<std::mutex> lk(m);
    if (writer == tid) return CELL_EDEADLK;
    auto free = [&] { return writer == 0 && readers == 0; };
    if (try_only) {
        if (!free()) return CELL_EBUSY;
    } else if (!wait_for(cv, lk, timeout_us, free)) {
        return CELL_ETIMEDOUT;
    }
    writer = tid;
    return CELL_OK;
}

uint32_t Rwlock::wunlock(uint64_t tid) {
    std::lock_guard<std::mutex> lk(m);
    if (writer != tid) return CELL_EPERM;
    writer = 0;
    cv.notify_all();
    return CELL_OK;
}

// ---------------------------------------------------------------- semaphore
uint32_t Semaphore::wait(uint64_t timeout_us, bool try_only) {
    std::unique_lock<std::mutex> lk(m);
    auto ready = [&] { return value > 0; };
    if (try_only) {
        if (!ready()) return CELL_EBUSY;
    } else if (!wait_for(cv, lk, timeout_us, ready)) {
        return CELL_ETIMEDOUT;
    }
    --value;
    return CELL_OK;
}

uint32_t Semaphore::post(int32_t n) {
    std::lock_guard<std::mutex> lk(m);
    if (n < 0) return CELL_EINVAL;
    if (int64_t(value) + n > max) return CELL_EBUSY;
    value += n;
    cv.notify_all();
    return CELL_OK;
}

// ---------------------------------------------------------------- event flag
uint32_t EventFlag::wait(uint64_t pattern, uint32_t mode, uint64_t& result, uint64_t timeout_us, bool try_only) {
    std::unique_lock<std::mutex> lk(m);
    auto met = [&] { return mode & event_flag::kWaitAnd ? (bits & pattern) == pattern : (bits & pattern) != 0; };
    if (try_only) {
        if (!met()) return CELL_EBUSY;
    } else if (!wait_for(cv, lk, timeout_us, met)) {
        result = bits;
        return CELL_ETIMEDOUT;
    }
    result = bits;
    if (mode & event_flag::kClearAll)
        bits = 0;
    else if (mode & event_flag::kClear)
        bits &= ~pattern;
    return CELL_OK;
}

void EventFlag::set(uint64_t pattern) {
    std::lock_guard<std::mutex> lk(m);
    bits |= pattern;
    cv.notify_all();
}

void EventFlag::clear(uint64_t pattern) {
    std::lock_guard<std::mutex> lk(m);
    bits &= pattern;
}

// ---------------------------------------------------------------- event queue
uint32_t EventQueue::send(const Event& e) {
    std::lock_guard<std::mutex> lk(m);
    if (destroyed) return CELL_ESRCH;
    if (events.size() >= size) return CELL_EBUSY;
    events.push_back(e);
    cv.notify_one();
    return CELL_OK;
}

uint32_t EventQueue::receive(Event& e, uint64_t timeout_us) {
    std::unique_lock<std::mutex> lk(m);
    if (!wait_for(cv, lk, timeout_us, [&] { return !events.empty() || destroyed; })) return CELL_ETIMEDOUT;
    if (events.empty()) return CELL_ECANCELED;
    e = events.front();
    events.pop_front();
    return CELL_OK;
}

}  // namespace sly4
