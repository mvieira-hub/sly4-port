// LV2 synchronisation syscalls (mutex, cond, rwlock, semaphore, event flag, event
// queue/port, timers). Argument and attribute layouts follow RPCS3's lv2/*.h.
#include <thread>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }
uint64_t tid() {
    PpuThread* t = current_thread();
    return t ? t->id : 0;
}
std::string name8(GuestMemory& m, GuestAddr at) {
    char s[9] = {};
    m.copy_from(s, at, 8);
    return s;
}

// Creates `obj`, writes its id to the u32 at `out`, returns CELL_OK.
template <class T>
uint32_t publish(GuestMemory& m, uint32_t out, std::shared_ptr<T> obj, uint32_t base) {
    const uint32_t id = idm().add(std::move(obj), base);
    m.write<uint32_t>(out, id);
    return CELL_OK;
}

#define OBJ(T, var, id)                      \
    auto var = idm().get<T>(uint32_t(id));   \
    if (!var) {                              \
        c.gpr[3] = CELL_ESRCH;               \
        return;                              \
    }

// ---------------------------------------------------------------- mutex / cond
constexpr uint32_t kSyncRecursive = 0x10;

void sys_mutex_create(PPUContext& c, GuestMemory& m) {
    const uint32_t attr = a32(c, 4);
    auto mx = std::make_shared<Mutex>();
    mx->recursive = m.read<uint32_t>(attr + 4) == kSyncRecursive;
    mx->name = name8(m, attr + 32);
    c.gpr[3] = publish(m, a32(c, 3), mx, idbase::kMutex);
}
void sys_mutex_destroy(PPUContext& c, GuestMemory&) {
    OBJ(Mutex, mx, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(mx->m);
        if (mx->owner || mx->cond_users) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
    }
    idm().remove(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
void sys_mutex_lock(PPUContext& c, GuestMemory&) {
    OBJ(Mutex, mx, c.gpr[3]);
    c.gpr[3] = mx->lock(tid(), c.gpr[4]);
}
void sys_mutex_trylock(PPUContext& c, GuestMemory&) {
    OBJ(Mutex, mx, c.gpr[3]);
    c.gpr[3] = mx->trylock(tid());
}
void sys_mutex_unlock(PPUContext& c, GuestMemory&) {
    OBJ(Mutex, mx, c.gpr[3]);
    c.gpr[3] = mx->unlock(tid());
}

void sys_cond_create(PPUContext& c, GuestMemory& m) {
    OBJ(Mutex, mx, c.gpr[4]);
    auto cv = std::make_shared<Cond>();
    cv->mutex = mx;
    cv->name = name8(m, a32(c, 5) + 16);
    {
        std::lock_guard<std::mutex> lk(mx->m);
        ++mx->cond_users;
    }
    c.gpr[3] = publish(m, a32(c, 3), cv, idbase::kCond);
}
void sys_cond_destroy(PPUContext& c, GuestMemory&) {
    OBJ(Cond, cv, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(cv->mutex->m);
        if (cv->waiters) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
        --cv->mutex->cond_users;
    }
    idm().remove(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
void sys_cond_wait(PPUContext& c, GuestMemory&) {
    OBJ(Cond, cv, c.gpr[3]);
    c.gpr[3] = cv->wait(tid(), c.gpr[4]);
}
void sys_cond_signal(PPUContext& c, GuestMemory&) {
    OBJ(Cond, cv, c.gpr[3]);
    c.gpr[3] = cv->signal(false);
}
void sys_cond_signal_all(PPUContext& c, GuestMemory&) {
    OBJ(Cond, cv, c.gpr[3]);
    c.gpr[3] = cv->signal(true);
}

// ---------------------------------------------------------------- rwlock
void sys_rwlock_create(PPUContext& c, GuestMemory& m) {
    auto rw = std::make_shared<Rwlock>();
    rw->name = name8(m, a32(c, 4) + 24);
    c.gpr[3] = publish(m, a32(c, 3), rw, idbase::kRwlock);
}
void sys_rwlock_destroy(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = idm().remove(a32(c, 3)) ? CELL_OK : CELL_ESRCH;
}
void sys_rwlock_rlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->rlock(c.gpr[4], false);
}
void sys_rwlock_tryrlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->rlock(0, true);
}
void sys_rwlock_runlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->runlock();
}
void sys_rwlock_wlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->wlock(tid(), c.gpr[4], false);
}
void sys_rwlock_trywlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->wlock(tid(), 0, true);
}
void sys_rwlock_wunlock(PPUContext& c, GuestMemory&) {
    OBJ(Rwlock, rw, c.gpr[3]);
    c.gpr[3] = rw->wunlock(tid());
}

// ---------------------------------------------------------------- semaphore
void sys_semaphore_create(PPUContext& c, GuestMemory& m) {
    const int32_t initial = int32_t(c.gpr[5]), max = int32_t(c.gpr[6]);
    if (max <= 0 || initial < 0 || initial > max) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    auto s = std::make_shared<Semaphore>();
    s->value = initial;
    s->max = max;
    s->name = name8(m, a32(c, 4) + 24);
    c.gpr[3] = publish(m, a32(c, 3), s, idbase::kSemaphore);
}
void sys_semaphore_destroy(PPUContext& c, GuestMemory&) {
    OBJ(Semaphore, s, c.gpr[3]);
    c.gpr[3] = idm().remove(a32(c, 3)) ? CELL_OK : CELL_ESRCH;
}
void sys_semaphore_wait(PPUContext& c, GuestMemory&) {
    OBJ(Semaphore, s, c.gpr[3]);
    c.gpr[3] = s->wait(c.gpr[4], false);
}
void sys_semaphore_trywait(PPUContext& c, GuestMemory&) {
    OBJ(Semaphore, s, c.gpr[3]);
    c.gpr[3] = s->wait(0, true);
}
void sys_semaphore_post(PPUContext& c, GuestMemory&) {
    OBJ(Semaphore, s, c.gpr[3]);
    c.gpr[3] = s->post(int32_t(c.gpr[4]));
}
void sys_semaphore_get_value(PPUContext& c, GuestMemory& m) {
    OBJ(Semaphore, s, c.gpr[3]);
    std::lock_guard<std::mutex> lk(s->m);
    m.write<int32_t>(a32(c, 4), s->value);
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- event flag
void sys_event_flag_create(PPUContext& c, GuestMemory& m) {
    auto f = std::make_shared<EventFlag>();
    f->bits = c.gpr[5];
    f->name = name8(m, a32(c, 4) + 24);
    c.gpr[3] = publish(m, a32(c, 3), f, idbase::kEventFlag);
}
void sys_event_flag_destroy(PPUContext& c, GuestMemory&) {
    OBJ(EventFlag, f, c.gpr[3]);
    c.gpr[3] = idm().remove(a32(c, 3)) ? CELL_OK : CELL_ESRCH;
}
void event_flag_wait(PPUContext& c, GuestMemory& m, bool try_only) {
    OBJ(EventFlag, f, c.gpr[3]);
    const uint32_t mode = a32(c, 5);
    if (!(mode & (event_flag::kWaitAnd | event_flag::kWaitOr))) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    uint64_t result = 0;
    const uint32_t r = f->wait(c.gpr[4], mode, result, try_only ? 0 : c.gpr[7], try_only);
    if (uint32_t out = a32(c, 6)) m.write<uint64_t>(out, result);
    c.gpr[3] = r;
}
void sys_event_flag_wait(PPUContext& c, GuestMemory& m) { event_flag_wait(c, m, false); }
void sys_event_flag_trywait(PPUContext& c, GuestMemory& m) { event_flag_wait(c, m, true); }
void sys_event_flag_set(PPUContext& c, GuestMemory&) {
    OBJ(EventFlag, f, c.gpr[3]);
    f->set(c.gpr[4]);
    c.gpr[3] = CELL_OK;
}
void sys_event_flag_clear(PPUContext& c, GuestMemory&) {
    OBJ(EventFlag, f, c.gpr[3]);
    f->clear(c.gpr[4]);
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- event queue / port
void sys_event_queue_create(PPUContext& c, GuestMemory& m) {
    const int32_t size = int32_t(c.gpr[6]);
    if (size <= 0 || size > 127) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    auto q = std::make_shared<EventQueue>();
    q->size = uint32_t(size);
    q->key = c.gpr[5];
    q->name = name8(m, a32(c, 4) + 8);
    c.gpr[3] = publish(m, a32(c, 3), q, idbase::kEventQueue);
}
void sys_event_queue_destroy(PPUContext& c, GuestMemory&) {
    OBJ(EventQueue, q, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(q->m);
        q->destroyed = true;
        q->cv.notify_all();
    }
    idm().remove(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
// The event comes back in r4..r7 (the sys_event_t pointer argument is unused).
void sys_event_queue_receive(PPUContext& c, GuestMemory&) {
    OBJ(EventQueue, q, c.gpr[3]);
    Event e{};
    const uint32_t r = q->receive(e, c.gpr[5]);
    if (r == CELL_OK) {
        c.gpr[4] = e.source;
        c.gpr[5] = e.data1;
        c.gpr[6] = e.data2;
        c.gpr[7] = e.data3;
    }
    c.gpr[3] = r;
}
void sys_event_queue_tryreceive(PPUContext& c, GuestMemory& m) {
    OBJ(EventQueue, q, c.gpr[3]);
    const uint32_t array = a32(c, 4), size = a32(c, 5);
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> lk(q->m);
        for (; n < size && !q->events.empty(); ++n) {
            const Event& e = q->events.front();
            m.write<uint64_t>(array + 32 * n, e.source);
            m.write<uint64_t>(array + 32 * n + 8, e.data1);
            m.write<uint64_t>(array + 32 * n + 16, e.data2);
            m.write<uint64_t>(array + 32 * n + 24, e.data3);
            q->events.pop_front();
        }
    }
    m.write<uint32_t>(a32(c, 6), n);
    c.gpr[3] = CELL_OK;
}
void sys_event_queue_drain(PPUContext& c, GuestMemory&) {
    OBJ(EventQueue, q, c.gpr[3]);
    std::lock_guard<std::mutex> lk(q->m);
    q->events.clear();
    c.gpr[3] = CELL_OK;
}

void sys_event_port_create(PPUContext& c, GuestMemory& m) {
    if (c.gpr[4] != 1) {  // SYS_EVENT_PORT_LOCAL
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    auto p = std::make_shared<EventPort>();
    p->name_value = c.gpr[5];
    const uint32_t id = idm().add(p, idbase::kEventPort);
    p->id = id;
    m.write<uint32_t>(a32(c, 3), id);
    c.gpr[3] = CELL_OK;
}
void sys_event_port_destroy(PPUContext& c, GuestMemory&) {
    OBJ(EventPort, p, c.gpr[3]);
    if (p->queue.lock()) {
        c.gpr[3] = CELL_EISCONN;
        return;
    }
    idm().remove(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
void sys_event_port_connect_local(PPUContext& c, GuestMemory&) {
    OBJ(EventPort, p, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    if (p->queue.lock()) {
        c.gpr[3] = CELL_EISCONN;
        return;
    }
    p->queue = q;
    c.gpr[3] = CELL_OK;
}
void sys_event_port_disconnect(PPUContext& c, GuestMemory&) {
    OBJ(EventPort, p, c.gpr[3]);
    if (!p->queue.lock()) {
        c.gpr[3] = CELL_ENOTCONN;
        return;
    }
    p->queue.reset();
    c.gpr[3] = CELL_OK;
}
void sys_event_port_send(PPUContext& c, GuestMemory&) {
    OBJ(EventPort, p, c.gpr[3]);
    auto q = p->queue.lock();
    if (!q) {
        c.gpr[3] = CELL_ENOTCONN;
        return;
    }
    const uint64_t source = p->name_value ? p->name_value : (uint64_t(0x01000500) << 32) | p->id;
    c.gpr[3] = q->send({source, c.gpr[4], c.gpr[5], c.gpr[6]});
}

// ---------------------------------------------------------------- timers
// A started timer has a host thread that sends {name, data1, data2, expire time} to the
// connected queue at each expiry (system time in microseconds, as sys_time_get_system_time).
struct Timer : KernelObject {
    std::mutex m;
    std::condition_variable cv;
    bool running = false, destroyed = false;
    uint64_t next = 0, period = 0;
    uint64_t generation = 0;
    std::weak_ptr<EventQueue> queue;
    uint64_t source = 0, data1 = 0, data2 = 0;
};

void timer_thread(std::shared_ptr<Timer> t, uint64_t gen) {
    std::unique_lock<std::mutex> lk(t->m);
    while (t->running && !t->destroyed && t->generation == gen) {
        const uint64_t now = guest_system_time();
        if (now < t->next) {
            t->cv.wait_for(lk, std::chrono::microseconds(t->next - now));
            continue;
        }
        if (auto q = t->queue.lock()) q->send({t->source, t->data1, t->data2, t->next});
        if (!t->period) {
            t->running = false;
            break;
        }
        t->next += t->period;
    }
}

void sys_timer_create(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = publish(m, a32(c, 3), std::make_shared<Timer>(), idbase::kTimer);
}
void sys_timer_destroy(PPUContext& c, GuestMemory&) {
    OBJ(Timer, t, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->destroyed = true;
        t->cv.notify_all();
    }
    idm().remove(a32(c, 3));
    c.gpr[3] = CELL_OK;
}
void sys_timer_get_information(PPUContext& c, GuestMemory& m) {
    OBJ(Timer, t, c.gpr[3]);
    std::lock_guard<std::mutex> lk(t->m);
    const uint32_t info = a32(c, 4);
    m.write<uint64_t>(info, t->next);
    m.write<uint64_t>(info + 8, t->period);
    m.write<uint32_t>(info + 16, t->running ? 1 : 0);
    c.gpr[3] = CELL_OK;
}
void sys_timer_start(PPUContext& c, GuestMemory&) {
    OBJ(Timer, t, c.gpr[3]);
    const uint64_t base = c.gpr[4], period = c.gpr[5];
    std::lock_guard<std::mutex> lk(t->m);
    if (t->running) {
        c.gpr[3] = CELL_EBUSY;
        return;
    }
    if (!base && !period) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    t->next = base ? base : guest_system_time() + period;
    t->period = period;
    t->running = true;
    std::thread(timer_thread, t, ++t->generation).detach();
    c.gpr[3] = CELL_OK;
}
void sys_timer_stop(PPUContext& c, GuestMemory&) {
    OBJ(Timer, t, c.gpr[3]);
    std::lock_guard<std::mutex> lk(t->m);
    t->running = false;
    t->cv.notify_all();
    c.gpr[3] = CELL_OK;
}
void sys_timer_connect_event_queue(PPUContext& c, GuestMemory&) {
    OBJ(Timer, t, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    std::lock_guard<std::mutex> lk(t->m);
    t->queue = q;
    t->source = c.gpr[5];
    t->data1 = c.gpr[6];
    t->data2 = c.gpr[7];
    c.gpr[3] = CELL_OK;
}
void sys_timer_disconnect_event_queue(PPUContext& c, GuestMemory&) {
    OBJ(Timer, t, c.gpr[3]);
    std::lock_guard<std::mutex> lk(t->m);
    t->queue.reset();
    c.gpr[3] = CELL_OK;
}

#undef OBJ

}  // namespace

void register_sync_syscalls() {
#define R(n, f) register_syscall(n, #f, f)
    R(70, sys_timer_create);
    R(71, sys_timer_destroy);
    R(72, sys_timer_get_information);
    register_syscall(73, "_sys_timer_start", sys_timer_start);
    R(74, sys_timer_stop);
    R(75, sys_timer_connect_event_queue);
    R(76, sys_timer_disconnect_event_queue);
    R(82, sys_event_flag_create);
    R(83, sys_event_flag_destroy);
    R(85, sys_event_flag_wait);
    R(86, sys_event_flag_trywait);
    R(87, sys_event_flag_set);
    R(90, sys_semaphore_create);
    R(91, sys_semaphore_destroy);
    R(92, sys_semaphore_wait);
    R(93, sys_semaphore_trywait);
    R(94, sys_semaphore_post);
    R(100, sys_mutex_create);
    R(101, sys_mutex_destroy);
    R(102, sys_mutex_lock);
    R(103, sys_mutex_trylock);
    R(104, sys_mutex_unlock);
    R(105, sys_cond_create);
    R(106, sys_cond_destroy);
    R(107, sys_cond_wait);
    R(108, sys_cond_signal);
    R(109, sys_cond_signal_all);
    R(114, sys_semaphore_get_value);
    R(118, sys_event_flag_clear);
    R(120, sys_rwlock_create);
    R(121, sys_rwlock_destroy);
    R(122, sys_rwlock_rlock);
    R(123, sys_rwlock_tryrlock);
    R(124, sys_rwlock_runlock);
    R(125, sys_rwlock_wlock);
    R(126, sys_rwlock_trywlock);
    R(127, sys_rwlock_wunlock);
    R(128, sys_event_queue_create);
    R(129, sys_event_queue_destroy);
    R(130, sys_event_queue_receive);
    R(131, sys_event_queue_tryreceive);
    R(133, sys_event_queue_drain);
    R(134, sys_event_port_create);
    R(135, sys_event_port_destroy);
    R(136, sys_event_port_connect_local);
    R(137, sys_event_port_disconnect);
    R(138, sys_event_port_send);
#undef R
}

}  // namespace sly4
