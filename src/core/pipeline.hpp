// Real-time render pipeline: a source thread paces SigGen into an SPSC ring,
// a DSP thread drains it through SpectrumEstimator and publishes the newest
// spectrum frame for the GUI to poll.
//
// Threading model (per PLAN.md): DSP blocks stay plain objects; the threads
// and the ring live only here. The GUI never blocks on DSP — it polls
// getLatestFrame(), which hands over at most one frame copy under a mutex.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp/spectrum.hpp"
#include "dsp/spsc_ring.hpp"
#include "source/siggen.hpp"

namespace cascade::core {

struct SpectrumFrame {
    std::vector<float> dbBins;   // fftshifted dB power spectrum, size == fftSize
    std::uint64_t seq = 0;       // strictly increasing per published frame
};

class Pipeline {
public:
    struct Config {
        double sampleRateHz = 1000000.0;
        std::size_t fftSize = 1024;    // must satisfy ComplexFFT::isValidSize
        float averagingAlpha = 0.5f;   // EMA weight, clamped by SpectrumEstimator
    };

    // Throws std::invalid_argument (from SpectrumEstimator/ComplexFFT) if
    // cfg.fftSize is not a legal FFT size — failing at construction beats a
    // dead DSP thread discovered later.
    explicit Pipeline(Config cfg);
    ~Pipeline();                                  // stops if running

    // Non-copyable: owns threads and a live ring.
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    cascade::source::SigGen& sigGen();            // configure tones before/while running
    void start();                                 // idempotent; spawns source-pacing + DSP threads
    void stop();                                  // idempotent; joins both threads
    bool running() const;
    bool getLatestFrame(SpectrumFrame& out);      // true iff a frame newer than out.seq was copied into out

private:
    void sourceThreadMain();
    void dspThreadMain();

    Config cfg_;
    cascade::source::SigGen sigGen_;
    cascade::dsp::SpscRing<std::complex<float>> ring_;
    cascade::dsp::SpectrumEstimator estimator_;   // touched only by the DSP thread while running

    // Single latest-frame slot. seq lives inside latest_ and NEVER resets —
    // not even across stop()/start() — so a consumer that kept its last seen
    // seq keeps receiving frames after a restart. latest_.seq == 0 means
    // "nothing published yet".
    std::mutex frameMutex_;
    SpectrumFrame latest_;

    // Control operations (start/stop/dtor) serialize against each other; the
    // threads themselves only ever read run_.
    std::mutex controlMutex_;
    std::atomic<bool> run_{false};
    std::thread srcThread_;
    std::thread dspThread_;
};

}  // namespace cascade::core
