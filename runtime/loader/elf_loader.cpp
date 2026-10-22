#include "loader/elf_loader.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/endian.h"

namespace sly4 {

namespace {

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_TLS = 7;
constexpr uint32_t PT_PROC_PARAM = 0x60000001;
constexpr uint32_t PT_PROC_PRX_PARAM = 0x60000002;
constexpr uint32_t PF_W = 2;
constexpr uint32_t kProcParamMagic = 0x13BCC5F6;
constexpr uint32_t kPrxParamMagic = 0x1B434CEC;
constexpr uint16_t EM_PPC64 = 21;
constexpr uint16_t ET_EXEC = 2;
constexpr uint32_t kStubRecordSize = 0x2C;

struct ProgramHeader {
    uint32_t type, flags;
    uint64_t offset, vaddr, file_size, mem_size, align;
};

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// sys_stub (.lib.stub record, 0x2C bytes):
//   +0x00 u8 size  +0x02 u16 version  +0x04 u16 attributes
//   +0x06 u16 num_func  +0x08 u16 num_var  +0x0A u16 num_tlsvar
//   +0x10 module name  +0x14 function NID table  +0x18 function slot table
//   +0x1C variable NIDs  +0x20 variable slots  +0x24 TLS NIDs  +0x28 TLS slots
}  // namespace

// The same 0x2C-byte records describe the imports of an executable (.lib.stub) and of a
// PRX module (library info imports_start..imports_end).
bool read_import_table(const GuestMemory& mem, GuestAddr start, GuestAddr end, std::vector<ImportedModule>& out,
                       std::string& error) {
    for (GuestAddr a = start; a + kStubRecordSize <= end;) {
        const uint8_t size = mem.read<uint8_t>(a);
        if (size < kStubRecordSize) {
            error = "registro .lib.stub com tamanho inválido em " + hex(a);
            return false;
        }
        ImportedModule m;
        m.stub_entry = a;
        m.version = mem.read<uint16_t>(a + 0x02);
        const uint16_t num_func = mem.read<uint16_t>(a + 0x06);
        m.num_variables = mem.read<uint16_t>(a + 0x08);
        m.num_tls = mem.read<uint16_t>(a + 0x0A);
        const GuestAddr name = mem.read<uint32_t>(a + 0x10);
        const GuestAddr nids = mem.read<uint32_t>(a + 0x14);
        const GuestAddr slots = mem.read<uint32_t>(a + 0x18);
        m.name = mem.read_cstring(name, 128);
        if (num_func && (!mem.is_committed(nids, 4u * num_func) || !mem.is_committed(slots, 4u * num_func))) {
            error = "tabelas de importação de " + m.name + " fora da imagem";
            return false;
        }
        for (uint16_t i = 0; i < num_func; ++i)
            m.functions.push_back({mem.read<uint32_t>(nids + 4u * i), slots + 4u * i});
        out.push_back(std::move(m));
        a += size;
    }
    return true;
}

size_t LoadedImage::imported_function_count() const {
    size_t n = 0;
    for (const auto& m : imports) n += m.functions.size();
    return n;
}

const ElfSection* LoadedImage::section_containing(GuestAddr a) const {
    for (const auto& s : sections)
        if (s.size && a >= s.addr && a - s.addr < s.size) return &s;
    return nullptr;
}

bool load_elf(const std::string& path, GuestMemory& mem, LoadedImage& out, std::string& error) {
    out = LoadedImage{};
    if (!mem.reserved()) {
        error = "memória do convidado não reservada";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "não foi possível abrir " + path;
        return false;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint8_t* b = file.data();
    auto in_file = [&](uint64_t off, uint64_t size) { return off <= file.size() && size <= file.size() - off; };

    if (file.size() < 0x40 || std::memcmp(b, "\x7F" "ELF", 4) != 0) {
        error = "não é um ELF (o EBOOT.BIN precisa ser descriptografado para EBOOT.elf)";
        return false;
    }
    if (b[4] != 2 || b[5] != 2) {
        error = "esperado ELF de 64 bits big-endian";
        return false;
    }
    if (load_be<uint16_t>(b + 0x10) != ET_EXEC || load_be<uint16_t>(b + 0x12) != EM_PPC64) {
        error = "esperado executável PowerPC64 (ET_EXEC, EM_PPC64)";
        return false;
    }
    const uint64_t entry = load_be<uint64_t>(b + 0x18);
    const uint64_t phoff = load_be<uint64_t>(b + 0x20);
    const uint16_t phentsize = load_be<uint16_t>(b + 0x36);
    const uint16_t phnum = load_be<uint16_t>(b + 0x38);
    if (phentsize < 0x38 || !in_file(phoff, uint64_t(phentsize) * phnum)) {
        error = "tabela de program headers inválida";
        return false;
    }

    std::vector<ProgramHeader> phdrs;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* p = b + phoff + uint64_t(i) * phentsize;
        phdrs.push_back({load_be<uint32_t>(p), load_be<uint32_t>(p + 4), load_be<uint64_t>(p + 8),
                         load_be<uint64_t>(p + 16), load_be<uint64_t>(p + 32), load_be<uint64_t>(p + 40),
                         load_be<uint64_t>(p + 48)});
    }

    // Segments first: everything else is read back from guest memory.
    for (const auto& ph : phdrs) {
        if (ph.type != PT_LOAD || ph.mem_size == 0) continue;  // the EBOOT has three empty PT_LOADs
        if (ph.vaddr + ph.mem_size > GuestMemory::kSize || ph.file_size > ph.mem_size ||
            !in_file(ph.offset, ph.file_size)) {
            error = "segmento PT_LOAD inválido em " + hex(ph.vaddr);
            return false;
        }
        const auto va = static_cast<GuestAddr>(ph.vaddr);
        const auto msz = static_cast<uint32_t>(ph.mem_size);
        if (!mem.commit(va, msz, Protection::ReadWrite)) {
            error = "commit do segmento " + hex(va) + " falhou";
            return false;
        }
        mem.copy_to(va, b + ph.offset, ph.file_size);
        out.segments.push_back({va, static_cast<uint32_t>(ph.file_size), msz, ph.flags});
        out.image_end = std::max<GuestAddr>(out.image_end, va + msz);
    }
    if (out.segments.empty()) {
        error = "nenhum segmento PT_LOAD";
        return false;
    }

    for (const auto& ph : phdrs) {
        if (ph.type == PT_TLS) {
            out.tls = {static_cast<GuestAddr>(ph.vaddr), static_cast<uint32_t>(ph.file_size),
                       static_cast<uint32_t>(ph.mem_size), static_cast<uint32_t>(ph.align)};
        } else if (ph.type == PT_PROC_PARAM && ph.file_size >= 0x20 && in_file(ph.offset, 0x20)) {
            const uint8_t* p = b + ph.offset;
            if (load_be<uint32_t>(p + 4) != kProcParamMagic) {
                error = "sys_process_param com magic inválido";
                return false;
            }
            out.param.present = true;
            out.param.version = load_be<uint32_t>(p + 0x08);
            out.param.sdk_version = load_be<uint32_t>(p + 0x0C);
            out.param.primary_prio = load_be<int32_t>(p + 0x10);
            out.param.primary_stack_size = load_be<uint32_t>(p + 0x14);
            out.param.malloc_page_size = load_be<uint32_t>(p + 0x18);
            out.param.ppc_seg = load_be<uint32_t>(p + 0x1C);
        } else if (ph.type == PT_PROC_PRX_PARAM && ph.file_size >= 0x20 && in_file(ph.offset, 0x20)) {
            const uint8_t* p = b + ph.offset;
            if (load_be<uint32_t>(p + 4) != kPrxParamMagic) {
                error = "sys_process_prx_param com magic inválido";
                return false;
            }
            out.libent_start = load_be<uint32_t>(p + 0x10);
            out.libent_end = load_be<uint32_t>(p + 0x14);
            out.libstub_start = load_be<uint32_t>(p + 0x18);
            out.libstub_end = load_be<uint32_t>(p + 0x1C);
        }
    }

    // The ELF entry is a function descriptor: {code address, TOC}.
    out.entry_descriptor = static_cast<GuestAddr>(entry);
    if (!mem.is_committed(out.entry_descriptor, 8)) {
        error = "descritor de entrada " + hex(entry) + " fora da imagem";
        return false;
    }
    out.entry_function = mem.read<uint32_t>(out.entry_descriptor);
    out.toc = mem.read<uint32_t>(out.entry_descriptor + 4);

    // Section headers (optional for loading, needed by the recompiler's analysis).
    const uint64_t shoff = load_be<uint64_t>(b + 0x28);
    const uint16_t shentsize = load_be<uint16_t>(b + 0x3A);
    const uint16_t shnum = load_be<uint16_t>(b + 0x3C);
    if (shoff && shentsize >= 0x40 && in_file(shoff, uint64_t(shentsize) * shnum)) {
        for (uint16_t i = 0; i < shnum; ++i) {
            const uint8_t* s = b + shoff + uint64_t(i) * shentsize;
            ElfSection sec;
            sec.type = load_be<uint32_t>(s + 0x04);
            sec.flags = load_be<uint64_t>(s + 0x08);
            sec.addr = static_cast<GuestAddr>(load_be<uint64_t>(s + 0x10));
            sec.size = static_cast<uint32_t>(load_be<uint64_t>(s + 0x20));
            out.sections.push_back(sec);
        }
    }
    if (const ElfSection* opd = out.section_containing(out.entry_descriptor); opd && !opd->executable()) {
        out.opd_start = opd->addr;
        out.opd_end = opd->addr + opd->size;
    }

    if (out.libstub_end > out.libstub_start) {
        if (!mem.is_committed(out.libstub_start, out.libstub_end - out.libstub_start)) {
            error = ".lib.stub fora da imagem";
            return false;
        }
        if (!read_import_table(mem, out.libstub_start, out.libstub_end, out.imports, error)) return false;
    }

    // Code segments become read-only on the host: a write to them is a bug worth a fault.
    for (const auto& s : out.segments)
        if (!(s.flags & PF_W)) mem.protect(s.vaddr, s.mem_size, Protection::ReadOnly);
    return true;
}

}  // namespace sly4
