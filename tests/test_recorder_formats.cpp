// test_recorder_formats.cpp - THE SAMPLE FORMAT OF A RECORDING (engine serial lane A, row P7): core/recorder.hpp setSampleFormat.
//
//   A  the default is what every take has always been: baseband I/Q as 32-bit float (bit-exact), audio as 16-bit integer, and a header byte for byte as before;
//   B  baseband I/Q as 16-bit integer: the header says 2 channels, 16 bits, PCM; the data is half the size and replays through IqFileSource to within one 16-bit step;
//   C  audio as 24-bit integer and as 32-bit float: the headers say so, the bytes decode to the samples (24-bit within one 24-bit step, float exactly), a NaN is written as silence;
//   D  a value its kind does not have is refused and changes nothing; a take keeps the format it started with when the next one is chosen; the next take uses the new one;
//   E  the 4 GiB ceiling is counted in the take's own frame size (a tiny ceiling ends a 16-bit I/Q take after exactly that many frames).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/recorder.hpp"
#include "source/iq_file_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Recorder;
using cascade::core::RecordKind;

namespace {

std::vector<unsigned char> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint16_t u16(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint16_t>(v[at] | (v[at + 1] << 8)); }
std::uint32_t u32(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint32_t>(v[at]) | (static_cast<std::uint32_t>(v[at + 1]) << 8) | (static_cast<std::uint32_t>(v[at + 2]) << 16) | (static_cast<std::uint32_t>(v[at + 3]) << 24); }

std::string take(Recorder& r, RecordKind kind, const fs::path& dir, double rate, const std::vector<std::complex<float>>& iq, const std::vector<float>& audio) {
    std::string error;
    CHECK(r.start(kind, dir.string(), rate, error));
    if (kind == RecordKind::BasebandIq) { r.writeIq(iq.data(), iq.size()); } else { r.writeAudio(audio.data(), audio.size()); }
    const std::string path = r.path();
    r.stop();
    return path;
}

}  // namespace

int main() {
    std::printf("test_recorder_formats\n");
    const fs::path dir = fs::path("rec_formats_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(dir, ec);

    // a stand-in signal: a ramp of values inside +-1, with a few beyond and a NaN
    std::vector<std::complex<float>> iq;
    std::vector<float> audio;
    for (int i = 0; i < 6000; ++i) {
        const float a = 0.9f * std::sin(0.01f * static_cast<float>(i)), b = 0.8f * std::cos(0.013f * static_cast<float>(i));
        iq.emplace_back(a, b);
        audio.push_back(a);
    }
    iq.emplace_back(1.7f, -2.5f);      // beyond full scale: a float take keeps it, a 16-bit take clips it
    audio.push_back(std::nanf(""));    // a demod glitch
    audio.push_back(1.5f);

    Recorder r;
    CHECK(r.sampleFormat(RecordKind::BasebandIq) == 0 && r.sampleFormat(RecordKind::Audio) == 0);

    // A: defaults
    {
        const std::string p = take(r, RecordKind::BasebandIq, dir, 48000.0, iq, audio);
        const std::vector<unsigned char> f = slurp(p);
        CHECK(f.size() == 44 + iq.size() * 8);
        CHECK(u16(f, 20) == 3 && u16(f, 22) == 2 && u16(f, 34) == 32 && u32(f, 28) == 48000u * 8u && u16(f, 32) == 8);
        std::uint32_t bits = u32(f, 44 + 8 * 6000);
        CHECK(std::bit_cast<float>(bits) == 1.7f);   // bit-exact, beyond full scale kept
        const std::string pa = take(r, RecordKind::Audio, dir, 48000.0, iq, audio);
        const std::vector<unsigned char> g = slurp(pa);
        CHECK(g.size() == 44 + audio.size() * 2);
        CHECK(u16(g, 20) == 1 && u16(g, 22) == 1 && u16(g, 34) == 16 && u16(g, 32) == 2);
    }

    // B: I/Q as 16-bit integer
    CHECK(r.setSampleFormat(RecordKind::BasebandIq, cascade::core::kRecordIqInt16));
    {
        const std::string p = take(r, RecordKind::BasebandIq, dir, 48000.0, iq, audio);
        const std::vector<unsigned char> f = slurp(p);
        std::printf("  B int16 I/Q: %zu bytes against %zu as float\n", f.size(), 44 + iq.size() * 8);
        CHECK(f.size() == 44 + iq.size() * 4);
        CHECK(u16(f, 20) == 1 && u16(f, 22) == 2 && u16(f, 34) == 16 && u16(f, 32) == 4 && u32(f, 28) == 48000u * 4u);
        CHECK(u32(f, 40) == iq.size() * 4);   // the data size was patched
        cascade::source::IqFileSource src;
        CHECK(src.open(p));
        CHECK(src.start());
        std::vector<std::complex<float>> back(iq.size());
        CHECK(src.read(back.data(), back.size()) == back.size());
        double worst = 0.0;
        for (std::size_t i = 0; i < 6000; ++i) { worst = std::fmax(worst, std::fmax(std::fabs(back[i].real() - iq[i].real()), std::fabs(back[i].imag() - iq[i].imag()))); }
        std::printf("  B replay: worst error %.2e (one step is %.2e)\n", worst, 1.0 / 32768.0);
        CHECK(worst <= 1.0 / 32768.0);
        CHECK(back[6000].real() == 32767.0f / 32768.0f && back[6000].imag() == -1.0f);   // beyond full scale clips
        src.stop();
    }

    // C: audio as 24-bit integer and as 32-bit float
    CHECK(r.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioInt24));
    {
        const std::string p = take(r, RecordKind::Audio, dir, 48000.0, iq, audio);
        const std::vector<unsigned char> f = slurp(p);
        CHECK(f.size() == 44 + audio.size() * 3);
        CHECK(u16(f, 20) == 1 && u16(f, 22) == 1 && u16(f, 34) == 24 && u16(f, 32) == 3 && u32(f, 28) == 48000u * 3u);
        double worst = 0.0;
        for (std::size_t i = 0; i < 6000; ++i) {
            std::int32_t v = f[44 + 3 * i] | (f[44 + 3 * i + 1] << 8) | (f[44 + 3 * i + 2] << 16);
            if ((v & 0x800000) != 0) { v -= 0x1000000; }
            worst = std::fmax(worst, std::fabs(static_cast<double>(v) / 8388608.0 - static_cast<double>(audio[i])));
        }
        std::printf("  C int24 audio: worst error %.2e (one step is %.2e)\n", worst, 1.0 / 8388608.0);
        CHECK(worst <= 0.5 / 8388608.0 + 1e-9);
        auto at = [&](std::size_t i) {
            std::int32_t v = f[44 + 3 * i] | (f[44 + 3 * i + 1] << 8) | (f[44 + 3 * i + 2] << 16);
            return (v & 0x800000) != 0 ? v - 0x1000000 : v;
        };
        CHECK(at(6000) == 0);          // the NaN is silence
        CHECK(at(6001) == 8388607);    // +1.5 saturates
    }
    CHECK(r.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioFloat32));
    {
        const std::string p = take(r, RecordKind::Audio, dir, 48000.0, iq, audio);
        const std::vector<unsigned char> f = slurp(p);
        CHECK(f.size() == 44 + audio.size() * 4);
        CHECK(u16(f, 20) == 3 && u16(f, 22) == 1 && u16(f, 34) == 32 && u16(f, 32) == 4);
        bool exact = true;
        for (std::size_t i = 0; i < 6000; ++i) { exact = exact && std::bit_cast<float>(u32(f, 44 + 4 * i)) == audio[i]; }
        CHECK(exact);
        CHECK(std::bit_cast<float>(u32(f, 44 + 4 * 6000)) == 0.0f);   // the NaN is silence
        CHECK(std::bit_cast<float>(u32(f, 44 + 4 * 6001)) == 1.5f);   // float keeps what int16 would clip
    }

    // D: refusals, and a take keeps its format
    CHECK(!r.setSampleFormat(RecordKind::BasebandIq, 2));
    CHECK(!r.setSampleFormat(RecordKind::Audio, 3));
    CHECK(!r.setSampleFormat(RecordKind::Audio, -1));
    CHECK(r.sampleFormat(RecordKind::BasebandIq) == 1 && r.sampleFormat(RecordKind::Audio) == 2);
    {
        std::string error;
        CHECK(r.setSampleFormat(RecordKind::BasebandIq, 0));
        CHECK(r.start(RecordKind::BasebandIq, dir.string(), 48000.0, error));
        CHECK(r.setSampleFormat(RecordKind::BasebandIq, cascade::core::kRecordIqInt16));   // chosen while the take is running
        r.writeIq(iq.data(), 100);
        const std::string path = r.path();
        r.stop();
        const std::vector<unsigned char> f = slurp(path);
        CHECK(u16(f, 34) == 32 && f.size() == 44 + 100 * 8);   // the running take stayed float
        const std::string next = take(r, RecordKind::BasebandIq, dir, 48000.0, iq, audio);
        CHECK(u16(slurp(next), 34) == 16);                      // the next one is 16-bit
    }

    // E: the ceiling is in the take's own frames: 4,000 bytes of 16-bit I/Q is 1,000 frames
    {
        Recorder t;
        CHECK(t.setSampleFormat(RecordKind::BasebandIq, cascade::core::kRecordIqInt16));
        t.setMaxDataBytesForTest(4000);
        std::string error;
        CHECK(t.start(RecordKind::BasebandIq, dir.string(), 48000.0, error));
        t.writeIq(iq.data(), 3000);
        CHECK(t.sizeLimitReached());
        CHECK(!t.recording());
        const std::vector<unsigned char> f = slurp(t.path());
        std::printf("  E ceiling: %zu bytes on disk, data chunk says %u\n", f.size(), u32(f, 40));
        CHECK(f.size() == 44 + 4000 && u32(f, 40) == 4000);
        Recorder a;
        CHECK(a.setSampleFormat(RecordKind::Audio, cascade::core::kRecordAudioInt24));
        a.setMaxDataBytesForTest(3000);
        CHECK(a.start(RecordKind::Audio, dir.string(), 48000.0, error));
        a.writeAudio(audio.data(), 5000);
        CHECK(a.sizeLimitReached());
        const std::vector<unsigned char> g = slurp(a.path());
        CHECK(g.size() == 44 + 3000 && u32(g, 40) == 3000);
    }
    fs::remove_all(dir, ec);
    return testSummary("test_recorder_formats");
}
