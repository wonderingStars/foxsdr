// Real-time render pipeline: a source thread feeds the active IqSource into
// an SPSC ring â€” pacing it with a real-time clock when the source is
// free-running (the built-in signal generator), letting the device pace when
// it is self-paced (hardware) â€” a DSP thread drains it through
// SpectrumEstimator and publishes the newest spectrum frame for the GUI to
// poll. The same DSP thread also runs the audio chain (P3/P7): every drained
// block is duplicated into
//   Vfo (decimate to a ~200 kHz channel; see setInputRateHz for the policy)
//   -> Demodulator -> [WFM: RdsDecoder tap, StereoFm matrix] -> Agc
//   -> Squelch -> RationalResampler x2 (channel rate -> 48 kHz)
//   -> Notch -> AutoNotch -> NoiseReduction -> AudioOut (mono or stereo).
//
// WFM DE-EMPHASIS OWNERSHIP (the one non-obvious wiring decision here).
// Both the stereo decoder and RDS need the composite (MPX) WITHOUT
// de-emphasis: a 50 us one-pole is ~40 dB down at 57 kHz, which would erase
// the RDS subcarrier outright, and the broadcast standard applies de-emphasis
// per channel AFTER the stereo matrix because the transmitter pre-emphasises
// L and R individually before matrixing. So in WFM the Demodulator's own
// de-emphasis is switched OFF and StereoFm owns it â€” including for MONO
// output, which is simply StereoFm with its stereo gate closed (L == R
// exactly). Routing mono through the same object is what makes toggling
// stereo, or losing pilot lock, inaudible in tone: the de-emphasis and the
// 15 kHz low-pass are identical on both sides of the switch, and StereoFm's
// gate ramps the difference channel in and out instead of hard-switching.
// The alternative (a second Demodulator instance purely for the MPX taps)
// would leave two different de-emphasis paths for mono and stereo audio and
// pay for a second discriminator; this way there is exactly one.
//
// AGC AND SQUELCH RUN ON THE INTERLEAVED STEREO STREAM, one instance each,
// so both channels always receive the identical gain and the identical gate
// envelope â€” two per-channel instances would each normalise their own channel
// to the target level and destroy the stereo image the matrix just recovered.
// The blocks are constructed at 2x the channel rate so their per-sample time
// constants stay correct in real time, and a mono stream (every non-WFM mode)
// therefore behaves exactly as it did before stereo existed.
//
// Why the audio chain shares the DSP thread instead of getting a third one:
// the whole chain costs a few tens of microseconds per 1024-sample block
// (dominated by the VFO's FIR at the input rate) against the block's ~0.5 ms
// real-time budget at 2 MS/s, so a dedicated thread would buy no headroom
// while adding another ring, its latency, and a shutdown-ordering hazard.
// Playback itself is already a separate thread â€” PortAudio's callback pulls
// from AudioOut's internal SPSC ring, so a slow device can never stall DSP.
//
// Threading model (per PLAN.md): DSP blocks stay plain objects; the threads
// and the ring live only here. The GUI never blocks on DSP â€” it polls
// getLatestFrame(), which hands over at most one frame copy under a mutex.
// Audio-chain parameter setters serialize against the DSP thread under one
// internal mutex and take effect at the next block.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp/agc.hpp"
#include "dsp/demod.hpp"
#include "dsp/noise_reduction.hpp"
#include "dsp/notch.hpp"
#include "dsp/rds.hpp"
#include "dsp/resampler.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/spsc_ring.hpp"
#include "dsp/squelch.hpp"
#include "dsp/stereo_fm.hpp"
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

// What the GUI needs to render the RDS panel, as ONE value snapshot so the
// fields it displays can never come from two different instants.
struct RdsSnapshot {
    bool synced = false;                // block synchronisation acquired
    cascade::dsp::RdsState state;       // PI / PS / PTY / RadioText / counters
};

// Forward-declared rather than included: only a pointer is stored here, and
// the plugin ABI header does not need to reach every translation unit that
// includes the pipeline.
class PluginRunner;

class Pipeline {
public:
    struct Config {
        double sampleRateHz = 1000000.0;
        std::size_t fftSize = 1024;    // must satisfy ComplexFFT::isValidSize
        float averagingAlpha = 0.5f;   // EMA weight, clamped by SpectrumEstimator
        // When true the constructor opens the default audio output device at
        // kAudioRateHz. When false no device is ever opened, but the audio
        // chain still runs (samples are counted and tapped, then dropped by
        // the deviceless sink's ring) â€” this is what lets --selftest verify
        // the chain headless on machines with no audio hardware.
        bool audioEnabled = true;
    };

    // Throws std::invalid_argument (from SpectrumEstimator/ComplexFFT) if
    // cfg.fftSize is not a legal FFT size â€” failing at construction beats a
    // dead DSP thread discovered later.
    explicit Pipeline(Config cfg);

    // Attaches (or detaches, with nullptr) the plugin decoder runner. The DSP
    // thread reads this pointer every block, so it is atomic; the RUNNER's own
    // lock is what makes the call itself safe against a concurrent rescan.
    //
    // The caller must detach BEFORE destroying the runner or unloading the
    // plugin modules â€” the DSP thread is still running at that moment, and a
    // pointer it dereferences must not become dangling underneath it.
    void setPluginRunner(PluginRunner* runner) {
        pluginRunner_.store(runner, std::memory_order_release);
    }
    ~Pipeline();                                  // stops if running

    // Non-copyable: owns threads and a live ring.
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    cascade::source::SigGen& sigGen();            // configure tones before/while running
    void start();                                 // idempotent; spawns source-pacing + DSP threads
    // Idempotent. The DSP thread is always joined outright â€” it never enters
    // a vendor driver, so its ~1 ms idle-sleep bounds the wait once the flags
    // clear. The SOURCE thread join is BOUNDED (currently 3 s) and can
    // ABANDON (detach) the thread instead of joining it: a vendor driver's
    // read() can ignore its own timeout and never return at all (field
    // report 4214EAE4), and that must not be allowed to freeze whatever
    // thread called stop() â€” the GUI thread, straight out of the toolbar's
    // Stop button, in the report that motivated this. See the .cpp for the
    // full abandonment policy (zombieSource_) and its consequences for
    // setSource()/~Pipeline().
    void stop();
    bool running() const;

    // Diagnostics/tests: source threads abandoned to a driver that never
    // returned (see quiesceSourceThreadLocked). 0 on every healthy path.
    int abandonedSourceThreads() const {
        return srcThreadsAbandoned_.load(std::memory_order_relaxed);
    }
    // True when a worker thread aborted on an exception â€” a USB SDR pulled
    // mid-stream being the case that motivated it. Latched until start().
    bool faulted() const;
    std::string faultMessage() const;             // empty unless faulted()
    bool getLatestFrame(SpectrumFrame& out);      // true iff a frame newer than out.seq was copied into out

    // --- Source selection ----------------------------------------------------
    // Replaces the IQ source feeding the ring; a null pointer restores the
    // built-in SigGenSource (whose generator sigGen() keeps returning either
    // way). Safe to call while the pipeline runs â€” the swap quiesces the
    // source thread with this handshake, all under controlMutex_:
    //   1. srcRun_ cleared            (the source loop is told to exit)
    //   2. outgoing source stop()     (aborts an in-flight bounded read â€”
    //      this is why IqSource::read must be bounded/abortable: a blocked
    //      read with no abort path would stall the swap for its full bound)
    //   3. srcThread_ joined          (after this NO thread can touch the
    //      outgoing source, so destroying it is race-free by construction)
    //   4. pointer swapped, outgoing source destroyed
    //   5. incoming source start(), srcRun_ set, source thread respawned
    // The DSP thread keeps running throughout, draining whatever the ring
    // still holds, so consumers see at worst a brief frame-rate dip â€” never
    // a stall, a deadlock, or a frame from a half-swapped source.
    //
    // ZOMBIE-SAFETY ADDENDUM. Steps 1-3 assume the outgoing thread is alive
    // and its driver eventually returns from read() â€” precisely what
    // stop()'s own bounded join can no longer promise (see stop()). When a
    // PRIOR stop() gave up waiting and detached the source thread instead of
    // joining it, srcThread_ is not joinable and steps 1-3 above are skipped
    // outright â€” there is nothing left to quiesce. What step 4 must NOT do
    // in that state is destroy the outgoing source: the detached thread may
    // still be parked inside its read(), or about to write one more block
    // through it before it next tests its own generation's stop token (see
    // sourceThreadBody in the .cpp). So the outgoing source is released
    // (deliberately leaked) instead of destroyed â€” the same trade
    // soapy_source.cpp's dead-device policy makes for the identical reason â€”
    // and only THEN is the swap allowed to proceed as normal.
    //
    // Steps 1-3 run whenever the source THREAD exists (joinable), not merely
    // while running() is true. The two differ after a thread fault: the fault
    // clears the run flag from the worker that died and joins nothing, so a
    // swap made from the "Device stopped" state would otherwise destroy a
    // source whose read() is still parked in the driver. Step 5 stays gated
    // on running(), so a faulted or stopped pipeline is never restarted by a
    // swap.
    void setSource(std::unique_ptr<cascade::source::IqSource> s);

    // The source currently feeding the ring (the built-in generator unless
    // setSource installed something else). The reference â€” and any const
    // char* obtained from it, including activeSourceName() â€” stays valid
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
    // WFM de-emphasis in microseconds: 50 (most of the world) / 75 (Americas,
    // South Korea) / 0 = off. Survives mode changes and rate switches.
    void setDeemphasisUs(double us);
    double deemphasisUs() const;

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

    // --- Broadcast-FM stereo (P7) --------------------------------------------
    // User switch, not a capability report: false forces mono (StereoFm's own
    // ramped force-mono gate, so the toggle is click-free). Default true â€”
    // it only ever changes anything in WFM with a locked pilot, which is
    // exactly when a broadcast receiver is expected to produce stereo.
    void setStereoEnabled(bool on);
    bool stereoEnabled() const;

    // Lock-free snapshots for the UI indicator, published once per DSP block
    // exactly like signalPowerDb(). pilotLocked() is the decoder's own lock
    // flag (meaningful only while WFM is running); stereoActive() is what the
    // "ST" indicator should light on: WFM AND enabled AND locked.
    bool pilotLocked() const;
    float pilotLevel() const;
    bool stereoActive() const;

    // --- RDS (P7) -------------------------------------------------------------
    // Snapshot of the decoder fed by the WFM composite. Published by the DSP
    // thread whenever the decoder's counters move (i.e. at most ~11 groups a
    // second, not once per block), and copied out here under its own mutex â€”
    // the GUI never contends with the audio path for audioMutex_ to read it.
    RdsSnapshot rdsSnapshot() const;

    // Discards all RDS and stereo decoder state. MUST be called on every
    // retune: the decoder has no idea the antenna moved, and a lingering PS
    // name from the previous station is a wrong readout, not a cosmetic one.
    // setDemodMode / setVfoOffsetHz / setSource / setInputRateHz already call
    // it; a caller that retunes the SOURCE (the pipeline never sees that) has
    // to call it itself.
    void resetRds();

    // --- Audio post-processing (P7) -------------------------------------------
    // Chain order after the resampler, per channel:
    //     notch -> auto-notch -> noise reduction
    // Rationale: the notches remove deterministic tones FIRST, so (a) the two
    // never fight over the same carrier â€” with the manual notch already on it,
    // the auto-notch's detector no longer sees the tone to chase â€” and (b) the
    // noise-reduction floor tracker gets a spectrum with no surviving
    // heterodyne to mistake for a noise floor. Noise reduction runs last
    // because its output is what the listener hears; anything after it would
    // reintroduce ripple into the floor it just smoothed.
    void setNoiseReductionEnabled(bool on);
    bool noiseReductionEnabled() const;
    void setNoiseReductionStrength(float s01);
    float noiseReductionStrength() const;

    void setNotchEnabled(bool on);
    bool notchEnabled() const;
    void setNotchFrequencyHz(double hz);
    double notchFrequencyHz() const;   // the CLAMPED value actually in use
    void setNotchQ(double q);
    double notchQ() const;

    void setAutoNotchEnabled(bool on);
    bool autoNotchEnabled() const;
    // Readouts for the UI: is the auto-notch currently parked on a tone, and
    // where. Both are lock-free snapshots refreshed once per DSP block.
    bool autoNotchEngaged() const;
    double autoNotchFrequencyHz() const;

    // --- Hard mute of the audio OUTPUT ---------------------------------------
    // Replaces the finished 48 kHz audio with digital silence, at the last
    // point before it fans out. Set by the GUI when a data decoder is running
    // on its own frequency (see plugin_ui.hpp's mute policy); the pipeline
    // itself has no opinion about when that is, and deliberately so - it knows
    // nothing about plugins, presets or what the user last clicked.
    //
    // WHY HERE AND NOT AT THE SINK. The sink is one of four consumers of this
    // signal: the local device, the rolling tap (which is what the web
    // server's audio stream is pumped from - AppWindow::publishWebAudio), the
    // audio recorder, and --selftest's measurement. Muting the sink alone
    // would leave a browser listening to the very hiss the desktop had just
    // been silenced of, which is worse than not muting at all, because the two
    // clients would then disagree about what the radio sounds like.
    //
    // WHY NOT VOLUME. Volume is the user's setting and has to still be there
    // when the mute lifts. This is a separate, temporary override that the
    // user never has to remember to undo.
    //
    // WHAT IT DOES NOT TOUCH: the decoder feed. Plugins are fed further up,
    // before the AGC and the squelch, precisely because they measure rather
    // than listen - so silencing the speakers cannot silence the decoder that
    // caused it. That would be a self-defeating feature, and the test for this
    // asserts both halves in one run.
    //
    // Atomic because it is written from the GUI thread and read by the DSP
    // thread once per block, and a lock for one bool on the hot path would be
    // a mutex the audio callback could contend for.
    void setAudioMuted(bool muted);
    bool audioMuted() const;

    // The audio sink, exposed for GUI wiring: volume, device enumeration, and
    // re-open on device change. The device the constructor opens (when
    // cfg.audioEnabled) is the system default at kAudioRateHz.
    cascade::sink::AudioOut& audio();

    // Opens an output device (-1 = system default) at kAudioRateHz, trying
    // STEREO first and falling back to mono when the device refuses two
    // channels. Use this rather than audio().open() from the GUI: the DSP
    // thread reads the resulting layout under audioMutex_ to decide whether
    // to push interleaved frames or a mono downmix, and going through the
    // sink directly would leave that mirror stale. Returns false only when
    // both attempts fail (the sink is then closed and write() simply drops).
    bool openAudioDevice(int deviceIndex);

    // --- Runtime input-rate follow (rate-follow) ------------------------------
    // Rebuilds the rate-dependent DSP chain for a new input sample rate. The
    // SOURCE is deliberately NOT touched: the caller owns the device side
    // (command the hardware rate through activeSource().setSampleRateHz, or
    // install a source built at the new rate via setSource â€” the built-in
    // generator is fixed-rate by its own contract); this call only makes the
    // DSP side follow.
    //
    // Accepted rates â€” both conditions must hold, otherwise the call returns
    // false and changes NOTHING (the chain keeps running at the old rate):
    //  1. rateHz in [8 kHz, 61.44 MHz] (the range SDR front ends this app
    //     targets can actually deliver);
    //  2. rateHz / decim is an INTEGER, where decim = round(rateHz / 200 kHz)
    //     clamped to >= 1. The integer requirement is what keeps the audio
    //     resampler exact: RationalResampler takes an integer L/M ratio
    //     (channelRate -> 48 kHz, reduced by gcd internally), so a fractional
    //     channel rate could only be approximated, silently detuning audio.
    //     Note the bounds are necessary, not sufficient â€” e.g. 61.44 MHz
    //     itself is refused (decim 307 gives a fractional channel rate).
    // The decim policy lands the channel rate in [150 kHz, 250 kHz] for every
    // accepted rate >= 300 kHz; below 300 kHz decim is 1 and the channel rate
    // equals the input rate.
    //
    // Concurrency (mirrors the setSource quiesce handshake, but for the DSP
    // thread â€” the source thread keeps running throughout): all under
    // controlMutex_,
    //   1. dspRun_ cleared          (the DSP loop is told to exit; run_ stays
    //      set, so the source keeps feeding the ring â€” a full ring drops the
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
    // unchanged and the estimator is rate-agnostic â€” the displayed span
    // simply reinterprets, which is the caller's frequency-axis job), the
    // ring (its capacity stays the construction-time sizing, so headroom in
    // milliseconds shrinks at higher rates â€” still >= 8 FFT blocks for any
    // accepted rate), seq numbering, and the audio tap/counters.
    //
    // Calling with the CURRENT rate is a cheap no-op returning true. Safe to
    // call stopped (no threads to quiesce â€” the chain is rebuilt for the next
    // start()) or running, from any control-plane thread.
    bool setInputRateHz(double rateHz);

    // The input rate the DSP chain is currently built for (construction rate
    // until the first successful setInputRateHz).
    double inputRateHz() const;

    // The audio-side channel rate (the Vfo's output rate = input rate /
    // decimation) â€” the rate the Demodulator runs at and the resampler
    // converts to 48 kHz. Exposed for the GUI's bandwidth limits and for
    // tests of the rate-follow policy.
    double channelRateHz() const;

    // Rate the chain resamples to and the device is opened at.
    static constexpr double kAudioRateHz = 48000.0;

    // --- Test support (used by --selftest) ----------------------------------
    // Total audio samples produced by the chain, counted BEFORE
    // AudioOut::write so the count advances with or without a device.
    std::uint64_t audioSamplesProduced() const;

    // Copies the most recent audio samples (a rolling 4096-FRAME tap taken
    // just before AudioOut::write) into dst in chronological order, as the
    // MONO downmix (L + R)/2. Returns the number copied: min(n, 4096, frames
    // produced so far). The downmix is exact for every mono path â€” L and R
    // are then bit-identical and (a + a)/2 == a in IEEE arithmetic â€” so the
    // --selftest measurement this backs is unchanged by the stereo work.
    std::size_t audioTap(float* dst, std::size_t n) const;

    // Both channels of the same rolling tap, for tests that need to prove the
    // channels actually differ (stereo separation) or actually match (forced
    // mono). Either destination may be null. Returns the frames copied.
    std::size_t audioTapStereo(float* dstLeft, float* dstRight,
                               std::size_t n) const;

    // --- Recorder taps (P6) ---------------------------------------------------
    // Non-owning recorder hooks fed by the DSP thread; nullptr (the default)
    // disconnects. The IQ recorder receives every drained block RAW â€” the
    // ring's baseband samples at inputRateHz(), before the VFO touches them â€”
    // via writeIq, so an IQ recording replayed through IqFileSource
    // reproduces exactly what the spectrum displayed. The audio recorder
    // receives the resampled 48 kHz output via writeAudio at precisely the
    // point audioTap taps: post-squelch, pre-AudioOut, volume-independent.
    //
    // Both pointers live under audioMutex_ â€” the mutex the DSP thread holds
    // across every write* call â€” which is what satisfies the Recorder
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
    // *Main are catch-all wrappers; *Body holds the real loop. See the
    // comment above sourceThreadMain in the .cpp: an exception escaping a
    // std::thread terminates the process, and yanking a USB SDR mid-stream
    // makes vendor drivers throw.
    // `chainRateHz` is a SNAPSHOT of cfg_.sampleRateHz taken by the spawning
    // call under controlMutex_, not read on the thread. setInputRateHz writes
    // that field under controlMutex_ alone and deliberately never joins the
    // source thread, so a read from this thread would be a data race on a
    // double â€” and the value is only ever consulted when a source reports a
    // nonsense rate, which is precisely the path that would then act on a torn
    // one. Passing it through the spawn edge needs no lock and no atomic.
    //
    // `stopToken` is THIS generation's dead-man switch (see the members
    // below and stop() in the .cpp): captured by value at spawn, tested
    // alongside run_/srcRun_ at the top of every loop iteration, and never
    // reassigned for the lifetime of the thread â€” a later generation gets an
    // entirely different shared_ptr, which is what lets stop() abandon a
    // stuck thread without that thread ever mistaking a NEW session's flags
    // for permission to keep running.
    void sourceThreadMain(double chainRateHz,
                          std::shared_ptr<std::atomic<bool>> stopToken);
    void dspThreadMain();
    void sourceThreadBody(double chainRateHz, const std::atomic<bool>& stopToken);
    void dspThreadBody();
    // Mints a fresh per-generation stop token and exit latch (srcStopToken_ /
    // srcExitFuture_ below) and spawns srcThread_ against them, capturing the
    // token BY VALUE into the thread's own lambda. Caller holds
    // controlMutex_, has already set srcRun_ (start() also sets run_ and
    // dspRun_) true, and has already called active_->start() â€” used
    // identically by start() and the setSource() resume path so the two
    // spawn sites cannot drift apart on this contract.
    void spawnSourceThread(double chainRateHz);
    void noteThreadFault(const char* where, const char* what);
    void processAudioBlock(const std::complex<float>* in, std::size_t n);
    // Rebuilds the rate-dependent parts of the stereo/RDS/audio-post chain
    // for a new channel rate. Caller holds audioMutex_.
    void rebuildChannelBlocks(double chanRate);
    // Copies the decoder's state into rdsPublished_ when something moved.
    // Caller holds audioMutex_ (it reads rds_); takes rdsMutex_ internally.
    void publishRds();
    // The body of resetRds(), for callers that ALREADY hold audioMutex_ â€”
    // re-locking it from inside a setter would deadlock instantly.
    void resetDecodersLocked();

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

    // Single latest-frame slot. seq lives inside latest_ and NEVER resets â€”
    // not even across stop()/start() â€” so a consumer that kept its last seen
    // seq keeps receiving frames after a restart. latest_.seq == 0 means
    // "nothing published yet".
    std::mutex frameMutex_;
    SpectrumFrame latest_;

    // Worker-thread fault state. faulted() latches until the next start(),
    // which is what lets the GUI say "the device went away" instead of
    // showing a frozen spectrum that looks like a hang.
    mutable std::mutex faultMutex_;
    std::atomic<bool> faulted_{false};
    std::string faultMsg_;

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
    // Mirrors the demod's de-emphasis so it survives the rebuild that a
    // sample-rate change performs. Guarded by audioMutex_ like demod_ itself.
    double deemphasisUs_ = 50.0;
    // Stereo decoder + RDS decoder, both fed the WFM composite. Held by
    // unique_ptr only so a channel-rate change can replace them wholesale
    // (neither has an assignment operator worth relying on, and both bake the
    // rate into their filter designs).
    std::unique_ptr<cascade::dsp::StereoFm> stereo_;
    std::unique_ptr<cascade::dsp::RdsDecoder> rds_;
    bool stereoEnabled_ = true;
    // Counters of the last published snapshot: the publish is skipped unless
    // the decoder actually advanced, which keeps the per-block cost at one
    // pair of integer compares instead of two string copies.
    std::uint32_t rdsPubGroups_ = 0;
    std::uint32_t rdsPubErrors_ = 0;

    cascade::dsp::Agc agc_;
    cascade::dsp::Squelch squelch_;
    cascade::dsp::PowerMeter meter_;      // S-meter source (channel power)
    // TWO resamplers with identical configuration, one per channel: the
    // resampler is mono, and running the same L/M state machine twice from
    // the same starting state keeps the channels sample-aligned forever.
    cascade::dsp::RationalResampler resampler_;    // left / mono
    cascade::dsp::RationalResampler resamplerR_;   // right
    // The decoder feed's own resampler. Separate because it is driven from a
    // different point in the chain (before the AGC), and a resampler is a
    // state machine â€” interleaving two signals through one corrupts both.
    cascade::dsp::RationalResampler resamplerD_;
    std::vector<float> preAgcBuf_;
    // Audio post-processing, per channel, all at kAudioRateHz (so a
    // rate-follow never rebuilds them). Held by unique_ptr because
    // NoiseReduction and AutoNotch own an FFT plan and are not assignable.
    std::unique_ptr<cascade::dsp::Notch> notchL_;
    std::unique_ptr<cascade::dsp::Notch> notchR_;
    std::unique_ptr<cascade::dsp::AutoNotch> autoNotchL_;
    std::unique_ptr<cascade::dsp::AutoNotch> autoNotchR_;
    std::unique_ptr<cascade::dsp::NoiseReduction> nrL_;
    std::unique_ptr<cascade::dsp::NoiseReduction> nrR_;
    cascade::sink::AudioOut audio_;       // constructed always; device opened
                                          // only when cfg_.audioEnabled
    // Channel layout the sink was last opened with (1 or 2), mirrored under
    // audioMutex_ so the DSP thread never races a GUI device switch.
    int audioChannels_ = 1;
    // FM discriminator scale: rad/sample -> ~0.5 full scale at +/-75 kHz
    // deviation (see pipeline.cpp for the derivation).
    float fmScale_ = 1.0f;
    // Scratch buffers, members so steady-state blocks never allocate.
    std::vector<std::complex<float>> chanBuf_;   // VFO output (channel rate)
    std::vector<float> audioBuf_;                // demod output / composite
    std::vector<float> decoderFeed_;             // mono tap for plugins, pre-notch/NR
    std::atomic<PluginRunner*> pluginRunner_{nullptr};
    std::vector<float> leftBuf_;                 // channel rate, per channel
    std::vector<float> rightBuf_;
    std::vector<float> ilvBuf_;                  // interleaved L,R (2m) for
                                                 // the single Agc + Squelch
    std::vector<std::complex<float>> gateBuf_;   // chanBuf_ duplicated 1->2 so
                                                 // the squelch's power source
                                                 // lines up with ilvBuf_
    std::vector<float> outL_;                    // resampled 48 kHz audio
    std::vector<float> outR_;
    std::vector<float> outIlv_;                  // interleaved 48 kHz for the sink
    std::vector<float> monoOut_;                 // (L+R)/2 for tap + recorder
    // Rolling pre-AudioOut tap window (interleaved L,R; kAudioTapSize FRAMES)
    // + producer-side counters (test support).
    std::vector<float> tapBuf_;
    std::size_t tapWrite_ = 0;   // in frames
    std::size_t tapFilled_ = 0;  // in frames
    std::atomic<float> signalDb_{-200.0f};
    std::atomic<std::uint64_t> audioSamples_{0};
    // UI snapshots, published once per block like signalDb_.
    std::atomic<bool> pilotLocked_{false};
    std::atomic<float> pilotLevel_{0.0f};
    std::atomic<bool> stereoActive_{false};
    std::atomic<bool> autoNotchEngaged_{false};
    // Hard mute of the finished audio; see setAudioMuted. Not under
    // audioMutex_ on purpose - the DSP thread already holds that across the
    // whole block, and a GUI thread that had to take it to set one bool would
    // stall behind a block of DSP for a per-frame update.
    std::atomic<bool> audioMuted_{false};
    std::atomic<double> autoNotchHz_{0.0};
    // Published RDS state. Its own mutex (never held together with any other)
    // so rdsSnapshot() from the GUI cannot stall behind an audio block.
    mutable std::mutex rdsMutex_;
    RdsSnapshot rdsPublished_;
    // Recorder taps (P6): non-owning, audioMutex_-guarded like the chain
    // blocks above â€” see the set*Recorder contract in the public section.
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

    // Per-generation shutdown state for the source thread â€” zombie-safety
    // for stop()'s bounded join (field report 4214EAE4; see stop() and
    // setSource() in the .cpp for the full contract). spawnSourceThread
    // mints a fresh pair at every (re)spawn: srcStopToken_ is captured BY
    // VALUE into that generation's thread lambda, so clearing it here can
    // only ever reach the CURRENT generation, never one already abandoned.
    // srcExitFuture_ is the matching exit latch, fulfilled with
    // set_value_at_thread_exit from inside the same lambda once the OS
    // thread is genuinely about to end â€” not merely once the loop returns.
    // See quiesceSourceThreadLocked() in the .cpp: the one bounded
    // stop-the-source-thread policy, shared by stop(), setSource() and
    // start() so no call site can quietly keep the old unbounded join.
    void quiesceSourceThreadLocked();

    // How many source threads this pipeline has ABANDONED (detached
    // because a driver never returned from read()). Exposed because a
    // test cannot tell a fast join from an instant abandonment by
    // timing alone - and "zero for healthy sessions" is the assertion
    // that keeps kSourceJoinWait honest.
    std::atomic<int> srcThreadsAbandoned_{0};

    std::shared_ptr<std::atomic<bool>> srcStopToken_;
    std::future<void> srcExitFuture_;
    // True once stop() has given up waiting on the source thread and
    // detached it rather than joining. While set, the outgoing external_
    // source must be released() (deliberately leaked), never destroyed or
    // reset: the detached thread may still be parked inside its read() and
    // reference it. Cleared by whichever of setSource()/~Pipeline() performs
    // that release for the generation that set it.
    bool zombieSource_ = false;
};

}  // namespace cascade::core
