#include "kernel/kernel.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#include "core/host_sleep.h"
#include "core/log.h"
#include "cpu/guest_atomic.h"
#include "cpu/ppu_rt.h"
#include "cpu/ppu_rt_trace.h"
#include "cpu/spu_blocks.h"
#include "kernel/hle.h"
#include "kernel/spu_thread.h"
#include "kernel/spurs.h"
#include "kernel/vfs.h"
#include "loader/elf_loader.h"
#include "loader/firmware.h"
#include "loader/nid.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sly4 {

namespace {

Kernel* g_kernel = nullptr;
thread_local PpuThread* t_current = nullptr;

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void on_hle(PPUContext& c, GuestMemory& m, uint32_t target) { hle().call(c, m, target); }

}  // namespace

PpuThread* current_thread() { return t_current; }

uint64_t guest_system_time() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return uint64_t(duration_cast<microseconds>(steady_clock::now() - t0).count());
}

Kernel& kernel() { return *g_kernel; }

Kernel::Kernel(GuestMemory& mem, const LoadedImage& img) : mem_(mem), img_(img) { g_kernel = this; }

Kernel::~Kernel() {
    if (g_kernel == this) g_kernel = nullptr;
}

bool Kernel::init(const NidNames& names, const KernelOptions& opt) {
    const GuestAddr main_base = align_up(img_.image_end, 0x10000);
    main_ = std::make_unique<GuestAllocator>(mem_, main_base, layout::kMainEnd - main_base, "principal");
    user_ = std::make_unique<GuestAllocator>(mem_, layout::kUserBase, layout::kUserSize, "usuÃ¡rio 1 MiB");
    user64k_ = std::make_unique<GuestAllocator>(mem_, layout::kUser64kBase, layout::kUser64kSize, "usuÃ¡rio 64 KiB");
    stacks_ = std::make_unique<GuestAllocator>(mem_, layout::kStackBase, layout::kStackSize, "pilhas");
    mmapper_ = std::make_unique<GuestAllocator>(mem_, layout::kMmapperBase, layout::kMmapperSize, "mmapper");

    call_log().trace = opt.trace;
    ring_lines_ = opt.ring;
    spu_cache_ = opt.spu_cache;
    if (opt.spu_profile) spu_profile_start();
    if (!opt.ppu_profile.empty()) ppu_profile_start(opt.ppu_profile);
    if (opt.verify_zlib) hle().set_call_hook(zlib_verify_call);
    spu_cache_enable(!spu_cache_.empty());
    HleTable& table = hle();
    vfs().setup(path_from_utf8(opt.game_dir), path_from_utf8(opt.data_dir));
    for (const HleModule& mod : {hle_sys_prx_for_user(), hle_sys_fs(), hle_cell_game(), hle_cell_sysutil(),
                                 hle_cell_sysutil_avconf_ext(), hle_cell_sysmodule(), hle_cell_rtc(), hle_cell_net_ctl(),
                                 hle_sys_net(), hle_cell_spurs(), hle_sys_prx_for_user_spu(), hle_sys_prx_for_user_libc(),
                                 hle_cell_gcm_sys(), hle_cell_audio(), hle_sys_io()})
        table.add(mod);

    // Firmware modules that run as they are (their recompiled code is linked in).
    std::vector<const LoadedImage*> images{&img_};
    if (!opt.firmware_dir.empty()) {
        for (const FirmwareModule& fm : kFirmwareModules) {
            const std::filesystem::path p = path_from_utf8(opt.firmware_dir) / fm.file;
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                SLY4_WARN("mÃ³dulo do firmware %s nÃ£o encontrado em %s: fica em HLE", fm.file, opt.firmware_dir.c_str());
                continue;
            }
            LoadedImage m;
            std::string error;
            if (!load_prx(path_to_utf8(p), mem_, fm.base, m, error)) {
                SLY4_ERROR("falha ao carregar %s: %s", fm.file, error.c_str());
                return false;
            }
            SLY4_INFO("firmware: %s (%s) em 0x%08X, %zu bibliotecas exportadas", fm.file, m.module_name.c_str(), fm.base,
                      m.exports.size());
            modules_.push_back(std::move(m));
        }
        for (const LoadedImage& m : modules_) images.push_back(&m);
    }
    if (!table.bind(mem_, *main_, images, names)) return false;
    register_sync_syscalls();
    register_memory_syscalls();
    register_fs_syscalls();
    register_spu_syscalls();

    rt::Hooks& h = rt::hooks();
    h.syscall = syscall_dispatch;
    h.hle = on_hle;
    h.hle_base = table.thunk_base();
    h.hle_size = table.thunk_bytes();
    if (!modules_.empty())
        rt::set_lle_trace([](PPUContext& c, uint32_t target) { hle().trace_lle(c, target); }, kFirmwareAreaBase,
                          kFirmwareAreaEnd - kFirmwareAreaBase);
    set_debug_who_hook([](char* buf, size_t size) {
        if (const PpuThread* t = t_current)
            std::snprintf(buf, size, "%s lr 0x%08X", t->name.c_str(), uint32_t(t->ctx.lr));
        else
            std::snprintf(buf, size, "(sem thread)");
    });
    return true;
}

bool Kernel::alloc_stack(PpuThread& t, uint32_t size) {
    size = align_up(size, GuestMemory::kPageSize);
    const GuestAddr base = stacks_->alloc(size + GuestMemory::kPageSize, 0x10000, false);
    if (!base) return false;
    t.stack_addr = base + GuestMemory::kPageSize;
    t.stack_size = size;
    return mem_.commit(t.stack_addr, size, Protection::ReadWrite);
}

void Kernel::set_tls_template(GuestAddr image, uint32_t file_size, uint32_t mem_size) {
    std::lock_guard<std::mutex> g(tls_lock_);
    tls_image_ = image;
    tls_file_ = file_size;
    tls_mem_ = mem_size;
}

uint64_t Kernel::alloc_tls(PpuThread& t) {
    std::lock_guard<std::mutex> g(tls_lock_);
    // 0x30-byte system area, then the TLS data; r13 points 0x7000 past the data (PPC64 TLS bias).
    const uint32_t align = std::max<uint32_t>(img_.tls.align, 0x10);
    const GuestAddr raw = main_->alloc(0x30 + tls_mem_ + align, 0x10);
    if (!raw) return 0;
    const GuestAddr data = align_up(raw + 0x30, align);
    t.tls_block = data - 0x30;
    std::memset(mem_.host(t.tls_block), 0, 0x30 + tls_mem_);
    if (tls_file_) std::memcpy(mem_.host(data), mem_.host(tls_image_), tls_file_);
    return uint64_t(data) + 0x7000;
}

int Kernel::run_main() {
    auto tp = std::make_shared<PpuThread>();
    PpuThread& t = *tp;
    t.id = next_thread_id();
    t.name = "main_thread";
    t.priority = img_.param.present ? img_.param.primary_prio : 1001;
    t.entry = img_.entry_function;
    uint32_t stack = img_.param.present ? img_.param.primary_stack_size : 0x100000;
    stack = align_up(std::clamp<uint32_t>(stack, 0x10000, 0x100000), 0x1000);
    if (!alloc_stack(t, stack)) {
        SLY4_ERROR("nÃ£o foi possÃ­vel criar a pilha da thread principal");
        request_exit(-1);
        return -1;
    }
    {
        std::lock_guard<std::mutex> g(threads_lock_);
        threads_.emplace(t.id, tp);
    }

    // argv/envp at the top of the stack: 64-bit pointer arrays, then the strings.
    const std::vector<std::string> argv = {"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"};
    const std::vector<std::string> envp;
    const uint32_t pointers = 8 * align_up(uint32_t(argv.size() + envp.size() + 2), 2);
    uint32_t strings = 0;
    for (const auto& s : argv) strings += align_up(uint32_t(s.size() + 1), 0x10);
    for (const auto& s : envp) strings += align_up(uint32_t(s.size() + 1), 0x10);
    const uint32_t args_size = align_up(pointers + strings, 0x10);
    const GuestAddr top = t.stack_addr + t.stack_size;
    const GuestAddr args = top - args_size;
    GuestAddr ptr = args, data = args + pointers;
    auto put = [&](const std::vector<std::string>& list) {
        const GuestAddr first = ptr;
        for (const auto& s : list) {
            mem_.copy_to(data, s.c_str(), s.size() + 1);
            mem_.write<uint64_t>(ptr, data);
            ptr += 8;
            data += align_up(uint32_t(s.size() + 1), 0x10);
        }
        mem_.write<uint64_t>(ptr, 0);
        ptr += 8;
        return first;
    };
    const GuestAddr argv_addr = put(argv);
    const GuestAddr envp_addr = put(envp);

    PPUContext& c = t.ctx;
    c.gpr[1] = top - args_size - 0x70;
    c.gpr[2] = img_.toc;
    c.gpr[3] = argv.size();
    c.gpr[4] = argv_addr;
    c.gpr[5] = envp_addr;
    c.gpr[6] = envp.size();
    c.gpr[7] = t.id;
    c.gpr[8] = img_.tls.image;
    c.gpr[9] = img_.tls.file_size;
    c.gpr[10] = img_.tls.mem_size;
    c.gpr[11] = img_.entry_descriptor;
    c.gpr[12] = img_.param.present && img_.param.malloc_page_size ? img_.param.malloc_page_size : 0x100000;
    c.lr = hle().thread_return();

    SLY4_INFO("thread principal 0x%" PRIX64 ": entrada 0x%08X, pilha 0x%08X (+0x%X), prioridade %d", t.id,
              img_.entry_function, t.stack_addr, t.stack_size, t.priority);
    try {
        start_modules(t);
        if (run_entry(t)) {
            SLY4_INFO("a funÃ§Ã£o de entrada da thread principal retornou (0x%" PRIX64 ")", t.exit_value);
            request_exit(int(t.exit_value));
        } else {
            SLY4_INFO("a thread principal saiu com sys_ppu_thread_exit(0x%" PRIX64 ")", t.exit_value);
        }
    } catch (const ProcessExit& e) {
        SLY4_INFO("sys_process_exit(%d)", e.status);
        request_exit(e.status);
    }
    return exit_status();
}

void Kernel::start_modules(PpuThread& t) {
    t_current = &t;
    t.host_tid = GetCurrentThreadId();
    for (const LoadedImage& m : modules_) {
        if (!m.module_start) continue;
        const PPUContext saved = t.ctx;
        t.ctx.gpr[3] = 0;  // args
        t.ctx.gpr[4] = 0;  // argp
        call_guest(t.ctx, mem_, m.module_start);
        SLY4_INFO("firmware: module_start de %s devolveu 0x%X", m.module_name.c_str(), uint32_t(t.ctx.gpr[3]));
        t.ctx = saved;
    }
    t_current = nullptr;
}

bool Kernel::run_entry(PpuThread& t) {
    t_current = &t;
    t.host_tid = GetCurrentThreadId();
    bool returned = false;
    try {
        rt::call_indirect(t.ctx, mem_, t.entry);
        t.exit_value = t.ctx.gpr[3];
        returned = true;
    } catch (const ThreadExit& e) {
        t.exit_value = e.value;
    } catch (...) {
        t_current = nullptr;
        throw;
    }
    t_current = nullptr;
    {
        std::lock_guard<std::mutex> g(t.m);
        t.done = true;
    }
    t.cv.notify_all();
    return returned;
}

void Kernel::request_exit(int status) {
    if (!exited_.exchange(true)) exit_status_ = status;
}

std::shared_ptr<PpuThread> Kernel::create_thread(GuestAddr entry_opd, uint64_t arg, int32_t prio, uint32_t stack_size,
                                                 bool joinable, std::string name, uint32_t& error) {
    auto t = std::make_shared<PpuThread>();
    t->id = next_thread_id();
    t->name = std::move(name);
    t->priority = prio;
    t->joinable = joinable;
    t->entry = mem_.read<uint32_t>(entry_opd);
    if (!alloc_stack(*t, std::max<uint32_t>(stack_size, 0x4000))) {
        error = CELL_ENOMEM;
        return nullptr;
    }
    PPUContext& c = t->ctx;
    c.gpr[1] = t->stack_addr + t->stack_size - 0x70;
    c.gpr[2] = mem_.read<uint32_t>(entry_opd + 4);
    c.gpr[3] = arg;
    c.gpr[13] = alloc_tls(*t);
    c.lr = hle().thread_return();
    std::lock_guard<std::mutex> g(threads_lock_);
    threads_.emplace(t->id, t);
    error = CELL_OK;
    return t;
}

namespace {
struct HostStart {
    std::function<void()> body;
};
DWORD WINAPI host_thread(void* p) {
    std::unique_ptr<HostStart> s(static_cast<HostStart*>(p));
    s->body();
    return 0;
}
// Recompiled guest calls nest as host calls: give each thread a large host stack.
bool start_host_thread(std::function<void()> body) {
    auto* s = new HostStart{std::move(body)};
    HANDLE h = CreateThread(nullptr, 64u << 20, host_thread, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        delete s;
        return false;
    }
    CloseHandle(h);
    return true;
}
}  // namespace

bool Kernel::start_thread(const std::shared_ptr<PpuThread>& t) {
    return start_host_thread([this, t] { thread_entry_point(t); });
}

bool Kernel::start_native_thread(const std::string& name, std::function<void(PpuThread&)> body) {
    auto t = std::make_shared<PpuThread>();
    t->id = next_thread_id();
    t->name = name;
    t->priority = 1000;
    if (!alloc_stack(*t, 0x10000)) return false;
    PPUContext& c = t->ctx;
    c.gpr[1] = t->stack_addr + t->stack_size - 0x70;
    c.gpr[2] = img_.toc;
    c.gpr[13] = alloc_tls(*t);
    {
        std::lock_guard<std::mutex> g(threads_lock_);
        threads_.emplace(t->id, t);
    }
    return start_host_thread([this, t, body = std::move(body)] {
        SetThreadDescription(GetCurrentThread(), std::wstring(t->name.begin(), t->name.end()).c_str());
        t_current = t.get();
        t->host_tid = GetCurrentThreadId();
        try {
            body(*t);
        } catch (const ProcessExit& e) {
            request_exit(e.status);
        } catch (const ThreadExit&) {
        }
        t_current = nullptr;
    });
}

void Kernel::thread_entry_point(const std::shared_ptr<PpuThread>& t) {
    SetThreadDescription(GetCurrentThread(), std::wstring(t->name.begin(), t->name.end()).c_str());
    try {
        run_entry(*t);
    } catch (const ProcessExit& e) {
        SLY4_INFO("sys_process_exit(%d) na thread %s", e.status, t->name.c_str());
        request_exit(e.status);
        return;
    }
    std::lock_guard<std::mutex> g(threads_lock_);
    if (!t->joinable) threads_.erase(t->id);
}

std::shared_ptr<PpuThread> Kernel::find_thread(uint64_t id) {
    std::lock_guard<std::mutex> g(threads_lock_);
    auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : it->second;
}

uint32_t Kernel::join_thread(uint64_t id, uint64_t& value) {
    auto t = find_thread(id);
    if (!t) return CELL_ESRCH;
    if (t.get() == current_thread()) return CELL_EDEADLK;
    {
        std::unique_lock<std::mutex> lk(t->m);
        if (!t->joinable || t->joined) return CELL_EINVAL;
        t->joined = true;
        t->cv.wait(lk, [&] { return t->done; });
        value = t->exit_value;
    }
    std::lock_guard<std::mutex> g(threads_lock_);
    threads_.erase(id);
    return CELL_OK;
}

uint32_t Kernel::detach_thread(uint64_t id) {
    auto t = find_thread(id);
    if (!t) return CELL_ESRCH;
    std::lock_guard<std::mutex> lk(t->m);
    if (!t->joinable || t->joined) return CELL_EINVAL;
    t->joinable = false;
    if (t->done) {
        std::lock_guard<std::mutex> g(threads_lock_);
        threads_.erase(id);
    }
    return CELL_OK;
}

void call_guest(PPUContext& c, GuestMemory& m, GuestAddr opd) {
    const uint64_t lr = c.lr, r1 = c.gpr[1], r2 = c.gpr[2];
    // A fresh frame below the caller's, leaving its 288-byte protected zone alone.
    c.gpr[1] = (r1 - 0x200) & ~uint64_t(0xF);
    m.write<uint64_t>(uint32_t(c.gpr[1]), r1);
    c.gpr[2] = m.read<uint32_t>(opd + 4);
    c.lr = hle().thread_return();
    rt::call_indirect(c, m, m.read<uint32_t>(opd));
    c.lr = lr;
    c.gpr[1] = r1;
    c.gpr[2] = r2;
}


// --ppu-profile: a host thread suspends the named PPU thread's host thread every ~1 ms and
// records where it is (the offset in sly4.exe, resolved with nm: the generated functions
// are named after their guest address).
namespace {
std::mutex g_prof_lock;
std::map<uint64_t, uint32_t> g_prof_hits;
uint64_t g_prof_samples = 0;
}  // namespace

void Kernel::ppu_profile_start(const std::string& name) {
    std::thread([this, name] {
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        HANDLE h = nullptr;
        uint32_t tid = 0;
        while (!exited()) {
            host_sleep_us(1000);
            if (!h) {
                std::lock_guard<std::mutex> g(threads_lock_);
                for (const auto& [id, t] : threads_)
                    if (t->name == name && t->host_tid) tid = t->host_tid;
                if (tid) h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
                if (!h) continue;
            }
            if (SuspendThread(h) == DWORD(-1)) break;  // the thread is gone
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(h, &ctx);
            ResumeThread(h);
            if (!ok) continue;
            std::lock_guard<std::mutex> g(g_prof_lock);
            ++g_prof_hits[ctx.Rip - base];
            ++g_prof_samples;
        }
        if (h) CloseHandle(h);
    }).detach();
}

void Kernel::ppu_profile_print() const {
    std::lock_guard<std::mutex> g(g_prof_lock);
    if (!g_prof_samples) return;
    std::vector<std::pair<uint32_t, uint64_t>> top;
    for (const auto& [rva, n] : g_prof_hits) top.push_back({n, rva});
    std::sort(top.rbegin(), top.rend());
    SLY4_INFO("perfil PPU: %" PRIu64 " amostras; endereços (RVA no sly4.exe) mais frequentes:", g_prof_samples);
    for (size_t i = 0; i < top.size() && i < 40; ++i)
        std::fprintf(stderr, "  0x%08" PRIX64 " %6u (%.1f%%)\n", top[i].second, top[i].first,
                     100.0 * top[i].first / double(g_prof_samples));
}

void Kernel::print_summary() const {
    {
        std::lock_guard<std::mutex> g(const_cast<std::mutex&>(threads_lock_));
        SLY4_INFO("threads: %zu", threads_.size());
        for (const auto& [id, t] : threads_) {
            const char* call = t->last_call.load();
            std::fprintf(stderr, "  0x%08" PRIX64 " %-26s %s Ãºltima chamada: %s(0x%" PRIX64 ", 0x%" PRIX64 ") de 0x%08X\n",
                         id, t->name.c_str(), t->done ? "terminada" : "ativa    ", call ? call : "-", t->last_r3.load(),
                         t->last_r4.load(), t->last_lr.load());
            if (t->done) continue;
            // Where it is now (read while it runs): LR and the return addresses saved in the
            // stack frames (back chain at 0(r1), LR at 16(r1)), for threads stuck in plain code.
            std::fprintf(stderr, "    agora: lr 0x%08X, pilha:", uint32_t(t->ctx.lr));
            uint32_t sp = uint32_t(t->ctx.gpr[1]);
            for (int i = 0; i < 8 && sp && mem_.is_committed(sp, 24); ++i) {
                const uint32_t next = mem_.read<uint32_t>(sp + 4);  // 64-bit back chain, low word
                if (next <= sp || !mem_.is_committed(next, 24)) break;
                std::fprintf(stderr, " 0x%08X", mem_.read<uint32_t>(next + 20));
                sp = next;
            }
            std::fprintf(stderr, "\n");
        }
    }
    uint64_t icalls = 0, isteps = 0;
    rt::interpreter_stats(icalls, isteps);
    if (icalls) SLY4_INFO("PPU: %" PRIu64 " entradas no interpretador, %" PRIu64 " instruções interpretadas", icalls, isteps);
    for (const auto& s : spu_threads())
        std::fprintf(stderr, "  SPU 0x%08X grupo %08X[%u] %-10s %12" PRIu64 " instruÃ§Ãµes, pc 0x%05X\n", s->id,
                     s->group ? s->group->id : 0, s->index, s->running ? "rodando" : "parada", s->executed.load(),
                     s->ctx.pc);
    // Where each SPU is: a few instructions around its PC.
    for (const auto& s : spu_threads()) {
        if (!s->running) continue;
        const uint32_t pc = s->ctx.pc;
        std::fprintf(stderr, "  SPU 0x%08X perto de 0x%05X ($3 = %08X, $4 = %08X, $5 = %08X):\n", s->id, pc,
                     s->ctx.gpr[3].pref(), s->ctx.gpr[4].pref(), s->ctx.gpr[5].pref());
        // SPURS taskset policy module context (RPCS3's SpursTasksetContext at LS 0x2700).
        std::fprintf(stderr, "    SPURS: taskset 0x%08X, tarefa %u (LS 0x27B8/0x27D4)\n", s->ctx.ls_u32(0x27BC),
                     s->ctx.ls_u32(0x27D4));
        std::fprintf(stderr, "    eventos: máscara 0x%X, pendentes 0x%X, interrupções %s; reserva %s 0x%08X%s\n",
                     s->ctx.event_mask, s->ctx.event_stat, s->ctx.irq ? "ligadas" : "desligadas",
                     s->ctx.reserved ? "em" : "nenhuma", s->ctx.reserve_ea,
                     s->ctx.reserved && std::memcmp(mem_.host(s->ctx.reserve_ea), s->ctx.reserve_data, 128) != 0
                         ? " (a linha mudou)"
                         : "");
        for (uint32_t a = (pc - 16) & kSpuLsMask; a != ((pc + 20) & kSpuLsMask); a = (a + 4) & kSpuLsMask)
            std::fprintf(stderr, "    %05X%s %s\n", a, a == pc ? " >" : "  ",
                         spu::disasm(spu::decode(s->ctx.ls_u32(a)), a).c_str());
    }
    spu_profile_print();
    ppu_profile_print();
    zlib_verify_summary();
    gcm_summary();
    audio_summary();
    SLY4_INFO("SPU: %zu blocos compilados", spu_block_count());
    if (!spu_cache_.empty()) {
        const size_t n = spu_cache_size();
        if (spu_cache_save(spu_cache_))
            SLY4_INFO("cache SPU: %zu blocos novos interpretados, gravados em %s", n, spu_cache_.c_str());
        else
            SLY4_ERROR("cache SPU: nÃ£o foi possÃ­vel gravar %s", spu_cache_.c_str());
    }
    call_log().print_ring(ring_lines_);
    auto hs = hle().stats();
    std::stable_sort(hs.begin(), hs.end(), [](const auto& a, const auto& b) { return a.calls > b.calls; });
    size_t called = 0, missing = 0;
    for (const auto& s : hs) {
        if (!s.calls) continue;
        ++called;
        missing += !s.implemented;
    }
    SLY4_INFO("HLE: %zu funÃ§Ãµes chamadas, %zu delas sem implementaÃ§Ã£o", called, missing);
    for (const auto& s : hs)
        if (s.calls)
            std::fprintf(stderr, "  %10" PRIu64 "  %s.%s%s\n", s.calls, s.module.c_str(), s.name.c_str(),
                         s.implemented ? "" : "  [sem implementaÃ§Ã£o]");
    auto ss = syscall_stats();
    std::stable_sort(ss.begin(), ss.end(), [](const auto& a, const auto& b) { return a.calls > b.calls; });
    SLY4_INFO("syscalls: %zu nÃºmeros chamados", ss.size());
    for (const auto& s : ss) {
        const char* n = syscall_name(s.number);
        std::fprintf(stderr, "  %10" PRIu64 "  %u %s%s\n", s.calls, s.number, n ? n : "?",
                     s.implemented ? "" : "  [sem implementaÃ§Ã£o]");
    }
    std::fflush(stderr);
}

}  // namespace sly4
