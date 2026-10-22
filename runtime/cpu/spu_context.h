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

    uint32_t ls_u32(uint32_t a) const { return load_be<uint32_t>(ls + (a & kSpuLsMask)); }
    void read_quad(uint32_t a, SpuReg& r) const {
        const uint8_t* p = ls + (a & kSpuLsMask & ~0xFu);
        for (int i = 0; i < 16; ++i) r.b[15 - i] = p[i];
    }
    void write_quad(uint32_t a, const SpuReg& r) {
        uint8_t* p = ls + (a & kSpuLsMask & ~0xFu);
        for (int i = 0; i < 16; ++i) p[i] = r.b[15 - i];
    }
};

// Local store with the alignment SIMD loads like (one per SPU).
struct SpuLocalStore {
    alignas(128) uint8_t data[0x40000];
};

}  // namespace sly4
