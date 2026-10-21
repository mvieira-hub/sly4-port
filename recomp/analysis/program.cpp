#include "analysis/program.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ppu/ppu_decoder.h"

namespace sly4::recomp {

using ppu::Insn;
using ppu::Op;

const char* term_name(Term t) {
    switch (t) {
        case Term::FallThrough: return "fallthrough";
        case Term::Branch: return "branch";
        case Term::CondBranch: return "condbranch";
        case Term::Return: return "return";
        case Term::CondReturn: return "condreturn";
        case Term::TailCall: return "tailcall";
        case Term::IndirectTail: return "indirect";
        case Term::JumpTable: return "jumptable";
        case Term::FallsIntoNext: return "fallsinto";
        case Term::NoReturnCall: return "noreturn";
        case Term::Trap: return "trap";
        case Term::Invalid: return "invalid";
    }
    return "?";
}

namespace {

constexpr uint32_t kSprCtr = 9;
constexpr uint32_t kStubMaxInsns = 12;

struct Ctx {
    const GuestMemory& mem;
    const LoadedImage& img;
    std::vector<std::pair<uint32_t, uint32_t>> exec;

    bool in_exec(uint32_t a) const {
        for (auto [lo, hi] : exec)
            if (a >= lo && a + 4 <= hi) return true;
        return false;
    }
    uint32_t word(uint32_t a) const { return mem.read<uint32_t>(a); }
    Insn at(uint32_t a) const { return ppu::decode(word(a)); }
};

bool branch_always(uint32_t bo) { return (bo & 0x14) == 0x14; }

// Import stubs: li r12,0 / oris r12,r12,slot@h / lwz r12,slot@l(r12) / ... / bctr,
// where slot is one of the .lib.stub function slots.
std::vector<Function> find_import_stubs(const Ctx& c) {
    std::unordered_map<uint32_t, std::pair<const ImportedModule*, uint32_t>> slots;
    for (const auto& m : c.img.imports)
        for (const auto& fn : m.functions) slots[fn.slot] = {&m, fn.nid};
    std::vector<Function> stubs;
    for (auto [lo, hi] : c.exec) {
        for (uint32_t a = lo + 4; a + 4 <= hi; a += 4) {
            const Insn x = c.at(a);
            if (x.op != Op::lwz || x.rd() != 12 || x.ra() != 12) continue;
            const Insn p = c.at(a - 4);
            uint32_t high, start;
            if (p.op == Op::oris && p.rd() == 12 && p.ra() == 12) {
                high = p.uimm() << 16;
                const Insn li = a >= lo + 8 ? c.at(a - 8) : Insn{};
                start = (li.op == Op::addi && li.rd() == 12 && li.ra() == 0) ? a - 8 : a - 4;
            } else if (p.op == Op::addis && p.rd() == 12 && p.ra() == 0) {
                high = static_cast<uint32_t>(p.simm()) << 16;
                start = a - 4;
            } else {
                continue;
            }
            auto it = slots.find(high + static_cast<uint32_t>(x.simm()));
            if (it == slots.end()) continue;
            Function f;
            f.entry = f.lo = start;
            f.kind = FuncKind::ImportStub;
            f.import_module = it->second.first->name;
            f.import_nid = it->second.second;
            f.import_slot = it->first;
            uint32_t end = start;
            for (uint32_t i = 0; i < kStubMaxInsns && end + 4 <= hi; ++i) {
                const Insn s = c.at(end);
                end += 4;
                if (s.op == Op::bcctr && !s.lk()) break;
            }
            f.hi = end;
            f.instructions = (end - start) / 4;
            Block b{start, end, Term::IndirectTail, {}};
            f.blocks.push_back(b);
            stubs.push_back(std::move(f));
        }
    }
    return stubs;
}

// Switch tables (see docs/RECOMP.md):
//   cmpli crN,0,rI,K ; bc 12,crN+gt,default ; [lwz rB,D(r2)] ; rldic/rlwinm rX,rI,2,..
//   lwzx rY,rX,rB ; extsw rY,rY ; add rZ,rY,rB ; mtctr rZ ; bctr ; .long offsets from rB
bool find_jump_table(const Ctx& c, uint32_t bcctr, JumpTable& jt) {
    constexpr uint32_t kLookBack = 32;
    uint32_t a = bcctr;
    // mtctr rZ just before the bctr, then add rZ = rY + rB, then lwzx rY = [rX + rB].
    int ctr_reg = -1, sum_src1 = -1, sum_src2 = -1, base_reg = -1;
    uint32_t lwzx_at = 0;
    for (uint32_t i = 0; i < 8 && a > 4; ++i) {
        a -= 4;
        if (!c.in_exec(a)) return false;
        const Insn in = c.at(a);
        if (ctr_reg < 0) {
            if (in.op == Op::mtspr && in.spr() == kSprCtr) ctr_reg = static_cast<int>(in.rd());
            else return false;
        } else if (sum_src1 < 0) {
            if (in.op == Op::add && static_cast<int>(in.rd()) == ctr_reg) {
                sum_src1 = static_cast<int>(in.ra());
                sum_src2 = static_cast<int>(in.rb());
            }
        } else if (in.op == Op::lwzx) {
            const int b = static_cast<int>(in.rb());
            if (b == sum_src1 || b == sum_src2) {
                base_reg = b;
                lwzx_at = a;
                break;
            }
        }
    }
    if (base_reg < 0) return false;
    const uint32_t index_reg = c.at(lwzx_at).ra();

    jt = JumpTable{};
    jt.bcctr = bcctr;
    jt.base = bcctr + 4;
    // Walk back through the straight-line code that leads to the lwzx (stop at any
    // unconditional transfer: what lies before it does not flow here) looking for the
    // TOC load of the table base, the scaling of the index and the cmpli guard on it.
    int index_src = -1;
    for (uint32_t i = 0, p = lwzx_at; i < kLookBack && p > 4; ++i) {
        p -= 4;
        if (!c.in_exec(p)) break;
        const Insn in = c.at(p);
        const bool unconditional = (in.op == Op::b && !in.lk()) ||
                                   ((in.op == Op::bc || in.op == Op::bclr || in.op == Op::bcctr) && !in.lk() &&
                                    branch_always(in.rd()));
        if (unconditional) break;
        if (!jt.toc_base && in.op == Op::lwz && static_cast<int>(in.rd()) == base_reg && in.ra() == 2) {
            jt.base = c.mem.read<uint32_t>(c.img.toc + static_cast<uint32_t>(in.simm()));
            jt.toc_base = true;
        } else if (index_src < 0 && in.ra() == index_reg &&
                   (in.op == Op::rlwinm || in.op == Op::rldic || in.op == Op::rldicr || in.op == Op::rldicl)) {
            index_src = static_cast<int>(in.rd());
        } else if (index_src >= 0 && in.op == Op::cmpli && in.l_cmp() == 0 &&
                   static_cast<int>(in.ra()) == index_src) {
            jt.count = in.uimm() + 1;
            jt.bounded = true;
            break;
        }
    }

    // The case code follows the table, so the table ends at the first target past it.
    uint32_t limit = 4096;
    auto entry_ok = [&](uint32_t i, uint32_t& target) {
        const uint32_t slot = jt.base + 4 * i;
        if (i >= limit || !c.mem.is_committed(slot, 4)) return false;
        target = jt.base + c.mem.read<uint32_t>(slot);
        if ((target & 3) != 0 || !c.in_exec(target) || c.at(target).op == Op::invalid) return false;
        if (target > jt.base) limit = std::min(limit, (target - jt.base) / 4);
        return i < limit;
    };
    uint32_t t;
    if (jt.bounded) {
        if (jt.count > 4096) return false;
        for (uint32_t i = 0; i < jt.count; ++i) {
            if (!entry_ok(i, t)) return false;
            jt.targets.push_back(t);
        }
    } else {
        for (uint32_t i = 0; entry_ok(i, t); ++i) jt.targets.push_back(t);
        jt.count = static_cast<uint32_t>(jt.targets.size());
        if (jt.count < 2) return false;
    }
    return true;
}

struct Explorer {
    const Ctx& c;
    const std::unordered_set<uint32_t>& entries;
    Program& prog;

    Function explore(uint32_t entry) {
        Function f;
        f.entry = entry;
        std::unordered_set<uint32_t> visited;
        std::set<uint32_t> leaders{entry};
        struct TermInfo {
            Term term;
            std::vector<uint32_t> succ;
        };
        std::unordered_map<uint32_t, TermInfo> terms;  // keyed by the block's last instruction
        std::vector<uint32_t> work{entry};

        auto add_succ = [&](uint32_t t) {
            leaders.insert(t);
            work.push_back(t);
        };
        auto is_other_entry = [&](uint32_t t) { return t != entry && entries.count(t); };
        auto invalid_at = [&](uint32_t at, uint32_t last) {
            prog.invalid_sites.emplace_back(at, entry);
            if (last) terms[last] = {Term::Invalid, {}};
        };

        while (!work.empty()) {
            uint32_t cur = work.back();
            work.pop_back();
            uint32_t last = 0;  // previous instruction of this run
            for (;;) {
                if (visited.count(cur)) break;
                if (last && is_other_entry(cur)) {
                    terms[last] = {Term::FallsIntoNext, {}};
                    f.tail_calls.push_back(cur);
                    break;
                }
                if (!c.in_exec(cur)) {
                    invalid_at(cur, last);
                    break;
                }
                const Insn in = c.at(cur);
                if (in.op == Op::invalid) {
                    invalid_at(cur, last);
                    break;
                }
                visited.insert(cur);
                last = cur;
                const uint32_t next = cur + 4;

                auto call = [&](uint32_t target) {
                    f.calls.push_back(target);
                    // A call followed by data or by another function never returns.
                    if (!c.in_exec(next) || is_other_entry(next) || c.at(next).op == Op::invalid) {
                        terms[cur] = {Term::NoReturnCall, {}};
                        prog.noreturn_sites.emplace_back(cur, target);
                        return false;
                    }
                    return true;
                };

                bool cont = true;
                switch (in.op) {
                    case Op::b: {
                        const uint32_t t = in.branch_target(cur);
                        if (in.lk()) {
                            cont = call(t);
                        } else if (is_other_entry(t)) {
                            terms[cur] = {Term::TailCall, {}};
                            f.tail_calls.push_back(t);
                            cont = false;
                        } else {
                            terms[cur] = {Term::Branch, {t}};
                            add_succ(t);
                            cont = false;
                        }
                        break;
                    }
                    case Op::bc: {
                        const uint32_t t = in.branch_target(cur);
                        if (in.lk()) {
                            if (t != next) cont = call(t);  // bcl 20,31,$+4 only reads the PC
                        } else if (branch_always(in.rd())) {
                            if (is_other_entry(t)) {
                                terms[cur] = {Term::TailCall, {}};
                                f.tail_calls.push_back(t);
                            } else {
                                terms[cur] = {Term::Branch, {t}};
                                add_succ(t);
                            }
                            cont = false;
                        } else {
                            TermInfo ti{Term::CondBranch, {next}};
                            if (is_other_entry(t)) f.tail_calls.push_back(t);
                            else ti.succ.push_back(t), add_succ(t);
                            add_succ(next);
                            terms[cur] = ti;
                            cont = false;
                        }
                        break;
                    }
                    case Op::bclr:
                        if (in.lk()) {
                            ++f.indirect_calls;
                        } else if (branch_always(in.rd())) {
                            terms[cur] = {Term::Return, {}};
                            cont = false;
                        } else {
                            terms[cur] = {Term::CondReturn, {next}};
                            add_succ(next);
                            cont = false;
                        }
                        break;
                    case Op::bcctr:
                        if (in.lk()) {
                            ++f.indirect_calls;
                        } else {
                            JumpTable jt;
                            if (branch_always(in.rd()) && find_jump_table(c, cur, jt)) {
                                TermInfo ti{Term::JumpTable, {}};
                                std::set<uint32_t> uniq(jt.targets.begin(), jt.targets.end());
                                for (uint32_t t : uniq) {
                                    ti.succ.push_back(t);
                                    add_succ(t);
                                }
                                terms[cur] = ti;
                                f.tables.push_back(std::move(jt));
                            } else {
                                TermInfo ti{Term::IndirectTail, {}};
                                if (!branch_always(in.rd())) ti.succ.push_back(next), add_succ(next);
                                terms[cur] = ti;
                            }
                            cont = false;
                        }
                        break;
                    case Op::tw:
                    case Op::td:
                    case Op::twi:
                    case Op::tdi:
                        if (in.rd() == 31) {
                            terms[cur] = {Term::Trap, {}};
                            cont = false;
                        }
                        break;
                    case Op::attn:
                        terms[cur] = {Term::Trap, {}};
                        cont = false;
                        break;
                    default:
                        break;
                }
                if (!cont) break;
                cur = next;
            }
        }

        // Blocks: sorted instructions split at leaders, terminators and gaps.
        std::vector<uint32_t> insns(visited.begin(), visited.end());
        std::sort(insns.begin(), insns.end());
        f.instructions = static_cast<uint32_t>(insns.size());
        if (!insns.empty()) {
            f.lo = insns.front();
            f.hi = insns.back() + 4;
        }
        Block* blk = nullptr;
        for (size_t i = 0; i < insns.size(); ++i) {
            const uint32_t a = insns[i];
            const bool contiguous = i > 0 && insns[i - 1] + 4 == a;
            const bool prev_terminated = i > 0 && terms.count(insns[i - 1]);
            if (!blk || !contiguous || prev_terminated || leaders.count(a)) {
                if (blk && contiguous && !prev_terminated) {
                    blk->term = Term::FallThrough;
                    blk->succ = {a};
                }
                f.blocks.push_back(Block{a, a + 4, Term::FallThrough, {}});
                blk = &f.blocks.back();
            }
            blk->end = a + 4;
            if (auto it = terms.find(a); it != terms.end()) {
                blk->term = it->second.term;
                blk->succ = it->second.succ;
            }
        }
        return f;
    }
};

}  // namespace

Program analyze(const GuestMemory& mem, const LoadedImage& img) {
    Program prog;
    Ctx c{mem, img, {}};
    for (const auto& s : img.sections)
        if (s.executable()) c.exec.emplace_back(s.addr, s.addr + s.size);
    std::sort(c.exec.begin(), c.exec.end());
    prog.exec_ranges = c.exec;
    if (c.exec.empty()) return prog;
    prog.word_base = c.exec.front().first;
    prog.word_class.assign((c.exec.back().second - prog.word_base) / 4, 0);

    const std::vector<Function> stubs = find_import_stubs(c);
    std::unordered_set<uint32_t> entries, opd_entries;
    for (const auto& s : stubs) entries.insert(s.entry);
    for (uint32_t a = img.opd_start; a + 8 <= img.opd_end; a += 8) {
        const uint32_t fn = mem.read<uint32_t>(a);
        if (c.in_exec(fn)) entries.insert(fn), opd_entries.insert(fn);
    }
    if (c.in_exec(img.entry_function)) entries.insert(img.entry_function);

    // Exploring a function can reveal new call targets, which in turn change how
    // branches into them are classified (tail call vs local), so iterate to a fixpoint.
    for (;;) {
        ++prog.stats.passes;
        prog.functions.clear();
        prog.invalid_sites.clear();
        prog.noreturn_sites.clear();
        for (const auto& s : stubs) prog.functions[s.entry] = s;
        Explorer ex{c, entries, prog};
        std::vector<uint32_t> found;
        std::vector<uint32_t> order(entries.begin(), entries.end());
        std::sort(order.begin(), order.end());
        for (uint32_t e : order) {
            if (prog.functions.count(e)) continue;  // import stub
            Function f = ex.explore(e);
            f.from_opd = opd_entries.count(e) != 0;
            for (uint32_t t : f.calls)
                if (c.in_exec(t) && !entries.count(t)) found.push_back(t);
            prog.functions[e] = std::move(f);
        }
        if (found.empty()) break;
        entries.insert(found.begin(), found.end());
    }

    // Word classes and statistics.
    std::vector<uint8_t> claims(prog.word_class.size(), 0);
    for (auto& [e, f] : prog.functions) {
        for (const Block& b : f.blocks)
            for (uint32_t a = b.start; a < b.end; a += 4) {
                const uint32_t i = (a - prog.word_base) / 4;
                prog.word_class[i] = 1;
                if (claims[i] < 255) ++claims[i];
            }
        for (const JumpTable& jt : f.tables) {
            ++prog.stats.jump_tables;
            prog.stats.bounded_tables += jt.bounded;
            prog.stats.toc_base_mismatch += jt.toc_base && jt.base != jt.bcctr + 4;
            for (uint32_t k = 0; k < jt.count; ++k) {
                const uint32_t a = jt.base + 4 * k;
                if (a >= prog.word_base && (a - prog.word_base) / 4 < prog.word_class.size() &&
                    prog.word_class[(a - prog.word_base) / 4] == 0)
                    prog.word_class[(a - prog.word_base) / 4] = 2;
            }
        }
    }
    for (size_t i = 0; i < prog.word_class.size(); ++i) {
        const uint32_t a = prog.word_base + static_cast<uint32_t>(4 * i);
        if (!c.in_exec(a)) continue;
        ++prog.stats.exec_words;
        prog.stats.nonzero_words += mem.read<uint32_t>(a) != 0;
        prog.stats.code_words += prog.word_class[i] == 1;
        prog.stats.table_words += prog.word_class[i] == 2;
        prog.stats.overlapping_instructions += claims[i] > 1;
    }
    prog.stats.invalid_reached = static_cast<uint32_t>(prog.invalid_sites.size());
    prog.stats.noreturn_calls = static_cast<uint32_t>(prog.noreturn_sites.size());
    return prog;
}

}  // namespace sly4::recomp
