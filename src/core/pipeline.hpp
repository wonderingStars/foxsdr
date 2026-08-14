// Real-time render pipeline: a source thread paces SigGen into an SPSC ring,
// a DSP thread drains it through SpectrumEstimator and publishes the newest
// spectrum frame for the GUI to poll. The same DSP thread also runs the audio
// chain (P3): every drained block is duplicated into
//   Vfo (decimate-by-10) -> Demodulator -> Agc -> Squelch ->
//   RationalResampler (channel rate -> 48 kHz) -> AudioOut::write.
//
// Why the audio chain shares the DSP thread instead of getting a third one:
// the whole chain costs a few tens of microseconds per 1024-sample block
// (dominated by the VFO's FIR at the input rate) against the block's ~0.5 ms
// real-time budget at 2 MS/s, so a dedicated thread would buy no headroom
// while adding another ring, its latency, and a shutdown-ordering hazard.
// Playback itself is already a separate thread — PortAudio's callback pulls
// from AudioOut's internal SPSC ring, so a slow device can never stall DSP.
//
// Threading model (per PLAN.md): DSP blocks stay plain objects; the threads
// and the ring live only here. The GUI never blocks on DSP — it polls
// getLatestFrame(), which hands over at most one frame copy under a mutex.
// Audio-chain parameter setters serialize against the DSP thread under one
// internal mutex and take effect at the next block.
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

#include "dsp/agc.hpp"
#include "dsp/demod.hpp"
#include "dsp/resampler.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/spsc_ring.hpp"
#include "dsp/squelch.hpp"
#include "dsp/vfo.hpp"
#include "sink/audio_out.hpp"
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
        // When true the constructor opens the default audio output device at
        // kAudioRateHz. When false no device is ever opened, but the audio
        // chain still runs (samples are counted and tapped, then dropped by
        // the deviceless sink's ring) — this is what lets --selftest verify
        // the chain headless on machines with no audio hardware.
        bool audioEnabled = true;
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

    // --- Audio chain control (P3) -------------------------------------------
    // All of these are callable from any thread while the pipeline runs: they
    // serialize against the DSP thread's per-block processing under one
    // internal mutex and take effect at the next audio block (~0.5 ms).

    // Selects the demodulator (default WFM). Also resets the AGC: a mode
    // switch changes the audio level regime, and carrying the old gain over
    // would blast or mute the first moments of the new mode.
    void setDemodMode(cascade::dsp::DemodMode m);
    cascade::dsp::DemodMode demodMode() const;

    // VFO tuning offset from the input center, Hz (phase-continuous retune).
    void setVfoOffsetHz(double offsetHz);
    double vfoOffsetHz() const;

    // Channel filter bandwidth, Hz (clamped by Vfo to what the channel rate
    // supports; the filter is redesigned and its history cleared).
    void setVfoBandwidthHz(double bandwidthHz);

    // Squelch open threshold in dB on the channel power (close at -3 dB below).
    void setSquelchDb(float thresholdDb);

    // Latest channel-power reading in dB (a PowerMeter over the VFO output,
    // i.e. the S-meter source). Updated once per DSP block into an atomic, so
    // this is a lock-free snapshot safe from any thread; -200 until the first
    // block after start().
    float signalPowerDb() const;

    // The audio sink, exposed for GUI wiring: volume, device enumeration, and
    // re-open on device change. The device the constructor opens (when
    // cfg.audioEnabled) is the system default at kAudioRateHz.
    cascade::sink::AudioOut& audio();

    // Rate the chain resamples to and the device is opened at.
    static constexpr double kAudioRateHz = 48000.0;

    // --- Test support (used by --selftest) ----------------------------------
    // Total audio samples produced by the chain, counted BEFORE
    // AudioOut::write so the count advances with or without a device.
    std::uint64_t audioSamplesProduced() const;

    // Copies the most recent audio samples (a rolling 4096-sample tap taken
    // just before AudioOut::write) into dst in chronological order. Returns
    // the number copied: min(n, 4096, samples produced so far).
    std::size_t audioTap(float* dst, std::size_t n) const;

private:
    void sourceThreadMain();
    void dspThreadMain();
    void processAudioBlock(const std::complex<float>* in, std::size_t n);

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

    // Audio chain state. Everything below audioMutex_ (except the atomics) is
    // touched only under it: by the DSP thread once per block and by the
    // setters/getters above. One mutex, never held together with frameMutex_
    // or controlMutex_ on the DSP thread, so no ordering to get wrong.
    mutable std::mutex audioMutex_;
    cascade::dsp::Vfo vfo_;
    cascade::dsp::Demodulator demod_;
    cascade::dsp::Agc agc_;
    cascade::dsp::Squelch squelch_;
    cascade::dsp::PowerMeter meter_;      // S-meter source (channel power)
    cascade::dsp::RationalResampler resampler_;
    cascade::sink::AudioOut audio_;       // constructed always; device opened
                                          // only when cfg_.audioEnabled
    // FM discriminator scale: rad/sample -> ~0.5 full scale at +/-75 kHz
    // deviation (see pipeline.cpp for the derivation).
    float fmScale_ = 1.0f;
    // Scratch buffers, members so steady-state blocks never allocate.
    std::vector<std::complex<float>> chanBuf_;   // VFO output (channel rate)
    std::vector<float> audioBuf_;                // demod/agc/squelch (channel rate)
    std::vector<float> outBuf_;                  // resampled 48 kHz audio
    // Rolling pre-AudioOut tap window + producer-side counter (test support).
    std::vector<float> tapBuf_;
    std::size_t tapWrite_ = 0;
    std::size_t tapFilled_ = 0;
    std::atomic<float> signalDb_{-200.0f};
    std::atomic<std::uint64_t> audioSamples_{0};

    // Control operations (start/stop/dtor) serialize against each other; the
    // threads themselves only ever read run_.
    std::mutex controlMutex_;
    std::atomic<bool> run_{false};
    std::thread srcThread_;
    std::thread dspThread_;
};

}  // namespace cascade::core
