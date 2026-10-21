// sly4_gendiff: differential test of the generated C++ (sly4_recomp emit) against the
// reference interpreter, function by function.
//
//   sly4_gendiff --eboot PS3_GAME\USRDIR\EBOOT.elf [--cases 8] [--seed 1] [--examples 3]
//
// For every generated function and several random-but-plausible initial states (valid
// stack and TOC, arguments pointing into a scratch area full of pointers and small
// values, the EBOOT loaded), the function runs once through the interpreter and once
// through the generated code; all registers and the touched memory (EBOOT data, stack,
// scratch) must end up identical. Cases where either side faults (bad address, trap) or
// runs too long are set aside, not compared.
#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/log.h"
#include "cpu/ppu_rt.h"
#include "cpu/ppu_semantics.h"
#include "functions.h"
#include "loader/elf_loader.h"

using namespace sly4;

namespace {

constexpr uint32_t kNull = 0x0, kNullSize = 0x10000;  // null page, committed so null derefs compare
constexpr uint32_t kScratch = 0x30000000, kScratchSize = 0x00100000;
constexpr uint32_t kStack = 0x30100000, kStackSize = 0x00040000;
constexpr uint32_t kSentinel = 0x00000100;  // return address: in the null page, never code
constexpr int64_t kBudget = 2'000'000;

struct Region {
    uint32_t addr, size;
};

void* g_jmp[5];
volatile int g_armed = 0;
const char* g_why = "";

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    if (g_armed && e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        g_armed = 0;
        g_why = "acesso inválido";
        __builtin_longjmp(g_jmp, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void on_fault(PPUContext&, GuestMemory&, const char* what) {
    g_armed = 0;
    g_why = what;
    __builtin_longjmp(g_jmp, 1);
}
void on_syscall(PPUContext& c, GuestMemory&) { c.gpr[3] = 0; }
int g_lr_mismatch = 0;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_lr_sites;  // blr address -> (expected, actual)
// A blr whose LR is not the one the function was entered with (typically an LR reloaded
// from a stack slot the random state filled with garbage): the generated code returns to
// its caller while the guest would jump to that LR, so the case leaves the contract.
void on_lr_mismatch(PPUContext& c, GuestMemory& m, uint32_t at, uint32_t expected) {
    ++g_lr_mismatch;
    g_lr_sites.emplace(at, std::make_pair(expected, uint32_t(c.lr) & ~3u));
    on_fault(c, m, "blr com LR diferente do da entrada");
}
const char* const kBudgetMsg = "orçamento de execução esgotado";
uint64_t fixed_timebase() { return 0x123456789ull; }

// Runs f with faults turned into a false return.
template <class F>
__attribute__((noinline)) bool guarded(F&& f) {
    if (__builtin_setjmp(g_jmp)) {
        std::fesetround(FE_TONEAREST);
        return false;
    }
    g_armed = 1;
    f();
    g_armed = 0;
    return true;
}

struct Snapshot {
    std::vector<std::vector<uint8_t>> parts;
};
Snapshot take(const GuestMemory& m, const std::vector<Region>& rs) {
    Snapshot s;
    for (const Region& r : rs) {
        s.parts.emplace_back(r.size);
        m.copy_from(s.parts.back().data(), r.addr, r.size);
    }
    return s;
}
void restore(GuestMemory& m, const std::vector<Region>& rs, const Snapshot& s) {
    for (size_t i = 0; i < rs.size(); ++i) m.copy_to(rs[i].addr, s.parts[i].data(), rs[i].size);
}

std::string compare(const PPUContext& a, const PPUContext& g, const GuestMemory& m, const std::vector<Region>& rs,
                    const Snapshot& after_interp) {
    char buf[200];
    for (int i = 0; i < 32; ++i)
        if (a.gpr[i] != g.gpr[i]) {
            std::snprintf(buf, sizeof(buf), "r%d interp=%016llx gerado=%016llx", i, (unsigned long long)a.gpr[i],
                          (unsigned long long)g.gpr[i]);
            return buf;
        }
    for (int i = 0; i < 32; ++i)
        if (a.fpr[i] != g.fpr[i]) {
            std::snprintf(buf, sizeof(buf), "f%d interp=%016llx gerado=%016llx", i, (unsigned long long)a.fpr[i],
                          (unsigned long long)g.fpr[i]);
            return buf;
        }
    for (int i = 0; i < 32; ++i)
        if (std::memcmp(a.vr[i].b, g.vr[i].b, 16)) return "v" + std::to_string(i);
    if (a.cr != g.cr) return "cr";
    if (a.xer() != g.xer()) return "xer";
    if (a.lr != g.lr) {
        std::snprintf(buf, sizeof(buf), "lr interp=%llx gerado=%llx", (unsigned long long)a.lr,
                      (unsigned long long)g.lr);
        return buf;
    }
    if (a.ctr != g.ctr) return "ctr";
    if (a.fpscr != g.fpscr) return "fpscr";
    if (a.vscr != g.vscr) return "vscr";
    for (size_t i = 0; i < rs.size(); ++i) {
        std::vector<uint8_t> now(rs[i].size);
        m.copy_from(now.data(), rs[i].addr, rs[i].size);
        for (uint32_t k = 0; k < rs[i].size; ++k)
            if (now[k] != after_interp.parts[i][k]) {
                std::snprintf(buf, sizeof(buf), "memoria 0x%08x interp=%02x gerado=%02x", rs[i].addr + k,
                              after_interp.parts[i][k], now[k]);
                return buf;
            }
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string eboot;
    int cases = 8, examples = 3;
    uint64_t seed = 1;
    std::vector<uint32_t> only;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--eboot")) eboot = argv[i + 1];
        else if (!std::strcmp(argv[i], "--cases")) cases = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--examples")) examples = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--only")) only.push_back(uint32_t(std::strtoul(argv[i + 1], nullptr, 0)));
    }
    if (eboot.empty()) {
        std::fprintf(stderr, "uso: sly4_gendiff --eboot EBOOT.elf [--cases 8] [--seed 1] [--examples 3]\n");
        return 2;
    }
    GuestMemory mem;
    LoadedImage img;
    std::string error;
    if (!mem.reserve() || !load_elf(eboot, mem, img, error)) {
        SLY4_ERROR("%s", error.c_str());
        return 3;
    }
    mem.commit(kNull, kNullSize, Protection::ReadWrite);
    mem.commit(kScratch, kScratchSize + kStackSize, Protection::ReadWrite);
    // code stays read-only; data segment(s), stack, scratch and null page are compared
    std::vector<Region> regions{{kNull, kNullSize}, {kScratch, kScratchSize}, {kStack, kStackSize}};
    for (const auto& s : img.segments)
        if (s.flags & 2) regions.push_back({s.vaddr, s.mem_size});

    std::mt19937_64 rng(seed);
    for (uint32_t a = kScratch; a < kScratch + kScratchSize; a += 4) {  // pointers, small values, zeros
        const uint32_t k = rng() % 8;
        mem.write<uint32_t>(a, k < 4 ? 0 : k < 6 ? kScratch + uint32_t(rng() % (kScratchSize - 0x1000)) / 16 * 16
                                                : uint32_t(rng() % 256));
    }
    const Snapshot pristine = take(mem, regions);

    gen::register_functions();
    rt::hooks().fault = on_fault;
    rt::hooks().syscall = on_syscall;
    rt::hooks().lr_mismatch = on_lr_mismatch;
    sem::timebase_source = fixed_timebase;
    AddVectoredExceptionHandler(1, on_exception);

    size_t nfn = 0;
    const rt::FnEntry* table = rt::function_table(nfn);
    size_t compared = 0, equal = 0, set_aside = 0, one_side_fault = 0;
    std::map<std::string, size_t> aside_reasons;
    std::vector<std::string> failures;
    std::map<uint32_t, int> failing_functions;
    for (size_t fi = 0; fi < nfn; ++fi) {
        if (!only.empty() && std::find(only.begin(), only.end(), table[fi].addr) == only.end()) continue;
        for (int k = 0; k < cases; ++k) {
            PPUContext s;
            for (int i = 0; i < 32; ++i) {
                const uint32_t pick = rng() % 4;
                s.gpr[i] = pick == 0 ? kScratch + uint32_t(rng() % (kScratchSize - 0x1000)) / 16 * 16
                         : pick == 1 ? rng() % 256
                         : pick == 2 ? uint64_t(-int64_t(rng() % 16))
                                     : rng() % 0x10000;
            }
            s.gpr[1] = kStack + kStackSize - 0x2000;
            s.gpr[2] = img.toc;
            s.gpr[13] = kScratch + 0x7000;
            for (int i = 0; i < 32; ++i) {
                const double d = double(int64_t(rng() % 2000) - 1000) / 16.0;
                std::memcpy(&s.fpr[i], &d, 8);
                for (int b = 0; b < 16; b += 4) {
                    const float f = float(int32_t(rng() % 2000) - 1000) / 8.0f;
                    std::memcpy(s.vr[i].b + b, &f, 4);
                }
            }
            s.cr = uint32_t(rng());
            s.lr = kSentinel;
            s.ctr = rng() % 64;
            s.budget = kBudget;

            restore(mem, regions, pristine);
            PPUContext a = s;
            const bool ok_a = guarded([&] { rt::interpret_until_return(a, mem, table[fi].addr); });
            const char* why_a = g_why;
            const Snapshot after_interp = ok_a ? take(mem, regions) : Snapshot{};

            restore(mem, regions, pristine);
            PPUContext g = s;
            const bool ok_g = guarded([&] { table[fi].fn(g, mem); });
            if (!ok_a || !ok_g) {
                ++set_aside;
                const char* why_g = g_why;
                // The interpreter budget counts instructions and the generated one blocks, so a
                // budget stop on one side only is expected and not a divergence.
                const bool budget = (!ok_a && std::strcmp(why_a, kBudgetMsg) == 0) ||
                                    (!ok_g && std::strcmp(why_g, kBudgetMsg) == 0);
                if (ok_a != ok_g && !budget) {
                    ++one_side_fault;
                    char line[160];
                    std::snprintf(line, sizeof(line), "f_%08X caso %d: so o %s falhou (%s)", table[fi].addr, k,
                                  ok_a ? "gerado" : "interpretador", ok_a ? why_g : why_a);
                    failures.push_back(line);
                }
                if (!ok_a) aside_reasons[std::string("interp: ") + why_a]++;
                if (!ok_g) aside_reasons[std::string("gerado: ") + why_g]++;
                continue;
            }
            ++compared;
            const std::string diff = compare(a, g, mem, regions, after_interp);
            if (diff.empty()) {
                ++equal;
                continue;
            }
            if (failing_functions[table[fi].addr]++ == 0 && int(failures.size()) < examples * 10) {
                char head[64];
                std::snprintf(head, sizeof(head), "f_%08X caso %d: ", table[fi].addr, k);
                failures.push_back(head + diff);
            }
        }
    }
    SLY4_INFO("%zu funções, %zu casos comparados: %zu iguais, %zu diferentes (%zu funções); %zu deixados de lado",
              nfn, compared, equal, compared - equal, failing_functions.size(), set_aside);
    SLY4_INFO("lr_mismatch: %d; falha em só um dos lados: %zu", g_lr_mismatch, one_side_fault);
    for (const auto& [why, n] : aside_reasons) std::printf("  deixado de lado (%zu): %s\n", n, why.c_str());
    for (const auto& [at, v] : g_lr_sites)
        std::printf("  lr_mismatch: blr em 0x%08X voltou para 0x%08X, esperado 0x%08X\n", at, v.second, v.first);
    for (size_t i = 0; i < failures.size() && int(i) < examples * 10; ++i) std::printf("  FALHA %s\n", failures[i].c_str());
    std::fflush(stdout);
    return compared != equal || one_side_fault ? 1 : 0;
}
