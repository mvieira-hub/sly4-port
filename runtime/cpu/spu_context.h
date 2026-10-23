// Architectural state of one SPU: 128 registers of 128 bits, the 256 KiB local store,
// the PC and the MFC (DMA) channel state.
//
// Registers use the same layout as the PPU's VMX registers (Vec128): guest bytes fully
// reversed, so guest word k of a register is host little-endian lane 3 - k. The
// "preferred slot" (word 0, where scalars and addresses live) is lane 3.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

#include <immintrin.h>

#include "memory/guest_memory.h"

namespace sly4 {

struct SpuReg {
    union {
        uint8_t b[16];
        uint16_t h[8];
        uint32_t w[4];
        uint64_t d[2];
        float f[4];
        double df[2];
    };
    // Guest element order (element 0 is the most significant / lowest address).
    uint8_t& u8(int i) { return b[15 - i]; }
    uint16_t& u16(int i) { return h[7 - i]; }
    uint32_t& u32(int i) { return w[3 - i]; }
    uint64_t& u64(int i) { return d[1 - i]; }
    float& f32(int i) { return f[3 - i]; }
    double& f64(int i) { return df[1 - i]; }
    uint8_t u8(int i) const { return b[15 - i]; }
    uint16_t u16(int i) const { return h[7 - i]; }
    uint32_t u32(int i) const { return w[3 - i]; }
    uint64_t u64(int i) const { return d[1 - i]; }
    float f32(int i) const { return f[3 - i]; }
    double f64(int i) const { return df[1 - i]; }
    uint32_t pref() const { return w[3]; }  // preferred word slot
};
static_assert(sizeof(SpuReg) == 16);

constexpr uint32_t kSpuLsSize = 0x40000;
constexpr uint32_t kSpuLsMask = kSpuLsSize - 1;  // local store limit (LSLR)

// MFC command queue parameters as written through channels 16-21.
struct SpuMfcParams {
    uint32_t lsa = 0, eah = 0, eal = 0, size = 0, tag = 0;
};

struct SpuIo;  // spu_channels.h

struct SPUContext {
    SpuReg gpr[128] = {};
    uint32_t pc = 0;
    uint32_t srr0 = 0;
    uint32_t stop_code = 0;  // signal of the last stop/stopd

    GuestMemory* mem = nullptr;  // main memory, for DMA
    SpuIo* io = nullptr;         // mailboxes, signals, events (thread group / SPURS)
    uint32_t fpscr[4] = {};  // SPU FPSCR (exception flags), read/written whole by fscrrd/fscrwr
    uint8_t* ls = nullptr;   // 256 KiB, guest byte order (big-endian)

    // MFC.
    SpuMfcParams mfc;
    uint32_t tag_mask = 0;
    uint32_t tag_update = 0;
    uint32_t atomic_stat = 0;   // MFC_RdAtomicStat
    bool reserved = false;      // getllar reservation (128-byte line)
    uint32_t reserve_ea = 0;
    uint8_t reserve_data[128] = {};
    uint32_t llr_lost = 0;  // set (atomically) by another writer of the reserved line

    // Events and decrementer (the SPU thread / SPURS layer supplies the rest).
    uint32_t event_mask = 0, event_stat = 0;
    uint32_t decrementer = 0;
    bool dec_armed = false;  // the decrementer event fires when it next goes negative
    // Interrupts (bie/bid, iret): with them on, a pending event in the mask makes the SPU
    // branch to 0 with the next PC in SRR0 and interrupts off.
    bool irq = false;

    // DMA lists stopped by an element with the stall-and-notify flag, by tag: they go on
    // when the SPU acknowledges (MFC_WrListStallAck).
    struct StalledList {
        bool active = false, put = false;
        uint32_t lsa = 0;   // local store address of the next element's data
        uint32_t list = 0;  // local store address of the next element
        uint32_t left = 0;  // bytes of list still to go
    };
    StalledList stalled[32];
    uint32_t list_stall_stat = 0;  // MFC_RdListStallStat

    // Compiled blocks run only when local store holds the code they came from. Checking that
    // with a memcmp on every entry cost ~10% of a busy SPU, so each 4 KiB page of local
    // store has an epoch, bumped by every write to it (SPU stores, DMA gets, getllar, the
    // kernel), and a block found to match is trusted until an epoch of its pages moves.
    static constexpr uint32_t kLsPages = kSpuLsSize / 4096;
    uint32_t ls_epoch[kLsPages] = {};
    struct CachedBlock {
        uint32_t pc = ~0u;
        uint32_t page0 = 0, page1 = 0, epoch0 = 0, epoch1 = 0;
        void* fn = nullptr;  // SpuBlockFn (spu_blocks.h)
    };
    static constexpr uint32_t kBlockCache = 4096;
    CachedBlock block_cache[kBlockCache];

    void ls_written(uint32_t lsa, uint32_t size) {
        if (!size) return;
        const uint32_t first = (lsa & kSpuLsMask) >> 12, last = ((lsa + size - 1) & kSpuLsMask) >> 12;
        for (uint32_t p = first;; p = (p + 1) % kLsPages) {
            ++ls_epoch[p];
            if (p == last) break;
        }
    }
    // Local store changed by means that do not say where (tests, loaders): forget everything.
    void ls_rewritten() {
        for (uint32_t& e : ls_epoch) ++e;
    }

    uint32_t ls_u32(uint32_t a) const { return load_be<uint32_t>(ls + (a & kSpuLsMask)); }
    // The register keeps the quadword's bytes reversed: one pshufb (two 64-bit byte swaps
    // without SSSE3).
    void read_quad(uint32_t a, SpuReg& r) const {
        const uint8_t* p = ls + (a & kSpuLsMask & ~0xFu);
#ifdef __SSSE3__
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(r.b), _mm_shuffle_epi8(v, reverse_bytes()));
#else
        uint64_t hi, lo;
        std::memcpy(&hi, p, 8);
        std::memcpy(&lo, p + 8, 8);
        r.d[1] = __builtin_bswap64(hi);
        r.d[0] = __builtin_bswap64(lo);
#endif
    }
    void write_quad(uint32_t a, const SpuReg& r) {
        ++ls_epoch[(a & kSpuLsMask) >> 12];
        uint8_t* p = ls + (a & kSpuLsMask & ~0xFu);
#ifdef __SSSE3__
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.b));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_shuffle_epi8(v, reverse_bytes()));
#else
        const uint64_t hi = __builtin_bswap64(r.d[1]), lo = __builtin_bswap64(r.d[0]);
        std::memcpy(p, &hi, 8);
        std::memcpy(p + 8, &lo, 8);
#endif
    }
#ifdef __SSSE3__
    static __m128i reverse_bytes() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
#endif
};

// Local store with the alignment SIMD loads like (one per SPU).
struct SpuLocalStore {
    alignas(128) uint8_t data[0x40000];
};

}  // namespace sly4
