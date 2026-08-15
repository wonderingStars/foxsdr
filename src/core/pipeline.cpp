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
// SPDX-License-Identifier: MIT
#include "core/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

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
constexpr float kAgcTarget = 0.5f;
constexpr float kAgcAttack = 0.005f;
constexpr float kAgcDecay = 0.0005f;
constexpr float kAgcMaxGain = 100.0f;

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
      squelch_(cfg.sampleRateHz / static_cast<double>(vfoDecim_)),
      meter_(kMeterAlpha),
      resampler_(static_cast<unsigned>(kAudioRateHz + 0.5),
                 static_cast<unsigned>(cfg.sampleRateHz /
                                           static_cast<double>(vfoDecim_) +
                                       0.5)),
      tapBuf_(kAudioTapSize, 0.0f) {
    active_ = &builtin_;  // the generator feeds the ring until setSource says otherwise
    estimator_.setAlpha(cfg.averagingAlpha);
    demod_.setMode(cascade::dsp::DemodMode::WFM);  // default mode per spec
    fmScale_ = static_cast<float>(
        0.5 * (cfg.sampleRateHz / static_cast<double>(vfoDecim_)) /
        (kTwoPi * kFmDeviationHz));
    // Open the default output device up front (not in start()) so a device
    // chosen through audio() before/between runs is never stomped by a later
    // start(); stop() leaves the stream open, playing silence once the ring
    // drains. A failed open (headless box) degrades to a deviceless sink:
    // write() still accepts samples, nothing ever blocks.
    if (cfg_.audioEnabled) { audio_.open(-1, kAudioRateHz); }
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
        meter_.reset();
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

void Pipeline::setDemodMode(cascade::dsp::DemodMode m) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    demod_.setMode(m);  // resets the demod's internal state (its contract)
    agc_.reset();       // new level regime: relearn the gain from neutral
}

cascade::dsp::DemodMode Pipeline::demodMode() const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    return demod_.mode();
}

void Pipeline::setVfoOffsetHz(double offsetHz) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    vfo_.setOffsetHz(offsetHz);
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

        // Squelch: reconstructed so its ~5 ms ramp and hold time stay real
        // time at the new channel rate; the requested threshold survives.
        // Gate state restarts closed — a rate switch is a stream restart.
        squelch_ = cascade::dsp::Squelch(chanRate);
        squelch_.setThresholdDb(squelchDb_);

        // Resampler ratio arithmetic: chanRate is proven integral by
        // acceptedInputRate and is < 300 kHz for every accepted rate, so the
        // unsigned casts are exact; RationalResampler reduces L/M by gcd
        // internally, making channelRate -> 48 kHz exact — e.g. 150000 ->
        // 8/25, 187500 -> 32/125, 200000 -> 6/25.
        resampler_ = cascade::dsp::RationalResampler(
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

std::size_t Pipeline::audioTap(float* dst, std::size_t n) const {
    std::lock_guard<std::mutex> lk(audioMutex_);
    const std::size_t avail = std::min(n, tapFilled_);
    // The newest sample sits at tapWrite_ - 1; walk back `avail` samples and
    // copy forward so dst is in chronological order.
    const std::size_t start =
        (tapWrite_ + tapBuf_.size() - avail) % tapBuf_.size();
    for (std::size_t i = 0; i < avail; ++i) {
        dst[i] = tapBuf_[(start + i) % tapBuf_.size()];
    }
    return avail;
}

void Pipeline::sourceThreadMain() {
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

void Pipeline::dspThreadMain() {
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

    // VFO: mix the tuned offset to DC, band-limit, decimate to channel rate.
    chanBuf_.resize(n / vfoDecim_ + 1);
    const std::size_t m = vfo_.process(in, n, chanBuf_.data(), chanBuf_.size());
    if (m == 0) { return; }

    // S-meter: channel power (post-filter, pre-demod), snapshotted for the GUI.
    meter_.process(chanBuf_.data(), m);
    signalDb_.store(meter_.powerDb(), std::memory_order_relaxed);

    // Demodulate 1:1 at the channel rate.
    audioBuf_.resize(m);
    demod_.process(chanBuf_.data(), m, audioBuf_.data());

    // FM modes: rad/sample -> ~0.5 full scale at +/-75 kHz deviation (see the
    // kFmDeviationHz comment). Other modes already emit signal-amplitude
    // scale, which the AGC normalizes.
    const cascade::dsp::DemodMode mode = demod_.mode();
    if (mode == cascade::dsp::DemodMode::NFM ||
        mode == cascade::dsp::DemodMode::WFM) {
        for (std::size_t i = 0; i < m; ++i) { audioBuf_[i] *= fmScale_; }
    }

    agc_.process(audioBuf_.data(), audioBuf_.data(), m);
    // Squelch measures the pre-demod channel and gates the (AGC'd) audio, so
    // closed-squelch output is exact digital silence after the ramp.
    squelch_.process(chanBuf_.data(), m, audioBuf_.data());

    // Channel rate -> 48 kHz for the device.
    outBuf_.resize(resampler_.maxOut(m));
    const std::size_t k =
        resampler_.process(audioBuf_.data(), m, outBuf_.data(), outBuf_.size());
    if (k == 0) { return; }

    // Test tap + counter BEFORE the sink, so --selftest sees the chain output
    // even with no device open; then the non-blocking push to the sink (a
    // full ring drops the overflow — the device, not this thread, is behind).
    for (std::size_t i = 0; i < k; ++i) {
        tapBuf_[tapWrite_] = outBuf_[i];
        tapWrite_ = (tapWrite_ + 1) % tapBuf_.size();
    }
    tapFilled_ = std::min(tapBuf_.size(), tapFilled_ + k);
    audioSamples_.fetch_add(k, std::memory_order_relaxed);
    audio_.write(outBuf_.data(), k);
}

}  // namespace cascade::core
