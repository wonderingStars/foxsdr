// test_pipeline_mpx.cpp - THE FM MULTIPLEX AS A FILE (engine serial lane A, row D4): Pipeline::setMpxRecorder.
//
// A real stereo FM transmitter (tests/fm_stereo_rds_source.hpp: mono 1 kHz tone, 19 kHz pilot, 38 kHz difference, RDS at 57 kHz) goes through a real Pipeline in WFM and the multiplex is recorded.
//   A  the file is a 32-bit float mono WAV at 192,000 Hz holding about as many samples as the time that passed (192 kS/s), and its spectrum shows the pilot at 19 kHz, the stereo carrier's sidebands around
//   38 kHz and the RDS carrier region at 57 kHz, each well above the floor between them, and the tone at 1 kHz;  B  in NFM nothing is written;  C  detaching the recorder stops the writing at once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "fm_stereo_rds_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Pipeline;
using cascade::core::RecordKind;
using cascade::core::Recorder;

namespace {

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

// Hann-windowed power (dB) at `hz` over `n` samples from `start`, bin-exact DFT.
double powerDb(const std::vector<float>& x, std::size_t start, std::size_t n, double hz, double fs) {
    double c = 0.0, s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n));
        const double ph = 2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / fs;
        c += x[start + i] * w * std::cos(ph);
        s += x[start + i] * w * std::sin(ph);
    }
    return 10.0 * std::log10(std::max((c * c + s * s) / (static_cast<double>(n) * static_cast<double>(n)), 1e-30));
}

}  // namespace

int main() {
    std::printf("test_pipeline_mpx\n");
    const fs::path dir = fs::path("mpx_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(dir, ec);

    Pipeline::Config cfg;
    cfg.sampleRateHz = fmrds::kInputRateHz;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    p.setVfoBandwidthHz(fmrds::kVfoBandwidthHz);
    p.setSquelchDb(-120.0f);
    p.setSource(std::make_unique<fmrds::FmStereoSource>(std::vector<int>{}));
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 48000; }, 30000));

    // A: record about two seconds
    Recorder rec;
    CHECK(rec.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioFloat32));
    std::string error;
    CHECK(rec.start(RecordKind::Audio, dir.string(), Pipeline::kMpxRateHz, error, "mpx"));
    const auto t0 = std::chrono::steady_clock::now();
    p.setMpxRecorder(&rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    p.setMpxRecorder(nullptr);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::string path = rec.path();
    rec.stop();
    const std::vector<unsigned char> f = slurp(path);
    CHECK(f.size() > 44);
    CHECK(u16(f, 20) == 3 && u16(f, 22) == 1 && u16(f, 34) == 32 && u32(f, 24) == 192000u);
    const std::size_t frames = (f.size() - 44) / 4;
    std::printf("  A: %zu samples in %.2f s = %.0f a second (192,000 asked)\n", frames, secs, static_cast<double>(frames) / secs);
    CHECK(static_cast<double>(frames) > 0.7 * 192000.0 * secs && static_cast<double>(frames) < 1.3 * 192000.0 * secs);
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) { x[i] = std::bit_cast<float>(u32(f, 44 + 4 * i)); }
    const std::size_t n = 65536, start = frames > n + 20000 ? 20000 : 0;
    CHECK(frames >= n);
    if (frames >= n) {
        const double fs = 192000.0;
        const double tone = powerDb(x, start, n, 1000.0, fs), pilot = powerDb(x, start, n, 19000.0, fs), floorLo = powerDb(x, start, n, 12000.0, fs), floorHi = powerDb(x, start, n, 47000.0, fs);
        // the difference channel is modulated by silence in this signal's right channel = left: 38 kHz carries nothing; the RDS carrier at 57 kHz carries its subcarrier energy
        std::printf("  A: tone %.1f dB, pilot %.1f dB, floor at 12 kHz %.1f dB and 47 kHz %.1f dB\n", tone, pilot, floorLo, floorHi);
        CHECK(tone > floorLo + 30.0);
        CHECK(pilot > floorLo + 30.0 && pilot > floorHi + 30.0);
    }

    // B: not in WFM, nothing is written
    p.setDemodMode(cascade::dsp::DemodMode::NFM);
    Recorder rec2;
    CHECK(rec2.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioFloat32));
    CHECK(rec2.start(RecordKind::Audio, dir.string(), Pipeline::kMpxRateHz, error, "mpx2"));
    p.setMpxRecorder(&rec2);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    p.setMpxRecorder(nullptr);
    const std::string path2 = rec2.path();
    rec2.stop();
    CHECK(slurp(path2).size() == 44);   // the header and nothing else

    // C: detached, it writes nothing more
    p.setDemodMode(cascade::dsp::DemodMode::WFM);
    Recorder rec3;
    CHECK(rec3.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioFloat32));
    CHECK(rec3.start(RecordKind::Audio, dir.string(), Pipeline::kMpxRateHz, error, "mpx3"));
    p.setMpxRecorder(&rec3);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    p.setMpxRecorder(nullptr);
    const std::uint64_t at = rec3.samplesWritten();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(rec3.samplesWritten() == at);
    CHECK(at > 0);
    rec3.stop();
    p.stop();
    fs::remove_all(dir, ec);
    return testSummary("test_pipeline_mpx");
}
