// Bank of up to ten manual notches for audio (default 48 kHz): each has a frequency, a width (the -3 dB bandwidth in Hz) and an
// on/off switch.
//
// REUSES dsp::Notch (dsp/notch.hpp), the existing textbook biquad band-stop with exact zeros on the unit circle; the bank adds
// width -> Q (Q = f / width), a series of ten, glide-and-fade so that a notch can be moved, switched on or switched off while the audio
// runs without a click, and a single-load "nothing is on" early-out.
//
// CLICK AVOIDANCE. A biquad whose coefficients jump keeps its old state and rings. Here the frequency and width glide towards their
// targets (3 ms time constant, updated every 16 samples of the STREAM, so any chunking of the input gives the same output), and an
// on/off change is a 10 ms linear cross-fade between the notch input and output (a notch that is switched on starts from a cleared state
// and faded in; one that is switched off keeps running until the fade reaches zero, then is cleared and skipped).
//
// THREADS. setNotch() / clear() from any one control thread (atomics; frequency and width are read separately, so a pair changed together
// may be seen one block apart, which the glide hides). process() on the DSP thread only: allocation-free, a no-op (and bit-exact
// passthrough) while no slot is on and none is fading out.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "dsp/notch.hpp"

namespace cascade::dsp {

class NotchBank {
public:
    static constexpr int kMaxNotches = 10;
    static constexpr double kMinWidthHz = 1.0;
    static constexpr double kMaxWidthHz = 2000.0;
    static constexpr double kMinFreqHz = 20.0;

    explicit NotchBank(double sampleRateHz = 48000.0)
        : fs_(sampleRateHz >= 8000.0 ? sampleRateHz : 48000.0),
          glide_(1.0 - std::exp(-static_cast<double>(kBlock) / (fs_ * 0.003))),
          fadeStep_(1.0 / (fs_ * 0.010)) {
        for (auto& s : slot_) {
            s.freq.store(1000.0);
            s.width.store(50.0);
            s.on.store(false);
        }
    }

    double sampleRateHz() const { return fs_; }
    double maxFreqHz() const { return 0.45 * fs_; }

    // Any thread (one writer). `index` 0..9; false (nothing changed) for a bad index or a non-finite frequency or width. Frequency is
    // clamped to [20 Hz, 0.45 fs], width to [1, 2000] Hz.
    bool setNotch(int index, double freqHz, double widthHz, bool on) {
        if (index < 0 || index >= kMaxNotches || !std::isfinite(freqHz) || !std::isfinite(widthHz)) { return false; }
        Slot& s = slot_[static_cast<std::size_t>(index)];
        s.freq.store(std::clamp(freqHz, kMinFreqHz, maxFreqHz()), std::memory_order_relaxed);
        s.width.store(std::clamp(widthHz, kMinWidthHz, kMaxWidthHz), std::memory_order_relaxed);
        setOn(index, on);
        return true;
    }
    bool setOn(int index, bool on) {
        if (index < 0 || index >= kMaxNotches) { return false; }
        slot_[static_cast<std::size_t>(index)].on.store(on, std::memory_order_relaxed);
        const std::uint32_t bit = 1u << index;
        if (on) { mask_.fetch_or(bit, std::memory_order_release); } else { mask_.fetch_and(~bit, std::memory_order_release); }
        return true;
    }
    void clear() {
        for (int i = 0; i < kMaxNotches; ++i) { setOn(i, false); }
    }
    int activeCount() const { return std::popcount(mask_.load(std::memory_order_acquire)); }

    // DSP thread. Filters in place.
    void process(float* x, std::size_t n) {
        const std::uint32_t wanted = mask_.load(std::memory_order_acquire);
        if (wanted == 0 && fadingOut_ == 0) { return; }  // the zero-cost path
        std::size_t i = 0;
        while (i < n) {
            if (blockPos_ == 0) { updateBlock(wanted); }
            const std::size_t seg = std::min(n - i, kBlock - blockPos_);
            runSegment(x + i, seg);
            blockPos_ = (blockPos_ + seg) % kBlock;
            i += seg;
        }
    }

    // Blocks of 16 samples the bank has worked on (stays 0 while nothing is on: the zero-cost proof for the tests).
    std::uint64_t blocksWorked() const { return blocks_; }

    // DSP thread: forget all filter memory and glides (next change of a slot starts clean).
    void reset() {
        for (auto& s : slot_) { s.reset(); }
        fadingOut_ = 0;
        blockPos_ = 0;
    }

private:
    static constexpr std::size_t kBlock = 16;

    struct Slot {
        std::atomic<double> freq{1000.0};
        std::atomic<double> width{50.0};
        std::atomic<bool> on{false};
        // DSP-owned
        Notch notch{48000.0, 1000.0, 20.0};
        double curF = 1000.0, curW = 50.0;
        double gain = 0.0;      // 0 = bypassed, 1 = fully notched
        double target = 0.0;    // fade target for this block
        bool running = false;   // filter is being run (gain > 0 or fading in)
        void reset() {
            notch.reset();
            gain = 0.0;
            target = 0.0;
            running = false;
        }
    };

    void updateBlock(std::uint32_t wanted) {
        ++blocks_;
        int fading = 0;
        for (int i = 0; i < kMaxNotches; ++i) {
            Slot& s = slot_[static_cast<std::size_t>(i)];
            const bool want = ((wanted >> i) & 1u) != 0;
            s.target = want ? 1.0 : 0.0;
            if (want && !s.running) {
                // Switched on: start from the target settings with a clean state, faded in from 0.
                s.curF = s.freq.load(std::memory_order_relaxed);
                s.curW = s.width.load(std::memory_order_relaxed);
                s.notch = Notch(fs_, s.curF, s.curF / s.curW);
                s.running = true;
                s.gain = 0.0;
            }
            if (!s.running) { continue; }
            if (want) {
                const double tf = s.freq.load(std::memory_order_relaxed);
                const double tw = s.width.load(std::memory_order_relaxed);
                const bool moved = (s.curF != tf) || (s.curW != tw);
                if (moved) {
                    s.curF = std::fabs(tf - s.curF) < 1e-3 ? tf : s.curF + glide_ * (tf - s.curF);
                    s.curW = std::fabs(tw - s.curW) < 1e-3 ? tw : s.curW + glide_ * (tw - s.curW);
                    s.notch.setFrequencyHz(s.curF);
                    s.notch.setQ(s.curF / s.curW);
                }
            }
            if (!want) { ++fading; }
        }
        fadingOut_ = fading;
    }

    void runSegment(float* x, std::size_t n) {
        for (auto& s : slot_) {
            if (!s.running) { continue; }
            if (s.gain >= 1.0 && s.target >= 1.0) {
                s.notch.process(x, n);
                continue;
            }
            float tmp[kBlock];
            for (std::size_t k = 0; k < n; ++k) { tmp[k] = x[k]; }
            s.notch.process(tmp, n);
            for (std::size_t k = 0; k < n; ++k) {
                if (s.gain < s.target) { s.gain = std::min(s.target, s.gain + fadeStep_); }
                else if (s.gain > s.target) { s.gain = std::max(s.target, s.gain - fadeStep_); }
                x[k] = static_cast<float>(x[k] + s.gain * (static_cast<double>(tmp[k]) - x[k]));
            }
            if (s.gain <= 0.0 && s.target <= 0.0) {
                s.reset();
                if (fadingOut_ > 0) { --fadingOut_; }
            }
        }
    }

    double fs_;
    double glide_;
    double fadeStep_;
    std::array<Slot, kMaxNotches> slot_;
    std::atomic<std::uint32_t> mask_{0};
    std::size_t blockPos_ = 0;
    int fadingOut_ = 0;
    std::uint64_t blocks_ = 0;
};

}  // namespace cascade::dsp
