// Loads the decrypted PS3 executable (EBOOT.elf: ELF64, big-endian, PowerPC64 with
// 32-bit addresses) into guest memory and reads what the runtime needs from it:
// entry descriptor and TOC, TLS image, process parameters and the import table.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "memory/guest_memory.h"

namespace sly4 {

struct ElfSegment {
    GuestAddr vaddr = 0;
    uint32_t file_size = 0;
    uint32_t mem_size = 0;  // > file_size: the rest is .bss (zeroed)
    uint32_t flags = 0;     // PF_R/PF_W/PF_X in the low bits; SPU/RSX access bits above
};

// PT_TLS: the initial image of thread-local storage, copied per thread by
// sys_initialize_tls (phase 3).
struct TlsTemplate {
    GuestAddr image = 0;
    uint32_t file_size = 0;
    uint32_t mem_size = 0;
    uint32_t align = 0;
};

// PT 0x60000001, sys_process_param_t.
struct ProcessParam {
    bool present = false;
    uint32_t version = 0;
    uint32_t sdk_version = 0;  // 0x00410001 = SDK 4.10
    int32_t primary_prio = 0;
    uint32_t primary_stack_size = 0;
    uint32_t malloc_page_size = 0;
    uint32_t ppc_seg = 0;
};

struct ImportedFunction {
    uint32_t nid = 0;
    // Word the PS3 loader fills with the address of the export's function descriptor;
    // the import stub loads it and branches there. The HLE layer fills it in phase 3.
    GuestAddr slot = 0;
};

struct ImportedModule {
    std::string name;
    GuestAddr stub_entry = 0;  // this module's sys_stub record in .lib.stub
    uint16_t version = 0;
    uint16_t num_variables = 0;  // imported variables (none expected)
    uint16_t num_tls = 0;
    std::vector<ImportedFunction> functions;
};

struct LoadedImage {
    GuestAddr entry_descriptor = 0;  // e_entry points to an .opd descriptor {function, toc}
    GuestAddr entry_function = 0;
    GuestAddr toc = 0;               // r2 for the whole executable
    std::vector<ElfSegment> segments;
    GuestAddr image_end = 0;         // end of the highest segment (memsz)
    TlsTemplate tls;
    ProcessParam param;
    GuestAddr libent_start = 0, libent_end = 0;   // exports (empty for a game)
    GuestAddr libstub_start = 0, libstub_end = 0;  // imports
    std::vector<ImportedModule> imports;

    size_t imported_function_count() const;
};

// Loads `path` into `mem` (which must be reserved). On failure returns false and
// sets `error`; segments already committed are left in place.
bool load_elf(const std::string& path, GuestMemory& mem, LoadedImage& out, std::string& error);

}  // namespace sly4
