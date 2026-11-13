// Spectrum block with a choice of FFT size (1,024 .. 524,288) and window, fixed frame rate through overlap, average / peak hold /
// min hold, and max-per-column reduction to display columns (a narrow carrier never vanishes between columns).
//
// (Stand-alone when it was written; the pipeline's SET_SPECTRUM path uses it now.) STAND-ALONE: nothing in the engine uses it yet. It reuses dsp::ComplexFFT (pffft) and nothing else of the existing spectrum path
// (dsp/spectrum.hpp has fixed windows and no hold modes; core/receiver_tables.hpp:40 fixes kFftSize = 1024).
//
// WINDOWS (periodic, sum of cosines w[n] = sum_k (-1)^k a_k cos(2 pi k n / N)):
//   Rectangular; Hann a = {0.5, 0.5}; Hamming a = {0.54, 0.46}; Blackman-Harris 4-term a = {0.35875, 0.48829, 0.14128, 0.01168}
//   (Harris, Proc. IEEE 66(1), 1978); flat-top a = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368} (the widely
//   published five-term flat-top; the first four digits of each are confirmed by the Lyons note cited in the record).
//
// LEVEL (SpecScale::Tone): a complex tone of amplitude A exactly on a bin reads 20 log10 A dB (a full-scale tone is 0 dB), for EVERY
// window, because the bin power is divided by (N * coherentGain)^2. SpecScale::Psd: |X|^2 / (fs * sum w^2), i.e. dB per Hz; white
// complex noise of power P reads 10 log10(P / fs) on average whatever the window.
//
// THREADS. Construction and prepare() allocate (control thread). process() is the DSP thread's: allocation-free. latest(), swapIn(),
// collectRetired() and the destructor belong to ONE control thread. A size / window / hop / column change is built by prepare() on
// the control thread, handed over by swapIn() (one atomic exchange) and adopted by the next process() call; the engine it replaces is
// parked in one of four slots and freed by the control thread, never by the DSP thread. If every slot is full the DSP thread keeps the
// old engine (it never frees). Non-finite input samples are the caller's to filter (they would poison a frame and the average).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/spectrum_block_gpu.hpp"   // wire-b second pass: the GPU stage (OFF by default)

namespace cascade::dsp {

enum class SpecWindow { Rectangular, Hann, Hamming, BlackmanHarris, FlatTop };
enum class SpecMode { Instant, Average, PeakHold, MinHold };
enum class SpecScale { Tone, Psd };

constexpr std::size_t kSpecMinFft = 1024;
constexpr std::size_t kSpecMaxFft = 524288;
constexpr std::size_t kSpecMaxColumns = 16384;

inline bool specSizeValid(std::size_t n) { return n >= kSpecMinFft && n <= kSpecMaxFft && (n & (n - 1)) == 0; }

// Periodic window into `out` (n floats), coefficients as in the header comment.
inline void specFillWindow(SpecWindow w, std::size_t n, float* out) {
    static constexpr double kHann[2] = {0.5, 0.5};
    static constexpr double kHamming[2] = {0.54, 0.46};
    static constexpr double kBh[4] = {0.35875, 0.48829, 0.14128, 0.01168};
    static constexpr double kFlat[5] = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368};
    const double* a = nullptr;
    int terms = 0;
    switch (w) {
        case SpecWindow::Rectangular: break;
        case SpecWindow::Hann: a = kHann; terms = 2; break;
        case SpecWindow::Hamming: a = kHamming; terms = 2; break;
        case SpecWindow::BlackmanHarris: a = kBh; terms = 4; break;
        case SpecWindow::FlatTop: a = kFlat; terms = 5; break;
    }
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (std::size_t i = 0; i < n; ++i) {
        double v = 1.0;
        if (a != nullptr) {
            v = 0.0;
            const double x = kTwoPi * static_cast<double>(i) / static_cast<double>(n);
            for (int k = 0; k < terms; ++k) { v += ((k & 1) != 0 ? -1.0 : 1.0) * a[k] * std::cos(k * x); }
        }
        out[i] = static_cast<float>(v);
    }
}

// Sum of w and of w^2 in double (coherent gain = sum / n; noise bandwidth in bins = n * sumSq / sum^2).
struct SpecWindowStats {
    double sum = 0.0;
    double sumSq = 0.0;
};
inline SpecWindowStats specWindowStats(const float* w, std::size_t n) {
    SpecWindowStats s;
    for (std::size_t i = 0; i < n; ++i) { s.sum += w[i]; s.sumSq += static_cast<double>(w[i]) * w[i]; }
    return s;
}

// Reduce `n` values (dB or linear: max is monotonic) to `cols` columns by the MAXIMUM per column. Column c covers bins
// [floor(c n / cols), floor((c+1) n / cols)) so every bin lies in exactly one column. If cols >= n each column takes the bin under
// its centre. Pure, allocation-free.
inline void specReduceMax(const float* in, std::size_t n, float* out, std::size_t cols) {
    if (cols == 0 || n == 0) { return; }
    if (cols >= n) {
        for (std::size_t c = 0; c < cols; ++c) { out[c] = in[std::min(n - 1, (2 * c + 1) * n / (2 * cols))]; }
        return;
    }
    for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t a = c * n / cols;
        const std::size_t b = (c + 1) * n / cols;
        float m = in[a];
        for (std::size_t i = a + 1; i < b; ++i) { m = std::max(m, in[i]); }
        out[c] = m;
    }
}

struct SpecConfig {
    std::size_t fftSize = 4096;
    SpecWindow window = SpecWindow::BlackmanHarris;
    double sampleRateHz = 2400000.0;
    // New samples between frames. 0 = derive from frameRateHz. Clamped to [fftSize/16, 2^26].
    std::size_t hop = 0;
    double frameRateHz = 30.0;
    std::size_t columns = 0;    // 0 or >= fftSize: the full fftSize bins; else max-reduced, <= kSpecMaxColumns
    SpecMode mode = SpecMode::Average;
    SpecScale scale = SpecScale::Tone;
    float alpha = 0.3f;         // Average: weight of the newest frame
    float peakDecayDbPerS = 0;  // PeakHold: 0 = hold until reset
};

// What a frame rate asks of the hop and what it gets (the hop floor fftSize/16 caps the rate when the FFT is large).
struct SpecPlan {
    std::size_t hop = 0;
    double frameRateHz = 0.0;
    double overlap = 0.0;  // 1 - hop/N; negative = gaps between frames
};
inline SpecPlan specPlan(double fs, std::size_t n, double wantedFps) {
    SpecPlan p;
    double h = (wantedFps > 0.0 && fs > 0.0) ? fs / wantedFps : static_cast<double>(n);
    const double lo = static_cast<double>(n) / 16.0;
    h = std::min(std::max(h, lo), 67108864.0);
    p.hop = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(h)));
    p.frameRateHz = fs / static_cast<double>(p.hop);
    p.overlap = 1.0 - static_cast<double>(p.hop) / static_cast<double>(n);
    return p;
}

class SpectrumBlock {
public:
    // Everything one size/window needs. Built on the control thread.
    class Engine {
    public:
        explicit Engine(const SpecConfig& cfgIn) : cfg_(cfgIn), fft_(cfgIn.fftSize) {
            live().fetch_add(1);
            n_ = cfg_.fftSize;
            if (cfg_.hop == 0) { cfg_.hop = specPlan(cfg_.sampleRateHz, n_, cfg_.frameRateHz).hop; }
            cfg_.hop = std::min<std::size_t>(std::max<std::size_t>(cfg_.hop, n_ / 16), 67108864);
            outCols_ = (cfg_.columns == 0 || cfg_.columns >= n_) ? n_ : std::min(cfg_.columns, kSpecMaxColumns);
            window_.resize(n_);
            specFillWindow(cfg_.window, n_, window_.data());
            const SpecWindowStats st = specWindowStats(window_.data(), n_);
            toneScale_ = static_cast<float>(1.0 / (st.sum * st.sum));
            psdScale_ = static_cast<float>(1.0 / (std::max(cfg_.sampleRateHz, 1.0) * st.sumSq));
            ring_.assign(n_, std::complex<float>());
            work_.assign(n_, std::complex<float>());
            spec_.assign(n_, std::complex<float>());
            power_.assign(n_, 0.0f);
            acc_.assign(n_, 0.0f);
            red_.assign(outCols_, 0.0f);
            for (auto& s : slot_) { s.db.assign(outCols_, -200.0f); }
            frameSeconds_ = static_cast<double>(cfg_.hop) / std::max(cfg_.sampleRateHz, 1.0);
        }
        ~Engine() { live().fetch_sub(1); }
        // Engines alive in the process (a leak / double-free probe for the tests).
        static std::atomic<int>& live() {
            static std::atomic<int> n{0};
            return n;
        }
        const SpecConfig& config() const { return cfg_; }
        std::size_t columns() const { return outCols_; }
        // Bytes this engine holds itself plus the FFT's two staging buffers (pffft's twiddle table, roughly 8 N bytes, is not counted).
        std::size_t memoryBytes() const {
            return n_ * (sizeof(std::complex<float>) * 3 + sizeof(float) * 3) + outCols_ * sizeof(float) * 4 + n_ * 16;
        }

    private:
        friend class SpectrumBlock;
        struct Slot {
            std::uint64_t seq = 0;
            std::vector<float> db;
        };

        void setModeFlags(SpecMode m, float alpha, float decay) {
            if (m != cfg_.mode) { primed_ = false; }
            cfg_.mode = m;
            cfg_.alpha = alpha;
            cfg_.peakDecayDbPerS = decay;
        }

        void push(const std::complex<float>* in, std::size_t n) {
            const std::size_t hop = cfg_.hop;
            while (n > 0) {
                const std::size_t seg = std::min(n, hop - phase_);
                // Only the last n_ samples of a hop period are ever used.
                const std::size_t keepFrom = hop > n_ ? hop - n_ : 0;
                std::size_t skip = 0;
                if (phase_ < keepFrom) { skip = std::min(seg, keepFrom - phase_); }
                if (skip < seg) { store(in + skip, seg - skip); }
                phase_ += seg;
                in += seg;
                n -= seg;
                if (phase_ == hop) {
                    phase_ = 0;
                    if (valid_ >= n_) { frame(); }
                }
            }
        }
        void store(const std::complex<float>* p, std::size_t n) {
            if (n >= n_) {
                p += n - n_;
                n = n_;
            }
            const std::size_t first = std::min(n, n_ - w_);
            std::memcpy(ring_.data() + w_, p, first * sizeof(std::complex<float>));
            if (n > first) { std::memcpy(ring_.data(), p + first, (n - first) * sizeof(std::complex<float>)); }
            w_ = (w_ + n) % n_;
            valid_ = std::min(n_, valid_ + n);
        }
        void frame() {
            const std::size_t tail = n_ - w_;
            const float sc = cfg_.scale == SpecScale::Tone ? toneScale_ : psdScale_;
            // wire-b second pass: the GPU stage (spectrum_block_gpu.hpp), OFF unless the process policy asks: 0 = not engaged (the CPU FFT below), 1 = the GPU's frame is in power_, 2 = none finished yet
            const int gpuWanted = policy_ != nullptr ? policy_->load(std::memory_order_relaxed) : 0;
            int gpuState = 0;
            if (gpuWanted != 0) { gpuState = gpu_.step(gpuWanted, n_, window_.data(), ring_.data(), w_); }
            if (gpuState == 2) { return; }   // (a frame arrives a hop or two late; this hop publishes none)
            if (gpuState == 1) {
                // the engine returns dB of |FFT(x w)|^2 / N^2, fftshifted: back to this block's power scale
                const float* g = gpu_.db();
                const float n2 = static_cast<float>(n_) * static_cast<float>(n_);
                for (std::size_t k = 0; k < n_; ++k) { power_[k] = std::pow(10.0f, g[k] / 10.0f) * n2 * sc; }
            } else {
                for (std::size_t i = 0; i < tail; ++i) { work_[i] = ring_[w_ + i] * window_[i]; }
                for (std::size_t i = 0; i < w_; ++i) { work_[tail + i] = ring_[i] * window_[tail + i]; }
                fft_.forward(work_.data(), spec_.data());
                // fftshift while converting to power: output index n/2 is DC.
                const std::size_t h = n_ / 2;
                for (std::size_t k = 0; k < h; ++k) {
                    power_[k + h] = std::norm(spec_[k]) * sc;
                    power_[k] = std::norm(spec_[k + h]) * sc;
                }
            }
            const float* out = power_.data();
            if (cfg_.mode != SpecMode::Instant) {
                if (!primed_) {
                    acc_ = power_;  // same size: no allocation
                    primed_ = true;
                } else if (cfg_.mode == SpecMode::Average) {
                    const float a = cfg_.alpha;
                    for (std::size_t k = 0; k < n_; ++k) { acc_[k] += a * (power_[k] - acc_[k]); }
                } else if (cfg_.mode == SpecMode::PeakHold) {
                    const float d = cfg_.peakDecayDbPerS > 0.0f
                                        ? std::pow(10.0f, -cfg_.peakDecayDbPerS * static_cast<float>(frameSeconds_) / 10.0f)
                                        : 1.0f;
                    for (std::size_t k = 0; k < n_; ++k) { acc_[k] = std::max(power_[k], acc_[k] * d); }
                } else {
                    for (std::size_t k = 0; k < n_; ++k) { acc_[k] = std::min(power_[k], acc_[k]); }
                }
                out = acc_.data();
            }
            Slot& s = slot_[static_cast<std::size_t>(wr_)];
            if (outCols_ == n_) {
                for (std::size_t c = 0; c < n_; ++c) { red_[c] = out[c]; }
            } else {
                specReduceMax(out, n_, red_.data(), outCols_);
            }
            for (std::size_t c = 0; c < outCols_; ++c) { s.db[c] = 10.0f * std::log10(std::max(red_[c], 1e-20f)); }
            s.seq = ++seq_;
            const int old = ready_.exchange(wr_ | kNew, std::memory_order_acq_rel);
            wr_ = old & 3;
            ++frames_;
        }
        void reset() { primed_ = false; }

        static constexpr int kNew = 4;
        SpecConfig cfg_;
        ComplexFFT fft_;
        std::size_t n_ = 0, outCols_ = 0;
        std::vector<float> window_;
        float toneScale_ = 1.0f, psdScale_ = 1.0f;
        std::vector<std::complex<float>> ring_, work_, spec_;
        std::vector<float> power_, acc_, red_;
        std::array<Slot, 3> slot_;
        std::size_t w_ = 0, valid_ = 0, phase_ = 0;
        double frameSeconds_ = 0.0;
        bool primed_ = false;
        BlockGpu gpu_;
        const std::atomic<int>* policy_ = nullptr;   // the owning block's GPU switch (set by the block before the engine runs)
        int wr_ = 0;  // writer's slot; slot 1 starts as the ready one, slot 2 as the reader's
        std::atomic<int> ready_{1};
        int rd_ = 2;
        std::uint64_t seq_ = 0, frames_ = 0, readSeq_ = 0;
    };

    // Throws std::invalid_argument for an invalid size.
    explicit SpectrumBlock(const SpecConfig& cfg) {
        if (!specSizeValid(cfg.fftSize)) { throw std::invalid_argument("fft size"); }
        active_ = new Engine(cfg);
        active_->policy_ = &gpuPolicy_;
        forReader_.store(active_, std::memory_order_release);
    }
    ~SpectrumBlock() {
        delete pending_.exchange(nullptr);
        for (auto& r : retired_) { delete r.exchange(nullptr); }
        delete active_;
    }
    SpectrumBlock(const SpectrumBlock&) = delete;
    SpectrumBlock& operator=(const SpectrumBlock&) = delete;

    // CONTROL THREAD. Builds a new engine (allocates, plans the FFT). Returns null for an invalid size.
    static std::unique_ptr<Engine> prepare(const SpecConfig& cfg) {
        if (!specSizeValid(cfg.fftSize)) { return nullptr; }
        return std::make_unique<Engine>(cfg);
    }
    // CONTROL THREAD. Hands an engine to the DSP thread (adopted at its next process()); frees any engine that was not yet adopted.
    void swapIn(std::unique_ptr<Engine> e) {
        collectRetired();
        if (!e) { return; }
        delete pending_.exchange(e.release(), std::memory_order_acq_rel);
    }
    // CONTROL THREAD. Frees engines the DSP thread has let go of.
    void collectRetired() {
        for (auto& r : retired_) { delete r.exchange(nullptr, std::memory_order_acq_rel); }
    }
    // Convenience: prepare + swapIn. False for an invalid size.
    bool reconfigure(const SpecConfig& cfg) {
        auto e = prepare(cfg);
        if (!e) { return false; }
        swapIn(std::move(e));
        return true;
    }

    // DSP THREAD. Allocation-free.
    void process(const std::complex<float>* in, std::size_t n) {
        adopt();
        active_->push(in, n);
    }
    // DSP THREAD. Mode, alpha and peak decay change without a new engine (a mode change restarts the hold).
    void setMode(SpecMode m, float alpha, float peakDecayDbPerS) {
        active_->setModeFlags(m, std::clamp(alpha, 1e-4f, 1.0f), std::max(0.0f, peakDecayDbPerS));
    }
    // DSP THREAD (between process() calls): forget the holds.
    void resetHolds() { active_->reset(); }

    // CONTROL THREAD. Copies the newest frame not yet read into out (cap floats); returns the columns written, 0 if nothing new.
    std::size_t latest(float* out, std::size_t cap, std::uint64_t* seq = nullptr) {
        Engine* e = forReader_.load(std::memory_order_acquire);
        if ((e->ready_.load(std::memory_order_acquire) & Engine::kNew) != 0) {
            const int old = e->ready_.exchange(e->rd_, std::memory_order_acq_rel);
            e->rd_ = old & 3;
        }
        const Engine::Slot& s = e->slot_[static_cast<std::size_t>(e->rd_)];
        if (s.seq == 0 || s.seq == e->readSeq_) { return 0; }
        const std::size_t c = std::min(cap, e->outCols_);
        std::memcpy(out, s.db.data(), c * sizeof(float));
        e->readSeq_ = s.seq;
        if (seq != nullptr) { *seq = s.seq; }
        return c;
    }
    // CONTROL THREAD: properties of the engine the DSP thread is running.
    std::size_t columns() const { return forReader_.load(std::memory_order_acquire)->outCols_; }
    std::size_t fftSize() const { return forReader_.load(std::memory_order_acquire)->n_; }
    std::uint64_t framesProduced() const { return forReader_.load(std::memory_order_acquire)->frames_; }
    const Engine& engine() const { return *forReader_.load(std::memory_order_acquire); }
    // CONTROL THREAD. The GPU stage's switch (spectrum_block_gpu.hpp): 0 CPU (the default), 1 Auto (from 65 536 points), 2 GPU. Takes effect at the next frame, with no rebuild.
    void setGpuPolicy(int p) { gpuPolicy_.store(p < 0 || p > 2 ? 0 : p, std::memory_order_release); }
    // Frames of the running engine finished on the Direct3D device / on the GPU engine's own CPU computation (0 while the GPU stage is off), and whether a device is up.
    std::uint64_t gpuFramesOnGpu() const { return forReader_.load(std::memory_order_acquire)->gpu_.framesOnGpu(); }
    std::uint64_t gpuFramesOnCpu() const { return forReader_.load(std::memory_order_acquire)->gpu_.framesOnCpu(); }
    bool gpuUp() const { return forReader_.load(std::memory_order_acquire)->gpu_.usingGpu(); }

private:
    void adopt() {
        if (pending_.load(std::memory_order_relaxed) == nullptr) { return; }
        // Only this thread fills a retired slot and only the control thread empties one, so a slot seen empty stays empty.
        for (auto& slot : retired_) {
            if (slot.load(std::memory_order_acquire) != nullptr) { continue; }
            Engine* np = pending_.exchange(nullptr, std::memory_order_acq_rel);
            if (np == nullptr) { return; }
            Engine* old = active_;
            np->policy_ = &gpuPolicy_;
            active_ = np;
            forReader_.store(np, std::memory_order_release);  // the reader moves off the old engine BEFORE it can be collected
            slot.store(old, std::memory_order_release);
            return;
        }
    }

    std::atomic<int> gpuPolicy_{0};   // 0 CPU (default), 1 Auto, 2 GPU: read by the engine at every frame
    Engine* active_ = nullptr;  // DSP thread's
    std::atomic<Engine*> forReader_{nullptr};
    std::atomic<Engine*> pending_{nullptr};
    std::array<std::atomic<Engine*>, 4> retired_{};
};

}  // namespace cascade::dsp
