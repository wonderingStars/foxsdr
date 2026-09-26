// frame_log.cpp - FrameCap (see frame_log.hpp). Out of line so the header,
// which app_window.hpp includes, never pulls in <windows.h>.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/frame_log.hpp"

#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace cascade::gui {

namespace {
std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

std::unique_ptr<FrameCap> FrameCap::fromEnvironment() {
    const char* v = std::getenv("FOXSDR_FRAME_CAP_HZ");
    if (v == nullptr || *v == '\0') { return nullptr; }
    const double hz = std::atof(v);
    if (!(hz > 0.0) || hz > 1000.0) { return nullptr; }
    return std::unique_ptr<FrameCap>(new FrameCap(hz));
}

FrameCap::FrameCap(double hz) : periodNs_(static_cast<std::int64_t>(1.0e9 / hz)) {
#if defined(_WIN32)
    // High resolution where the OS has it (Windows 10 1803+); an ordinary
    // waitable timer otherwise, which is still no coarser than Sleep.
    HANDLE h = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    if (h == nullptr) { h = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); }
    timer_ = h;
#endif
}

FrameCap::~FrameCap() {
#if defined(_WIN32)
    if (timer_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(timer_)); }
#endif
}

void FrameCap::frameStart() { startNs_ = steadyNowNs(); }

void FrameCap::waitForNext() {
    const std::int64_t due = startNs_ + periodNs_;
    const std::int64_t left = due - steadyNowNs();
    if (left <= 0) { return; }
#if defined(_WIN32)
    if (timer_ != nullptr) {
        LARGE_INTEGER rel;
        rel.QuadPart = -static_cast<LONGLONG>(left / 100);  // 100 ns units, relative
        if (::SetWaitableTimer(static_cast<HANDLE>(timer_), &rel, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(static_cast<HANDLE>(timer_), 1000);  // bounded: left < one period
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::nanoseconds(left));
}

}  // namespace cascade::gui
