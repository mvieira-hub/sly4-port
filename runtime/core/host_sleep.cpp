#include "core/host_sleep.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace sly4 {

namespace {
struct Timer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() {
        if (h) CloseHandle(h);
    }
};
}  // namespace

void host_sleep_us(uint64_t us) {
    thread_local Timer t;
    if (!t.h) {  // Windows before 10 1803: millisecond sleeps
        Sleep(DWORD((us + 999) / 1000));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us * 10);  // relative, 100 ns units
    SetWaitableTimer(t.h, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(t.h, INFINITE);
}

void host_sleep_until(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        host_sleep_us(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count()) + 1);
    }
}

}  // namespace sly4
