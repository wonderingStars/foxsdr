// Tests for source/iq_file_source.hpp / iq_file_source.cpp.
//
// Every WAV is synthesized in-test byte by byte (no fixtures), written to the
// current working directory — ctest runs each test from build-<slug>/tests,
// which is gitignored — with the process id in the filename so parallel
// suites cannot collide. Files are deleted on success and left behind on
// failure for inspection.
//
// Reference checking:
//   - int16 scaling is proven against value/32768.0f computed here, exactly
//     (a power-of-two divide is bit-reproducible in float).
//   - float32 passthrough is proven bit-exact via bit_cast comparison.
//   - The tone file is proven with a direct O(n^2) double-precision DFT, per
//     the project testing protocol, never against implementation constants.
//
// SPDX-License-Identifier: MIT
#include "source/iq_file_source.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#include "test_check.hpp"

using cascade::source::IqFileSource;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// ---------------------------------------------------------------------------
// Temp-file bookkeeping
// ---------------------------------------------------------------------------

std::vector<std::string> g_tempFiles;

std::string tmpPath(const char* tag) {
    return "iq_file_" + std::to_string(TEST_GETPID()) + "_" + tag + ".wav";
}

bool writeFile(const std::string& path, const std::vector<unsigned char>& bytes) {
    g_tempFiles.push_back(path);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------
// WAV byte builders — assembled by hand so malformed variants are trivial
// ---------------------------------------------------------------------------

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

// A chunk to splice between the standard ones (LIST/JUNK skipping test).
struct ExtraChunk {
    const char* id;
    std::vector<unsigned char> body;
};

void putChunk(std::vector<unsigned char>& v, const ExtraChunk& c) {
    putTag(v, c.id);
    putU32(v, static_cast<std::uint32_t>(c.body.size()));
    v.insert(v.end(), c.body.begin(), c.body.end());
    if (c.body.size() % 2 != 0) {
        v.push_back(0);  // RIFF word alignment: odd chunks carry a pad byte
    }
}

std::vector<unsigned char> wavBytes(std::uint16_t formatTag, std::uint16_t channels,
                                    std::uint32_t rateHz, std::uint16_t bits,
                                    const std::vector<unsigned char>& payload,
                                    long long dataSizeOverride = -1,
                                    bool withFmt = true, bool withData = true,
                                    const std::vector<ExtraChunk>& beforeFmt = {},
                                    const std::vector<ExtraChunk>& afterFmt = {}) {
    std::vector<unsigned char> v;
    putTag(v, "RIFF");
    putU32(v, 0);  // patched below; the parser must not trust it anyway
    putTag(v, "WAVE");
    for (const auto& c : beforeFmt) {
        putChunk(v, c);
    }
    if (withFmt) {
        putTag(v, "fmt ");
        putU32(v, 16);
        putU16(v, formatTag);
        putU16(v, channels);
        putU32(v, rateHz);
        putU32(v, rateHz * channels * (bits / 8u));  // byte rate
        putU16(v, static_cast<std::uint16_t>(channels * (bits / 8u)));
        putU16(v, bits);
    }
    for (const auto& c : afterFmt) {
        putChunk(v, c);
    }
    if (withData) {
        putTag(v, "data");
        const std::uint32_t declared =
            dataSizeOverride >= 0
                ? static_cast<std::uint32_t>(dataSizeOverride)
                : static_cast<std::uint32_t>(payload.size());
        putU32(v, declared);
        v.insert(v.end(), payload.begin(), payload.end());
    }
    const std::uint32_t riffSize = static_cast<std::uint32_t>(v.size() - 8);
    v[4] = static_cast<unsigned char>(riffSize & 0xFFu);
    v[5] = static_cast<unsigned char>((riffSize >> 8) & 0xFFu);
    v[6] = static_cast<unsigned char>((riffSize >> 16) & 0xFFu);
    v[7] = static_cast<unsigned char>((riffSize >> 24) & 0xFFu);
    return v;
}

std::vector<unsigned char> bytesFromI16(const std::vector<std::int16_t>& v) {
    std::vector<unsigned char> out;
    out.reserve(v.size() * 2);
    for (std::int16_t s : v) {
        const std::uint16_t u = static_cast<std::uint16_t>(s);
        out.push_back(static_cast<unsigned char>(u & 0xFFu));
        out.push_back(static_cast<unsigned char>((u >> 8) & 0xFFu));
    }
    return out;
}

std::vector<unsigned char> bytesFromF32(const std::vector<float>& v) {
    std::vector<unsigned char> out;
    out.reserve(v.size() * 4);
    for (float s : v) {
        const std::uint32_t u = std::bit_cast<std::uint32_t>(s);
        out.push_back(static_cast<unsigned char>(u & 0xFFu));
        out.push_back(static_cast<unsigned char>((u >> 8) & 0xFFu));
        out.push_back(static_cast<unsigned char>((u >> 16) & 0xFFu));
        out.push_back(static_cast<unsigned char>((u >> 24) & 0xFFu));
    }
    return out;
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

// Direct-DFT ground truth (O(n^2), double precision).
std::vector<std::complex<double>> dft(const std::vector<std::complex<float>>& x) {
    const std::size_t n = x.size();
    std::vector<std::complex<double>> spec(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t m = 0; m < n; ++m) {
            const double a = -kTwoPi * static_cast<double>(k) *
                             static_cast<double>(m) / static_cast<double>(n);
            acc += std::complex<double>(x[m].real(), x[m].imag()) *
                   std::complex<double>(std::cos(a), std::sin(a));
        }
        spec[k] = acc;
    }
    return spec;
}

std::size_t peakBin(const std::vector<std::complex<double>>& spec) {
    std::size_t best = 0;
    double bestMag = -1.0;
    for (std::size_t k = 0; k < spec.size(); ++k) {
        const double m = std::abs(spec[k]);
        if (m > bestMag) {
            bestMag = m;
            best = k;
        }
    }
    return best;
}

// Substring check with the actual message printed on mismatch, so a failing
// run shows what the source really said.
bool errContains(const IqFileSource& s, const char* sub) {
    const std::string e = s.lastError();
    const bool ok = e.find(sub) != std::string::npos;
    if (!ok) {
        std::printf("  lastError = \"%s\" (wanted substring \"%s\")\n",
                    e.c_str(), sub);
    }
    return ok;
}

// Shared expectation for every malformed-file case: open fails with the
// given reason, and the source stays completely inert afterwards.
void expectOpenFails(const std::vector<unsigned char>& bytes, const char* tag,
                     const char* wantSubstring) {
    const std::string path = tmpPath(tag);
    CHECK(writeFile(path, bytes));
    IqFileSource src;
    CHECK(!src.open(path));
    CHECK(errContains(src, wantSubstring));
    CHECK(!src.start());
    CHECK(!src.running());
    std::complex<float> buf[4];
    CHECK(src.read(buf, 4) == 0u);
}

// Fixed-seed LCG (Numerical Recipes constants) for deterministic int16
// sample data — no <random>, per the testing protocol.
std::uint32_t g_lcg = 0x2468ACE1u;
std::int16_t nextI16() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(g_lcg >> 16));
}

}  // namespace

int main() {
    // --- read()/start() before open: inert, no error spam ------------------
    {
        IqFileSource src;
        std::complex<float> buf[8];
        CHECK(src.read(buf, 8) == 0u);
        CHECK(src.lastError()[0] == '\0');  // polling is not an error...
        CHECK(src.read(buf, 8) == 0u);
        CHECK(src.lastError()[0] == '\0');  // ...and never becomes one
        CHECK(src.read(nullptr, 0) == 0u);
        CHECK(!src.running());
        CHECK(!src.selfPaced());
        CHECK(!src.start());  // start without open must fail loudly
        CHECK(errContains(src, "open"));
        CHECK(std::string(src.name()) == "IQ file");
    }

    // --- open() on a nonexistent path fails cleanly -------------------------
    {
        IqFileSource src;
        // Only writeFile() registers cleanup entries, and this path is never
        // written — the whole point is that it does not exist.
        const std::string missing = tmpPath("does_not_exist_never_created");
        CHECK(!src.open(missing));
        CHECK(errContains(src, "cannot open"));
        CHECK(!src.start());
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0u);
    }

    // --- int16 roundtrip: exact to the documented 1/32768 scale ------------
    {
        const std::vector<std::int16_t> interleaved = {
            // I, Q pairs; extremes prove the [-1, 1) mapping end to end
            -32768, 32767, 0, 1, -1, 12345, -12345, 256,
            -257,   16384, -16384, 2, 32766, -32767, 100, -100,
        };
        const std::string path = tmpPath("i16_roundtrip");
        CHECK(writeFile(path,
                        wavBytes(1, 2, 48000, 16, bytesFromI16(interleaved))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.lastError()[0] == '\0');
        CHECK_NEAR(src.sampleRateHz(), 48000.0, 0.0);
        CHECK(!src.selfPaced());
        CHECK(!src.setSampleRateHz(96000.0));            // rate is fixed...
        CHECK_NEAR(src.sampleRateHz(), 48000.0, 0.0);    // ...and unchanged
        CHECK(src.setCenterFrequencyHz(101.5e6));        // nominal, accepted
        CHECK_NEAR(src.centerFrequencyHz(), 101.5e6, 0.0);
        CHECK(std::string(src.name()) == "IQ file: " + path);
        CHECK(!src.running());
        std::complex<float> probe[2];
        CHECK(src.read(probe, 2) == 0u);  // opened but not started: no data
        CHECK(src.start());
        CHECK(src.running());
        CHECK(src.start());  // idempotent while running
        const std::size_t frames = interleaved.size() / 2;
        std::vector<std::complex<float>> x(frames);
        CHECK(src.read(x.data(), frames) == frames);
        for (std::size_t i = 0; i < frames; ++i) {
            // Exact equality on purpose: int16 / 2^15 is bit-reproducible.
            const float wantRe =
                static_cast<float>(interleaved[2 * i]) / 32768.0f;
            const float wantIm =
                static_cast<float>(interleaved[2 * i + 1]) / 32768.0f;
            CHECK(x[i].real() == wantRe);
            CHECK(x[i].imag() == wantIm);
        }
        src.stop();
        CHECK(!src.running());
        src.stop();  // idempotent
        CHECK(src.read(x.data(), 1) == 0u);  // stopped: no data, no error
        CHECK(src.lastError()[0] == '\0');
    }

    // --- float32 roundtrip: bit-exact passthrough ---------------------------
    {
        const std::vector<float> interleaved = {
            0.1f,   -0.9999f, 1.5f,     -2.25f,  // out-of-range preserved
            -0.0f,  1.0f,     3.0e-39f, 0.5f,    // -0 and a subnormal survive
            0.125f, -1.0f,    0.7071067f, 1e-20f,
        };
        const std::string path = tmpPath("f32_roundtrip");
        CHECK(writeFile(path,
                        wavBytes(3, 2, 250000, 32, bytesFromF32(interleaved))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK_NEAR(src.sampleRateHz(), 250000.0, 0.0);
        CHECK(src.start());
        const std::size_t frames = interleaved.size() / 2;
        std::vector<std::complex<float>> x(frames);
        CHECK(src.read(x.data(), frames) == frames);
        for (std::size_t i = 0; i < frames; ++i) {
            // bit_cast comparison: catches sign-of-zero and subnormal loss
            // that a float == would wave through.
            CHECK(std::bit_cast<std::uint32_t>(x[i].real()) ==
                  std::bit_cast<std::uint32_t>(interleaved[2 * i]));
            CHECK(std::bit_cast<std::uint32_t>(x[i].imag()) ==
                  std::bit_cast<std::uint32_t>(interleaved[2 * i + 1]));
        }
    }

    // --- loop seam: 1000 frames read as 2500, seam at 1000 and 2000 --------
    std::vector<std::int16_t> seamData(2000);  // 1000 frames, reused below
    for (std::int16_t& s : seamData) {
        s = nextI16();
    }
    {
        const std::string path = tmpPath("loop_seam");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(seamData))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        // Sentinel prefill: if the source under-delivers, the comparison
        // fails deterministically instead of reading stale stack noise.
        std::vector<std::complex<float>> x(2500,
                                           std::complex<float>(-999.0f, -999.0f));
        const std::size_t got = src.read(x.data(), 2500);
        CHECK(got == 2500u);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < 2500; ++i) {
            const std::size_t f = i % 1000;  // seam wraps at 1000 and 2000
            const float wantRe =
                static_cast<float>(seamData[2 * f]) / 32768.0f;
            const float wantIm =
                static_cast<float>(seamData[2 * f + 1]) / 32768.0f;
            if (x[i].real() != wantRe || x[i].imag() != wantIm) {
                ++mismatches;
                if (mismatches <= 3) {
                    std::printf("  seam mismatch at sample %zu (frame %zu)\n",
                                i, f);
                }
            }
        }
        CHECK(mismatches == 0u);

        // Same file, chunked reads with odd sizes so the wrap lands mid-call
        // at unaligned positions — proves the seam carries across read()
        // boundaries, not just within one call.
        IqFileSource src2;
        CHECK(src2.open(path));
        CHECK(src2.start());
        std::vector<std::complex<float>> y(2500,
                                           std::complex<float>(-999.0f, -999.0f));
        std::size_t off = 0;
        for (std::size_t chunk : {700u, 613u, 487u, 700u}) {  // totals 2500
            CHECK(src2.read(y.data() + off, chunk) == chunk);
            off += chunk;
        }
        CHECK(off == 2500u);
        std::size_t mm2 = 0;
        for (std::size_t i = 0; i < 2500; ++i) {
            const std::size_t f = i % 1000;
            if (y[i].real() !=
                    static_cast<float>(seamData[2 * f]) / 32768.0f ||
                y[i].imag() !=
                    static_cast<float>(seamData[2 * f + 1]) / 32768.0f) {
                ++mm2;
            }
        }
        CHECK(mm2 == 0u);
    }

    // --- trailing partial frame: ignored, seam stays frame-aligned ---------
    {
        // Same 1000 frames plus 2 stray bytes (half a frame) in the data
        // chunk. The partial frame must never be delivered and must not
        // shift the loop seam.
        std::vector<unsigned char> payload = bytesFromI16(seamData);
        payload.push_back(0xAA);
        payload.push_back(0x55);
        const std::string path = tmpPath("partial_frame");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, payload)));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        std::vector<std::complex<float>> x(2100,
                                           std::complex<float>(-999.0f, -999.0f));
        CHECK(src.read(x.data(), 2100) == 2100u);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < 2100; ++i) {
            const std::size_t f = i % 1000;
            if (x[i].real() !=
                    static_cast<float>(seamData[2 * f]) / 32768.0f ||
                x[i].imag() !=
                    static_cast<float>(seamData[2 * f + 1]) / 32768.0f) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0u);
    }

    // --- stop()/start() rewinds; start() while running does not ------------
    {
        const std::string path = tmpPath("restart");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(seamData))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        std::vector<std::complex<float>> a(10);
        CHECK(src.read(a.data(), 10) == 10u);
        CHECK(src.start());  // idempotent: must NOT rewind a running source
        std::vector<std::complex<float>> b(10);
        CHECK(src.read(b.data(), 10) == 10u);
        // b continues at frame 10, not a replay of frame 0
        CHECK(b[0].real() == static_cast<float>(seamData[20]) / 32768.0f);
        src.stop();
        CHECK(src.start());  // restart: deterministic replay from frame 0
        std::vector<std::complex<float>> c(10);
        CHECK(src.read(c.data(), 10) == 10u);
        for (std::size_t i = 0; i < 10; ++i) {
            CHECK(c[i].real() == a[i].real());
            CHECK(c[i].imag() == a[i].imag());
        }
    }

    // --- extra RIFF chunks (LIST before fmt, odd-sized JUNK after) skipped -
    {
        const std::vector<std::int16_t> interleaved = {100, -200, 300, -400};
        ExtraChunk list{"LIST", {'I', 'N', 'F', 'O', 'x'}};  // odd size: 5
        ExtraChunk junk{"JUNK", {1, 2, 3}};                  // odd size: 3
        const std::string path = tmpPath("extra_chunks");
        CHECK(writeFile(path, wavBytes(1, 2, 8000, 16, bytesFromI16(interleaved),
                                       -1, true, true, {list}, {junk})));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK_NEAR(src.sampleRateHz(), 8000.0, 0.0);
        CHECK(src.start());
        std::complex<float> x[2];
        CHECK(src.read(x, 2) == 2u);
        CHECK(x[0].real() == 100.0f / 32768.0f);
        CHECK(x[0].imag() == -200.0f / 32768.0f);
        CHECK(x[1].real() == 300.0f / 32768.0f);
        CHECK(x[1].imag() == -400.0f / 32768.0f);
    }

    // --- malformed files: each rejected with a specific reason -------------
    {
        const std::vector<unsigned char> stereo16 =
            bytesFromI16({1000, -1000, 2000, -2000});
        // mono
        expectOpenFails(wavBytes(1, 1, 48000, 16, bytesFromI16({1, 2, 3, 4})),
                        "mono", "2 channels");
        // 8-bit PCM
        expectOpenFails(wavBytes(1, 2, 48000, 8, {10, 20, 30, 40}),
                        "pcm8", "bit depth 8");
        // 24-bit PCM
        expectOpenFails(wavBytes(1, 2, 48000, 24,
                                 {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}),
                        "pcm24", "bit depth 24");
        // 64-bit float
        expectOpenFails(wavBytes(3, 2, 48000, 64,
                                 std::vector<unsigned char>(32, 0)),
                        "f64", "bit depth 64");
        // ADPCM (format tag 2)
        expectOpenFails(wavBytes(2, 2, 48000, 4, {1, 2, 3, 4}),
                        "adpcm", "format tag 2");
        // WAVE_FORMAT_EXTENSIBLE (0xFFFE) — refused, not guessed at
        expectOpenFails(wavBytes(0xFFFE, 2, 48000, 16, stereo16),
                        "extensible", "format tag 65534");
        // truncated data chunk: declares 4000 more bytes than exist
        expectOpenFails(wavBytes(1, 2, 48000, 16, stereo16,
                                 static_cast<long long>(stereo16.size()) + 4000),
                        "truncated", "truncated");
        // missing fmt chunk entirely
        expectOpenFails(wavBytes(1, 2, 48000, 16, stereo16, -1, false, true),
                        "no_fmt", "missing fmt");
        // missing data chunk entirely
        expectOpenFails(wavBytes(1, 2, 48000, 16, {}, -1, true, false),
                        "no_data", "missing data");
        // zero-length data chunk: no complete frames to loop
        expectOpenFails(wavBytes(1, 2, 48000, 16, {}),
                        "empty_data", "no complete");
        // zero sample rate
        expectOpenFails(wavBytes(1, 2, 0, 16, stereo16),
                        "zero_rate", "sample rate");
        // not a RIFF file at all
        expectOpenFails({'t', 'h', 'i', 's', ' ', 'i', 's', ' ', 'n', 'o',
                         't', ' ', 'a', ' ', 'w', 'a', 'v', '!', '!', '!'},
                        "not_riff", "RIFF");
    }

    // --- failed open() resets a previously good source ----------------------
    {
        const std::string good = tmpPath("good_then_bad");
        CHECK(writeFile(good, wavBytes(1, 2, 48000, 16,
                                       bytesFromI16({1, 2, 3, 4}))));
        IqFileSource src;
        CHECK(src.open(good));
        const std::string bad = tmpPath("bad_after_good");
        CHECK(writeFile(bad, wavBytes(1, 1, 48000, 16, bytesFromI16({1, 2}))));
        CHECK(!src.open(bad));  // mono: rejected...
        CHECK(!src.start());    // ...and the old file must not linger
        std::complex<float> buf[2];
        CHECK(src.read(buf, 2) == 0u);
    }

    // --- tone WAV: DFT peak at the synthesized bin, correct level ----------
    {
        // 0.5 * exp(j*2*pi*37*m/1024): an exact-bin complex exponential, so
        // the DFT concentrates all energy in bin 37 at |X| = 0.5 * N.
        const std::size_t n = 1024;
        const double amp = 0.5;
        std::vector<float> interleaved(2 * n);
        for (std::size_t m = 0; m < n; ++m) {
            const double ph =
                kTwoPi * 37.0 * static_cast<double>(m) / static_cast<double>(n);
            interleaved[2 * m] = static_cast<float>(amp * std::cos(ph));
            interleaved[2 * m + 1] = static_cast<float>(amp * std::sin(ph));
        }
        const std::string path = tmpPath("tone");
        CHECK(writeFile(path,
                        wavBytes(3, 2, 1024000, 32, bytesFromF32(interleaved))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        std::vector<std::complex<float>> x(n);
        CHECK(src.read(x.data(), n) == n);
        const auto spec = dft(x);
        CHECK(peakBin(spec) == 37u);
        const double levelDb =
            20.0 * std::log10(std::abs(spec[37]) / static_cast<double>(n));
        CHECK_NEAR(levelDb, 20.0 * std::log10(amp), 0.05);  // -6.02 dB
        // Exact-bin tone: everything else is float-rounding leakage only.
        for (std::size_t k = 0; k < n; ++k) {
            if (k != 37) {
                CHECK(std::abs(spec[k]) < amp * static_cast<double>(n) * 1e-3);
            }
        }
    }

    // --- basename extraction handles path separators ------------------------
    {
        const std::string fname = tmpPath("basename");
        CHECK(writeFile(fname, wavBytes(1, 2, 48000, 16,
                                        bytesFromI16({5, 6, 7, 8}))));
        IqFileSource src;
        CHECK(src.open("./" + fname));  // separator must be stripped
        CHECK(std::string(src.name()) == "IQ file: " + fname);
    }

    const int rc = testSummary("test_iq_file_source");
    if (rc == 0) {
        // Success: remove every temp WAV. On failure they stay for autopsy.
        for (const std::string& p : g_tempFiles) {
            std::remove(p.c_str());
        }
    }
    return rc;
}
