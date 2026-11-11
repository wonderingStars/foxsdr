// IF shift and passband tuning: the maths that places the pass band of the channel filter relative to
// the tuned frequency (shift = move the whole band, low cut / high cut = move one edge at a time), for
// SSB, CW and AM, and a complex band-pass filter whose edges can be changed while it runs without a click.
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline, the engine, the settings or the screen
// (docs/feature-records/signal-blocks-2.md, "WIRING HAND-OFF", says where it goes). The existing SSB
// path (dsp/demod.cpp, Demodulator::setSsbBandwidthHz) has a fixed pass band and is untouched.
//
// THE MATHS (passbandEdges)
//  All frequencies are Hz relative to the TUNED frequency, i.e. the baseband the Demodulator gets
//  (carrier or zero-beat at 0, positive = above the tuned frequency). The filter's two -6 dB points are
//  the edges. SHIFT is audio-referred, so one slider direction means the same thing in USB and LSB:
//  positive shift moves the pass band toward HIGHER AUDIO frequency (to more positive baseband
//  frequencies for USB, more negative for LSB).
//    USB   [ lowCut + shift ,  highCut + shift ]                       (e.g. 300..2700)
//    LSB   [ -(highCut + shift) , -(lowCut + shift) ]
//    CW    audio edges [lowCut + shift, highCut + shift] around the pitch, beaten down by the pitch the
//          Demodulator's CW mode adds: baseband [lowCut + shift - pitch, highCut + shift - pitch]
//          (e.g. 450..950 around a 700 Hz pitch is baseband -250..+250: zero-beat in the middle).
//    AM    the two edges are the sideband extents: lowCut = how far BELOW the carrier, highCut = how far
//          ABOVE it; baseband [-lowCut + shift, highCut + shift]. The carrier must stay inside the band
//          or an envelope detector has nothing to detect, so the lower edge is held at or below
//          -kAmCarrierMarginHz and the upper at or above +kAmCarrierMarginHz (flagged `clamped`).
//  Both cuts are clamped to [0, 0.45 rate], swapped if reversed, the band is kept at least
//  kMinBandwidthHz wide, and non-finite values read as the defaults. Pure function, no allocation.
//
// THE FILTER (PassbandFilter)
//  A linear-phase complex FIR: a real low-pass of cutoff (hi - lo)/2, designed by the Kaiser-window
//  method, modulated to the band centre (hi + lo)/2 (complex taps h[k] = lp[k] exp(j 2 pi fc (k - D) / fs),
//  phase referred to the centre tap so the filter has no phase term beyond the pure delay D). Taps are
//  scaled so the gain at the centre of the band is exactly 1. The Kaiser design relations are the
//  standard ones (Kaiser, 1974; Oppenheim and Schafer, "Discrete-Time Signal Processing", 3rd ed.,
//  section 7.5.3): for a stop-band attenuation A dB,
//     beta = 0.1102 (A - 8.7)                                  for A > 50,
//     N    = (A - 7.95) / (14.36 * transitionHz / fs) + 1      taps (made odd).
//  (From memory of the textbook; UNVERIFIED against it. The measured attenuation is what
//  tests/test_passband.cpp pins.) The -6 dB points sit on the requested edges; the stop band begins
//  transitionHz/2 outside them. The tap count is capped at maxTaps: a capped design is wider in
//  transition than asked, and actualTransitionHz() says by how much.
//
// GLITCH-FREE CHANGES
//  The filter output is linear in the taps, so ramping the taps from the old design to the new one is
//  the same as crossfading the old and the new filter's outputs, at the cost of ONE filter. Every
//  kBlock (16) samples the DSP thread moves the working taps a fraction alpha toward the target
//  taps (a one-pole approach with time constant rampSeconds, default 8 ms; alpha = 1 - exp(-kBlock /
//  (rate * rampSeconds))), so a retarget in the middle of a ramp simply bends the ramp. Every design
//  has the same tap count, hence the same delay: nothing shifts in time. rampSeconds 0 switches the
//  taps at once (used by the tests as the "no ramp" break-it).
//
// THREADS AND ALLOCATION
//  The constructor allocates (set-up). setPassband() runs on the CONTROL thread, designs the taps into
//  a preallocated target buffer (about 0.1-0.5 ms; measured in the test) and publishes them with a
//  sequence counter; it never allocates after construction and never blocks the DSP thread. process() runs
//  on the DSP thread only: it never designs anything, never allocates, locks or waits, and reads the
//  target through the sequence counter (a read that overlaps a write is discarded and retried at the
//  next block). Only one thread may call setPassband() at a time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cascade::dsp {

enum class PassbandMode : int { Usb = 0, Lsb = 1, Cw = 2, Am = 3 };

struct PassbandSpec {
    PassbandMode mode = PassbandMode::Usb;
    double lowCutHz = 300.0;
    double highCutHz = 2700.0;
    double shiftHz = 0.0;
    double cwPitchHz = 700.0;   // used by CW only
};

struct PassbandEdges {
    double loHz = 0.0;       // lower -6 dB edge, relative to the tuned frequency
    double hiHz = 0.0;       // upper -6 dB edge
    bool clamped = false;    // something was moved to keep the band legal (see the header)
};

inline constexpr double kPassbandMinBandwidthHz = 50.0;
inline constexpr double kAmCarrierMarginHz = 100.0;

inline PassbandEdges passbandEdges(const PassbandSpec& in, double rateHz) {
    PassbandEdges e;
    const double nyq = 0.45 * rateHz;
    const auto fin = [&](double v, double dflt) { return std::isfinite(v) ? v : dflt; };
    double lo = std::max(0.0, fin(in.lowCutHz, 300.0));
    double hi = std::max(0.0, fin(in.highCutHz, 2700.0));
    const double shift = fin(in.shiftHz, 0.0);
    const double pitch = fin(in.cwPitchHz, 700.0);
    if (lo > hi && in.mode != PassbandMode::Am) { std::swap(lo, hi); e.clamped = true; }
    double a = 0.0, b = 0.0;
    switch (in.mode) {
        case PassbandMode::Usb: a = lo + shift; b = hi + shift; break;
        case PassbandMode::Lsb: a = -(hi + shift); b = -(lo + shift); break;
        case PassbandMode::Cw: a = lo + shift - pitch; b = hi + shift - pitch; break;
        case PassbandMode::Am:
            // here lowCut and highCut are extents on each side of the carrier, so they are NOT swapped
            lo = std::max(0.0, fin(in.lowCutHz, 4500.0));
            hi = std::max(0.0, fin(in.highCutHz, 4500.0));
            a = -lo + shift;
            b = hi + shift;
            if (a > -kAmCarrierMarginHz) { a = -kAmCarrierMarginHz; e.clamped = true; }
            if (b < kAmCarrierMarginHz) { b = kAmCarrierMarginHz; e.clamped = true; }
            break;
    }
    if (a < -nyq) { a = -nyq; e.clamped = true; }
    if (b > nyq) { b = nyq; e.clamped = true; }
    if (a > nyq - kPassbandMinBandwidthHz) { a = nyq - kPassbandMinBandwidthHz; e.clamped = true; }
    if (b < -nyq + kPassbandMinBandwidthHz) { b = -nyq + kPassbandMinBandwidthHz; e.clamped = true; }
    if (b - a < kPassbandMinBandwidthHz) {
        const double c = 0.5 * (a + b);
        a = c - 0.5 * kPassbandMinBandwidthHz;
        b = c + 0.5 * kPassbandMinBandwidthHz;
        e.clamped = true;
    }
    e.loHz = a;
    e.hiHz = b;
    return e;
}

class PassbandFilter {
public:
    static constexpr std::size_t kBlock = 16;

    explicit PassbandFilter(double rateHz, double transitionHz = 300.0, double stopbandDb = 80.0,
                            std::size_t maxTaps = 2047, double rampSeconds = 0.008)
        : rate_(rateHz > 1000.0 ? rateHz : 48000.0) {
        const double A = std::clamp(stopbandDb, 40.0, 120.0);
        const double dfn = std::max(transitionHz, 1.0) / rate_;
        std::size_t n = static_cast<std::size_t>(std::ceil((A - 7.95) / (14.36 * dfn))) + 1;
        n = std::clamp<std::size_t>(n, 31, std::max<std::size_t>(maxTaps, 31));
        if (n % 2 == 0) { ++n; }
        taps_ = n;
        delay_ = (n - 1) / 2;
        // the transition the (possibly capped) tap count really gives: invert N = (A - 7.95)/(14.36 df/fs) + 1
        actualTransitionHz_ = (A - 7.95) / (14.36 * static_cast<double>(n - 1)) * rate_;
        beta_ = A > 50.0 ? 0.1102 * (A - 8.7) : (A > 21.0 ? 0.5842 * std::pow(A - 21.0, 0.4) + 0.07886 * (A - 21.0) : 0.0);
        i0Beta_ = besselI0(beta_);
        window_.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            const double r = (2.0 * static_cast<double>(k) / static_cast<double>(n - 1)) - 1.0;
            window_[k] = besselI0(beta_ * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta_;
        }
        tgtR_.reset(new std::atomic<float>[n]);
        tgtI_.reset(new std::atomic<float>[n]);
        curR_.assign(n, 0.0f);
        curI_.assign(n, 0.0f);
        scratchR_.assign(n, 0.0f);
        scratchI_.assign(n, 0.0f);
        dstR_.assign(n, 0.0f);
        dstI_.assign(n, 0.0f);
        xr_.assign(2 * n, 0.0f);
        xi_.assign(2 * n, 0.0f);
        setRampSeconds(rampSeconds);
        setPassband(PassbandSpec{}, true);
    }

    // Control thread. Designs the taps for `spec` and publishes them; `immediate` makes the DSP thread
    // take them at its next block without a ramp (use it at start-up and at a mode change).
    void setPassband(const PassbandSpec& spec, bool immediate = false) {
        const PassbandEdges e = passbandEdges(spec, rate_);
        design(e);
        spec_ = spec;
        edges_ = e;
        if (immediate) { snap_.store(true, std::memory_order_release); }
        // publish: odd while writing, even when complete
        seq_.fetch_add(1, std::memory_order_acq_rel);
        for (std::size_t j = 0; j < taps_; ++j) {
            tgtR_[j].store(scratchR_[j], std::memory_order_relaxed);
            tgtI_[j].store(scratchI_[j], std::memory_order_relaxed);
        }
        seq_.fetch_add(1, std::memory_order_acq_rel);
    }
    const PassbandSpec& spec() const { return spec_; }          // control thread
    const PassbandEdges& edges() const { return edges_; }       // control thread

    // Time constant of the tap ramp; 0 = switch at once (not click-free: the tests use it as the break-it).
    void setRampSeconds(double s) {
        const double v = std::isfinite(s) ? std::clamp(s, 0.0, 1.0) : 0.008;
        alpha_.store(v <= 0.0 ? 1.0f : static_cast<float>(1.0 - std::exp(-static_cast<double>(kBlock) / (rate_ * v))),
                     std::memory_order_relaxed);
    }

    // DSP thread. n complex samples in, n out (exactly one for one, delayed by latencySamples()).
    // `out` may be the same buffer as `in`. Non-finite input reads as zero.
    std::size_t process(const std::complex<float>* in, std::size_t n, std::complex<float>* out) {
        const std::size_t N = taps_;
        for (std::size_t i = 0; i < n; ++i) {
            if (blockPos_ == 0) { updateTaps(); }
            if (++blockPos_ == kBlock) { blockPos_ = 0; }
            float vr = in[i].real();
            float vi = in[i].imag();
            if (!std::isfinite(vr) || !std::isfinite(vi)) { vr = 0.0f; vi = 0.0f; }
            xr_[wp_] = vr;
            xr_[wp_ + N] = vr;
            xi_[wp_] = vi;
            xi_[wp_ + N] = vi;
            const float* br = &xr_[wp_ + 1];
            const float* bi = &xi_[wp_ + 1];
            const float* hr = curR_.data();
            const float* hi = curI_.data();
            float rr = 0.0f, ii = 0.0f, ri = 0.0f, ir = 0.0f;
            for (std::size_t j = 0; j < N; ++j) {
                rr += hr[j] * br[j];
                ii += hi[j] * bi[j];
                ri += hr[j] * bi[j];
                ir += hi[j] * br[j];
            }
            wp_ = (wp_ + 1 == N) ? 0 : wp_ + 1;
            out[i] = std::complex<float>(rr - ii, ri + ir);
        }
        return n;
    }

    // DSP thread (or parked). Clears the signal history; the taps (settings) are kept.
    void reset() {
        std::fill(xr_.begin(), xr_.end(), 0.0f);
        std::fill(xi_.begin(), xi_.end(), 0.0f);
        wp_ = 0;
    }

    std::size_t latencySamples() const { return delay_; }
    std::size_t taps() const { return taps_; }
    double rateHz() const { return rate_; }
    double actualTransitionHz() const { return actualTransitionHz_; }

private:
    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 60; ++k) {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
            if (term < 1e-17 * sum) { break; }
        }
        return sum;
    }

    // Designs the REVERSED complex taps for the edges into scratch (control thread; no allocation).
    void design(const PassbandEdges& e) {
        const double fcut = 0.5 * (e.hiHz - e.loHz);      // low-pass cutoff = half the bandwidth
        const double fmid = 0.5 * (e.hiHz + e.loHz);      // band centre
        const double wc = 6.283185307179586 * fcut / rate_;
        const double wm = 6.283185307179586 * fmid / rate_;
        double sum = 0.0;
        for (std::size_t k = 0; k < taps_; ++k) {
            const double m = static_cast<double>(k) - static_cast<double>(delay_);
            const double h = (m == 0.0 ? wc / 3.14159265358979323846 : std::sin(wc * m) / (3.14159265358979323846 * m)) * window_[k];
            const std::size_t j = taps_ - 1 - k;           // reversed: history is read oldest first
            scratchR_[j] = static_cast<float>(h);          // temporarily the real low-pass
            sum += h;
        }
        const double inv = sum != 0.0 ? 1.0 / sum : 1.0;
        for (std::size_t k = 0; k < taps_; ++k) {
            const std::size_t j = taps_ - 1 - k;
            const double m = static_cast<double>(k) - static_cast<double>(delay_);
            const double h = static_cast<double>(scratchR_[j]) * inv;
            scratchR_[j] = static_cast<float>(h * std::cos(wm * m));
            scratchI_[j] = static_cast<float>(h * std::sin(wm * m));
        }
    }

    // DSP thread, once per block: take the newest published target and move the working taps toward it.
    void updateTaps() {
        const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 != seenSeq_ && (s1 & 1U) == 0U) {
            // copy into the DSP-side target (curTgt), then confirm nothing was written meanwhile
            for (std::size_t j = 0; j < taps_; ++j) {
                dstR_[j] = tgtR_[j].load(std::memory_order_relaxed);
                dstI_[j] = tgtI_[j].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) { seenSeq_ = s1; haveTarget_ = true; }
        }
        if (!haveTarget_) { return; }
        if (snap_.exchange(false, std::memory_order_acq_rel)) {
            std::copy(dstR_.begin(), dstR_.end(), curR_.begin());
            std::copy(dstI_.begin(), dstI_.end(), curI_.begin());
            return;
        }
        const float a = alpha_.load(std::memory_order_relaxed);
        if (a >= 1.0f) {
            std::copy(dstR_.begin(), dstR_.end(), curR_.begin());
            std::copy(dstI_.begin(), dstI_.end(), curI_.begin());
            return;
        }
        for (std::size_t j = 0; j < taps_; ++j) {
            curR_[j] += a * (dstR_[j] - curR_[j]);
            curI_[j] += a * (dstI_[j] - curI_[j]);
        }
    }

    double rate_;
    std::size_t taps_ = 0;
    std::size_t delay_ = 0;
    double actualTransitionHz_ = 0.0;
    double beta_ = 0.0, i0Beta_ = 1.0;
    std::vector<double> window_;

    // control side
    PassbandSpec spec_;
    PassbandEdges edges_;
    std::vector<float> scratchR_, scratchI_;
    // shared (seqlock)
    std::unique_ptr<std::atomic<float>[]> tgtR_, tgtI_;
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<bool> snap_{false};
    std::atomic<float> alpha_{1.0f};
    // DSP side
    std::vector<float> dstR_, dstI_;
    std::vector<float> curR_, curI_;
    std::vector<float> xr_, xi_;
    std::size_t wp_ = 0, blockPos_ = 0;
    std::uint32_t seenSeq_ = 0;
    bool haveTarget_ = false;
};

}  // namespace cascade::dsp
