#include "memory/guest_memory.h"

#include <algorithm>

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

LONG CALLBACK guest_fault_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || !g_fault_memory)
        return EXCEPTION_CONTINUE_SEARCH;
    auto guest = g_fault_memory->guest_of(reinterpret_cast<const void*>(rec->ExceptionInformation[1]));
    if (guest) {
        const char* kind = rec->ExceptionInformation[0] == 1 ? "escrita" : rec->ExceptionInformation[0] == 8 ? "execução" : "leitura";
        SLY4_ERROR("acesso inválido à memória do PS3: %s em 0x%08X (código do host em %p)", kind, *guest,
                   rec->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_guest_fault_handler(const GuestMemory& mem) {
    if (!g_fault_memory) AddVectoredExceptionHandler(1, guest_fault_handler);
    g_fault_memory = &mem;
}

}  // namespace sly4
