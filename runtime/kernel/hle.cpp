#include "kernel/hle.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "core/log.h"
#include "kernel/guest_alloc.h"
#include "kernel/kernel.h"
#include "loader/elf_loader.h"
#include "loader/nid.h"

namespace sly4 {

bool CallLog::first_call(uint32_t key) {
    std::lock_guard<std::mutex> g(lock_);
    return seen_.insert(key).second;
}

void CallLog::record(const std::string& line) {
    {
        std::lock_guard<std::mutex> g(lock_);
        sequence_.push_back(line);
    }
    std::fprintf(stderr, "[hle] %s\n", line.c_str());
    std::fflush(stderr);
}

std::vector<std::string> CallLog::sequence() const {
    std::lock_guard<std::mutex> g(lock_);
    return sequence_;
}

void CallLog::ring(const char* thread, const char* name, uint64_t r3, uint64_t r4, uint32_t lr) {
    std::lock_guard<std::mutex> g(ring_lock_);
    ring_[ring_next_++ % kRing] = {thread, name, r3, r4, lr};
}

void CallLog::print_ring(size_t last) const {
    std::lock_guard<std::mutex> g(ring_lock_);
    const size_t n = std::min({last, ring_next_, kRing});
    std::fprintf(stderr, "[info] últimas %zu chamadas:\n", n);
    for (size_t i = ring_next_ - n; i < ring_next_; ++i) {
        const RingEntry& e = ring_[i % kRing];
        std::fprintf(stderr, "  {%s} %s(0x%" PRIX64 ", 0x%" PRIX64 ") <- 0x%08X\n", e.thread, e.name, e.r3, e.r4, e.lr);
    }
}

CallLog& call_log() {
    static CallLog log;
    return log;
}

HleTable& hle() {
    static HleTable table;
    return table;
}

void HleTable::add(const HleModule& module) {
    for (const HleFunction& f : module.functions) known_.push_back({module.name, f.name, f.fn});
}

bool HleTable::bind(GuestMemory& mem, GuestAllocator& alloc, const LoadedImage& img, const NidNames& names) {
    size_t total = img.imported_function_count();
    // Descriptors (8 bytes each) followed by the thunk words, plus the thread-return thunk.
    const uint32_t bytes = uint32_t(total * 8 + (total + 1) * 4);
    const GuestAddr area = alloc.alloc(bytes, 0x100);
    if (!area) return false;
    const GuestAddr desc_base = area;
    thunk_base_ = area + uint32_t(total * 8);
    thunk_count_ = uint32_t(total + 1);

    size_t implemented = 0;
    for (const ImportedModule& mod : img.imports) {
        for (const ImportedFunction& f : mod.functions) {
            Import& im = imports_.emplace_back();
            im.module = mod.name;
            im.nid = f.nid;
            for (const Known& k : known_)
                if (k.module == mod.name && nid_of(k.name) == f.nid) {
                    im.name = k.name;
                    im.fn = k.fn;
                }
            if (im.name.empty()) {
                const std::string* n = names.find(f.nid);
                char buf[24];
                std::snprintf(buf, sizeof(buf), "nid_%08X", f.nid);
                im.name = n ? *n : buf;
            }
            implemented += im.fn != nullptr;
            const uint32_t i = uint32_t(imports_.size() - 1);
            const GuestAddr desc = desc_base + 8 * i;
            mem.write<uint32_t>(desc, thunk_base_ + 4 * i);
            mem.write<uint32_t>(desc + 4, 0);
            mem.write<uint32_t>(f.slot, desc);
        }
    }
    SLY4_INFO("HLE: %zu importações ligadas, %zu com implementação nativa; descritores em 0x%08X", imports_.size(),
              implemented, desc_base);
    return true;
}

void HleTable::call(PPUContext& c, GuestMemory& m, uint32_t target) {
    const uint32_t i = (target - thunk_base_) / 4;
    if (i >= imports_.size()) {
        SLY4_ERROR("HLE: salto para o retorno de thread fora da entrada (LR 0x%08" PRIX64 ")", c.lr);
        return;
    }
    Import& im = imports_[i];
    im.calls.fetch_add(1, std::memory_order_relaxed);
    if (PpuThread* t = current_thread()) {
        t->last_call = im.name.c_str();
        t->last_lr = uint32_t(c.lr) & ~3u;
        t->last_r3 = c.gpr[3];
        t->last_r4 = c.gpr[4];
        call_log().ring(t->name.c_str(), im.name.c_str(), c.gpr[3], c.gpr[4], uint32_t(c.lr) & ~3u);
    }
    CallLog& log = call_log();
    const bool first = log.first_call(i);
    if (first || log.trace) {
        char line[320];
        const PpuThread* t = current_thread();
        std::snprintf(line, sizeof(line),
                      "{%s} %s.%s(0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64 ", 0x%" PRIX64
                      ") <- 0x%08X%s",
                      t ? t->name.c_str() : "?", im.module.c_str(), im.name.c_str(), c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6],
                      c.gpr[7], c.gpr[8],
                      uint32_t(c.lr) & ~3u, im.fn ? "" : "  [sem implementação: CELL_OK]");
        log.record(line);
    }
    if (im.fn)
        im.fn(c, m);
    else
        c.gpr[3] = 0;
}

std::vector<HleTable::Stat> HleTable::stats() const {
    std::vector<Stat> out;
    for (const Import& im : imports_) out.push_back({im.module, im.name, im.calls.load(), im.fn != nullptr});
    return out;
}

}  // namespace sly4
