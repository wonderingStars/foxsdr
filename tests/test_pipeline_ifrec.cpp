// test_pipeline_ifrec.cpp - THE IF AS A FILE (engine serial lane A, row P4): Pipeline::setIfRecorder.
//
// Two carriers (with noise and a 20 ppm clock error) go through a real Pipeline tuned to the first: one 1,500 Hz from the tuned frequency, inside a 6 kHz passband, and one 60 kHz away, outside it.
//   A  the file is a complex 32-bit float WAV at the CHANNEL rate (the header says so), as long as the time that passed; the in-band carrier is there (1,500 Hz) and the out-of-band one is at least 50 dB
//   under it - the file is the passband only;  B  as 16-bit integer I/Q the file is half the size and says so;  C  the squelch does not gate it (a closed squelch records the same);  D  detaching stops it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
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
#include "core/recorder.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Pipeline;
using cascade::core::RecordKind;
using cascade::core::Recorder;

namespace {

constexpr double kRate = 2400000.0;   // channel rate 200 kHz
constexpr double kPi = 3.14159265358979323846;

class TwoCarriers final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.002);
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double t = static_cast<double>(k_) * 1.00002 / kRate;
            const double a = 2.0 * kPi * 1500.0 * t, b = 2.0 * kPi * 60000.0 * t;
            dst[i] = {static_cast<float>(0.25 * std::cos(a) + 0.25 * std::cos(b) + noise(rng_)), static_cast<float>(0.25 * std::sin(a) + 0.25 * std::sin(b) + noise(rng_))};
        }
        return n;
    }
    const char* name() const override { return "two carriers"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::mt19937_64 rng_{0x1f};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}
std::vector<unsigned char> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint32_t u32(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint32_t>(v[at]) | (static_cast<std::uint32_t>(v[at + 1]) << 8) | (static_cast<std::uint32_t>(v[at + 2]) << 16) | (static_cast<std::uint32_t>(v[at + 3]) << 24); }
std::uint16_t u16(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint16_t>(v[at] | (v[at + 1] << 8)); }

std::vector<std::complex<float>> decode(const std::vector<unsigned char>& f, bool int16) {
    std::vector<std::complex<float>> out;
    const std::size_t step = int16 ? 4 : 8;
    for (std::size_t at = 44; at + step <= f.size(); at += step) {
        if (int16) {
            out.emplace_back(static_cast<float>(static_cast<std::int16_t>(u16(f, at))) / 32768.0f, static_cast<float>(static_cast<std::int16_t>(u16(f, at + 2))) / 32768.0f);
        } else {
            out.emplace_back(std::bit_cast<float>(u32(f, at)), std::bit_cast<float>(u32(f, at + 4)));
        }
    }
    return out;
}
// Hann-windowed power in dB at `hz` (complex input: signed frequency).
double powerDb(const std::vector<std::complex<float>>& x, std::size_t start, std::size_t n, double hz, double fs) {
    std::complex<double> acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const double ph = -2.0 * kPi * hz * static_cast<double>(i) / fs;
        acc += std::complex<double>(x[start + i]) * w * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return 10.0 * std::log10(std::max(std::norm(acc) / (static_cast<double>(n) * static_cast<double>(n)), 1e-30));
}

}  // namespace

int main() {
    std::printf("test_pipeline_ifrec\n");
    const fs::path dir = fs::path("ifrec_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    p.setSource(std::make_unique<TwoCarriers>());
    p.setDemodMode(cascade::dsp::DemodMode::AM);
    p.setVfoBandwidthHz(6000.0);
    p.setSquelchDb(-120.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 48000; }, 30000));
    const double chan = p.channelRateHz();
    CHECK(chan == 200000.0);

    auto take = [&](int fmt, double secs, std::string& pathOut) {
        Recorder rec;
        CHECK(rec.setSampleFormat(RecordKind::BasebandIq, fmt));
        std::string error;
        CHECK(rec.start(RecordKind::BasebandIq, dir.string(), chan, error, "if"));
        p.setIfRecorder(&rec);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(secs * 1000.0)));
        p.setIfRecorder(nullptr);
        pathOut = rec.path();
        rec.stop();
    };

    // A: float
    std::string path;
    take(0, 1.5, path);
    std::vector<unsigned char> f = slurp(path);
    CHECK(f.size() > 44);
    CHECK(u16(f, 20) == 3 && u16(f, 22) == 2 && u16(f, 34) == 32 && u32(f, 24) == 200000u);
    const std::vector<std::complex<float>> x = decode(f, false);
    std::printf("  A: %zu samples = %.2f s at %.0f Hz\n", x.size(), static_cast<double>(x.size()) / chan, chan);
    CHECK(static_cast<double>(x.size()) > 0.7 * 1.5 * chan && static_cast<double>(x.size()) < 1.3 * 1.5 * chan);
    const std::size_t n = 65536, start = x.size() > n + 20000 ? 20000 : 0;
    CHECK(x.size() >= n);
    if (x.size() >= n) {
        const double inBand = powerDb(x, start, n, 1500.0, chan), outBand = powerDb(x, start, n, 60000.0, chan);
        std::printf("  A: in-band carrier %.1f dB, out-of-band carrier %.1f dB (%.1f dB down)\n", inBand, outBand, inBand - outBand);
        CHECK(inBand > -20.0);
        CHECK(inBand - outBand > 50.0);
    }

    // B: 16-bit integer: half the size, says so, same content
    std::string path16;
    take(cascade::core::kRecordIqInt16, 1.5, path16);
    const std::vector<unsigned char> g = slurp(path16);
    CHECK(g.size() > 44 && u16(g, 20) == 1 && u16(g, 22) == 2 && u16(g, 34) == 16 && u32(g, 24) == 200000u);
    const std::vector<std::complex<float>> y = decode(g, true);
    if (y.size() >= n) { CHECK(powerDb(y, 0, n, 1500.0, chan) > -20.0); }

    // C: a closed squelch does not gate it
    p.setSquelchDb(20.0f);
    std::string pathSq;
    take(0, 1.0, pathSq);
    const std::vector<unsigned char> h = slurp(pathSq);
    CHECK(h.size() > 44 + 8 * static_cast<std::size_t>(0.5 * chan));
    p.setSquelchDb(-120.0f);

    // D: detached, it writes nothing more
    Recorder rec;
    std::string error;
    CHECK(rec.start(RecordKind::BasebandIq, dir.string(), chan, error, "if"));
    p.setIfRecorder(&rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    p.setIfRecorder(nullptr);
    const std::uint64_t at = rec.samplesWritten();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(rec.samplesWritten() == at && at > 0);
    rec.stop();
    p.stop();
    fs::remove_all(dir, ec);
    return testSummary("test_pipeline_ifrec");
}
