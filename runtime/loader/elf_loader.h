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

// ELF section header. RPCS3's decrypted EBOOT keeps the section table but not the
// name table, so sections are told apart by flags and by what they contain.
struct ElfSection {
    GuestAddr addr = 0;
    uint32_t size = 0;
    uint32_t type = 0;   // SHT_PROGBITS = 1, SHT_NOBITS = 8
    uint64_t flags = 0;  // SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4
    bool executable() const { return (flags & 4) && type == 1 && size; }
};

// A library a PRX module exports: NID -> address of the function's descriptor.
struct ExportedLibrary {
    std::string name;
    std::vector<std::pair<uint32_t, GuestAddr>> functions;
};

struct LoadedImage {
    std::string module_name;          // PRX modules only ("cellSpurs_Library"...)
    std::vector<ExportedLibrary> exports;
    GuestAddr module_start = 0, module_stop = 0;  // PRX: descriptors of module_start/stop (0 if none)
    // Function entries known without an .opd section (a PRX's descriptors).
    std::vector<GuestAddr> function_entries;

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
    std::vector<ElfSection> sections;
    // .opd: the section holding the entry descriptor; 8-byte {function, toc} records.
    GuestAddr opd_start = 0, opd_end = 0;

    size_t imported_function_count() const;
    const ElfSection* section_containing(GuestAddr a) const;
};

// Loads `path` into `mem` (which must be reserved). On failure returns false and
// sets `error`; segments already committed are left in place.
bool load_elf(const std::string& path, GuestMemory& mem, LoadedImage& out, std::string& error);

// Reads 0x2C-byte import records (sys_stub) from [start, end) into `out`.
bool read_import_table(const GuestMemory& mem, GuestAddr start, GuestAddr end, std::vector<ImportedModule>& out,
                       std::string& error);

// Loads a decrypted PRX module (ELF64, type 0xFFA4) with its segments at `base` +
// their virtual address, applies its SCE relocations and reads its library info,
// exports and imports. module_start is the descriptor of the special export 0xBC9A0086.
bool load_prx(const std::string& path, GuestMemory& mem, GuestAddr base, LoadedImage& out, std::string& error);

}  // namespace sly4
