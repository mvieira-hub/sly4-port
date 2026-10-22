// SPU channels (rdch/wrch/rchcnt). The MFC (DMA between local store and main memory,
// tag status, atomic 128-byte lines), the decrementer and the event mask live here;
// mailboxes, signal notification and event sources belong to whoever runs the SPU
// (SPU thread group or SPURS) and are reached through SpuIo.
#pragma once

#include <cstdint>

#include "cpu/spu_context.h"

namespace sly4 {

struct SPUContext;

enum class SpuChannelResult : uint8_t { Ok, Wait, Invalid };

namespace spu_ch {
constexpr uint32_t RdEventStat = 0, WrEventMask = 1, WrEventAck = 2, RdSigNotify1 = 3, RdSigNotify2 = 4,
                   WrDec = 7, RdDec = 8, MFC_WrMSSyncReq = 9, RdEventMask = 11, MFC_RdTagMask = 12, RdMachStat = 13,
                   WrSRR0 = 14, RdSRR0 = 15,
                   MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18, MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21,
                   MFC_WrTagMask = 22, MFC_WrTagUpdate = 23, MFC_RdTagStat = 24, MFC_RdListStallStat = 25,
                   MFC_WrListStallAck = 26, MFC_RdAtomicStat = 27, WrOutMbox = 28, RdInMbox = 29, WrOutIntrMbox = 30,
                   // performance monitor: a bookmark for the trace, start and stop events
                   SetBkmkTag = 69, PmStartEv = 70, PmStopEv = 71;
}

// Events (SPU_RdEventStat bits).
namespace spu_event {
constexpr uint32_t LlrLost = 0x400, Sn1 = 0x200, Sn2 = 0x100, OutMbox = 0x80, OutIntrMbox = 0x40, Decrementer = 0x20,
                   InMbox = 0x10, QueueVacancy = 0x8, ListStall = 0x2, TagGroup = 0x1;
}

class SpuIo {
public:
    virtual ~SpuIo() = default;
    // Channels not handled by the MFC: return Wait to block (the instruction is retried).
    virtual SpuChannelResult read(SPUContext& c, uint32_t ch, uint32_t& value) = 0;
    virtual SpuChannelResult write(SPUContext& c, uint32_t ch, uint32_t value) = 0;
    virtual uint32_t count(SPUContext& c, uint32_t ch) = 0;
    // Event conditions raised from outside (mailboxes, signals, lost reservation), for
    // interrupts; the owner must not hold its own lock when this is called.
    virtual uint32_t events(SPUContext&) { return 0; }
};

SpuChannelResult spu_read_channel(SPUContext& c, uint32_t ch, uint32_t& value);
SpuChannelResult spu_write_channel(SPUContext& c, uint32_t ch, uint32_t value);
uint32_t spu_channel_count(SPUContext& c, uint32_t ch);

// The decrementer event (bit 0x20) once the decrementer, armed by a write with its top
// bit clear, has gone negative.
void spu_check_decrementer(SPUContext& c);
// Updates c.event_stat (decrementer and SpuIo::events) and returns the pending events in
// the mask.
uint32_t spu_update_events(SPUContext& c);
// With interrupts on and an event pending: SRR0 = PC, PC = 0, interrupts off. Called
// between blocks / taken branches. True if the interrupt was taken.
inline bool spu_check_interrupt(SPUContext& c) {
    if (!c.irq || !spu_update_events(c)) return false;
    c.srr0 = c.pc;
    c.pc = 0;
    c.irq = false;
    return true;
}

// MFC command (MFC_Cmd) with the parameters already in c.mfc. DMA completes at once.
SpuChannelResult spu_mfc_command(SPUContext& c, uint32_t cmd);

}  // namespace sly4
