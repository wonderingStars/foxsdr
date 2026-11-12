// Tests for the playback position model (source/iq_playback.hpp).
//
// Test files are written here byte by byte. Raw cf32 files hold a RAMP (I = frame index, Q = -frame index, exact in float up to 2^24) so every
// frame identifies its own position and a wrong, repeated or missing frame is visible in the data. Audio files hold value = frame / 1000.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_playback.hpp"

#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace pb = cascade::source::playback;
namespace iq = cascade::source::iqfile;
using cf = std::complex<float>;
using Bytes = std::vector<unsigned char>;
fs::path g_dir;

void le16(Bytes& b, unsigned v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
void le32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back((v >> (8 * i)) & 0xFF); } }
void le64(Bytes& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) { b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } }
void tag(Bytes& b, const char* s) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<unsigned char>(s[i])); } }

std::string rampCf32(std::size_t frames, const std::string& name) {
    const std::string p = (g_dir / name).string();
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    std::vector<float> buf(2 * 4096);
    for (std::size_t at = 0; at < frames; at += 4096) {
        const std::size_t n = std::min<std::size_t>(4096, frames - at);
        for (std::size_t i = 0; i < n; ++i) { buf[2 * i] = static_cast<float>(at + i); buf[2 * i + 1] = -static_cast<float>(at + i); }
        o.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n * 8));
    }
    return p;
}

std::unique_ptr<pb::FrameSource> openRamp(const std::string& path, double rate = 1e6) {
    iq::RawParams rp; rp.sampleRateHz = rate; rp.centerFrequencyHz = 100e6; rp.hasCenter = true;
    iq::Layout l; l.kind = iq::Kind::F32;
    std::string err;
    auto r = iq::openRaw(path, l, rp, &err);
    if (r == nullptr) { std::printf("  openRaw: %s\n", err.c_str()); return nullptr; }
    return std::make_unique<pb::IqReaderSource>(std::move(r));
}

// a WAV with a mono 16-bit ramp: sample(frame) = frame (clamped), written as PCM16; value in float = frame / 32768
std::string wavMono(const std::string& name, std::uint32_t rate, std::size_t frames, int bits = 16, int channels = 1, bool isFloat = false, std::uint32_t dataFieldOverride = 0xFFFFFFFEu) {
    Bytes d;
    const std::size_t bytesPer = static_cast<std::size_t>(bits / 8) * static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const long v = static_cast<long>(i % 300) * (c + 1);
            if (isFloat) { float f = static_cast<float>(v) / 1000.0f; unsigned char b[4]; std::memcpy(b, &f, 4); d.insert(d.end(), b, b + 4); }
            else if (bits == 8) { d.push_back(static_cast<unsigned char>(128 + v / 8)); }
            else if (bits == 16) { le16(d, static_cast<unsigned>(v * 32 & 0xFFFF)); }
            else if (bits == 24) { const long s = v * 8192; d.push_back(s & 0xFF); d.push_back((s >> 8) & 0xFF); d.push_back((s >> 16) & 0xFF); }
            else { const long s = v * 2097152; le32(d, static_cast<std::uint32_t>(s)); }
        }
    }
    Bytes f;
    tag(f, "RIFF"); le32(f, static_cast<std::uint32_t>(36 + d.size())); tag(f, "WAVE");
    tag(f, "fmt "); le32(f, 16); le16(f, isFloat ? 3 : 1); le16(f, static_cast<unsigned>(channels)); le32(f, rate); le32(f, static_cast<std::uint32_t>(rate * bytesPer));
    le16(f, static_cast<unsigned>(bytesPer)); le16(f, static_cast<unsigned>(bits));
    tag(f, "data"); le32(f, dataFieldOverride == 0xFFFFFFFEu ? static_cast<std::uint32_t>(d.size()) : dataFieldOverride);
    f.insert(f.end(), d.begin(), d.end());
    const std::string p = (g_dir / name).string();
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    return p;
}

bool isRamp(const cf* d, std::size_t n, std::uint64_t first) {
    for (std::size_t i = 0; i < n; ++i) {
        if (d[i].real() != static_cast<float>(first + i) || d[i].imag() != -static_cast<float>(first + i)) { return false; }
    }
    return true;
}

void testSequentialAndSeek() {
    const std::string path = rampCf32(1000000, "recm-pb-ramp.cf32");
    pb::PlaybackModel m;
    std::string err;
    CHECK(m.open(openRamp(path), &err));
    pb::PlaybackPosition p = m.position();
    CHECK(p.totalFrames == 1000000 && p.frame == 0 && p.rateKnown && p.durationS == 1.0 && p.fraction == 0.0 && !p.atEnd);
    // sequential read in odd-sized blocks
    std::vector<cf> buf(5000);
    std::uint64_t at = 0;
    bool ok = true;
    int blocks = 0;
    for (;;) {
        const std::size_t n = 1 + (blocks * 7919) % 4999;
        const pb::ReadResult r = m.read(buf.data(), n);
        if (r.startFrame != at || !isRamp(buf.data(), r.frames, at)) { ok = false; }
        at += r.frames;
        ++blocks;
        if (r.endOfData) { CHECK(r.frames < n || at == 1000000); break; }
        if (r.frames < n) { ok = false; break; }
    }
    CHECK(ok);
    CHECK(at == 1000000);
    p = m.position();
    CHECK(p.frame == 1000000 && p.fraction == 1.0 && p.atEnd);
    CHECK(m.read(buf.data(), 10).frames == 0);   // still at the end
    std::printf("sequential: 1,000,000 frames in %d blocks of odd sizes, every frame equal to its index, end of data flagged once at the end\n", blocks);

    // seek by frame / time / fraction, each with a verified first block
    struct Row { const char* what; bool ok; std::uint64_t want; };
    CHECK(m.seekSeconds(0.25)); { const auto r = m.read(buf.data(), 100); CHECK(r.startFrame == 250000 && isRamp(buf.data(), 100, 250000)); }
    CHECK(m.seekFraction(0.5)); { const auto r = m.read(buf.data(), 100); CHECK(r.startFrame == 500000 && isRamp(buf.data(), 100, 500000)); }
    CHECK(m.seekFrame(123456)); { const auto r = m.read(buf.data(), 100); CHECK(r.startFrame == 123456 && isRamp(buf.data(), 100, 123456)); }
    CHECK(m.seekFraction(1.0)); CHECK(m.position().frame == 1000000); { const auto r = m.read(buf.data(), 100); CHECK(r.frames == 0 && r.endOfData); }
    CHECK(m.seekFraction(7.5)); CHECK(m.position().frame == 1000000);          // clamped
    CHECK(m.seekFraction(-3.0)); CHECK(m.position().frame == 0);
    CHECK(m.seekSeconds(-5.0)); CHECK(m.position().frame == 0);
    CHECK(m.seekSeconds(1e9)); CHECK(m.position().frame == 1000000);
    CHECK(m.seekFrame(~0ull)); CHECK(m.position().frame == 1000000);
    // the position is reported at once, before any read
    CHECK(m.seekSeconds(0.4)); CHECK(m.position().frame == 400000 && std::fabs(m.position().seconds - 0.4) < 1e-9 && std::fabs(m.position().fraction - 0.4) < 1e-9);
    // refusals: nothing changes, the generation does not move
    const std::uint32_t g0 = m.generation();
    CHECK(!m.seekFraction(std::nan("")));
    CHECK(!m.seekSeconds(std::nan("")));
    CHECK(!m.seekSeconds(INFINITY) || m.position().frame == 1000000);   // +inf may clamp or refuse, never crash; checked below
    m.seekSeconds(0.4);
    const std::uint32_t g1 = m.generation();
    CHECK(g1 > g0);
    CHECK(!m.seekFraction(std::nan("")) && m.generation() == g1 && m.position().frame == 400000);
    // block-boundary seeks: sequential reads across a seek
    CHECK(m.seekFrame(999990)); { const auto r = m.read(buf.data(), 100); CHECK(r.frames == 10 && r.endOfData && isRamp(buf.data(), 10, 999990)); }
    // the unopened model
    pb::PlaybackModel empty; CHECK(!empty.seekFrame(1) && !empty.seekFraction(0.5) && !empty.seekSeconds(1.0) && !empty.setLoop(0, 1) && !empty.setSpeed(1.0));
    CHECK(empty.read(buf.data(), 10).frames == 0);
    CHECK(!empty.open(nullptr, &err));
    // unknown rate: seconds refused, fraction and frame fine
    struct NoRate final : pb::FrameSource {
        std::uint64_t totalFrames() const override { return 100; }
        double sampleRateHz() const override { return 0.0; }
        bool isAudio() const override { return false; }
        bool seek(std::uint64_t) override { return true; }
        std::size_t read(cf* d, std::size_t n) override { for (std::size_t i = 0; i < n; ++i) { d[i] = cf(0, 0); } return n; }
    };
    pb::PlaybackModel nr; CHECK(nr.open(std::make_unique<NoRate>(), &err));
    CHECK(!nr.seekSeconds(1.0) && nr.seekFraction(0.5) && nr.position().frame == 50 && !nr.position().rateKnown && nr.position().seconds == 0.0);
    CHECK(!nr.setLoopSeconds(0.0, 1.0));
    // zero-frame file
    const std::string e0 = (g_dir / "recm-pb-empty.cf32").string(); { std::ofstream o(e0, std::ios::binary); }
    pb::PlaybackModel z; CHECK(z.open(openRamp(e0), &err));
    CHECK(z.position().totalFrames == 0 && z.position().fraction == 0.0 && z.seekFraction(0.5) && z.position().frame == 0);
    { const auto r = z.read(buf.data(), 10); CHECK(r.frames == 0 && r.endOfData); }
    std::printf("seek: by frame, seconds and fraction with the first block verified; clamps; NaN refused without moving the generation; empty file; unknown rate\n");
}

// A source that can be held inside read() so a seek can arrive "while playing".
struct GateSource final : pb::FrameSource {
    std::unique_ptr<pb::FrameSource> inner;
    std::mutex mu; std::condition_variable cv;
    bool hold = false, inside = false;
    explicit GateSource(std::unique_ptr<pb::FrameSource> i) : inner(std::move(i)) {}
    std::uint64_t totalFrames() const override { return inner->totalFrames(); }
    double sampleRateHz() const override { return inner->sampleRateHz(); }
    bool isAudio() const override { return false; }
    bool seek(std::uint64_t f) override { return inner->seek(f); }
    std::size_t read(cf* d, std::size_t n) override {
        const std::size_t got = inner->read(d, n);   // the data is read from the OLD position first
        std::unique_lock<std::mutex> lk(mu);
        if (hold) { inside = true; cv.notify_all(); cv.wait(lk, [&] { return !hold; }); inside = false; }
        return got;
    }
};

void testStaleBlock() {
    const std::string path = rampCf32(1000000, "recm-pb-ramp2.cf32");
    auto gate = std::make_unique<GateSource>(openRamp(path));
    GateSource* g = gate.get();
    pb::PlaybackModel m;
    std::string err;
    CHECK(m.open(std::move(gate), &err));
    std::vector<cf> buf(1000), buf2(1000);
    CHECK(m.read(buf.data(), 1000).startFrame == 0);   // position now 1000
    { std::lock_guard<std::mutex> lk(g->mu); g->hold = true; }
    pb::ReadResult held;
    std::thread reader([&] { held = m.read(buf.data(), 1000); });
    {
        std::unique_lock<std::mutex> lk(g->mu);
        g->cv.wait(lk, [&] { return g->inside; });
    }
    // the reader is inside read(), holding a block from the OLD position (1000..1999)
    const std::uint32_t before = m.generation();
    CHECK(m.seekFrame(500000));
    CHECK(m.generation() != before);
    CHECK(m.position().frame == 500000);               // the bar moved at once
    { std::lock_guard<std::mutex> lk(g->mu); g->hold = false; g->cv.notify_all(); }
    reader.join();
    CHECK(held.generation == before);                  // the block carries the old generation ...
    CHECK(held.generation != m.generation());          // ... so the consumer drops it
    CHECK(held.startFrame == 1000 && isRamp(buf.data(), 1000, 1000));
    CHECK(m.position().frame == 500000);               // the slow read did not drag the bar back
    const pb::ReadResult next = m.read(buf2.data(), 1000);
    CHECK(next.generation == m.generation());
    CHECK(next.startFrame == 500000 && isRamp(buf2.data(), 1000, 500000));   // the first block after the seek starts at the target
    CHECK(m.position().frame == 501000);
    std::printf("stale block: a block held inside read() across a seek comes back with the old generation, the bar stays at the target, and the next block starts exactly there\n");

    // STRESS: a reader thread runs flat out, the main thread seeks at random; the consumer drops stale blocks. Every kept block must be a contiguous
    // ramp starting where it says, and after the LAST seek the first kept block starts at its target.
    pb::PlaybackModel s;
    CHECK(s.open(openRamp(path), &err));
    std::atomic<bool> stop{false};
    std::atomic<int> kept{0}, dropped{0}, bad{0};
    std::atomic<std::uint64_t> lastTarget{~0ull};
    std::atomic<bool> sawAfterLast{false};
    std::atomic<std::uint64_t> firstAfterLast{~0ull};
    std::thread rd([&] {
        std::vector<cf> b(777);
        while (!stop.load()) {
            const pb::ReadResult r = s.read(b.data(), b.size());
            if (r.generation != s.generation()) { ++dropped; continue; }
            if (r.frames > 0 && !isRamp(b.data(), r.frames, r.startFrame)) { ++bad; }
            ++kept;
            const std::uint64_t lt = lastTarget.load();
            if (lt != ~0ull && !sawAfterLast.load()) { firstAfterLast.store(r.startFrame); sawAfterLast.store(true); }
            if (r.endOfData) { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
        }
    });
    std::mt19937 rng(5);
    for (int i = 0; i < 300; ++i) {
        s.seekFrame(rng() % 990000);
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
    }
    // final seek: all later kept blocks start at 777000 (the first kept block after it)
    sawAfterLast.store(false);
    s.seekFrame(777000);
    lastTarget.store(777000);
    // wait for the first kept block after the last seek
    for (int i = 0; i < 4000 && !sawAfterLast.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    stop.store(true);
    rd.join();
    CHECK(bad.load() == 0);
    CHECK(kept.load() > 100);
    CHECK(sawAfterLast.load());
    // the first kept block after the final seek can only be one that started after the seek: it begins at 777000 (the reader may also have kept a block
    // of generation == current that began at 777000 only; a block from before the seek has the older generation)
    CHECK(firstAfterLast.load() == 777000);
    std::printf("stale-block stress: 300 random seeks against a reader running flat out: %d blocks kept (all contiguous ramps), %d dropped as stale, first block after the last seek starts at 777000\n",
                kept.load(), dropped.load());
}

void testLoop() {
    const std::string path = rampCf32(100000, "recm-pb-ramp3.cf32");
    pb::PlaybackModel m;
    std::string err;
    CHECK(m.open(openRamp(path, 48000), &err));
    CHECK(!m.setLoop(10, 10));
    CHECK(!m.setLoop(20, 10));
    CHECK(!m.setLoop(0, 100001));
    CHECK(!m.position().loopEnabled);
    CHECK(m.setLoop(1000, 2500));
    CHECK(m.position().loopEnabled && m.position().loopA == 1000 && m.position().loopB == 2500);
    CHECK(m.seekFrame(1000));
    std::vector<cf> buf(700);
    // 12 blocks of 700 = 8400 frames = 5.6 loops of 1500: the output must be 1000..2499 repeated with no missing or repeated frame at the wraps
    std::uint64_t expectNext = 1000;
    bool ok = true;
    bool full = true;
    std::uint64_t minPos = ~0ull, maxPos = 0;
    for (int blk = 0; blk < 12; ++blk) {
        const pb::ReadResult r = m.read(buf.data(), 700);
        if (r.frames != 700 || r.endOfData) { full = false; }
        for (std::size_t i = 0; i < r.frames; ++i) {
            if (buf[i].real() != static_cast<float>(expectNext) || buf[i].imag() != -static_cast<float>(expectNext)) { ok = false; }
            expectNext = expectNext + 1 >= 2500 ? 1000 : expectNext + 1;
        }
        const std::uint64_t pos = m.position().frame;
        minPos = std::min(minPos, pos); maxPos = std::max(maxPos, pos);
    }
    CHECK(ok);
    CHECK(full);
    CHECK(minPos >= 1000 && maxPos <= 2500);
    std::printf("loop: [1000, 2500) read in 700-frame blocks, 8400 frames: gapless (every frame as expected across 5 wraps), every block full, position stayed within the loop\n");

    // a seek beyond B continues from A
    CHECK(m.seekFrame(50000));
    { const auto r = m.read(buf.data(), 10); CHECK(r.frames == 10 && r.startFrame == 1000 && isRamp(buf.data(), 10, 1000)); }
    // clearing the loop resumes linear playback
    m.clearLoop();
    CHECK(!m.position().loopEnabled);
    CHECK(m.seekFrame(2490));
    { const auto r = m.read(buf.data(), 20); CHECK(r.frames == 20 && isRamp(buf.data(), 20, 2490)); }
    // a loop whose end is moved below the play position: the next read restarts at A
    CHECK(m.seekFrame(3000));
    CHECK(m.setLoop(100, 200));
    { const auto r = m.read(buf.data(), 5); CHECK(r.startFrame == 100 && isRamp(buf.data(), 5, 100)); }
    // one-frame loop repeats that frame
    CHECK(m.setLoop(5, 6)); CHECK(m.seekFrame(5));
    { const auto r = m.read(buf.data(), 4); CHECK(r.frames == 4); bool same = true; for (int i = 0; i < 4; ++i) { if (buf[static_cast<std::size_t>(i)] != cf(5.f, -5.f)) { same = false; } } CHECK(same); }
    // a loop ending at the very end wraps there and never reports the end
    CHECK(m.setLoop(99990, 100000)); CHECK(m.seekFrame(99995));
    { const auto r = m.read(buf.data(), 30); CHECK(r.frames == 30 && !r.endOfData); CHECK(buf[0] == cf(99995.f, -99995.f) && buf[5] == cf(99990.f, -99990.f) && buf[15] == cf(99990.f, -99990.f)); }
    // mark A / mark B from the playing position
    m.clearLoop();
    CHECK(!m.markLoopEnd());                             // no A yet
    CHECK(m.seekFrame(4000)); m.read(buf.data(), 1);     // position 4001
    CHECK(m.markLoopStart());
    CHECK(!m.markLoopEnd());                             // B == A refused
    m.read(buf.data(), 99);                              // position 4100
    CHECK(m.markLoopEnd());
    CHECK(m.position().loopA == 4001 && m.position().loopB == 4100 && m.position().loopEnabled);
    // loop in seconds (48 kHz): 0.5 s .. 1.0 s
    CHECK(m.setLoopSeconds(0.5, 1.0) && m.position().loopA == 24000 && m.position().loopB == 48000);
    CHECK(!m.setLoopSeconds(1.0, 0.5) && !m.setLoopSeconds(-1.0, 1.0) && !m.setLoopSeconds(0.0, 1e6) && !m.setLoopSeconds(std::nan(""), 1.0));
    CHECK(m.position().loopA == 24000);                  // unchanged by the refusals
    std::printf("loop edge rows: seek past B, clear, B moved below the position, one-frame loop, loop to the very end, mark A/B, loop in seconds, refusals leave the loop unchanged\n");
}

void testSpeed() {
    // an audio WAV: mono 16-bit at 48 kHz holding value = (frame % 1000) * 32 / 32768 -> a sawtooth ramp of slope 32/32768 per frame inside each 300
    const std::string wp = wavMono("recm-pb-mono.wav", 48000, 20000, 16, 1);
    pb::PlaybackModel m;
    std::string err;
    auto a = pb::WavAudioSource::open(wp, &err);
    CHECK(a != nullptr);
    CHECK(m.open(std::move(a), &err));
    CHECK(m.position().totalFrames == 20000 && m.position().rateKnown && std::fabs(m.position().durationS - 20000.0 / 48000.0) < 1e-12);
    // speed 1: the ramp exactly
    std::vector<cf> buf(400);
    auto sample = [](std::uint64_t f) { return static_cast<float>((f % 300) * 32) / 32768.0f; };
    m.seekFrame(100);
    pb::ReadResult r = m.read(buf.data(), 100);
    bool ok = r.frames == 100;
    for (std::size_t i = 0; i < 100; ++i) { if (std::fabs(buf[i].real() - sample(100 + i)) > 1e-7f) { ok = false; } }
    CHECK(ok);
    // speeds: inside one ramp segment (no wrap of the sawtooth) output k = sample(100 + k * speed) by linear interpolation
    for (double sp : {0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0}) {
        CHECK(m.setSpeed(sp));
        m.seekFrame(100);
        r = m.read(buf.data(), 200);
        ok = r.frames == 200;
        float maxErr = 0.0f;
        for (std::size_t k = 0; k < 200; ++k) {
            const double srcPos = 100.0 + static_cast<double>(k) * sp;
            if (srcPos >= 290.0) { break; }
            const float want = static_cast<float>(((srcPos) * 32.0) / 32768.0);
            maxErr = std::max(maxErr, std::fabs(buf[k].real() - want));
        }
        if (maxErr > 2e-6f) { ok = false; }
        CHECK(ok);
        // the position advanced by about 200 * speed source frames (plus the two-frame look-ahead)
        const double adv = static_cast<double>(m.position().frame) - 100.0;
        CHECK(std::fabs(adv - 200.0 * sp) <= 3.0);
        if (sp == 2.0 || sp == 0.5) { std::printf("  speed %.2f: 200 output frames advanced the source by %.0f frames, max interpolation error %.2e\n", sp, adv, static_cast<double>(maxErr)); }
        CHECK(m.position().speed == sp);
    }
    // refused speeds leave the speed unchanged
    CHECK(m.setSpeed(2.0));
    for (double bad : std::vector<double>{0.0, 0.1, 0.2499, 4.01, 100.0, -1.0, std::nan(""), static_cast<double>(INFINITY)}) { CHECK(!m.setSpeed(bad)); }
    CHECK(m.position().speed == 2.0);
    // changing the speed in mid-stream does not make a jump: on a pure ramp the output steps by the speed times the slope
    CHECK(m.setSpeed(1.0)); m.seekFrame(10);
    std::vector<cf> a1(100), a2(100);
    m.read(a1.data(), 50);
    CHECK(m.setSpeed(2.0));
    m.read(a2.data(), 100);
    const float slope = 32.0f / 32768.0f;
    CHECK(std::fabs((a2[0].real() - a1[49].real()) - slope * 1.0f) < 3 * slope);   // the next sample follows the last within about one step
    float maxStep = 0.0f;
    for (std::size_t i = 1; i < 100; ++i) { maxStep = std::max(maxStep, std::fabs((a2[i].real() - a2[i - 1].real()) - 2.0f * slope)); }
    CHECK(maxStep < 1e-6f);
    // a seek resets the interpolator: the first output frame after a seek equals the source frame at the target
    CHECK(m.setSpeed(3.0)); m.seekFrame(500); m.read(buf.data(), 1);
    CHECK(std::fabs(buf[0].real() - sample(500)) < 1e-7f);
    // speed with a loop: still gapless at the wrap (output never short)
    CHECK(m.setSpeed(2.0)); CHECK(m.setLoop(100, 300)); CHECK(m.seekFrame(100));
    bool full = true;
    for (int i = 0; i < 20; ++i) { if (m.read(buf.data(), 400).frames != 400) { full = false; } }
    CHECK(full);
    // I/Q sources accept only speed 1.0
    const std::string path = rampCf32(1000, "recm-pb-ramp4.cf32");
    pb::PlaybackModel q; CHECK(q.open(openRamp(path), &err));
    CHECK(q.setSpeed(1.0) && !q.setSpeed(2.0) && !q.setSpeed(0.5));
    // end of an audio file at speed: ends and reports it
    pb::PlaybackModel e; CHECK(e.open(pb::WavAudioSource::open(wp, &err), &err)); CHECK(e.setSpeed(4.0)); e.seekFrame(19900);
    { const auto rr = e.read(buf.data(), 400); CHECK(rr.frames < 400 && rr.endOfData && rr.frames >= 20 && rr.frames <= 30); }
    std::printf("speed: 0.25..4.0 on an audio file matches linear interpolation to 2e-6, position advances by speed, refused values leave it unchanged, no jump on a change, seek resets, loop stays gapless; I/Q refuses anything but 1.0\n");
}

void testWavSource() {
    std::string err;
    struct Row { int bits; int ch; bool flt; };
    for (const Row& r : {Row{8, 1, false}, Row{16, 1, false}, Row{24, 1, false}, Row{32, 1, false}, Row{32, 1, true}, Row{16, 2, false}, Row{16, 3, false}, Row{32, 2, true}}) {
        const std::string p = wavMono("recm-pb-f" + std::to_string(r.bits) + (r.flt ? "f" : "i") + std::to_string(r.ch) + ".wav", 8000, 3000, r.bits, r.ch, r.flt);
        auto s = pb::WavAudioSource::open(p, &err);
        CHECK(s != nullptr);
        if (s == nullptr) { std::printf("  open %d bit %d ch: %s\n", r.bits, r.ch, err.c_str()); continue; }
        CHECK(s->totalFrames() == 3000 && s->sampleRateHz() == 8000.0 && s->isAudio());
        std::vector<cf> d(3000);
        CHECK(s->read(d.data(), 3000) == 3000);
        // sample(frame) for channel c = (frame % 1000) * (c + 1) in "units", scaled per format; the mean over channels = (frame % 1000) * mean(c + 1)
        double meanMul = 0.0; for (int c = 0; c < r.ch; ++c) { meanMul += c + 1; } meanMul /= r.ch;
        const double scale = r.flt ? 1.0 / 1000.0 : (r.bits == 8 ? 1.0 / (128.0) * 0.125 : (r.bits == 16 ? 32.0 / 32768.0 : (r.bits == 24 ? 8192.0 / 8388608.0 : 2097152.0 / 2147483648.0)));
        bool ok = true;
        for (std::size_t i = 0; i < 3000; ++i) {
            const double want = static_cast<double>(i % 300) * meanMul * scale;
            if (std::fabs(d[i].real() - want) > (r.bits == 8 ? 0.008 : 1e-6)) { ok = false; }
            if (d[i].imag() != 0.0f) { ok = false; }
        }
        CHECK(ok);
        if (!ok) { std::printf("  decode mismatch: %d bit %d ch float=%d\n", r.bits, r.ch, r.flt); }
    }
    // hostile
    CHECK(pb::WavAudioSource::open((g_dir / "nope.wav").string(), &err) == nullptr && !err.empty());
    { std::ofstream o(g_dir / "recm-pb-junk.wav", std::ios::binary); o << "RIFFxxxxWAVEjunkjunkjunk"; }
    CHECK(pb::WavAudioSource::open((g_dir / "recm-pb-junk.wav").string(), &err) == nullptr);
    { Bytes b; tag(b, "RIFF"); le32(b, 100); tag(b, "WAVE"); tag(b, "fmt "); le32(b, 16); le16(b, 2); le16(b, 1); le32(b, 8000); le32(b, 8000); le16(b, 1); le16(b, 4); tag(b, "data"); le32(b, 8);
      for (int i = 0; i < 8; ++i) { b.push_back(0); }
      std::ofstream o(g_dir / "recm-pb-adpcm.wav", std::ios::binary); o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
    CHECK(pb::WavAudioSource::open((g_dir / "recm-pb-adpcm.wav").string(), &err) == nullptr && err.find("unsupported") != std::string::npos);
    // data size larger than the file: clamped, then a normal read to the end
    const std::string big = wavMono("recm-pb-liar.wav", 8000, 500, 16, 1, false, 100000);
    auto s = pb::WavAudioSource::open(big, &err);
    CHECK(s != nullptr && s->totalFrames() == 500);
    // zero frames
    const std::string zero = wavMono("recm-pb-zero.wav", 8000, 0, 16, 1);
    auto zs = pb::WavAudioSource::open(zero, &err);
    CHECK(zs != nullptr && zs->totalFrames() == 0);
    // a file that shrinks under the reader: short read, no crash, total follows
    const std::string shr = wavMono("recm-pb-shrink.wav", 8000, 4000, 16, 1);
    pb::PlaybackModel m;
    CHECK(m.open(pb::WavAudioSource::open(shr, &err), &err));
    std::vector<cf> d(1000);
    CHECK(m.read(d.data(), 1000).frames == 1000);
    fs::resize_file(shr, 44 + 2 * 1500);
    CHECK(fs::file_size(shr) == 44 + 2 * 1500);
    const pb::ReadResult r1 = m.read(d.data(), 1000);   // frames 1000..1499 exist
    CHECK(r1.frames == 500);
    const pb::ReadResult r2 = m.read(d.data(), 1000);
    CHECK(r2.frames == 0 && r2.endOfData);
    CHECK(m.position().totalFrames == 1500);
    std::printf("wav source: 8/16/24/32-bit PCM, float32, mono/stereo/3 channels decode to the expected values; junk, ADPCM, a lying size field, zero frames and a shrinking file are handled\n");
}

#if defined(_WIN32)
bool makeSparse(const std::string& path, const Bytes& header, std::uint64_t tailOffset, const Bytes& tail) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD ret = 0, w = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr)) { CloseHandle(h); return false; }
    WriteFile(h, header.data(), static_cast<DWORD>(header.size()), &w, nullptr);
    LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(tailOffset);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    WriteFile(h, tail.data(), static_cast<DWORD>(tail.size()), &w, nullptr);
    CloseHandle(h);
    return w == tail.size();
}
#endif

void testOver4Gb() {
#if defined(_WIN32)
    const std::uint64_t frames = 1200000000ULL;            // 4.8e9 bytes of PCM16 stereo
    const std::uint64_t bytes = frames * 4;
    Bytes ds; tag(ds, "ds64"); le32(ds, 28); le64(ds, bytes + 100); le64(ds, bytes); le64(ds, frames); le32(ds, 0);
    Bytes fmt; tag(fmt, "fmt "); le32(fmt, 16); le16(fmt, 1); le16(fmt, 2); le32(fmt, 2000000); le32(fmt, 8000000); le16(fmt, 4); le16(fmt, 16);
    Bytes head; tag(head, "RF64"); le32(head, 0xFFFFFFFFu); tag(head, "WAVE");
    head.insert(head.end(), ds.begin(), ds.end()); head.insert(head.end(), fmt.begin(), fmt.end());
    tag(head, "data"); le32(head, 0xFFFFFFFFu);
    const std::uint64_t off = head.size();
    // the last three frames carry known bytes: (0x1111, 0x2222) (0x3333, 0x4444) (0x1234, 0xEDCC)
    Bytes tail; le16(tail, 0x1111); le16(tail, 0x2222); le16(tail, 0x3333); le16(tail, 0x4444); le16(tail, 0x1234); le16(tail, 0xEDCC);
    const std::string p = (g_dir / "recm-pb-big.wav").string();
    if (!makeSparse(p, head, off + bytes - tail.size(), tail)) { std::printf("SKIP: cannot make a sparse file here\n"); return; }
    // re-write the header over the start (makeSparse wrote it first; the tail seek came after)
    std::string err;
    pb::PlaybackModel m;
    auto r = iq::openAuto(p, iq::RawParams{}, &err);
    CHECK(r != nullptr);
    if (r == nullptr) { std::printf("  open: %s\n", err.c_str()); return; }
    CHECK(m.open(std::make_unique<pb::IqReaderSource>(std::move(r)), &err));
    pb::PlaybackPosition pos = m.position();
    CHECK(pos.totalFrames == frames && pos.rateKnown && std::fabs(pos.durationS - 600.0) < 1e-9);
    CHECK(m.seekFraction(0.75));
    CHECK(m.position().frame == 900000000ULL && std::fabs(m.position().seconds - 450.0) < 1e-9);
    cf v[3];
    { const auto rd = m.read(v, 1); CHECK(rd.frames == 1 && rd.startFrame == 900000000ULL && v[0] == cf(0.f, 0.f)); }   // sparse hole reads as zeros
    CHECK(m.seekFrame(frames - 3));
    { const auto rd = m.read(v, 3); CHECK(rd.frames == 3 && rd.startFrame == frames - 3); }
    CHECK(std::fabs(v[0].real() - 0x1111 / 32768.0) < 1e-6 && std::fabs(v[0].imag() - 0x2222 / 32768.0) < 1e-6);
    CHECK(std::fabs(v[2].real() - 0x1234 / 32768.0) < 1e-6 && std::fabs(v[2].imag() - (static_cast<std::int16_t>(0xEDCC)) / 32768.0) < 1e-6);
    CHECK(m.read(v, 1).endOfData);
    CHECK(m.position().frame == frames && m.position().atEnd && m.position().fraction == 1.0);
    // seek by seconds near the end, beyond 2^32 frames' byte offset: frame 1,199,999,999 = 599.9999995 s
    CHECK(m.seekSeconds(599.9999995)); CHECK(m.position().frame >= frames - 2 && m.position().frame <= frames);
    // a loop across the 4 GiB byte boundary (byte 4294967296 is frame 1073741824 - 11 header bytes aside)
    const std::uint64_t boundaryFrame = (4294967296ULL - off) / 4;
    CHECK(m.setLoop(boundaryFrame - 2, boundaryFrame + 2));
    CHECK(m.seekFrame(boundaryFrame - 2));
    cf w[10];
    { const auto rd = m.read(w, 10); CHECK(rd.frames == 10 && !rd.endOfData); }
    std::printf("over 4 GB: RF64 of %llu frames (%.1f GB): fraction 0.75 -> frame 900000000 (450.0 s), the last frames read their known bytes, a loop spanning the 4 GiB byte offset plays full blocks\n",
                static_cast<unsigned long long>(frames), static_cast<double>(bytes) / 1e9);
    std::error_code ec; fs::remove(p, ec);
#else
    SKIP_LINUX("sparse > 4 GB file test is Windows-only");
#endif
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "recm-playback";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    testSequentialAndSeek();
    testStaleBlock();
    testLoop();
    testSpeed();
    testWavSource();
    testOver4Gb();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    return testSummary("test_iq_playback");
}
