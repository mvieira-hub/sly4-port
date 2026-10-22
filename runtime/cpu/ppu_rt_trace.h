// Tracing of calls into firmware modules that run as they are (LLE): their exports are
// entered through rt::call_indirect, which reports targets in [base, base + size) here.
// Kept out of ppu_rt.h, which every generated file includes.
#pragma once

#include <cstdint>

#include "cpu/ppu_context.h"

namespace sly4::rt {

using LleTraceFn = void (*)(PPUContext& c, uint32_t target);
void set_lle_trace(LleTraceFn fn, uint32_t base, uint32_t size);

}  // namespace sly4::rt
