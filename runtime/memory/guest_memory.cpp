#include "memory/guest_memory.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/log.h"

namespace sly4 {

namespace {

DWORD to_win32(Protection p) {
    switch (p) {
        case Protection::ReadOnly: return PAGE_READONLY;
        case Protection::ReadWrite: return PAGE_READWRITE;
        default: return PAGE_NOACCESS;
    }
}

// [addr, addr + size) widened to whole pages; false if it leaves the 4 GiB space.
bool page_range(GuestAddr addr, uint32_t size, uint64_t& start, uint64_t& length) {
    if (size == 0) return false;
    start = addr & ~uint64_t(GuestMemory::kPageSize - 1);
    uint64_t end = (uint64_t(addr) + size + GuestMemory::kPageSize - 1) & ~uint64_t(GuestMemory::kPageSize - 1);
    if (end > GuestMemory::kSize) return false;
    length = end - start;
    return true;
}

}  // namespace

bool GuestMemory::reserve() {
    if (base_) return true;
    void* p = VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        SLY4_ERROR("não foi possível reservar 4 GiB para a memória do PS3 (erro %lu)", GetLastError());
        return false;
    }
    base_ = static_cast<uint8_t*>(p);
    return true;
}

void GuestMemory::release() {
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
    base_ = nullptr;
}

bool GuestMemory::commit(GuestAddr addr, uint32_t size, Protection prot) {
    uint64_t start, length;
    if (!base_ || !page_range(addr, size, start, length)) return false;
    if (!VirtualAlloc(base_ + start, length, MEM_COMMIT, to_win32(prot))) {
        SLY4_ERROR("commit de 0x%08X (+0x%X) falhou (erro %lu)", addr, size, GetLastError());
        return false;
    }
    return true;
}

bool GuestMemory::protect(GuestAddr addr, uint32_t size, Protection prot) {
    uint64_t start, length;
    if (!base_ || !page_range(addr, size, start, length)) return false;
    DWORD old;
    return VirtualProtect(base_ + start, length, to_win32(prot), &old) != 0;
}

void GuestMemory::decommit(GuestAddr addr, uint32_t size) {
    uint64_t start, length;
    if (base_ && page_range(addr, size, start, length)) VirtualFree(base_ + start, length, MEM_DECOMMIT);
}

bool GuestMemory::is_committed(GuestAddr addr, uint32_t size) const {
    uint64_t start, length;
    if (!base_ || !page_range(addr, size, start, length)) return false;
    uint64_t pos = start, end = start + length;
    while (pos < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(base_ + pos, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        pos = static_cast<uint64_t>(static_cast<uint8_t*>(mbi.BaseAddress) - base_) + mbi.RegionSize;
    }
    return true;
}

uint64_t GuestMemory::committed_bytes() const {
    uint64_t total = 0, pos = 0;
    while (base_ && pos < kSize) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(base_ + pos, &mbi, sizeof(mbi))) break;
        uint64_t region_end = static_cast<uint64_t>(static_cast<uint8_t*>(mbi.BaseAddress) - base_) + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) total += std::min(region_end, kSize) - pos;
        pos = region_end;
    }
    return total;
}

std::optional<GuestAddr> GuestMemory::guest_of(const void* p) const {
    auto q = static_cast<const uint8_t*>(p);
    if (!base_ || q < base_ || q >= base_ + kSize) return std::nullopt;
    return static_cast<GuestAddr>(q - base_);
}

Vec128 GuestMemory::read128(GuestAddr addr) const {
    Vec128 v;
    const uint8_t* src = host(addr);
    for (int i = 0; i < 16; ++i) v.b[i] = src[15 - i];
    return v;
}

void GuestMemory::write128(GuestAddr addr, const Vec128& v) {
    uint8_t* dst = host(addr);
    for (int i = 0; i < 16; ++i) dst[i] = v.b[15 - i];
}

std::string GuestMemory::read_cstring(GuestAddr addr, size_t max_len) const {
    std::string s;
    uint64_t a = addr;
    while (s.size() < max_len && a < kSize) {
        if ((a & (kPageSize - 1)) == 0 || s.empty()) {
            if (!is_committed(static_cast<GuestAddr>(a), 1)) break;
        }
        char c = static_cast<char>(base_[a++]);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

namespace {

const GuestMemory* g_fault_memory = nullptr;
constexpr uint32_t kPage = GuestMemory::kPageSize;

void set_protection(GuestAddr page, bool read_only) {
    DWORD old = 0;
    VirtualProtect(g_fault_memory->host(page), kPage, read_only ? PAGE_READONLY : PAGE_READWRITE, &old);
}

// --watch-write: one guest page kept read-only; each write to it is reported and then let
// through, one instruction with the page writable (single step), after which it is
// protected again.
struct WriteWatch {
    GuestAddr page = 0, first = 0, last = 0;  // the page, and the watched bytes [first, last)
    WriteWatchHook hook = nullptr;
    bool armed = false;
};
WriteWatch g_watch;

// Tracked ranges (track_writes): their pages read-only, counted per page because ranges
// can share one. A write inside a range ends its tracking and is recorded; a write to a
// tracked page outside every range passes like a watched one.
std::mutex g_track_lock;
std::map<GuestAddr, uint32_t> g_tracked;                 // start -> size
std::vector<std::pair<GuestAddr, uint32_t>> g_written;  // ranges ended by a write
std::atomic<bool> g_any_written{false};
std::unique_ptr<std::atomic<uint8_t>[]> g_page_count;   // per 4 KiB page of the 4 GiB space
thread_local GuestAddr t_reprotect = 0;  // page to protect again after the single step (+1)
thread_local int t_host_writes = 0;     // HostWriteScope: this thread writes for the renderer

std::atomic<uint8_t>& page_count(GuestAddr page) { return g_page_count[page / kPage]; }

// Counts the range's pages up or down; the pages whose count leaves or reaches zero change
// protection, consecutive ones in a single call (a 1280x720 surface is 900 pages, and one
// VirtualProtect each cost milliseconds per frame).
void change_range(GuestAddr addr, uint32_t size, bool protect) {
    GuestAddr run = 0;
    uint32_t pages = 0;
    auto apply = [&] {
        if (!pages) return;
        DWORD old = 0;
        VirtualProtect(g_fault_memory->host(run), size_t(pages) * kPage, protect ? PAGE_READONLY : PAGE_READWRITE, &old);
        pages = 0;
    };
    for (GuestAddr p = addr & ~(kPage - 1); p < addr + size; p += kPage) {
        const bool flips = protect ? page_count(p)++ == 0 : --page_count(p) == 0;
        if (!flips) {
            apply();
            continue;
        }
        if (!pages) run = p;
        ++pages;
    }
    apply();
}
void protect_range(GuestAddr addr, uint32_t size) { change_range(addr, size, true); }
void unprotect_range(GuestAddr addr, uint32_t size) { change_range(addr, size, false); }

// Ends the tracking of every range overlapping [addr, addr + size); with `record` they go
// to the written list. Under g_track_lock.
bool end_tracking(GuestAddr addr, uint32_t size, bool record) {
    bool found = false;
    auto it = g_tracked.upper_bound(addr);
    if (it != g_tracked.begin()) --it;
    while (it != g_tracked.end() && it->first < addr + size) {
        if (it->first + it->second > addr) {
            unprotect_range(it->first, it->second);
            if (record) g_written.emplace_back(it->first, it->second);
            it = g_tracked.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (found && record) g_any_written = true;
    return found;
}

// Lets one write to `page` through: writable for one instruction, then protected again.
LONG pass_once(EXCEPTION_POINTERS* info, GuestAddr page) {
    set_protection(page, false);
    info->ContextRecord->EFlags |= 0x100;  // trap after this one instruction
    t_reprotect = page + 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK guest_fault_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reprotect) {
        const GuestAddr page = t_reprotect - 1;
        t_reprotect = 0;
        if (g_watch.armed && page == g_watch.page) {
            set_protection(page, true);
        } else if (g_page_count) {
            // Under the lock: another thread may have ended the last range on this page
            // meanwhile, and protecting it then would leave it read-only for good.
            std::lock_guard<std::mutex> lk(g_track_lock);
            if (page_count(page) > 0) set_protection(page, true);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || !g_fault_memory)
        return EXCEPTION_CONTINUE_SEARCH;
    auto guest = g_fault_memory->guest_of(reinterpret_cast<const void*>(rec->ExceptionInformation[1]));
    if (guest && rec->ExceptionInformation[0] == 1) {
        const GuestAddr page = *guest & ~(kPage - 1);
        if (g_watch.armed && page == g_watch.page) {
            if (*guest >= g_watch.first && *guest < g_watch.last) g_watch.hook(*guest, rec->ExceptionAddress);
            return pass_once(info, page);
        }
        if (g_page_count) {
            std::lock_guard<std::mutex> lk(g_track_lock);
            if (page_count(page) > 0) {
                // The renderer writing a surface back passes without ending anything; a
                // write inside a range ends it; one beside them passes once.
                if (t_host_writes || !end_tracking(*guest, 1, true)) {
                    if (page_count(page) > 0) return pass_once(info, page);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            // Its last range may have ended between the fault and here: then the page is
            // writable again and the write is simply retried.
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(g_fault_memory->host(page), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                mbi.Protect == PAGE_READWRITE)
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (guest) {
        const char* kind = rec->ExceptionInformation[0] == 1 ? "escrita" : rec->ExceptionInformation[0] == 8 ? "execução" : "leitura";
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        SLY4_ERROR("acesso inválido à memória do PS3: %s em 0x%08X (código do host: RVA 0x%llX no sly4.exe; página %s, "
                   "%u faixas vigiadas nela)",
                   kind, *guest, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress) - exe),
                   g_fault_memory->is_committed(*guest & ~(kPage - 1), 1) ? "confirmada" : "não confirmada",
                   g_page_count ? unsigned(page_count(*guest & ~(kPage - 1))) : 0u);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_guest_fault_handler(const GuestMemory& mem) {
    if (!g_fault_memory) AddVectoredExceptionHandler(1, guest_fault_handler);
    g_fault_memory = &mem;
    if (!g_page_count) g_page_count = std::make_unique<std::atomic<uint8_t>[]>(0x100000);
}

bool watch_writes(GuestAddr addr, uint32_t size, WriteWatchHook hook) {
    if (!g_fault_memory || !hook || !g_fault_memory->is_committed(addr, 1)) return false;
    g_watch.page = addr & ~(kPage - 1);
    g_watch.first = addr;
    g_watch.last = addr + size;
    g_watch.hook = hook;
    g_watch.armed = true;
    set_protection(g_watch.page, true);
    return true;
}

void track_writes(GuestAddr addr, uint32_t size) {
    if (!g_page_count || !size || !g_fault_memory->is_committed(addr, size)) return;
    std::lock_guard<std::mutex> lk(g_track_lock);
    end_tracking(addr, size, false);  // a range drawn again replaces the old one
    g_tracked[addr] = size;
    protect_range(addr, size);
}

void untrack_writes(GuestAddr addr, uint32_t size) {
    if (!g_page_count || !size) return;
    std::lock_guard<std::mutex> lk(g_track_lock);
    end_tracking(addr, size, false);
}

void before_host_write(GuestAddr addr, uint32_t size) {
    if (!g_page_count || !size) return;
    std::lock_guard<std::mutex> lk(g_track_lock);
    end_tracking(addr, size, true);
}

void before_host_write(const void* host, uint64_t size) {
    if (!g_fault_memory) return;
    if (auto g = g_fault_memory->guest_of(host)) before_host_write(*g, uint32_t(size));
}

bool take_written(std::vector<std::pair<GuestAddr, uint32_t>>& out) {
    if (!g_any_written.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(g_track_lock);
    out.swap(g_written);
    g_written.clear();
    g_any_written = false;
    return !out.empty();
}

HostWriteScope::HostWriteScope() { ++t_host_writes; }
HostWriteScope::~HostWriteScope() { --t_host_writes; }

}  // namespace sly4
