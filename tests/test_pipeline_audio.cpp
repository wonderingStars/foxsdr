// End-to-end tests for the Pipeline's P7 audio wiring: broadcast-FM stereo,
// the RDS tap, and the reset-on-retune rule.
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
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
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

    return testSummary("test_pipeline_audio");
}
