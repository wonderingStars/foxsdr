// Zoom FFT: a narrow, high-resolution spectrum around a chosen frequency, by the classic route: mix the wanted centre down to DC,
// low-pass and decimate by M, then take a small windowed FFT of the slow stream.
//
// RESOLUTION: the bin width is fs / (M N) Hz (fs the input rate, N the FFT size). Example: fs 48 kHz, M 64, N 2048 gives 0.366 Hz bins and
// a displayed span of +-300 Hz; the same N at the full rate would give 23.4 Hz bins. The window is Blackman-Harris (4-term, -92 dB
// sidelobes; its main lobe is 8 bins wide, so two equal tones need to be about 6 bins apart to show a dip).
// FILTER: a Blackman-Harris windowed-sinc low-pass of 40 M + 1 taps (existing windowedSincLowpass + FirDecimator), -6 dB at 0.5 fs / M,
// flat to within the measured figure inside the usable span of +-0.4 fs / M and at least 80 dB down from 0.6 fs / M (where the first alias
// of the other side lands inside the span). The displayed bins cover the whole +-0.5 fs / M; only the middle 80 % is claimed as clean.
// LEVEL: bins are |X_k| / sum(w): a unit tone exactly on a bin reads 0 dB, between bins it reads up to 0.83 dB lower (scalloping of
// the Blackman-Harris window, the usual figure; measured). toneLevelDb() instead sums the power over the main lobe and divides by the
// window's noise bandwidth (Parseval), which is independent of where the tone falls between bins. The spectrum is fftshifted: bin N/2
// is the centre frequency, bin k is centre + (k - N/2) fs / (M N).
// FRAMES: a frame every N / 2 decimated samples (50 % overlap). The newest frame is published with a sequence lock; any thread may
// copy it with latest().
// COST: the filter costs 40 complex-by-real multiply-adds per INPUT sample whatever M is (e.g. 40 x 192 kS/s = 7.7 MMAC/s): at multi-MS/s
// source rates the wiring should feed it from an already decimated stream.
//
// THREADS: push() from ONE thread; setCenterHz() any thread (atomic, applied at the next push, the oscillator phase continues);
// latest() any thread. No allocation after construction (the decimator is warmed up in the constructor; push works in 256-sample
// pieces). Non-finite samples read as 0. Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/fir.hpp"
#include "dsp/nco.hpp"
#include "dsp/window.hpp"

namespace cascade::dsp {

class ZoomFft {
public:
    ZoomFft(double sampleRateHz, std::size_t decimation, std::size_t fftSize = 1024)
        : fs_(sampleRateHz > 0.0 ? sampleRateHz : 48000.0),
          m_(std::max<std::size_t>(decimation, 1)),
          n_(ComplexFFT::isValidSize(fftSize) ? fftSize : 1024),
          dec_(m_ == 1 ? std::vector<float>{1.0f} : windowedSincLowpass(40 * m_ + 1, 0.5 / static_cast<double>(m_), WindowType::BlackmanHarris), m_),
          fft_(n_),
          win_(makeWindow(WindowType::BlackmanHarris, n_)),
          ring_(n_),
          buf_(n_),
          spec_(n_),
          mixed_(256),
          decOut_(256 / m_ + 2),
          frame_(n_, -200.0f) {
        double s = 0.0, s2 = 0.0;
        for (float w : win_) { s += w; s2 += static_cast<double>(w) * w; }
        sumW_ = s;
        enbw_ = static_cast<double>(n_) * s2 / (s * s);
        // Warm up the decimator's scratch buffer so push() never allocates, then forget the zeros.
        std::vector<std::complex<float>> z(256);
        dec_.process(z.data(), z.size(), decOut_.data());
        dec_.reset();
    }

    std::size_t fftSize() const { return n_; }
    std::size_t decimation() const { return m_; }
    double binHz() const { return fs_ / static_cast<double>(m_ * n_); }
    double outputRateHz() const { return fs_ / static_cast<double>(m_); }
    double usableSpanHz() const { return 0.8 * outputRateHz(); }
    double enbwBins() const { return enbw_; }
    // Frequency of bin k relative to the centre, Hz.
    double binOffsetHz(std::size_t k) const { return (static_cast<double>(k) - static_cast<double>(n_ / 2)) * binHz(); }

    void setCenterHz(double hz) {
        if (std::isfinite(hz)) { center_.store(std::min(std::max(hz, -0.5 * fs_), 0.5 * fs_)); }
    }
    double centerHz() const { return center_.load(); }

    void push(const std::complex<float>* x, std::size_t n) {
        const double c = center_.load();
        if (c != appliedCenter_) {
            nco_.setFrequency(-c / fs_);  // shift the chosen centre down to DC
            appliedCenter_ = c;
        }
        std::size_t done = 0;
        while (done < n) {
            const std::size_t take = std::min<std::size_t>(256, n - done);
            for (std::size_t i = 0; i < take; ++i) {
                const std::complex<float> v = x[done + i];
                mixed_[i] = (std::isfinite(v.real()) && std::isfinite(v.imag())) ? v : std::complex<float>(0.0f, 0.0f);
            }
            nco_.mix(mixed_.data(), mixed_.data(), take);
            const std::size_t got = dec_.process(mixed_.data(), take, decOut_.data());
            for (std::size_t i = 0; i < got; ++i) { addSample(decOut_[i]); }
            done += take;
        }
    }

    // Copies the newest frame (n_ dB values, fftshifted) into `out`; returns its frame number (1, 2, ...), 0 when none yet or when the
    // copy could not be made consistent.
    std::uint64_t latest(float* out) const {
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u)) { if (s1 == 0) { return 0; } continue; }
            std::copy(frame_.begin(), frame_.end(), out);
            if (seq_.load(std::memory_order_acquire) == s1) { return s1 / 2; }
        }
        return 0;
    }

    // Level of the strongest tone near `bin` (+-`reach` bins), dB re a unit tone: the power over the main lobe divided by the window's
    // noise bandwidth. Independent of where the tone falls between bins. Returns -200 when the range is empty.
    double toneLevelDb(const float* db, std::size_t bin, std::size_t reach = 4) const {
        std::size_t best = bin;
        const std::size_t lo = bin > reach ? bin - reach : 0, hi = std::min(bin + reach, n_ - 1);
        for (std::size_t k = lo; k <= hi; ++k) { if (db[k] > db[best]) { best = k; } }
        const std::size_t a = best > 4 ? best - 4 : 0, b = std::min(best + 4, n_ - 1);
        double p = 0.0;
        for (std::size_t k = a; k <= b; ++k) { p += std::pow(10.0, static_cast<double>(db[k]) / 10.0); }
        return p > 0.0 ? 10.0 * std::log10(p / enbw_) : -200.0;
    }

    std::uint64_t frames() const { return seq_.load(std::memory_order_acquire) / 2; }

private:
    void addSample(std::complex<float> v) {
        ring_[fill_ % n_] = v;
        ++fill_;
        ++sinceFrame_;
        if (fill_ >= n_ && sinceFrame_ >= n_ / 2) {
            sinceFrame_ = 0;
            const std::size_t start = fill_ - n_;
            for (std::size_t i = 0; i < n_; ++i) { buf_[i] = ring_[(start + i) % n_] * win_[i]; }
            fft_.forward(buf_.data(), spec_.data());
            const std::uint64_t s = seq_.load(std::memory_order_relaxed);
            seq_.store(s + 1, std::memory_order_release);  // odd: writing
            for (std::size_t k = 0; k < n_; ++k) {
                const std::size_t src = (k + n_ / 2) % n_;  // fftshift
                const double p = std::norm(std::complex<double>(spec_[src].real(), spec_[src].imag())) / (sumW_ * sumW_);
                frame_[k] = p > 1e-20 ? static_cast<float>(10.0 * std::log10(p)) : -200.0f;
            }
            seq_.store(s + 2, std::memory_order_release);
        }
    }

    double fs_;
    std::size_t m_, n_;
    FirDecimator dec_;
    ComplexFFT fft_;
    std::vector<float> win_;
    std::vector<std::complex<float>> ring_, buf_, spec_, mixed_, decOut_;
    std::vector<float> frame_;
    Nco nco_;
    double sumW_ = 1.0, enbw_ = 2.0;
    double appliedCenter_ = 0.0;
    std::atomic<double> center_{0.0};
    std::size_t fill_ = 0, sinceFrame_ = 0;
    std::atomic<std::uint64_t> seq_{0};
};

}  // namespace cascade::dsp
