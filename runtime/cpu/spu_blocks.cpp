#include "cpu/spu_blocks.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "cpu/spu_interpreter.h"

namespace sly4 {

namespace {

// ---------------------------------------------------------------- compiled blocks
// Entries by pc: head[pc / 4] is 1 + the index of the first entry for that pc, and
// next[i] chains the others (the same address can hold different code).
struct BlockTable {
    std::vector<SpuBlockEntry> entries;
    std::vector<uint32_t> next;
    std::unique_ptr<uint32_t[]> head = std::make_unique<uint32_t[]>(kSpuLsSize / 4);
};
BlockTable g_blocks;

// ---------------------------------------------------------------- SPU cache
uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

std::atomic<bool> g_recording{false};
std::mutex g_cache_lock;
std::unordered_map<uint64_t, SpuCachedBlock> g_cache;  // key: hash of pc + code

uint64_t cache_key(uint32_t pc, const uint8_t* code, uint32_t size) {
    return fnv1a(code, size, fnv1a(reinterpret_cast<const uint8_t*>(&pc), 4));
}

void record(const uint8_t* ls, uint32_t pc) {
    thread_local std::unordered_set<uint64_t> seen;  // most entries repeat: no lock for them
    const uint32_t size = spu_block_size(ls, pc);
    const uint64_t key = cache_key(pc, ls + pc, size);
    if (!seen.insert(key).second) return;
    std::lock_guard<std::mutex> g(g_cache_lock);
    if (g_cache.count(key)) return;
    SpuCachedBlock& b = g_cache[key];
    b.pc = pc;
    b.code.assign(ls + pc, ls + pc + size);
}

constexpr char kMagic[8] = {'S', 'L', 'Y', '4', 'S', 'P', 'U', 'C'};

}  // namespace

uint32_t spu_block_size(const uint8_t* ls, uint32_t pc) {
    uint32_t n = 0;
    for (uint32_t a = pc; a < kSpuLsSize && n < kSpuMaxBlockWords; a += 4) {
        ++n;
        if (spu::ends_block(spu::decode(load_be<uint32_t>(ls + a)).op)) break;
    }
    return n * 4;
}

void spu_register_blocks(const SpuBlockEntry* table, size_t count) {
    BlockTable& t = g_blocks;
    for (size_t i = 0; i < count; ++i) {
        const SpuBlockEntry& e = table[i];
        if (e.pc >= kSpuLsSize || (e.pc & 3)) continue;
        t.entries.push_back(e);
        t.next.push_back(t.head[e.pc / 4]);
        t.head[e.pc / 4] = uint32_t(t.entries.size());
    }
}

size_t spu_block_count() { return g_blocks.entries.size(); }

const SpuBlockEntry* spu_block_table(size_t& count) {
    count = g_blocks.entries.size();
    return g_blocks.entries.data();
}

SpuBlockFn spu_find_block(SPUContext& c) {
    SPUContext::CachedBlock& b = c.block_cache[(c.pc >> 2) % SPUContext::kBlockCache];
    if (b.pc == c.pc && c.ls_epoch[b.page0] == b.epoch0 && c.ls_epoch[b.page1] == b.epoch1) {
        return reinterpret_cast<SpuBlockFn>(b.fn);
    }
    const BlockTable& t = g_blocks;
    for (uint32_t i = t.head[c.pc / 4]; i; i = t.next[i - 1]) {
        const SpuBlockEntry& e = t.entries[i - 1];
        if (c.pc + e.size <= kSpuLsSize && std::memcmp(c.ls + c.pc, e.code, e.size) == 0) {
            b.pc = c.pc;
            b.page0 = c.pc >> 12;
            b.page1 = (c.pc + e.size - 1) >> 12;
            b.epoch0 = c.ls_epoch[b.page0];
            b.epoch1 = c.ls_epoch[b.page1];
            b.fn = reinterpret_cast<void*>(e.fn);
            return e.fn;
        }
    }
    return nullptr;
}

namespace {

std::atomic<uint32_t> g_ls_watch{~0u};
std::atomic<int> g_ls_watch_logged{0};

// --spu-ls-watch: the block that changed the watched word, with the registers it started
// with (arguments, saved registers) and where it left.
void ls_watch_report(const SPUContext& c, uint32_t lsa, uint32_t old, uint32_t now, uint32_t pc,
                     const SpuReg (&entry)[128]) {
    if (g_ls_watch_logged.fetch_add(1) >= 200) return;
    std::string s;
    char buf[64];
    for (int r = 0; r < 128; ++r) {
        const uint32_t v = entry[r].pref();
        if (r >= 10 && r < 75 && v == 0) continue;  // the volatile ones only when set
        std::snprintf(buf, sizeof(buf), " $%d=%08X", r, v);
        s += buf;
    }
    std::fprintf(stderr,
                 "[spu-ls-watch] LS %p (taskset SPURS 0x%08X): [0x%05X] %08X -> %08X no bloco 0x%05X, que saiu em 0x%05X "
                 "($0 %08X $3 %08X)\n  registradores na entrada:%s\n",
                 static_cast<const void*>(c.ls), c.ls_u32(0x27BC), lsa, old, now, pc, c.pc,
                 c.gpr[0].pref(), c.gpr[3].pref(), s.c_str());
}

template <bool kWatch>
SpuStep run_blocks(SPUContext& c, uint64_t max_steps, uint64_t& executed) {
    spusem::SpuFloatMode fp;
    uint64_t n = 0;
    const bool rec = g_recording.load(std::memory_order_relaxed);
    const uint32_t watch = kWatch ? g_ls_watch.load(std::memory_order_relaxed) : 0;
    while (n < max_steps) {
        SpuStep st;
        spu_check_interrupt(c);
        uint32_t watch_pc = 0, watch_old = 0;
        SpuReg entry[kWatch ? 128 : 1];
        if constexpr (kWatch) {
            watch_pc = c.pc;
            watch_old = c.ls_u32(watch);
            std::memcpy(entry, c.gpr, sizeof(entry));
        }
        // The block cache's hit here, without a call (spu_find_block was ~8% of a busy SPU).
        const SPUContext::CachedBlock& b = c.block_cache[(c.pc >> 2) % SPUContext::kBlockCache];
        SpuBlockFn fn = b.pc == c.pc && c.ls_epoch[b.page0] == b.epoch0 && c.ls_epoch[b.page1] == b.epoch1
                            ? reinterpret_cast<SpuBlockFn>(b.fn)
                            : spu_find_block(c);
        if (fn) {
            st = fn(c, n);
        } else {
            if (rec) record(c.ls, c.pc);
            // Interpret up to the first instruction that leaves the straight line.
            do {
                st = spu_step(c);
                if (st == SpuStep::Ok || st == SpuStep::Branch || st == SpuStep::Stop || st == SpuStep::Halt) ++n;
            } while (st == SpuStep::Ok && n < max_steps);
        }
        if constexpr (kWatch) {
            if (const uint32_t now = c.ls_u32(watch); now != watch_old)
                ls_watch_report(c, watch, watch_old, now, watch_pc, entry);
        }
        if (st != SpuStep::Ok && st != SpuStep::Branch) {
            executed += n;
            return st;
        }
    }
    executed += n;
    return SpuStep::Ok;
}

}  // namespace

void spu_ls_watch(uint32_t lsa) { g_ls_watch = lsa & kSpuLsMask & ~3u; }

SpuStep spu_run_blocks(SPUContext& c, uint64_t max_steps, uint64_t& executed) {
    if (g_ls_watch.load(std::memory_order_relaxed) != ~0u) [[unlikely]]
        return run_blocks<true>(c, max_steps, executed);
    return run_blocks<false>(c, max_steps, executed);
}

// ---------------------------------------------------------------- SPU cache file
// "SLY4SPUC", then per block: u32 pc, u32 size, size bytes of code (little-endian
// header fields; the code as in local store).
void spu_cache_enable(bool on) { g_recording = on; }

size_t spu_cache_size() {
    std::lock_guard<std::mutex> g(g_cache_lock);
    return g_cache.size();
}

bool spu_cache_read(const std::string& path, std::vector<SpuCachedBlock>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return true;
    char magic[8];
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0;
    uint32_t head[2];
    while (ok && std::fread(head, 4, 2, f) == 2) {
        if (head[0] >= kSpuLsSize || head[1] == 0 || head[1] > kSpuMaxBlockWords * 4 || head[0] + head[1] > kSpuLsSize) {
            ok = false;
            break;
        }
        SpuCachedBlock b;
        b.pc = head[0];
        b.code.resize(head[1]);
        ok = std::fread(b.code.data(), 1, head[1], f) == head[1];
        if (ok) out.push_back(std::move(b));
    }
    std::fclose(f);
    if (!ok) SLY4_ERROR("cache SPU %s: arquivo inválido", path.c_str());
    return ok;
}

bool spu_cache_save(const std::string& path) {
    std::vector<SpuCachedBlock> old;
    spu_cache_read(path, old);
    std::lock_guard<std::mutex> g(g_cache_lock);
    for (SpuCachedBlock& b : old) {
        const uint64_t key = cache_key(b.pc, b.code.data(), uint32_t(b.code.size()));
        if (!g_cache.count(key)) g_cache[key] = std::move(b);
    }
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fwrite(kMagic, 1, 8, f);
    for (const auto& [key, b] : g_cache) {
        const uint32_t head[2] = {b.pc, uint32_t(b.code.size())};
        std::fwrite(head, 4, 2, f);
        std::fwrite(b.code.data(), 1, b.code.size(), f);
    }
    const bool ok = std::fclose(f) == 0;
    std::remove(path.c_str());
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace sly4
