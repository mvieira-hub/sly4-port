#include "emit/ppu_emitter.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

#include "ppu/ppu_decoder.h"

namespace sly4::recomp {

using ppu::Insn;
using ppu::Op;

namespace {

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08Xu", v);
    return b;
}
std::string fname(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof(b), "f_%08X", a);
    return b;
}
std::string label(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof(b), "L_%08X", a);
    return b;
}

bool can_fail(Op op) { return op == Op::mfspr || op == Op::mtspr || op == Op::mftb || op == Op::mfocrf; }
bool always(uint32_t bo) { return (bo & 0x14) == 0x14; }

class FunctionWriter {
public:
    FunctionWriter(std::ostream& o, const GuestMemory& mem, const std::unordered_set<uint32_t>& emitted)
        : o_(o), mem_(mem), emitted_(emitted) {}

    void write(const Function& f, EmitStats& st) {
        std::set<uint32_t> starts;
        for (const Block& b : f.blocks) starts.insert(b.start);
        o_ << "void " << fname(f.entry) << "(PPUContext& c, GuestMemory& m) {\n"
           << "    [[maybe_unused]] const uint32_t ret = uint32_t(c.lr) & ~3u;\n";
        // Blocks are emitted in address order; the entry may not be the lowest block.
        std::vector<const Block*> order;
        for (const Block& b : f.blocks) order.push_back(&b);
        std::sort(order.begin(), order.end(), [](const Block* x, const Block* y) { return x->start < y->start; });
        if (!order.empty() && order.front()->start != f.entry) o_ << "    goto " << label(f.entry) << ";\n";
        for (size_t i = 0; i < order.size(); ++i) {
            const Block& b = *order[i];
            const uint32_t next_emitted = i + 1 < order.size() ? order[i + 1]->start : 0;
            write_block(b, next_emitted, st);
            ++st.blocks;
        }
        o_ << "}\n\n";
    }

private:
    std::ostream& o_;
    const GuestMemory& mem_;
    const std::unordered_set<uint32_t>& emitted_;

    void line(const std::string& s) { o_ << "    " << s << "\n"; }

    std::string call(uint32_t target) const {
        if (emitted_.count(target)) return fname(target) + "(c, m);";
        return "rt::call_indirect(c, m, " + hex32(target) + ");";
    }
    std::string sem_call(const Insn& in) const {
        const char* e = ppu::enum_name(in.op);
        return std::string("sem::op<Op::") + e + ">(c, m, Insn{Op::" + e + ", " + hex32(in.w) + "})";
    }
    void goto_if_needed(uint32_t target, uint32_t next_emitted) {
        if (target != next_emitted) line("goto " + label(target) + ";");
    }

    // Instructions that are not block terminators (including calls and sc).
    void write_insn(uint32_t a, const Insn& in) {
        const std::string next = hex32(a + 4);
        switch (in.op) {
            case Op::b:  // only bl reaches here
                line("c.lr = " + next + "; " + call(in.branch_target(a)));
                return;
            case Op::bc: {  // bcl
                const uint32_t t = in.branch_target(a);
                if (t == a + 4) {  // bcl 20,31,$+4: reads the PC
                    line("c.lr = " + next + ";");
                } else {
                    line("{ const bool k = sem::branch_taken(c, " + std::to_string(in.rd()) + ", " +
                         std::to_string(in.ra()) + ", true); c.lr = " + next + "; if (k) " + call(t) + " }");
                }
                return;
            }
            case Op::bclr:  // bclrl
                line("{ const uint32_t t = uint32_t(c.lr) & ~3u; const bool k = sem::branch_taken(c, " +
                     std::to_string(in.rd()) + ", " + std::to_string(in.ra()) + ", true); c.lr = " + next +
                     "; if (k) rt::call_indirect(c, m, t); }");
                return;
            case Op::bcctr:  // bcctrl
                line("{ const uint32_t t = uint32_t(c.ctr) & ~3u; const bool k = sem::branch_taken(c, " +
                     std::to_string(in.rd()) + ", " + std::to_string(in.ra()) + ", false); c.lr = " + next +
                     "; if (k) rt::call_indirect(c, m, t); }");
                return;
            case Op::sc:
                line("c.cia = " + next + "; rt::syscall(c, m);");
                return;
            case Op::tw: case Op::td: case Op::twi: case Op::tdi: {
                const bool word = in.op == Op::tw || in.op == Op::twi;
                const std::string ty = word ? "int32_t" : "int64_t";
                const std::string lhs = ty + "(c.gpr[" + std::to_string(in.ra()) + "])";
                const std::string rhs = (in.op == Op::tw || in.op == Op::td)
                                            ? ty + "(c.gpr[" + std::to_string(in.rb()) + "])"
                                            : std::to_string(in.simm());
                line("if (sem::trap_taken(" + std::to_string(in.rd()) + ", " + lhs + ", " + rhs + ")) { c.cia = " +
                     hex32(a) + "; rt::trap(c, m); return; }");
                return;
            }
            default:
                if (can_fail(in.op))
                    line("if (" + sem_call(in) + " != PpuStep::Ok) { c.cia = " + hex32(a) +
                         "; rt::invalid(c, m); return; }");
                else
                    line(sem_call(in) + ";");
                return;
        }
    }

    void write_block(const Block& b, uint32_t next_emitted, EmitStats& st) {
        o_ << label(b.start) << ":\n";
        line("SLY4_BLOCK();");
        const bool consumes_last = b.term == Term::Branch || b.term == Term::CondBranch || b.term == Term::Return ||
                                   b.term == Term::CondReturn || b.term == Term::TailCall ||
                                   b.term == Term::IndirectTail || b.term == Term::JumpTable || b.term == Term::Trap;
        for (uint32_t a = b.start; a < b.end; a += 4) {
            const Insn in = ppu::decode(mem_.read<uint32_t>(a));
            ++st.instructions;
            if (a + 4 == b.end && consumes_last) {
                write_terminator(a, in, b, next_emitted);
                return;
            }
            write_insn(a, in);
        }
        switch (b.term) {
            case Term::FallThrough:
                if (!b.succ.empty()) goto_if_needed(b.succ[0], next_emitted);
                break;
            case Term::FallsIntoNext:
                line(call(b.end) + " return;");
                break;
            case Term::NoReturnCall:
                line("c.cia = " + hex32(b.end) + "; rt::invalid(c, m); return;  // callee should not return");
                break;
            case Term::Invalid:
                line("c.cia = " + hex32(b.end) + "; rt::invalid(c, m); return;");
                break;
            default:
                break;
        }
    }

    void write_terminator(uint32_t a, const Insn& in, const Block& b, uint32_t next_emitted) {
        const std::string bo = std::to_string(in.rd()), bi = std::to_string(in.ra());
        const uint32_t next = a + 4;
        auto has_succ = [&](uint32_t t) { return std::find(b.succ.begin(), b.succ.end(), t) != b.succ.end(); };
        switch (b.term) {
            case Term::Branch:
                goto_if_needed(in.branch_target(a), next_emitted);
                break;
            case Term::TailCall:
                line(call(in.branch_target(a)) + " return;");
                break;
            case Term::CondBranch: {
                const uint32_t t = in.branch_target(a);
                if (has_succ(t))
                    line("if (sem::branch_taken(c, " + bo + ", " + bi + ", true)) goto " + label(t) + ";");
                else  // conditional tail call
                    line("if (sem::branch_taken(c, " + bo + ", " + bi + ", true)) { " + call(t) + " return; }");
                goto_if_needed(next, next_emitted);
                break;
            }
            case Term::Return:
                line("if ((uint32_t(c.lr) & ~3u) != ret) rt::lr_mismatch(c, m, " + hex32(a) + ", ret);");
                line("return;");
                break;
            case Term::CondReturn:
                line("if (sem::branch_taken(c, " + bo + ", " + bi +
                     ", true)) { if ((uint32_t(c.lr) & ~3u) != ret) rt::lr_mismatch(c, m, " + hex32(a) +
                     ", ret); return; }");
                goto_if_needed(next, next_emitted);
                break;
            case Term::IndirectTail:
                line("{ const uint32_t t = uint32_t(c.ctr) & ~3u; if (sem::branch_taken(c, " + bo + ", " + bi +
                     ", false)) { rt::call_indirect(c, m, t); return; } }");
                if (!always(in.rd())) goto_if_needed(next, next_emitted);
                break;
            case Term::JumpTable: {
                line("switch (uint32_t(c.ctr) & ~3u) {");
                for (uint32_t t : b.succ) line("    case " + hex32(t) + ": goto " + label(t) + ";");
                line("    default: rt::bad_jump(c, m, " + hex32(a) + ", uint32_t(c.ctr) & ~3u); return;");
                line("}");
                break;
            }
            case Term::Trap:
                line("c.cia = " + hex32(a) + "; rt::trap(c, m); return;");
                break;
            default:
                break;
        }
    }
};

}  // namespace

bool emit_program(const Program& prog, const GuestMemory& mem, const EmitOptions& opt, EmitStats& stats,
                  std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(opt.out_dir, ec);
    if (ec) {
        error = "não foi possível criar " + opt.out_dir.string();
        return false;
    }
    std::vector<const Function*> funcs;
    if (opt.only.empty()) {
        for (const auto& [e, f] : prog.functions) funcs.push_back(&f);
    } else {
        for (uint32_t e : opt.only)
            if (auto it = prog.functions.find(e); it != prog.functions.end()) funcs.push_back(&it->second);
    }
    std::unordered_set<uint32_t> emitted;
    for (const Function* f : funcs) emitted.insert(f->entry);

    {
        std::ofstream h(opt.out_dir / "functions.h");
        h << "// Generated by sly4_recomp emit from the user's EBOOT.elf. Do not edit; not for distribution.\n"
             "#pragma once\n\n#include \"cpu/ppu_rt.h\"\n#include \"cpu/ppu_semantics.h\"\n\n"
             "#ifdef SLY4_GEN_BUDGET\n"
             "#define SLY4_BLOCK() if (--c.budget < 0) { rt::budget_exceeded(c, m); return; }\n"
             "#else\n#define SLY4_BLOCK()\n#endif\n\n"
             "namespace sly4::gen {\n\nusing namespace sly4;\nusing ppu::Insn;\nusing ppu::Op;\n\n";
        for (const Function* f : funcs) h << "void " << fname(f->entry) << "(PPUContext& c, GuestMemory& m);\n";
        h << "\nvoid register_functions();\n\n}  // namespace sly4::gen\n";
    }

    std::vector<std::string> sources;
    for (size_t first = 0; first < funcs.size(); first += opt.functions_per_file) {
        char name[32];
        std::snprintf(name, sizeof(name), "ppu_%04zu.cpp", sources.size());
        std::ofstream o(opt.out_dir / name);
        o << "// Generated by sly4_recomp emit. Do not edit.\n#include \"functions.h\"\n\nnamespace sly4::gen {\n\n";
        FunctionWriter w(o, mem, emitted);
        for (size_t i = first; i < funcs.size() && i < first + opt.functions_per_file; ++i) w.write(*funcs[i], stats);
        o << "}  // namespace sly4::gen\n";
        sources.push_back(name);
    }

    {
        std::ofstream t(opt.out_dir / "table.cpp");
        t << "// Generated by sly4_recomp emit. Do not edit.\n#include \"functions.h\"\n\nnamespace sly4::gen {\n\n"
             "namespace {\nconst rt::FnEntry kTable[] = {\n";
        std::vector<uint32_t> sorted(emitted.begin(), emitted.end());
        std::sort(sorted.begin(), sorted.end());
        for (uint32_t e : sorted) t << "    {" << hex32(e) << ", &" << fname(e) << "},\n";
        t << "};\n}  // namespace\n\nvoid register_functions() { rt::set_function_table(kTable, "
          << sorted.size() << "); }\n\n}  // namespace sly4::gen\n";
        sources.push_back("table.cpp");
    }
    {
        std::ofstream s(opt.out_dir / "sources.cmake");
        s << "# Generated by sly4_recomp emit.\nset(SLY4_GEN_SOURCES\n";
        for (const auto& n : sources) s << "    ${CMAKE_CURRENT_LIST_DIR}/" << n << "\n";
        s << ")\n";
    }
    stats.functions = funcs.size();
    stats.files = sources.size();
    return true;
}

}  // namespace sly4::recomp
