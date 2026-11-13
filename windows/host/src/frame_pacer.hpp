#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace wireview::host {
// Ordinary Sleep(1) can wait an entire ~15.6 ms tick in the hidden Host.
// A per-stream high-resolution timer preserves 8.33 ms pacing without spinning.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    FramePacer() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {
        if (timer_ == nullptr) { fail("CreateWaitableTimerExW(frame pacing)"); }
    }
    ~FramePacer() { CloseHandle(timer_); }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    void wait_until(Clock::time_point target) {
        const auto remaining = target - Clock::now();
        if (remaining <= Clock::duration::zero()) { return; }
        using TimerTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<TimerTicks>(remaining).count());
        if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            fail("SetWaitableTimerEx(frame pacing)");
        }
        if (WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0) { fail("WaitForSingleObject(frame pacing)"); }
    }
    void wait_for(Clock::duration duration) { wait_until(Clock::now() + duration); }
private:
    [[noreturn]] static void fail(const char* operation) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(GetLastError()));
    }
    HANDLE timer_{};
};
} // namespace wireview::host
