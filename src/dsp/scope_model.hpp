// scope_model.hpp - the SCOPE NODE as a model: a ring of the last samples, a TRIGGER (edge, level, hysteresis, hold-off, pre-trigger), three modes
// (Auto, Normal, Single) and a DECIMATED frame (min and max per display column, so a narrow spike is never averaged away). Not wired (the node kind
// and the runner are forbidden files for this lane).
//
// WHAT EXISTS: gui/demod_scope.hpp has a trigger for the DEMOD scope (a display-side helper that looks for an edge in a finished block and draws from
// it). It lives in cascade_gui and works on a block already in hand; a node on the patch canvas has a STREAM, not a block, so this is the streaming
// version in the engine library: it decides, sample by sample, when a frame starts and when it is complete, without allocating. The two do not share
// code and do not need to.
//
// THE RULES (each tested):
//   - INPUT: real samples (push) or complex ones reduced to a real trace (pushIq with I, Q, Magnitude or Phase). A non-finite sample reads as 0.
//   - FRAME: `window` samples (the time base), `pre` of them before the trigger sample (preFraction 0..1 of the window, default 0.25).
//   - TRIGGER: a sample v[i] with v[i-1] < level <= v[i] (Rising) or v[i-1] > level >= v[i] (Falling), armed by having first been at least `hysteresis`
//     on the other side of the level (so noise around the level does not re-fire), and no sooner than `holdoff` samples after the previous trigger.
//   - AUTO: if no trigger arrives for autoTimeoutSamples (default 2 x window) after the last frame was taken or the scope was armed, a free-running
//     frame ending at the newest sample is delivered (flagged `triggered = false`) so a flat or untriggerable trace still draws. NORMAL waits for a
//     trigger and keeps showing the last frame. SINGLE delivers one frame and stops until arm().
//   - A frame is complete when `window - pre` samples have arrived after the trigger sample. Triggers that arrive while a frame is still filling are
//     ignored (the next frame is looked for after it is taken).
//   - DECIMATION: frame(columns, min, max): each column is the min and max of the window samples that fall in it (window / columns of them); with
//     fewer samples than columns each column repeats the nearest sample. A spike of one sample shows in its column's max at any decimation.
//   - The ring holds `capacity` samples (a power of two, at most kCapacity); window is clamped to half of it. A delivered frame is COPIED out of
//     the ring when it completes, so it stays valid however long the stream runs on. No allocation in push(); set-up (configure) allocates. One thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::dsp {

enum class ScopeMode : int { Auto = 0, Normal = 1, Single = 2 };
enum class ScopeEdge : int { Rising = 0, Falling = 1 };
enum class ScopeTrace : int { I = 0, Q = 1, Magnitude = 2, Phase = 3 };

struct ScopeSettings {
    ScopeMode mode = ScopeMode::Auto;
    ScopeEdge edge = ScopeEdge::Rising;
    double level = 0.0;
    double hysteresis = 0.01;
    std::size_t holdoff = 0;               // samples
    double preFraction = 0.25;
    std::size_t window = 1024;             // samples in a frame
    std::size_t autoTimeout = 0;           // 0 = 2 x window
};

class ScopeModel {
public:
    static constexpr std::size_t kCapacity = 1u << 20;

    explicit ScopeModel(std::size_t capacity = 1u << 16) { configure(capacity); }

    // Allocates the ring (a power of two from 256 up to kCapacity; anything else is rounded up). Clears everything.
    void configure(std::size_t capacity) {
        std::size_t c = 256;
        while (c < capacity && c < kCapacity) { c <<= 1; }
        ring_.assign(c, 0.0f);
        frame_.assign(c / 2, 0.0f);
        mask_ = c - 1;
        clear();
        setSettings(s_);
    }
    void setSettings(const ScopeSettings& in) {
        ScopeSettings n = in;
        n.window = std::min(std::max<std::size_t>(n.window, 2), ring_.size() / 2);
        n.preFraction = std::isfinite(n.preFraction) ? std::min(std::max(n.preFraction, 0.0), 1.0) : 0.25;
        n.hysteresis = std::isfinite(n.hysteresis) ? std::max(n.hysteresis, 0.0) : 0.01;
        if (!std::isfinite(n.level)) { n.level = s_.level; }
        s_ = n;
        pre_ = static_cast<std::size_t>(std::lround(static_cast<double>(s_.window) * s_.preFraction));
        pre_ = std::min(pre_, s_.window - 1);
        timeout_ = s_.autoTimeout != 0 ? s_.autoTimeout : 2 * s_.window;
    }
    const ScopeSettings& settings() const { return s_; }

    void clear() {
        total_ = 0;
        armedSide_ = false;
        sinceTrigger_ = static_cast<std::size_t>(-1) / 2;
        waiting_ = false;
        ready_ = false;
        done_ = false;
        sinceFrame_ = 0;
        frameCount_ = 0;
        prev_ = 0.0f;
        havePrev_ = false;
    }
    // Single mode: look for the next frame. Also clears a frame that was never read.
    void arm() {
        done_ = false;
        ready_ = false;
        waiting_ = false;
        sinceFrame_ = 0;
        armedSide_ = false;
    }

    bool frameReady() const { return ready_; }
    // The frame's trigger sample was real (false for an Auto free-run frame).
    bool frameTriggered() const { return frameTriggered_; }
    std::uint64_t frameCount() const { return frameCount_; }

    void push(const float* x, std::size_t n) {
        if (x == nullptr) { return; }
        for (std::size_t i = 0; i < n; ++i) { one(std::isfinite(x[i]) ? x[i] : 0.0f); }
    }
    void pushIq(const std::complex<float>* x, std::size_t n, ScopeTrace t) {
        if (x == nullptr) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            const float re = std::isfinite(x[i].real()) ? x[i].real() : 0.0f, im = std::isfinite(x[i].imag()) ? x[i].imag() : 0.0f;
            float v = 0.0f;
            switch (t) {
                case ScopeTrace::I: v = re; break;
                case ScopeTrace::Q: v = im; break;
                case ScopeTrace::Magnitude: v = std::sqrt(re * re + im * im); break;
                case ScopeTrace::Phase: v = std::atan2(im, re); break;
            }
            one(v);
        }
    }

    // The newest complete frame, decimated to `columns` columns (min and max each). Returns false when no frame is ready. The frame stays readable
    // (Normal keeps showing it) until a newer one replaces it; reading does not consume it.
    bool frame(std::size_t columns, float* minOut, float* maxOut) const {
        if (!hasFrame_ || columns == 0 || minOut == nullptr || maxOut == nullptr) { return false; }
        const std::size_t len = frameLen_;
        for (std::size_t c = 0; c < columns; ++c) {
            std::size_t a = c * len / columns, b = (c + 1) * len / columns;
            if (b <= a) { b = a + 1; }  // fewer samples than columns: the nearest sample
            if (a >= len) { a = len - 1; b = len; }
            float lo = frame_[a], hi = lo;
            for (std::size_t k = a + 1; k < b && k < len; ++k) {
                const float v = frame_[k];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            minOut[c] = lo;
            maxOut[c] = hi;
        }
        return true;
    }
    // Sample j (0 .. frameLength()-1) of the frame as delivered; for the tests and for a raw view.
    bool frameSample(std::size_t j, float* v) const {
        if (!hasFrame_ || j >= frameLen_) { return false; }
        *v = frame_[j];
        return true;
    }
    std::size_t frameLength() const { return hasFrame_ ? frameLen_ : 0; }
    // Index of the trigger sample inside the frame (the pre-trigger count) for a triggered frame.
    std::size_t frameTriggerIndex() const { return frameTrigIdx_; }
    // Marks the newest frame as seen (the node's drawing code calls this so frameReady() means "something new").
    void acknowledge() { ready_ = false; }

private:
    float at(std::uint64_t absIndex) const { return ring_[absIndex & mask_]; }

    void one(float v) {
        ring_[total_ & mask_] = v;
        const std::uint64_t idx = total_;
        ++total_;
        if (sinceTrigger_ < static_cast<std::size_t>(-1) / 2) { ++sinceTrigger_; }
        ++sinceFrame_;
        if (!done_) {
            if (waiting_) {
                if (total_ >= trigAbs_ + 1 + (s_.window - pre_ - 1)) { completeFrame(trigAbs_ - pre_, true); }
            } else {
                // the hysteresis arming: be at least `hysteresis` on the far side of the level before an edge counts
                const double lv = s_.level, h = s_.hysteresis;
                if (s_.edge == ScopeEdge::Rising) { if (v <= lv - h) { armedSide_ = true; } }
                else if (v >= lv + h) { armedSide_ = true; }
                bool crossed = false;
                if (havePrev_) {
                    crossed = s_.edge == ScopeEdge::Rising ? (prev_ < lv && v >= lv) : (prev_ > lv && v <= lv);
                }
                if (crossed && armedSide_ && sinceTrigger_ >= s_.holdoff && idx >= pre_) {
                    armedSide_ = false;
                    sinceTrigger_ = 0;
                    trigAbs_ = idx;
                    waiting_ = true;
                    if (s_.window - pre_ - 1 == 0) { completeFrame(trigAbs_ - pre_, true); }
                } else if (s_.mode == ScopeMode::Auto && sinceFrame_ >= timeout_ && total_ >= s_.window) {
                    completeFrame(total_ - s_.window, false);
                }
            }
        }
        prev_ = v;
        havePrev_ = true;
    }

    void completeFrame(std::uint64_t start, bool triggered) {
        frameStart_ = start;
        frameLen_ = s_.window;
        for (std::size_t k = 0; k < frameLen_; ++k) { frame_[k] = at(start + k); }  // copied out: the ring goes on turning
        frameTrigIdx_ = triggered ? pre_ : 0;
        frameTriggered_ = triggered;
        hasFrame_ = true;
        ready_ = true;
        waiting_ = false;
        sinceFrame_ = 0;
        ++frameCount_;
        if (s_.mode == ScopeMode::Single) { done_ = true; }
    }

    ScopeSettings s_{};
    std::vector<float> ring_, frame_;
    std::uint64_t mask_ = 0;
    std::uint64_t total_ = 0;
    std::size_t pre_ = 0, timeout_ = 0;
    bool armedSide_ = false, waiting_ = false, ready_ = false, done_ = false, hasFrame_ = false, frameTriggered_ = false, havePrev_ = false;
    std::size_t sinceTrigger_ = 0, sinceFrame_ = 0;
    std::uint64_t trigAbs_ = 0, frameStart_ = 0, frameCount_ = 0;
    std::size_t frameLen_ = 0, frameTrigIdx_ = 0;
    float prev_ = 0.0f;
};

}  // namespace cascade::dsp
