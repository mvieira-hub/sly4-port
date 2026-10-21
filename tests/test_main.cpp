// sly4_tests.exe [filter]: runs every registered test (or those whose name contains filter).
#include <cstring>

#include "check.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sly4::test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

int& failures() {
    static int n = 0;
    return n;
}

}  // namespace sly4::test

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    using namespace sly4::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0, skipped = 0;
    for (const Case& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = failures();
        try {
            c.fn();
            if (failures() == before) {
                ++passed;
                std::printf("ok      %s\n", c.name);
            } else {
                ++failed;
                std::printf("FALHOU  %s\n", c.name);
            }
        } catch (const Skip& s) {
            ++skipped;
            std::printf("pulado  %s (%s)\n", c.name, s.reason.c_str());
        }
    }
    std::printf("\n%d ok, %d falharam, %d pulados\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
