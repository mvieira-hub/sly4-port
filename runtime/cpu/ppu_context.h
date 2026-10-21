// Architectural state of one PPU thread (Cell PowerPC64, user mode, 64-bit mode).
#pragma once

#include <cstdint>
#include <cstring>

#include "memory/guest_memory.h"

namespace sly4 {

// FPSCR bits, Power ISA numbering converted to masks (bit 0 = 0x80000000).
namespace fpscr {
constexpr uint32_t FX = 1u << 31, FEX = 1u << 30, VX = 1u << 29, OX = 1u << 28, UX = 1u << 27, ZX = 1u << 26,
                   XX = 1u << 25, VXSNAN = 1u << 24, VXISI = 1u << 23, VXIDI = 1u << 22, VXZDZ = 1u << 21,
                   VXIMZ = 1u << 20, VXVC = 1u << 19, FR = 1u << 18, FI = 1u << 17, FPRF = 0x1Fu << 12,
                   VXSOFT = 1u << 10, VXSQRT = 1u << 9, VXCVI = 1u << 8, VE = 1u << 7, OE = 1u << 6, UE = 1u << 5,
                   ZE = 1u << 4, XE = 1u << 3, NI = 1u << 2, RN = 3u;
constexpr uint32_t VX_ALL = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI;
}  // namespace fpscr

namespace vscr {
constexpr uint32_t NJ = 1u << 16;  // non-Java mode: denormals flushed to zero
constexpr uint32_t SAT = 1u;       // sticky saturation
}  // namespace vscr

struct PPUContext {
    uint64_t gpr[32] = {};
    uint64_t fpr[32] = {};  // raw IEEE double bits (NaN payloads must survive moves)
    Vec128 vr[32] = {};     // GuestMemory convention: bytes reversed (b[15] = guest byte 0)
    uint32_t cr = 0;        // CR0 in the top nibble (ISA bit 0 = 0x80000000)
    uint64_t lr = 0, ctr = 0;
    bool xer_so = false, xer_ov = false, xer_ca = false;
    uint8_t xer_count = 0;  // XER[57:63], used by lswx/stswx (unused by the game)
    uint32_t fpscr = 0;
    uint32_t vscr = vscr::NJ;
    uint32_t vrsave = 0;
    uint32_t cia = 0;  // address of the instruction being executed

    // lwarx/ldarx reservation (emulated with compare-and-swap on the reserved value).
    bool reserved = false;
    uint32_t reserve_addr = 0;
    uint64_t reserve_value = 0;
    uint8_t reserve_size = 0;

    uint64_t xer() const {
        return (uint64_t(xer_so) << 31) | (uint64_t(xer_ov) << 30) | (uint64_t(xer_ca) << 29) | xer_count;
    }
    void set_xer(uint64_t v) {
        xer_so = (v >> 31) & 1;
        xer_ov = (v >> 30) & 1;
        xer_ca = (v >> 29) & 1;
        xer_count = v & 0x7F;
    }

    // CR field n (0..7) as a 4-bit value LT GT EQ SO.
    uint32_t crf(uint32_t n) const { return (cr >> (28 - 4 * n)) & 0xF; }
    void set_crf(uint32_t n, uint32_t v) {
        const uint32_t shift = 28 - 4 * n;
        cr = (cr & ~(0xFu << shift)) | ((v & 0xF) << shift);
    }
    bool crbit(uint32_t b) const { return (cr >> (31 - b)) & 1; }
    void set_crbit(uint32_t b, bool v) { cr = v ? cr | (1u << (31 - b)) : cr & ~(1u << (31 - b)); }

    double f(uint32_t r) const {
        double d;
        std::memcpy(&d, &fpr[r], 8);
        return d;
    }
    void set_f(uint32_t r, double d) { std::memcpy(&fpr[r], &d, 8); }
};

}  // namespace sly4
