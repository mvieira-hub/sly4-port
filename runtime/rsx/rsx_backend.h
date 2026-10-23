// What the RSX thread hands to a renderer: clears, draws and flips, with the register
// state at that point. Called on the RSX thread.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "memory/guest_memory.h"
#include "rsx/rsx_state.h"

namespace sly4::rsx {

struct DrawCommand {
    enum class Kind { Arrays, Indexed, Inline };
    Primitive primitive = Primitive::None;
    Kind kind = Kind::Arrays;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // first, count (vertices or indices)
    std::vector<uint32_t> inline_data;                  // INLINE_ARRAY words / ARRAY_ELEMENT indices
    uint32_t index_offset = 0, index_location = 0;
    bool index_16bit = false;
    uint32_t vertex_count() const {
        uint32_t n = 0;
        for (const auto& r : ranges) n += r.second;
        return n;
    }
};

// NV3089 scaled copy: the source rectangle [0, src_w) x [0, src_h) of the image at src
// (pitch src_pitch) into [dst_x, dst_x + dst_w) x [dst_y, dst_y + dst_h) of the image at
// dst (pitch dst_pitch); both 32-bit pixels.
struct ScaledCopy {
    GuestAddr src = 0, dst = 0;
    uint32_t src_pitch = 0, dst_pitch = 0;
    uint32_t src_w = 0, src_h = 0;
    uint32_t dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    bool linear = false;  // bilinear filter, else nearest
};

class Backend {
public:
    // Does the copy on the GPU when both images are surfaces it holds; false: the caller
    // copies guest memory on the CPU.
    virtual bool scaled_copy(GuestMemory& mem, const ScaledCopy& c) { return false; }
    virtual ~Backend() = default;
    // CLEAR_SURFACE: mask bit 0 depth, 1 stencil, 4-7 R, G, B, A.
    virtual void clear(GuestMemory& mem, const State& s, uint32_t mask) = 0;
    virtual void draw(GuestMemory& mem, const State& s, const DrawCommand& d) = 0;
    // GCM flip to display buffer `buffer`.
    virtual void flip(GuestMemory& mem, const State& s, uint32_t buffer) = 0;
    // Before the RSX thread touches memory itself (copies, reports, semaphores read by the
    // CPU): results the GPU holds must be in guest memory.
    virtual void sync(GuestMemory& mem) = 0;
    // The RSX thread is about to read or partly overwrite [ea, ea + size) with the CPU
    // (2D copies): what the GPU drew there must be in guest memory first.
    virtual void flush(GuestMemory& mem, GuestAddr ea, uint32_t size) { sync(mem); }
    // Guest memory [ea, ea + size) was written by the RSX thread (copies): cached copies
    // of it (textures, surfaces) are stale.
    virtual void invalidate(GuestAddr ea, uint32_t size) = 0;
};

// The renderer in use (nullptr = none: nothing is drawn). Set before the RSX starts.
void set_backend(Backend* b);

}  // namespace sly4::rsx
