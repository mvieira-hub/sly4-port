#include "loader/image_report.h"

#include <cstdio>

#include "core/log.h"

namespace sly4 {

void log_image_summary(const LoadedImage& img, const NidNames& names) {
    SLY4_INFO("EBOOT: entrada 0x%08X -> função 0x%08X, TOC (r2) 0x%08X", img.entry_descriptor, img.entry_function,
              img.toc);
    for (const auto& s : img.segments)
        SLY4_INFO("  segmento 0x%08X-0x%08X arquivo 0x%X memória 0x%X flags 0x%08X%s", s.vaddr, s.vaddr + s.mem_size,
                  s.file_size, s.mem_size, s.flags, (s.flags & 2) ? "" : " (código, somente leitura)");
    if (img.tls.mem_size)
        SLY4_INFO("  TLS: imagem 0x%08X, 0x%X bytes iniciais, 0x%X no total, alinhamento %u", img.tls.image,
                  img.tls.file_size, img.tls.mem_size, img.tls.align);
    if (img.param.present)
        // sdk_version is BCD-like: 0x00410001 = SDK 4.10 (major in bits 20-23, minor in 12-19).
        SLY4_INFO("  processo: SDK %X.%02X (0x%08X), prioridade %d, pilha 0x%X, página de malloc 0x%X",
                  (img.param.sdk_version >> 20) & 0xF, (img.param.sdk_version >> 12) & 0xFF, img.param.sdk_version,
                  img.param.primary_prio,
                  img.param.primary_stack_size, img.param.malloc_page_size);
    size_t named = 0;
    for (const auto& m : img.imports)
        for (const auto& fn : m.functions) named += names.find(fn.nid) != nullptr;
    SLY4_INFO("  importações: %zu funções de %zu módulos (%zu com nome), .lib.stub 0x%08X-0x%08X",
              img.imported_function_count(), img.imports.size(), named, img.libstub_start, img.libstub_end);
    for (const auto& m : img.imports) {
        size_t n = 0;
        for (const auto& fn : m.functions) n += names.find(fn.nid) != nullptr;
        SLY4_INFO("    %-22s %3zu funções (%zu com nome)%s", m.name.c_str(), m.functions.size(), n,
                  m.num_variables || m.num_tls ? " + variáveis importadas!" : "");
    }
}

bool write_imports_csv(const LoadedImage& img, const NidNames& names, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "module,nid,name,slot\n");
    for (const auto& m : img.imports) {
        for (const auto& fn : m.functions) {
            const std::string* name = names.find(fn.nid);
            std::fprintf(f, "%s,0x%08X,%s,0x%08X\n", m.name.c_str(), fn.nid, name ? name->c_str() : "", fn.slot);
        }
    }
    std::fclose(f);
    return true;
}

}  // namespace sly4
