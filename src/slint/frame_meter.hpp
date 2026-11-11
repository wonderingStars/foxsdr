// THE FRAME METER (screen stage S6, B8; docs/multi-radio-records/screen-s6.md): what paces the Slint host's frames, measured in the host itself.
//
// A bounded run with FOXSDR_SLINT_FRAME_LOG=1 notes (1) when each 33 ms tick of the host starts and how long its work took (the host's own timer), and (2) when the renderer finished each frame
// (Slint's rendering notifier, AfterRendering - available only to the OpenGL renderers; the software renderer has none and says so), and prints one summary at the end of the run:
//
//   FRAMEMETER ticks=<n> interval mean/median/p95/max <ms> work mean/p95/max <ms> draws=<n> interval mean/median/p95/max <ms> cpu=<process CPU seconds> wall=<seconds> notifier=<yes|no>
//
// Nothing is measured, kept or printed outside a bounded run with the variable set; the cost in a release is one null pointer test per tick.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace foxsdr::slinthost {

class FrameMeter {
public:
    using Clock = std::chrono::steady_clock;

    void begin() { start_ = Clock::now(); startCpu_ = cpuSeconds(); }
    // the start of a tick; returns the moment, for endTick
    Clock::time_point tickStart() {
        const auto now = Clock::now();
        if (haveTick_) { tickMs_.push_back(ms(now - lastTick_)); }
        lastTick_ = now;
        haveTick_ = true;
        return now;
    }
    void tickEnd(Clock::time_point began) { workMs_.push_back(ms(Clock::now() - began)); }
    // a frame the renderer finished
    void drawn() {
        const auto now = Clock::now();
        if (haveDraw_) { drawMs_.push_back(ms(now - lastDraw_)); }
        lastDraw_ = now;
        haveDraw_ = true;
        ++draws_;
    }
    void setNotifier(bool yes) { notifier_ = yes; }
    // skip the first n of each series (a window that is still opening is not the steady state)
    std::string summary(std::size_t warmup = 20) const {
        const auto trim = [&](std::vector<double> v) { if (v.size() > warmup) { v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(warmup)); } return v; };
        const Stats t = stats(trim(tickMs_)), w = stats(trim(workMs_)), d = stats(trim(drawMs_));
        const double wall = std::chrono::duration<double>(Clock::now() - start_).count();
        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "FRAMEMETER ticks=%zu interval mean %.2f median %.2f p95 %.2f max %.2f work mean %.2f p95 %.2f max %.2f draws=%zu interval mean %.2f median %.2f p95 %.2f max %.2f cpu=%.2f wall=%.2f notifier=%s",
                      tickMs_.size(), t.mean, t.median, t.p95, t.max, w.mean, w.p95, w.max, draws_, d.mean, d.median, d.p95, d.max, cpuSeconds() - startCpu_, wall, notifier_ ? "yes" : "no");
        return buf;
    }

private:
    struct Stats { double mean = 0, median = 0, p95 = 0, max = 0; };
    static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    static Stats stats(std::vector<double> v) {
        Stats s;
        if (v.empty()) { return s; }
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (const double x : v) { sum += x; }
        s.mean = sum / static_cast<double>(v.size());
        s.median = v[v.size() / 2];
        s.p95 = v[std::min(v.size() - 1, static_cast<std::size_t>(static_cast<double>(v.size()) * 0.95))];
        s.max = v.back();
        return s;
    }
    static double cpuSeconds() {
#if defined(_WIN32)
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0.0; }
        const auto to = [](const FILETIME& f) { return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
        return to(k) + to(u);
#else
        return 0.0;
#endif
    }
    std::vector<double> tickMs_, workMs_, drawMs_;
    Clock::time_point start_{}, lastTick_{}, lastDraw_{};
    double startCpu_ = 0.0;
    bool haveTick_ = false, haveDraw_ = false, notifier_ = false;
    std::size_t draws_ = 0;
};

}  // namespace foxsdr::slinthost
