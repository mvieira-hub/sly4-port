// sly4_spudiff: differential test of the recompiled SPU blocks (sly4_recomp spu-emit)
// against the SPU interpreter (docs/SPU.md, 4e).
//
//   sly4_spudiff [--cases 16] [--seed 1]
//       Every compiled block runs from random states (all 128 registers and the local
//       store random, the block's code in place) once in the interpreter and once
//       compiled; registers, local store, PC, channel state and the instruction count
//       must match. Blocks that start DMA or use the decrementer (which depends on the
//       clock) are set aside, as are cases where a store overwrites the block itself.
//   sly4_spudiff --oracle --spu-elf <EDGE zlib .elf> --psarc <Sly4.psarc> [--blocks 200]
//       The game's inflate (sly4_recomp spu-oracle) run with the compiled blocks.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>

#include "core/log.h"
#include "cpu/spu_blocks.h"
#include "cpu/spu_channels.h"
#include "cpu/spu_interpreter.h"
#include "spu_oracle.h"

namespace sly4::gen {
void register_spu_blocks();
}

using namespace sly4;

namespace {

// Mailboxes, signals and events: every read gets the next value of a counter, so the
// interpreter and the compiled block see the same sequence.
struct StubIo : SpuIo {
    uint32_t next = 0x1000;
    SpuChannelResult read(SPUContext&, uint32_t, uint32_t& v) override {
        v = next++;
        return SpuChannelResult::Ok;
    }
    SpuChannelResult write(SPUContext&, uint32_t, uint32_t) override { return SpuChannelResult::Ok; }
    uint32_t count(SPUContext&, uint32_t) override { return 1; }
};

// DMA (MFC_Cmd) needs main memory; the decrementer reads the host clock.
bool comparable(const SpuBlockEntry& e) {
    for (uint32_t i = 0; i < e.size; i += 4) {
        const spu::Insn in = spu::decode(load_be<uint32_t>(e.code + i));
        if (in.op == spu::Op::wrch && (in.ra() == spu_ch::MFC_Cmd || in.ra() == spu_ch::WrDec)) return false;
        if (in.op == spu::Op::rdch && in.ra() == spu_ch::RdDec) return false;
    }
    return true;
}

struct Run {
    std::unique_ptr<SpuLocalStore> ls = std::make_unique<SpuLocalStore>();
    SPUContext c{};
    StubIo io;
    SpuStep st = SpuStep::Ok;
    uint64_t n = 0;
};

void copy_state(const Run& from, Run& to) {
    std::memcpy(to.ls->data, from.ls->data, kSpuLsSize);
    to.c = from.c;
    to.c.ls = to.ls->data;
    to.c.io = &to.io;
    to.io.next = from.io.next;
}

// The interpreter from the block's entry until it leaves the straight line (or reaches
// the end of a cut block), counting as spu_run_blocks does.
void interpret(Run& r, const SpuBlockEntry& e) {
    const uint32_t end = (e.pc + e.size) & kSpuLsMask;
    for (uint32_t i = 0; i <= e.size / 4; ++i) {
        r.st = spu_step(r.c);
        if (r.st == SpuStep::Ok || r.st == SpuStep::Branch || r.st == SpuStep::Stop || r.st == SpuStep::Halt) ++r.n;
        if (r.st != SpuStep::Ok) return;
        if (r.c.pc == end) {
            r.st = SpuStep::Branch;  // what a cut block returns
            return;
        }
    }
}

const char* diff(const Run& a, const Run& b) {
    if (a.st != b.st) return "resultado";
    if (a.n != b.n) return "contagem de instruções";
    if (a.c.pc != b.c.pc) return "pc";
    if (std::memcmp(a.c.gpr, b.c.gpr, sizeof(a.c.gpr)) != 0) return "registradores";
    if (std::memcmp(a.ls->data, b.ls->data, kSpuLsSize) != 0) return "local store";
    if (a.c.stop_code != b.c.stop_code || a.c.srr0 != b.c.srr0) return "stop/srr0";
    if (std::memcmp(a.c.fpscr, b.c.fpscr, sizeof(a.c.fpscr)) != 0) return "fpscr";
    if (std::memcmp(&a.c.mfc, &b.c.mfc, sizeof(a.c.mfc)) != 0 || a.c.tag_mask != b.c.tag_mask ||
        a.c.tag_update != b.c.tag_update || a.c.atomic_stat != b.c.atomic_stat)
        return "MFC";
    if (a.c.event_mask != b.c.event_mask || a.c.event_stat != b.c.event_stat || a.c.irq != b.c.irq) return "eventos";
    if (a.io.next != b.io.next) return "canais";
    return nullptr;
}

int blocks(int argc, char** argv) {
    int cases = 16;
    uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--cases")) cases = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 0);
    }
    size_t count = 0;
    const SpuBlockEntry* table = spu_block_table(count);
    std::mt19937_64 rng(seed);
    Run start, ref, gen;
    size_t compared = 0, equal = 0, different = 0, skipped_blocks = 0, skipped_cases = 0, shown = 0;
    std::set<uint32_t> shown_blocks;
    for (size_t b = 0; b < count; ++b) {
        const SpuBlockEntry& e = table[b];
        if (!comparable(e)) {
            ++skipped_blocks;
            continue;
        }
        for (int k = 0; k < cases; ++k) {
            for (uint32_t i = 0; i < kSpuLsSize; i += 8) {
                const uint64_t v = rng();
                std::memcpy(start.ls->data + i, &v, 8);
            }
            std::memcpy(start.ls->data + e.pc, e.code, e.size);
            start.c = SPUContext{};
            for (SpuReg& r : start.c.gpr) r.d[0] = rng(), r.d[1] = rng();
            // Half the cases with small register values, so compares and branches go both ways.
            if (k & 1)
                for (SpuReg& r : start.c.gpr)
                    for (int w = 0; w < 4; ++w) r.w[w] &= 0x3F;
            start.c.pc = e.pc;
            start.c.event_mask = uint32_t(rng());
            start.c.srr0 = uint32_t(rng()) & kSpuLsMask & ~3u;
            start.io.next = uint32_t(rng());
            copy_state(start, ref);
            copy_state(start, gen);
            ref.n = gen.n = 0;
            interpret(ref, e);
            if (std::memcmp(ref.ls->data + e.pc, e.code, e.size) != 0) {  // the block rewrote itself
                ++skipped_cases;
                continue;
            }
            gen.st = e.fn(gen.c, gen.n);
            ++compared;
            if (const char* what = diff(ref, gen)) {
                ++different;
                if (shown_blocks.insert(e.pc).second && shown++ < 40) {  // the first case of each block
                    SLY4_ERROR("bloco 0x%05X (%u bytes), caso %d: %s diferente (interpretador: pc 0x%05X, %" PRIu64
                               " instruções; compilado: pc 0x%05X, %" PRIu64 ")",
                               e.pc, e.size, k, what, ref.c.pc, ref.n, gen.c.pc, gen.n);
                    for (int r = 0; r < 128; ++r) {
                        if (std::memcmp(&ref.c.gpr[r], &gen.c.gpr[r], 16) == 0) continue;
                        // The last instruction of the block that writes it.
                        std::string writer = "?";
                        for (uint32_t i = 0; i < e.size; i += 4) {
                            const spu::Insn in = spu::decode(load_be<uint32_t>(e.code + i));
                            const bool rrr = spu::form(in.op) == spu::Form::RRR;
                            if ((rrr ? in.rc() : in.rt()) == uint32_t(r)) writer = spu::disasm(in, e.pc + i);
                        }
                        std::fprintf(stderr, "    $%d: %08X %08X %08X %08X  x  %08X %08X %08X %08X   (%s)\n", r,
                                     ref.c.gpr[r].u32(0), ref.c.gpr[r].u32(1), ref.c.gpr[r].u32(2),
                                     ref.c.gpr[r].u32(3), gen.c.gpr[r].u32(0), gen.c.gpr[r].u32(1),
                                     gen.c.gpr[r].u32(2), gen.c.gpr[r].u32(3), writer.c_str());
                    }
                }
            } else {
                ++equal;
            }
        }
    }
    SLY4_INFO("%zu blocos compilados; %zu deixados de lado (DMA ou decrementador)", count, skipped_blocks);
    SLY4_INFO("%zu casos comparados: %zu iguais, %zu diferentes; %zu casos em que o bloco se reescreveu", compared,
              equal, different, skipped_cases);
    return different ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    gen::register_spu_blocks();
    spusem::SpuFloatMode fp;  // as SPU code runs (spu_run_blocks)
    if (argc > 1 && !std::strcmp(argv[1], "--oracle")) return spu_oracle::cmd(argc, argv);  // parses from argv[2]
    return blocks(argc, argv);
}
