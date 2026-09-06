// Tests for core/recorder.hpp / recorder.cpp.
//
// Core strategy is SYMMETRY with the already-proven reader: an IQ recording
// is read back through source/iq_file_source and compared bit-exact
// (float32 passthrough both ways), which checks the header, the layout and
// the payload in one pass without trusting any recorder internals. Audio
// (1-channel, which IqFileSource rejects by design) and every RIFF size
// field are instead parsed byte-by-byte in-test.
//
// Temp policy: everything lands in per-case directories named with the
// process id (ctest runs this from build-<slug>/tests), removed on success
// and left behind for autopsy on failure.
//
// The one behaviour that cannot be reached honestly is the 4 GiB data-chunk
// ceiling — nothing here is going to write 4 GiB to find out what happens at
// the end of it — so those cases move the ceiling down to a few frames with
// Recorder::setMaxDataBytesForTest and run the same room arithmetic and the
// same end-of-take path over a handful of bytes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/recorder.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "source/iq_file_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Recorder;
using cascade::core::RecordKind;
using cascade::source::IqFileSource;

namespace {

// ---------------------------------------------------------------------------
// Temp bookkeeping
// ---------------------------------------------------------------------------

std::vector<std::string> g_tempDirs;
std::vector<std::string> g_tempFiles;

std::string tmpDir(const char* tag) {
    std::string d =
        "recorder_" + std::to_string(TEST_GETPID()) + "_" + tag;
    g_tempDirs.push_back(d);
    return d;
}

// ---------------------------------------------------------------------------
// Byte-level helpers (mirrors of the WAV spec, not of the implementation)
// ---------------------------------------------------------------------------

std::uint16_t u16le(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(p[1] << 8));
}

std::uint32_t u32le(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::vector<unsigned char> readAll(const std::string& path) {
    std::vector<unsigned char> v;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::printf("  readAll: cannot open \"%s\"\n", path.c_str());
        return v;
    }
    unsigned char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        v.insert(v.end(), buf, buf + n);
    }
    std::fclose(f);
    return v;
}

// The recorder writes into a name derived from the wall clock, so tests
// locate the output as "the single file in a directory this test created".
std::string onlyWavIn(const std::string& dir) {
    std::string found;
    int count = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file()) {
            ++count;
            found = e.path().string();
        }
    }
    if (count != 1) {
        std::printf("  expected exactly 1 file in \"%s\", found %d\n",
                    dir.c_str(), count);
        return std::string();
    }
    return found;
}

// Parses the fixed 44-byte header the recorder claims to write, checking
// every field against the WAV spec — never against recorder constants.
void checkHeader(const std::vector<unsigned char>& b, std::uint16_t wantTag,
                 std::uint16_t wantCh, std::uint32_t wantRate,
                 std::uint16_t wantBits, std::uint64_t wantDataBytes) {
    CHECK(b.size() == 44u + wantDataBytes);
    if (b.size() < 44) {
        return;  // the size CHECK above already failed; avoid OOB reads
    }
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(u32le(&b[4]) == 36u + wantDataBytes);  // RIFF size = file - 8
    CHECK(std::memcmp(&b[8], "WAVE", 4) == 0);
    CHECK(std::memcmp(&b[12], "fmt ", 4) == 0);
    CHECK(u32le(&b[16]) == 16u);  // classic PCM-style fmt block
    CHECK(u16le(&b[20]) == wantTag);
    CHECK(u16le(&b[22]) == wantCh);
    CHECK(u32le(&b[24]) == wantRate);
    CHECK(u32le(&b[28]) == wantRate * wantCh * (wantBits / 8u));  // byte rate
    CHECK(u16le(&b[32]) == wantCh * (wantBits / 8u));             // block align
    CHECK(u16le(&b[34]) == wantBits);
    CHECK(std::memcmp(&b[36], "data", 4) == 0);
    CHECK(u32le(&b[40]) == wantDataBytes);
}

// Fixed-seed LCG (Numerical Recipes constants) — deterministic sample data,
// no <random>, per the project testing protocol.
std::uint32_t g_lcg = 0x13579BDFu;
std::uint32_t nextU32() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}
// Finite float in about [-1.5, 1.5]: exercises the audio clamp on both
// sides while every value is still an exact float for IQ comparison.
float nextFloat() {
    return (static_cast<float>(static_cast<std::int32_t>(nextU32())) /
            2147483648.0f) * 1.5f;
}

}  // namespace

int main() {
    // --- makeFilename pinned exactly against fixed std::tm values ----------
    {
        std::tm t{};
        t.tm_year = 2026 - 1900;
        t.tm_mon = 7;  // August (tm_mon is 0-based)
        t.tm_mday = 15;
        t.tm_hour = 14;
        t.tm_min = 30;
        t.tm_sec = 59;
        CHECK(Recorder::makeFilename(RecordKind::BasebandIq, 2000000.0, t) ==
              "iq_20260815_143059_2000000Hz.wav");
        CHECK(Recorder::makeFilename(RecordKind::Audio, 48000.0, t) ==
              "audio_20260815_143059_48000Hz.wav");
        std::tm t2{};
        t2.tm_year = 1999 - 1900;
        t2.tm_mon = 0;  // January
        t2.tm_mday = 5;
        t2.tm_hour = 3;
        t2.tm_min = 4;
        t2.tm_sec = 5;
        // Single-digit fields must zero-pad, and the rate rounds to integer.
        CHECK(Recorder::makeFilename(RecordKind::Audio, 8000.4, t2) ==
              "audio_19990105_030405_8000Hz.wav");
    }

    // --- pre-start: everything inert, stop() safe and idempotent -----------
    {
        Recorder rec;
        CHECK(!rec.recording());
        CHECK(rec.samplesWritten() == 0u);
        CHECK(rec.bytesWritten() == 0u);
        const std::complex<float> iq[2] = {{0.5f, -0.5f}, {0.25f, 0.75f}};
        const float au[2] = {0.1f, -0.1f};
        rec.writeIq(iq, 2);
        rec.writeAudio(au, 2);
        CHECK(rec.samplesWritten() == 0u);
        CHECK(rec.bytesWritten() == 0u);
        rec.stop();
        rec.stop();
        CHECK(!rec.recording());
    }

    // --- invalid sample rates refused before touching the filesystem -------
    {
        Recorder rec;
        std::string err;
        CHECK(!rec.start(RecordKind::BasebandIq, ".", 0.0, err));
        CHECK(!err.empty());
        CHECK(!rec.start(RecordKind::BasebandIq, ".", -48000.0, err));
        CHECK(!err.empty());
        CHECK(!rec.recording());
    }

    // --- invalid directory (a FILE squats on the path) -> false + error ----
    {
        const std::string blocker =
            "recorder_" + std::to_string(TEST_GETPID()) + "_blocker";
        g_tempFiles.push_back(blocker);
        std::FILE* bf = std::fopen(blocker.c_str(), "wb");
        CHECK(bf != nullptr);
        if (bf != nullptr) {
            std::fputs("x", bf);
            std::fclose(bf);
        }
        Recorder rec;
        std::string err;
        CHECK(!rec.start(RecordKind::Audio, blocker + "/sub", 48000.0, err));
        CHECK(!err.empty());
        CHECK(!rec.recording());
        const float a[2] = {0.0f, 0.0f};
        rec.writeAudio(a, 2);  // after a failed start: still inert
        CHECK(rec.samplesWritten() == 0u);
        rec.stop();  // and stop() after a failed start is a no-op
    }

    // =======================================================================
    // IQ round trip through IqFileSource — the core symmetry test
    // =======================================================================
    Recorder rec;  // reused across IQ and audio cases to prove restartability
    {
        const std::string dir = tmpDir("iq");
        std::string err;
        CHECK(rec.start(RecordKind::BasebandIq, dir, 250000.0, err));
        CHECK(err.empty());
        CHECK(rec.recording());
        CHECK(rec.kind() == RecordKind::BasebandIq);

        // Wrong-kind write is a documented no-op.
        const float au[2] = {0.5f, -0.5f};
        rec.writeAudio(au, 2);
        CHECK(rec.samplesWritten() == 0u);

        // Double start refused, current take untouched.
        std::string err2;
        CHECK(!rec.start(RecordKind::Audio, dir, 48000.0, err2));
        CHECK(!err2.empty());
        CHECK(rec.recording());
        CHECK(rec.kind() == RecordKind::BasebandIq);

        // 10000 frames: crosses the 4096-frame staging block boundary, and
        // the leading specials prove IQ is a bit-exact passthrough (no
        // clamp, -0 and subnormals survive — same properties the file
        // source's own tests pin on the read side).
        std::vector<std::complex<float>> tx(10000);
        tx[0] = {-0.0f, 1.5f};
        tx[1] = {3.0e-39f, -2.25f};
        tx[2] = {1.0f, -1.0f};
        for (std::size_t i = 3; i < tx.size(); ++i) {
            tx[i] = {nextFloat(), nextFloat()};
        }
        rec.writeIq(tx.data(), 6000);
        rec.writeIq(tx.data() + 6000, 4000);  // append across calls
        CHECK(rec.samplesWritten() == 10000u);
        CHECK(rec.bytesWritten() == 80000u);

        const std::string wavPath = onlyWavIn(dir);
        CHECK(!wavPath.empty());
        {
            // Filename shape: start() must produce exactly a makeFilename()
            // name for the requested rate (the timestamp is the wall clock,
            // so only its width is checkable).
            const std::string base = fs::path(wavPath).filename().string();
            CHECK(base.rfind("iq_", 0) == 0);
            CHECK(base.size() ==
                  std::string("iq_YYYYMMDD_HHMMSS_250000Hz.wav").size());
            CHECK(base.find("_250000Hz.wav") != std::string::npos);
        }

        // Crash contract, observed directly: while recording, the header on
        // disk was flushed by start() and still declares ZERO samples — the
        // exact bytes a crash right now would leave behind, and they parse.
        {
            const std::vector<unsigned char> mid = readAll(wavPath);
            CHECK(mid.size() >= 44u);
            if (mid.size() >= 44u) {
                CHECK(std::memcmp(mid.data(), "RIFF", 4) == 0);
                CHECK(u32le(&mid[4]) == 36u);   // RIFF size: empty data chunk
                CHECK(u32le(&mid[40]) == 0u);   // data size: zero-length
            }
        }

        rec.stop();
        CHECK(!rec.recording());
        CHECK(rec.samplesWritten() == 10000u);  // counters survive stop()
        rec.stop();  // idempotent: a second stop must not re-patch or crash

        // Header parsed by hand: float32 (tag 3), stereo, patched sizes.
        checkHeader(readAll(wavPath), 3, 2, 250000, 32, 80000);

        // Read back through the proven reader, bit-exact.
        IqFileSource src;
        CHECK(src.open(wavPath));
        CHECK_NEAR(src.sampleRateHz(), 250000.0, 0.0);
        CHECK(src.start());
        std::vector<std::complex<float>> rx(
            tx.size(), std::complex<float>(-999.0f, -999.0f));
        CHECK(src.read(rx.data(), rx.size()) == rx.size());
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < tx.size(); ++i) {
            if (std::bit_cast<std::uint32_t>(rx[i].real()) !=
                    std::bit_cast<std::uint32_t>(tx[i].real()) ||
                std::bit_cast<std::uint32_t>(rx[i].imag()) !=
                    std::bit_cast<std::uint32_t>(tx[i].imag())) {
                ++mismatches;
                if (mismatches <= 3) {
                    std::printf("  IQ mismatch at frame %zu\n", i);
                }
            }
        }
        CHECK(mismatches == 0u);
        src.stop();
    }

    // =======================================================================
    // Audio round trip: clamp + int16, same 1/32768 convention as the reader
    // =======================================================================
    {
        const std::string dir = tmpDir("audio");
        std::string err;
        CHECK(rec.start(RecordKind::Audio, dir, 48000.0, err));
        CHECK(rec.kind() == RecordKind::Audio);
        CHECK(rec.samplesWritten() == 0u);  // counters reset by start()

        // Wrong-kind write is a no-op in this direction too.
        const std::complex<float> iq[2] = {{0.5f, -0.5f}, {0.1f, 0.2f}};
        rec.writeIq(iq, 2);
        CHECK(rec.samplesWritten() == 0u);

        std::vector<float> atx(5000);
        atx[0] = 0.0f;
        atx[1] = 1.0f;    // no int16 code for +1.0: must saturate to 32767
        atx[2] = -1.0f;   // exactly representable: -32768
        atx[3] = 1.5f;    // out of range: clamps, then saturates
        atx[4] = -2.0f;   // out of range: clamps to exactly -1
        atx[5] = 1.0f / 32768.0f;
        atx[6] = -1.0f / 32768.0f;
        for (std::size_t i = 7; i < atx.size(); ++i) {
            atx[i] = nextFloat();  // spans ±1.5: clamping on both sides
        }
        rec.writeAudio(atx.data(), 3000);
        rec.writeAudio(atx.data() + 3000, 2000);
        CHECK(rec.samplesWritten() == 5000u);
        CHECK(rec.bytesWritten() == 10000u);
        rec.stop();

        const std::string wavPath = onlyWavIn(dir);
        CHECK(!wavPath.empty());
        CHECK(fs::path(wavPath).filename().string().rfind("audio_", 0) == 0);
        const std::vector<unsigned char> b = readAll(wavPath);
        checkHeader(b, 1, 1, 48000, 16, 10000);
        if (b.size() == 44u + 10000u) {
            // Round trip within 1 LSB of the clamped input — the documented
            // convention shared with IqFileSource's i/32768 read mapping.
            std::size_t bad = 0;
            for (std::size_t i = 0; i < atx.size(); ++i) {
                const std::int16_t q =
                    static_cast<std::int16_t>(u16le(&b[44 + 2 * i]));
                const float got = static_cast<float>(q) / 32768.0f;
                double want = static_cast<double>(atx[i]);
                want = want > 1.0 ? 1.0 : (want < -1.0 ? -1.0 : want);
                if (std::fabs(static_cast<double>(got) - want) >
                    1.0 / 32768.0 + 1e-9) {
                    ++bad;
                    if (bad <= 3) {
                        std::printf("  audio sample %zu: wrote %.8f read %.8f\n",
                                    i, want, static_cast<double>(got));
                    }
                }
            }
            CHECK(bad == 0u);
            // The unambiguous codes, pinned exactly.
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 0])) == 0);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 2])) == 32767);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 4])) == -32768);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 6])) == 32767);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 8])) == -32768);
        }
    }

    // =======================================================================
    // RIFF sizes for 0, 1 and 100k samples, parsed in-test
    // =======================================================================
    {
        // 0 samples: start immediately followed by stop. The 44-byte husk is
        // exactly the crash-contract artifact, finalized.
        const std::string dir = tmpDir("r0");
        Recorder r;
        std::string err;
        CHECK(r.start(RecordKind::BasebandIq, dir, 2000000.0, err));
        r.stop();
        checkHeader(readAll(onlyWavIn(dir)), 3, 2, 2000000, 32, 0);
    }
    {
        // 1 sample, payload verified bit-exact at its absolute offset.
        const std::string dir = tmpDir("r1");
        Recorder r;
        std::string err;
        CHECK(r.start(RecordKind::BasebandIq, dir, 2000000.0, err));
        const std::complex<float> one(0.25f, -0.75f);
        r.writeIq(&one, 1);
        r.stop();
        const std::vector<unsigned char> b = readAll(onlyWavIn(dir));
        checkHeader(b, 3, 2, 2000000, 32, 8);
        if (b.size() == 52u) {
            CHECK(u32le(&b[44]) == std::bit_cast<std::uint32_t>(0.25f));
            CHECK(u32le(&b[48]) == std::bit_cast<std::uint32_t>(-0.75f));
        }
    }
    {
        // 100k frames in ONE call: larger than the 4096-frame staging block
        // AND the 256 KiB stdio buffer, so both spill paths run for real.
        const std::string dir = tmpDir("r100k");
        Recorder r;
        std::string err;
        CHECK(r.start(RecordKind::BasebandIq, dir, 1000000.0, err));
        std::vector<std::complex<float>> tx(100000);
        for (auto& v : tx) {
            v = {nextFloat(), nextFloat()};
        }
        r.writeIq(tx.data(), tx.size());
        CHECK(r.samplesWritten() == 100000u);
        CHECK(r.bytesWritten() == 800000u);
        // 800 kB is nowhere near the real ceiling: a take this size must not
        // trip the end-of-take path that the cases at the bottom exercise.
        CHECK(!r.sizeLimitReached());
        CHECK(!r.writeFailed());
        r.stop();
        const std::vector<unsigned char> b = readAll(onlyWavIn(dir));
        checkHeader(b, 3, 2, 1000000, 32, 800000);
        if (b.size() == 44u + 800000u) {
            // First and last frame at their absolute offsets: proves no
            // drift across every staging/stdio boundary in between.
            CHECK(u32le(&b[44]) ==
                  std::bit_cast<std::uint32_t>(tx[0].real()));
            CHECK(u32le(&b[48]) ==
                  std::bit_cast<std::uint32_t>(tx[0].imag()));
            CHECK(u32le(&b[44 + 8 * 99999]) ==
                  std::bit_cast<std::uint32_t>(tx[99999].real()));
            CHECK(u32le(&b[44 + 8 * 99999 + 4]) ==
                  std::bit_cast<std::uint32_t>(tx[99999].imag()));
        }
    }

    // --- start() creates the whole missing directory chain ------------------
    {
        const std::string root = tmpDir("deep");  // registered for cleanup
        const std::string nested = root + "/a/b/c";
        Recorder r;
        std::string err;
        CHECK(r.start(RecordKind::Audio, nested, 8000.0, err));
        CHECK(err.empty());
        CHECK(fs::is_directory(nested));
        const float a[3] = {0.25f, -0.25f, 0.5f};
        r.writeAudio(a, 3);
        r.stop();
        checkHeader(readAll(onlyWavIn(nested)), 1, 1, 8000, 16, 6);
    }

    // =======================================================================
    // The data-chunk ceiling ENDS the take — it used to stall it in silence
    // =======================================================================
    //
    // 83 rather than a round 80 because the product's own ceiling is not a
    // whole number of frames either (0xFFFFFFFF - 36 leaves 3 bytes over an
    // 8-byte IQ frame). Those last 3 bytes must be simply unusable, not a
    // zero-length write the staging loop can spin on.
    {
        const std::string dir = tmpDir("cap_iq");
        Recorder r;
        r.setMaxDataBytesForTest(83);  // 10 IQ frames fit, 3 bytes spare
        std::string err;
        CHECK(r.start(RecordKind::BasebandIq, dir, 250000.0, err));
        CHECK(!r.sizeLimitReached());
        CHECK(!r.writeFailed());

        std::vector<std::complex<float>> tx(25);
        for (std::size_t i = 0; i < tx.size(); ++i) {
            tx[i] = {nextFloat(), nextFloat()};
        }
        // 25 frames offered, 10 of them fit: the block is truncated to what
        // the chunk can still hold, and that is the end of the take.
        r.writeIq(tx.data(), tx.size());
        CHECK(r.samplesWritten() == 10u);
        CHECK(r.bytesWritten() == 80u);
        // THE RECORDER MUST STOP SAYING IT IS RECORDING. This is the defect:
        // a full chunk used to make writeIq return with recording() still
        // true and no flag set anywhere, so the panel kept its REC lamp and
        // its climbing clock over a file nothing was reaching any more.
        CHECK(!r.recording());
        CHECK(r.sizeLimitReached());
        CHECK(r.writeFailed());  // the same "nothing reaches the file" flag

        // And the file is FINALIZED without anyone calling stop(): the header
        // on disk describes the 10 frames that were kept. Before the fix it
        // was still the zero-length husk start() flushed, with the 80 sample
        // bytes sitting unflushed in the 256 KiB stdio buffer.
        const std::string wavPath = onlyWavIn(dir);
        CHECK(!wavPath.empty());
        const std::vector<unsigned char> b = readAll(wavPath);
        checkHeader(b, 3, 2, 250000, 32, 80);
        if (b.size() == 44u + 80u) {
            // What fitted is the FIRST 10 frames, in order and bit-exact.
            CHECK(u32le(&b[44]) == std::bit_cast<std::uint32_t>(tx[0].real()));
            CHECK(u32le(&b[48]) == std::bit_cast<std::uint32_t>(tx[0].imag()));
            CHECK(u32le(&b[44 + 8 * 9]) ==
                  std::bit_cast<std::uint32_t>(tx[9].real()));
            CHECK(u32le(&b[44 + 8 * 9 + 4]) ==
                  std::bit_cast<std::uint32_t>(tx[9].imag()));
        }

        // Writes after the ceiling are inert, and a Stop pressed on a take
        // that already finalized itself must not re-patch or re-close it.
        r.writeIq(tx.data(), tx.size());
        CHECK(r.samplesWritten() == 10u);
        CHECK(r.bytesWritten() == 80u);
        r.stop();
        CHECK(!r.recording());
        CHECK(r.samplesWritten() == 10u);  // counters survive, as after stop()
        CHECK(readAll(wavPath) == b);      // byte for byte the same file
        r.stop();                          // and still idempotent

        // The next take starts clean — both flags belong to the take that
        // set them, not to the recorder.
        const std::string dir2 = tmpDir("cap_iq_next");
        r.setMaxDataBytesForTest(1024);
        CHECK(r.start(RecordKind::BasebandIq, dir2, 250000.0, err));
        CHECK(r.recording());
        CHECK(!r.sizeLimitReached());
        CHECK(!r.writeFailed());
        r.writeIq(tx.data(), 3);
        r.stop();
        checkHeader(readAll(onlyWavIn(dir2)), 3, 2, 250000, 32, 24);
    }
    {
        // A block that lands EXACTLY on the ceiling runs out of samples and
        // of room in the same pass, so nothing takes the loop round again to
        // notice. Ending it after the loop is what stops a full file being
        // left open until another block arrives — and if the user pressed
        // Stop on the pipeline first, that block never arrives at all.
        const std::string dir = tmpDir("cap_exact");
        Recorder r;
        r.setMaxDataBytesForTest(83);  // 10 frames again
        std::string err;
        CHECK(r.start(RecordKind::BasebandIq, dir, 2000000.0, err));
        std::vector<std::complex<float>> tx(10);
        for (std::size_t i = 0; i < tx.size(); ++i) {
            tx[i] = {nextFloat(), nextFloat()};
        }
        r.writeIq(tx.data(), tx.size());  // fills the chunk exactly
        CHECK(!r.recording());
        CHECK(r.sizeLimitReached());
        CHECK(r.writeFailed());
        CHECK(r.samplesWritten() == 10u);
        CHECK(r.bytesWritten() == 80u);
        checkHeader(readAll(onlyWavIn(dir)), 3, 2, 2000000, 32, 80);
    }
    {
        // Audio takes end the same way. At 96 kB/s nobody reaches the real
        // ceiling in a sitting, but a file that quietly stops growing is not
        // a behaviour worth keeping for the rare case either.
        const std::string dir = tmpDir("cap_audio");
        Recorder r;
        r.setMaxDataBytesForTest(11);  // 5 int16 samples fit, 1 byte spare
        std::string err;
        CHECK(r.start(RecordKind::Audio, dir, 48000.0, err));
        const float a[12] = {0.5f,  -0.5f, 0.25f, -0.25f, 0.125f, -0.125f,
                             0.75f, -0.75f, 1.0f, -1.0f,  0.0f,   0.375f};
        r.writeAudio(a, 12);
        CHECK(r.samplesWritten() == 5u);
        CHECK(r.bytesWritten() == 10u);
        CHECK(!r.recording());
        CHECK(r.sizeLimitReached());
        CHECK(r.writeFailed());
        r.writeAudio(a, 12);  // inert after the ceiling
        CHECK(r.samplesWritten() == 5u);
        const std::vector<unsigned char> b = readAll(onlyWavIn(dir));
        checkHeader(b, 1, 1, 48000, 16, 10);
        if (b.size() == 44u + 10u) {
            // The five that fitted are the first five, quantized as usual.
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 0])) == 16384);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 2])) == -16384);
            CHECK(static_cast<std::int16_t>(u16le(&b[44 + 8])) == 4096);
        }
    }

    const int rc = testSummary("test_recorder");
    if (rc == 0) {
        // Success: remove every per-case directory and the blocker file.
        // On failure they stay behind for inspection.
        for (const std::string& d : g_tempDirs) {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
        for (const std::string& f : g_tempFiles) {
            std::remove(f.c_str());
        }
    }
    return rc;
}
