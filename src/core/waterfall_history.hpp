// Live waterfall history: a ring of the spectrum lines that have scrolled by, and a VIEW over it that can be paused and scrolled back in time.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). The producer of spectrum lines (the thread that today draws the live waterfall)
// calls push() once per line; the screen asks for a window of lines with copyView() each frame and calls pause(), scrollBack() and jumpToLive() from its
// controls. Today's waterfall keeps only what fits the picture, so a signal that went by is gone; this keeps it.
//
// HOW IT IS KEPT. Every line is `bins` values in dB quantised to ONE BYTE over [dbMin, dbMax] (default -150 .. 0 dB: step 0.588 dB, error at most half a step
// = 0.294 dB, values outside clamp, NaN reads as the floor). Next to each line the ring keeps its timestamp (milliseconds, forced not to go back) and the
// tuner centre and span it was taken at, so a view that crosses a retune can mark it. A line costs bins + 24 bytes.
// MEMORY CAP: capacityLines = min(maxLines, maxBytes / (bins + 24)); the whole ring is allocated in the constructor and never grows, so push() and
// copyView() allocate nothing. Default maxBytes is 192 MiB: at 4096 bins that is 48,865 lines (13.5 minutes at 60 lines a second); at 16384 bins, 12,270 lines.
// A ring that could not hold 16 lines is invalid() and refuses every push. The oldest line is overwritten when the ring is full; the view is told.
//
// THE VIEW. Live: copyView() returns the newest `rows` lines. pause() HOLDS the view where it is (new lines keep being stored, the picture stays);
// scrollBack(n) moves it n lines older and holds it; scrollForward(n) moves it newer and returns to live when it reaches the newest line; jumpToLive()
// returns at once; seekToTime() holds the view at the line nearest a time. The view is anchored to an ABSOLUTE line number, so lines arriving while it is
// held do not move it; when the ring overwrites the lines the view shows, the anchor is slid forward to the oldest stored line and `viewSlid` is set so the
// screen can say "history ran out". behindLines() says how far behind live the held view is.
//
// THREADS: one producer calls push() and reset(); any other threads call the rest. One mutex, held for one line copy (push) or one window copy
// (copyView); nothing waits on I/O. A returned window is a consistent snapshot: contiguous lines, none torn.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cascade::core::wfhistory {

constexpr std::size_t kMinLines = 16;
constexpr std::size_t kLineOverheadBytes = 24;

struct LineMeta {
    std::int64_t timeMs = 0;
    float centerMhz = 0.0f;              // the tuner centre when the line was taken, in MHz (float: 1 Hz resolution up to 8 MHz, ~8 Hz at 100 MHz; a marker, not a measurement)
    float spanKhz = 0.0f;
    std::uint32_t epoch = 0;             // the reset() generation (a changed bin count starts a new epoch)
};

struct Config {
    std::size_t bins = 0;
    std::size_t maxBytes = 192u * 1024u * 1024u;
    std::size_t maxLines = 1u << 22;
    float dbMin = -150.0f;
    float dbMax = 0.0f;
};

struct State {
    bool valid = false;
    std::uint64_t totalPushed = 0;
    std::size_t stored = 0;
    std::uint64_t oldestLine = 0;        // absolute number of the oldest stored line
    std::uint64_t newestLine = 0;        // absolute number of the newest line (valid when stored > 0)
    bool held = false;
    std::uint64_t viewNewest = 0;        // absolute number of the newest line of the view
    std::uint64_t behindLines = 0;
    bool viewSlid = false;               // the ring overwrote lines the held view showed
    std::uint32_t epoch = 0;
};

class WaterfallHistory {
public:
    explicit WaterfallHistory(const Config& cfg) : cfg_(cfg) { allocate(); }

    bool valid() const { return valid_; }
    std::size_t bins() const { return cfg_.bins; }
    std::size_t capacityLines() const { return cap_; }
    std::size_t bytesUsed() const { return cap_ * (cfg_.bins + kLineOverheadBytes); }
    float dbStep() const { return (cfg_.dbMax - cfg_.dbMin) / 255.0f; }

    // ---- producer ----------------------------------------------------------------------------------------------------------------------------

    // One line of `bins` dB values. False when invalid or `line` is null. The timestamp is clamped to be no earlier than the previous line's.
    bool push(const float* line, std::int64_t timeMs, double centerHz, double spanHz) {
        if (!valid_ || line == nullptr) { return false; }
        std::lock_guard<std::mutex> lk(m_);
        const std::size_t slot = static_cast<std::size_t>(total_ % cap_);
        std::uint8_t* dst = data_.data() + slot * cfg_.bins;
        const float lo = cfg_.dbMin, step = dbStep();
        for (std::size_t i = 0; i < cfg_.bins; ++i) {
            const float v = line[i];
            float q = std::isnan(v) ? 0.0f : (v - lo) / step;
            q = std::min(255.0f, std::max(0.0f, q));
            dst[i] = static_cast<std::uint8_t>(q + 0.5f);
        }
        LineMeta& mt = meta_[slot];
        mt.timeMs = (total_ > 0 && timeMs < lastTime_) ? lastTime_ : timeMs;
        mt.centerMhz = static_cast<float>(centerHz / 1.0e6);
        mt.spanKhz = static_cast<float>(spanHz / 1.0e3);
        mt.epoch = epoch_;
        lastTime_ = mt.timeMs;
        ++total_;
        if (held_) { slideIfEvicted(); }
        return true;
    }

    // A new bin count (the FFT size changed) or a new dB range starts an empty history; the old lines have a different shape and cannot be shown with
    // the new ones. The epoch number goes up so a screen can tell.
    bool reset(const Config& cfg) {
        std::lock_guard<std::mutex> lk(m_);
        const std::uint32_t e = epoch_ + 1;
        cfg_ = cfg;
        total_ = 0; held_ = false; viewNewest_ = 0; slid_ = false; lastTime_ = 0;
        allocateLocked();
        epoch_ = e;
        return valid_;
    }

    // ---- the view ----------------------------------------------------------------------------------------------------------------------------

    void pause() {
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || total_ == 0) { held_ = false; return; }
        if (!held_) { held_ = true; viewNewest_ = total_ - 1; slid_ = false; }
    }
    void jumpToLive() { std::lock_guard<std::mutex> lk(m_); held_ = false; slid_ = false; }

    // Moves the view `n` lines older and holds it; clamped so a full window of `rows` lines stays inside the stored history. `rows` is the window height.
    void scrollBack(std::size_t n, std::size_t rows) {
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || total_ == 0) { return; }
        if (!held_) { held_ = true; viewNewest_ = total_ - 1; slid_ = false; }
        const std::uint64_t lowest = lowestAnchor(rows);
        viewNewest_ = viewNewest_ >= lowest + n ? viewNewest_ - n : lowest;
    }
    // Moves the view `n` lines newer; reaching the newest line returns to live.
    void scrollForward(std::size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || !held_) { return; }
        if (viewNewest_ + n >= total_ - 1) { held_ = false; slid_ = false; } else { viewNewest_ += n; }
    }
    // Holds the view with the line nearest `timeMs` as its newest. False when nothing is stored.
    bool seekToTime(std::int64_t timeMs, std::size_t rows) {
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || total_ == 0) { return false; }
        std::uint64_t lo = oldest(), hi = total_ - 1;
        while (lo < hi) {                                                  // first line with time >= timeMs (times never go back)
            const std::uint64_t mid = lo + (hi - lo) / 2;
            if (meta_[static_cast<std::size_t>(mid % cap_)].timeMs < timeMs) { lo = mid + 1; } else { hi = mid; }
        }
        std::uint64_t best = lo;
        if (lo > oldest()) {
            const std::int64_t a = meta_[static_cast<std::size_t>(lo % cap_)].timeMs - timeMs, b = timeMs - meta_[static_cast<std::size_t>((lo - 1) % cap_)].timeMs;
            if (b < a) { best = lo - 1; }
        }
        held_ = true; slid_ = false;
        viewNewest_ = std::max<std::uint64_t>(best, lowestAnchor(rows));
        if (viewNewest_ >= total_ - 1) { held_ = false; }
        return true;
    }

    // Copies the window of up to `rows` lines ending at the view's newest line, OLDEST FIRST, as quantised bytes (`rows * bins`) and metadata (`rows`, may be
    // null). Returns how many lines were written (fewer than `rows` while the history is short; they fill from index 0).
    std::size_t copyViewU8(std::size_t rows, std::uint8_t* dst, LineMeta* meta = nullptr) const {
        if (dst == nullptr || rows == 0) { return 0; }
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || total_ == 0) { return 0; }
        const std::uint64_t newest = held_ ? std::min<std::uint64_t>(viewNewest_, total_ - 1) : total_ - 1;
        const std::uint64_t avail = newest + 1 - oldest();
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(rows, avail));
        const std::uint64_t first = newest + 1 - n;
        for (std::size_t r = 0; r < n; ++r) {
            const std::size_t slot = static_cast<std::size_t>((first + r) % cap_);
            std::memcpy(dst + r * cfg_.bins, data_.data() + slot * cfg_.bins, cfg_.bins);
            if (meta != nullptr) { meta[r] = meta_[slot]; }
        }
        return n;
    }

    // The same window as dB floats (each value is the quantised value, within half a step of what was pushed).
    std::size_t copyView(std::size_t rows, float* dst, LineMeta* meta = nullptr) const {
        if (dst == nullptr || rows == 0) { return 0; }
        std::lock_guard<std::mutex> lk(m_);
        if (!valid_ || total_ == 0) { return 0; }
        const std::uint64_t newest = held_ ? std::min<std::uint64_t>(viewNewest_, total_ - 1) : total_ - 1;
        const std::uint64_t avail = newest + 1 - oldest();
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(rows, avail));
        const std::uint64_t first = newest + 1 - n;
        const float lo = cfg_.dbMin, step = dbStep();
        for (std::size_t r = 0; r < n; ++r) {
            const std::size_t slot = static_cast<std::size_t>((first + r) % cap_);
            const std::uint8_t* src = data_.data() + slot * cfg_.bins;
            float* out = dst + r * cfg_.bins;
            for (std::size_t i = 0; i < cfg_.bins; ++i) { out[i] = lo + static_cast<float>(src[i]) * step; }
            if (meta != nullptr) { meta[r] = meta_[slot]; }
        }
        return n;
    }

    State state() const {
        std::lock_guard<std::mutex> lk(m_);
        State s;
        s.valid = valid_;
        s.totalPushed = total_;
        s.stored = static_cast<std::size_t>(total_ - oldestLocked());
        s.oldestLine = oldestLocked();
        s.newestLine = total_ > 0 ? total_ - 1 : 0;
        s.held = held_;
        s.viewNewest = held_ ? std::min<std::uint64_t>(viewNewest_, total_ > 0 ? total_ - 1 : 0) : s.newestLine;
        s.behindLines = total_ > 0 ? s.newestLine - s.viewNewest : 0;
        s.viewSlid = slid_;
        s.epoch = epoch_;
        return s;
    }

private:
    void allocate() { std::lock_guard<std::mutex> lk(m_); allocateLocked(); }

    void allocateLocked() {
        valid_ = false;
        data_.clear(); data_.shrink_to_fit(); meta_.clear(); meta_.shrink_to_fit();
        cap_ = 0;
        if (cfg_.bins == 0 || cfg_.bins > (1u << 20) || !(cfg_.dbMax > cfg_.dbMin) || !std::isfinite(cfg_.dbMin) || !std::isfinite(cfg_.dbMax)) { return; }
        const std::size_t perLine = cfg_.bins + kLineOverheadBytes;
        std::size_t lines = std::min<std::size_t>(cfg_.maxLines, cfg_.maxBytes / perLine);
        if (lines < kMinLines) { return; }
        cap_ = lines;
        data_.assign(cap_ * cfg_.bins, 0);
        meta_.assign(cap_, LineMeta());
        valid_ = true;
    }

    std::uint64_t oldestLocked() const { return total_ > cap_ ? total_ - cap_ : 0; }
    std::uint64_t oldest() const { return oldestLocked(); }

    // The lowest anchor that still shows `rows` lines (or the oldest line + rows - 1; for a short history, the newest line).
    std::uint64_t lowestAnchor(std::size_t rows) const {
        const std::uint64_t o = oldest();
        const std::uint64_t want = o + (rows > 0 ? rows - 1 : 0);
        return std::min<std::uint64_t>(want, total_ - 1);
    }

    void slideIfEvicted() {
        const std::uint64_t o = oldest();
        if (viewNewest_ < o) { viewNewest_ = o; slid_ = true; }
    }

    Config cfg_;
    mutable std::mutex m_;
    bool valid_ = false;
    std::size_t cap_ = 0;
    std::vector<std::uint8_t> data_;
    std::vector<LineMeta> meta_;
    std::uint64_t total_ = 0;
    std::int64_t lastTime_ = 0;
    bool held_ = false;
    std::uint64_t viewNewest_ = 0;
    bool slid_ = false;
    std::uint32_t epoch_ = 0;
};

}  // namespace cascade::core::wfhistory
