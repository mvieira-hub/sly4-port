// sysPrxForUser: the user-side half of the kernel (process, TLS, PPU thread helpers,
// lightweight mutex/cond, mmapper wrappers, PRX loading).
#include <cinttypes>
#include <mutex>

#include "core/log.h"
#include "kernel/hle.h"
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

// ---------------------------------------------------------------- process / TLS
// sys_initialize_tls(thread_id, tls_image, file_size, mem_size): called by the game's
// crt0 with the PT_TLS values the kernel passed in r7..r10; sets up r13.
void sys_initialize_tls(PPUContext& c, GuestMemory&) {
    Kernel& k = kernel();
    k.set_tls_template(a32(c, 4), a32(c, 5), a32(c, 6));
    PpuThread* t = current_thread();
    if (!t) return;
    if (!t->tls_block) {
        const uint64_t r13 = k.alloc_tls(*t);
        if (!r13) SLY4_ERROR("sys_initialize_tls: sem memória para o TLS");
        c.gpr[13] = r13;
    }
}

void sys_process_exit(PPUContext& c, GuestMemory&) { throw ProcessExit{int32_t(c.gpr[3])}; }

// Registers a function the system runs when the process spawns a new one; nothing to do.
void ignore_ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

void sys_time_get_system_time(PPUContext& c, GuestMemory&) { c.gpr[3] = guest_system_time(); }

void sys_process_is_stack(PPUContext& c, GuestMemory&) {
    const uint32_t a = a32(c, 3);
    c.gpr[3] = a - layout::kStackBase < layout::kStackSize ? 1 : 0;
}

// ---------------------------------------------------------------- PPU threads
constexpr uint64_t kThreadJoinable = 1, kThreadInterrupt = 2;

// sys_ppu_thread_create(u64* id, entry descriptor, arg, prio, stack size, flags, name)
void sys_ppu_thread_create(PPUContext& c, GuestMemory& m) {
    const uint64_t flags = c.gpr[8];
    const std::string name = a32(c, 9) ? m.read_cstring(a32(c, 9), 64) : "";
    uint32_t error = CELL_OK;
    auto t = kernel().create_thread(a32(c, 4), c.gpr[5], int32_t(c.gpr[6]), a32(c, 7), flags & kThreadJoinable,
                                    name, error);
    if (!t) {
        c.gpr[3] = error;
        return;
    }
    m.write<uint64_t>(a32(c, 3), t->id);
    SLY4_INFO("thread 0x%" PRIX64 " '%s' criada: entrada 0x%08X, arg 0x%" PRIX64 ", prioridade %d, pilha 0x%X%s",
              t->id, name.c_str(), t->entry, c.gpr[5], t->priority, t->stack_size,
              flags & kThreadInterrupt ? " (de interrupção: não iniciada)" : "");
    if (!(flags & kThreadInterrupt) && !kernel().start_thread(t)) {
        c.gpr[3] = CELL_EAGAIN;
        return;
    }
    c.gpr[3] = CELL_OK;
}

void sys_ppu_thread_exit(PPUContext& c, GuestMemory&) { throw ThreadExit{c.gpr[3]}; }

void sys_ppu_thread_get_id(PPUContext& c, GuestMemory& m) {
    m.write<uint64_t>(a32(c, 3), tid());
    c.gpr[3] = CELL_OK;
}

// sys_ppu_thread_once(s32* once_ctrl, init descriptor): runs init exactly once.
void sys_ppu_thread_once(PPUContext& c, GuestMemory& m) {
    static std::recursive_mutex lock;
    std::lock_guard<std::recursive_mutex> g(lock);
    const uint32_t ctrl = a32(c, 3), init = a32(c, 4);
    if (m.read<int32_t>(ctrl) == 0) {  // SYS_PPU_THREAD_ONCE_INIT
        m.write<int32_t>(ctrl, 1);     // SYS_PPU_THREAD_DONE_INIT
        call_guest(c, m, init);
    }
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- lwmutex / lwcond
// sys_lwmutex_t: owner u32 @0, waiter u32 @4, attribute @8, recursive_count @12,
// sleep_queue (our id) @16. The guest-visible fields are kept up to date for code that
// peeks at them; the state that matters is the host Mutex.
constexpr uint32_t kLwmutexFree = 0xFFFFFFFF, kLwmutexDead = 0xFFFFFFFE;
constexpr uint32_t kSyncRecursive = 0x10;

std::shared_ptr<Mutex> lwmutex_of(GuestMemory& m, GuestAddr lw) {
    return lw ? idm().get<Mutex>(m.read<uint32_t>(lw + 16)) : nullptr;
}
void lwmutex_publish(GuestMemory& m, GuestAddr lw, Mutex& mx) {
    m.write<uint32_t>(lw, mx.owner ? uint32_t(mx.owner) : kLwmutexFree);
    m.write<uint32_t>(lw + 12, mx.count);
}

void sys_lwmutex_create(PPUContext& c, GuestMemory& m) {
    const uint32_t lw = a32(c, 3), attr = a32(c, 4);
    auto mx = std::make_shared<Mutex>();
    const uint32_t protocol = m.read<uint32_t>(attr), recursive = m.read<uint32_t>(attr + 4);
    mx->recursive = recursive == kSyncRecursive;
    mx->name = name8(m, attr + 8);
    const uint32_t id = idm().add(mx, idbase::kLwmutex);
    m.write<uint32_t>(lw, kLwmutexFree);
    m.write<uint32_t>(lw + 4, 0);
    m.write<uint32_t>(lw + 8, protocol | recursive);
    m.write<uint32_t>(lw + 12, 0);
    m.write<uint32_t>(lw + 16, id);
    m.write<uint32_t>(lw + 20, 0);
    c.gpr[3] = CELL_OK;
}
void sys_lwmutex_destroy(PPUContext& c, GuestMemory& m) {
    const uint32_t lw = a32(c, 3);
    auto mx = lwmutex_of(m, lw);
    if (!mx) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mx->m);
        if (mx->owner || mx->cond_users) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
    }
    idm().remove(m.read<uint32_t>(lw + 16));
    m.write<uint32_t>(lw, kLwmutexDead);
    m.write<uint32_t>(lw + 16, 0);
    c.gpr[3] = CELL_OK;
}
void sys_lwmutex_lock(PPUContext& c, GuestMemory& m) {
    const uint32_t lw = a32(c, 3);
    auto mx = lwmutex_of(m, lw);
    if (!mx) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    const uint32_t r = mx->lock(tid(), c.gpr[4]);
    if (r == CELL_OK) lwmutex_publish(m, lw, *mx);
    c.gpr[3] = r;
}
void sys_lwmutex_trylock(PPUContext& c, GuestMemory& m) {
    const uint32_t lw = a32(c, 3);
    auto mx = lwmutex_of(m, lw);
    if (!mx) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    const uint32_t r = mx->trylock(tid());
    if (r == CELL_OK) lwmutex_publish(m, lw, *mx);
    c.gpr[3] = r;
}
void sys_lwmutex_unlock(PPUContext& c, GuestMemory& m) {
    const uint32_t lw = a32(c, 3);
    auto mx = lwmutex_of(m, lw);
    if (!mx) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    const uint64_t me = tid();
    // Publish before releasing, while this thread still owns the mutex.
    if (mx->owner == me) {
        m.write<uint32_t>(lw + 12, mx->count - 1);
        if (mx->count == 1) m.write<uint32_t>(lw, kLwmutexFree);
    }
    c.gpr[3] = mx->unlock(me);
}

// sys_lwcond_t: lwmutex address u32 @0, lwcond_queue (our id) u32 @4.
std::shared_ptr<Cond> lwcond_of(GuestMemory& m, GuestAddr lc) {
    return lc ? idm().get<Cond>(m.read<uint32_t>(lc + 4)) : nullptr;
}

void sys_lwcond_create(PPUContext& c, GuestMemory& m) {
    const uint32_t lc = a32(c, 3), lw = a32(c, 4), attr = a32(c, 5);
    auto mx = lwmutex_of(m, lw);
    if (!mx) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    auto cv = std::make_shared<Cond>();
    cv->mutex = mx;
    cv->name = attr ? name8(m, attr) : "";
    {
        std::lock_guard<std::mutex> lk(mx->m);
        ++mx->cond_users;
    }
    m.write<uint32_t>(lc, lw);
    m.write<uint32_t>(lc + 4, idm().add(cv, idbase::kLwcond));
    c.gpr[3] = CELL_OK;
}
void sys_lwcond_destroy(PPUContext& c, GuestMemory& m) {
    const uint32_t lc = a32(c, 3);
    auto cv = lwcond_of(m, lc);
    if (!cv) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(cv->mutex->m);
        if (cv->waiters) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
        --cv->mutex->cond_users;
    }
    idm().remove(m.read<uint32_t>(lc + 4));
    m.write<uint32_t>(lc + 4, 0);
    c.gpr[3] = CELL_OK;
}
void sys_lwcond_wait(PPUContext& c, GuestMemory& m) {
    const uint32_t lc = a32(c, 3);
    auto cv = lwcond_of(m, lc);
    if (!cv) {
        c.gpr[3] = CELL_ESRCH;
        return;
    }
    const uint32_t lw = m.read<uint32_t>(lc);
    const uint64_t me = tid();
    if (cv->mutex->owner == me) m.write<uint32_t>(lw, kLwmutexFree);
    const uint32_t r = cv->wait(me, c.gpr[4]);
    if (cv->mutex->owner == me) lwmutex_publish(m, lw, *cv->mutex);
    c.gpr[3] = r;
}
void sys_lwcond_signal(PPUContext& c, GuestMemory& m) {
    auto cv = lwcond_of(m, a32(c, 3));
    c.gpr[3] = cv ? cv->signal(false) : CELL_ESRCH;
}
void sys_lwcond_signal_all(PPUContext& c, GuestMemory& m) {
    auto cv = lwcond_of(m, a32(c, 3));
    c.gpr[3] = cv ? cv->signal(true) : CELL_ESRCH;
}

// ---------------------------------------------------------------- mmapper wrappers
void sys_mmapper_allocate_memory(PPUContext& c, GuestMemory& m) {
    uint32_t id = 0;
    const uint32_t r = mmapper_allocate_memory(c.gpr[3], c.gpr[4], id);
    if (r == CELL_OK) m.write<uint32_t>(a32(c, 5), id);
    c.gpr[3] = r;
}
void sys_mmapper_free_memory(PPUContext& c, GuestMemory&) { c.gpr[3] = mmapper_free_memory(a32(c, 3)); }
void sys_mmapper_map_memory(PPUContext& c, GuestMemory&) { c.gpr[3] = mmapper_map_memory(a32(c, 3), a32(c, 4)); }
void sys_mmapper_unmap_memory(PPUContext& c, GuestMemory& m) {
    uint32_t id = 0;
    const uint32_t r = mmapper_unmap_memory(a32(c, 3), id);
    if (r == CELL_OK && a32(c, 4)) m.write<uint32_t>(a32(c, 4), id);
    c.gpr[3] = r;
}

}  // namespace

HleModule hle_sys_prx_for_user() {
    return {"sysPrxForUser",
            {
                {"sys_initialize_tls", sys_initialize_tls},
                {"sys_process_exit", sys_process_exit},
                {"_sys_process_atexitspawn", ignore_ok},
                {"_sys_process_at_Exitspawn", ignore_ok},
                {"sys_prx_exitspawn_with_level", ignore_ok},
                {"sys_time_get_system_time", sys_time_get_system_time},
                {"sys_process_is_stack", sys_process_is_stack},
                {"sys_ppu_thread_create", sys_ppu_thread_create},
                {"sys_ppu_thread_exit", sys_ppu_thread_exit},
                {"sys_ppu_thread_get_id", sys_ppu_thread_get_id},
                {"sys_ppu_thread_once", sys_ppu_thread_once},
                {"sys_lwmutex_create", sys_lwmutex_create},
                {"sys_lwmutex_destroy", sys_lwmutex_destroy},
                {"sys_lwmutex_lock", sys_lwmutex_lock},
                {"sys_lwmutex_trylock", sys_lwmutex_trylock},
                {"sys_lwmutex_unlock", sys_lwmutex_unlock},
                {"sys_lwcond_create", sys_lwcond_create},
                {"sys_lwcond_destroy", sys_lwcond_destroy},
                {"sys_lwcond_wait", sys_lwcond_wait},
                {"sys_lwcond_signal", sys_lwcond_signal},
                {"sys_lwcond_signal_all", sys_lwcond_signal_all},
                {"sys_mmapper_allocate_memory", sys_mmapper_allocate_memory},
                {"sys_mmapper_free_memory", sys_mmapper_free_memory},
                {"sys_mmapper_map_memory", sys_mmapper_map_memory},
                {"sys_mmapper_unmap_memory", sys_mmapper_unmap_memory},
            }};
}

}  // namespace sly4
