// Real-time render pipeline: a source thread feeds the active IqSource into
// an SPSC ring — pacing it with a real-time clock when the source is
// free-running (the built-in signal generator), letting the device pace when
// it is self-paced (hardware) — a DSP thread drains it through
// SpectrumEstimator and publishes the newest spectrum frame for the GUI to
// poll. The same DSP thread also runs the audio chain (P3): every drained
// block is duplicated into
//   Vfo (decimate to a ~200 kHz channel; see setInputRateHz for the policy)
//   -> Demodulator -> Agc -> Squelch ->
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

// Forward declaration (core/recorder.hpp): the pipeline stores only
// non-owning pointers, so the full type is needed in pipeline.cpp alone.
class Recorder;

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

    // --- Runtime input-rate follow (rate-follow) ------------------------------
    // Rebuilds the rate-dependent DSP chain for a new input sample rate. The
    // SOURCE is deliberately NOT touched: the caller owns the device side
    // (command the hardware rate through activeSource().setSampleRateHz, or
    // install a source built at the new rate via setSource — the built-in
    // generator is fixed-rate by its own contract); this call only makes the
    // DSP side follow.
    //
    // Accepted rates — both conditions must hold, otherwise the call returns
    // false and changes NOTHING (the chain keeps running at the old rate):
    //  1. rateHz in [8 kHz, 61.44 MHz] (the range SDR front ends this app
    //     targets can actually deliver);
    //  2. rateHz / decim is an INTEGER, where decim = round(rateHz / 200 kHz)
    //     clamped to >= 1. The integer requirement is what keeps the audio
    //     resampler exact: RationalResampler takes an integer L/M ratio
    //     (channelRate -> 48 kHz, reduced by gcd internally), so a fractional
    //     channel rate could only be approximated, silently detuning audio.
    //     Note the bounds are necessary, not sufficient — e.g. 61.44 MHz
    //     itself is refused (decim 307 gives a fractional channel rate).
    // The decim policy lands the channel rate in [150 kHz, 250 kHz] for every
    // accepted rate >= 300 kHz; below 300 kHz decim is 1 and the channel rate
    // equals the input rate.
    //
    // Concurrency (mirrors the setSource quiesce handshake, but for the DSP
    // thread — the source thread keeps running throughout): all under
    // controlMutex_,
    //   1. dspRun_ cleared          (the DSP loop is told to exit; run_ stays
    //      set, so the source keeps feeding the ring — a full ring drops the
    //      overflow by design and never blocks the source)
    //   2. dspThread_ joined        (after this NO thread touches the
    //      estimator or the audio chain, so rebuilding them is race-free)
    //   3. chain rebuilt under audioMutex_ (audioTap()/setters may arrive
    //      from other threads while the DSP thread is down)
    //   4. dspRun_ set, DSP thread respawned
    // Holding controlMutex_ across the whole switch is what makes a
    // stop()-during-switch impossible by construction: stop(), start(),
    // setSource() and this call all serialize on that mutex, so a stop can
    // only run before the switch begins or after it completes.
    //
    // What changes: VFO (decim per the policy above; the last REQUESTED
    // bandwidth is re-applied, re-clamped for the new channel rate; the
    // tuning offset is preserved), Demodulator (reconstructed at the new
    // channel rate, mode preserved), Squelch (reconstructed so its ramp/hold
    // stay real-time, threshold preserved), RationalResampler (rebuilt
    // channelRate -> 48 kHz), the FM scale, and the AGC/S-meter state (reset:
    // new rate regime). What does not: the spectrum estimator (fftSize is
    // unchanged and the estimator is rate-agnostic — the displayed span
    // simply reinterprets, which is the caller's frequency-axis job), the
    // ring (its capacity stays the construction-time sizing, so headroom in
    // milliseconds shrinks at higher rates — still >= 8 FFT blocks for any
    // accepted rate), seq numbering, and the audio tap/counters.
    //
    // Calling with the CURRENT rate is a cheap no-op returning true. Safe to
    // call stopped (no threads to quiesce — the chain is rebuilt for the next
    // start()) or running, from any control-plane thread.
    bool setInputRateHz(double rateHz);

    // The input rate the DSP chain is currently built for (construction rate
    // until the first successful setInputRateHz).
    double inputRateHz() const;

    // The audio-side channel rate (the Vfo's output rate = input rate /
    // decimation) — the rate the Demodulator runs at and the resampler
    // converts to 48 kHz. Exposed for the GUI's bandwidth limits and for
    // tests of the rate-follow policy.
    double channelRateHz() const;

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

    // --- Recorder taps (P6) ---------------------------------------------------
    // Non-owning recorder hooks fed by the DSP thread; nullptr (the default)
    // disconnects. The IQ recorder receives every drained block RAW — the
    // ring's baseband samples at inputRateHz(), before the VFO touches them —
    // via writeIq, so an IQ recording replayed through IqFileSource
    // reproduces exactly what the spectrum displayed. The audio recorder
    // receives the resampled 48 kHz output via writeAudio at precisely the
    // point audioTap taps: post-squelch, pre-AudioOut, volume-independent.
    //
    // Both pointers live under audioMutex_ — the mutex the DSP thread holds
    // across every write* call — which is what satisfies the Recorder
    // threading contract ("start()/stop() must not overlap an in-flight
    // write") without parking the DSP thread: after set*Recorder(nullptr)
    // returns, no write against the old pointer is in flight or can begin,
    // so the caller may immediately stop()/destroy the recorder. Install
    // order for a new take is therefore Recorder::start() FIRST, then
    // set*Recorder(); teardown is set*Recorder(nullptr) FIRST, then stop().
    // writeIq/writeAudio ignore wrong-kind and stopped recorders by their
    // own contract, so the DSP hot path needs only the null checks.
    void setIqRecorder(Recorder* r);
    void setAudioRecorder(Recorder* r);

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
    // Rate-dependent chain parameters, all guarded by audioMutex_ alongside
    // the blocks they configure. vfoDecim_ is the CURRENT decimation (the
    // header constant it replaced assumed a fixed 2 MS/s input). The two
    // "requested" values exist because Vfo/Squelch clamp or don't expose
    // their setting: a rebuild re-applies the caller's request, re-clamped
    // for the new channel rate, instead of compounding old clamps.
    unsigned vfoDecim_ = 1;
    double vfoBandwidthHz_ = 0.0;  // last requested (pre-clamp) VFO bandwidth
    float squelchDb_ = -50.0f;     // last requested threshold (mirrors the
                                   // Squelch construction default)
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
    // Recorder taps (P6): non-owning, audioMutex_-guarded like the chain
    // blocks above — see the set*Recorder contract in the public section.
    Recorder* iqRecorder_ = nullptr;
    Recorder* audioRecorder_ = nullptr;

    // Control operations (start/stop/setSource/setInputRateHz/dtor) serialize
    // against each other under controlMutex_; the threads themselves only
    // ever read the flags. run_ gates BOTH threads (a stop); srcRun_
    // additionally gates just the source thread so setSource can quiesce it
    // alone while the DSP thread keeps draining the ring; dspRun_ is the
    // symmetric per-thread gate for the DSP thread so setInputRateHz can
    // quiesce IT alone while the source keeps feeding the ring. Mutable so
    // const readers of cfg_ (inputRateHz) can take it.
    mutable std::mutex controlMutex_;
    std::atomic<bool> run_{false};
    std::atomic<bool> srcRun_{false};
    std::atomic<bool> dspRun_{false};
    std::thread srcThread_;
    std::thread dspThread_;
};

}  // namespace cascade::core
