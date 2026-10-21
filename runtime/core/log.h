// Minimal logging to the console (stderr), flushed per line so it survives crashes.
#pragma once

#include <cstdio>

#define SLY4_LOG(level, ...)                         \
    do {                                             \
        std::fprintf(stderr, "[" level "] " __VA_ARGS__); \
        std::fputc('\n', stderr);                    \
        std::fflush(stderr);                         \
    } while (0)

#define SLY4_INFO(...) SLY4_LOG("info", __VA_ARGS__)
#define SLY4_WARN(...) SLY4_LOG("aviso", __VA_ARGS__)
#define SLY4_ERROR(...) SLY4_LOG("erro", __VA_ARGS__)
