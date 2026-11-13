// Real-time render pipeline: a source thread feeds the active IqSource into
// an SPSC ring — pacing it with a real-time clock when the source is
// free-running (the built-in signal generator), letting the device pace when
// it is self-paced (hardware) — a DSP thread drains it through
// SpectrumEstimator and publishes the newest spectrum frame for the GUI to
// poll. The same DSP thread also runs the audio chain (P3/P7): every drained
// block is duplicated into
//   Vfo (decimate to a ~200 kHz channel; see setInputRateHz for the policy)
//   -> Demodulator -> [WFM: RdsDecoder tap, StereoFm matrix] -> Agc
//   -> Squelch -> RationalResampler x2 (channel rate -> 48 kHz)
//   -> Notch -> AutoNotch -> NoiseReduction -> peak Limiter (dsp/limiter.hpp)
//   -> AudioOut (mono or stereo) and Recorder.
//
// THE LIMITER, immediately below the hard mute / patch / plugin takeover and
// immediately above the mono downmix (pipeline.cpp), is the identity for
// anything the Agc already keeps near its target and only engages for the
// sliver of amplitude that would otherwise reach the int16 quantizer or the
// sound device's own hard clip — see the comment at the call site and
// tests/test_audio_clip.cpp for why the Agc alone cannot guarantee that on
// its own for a high-crest-factor signal (wideband noise let in by a widened
// channel filter is exactly that shape).
//
// WFM DE-EMPHASIS OWNERSHIP (the one non-obvious wiring decision here).
// Both the stereo decoder and RDS need the composite (MPX) WITHOUT
// de-emphasis: a 50 us one-pole is ~40 dB down at 57 kHz, which would erase
// the RDS subcarrier outright, and the broadcast standard applies de-emphasis
// per channel AFTER the stereo matrix because the transmitter pre-emphasises
// L and R individually before matrixing. So in WFM the Demodulator's own
// de-emphasis is switched OFF and StereoFm owns it — including for MONO
// output, which is simply StereoFm with its stereo gate closed (L == R
// exactly). Routing mono through the same object is what makes toggling
// stereo, or losing pilot lock, inaudible in tone: the de-emphasis and the
// 15 kHz low-pass are identical on both sides of the switch, and StereoFm's
// gate ramps the difference channel in and out instead of hard-switching.
// The alternative (a second Demodulator instance purely for the MPX taps)
// would leave two different de-emphasis paths for mono and stereo audio and
// pay for a second discriminator; this way there is exactly one.
//
// NFM IS THE OTHER HALF OF THAT RULE, and it was missing. StereoFm runs only
// in WFM, so forwarding the user's setting to StereoFm and nowhere else meant
// NFM had no de-emphasis at all — the Radio panel's De-emph combo is enabled
// for both FM modes and is saved to the config, and in NFM it changed nothing.
// The setting now goes to the Demodulator as well, and one function —
// applyDemodDeemphasisLocked — decides which of the two owns it: WFM keeps
// its 0, every other mode carries the user's constant.
//
// AGC AND SQUELCH RUN ON THE INTERLEAVED STEREO STREAM, one instance each,
// so both channels always receive the identical gain and the identical gate
// envelope — two per-channel instances would each normalise their own channel
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
// Playback itself is already a separate thread — PortAudio's callback pulls
// from AudioOut's internal SPSC ring, so a slow device can never stall DSP.
//
// Threading model (per PLAN.md): DSP blocks stay plain objects; the threads
// and the ring live only here. The GUI never blocks on DSP — it polls
// getLatestFrame(), which hands over at most one frame copy under a mutex.
// Audio-chain parameter setters take effect at the next block. They do NOT wait
// for the DSP thread: see "SETTERS POST, THEY DO NOT WAIT" below.
//
// AND THE PARAMETER GETTERS TAKE NO LOCK AT ALL, which is not an optimisation.
//
// processAudioBlock() holds audioMutex_ across a WHOLE block - the VFO's FIR at
// the input rate, the demodulator, the stereo matrix, RDS, the AGC, the
// squelch, three resamplers, the notches, the noise reduction and the sink
// write. On a machine that keeps up that is a few tens of microseconds per
// block and nobody notices. On one that does not, the blocks run back to back
// and the mutex is held essentially all of the time.
//
// The GUI reads these parameters EVERY FRAME - vfoOffsetHz() alone from eleven
// places in app_window.cpp, inputRateHz() from thirteen, channelRateHz() from
// eight, and currentConfig() once more on top of that for the debounced config
// save. Each of those used to be a std::mutex acquisition queued behind the DSP
// thread, so a machine whose DSP could not keep up convoyed its RENDER thread
// behind the audio chain. Field report "hang ntdll.dll @
// cascade::core::Pipeline::vfoOffsetHz" (0.96.2, an NESDR SMArt v5 starving its
// audio callback 110 times a minute) is exactly that: the GUI thread stalled
// over five seconds inside AppWindow::run -> maybeSaveConfig -> currentConfig
// -> vfoOffsetHz -> a mutex wait, and the hang watchdog filing a report against
// an application whose only fault was being on a machine that was too slow.
//
// So every getter the GUI polls per frame reads an ATOMIC MIRROR, published by
// whichever setter changed the value, under the same lock, at the moment it
// changed - publishParamMirrorsLocked() below, one function so a new setter
// cannot mirror half of what it wrote. The setters' semantics are untouched:
// they still serialize against the DSP thread and still take effect at the next
// block. What changes is only that READING a parameter can no longer wait for
// one.
//
// SETTERS POST, THEY DO NOT WAIT. A setter cannot read an atomic: it has to
// change the chain, and the chain is guarded by the very mutex above. Field
// reports "hang ntdll.dll @ cascade::core::Pipeline::resetRds" (a retune from the
// toolbar readout) and "... Pipeline::setDeemphasisUs" (a plugin preset that woke
// ~28 decoders at once), both 0.99.81, are the GUI thread parked over five
// seconds in a mutex wait inside a setter: processAudioBlock() holds audioMutex_
// across every I/Q decoder plugin's process(), so with thirty decoders on a
// machine that cannot keep up the DSP thread is inside the lock nearly all of the
// time, and std::mutex is not fair - the DSP thread's next lock() beats the
// waiting GUI thread's again and again.
//
// So a setter builds a small command (the value is captured BY VALUE), appends it
// to cmdQueue_ under cmdMutex_ - a lock held for a vector push or swap, never for
// DSP work - and only then TRIES audioMutex_ without waiting. If it gets it (no
// block in flight) it applies the queue itself, in order, exactly as the old
// blocking setter did. If it does not, the DSP thread applies the queue at the top
// of its next block, under the lock it already holds. Either way the call returns
// in microseconds, the change reaches the chain between two blocks (never inside
// one: no torn parameter set), and commands apply in the order they were posted.
//
// What a caller can still observe: a getter read straight after a setter returns
// the value just set for every parameter whose mirror is verbatim (offset, mode,
// de-emphasis, the on/off switches - stored with the post). The few that the live
// object CLAMPS (noise-reduction strength, the notch's frequency and Q) show the
// previous value until the command has run, at most one block later when the DSP
// thread is running; that is the same "what is actually in use" the getter has
// always meant. NOT posted, because their contract is that the call has FINISHED
// when it returns: the recorder pointers (a caller destroys the recorder next) and
// the rate switch (it joins the DSP thread by design).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/spectrum_frame.hpp"
#include "dsp/agc.hpp"
#include "dsp/ctcss.hpp"
#include "dsp/dcs.hpp"
#include "dsp/tone_squelch.hpp"
#include "dsp/demod.hpp"
#include "dsp/noise_reduction.hpp"
#include "dsp/nr_methods.hpp"
#include "dsp/notch_bank.hpp"
#include "dsp/audio_eq.hpp"
#include "dsp/noise_squelch.hpp"
#include "dsp/notch.hpp"
#include "dsp/rds.hpp"
#include "core/patch_runner.hpp"
#include "dsp/resampler.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/spectrum_block.hpp"
#include "dsp/iq_correct.hpp"
#include "dsp/afc.hpp"
#include "dsp/zoom_fft.hpp"
#include "dsp/cw_filter.hpp"
#include "dsp/stereo_place.hpp"
#include "dsp/spsc_ring.hpp"
#include "dsp/squelch.hpp"
#include "dsp/oirt_stereo.hpp"
#include "dsp/stereo_fm.hpp"
#include "dsp/vfo.hpp"
// The demod scope's two rolling taps. A header, no .cpp: the whole thing is a
// template over the element type, because one tap carries floats and the
// other carries complex.
#include "core/scope_tap.hpp"
#include "sink/audio_out.hpp"
#include "sink/drift_matcher.hpp"
#include "core/freq_converter.hpp"
#include "source/converter_view.hpp"
#include "source/iq_source.hpp"
#include "source/siggen_source.hpp"

namespace cascade::core {

namespace beats {
class Registration;  // core/thread_beats.hpp: the source thread's heartbeat is passed down by reference
}

// Forward declaration (core/recorder.hpp): the pipeline stores only
// non-owning pointers, so the full type is needed in pipeline.cpp alone.
class Recorder;

// (SpectrumFrame moved to core/spectrum_frame.hpp with the fence: the window holds the frame it drew.)

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
        // the deviceless sink's ring) — this is what lets --selftest verify
        // the chain headless on machines with no audio hardware.
        bool audioEnabled = true;
    };

    // Throws std::invalid_argument (from SpectrumEstimator/ComplexFFT) if
    // cfg.fftSize is not a legal FFT size — failing at construction beats a
    // dead DSP thread discovered later.
    explicit Pipeline(Config cfg);

    // Attaches (or detaches, with nullptr) the plugin decoder runner. The DSP
    // thread reads this pointer every block, so it is atomic; the RUNNER's own
    // lock is what makes the call itself safe against a concurrent rescan.
    //
    // The caller must detach BEFORE destroying the runner or unloading the
    // plugin modules — the DSP thread is still running at that moment, and a
    // pointer it dereferences must not become dangling underneath it.
    void setPluginRunner(PluginRunner* runner) {
        pluginRunner_.store(runner, std::memory_order_release);
    }
    // The runner attached now, or null. For READING its status from the
    // thread that owns the pipeline (core/engine_measure.cpp asks whether the
    // decoders it is meant to be measuring beside are actually being fed); the
    // same detach-before-destroy rule as above keeps the pointer valid there.
    PluginRunner* pluginRunner() const { return pluginRunner_.load(std::memory_order_acquire); }
    ~Pipeline();                                  // stops if running

    // Non-copyable: owns threads and a live ring.
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    cascade::source::SigGen& sigGen();            // configure tones before/while running
    void start();                                 // idempotent; spawns source-pacing + DSP threads
    // Idempotent. The DSP thread is always joined outright — it never enters
    // a vendor driver, so its ~1 ms idle-sleep bounds the wait once the flags
    // clear. The SOURCE thread join is BOUNDED (currently 3 s) and can
    // ABANDON (detach) the thread instead of joining it: a vendor driver's
    // read() can ignore its own timeout and never return at all (field
    // report 4214EAE4), and that must not be allowed to freeze whatever
    // thread called stop() — the GUI thread, straight out of the toolbar's
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
    // Diagnostics, and the usage record's `recovered.dspexc`: how many times the
    // DSP thread threw an exception that dspThreadMain caught (the pipeline then
    // stops itself and says so; see noteThreadFault). 0 on every healthy path. Only
    // a counter: the window's own poll turns a rise into the count, so nothing
    // here counts from the signal thread (core/health_events.hpp).
    int dspThreadExceptions() const {
        return dspExceptions_.load(std::memory_order_relaxed);
    }
    // What the DSP thread's request for scheduling priority came to the last time
    // that thread started (0.99.73, core/thread_priority.hpp): "mmcss",
    // "above-normal", "sched-rr" or "none: <reason>"; empty until a DSP thread
    // has started. The same words are in the log (`dsp: thread priority ...`).
    // sourceThreadPriority() is the same for the thread that fills the I/Q ring.
    std::string dspThreadPriority() const;
    std::string sourceThreadPriority() const;
    // Diagnostics/tests (0.99.73): the correction the DriftMatcher is asking for
    // right now, in ppm - positive while it plays more audio than arrives to
    // deepen the sink's ring towards AudioOut::targetFrames(), negative while the
    // ring is deeper than that. What a test reads to see that the pipeline hands
    // the matcher the SINK's target and not a constant.
    double driftCorrectionPpm();
    // Diagnostics/tests: samples the source thread read but the ring had no
    // room for (SpscRing::write accepts only what fits). Never reset, so a
    // caller measures a window by taking a difference. Before this counter
    // existed the loss was silent: a ring too small for one source read
    // dropped part of every chunk and nothing anywhere said so.
    std::uint64_t ringDroppedSamples() const {
        return ringDropped_.load(std::memory_order_relaxed);
    }
    // Diagnostics/tests (0.99.86): every sample the source thread has read and
    // offered to the ring, kept or dropped - the denominator ringDroppedSamples()
    // is a share of. Never reset, measured by difference like the drop count.
    // GitHub issue 6: the once-a-minute log line says what share of the minute's
    // input was lost (AppWindow::closeDspMinute).
    std::uint64_t sourceSamplesRead() const {
        return srcSamplesRead_.load(std::memory_order_relaxed);
    }
    // Diagnostics/tests: the ring's current capacity in samples. Sized from
    // the SOURCE's rate each time a source thread is spawned - a quarter of a
    // second of it since GitHub issue 6 (ringCapacityFor in the .cpp) - never
    // below the construction-time size (see ringCapacityForActiveSourceLocked).
    std::size_t ringCapacity();
    // THE RING AND THE DSP THREAD, AS A MINUTE'S DROP LINE AND THE BUNDLE READ
    // THEM (GitHub issue 6, the 0.99.90 report; AppWindow::closeDspMinute and
    // DiagBundleInput). Three figures beside the drop count: how many writes
    // lost part of themselves (one write per source read, so twenty big holes
    // and eight hundred tiny ones read differently), the fullest the ring has
    // been (how near the edge a minute that lost nothing still came), and the
    // longest the DSP thread spent on one block (a stall on the DSP side, as
    // against a run of samples arriving late from the source). The two maxima
    // are windows: take*() returns the mark and starts a new one; the plain
    // getter only looks. Never reset otherwise.
    std::uint64_t ringCutWrites() const;
    std::size_t ringHighWaterSamples() const;
    std::size_t takeRingHighWaterSamples();
    std::uint64_t dspBlockLongestNs() const;
    std::uint64_t takeDspBlockLongestNs();
    // True when a worker thread aborted on an exception — a USB SDR pulled
    // mid-stream being the case that motivated it. Latched until start().
    bool faulted() const;
    std::string faultMessage() const;             // empty unless faulted()
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
    // a stall, a deadlock, or a frame from a half-swapped source. The one
    // exception is an incoming source whose rate needs a different ring size
    // (one read is 10 ms of the SOURCE's rate): then, between steps 5's
    // start() and the respawn, the DSP thread is joined, the ring replaced,
    // and the DSP thread restarted once the new source thread exists.
    //
    // ZOMBIE-SAFETY ADDENDUM. Steps 1-3 assume the outgoing thread is alive
    // and its driver eventually returns from read() — precisely what
    // stop()'s own bounded join can no longer promise (see stop()). When a
    // PRIOR stop() gave up waiting and detached the source thread instead of
    // joining it, srcThread_ is not joinable and steps 1-3 above are skipped
    // outright — there is nothing left to quiesce. What step 4 must NOT do
    // in that state is destroy the outgoing source: the detached thread may
    // still be parked inside its read(), or about to write one more block
    // through it before it next tests its own generation's stop token (see
    // sourceThreadBody in the .cpp). So the outgoing source is released
    // (deliberately leaked) instead of destroyed — the same trade
    // soapy_source.cpp's dead-device policy makes for the identical reason —
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
    // setSource installed something else). The reference — and any const
    // char* obtained from it, including activeSourceName() — stays valid
    // only until the next setSource(), which may destroy the object; per the
    // IqSource threading contract both are meant for the GUI/control thread,
    // the same thread that performs swaps.
    //
    // IT SPEAKS AIR FREQUENCIES (0.99.36). What comes back is the converter
    // view (source/converter_view.hpp) over that source: with an up- or
    // down-converter set, centerFrequencyHz() is where the signal really is
    // and setCenterFrequencyHz() takes that same air frequency, telling the
    // radio the converted one. With no converter - the state every install
    // starts in - it is the source exactly. Everything the application tunes
    // or reads goes through here, which is what makes it the one translation
    // layer; the radio's own figure is rawSource()'s.
    cascade::source::IqSource& activeSource();
    const char* activeSourceName();

    // The source itself, speaking the RADIO's frequency. For the few readers
    // that must say what the radio was told (the status column's "radio at
    // ..." line); never for tuning.
    cascade::source::IqSource& rawSource();

    // --- The converter in front of the radio (0.99.36) -----------------------
    // See core/freq_converter.hpp. GUI/control thread. setSource() puts it back
    // to OFF on every swap, so a converter never follows one radio onto the
    // next: the caller applies the new radio's own remembered setting after
    // the swap. Changing it does not retune the radio - the readback is simply
    // read through the new setting - which is what the caller wants when the
    // user switches a converter on over a radio already tuned to its output.
    void setConverter(const ConverterSetting& s);
    ConverterSetting converter();

    // --- The crystal correction by retuning (0.99.56) ------------------------
    // See core/ppm_correction.hpp. GUI/control thread. The same rules as the
    // converter: setSource() puts it back to 0 on every swap, the caller
    // applies the new radio's own value after the swap (and only for a radio
    // that cannot correct its own crystal), and changing it does not retune.
    // `memo`: a tune already made at this correction (ConverterView::setPpm).
    void setSoftwarePpm(double ppm, const PpmMemo& memo = {});
    double softwarePpm();

    // --- Audio chain control (P3) -------------------------------------------
    // All of these are callable from any thread while the pipeline runs. They
    // never wait for the DSP thread: the change is queued and applied between two
    // audio blocks (~0.5 ms later on a machine that keeps up) - see "SETTERS POST,
    // THEY DO NOT WAIT" at the top of this file.

    // Selects the demodulator (default WFM). Also resets the AGC: a mode
    // switch changes the audio level regime, and carrying the old gain over
    // would blast or mute the first moments of the new mode.
    void setDemodMode(cascade::dsp::DemodMode m);
    cascade::dsp::DemodMode demodMode() const;
    // FM de-emphasis in microseconds: 50 (most of the world) / 75 (Americas,
    // South Korea) / 0 = off. Applies to BOTH FM modes — WFM through StereoFm,
    // NFM through the Demodulator's own one-pole (see the ownership note at the
    // top of this header). Survives mode changes and rate switches.
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

    // Whether the squelch gate is OPEN - the gate's own state, not a
    // comparison of the S-meter with the threshold. They differ exactly where
    // it matters: the gate opens above the threshold but closes only 3 dB
    // below it and holds for 100 ms, so the S-meter sitting just under the
    // threshold can still be passing audio. A closed gate is a complete
    // silence downstream: the audio chain keeps FEEDING the sink (zeros, at the
    // normal rate), so nothing starves, nothing is logged, and every other
    // light on the receiver - spectrum, waterfall, S-meter - stays live. It is
    // the state a "no audio" report most needs and, before this, the one
    // nothing recorded. Published once per DSP block into an atomic: a
    // lock-free snapshot safe from any thread, false until the first block
    // after start().
    bool squelchOpen() const;

    // --- Broadcast-FM stereo (P7) --------------------------------------------
    // User switch, not a capability report: false forces mono (StereoFm's own
    // ramped force-mono gate, so the toggle is click-free). Default true —
    // it only ever changes anything in WFM with a locked pilot, which is
    // exactly when a broadcast receiver is expected to produce stereo.
    void setStereoEnabled(bool on);
    bool stereoEnabled() const;
    // THE STEREO SYSTEM (engine wiring W2; dsp/oirt_stereo.hpp): which composite decoder WFM runs. 1 = the pilot-tone system (every receiver the world over but
    // the OIRT band), 2 = OIRT polar modulation (65.8 to 74 MHz in Russia, Belarus, Ukraine, Moldova, Armenia). The ENGINE decides which (automatic by the tuned
    // frequency, or the user's choice) and says so here; the default is 1, the behaviour before this existed. A value other than 1 or 2 is ignored. The OIRT decoder
    // is fed the discriminator scaled so +-1 is kOirtMaxDeviationHz (50 kHz, the OIRT system's maximum deviation; UNVERIFIED against the standard's table).
    static constexpr double kOirtMaxDeviationHz = 50000.0;
    void setStereoSystem(int system);
    int stereoSystem() const;
    // True while the OIRT decoder is the one in the path (WFM, system 2, a channel rate it can decode at).
    bool oirtActive() const;

    // Lock-free snapshots for the UI indicator, published once per DSP block
    // exactly like signalPowerDb(). pilotLocked() is the decoder's own lock
    // flag (meaningful only while WFM is running); stereoActive() is what the
    // "ST" indicator should light on: WFM AND enabled AND locked.
    double pilotOffsetHz() const;   // the stereo decoder's pilot PLL residual, Hz (R18); 0 when not WFM or not locked
    bool pilotLocked() const;
    float pilotLevel() const;
    bool stereoActive() const;

    // --- RDS (P7) -------------------------------------------------------------
    // Snapshot of the decoder fed by the WFM composite. Published by the DSP
    // thread whenever the decoder's counters move (i.e. at most ~11 groups a
    // second, not once per block), and copied out here under its own mutex —
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
    // never fight over the same carrier — with the manual notch already on it,
    // the auto-notch's detector no longer sees the tone to chase — and (b) the
    // noise-reduction floor tracker gets a spectrum with no surviving
    // heterodyne to mistake for a noise floor. Noise reduction runs last
    // because its output is what the listener hears; anything after it would
    // reintroduce ripple into the floor it just smoothed.
    void setNoiseReductionEnabled(bool on);
    bool noiseReductionEnabled() const;
    void setNoiseReductionStrength(float s01);
    float noiseReductionStrength() const;

    // --- AGC choice -----------------------------------------------------------
    // Auto (the default) is the loop this pipeline has always had, sample for
    // sample; Fast, Medium and Slow are constant-slew loops whose recovery time
    // is the same at every channel rate; Off freezes the gain (see dsp/agc.hpp).
    // Posted like noise reduction: the setter returns at once, the DSP thread
    // applies it between blocks, the getters read mirrors.
    //   setAgcMode        the mode takes effect with the gain held at that moment
    //                     (entering Off freezes it).
    //   setAgcFixedGain   dB above unity, clamped to -20..+40; it is the gain Off
    //                     applies, and takes effect at once while the mode is Off.
    //                     Set it AFTER the mode: entering Off freezes the held
    //                     gain, which a fixed gain set earlier does not survive.
    //   agcMode / agcFixedGainDb  what is in use (the fixed gain is the clamped,
    //                     live value, published when the command has run).
    //   agcGainDb         the gain applied right now, dB, published once per block.
    void setAgcMode(cascade::dsp::Agc::Mode mode);
    cascade::dsp::Agc::Mode agcMode() const;
    void setAgcFixedGain(float db);
    float agcFixedGainDb() const;
    float agcGainDb() const;

    // --- Noise blanker ----------------------------------------------------------
    // Off by default. Removes short wide-band clicks at the channel rate, in the
    // Vfo, before the narrow filter (see dsp/noise_blanker.hpp and Vfo's hook).
    // EVERYTHING fed from the channel hears the blanked signal - audio, S-meter,
    // scope and decoders on this radio; the raw block (I/Q recorder, I/Q plugins,
    // patch) is upstream and is not touched. Posted like noise reduction.
    //   setNoiseBlankerEnabled       on/off (verbatim mirror).
    //   setNoiseBlankerThresholdDb   dB above the average magnitude, clamped to
    //                                6..30 (default 15); the getter reports the
    //                                clamped value once the command has run.
    //   noiseBlankerBlankedPercent   share of the last 100 ms blanked, percent;
    //                                0 when off or standing down.
    //   noiseBlankerStandingDown     true while more than a quarter of the last
    //                                100 ms would be blanked and the signal is
    //                                therefore let through.
    // Published once per block.
    void setNoiseBlankerEnabled(bool on);
    bool noiseBlankerEnabled() const;
    void setNoiseBlankerThresholdDb(float db);
    float noiseBlankerThresholdDb() const;
    float noiseBlankerBlankedPercent() const;
    bool noiseBlankerStandingDown() const;

    // TONE SQUELCH (CTCSS / DCS; engine wiring lane 1, dsp/tone_squelch.hpp has the rules). NFM only: in any other mode the setting is kept and does nothing. The
    // detector listens to the discriminator output at the channel rate, before the AGC and the squelch; while a tone squelch is on the squelch's gate needs the
    // tone as well as the carrier. Posted like the other settings; the detectors are rebuilt for the channel rate by rebuildChannelBlocks.
    //   setToneSquelch      kind 0 Off, 1 CTCSS (hz: 0 any tone, else one of the 50), 2 DCS (dcsDigits: 0 any code, else one of the 83 written as digits, 23 for
    //                       023; polarity 0 any, 1 normal, 2 inverted). False, and nothing changed, for a value outside those.
    //   setToneRemove       a high-pass at the audio rate that takes the sub-audible tone out of what is heard (off by default).
    //   toneKind ... toneRemove      the settings as asked (verbatim mirrors).
    //   toneDetectedHz / toneDetectedCode / toneDetectedPolarity   what the detector has locked on now (0 when none) - measured, once per block.
    //   toneGateOpen        the tone condition: true while no tone is required or the required tone is present - measured.
    //   carrierSquelchOpen  the carrier gate alone; squelchOpen() is the same (kept for the auto-squelch and the heard-time counters).
    //   audioGateOpen       the carrier AND the tone: the gate the listener hears.
    // NOISE-REDUCTION METHOD (engine wiring lane 1): 0 the standard noise reduction, 1 minimum-statistics spectral subtraction, 2 the adaptive line enhancer. The on/off switch and
    // the strength are setNoiseReductionEnabled / Strength's for every method. The line enhancer is for CW and steady carriers and is applied in CW and RAW only; in any other mode the
    // standard one runs in its place (noiseReductionMethodActive says which is running). False (nothing changed) for a method past 2.
    // THE MANUAL NOTCH BANK (engine wiring lane 1; dsp/notch_bank.hpp): up to ten notches on the finished audio of each channel, after the single notch. A slot moves, widens or switches
    // on and off without a click (the block glides and fades). False for a slot past 9 or a number that is not one. Any thread; nothing is posted (the bank's settings are atomics).
    bool setManualNotch(int slot, double hz, double widthHz, bool on);
    // The CW sidetone pitch (engine wiring lane 1): 300 to 1200 Hz, default 700. False (nothing changed) outside that or for a number that is not one. Posted; kept across a rate change.
    // THE FM NOISE SQUELCH (engine wiring lane 1; dsp/noise_squelch.hpp): NFM only, a second gate beside the level squelch. The detector listens to the discriminator output at the channel
    // rate (here after the user's de-emphasis and the FM scaling, so the threshold is a number of this chain, not a universal one: the screen shows noiseSquelchLevelDb beside it);
    // its gain multiplies the audio after the level squelch. False for a threshold outside -90..0 dB or a tail outside 0..2000 ms. Posted; kept across a rate rebuild.
    bool setNoiseSquelch(bool on, double thresholdDb, double tailMs);
    bool noiseSquelchOn() const;
    double noiseSquelchLevelDb() const;   // measured, once per block
    bool noiseSquelchOpen() const;
    // THE AUDIO EQUALISER AND FILTERS (engine wiring lane 1; dsp/audio_eq.hpp) on the finished audio of each channel, after the noise reduction: bands 0..7 (bass, middle, treble, then the
    // graphic 100 / 300 / 1000 / 2500 / 5000 Hz) +-12 dB, a high-pass (0, 100, 300 Hz) and a low-pass (0, 2400, 3000, 6000 Hz). All flat and off: the samples are not touched. False for a value
    // outside those. The block's settings are atomics: nothing is posted.
    bool setEqBand(int band, double db);
    bool setAudioFilters(int highPassHz, int lowPassHz);
    bool eqBypassed() const;
    bool setCwPitchHz(double hz);
    double cwPitchHz() const;
    // THE SPECTRUM'S SIZE, WINDOW AND FRAME RATE (engine serial lane A, rows S1 S2 S3; dsp/spectrum_block.hpp). fftSize: a power of two, 1,024 to 131,072; window: 0 rectangular, 1 Hann, 2 Hamming,
    // 3 Blackman-Harris (the default), 4 flat-top; frameRateHz: 0 = automatic (30 a second on the block path), else 1 to 120. 1,024 / Blackman-Harris / automatic is the LEGACY estimator (the frame every
    // drained block, as it has always been); anything else runs the spectrum block, fed the same samples, its tone level lowered by the legacy estimator's (uncompensated) Blackman-Harris offset so a
    // choice never moves a displayed level. The audio chain is not touched: its block stays the construction FFT size. False (nothing changed) for a value outside those. Any control thread.
    bool setSpectrum(std::size_t fftSize, int window, double frameRateHz);
    std::size_t spectrumFftSize() const;
    int spectrumWindow() const;
    double spectrumFrameRateHz() const;
    // The bins of the newest frame (the ring's reader sees a frame of this many): fftSize on both paths.
    std::size_t spectrumBinCount() const;
    // I/Q CORRECTION AT THE SOURCE RATE (engine serial lane A, row F16; dsp/iq_correct.hpp): mask bit 0 removes the DC offset, bit 1 balances gain and phase (blind image rejection,
    // for a signal alike either side of its centre), bit 2 swaps I and Q. It runs on the drained block in place BEFORE the spectrum, the audio chain and the I/Q recorder see it, and
    // costs nothing while the mask is 0 (the default: the samples are untouched). False (nothing changed) for a mask above 7. Any thread; kept across a rate change.
    // AUTOMATIC FREQUENCY CONTROL (engine serial lane A, row F13; dsp/afc.hpp): follows a drifting carrier by nudging the VFO. NFM, WFM and AM track the block's mean frequency, CW the strongest
    // line (a keyed carrier); the other modes have no carrier to follow and are left alone. pullInHz (50 to 20,000) is how far from the tuned frequency a carrier is caught, and twice that how
    // far it is followed once caught. The nudge is added to the user's offset (vfoOffsetHz() stays the user's own) and a retune forgets it. False (nothing changed) outside the range.
    // ZOOM FFT OF THE IF PASSBAND (engine serial lane A, row S12; dsp/zoom_fft.hpp): a second, high-resolution spectrum of the channel (the IF after the VFO, centred on DC), fed at the channel rate.
    // fftSize 0 turns it off; otherwise a power of two from 256 to 16384. spanHz is the width of the whole picture (100 Hz up to the channel rate); the decimation is the channel rate over it, rounded, so the span
    // actually shown is zoomFrame()'s outputRateHz. Rebuilt with the channel blocks (a change of input rate keeps the setting). Returns false for a value out of range. Any thread.
    struct ZoomFrameInfo {
        std::uint64_t frame = 0;        // 0 = none yet
        std::size_t bins = 0;
        double binHz = 0.0;
        double spanHz = 0.0;            // the whole picture (the output rate of the zoom decimator)
        double usableSpanHz = 0.0;      // the middle part that is clean (80 %)
    };
    bool setIfZoom(std::size_t fftSize, double spanHz);
    // Copies the newest zoom frame (bins dB values, fftshifted: bin bins/2 is DC) into out (capacity cap); frame 0 = none (off, or not a full frame yet) or cap too small.
    ZoomFrameInfo zoomFrame(float* out, std::size_t cap) const;
    std::size_t ifZoomFftSize() const;   // 0 = off
    bool setAfc(bool on, double pullInHz);
    bool afcOn() const;
    double afcPullInHz() const;
    double afcCorrectionHz() const;   // what is added to the VFO now (0 while off, unlocked or in a mode without a carrier)
    bool afcLocked() const;
    // THE CW PEAK FILTER (engine serial lane A, row F8; dsp/cw_filter.hpp CwPeakFilter): an audio peaking filter centred on the CW pitch (it follows setCwPitchHz), width 30 to 500 Hz, boost 0 to
    // 24 dB, on the finished audio of CW mode only (after the equaliser; the decoder tap is far upstream and never sees it). Off, or in any other mode, it is a bit-exact no-op. False (nothing
    // changed) for a width or boost outside those. Any thread.
    bool setCwPeak(bool on, double widthHz, double boostDb);
    bool cwPeakOn() const;
    double cwPeakWidthHz() const;
    double cwPeakBoostDb() const;
    // STEREO PLACEMENT OF A MONO RECEIVER (engine serial lane A, rows F20 and D5; dsp/stereo_place.hpp): mode 0 off (left = right, as it always was), 1 pan (value -1 left ear only .. +1 right ear
    // only, constant power), 2 pseudo-stereo (value 0..1, the amount: complementary combs, a mono sum keeps the signal), 3 binaural (value -90..+90 degrees of azimuth: a time and level difference).
    // Runs on the finished audio after the equaliser, and only while the audio is mono: a stereo broadcast keeps its own two channels. False (nothing changed) outside those. Any thread.
    bool setStereoPlace(int mode, double value);
    int stereoPlaceMode() const;
    double stereoPlaceValue() const;
    // THE CHANNEL FILTER'S SHAPE (engine serial lane A, row F6; Vfo::setChannelShape): 0 the filter the VFO has always had, 1 sharp, 2 medium, 3 soft. Applies where the VFO has a narrow stage (a bandwidth under a quarter of the
    // channel rate); channelShapeActive() says whether it does now. Posted; kept across a rate change. False (nothing changed) outside 0..3.
    bool setChannelShape(int shape);
    int channelShape() const;
    bool channelShapeActive() const;
    // THE FM MULTIPLEX AS A FILE (engine serial lane A, row D4): in WFM the composite (the discriminator's output before the stereo decoder: mono, the 19 kHz pilot, the 38 kHz difference, the 57 kHz RDS)
    // is resampled to 192 kHz and handed to this recorder (an Audio take, so a 32-bit float WAV when its sample format is set so) for a program such as RDS Spy to open. Nothing is written in any
    // other mode. Like setIqRecorder: after the setter returns no write against the old recorder is in flight; nullptr stops it. Any thread.
    static constexpr double kMpxRateHz = 192000.0;
    void setMpxRecorder(Recorder* r);
    // THE IF AS A FILE (engine serial lane A, row P4): the channel after the VFO has mixed it to DC and the channel filter has taken everything but the passband out, at the channel rate (channelRateHz()), as
    // I/Q to this recorder (a BasebandIq take: 32-bit float or 16-bit integer by its sample format). Before the demodulator, so it is the same in every mode; the squelch does not gate it. Like setIqRecorder:
    // after the setter returns no write against the old recorder is in flight; nullptr stops it. A change of input rate changes the channel rate: the engine ends the take then. Any thread.
    void setIfRecorder(Recorder* r);
    bool setIqCorrection(int mask);
    int iqCorrection() const;
    double iqImageRejectionDb() const;   // the balance estimator's reading of the input, dB (200 while nothing is measured)
    int manualNotchCount() const;   // slots that are on
    bool setNoiseReductionMethod(int method);
    int noiseReductionMethod() const;
    int noiseReductionMethodActive() const;   // measured, once per block: 0 while the noise reduction is off
    bool setToneSquelch(int kind, double hz, int dcsDigits, int polarity);
    void setToneRemove(bool on);
    int toneKind() const;
    double toneHz() const;
    int toneDcsDigits() const;
    int tonePolarity() const;
    bool toneRemove() const;
    double toneDetectedHz() const;
    int toneDetectedCode() const;
    int toneDetectedPolarity() const;
    bool toneGateOpen() const;
    bool audioGateOpen() const;

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
    // point before it fans out. Set by the GUI for the user's Mute key and
    // transmit monitoring; data-decoder carrier suppression is separate below.
    //
    // WHY HERE AND NOT AT THE SINK. The sink is one of four consumers of this
    // signal: the local device, the rolling tap (which is what the web
    // server's audio stream is fed from - the engine's read_audio ring, ApiHost::feedAudioLocked), the
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
    // A data decoder suppresses the demodulated carrier, while a plugin's
    // decoded programme remains audible. User and transmit mute stay above.
    void setDemodAudioMuted(bool muted);
    // Whether carrier suppression currently silences the finished output.
    // Decoded plugin or patch audio can still replace the quiet carrier.
    bool pluginAudioMuted() const;

    // The audio sink, exposed for GUI wiring: volume, device enumeration, and
    // re-open on device change. The device the constructor opens (when
    // cfg.audioEnabled) is the system default at kAudioRateHz.
    // THE PATCH. The GUI thread builds a set of strips and publishes it;
    // this thread adopts it at a block boundary. See core/patch_runner.hpp
    // for why the hand-off is shaped the way it is - in short, the audio
    // thread must never be able to queue behind a GUI thread that is
    // allocating.
    cascade::core::patch::Runner& patchRunner() { return patch_; }

    cascade::sink::AudioOut& audio();

    // Opens an output device (-1 = system default) at kAudioRateHz, trying
    // STEREO first and falling back to mono when the device refuses two
    // channels. Use this rather than audio().open() from the GUI: the DSP
    // thread reads the resulting layout under audioMutex_ to decide whether
    // to push interleaved frames or a mono downmix, and going through the
    // sink directly would leave that mirror stale. Returns false only when
    // both attempts fail (the sink is then closed and write() simply drops).
    //
    // BLOCKING, and on some machines for a very long time: on Windows this
    // reaches waveOutOpen, which has no timeout and held one field session's
    // GUI thread for 57 seconds. Call it directly only where blocking is
    // harmless - the constructor, before any frame loop exists. Everything
    // else goes through gui::AudioOpen with audioOpener() below.
    bool openAudioDevice(int deviceIndex);

    // The same open, packaged so it can run on a worker thread that may
    // outlive this Pipeline: the returned callable owns a reference to the
    // sink (see audio_ below) and touches nothing else of this object.
    //
    // WHAT IT DELIBERATELY LEAVES OUT is the channel mirror. audioChannels_ is
    // read by the DSP thread once per block and belongs to this object, not
    // to the worker, so the caller republishes it with publishAudioChannels()
    // on the frame it collects the result. The cost of that deferral is at
    // most one audio block pushed in the previous layout during a device
    // switch, which is inaudible against the switch itself; the cost of
    // letting an abandoned worker write it would be a use-after-free.
    std::function<bool(int)> audioOpener();

    // Mirrors the sink's current channel layout for the DSP thread. Called
    // from the GUI thread when an asynchronous open completes; `ok` false
    // means the open failed and the layout falls back to mono, exactly as the
    // synchronous path does. One atomic store - no lock, so it cannot queue the
    // frame that collects the open behind a DSP block.
    void publishAudioChannels(bool ok);

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
    //  2. rateHz / decim is an INTEGER for some decimation the chain can use.
    //     decim = round(rateHz / 200 kHz), clamped to >= 1, whenever that
    //     divides the rate exactly; otherwise the decimation whose channel is
    //     an exact integer in [150 kHz, 300 kHz), preferring a channel of at
    //     least 166.7 kHz (the default 150 kHz WFM filter then fits the Vfo's
    //     0.9x clamp unclipped) and among those the one nearest 200 kHz - so
    //     2.56 MS/s runs a 256 kHz channel and 2.88 MS/s a 192 kHz one
    //     (pipeline.cpp, decimationForInputRate). The integer requirement is
    //     what keeps the audio resampler exact: RationalResampler takes an
    //     integer L/M ratio (channelRate -> 48 kHz, reduced by gcd
    //     internally), so a fractional channel rate could only be
    //     approximated, silently detuning audio. A rate with no such
    //     decimation (a non-integer rate, or e.g. 2000001 = 3 x 666667) is
    //     refused.
    // The channel rate is in [150 kHz, 300 kHz) for every accepted rate
    // >= 300 kHz (in [150 kHz, 250 kHz] whenever the nominal decimation is
    // used); below 300 kHz decim is 1 and the channel rate equals the input.
    //
    // Concurrency (mirrors the setSource quiesce handshake, but for the DSP
    // thread — the source thread keeps running throughout): all under
    // controlMutex_,
    //   1. dspRun_ cleared          (the DSP loop is told to exit; run_ stays
    //      set, so the source keeps feeding the ring — a full ring drops the
    //      overflow by design and never blocks the source)
    //   2. dspThread_ joined        (after this NO thread touches the
    //      estimator or the audio chain, so rebuilding them is race-free)
    //   3. chain rebuilt under audioMutex_ (the setters may arrive from other
    //      threads while the DSP thread is down)
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
    // ring (sized for the SOURCE's rate when its thread was spawned, by
    // start() or setSource(); this call leaves the source thread running, so
    // it cannot replace the ring under it. A device whose rate is changed
    // live keeps both the ring and its read size from that spawn, so each
    // read still fits and only the headroom in milliseconds shrinks; any
    // overflow is counted in ringDroppedSamples()), seq numbering, and the
    // audio tap/counters.
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

    // How long the chain takes to hand the speakers to a plugin, or take them
    // back, in FRAMES at kAudioRateHz: 5 ms.
    //
    // A HARD CUT BETWEEN TWO UNRELATED SIGNALS IS A CLICK, and a loud one - the
    // step at the seam is the sum of two amplitudes that know nothing about
    // each other, which is a broadband impulse straight into the speakers. 5 ms
    // is long enough to make that step inaudible and short enough that nobody
    // perceives the changeover as a fade. Public so the test can measure the
    // seam against the number rather than against a guess.
    static constexpr std::size_t kPluginFadeFrames = 240;

    // --- Test support (used by --selftest) ----------------------------------

    // TEST HOOK for the lock-free getters above, and the only way to stage the
    // condition that produced the 0.96.2 report: the DSP thread holding its
    // mutex for longer than a frame. A test cannot make a real machine too slow
    // on demand, and driving a real pipeline hard enough would measure the
    // bench rather than the property - so this holds the SAME mutex the DSP
    // thread holds across a block, for a stated time, from whatever thread
    // calls it.
    //
    // `acquired` (optional) is set true once the lock is actually held, so the
    // test can start timing when the contention really exists rather than after
    // a sleep it guessed at. Never called by the application.
    enum class LockForTest { Audio, Control };
    void holdLockForTest(LockForTest which, int holdMs, std::atomic<bool>* acquired);
    // True while a posted setter change has not yet run (see "SETTERS POST, THEY DO
    // NOT WAIT"). A test that held the lock and posted through it waits for this to
    // fall before it reads the chain's own, clamped figures back.
    bool commandsPendingForTest() const {
        return cmdPending_.load(std::memory_order_acquire);
    }
    // Total audio samples produced by the chain, counted BEFORE
    // AudioOut::write so the count advances with or without a device.
    std::uint64_t audioSamplesProduced() const;

    // Copies the most recent audio samples (a rolling 4096-FRAME tap taken
    // just before AudioOut::write) into dst in chronological order, as the
    // MONO downmix (L + R)/2. Returns the number copied: min(n, 4096, frames
    // produced so far). The downmix is exact for every mono path — L and R
    // are then bit-identical and (a + a)/2 == a in IEEE arithmetic — so the
    // --selftest measurement this backs is unchanged by the stereo work.
    std::size_t audioTap(float* dst, std::size_t n) const;

    // Both channels of the same rolling tap, for tests that need to prove the
    // channels actually differ (stereo separation) or actually match (forced
    // mono). Either destination may be null. Returns the frames copied.
    std::size_t audioTapStereo(float* dstLeft, float* dstRight,
                               std::size_t n) const;

    // --- The DEMOD SCOPE's two taps (0.94.0) ---------------------------------
    //
    // A THIRD AND FOURTH TAP, and the reasons they are not the two above are
    // in core/scope_tap.hpp and beside each push site in the .cpp. In short:
    // audioTap is 85 ms long and is a locked copy (tapMutex_), and the
    // scope's longest sweep is half a second and is read by the render thread
    // once a frame.
    //
    // WHAT EACH ONE CARRIES:
    //
    //   scopeAudio()  the mono downmix of the FINISHED audio, at
    //                 kAudioRateHz, taken at the same instant the recorder
    //                 takes its copy - which is BELOW the plugin audio
    //                 replacement and BELOW the hard mute. So when a plugin
    //                 is playing through CASCADE_CAP_AUDIO_OUT, this is the
    //                 plugin's sound and not the hiss the analog chain made
    //                 of the same carrier.
    //
    //   scopeIq()     the channel I/Q at channelRateHz(), after the VFO and
    //                 before the demodulator - the signal itself, which is
    //                 what makes AM, FM and SSB look different.
    //
    // Both are const references: a caller reads them with snapshot(), which
    // is const, and only the DSP thread ever pushes. Neither needs the
    // pipeline to be running - a stopped chain simply stops advancing
    // written(), and a reader that watches that counter can tell the
    // difference between silence and a stall.
    //   scopeMpx()    the FM MULTIPLEX at channelRateHz(): the discriminator's
    //                 output in WFM, before de-emphasis and before the stereo
    //                 decoder, which is the only place the pilot, the
    //                 difference sidebands, RDS and any SCA still exist as
    //                 separate things. Pushed ONLY in WFM - in every other
    //                 mode the same buffer is ordinary audio and there is no
    //                 multiplex to show - so a reader watching written() sees
    //                 it stop advancing the moment the mode changes, which is
    //                 what the scope's own liveness rule already knows how to
    //                 read.
    const ScopeTap<float>& scopeAudio() const { return scopeAudio_; }
    const ScopeTap<std::complex<float>>& scopeIq() const { return scopeIq_; }
    const ScopeTap<float>& scopeMpx() const { return scopeMpx_; }

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
    // (The GUI opens the file on a worker so a slow disk cannot freeze the
    // window, and arms the recorder with Recorder::begin() on the GUI thread
    // once the file is open: begin() is the "start" of this contract, and the
    // tap still comes after it. See Recorder's "Slow disks".)
    // writeIq/writeAudio ignore wrong-kind and stopped recorders by their
    // own contract, so the DSP hot path needs only the null checks.
    void setIqRecorder(Recorder* r);
    // wire-b begin: A TAP FOR THE RAW BAND (network I/Q out, engine/engine_wire_b.cpp). Given the drained block at the input rate exactly where the I/Q recorder gets it, on the DSP thread: submit must be
    // wait-free and allocation-free (net::IqOut::submit is). Set / cleared under audioMutex_, so once setIqSink(nullptr) returns no block is inside the old sink and it may be destroyed.
    struct IqSink {
        virtual ~IqSink() = default;
        virtual void submit(const float* iq, std::size_t nComplex) noexcept = 0;
    };
    void setIqSink(IqSink* s);
    // wire-b end
    // wire-b begin: THE GPU SPECTRUM (core/pipeline_gpufft.cpp; dsp/gpu_fft.hpp). Mode 0 Cpu (the DEFAULT: no device is ever made), 1 Auto (the GPU only from 65 536 points: at the legacy
    // size of this path the CPU estimator keeps running), 2 Gpu. Any thread. Every failure falls back to the CPU path silently; the report says what is running and why not.
    void setGpuFftMode(int mode);
    struct GpuFftReport {
        int mode = 0;                 // what was asked for
        bool engaged = false;         // the GPU engine is publishing this path's frames
        int backend = 0;              // 0 the engine computes on the CPU, 1 on the GPU
        std::uint64_t submitted = 0, completedGpu = 0, completedCpu = 0, droppedFrames = 0, deviceLost = 0;
        std::string error;            // the latest GPU problem, text only (no adapter name)
        std::uint64_t blockGpu = 0, blockCpu = 0;   // second pass: frames of the SPECTRUM BLOCK (SET_SPECTRUM) the Direct3D device / the engine's CPU computation finished
        bool blockUp = false;         // a device is up for the spectrum block
    };
    GpuFftReport gpuFftReport() const;
    // wire-b end
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
    // double — and the value is only ever consulted when a source reports a
    // nonsense rate, which is precisely the path that would then act on a torn
    // one. Passing it through the spawn edge needs no lock and no atomic.
    //
    // `stopToken` is THIS generation's dead-man switch (see the members
    // below and stop() in the .cpp): captured by value at spawn, tested
    // alongside run_/srcRun_ at the top of every loop iteration, and never
    // reassigned for the lifetime of the thread — a later generation gets an
    // entirely different shared_ptr, which is what lets stop() abandon a
    // stuck thread without that thread ever mistaking a NEW session's flags
    // for permission to keep running.
    //
    // `priority` is where this generation writes how its priority request came
    // out (see sourceThreadPriority()): a shared_ptr captured by value, like the
    // token, so that a thread the session abandoned and that wakes up late
    // writes into memory it owns a share of and not into a Pipeline that has
    // gone.
    struct PriorityNote {
        std::mutex m;
        std::string text;
        void set(const std::string& s) {
            std::lock_guard<std::mutex> lk(m);
            text = s;
        }
        std::string get() {
            std::lock_guard<std::mutex> lk(m);
            return text;
        }
    };
    void sourceThreadMain(double chainRateHz,
                          std::shared_ptr<std::atomic<bool>> stopToken,
                          std::shared_ptr<PriorityNote> priority);
    void dspThreadMain();
    void sourceThreadBody(double chainRateHz, const std::atomic<bool>& stopToken,
                          cascade::core::beats::Registration& beat);
    void dspThreadBody();
    // Mints a fresh per-generation stop token and exit latch (srcStopToken_ /
    // srcExitFuture_ below) and spawns srcThread_ against them, capturing the
    // token BY VALUE into the thread's own lambda. Caller holds
    // controlMutex_, has already set srcRun_ (start() also sets run_ and
    // dspRun_) true, and has already called active_->start() — used
    // identically by start() and the setSource() resume path so the two
    // spawn sites cannot drift apart on this contract.
    void spawnSourceThread(double chainRateHz);
    // The ring capacity the active source needs, and the replacement of the
    // ring when that differs from what it has. Both need controlMutex_;
    // resizeRingLocked additionally needs both worker threads stopped (see
    // the .cpp).
    std::size_t ringCapacityForActiveSourceLocked() const;
    void resizeRingLocked(std::size_t capacity);
    void noteThreadFault(const char* where, const char* what);
    void processAudioBlock(const std::complex<float>* in, std::size_t n);
    // Rebuilds the rate-dependent parts of the stereo/RDS/audio-post chain
    // for a new channel rate. Caller holds audioMutex_.
    void rebuildChannelBlocks(double chanRate);
    // Copies the decoder's state into rdsPublished_ when something moved.
    // Caller holds audioMutex_ (it reads rds_); takes rdsMutex_ internally.
    void publishRds();
    // The body of resetRds(), for callers that ALREADY hold audioMutex_ —
    // re-locking it from inside a setter would deadlock instantly.
    void resetDecodersLocked();
    // Pushes deemphasisUs_ into the DEMODULATOR, which owns de-emphasis for
    // every mode except WFM — there StereoFm owns it (see the ownership note
    // at the top of this header) and the discriminator must stay flat, so this
    // sends 0 us instead. One rule in one place: it used to be written out as
    // a bare "off" at three separate call sites, which is how NFM ended up
    // ignoring the setting entirely. Caller holds audioMutex_ (the constructor
    // runs it before any thread exists); touches only demod_, so it is safe to
    // call before stereo_ has been built.
    void applyDemodDeemphasisLocked();
    // Turns the user's bandwidth (vfoBandwidthHz_) into what the two blocks
    // that enforce it are told, for the CURRENT mode: the channel filter's own
    // bandwidth and, for the sideband modes, the demodulator's sideband width.
    // One rule in one place, called from every point that changes the mode, the
    // bandwidth or the rate (and so rebuilds either block). Caller holds
    // audioMutex_ (the constructor runs it before any thread exists).
    //
    // AM, DSB, FM, RAW: the channel filter IS the bandwidth, symmetric about
    // the tuned frequency. USB, LSB, CW: the wanted sideband lies on ONE side of
    // the carrier and the demodulator selects it, so the bandwidth is the
    // sideband's width (Demodulator::setSsbBandwidthHz) and the channel filter
    // - symmetric, so it needs the sideband's reach on both sides - is left
    // kSidebandChannelFactor times as wide, wide enough never to touch the
    // audio and narrow enough to keep the neighbouring channels out.
    void applyChannelBandwidthLocked();

    // PUBLISHES EVERY PER-FRAME GETTER'S MIRROR from the live objects. Caller
    // holds audioMutex_ (the constructor runs it before any thread exists).
    // Called at the END of every setter that can move one of these values, and
    // at the end of the rate-switch rebuild - one function rather than a store
    // beside each assignment, because a setter that mirrors three of the four
    // values it changed is a wrong readout that no test of the setter itself
    // would see. tests/test_pipeline_getters.cpp drives every setter and
    // compares each getter against the value that went in, so a mirror that is
    // never published fails there rather than on a user's screen.
    //
    // inputRateHz's mirror is deliberately NOT published here: cfg_.sampleRateHz
    // lives under controlMutex_, not audioMutex_, and setInputRateHz publishes
    // it at the one line that commits the new rate.
    void publishParamMirrorsLocked();

    // --- Setters post, they do not wait (see the header comment) -------------
    // A change to the audio chain: runs under audioMutex_, on whichever thread
    // holds it, against the LIVE objects at that moment - so it captures values,
    // never pointers into the chain (the chain can be rebuilt between the post
    // and the run).
    using Command = std::function<void()>;
    // Queues `apply`, runs `mirrorNow` (optional: stores the VERBATIM mirror
    // values so a getter read right after the setter returns sees the new value),
    // then applies the whole queue itself if audioMutex_ is free, and otherwise
    // leaves it to the DSP thread's next block. Never waits for audioMutex_.
    void post(Command apply, Command mirrorNow = {});
    // Caller holds audioMutex_. Runs everything queued, in order, until the queue
    // is empty, then publishes the mirrors from the live objects - under
    // cmdMutex_ and only on an empty queue, so a later post's verbatim mirror
    // cannot be overwritten by an older value from the live chain.
    void drainCommandsLocked();

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
    // activeSource(): active_ seen through the converter (0.99.36). Re-bound
    // to active_ at every swap, and its converter reset to OFF there. The
    // source thread never reads THROUGH it - only its mirrors() flag, after
    // its own stop-token test - because an abandoned thread must never be
    // re-pointed at the next session's source by a swap it slept through.
    cascade::source::ConverterView airView_;
    // The construction-time ring size, and the least the ring is ever given:
    // a source is sized UP from it when one of its reads would not fit, never
    // down (see ringCapacityForActiveSourceLocked). Declared before ring_,
    // which is built from it.
    const std::size_t ringFloor_;
    // Owned through a pointer because its SIZE follows the source: it is
    // replaced (resizeRingLocked) at a source-thread spawn whose source needs
    // a different capacity, and only while neither worker thread runs.
    std::unique_ptr<cascade::dsp::SpscRing<std::complex<float>>> ring_;
    cascade::dsp::SpectrumEstimator estimator_;   // touched only by the DSP thread while running
    // wire-b begin: the GPU spectrum's state (pipeline_gpufft.cpp)
    struct GpuFftState;
    static std::shared_ptr<GpuFftState> makeGpuFftState();
    std::shared_ptr<GpuFftState> gpuFft_ = makeGpuFftState();
    int gpuFrame(const std::complex<float>* acc, std::size_t n, float* db);   // DSP thread: 0 not engaged, 1 a frame is in db, 2 none ready
    // wire-b end

    // Single latest-frame slot. seq lives inside latest_ and NEVER resets —
    // not even across stop()/start() — so a consumer that kept its last seen
    // seq keeps receiving frames after a restart. latest_.seq == 0 means
    // "nothing published yet".
    mutable std::mutex frameMutex_;
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
    // THE SETTERS' QUEUE. cmdMutex_ guards cmdQueue_ and nothing else, and is held
    // only for a push, a swap or a mirror store - never across a DSP block and
    // never while waiting for another lock. LOCK ORDER: controlMutex_ ->
    // audioMutex_ -> cmdMutex_ (the DSP thread's drain), and a setter takes
    // cmdMutex_ and then only TRIES audioMutex_ (never waits), so the reverse
    // direction cannot deadlock. cmdScratch_ is the drain's working vector, kept
    // so the DSP thread swaps buffers instead of allocating; guarded by
    // audioMutex_.
    std::mutex cmdMutex_;
    std::vector<Command> cmdQueue_;
    std::vector<Command> cmdScratch_;
    std::atomic<bool> cmdPending_{false};
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
    std::unique_ptr<cascade::dsp::OirtStereo> oirt_;   // the same channel rate and settings as stereo_, built beside it (setStereoSystem chooses)
    int stereoSystem_ = 1;                              // the DSP thread's (and the setter's, under the post)
    std::atomic<int> mirrorStereoSystem_{1};
    std::atomic<bool> oirtActive_{false};
    std::vector<float> oirtIn_, oirtL_, oirtR_;         // the OIRT branch's scratch (the decoder's output capacity is one more than its input)
    std::unique_ptr<cascade::dsp::RdsDecoder> rds_;
    bool stereoEnabled_ = true;
    // Counters of the last published snapshot: the publish is skipped unless
    // the decoder actually advanced, which keeps the per-block cost at one
    // pair of integer compares instead of two string copies.
    std::uint32_t rdsPubGroups_ = 0;
    std::uint32_t rdsPubErrors_ = 0;

    cascade::dsp::Agc agc_;
    cascade::dsp::Squelch squelch_;
    // TONE SQUELCH: the settings the DSP thread works from (changed only by posted commands, under audioMutex_), the two detectors (only the chosen kind runs),
    // and the two high-passes of the tone remover, one per channel, at the audio rate.
    int toneKind_ = 0;
    double toneHz_ = 0.0;
    unsigned toneDcsOctal_ = 0;
    int toneDcsPol_ = 0;
    bool toneRemove_ = false;
    double toneChanRateHz_ = 0.0;
    // NOISE-REDUCTION METHOD: the choice and what the posted switch and strength last said (so a method built later starts from them), and the two channels' alternative reducers.
    cascade::dsp::AudioEq eqL_{kAudioRateHz};
    cascade::dsp::AudioEq eqR_{kAudioRateHz};
    cascade::dsp::StereoPlacer placer_{kAudioRateHz};     // (F20/D5: setters are atomics, process() runs on the DSP thread)
    std::vector<float> placeIn_;
    std::atomic<int> mirrorPlaceMode_{0};
    std::atomic<double> mirrorPlaceValue_{0.0};
    cascade::dsp::CwPeakFilter cwPeakL_{kAudioRateHz};   // (F8: setters are atomics, process() runs on the DSP thread)
    cascade::dsp::CwPeakFilter cwPeakR_{kAudioRateHz};
    std::atomic<bool> mirrorCwPeakOn_{false};
    std::atomic<double> mirrorCwPeakWidth_{100.0};
    std::atomic<double> mirrorCwPeakBoost_{12.0};
    cascade::dsp::NotchBank notchBankL_{kAudioRateHz};
    cascade::dsp::NotchBank notchBankR_{kAudioRateHz};
    std::unique_ptr<cascade::dsp::IqCorrector> iqCorr_;   // swapped only while the DSP thread is quiesced (a rate change)
    std::atomic<int> iqMask_{0};
    // AFC (setAfc): the DSP thread's copies, the tracker (rebuilt with the channel blocks) and the lock-free mirrors the control thread reads.
    // IF ZOOM (setIfZoom): the settings (DSP thread's copy, mirrored for the control thread), and the block, swapped whole so a reader holds it while a rebuild replaces it.
    std::size_t zoomFft_ = 0;
    double zoomSpanHz_ = 0.0;
    std::atomic<std::size_t> mirrorZoomFft_{0};
    std::atomic<std::shared_ptr<cascade::dsp::ZoomFft>> zoom_;
    void rebuildZoom(double chanRate);
    std::unique_ptr<cascade::dsp::AfcTracker> afc_;
    bool afcOn_ = false;
    double afcPullHz_ = 2500.0;
    double userOffsetHz_ = 0.0;      // the VFO offset the user asked for; the VFO runs at this plus afcAppliedHz_
    double afcAppliedHz_ = 0.0;
    std::atomic<bool> mirrorAfcOn_{false};
    std::atomic<double> mirrorAfcPullHz_{2500.0};
    std::atomic<double> mirrorAfcCorrHz_{0.0};
    std::atomic<bool> mirrorAfcLocked_{false};
    int chanShapeNow_ = 0;                       // F6 (DSP thread's copy)
    std::atomic<int> mirrorChanShape_{0};
    std::atomic<bool> mirrorChanShapeActive_{false};
    double cwPitchNow_ = 700.0;
    // The spectrum block path (setSpectrum): the block exists from the first non-default choice on and is never freed while the pipeline lives; specOn_ says whether the DSP thread feeds it.
    std::unique_ptr<cascade::dsp::SpectrumBlock> specBlock_;
    std::atomic<bool> specOn_{false};
    std::size_t specFft_ = 0;       // the choice (0 = the construction size); all of these under frameMutex_, which also makes the block's ONE control thread (latest, swapIn, collectRetired) one lock
    int specWindow_ = 3;
    double specFps_ = 0.0;
    std::vector<float> specScratch_;   // getLatestFrame's read buffer
    void rebuildSpectrumBlock();   // the choice -> the block (any control thread; takes frameMutex_ for the hand-over only)
    std::unique_ptr<cascade::dsp::NoiseSquelch> noiseSq_;
    bool noiseSqOn_ = false;
    double noiseSqThresholdDb_ = -30.0;
    double noiseSqTailMs_ = 100.0;
    std::vector<float> noiseSqGain_;
    int nrMethod_ = 0;
    bool nrOn_ = false;
    float nrStrengthNow_ = 0.5f;
    std::unique_ptr<cascade::dsp::NoiseReducer> nrAltL_;
    std::unique_ptr<cascade::dsp::NoiseReducer> nrAltR_;
    cascade::dsp::CtcssDetector ctcss_;
    cascade::dsp::DcsDecoder dcs_;
    cascade::dsp::CtcssHighpass toneHpL_;
    cascade::dsp::CtcssHighpass toneHpR_;
    // (re)configures the chosen detector for toneChanRateHz_ and applies the choice; caller holds audioMutex_
    void configureToneLocked();
    cascade::dsp::PowerMeter meter_;      // S-meter source (channel power)
    // TWO resamplers with identical configuration, one per channel: the
    // resampler is mono, and running the same L/M state machine twice from
    // the same starting state keeps the channels sample-aligned forever.
    cascade::dsp::RationalResampler resampler_;    // left / mono
    cascade::dsp::RationalResampler resamplerR_;   // right
    // The decoder feed's own resampler. Separate because it is driven from a
    // different point in the chain (before the AGC), and a resampler is a
    // state machine — interleaving two signals through one corrupts both.
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
    // SHARED, NOT HELD BY VALUE, and the reason is lifetime rather than
    // sharing. A device open is a blocking driver call, so the GUI runs it on
    // a worker (core/audio_open.hpp) and abandons that worker at quit rather
    // than joining it - which means the worker can still be inside
    // Pa_OpenStream after this Pipeline is gone. audioOpener() hands the
    // worker a copy of this pointer, so the sink outlives the abandonment and
    // whichever thread drops the last reference is the one that closes the
    // stream. Never null.
    // The patch, its takeover fade, and the last sample it played - the
    // same three things the plugin path keeps, for the same reasons.
    cascade::core::patch::Runner patch_;
    std::vector<float> patchL_;
    std::vector<float> patchR_;
    float patchFade_ = 0.0f;
    float patchLastL_ = 0.0f;
    float patchLastR_ = 0.0f;

    std::shared_ptr<cascade::sink::AudioOut> audio_;  // device opened only
                                                      // when cfg_.audioEnabled
    // Channel layout the sink was last opened with (1 or 2), mirrored for the DSP
    // thread so it never races a GUI device switch. Atomic rather than under
    // audioMutex_: the GUI publishes it on the frame an async open completes, and
    // a lock there queued that frame behind a DSP block.
    std::atomic<int> audioChannels_{1};
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
    // Holds the sink's lead against the radio's clock (sink/drift_matcher.hpp):
    // only the sound card's copy is resampled - the taps, the scope and the
    // recorder keep the radio's own timing. Guarded by audioMutex_.
    cascade::sink::DriftMatcher driftMatcher_;
    std::vector<float> matchedOut_;              // driftMatcher_'s output
    std::vector<float> monoOut_;                 // (L+R)/2 for tap + recorder
    // A plugin's audio (CASCADE_CAP_AUDIO_OUT), already at kAudioRateHz, and
    // the crossfade that hands the speakers over. DSP thread only.
    std::vector<float> plugL_;
    std::vector<float> plugR_;
    float pluginFade_ = 0.0f;  // 0 = demodulated audio, 1 = the plugin's
    // The last sample actually played from the plugin, held through the fade
    // OUT. Fading to digital zero instead would put a step the size of that
    // sample at the seam - the click the fade exists to prevent, moved to the
    // other end of the takeover.
    float pluginLastL_ = 0.0f;
    float pluginLastR_ = 0.0f;
    // Rolling pre-AudioOut tap window (interleaved L,R; kAudioTapSize FRAMES)
    // + producer-side counters (test support).
    //
    // GUARDED BY tapMutex_, NOT audioMutex_. audioTap()/audioTapStereo() used to
    // take audioMutex_, which processAudioBlock holds across a WHOLE block, so
    // the GUI thread - AppWindow::publishWebAudio read the tap every frame that
    // had new audio while the browser remote was on (since stage 5e the engine's read_audio feed does, on whichever thread reads or on the pass) - queued behind the entire
    // DSP block (measured: 1507 ms inside publishWebAudio with the mutex held
    // for 1500 ms by the test seam that stands in for a plugin stretching a
    // block). tapMutex_ is held only for the copy of one block's frames
    // into the window, and for a reader's copy out of it: microseconds.
    //
    // LOCK ORDER: on the DSP thread audioMutex_ is taken first and tapMutex_
    // nests inside it for just that copy; the readers take tapMutex_ ALONE.
    // Nothing ever waits for audioMutex_ while holding tapMutex_, so the two
    // cannot deadlock. Keep it that way: never take audioMutex_ (or anything
    // that does) under tapMutex_.
    mutable std::mutex tapMutex_;
    std::vector<float> tapBuf_;
    std::size_t tapWrite_ = 0;   // in frames
    std::size_t tapFilled_ = 0;  // in frames
    // The demod scope's rolling windows - see scopeAudio()/scopeIq() above.
    // The sizes are the longest sweep the scope offers plus room for the
    // trigger to hunt in: half a second at 48 kHz is 24000 samples, and the
    // same half second of channel I/Q at a typical 200 kHz channel rate is
    // 100000. Both are powers of two because the tap indexes by mask, and
    // both are allocated once at construction - the DSP thread never
    // allocates.
    static constexpr std::size_t kScopeAudioTapSamples = 65536;   // 1.37 s @ 48 kHz
    static constexpr std::size_t kScopeIqTapSamples = 262144;     // 1.31 s @ 200 kHz
    // The multiplex needs resolution rather than duration: one transform of
    // 8192 bins at a 200 kHz channel rate resolves 24 Hz, which separates the
    // pilot from everything near it and puts RDS's sidebands either side of
    // 57 kHz where they belong. 65536 is eight of those transforms' worth -
    // enough for the page to average without ever waiting for the ring to
    // fill - and it is a power of two because the tap indexes by mask.
    static constexpr std::size_t kScopeMpxTapSamples = 65536;     // 0.33 s @ 200 kHz
    ScopeTap<float> scopeAudio_{kScopeAudioTapSamples};
    ScopeTap<std::complex<float>> scopeIq_{kScopeIqTapSamples};
    ScopeTap<float> scopeMpx_{kScopeMpxTapSamples};
    std::atomic<float> signalDb_{-200.0f};
    std::atomic<bool> squelchOpen_{false};
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
    std::atomic<bool> demodAudioMuted_{false};
    std::atomic<bool> programmeAudio_{false};
    std::atomic<double> autoNotchHz_{0.0};

    // THE PER-FRAME GETTERS' MIRRORS. See the header comment: the GUI polls
    // these every frame and must never queue behind a DSP block to read one.
    // Each is written under audioMutex_ by publishParamMirrorsLocked() (or, for
    // the input rate, under controlMutex_ by setInputRateHz) and read with no
    // lock at all by the getter of the same name.
    //
    // The values are the CLAMPED, live ones - notchFrequencyHz() has always
    // returned what the biquad actually uses rather than what was asked for,
    // and mirroring the request instead would have changed a readout while
    // fixing a stall.
    std::atomic<double> mirrorVfoOffsetHz_{0.0};
    std::atomic<double> mirrorChannelRateHz_{0.0};
    std::atomic<double> mirrorInputRateHz_{0.0};
    std::atomic<int> mirrorDemodMode_{0};
    std::atomic<double> mirrorDeemphasisUs_{50.0};
    std::atomic<bool> mirrorStereoEnabled_{true};
    std::atomic<bool> mirrorNrEnabled_{false};
    std::atomic<float> mirrorNrStrength_{0.0f};
    std::atomic<int> mirrorAgcMode_{0};          // Agc::Mode
    std::atomic<float> mirrorAgcFixedDb_{0.0f};
    std::atomic<float> agcGainDb_{0.0f};         // live, once per block
    std::atomic<bool> mirrorNbEnabled_{false};
    std::atomic<float> mirrorNbThresholdDb_{15.0f};
    std::atomic<float> nbBlankedPct_{0.0f};      // live, once per block
    std::atomic<bool> nbStandingDown_{false};
    std::atomic<double> mirrorCwPitchHz_{700.0};
    std::atomic<bool> mirrorNoiseSqOn_{false};
    std::atomic<double> noiseSqLevelDb_{-200.0};
    std::atomic<bool> noiseSqOpen_{false};
    std::atomic<int> mirrorNrMethod_{0};
    std::atomic<int> nrMethodActive_{0};
    std::atomic<int> mirrorToneKind_{0};
    std::atomic<double> mirrorToneHz_{0.0};
    std::atomic<int> mirrorToneDcsDigits_{0};
    std::atomic<int> mirrorTonePol_{0};
    std::atomic<bool> mirrorToneRemove_{false};
    std::atomic<double> toneDetectedHz_{0.0};    // live, once per block
    std::atomic<int> toneDetectedCode_{0};       // DCS, as digits (23 for 023); 0 none
    std::atomic<int> toneDetectedPol_{0};        // 1 normal, 2 inverted; 0 none
    std::atomic<bool> toneGateOpen_{true};
    std::atomic<bool> audioGateOpen_{false};
    std::atomic<bool> mirrorNotchEnabled_{false};
    std::atomic<double> mirrorNotchHz_{0.0};
    std::atomic<double> mirrorNotchQ_{0.0};
    std::atomic<bool> mirrorAutoNotchEnabled_{false};
    // Published RDS state. Its own mutex (never held together with any other)
    // so rdsSnapshot() from the GUI cannot stall behind an audio block.
    mutable std::mutex rdsMutex_;
    RdsSnapshot rdsPublished_;
    // Recorder taps (P6): non-owning, audioMutex_-guarded like the chain
    // blocks above — see the set*Recorder contract in the public section.
    std::atomic<double> pilotOffsetHz_{0.0};            // R18
    Recorder* ifRecorder_ = nullptr;                     // P4 (under audioMutex_)
    Recorder* mpxRecorder_ = nullptr;                    // D4 (under audioMutex_)
    cascade::dsp::RationalResampler mpxResampler_{192000u, 200000u};   // composite rate -> 192 kHz; rebuilt with the channel blocks
    std::vector<float> mpxBuf_;
    Recorder* iqRecorder_ = nullptr;
    IqSink* iqSink_ = nullptr;   // wire-b: guarded by audioMutex_ (see setIqSink)
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
    // How each thread's priority request came out (dspThreadPriority(),
    // sourceThreadPriority()). Both are shared_ptrs because the source thread's
    // generation holds its own share (spawnSourceThread); the DSP thread is
    // always joined, so its note is only ever touched while the Pipeline lives.
    std::shared_ptr<PriorityNote> dspPriority_ = std::make_shared<PriorityNote>();
    std::shared_ptr<PriorityNote> srcPriority_ = std::make_shared<PriorityNote>();

    // Per-generation shutdown state for the source thread — zombie-safety
    // for stop()'s bounded join (field report 4214EAE4; see stop() and
    // setSource() in the .cpp for the full contract). spawnSourceThread
    // mints a fresh pair at every (re)spawn: srcStopToken_ is captured BY
    // VALUE into that generation's thread lambda, so clearing it here can
    // only ever reach the CURRENT generation, never one already abandoned.
    // srcExitFuture_ is the matching exit latch, fulfilled with
    // set_value_at_thread_exit from inside the same lambda once the OS
    // thread is genuinely about to end — not merely once the loop returns.
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
    // See dspThreadExceptions(). Written only by the DSP thread's catch blocks.
    std::atomic<int> dspExceptions_{0};
    // See ringDroppedSamples(). Written only by the source thread.
    std::atomic<std::uint64_t> ringDropped_{0};
    // See sourceSamplesRead(). Written only by the source thread, BEFORE the
    // write that may drop part of the same read, so a reader that loads the
    // drop count first never sees a drop without its read.
    std::atomic<std::uint64_t> srcSamplesRead_{0};
    // See ringCutWrites() / ringHighWaterSamples(): written only by the source
    // thread, in noteRingWrite, which moves ringDropped_ too.
    std::atomic<std::uint64_t> ringCutWrites_{0};
    std::atomic<std::size_t> ringHighWater_{0};
    // See dspBlockLongestNs(): written only by the DSP thread, in noteDspBlock.
    std::atomic<std::uint64_t> dspBlockLongestNs_{0};
    void noteRingWrite(std::size_t got, std::size_t put);
    void noteDspBlock(std::chrono::steady_clock::duration took);

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
