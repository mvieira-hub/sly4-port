#include "cpu/spu_channels.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "cpu/guest_atomic.h"
#include "cpu/spu_context.h"

namespace sly4 {

namespace {

constexpr uint32_t kAtomicPutllcFailure = 1, kAtomicPutllucSuccess = 2, kAtomicGetllarSuccess = 4;

uint64_t now_ticks() {  // 79.8 MHz timebase, as the decrementer counts on the PS3
    using namespace std::chrono;
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) * 798 / 10000;
}

// Debug label for --watch-line: which SPU (by local store), the SPURS task in it and $0.
const char* spu_who(const SPUContext& c, const char* op, char (&buf)[96]) {
    static std::mutex m;
    static std::vector<const uint8_t*> seen;
    size_t idx;
    {
        std::lock_guard<std::mutex> g(m);
        auto it = std::find(seen.begin(), seen.end(), c.ls);
        idx = size_t(it - seen.begin());
        if (it == seen.end()) seen.push_back(c.ls);
    }
    std::snprintf(buf, sizeof(buf), "SPU#%zu %s tarefa %u lr 0x%05X", idx, op, c.ls_u32(0x27D4),
                  c.gpr[0].pref());
    return buf;
}

void transfer(SPUContext& c, bool put, uint32_t lsa, uint32_t ea, uint32_t size) {
    lsa &= kSpuLsMask;
    if (lsa + size > kSpuLsSize) size = kSpuLsSize - lsa;  // never past the local store
    uint8_t* main = c.mem->host(ea);
    GuestAddr lines[kMaxDebugLines];
    if (const int n = !put ? debug_lines_hit(ea, size, lines) : 0) {  // reads: only who and when
        char who[96];
        spu_who(c, "get (DMA)", who);
        debug_line_log(*c.mem, lines[0], c.mem->host(lines[0]), who, c.pc);
    }
    if (const int n = put ? debug_lines_hit(ea, size, lines) : 0) {
        uint8_t before[kMaxDebugLines][128];
        for (int i = 0; i < n; ++i) std::memcpy(before[i], c.mem->host(lines[i]), 128);
        std::memcpy(main, c.ls + lsa, size);
        char who[96];
        spu_who(c, "put (DMA)", who);
        for (int i = 0; i < n; ++i) debug_line_log(*c.mem, lines[i], before[i], who, c.pc);
        return;
    }
    if (put)
        std::memcpy(main, c.ls + lsa, size);
    else
        std::memcpy(c.ls + lsa, main, size);
}

// Runs a DMA list from where it is. The list (8-byte elements: stall-and-notify flag in
// bit 31 + size, then the low EA word) is in local store; each element's data follows
// the previous one, 16-byte aligned. An element with the flag stops the list after its
// transfer and raises the list stall event until the SPU acknowledges it.
void run_list(SPUContext& c, uint32_t tag) {
    SPUContext::StalledList& l = c.stalled[tag];
    while (l.active && l.left >= 8) {
        const uint32_t e0 = c.ls_u32(l.list), e1 = c.ls_u32(l.list + 4);
        const uint32_t n = e0 & 0x7FFF;
        if (n) transfer(c, l.put, l.lsa, e1, n);
        l.lsa = (l.lsa + n + 15) & ~15u;
        l.list = (l.list + 8) & kSpuLsMask;
        l.left -= 8;
        if (e0 & 0x80000000u) {
            c.list_stall_stat |= 1u << tag;
            c.event_stat |= spu_event::ListStall;
            return;
        }
    }
    l.active = false;
}

}  // namespace

SpuChannelResult spu_mfc_command(SPUContext& c, uint32_t cmd) {
    const uint32_t lsa = c.mfc.lsa, ea = c.mfc.eal, size = c.mfc.size;
    switch (cmd) {
        case 0x20: case 0x21: case 0x22:  // put, putb, putf
            transfer(c, true, lsa, ea, size);
            return SpuChannelResult::Ok;
        case 0x40: case 0x41: case 0x42:  // get, getb, getf
            transfer(c, false, lsa, ea, size);
            return SpuChannelResult::Ok;
        case 0x24: case 0x25: case 0x26:    // putl, putlb, putlf
        case 0x44: case 0x45: case 0x46: {  // getl, getlb, getlf
            SPUContext::StalledList& l = c.stalled[c.mfc.tag & 31];
            l = {true, cmd < 0x40, lsa, ea & kSpuLsMask, size & ~7u};
            run_list(c, c.mfc.tag & 31);
            return SpuChannelResult::Ok;
        }
        case 0xD0: {  // getllar: load a 128-byte line and reserve it
            const uint32_t line = ea & ~127u;
            std::lock_guard<std::mutex> g(guest_line_lock(line));
            std::memcpy(c.reserve_data, c.mem->host(line), 128);
            std::memcpy(c.ls + (lsa & kSpuLsMask & ~127u), c.reserve_data, 128);
            c.reserved = true;
            c.reserve_ea = line;
            __atomic_store_n(&c.llr_lost, 0u, __ATOMIC_RELEASE);
            c.atomic_stat = kAtomicGetllarSuccess;
            return SpuChannelResult::Ok;
        }
        case 0xB4: {  // putllc: store the line if the reservation still holds
            const uint32_t line = ea & ~127u;
            bool stored = false;
            {
                std::lock_guard<std::mutex> g(guest_line_lock(line));
                uint8_t* p = c.mem->host(line);
                if (c.reserved && c.reserve_ea == line && std::memcmp(p, c.reserve_data, 128) == 0) {
                    const bool dbg = debug_line_hit(line, 128) != 0;
                    uint8_t before[128];
                    if (dbg) std::memcpy(before, p, 128);
                    guest_store_line_changes(p, c.reserve_data, c.ls + (lsa & kSpuLsMask & ~127u));
                    if (dbg) {
                        char who[96];
                        debug_line_log(*c.mem, line, before, spu_who(c, "putllc", who), c.pc);
                    }
                    stored = true;
                }
            }
            c.atomic_stat = stored ? 0 : kAtomicPutllcFailure;
            c.reserved = false;
            if (stored) guest_line_written(line);
            return SpuChannelResult::Ok;
        }
        case 0xB0:    // putlluc
        case 0xB8: {  // putqlluc
            {
                std::lock_guard<std::mutex> g(guest_line_lock(ea & ~127u));
                uint8_t before[128];
                const bool dbg = debug_line_hit(ea & ~127u, 128) != 0;
                if (dbg) std::memcpy(before, c.mem->host(ea & ~127u), 128);
                std::memcpy(c.mem->host(ea & ~127u), c.ls + (lsa & kSpuLsMask & ~127u), 128);
                if (dbg) {
                    char who[96];
                    debug_line_log(*c.mem, ea & ~127u, before, spu_who(c, "putlluc", who), c.pc);
                }
            }
            if (cmd == 0xB0) c.atomic_stat = kAtomicPutllucSuccess;
            if (c.reserved && c.reserve_ea == (ea & ~127u)) c.reserved = false;
            guest_line_written(ea);
            return SpuChannelResult::Ok;
        }
        case 0xC0: case 0xC8: case 0xCC:  // barrier, eieio, sync
            return SpuChannelResult::Ok;
        default:
            SLY4_WARN("SPU: comando MFC 0x%X sem implementação (lsa 0x%X, ea 0x%X, tamanho 0x%X)", cmd, lsa, ea, size);
            return SpuChannelResult::Invalid;
    }
}

void spu_check_decrementer(SPUContext& c) {
    if (c.dec_armed && int32_t(c.decrementer - uint32_t(now_ticks())) < 0) {
        c.dec_armed = false;
        c.event_stat |= spu_event::Decrementer;
    }
}

uint32_t spu_update_events(SPUContext& c) {
    spu_check_decrementer(c);
    if (c.io) c.event_stat |= c.io->events(c);
    return c.event_stat & c.event_mask;
}

SpuChannelResult spu_read_channel(SPUContext& c, uint32_t ch, uint32_t& v) {
    switch (ch) {
        case spu_ch::MFC_RdTagStat: v = c.tag_mask; return SpuChannelResult::Ok;  // DMA is already done
        case spu_ch::MFC_RdTagMask: v = c.tag_mask; return SpuChannelResult::Ok;
        case spu_ch::MFC_RdAtomicStat: v = c.atomic_stat; return SpuChannelResult::Ok;
        case spu_ch::MFC_RdListStallStat:
            v = c.list_stall_stat;
            c.list_stall_stat = 0;
            return SpuChannelResult::Ok;
        case spu_ch::RdDec: v = c.decrementer - uint32_t(now_ticks()); return SpuChannelResult::Ok;
        case spu_ch::RdEventMask: v = c.event_mask; return SpuChannelResult::Ok;
        case spu_ch::RdSRR0: v = c.srr0; return SpuChannelResult::Ok;
        case spu_ch::RdMachStat: v = 0; return SpuChannelResult::Ok;
        case spu_ch::RdEventStat:
            spu_check_decrementer(c);
            if ((v = c.event_stat & c.event_mask) != 0) return SpuChannelResult::Ok;
            break;  // nothing pending: the owner decides (wait for an event source)
        default: break;
    }
    return c.io ? c.io->read(c, ch, v) : SpuChannelResult::Invalid;
}

SpuChannelResult spu_write_channel(SPUContext& c, uint32_t ch, uint32_t v) {
    switch (ch) {
        case spu_ch::MFC_LSA: c.mfc.lsa = v; return SpuChannelResult::Ok;
        case spu_ch::MFC_EAH: c.mfc.eah = v; return SpuChannelResult::Ok;
        case spu_ch::MFC_EAL: c.mfc.eal = v; return SpuChannelResult::Ok;
        case spu_ch::MFC_Size: c.mfc.size = v & 0x7FFF; return SpuChannelResult::Ok;
        case spu_ch::MFC_TagID: c.mfc.tag = v & 0x1F; return SpuChannelResult::Ok;
        case spu_ch::MFC_Cmd: return spu_mfc_command(c, v & 0xFF);
        case spu_ch::MFC_WrTagMask: c.tag_mask = v; return SpuChannelResult::Ok;
        case spu_ch::MFC_WrTagUpdate: c.tag_update = v; return SpuChannelResult::Ok;
        case spu_ch::MFC_WrListStallAck:
            if (c.stalled[v & 31].active) run_list(c, v & 31);
            return SpuChannelResult::Ok;
        case spu_ch::MFC_WrMSSyncReq: return SpuChannelResult::Ok;  // DMA is synchronous already
        case spu_ch::WrDec:
            c.decrementer = v + uint32_t(now_ticks());
            c.dec_armed = !(v & 0x80000000u);
            return SpuChannelResult::Ok;
        case spu_ch::WrEventMask: c.event_mask = v; return SpuChannelResult::Ok;
        case spu_ch::WrEventAck: c.event_stat &= ~v; return SpuChannelResult::Ok;
        case spu_ch::WrSRR0: c.srr0 = v; return SpuChannelResult::Ok;
        case spu_ch::SetBkmkTag: case spu_ch::PmStartEv: case spu_ch::PmStopEv:
            return SpuChannelResult::Ok;  // no performance monitor: nothing to record
        default: break;
    }
    return c.io ? c.io->write(c, ch, v) : SpuChannelResult::Invalid;
}

uint32_t spu_channel_count(SPUContext& c, uint32_t ch) {
    switch (ch) {
        case spu_ch::MFC_Cmd: return 16;  // the queue is always empty
        case spu_ch::MFC_LSA: case spu_ch::MFC_EAH: case spu_ch::MFC_EAL: case spu_ch::MFC_Size:
        case spu_ch::MFC_TagID: case spu_ch::MFC_WrTagMask: case spu_ch::MFC_WrTagUpdate:
        case spu_ch::MFC_RdTagStat: case spu_ch::MFC_RdAtomicStat: case spu_ch::WrDec: case spu_ch::RdDec:
        case spu_ch::WrEventMask: case spu_ch::WrEventAck: case spu_ch::RdEventMask: case spu_ch::RdSRR0:
        case spu_ch::WrSRR0: case spu_ch::RdMachStat: case spu_ch::MFC_WrListStallAck: case spu_ch::MFC_RdTagMask:
        case spu_ch::MFC_WrMSSyncReq: case spu_ch::SetBkmkTag: case spu_ch::PmStartEv: case spu_ch::PmStopEv:
            return 1;
        case spu_ch::MFC_RdListStallStat: return c.list_stall_stat ? 1 : 0;
        case spu_ch::RdEventStat:
            spu_check_decrementer(c);
            if (c.event_stat & c.event_mask) return 1;
            break;
        default: break;
    }
    return c.io ? c.io->count(c, ch) : 0;
}

}  // namespace sly4
