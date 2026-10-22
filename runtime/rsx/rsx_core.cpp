// The RSX thread: FIFO, state, synchronisation and the 2D engines (docs/GPU.md).
#include "rsx/rsx.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <thread>

#include "core/host_sleep.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "rsx/rsx_backend.h"
#include "rsx/rsx_state.h"

namespace sly4::rsx {

namespace {

constexpr uint32_t kUnmapped = 0xFFFF;
constexpr uint32_t kPages = 4096;  // 1 MiB pages of the 4 GiB space

// RSX command words.
constexpr uint32_t kOldJumpMask = 0xE0000003, kOldJump = 0x20000000, kNewJump = 1, kCall = 2, kReturn = 0x00020000;
constexpr uint32_t kNonIncrement = 0x40000000;
// Context DMA handles of the semaphore/report areas.
constexpr uint32_t kDmaLocal = 0xFEED0000, kDmaMain = 0xFEED0001, kDmaSemaphoreRw = 0x66606660,
                   kDmaSemaphoreR = 0x66616661, kDmaReportLocal = 0x66626660, kDmaReportMain = 0xBAD68000;

struct IoMap {
    std::mutex lock;
    uint16_t io_to_ea[kPages], ea_to_io[kPages], pages[kPages];
    IoMap() {
        std::fill(std::begin(io_to_ea), std::end(io_to_ea), kUnmapped);
        std::fill(std::begin(ea_to_io), std::end(ea_to_io), kUnmapped);
        std::fill(std::begin(pages), std::end(pages), 0);
    }
};
IoMap g_io;

std::mutex g_display_lock;
DisplayBuffer g_display[8];

Backend* g_backend = nullptr;
GuestMemory* g_mem = nullptr;
GuestAddr g_control = 0, g_labels = 0;

struct Counters {
    std::atomic<uint64_t> commands{0}, flips{0}, draws{0}, vertices{0}, clears{0}, copies{0}, blits{0};
};
Counters g_count;

// Frame capture (--rsx-capture).
uint32_t g_capture_frame = UINT32_MAX;
std::string g_capture_path;
FILE* g_capture = nullptr;

// ---------------------------------------------------------------- the command processor
class Processor {
public:
    // The context DMAs start where libgcm expects them (RPCS3's method_registers.reset()).
    explicit Processor(GuestMemory& m) : m_(m) {
        using namespace nv4097;
        s_.set(nv406e::SemaphoreContext, kDmaSemaphoreRw);
        s_.set(SetContextDmaSemaphore, kDmaSemaphoreRw);
        s_.set(SetContextDmaReport, kDmaReportLocal);
        for (uint32_t r : {SetContextDmaColorA, SetContextDmaColorB, SetContextDmaColorC, SetContextDmaColorD,
                           SetContextDmaZeta, SetContextDmaVertexA, SetContextDmaVertexB})
            s_.set(r, kDmaLocal);
    }
    void run();

private:
    GuestMemory& m_;
    State s_;
    uint32_t call_return_ = 0;
    bool in_call_ = false;
    DrawCommand draw_;
    bool in_draw_ = false;

    GuestAddr dma_address(uint32_t dma, uint32_t offset);
    void method(uint32_t reg, uint32_t arg, uint32_t index);
    void end_draw();
    void copy_buffer();  // NV0039
    void image_from_cpu(uint32_t index, uint32_t arg);  // NV308A
    void scaled_blit();  // NV3089
    void capture(uint32_t reg, uint32_t arg);
    bool vertex_data(uint32_t reg, uint32_t arg);  // SET_VERTEX_DATA*_M
};

GuestAddr Processor::dma_address(uint32_t dma, uint32_t offset) {
    uint32_t ea = 0;
    switch (dma) {
        case kDmaLocal: return kLocalBase + offset;
        case kDmaMain: return io_to_ea(offset, ea) ? ea : 0;
        case kDmaSemaphoreRw:
        case kDmaSemaphoreR: return g_labels + (offset & 0xFF0);
        case kDmaReportLocal: return kLocalBase + kReportsOffset + offset;
        case kDmaReportMain: return io_to_ea(kReportsOffset + offset, ea) ? ea : 0;
        default: return 0;
    }
}

uint64_t timestamp_ns() { return guest_system_time() * 1000; }

void Processor::capture(uint32_t reg, uint32_t arg) {
    if (g_count.flips.load() != g_capture_frame) {
        if (g_capture) {
            std::fclose(g_capture);
            g_capture = nullptr;
            g_capture_frame = UINT32_MAX;
            SLY4_INFO("RSX: quadro capturado em %s", g_capture_path.c_str());
        }
        return;
    }
    if (!g_capture) g_capture = std::fopen(g_capture_path.c_str(), "w");
    if (!g_capture) return;
    uint32_t index;
    const char* name = method_name(reg, index);
    float f;
    std::memcpy(&f, &arg, 4);
    if (name)
        std::fprintf(g_capture, "%04X %s[%u] = 0x%08X (%g)\n", reg, name, index, arg, double(f));
    else
        std::fprintf(g_capture, "%04X ? = 0x%08X\n", reg, arg);
}

void Processor::method(uint32_t reg, uint32_t arg, uint32_t index) {
    using namespace nv4097;
    if (g_capture_frame != UINT32_MAX) capture(reg, arg);
    // Registers whose value is a stream (program words, constants, inline data) keep their
    // own memories; everything else is just stored.
    if (reg >= SetTransformProgram && reg < SetTransformProgram + 0x80) {
        const uint32_t slot = (reg - SetTransformProgram) / 4 % 4;
        const uint32_t at = (s_.transform_program_load % kTransformProgramSize) * 4 + slot;
        s_.transform_program[at] = arg;
        if (slot == 3) ++s_.transform_program_load;
        return;
    }
    if (reg >= SetTransformConstant && reg < SetTransformConstant + 0x80) {
        const uint32_t i = (reg - SetTransformConstant) / 4;
        const uint32_t at = (s_.reg(SetTransformConstantLoad) + i / 4) * 4 + i % 4;
        if (at < s_.transform_constants.size()) std::memcpy(&s_.transform_constants[at], &arg, 4);
        return;
    }
    if (vertex_data(reg, arg)) return;
    if (reg >= nv308a::Color && reg < nv308a::Color + 0x700 * 4) {
        image_from_cpu((reg - nv308a::Color) / 4, arg);
        return;
    }
    s_.set(reg, arg);
    switch (reg) {
        case SetTransformProgramLoad: s_.transform_program_load = arg; break;
        case nv406e::SetReference: m_.write<uint32_t>(g_control + 8, arg); break;
        case nv406e::SemaphoreAcquire:
            // Waits for the PPU (or a release earlier in the FIFO) to write the value.
            if (GuestAddr a = dma_address(s_.reg(nv406e::SemaphoreContext), s_.reg(nv406e::SemaphoreOffset)))
                for (int i = 0; m_.read<uint32_t>(a) != arg && !kernel().exited(); ++i) host_sleep_us(i < 100 ? 10 : 500);
            break;
        case nv406e::SemaphoreRelease:
            if (g_backend) g_backend->sync(m_);
            if (GuestAddr a = dma_address(s_.reg(nv406e::SemaphoreContext), s_.reg(nv406e::SemaphoreOffset)))
                m_.write<uint32_t>(a, arg);
            break;
        case BackEndWriteSemaphoreRelease:
            if (g_backend) g_backend->sync(m_);
            if (GuestAddr a = dma_address(s_.reg(SetContextDmaSemaphore), s_.reg(SetSemaphoreOffset)))
                m_.write<uint32_t>(a, (arg & 0xFF00FF00) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
            break;
        case TextureReadSemaphoreRelease:
            if (GuestAddr a = dma_address(s_.reg(SetContextDmaSemaphore), s_.reg(SetSemaphoreOffset)))
                m_.write<uint32_t>(a, arg);
            break;
        case GetReport:
            // {u64 timestamp, u32 value, u32 pad}; occlusion counts are not measured (0).
            if (GuestAddr a = dma_address(s_.reg(SetContextDmaReport), arg & 0xFFFFFF)) {
                m_.write<uint64_t>(a, timestamp_ns());
                m_.write<uint32_t>(a + 8, 0);
                m_.write<uint32_t>(a + 12, 0);
            }
            break;
        case ClearSurface:
            g_count.clears.fetch_add(1, std::memory_order_relaxed);
            if (g_backend) g_backend->clear(m_, s_, arg);
            break;
        case SetBeginEnd:
            if (arg) {
                draw_ = DrawCommand{};
                draw_.primitive = Primitive(arg);
                in_draw_ = true;
            } else if (in_draw_) {
                end_draw();
            }
            break;
        case DrawArrays:
        case DrawIndexArray:
            draw_.kind = reg == DrawArrays ? DrawCommand::Kind::Arrays : DrawCommand::Kind::Indexed;
            draw_.ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1});
            break;
        case InlineArray:
            draw_.kind = DrawCommand::Kind::Inline;
            draw_.inline_data.push_back(arg);
            break;
        case ArrayElement16:
            draw_.kind = DrawCommand::Kind::Inline;
            draw_.inline_data.push_back(arg & 0xFFFF);
            draw_.inline_data.push_back(arg >> 16);
            break;
        case ArrayElement32:
            draw_.kind = DrawCommand::Kind::Inline;
            draw_.inline_data.push_back(arg);
            break;
        case nv0039::BufferNotify: copy_buffer(); break;
        case nv3089::ImageInOffset: break;
        case nv3089::ImageIn: scaled_blit(); break;
        case GcmFlipCommand: {
            const uint64_t n = g_count.flips.fetch_add(1) + 1;
            if (g_backend) g_backend->flip(m_, s_, arg);
            if (n == 1 || n % 600 == 0) SLY4_INFO("RSX: %" PRIu64 " flips (buffer %u)", n, arg);
            break;
        }
        default: break;
    }
    (void)index;
}

void Processor::end_draw() {
    in_draw_ = false;
    using namespace nv4097;
    draw_.index_offset = s_.reg(SetIndexArrayAddress);
    draw_.index_location = s_.reg(SetIndexArrayDma) & 0xF;
    draw_.index_16bit = ((s_.reg(SetIndexArrayDma) >> 4) & 0xF) == 1;
    g_count.draws.fetch_add(1, std::memory_order_relaxed);
    g_count.vertices.fetch_add(draw_.kind == DrawCommand::Kind::Inline ? draw_.inline_data.size() : draw_.vertex_count(),
                               std::memory_order_relaxed);
    if (g_backend) g_backend->draw(m_, s_, draw_);
}

// SET_VERTEX_DATA{1,2,3,4}F_M and 4UB_M: the constant value of attribute i.
bool Processor::vertex_data(uint32_t reg, uint32_t arg) {
    using namespace nv4097;
    auto set = [&](uint32_t attr, uint32_t comp, float v) {
        if (comp == 0) {  // a new value: the components it does not give keep 0, 0, 0, 1
            for (uint32_t c = 1; c < 4; ++c) s_.vertex_data[attr * 4 + c] = c == 3 ? 1.0f : 0.0f;
        }
        s_.vertex_data[attr * 4 + comp] = v;
    };
    float f;
    std::memcpy(&f, &arg, 4);
    if (reg >= SetVertexData4fM && reg < SetVertexData4fM + 0x100) {
        const uint32_t i = (reg - SetVertexData4fM) / 4;
        s_.vertex_data[i] = f;
        return true;
    }
    if (reg >= SetVertexData3fM && reg < SetVertexData3fM + 0xC0) {
        const uint32_t i = (reg - SetVertexData3fM) / 4;
        set(i / 3, i % 3, f);
        return true;
    }
    if (reg >= SetVertexData2fM && reg < SetVertexData2fM + 0x80) {
        const uint32_t i = (reg - SetVertexData2fM) / 4;
        set(i / 2, i % 2, f);
        return true;
    }
    if (reg >= SetVertexData1fM && reg < SetVertexData1fM + 0x40) {
        set((reg - SetVertexData1fM) / 4, 0, f);
        return true;
    }
    if (reg >= SetVertexData4ubM && reg < SetVertexData4ubM + 0x40) {
        const uint32_t a = (reg - SetVertexData4ubM) / 4;
        for (uint32_t c = 0; c < 4; ++c) s_.vertex_data[a * 4 + c] = float((arg >> (8 * c)) & 0xFF) / 255.0f;
        return true;
    }
    return false;
}

// NV0039: line_count lines of line_length bytes from (offset_in, pitch_in) to
// (offset_out, pitch_out).
void Processor::copy_buffer() {
    using namespace nv0039;
    const GuestAddr in = dma_address(s_.reg(SetContextDmaBufferIn), s_.reg(OffsetIn));
    const GuestAddr out = dma_address(s_.reg(SetContextDmaBufferOut), s_.reg(OffsetOut));
    const uint32_t len = s_.reg(LineLengthIn), lines = s_.reg(LineCount);
    const int32_t pitch_in = int32_t(s_.reg(PitchIn)), pitch_out = int32_t(s_.reg(PitchOut));
    if (!in || !out || !len || !lines) return;
    if (g_backend) g_backend->sync(m_);
    for (uint32_t l = 0; l < lines; ++l)
        std::memmove(m_.host(out + uint32_t(int32_t(l) * pitch_out)), m_.host(in + uint32_t(int32_t(l) * pitch_in)), len);
    if (g_backend) g_backend->invalidate(out, lines > 1 ? uint32_t((lines - 1) * uint32_t(pitch_out)) + len : len);
    g_count.copies.fetch_add(1, std::memory_order_relaxed);
}

uint32_t transfer_dest_bpp(uint32_t format) { return format == 4 ? 2 : 4; }  // R5G6B5, else A8R8G8B8/Y32

// NV308A COLOR[index]: pixels written by the CPU through the FIFO into the NV3062 surface,
// starting at POINT; 32-bit formats take one pixel per word, 16-bit ones two.
void Processor::image_from_cpu(uint32_t index, uint32_t arg) {
    const uint32_t bpp = transfer_dest_bpp(s_.reg(nv3062::SetColorFormat));
    const uint32_t point = s_.reg(nv308a::Point), pitch = s_.reg(nv3062::SetPitch) >> 16;
    const uint32_t x = (point & 0xFFFF) + index * (4 / bpp), y = point >> 16;
    const GuestAddr dst = dma_address(s_.reg(nv3062::SetContextDmaImageDestin), s_.reg(nv3062::SetOffsetDestin));
    if (!dst) return;
    const GuestAddr a = dst + y * pitch + x * bpp;
    if (bpp == 4) {
        m_.write<uint32_t>(a, arg);
    } else {
        m_.write<uint16_t>(a, uint16_t(arg));
        m_.write<uint16_t>(a + 2, uint16_t(arg >> 16));
    }
    if (g_backend) g_backend->invalidate(a, 4);
}

// NV3089: scaled copy from the source image into the NV3062 surface, nearest sampling,
// same pixel size on both sides.
void Processor::scaled_blit() {
    using namespace nv3089;
    g_count.blits.fetch_add(1, std::memory_order_relaxed);
    const uint32_t in_fmt = s_.reg(ImageInFormat);
    const uint32_t src_pitch = in_fmt & 0xFFFF;
    const uint32_t in_w = s_.reg(ImageInSize) & 0xFFFF, in_h = s_.reg(ImageInSize) >> 16;
    const uint32_t out_x = s_.reg(ImageOutPoint) & 0xFFFF, out_y = s_.reg(ImageOutPoint) >> 16;
    const uint32_t out_w = s_.reg(ImageOutSize) & 0xFFFF, out_h = s_.reg(ImageOutSize) >> 16;
    const uint32_t src_fmt = s_.reg(SetColorFormat);
    const uint32_t src_bpp = (src_fmt == 1 || src_fmt == 2 || src_fmt == 7) ? 2 : (src_fmt == 8 ? 1 : 4);
    const uint32_t dst_bpp = transfer_dest_bpp(s_.reg(nv3062::SetColorFormat));
    const uint32_t dst_pitch = s_.reg(nv3062::SetPitch) >> 16;
    const GuestAddr src = dma_address(s_.reg(SetContextDmaImage), s_.reg(ImageInOffset));
    const GuestAddr dst = dma_address(s_.reg(nv3062::SetContextDmaImageDestin), s_.reg(nv3062::SetOffsetDestin));
    const uint32_t dsdx = s_.reg(DsDx), dtdy = s_.reg(DtDy);  // 12.20 fixed point
    if (!src || !dst || !out_w || !out_h || src_bpp != dst_bpp) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            SLY4_WARN("RSX: cópia escalada não suportada (formato %u -> %u, %ux%u)", src_fmt,
                      s_.reg(nv3062::SetColorFormat), out_w, out_h);
        return;
    }
    if (g_backend) g_backend->sync(m_);
    for (uint32_t y = 0; y < out_h; ++y) {
        const uint32_t sy = std::min(in_h ? in_h - 1 : 0, uint32_t((uint64_t(y) * dtdy) >> 20));
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t sx = std::min(in_w ? in_w - 1 : 0, uint32_t((uint64_t(x) * dsdx) >> 20));
            std::memcpy(m_.host(dst + (out_y + y) * dst_pitch + (out_x + x) * dst_bpp),
                        m_.host(src + sy * src_pitch + sx * src_bpp), src_bpp);
        }
    }
    if (g_backend) g_backend->invalidate(dst + out_y * dst_pitch, out_h * dst_pitch);
}

// Runs the FIFO from get to put, as long as the game keeps moving put.
void Processor::run() {
    while (!kernel().exited()) {
        uint32_t get = m_.read<uint32_t>(g_control + 4);
        const uint32_t put = m_.read<uint32_t>(g_control);
        if (get == put) {
            host_sleep_us(200);
            continue;
        }
        uint32_t ea;
        if (!io_to_ea(get, ea)) {
            SLY4_ERROR("RSX: FIFO em offset 0x%08X sem mapeamento de IO; parando", get);
            return;
        }
        const uint32_t cmd = m_.read<uint32_t>(ea);
        g_count.commands.fetch_add(1, std::memory_order_relaxed);
        uint32_t next = get + 4;
        if ((cmd & kOldJumpMask) == kOldJump) {
            next = cmd & 0x1FFFFFFC;
        } else if ((cmd & 3) == kNewJump) {
            next = cmd & ~3u;
        } else if ((cmd & 3) == kCall) {
            call_return_ = get + 4;
            in_call_ = true;
            next = cmd & ~3u;
        } else if (cmd == kReturn) {
            if (in_call_) next = call_return_;
            in_call_ = false;
        } else if (cmd != 0) {
            const uint32_t count = (cmd >> 18) & 0x7FF, reg = cmd & 0xFFFC;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t arg_ea;
                if (!io_to_ea(get + 4 + 4 * i, arg_ea)) break;
                method((cmd & kNonIncrement) ? reg : reg + 4 * i, m_.read<uint32_t>(arg_ea), i);
            }
            next = get + 4 + 4 * count;
        }
        m_.write<uint32_t>(g_control + 4, next);
    }
}

}  // namespace

// ---------------------------------------------------------------- IO map
std::mutex& io_lock() { return g_io.lock; }

bool io_to_ea(uint32_t offset, uint32_t& ea) {
    const uint32_t page = g_io.io_to_ea[(offset >> 20) & (kPages - 1)];
    if (page == kUnmapped) return false;
    ea = (page << 20) | (offset & 0xFFFFF);
    return true;
}

bool ea_to_offset(GuestAddr ea, uint32_t& offset) {
    if (ea >= kLocalBase && ea - kLocalBase < kLocalSize) {
        offset = ea - kLocalBase;
        return true;
    }
    const uint32_t io = g_io.ea_to_io[ea >> 20];
    if (io == kUnmapped) return false;
    offset = (io << 20) | (ea & 0xFFFFF);
    return true;
}

void map_io(uint32_t ea_page, uint32_t io_page, uint32_t pages) {
    for (uint32_t i = 0; i < pages; ++i) {
        g_io.io_to_ea[io_page + i] = uint16_t(ea_page + i);
        g_io.ea_to_io[ea_page + i] = uint16_t(io_page + i);
    }
    g_io.pages[ea_page] = uint16_t(pages);
}

uint32_t unmap_io(uint32_t ea_page) {
    const uint32_t pages = g_io.pages[ea_page & (kPages - 1)];
    for (uint32_t i = 0; i < pages; ++i) {
        const uint32_t io = g_io.ea_to_io[ea_page + i];
        if (io != kUnmapped) g_io.io_to_ea[io] = kUnmapped;
        g_io.ea_to_io[ea_page + i] = kUnmapped;
    }
    g_io.pages[ea_page & (kPages - 1)] = 0;
    return pages;
}

bool io_range_free(uint32_t io_page, uint32_t pages) {
    if (io_page + pages > kPages) return false;
    for (uint32_t i = 0; i < pages; ++i)
        if (g_io.io_to_ea[io_page + i] != kUnmapped) return false;
    return true;
}

GuestAddr location_address(uint32_t location, uint32_t offset) {
    if (location == kLocationLocal) return kLocalBase + offset;
    uint32_t ea = 0;
    return io_to_ea(offset, ea) ? ea : 0;
}

// ---------------------------------------------------------------- display, thread, tools
void set_display_buffer(uint32_t id, const DisplayBuffer& b) {
    std::lock_guard<std::mutex> lk(g_display_lock);
    if (id < 8) g_display[id] = b;
}
DisplayBuffer display_buffer(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_display_lock);
    return id < 8 ? g_display[id] : DisplayBuffer{};
}

void set_backend(Backend* b) { g_backend = b; }

void start(GuestMemory& mem, GuestAddr control, GuestAddr labels) {
    g_mem = &mem;
    g_control = control;
    g_labels = labels;
    std::thread([&mem] {
        Processor p(mem);
        p.run();
    }).detach();
}

GuestAddr control_address() { return g_control; }
GuestAddr labels_address() { return g_labels; }

void set_capture(uint32_t frame, const std::string& path) {
    g_capture_frame = frame;
    g_capture_path = path;
}

void summary() {
    if (!g_control) return;
    const uint64_t flips = g_count.flips.load();
    SLY4_INFO("RSX: %" PRIu64 " comandos, %" PRIu64 " flips, %" PRIu64 " draws (%" PRIu64 " vértices), %" PRIu64
              " clears, %" PRIu64 " cópias, %" PRIu64 " cópias escaladas",
              g_count.commands.load(), flips, g_count.draws.load(), g_count.vertices.load(), g_count.clears.load(),
              g_count.copies.load(), g_count.blits.load());
}

}  // namespace sly4::rsx
