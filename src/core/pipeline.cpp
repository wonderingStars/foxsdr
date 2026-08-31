// Pipeline implementation. Pacing approach (documented per task):
//
// For FREE-RUNNING sources (selfPaced() == false: the generator, files) the
// source thread reads ~10 ms chunks and sleeps to ABSOLUTE deadlines
// (steady_clock, sleep_until, next += period) rather than sleeping a fixed
// duration per chunk. Windows' default timer granularity (~15.6 ms) makes any
// single sleep overshoot badly, but with absolute deadlines an overshoot is
// followed by zero-sleep catch-up chunks until the schedule is level again, so
// the *average* sample rate self-corrects to well within the ~20% tolerance.
// 10 ms chunks keep stop() latency low without busy-spinning. If the thread
// falls more than 250 ms behind (suspend, debugger, load spike) the schedule
// resyncs to "now" instead of bursting an unbounded backlog.
//
// For SELF-PACED sources (hardware) the DEVICE is the clock: read() blocks
// (bounded) until samples exist, so the loop just reads and writes the ring
// with no pacing sleep at all â€” any sleep of our own would let a real-time
// device's samples pile up and be lost. Zero returns (a bounded block that
// timed out empty) are retried immediately; the bound on read() is also what
// lets stop()/setSource() quiesce the thread promptly (see setSource).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include "core/diag_log.hpp"
#include "core/plugin_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/recorder.hpp"

namespace cascade::core {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Ring sized for at least 4 source chunks (~40 ms) of headroom so a scheduler
// hiccup on the DSP side does not force sample drops, and at least 8 FFT
// blocks so accumulation never starves at low sample rates. Rounded up to a
// power of two because SpscRing requires it.
std::size_t ringCapacityFor(const Pipeline::Config& cfg) {
    double want = 4.0 * cfg.sampleRateHz * 0.010;
    const double blocks = 8.0 * static_cast<double>(cfg.fftSize);
    if (want < blocks) { want = blocks; }
    std::size_t cap = 1;
    while (static_cast<double>(cap) < want) { cap <<= 1; }
    return cap;
}

// --- Audio chain constants ---------------------------------------------------

// Target channel rate: the VFO decimation is chosen as round(rate / 200 kHz)
// (clamped >= 1) so the channel lands as close to 200 kHz as an integer
// decimation allows â€” wide enough for WFM (+/-75 kHz deviation plus guard)
// while cutting the demod/AGC/squelch work to a small fraction of the input
// rate. For any input rate >= 300 kHz this puts the channel in
// [150 kHz, 250 kHz]; below 300 kHz the decimation is 1 and the channel IS
// the input.
constexpr double kTargetChannelRateHz = 200000.0;

unsigned decimationForInputRate(double rateHz) {
    double d = std::round(rateHz / kTargetChannelRateHz);
    if (d < 1.0) { d = 1.0; }
    return static_cast<unsigned>(d);
}

// Input-rate acceptance for setInputRateHz (documented in the header):
// [8 kHz, 61.44 MHz] AND an integer channel rate, because RationalResampler
// needs an exact integer L/M ratio for channelRate -> 48 kHz â€” a fractional
// channel rate could only be approximated, silently detuning all audio.
// A NaN rate fails the range test (every comparison with NaN is false).
constexpr double kMinInputRateHz = 8000.0;
constexpr double kMaxInputRateHz = 61.44e6;

// A hardware rate readback is a double the device COMPUTED from a master
// clock and an integer divider, so it is very often not the round number that
// was requested: a B200 asked for 2.4 MS/s reports 2399999.992520 Hz.
//
// The exact-integer channel-rate test below then refuses it, because
// 2399999.99252 / 12 is 199999.99937... - and the refusal message, printed to
// zero decimals, reads "refused 2400000 S/s", which looks like nonsense. The
// real consequence is worse than cosmetic: the chain keeps its previous rate
// while the device streams at the new one, so every decoder is handed samples
// on a time base that is wrong by whatever the two rates differ by.
//
// So a readback within a hair of an integer is treated as that integer. The
// tolerance is relative, and deliberately tiny - 0.24 Hz at 2.4 MS/s, a tenth
// of a part per million, orders of magnitude below any crystal's own error and
// below what any decoder in this project can notice. A device genuinely
// running at a non-integer rate is still refused, because that is a rate the
// resampler truly cannot handle exactly.
double snapToIntegerRate(double rateHz) {
    const double nearest = std::round(rateHz);
    const double tolerance = std::max(1e-3, rateHz * 1e-7);
    return std::fabs(rateHz - nearest) <= tolerance ? nearest : rateHz;
}

// `snappedOut` is the rate the CHAIN should actually run at, and the caller
// must use it in place of the requested one: storing the raw readback while
// building the resampler from the snapped value would leave the pipeline
// reporting a rate it is not running.
bool acceptedInputRate(double rateHz, unsigned* decimOut, double* chanRateOut,
                       double* snappedOut) {
    if (!(rateHz >= kMinInputRateHz && rateHz <= kMaxInputRateHz)) {
        return false;
    }
    const double snapped = snapToIntegerRate(rateHz);
    const unsigned decim = decimationForInputRate(snapped);
    const double chanRate = snapped / static_cast<double>(decim);
    if (chanRate != std::floor(chanRate)) { return false; }
    *decimOut = decim;
    *chanRateOut = chanRate;
    *snappedOut = snapped;
    return true;
}

// Default channel bandwidth: 150 kHz, the standard WFM channel width (Carson
// bandwidth of +/-75 kHz deviation with 15 kHz audio is ~180 kHz; 150 kHz is
// the conventional receiver setting and fits the 200 kHz channel's 0.9x clamp).
constexpr double kDefaultVfoBandwidthHz = 150000.0;

// FM discriminator scaling. QuadDemod emits instantaneous frequency in
// rad/sample, so a full +/-75 kHz broadcast-WFM deviation peaks at
// 2*pi*75000/channelRate rad/sample (2.36 at 200 kHz). The chain wants that
// signal near 0.5 full scale BEFORE the AGC, so the AGC trims rather than
// rescues:  fmScale = 0.5 * channelRate / (2*pi*75000).
constexpr double kFmDeviationHz = 75000.0;

// AGC: target 0.5 full scale (6 dB of headroom above the nominal level before
// the sink's clip). Rates are per-sample at the channel rate (200 kHz here):
// attack 0.005 pulls an over-target onset down within ~1 ms; decay 0.0005
// raises a quiet signal to target over ~100-300 ms, slow enough not to pump
// the noise floor audibly in speech pauses. Max gain 100 (40 dB) bounds the
// wind-up on silence.
//
// The AGC now sees the INTERLEAVED two-channel stream (see the header), i.e.
// two samples per channel-rate instant, so the rates are halved to keep those
// same real-time constants. For mono content â€” every non-WFM mode, where the
// two channels are identical â€” the halved rate applied twice per sample
// reproduces the original single update to first order, so the audio level
// behaviour is unchanged from the mono-only build.
constexpr float kAgcTarget = 0.5f;
constexpr float kAgcAttack = 0.005f / 2.0f;
constexpr float kAgcDecay = 0.0005f / 2.0f;
constexpr float kAgcMaxGain = 100.0f;

// Audio post-processing defaults (all OFF, so a fresh install and every
// existing config sound exactly as they did before P7 added them).
constexpr float kDefaultNrStrength = 0.5f;
constexpr double kDefaultNotchHz = 1000.0;
constexpr double kDefaultNotchQ = 30.0;
// 512 bins at 48 kHz: 93.75 Hz resolution, 10.7 ms of noise-reduction
// latency, and the frame size both modules' constants were tuned against.
constexpr std::size_t kAudioFftSize = 512;

// S-meter ballistics: EMA alpha 0.0005 is a ~10 ms time constant at 200 kHz,
// steady under a 60 fps GUI poll without hiding real level changes.
constexpr float kMeterAlpha = 0.0005f;

// Rolling audio tap depth (test support): 4096 samples = 85 ms at 48 kHz,
// exactly the DFT window --selftest analyzes.
constexpr std::size_t kAudioTapSize = 4096;

// How long Pipeline::stop() waits for the source thread to actually exit
// after asking it to, before giving up and abandoning it (see stop()'s own
// comment and field report 4214EAE4). Generous relative to every legitimate
// exit path â€” SoapySource::stop() bounds its own driver-lock wait to 1.5 s
// (kControlLockWait in soapy_source.cpp) before this wait even starts, and a
// free-running source notices the cleared flags within one 10 ms chunk â€” so
// hitting this bound at all means the driver's read() genuinely never
// returned, not that the thread was merely slow.
constexpr std::chrono::seconds kSourceJoinWait{3};

}  // namespace

Pipeline::Pipeline(Config cfg)
    : cfg_(cfg),
      builtin_(cfg.sampleRateHz),
      ring_(ringCapacityFor(cfg)),
      estimator_(cfg.fftSize, cascade::dsp::WindowType::BlackmanHarris),
      vfoDecim_(decimationForInputRate(cfg.sampleRateHz)),
      vfoBandwidthHz_(kDefaultVfoBandwidthHz),
      vfo_(cfg.sampleRateHz, vfoDecim_, kDefaultVfoBandwidthHz),
      demod_(cfg.sampleRateHz / static_cast<double>(vfoDecim_)),
      agc_(kAgcTarget, kAgcAttack, kAgcDecay, kAgcMaxGain),
      // 2x the channel rate: the squelch gates the INTERLEAVED stream, so its
      // 5 ms ramp and 100 ms hold are only real-time if it is told the rate
      // it will actually be clocked at.
      squelch_(2.0 * cfg.sampleRateHz / static_cast<double>(vfoDecim_)),
      meter_(kMeterAlpha),
      resampler_(static_cast<unsigned>(kAudioRateHz + 0.5),
                 static_cast<unsigned>(cfg.sampleRateHz /
                                           static_cast<double>(vfoDecim_) +
                                       0.5)),
      resamplerR_(static_cast<unsigned>(kAudioRateHz + 0.5),
                  static_cast<unsigned>(cfg.sampleRateHz /
                                            static_cast<double>(vfoDecim_) +
                                        0.5)),
      resamplerD_(static_cast<unsigned>(kAudioRateHz + 0.5),
                  static_cast<unsigned>(cfg.sampleRateHz /
                                            static_cast<double>(vfoDecim_) +
                                        0.5)),
      tapBuf_(2 * kAudioTapSize, 0.0f) {
    active_ = &builtin_;  // the generator feeds the ring until setSource says otherwise
    estimator_.setAlpha(cfg.averagingAlpha);
    demod_.setMode(cascade::dsp::DemodMode::WFM);  // default mode per spec
    // WFM de-emphasis belongs to StereoFm, never to the discriminator (see
    // the ownership note in the header): switched off here once, and kept off
    // by every rebuild, so the composite handed to RDS and to the stereo
    // matrix is always flat.
    demod_.setDeemphasisUs(0.0);
    fmScale_ = static_cast<float>(
        0.5 * (cfg.sampleRateHz / static_cast<double>(vfoDecim_)) /
        (kTwoPi * kFmDeviationHz));
    rebuildChannelBlocks(cfg.sampleRateHz / static_cast<double>(vfoDecim_));
    // Post-resampler chain, fixed at the sink rate so no rate-follow touches
    // it. Everything starts disabled/bypassing: NoiseReduction at strength 0
    // is a bit-exact delay line and a disabled Notch does not even touch the
    // samples, so an untouched install is byte-for-byte the pre-P7 audio.
    notchL_ = std::make_unique<cascade::dsp::Notch>(kAudioRateHz, kDefaultNotchHz,
                                                    kDefaultNotchQ);
    notchR_ = std::make_unique<cascade::dsp::Notch>(kAudioRateHz, kDefaultNotchHz,
                                                    kDefaultNotchQ);
    notchL_->setEnabled(false);
    notchR_->setEnabled(false);
    autoNotchL_ = std::make_unique<cascade::dsp::AutoNotch>(
        kAudioRateHz, kAudioFftSize, kDefaultNotchQ);
    autoNotchR_ = std::make_unique<cascade::dsp::AutoNotch>(
        kAudioRateHz, kAudioFftSize, kDefaultNotchQ);
    autoNotchL_->setEnabled(false);
    autoNotchR_->setEnabled(false);
    nrL_ = std::make_unique<cascade::dsp::NoiseReduction>(kAudioRateHz, kAudioFftSize);
    nrR_ = std::make_unique<cascade::dsp::NoiseReduction>(kAudioRateHz, kAudioFftSize);
    nrL_->setEnabled(false);
    nrR_->setEnabled(false);
    nrL_->setStrength(kDefaultNrStrength);
    nrR_->setStrength(kDefaultNrStrength);
    // Open the default output device up front (not in start()) so a device
    // chosen through audio() before/between runs is never stomped by a later
    // start(); stop() leaves the stream open, playing silence once the ring
    // drains. A failed open (headless box) degrades to a deviceless sink:
    // write() still accepts samples, nothing ever blocks.
    if (cfg_.audioEnabled) { openAudioDevice(-1); }
}

void Pipeline::rebuildChannelBlocks(double chanRate) {
    // Both decoders bake the composite rate into their filter designs, so a
    // rate change replaces them outright. Settings that are user choices
    // (de-emphasis, force-mono) are re-applied; decoded CONTENT is not
    // carried over â€” a rate change is a stream discontinuity, and a PS name
    // that survived one would be exactly the stale readout resetRds() exists
    // to prevent.
    stereo_ = std::make_unique<cascade::dsp::StereoFm>(chanRate);
    stereo_->setDeemphasisUs(deemphasisUs_);
    stereo_->setForceMono(!stereoEnabled_);
    rds_ = std::make_unique<cascade::dsp::RdsDecoder>(chanRate);
    resetDecodersLocked();
}

void Pipeline::resetDecodersLocked() {
    rds_->reset();
    stereo_->reset();  // stream state only; force-mono and tau are settings
    rdsPubGroups_ = 0;
    rdsPubErrors_ = 0;
    {
        std::lock_guard<std::mutex> lk(rdsMutex_);
        rdsPublished_ = RdsSnapshot{};
    }
    pilotLocked_.store(false, std::memory_order_relaxed);
    pilotLevel_.store(0.0f, std::memory_order_relaxed);
    stereoActive_.store(false, std::memory_order_relaxed);
}

void Pipeline::publishRds() {
    const cascade::dsp::RdsState st = rds_->state();
    // Nothing moved: skip the copy. Two integer compares per block instead of
    // two string assignments keeps this off the DSP thread's hot path.
    if (st.groupsDecoded == rdsPubGroups_ && st.blockErrors == rdsPubErrors_) {
        return;
    }
    rdsPubGroups_ = st.groupsDecoded;
    rdsPubErrors_ = st.blockErrors;
    std::lock_guard<std::mutex> lk(rdsMutex_);
    rdsPublished_.synced = rds_->synced();
    rdsPublished_.state = st;
}

bool Pipeline::openAudioDevice(int deviceIndex) {
    // Stereo first, mono as the fallback: a two-channel open is what the WFM
    // stereo path needs, but a mono-only device (or a host API that refuses
    // the layout) must still produce sound rather than nothing.
    bool ok = audio_.open(deviceIndex, kAudioRateHz, 2);
    if (!ok) { ok = audio_.open(deviceIndex, kAudioRateHz, 1); }
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioChannels_ = ok ? audio_.channels() : 1;
    return ok;
}

Pipeline::~Pipeline() {
    stop();
    // Defense in depth for the same zombie-safety policy setSource() enforces
    // (see there and stop()): if stop() just abandoned the source thread and
    // no setSource() has run since to release it, external_ still nominally
    // owns the object that thread may be parked inside, and the member
    // destructors that run right after this constructor body would free it
    // out from under a thread this process no longer controls. release()
    // leaks it instead, for the identical reason setSource() does.
    //
    // Pipeline's one caller (AppWindow) holds it for the entire process
    // lifetime, so this path is not reachable today â€” a detached thread that
    // outlives process teardown never gets to run further instructions on
    // either platform this ships for. The fix costs nothing, and a future
    // caller with a shorter-lived Pipeline must not have to rediscover this
    // the hard way.
    if (zombieSource_) {
        static_cast<void>(external_.release());
        zombieSource_ = false;
    }
}

cascade::source::SigGen& Pipeline::sigGen() {
    // Always the BUILT-IN generator, even while an external source is active:
    // callers configure tones on it and those settings take effect whenever
    // the generator is (re)selected. This keeps the pre-refactor surface
    // byte-compatible.
    return builtin_.sigGen();
}

void Pipeline::setSource(std::unique_ptr<cascade::source::IqSource> s) {
    std::lock_guard<std::mutex> lk(controlMutex_);
    const bool live = run_.load(std::memory_order_relaxed);
    // WHETHER TO QUIESCE IS A QUESTION ABOUT THE THREAD, NOT ABOUT run_.
    //
    // This used to be gated on `live`, which is right only while run_ and the
    // source thread agree about existing. A thread fault breaks that
    // agreement: noteThreadFault clears run_ from whichever worker died and
    // cannot join anything, so after a DSP-thread fault run_ is false while
    // the source thread is still alive and, for a self-paced source, still
    // parked inside read() waiting on the device. Skipping the handshake then
    // destroyed the outgoing source underneath that read â€” a use-after-free
    // reached by nothing more exotic than "the radio stopped, pick another
    // one". joinable() answers the question that actually matters, and is
    // false exactly when a previous stop()/setSource already joined, so the
    // ordinary paths are unchanged.
    //
    // The handshake itself is unchanged and its order still matters: flag the
    // source loop down, abort any in-flight bounded read via the source's own
    // stop() â€” a read that returns after the flag flips must see srcRun_
    // false â€” then join. After the join no thread can touch the outgoing
    // source. active_->stop() is idempotent per the IqSource contract, so
    // running it for an already-exited thread costs nothing.
    // The SAME bounded quiesce as stop() - this is the very path the field
    // hang took ("the user switched sources twice"), and it carried a plain
    // unbounded join long after stop() was bounded. See
    // quiesceSourceThreadLocked() for the policy; if it had to abandon the
    // thread, zombieSource_ is now set and the release below handles the
    // outgoing object.
    quiesceSourceThreadLocked();
    // ZOMBIE-SAFETY: a PRIOR stop() may have already given up on this exact
    // thread and detached it instead of joining (see stop()'s abandonment
    // policy) â€” in which case the block above found nothing joinable and did
    // nothing, but the detached thread can still be parked inside the
    // OUTGOING source's read(), or about to write one more block through it
    // before it next tests its own generation's stop token (sourceThreadBody
    // below). Destroying that object here would be a use-after-free reached
    // by nothing more exotic than "the radio hung, pick another one" â€” so it
    // is released (deliberately leaked) instead, the same trade
    // soapy_source.cpp's dead-device policy makes for the identical reason.
    // Once released it is no longer this swap's problem: clear the flag so a
    // later stop()/setSource() does not leak a second, perfectly healthy
    // source it never needed to.
    if (zombieSource_) {
        static_cast<void>(external_.release());
        zombieSource_ = false;
    }
    // Swap. Destroying the outgoing external source here is race-free on
    // every path but the one just handled above: the source thread is
    // joined, never existed, or (zombieSource_) was already released, so
    // there is nothing left for external_ to own.
    if (s) {
        external_ = std::move(s);
        active_ = external_.get();
    } else {
        external_.reset();
        active_ = &builtin_;  // null restores the built-in generator
    }
    {
        // A new antenna is a new station: drop the decoded RDS content and
        // the pilot lock with the old source. controlMutex_ -> audioMutex_ is
        // the same order start() and setInputRateHz use.
        std::lock_guard<std::mutex> alk(audioMutex_);
        resetDecodersLocked();
    }
    if (live) {
        // Resume: start the incoming source BEFORE its thread exists so the
        // first read() never races the device open, then respawn.
        active_->start();
        srcRun_.store(true, std::memory_order_relaxed);
        // cfg_.sampleRateHz is read HERE, under controlMutex_, and handed to
        // the thread; see sourceThreadMain's declaration. spawnSourceThread
        // also mints this generation's stop token/exit latch â€” zombie-safety
        // for whatever stop() eventually ends this session (see stop()).
        spawnSourceThread(cfg_.sampleRateHz);
    }
}

cascade::source::IqSource& Pipeline::activeSource() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    return *active_;
}

const char* Pipeline::activeSourceName() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    return active_->name();
}

void Pipeline::start() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    if (run_.load(std::memory_order_relaxed)) { return; }  // idempotent

    // A fresh run clears any previous fault, and joins the threads a fault
    // left behind: noteThreadFault only flips the flags, it cannot join
    // itself, so the std::thread objects are still joinable here.
    faulted_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> flk(faultMutex_);
        faultMsg_.clear();
    }
    // Abort an in-flight bounded read BEFORE joining, the same order stop()
    // and setSource use. This join is reached only after a fault (a stopped
    // pipeline has already joined both threads), and a DSP-thread fault leaves
    // the SOURCE thread alive and, for a self-paced source, parked inside
    // read() waiting on the device. Joining it without the abort meant Start
    // could not return until that read timed out on its own â€” a device timeout
    // is commonly a second or more, and controlMutex_ is held throughout, so
    // pressing Start after the radio dropped froze the application for the
    // whole of it. active_->stop() is idempotent per the IqSource contract,
    // and srcRun_ is already false on the fault path; both are set here anyway
    // so the handshake reads the same as the other two call sites.
    // Bounded like the other two call sites (see quiesceSourceThreadLocked):
    // this join is reached after a fault, and a DSP fault leaves the source
    // thread alive - possibly parked in the very driver read that caused it.
    // Play must not become the freeze Stop was cured of.
    quiesceSourceThreadLocked();
    if (dspThread_.joinable()) { dspThread_.join(); }

    // A quiesce that had to ABANDON the thread leaves the current source with
    // a parked driver thread still inside it. Restarting that same source
    // would put a second thread into a driver that has not returned from the
    // first - the exact two-threads-in-one-driver corruption 0.63.0 ended.
    // Refuse, in words the toolbar shows; the user's way forward is picking
    // another source (setSource leaks the zombie safely) or restarting.
    if (zombieSource_) {
        noteThreadFault("source thread",
                        "the radio's driver is holding an abandoned thread and "
                        "this source cannot restart; pick another source or "
                        "restart FoxSDR");
        return;
    }

    // A restart is a fresh acquisition: re-prime the average and discard
    // samples left in the ring from the previous run, so the first frames
    // reflect the current generator settings rather than stale history.
    // Draining here is safe â€” both threads are joined at this point.
    estimator_.reset();
    std::complex<float> scratch[256];
    while (ring_.read(scratch, 256) != 0) {}

    // Same fresh-acquisition treatment for the audio chain: clear filter
    // histories, oscillator phases, AGC gain, resampler state, and the meter.
    // Squelch has no reset by design â€” its gate/hysteresis state re-converges
    // within milliseconds and carrying it over cannot corrupt audio. Tuning
    // (mode, offset, bandwidth, threshold) survives the restart.
    {
        std::lock_guard<std::mutex> alk(audioMutex_);
        vfo_.reset();
        demod_.reset();
        agc_.reset();
        resampler_.reset();
        resamplerR_.reset();
        resamplerD_.reset();
        meter_.reset();
        // The stereo/RDS decoders and the audio post-chain are stream state
        // too: a restart is a fresh acquisition, and a PS name recovered
        // before the previous Stop is exactly as stale as one from another
        // station.
        resetDecodersLocked();
        notchL_->reset();
        notchR_->reset();
        autoNotchL_->reset();
        autoNotchR_->reset();
        nrL_->reset();
        nrR_->reset();
    }
    signalDb_.store(-200.0f, std::memory_order_relaxed);

    // Start the source before its thread exists so the first read() never
    // races the device open. A failed start is not fatal to the pipeline:
    // the thread runs, read() yields nothing, no frames appear, and the
    // reason stays readable through activeSource().lastError().
    active_->start();

    run_.store(true, std::memory_order_relaxed);
    srcRun_.store(true, std::memory_order_relaxed);
    dspRun_.store(true, std::memory_order_relaxed);
    // Same snapshot-under-controlMutex_ as the setSource respawn above.
    // spawnSourceThread also mints this generation's stop token/exit latch
    // (zombie-safety for stop(); see there).
    spawnSourceThread(cfg_.sampleRateHz);
    dspThread_ = std::thread(&Pipeline::dspThreadMain, this);
}

// THE ONE QUIESCE POLICY, shared by every caller that must get the source
// thread out of the way: stop(), setSource() and start(). It exists as a
// single function because the first version of this policy was applied to
// stop() alone - and the field hang it was written for (4214EAE4) arrives
// through setSource() at least as often: the user does not press Stop when a
// radio wedges, they pick another source, and that path still carried the
// old unbounded join. One helper is what makes "which call sites are safe"
// a non-question.
//
// Caller holds controlMutex_. On return the source thread is either joined
// or abandoned (detached, zombieSource_ set, counted); either way no join on
// srcThread_ can block after this returns.
void Pipeline::quiesceSourceThreadLocked() {
    // NOTHING TO QUIESCE, NOTHING TOUCHED - and the guard is load-bearing,
    // not an optimisation. The first draft of this helper ran its abort
    // unconditionally, which changed start()'s first-ever call from "spawn
    // the thread" to "call active_->stop() on a source that has never read":
    // for a stateful test double (and for any real source whose stop() latches
    // a release), that pre-emptively opened its abort gate, and the FIRST
    // genuine read then fell straight through it. Two long-standing suites
    // caught it within minutes. joinable() is false exactly when there is no
    // thread - never spawned, already joined, or already abandoned - and in
    // every one of those states the source must not be told to abort.
    if (!srcThread_.joinable()) { return; }
    // This generation's dead-man switch first, so even a thread that never
    // sees the flags below (parked in a driver) is condemned before anything
    // waits on it. An abandoned earlier generation holds its OWN token copy;
    // this store cannot reach it.
    if (srcStopToken_) { srcStopToken_->store(false, std::memory_order_relaxed); }
    srcRun_.store(false, std::memory_order_relaxed);
    // Abort an in-flight bounded read BEFORE joining: a self-paced source
    // may be parked inside read() waiting for samples, and its stop() is the
    // contract's abort path (IqSource requires bounded/abortable reads for
    // exactly this shutdown). Idempotent per contract, so calling it on a
    // never-started or already-stopped source is harmless.
    active_->stop();
    if (srcThread_.joinable()) {
        // BOUNDED â€” "joinable" only promises the thread has not yet been
        // joined or detached, never that it will ever finish. A vendor
        // driver's read() can ignore active_->stop() entirely and simply
        // never return from readStream (field report 4214EAE4: a Mirics
        // device that outlived its own 20 ms timeout parameter).
        // SoapySource::stop() bounds ITS OWN wait for the driver lock
        // (kControlLockWait in soapy_source.cpp), but that lock is exactly
        // what the parked read already holds, so bounding it there cannot
        // make the read itself return. Before this fix the unconditional
        // srcThread_.join() that used to sit here waited for that read
        // forever, on whatever thread called Pipeline::stop() â€” the GUI
        // thread, straight out of the toolbar's Stop button.
        //
        // srcExitFuture_ is fulfilled with set_value_at_thread_exit from
        // inside the thread's own lambda (spawnSourceThread), which is what
        // makes "the future is ready" mean "the OS thread is genuinely
        // finished, join() below will return immediately" rather than merely
        // "sourceThreadMain returned" (the two can differ by thread-local
        // teardown time). The wait exists so the join that follows it is
        // provably fast, never so the wait replaces the join.
        const bool exited =
            srcExitFuture_.valid() &&
            srcExitFuture_.wait_for(kSourceJoinWait) == std::future_status::ready;
        if (exited) {
            srcThread_.join();
        } else {
            // ABANDONMENT â€” the same policy soapy_source.cpp's dead-device
            // latch already established one level down, extended here to
            // the thread itself: the interface must survive a driver that
            // will not answer, even at the cost of a leaked thread and
            // (until the next setSource()/~Pipeline() lets it go â€” see
            // there) the source object it may still be parked inside.
            // zombieSource_ is what turns that leak into a documented,
            // DELIBERATE one instead of a later use-after-free.
            srcThread_.detach();
            zombieSource_ = true;
            // Counted for tests and diagnostics: "how many driver
            // threads has this process abandoned" is the number that
            // proves a healthy stop() took the join path - a timing
            // assertion alone cannot tell a fast join from an instant
            // abandonment (found by mutating kSourceJoinWait to 0:
            // every test still passed).
            srcThreadsAbandoned_.fetch_add(1, std::memory_order_relaxed);
            core::diagWarnf(
                "pipeline: source thread did not exit within %lld s of "
                "stop() - the driver's read() never returned; abandoning "
                "the thread rather than freezing the interface (field-hang "
                "class 4214EAE4)",
                static_cast<long long>(kSourceJoinWait.count()));
        }
    }
}

void Pipeline::stop() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    run_.store(false, std::memory_order_relaxed);
    srcRun_.store(false, std::memory_order_relaxed);
    dspRun_.store(false, std::memory_order_relaxed);
    // This generation's dead-man switch, cleared alongside the flags above â€”
    // see spawnSourceThread and the header's zombie-safety addendum on
    // setSource(). A thread that ends up abandoned a few lines down keeps
    // its OWN copy of the shared_ptr (captured by value at spawn), so this
    // store can only ever reach the CURRENT generation; an already-abandoned
    // earlier one holds a token this line never touches.
    quiesceSourceThreadLocked();
    if (dspThread_.joinable()) { dspThread_.join(); }
}

bool Pipeline::running() const {
    return run_.load(std::memory_order_relaxed);
}

bool Pipeline::getLatestFrame(SpectrumFrame& out) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (latest_.seq <= out.seq) { return false; }
    out = latest_;
    return true;
}

void Pipeline::setDeemphasisUs(double us) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    deemphasisUs_ = us;
    // The STEREO decoder owns WFM de-emphasis, for both stereo and mono
    // output (header: "WFM DE-EMPHASIS OWNERSHIP"); demod_ stays flat so the
    // composite reaching RDS keeps its 57 kHz subcarrier.
    stereo_->setDeemphasisUs(us);
}

double Pipeline::deemphasisUs() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return deemphasisUs_;
}

void Pipeline::setDemodMode(cascade::dsp::DemodMode m) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    if (demod_.mode() == m) { return; }  // idempotent: no gratuitous reset
    demod_.setMode(m);  // resets the demod's internal state (its contract)
    // setMode restores the library default de-emphasis; WFM's belongs to
    // StereoFm, so switch it straight back off (see the header).
    demod_.setDeemphasisUs(0.0);
    agc_.reset();       // new level regime: relearn the gain from neutral
    // Leaving WFM abandons the composite the decoders were tracking, and
    // coming back to it is a fresh acquisition either way.
    resetDecodersLocked();
}

void Pipeline::setStereoEnabled(bool on) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    stereoEnabled_ = on;
    // Through StereoFm's own ramped gate, so the toggle fades between mono
    // and stereo instead of stepping â€” a step in the difference channel is an
    // audible click in both speakers.
    stereo_->setForceMono(!on);
}

bool Pipeline::stereoEnabled() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return stereoEnabled_;
}

bool Pipeline::pilotLocked() const {
    return pilotLocked_.load(std::memory_order_relaxed);
}

float Pipeline::pilotLevel() const {
    return pilotLevel_.load(std::memory_order_relaxed);
}

bool Pipeline::stereoActive() const {
    return stereoActive_.load(std::memory_order_relaxed);
}

RdsSnapshot Pipeline::rdsSnapshot() const {
    std::lock_guard<std::mutex> lk(rdsMutex_);
    return rdsPublished_;
}

void Pipeline::resetRds() {
    std::lock_guard<std::mutex> lk(audioMutex_);
    resetDecodersLocked();
}

void Pipeline::setNoiseReductionEnabled(bool on) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    nrL_->setEnabled(on);
    nrR_->setEnabled(on);
}

bool Pipeline::noiseReductionEnabled() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return nrL_->isEnabled();
}

void Pipeline::setNoiseReductionStrength(float s01) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    nrL_->setStrength(s01);
    nrR_->setStrength(s01);
}

float Pipeline::noiseReductionStrength() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return nrL_->strength();
}

void Pipeline::setNotchEnabled(bool on) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    // Clear the biquad memory on the way in: a notch re-enabled with the
    // charge it held when it was switched off thumps.
    if (on && !notchL_->isEnabled()) {
        notchL_->reset();
        notchR_->reset();
    }
    notchL_->setEnabled(on);
    notchR_->setEnabled(on);
}

bool Pipeline::notchEnabled() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return notchL_->isEnabled();
}

void Pipeline::setNotchFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    notchL_->setFrequencyHz(hz);
    notchR_->setFrequencyHz(hz);
}

double Pipeline::notchFrequencyHz() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return notchL_->frequencyHz();
}

void Pipeline::setNotchQ(double q) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    notchL_->setQ(q);
    notchR_->setQ(q);
    autoNotchL_->setQ(q);
    autoNotchR_->setQ(q);
}

double Pipeline::notchQ() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return notchL_->q();
}

void Pipeline::setAutoNotchEnabled(bool on) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    if (on && !autoNotchL_->isEnabled()) {
        autoNotchL_->reset();
        autoNotchR_->reset();
    }
    autoNotchL_->setEnabled(on);
    autoNotchR_->setEnabled(on);
    if (!on) {
        autoNotchEngaged_.store(false, std::memory_order_relaxed);
    }
}

bool Pipeline::autoNotchEnabled() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return autoNotchL_->isEnabled();
}

bool Pipeline::autoNotchEngaged() const {
    return autoNotchEngaged_.load(std::memory_order_relaxed);
}

double Pipeline::autoNotchFrequencyHz() const {
    return autoNotchHz_.load(std::memory_order_relaxed);
}

cascade::dsp::DemodMode Pipeline::demodMode() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return demod_.mode();
}

void Pipeline::setVfoOffsetHz(double offsetHz) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    if (vfo_.offsetHz() == offsetHz) { return; }  // no move, nothing to forget
    vfo_.setOffsetHz(offsetHz);
    // A retune is a different station: the RDS content and the pilot lock
    // that belonged to the old one must not survive it. (The SOURCE centre
    // moving is invisible from here â€” that caller has to call resetRds().)
    resetDecodersLocked();
}

double Pipeline::vfoOffsetHz() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return vfo_.offsetHz();
}

void Pipeline::setVfoBandwidthHz(double bandwidthHz) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    // The raw request is remembered separately from the Vfo's clamped value
    // so a later rate switch can re-apply the caller's intent, re-clamped for
    // the NEW channel rate (clamping a clamp would ratchet the bandwidth).
    vfoBandwidthHz_ = bandwidthHz;
    vfo_.setBandwidthHz(bandwidthHz);
}

void Pipeline::setSquelchDb(float thresholdDb) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    squelchDb_ = thresholdDb;  // remembered for rate-switch rebuilds
    squelch_.setThresholdDb(thresholdDb);
}

float Pipeline::signalPowerDb() const {
    return signalDb_.load(std::memory_order_relaxed);
}

void Pipeline::setAudioMuted(bool muted) {
    audioMuted_.store(muted, std::memory_order_relaxed);
}

bool Pipeline::audioMuted() const {
    return audioMuted_.load(std::memory_order_relaxed);
}

cascade::sink::AudioOut& Pipeline::audio() {
    return audio_;
}

bool Pipeline::setInputRateHz(double rateHz) {
    // Holding controlMutex_ across the WHOLE switch is what makes a
    // stop()-during-switch impossible by construction (documented in the
    // header): stop/start/setSource/dtor all serialize on this mutex.
    std::lock_guard<std::mutex> lk(controlMutex_);

    // Cheap no-op: the chain already follows this exact rate. Checked before
    // validation on purpose â€” "keep doing what you are doing" can never fail.
    if (rateHz == cfg_.sampleRateHz) { return true; }

    unsigned decim = 1;
    double chanRate = 0.0;
    double snapped = 0.0;
    if (!acceptedInputRate(rateHz, &decim, &chanRate, &snapped)) { return false; }
    // From here on the SNAPPED rate is the rate: a device readback that was a
    // hair off an integer is treated as the integer it was aiming at, and
    // everything the chain builds - and everything it later reports - must
    // agree on that one value.
    rateHz = snapped;
    // Re-check the cheap no-op against the snapped value too, or a device
    // whose readback jitters in the last decimal place would tear the whole
    // DSP chain down and rebuild it on every source refresh.
    if (rateHz == cfg_.sampleRateHz) { return true; }

    const bool live = run_.load(std::memory_order_relaxed);
    // Same split setSource makes, and for the same reason: whether to QUIESCE
    // is a question about the thread, whether to RESUME is a question about
    // run_. A thread fault clears run_ from whichever worker died without
    // joining anything, so in that state `live` is false while dspThread_ is
    // still a joinable object.
    //
    // Unlike setSource, gating the quiesce on run_ here was never a
    // use-after-free: setInputRateHz rebuilds only DSP-side blocks, every one
    // of them under audioMutex_, which processAudioBlock holds for its entire
    // body â€” so the rebuild was already mutually excluded from a
    // dying-but-still-live DSP thread, and it never touches the source, which
    // is what setSource's bug destroyed underneath a parked read. That was
    // tested rather than assumed (14400 rate changes across 240 pipelines
    // faulted from both threads: no tear, no hang, no leak). This is a
    // consistency fix, not a bug fix, and joinable() is false exactly when a
    // previous stop()/setInputRateHz already joined and true exactly while a
    // spawned thread has not been â€” so on every healthy path (never started,
    // stopped, running) it agrees with `live` and nothing changes.
    //
    // It is still worth making on two counts: it reaps the corpse a fault
    // leaves behind instead of carrying it to the next start(), and it closes
    // the one route by which this function could have killed the process â€”
    // with the quiesce skipped, the respawn below is a single loosened gate
    // away from assigning a std::thread onto a joinable one, which is an
    // immediate std::terminate. The RESUME deliberately stays on `live`: a
    // rate change is not a restart, and a faulted pipeline must stay down
    // until start() says otherwise.
    //
    // Stated plainly so nobody mistakes the test coverage for more than it is:
    // NO public-API observation separates the two spellings of this gate. The
    // only state in which they differ is `run_ false with dspThread_
    // joinable`, i.e. after a fault, and there dspRun_ is already false and the
    // DSP loop has already left, so joining the corpse changes nothing a caller
    // can see (start() would otherwise join it, stop() and ~Pipeline too). The
    // post-fault rate change IS pinned by test_pipeline.cpp's last block, but
    // that block passes with either spelling by construction â€” this line is a
    // consistency and robustness choice, not a defect fix, and no regression
    // test can guard it without exposing thread identity the class does not
    // publish.
    //
    // The handshake itself is unchanged â€” clear the DSP loop's private gate,
    // then join. run_ stays set (on the live path) so the source thread keeps
    // feeding the ring throughout (a full ring drops overflow by design, never
    // blocks the source); the DSP loop's 1 ms idle sleep bounds the join
    // latency. After the join NO thread touches the estimator or the audio
    // chain, so the rebuild below is race-free by construction. The partial
    // FFT block the thread was accumulating is discarded â€” a rate switch is a
    // stream discontinuity anyway, so mixing old-rate and new-rate samples in
    // one FFT would be worse than dropping less than one frame.
    if (dspThread_.joinable()) {
        dspRun_.store(false, std::memory_order_relaxed);
        dspThread_.join();
    }

    {
        // audioMutex_ is still required even with the DSP thread down:
        // audioTap()/setDemodMode()/setVfo* may arrive concurrently from
        // other control-plane threads. controlMutex_ -> audioMutex_ is the
        // same acquisition order start() uses, and the DSP thread never takes
        // controlMutex_, so the ordering cannot deadlock.
        std::lock_guard<std::mutex> alk(audioMutex_);

        // Preserve caller-visible tuning across the rebuild.
        const double offset = vfo_.offsetHz();
        const cascade::dsp::DemodMode mode = demod_.mode();

        vfoDecim_ = decim;
        vfo_ = cascade::dsp::Vfo(rateHz, decim, vfoBandwidthHz_);
        vfo_.setOffsetHz(offset);

        // The demodulator MUST be rebuilt at the new channel rate: every mode
        // bakes the rate into its coefficients (CW/SSB BFO frequencies, WFM
        // deemphasis pole, AM DC-blocker pole), so a stale demod would e.g.
        // scale the CW sidetone by oldChannel/newChannel.
        demod_ = cascade::dsp::Demodulator(chanRate);
        demod_.setMode(mode);
        // A rebuilt demodulator starts at the library default de-emphasis;
        // WFM's belongs to StereoFm (see the header), so switch it off again
        // â€” leaving it on would put a second 50 us pole in the audio and
        // gut the 57 kHz subcarrier RDS needs.
        demod_.setDeemphasisUs(0.0);

        // Stereo + RDS decoders, rebuilt for the new composite rate (this
        // re-applies the user's de-emphasis and force-mono settings and
        // clears the decoded content).
        rebuildChannelBlocks(chanRate);

        // Squelch: reconstructed so its ~5 ms ramp and hold time stay real
        // time at the new channel rate; the requested threshold survives.
        // Gate state restarts closed â€” a rate switch is a stream restart.
        // 2x, because it gates the interleaved two-channel stream.
        squelch_ = cascade::dsp::Squelch(2.0 * chanRate);
        squelch_.setThresholdDb(squelchDb_);

        // Resampler ratio arithmetic: chanRate is proven integral by
        // acceptedInputRate and is < 300 kHz for every accepted rate, so the
        // unsigned casts are exact; RationalResampler reduces L/M by gcd
        // internally, making channelRate -> 48 kHz exact â€” e.g. 150000 ->
        // 8/25, 187500 -> 32/125, 200000 -> 6/25.
        resampler_ = cascade::dsp::RationalResampler(
            static_cast<unsigned>(kAudioRateHz + 0.5),
            static_cast<unsigned>(chanRate + 0.5));
        // Identical construction, identical (fresh) state: the two channels
        // stay sample-aligned because they are the same state machine driven
        // by the same number of inputs, never because anything syncs them.
        resamplerR_ = cascade::dsp::RationalResampler(
            static_cast<unsigned>(kAudioRateHz + 0.5),
            static_cast<unsigned>(chanRate + 0.5));
        // A THIRD one, for the decoder feed. It has to be its own instance
        // rather than a shared one because it is driven from a different tap
        // point in the chain - before the AGC - and a resampler is a state
        // machine: interleaving two different signals through one would
        // corrupt both.
        resamplerD_ = cascade::dsp::RationalResampler(
            static_cast<unsigned>(kAudioRateHz + 0.5),
            static_cast<unsigned>(chanRate + 0.5));

        // New rate regime: relearn the gain and the S-meter from neutral.
        // (Their per-sample time constants stay tuned for ~200 kHz channels,
        // which every accepted rate >= 300 kHz lands near; below that the
        // ballistics merely slow proportionally â€” an accepted tradeoff.)
        agc_.reset();
        meter_.reset();

        fmScale_ = static_cast<float>(0.5 * chanRate /
                                      (kTwoPi * kFmDeviationHz));
    }
    signalDb_.store(-200.0f, std::memory_order_relaxed);

    // Commit the new rate only after the rebuild cannot fail anymore, so a
    // refused/aborted call really did change nothing. The spectrum estimator
    // and the ring are deliberately untouched (rate-agnostic; see header).
    cfg_.sampleRateHz = rateHz;

    if (live) {
        dspRun_.store(true, std::memory_order_relaxed);
        dspThread_ = std::thread(&Pipeline::dspThreadMain, this);
    }
    return true;
}

double Pipeline::inputRateHz() const {
    std::lock_guard<std::mutex> lk(controlMutex_);
    return cfg_.sampleRateHz;
}

double Pipeline::channelRateHz() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return vfo_.channelRateHz();
}

std::uint64_t Pipeline::audioSamplesProduced() const {
    return audioSamples_.load(std::memory_order_relaxed);
}

void Pipeline::setIqRecorder(Recorder* r) {
    // audioMutex_ (not a new lock): the DSP thread holds it across every
    // writeIq call, so this assignment cannot interleave with a write â€” the
    // serialization guarantee the header documents for record toggles.
    std::lock_guard<std::mutex> lk(audioMutex_);
    iqRecorder_ = r;
}

void Pipeline::setAudioRecorder(Recorder* r) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioRecorder_ = r;
}

std::size_t Pipeline::audioTap(float* dst, std::size_t n) const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    const std::size_t frames = tapBuf_.size() / 2;
    const std::size_t avail = std::min(n, tapFilled_);
    // The newest frame sits at tapWrite_ - 1; walk back `avail` frames and
    // copy forward so dst is in chronological order. Mono downmix: exact for
    // every mono path, where the two channels are bit-identical.
    const std::size_t start = (tapWrite_ + frames - avail) % frames;
    for (std::size_t i = 0; i < avail; ++i) {
        const std::size_t f = (start + i) % frames;
        dst[i] = 0.5f * (tapBuf_[2 * f] + tapBuf_[2 * f + 1]);
    }
    return avail;
}

std::size_t Pipeline::audioTapStereo(float* dstLeft, float* dstRight,
                                     std::size_t n) const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    const std::size_t frames = tapBuf_.size() / 2;
    const std::size_t avail = std::min(n, tapFilled_);
    const std::size_t start = (tapWrite_ + frames - avail) % frames;
    for (std::size_t i = 0; i < avail; ++i) {
        const std::size_t f = (start + i) % frames;
        if (dstLeft != nullptr) { dstLeft[i] = tapBuf_[2 * f]; }
        if (dstRight != nullptr) { dstRight[i] = tapBuf_[2 * f + 1]; }
    }
    return avail;
}

void Pipeline::noteThreadFault(const char* where, const char* what) {
    {
        std::lock_guard<std::mutex> lk(faultMutex_);
        faultMsg_ = std::string(where) + ": " + (what != nullptr ? what : "unknown");
    }
    faulted_.store(true, std::memory_order_relaxed);
    // Bring the rest of the pipeline down with it. A half-running pipeline â€”
    // source dead, DSP still spinning on an empty ring â€” presents as "running"
    // in the UI while producing nothing, which is worse than a clean stop.
    srcRun_.store(false, std::memory_order_relaxed);
    dspRun_.store(false, std::memory_order_relaxed);
    run_.store(false, std::memory_order_relaxed);
}

bool Pipeline::faulted() const { return faulted_.load(std::memory_order_relaxed); }

std::string Pipeline::faultMessage() const {
    std::lock_guard<std::mutex> lk(faultMutex_);
    return faultMsg_;
}

// Mints a fresh stop token + exit latch for one generation of the source
// thread and spawns it against them. See the header (srcStopToken_ /
// srcExitFuture_) and stop() below for the full zombie-safety contract this
// exists to serve; start() and the setSource() resume path both call this
// rather than constructing std::thread directly, so the two spawn sites
// cannot drift apart on it.
void Pipeline::spawnSourceThread(double chainRateHz) {
    auto token = std::make_shared<std::atomic<bool>>(true);
    srcStopToken_ = token;
    auto exitPromise = std::make_shared<std::promise<void>>();
    srcExitFuture_ = exitPromise->get_future();
    // token and exitPromise are captured BY VALUE (shared_ptr copies), which
    // is what makes this generation's identity independent of whatever
    // srcStopToken_/srcExitFuture_ hold by the time this thread actually
    // runs â€” a later spawn overwrites both members with a DIFFERENT pair,
    // and this closure never sees that change.
    srcThread_ = std::thread([this, chainRateHz, token, exitPromise]() {
        sourceThreadMain(chainRateHz, token);
        // Fulfilled from INSIDE the thread, after sourceThreadMain has fully
        // returned, so "the future is ready" means "this OS thread is done
        // and join() will return immediately" â€” see stop()'s use of it.
        exitPromise->set_value_at_thread_exit();
    });
}

// Both worker bodies run inside a catch-all. An exception that escapes a
// std::thread is an immediate std::terminate (Windows reports it as
// 0xC0000409 fail-fast in ucrtbase), and vendor drivers genuinely do throw
// from deep inside a stream read when the USB device is pulled mid-capture â€”
// that is precisely how unplugging the SDR used to kill the whole app.
void Pipeline::sourceThreadMain(double chainRateHz,
                                std::shared_ptr<std::atomic<bool>> stopToken) {
    try {
        sourceThreadBody(chainRateHz, *stopToken);
    } catch (const std::exception& e) {
        noteThreadFault("source thread", e.what());
    } catch (...) {
        noteThreadFault("source thread", "non-standard exception");
    }
}

void Pipeline::dspThreadMain() {
    try {
        dspThreadBody();
    } catch (const std::exception& e) {
        noteThreadFault("DSP thread", e.what());
    } catch (...) {
        noteThreadFault("DSP thread", "non-standard exception");
    }
}

void Pipeline::sourceThreadBody(double chainRateHz,
                                const std::atomic<bool>& stopToken) {
    using clock = std::chrono::steady_clock;

    // Stable for this thread's entire lifetime: setSource only changes
    // active_ after joining this thread (the quiesce handshake), and the
    // spawn/join edges give the necessary happens-before, so one plain read
    // here is race-free.
    cascade::source::IqSource& src = *active_;

    constexpr double kChunkSec = 0.010;
    // Chunk sizing (and, below, free-running pacing) follows the SOURCE's own
    // rate, not the DSP chain's: setInputRateHz deliberately never touches
    // the source side, and a caller-installed source may legitimately run at
    // a different rate than the pipeline was constructed with. Read once at
    // thread entry â€” sources that can change rate are re-read on the next
    // (re)spawn, and rate changes on a live free-running source are refused
    // by the sources themselves (SigGenSource/file are fixed-rate). Fall back
    // to the chain's rate if a source reports a nonsense rate â€” as a SNAPSHOT
    // handed over at spawn (see the header), never a read of cfg_ from here:
    // setInputRateHz writes that field under controlMutex_ without ever
    // joining this thread, so reading it here would be a data race on the one
    // path that actually uses the value.
    double srcRate = src.sampleRateHz();
    if (!(srcRate > 0.0)) { srcRate = chainRateHz; }
    std::size_t chunk = static_cast<std::size_t>(srcRate * kChunkSec + 0.5);
    if (chunk == 0) { chunk = 1; }
    std::vector<std::complex<float>> buf(chunk);

    if (src.selfPaced()) {
        // The device paces: read() blocks (bounded) until samples arrive, so
        // there is NO clock of our own â€” a pacing sleep on the DELIVERING
        // path would let a real-time device's samples back up and be lost. A
        // zero return (bounded block timed out with nothing available) is
        // retried, and the source's abortable stop() is what lets stop() and
        // setSource() get this thread out of read() promptly â€” WHEN the
        // driver honours that abort at all. stopToken is the fallback for
        // when it does not: it is THIS generation's own dead-man switch
        // (see the header and Pipeline::stop()), so a thread that stop()
        // eventually gives up on and abandons still wakes into a token that
        // reads false forever, even if a brand new session's run_/srcRun_
        // have since gone back to true â€” without it, an abandoned thread
        // that finally returns from a stuck read would resume looping and
        // start writing into ring_ beside whatever NEW source thread is now
        // running, which is exactly the two-threads-in-one-driver class of
        // corruption 0.63.0 existed to end, just one level up (the ring
        // instead of the vendor stack).
        // TOKEN FIRST in the condition, and re-tested IMMEDIATELY after the
        // read returns, before anything else. Both orderings are load-bearing
        // for the abandoned case: a zombie waking from a stuck read must not
        // touch ONE member of a Pipeline that has moved on - not the ring
        // (dual-producer corruption beside the new session's thread), not
        // noteThreadFault (it would kill the new session with the old
        // source's message), not even the run_ flags in the loop condition
        // (a shorter-lived Pipeline may be destroyed under it). The token and
        // the leaked source are the only memory an abandoned thread may still
        // read - the token because its shared_ptr is captured by value, the
        // source because setSource()/~Pipeline deliberately leak it.
        while (stopToken.load(std::memory_order_relaxed) &&
               run_.load(std::memory_order_relaxed) &&
               srcRun_.load(std::memory_order_relaxed)) {
            const std::size_t got = src.read(buf.data(), chunk);
            if (!stopToken.load(std::memory_order_relaxed)) { return; }
            if (got != 0) {
                // Same overflow policy as the free-running path: if the DSP
                // side stalled and the ring is full, the excess is dropped
                // (write() accepts what fits) â€” never block a live device.
                ring_.write(buf.data(), got);
            }
            // Device loss does NOT arrive here as an exception: a source that
            // let the driver's throw escape would be torn down mid-fault, so
            // SoapySource catches it and reports through faulted() instead.
            // Which means the catch-all in sourceThreadMain never fires for
            // the commonest hardware failure there is, and without this poll
            // the loop simply kept reading a dead handle â€” the spectrum
            // froze, the app still called itself running, and the user saw a
            // silently dead radio. Route it into the same fault path.
            if (src.faulted()) {
                noteThreadFault("source thread", src.lastError());
                return;
            }
            if (got == 0) {
                // Retry, but not instantly. The comment above used to claim
                // the source's own block bound paced this loop; that holds
                // only while the source still blocks. One that returns 0
                // without blocking â€” a closed or dead handle, a driver whose
                // readStream fails fast â€” turned the retry into a hot spin
                // measured at 44 million reads in 200 ms, i.e. a whole core
                // burned to produce nothing. The backoff runs ONLY when there
                // were no samples, so a healthy device never reaches it and
                // pays nothing; 1 ms is a tenth of the 10 ms chunk period, so
                // even a source that meters data out in small bursts cannot
                // be starved by it.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return;
    }

    // Free-running source: read() always fills the chunk immediately, so
    // this loop supplies the real-time pacing (absolute deadlines, catch-up
    // and stall-resync â€” see the file header comment).
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(kChunkSec));
    auto next = clock::now() + period;

    // stopToken alongside the pre-existing flags for the same reason as the
    // self-paced branch above: it is what keeps an abandoned generation of
    // this thread from ever mistaking a later session's run_/srcRun_ for
    // permission to resume. Free-running sources cannot actually stall
    // read() (it always fills the chunk immediately by contract), so this
    // branch never triggers stop()'s abandonment path in practice â€” the
    // check costs one atomic load and keeps the two loops' shutdown
    // semantics identical rather than silently diverging.
    while (stopToken.load(std::memory_order_relaxed) &&
           run_.load(std::memory_order_relaxed) &&
           srcRun_.load(std::memory_order_relaxed)) {
        const std::size_t got = src.read(buf.data(), chunk);
        // Same post-read token test as the self-paced loop, same reasons.
        if (!stopToken.load(std::memory_order_relaxed)) { return; }
        // A real-time source must not block: if the DSP side stalled and the
        // ring is full, the overflow is dropped (write() accepts what fits).
        ring_.write(buf.data(), got);

        std::this_thread::sleep_until(next);
        next += period;
        const auto now = clock::now();
        if (now - next > std::chrono::milliseconds(250)) {
            next = now;  // resync after a long stall instead of bursting
        }
    }
}

void Pipeline::dspThreadBody() {
    const std::size_t n = cfg_.fftSize;
    std::vector<std::complex<float>> acc(n);
    std::vector<float> db(n);
    std::size_t filled = 0;

    // dspRun_ additionally gates this loop so setInputRateHz can quiesce the
    // DSP thread alone (the source keeps feeding the ring), mirroring how
    // srcRun_ lets setSource quiesce the source thread alone.
    while (run_.load(std::memory_order_relaxed) &&
           dspRun_.load(std::memory_order_relaxed)) {
        filled += ring_.read(acc.data() + filled, n - filled);
        if (filled < n) {
            // Ring drained mid-block: yield instead of spinning. 1 ms is far
            // below the 10 ms production cadence, so this never limits the
            // frame rate; it just caps idle CPU burn.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        filled = 0;
        estimator_.process(acc.data(), db.data());

        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            latest_.dbBins = db;  // vector assignment reuses capacity after the first frame
            latest_.seq += 1;     // strictly increasing; never reset (see header)
        }

        // Duplicate the same drained block into the audio path (the spectrum
        // path above is untouched â€” both consume identical samples).
        processAudioBlock(acc.data(), n);
    }
}

void Pipeline::processAudioBlock(const std::complex<float>* in, std::size_t n) {
    std::lock_guard<std::mutex> lk(audioMutex_);

    // IQ recorder tap (P6): the drained block RAW, before any channel
    // processing â€” "record baseband at the input rate" is exactly the ring's
    // samples. Every sample the DSP thread consumes passes through here, so
    // the recording has no gaps while the pipeline runs. writeIq itself
    // ignores stopped/wrong-kind recorders (its contract), so the hot path
    // pays only this null check when idle.
    if (iqRecorder_ != nullptr) { iqRecorder_->writeIq(in, n); }

    // I/Q decoder plugins tap the RAW device band here, at the same point the
    // I/Q recorder does and for the same reason: this is the last place the
    // full band exists. Below this the VFO mixes, filters and decimates to one
    // narrow channel, which is exactly what an I/Q decoder must not be given -
    // ADS-B alone needs 2 MHz of spectrum, and AIS wants two channels 50 kHz
    // apart. They do their own tuning; the VFO is the user's, not theirs.
    //
    // std::complex<float> is layout-compatible with float[2] (guaranteed by
    // the standard), and the ABI's interleaved I,Q,I,Q rule is that same
    // layout, so this is a reinterpretation and not a copy.
    if (PluginRunner* runner = pluginRunner_.load(std::memory_order_acquire)) {
        runner->processIq(reinterpret_cast<const float*>(in), n);
    }

    // VFO: mix the tuned offset to DC, band-limit, decimate to channel rate.
    chanBuf_.resize(n / vfoDecim_ + 1);
    const std::size_t m = vfo_.process(in, n, chanBuf_.data(), chanBuf_.size());
    if (m == 0) { return; }

    // S-meter: channel power (post-filter, pre-demod), snapshotted for the GUI.
    meter_.process(chanBuf_.data(), m);
    signalDb_.store(meter_.powerDb(), std::memory_order_relaxed);

    // Demodulate 1:1 at the channel rate. In WFM this is the COMPOSITE (MPX)
    // with no de-emphasis applied â€” see the ownership note in the header.
    audioBuf_.resize(m);
    demod_.process(chanBuf_.data(), m, audioBuf_.data());

    const cascade::dsp::DemodMode mode = demod_.mode();
    const bool wfm = (mode == cascade::dsp::DemodMode::WFM);

    // RDS gets the RAW discriminator output: no de-emphasis, no scaling, no
    // AGC, no squelch â€” bit-for-bit the signal the --rds-check bench path
    // feeds the same decoder, which is the only configuration proven off air.
    // (Scale is irrelevant to it anyway: both its loops normalise by the
    // running mean power.)
    if (wfm) {
        rds_->process(audioBuf_.data(), m);
        publishRds();
    }

    // FM modes: rad/sample -> ~0.5 full scale at +/-75 kHz deviation (see the
    // kFmDeviationHz comment). Other modes already emit signal-amplitude
    // scale, which the AGC normalizes.
    if (mode == cascade::dsp::DemodMode::NFM || wfm) {
        for (std::size_t i = 0; i < m; ++i) { audioBuf_[i] *= fmScale_; }
    }

    // --- One channel or two -------------------------------------------------
    leftBuf_.resize(m);
    rightBuf_.resize(m);
    if (wfm) {
        // ALWAYS through the stereo decoder, even when the user forced mono
        // or no pilot is present: that is what makes the transition
        // click-free and tone-identical, because both sides of the switch
        // carry the same 15 kHz low-pass and the same de-emphasis, and the
        // difference channel is ramped in and out by StereoFm's own gate
        // (with the gate shut it emits L == R bit-exactly).
        stereo_->process(audioBuf_.data(), m, leftBuf_.data(), rightBuf_.data());
        pilotLocked_.store(stereo_->pilotLocked(), std::memory_order_relaxed);
        pilotLevel_.store(stereo_->pilotLevel(), std::memory_order_relaxed);
        stereoActive_.store(stereoEnabled_ && stereo_->pilotLocked() &&
                                stereo_->stereoCapable(),
                            std::memory_order_relaxed);
    } else {
        // Every other mode is mono by nature: duplicate, so the rest of the
        // chain has exactly one shape.
        std::copy(audioBuf_.begin(), audioBuf_.begin() + static_cast<std::ptrdiff_t>(m),
                  leftBuf_.begin());
        std::copy(audioBuf_.begin(), audioBuf_.begin() + static_cast<std::ptrdiff_t>(m),
                  rightBuf_.begin());
        pilotLocked_.store(false, std::memory_order_relaxed);
        pilotLevel_.store(0.0f, std::memory_order_relaxed);
        stereoActive_.store(false, std::memory_order_relaxed);
    }

    // --- AGC + squelch on the interleaved pair ------------------------------
    // One instance of each over L,R,L,R... so both channels get the identical
    // gain and the identical gate envelope (header: two per-channel AGCs
    // would each normalise their own channel and flatten the stereo image).
    // gateBuf_ repeats every channel sample so the squelch's power source
    // stays index-aligned with the audio it gates.
    // --- DECODER TAP, BEFORE the AGC and the squelch ------------------------
    //
    // Both of those exist to make audio comfortable to LISTEN to, and a
    // decoder does not listen - it measures. They are two consumers of the
    // same signal with opposite requirements, so each gets what it needs
    // rather than the user being made to mediate.
    //
    // The AGC is not a theoretical objection. Morse is on-off keyed, so during
    // the gaps between elements the AGC ramps its gain up and amplifies the
    // channel filter's ringing at the tone frequency, keeping the tone bin
    // energised through what should be silence. Measured through this path,
    // the CW decoder saw an audio peak of 1.10 - above full scale, the AGC's
    // fingerprint - and read "CQ DE G0ABC" as "TT", every element merged into
    // one long mark. The same mechanism damages any decoder that keys on
    // amplitude.
    //
    // The squelch goes with it for the same reason and one of its own: a weak
    // signal is exactly the one worth decoding, and gating it to digital
    // silence to spare the user a hiss they are not listening to serves
    // nobody. A user who sets a squelch threshold is asking for quiet
    // speakers, not for the decoders to stop.
    //
    // Still AFTER demodulation and BEFORE notch, auto-notch and noise
    // reduction - see the note further down, which is unchanged: those three
    // are perceptual processing and every one of them damages data.
    if (PluginRunner* runner = pluginRunner_.load(std::memory_order_acquire)) {
        preAgcBuf_.resize(m);
        for (std::size_t i = 0; i < m; ++i) {
            preAgcBuf_[i] = 0.5f * (leftBuf_[i] + rightBuf_[i]);
        }
        const std::size_t dcap = resamplerD_.maxOut(m);
        decoderFeed_.resize(dcap);
        const std::size_t dk =
            resamplerD_.process(preAgcBuf_.data(), m, decoderFeed_.data(), dcap);
        if (dk > 0) { runner->processAudio(decoderFeed_.data(), dk); }
    }

    ilvBuf_.resize(2 * m);
    gateBuf_.resize(2 * m);
    for (std::size_t i = 0; i < m; ++i) {
        ilvBuf_[2 * i] = leftBuf_[i];
        ilvBuf_[2 * i + 1] = rightBuf_[i];
        gateBuf_[2 * i] = chanBuf_[i];
        gateBuf_[2 * i + 1] = chanBuf_[i];
    }
    agc_.process(ilvBuf_.data(), ilvBuf_.data(), 2 * m);
    // Squelch measures the pre-demod channel and gates the (AGC'd) audio, so
    // closed-squelch output is exact digital silence after the ramp.
    squelch_.process(gateBuf_.data(), 2 * m, ilvBuf_.data());
    for (std::size_t i = 0; i < m; ++i) {
        leftBuf_[i] = ilvBuf_[2 * i];
        rightBuf_[i] = ilvBuf_[2 * i + 1];
    }

    // --- Channel rate -> 48 kHz, one resampler per channel -------------------
    const std::size_t cap = resampler_.maxOut(m);
    outL_.resize(cap);
    outR_.resize(cap);
    const std::size_t kL =
        resampler_.process(leftBuf_.data(), m, outL_.data(), cap);
    const std::size_t kR =
        resamplerR_.process(rightBuf_.data(), m, outR_.data(), cap);
    // Identical configuration driven by identical input counts from identical
    // initial state: the two can only ever produce the same count. The min is
    // a belt-and-braces guard so a future divergence truncates instead of
    // reading past an end.
    const std::size_t k = std::min(kL, kR);
    if (k == 0) { return; }

    // The decoders were fed further up, before the AGC and the squelch - see
    // the note there. They are deliberately fed before notch, auto-notch and
    // noise reduction too: those three are perceptual processing, and every
    // one of them damages data. The auto-notch hunts continuous tones and
    // would attack an AFSK mark tone or a POCSAG carrier as if it were
    // interference; the noise reduction is spectral subtraction, which smears
    // exactly the sharp transitions a slicer keys on. Feeding decoders the
    // ear-facing audio would make the product's own audio settings silently
    // change decode rates.

    // --- Audio post-processing: notch -> auto-notch -> noise reduction ------
    // Order rationale in the header. All three are bypasses when disabled.
    notchL_->process(outL_.data(), k);
    notchR_->process(outR_.data(), k);
    autoNotchL_->process(outL_.data(), k);
    autoNotchR_->process(outR_.data(), k);
    autoNotchEngaged_.store(autoNotchL_->isEngaged(), std::memory_order_relaxed);
    autoNotchHz_.store(autoNotchL_->frequencyHz(), std::memory_order_relaxed);
    nrL_->process(outL_.data(), k, outL_.data());
    nrR_->process(outR_.data(), k, outR_.data());

    // --- HARD MUTE, at the one point every consumer is downstream of --------
    //
    // See setAudioMuted for why the mute lives in the pipeline rather than at
    // the sink. This line is where "every consumer" is actually true: below it
    // are the mono downmix, the rolling tap (which the web server's audio
    // stream is pumped from), the audio recorder and the sound device, and
    // there is no fifth. Above it are the plugin decoder feed, the spectrum,
    // the S-meter and RDS - none of which the user asked to silence, and one
    // of which is the whole reason the mute exists.
    //
    // WRITTEN RATHER THAN SKIPPED. Returning early would be cheaper and would
    // be a bug: the tap would stop advancing, audioSamplesProduced() would
    // freeze, and the web audio pump reads the difference between successive
    // values of that counter to decide what to send. A frozen counter means
    // the browser is sent nothing at all and simply stalls, which is not the
    // same thing as being sent silence - and the audio-output watchdog reads
    // the same counter to decide whether the chain is alive. Silence has to be
    // produced, not withheld.
    //
    // Read once per block, not per sample, so a mute that arrives mid-block
    // takes effect on the next one - at 48 kHz that is under a millisecond and
    // it keeps the branch out of the inner loop.
    if (audioMuted_.load(std::memory_order_relaxed)) {
        std::fill(outL_.begin(), outL_.begin() + static_cast<std::ptrdiff_t>(k), 0.0f);
        std::fill(outR_.begin(), outR_.begin() + static_cast<std::ptrdiff_t>(k), 0.0f);
    }

    // Mono downmix for the test tap's readers and the recorder, which stays
    // the documented mono 16-bit WAV.
    monoOut_.resize(k);
    for (std::size_t i = 0; i < k; ++i) {
        monoOut_[i] = 0.5f * (outL_[i] + outR_[i]);
    }

    // Test tap + counter BEFORE the sink, so --selftest sees the chain output
    // even with no device open; then the non-blocking push to the sink (a
    // full ring drops the overflow â€” the device, not this thread, is behind).
    const std::size_t tapFrames = tapBuf_.size() / 2;
    for (std::size_t i = 0; i < k; ++i) {
        tapBuf_[2 * tapWrite_] = outL_[i];
        tapBuf_[2 * tapWrite_ + 1] = outR_[i];
        tapWrite_ = (tapWrite_ + 1) % tapFrames;
    }
    tapFilled_ = std::min(tapFrames, tapFilled_ + k);
    // Counted in FRAMES, unchanged in meaning: one per 48 kHz instant.
    audioSamples_.fetch_add(k, std::memory_order_relaxed);
    // Audio recorder tap (P6): the same post-chain 48 kHz samples the test
    // tap above just captured â€” post-squelch, pre-AudioOut, so the recording
    // is independent of the output device and its volume.
    if (audioRecorder_ != nullptr) { audioRecorder_->writeAudio(monoOut_.data(), k); }
    if (audioChannels_ == 2) {
        outIlv_.resize(2 * k);
        for (std::size_t i = 0; i < k; ++i) {
            outIlv_[2 * i] = outL_[i];
            outIlv_[2 * i + 1] = outR_[i];
        }
        audio_.writeStereo(outIlv_.data(), k);
    } else {
        audio_.write(monoOut_.data(), k);
    }
}

}  // namespace cascade::core
