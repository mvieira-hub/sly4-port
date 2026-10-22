// cellAudio: the game mixes everything itself (SCREAM, on the SPUs) and writes float PCM
// into ring buffers of up to 8 "ports"; the audio hardware reads one block of 256
// samples per port every 5.33 ms (48 kHz) and advances each port's read index, which
// the game watches to know where to write next. This module keeps that clock running,
// as RPCS3's cellAudio.cpp does. Nothing is played yet (phase 6): the blocks are
// consumed, measured (peak level, for the summary) and cleared.
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/host_sleep.h"
#include "core/log.h"
#include "kernel/guest_alloc.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

constexpr uint32_t CELL_AUDIO_ERROR_ALREADY_INIT = 0x80310701, CELL_AUDIO_ERROR_NOT_INIT = 0x80310703,
                   CELL_AUDIO_ERROR_PARAM = 0x80310704, CELL_AUDIO_ERROR_PORT_FULL = 0x80310705,
                   CELL_AUDIO_ERROR_PORT_ALREADY_RUN = 0x80310706, CELL_AUDIO_ERROR_PORT_NOT_OPEN = 0x80310707,
                   CELL_AUDIO_ERROR_PORT_NOT_RUN = 0x80310708;
constexpr uint32_t kStatusReady = 1, kStatusRun = 2, kStatusClose = 0x1010;
constexpr uint32_t kPorts = 8, kSamples = 256, kMaxChannels = 8, kMaxBlocks = 16;
constexpr uint32_t kPortBytes = kMaxChannels * kMaxBlocks * kSamples * 4;

struct Port {
    uint32_t status = kStatusClose;
    uint32_t channels = 0, blocks = 0;
    float level = 1.0f;
    uint64_t position = 0;  // blocks consumed
};

struct Audio {
    std::mutex lock;
    bool initialized = false, thread_started = false;
    GuestAddr buffers = 0, indexes = 0;
    Port ports[kPorts];
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint32_t> peak_bits{0};  // max |sample| seen, as float bits
};
Audio g;

GuestAddr port_buffer(uint32_t n) { return g.buffers + kPortBytes * n; }

// Every 5.33 ms: each running port gives up its current block.
void audio_thread() {
    GuestMemory& m = kernel().mem();
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000ull * kSamples / 48000);
    while (!kernel().exited()) {
        next += period;
        host_sleep_until(next);
        std::lock_guard<std::mutex> lk(g.lock);
        for (uint32_t n = 0; n < kPorts; ++n) {
            Port& p = g.ports[n];
            if (p.status != kStatusRun || !p.blocks) continue;
            const uint32_t block = uint32_t(p.position % p.blocks);
            const uint32_t bytes = p.channels * kSamples * 4;
            const GuestAddr a = port_buffer(n) + block * bytes;
            float peak = 0;
            for (uint32_t i = 0; i < bytes; i += 4) {
                const float s = std::fabs(std::bit_cast<float>(m.read<uint32_t>(a + i)));
                if (s > peak && std::isfinite(s)) peak = s;
            }
            if (peak > std::bit_cast<float>(g.peak_bits.load())) g.peak_bits = std::bit_cast<uint32_t>(peak);
            std::memset(m.host(a), 0, bytes);
            ++p.position;
            m.write<uint64_t>(g.indexes + 8 * n, p.position % p.blocks);
            g.blocks.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void cellAudioInit(PPUContext& c, GuestMemory& m) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_ALREADY_INIT;
        return;
    }
    if (!g.buffers) {
        g.buffers = kernel().main_memory().alloc(kPortBytes * kPorts, 0x10000);
        g.indexes = kernel().main_memory().alloc(8 * kPorts, 0x80);
        if (!g.buffers || !g.indexes) {
            c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
            return;
        }
    }
    std::memset(m.host(g.buffers), 0, kPortBytes * kPorts);
    std::memset(m.host(g.indexes), 0, 8 * kPorts);
    for (Port& p : g.ports) p = Port{};
    g.initialized = true;
    if (!g.thread_started) {
        g.thread_started = true;
        std::thread(audio_thread).detach();
    }
    c.gpr[3] = CELL_OK;
}

void cellAudioQuit(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
        return;
    }
    for (Port& p : g.ports) p = Port{};
    g.initialized = false;
    c.gpr[3] = CELL_OK;
}

// cellAudioPortOpen(CellAudioPortParam* param, u32* portNum); the param is
// {u64 nChannel, u64 nBlock, u64 attr, float level}.
void cellAudioPortOpen(PPUContext& c, GuestMemory& m) {
    const GuestAddr param = a32(c, 3);
    const uint64_t channels = m.read<uint64_t>(param), blocks = m.read<uint64_t>(param + 8);
    const uint64_t attr = m.read<uint64_t>(param + 16);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
        return;
    }
    if ((channels != 2 && channels != 8) || (blocks != 2 && blocks != 4 && blocks != 8 && blocks != 16)) {
        c.gpr[3] = CELL_AUDIO_ERROR_PARAM;
        return;
    }
    for (uint32_t n = 0; n < kPorts; ++n) {
        Port& p = g.ports[n];
        if (p.status != kStatusClose) continue;
        p = Port{};
        p.status = kStatusReady;
        p.channels = uint32_t(channels);
        p.blocks = uint32_t(blocks);
        p.level = (attr & 0x1000) ? std::bit_cast<float>(m.read<uint32_t>(param + 24)) : 1.0f;  // INITLEVEL
        std::memset(m.host(port_buffer(n)), 0, kPortBytes);
        m.write<uint64_t>(g.indexes + 8 * n, 0);
        m.write<uint32_t>(a32(c, 4), n);
        SLY4_INFO("cellAudio: porta %u aberta, %u canais, %u blocos, buffer em 0x%08X", n, p.channels, p.blocks,
                  port_buffer(n));
        c.gpr[3] = CELL_OK;
        return;
    }
    c.gpr[3] = CELL_AUDIO_ERROR_PORT_FULL;
}

// cellAudioGetPortConfig(portNum, CellAudioPortConfig*): {u32 readIndexAddr, u32 status,
// u64 nChannel, u64 nBlock, u32 portSize, u32 portAddr}.
void cellAudioGetPortConfig(PPUContext& c, GuestMemory& m) {
    const uint32_t n = a32(c, 3);
    const GuestAddr cfg = a32(c, 4);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
        return;
    }
    if (n >= kPorts || !cfg) {
        c.gpr[3] = CELL_AUDIO_ERROR_PARAM;
        return;
    }
    const Port& p = g.ports[n];
    m.write<uint32_t>(cfg, g.indexes + 8 * n);
    m.write<uint32_t>(cfg + 4, p.status);
    m.write<uint64_t>(cfg + 8, p.channels);
    m.write<uint64_t>(cfg + 16, p.blocks);
    m.write<uint32_t>(cfg + 24, p.channels * p.blocks * kSamples * 4);
    m.write<uint32_t>(cfg + 28, port_buffer(n));
    c.gpr[3] = CELL_OK;
}

template <bool Start>
void cellAudioPortStartStop(PPUContext& c, GuestMemory&) {
    const uint32_t n = a32(c, 3);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
        return;
    }
    if (n >= kPorts) {
        c.gpr[3] = CELL_AUDIO_ERROR_PARAM;
        return;
    }
    Port& p = g.ports[n];
    if (p.status == kStatusClose) {
        c.gpr[3] = CELL_AUDIO_ERROR_PORT_NOT_OPEN;
        return;
    }
    if (Start && p.status == kStatusRun) {
        c.gpr[3] = CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
        return;
    }
    if (!Start && p.status != kStatusRun) {
        c.gpr[3] = CELL_AUDIO_ERROR_PORT_NOT_RUN;
        return;
    }
    p.status = Start ? kStatusRun : kStatusReady;
    c.gpr[3] = CELL_OK;
}

void cellAudioPortClose(PPUContext& c, GuestMemory&) {
    const uint32_t n = a32(c, 3);
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.initialized) {
        c.gpr[3] = CELL_AUDIO_ERROR_NOT_INIT;
        return;
    }
    if (n >= kPorts || g.ports[n].status == kStatusClose) {
        c.gpr[3] = n >= kPorts ? CELL_AUDIO_ERROR_PARAM : CELL_AUDIO_ERROR_PORT_NOT_OPEN;
        return;
    }
    g.ports[n] = Port{};
    c.gpr[3] = CELL_OK;
}

}  // namespace

HleModule hle_cell_audio() {
    return {"cellAudio",
            {
                {"cellAudioInit", cellAudioInit},
                {"cellAudioQuit", cellAudioQuit},
                {"cellAudioPortOpen", cellAudioPortOpen},
                {"cellAudioGetPortConfig", cellAudioGetPortConfig},
                {"cellAudioPortStart", cellAudioPortStartStop<true>},
                {"cellAudioPortStop", cellAudioPortStartStop<false>},
                {"cellAudioPortClose", cellAudioPortClose},
            }};
}

void audio_summary() {
    if (!g.buffers) return;
    SLY4_INFO("cellAudio: %" PRIu64 " blocos de 256 amostras consumidos, pico %.3f", g.blocks.load(),
              double(std::bit_cast<float>(g.peak_bits.load())));
}

}  // namespace sly4
