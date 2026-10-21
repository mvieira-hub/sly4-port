// Control-flow analysis of the PPU executable: which words are code, where functions
// start and end, their basic blocks, calls, tail calls and switch jump tables.
//
// Function entries come from the .opd descriptors (every address-taken function on
// PPC64), the ELF entry and every call target reached from them. Each function is
// explored by following its branches (recursive descent), so data in .text (float
// constants, jump tables, padding) is never treated as code.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "loader/elf_loader.h"
#include "memory/guest_memory.h"

namespace sly4::recomp {

// How a basic block ends.
enum class Term : uint8_t {
    FallThrough,     // next instruction starts another block of the same function
    Branch,          // b / bc always, intra-function
    CondBranch,      // bc: target + fall-through
    Return,          // blr
    CondReturn,      // conditional blr: returns or falls through
    TailCall,        // b to another function's entry
    IndirectTail,    // bctr that is not a recognised jump table
    JumpTable,       // bctr through a switch table: successors = cases
    FallsIntoNext,   // runs into the entry of another function without branching
    NoReturnCall,    // bl followed by data or by another function: the callee does not return
    Trap,            // unconditional tw/td/twi/tdi, attn
    Invalid,         // reached a word that does not decode (analysis error)
};

const char* term_name(Term t);

struct Block {
    uint32_t start = 0, end = 0;  // [start, end)
    Term term = Term::FallThrough;
    std::vector<uint32_t> succ;   // intra-function successors
};

struct JumpTable {
    uint32_t bcctr = 0;    // address of the bctr
    uint32_t base = 0;     // table address; entries are offsets relative to it
    uint32_t count = 0;
    bool bounded = false;  // count read from the cmpli guard (true) or estimated
    bool toc_base = false; // base loaded from the TOC (and checked)
    std::vector<uint32_t> targets;
};

enum class FuncKind : uint8_t { Code, ImportStub };

struct Function {
    uint32_t entry = 0;
    FuncKind kind = FuncKind::Code;
    bool from_opd = false;
    uint32_t lo = 0, hi = 0;  // lowest instruction address, highest + 4
    uint32_t instructions = 0;
    std::vector<Block> blocks;
    std::vector<uint32_t> calls;           // direct call targets (bl / bcl)
    std::vector<uint32_t> tail_calls;      // b / falls-into targets that are other functions
    uint32_t indirect_calls = 0;           // bctrl / bclrl sites
    std::vector<JumpTable> tables;
    // Import stubs only: which import this stub forwards to.
    std::string import_module;
    uint32_t import_nid = 0;
    uint32_t import_slot = 0;
};

struct AnalysisStats {
    uint32_t passes = 0;
    uint32_t code_words = 0, table_words = 0, nonzero_words = 0, exec_words = 0;
    uint32_t overlapping_instructions = 0;  // instructions claimed by more than one function
    uint32_t invalid_reached = 0;
    uint32_t noreturn_calls = 0;
    uint32_t jump_tables = 0, bounded_tables = 0, toc_base_mismatch = 0;
};

struct Program {
    std::map<uint32_t, Function> functions;
    std::vector<std::pair<uint32_t, uint32_t>> exec_ranges;  // executable sections [lo, hi)
    // Per word of [word_base, word_base + 4 * size): 0 = not reached, 1 = code, 2 = jump table.
    uint32_t word_base = 0;
    std::vector<uint8_t> word_class;
    std::vector<std::pair<uint32_t, uint32_t>> invalid_sites;   // (address, function)
    std::vector<std::pair<uint32_t, uint32_t>> noreturn_sites;  // (call address, callee)
    AnalysisStats stats;

    uint8_t class_of(uint32_t addr) const {
        uint32_t i = (addr - word_base) / 4;
        return addr >= word_base && i < word_class.size() ? word_class[i] : 0;
    }
};

Program analyze(const GuestMemory& mem, const LoadedImage& img);

}  // namespace sly4::recomp
