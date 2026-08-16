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
// with no pacing sleep at all — any sleep of our own would let a real-time
// device's samples pile up and be lost. Zero returns (a bounded block that
// timed out empty) are retried immediately; the bound on read() is also what
// lets stop()/setSource() quiesce the thread promptly (see setSource).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

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
// decimation allows — wide enough for WFM (+/-75 kHz deviation plus guard)
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
// needs an exact integer L/M ratio for channelRate -> 48 kHz — a fractional
// channel rate could only be approximated, silently detuning all audio.
// A NaN rate fails the range test (every comparison with NaN is false).
constexpr double kMinInputRateHz = 8000.0;
constexpr double kMaxInputRateHz = 61.44e6;

bool acceptedInputRate(double rateHz, unsigned* decimOut, double* chanRateOut) {
    if (!(rateHz >= kMinInputRateHz && rateHz <= kMaxInputRateHz)) {
        return false;
    }
    const unsigned decim = decimationForInputRate(rateHz);
    const double chanRate = rateHz / static_cast<double>(decim);
    if (chanRate != std::floor(chanRate)) { return false; }
    *decimOut = decim;
    *chanRateOut = chanRate;
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
// same real-time constants. For mono content — every non-WFM mode, where the
// two channels are identical — the halved rate applied twice per sample
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
    // carried over — a rate change is a stream discontinuity, and a PS name
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
    if (live) {
        // Quiesce handshake (documented in the header): flag the source loop
        // down, abort any in-flight bounded read via the source's own stop()
        // — the order matters, a read that returns after the flag flips must
        // see srcRun_ false — then join. After the join no thread can touch
        // the outgoing source.
        srcRun_.store(false, std::memory_order_relaxed);
        active_->stop();
        if (srcThread_.joinable()) { srcThread_.join(); }
    }
    // Swap. Destroying the outgoing external source here is race-free: the
    // source thread is joined (or never existed), and it was the only other
    // toucher.
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
        srcThread_ = std::thread(&Pipeline::sourceThreadMain, this);
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
    if (srcThread_.joinable()) { srcThread_.join(); }
    if (dspThread_.joinable()) { dspThread_.join(); }

    // A restart is a fresh acquisition: re-prime the average and discard
    // samples left in the ring from the previous run, so the first frames
    // reflect the current generator settings rather than stale history.
    // Draining here is safe — both threads are joined at this point.
    estimator_.reset();
    std::complex<float> scratch[256];
    while (ring_.read(scratch, 256) != 0) {}

    // Same fresh-acquisition treatment for the audio chain: clear filter
    // histories, oscillator phases, AGC gain, resampler state, and the meter.
    // Squelch has no reset by design — its gate/hysteresis state re-converges
    // within milliseconds and carrying it over cannot corrupt audio. Tuning
    // (mode, offset, bandwidth, threshold) survives the restart.
    {
        std::lock_guard<std::mutex> alk(audioMutex_);
        vfo_.reset();
        demod_.reset();
        agc_.reset();
        resampler_.reset();
        resamplerR_.reset();
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
    srcThread_ = std::thread(&Pipeline::sourceThreadMain, this);
    dspThread_ = std::thread(&Pipeline::dspThreadMain, this);
}

void Pipeline::stop() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    run_.store(false, std::memory_order_relaxed);
    srcRun_.store(false, std::memory_order_relaxed);
    dspRun_.store(false, std::memory_order_relaxed);
    // Abort an in-flight bounded read BEFORE joining: a self-paced source
    // may be parked inside read() waiting for samples, and its stop() is the
    // contract's abort path (IqSource requires bounded/abortable reads for
    // exactly this shutdown). Idempotent per contract, so calling it on a
    // never-started or already-stopped source is harmless. Free-running
    // threads instead notice the flags within one sleep quantum (<= ~10 ms
    // source, ~1 ms DSP); joinable() makes a second stop() a no-op.
    active_->stop();
    if (srcThread_.joinable()) { srcThread_.join(); }
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
    // and stereo instead of stepping — a step in the difference channel is an
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
    // moving is invisible from here — that caller has to call resetRds().)
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

cascade::sink::AudioOut& Pipeline::audio() {
    return audio_;
}

bool Pipeline::setInputRateHz(double rateHz) {
    // Holding controlMutex_ across the WHOLE switch is what makes a
    // stop()-during-switch impossible by construction (documented in the
    // header): stop/start/setSource/dtor all serialize on this mutex.
    std::lock_guard<std::mutex> lk(controlMutex_);

    // Cheap no-op: the chain already follows this exact rate. Checked before
    // validation on purpose — "keep doing what you are doing" can never fail.
    if (rateHz == cfg_.sampleRateHz) { return true; }

    unsigned decim = 1;
    double chanRate = 0.0;
    if (!acceptedInputRate(rateHz, &decim, &chanRate)) { return false; }

    const bool live = run_.load(std::memory_order_relaxed);
    if (live) {
        // Quiesce the DSP thread — the mirror image of the setSource source-
        // thread handshake: clear its private gate, then join. run_ stays set
        // so the source thread keeps feeding the ring throughout (a full ring
        // drops overflow by design, never blocks the source); the DSP loop's
        // 1 ms idle sleep bounds the join latency. After the join NO thread
        // touches the estimator or the audio chain, so the rebuild below is
        // race-free by construction. The partial FFT block the thread was
        // accumulating is discarded — a rate switch is a stream discontinuity
        // anyway, so mixing old-rate and new-rate samples in one FFT would be
        // worse than dropping less than one frame.
        dspRun_.store(false, std::memory_order_relaxed);
        if (dspThread_.joinable()) { dspThread_.join(); }
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
        // — leaving it on would put a second 50 us pole in the audio and
        // gut the 57 kHz subcarrier RDS needs.
        demod_.setDeemphasisUs(0.0);

        // Stereo + RDS decoders, rebuilt for the new composite rate (this
        // re-applies the user's de-emphasis and force-mono settings and
        // clears the decoded content).
        rebuildChannelBlocks(chanRate);

        // Squelch: reconstructed so its ~5 ms ramp and hold time stay real
        // time at the new channel rate; the requested threshold survives.
        // Gate state restarts closed — a rate switch is a stream restart.
        // 2x, because it gates the interleaved two-channel stream.
        squelch_ = cascade::dsp::Squelch(2.0 * chanRate);
        squelch_.setThresholdDb(squelchDb_);

        // Resampler ratio arithmetic: chanRate is proven integral by
        // acceptedInputRate and is < 300 kHz for every accepted rate, so the
        // unsigned casts are exact; RationalResampler reduces L/M by gcd
        // internally, making channelRate -> 48 kHz exact — e.g. 150000 ->
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

        // New rate regime: relearn the gain and the S-meter from neutral.
        // (Their per-sample time constants stay tuned for ~200 kHz channels,
        // which every accepted rate >= 300 kHz lands near; below that the
        // ballistics merely slow proportionally — an accepted tradeoff.)
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
    // writeIq call, so this assignment cannot interleave with a write — the
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
    // Bring the rest of the pipeline down with it. A half-running pipeline —
    // source dead, DSP still spinning on an empty ring — presents as "running"
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

// Both worker bodies run inside a catch-all. An exception that escapes a
// std::thread is an immediate std::terminate (Windows reports it as
// 0xC0000409 fail-fast in ucrtbase), and vendor drivers genuinely do throw
// from deep inside a stream read when the USB device is pulled mid-capture —
// that is precisely how unplugging the SDR used to kill the whole app.
void Pipeline::sourceThreadMain() {
    try {
        sourceThreadBody();
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

void Pipeline::sourceThreadBody() {
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
    // thread entry — sources that can change rate are re-read on the next
    // (re)spawn, and rate changes on a live free-running source are refused
    // by the sources themselves (SigGenSource/file are fixed-rate). Fall back
    // to the chain's rate if a source reports a nonsense rate.
    double srcRate = src.sampleRateHz();
    if (!(srcRate > 0.0)) { srcRate = cfg_.sampleRateHz; }
    std::size_t chunk = static_cast<std::size_t>(srcRate * kChunkSec + 0.5);
    if (chunk == 0) { chunk = 1; }
    std::vector<std::complex<float>> buf(chunk);

    if (src.selfPaced()) {
        // The device paces: read() blocks (bounded) until samples arrive, so
        // there is NO clock of our own — a pacing sleep here would let a
        // real-time device's samples back up and be lost. A zero return
        // (bounded block timed out with nothing available) is retried
        // immediately; the source's block bound is what keeps this loop from
        // spinning hot, and its abortable stop() is what lets stop() and
        // setSource() get this thread out of read() promptly.
        while (run_.load(std::memory_order_relaxed) &&
               srcRun_.load(std::memory_order_relaxed)) {
            const std::size_t got = src.read(buf.data(), chunk);
            if (got != 0) {
                // Same overflow policy as the free-running path: if the DSP
                // side stalled and the ring is full, the excess is dropped
                // (write() accepts what fits) — never block a live device.
                ring_.write(buf.data(), got);
            }
        }
        return;
    }

    // Free-running source: read() always fills the chunk immediately, so
    // this loop supplies the real-time pacing (absolute deadlines, catch-up
    // and stall-resync — see the file header comment).
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(kChunkSec));
    auto next = clock::now() + period;

    while (run_.load(std::memory_order_relaxed) &&
           srcRun_.load(std::memory_order_relaxed)) {
        const std::size_t got = src.read(buf.data(), chunk);
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
        // path above is untouched — both consume identical samples).
        processAudioBlock(acc.data(), n);
    }
}

void Pipeline::processAudioBlock(const std::complex<float>* in, std::size_t n) {
    std::lock_guard<std::mutex> lk(audioMutex_);

    // IQ recorder tap (P6): the drained block RAW, before any channel
    // processing — "record baseband at the input rate" is exactly the ring's
    // samples. Every sample the DSP thread consumes passes through here, so
    // the recording has no gaps while the pipeline runs. writeIq itself
    // ignores stopped/wrong-kind recorders (its contract), so the hot path
    // pays only this null check when idle.
    if (iqRecorder_ != nullptr) { iqRecorder_->writeIq(in, n); }

    // VFO: mix the tuned offset to DC, band-limit, decimate to channel rate.
    chanBuf_.resize(n / vfoDecim_ + 1);
    const std::size_t m = vfo_.process(in, n, chanBuf_.data(), chanBuf_.size());
    if (m == 0) { return; }

    // S-meter: channel power (post-filter, pre-demod), snapshotted for the GUI.
    meter_.process(chanBuf_.data(), m);
    signalDb_.store(meter_.powerDb(), std::memory_order_relaxed);

    // Demodulate 1:1 at the channel rate. In WFM this is the COMPOSITE (MPX)
    // with no de-emphasis applied — see the ownership note in the header.
    audioBuf_.resize(m);
    demod_.process(chanBuf_.data(), m, audioBuf_.data());

    const cascade::dsp::DemodMode mode = demod_.mode();
    const bool wfm = (mode == cascade::dsp::DemodMode::WFM);

    // RDS gets the RAW discriminator output: no de-emphasis, no scaling, no
    // AGC, no squelch — bit-for-bit the signal the --rds-check bench path
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

    // --- Decoder plugins tap in HERE, and the position is deliberate --------
    // Post-demodulation, post-AGC, post-squelch, resampled to the fixed audio
    // rate the decoders were created for - but BEFORE notch, auto-notch and
    // noise reduction.
    //
    // Those three are perceptual processing: they exist to make a voice
    // pleasanter to a human ear, and every one of them damages data. The
    // auto-notch hunts continuous tones and would attack an AFSK mark tone or
    // a POCSAG carrier as if it were interference; the noise reduction is
    // spectral subtraction, which smears exactly the sharp transitions a
    // slicer keys on. Feeding decoders the ear-facing audio would make the
    // product's own audio settings silently change decode rates.
    //
    // Mono because that is what the audio decoder ABI carries, and the two
    // channels are identical here in every mode except WFM stereo.
    if (PluginRunner* runner = pluginRunner_.load(std::memory_order_acquire)) {
        decoderFeed_.resize(k);
        for (std::size_t i = 0; i < k; ++i) {
            decoderFeed_[i] = 0.5f * (outL_[i] + outR_[i]);
        }
        runner->processAudio(decoderFeed_.data(), k);
    }

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

    // Mono downmix for the test tap's readers and the recorder, which stays
    // the documented mono 16-bit WAV.
    monoOut_.resize(k);
    for (std::size_t i = 0; i < k; ++i) {
        monoOut_[i] = 0.5f * (outL_[i] + outR_[i]);
    }

    // Test tap + counter BEFORE the sink, so --selftest sees the chain output
    // even with no device open; then the non-blocking push to the sink (a
    // full ring drops the overflow — the device, not this thread, is behind).
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
    // tap above just captured — post-squelch, pre-AudioOut, so the recording
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
