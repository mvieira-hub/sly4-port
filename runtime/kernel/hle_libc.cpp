// sysPrxForUser's small C library (_sys_memset, _sys_strcpy, _sys_printf...) and process
// helpers, as firmware modules (libsre) use them.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "kernel/hle.h"
#include "kernel/kernel.h"
#include "loader/elf_loader.h"

namespace sly4 {

namespace {

uint32_t a32(const PPUContext& c, int n) { return uint32_t(c.gpr[n]); }

// ---------------------------------------------------------------- memory and strings
void sys_memset(PPUContext& c, GuestMemory& m) {
    std::memset(m.host(a32(c, 3)), int(c.gpr[4] & 0xFF), a32(c, 5));
}
void sys_memcpy(PPUContext& c, GuestMemory& m) { std::memmove(m.host(a32(c, 3)), m.host(a32(c, 4)), a32(c, 5)); }
void sys_memcmp(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = uint64_t(int64_t(std::memcmp(m.host(a32(c, 3)), m.host(a32(c, 4)), a32(c, 5))));
}
// Guest strings are copied with memmove: the compiler cannot prove two guest pointers
// distinct, and the PS3 functions do not promise anything when they overlap either.
char* str(GuestMemory& m, uint32_t a) { return reinterpret_cast<char*>(m.host(a)); }
void sys_strlen(PPUContext& c, GuestMemory& m) { c.gpr[3] = std::strlen(str(m, a32(c, 3))); }
void sys_strcpy(PPUContext& c, GuestMemory& m) {
    std::memmove(str(m, a32(c, 3)), str(m, a32(c, 4)), std::strlen(str(m, a32(c, 4))) + 1);
}
void sys_strncpy(PPUContext& c, GuestMemory& m) {
    const size_t n = a32(c, 5), len = strnlen(str(m, a32(c, 4)), n);
    std::memmove(str(m, a32(c, 3)), str(m, a32(c, 4)), len);
    std::memset(str(m, a32(c, 3)) + len, 0, n - len);
}
void sys_strcat(PPUContext& c, GuestMemory& m) {
    char* d = str(m, a32(c, 3));
    std::memmove(d + std::strlen(d), str(m, a32(c, 4)), std::strlen(str(m, a32(c, 4))) + 1);
}
void sys_strncat(PPUContext& c, GuestMemory& m) {
    char* d = str(m, a32(c, 3)) + std::strlen(str(m, a32(c, 3)));
    const size_t len = strnlen(str(m, a32(c, 4)), a32(c, 5));
    std::memmove(d, str(m, a32(c, 4)), len);
    d[len] = 0;
}
void sys_strcmp(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = uint64_t(int64_t(
        std::strcmp(reinterpret_cast<char*>(m.host(a32(c, 3))), reinterpret_cast<char*>(m.host(a32(c, 4))))));
}
void sys_strncmp(PPUContext& c, GuestMemory& m) {
    c.gpr[3] = uint64_t(int64_t(std::strncmp(reinterpret_cast<char*>(m.host(a32(c, 3))),
                                             reinterpret_cast<char*>(m.host(a32(c, 4))), a32(c, 5))));
}

// ---------------------------------------------------------------- printf
// Variadic arguments on the PPC64 ELF ABI: 64-bit slots in r3..r10, then the caller's
// parameter save area (48(r1) + 8 * slot); a va_list points at such slots.
struct Args {
    PPUContext& c;
    GuestMemory& m;
    int slot;         // next slot index when reading registers
    GuestAddr list;   // va_list (0: registers / stack)
    uint64_t next() {
        if (list) {
            const uint64_t v = m.read<uint64_t>(list);
            list += 8;
            return v;
        }
        const int s = slot++;
        return s < 8 ? c.gpr[3 + s] : m.read<uint64_t>(uint32_t(c.gpr[1]) + 48 + 8 * uint32_t(s));
    }
};

std::string format(GuestMemory& m, const char* f, Args& args) {
    std::string out;
    char spec[32], buf[512];
    while (*f) {
        if (*f != '%') {
            out += *f++;
            continue;
        }
        const char* start = f++;
        if (*f == '%') {
            out += '%';
            ++f;
            continue;
        }
        while (*f && std::strchr("-+ #0", *f)) ++f;
        if (*f == '*') {
            args.next();  // width from the arguments (ignored)
            ++f;
        }
        while (*f >= '0' && *f <= '9') ++f;
        if (*f == '.') {
            ++f;
            if (*f == '*') {
                args.next();
                ++f;
            }
            while (*f >= '0' && *f <= '9') ++f;
        }
        while (*f && std::strchr("hlLqjzt", *f)) ++f;
        const char conv = *f ? *f++ : 0;
        // The spec without length modifiers, with the right one for the host type.
        std::string s;
        for (const char* p = start; p < f - 1; ++p)
            if (!std::strchr("hlLqjzt*", *p)) s += *p;
        const uint64_t v = conv ? args.next() : 0;
        switch (conv) {
            case 'd': case 'i':
                std::snprintf(spec, sizeof(spec), "%sll%c", s.c_str(), conv);
                std::snprintf(buf, sizeof(buf), spec, (long long)int64_t(v));
                break;
            case 'u': case 'x': case 'X': case 'o':
                std::snprintf(spec, sizeof(spec), "%sll%c", s.c_str(), conv);
                std::snprintf(buf, sizeof(buf), spec, (unsigned long long)v);
                break;
            case 'c':
                std::snprintf(spec, sizeof(spec), "%sc", s.c_str());
                std::snprintf(buf, sizeof(buf), spec, int(v & 0xFF));
                break;
            case 's':
                std::snprintf(spec, sizeof(spec), "%ss", s.c_str());
                std::snprintf(buf, sizeof(buf), spec, v ? m.read_cstring(uint32_t(v), 400).c_str() : "(null)");
                break;
            case 'p':
                std::snprintf(buf, sizeof(buf), "0x%x", uint32_t(v));
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                double d;
                std::memcpy(&d, &v, 8);
                std::snprintf(spec, sizeof(spec), "%s%c", s.c_str(), conv);
                std::snprintf(buf, sizeof(buf), spec, d);
                break;
            }
            default: buf[0] = 0; break;
        }
        out += buf;
    }
    return out;
}

void put_console(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    std::fprintf(stderr, "[printf] %s\n", t.c_str());
    std::fflush(stderr);
}
void write_out(GuestMemory& m, GuestAddr buf, uint32_t size, const std::string& s) {
    if (!size) return;
    const uint32_t n = std::min<uint32_t>(uint32_t(s.size()), size - 1);
    m.copy_to(buf, s.data(), n);
    m.write<uint8_t>(buf + n, 0);
}

// _sys_printf(fmt, ...)
void sys_printf(PPUContext& c, GuestMemory& m) {
    Args args{c, m, 1, 0};
    const std::string s = format(m, m.read_cstring(a32(c, 3), 1024).c_str(), args);
    put_console(s);
    c.gpr[3] = s.size();
}
// _sys_vprintf(fmt, va_list)
void sys_vprintf(PPUContext& c, GuestMemory& m) {
    Args args{c, m, 0, a32(c, 4)};
    const std::string s = format(m, m.read_cstring(a32(c, 3), 1024).c_str(), args);
    put_console(s);
    c.gpr[3] = s.size();
}
// _sys_sprintf(buf, fmt, ...)
void sys_sprintf(PPUContext& c, GuestMemory& m) {
    Args args{c, m, 2, 0};
    const std::string s = format(m, m.read_cstring(a32(c, 4), 1024).c_str(), args);
    write_out(m, a32(c, 3), 0x10000, s);
    c.gpr[3] = s.size();
}
// _sys_snprintf(buf, size, fmt, ...)
void sys_snprintf(PPUContext& c, GuestMemory& m) {
    Args args{c, m, 3, 0};
    const std::string s = format(m, m.read_cstring(a32(c, 5), 1024).c_str(), args);
    write_out(m, a32(c, 3), a32(c, 4), s);
    c.gpr[3] = s.size();
}
// _sys_vsnprintf(buf, size, fmt, va_list)
void sys_vsnprintf(PPUContext& c, GuestMemory& m) {
    Args args{c, m, 0, a32(c, 6)};
    const std::string s = format(m, m.read_cstring(a32(c, 5), 1024).c_str(), args);
    write_out(m, a32(c, 3), a32(c, 4), s);
    c.gpr[3] = s.size();
}

// ---------------------------------------------------------------- process
// sys_process_get_paramsfo(buffer): 0x40 bytes, the first one 1 and the title id after it.
void sys_process_get_paramsfo(PPUContext& c, GuestMemory& m) {
    const uint32_t b = a32(c, 3);
    std::memset(m.host(b), 0, 0x40);
    m.write<uint8_t>(b, 1);
    const std::string id = param_sfo_string("TITLE_ID");
    m.copy_to(b + 1, id.data(), std::min<size_t>(id.size(), 9));
    c.gpr[3] = CELL_OK;
}
// sys_prx_get_module_id_by_name: no other module is loaded by name.
void sys_prx_get_module_id_by_name(PPUContext& c, GuestMemory& m) {
    SLY4_INFO("sys_prx_get_module_id_by_name(\"%s\"): não carregado", m.read_cstring(a32(c, 3), 64).c_str());
    c.gpr[3] = 0x8001112E;  // CELL_PRX_ERROR_UNKNOWN_MODULE
}

// SPU printf: the game registers callbacks that connect a group's/thread's SPU printf
// port to its printf server; attach/detach call them.
std::mutex g_spf_lock;
GuestAddr g_spf[4] = {};  // attach group, detach group, attach thread, detach thread (descriptors)
void sys_spu_printf_initialize(PPUContext& c, GuestMemory&) {
    std::lock_guard<std::mutex> g(g_spf_lock);
    for (int i = 0; i < 4; ++i) g_spf[i] = a32(c, 4 + i);
    c.gpr[3] = CELL_OK;
}
template <int K>
void sys_spu_printf_call(PPUContext& c, GuestMemory& m) {
    GuestAddr fn;
    {
        std::lock_guard<std::mutex> g(g_spf_lock);
        fn = g_spf[K];
    }
    if (!fn) {
        c.gpr[3] = CELL_ESTAT;
        return;
    }
    call_guest(c, m, fn);  // r3 (group or thread id) is passed through
}

}  // namespace

HleModule hle_sys_prx_for_user_libc() {
    return {"sysPrxForUser",
            {
                {"_sys_memset", sys_memset},
                {"_sys_memcpy", sys_memcpy},
                {"_sys_memcmp", sys_memcmp},
                {"_sys_strlen", sys_strlen},
                {"_sys_strcpy", sys_strcpy},
                {"_sys_strncpy", sys_strncpy},
                {"_sys_strcat", sys_strcat},
                {"_sys_strncat", sys_strncat},
                {"_sys_strcmp", sys_strcmp},
                {"_sys_strncmp", sys_strncmp},
                {"_sys_printf", sys_printf},
                {"_sys_vprintf", sys_vprintf},
                {"_sys_sprintf", sys_sprintf},
                {"_sys_snprintf", sys_snprintf},
                {"_sys_vsnprintf", sys_vsnprintf},
                {"sys_process_get_paramsfo", sys_process_get_paramsfo},
                {"sys_prx_get_module_id_by_name", sys_prx_get_module_id_by_name},
                {"_sys_spu_printf_initialize", sys_spu_printf_initialize},
                {"_sys_spu_printf_attach_group", sys_spu_printf_call<0>},
                {"_sys_spu_printf_detach_group", sys_spu_printf_call<1>},
                {"_sys_spu_printf_attach_thread", sys_spu_printf_call<2>},
                {"_sys_spu_printf_detach_thread", sys_spu_printf_call<3>},
            }};
}

}  // namespace sly4
