// Short sleeps on the host. With MinGW, std::this_thread::sleep_for below a millisecond
// returns at once (Sleep(0)), which turns polling loops into busy loops and makes
// periodic clocks drift; these use a high-resolution waitable timer instead.
#pragma once

#include <chrono>
#include <cstdint>

namespace sly4 {

void host_sleep_us(uint64_t us);
// Sleeps until `deadline` (steady clock), to within the timer's resolution.
void host_sleep_until(std::chrono::steady_clock::time_point deadline);

}  // namespace sly4
