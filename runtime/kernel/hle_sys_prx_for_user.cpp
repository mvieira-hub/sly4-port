// sysPrxForUser: the user-side half of the kernel (process, TLS, PPU thread helpers,
// lightweight mutex/cond, mmapper wrappers, PRX loading).
#include <chrono>
#include <cinttypes>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"

namespace sly4 {

namespace {

// sys_initialize_tls(thread_id, tls_image, file_size, mem_size): called by the game's
// crt0 with the PT_TLS values the kernel passed in r7..r10; sets up r13.
void sys_initialize_tls(PPUContext& c, GuestMemory&) {
    Kernel& k = kernel();
    k.set_tls_template(uint32_t(c.gpr[4]), uint32_t(c.gpr[5]), uint32_t(c.gpr[6]));
    PpuThread* t = current_thread();
    if (!t) return;
    if (!t->tls_block) {
        const uint64_t r13 = k.alloc_tls(*t);
        if (!r13) SLY4_ERROR("sys_initialize_tls: sem memória para o TLS");
        c.gpr[13] = r13;
    }
}

void sys_process_exit(PPUContext& c, GuestMemory&) { throw ProcessExit{int32_t(c.gpr[3])}; }
void sys_ppu_thread_exit(PPUContext& c, GuestMemory&) { throw ThreadExit{c.gpr[3]}; }

// Registers a function the system runs when the process spawns a new one; nothing to do.
void ignore_ok(PPUContext& c, GuestMemory&) { c.gpr[3] = CELL_OK; }

void sys_time_get_system_time(PPUContext& c, GuestMemory&) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    c.gpr[3] = uint64_t(duration_cast<microseconds>(steady_clock::now() - t0).count());
}

void sys_ppu_thread_get_id(PPUContext& c, GuestMemory& m) {
    PpuThread* t = current_thread();
    m.write<uint64_t>(uint32_t(c.gpr[3]), t ? t->id : 0);
    c.gpr[3] = CELL_OK;
}

void sys_process_is_stack(PPUContext& c, GuestMemory&) {
    const uint32_t a = uint32_t(c.gpr[3]);
    c.gpr[3] = a - layout::kStackBase < layout::kStackSize ? 1 : 0;
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
                {"sys_ppu_thread_get_id", sys_ppu_thread_get_id},
                {"sys_ppu_thread_exit", sys_ppu_thread_exit},
                {"sys_process_is_stack", sys_process_is_stack},
            }};
}

}  // namespace sly4
