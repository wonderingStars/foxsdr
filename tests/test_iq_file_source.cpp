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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_file_source.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
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

// ---------------------------------------------------------------------------
// Stop-responsiveness harness
// ---------------------------------------------------------------------------
//
// read() serves a large request as a sequence of bounded disk reads. On a
// healthy local file every one of them returns in microseconds, so the whole
// call looks atomic; on a dead SMB share or a failing USB disk a single read
// parks in the filesystem for as long as its own timeout, and the pipeline's
// stop()/setSource() handshake — flag down, source stop(), JOIN the source
// thread — waits for however many of those reads the loop still intends to
// issue. That is the hang under test: not one slow read (nothing in user
// space can shorten that), but the loop continuing to issue MORE of them
// after stop() has already been asked for.
//
// A real disk cannot be made to block on demand, so both fakes below drive
// the class through its readRaw seam: one counts the reads, the other blocks
// inside one until the test releases it. Neither asserts a wall-clock latency
// — the observable is the COUNT of reads issued after the stop, which is
// exact and cannot flake under load.

// Calls stop() from inside the first disk read, i.e. the control thread's
// stop() lands while this very chunk is in flight. No threads: the ordering
// that matters is reproduced exactly, deterministically.
class StopDuringReadSource : public IqFileSource {
public:
    int rawCalls() const { return rawCalls_; }

protected:
    std::size_t readRaw(unsigned char* dst, std::size_t bytes) override {
        ++rawCalls_;
        if (rawCalls_ == 1) {
            stop();
        }
        return IqFileSource::readRaw(dst, bytes);
    }

private:
    int rawCalls_ = 0;
};

// Blocks inside the FIRST disk read until the test releases it, then behaves
// normally. Only the first read blocks on purpose: an unfixed build must fail
// this test by finishing the request, never by hanging the suite forever.
class BlockingReadSource : public IqFileSource {
public:
    // Blocks (generously bounded) until the reader thread is parked inside a
    // disk read. False means it never got there — reported, never ignored.
    bool waitUntilBlocked() {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::seconds(30),
                            [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }

    int rawCalls() const { return rawCalls_.load(); }
    // Was stop() already complete at the moment the blocked read woke up?
    bool stopSeenOnWake() const { return stopSeenOnWake_.load(); }
    void noteStopDone() { stopDone_.store(true); }

protected:
    std::size_t readRaw(unsigned char* dst, std::size_t bytes) override {
        if (rawCalls_.fetch_add(1) == 0) {
            std::unique_lock<std::mutex> lk(m_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lk, [this] { return released_; });
            stopSeenOnWake_.store(stopDone_.load());
        }
        return IqFileSource::readRaw(dst, bytes);
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
    std::atomic<int> rawCalls_{0};
    std::atomic<bool> stopDone_{false};
    std::atomic<bool> stopSeenOnWake_{false};
};

// Counts the disk reads ONE open() costs. open()'s RIFF chunk walk goes
// through readHeaderRaw, the streaming seam's sibling, so the work it puts on
// the GUI thread is countable without a slow disk and without any timing.
//
// Deliberately NOT an override of readRaw: that seam belongs to the playback
// path, and the two fakes above reserve their FIRST call to it for a read they
// stall on purpose. Counting from a separate virtual is what lets both live in
// one file without either changing what the other measures.
class CountingOpenSource : public IqFileSource {
public:
    int headerReads() const { return headerReads_; }

protected:
    std::size_t readHeaderRaw(unsigned char* dst, std::size_t bytes) override {
        ++headerReads_;
        return IqFileSource::readHeaderRaw(dst, bytes);
    }

private:
    int headerReads_ = 0;
};

// RIFF/WAVE whose body is nothing but `count` chunks that declare size 0.
// A zero-size chunk advances the walk by exactly its own 8-byte header, so an
// unbounded walk issues one seek+read per 8 bytes of file — which is the
// failure being bounded, and it is what a truncated recording's run of zeros
// looks like, not only what a hostile author would write.
std::vector<unsigned char> zeroChunkWav(std::size_t count) {
    std::vector<unsigned char> v;
    putTag(v, "RIFF");
    putU32(v, 0);
    putTag(v, "WAVE");
    for (std::size_t i = 0; i < count; ++i) {
        putTag(v, "JUNK");
        putU32(v, 0);
    }
    const std::uint32_t riffSize = static_cast<std::uint32_t>(v.size() - 8);
    v[4] = static_cast<unsigned char>(riffSize & 0xFFu);
    v[5] = static_cast<unsigned char>((riffSize >> 8) & 0xFFu);
    v[6] = static_cast<unsigned char>((riffSize >> 16) & 0xFFu);
    v[7] = static_cast<unsigned char>((riffSize >> 24) & 0xFFu);
    return v;
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

    // --- float32: non-finite samples are sanitised at the entry point ------
    // A float32 WAV can encode NaN and both infinities, and nothing downstream
    // of the source defends the sample path against them: one such sample
    // latches the AGC gain, the squelch EMA and the noise reducer's spectrum
    // for the rest of the session. The decoder is the entry point, so it is
    // where they are replaced with silence. Finite samples in the same buffer
    // must still come through bit-exact.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<float> interleaved = {
            0.25f,               -0.5f,                // clean frame
            nan,                 0.75f,                // NaN in I only
            0.125f,              -nan,                 // NaN in Q only
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),   // both infinities
            -0.0f,               0.875f,               // -0 is finite: preserved
        };
        const std::string path = tmpPath("f32_nonfinite");
        CHECK(writeFile(path,
                        wavBytes(3, 2, 250000, 32, bytesFromF32(interleaved))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        const std::size_t frames = interleaved.size() / 2;
        std::vector<std::complex<float>> x(frames);
        CHECK(src.read(x.data(), frames) == frames);
        std::size_t nonFinite = 0;
        std::size_t wrongZero = 0;
        std::size_t finiteMismatch = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const float parts[2] = {x[i].real(), x[i].imag()};
            for (std::size_t c = 0; c < 2; ++c) {
                const float want = interleaved[2 * i + c];
                if (!std::isfinite(parts[c])) {
                    ++nonFinite;
                } else if (!std::isfinite(want)) {
                    // Sanitised: exactly +0, so the sample is true silence.
                    if (std::bit_cast<std::uint32_t>(parts[c]) != 0u) {
                        ++wrongZero;
                    }
                } else if (std::bit_cast<std::uint32_t>(parts[c]) !=
                           std::bit_cast<std::uint32_t>(want)) {
                    ++finiteMismatch;
                }
            }
        }
        CHECK(nonFinite == 0);
        CHECK(wrongZero == 0);
        CHECK(finiteMismatch == 0);
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

    // --- stop() mid-read stops issuing disk reads --------------------------
    // The pipeline's stop()/setSource() handshake clears its run flag, calls
    // the source's stop(), then JOINS the source thread — so every disk read
    // read() still chooses to issue after that stop() is time the GUI spends
    // frozen. On a healthy file that is invisible; on a dead SMB share each
    // one costs the filesystem's own timeout. read() must therefore hand back
    // what it already has rather than start another read.
    {
        const std::string path = tmpPath("stop_midread");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(seamData))));
        StopDuringReadSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        // 10000 samples out of a 1000-frame file needs at least ten disk
        // reads whatever the internal chunk cap is, so "exactly one" below is
        // a property of the stop check and not of the chunk size.
        constexpr std::size_t kWant = 10000;
        std::vector<std::complex<float>> x(kWant,
                                           std::complex<float>(-999.0f, -999.0f));
        const std::size_t got = src.read(x.data(), kWant);
        CHECK(src.rawCalls() == 1);
        CHECK(got > 0u);
        CHECK(got < kWant);
        CHECK(!src.running());
        // What it did deliver is real data in order: an abort truncates the
        // request, it never corrupts or zero-pads the part already decoded.
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < got; ++i) {
            const std::size_t f = i % 1000;
            if (x[i].real() != static_cast<float>(seamData[2 * f]) / 32768.0f ||
                x[i].imag() != static_cast<float>(seamData[2 * f + 1]) / 32768.0f) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0u);
        CHECK(src.lastError()[0] == '\0');  // a stop is not an I/O error
    }

    // --- stop() arriving from another thread while a read is parked --------
    // Same property, driven the way it actually happens: the control thread
    // calls stop() while the source thread sits inside a disk read that has
    // not returned yet. The read that is already in flight cannot be taken
    // back (that is the filesystem's business); the point is that no further
    // one is issued once it does return.
    {
        const std::string path = tmpPath("stop_blocked_read");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(seamData))));
        BlockingReadSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        constexpr std::size_t kWant = 10000;
        std::vector<std::complex<float>> x(kWant,
                                           std::complex<float>(-999.0f, -999.0f));
        std::atomic<std::size_t> got{0};
        std::thread reader([&] { got.store(src.read(x.data(), kWant)); });
        // Generous (30 s) and only a deadlock escape, never a latency
        // assertion — a loaded machine cannot turn this into a false red.
        const bool blocked = src.waitUntilBlocked();
        CHECK(blocked);
        src.stop();          // the control thread's abort, issued mid-read
        src.noteStopDone();
        src.release();       // ...and only now does the "disk" answer
        reader.join();
        CHECK(src.stopSeenOnWake());  // the stop really did land mid-read
        CHECK(src.rawCalls() == 1);   // and nothing further was issued
        CHECK(got.load() < kWant);
        CHECK(!src.running());
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

    // --- open()'s chunk walk is bounded, and stops once it has what it needs -
    {
        // THE FAILURE THIS GUARDS. open() does its whole RIFF header walk on
        // the CALLING thread, which for both call sites is the GUI thread, and
        // the walk's trip count came entirely from the file: a chunk declaring
        // size 0 moves the cursor 8 bytes, so the loop issued one blocking
        // seek+read per 8 bytes of file. 40000 such chunks is a 320 KB file
        // and 40000 reads; a gigabyte capture whose header area was zeroed by
        // a truncated write is 134 million of them, with the window frozen for
        // all of it. The bound is what makes Open's cost a property of the
        // code rather than of the file it was pointed at.
        //
        // The file is finite so an unfixed build FAILS this test rather than
        // hanging the suite — a hang proves nothing about a bound.
        constexpr std::size_t chunks = 40000;
        // Compile-time, not a CHECK: both sides are constants, so a runtime
        // conditional on them is only a warning waiting to happen.
        static_assert(chunks > IqFileSource::kMaxHeaderChunks * 4,
                      "the storm must dwarf the bound or it proves nothing");
        const std::string path = tmpPath("zero_size_chunk_storm");
        CHECK(writeFile(path, zeroChunkWav(chunks)));

        CountingOpenSource src;
        CHECK(!src.open(path));  // no fmt, no data: it cannot succeed
        CHECK(errContains(src, "gave up walking"));
        // One read for the 12-byte RIFF header, then at most the cap. The
        // slack covers nothing in particular; it exists so the assertion is
        // about the ORDER of the bound, not an exact arithmetic identity that
        // a later refactor would have to chase.
        CHECK(src.headerReads() <= static_cast<int>(IqFileSource::kMaxHeaderChunks) + 4);
        if (src.headerReads() > static_cast<int>(IqFileSource::kMaxHeaderChunks) + 4) {
            std::printf("FAIL open() issued %d disk reads walking %zu chunks (bound %zu)\n",
                        src.headerReads(), chunks, IqFileSource::kMaxHeaderChunks);
        }

        // The other half of the contract: a real file is unaffected. It still
        // opens, with the same rate and samples, and costs a handful of reads
        // — the walk stops as soon as it holds both fmt and data rather than
        // reading on to the end.
        //
        // The trailing chunks are what make that measurable. A file whose data
        // chunk is last cannot tell the two behaviours apart; broadcast WAVs
        // routinely carry metadata after the audio, and walking it is pure
        // blocking work on the GUI thread for an answer already known.
        ExtraChunk pad{"JUNK", {1, 2, 3, 4}};
        std::vector<unsigned char> goodBytes =
            wavBytes(1, 2, 8000, 16, bytesFromI16({7, -7, 9, -9}), -1, true, true,
                     {pad}, {pad});
        for (int i = 0; i < 5000; ++i) {
            putTag(goodBytes, "JUNK");
            putU32(goodBytes, 0);
        }
        const std::uint32_t goodRiff = static_cast<std::uint32_t>(goodBytes.size() - 8);
        goodBytes[4] = static_cast<unsigned char>(goodRiff & 0xFFu);
        goodBytes[5] = static_cast<unsigned char>((goodRiff >> 8) & 0xFFu);
        goodBytes[6] = static_cast<unsigned char>((goodRiff >> 16) & 0xFFu);
        goodBytes[7] = static_cast<unsigned char>((goodRiff >> 24) & 0xFFu);
        const std::string good = tmpPath("bounded_walk_healthy");
        CHECK(writeFile(good, goodBytes));
        CountingOpenSource ok;
        CHECK(ok.open(good));
        CHECK_NEAR(ok.sampleRateHz(), 8000.0, 0.0);
        CHECK(ok.headerReads() <= 8);
        CHECK(ok.start());
        std::complex<float> got[2];
        CHECK(ok.read(got, 2) == 2u);
        CHECK(got[0].real() == 7.0f / 32768.0f);
        CHECK(got[1].imag() == -9.0f / 32768.0f);
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

    // --- a file cut short under the receiver is a FAULT, not a quiet radio --
    //
    // read() zero-fills after an I/O error and keeps returning n, by design;
    // what was missing was any way for the pipeline to learn it had happened.
    // The file is overwritten with a shorter one WHILE the source has it open
    // (the handle is opened shared), so the next read runs off the end of the
    // data chunk open() validated - exactly a recording deleted or truncated
    // during playback. Before this flag, that session played silence for ever
    // with every lamp green.
    {
        std::vector<std::int16_t> body;
        for (int i = 0; i < 4000; ++i) { body.push_back(nextI16()); }
        const std::string path = tmpPath("truncated_midplay");
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(body))));
        IqFileSource src;
        CHECK(src.open(path));
        CHECK(src.start());
        CHECK(!src.faulted());
        std::complex<float> buf[256];
        CHECK(src.read(buf, 256) == 256u);
        CHECK(!src.faulted());
        CHECK(src.lastError()[0] == '\0');

        // Cut the file down to a header and a handful of frames.
        std::vector<std::int16_t> stub;
        for (int i = 0; i < 8; ++i) { stub.push_back(nextI16()); }
        CHECK(writeFile(path, wavBytes(1, 2, 48000, 16, bytesFromI16(stub))));

        // Reads still answer in full - the pacing contract - but the source
        // now says so, and keeps saying so.
        std::size_t got = 0;
        for (int i = 0; i < 16 && !src.faulted(); ++i) { got = src.read(buf, 256); }
        CHECK(got == 256u);
        CHECK(src.faulted());
        CHECK(errContains(src, "I/O error"));
        CHECK(src.read(buf, 256) == 256u);
        CHECK(src.faulted());

        // A fresh open() on a good file clears it: the fault belonged to the
        // file that went away, not to the source.
        const std::string good = tmpPath("truncated_midplay_recovered");
        CHECK(writeFile(good, wavBytes(1, 2, 48000, 16, bytesFromI16(body))));
        CHECK(src.open(good));
        CHECK(!src.faulted());
        CHECK(src.lastError()[0] == '\0');
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
