// See frame_timing.hpp. The hot path - a scope's two clock reads - is inline in
// the header; everything that runs only for a frame that was already slow, or
// only once, or only in a test, is here. That is a cost in its own right:
// src/gui/app_window.cpp is one translation unit with more sections than the
// object format allows before /bigobj, and every inline function and template
// instance the header put in it counted against that.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/frame_timing.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cascade::core {

std::int64_t frameSteadyNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t frameAwakeNanos() {
#if defined(_WIN32)
    // The time the machine spent AWAKE, which is what the unbiased interrupt
    // time is by definition ("does not include time spent in sleep or
    // hibernation"). steady_clock on this platform is QueryPerformanceCounter,
    // which DOES count a suspend: read side by side on a Windows 11 desktop the
    // two differed by exactly the time it had spent asleep since boot. So a
    // frame that straddled a suspend measures the suspend on the first clock and
    // not on this one, and the difference is how it is told from a slow frame.
    ULONGLONG t100ns = 0;
    if (!::QueryUnbiasedInterruptTime(&t100ns)) { return frameSteadyNanos(); }
    return static_cast<std::int64_t>(t100ns) * 100;
#else
    // CLOCK_MONOTONIC, which steady_clock is on Linux, does not run while the
    // machine is suspended: the two clocks cannot disagree.
    return frameSteadyNanos();
#endif
}

bool frameScopeFromName(const char* name, FrameScope& out) {
    if (name == nullptr) { return false; }
    for (int i = 0; i < kFrameScopeCount; ++i) {
        if (std::strcmp(name, kFrameScopeNames[i]) == 0) {
            out = static_cast<FrameScope>(i);
            return true;
        }
    }
    return false;
}

std::string slowFramesText(const SlowFrameCounts& c) {
    std::string out;
    for (int s = 0; s < kFrameScopeCount; ++s) {
        bool any = false;
        for (int t = 0; t < kSlowFrameTiers; ++t) { any = any || c.count[s][t] != 0; }
        if (!any) { continue; }
        if (!out.empty()) { out += ", "; }
        out += kFrameScopeNames[s];
        for (int t = 0; t < kSlowFrameTiers; ++t) {
            out += t == 0 ? " " : "/";
            out += std::to_string(c.count[s][t]);
        }
    }
    return out.empty() ? std::string("none") : out;
}

const std::vector<std::string>& frameBundleFieldNames() {
    static const std::vector<std::string> names = {"slow-frames"};
    return names;
}

std::string withSlowFramesField(std::string bundle, const std::string& value) {
    const std::string line = "slow-frames: " + (value.empty() ? std::string("none") : value) + "\n";
    const std::size_t at = bundle.find("\n--- log ---\n");
    if (at == std::string::npos) {
        bundle += line;
    } else {
        bundle.insert(at, line);
    }
    return bundle;
}

namespace {

// A whole non-negative number and nothing else ("45", not "45x", not "").
bool wholeNumber(const std::string& s, long& out) {
    if (s.empty() || s.size() > 9) { return false; }
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

}  // namespace

std::vector<FrameStall> parseFrameStalls(const char* text) {
    std::vector<FrameStall> out;
    if (text == nullptr) { return out; }
    const std::string all(text);
    std::size_t pos = 0;
    while (pos <= all.size() && static_cast<int>(out.size()) < kMaxFrameStalls) {
        std::size_t comma = all.find(',', pos);
        if (comma == std::string::npos) { comma = all.size(); }
        const std::string item = all.substr(pos, comma - pos);
        pos = comma + 1;
        const std::size_t eq = item.find('=');
        const std::size_t at = item.find('@');
        if (eq == std::string::npos || at == std::string::npos || at < eq) { continue; }
        FrameStall st;
        if (!frameScopeFromName(item.substr(0, eq).c_str(), st.scope)) { continue; }
        long ms = 0;
        if (!wholeNumber(item.substr(eq + 1, at - eq - 1), ms) ||
            !wholeNumber(item.substr(at + 1), st.frame)) {
            continue;
        }
        st.ms = static_cast<int>(ms);
        if (ms < 1 || ms > 5000) { continue; }
        out.push_back(st);
    }
    return out;
}

FrameSituations parseFrameSituations(const char* text) {
    FrameSituations out;
    if (text == nullptr) { return out; }
    const std::string all(text);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        std::size_t comma = all.find(',', pos);
        if (comma == std::string::npos) { comma = all.size(); }
        const std::string item = all.substr(pos, comma - pos);
        pos = comma + 1;
        const std::size_t at = item.find('@');
        if (at == std::string::npos) { continue; }
        const std::string what = item.substr(0, at);
        const std::string arg = item.substr(at + 1);
        long a = 0;
        long b = 0;
        if (what == "hidden") {
            const std::size_t dash = arg.find('-');
            if (dash == std::string::npos || !wholeNumber(arg.substr(0, dash), a) ||
                !wholeNumber(arg.substr(dash + 1), b)) {
                continue;
            }
            out.hiddenFirst = a;
            out.hiddenLast = b;
        } else if (what == "display") {
            if (wholeNumber(arg, a)) { out.displayAt = a; }
        } else if (what == "modal") {
            if (wholeNumber(arg, a)) { out.modalAt = a; }
        }
    }
    return out;
}

// --- the timer ------------------------------------------------------------------

void FrameTimer::finish() {
    settlePrevious(false);
    if (recorded_ == 0) { return; }
    const std::string line = "frame: slow frames this session - " + slowFramesText(counts()) +
                             " (250 ms to 1 s / 1 s to 5 s / 5 s or more, by scope)";
    emit(true, line.c_str());
}

SlowFrameCounts FrameTimer::counts() const {
    SlowFrameCounts c;
    for (int s = 0; s < kFrameScopeCount; ++s) {
        for (int t = 0; t < kSlowFrameTiers; ++t) {
            c.count[s][t] = table_[s][t].load(std::memory_order_relaxed);
        }
    }
    return c;
}

std::string FrameTimer::summaryText() const {
    // The table is as long as it has entries - every scope can have one - so it is
    // appended to the figures rather than formatted into a fixed buffer with them.
    char head[160];
    const double n = frames_ > 0 ? static_cast<double>(frames_) : 1.0;
    std::snprintf(head, sizeof(head),
                  "%llu frames, mean %.2f ms, longest %.1f ms (in %s), %.1f scope switches a "
                  "frame; slow frames: ",
                  static_cast<unsigned long long>(frames_),
                  static_cast<double>(sumCountedNs_) / n / 1e6,
                  static_cast<double>(maxCountedNs_) / 1e6, frameScopeName(maxScope_),
                  static_cast<double>(switches_) / n);
    std::string out = head;
    out += slowFramesText(counts());
    out += "; slow frames not counted: ";
    out += std::to_string(static_cast<unsigned long long>(excludedSlow_));
    return out;
}

void FrameTimer::setStalls(const std::vector<FrameStall>& stalls) {
    stallCount_ = 0;
    for (const FrameStall& s : stalls) {
        if (stallCount_ < kMaxFrameStalls) { stalls_[stallCount_++] = s; }
    }
}

void FrameTimer::resetForTest() {
    for (auto& row : table_) {
        for (auto& c : row) { c.store(0, std::memory_order_relaxed); }
    }
    for (int i = 0; i < kFrameScopeCount; ++i) {
        acc_[i] = 0;
        lastLogNs_[i] = 0;
        logged_[i] = false;
        suppressed_[i] = 0;
    }
    steady_ = &frameSteadyNanos;
    awake_ = &frameAwakeNanos;
    sink_ = nullptr;
    stallCount_ = 0;
    held_ = Held{};
    cur_ = FrameScope::Other;
    current_.store(static_cast<std::uint8_t>(FrameScope::Other), std::memory_order_relaxed);
    frames_ = 0;
    switches_ = 0;
    sumCountedNs_ = 0;
    maxCountedNs_ = 0;
    maxScope_ = FrameScope::Other;
    recorded_ = 0;
    excludedSlow_ = 0;
    excluded_ = false;
    userDepth_ = 0;
}

FrameScope FrameTimer::dominant(std::int64_t* ns) const {
    int best = idx(FrameScope::Other);
    std::int64_t bestNs = -1;
    for (int s = 0; s < kFrameScopeCount; ++s) {
        if (s == idx(FrameScope::UserWait) || s == idx(FrameScope::Startup)) { continue; }
        if (acc_[s] > bestNs) {
            bestNs = acc_[s];
            best = s;
        }
    }
    if (ns != nullptr) { *ns = bestNs < 0 ? 0 : bestNs; }
    return static_cast<FrameScope>(best);
}

void FrameTimer::slowFrame(std::int64_t total, std::int64_t counted) {
    // A frame that was inside a display grace or a hidden window: slow, and not
    // ours.
    if (excluded_) {
        ++excludedSlow_;
        return;
    }
    // A frame that spanned a suspend: the steady clock ran on while the machine was
    // asleep and the awake clock did not.
    const std::int64_t awakeNs = awake_() - awakeStart_;
    if (total - awakeNs > kSuspendSlackNs) {
        ++excludedSlow_;
        return;
    }
    Held h;
    h.valid = true;
    h.countedNs = counted;
    h.tier = slowFrameTier(counted);
    h.scope = dominant(&h.scopeNs);
    if (frameIndex_ < kStartupFrames) {
        h.part = h.scope;
        h.partNs = h.scopeNs;
        h.scope = FrameScope::Startup;
        h.scopeNs = counted;
    }
    // A previous frame still held means settlePrevious() was not called: count it
    // rather than lose it.
    if (held_.valid) { commit(held_); }
    held_ = h;
}

namespace {

long long ms(std::int64_t ns) { return static_cast<long long>((ns + 500'000) / 1'000'000); }

}  // namespace

void FrameTimer::commit(const Held& h) {
    const int si = idx(h.scope);
    table_[si][h.tier].fetch_add(1, std::memory_order_relaxed);
    ++recorded_;
    // THE USAGE RECORD (0.99.64, core/health_events.hpp): one count in `slow.<scope>.
    // <tier>`, for the PROCESS's timer only - a timer a test builds is not the
    // window's and counts into its own table and nothing else. On the slow path
    // only (a frame already 250 ms or more late), on the window's thread, and it
    // WAITS ON NO DISK: the ledger takes a mutex that is never held across I/O,
    // updates a small map, and hands the file to a thread of its own.
    if (mirror_) { health::noteSlowFrame(si, h.tier); }
    const std::int64_t now = steady_();
    if (logged_[si] && now - lastLogNs_[si] < kLogGapNs) {
        ++suppressed_[si];
        return;
    }
    char line[kLineChars];
    int n = std::snprintf(line, sizeof(line), "frame: %lld ms, %lld ms of it in %s",
                          ms(h.countedNs), ms(h.scopeNs), frameScopeName(h.scope));
    if (h.scope == FrameScope::Startup && n > 0 && n < static_cast<int>(sizeof(line))) {
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n),
                           "; largest part: %s %lld ms", frameScopeName(h.part), ms(h.partNs));
    }
    if (suppressed_[si] > 0 && n > 0 && n < static_cast<int>(sizeof(line))) {
        std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n),
                      "; %u more slow frames in it not logged", suppressed_[si]);
    }
    logged_[si] = true;
    lastLogNs_[si] = now;
    suppressed_[si] = 0;
    emit(h.tier >= 1, line);
}

void FrameTimer::emit(bool warn, const char* line) const {
    if (sink_ != nullptr) {
        sink_(warn, line);
    } else if (warn) {
        diagWarnf("%s", line);
    } else {
        diagLogf("%s", line);
    }
}

void FrameTimer::runStall(FrameScope s) {
    for (int i = 0; i < stallCount_; ++i) {
        FrameStall& st = stalls_[i];
        if (st.scope == s && st.frame == frameIndex_ && st.ms > 0) {
            const int hold = st.ms;
            st.ms = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        }
    }
}

}  // namespace cascade::core
