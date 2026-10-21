#include "kernel/kernel.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "core/log.h"
#include "cpu/ppu_rt.h"
#include "kernel/hle.h"
#include "loader/elf_loader.h"
#include "loader/nid.h"

namespace sly4 {

namespace {

Kernel* g_kernel = nullptr;
thread_local PpuThread* t_current = nullptr;

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void on_hle(PPUContext& c, GuestMemory& m, uint32_t target) { hle().call(c, m, target); }

}  // namespace

PpuThread* current_thread() { return t_current; }

Kernel& kernel() { return *g_kernel; }

Kernel::Kernel(GuestMemory& mem, const LoadedImage& img) : mem_(mem), img_(img) { g_kernel = this; }

Kernel::~Kernel() {
    if (g_kernel == this) g_kernel = nullptr;
}

bool Kernel::init(const NidNames& names, const KernelOptions& opt) {
    const GuestAddr main_base = align_up(img_.image_end, 0x10000);
    main_ = std::make_unique<GuestAllocator>(mem_, main_base, layout::kMainEnd - main_base, "principal");
    user_ = std::make_unique<GuestAllocator>(mem_, layout::kUserBase, layout::kUserSize, "usuário");
    stacks_ = std::make_unique<GuestAllocator>(mem_, layout::kStackBase, layout::kStackSize, "pilhas");

    call_log().trace = opt.trace;
    HleTable& table = hle();
    table.add(hle_sys_prx_for_user());
    if (!table.bind(mem_, *main_, img_, names)) return false;

    rt::Hooks& h = rt::hooks();
    h.syscall = syscall_dispatch;
    h.hle = on_hle;
    h.hle_base = table.thunk_base();
    h.hle_size = table.thunk_bytes();
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
    PpuThread t;
    t.id = next_thread_id();
    t.name = "main_thread";
    t.priority = img_.param.present ? img_.param.primary_prio : 1001;
    uint32_t stack = img_.param.present ? img_.param.primary_stack_size : 0x100000;
    stack = align_up(std::clamp<uint32_t>(stack, 0x10000, 0x100000), 0x1000);
    if (!alloc_stack(t, stack)) {
        SLY4_ERROR("não foi possível criar a pilha da thread principal");
        return -1;
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
    t_current = &t;
    int status = 0;
    try {
        rt::call_indirect(c, mem_, img_.entry_function);
        SLY4_INFO("a função de entrada retornou");
    } catch (const ProcessExit& e) {
        status = e.status;
        SLY4_INFO("sys_process_exit(%d)", status);
    } catch (const ThreadExit& e) {
        SLY4_INFO("a thread principal saiu com sys_ppu_thread_exit(0x%" PRIX64 ")", e.value);
    }
    t_current = nullptr;
    return status;
}

void Kernel::print_summary() const {
    auto hs = hle().stats();
    std::stable_sort(hs.begin(), hs.end(), [](const auto& a, const auto& b) { return a.calls > b.calls; });
    size_t called = 0, missing = 0;
    for (const auto& s : hs) {
        if (!s.calls) continue;
        ++called;
        missing += !s.implemented;
    }
    SLY4_INFO("HLE: %zu funções chamadas, %zu delas sem implementação", called, missing);
    for (const auto& s : hs)
        if (s.calls)
            std::fprintf(stderr, "  %10" PRIu64 "  %s.%s%s\n", s.calls, s.module.c_str(), s.name.c_str(),
                         s.implemented ? "" : "  [sem implementação]");
    auto ss = syscall_stats();
    std::stable_sort(ss.begin(), ss.end(), [](const auto& a, const auto& b) { return a.calls > b.calls; });
    SLY4_INFO("syscalls: %zu números chamados", ss.size());
    for (const auto& s : ss) {
        const char* n = syscall_name(s.number);
        std::fprintf(stderr, "  %10" PRIu64 "  %u %s%s\n", s.calls, s.number, n ? n : "?",
                     s.implemented ? "" : "  [sem implementação]");
    }
    std::fflush(stderr);
}

}  // namespace sly4
