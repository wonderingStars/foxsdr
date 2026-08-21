// End-to-end tests for the Pipeline's P7 audio wiring: broadcast-FM stereo,
// the RDS tap, the reset-on-retune rule, and a NaN-bearing capture driven
// through the real file decoder into the real audio chain.
//
// WHY THIS EXISTS. dsp/stereo_fm and dsp/rds already have exhaustive unit
// suites, but both are fed a synthetic composite there. Nothing proved the
// PIPELINE hands them the right signal — and the one thing it could plausibly
// get wrong is exactly the thing that destroys both: leaving the WFM
// discriminator's de-emphasis switched on, which is ~40 dB down at 57 kHz and
// would erase RDS outright while quietly double-de-emphasising the audio.
// So the input here is a real FM carrier, modulated with a real broadcast
// multiplex, and everything is measured at the far end of the chain through
// the audio tap.
//
// The reference signal is built in this file from the published broadcast-FM
// and RDS standards (subcarrier frequencies, the 0.45/0.09/0.45 composite
// proportions, the shortened cyclic (26,16) block code and its offset words),
// the same clean-room way tests/test_rds.cpp builds its own. Nothing is
// compared against a constant taken from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "source/iq_file_source.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPilotHz = 19000.0;
// Arbitrary non-zero pilot phase: a receiver that only works when the
// transmitter's pilot happens to start at phase 0 is not a receiver.
constexpr double kPilotPhase = 0.7;
constexpr double kSubcarrier38Hz = 38000.0;
constexpr double kRdsCarrierHz = 57000.0;
constexpr double kRdsHalfBitHz = 2375.0;  // 2 * 57000/48
constexpr double kAudioToneHz = 1000.0;

// Input rate for the whole test. 250 kHz gives decimation 1 and therefore a
// 250 kHz channel — the same composite rate the off-air --rds-check bench
// path uses — and leaves the 0.9 * channel-rate VFO filter wide enough for a
// Carson bandwidth of 2 * (50 + 57) = 214 kHz. At the app's usual 200 kHz
// channel the RDS sidebands would fold, which would be a defect of the TEST
// signal, not of the receiver.
constexpr double kInputRateHz = 250000.0;
constexpr double kPeakDeviationHz = 50000.0;
constexpr double kVfoBandwidthHz = 220000.0;

// --- RDS encoder (clean room, from the standard) -----------------------------

constexpr std::uint32_t kGen = 0x5B9u;  // x^10+x^8+x^7+x^5+x^4+x^3+1
constexpr std::uint16_t kOffA = 0x0FCu;
constexpr std::uint16_t kOffB = 0x198u;
constexpr std::uint16_t kOffC = 0x168u;
constexpr std::uint16_t kOffD = 0x1B4u;

std::uint16_t crc10(std::uint16_t data) {
    std::uint32_t reg = static_cast<std::uint32_t>(data) << 10;
    for (int bit = 25; bit >= 10; --bit) {
        if (((reg >> bit) & 1u) != 0u) { reg ^= (kGen << (bit - 10)); }
    }
    return static_cast<std::uint16_t>(reg & 0x3FFu);
}

struct BitStream {
    std::vector<std::uint8_t> bits;

    void push(std::uint32_t value, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((value >> i) & 1u));
        }
    }
    void block(std::uint16_t data, std::uint16_t offset) {
        push(data, 16);
        push(static_cast<std::uint32_t>(crc10(data) ^ offset), 10);
    }
    void group(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d) {
        block(a, kOffA);
        block(b, kOffB);
        block(c, kOffC);
        block(d, kOffD);
    }
};

std::uint16_t blockB(unsigned type, bool tp, unsigned pty, unsigned rest5) {
    return static_cast<std::uint16_t>(((type & 0xFu) << 12) | (0u << 11) |
                                      ((tp ? 1u : 0u) << 10) |
                                      ((pty & 0x1Fu) << 5) | (rest5 & 0x1Fu));
}

std::uint16_t pack2(char hi, char lo) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(hi)) << 8) |
        static_cast<std::uint8_t>(lo));
}

// Interleaved 0A (PS) and 2A (RadioText) groups, repeated: a real station
// sends both continuously, and interleaving them exercises the group-type
// dispatch the decoder does per group rather than one type in isolation.
std::vector<std::uint8_t> buildRdsBits(std::uint16_t pi, const std::string& ps8,
                                       const std::string& rt, unsigned pty,
                                       int cycles) {
    BitStream bs;
    for (int c = 0; c < cycles; ++c) {
        for (unsigned addr = 0; addr < 4; ++addr) {
            const unsigned rest = (1u << 4) | (1u << 3) | addr;  // TA, MS=music
            bs.group(pi, blockB(0u, true, pty, rest), 0xE0CDu,
                     pack2(ps8[2 * addr], ps8[2 * addr + 1]));
        }
        for (unsigned addr = 0; 4u * addr < rt.size(); ++addr) {
            bs.group(pi, blockB(2u, true, pty, addr & 0xFu),
                     pack2(rt[4 * addr], rt[4 * addr + 1]),
                     pack2(rt[4 * addr + 2], rt[4 * addr + 3]));
        }
    }
    return bs.bits;
}

// Differential encode (t[k] = t[k-1] XOR d[k]) then biphase: 1 -> (+1,-1),
// 0 -> (-1,+1).
std::vector<int> biphaseLevels(const std::vector<std::uint8_t>& dataBits) {
    std::vector<int> levels;
    levels.reserve(2 * dataBits.size());
    int prev = 0;
    for (const std::uint8_t b : dataBits) {
        prev ^= (b & 1);
        levels.push_back(prev ? +1 : -1);
        levels.push_back(prev ? -1 : +1);
    }
    return levels;
}

// --- An FM broadcast transmitter as an IqSource ------------------------------

// Generates a constant-envelope FM carrier at DC, modulated by a standard
// stereo multiplex carrying a HARD LEFT audio tone (L = tone, R = 0) plus the
// RDS subcarrier. Free-running, so the pipeline paces it in real time exactly
// as it paces the built-in generator; samples are synthesised on demand, so
// the whole transmission costs a few hundred bytes of state.
class FmStereoSource : public cascade::source::IqSource {
public:
    explicit FmStereoSource(std::vector<int> rdsLevels)
        : levels_(std::move(rdsLevels)) {}

    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kInputRateHz; }
    bool setSampleRateHz(double) override { return false; }  // fixed-rate
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override {
        centerHz_ = hz;  // nominal: there is no tuner, the modulation is fixed
        return true;
    }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(sample_) / kInputRateHz;
            ++sample_;
            phase_ += kTwoPi * kPeakDeviationHz * mpx(t) / kInputRateHz;
            if (phase_ > kTwoPi) { phase_ -= kTwoPi; }
            if (phase_ < -kTwoPi) { phase_ += kTwoPi; }
            dst[i] = std::complex<float>(static_cast<float>(std::cos(phase_)),
                                         static_cast<float>(std::sin(phase_)));
        }
        return n;
    }

    const char* name() const override { return "FM stereo test transmitter"; }
    const char* lastError() const override { return ""; }

private:
    // The composite, normalised to unit peak so kPeakDeviationHz really is
    // the peak deviation. Proportions are the standard ones: 45% sum, 9%
    // pilot, 45% difference on a suppressed 38 kHz carrier, ~5% RDS.
    //
    // The subcarrier is cos(2p) where the pilot is cos(p) — the standard's
    // "exactly twice the pilot frequency AND twice its phase", which is the
    // whole reason a pilot is transmitted at all. Getting that phase relation
    // wrong does not merely attenuate the difference channel, it multiplies
    // it by cos(error): at 90 degrees the separation is exactly 0 dB, which
    // is indistinguishable from a receiver that never decoded stereo. The
    // pilot also carries a non-zero absolute phase, as a real transmitter's
    // does, so nothing here can accidentally pass on a phase of zero.
    double mpx(double t) const {
        const double left = std::sin(kTwoPi * kAudioToneHz * t);  // R = 0
        const double p = kTwoPi * kPilotHz * t + kPilotPhase;
        double v = 0.45 * left;  // (L+R)
        v += 0.09 * std::cos(p);
        v += 0.45 * left * std::cos(2.0 * p);  // (L-R), DSB-SC at 2p
        if (!levels_.empty()) {
            const std::size_t m =
                static_cast<std::size_t>(t * kRdsHalfBitHz) % levels_.size();
            v += 0.05 * static_cast<double>(levels_[m]) *
                 std::cos(kTwoPi * kRdsCarrierHz * t);
        }
        return v / 1.04;  // 0.45 + 0.09 + 0.45 + 0.05
    }

    std::vector<int> levels_;
    std::uint64_t sample_ = 0;
    double phase_ = 0.0;
    double centerHz_ = 100000000.0;
    bool running_ = false;
};

// --- Measurement helpers ------------------------------------------------------

double rms(const std::vector<float>& v, std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        acc += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return (n == 0) ? 0.0 : std::sqrt(acc / static_cast<double>(n));
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b,
               std::size_t n) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        acc += d * d;
    }
    return (n == 0) ? 0.0 : std::sqrt(acc / static_cast<double>(n));
}

// Waits (bounded) for `pred` to hold, polling every 20 ms. Returns whether it
// held before the deadline, so the caller reports a real failure instead of
// hanging the suite.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// --- A float32 IQ capture on disk, and one run of the whole chain over it ----
//
// WHY A FILE. Every non-finite guard in this tree — the decoder's entry
// sanitise, the AGC, the squelch/S-meter EMA, the noise reducer — is proven in
// isolation by its own unit suite, and each of those suites feeds its subject
// directly. Nothing showed the pieces WIRED: that a capture carrying NaN and
// both infinities, decoded by the real file source and pushed through the real
// pipeline, cannot put a single non-finite sample into the audio the user
// hears, and that the chain is still tracking the signal afterwards rather
// than sitting latched. That is an integration claim, so the input has to
// arrive the way a corrupt recording really does: as a WAV on disk.
//
// Only the minimum RIFF a valid float32 capture needs is written here. Every
// header edge case belongs to tests/test_iq_file_source.cpp, which pins them.

void putU16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFFu));
}

void putU32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 16) & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 24) & 0xFFu));
}

void putTag(std::vector<unsigned char>& v, const char* tag4) {
    v.insert(v.end(), tag4, tag4 + 4);
}

bool writeFloatIqWav(const std::string& path, std::uint32_t rateHz,
                     const std::vector<float>& interleaved) {
    std::vector<unsigned char> v;
    putTag(v, "RIFF");
    putU32(v, 0);  // patched below
    putTag(v, "WAVE");
    putTag(v, "fmt ");
    putU32(v, 16);
    putU16(v, 3);  // WAVE_FORMAT_IEEE_FLOAT
    putU16(v, 2);  // ch0 = I, ch1 = Q
    putU32(v, rateHz);
    putU32(v, rateHz * 2u * 4u);  // byte rate
    putU16(v, 8);                 // block align
    putU16(v, 32);                // bits per sample
    putTag(v, "data");
    putU32(v, static_cast<std::uint32_t>(interleaved.size() * 4));
    for (const float s : interleaved) {
        putU32(v, std::bit_cast<std::uint32_t>(s));
    }
    const std::uint32_t riffSize = static_cast<std::uint32_t>(v.size() - 8);
    v[4] = static_cast<unsigned char>(riffSize & 0xFFu);
    v[5] = static_cast<unsigned char>((riffSize >> 8) & 0xFFu);
    v[6] = static_cast<unsigned char>((riffSize >> 16) & 0xFFu);
    v[7] = static_cast<unsigned char>((riffSize >> 24) & 0xFFu);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size()));
    return static_cast<bool>(f);
}

// 240 kHz: decimation round(240000/200000) = 1, so the channel rate is the
// input rate and integer, which the pipeline's rate contract requires.
constexpr double kNanFileRateHz = 240000.0;
constexpr std::size_t kNanFileFrames = 24000;  // 100 ms per pass through the file
constexpr float kCarrierAmplitude = 0.5f;

// A constant DC phasor. In CW a carrier sitting on the VFO centre beats
// against the demodulator's own 700 Hz sidetone, so the audio the chain must
// keep producing is a pure tone whose PITCH comes from the mode and whose
// LEVEL comes from the AGC — neither is read back from the implementation, and
// neither depends on modulation the file would have to carry.
std::vector<float> dcCarrierFrames() {
    std::vector<float> v(2 * kNanFileFrames, 0.0f);
    for (std::size_t i = 0; i < kNanFileFrames; ++i) {
        v[2 * i] = kCarrierAmplitude;  // I
        v[2 * i + 1] = 0.0f;           // Q
    }
    return v;
}

// The same carrier with a short burst of NaN and both infinities spliced in.
// The burst is deliberately SHORT relative to the file (64 frames in 24000,
// 0.27 ms in 100 ms): the source loops, so the chain meets the burst afresh on
// every pass and is still meeting it while the measurement below is taken —
// which is what makes "it recovered" mean "it recovers every time", not "it
// survived one hit". At that duty cycle the hole itself moves the audio RMS by
// far less than the tolerance used, while a latched AGC gain, squelch EMA or
// noise-reduction spectrum moves it by everything.
std::vector<float> poisonedCarrierFrames() {
    std::vector<float> v = dcCarrierFrames();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    constexpr std::size_t kBurstStart = 800;
    constexpr std::size_t kBurstFrames = 64;
    for (std::size_t i = 0; i < kBurstFrames; ++i) {
        const std::size_t f = kBurstStart + i;
        v[2 * f] = (i % 3 == 0) ? nan : ((i % 3 == 1) ? inf : -inf);
        v[2 * f + 1] = (i % 2 == 0) ? -nan : -inf;
    }
    return v;
}

// Dominant audio frequency of one tap window on the 48 kHz grid, by direct DFT
// over bins 1..N/2 (DC excluded so it can never win). An in-test reference
// computation, independent of the project's FFT wrapper.
double dominantHz(const std::vector<float>& w) {
    std::size_t bestBin = 1;
    double bestPow = -1.0;
    for (std::size_t k = 1; k <= w.size() / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        const double s = -kTwoPi * static_cast<double>(k) /
                         static_cast<double>(w.size());
        for (std::size_t i = 0; i < w.size(); ++i) {
            const double angle = s * static_cast<double>(i);
            const double x = static_cast<double>(w[i]);
            re += x * std::cos(angle);
            im += x * std::sin(angle);
        }
        const double pw = re * re + im * im;
        if (pw > bestPow) {
            bestPow = pw;
            bestBin = k;
        }
    }
    return static_cast<double>(bestBin) *
           cascade::core::Pipeline::kAudioRateHz /
           static_cast<double>(w.size());
}

struct ChainRun {
    int windows = 0;          // tap windows actually inspected
    std::size_t nonFinite = 0;  // audio samples that were not finite
    double rms = 0.0;         // steady-state audio level
    double toneHz = -1.0;     // steady-state audio pitch
};

constexpr int kNanWindows = 16;   // ~1.4 s of audio at 48 kHz
constexpr int kNanWarmup = 6;     // windows discarded while the AGC converges
constexpr std::size_t kNanWin = 4096;  // one full tap window

// Plays `wavPath` through a complete pipeline — file decoder, VFO, CW demod,
// AGC, squelch, noise reduction — and watches the audio tap, the last point
// before the samples are handed to the sound device.
//
// Consecutive windows are gated on the producer counter advancing by a whole
// window, so the windows tile the stream rather than re-reading one another;
// nothing here is timed against the wall clock beyond the liveness deadline
// that turns a stalled chain into a failure instead of a hang.
ChainRun playThroughChain(const std::string& wavPath) {
    ChainRun r;
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = kNanFileRateHz;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // headless: the tap is what gets measured
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setDemodMode(cascade::dsp::DemodMode::CW);
    pipeline.setVfoOffsetHz(0.0);
    pipeline.setSquelchDb(-120.0f);           // in the path, never gating
    pipeline.setNoiseReductionEnabled(true);  // and the noise reducer with it

    auto src = std::make_unique<cascade::source::IqFileSource>();
    if (!src->open(wavPath)) {
        std::printf("  open failed: %s\n", src->lastError());
        return r;
    }
    pipeline.setSource(std::move(src));
    pipeline.start();

    std::vector<float> win(kNanWin, 0.0f);
    double acc = 0.0;
    int accWindows = 0;
    for (int i = 0; i < kNanWindows; ++i) {
        const std::uint64_t base = pipeline.audioSamplesProduced();
        const bool advanced = waitFor(
            [&] { return pipeline.audioSamplesProduced() - base >= kNanWin; },
            30000);
        if (!advanced) { break; }
        if (pipeline.audioTap(win.data(), win.size()) != win.size()) { continue; }
        ++r.windows;
        for (const float s : win) {
            if (!std::isfinite(s)) { ++r.nonFinite; }
        }
        if (i >= kNanWarmup) {
            // Sum of squares over every steady-state window: a level that is
            // not finite would poison this, which the nonFinite count catches
            // first and reports specifically.
            for (const float s : win) {
                acc += static_cast<double>(s) * static_cast<double>(s);
            }
            ++accWindows;
        }
    }
    if (accWindows > 0) {
        r.rms = std::sqrt(acc / (static_cast<double>(accWindows) *
                                 static_cast<double>(kNanWin)));
        r.toneHz = dominantHz(win);  // the last window, i.e. steady state
    }
    pipeline.stop();
    return r;
}

}  // namespace

int main() {
    const std::string ps = "CASCADE!";        // exactly 8 characters
    const std::string rt = "STEREO RDS TEST ";  // whole 4-char segments
    constexpr std::uint16_t kPi = 0x2C41;
    constexpr unsigned kPty = 10;

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = kInputRateHz;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // headless: the tap is what gets measured
    cascade::core::Pipeline pipeline(cfg);

    // WFM is the construction default; the bandwidth has to be widened from
    // the 150 kHz broadcast default so the test signal is not truncated, and
    // the squelch opened so nothing gates the measurement.
    pipeline.setVfoBandwidthHz(kVfoBandwidthHz);
    pipeline.setSquelchDb(-120.0f);
    pipeline.setStereoEnabled(true);
    pipeline.setSource(std::make_unique<FmStereoSource>(
        biphaseLevels(buildRdsBits(kPi, ps, rt, kPty, 200))));
    pipeline.start();

    // --- Pilot lock + RDS decode from a real modulated carrier --------------
    const bool decoded = waitFor(
        [&] {
            const cascade::core::RdsSnapshot s = pipeline.rdsSnapshot();
            return s.state.psValid && s.state.piValid && pipeline.pilotLocked() &&
                   pipeline.audioSamplesProduced() > 48000;
        },
        30000);
    const cascade::core::RdsSnapshot snap = pipeline.rdsSnapshot();
    std::printf("rds: synced=%d pi=%04X ps=\"%s\" pty=%u groups=%u errors=%u\n",
                snap.synced ? 1 : 0, snap.state.pi, snap.state.ps.c_str(),
                static_cast<unsigned>(snap.state.pty), snap.state.groupsDecoded,
                snap.state.blockErrors);
    CHECK(decoded);
    CHECK(snap.synced);
    CHECK(snap.state.piValid);
    CHECK(snap.state.pi == kPi);
    CHECK(snap.state.psValid);
    CHECK(snap.state.ps == ps);
    CHECK(snap.state.pty == kPty);
    CHECK(snap.state.tp);
    CHECK(snap.state.ta);
    // RadioText arrives a group at a time; it is a prefix of the message
    // until the last segment lands, so require it to be one.
    CHECK(!snap.state.radioText.empty());
    CHECK(rt.rfind(snap.state.radioText, 0) == 0);

    // The pilot must be reported LOCKED and the stereo path ACTIVE — the
    // indicator the GUI lights.
    CHECK(pipeline.pilotLocked());
    CHECK(pipeline.stereoActive());
    CHECK(pipeline.pilotLevel() > 0.5f);

    // --- Stereo separation ---------------------------------------------------
    // The transmitter sends the tone hard left, so the decoded left channel
    // must dominate the right one. This is the assertion that fails the
    // instant the pipeline stops routing L/R separately (or feeds the stereo
    // decoder a de-emphasised composite, which collapses the difference
    // channel along with the pilot).
    constexpr std::size_t kWin = 4096;
    std::vector<float> left(kWin, 0.0f);
    std::vector<float> right(kWin, 0.0f);
    std::size_t got = pipeline.audioTapStereo(left.data(), right.data(), kWin);
    CHECK(got == kWin);
    const double lRms = rms(left, got);
    const double rRms = rms(right, got);
    const double separationDb = 20.0 * std::log10((lRms + 1e-12) / (rRms + 1e-12));
    std::printf("stereo: L rms=%.5f R rms=%.5f separation=%.1f dB\n", lRms, rRms,
                separationDb);
    CHECK(lRms > 0.01);          // there is actually audio
    CHECK(separationDb > 12.0);  // mono duplication would read 0.0 dB

    // --- Forced mono: both channels carry the same audio ---------------------
    // Not bit-identical: the single AGC walks its gain between the two
    // interleaved samples of a frame, which is a per-sample difference of at
    // most rate * (target - level) — under 0.3% here, and it is what buys
    // both channels one common gain law. What must hold is that the
    // DIFFERENCE collapses.
    pipeline.setStereoEnabled(false);
    CHECK(!pipeline.stereoEnabled());
    // 20 ms of gate ramp plus a full tap window of new audio at 48 kHz.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    got = pipeline.audioTapStereo(left.data(), right.data(), kWin);
    CHECK(got == kWin);
    const double monoDiff = rmsDiff(left, right, got) / (rms(left, got) + 1e-12);
    std::printf("forced mono: |L-R| / |L| = %.6f\n", monoDiff);
    CHECK(monoDiff < 0.01);
    CHECK(!pipeline.stereoActive());  // enabled == false, whatever the pilot does

    // Re-enabling brings the difference channel back, so the toggle is not a
    // one-way door.
    pipeline.setStereoEnabled(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    got = pipeline.audioTapStereo(left.data(), right.data(), kWin);
    const double reDiff = rmsDiff(left, right, got) / (rms(left, got) + 1e-12);
    std::printf("stereo again: |L-R| / |L| = %.6f\n", reDiff);
    CHECK(reDiff > 0.5);

    // --- RDS must be forgotten on a retune -----------------------------------
    // A decoder has no idea the receiver moved; if the pipeline does not
    // reset it, the previous station's PS name stays on screen over the new
    // one. Checked immediately after the retune, which is orders of magnitude
    // sooner than the ~0.4 s any re-acquisition would need.
    CHECK(pipeline.rdsSnapshot().state.psValid);  // still there before the move
    pipeline.setVfoOffsetHz(60000.0);
    const cascade::core::RdsSnapshot after = pipeline.rdsSnapshot();
    std::printf("after retune: psValid=%d piValid=%d ps=\"%s\" synced=%d\n",
                after.state.psValid ? 1 : 0, after.state.piValid ? 1 : 0,
                after.state.ps.c_str(), after.synced ? 1 : 0);
    CHECK(!after.state.psValid);
    CHECK(after.state.ps.empty());
    CHECK(!after.state.piValid);
    CHECK(!after.synced);
    CHECK(after.state.radioText.empty());
    CHECK(!pipeline.pilotLocked());  // the stereo decoder is reset with it

    // A mode change is the other way a station is abandoned.
    pipeline.setVfoOffsetHz(0.0);
    CHECK(waitFor([&] { return pipeline.rdsSnapshot().state.psValid; }, 30000));
    pipeline.setDemodMode(cascade::dsp::DemodMode::AM);
    CHECK(!pipeline.rdsSnapshot().state.psValid);
    CHECK(!pipeline.stereoActive());

    pipeline.stop();

    // --- The mono tap is exactly (L + R)/2 of the same frames ----------------
    // Deliberately AFTER stop(): the tap is a rolling window, so two reads
    // taken while the DSP thread is publishing can legitimately land on
    // windows shifted by a block, and every sample would then "mismatch" for
    // a reason that has nothing to do with the downmix. With the producer
    // stopped both reads see the identical window and the identity is exact.
    // (--selftest depends on this: its 700 Hz measurement reads the mono tap
    // of what is now a two-channel chain.)
    std::vector<float> mono(kWin, 0.0f);
    const std::size_t frames = pipeline.audioTapStereo(left.data(), right.data(), kWin);
    CHECK(pipeline.audioTap(mono.data(), kWin) == frames);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        if (mono[i] != 0.5f * (left[i] + right[i])) { ++mismatches; }
    }
    std::printf("mono downmix mismatches over %zu frames: %zu\n", frames, mismatches);
    CHECK(frames == kWin);
    CHECK(mismatches == 0u);

    // --- A NaN-bearing capture, decoder to audio, in one run -----------------
    //
    // WHAT THIS PINS: the WIRING. That a float32 capture carrying NaN and both
    // infinities, read by source/iq_file_source and pushed through the live
    // pipeline (VFO, CW demod, AGC, squelch, noise reduction), puts no
    // non-finite sample into the audio stream, and that the chain keeps
    // TRACKING the signal while the bursts keep arriving instead of latching.
    //
    // WHAT IT CANNOT PIN: which guard did the containing. Each individual
    // guard is proven, and proven to be load-bearing, by its own unit suite —
    // tests/test_iq_file_source.cpp for the decoder's entry sanitise,
    // test_demod_agc.cpp for the AGC, test_squelch.cpp for the gate and meter,
    // test_noise_reduction.cpp for the spectral state. This case would still
    // pass if one of them were removed and another happened to cover for it,
    // which is exactly why those suites exist alongside it.
    //
    // The reference is a second run of the SAME chain over the SAME carrier
    // with the burst left out — the idiom test_demod_agc.cpp uses for AGC
    // recovery — so the expected audio level is measured, never a constant
    // read back from the implementation.
    {
        const std::string tag = std::to_string(TEST_GETPID());
        const std::string cleanPath = "pipeline_audio_" + tag + "_clean.wav";
        const std::string nanPath = "pipeline_audio_" + tag + "_nan.wav";
        CHECK(writeFloatIqWav(cleanPath,
                              static_cast<std::uint32_t>(kNanFileRateHz),
                              dcCarrierFrames()));
        CHECK(writeFloatIqWav(nanPath, static_cast<std::uint32_t>(kNanFileRateHz),
                              poisonedCarrierFrames()));

        const ChainRun clean = playThroughChain(cleanPath);
        const ChainRun poisoned = playThroughChain(nanPath);
        std::printf("clean:    windows=%d nonFinite=%zu rms=%.6f tone=%.1f Hz\n",
                    clean.windows, clean.nonFinite, clean.rms, clean.toneHz);
        std::printf("poisoned: windows=%d nonFinite=%zu rms=%.6f tone=%.1f Hz\n",
                    poisoned.windows, poisoned.nonFinite, poisoned.rms,
                    poisoned.toneHz);

        // The reference itself really moved: audio flowed for the whole run
        // and it is the 700 Hz CW sidetone at a level the AGC drove up. A
        // reference that produced silence would make every comparison below
        // vacuously true.
        CHECK(clean.windows == kNanWindows);
        CHECK(clean.nonFinite == 0u);
        CHECK(clean.rms > 0.01);
        CHECK(std::fabs(clean.toneHz - 700.0) <= 40.0);

        // (a) Not one non-finite sample ever reaches the audio stream.
        //
        // Measured note on what makes this red: the containment is REDUNDANT.
        // dsp/noise_reduction's per-sample isfinite substitution runs on every
        // audio sample whether or not reduction is enabled — the pipeline
        // calls process() unconditionally — so it alone keeps this count at 0
        // even with the decoder's entry sanitise deleted (verified: the level
        // in (b) collapses, this count does not). Deleting BOTH is what turns
        // this red, which is the honest statement of what it watches: the
        // whole path's output, not any one guard in it.
        CHECK(poisoned.windows == kNanWindows);
        CHECK(poisoned.nonFinite == 0u);

        // (b) Recovery: with bursts still arriving every 100 ms, the audio is
        // the same tone at the same level as the run that never saw one.
        //
        // The 3% band is set from the separation actually measured, not from
        // taste: a healthy build lands 0.10-0.14% below the reference over
        // repeated runs (the burst's own 0.27% duty-cycle hole, sanitised to
        // silence at the decoder), while a build with that entry sanitise
        // removed lands 12.6-12.9% below it — NaN reaches the channel filter
        // and each 64-frame burst takes a filter-length hole of audio with it.
        // Two orders of magnitude apart, so 3% is a threshold rather than a
        // fitted bound. A latched AGC gain, squelch EMA or noise-reduction
        // spectrum leaves the level at zero, not within any band at all.
        CHECK(std::fabs(poisoned.toneHz - 700.0) <= 40.0);
        CHECK(poisoned.rms > 0.0);
        CHECK(std::fabs(poisoned.rms - clean.rms) <= 0.03 * clean.rms);

        // Kept on failure for inspection, removed on success — the same
        // convention tests/test_iq_file_source.cpp uses for its captures.
        if (g_checksFailed == 0) {
            std::remove(cleanPath.c_str());
            std::remove(nanPath.c_str());
        }
    }

    return testSummary("test_pipeline_audio");
}
