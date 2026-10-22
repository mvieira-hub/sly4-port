// LV2 SPU syscalls (sys_spu_image_*, sys_spu_thread_group_*, sys_spu_thread_*) and the
// SPU image helpers of sysPrxForUser. Argument layouts follow RPCS3's lv2/sys_spu.h.
#include <cinttypes>
#include <cstring>
#include <thread>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "kernel/spu_thread.h"
#include "kernel/vfs.h"
#include "loader/spu_elf.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

constexpr uint32_t kGroupId = 0x04000100, kThreadId = 0x02000000;
constexpr uint32_t kGroupEventRun = 1;  // SYS_SPU_THREAD_GROUP_EVENT_RUN

#define OBJ(T, var, id)                    \
    auto var = idm().get<T>(uint32_t(id)); \
    if (!var) {                            \
        c.gpr[3] = CELL_ESRCH;             \
        return;                            \
    }

// Copies the image's segments into a thread's local store.
void deploy(GuestMemory& m, GuestAddr img, uint8_t* ls) {
    const GuestAddr segs = m.read<uint32_t>(img + 8);
    const int32_t n = m.read<int32_t>(img + 12);
    for (int32_t i = 0; i < n; ++i) {
        const GuestAddr e = segs + 16 * uint32_t(i);
        const int32_t type = m.read<int32_t>(e);
        const uint32_t lsa = m.read<uint32_t>(e + 4) & kSpuLsMask, size = m.read<uint32_t>(e + 8),
                       src = m.read<uint32_t>(e + 12);
        const uint32_t len = std::min(size, kSpuLsSize - lsa);
        if (type == 1) {
            std::memcpy(ls + lsa, m.host(src), len);
        } else if (type == 2) {
            for (uint32_t k = 0; k + 4 <= len; k += 4) store_be<uint32_t>(ls + lsa + k, src);
        }
    }
}

// ---------------------------------------------------------------- images
// Reads an SPU ELF from an open file at `offset` into main memory and imports it.
uint32_t image_from_fd(GuestMemory& m, GuestAddr img, uint32_t fd, uint64_t offset) {
    uint8_t head[52];
    uint64_t done = 0;
    if (fs_read_at(fd, offset, head, sizeof(head), done) != CELL_OK || done != sizeof(head)) return CELL_ENOEXEC;
    const uint32_t phoff = load_be<uint32_t>(head + 28), phnum = load_be<uint16_t>(head + 44);
    std::vector<uint8_t> ph(size_t(phnum) * 32);
    if (fs_read_at(fd, offset + phoff, ph.data(), ph.size(), done) != CELL_OK) return CELL_ENOEXEC;
    uint32_t end = phoff + phnum * 32;
    for (uint32_t i = 0; i < phnum; ++i)
        end = std::max(end, load_be<uint32_t>(&ph[32 * i + 4]) + load_be<uint32_t>(&ph[32 * i + 16]));
    const GuestAddr buf = kernel().main_memory().alloc(end, 128);
    if (!buf) return CELL_ENOMEM;
    if (fs_read_at(fd, offset, m.host(buf), end, done) != CELL_OK) return CELL_EIO_;
    return spu_image_import(m, img, buf);
}

void sys_spu_image_open(PPUContext& c, GuestMemory& m) {
    const std::string path = m.read_cstring(a32(c, 4), 1024);
    uint32_t fd = 0;
    uint32_t r = fs_open(path, 0, fd);
    if (r == CELL_OK) {
        r = image_from_fd(m, a32(c, 3), fd, 0);
        fs_close(fd);
    }
    SLY4_INFO("SPU: sys_spu_image_open(%s) -> 0x%X", path.c_str(), r);
    c.gpr[3] = r;
}
void sys_spu_image_open_by_fd(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = image_from_fd(m, a32(c, 3), a32(c, 4), c.gpr[5]);
}
void ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

// ---------------------------------------------------------------- groups and threads
// sys_spu_thread_group_create(u32* id, num, prio, attr {u32 nsize, u32 name, s32 type, u32 ct})
void sys_spu_thread_group_create(PPUContext& c, GuestMemory& m) {
    const uint32_t num = a32(c, 4), attr = a32(c, 6);
    if (num < 1 || num > 6) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    auto g = std::make_shared<SpuThreadGroup>();
    g->num = num;
    g->priority = int32_t(c.gpr[5]);
    g->threads.resize(num);
    if (attr && m.read<uint32_t>(attr + 4)) g->name = m.read_cstring(m.read<uint32_t>(attr + 4), 128);
    g->id = idm().add(g, kGroupId);
    m.write<uint32_t>(a32(c, 3), g->id);
    SLY4_INFO("SPU: grupo 0x%08X '%s' criado com %u SPUs, prioridade %d", g->id, g->name.c_str(), num, g->priority);
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_destroy(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(g->m);
        if (g->started) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
    }
    for (auto& t : g->threads)
        if (t) idm().remove(t->id);
    idm().remove(g->id);
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_initialize(u32* thread, group, spu_num, img*, attr {u32 name, u32 nsize,
// u32 option}, arg {u64 arg1..arg4})
void sys_spu_thread_initialize(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThreadGroup, g, c.gpr[4]);
    const uint32_t num = a32(c, 5), img = a32(c, 6), attr = a32(c, 7), arg = a32(c, 8);
    if (num >= g->num || !img) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    auto t = std::make_shared<SpuThread>();
    t->group = g.get();
    t->index = num;
    t->image = img;
    if (attr && m.read<uint32_t>(attr)) t->name = m.read_cstring(m.read<uint32_t>(attr), 128);
    for (int i = 0; i < 4 && arg; ++i) t->args[i] = m.read<uint64_t>(arg + 8 * uint32_t(i));
    t->id = idm().add(t, kThreadId);
    add_spu_thread(t);
    g->threads[num] = t;
    m.write<uint32_t>(a32(c, 3), t->id);
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_start(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(g->m);
        if (g->started) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
        g->started = true;
        g->join_cause = 0;
        g->exit_status = 0;
        g->running = 0;
        for (auto& t : g->threads)
            if (t) ++g->running;
    }
    for (auto& t : g->threads) {
        if (!t) continue;
        std::memset(t->ls->data, 0, sizeof(t->ls->data));
        deploy(m, t->image, t->ls->data);
        SPUContext& s = t->ctx;
        s = SPUContext{};
        s.ls = t->ls->data;
        s.mem = &m;
        s.io = t.get();
        for (int i = 0; i < 4; ++i) s.gpr[3 + i].u64(0) = t->args[i];
        s.gpr[1].u32(0) = 0x3FFF0;  // the program's crt sets up its own stack, this is a fallback
        s.pc = m.read<uint32_t>(t->image + 4) & kSpuLsMask;
        t->stop_requested = false;
        t->in_mbox.clear();
        t->out_mbox.clear();
        std::thread([t] { t->run(); }).detach();
    }
    SLY4_INFO("SPU: grupo 0x%08X '%s' iniciado", g->id, g->name.c_str());
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_terminate(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    {
        std::lock_guard<std::mutex> lk(g->m);
        g->exit_status = a32(c, 4);
        g->join_cause = 4;  // SYS_SPU_THREAD_GROUP_JOIN_TERMINATED
    }
    for (auto& t : g->threads)
        if (t) {
            t->stop_requested = true;
            t->cv.notify_all();
        }
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_group_join(id, u32* cause, u32* status)
void sys_spu_thread_group_join(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    std::unique_lock<std::mutex> lk(g->m);
    g->cv.wait(lk, [&] { return !g->started; });
    if (a32(c, 4)) m.write<uint32_t>(a32(c, 4), g->join_cause);
    if (a32(c, 5)) m.write<uint32_t>(a32(c, 5), g->exit_status);
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_set_priority(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    g->priority = int32_t(c.gpr[4]);
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_get_priority(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    m.write<int32_t>(a32(c, 4), g->priority);
    c.gpr[3] = CELL_OK;
}

// sys_spu_thread_write_ls / read_ls(id, lsa, value, type = size in bytes)
void sys_spu_thread_write_ls(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    const uint32_t lsa = a32(c, 4) & kSpuLsMask, type = a32(c, 6);
    uint8_t* p = t->ls->data + lsa;
    switch (type) {
        case 1: *p = uint8_t(c.gpr[5]); break;
        case 2: store_be<uint16_t>(p, uint16_t(c.gpr[5])); break;
        case 4: store_be<uint32_t>(p, uint32_t(c.gpr[5])); break;
        case 8: store_be<uint64_t>(p, c.gpr[5]); break;
        default: c.gpr[3] = CELL_EINVAL; return;
    }
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_read_ls(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThread, t, c.gpr[3]);
    const uint32_t lsa = a32(c, 4) & kSpuLsMask, type = a32(c, 6);
    const uint8_t* p = t->ls->data + lsa;
    uint64_t v;
    switch (type) {
        case 1: v = *p; break;
        case 2: v = load_be<uint16_t>(p); break;
        case 4: v = load_be<uint32_t>(p); break;
        case 8: v = load_be<uint64_t>(p); break;
        default: c.gpr[3] = CELL_EINVAL; return;
    }
    m.write<uint64_t>(a32(c, 5), v);
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_write_snr(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    const uint32_t n = a32(c, 4);
    if (n > 1) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    t->write_snr(int(n), a32(c, 5));
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_write_spu_mb(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    t->push_in_mbox(a32(c, 4));
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_set_spu_cfg(id, value): bit 0 = SNR1 in OR mode, bit 1 = SNR2.
void sys_spu_thread_set_spu_cfg(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    std::lock_guard<std::mutex> lk(t->m);
    t->snr_or[0] = c.gpr[4] & 1;
    t->snr_or[1] = c.gpr[4] & 2;
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_get_spu_cfg(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThread, t, c.gpr[3]);
    m.write<uint64_t>(a32(c, 4), (t->snr_or[0] ? 1 : 0) | (t->snr_or[1] ? 2 : 0));
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- events
// sys_spu_thread_connect_event(id, eq, et, spup): SPU port -> event queue.
void sys_spu_thread_connect_event(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    const uint32_t spup = a32(c, 6);
    if (spup > 63) {
        c.gpr[3] = CELL_EINVAL;
        return;
    }
    if (t->ports[spup].lock()) {
        c.gpr[3] = CELL_EISCONN;
        return;
    }
    t->ports[spup] = q;
    SLY4_INFO("SPU 0x%08X: porta SPU %u -> fila 0x%08X", a32(c, 3), spup, a32(c, 4));
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_disconnect_event(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    const uint32_t spup = a32(c, 5);
    if (spup <= 63) t->ports[spup].reset();
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_bind_queue(id, eq, spuq_num): the queue an SPU receives from by number.
void sys_spu_thread_bind_queue(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    for (auto& [n, w] : t->bound)
        if (n == a32(c, 5)) {
            c.gpr[3] = CELL_EBUSY;
            return;
        }
    t->bound.push_back({a32(c, 5), q});
    SLY4_INFO("SPU 0x%08X: fila 0x%08X recebida como spuq %u", a32(c, 3), a32(c, 4), a32(c, 5));
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_unbind_queue(PPUContext& c, GuestMemory&) {
    OBJ(SpuThread, t, c.gpr[3]);
    std::erase_if(t->bound, [&](const auto& b) { return b.first == a32(c, 4); });
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_group_connect_event(id, eq, et): group run/exception events.
void sys_spu_thread_group_connect_event(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    if (a32(c, 5) == kGroupEventRun) g->run_queue = q;
    c.gpr[3] = CELL_OK;
}
void sys_spu_thread_group_disconnect_event(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    if (a32(c, 4) == kGroupEventRun) g->run_queue.reset();
    c.gpr[3] = CELL_OK;
}
// sys_spu_thread_group_connect_event_all_threads(id, eq, u64 req, u8* spup): the lowest
// port in `req` free in every thread of the group, connected in all of them.
void sys_spu_thread_group_connect_event_all_threads(PPUContext& c, GuestMemory& m) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    OBJ(EventQueue, q, c.gpr[4]);
    const uint64_t req = c.gpr[5];
    for (uint32_t p = 0; p < 64; ++p) {
        if (!(req & (1ull << p))) continue;
        bool free = true;
        for (auto& t : g->threads)
            if (t && t->ports[p].lock()) free = false;
        if (!free) continue;
        for (auto& t : g->threads)
            if (t) t->ports[p] = q;
        m.write<uint8_t>(a32(c, 6), uint8_t(p));
        SLY4_INFO("grupo SPU 0x%08X: porta SPU %u de todas as threads -> fila 0x%08X", a32(c, 3), p, a32(c, 4));
        c.gpr[3] = CELL_OK;
        return;
    }
    c.gpr[3] = CELL_EISCONN;
}
void sys_spu_thread_group_disconnect_event_all_threads(PPUContext& c, GuestMemory&) {
    OBJ(SpuThreadGroup, g, c.gpr[3]);
    const uint32_t p = a32(c, 4);
    if (p <= 63)
        for (auto& t : g->threads)
            if (t) t->ports[p].reset();
    c.gpr[3] = CELL_OK;
}

// ---------------------------------------------------------------- sysPrxForUser helpers
// sys_spu_image_import(img*, src, type): the ELF stays where it is (both types).
void sys_spu_image_import(PPUContext& c, GuestMemory& m) { c.gpr[3] = spu_image_import(m, a32(c, 3), a32(c, 4)); }
// sys_spu_elf_get_information(elf, u32* entry, s32* nseg)
void sys_spu_elf_get_information(PPUContext& c, GuestMemory& m) {
    SpuImage info;
    std::string error;
    if (!parse_spu_elf(m.host(a32(c, 3)), 0x1000000, info, error)) {
        c.gpr[3] = CELL_ENOEXEC;
        return;
    }
    uint32_t n = 0;
    for (const SpuSegment& s : info.segments) n += 1 + (s.mem_size > s.file_size);
    m.write<uint32_t>(a32(c, 4), info.entry);
    m.write<int32_t>(a32(c, 5), int32_t(n));
    c.gpr[3] = CELL_OK;
}

#undef OBJ

}  // namespace

void register_spu_syscalls() {
    register_syscall(156, "sys_spu_image_open", sys_spu_image_open);
    register_syscall(158, "sys_spu_image_close", ok);
    register_syscall(169, "sys_spu_initialize", ok);
    register_syscall(170, "sys_spu_thread_group_create", sys_spu_thread_group_create);
    register_syscall(171, "sys_spu_thread_group_destroy", sys_spu_thread_group_destroy);
    register_syscall(172, "sys_spu_thread_initialize", sys_spu_thread_initialize);
    register_syscall(173, "sys_spu_thread_group_start", sys_spu_thread_group_start);
    register_syscall(177, "sys_spu_thread_group_terminate", sys_spu_thread_group_terminate);
    register_syscall(178, "sys_spu_thread_group_join", sys_spu_thread_group_join);
    register_syscall(179, "sys_spu_thread_group_set_priority", sys_spu_thread_group_set_priority);
    register_syscall(180, "sys_spu_thread_group_get_priority", sys_spu_thread_group_get_priority);
    register_syscall(181, "sys_spu_thread_write_ls", sys_spu_thread_write_ls);
    register_syscall(182, "sys_spu_thread_read_ls", sys_spu_thread_read_ls);
    register_syscall(184, "sys_spu_thread_write_snr", sys_spu_thread_write_snr);
    register_syscall(185, "sys_spu_thread_group_connect_event", sys_spu_thread_group_connect_event);
    register_syscall(186, "sys_spu_thread_group_disconnect_event", sys_spu_thread_group_disconnect_event);
    register_syscall(187, "sys_spu_thread_set_spu_cfg", sys_spu_thread_set_spu_cfg);
    register_syscall(188, "sys_spu_thread_get_spu_cfg", sys_spu_thread_get_spu_cfg);
    register_syscall(190, "sys_spu_thread_write_spu_mb", sys_spu_thread_write_spu_mb);
    register_syscall(191, "sys_spu_thread_connect_event", sys_spu_thread_connect_event);
    register_syscall(192, "sys_spu_thread_disconnect_event", sys_spu_thread_disconnect_event);
    register_syscall(193, "sys_spu_thread_bind_queue", sys_spu_thread_bind_queue);
    register_syscall(194, "sys_spu_thread_unbind_queue", sys_spu_thread_unbind_queue);
    register_syscall(251, "sys_spu_thread_group_connect_event_all_threads",
                     sys_spu_thread_group_connect_event_all_threads);
    register_syscall(252, "sys_spu_thread_group_disconnect_event_all_threads",
                     sys_spu_thread_group_disconnect_event_all_threads);
    register_syscall(260, "sys_spu_image_open_by_fd", sys_spu_image_open_by_fd);
}

HleModule hle_sys_prx_for_user_spu() {
    return {"sysPrxForUser",
            {
                {"sys_spu_image_import", sys_spu_image_import},
                {"sys_spu_image_close", ok},
                {"sys_spu_elf_get_information", sys_spu_elf_get_information},
            }};
}

}  // namespace sly4
