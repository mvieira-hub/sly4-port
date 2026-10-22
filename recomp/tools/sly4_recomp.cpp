// sly4_recomp.exe: static recompiler toolkit for the PS3 executable (phase 2).
//
//   sly4_recomp disasm --eboot <EBOOT.elf> [--start 0x10230] [--end 0xABD560] [--out file]
//       Decodes every word of the range and prints "addr:\tword\tinstruction" in the
//       syntax of objdump -M cell,raw, to be compared with binutils
//       (tools/ppc/compare_disasm.py). Default range: the EBOOT .text section.
//   sly4_recomp analyze --eboot <EBOOT.elf> --out-dir <dir>
//       Control-flow analysis (functions, blocks, jump tables, code vs data); writes
//       functions.csv, jumptables.csv, wordmap.csv, noreturn.csv and invalid.csv.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <random>

#include "analysis/program.h"
#include "core/log.h"
#include "emit/ppu_emitter.h"
#include "emit/spu_emitter.h"
#include "loader/elf_loader.h"
#include "loader/nid.h"
#include "oracle.h"
#include "spu_oracle.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"
#include "loader/spu_elf.h"
#include "spu/spu_decoder.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int usage() {
    std::fprintf(stderr,
                 "uso: sly4_recomp disasm  --eboot <EBOOT.elf> [--start 0x10230] [--end 0xABD560] [--out arquivo]\n"
                 "     sly4_recomp analyze --eboot <EBOOT.elf> --out-dir <pasta> [--nid-names arquivo]\n"
                 "     sly4_recomp emit    --eboot <EBOOT.elf> --out-dir <pasta> [--sample N --seed S] [--per-file 300]\n"
                 "     sly4_recomp oracle-gen / oracle-check (ver docs/RECOMP.md)\n"
                 "     sly4_recomp spu-disasm --elf <programa SPU .elf> [--out arquivo]\n"
                 "     sly4_recomp spu-oracle --spu-elf <tarefa EDGE zlib .elf> --psarc <Sly4.psarc> [--blocks 200]\n"
                 "     sly4_recomp spu-emit [--cache work\\spu_cache.bin] [--elf programa.elf ...] --out-dir generated\\spu\n"
                 "     sly4_recomp psarc-hash --psarc <Sly4.psarc> --out arquivo.txt\n");
    return 2;
}

bool load(const std::string& eboot, sly4::GuestMemory& mem, sly4::LoadedImage& img) {
    std::string error;
    if (!mem.reserve() || !sly4::load_elf(eboot, mem, img, error)) {
        SLY4_ERROR("falha ao carregar %s: %s", eboot.c_str(), error.c_str());
        return false;
    }
    return true;
}

// The image analysed/emitted: the game's EBOOT (--eboot) or a firmware PRX module
// (--prx, loaded at --base, the same address the runtime loads it at).
struct ImageArgs {
    std::string eboot, prx;
    uint32_t base = 0;
    bool parse(int& i, int argc, char** argv) {
        if (i + 1 >= argc) return false;
        if (std::strcmp(argv[i], "--eboot") == 0) eboot = argv[++i];
        else if (std::strcmp(argv[i], "--prx") == 0) prx = argv[++i];
        else if (std::strcmp(argv[i], "--base") == 0) base = std::strtoul(argv[++i], nullptr, 0);
        else return false;
        return true;
    }
    bool empty() const { return eboot.empty() && (prx.empty() || !base); }
    bool load(sly4::GuestMemory& mem, sly4::LoadedImage& img) const {
        if (!eboot.empty()) return ::load(eboot, mem, img);
        std::string error;
        if (!mem.reserve() || !sly4::load_prx(prx, mem, base, img, error)) {
            SLY4_ERROR("falha ao carregar %s: %s", prx.c_str(), error.c_str());
            return false;
        }
        SLY4_INFO("%s: módulo %s em 0x%08X, TOC 0x%08X, %zu bibliotecas exportadas, %zu importadas, %zu funções conhecidas",
                  prx.c_str(), img.module_name.c_str(), base, img.toc, img.exports.size(), img.imports.size(),
                  img.function_entries.size());
        for (const auto& lib : img.exports) SLY4_INFO("  exporta %s: %zu funções", lib.name.c_str(), lib.functions.size());
        for (const auto& m : img.imports) SLY4_INFO("  importa %s: %zu funções", m.name.c_str(), m.functions.size());
        return true;
    }
};

FILE* open_out(const std::filesystem::path& p) {
    FILE* f = std::fopen(p.string().c_str(), "w");
    if (!f) SLY4_ERROR("não foi possível criar %s", p.string().c_str());
    return f;
}

int cmd_analyze(int argc, char** argv) {
    ImageArgs image;
    std::string out_dir, nid_file = "tools/data/ps3_nid_candidates.txt";
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (image.parse(i, argc, argv)) continue;
        else if (std::strcmp(argv[i], "--out-dir") == 0) out_dir = argv[++i];
        else if (std::strcmp(argv[i], "--nid-names") == 0) nid_file = argv[++i];
        else return usage();
    }
    if (image.empty() || out_dir.empty()) return usage();
    sly4::GuestMemory mem;
    sly4::LoadedImage img;
    if (!image.load(mem, img)) return 3;
    sly4::NidNames names;
    names.load_file(nid_file);

    const auto t0 = std::chrono::steady_clock::now();
    const sly4::recomp::Program prog = sly4::recomp::analyze(mem, img);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    using sly4::recomp::FuncKind;
    using sly4::recomp::Term;
    const auto& st = prog.stats;
    size_t code_funcs = 0, stubs = 0, opd = 0, blocks = 0, calls = 0, tails = 0, indirect = 0;
    std::map<Term, size_t> terms;
    for (const auto& [e, f] : prog.functions) {
        (f.kind == FuncKind::ImportStub ? stubs : code_funcs)++;
        opd += f.from_opd;
        blocks += f.blocks.size();
        calls += f.calls.size();
        tails += f.tail_calls.size();
        indirect += f.indirect_calls;
        for (const auto& b : f.blocks) ++terms[b.term];
    }
    SLY4_INFO("análise em %.2f s, %u passadas", secs, st.passes);
    SLY4_INFO("funções: %zu de código (%zu pelo .opd, %zu só por chamada) + %zu stubs de importação", code_funcs,
              opd, code_funcs - opd, stubs);
    SLY4_INFO("blocos: %zu; chamadas diretas: %zu; tail calls: %zu; chamadas indiretas: %zu", blocks, calls, tails,
              indirect);
    std::string tl;
    for (auto [t, n] : terms) tl += std::string(" ") + sly4::recomp::term_name(t) + "=" + std::to_string(n);
    SLY4_INFO("fim de bloco:%s", tl.c_str());
    SLY4_INFO("tabelas de salto: %u (%u com limite pelo cmpli; base da TOC diferente de bctr+4: %u)", st.jump_tables,
              st.bounded_tables, st.toc_base_mismatch);
    SLY4_INFO("palavras executáveis: %u; não nulas: %u; código: %u (%.2f%% das não nulas); tabelas: %u",
              st.exec_words, st.nonzero_words, st.code_words, 100.0 * st.code_words / std::max(1u, st.nonzero_words),
              st.table_words);
    SLY4_INFO("instruções em mais de uma função: %u; chamadas sem retorno: %u; inválidas alcançadas: %u",
              st.overlapping_instructions, st.noreturn_calls, st.invalid_reached);

    std::filesystem::create_directories(out_dir);
    const std::filesystem::path dir(out_dir);
    if (FILE* f = open_out(dir / "functions.csv")) {
        std::fprintf(f, "entry,lo,hi,kind,opd,instructions,blocks,calls,tail_calls,indirect_calls,tables,name\n");
        for (const auto& [e, fn] : prog.functions) {
            std::string name;
            if (fn.kind == FuncKind::ImportStub) {
                const std::string* n = names.find(fn.import_nid);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "nid_%08X", fn.import_nid);
                name = fn.import_module + "::" + (n ? *n : std::string(buf));
            }
            std::fprintf(f, "0x%08X,0x%08X,0x%08X,%s,%d,%u,%zu,%zu,%zu,%u,%zu,%s\n", e, fn.lo, fn.hi,
                         fn.kind == FuncKind::ImportStub ? "stub" : "code", fn.from_opd ? 1 : 0, fn.instructions,
                         fn.blocks.size(), fn.calls.size(), fn.tail_calls.size(), fn.indirect_calls,
                         fn.tables.size(), name.c_str());
        }
        std::fclose(f);
    }
    if (FILE* f = open_out(dir / "jumptables.csv")) {
        std::fprintf(f, "function,bctr,base,count,bounded,toc_base,targets\n");
        for (const auto& [e, fn] : prog.functions)
            for (const auto& jt : fn.tables)
                std::fprintf(f, "0x%08X,0x%08X,0x%08X,%u,%d,%d,%zu\n", e, jt.bcctr, jt.base, jt.count, jt.bounded,
                             jt.toc_base, std::set<uint32_t>(jt.targets.begin(), jt.targets.end()).size());
        std::fclose(f);
    }
    // Runs of words by class: code / table / unreached (only non-zero unreached runs listed).
    if (FILE* f = open_out(dir / "wordmap.csv")) {
        std::fprintf(f, "start,end,class,words,nonzero,decodable\n");
        for (auto [lo, hi] : prog.exec_ranges) {
            uint32_t a = lo;
            while (a < hi) {
                const uint8_t cls = prog.class_of(a);
                uint32_t b = a, nonzero = 0, valid = 0;
                while (b < hi && prog.class_of(b) == cls) {
                    const uint32_t w = mem.read<uint32_t>(b);
                    nonzero += w != 0;
                    valid += sly4::ppu::decode(w).op != sly4::ppu::Op::invalid;
                    b += 4;
                }
                if (cls != 0 || nonzero)
                    std::fprintf(f, "0x%08X,0x%08X,%s,%u,%u,%u\n", a, b, cls == 1 ? "code" : cls == 2 ? "table" : "unreached",
                                 (b - a) / 4, nonzero, valid);
                a = b;
            }
        }
        std::fclose(f);
    }
    if (FILE* f = open_out(dir / "noreturn.csv")) {
        std::fprintf(f, "call,callee\n");
        for (auto [call, callee] : prog.noreturn_sites) std::fprintf(f, "0x%08X,0x%08X\n", call, callee);
        std::fclose(f);
    }
    if (FILE* f = open_out(dir / "invalid.csv")) {
        std::fprintf(f, "address,function\n");
        for (auto [a, fn] : prog.invalid_sites) std::fprintf(f, "0x%08X,0x%08X\n", a, fn);
        std::fclose(f);
    }
    SLY4_INFO("CSVs gravados em %s", out_dir.c_str());
    return st.invalid_reached ? 1 : 0;
}

int cmd_emit(int argc, char** argv) {
    ImageArgs image;
    std::string out_dir, name;
    size_t sample = 0, per_file = 300;
    uint64_t seed = 1;
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (image.parse(i, argc, argv)) continue;
        else if (std::strcmp(argv[i], "--out-dir") == 0) out_dir = argv[++i];
        else if (std::strcmp(argv[i], "--name") == 0) name = argv[++i];
        else if (std::strcmp(argv[i], "--sample") == 0) sample = std::strtoull(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--seed") == 0) seed = std::strtoull(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--per-file") == 0) per_file = std::strtoull(argv[++i], nullptr, 0);
        else return usage();
    }
    if (image.empty() || out_dir.empty()) return usage();
    sly4::GuestMemory mem;
    sly4::LoadedImage img;
    if (!image.load(mem, img)) return 3;
    const auto t0 = std::chrono::steady_clock::now();
    const sly4::recomp::Program prog = sly4::recomp::analyze(mem, img);

    sly4::recomp::EmitOptions opt;
    opt.out_dir = out_dir;
    opt.functions_per_file = per_file;
    opt.name = name;
    if (sample) {  // deterministic sample of code functions (stubs are reached indirectly)
        std::vector<uint32_t> code;
        for (const auto& [e, f] : prog.functions)
            if (f.kind == sly4::recomp::FuncKind::Code) code.push_back(e);
        std::mt19937_64 rng(seed);
        std::shuffle(code.begin(), code.end(), rng);
        code.resize(std::min(sample, code.size()));
        std::sort(code.begin(), code.end());
        opt.only = code;
    }
    sly4::recomp::EmitStats st;
    std::string error;
    if (!sly4::recomp::emit_program(prog, mem, opt, st, error)) {
        SLY4_ERROR("%s", error.c_str());
        return 3;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%zu funções, %zu blocos, %zu instruções em %zu arquivos (%s) em %.1f s", st.functions, st.blocks,
              st.instructions, st.files, out_dir.c_str(), secs);
    return 0;
}

int cmd_disasm(int argc, char** argv) {
    std::string eboot, out;
    uint32_t start = 0x10230, end = 0xABD560;
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (std::strcmp(argv[i], "--eboot") == 0) eboot = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0) out = argv[++i];
        else if (std::strcmp(argv[i], "--start") == 0) start = std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--end") == 0) end = std::strtoul(argv[++i], nullptr, 0);
        else return usage();
    }
    if (eboot.empty() || end <= start) return usage();

    sly4::GuestMemory mem;
    sly4::LoadedImage img;
    if (!load(eboot, mem, img)) return 3;
    if (!mem.is_committed(start, end - start)) {
        SLY4_ERROR("faixa 0x%X-0x%X fora da imagem", start, end);
        return 3;
    }

    FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), "w");
    if (!f) {
        SLY4_ERROR("não foi possível criar %s", out.c_str());
        return 3;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    const auto t0 = std::chrono::steady_clock::now();
    size_t count = 0, invalid = 0;
    for (uint32_t a = start; a + 4 <= end; a += 4) {
        const uint32_t w = mem.read<uint32_t>(a);
        const sly4::ppu::Insn in = sly4::ppu::decode(w);
        invalid += in.op == sly4::ppu::Op::invalid;
        std::fprintf(f, "%x:\t%08x\t%s\n", a, w, sly4::ppu::disasm(in, a).c_str());
        ++count;
    }
    if (f != stdout) std::fclose(f);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%zu palavras decodificadas em %.2f s (%zu sem instrução válida)", count, s, invalid);
    return 0;
}

// Disassembles the executable segments of an SPU ELF file (tools/spu_extract.py output).
int cmd_spu_disasm(int argc, char** argv) {
    std::string elf, out;
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (std::strcmp(argv[i], "--elf") == 0) elf = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0) out = argv[++i];
        else return usage();
    }
    if (elf.empty()) return usage();
    FILE* in = std::fopen(elf.c_str(), "rb");
    if (!in) {
        SLY4_ERROR("não foi possível abrir %s", elf.c_str());
        return 3;
    }
    std::vector<uint8_t> data;
    for (int c; (c = std::fgetc(in)) != EOF;) data.push_back(uint8_t(c));
    std::fclose(in);
    sly4::SpuImage img;
    std::string error;
    if (!sly4::parse_spu_elf(data.data(), data.size(), img, error)) {
        SLY4_ERROR("%s: %s", elf.c_str(), error.c_str());
        return 3;
    }
    FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), "w");
    if (!f) {
        SLY4_ERROR("não foi possível criar %s", out.c_str());
        return 3;
    }
    size_t count = 0, invalid = 0;
    for (const sly4::SpuSegment& seg : img.segments) {
        if (!seg.executable()) continue;
        for (uint32_t off = 0; off + 4 <= seg.file_size; off += 4) {
            const uint32_t w = sly4::load_be<uint32_t>(data.data() + seg.offset + off);
            const sly4::spu::Insn insn = sly4::spu::decode(w);
            invalid += insn.op == sly4::spu::Op::invalid;
            std::fprintf(f, "%x:\t%08x\t%s\n", seg.vaddr + off, w, sly4::spu::disasm(insn, seg.vaddr + off).c_str());
            ++count;
        }
    }
    if (f != stdout) std::fclose(f);
    SLY4_INFO("%s: %zu palavras (%zu sem instrução válida)", elf.c_str(), count, invalid);
    return 0;
}

// C++ for SPU blocks: the ones the SPU cache recorded while the game ran (--cache) and
// the ones found statically in SPU ELF files (--elf, any number).
int cmd_spu_emit(int argc, char** argv) {
    std::string cache, out_dir;
    std::vector<std::string> elfs;
    for (int i = 2; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (std::strcmp(argv[i], "--cache") == 0) cache = argv[++i];
        else if (std::strcmp(argv[i], "--elf") == 0) elfs.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--out-dir") == 0) out_dir = argv[++i];
        else return usage();
    }
    if (out_dir.empty() || (cache.empty() && elfs.empty())) return usage();
    std::vector<sly4::SpuCachedBlock> blocks;
    if (!cache.empty()) {
        if (!sly4::spu_cache_read(cache, blocks)) return 3;
        SLY4_INFO("%s: %zu blocos", cache.c_str(), blocks.size());
    }
    auto ls = std::make_unique<sly4::SpuLocalStore>();
    for (const std::string& path : elfs) {
        FILE* in = std::fopen(path.c_str(), "rb");
        if (!in) {
            SLY4_ERROR("não foi possível abrir %s", path.c_str());
            return 3;
        }
        std::vector<uint8_t> data;
        for (int c; (c = std::fgetc(in)) != EOF;) data.push_back(uint8_t(c));
        std::fclose(in);
        sly4::SpuImage img;
        std::string error;
        if (!sly4::parse_spu_elf(data.data(), data.size(), img, error)) {
            SLY4_ERROR("%s: %s", path.c_str(), error.c_str());
            return 3;
        }
        std::memset(ls->data, 0, sizeof(ls->data));
        sly4::load_spu_segments(data.data(), img, ls->data);
        std::vector<std::pair<uint32_t, uint32_t>> code;
        for (const sly4::SpuSegment& s : img.segments)
            if (s.executable()) code.emplace_back(s.vaddr, s.vaddr + s.file_size);
        const size_t before = blocks.size();
        sly4::recomp::spu_static_blocks(ls->data, code, img.entry, blocks);
        SLY4_INFO("%s: %zu blocos achados no código", path.c_str(), blocks.size() - before);
    }
    sly4::recomp::SpuEmitStats stats;
    std::string error;
    if (!sly4::recomp::emit_spu_blocks(blocks, out_dir, stats, error)) {
        SLY4_ERROR("%s", error.c_str());
        return 3;
    }
    SLY4_INFO("%zu blocos (%zu instruções) em %zu arquivos em %s", stats.blocks, stats.instructions, stats.files,
              out_dir.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) return usage();
    if (std::strcmp(argv[1], "disasm") == 0) return cmd_disasm(argc, argv);
    if (std::strcmp(argv[1], "spu-disasm") == 0) return cmd_spu_disasm(argc, argv);
    if (std::strcmp(argv[1], "spu-oracle") == 0) return sly4::spu_oracle::cmd(argc, argv);
    if (std::strcmp(argv[1], "spu-emit") == 0) return cmd_spu_emit(argc, argv);
    if (std::strcmp(argv[1], "psarc-hash") == 0) return sly4::spu_oracle::cmd_hash(argc, argv);
    if (std::strcmp(argv[1], "analyze") == 0) return cmd_analyze(argc, argv);
    if (std::strcmp(argv[1], "emit") == 0) return cmd_emit(argc, argv);
    if (std::strcmp(argv[1], "oracle-gen") == 0) return sly4::oracle::cmd_gen(argc, argv);
    if (std::strcmp(argv[1], "oracle-check") == 0) return sly4::oracle::cmd_check(argc, argv);
    return usage();
}
