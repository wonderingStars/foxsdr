// Real-time render pipeline: a source thread feeds the active IqSource into
// an SPSC ring — pacing it with a real-time clock when the source is
// free-running (the built-in signal generator), letting the device pace when
// it is self-paced (hardware) — a DSP thread drains it through
// SpectrumEstimator and publishes the newest spectrum frame for the GUI to
// poll. The same DSP thread also runs the audio chain (P3): every drained
// block is duplicated into
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
#include <memory>
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
#include "source/iq_source.hpp"
#include "source/siggen_source.hpp"

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

    // --- Source selection ----------------------------------------------------
    // Replaces the IQ source feeding the ring; a null pointer restores the
    // built-in SigGenSource (whose generator sigGen() keeps returning either
    // way). Safe to call while the pipeline runs — the swap quiesces the
    // source thread with this handshake, all under controlMutex_:
    //   1. srcRun_ cleared            (the source loop is told to exit)
    //   2. outgoing source stop()     (aborts an in-flight bounded read —
    //      this is why IqSource::read must be bounded/abortable: a blocked
    //      read with no abort path would stall the swap for its full bound)
    //   3. srcThread_ joined          (after this NO thread can touch the
    //      outgoing source, so destroying it is race-free by construction)
    //   4. pointer swapped, outgoing source destroyed
    //   5. incoming source start(), srcRun_ set, source thread respawned
    // The DSP thread keeps running throughout, draining whatever the ring
    // still holds, so consumers see at worst a brief frame-rate dip — never
    // a stall, a deadlock, or a frame from a half-swapped source.
    void setSource(std::unique_ptr<cascade::source::IqSource> s);

    // The source currently feeding the ring (the built-in generator unless
    // setSource installed something else). The reference — and any const
    // char* obtained from it, including activeSourceName() — stays valid
    // only until the next setSource(), which may destroy the object; per the
    // IqSource threading contract both are meant for the GUI/control thread,
    // the same thread that performs swaps.
    cascade::source::IqSource& activeSource();
    const char* activeSourceName();

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
    // Built-in generator source: always alive (a member, not a unique_ptr)
    // so setSource(nullptr) can restore it without allocation or failure.
    // external_ holds a caller-installed source; active_ points at whichever
    // of the two feeds the ring. active_ only changes while the source
    // thread is quiesced (see setSource) or before it exists, so the thread
    // reads it once at entry without locking.
    cascade::source::SigGenSource builtin_;
    std::unique_ptr<cascade::source::IqSource> external_;
    cascade::source::IqSource* active_ = nullptr;  // ctor sets &builtin_
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

    // Control operations (start/stop/setSource/dtor) serialize against each
    // other under controlMutex_; the threads themselves only ever read the
    // two flags. run_ gates BOTH threads (a stop); srcRun_ additionally
    // gates just the source thread so setSource can quiesce it alone while
    // the DSP thread keeps draining the ring.
    std::mutex controlMutex_;
    std::atomic<bool> run_{false};
    std::atomic<bool> srcRun_{false};
    std::thread srcThread_;
    std::thread dspThread_;
};

}  // namespace cascade::core
