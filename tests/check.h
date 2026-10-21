// Minimal test harness (no external dependency): TEST(name) registers a test,
// CHECK/CHECK_EQ record failures without stopping, SKIP() ends a test as skipped.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace sly4::test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
int& failures();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Skip {
    std::string reason;
};

template <class T>
std::string show(const T& v) {
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
        return buf;
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(v);
    } else {
        return std::string(v);
    }
}

}  // namespace sly4::test

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::sly4::test::Registrar name##_registrar{#name, name};        \
    static void name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++::sly4::test::failures();                                                  \
            std::printf("    FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        /* copies: binding a reference to e.g. *optional_temporary would dangle */                \
        const auto va_ = (a);                                                                       \
        const auto vb_ = (b);                                                                       \
        if (!(va_ == vb_)) {                                                                        \
            ++::sly4::test::failures();                                                             \
            std::printf("    FALHOU %s:%d: %s == %s (%s != %s)\n", __FILE__, __LINE__, #a, #b,      \
                        ::sly4::test::show(va_).c_str(), ::sly4::test::show(vb_).c_str());          \
        }                                                                                           \
    } while (0)

#define SKIP(reason) throw ::sly4::test::Skip{reason}
