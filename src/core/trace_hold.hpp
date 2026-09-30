// trace_hold.hpp - the spectrum's PEAK HOLD and AVERAGE traces (a user's
// request, 2026-09-30, beside the waterfall markers): right-click the
// spectrum, choose Normal, Peak hold or Average; the held trace is drawn in
// bold over the live one, can be started over, and the average has a length
// in milliseconds.
//
// PURE - no ImGui, no clock of its own - so tests/test_trace_hold.cpp pins it.
//
// THE RULES, each tested:
//   - PEAK is the largest value each bin has shown since the trace was last
//     started, held until Reset: a burst seen once stays drawn.
//   - AVERAGE is a true sliding window of the chosen length, not an
//     exponential blend: every frame that arrived in the last N ms counts
//     equally and nothing older counts at all, which is what "the length of
//     the average" means. It is taken over POWER (each dB value turned back
//     into linear power, averaged, and converted to dB again): averaging the
//     decibels themselves reads a noise floor about 2.5 dB low.
//   - A HOLD BELONGS TO ONE AXIS. The caller passes a key built from what
//     decides which frequency a bin is (centre, sample rate, bin count); when
//     it changes the trace starts again, because a peak held at the old
//     frequency drawn at the new one is a picture of nothing on the air.
//   - Choosing another mode starts the trace again; changing the average's
//     length does not (the window simply takes more or fewer frames).
//   - A frame whose bin count differs is a new axis too. A value that is not
//     finite counts as the floor, never as a peak.
//   - The window holds at most kTraceHoldMaxFrames frames: a fast pipeline on
//     a 10 s average must not grow memory without end. heldSeconds() says how
//     much time is actually in it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TRACE_HOLD_HPP
#define CASCADE_CORE_TRACE_HOLD_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace cascade::core {

enum class TraceMode : int { Normal = 0, Peak = 1, Average = 2 };

inline constexpr double kTraceAverageMinMs = 50.0;
inline constexpr double kTraceAverageMaxMs = 10000.0;
inline constexpr double kTraceAverageDefaultMs = 1000.0;
inline constexpr std::size_t kTraceHoldMaxFrames = 1200;
// What a non-finite bin counts as: well under any real noise floor.
inline constexpr float kTraceFloorDb = -200.0f;

// The config's spelling of a mode, and back; anything unknown is Normal.
inline const char* traceModeName(TraceMode m) {
    switch (m) {
        case TraceMode::Peak: return "peak";
        case TraceMode::Average: return "average";
        default: return "normal";
    }
}
inline TraceMode traceModeFromName(const std::string& s) {
    if (s == "peak") { return TraceMode::Peak; }
    if (s == "average") { return TraceMode::Average; }
    return TraceMode::Normal;
}

inline double clampTraceAverageMs(double ms) {
    if (!std::isfinite(ms)) { return kTraceAverageDefaultMs; }
    return std::clamp(ms, kTraceAverageMinMs, kTraceAverageMaxMs);
}

class TraceHold {
public:
    TraceMode mode() const { return mode_; }
    double averageMs() const { return averageMs_; }

    void setMode(TraceMode m) {
        if (m == mode_) { return; }
        mode_ = m;
        reset();
    }

    void setAverageMs(double ms) {
        averageMs_ = clampTraceAverageMs(ms);
        trimWindow(lastT_);
        rebuildAverage();
    }

    // Starts the trace again: nothing held, nothing averaged.
    void reset() {
        window_.clear();
        sum_.clear();
        trace_.clear();
        n_ = 0;
        frames_ = 0;
        pushesSinceRebuild_ = 0;
    }

    // One spectrum frame of `n` dB values that arrived at `nowS` seconds on
    // the caller's monotonic clock, on the axis `key`. Normal mode keeps
    // nothing.
    void push(const float* db, int n, double nowS, std::uint64_t key) {
        if (mode_ == TraceMode::Normal || db == nullptr || n <= 0) { return; }
        if (key != key_ || static_cast<std::size_t>(n) != n_) {
            reset();
            key_ = key;
            n_ = static_cast<std::size_t>(n);
        }
        if (std::isfinite(nowS)) { lastT_ = nowS; }
        ++frames_;
        if (mode_ == TraceMode::Peak) {
            if (trace_.size() != n_) { trace_.assign(n_, kTraceFloorDb); }
            for (std::size_t i = 0; i < n_; ++i) {
                const float v = std::isfinite(db[i]) ? db[i] : kTraceFloorDb;
                if (v > trace_[i]) { trace_[i] = v; }
            }
            return;
        }
        // Average: the frame in linear power, into the window and the sum.
        Frame f;
        f.t = lastT_;
        f.power.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            const float v = std::isfinite(db[i]) ? db[i] : kTraceFloorDb;
            f.power[i] = std::pow(10.0, static_cast<double>(v) / 10.0);
        }
        if (sum_.size() != n_) { sum_.assign(n_, 0.0); }
        for (std::size_t i = 0; i < n_; ++i) { sum_[i] += f.power[i]; }
        window_.push_back(std::move(f));
        while (window_.size() > kTraceHoldMaxFrames) { popOldest(); }
        trimWindow(lastT_);
        // The running sum loses a little to rounding with every frame taken
        // out of it; rebuilt from the window now and then so it never drifts.
        if (++pushesSinceRebuild_ >= 256) {
            rebuildSum();
        }
        rebuildAverage();
    }

    // The trace to draw in bold, n values in dB - empty in Normal mode or
    // before a frame has arrived.
    const std::vector<float>& trace() const { return trace_; }

    // Frames that went into it: since the last start (peak), or in the
    // window (average).
    std::size_t frames() const { return mode_ == TraceMode::Average ? window_.size() : frames_; }

    // The time the average actually covers, oldest frame to newest; 0 for a
    // single frame or in another mode.
    double heldSeconds() const {
        if (mode_ != TraceMode::Average || window_.size() < 2) { return 0.0; }
        return window_.back().t - window_.front().t;
    }

private:
    struct Frame {
        double t = 0.0;
        std::vector<double> power;
    };

    void popOldest() {
        const Frame& old = window_.front();
        for (std::size_t i = 0; i < sum_.size() && i < old.power.size(); ++i) { sum_[i] -= old.power[i]; }
        window_.pop_front();
    }

    // Frames older than the window's length leave it; the newest always stays.
    void trimWindow(double nowS) {
        const double span = averageMs_ / 1000.0;
        while (window_.size() > 1 && nowS - window_.front().t > span) { popOldest(); }
    }

    void rebuildSum() {
        sum_.assign(n_, 0.0);
        for (const Frame& f : window_) {
            for (std::size_t i = 0; i < n_ && i < f.power.size(); ++i) { sum_[i] += f.power[i]; }
        }
        pushesSinceRebuild_ = 0;
    }

    void rebuildAverage() {
        if (mode_ != TraceMode::Average || window_.empty() || sum_.size() != n_) {
            if (mode_ == TraceMode::Average) { trace_.clear(); }
            return;
        }
        trace_.resize(n_);
        const double count = static_cast<double>(window_.size());
        for (std::size_t i = 0; i < n_; ++i) {
            const double mean = std::max(sum_[i] / count, 1e-30);
            trace_[i] = static_cast<float>(10.0 * std::log10(mean));
        }
    }

    TraceMode mode_ = TraceMode::Normal;
    double averageMs_ = kTraceAverageDefaultMs;
    std::uint64_t key_ = 0;
    std::size_t n_ = 0;
    std::size_t frames_ = 0;
    double lastT_ = 0.0;
    int pushesSinceRebuild_ = 0;
    std::deque<Frame> window_;
    std::vector<double> sum_;
    std::vector<float> trace_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TRACE_HOLD_HPP
