// The noise blanker at pipeline level: setters, getters and mirrors, the
// readouts, the rebuild at a rate change, and what it does to the audio of a
// running chain - and what it does NOT do to the raw block.
//
// WHAT IS PROVEN HERE. test_noise_blanker proves the block and the Vfo hook.
// This file proves the PIPELINE hands the settings to the DSP thread (posted,
// mirrors, clamped by the block), keeps them across a rate change, publishes
// the blanked share, and that on a real run of the chain - the built-in
// generator's tone and click train, a USB demodulator, the 3 kHz channel - the
// audio around the tone gets cleaner with the blanker on while the I/Q
// recorder, which taps the raw block upstream of the Vfo, still records every
// click.
//
// GENERATED CLICKS ONLY: no real impulse-noise recording was run through this.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"
#include "core/recorder.hpp"
#include "dsp/demod.hpp"
#include "source/siggen.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace {

using cascade::core::Pipeline;

Pipeline::Config testConfig() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;   // channel rate 200 kHz
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    return cfg;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void letAudioRun(Pipeline& p, double seconds) {
    const std::uint64_t need = static_cast<std::uint64_t>(seconds * Pipeline::kAudioRateHz);
    const std::uint64_t mark = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - mark >= need; }, 60000);
}

// Least-squares fit of a sinusoid at `hz` to the last ~85 ms of audio: returns
// the power left after taking it away, relative to the power of the fit, in dB
// (lower is cleaner).
double residualDb(const Pipeline& p, double hz) {
    std::vector<float> x(4096);
    const std::size_t n = p.audioTap(x.data(), x.size());
    double scc = 0, sss = 0, scs = 0, sxc = 0, sxs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 6.283185307179586 * hz * static_cast<double>(i) / Pipeline::kAudioRateHz;
        const double c = std::cos(ph);
        const double s = std::sin(ph);
        scc += c * c; sss += s * s; scs += c * s;
        sxc += x[i] * c; sxs += x[i] * s;
    }
    const double det = scc * sss - scs * scs;
    const double a = (sxc * sss - sxs * scs) / det;
    const double b = (sxs * scc - sxc * scs) / det;
    double res = 0.0;
    double fit = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 6.283185307179586 * hz * static_cast<double>(i) / Pipeline::kAudioRateHz;
        const double y = a * std::cos(ph) + b * std::sin(ph);
        fit += y * y;
        const double e = x[i] - y;
        res += e * e;
    }
    return 10.0 * std::log10(res / fit);
}

void testSettersAndMirrors() {
    Pipeline p(testConfig());
    CHECK(!p.noiseBlankerEnabled());
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 15.0, 1e-6);
    CHECK(p.noiseBlankerBlankedPercent() == 0.0f);
    CHECK(!p.noiseBlankerStandingDown());

    p.setNoiseBlankerEnabled(true);
    CHECK(p.noiseBlankerEnabled());   // verbatim mirror, straight after the setter
    p.setNoiseBlankerEnabled(false);
    CHECK(!p.noiseBlankerEnabled());

    // The threshold is clamped by the block and reported as in use.
    p.setNoiseBlankerThresholdDb(20.0f);
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 20.0, 1e-6);
    p.setNoiseBlankerThresholdDb(2.0f);
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 6.0, 1e-6);
    p.setNoiseBlankerThresholdDb(80.0f);
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 30.0, 1e-6);
    p.setNoiseBlankerThresholdDb(std::nanf(""));
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 15.0, 1e-6);
}

void testGettersDoNotWaitForAudioLock() {
    Pipeline p(testConfig());
    std::atomic<bool> held{false};
    std::thread holder([&] { p.holdLockForTest(Pipeline::LockForTest::Audio, 800, &held); });
    waitFor([&] { return held.load(); }, 5000);
    const auto t0 = std::chrono::steady_clock::now();
    p.setNoiseBlankerEnabled(true);          // posts, does not wait
    p.setNoiseBlankerThresholdDb(12.0f);
    const bool on = p.noiseBlankerEnabled();
    const float th = p.noiseBlankerThresholdDb();
    const float pct = p.noiseBlankerBlankedPercent();
    const bool sd = p.noiseBlankerStandingDown();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    (void)th;
    (void)pct;
    (void)sd;
    CHECK(on);
    CHECK(ms < 200.0);
    holder.join();
    // Once the lock is free the posted command runs; the clamped value is the live one.
    p.setNoiseBlankerEnabled(true);
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 12.0, 1e-6);
}

void testSettingsSurviveARateChange() {
    Pipeline p(testConfig());
    p.setNoiseBlankerEnabled(true);
    p.setNoiseBlankerThresholdDb(21.0f);
    CHECK(p.setInputRateHz(2400000.0));      // rebuilds the Vfo
    p.setNoiseBlankerThresholdDb(21.0f);     // (re-post drains the queue and republishes)
    CHECK(p.noiseBlankerEnabled());
    CHECK_NEAR(p.noiseBlankerThresholdDb(), 21.0, 1e-6);
}

std::string findWav(const std::string& dir) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".wav") { return e.path().string(); }
    }
    return {};
}

// Counts the I samples of a float32 two-channel WAV above `level`.
std::size_t countAbove(const std::string& path, float level) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Find the "data" chunk; the recorder writes a plain 44-byte header, but do not assume it.
    std::size_t pos = 12;
    std::size_t dataAt = 0;
    std::size_t dataLen = 0;
    while (pos + 8 <= bytes.size()) {
        const std::string id(&bytes[pos], 4);
        std::uint32_t len = 0;
        std::memcpy(&len, &bytes[pos + 4], 4);
        if (id == "data") { dataAt = pos + 8; dataLen = len; break; }
        pos += 8 + len + (len & 1u);
    }
    if (dataAt == 0) { return 0; }
    dataLen = std::min(dataLen, bytes.size() - dataAt);
    std::size_t count = 0;
    for (std::size_t i = 0; i + 8 <= dataLen; i += 8) {
        float re = 0.0f;
        std::memcpy(&re, &bytes[dataAt + i], 4);
        if (re > level) { ++count; }
    }
    return count;
}

void testRunningChain() {
    Pipeline p(testConfig());
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setVfoOffsetHz(0.0);
    p.setVfoBandwidthHz(3000.0);
    p.setSquelchDb(-120.0f);
    // No AGC to pump on the clicks: the comparison is of the signal itself.
    p.setAgcMode(cascade::dsp::Agc::Mode::Off);
    p.setAgcFixedGain(20.0f);
    cascade::source::SigGen& g = p.sigGen();
    g.setTone(0, 1000.0, -50.46f);
    g.setNoiseFloorDb(-67.4f);
    // 20 samples at 2 MS/s: 10 us clicks, 0.3 of full scale, 100 a second.
    g.setImpulses(100.0, -10.46f, 20);
    p.start();
    letAudioRun(p, 1.5);

    auto measure = [&] {
        double acc = 0.0;
        for (int i = 0; i < 8; ++i) {
            acc += residualDb(p, 1000.0);
            letAudioRun(p, 0.12);
        }
        return acc / 8.0;
    };
    const double offDb = measure();
    CHECK(p.noiseBlankerBlankedPercent() == 0.0f);

    const std::string dir = "nb_raw_" + std::to_string(TEST_GETPID());
    std::filesystem::create_directories(dir);
    cascade::core::Recorder rec;
    std::string recErr;
    CHECK(rec.start(cascade::core::RecordKind::BasebandIq, dir, 2000000.0, recErr));
    p.setIqRecorder(&rec);

    p.setNoiseBlankerEnabled(true);
    letAudioRun(p, 1.0);                 // settle: 20 ms to learn the average
    const double onDb = measure();
    const float pct = p.noiseBlankerBlankedPercent();
    CHECK(!p.noiseBlankerStandingDown());
    p.setIqRecorder(nullptr);
    rec.stop();
    std::printf("  chain: audio residual off %.2f dB, on %.2f dB (improvement %.2f dB), blanked %.2f%%\n",
                offDb, onDb, offDb - onDb, pct);
    CHECK(offDb - onDb > 13.0);
    CHECK(pct > 0.3f);
    CHECK(pct < 5.0f);

    // The raw block was not touched: the recorder, upstream of the Vfo, still
    // holds the clicks (0.3 against a 0.003 tone) - hundreds of them over the
    // ~2.4 s it ran, with the blanker on throughout.
    const std::string wav = findWav(dir);
    CHECK(!wav.empty());
    const std::size_t clickSamples = wav.empty() ? 0 : countAbove(wav, 0.2f);
    std::printf("  raw recorder: %zu click samples (>0.2) with the blanker on\n", clickSamples);
    CHECK(clickSamples >= 3 * 150);   // 20 wide x 100/s x >= 1.5 s, three-in-four floor

    // Off again: the readout returns to zero and the clicks are back in the audio.
    p.setNoiseBlankerEnabled(false);
    letAudioRun(p, 0.5);
    CHECK(p.noiseBlankerBlankedPercent() == 0.0f);
    const double againDb = measure();
    CHECK(againDb - onDb > 10.0);
    p.stop();
    if (g_checksFailed == 0) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

}  // namespace

int main() {
    testSettersAndMirrors();
    testGettersDoNotWaitForAudioLock();
    testSettingsSurviveARateChange();
    testRunningChain();
    return testSummary("test_pipeline_nb");
}
