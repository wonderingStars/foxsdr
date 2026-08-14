// Pipeline implementation. Pacing approach (documented per task):
//
// The source thread generates ~10 ms chunks and sleeps to ABSOLUTE deadlines
// (steady_clock, sleep_until, next += period) rather than sleeping a fixed
// duration per chunk. Windows' default timer granularity (~15.6 ms) makes any
// single sleep overshoot badly, but with absolute deadlines an overshoot is
// followed by zero-sleep catch-up chunks until the schedule is level again, so
// the *average* sample rate self-corrects to well within the ~20% tolerance.
// 10 ms chunks keep stop() latency low without busy-spinning. If the thread
// falls more than 250 ms behind (suspend, debugger, load spike) the schedule
// resyncs to "now" instead of bursting an unbounded backlog.
//
// SPDX-License-Identifier: MIT
#include "core/pipeline.hpp"

#include <algorithm>
#include <chrono>

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

// Decimate the input by 10: 2 MS/s -> 200 kHz channel, wide enough for WFM
// (+/-75 kHz deviation plus guard) while cutting the demod/AGC/squelch work
// to a tenth of the input rate.
constexpr unsigned kVfoDecimation = 10;

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
      sigGen_(cfg.sampleRateHz),
      ring_(ringCapacityFor(cfg)),
      estimator_(cfg.fftSize, cascade::dsp::WindowType::BlackmanHarris),
      vfo_(cfg.sampleRateHz, kVfoDecimation, kDefaultVfoBandwidthHz),
      demod_(cfg.sampleRateHz / kVfoDecimation),
      agc_(kAgcTarget, kAgcAttack, kAgcDecay, kAgcMaxGain),
      squelch_(cfg.sampleRateHz / kVfoDecimation),
      meter_(kMeterAlpha),
      resampler_(static_cast<unsigned>(kAudioRateHz + 0.5),
                 static_cast<unsigned>(cfg.sampleRateHz /
                                           static_cast<double>(kVfoDecimation) +
                                       0.5)),
      tapBuf_(kAudioTapSize, 0.0f) {
    estimator_.setAlpha(cfg.averagingAlpha);
    demod_.setMode(cascade::dsp::DemodMode::WFM);  // default mode per spec
    fmScale_ = static_cast<float>(
        0.5 * (cfg.sampleRateHz / static_cast<double>(kVfoDecimation)) /
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
    return sigGen_;
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

    run_.store(true, std::memory_order_relaxed);
    srcThread_ = std::thread(&Pipeline::sourceThreadMain, this);
    dspThread_ = std::thread(&Pipeline::dspThreadMain, this);
}

void Pipeline::stop() {
    std::lock_guard<std::mutex> lk(controlMutex_);
    run_.store(false, std::memory_order_relaxed);
    // Threads notice the flag within one sleep quantum (<= ~10 ms source,
    // ~1 ms DSP); joinable() makes a second stop() a no-op.
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
    vfo_.setBandwidthHz(bandwidthHz);
}

void Pipeline::setSquelchDb(float thresholdDb) {
    std::lock_guard<std::mutex> lk(audioMutex_);
    squelch_.setThresholdDb(thresholdDb);
}

float Pipeline::signalPowerDb() const {
    return signalDb_.load(std::memory_order_relaxed);
}

cascade::sink::AudioOut& Pipeline::audio() {
    return audio_;
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

    constexpr double kChunkSec = 0.010;
    std::size_t chunk = static_cast<std::size_t>(cfg_.sampleRateHz * kChunkSec + 0.5);
    if (chunk == 0) { chunk = 1; }
    std::vector<std::complex<float>> buf(chunk);

    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(kChunkSec));
    auto next = clock::now() + period;

    while (run_.load(std::memory_order_relaxed)) {
        sigGen_.generate(buf.data(), chunk);
        // A real-time source must not block: if the DSP side stalled and the
        // ring is full, the overflow is dropped (write() accepts what fits).
        ring_.write(buf.data(), chunk);

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

    while (run_.load(std::memory_order_relaxed)) {
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
    chanBuf_.resize(n / kVfoDecimation + 1);
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
