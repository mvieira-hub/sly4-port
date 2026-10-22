// PPU -> C++ emitter: turns the analysed program (recomp/analysis) into C++ sources.
//
// Every guest function becomes void f_XXXXXXXX(PPUContext&, GuestMemory&); every basic
// block a label. Ordinary instructions call the shared semantics with a constant word
// (sem::op<Op::X>(c, m, Insn{Op::X, 0x...})); control flow is mapped onto C++:
// intra-function branches -> goto, bl -> direct call, blr -> return, tail calls ->
// call + return, jump tables -> switch on CTR, indirect calls -> rt::call_indirect.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "analysis/program.h"
#include "memory/guest_memory.h"

namespace sly4::recomp {

struct EmitOptions {
    std::filesystem::path out_dir;
    size_t functions_per_file = 300;
    std::vector<uint32_t> only;  // emit just these entries (others are reached through rt::call_indirect)
    // Firmware modules get register_functions_<name>(); the game keeps register_functions().
    std::string name;
};

struct EmitStats {
    size_t functions = 0, files = 0, instructions = 0, blocks = 0;
};

bool emit_program(const Program& prog, const GuestMemory& mem, const EmitOptions& opt, EmitStats& stats,
                  std::string& error);

}  // namespace sly4::recomp
