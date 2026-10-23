// "sly4_recomp spu-oracle": validates the SPU interpreter by running the game's own
// EDGE zlib inflate (inside the SPU task the FIOS uses) on real blocks of Sly4.psarc and
// comparing its output with the native inflate (runtime/core/inflate.cpp).
//
// The task's inner function at 0x3740 fetches the compressed data by DMA, inflates it
// in local store and writes the result back by DMA:
//   f(r3 = dst ea, r4 = uncompressed size, r5 = src ea, r6 = compressed size, r7 = 0,
//     r8 = LS work buffer, r9 = its size, r10 = LS work buffer, r11 = its size)
// exactly as the task's request loop calls it (0x3248-0x3274).
#include "spu_oracle.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/inflate.h"
#include "core/log.h"
#include "cpu/spu_blocks.h"
#include "cpu/spu_interpreter.h"
#include "loader/spu_elf.h"
#include "memory/guest_memory.h"

namespace sly4::spu_oracle {

namespace {

struct Block {
    uint64_t offset;
    uint32_t stored, expected;
};

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

// The compressed blocks of a PSARC archive (64 KiB blocks, zlib).
bool psarc_blocks(std::ifstream& f, std::vector<Block>& out) {
    uint8_t head[32];
    if (!f.read(reinterpret_cast<char*>(head), 32) || std::memcmp(head, "PSAR", 4) != 0) return false;
    const uint32_t toc_len = uint32_t(be(head + 12, 4)), ent = uint32_t(be(head + 16, 4)), count = uint32_t(be(head + 20, 4));
    const uint32_t block_size = uint32_t(be(head + 24, 4));
    std::vector<uint8_t> toc(toc_len - 32);
    if (!f.read(reinterpret_cast<char*>(toc.data()), toc.size())) return false;
    const int width = block_size <= 0x10000 ? 2 : block_size <= 0x1000000 ? 3 : 4;
    const uint8_t* table = toc.data() + size_t(count) * ent;
    const size_t nblocks = (toc.size() - size_t(count) * ent) / width;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = toc.data() + size_t(i) * ent;
        uint64_t left = be(e + 20, 5), pos = be(e + 25, 5);
        for (uint32_t b = uint32_t(be(e + 16, 4)); left > 0 && b < nblocks; ++b) {
            const uint32_t stored = uint32_t(be(table + size_t(b) * width, width)) ?: block_size;
            const uint32_t expected = uint32_t(std::min<uint64_t>(block_size, left));
            if (stored != expected) out.push_back({pos, stored, expected});
            left -= expected;
            pos += stored;
        }
    }
    return true;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace

int cmd(int argc, char** argv) {
    spusem::SpuFloatMode fp;  // as SPU code runs (spu_run, spu_run_blocks)
    std::string elf_path, psarc_path;
    int count = 200;
    uint64_t seed = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--spu-elf")) elf_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--psarc")) psarc_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--blocks")) count = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[i + 1], nullptr, 0);
    }
    if (elf_path.empty() || psarc_path.empty()) {
        std::fprintf(stderr, "uso: sly4_recomp spu-oracle --spu-elf work\\spu\\spu_00E32D00.elf --psarc <Sly4.psarc> [--blocks 200] [--seed 1]\n");
        return 2;
    }

    std::ifstream ef(elf_path, std::ios::binary);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    SpuImage img;
    std::string error;
    if (!parse_spu_elf(elf.data(), elf.size(), img, error)) {
        SLY4_ERROR("%s: %s", elf_path.c_str(), error.c_str());
        return 3;
    }
    std::ifstream pf(psarc_path, std::ios::binary);
    std::vector<Block> blocks;
    if (!pf || !psarc_blocks(pf, blocks) || blocks.empty()) {
        SLY4_ERROR("não foi possível ler os blocos de %s", psarc_path.c_str());
        return 3;
    }
    std::mt19937_64 rng(seed);
    std::shuffle(blocks.begin(), blocks.end(), rng);
    if (blocks.size() > size_t(count)) blocks.resize(size_t(count));

    GuestMemory mem;
    constexpr GuestAddr kSrc = 0x10000000, kDst = 0x10080000;
    constexpr uint32_t kReturn = 0x2F00;
    if (!mem.reserve() || !mem.commit(kSrc, 0x100000, Protection::ReadWrite)) return 3;
    auto ls = std::make_unique<SpuLocalStore>();

    size_t equal = 0, different = 0, failed = 0;
    uint64_t instructions = 0;
    std::vector<uint64_t> coverage(size_t(spu::Op::count));
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> raw, native(0x10000);
    for (const Block& b : blocks) {
        raw.resize(b.stored);
        pf.clear();
        pf.seekg(std::streamoff(b.offset));
        pf.read(reinterpret_cast<char*>(raw.data()), b.stored);
        if (raw.size() < 2 || raw[0] != 0x78) continue;  // not a zlib stream (stored as is)
        size_t n = 0, used = 0;
        if (inflate_raw(raw.data() + 2, raw.size() - 2, native.data(), native.size(), n, used) != InflateResult::Ok ||
            n != b.expected)
            continue;

        // Fresh local store with the task loaded; "stop 0x1234" at kReturn (below the task,
        // in the SPURS kernel area the task does not use) catches the return.
        std::memset(ls->data, 0, sizeof(ls->data));
        load_spu_segments(elf.data(), img, ls->data);
        store_be<uint32_t>(ls->data + kReturn, 0x00001234);
        std::memset(mem.host(kSrc), 0, 0x100000);
        mem.copy_to(kSrc, raw.data() + 2, raw.size() - 2);

        SPUContext c{};
        c.ls = ls->data;
        c.mem = &mem;
        auto setw = [&](int r, uint32_t v) { c.gpr[r].u32(0) = v; };
        // Stack at the top of local store. SPU ABI: word 1 of $1 is the stack space still
        // available, which function prologues decrease and check (heqi on its sign).
        setw(1, 0x3FFF0);
        c.gpr[1].u32(1) = 0x3FFF0 - 0x37000;  // down to the end of the task's .bss
        setw(0, kReturn);  // link register: the stop above
        setw(3, kDst);
        setw(4, uint32_t(n));
        setw(5, kSrc);
        setw(6, uint32_t(raw.size() - 2));
        setw(7, 0);
        setw(8, 0xC480);
        setw(9, 0x10080);
        setw(10, 0x1C500);
        setw(11, 0x10080);
        c.pc = 0x3740;
        uint64_t ran = 0;
        SpuStep st = SpuStep::Ok;
#ifdef SLY4_SPU_ORACLE_BLOCKS
        st = spu_run_blocks(c, 200'000'000, ran);  // the compiled blocks (sly4_spudiff --oracle)
#else
        while (ran < 200'000'000) {  // spu_run, counting which instructions execute
            const spu::Insn in = spu::decode(c.ls_u32(c.pc));
            ++coverage[size_t(in.op)];
            ++ran;
            st = spu_execute(c, in);
            if (st != SpuStep::Ok && st != SpuStep::Branch) break;
        }
#endif
        instructions += ran;
        if (st != SpuStep::Stop || c.stop_code != 0x1234) {
            ++failed;
            SLY4_ERROR("bloco em 0x%" PRIX64 ": o SPU parou (%d, código 0x%X) em 0x%05X depois de %" PRIu64
                       " instruções",
                       b.offset, int(st), c.stop_code, c.pc, ran);
            continue;
        }
        if (std::memcmp(mem.host(kDst), native.data(), n) == 0) {
            ++equal;
        } else {
            ++different;
            size_t at = 0;
            while (at < n && mem.read<uint8_t>(kDst + uint32_t(at)) == native[at]) ++at;
            SLY4_ERROR("bloco em 0x%" PRIX64 ": saída diferente a partir do byte %zu de %zu (r3 = 0x%X)", b.offset,
                       at, n, c.gpr[3].pref());
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    SLY4_INFO("%zu blocos do psarc: %zu iguais ao inflate nativo, %zu diferentes, %zu com o SPU parado; "
              "%" PRIu64 " instruções SPU em %.1f s",
              equal + different + failed, equal, different, failed, instructions, s);
#ifdef SLY4_SPU_ORACLE_BLOCKS
    return different || failed ? 1 : 0;
#endif
    std::string ops;
    size_t covered = 0;
    for (size_t i = 1; i < coverage.size(); ++i)
        if (coverage[i]) {
            ++covered;
            ops += std::string(" ") + spu::mnemonic(spu::Op(i));
        }
    SLY4_INFO("%zu instruções diferentes executadas:%s", covered, ops.c_str());
    return different || failed ? 1 : 0;
}

}  // namespace sly4::spu_oracle

namespace sly4::spu_oracle {

// "sly4_recomp psarc-hash --psarc <Sly4.psarc> --out f.txt": every compressed block,
// inflated by runtime/core/inflate.cpp, as "offset size crc32" (the format of
// tools/psarc_blockhash.py, which uses Python's zlib).
int cmd_hash(int argc, char** argv) {
    std::string psarc_path, out_path;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--psarc")) psarc_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--out")) out_path = argv[i + 1];
    }
    if (psarc_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "uso: sly4_recomp psarc-hash --psarc <Sly4.psarc> --out arquivo.txt\n");
        return 2;
    }
    std::ifstream pf(psarc_path, std::ios::binary);
    std::vector<Block> blocks;
    if (!pf || !psarc_blocks(pf, blocks)) {
        SLY4_ERROR("não foi possível ler os blocos de %s", psarc_path.c_str());
        return 3;
    }
    FILE* out = std::fopen(out_path.c_str(), "wb");  // LF line ends, as the Python script writes
    if (!out) return 3;
    std::vector<uint8_t> raw, data(0x10000);
    size_t failed = 0;
    for (const Block& b : blocks) {
        raw.resize(b.stored);
        pf.clear();
        pf.seekg(std::streamoff(b.offset));
        pf.read(reinterpret_cast<char*>(raw.data()), b.stored);
        size_t n = 0, used = 0;
        if (raw.size() < 2 || inflate_raw(raw.data() + 2, raw.size() - 2, data.data(), data.size(), n, used) !=
                                  InflateResult::Ok) {
            ++failed;
            std::fprintf(out, "%" PRIx64 " erro\n", b.offset);
            continue;
        }
        std::fprintf(out, "%" PRIx64 " %zu %08x\n", b.offset, n, crc32(data.data(), n));
    }
    std::fclose(out);
    SLY4_INFO("%zu blocos comprimidos -> %s (%zu com erro)", blocks.size(), out_path.c_str(), failed);
    return failed ? 1 : 0;
}

}  // namespace sly4::spu_oracle
