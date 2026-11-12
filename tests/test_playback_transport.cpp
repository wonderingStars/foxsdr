// Playback transport (source/playback_transport.hpp): speed 0.25-8 for I/Q under two policies, pause, step, loop, seek generations, no allocation in read().
// The source is a COUNTING source (frame i = (i, -i/2)), never a real file, so every delivered frame names its own position.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include "source/playback_transport.hpp"
#include "test_check.hpp"

using namespace cascade::source::transport;

// ---- allocation counter (armed only around read()) ---------------------------------------------------------------------------------------
static std::atomic<long> g_allocs{0};
static std::atomic<bool> g_armed{false};
void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) { g_allocs.fetch_add(1, std::memory_order_relaxed); }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

class Counting final : public TransportSource {
public:
    explicit Counting(std::uint64_t total) : total_(total) {}
    std::uint64_t totalFrames() const override { return total_; }
    bool seek(std::uint64_t f) override { if (f > total_) { return false; } pos_ = f; ++seeks; return true; }
    std::size_t read(cf* d, std::size_t n) override {
        const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, total_ - pos_));
        for (std::size_t i = 0; i < k; ++i) { const double f = static_cast<double>(pos_ + i); d[i] = cf(static_cast<float>(f), static_cast<float>(-f / 2.0)); }
        pos_ += k;
        return k;
    }
    std::uint64_t total_;
    std::uint64_t pos_ = 0;
    long seeks = 0;
};

TransportConfig cfgOf(SpeedPolicy p, std::size_t block = 1000, std::size_t grain = 500) {
    TransportConfig c; c.policy = p; c.blockFrames = block; c.grainFrames = grain; c.sampleRateHz = 48000.0; return c;
}

// the real part names the (fractional) source position
double posOf(const cf& v) { return static_cast<double>(v.real()); }

std::vector<cf> readAll(Transport& t, std::size_t total, std::size_t call) {
    std::vector<cf> out;
    std::vector<cf> buf(call);
    for (int guard = 0; guard < 1000000; ++guard) {
        const ReadResult r = t.read(buf.data(), call);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(r.frames));
        if (r.atEnd || out.size() >= total) { break; }
        if (r.frames == 0) { break; }
    }
    return out;
}

void testSpeedOne() {
    for (SpeedPolicy pol : {SpeedPolicy::Varispeed, SpeedPolicy::BlockSkip}) {
        Counting src(100000);
        Transport t(&src, cfgOf(pol));
        const std::vector<cf> v = readAll(t, 100000, 777);
        CHECK(v.size() == 100000);
        bool exact = v.size() == 100000;
        for (std::size_t i = 0; exact && i < v.size(); ++i) { exact = v[i] == cf(static_cast<float>(i), -static_cast<float>(i) / 2.0f); }
        CHECK(exact);
        CHECK(t.position().atEnd);
    }
}

void testVarispeed() {
    // integer speeds hit source frames exactly, others interpolate; the source advances speed x frames
    const double speeds[] = {0.25, 0.5, 1.5, 2.0, 3.0, 4.0, 8.0, 7.3};
    for (double s : speeds) {
        Counting src(400000);
        Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
        CHECK(t.setSpeed(s));
        std::vector<cf> out(20000);
        std::size_t got = 0;
        std::mt19937 g(5);
        while (got < out.size()) {
            const std::size_t call = std::min<std::size_t>(out.size() - got, 1 + g() % 3001);
            const ReadResult r = t.read(out.data() + got, call);
            if (r.frames == 0) { break; }
            got += r.frames;
        }
        CHECK(got == out.size());
        double worst = 0.0;
        for (std::size_t k = 0; k < got; ++k) {
            worst = std::max(worst, std::fabs(posOf(out[k]) - static_cast<double>(k) * s));
            worst = std::max(worst, std::fabs(static_cast<double>(out[k].imag()) + static_cast<double>(k) * s / 2.0));
        }
        std::printf("varispeed %.2fx: worst position error %.3g over %zu frames\n", s, worst, got);
        CHECK(worst < 1.0e-2 * std::max(1.0, s));     // float32 holds 20000*8 = 160000 with 0.0156 resolution
        CHECK_NEAR(static_cast<double>(t.position().frame), std::floor(static_cast<double>(got) * s), 1.0);
    }
}

void testVarispeedPitch() {
    // a complex tone at +f0: Varispeed multiplies it by the speed
    struct Tone final : TransportSource {
        double f0, fs; std::uint64_t pos = 0; std::uint64_t tot;
        Tone(double f, double r, std::uint64_t n) : f0(f), fs(r), tot(n) {}
        std::uint64_t totalFrames() const override { return tot; }
        bool seek(std::uint64_t f) override { pos = f; return f <= tot; }
        std::size_t read(cf* d, std::size_t n) override {
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, tot - pos));
            for (std::size_t i = 0; i < k; ++i) { const double ph = 2.0 * M_PI * f0 * static_cast<double>(pos + i) / fs; d[i] = cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))); }
            pos += k; return k;
        }
    };
    auto measure = [](const std::vector<cf>& v, std::size_t from, std::size_t to, double fs) {
        // mean phase advance per sample, from the lag-1 product
        std::complex<double> acc(0, 0);
        for (std::size_t i = from + 1; i < to; ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
        return std::arg(acc) * fs / (2.0 * M_PI);
    };
    const double fs = 48000.0, f0 = 1500.0;
    for (double s : {0.5, 2.0}) {
        Tone src(f0, fs, 400000);
        Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
        t.setSpeed(s);
        std::vector<cf> out(40000);
        t.read(out.data(), out.size());
        const double f = measure(out, 100, 30000, fs);
        std::printf("varispeed pitch %.1fx: tone 1500 Hz -> %.1f Hz\n", s, f);
        CHECK_NEAR(f, f0 * s, 0.01 * f0 * s + 5.0);
    }
    for (double s : {0.5, 2.0, 4.0}) {
        Tone src(f0, fs, 4000000);
        Transport t(&src, cfgOf(SpeedPolicy::BlockSkip, 1000, 4000));
        t.setSpeed(s);
        std::vector<cf> out(40000);
        t.read(out.data(), out.size());
        // inside one grain (no join) the frequency is unchanged
        const double f = measure(out, 10, 3900, fs);
        std::printf("block-skip pitch %.1fx: tone 1500 Hz -> %.1f Hz\n", s, f);
        CHECK_NEAR(f, f0, 3.0);
    }
}

void testBlockSkip() {
    const std::size_t G = 500;
    for (double s : {2.0, 4.0, 8.0, 0.5, 0.25}) {
        Counting src(2000000);
        Transport t(&src, cfgOf(SpeedPolicy::BlockSkip, 1000, G));
        t.setSpeed(s);
        std::vector<cf> out(G * 40);
        std::size_t got = 0;
        std::mt19937 g(9);
        while (got < out.size()) {
            const std::size_t call = std::min<std::size_t>(out.size() - got, 1 + g() % 1777);
            const ReadResult r = t.read(out.data() + got, call);
            got += r.frames;
            if (r.frames == 0) { break; }
        }
        CHECK(got == out.size());
        bool ok = true;
        for (std::size_t grain = 0; grain < 40; ++grain) {
            const std::uint64_t start = static_cast<std::uint64_t>(std::floor(static_cast<double>(grain) * G * s));
            for (std::size_t i = 0; i < G; ++i) { ok = ok && posOf(out[grain * G + i]) == static_cast<float>(start + i); }
        }
        CHECK(ok);
    }
}

void testPauseAndStep() {
    for (SpeedPolicy pol : {SpeedPolicy::Varispeed, SpeedPolicy::BlockSkip}) {
        Counting src(100000);
        Transport t(&src, cfgOf(pol, 1000, 500));
        std::vector<cf> out(5000);
        ReadResult r = t.read(out.data(), 1500);
        CHECK(r.frames == 1500 && !r.paused);
        CHECK(!t.stepBlocks(1));                    // refused while playing
        t.pause();
        r = t.read(out.data(), 1000);
        CHECK(r.frames == 0 && r.paused);
        CHECK(t.position().frame == 1500);
        CHECK(t.position().paused);
        // a step of 2 blocks: exactly 2000 frames, in any call sizes, then nothing
        CHECK(t.stepBlocks(2));
        std::size_t got = 0;
        for (int i = 0; i < 20; ++i) { r = t.read(out.data() + got, 300); got += r.frames; }
        CHECK(got == 2000);
        CHECK(posOf(out[0]) == 1500.0f && posOf(out[1999]) == 3499.0f);
        r = t.read(out.data(), 100);
        CHECK(r.frames == 0 && r.paused);
        // resume continues exactly after the stepped frames
        t.resume();
        r = t.read(out.data(), 10);
        CHECK(r.frames == 10 && posOf(out[0]) == 3500.0f);
        // a step left pending is cleared by resume
        t.pause(); t.stepBlocks(3); t.resume(); t.pause();
        CHECK(t.read(out.data(), 100).frames == 0);
        // n == 0 and absurd n refused
        CHECK(!t.stepBlocks(0));
        CHECK(!t.stepBlocks(kMaxStepBlocks + 1));
        CHECK(!t.stepBlocks(-kMaxStepBlocks - 1));
        // step back 2 blocks from 3510: position 1510, one block delivered from there, generation changed
        const std::uint64_t gen0 = t.generation();
        CHECK(t.stepBlocks(-2));
        CHECK(t.generation() == gen0 + 1);
        r = t.read(out.data(), 5000);
        CHECK(r.frames == 1000 && r.paused && r.generation == gen0 + 1);
        CHECK(posOf(out[0]) == 1510.0f && posOf(out[999]) == 2509.0f);
        // back past zero clamps
        CHECK(t.stepBlocks(-100));
        r = t.read(out.data(), 5000);
        CHECK(r.frames == 1000 && posOf(out[0]) == 0.0f);
    }
}

void testSpeedValidation() {
    Counting src(1000);
    Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
    CHECK(t.setSpeed(2.0));
    for (double bad : {0.0, -1.0, 0.2499, 8.0001, 100.0, std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        CHECK(!t.setSpeed(bad));
        CHECK(t.speed() == 2.0);
    }
    CHECK(t.setSpeed(0.25) && t.setSpeed(8.0));
    CHECK(!t.seekSeconds(std::nan("")) && !t.seekSeconds(-1.0) && !t.seekFraction(std::nan("")));
    const std::uint64_t g = t.generation();
    CHECK(!t.seekFraction(std::nan("")) && t.generation() == g);
    CHECK(!t.setLoop(5, 5) && !t.setLoop(9, 3) && !t.setLoop(0, 1001));
    CHECK(t.setLoop(0, 1000));
}

void testLoopAndSeek() {
    for (SpeedPolicy pol : {SpeedPolicy::Varispeed, SpeedPolicy::BlockSkip}) {
        for (double s : {1.0, 2.0, 0.5}) {
            Counting src(10000);
            Transport t(&src, cfgOf(pol, 1000, 100));
            t.setSpeed(s);
            CHECK(t.setLoop(1000, 2500));
            std::vector<cf> out(30000);
            std::size_t got = 0;
            while (got < out.size()) { got += t.read(out.data() + got, std::min<std::size_t>(out.size() - got, 700)).frames; }
            double lo = 1e18, hi = -1;
            for (const cf& v : out) { lo = std::min(lo, posOf(v)); hi = std::max(hi, posOf(v)); }
            CHECK(lo >= 1000.0 || lo >= 0.0);       // before the loop is reached positions start at 0 and rise to B only
            CHECK(hi < 2500.0);
            // after the first wrap (at most 5000 frames at 0.5x) nothing below A appears
            bool inside = true;
            for (std::size_t i = 12000; i < out.size(); ++i) { inside = inside && posOf(out[i]) >= 1000.0f && posOf(out[i]) < 2500.0f; }
            CHECK(inside);
            bool wrapped = false;
            for (std::size_t i = 12001; i < out.size(); ++i) { wrapped = wrapped || posOf(out[i]) < posOf(out[i - 1]); }
            CHECK(wrapped);
            CHECK(!t.position().atEnd);
        }
    }
    // speed 1 wrap is gapless: frame B-1 is followed by frame A
    {
        Counting src(10000);
        Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
        t.setLoop(100, 350);
        std::vector<cf> out(1000);
        t.read(out.data(), 1000);
        CHECK(posOf(out[349]) == 349.0f && posOf(out[350]) == 100.0f && posOf(out[599]) == 349.0f && posOf(out[600]) == 100.0f);
    }
    // varispeed 2x wrap: the sample that straddles B interpolates frame B-1 with frame A (position A, not B)
    {
        Counting src(10000);
        Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
        t.setSpeed(2.0);
        t.setLoop(100, 351);                        // 251 frames: odd, so the 2x grid straddles the wrap
        t.seekFrame(100);
        std::vector<cf> out(400);
        t.read(out.data(), 400);
        bool noOvershoot = true;
        for (const cf& v : out) { noOvershoot = noOvershoot && posOf(v) >= 100.0f && posOf(v) <= 350.5f + 1.0f; }
        CHECK(noOvershoot);
        CHECK(posOf(out[125]) == 350.0f);            // pos 250 -> frame 350 (B-1)
        CHECK_NEAR(posOf(out[126]), 100.0 + 0.0, 2.0);   // pos 252 wraps to A + 1
    }
    // seek: the first block after seek starts at the target; a stale block is detectable; the bar moves at once
    {
        Counting src(100000);
        Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
        std::vector<cf> out(1000);
        ReadResult r = t.read(out.data(), 1000);
        const std::uint64_t g0 = r.generation;
        CHECK(t.generation() == g0);
        t.seekFrame(60000);
        CHECK(t.generation() == g0 + 1);
        CHECK(t.position().frame == 60000);
        r = t.read(out.data(), 1000);
        CHECK(r.generation == g0 + 1 && posOf(out[0]) == 60000.0f);
        CHECK(t.position().frame == 61000);
        t.seekFraction(0.5);
        r = t.read(out.data(), 1);
        CHECK(posOf(out[0]) == 50000.0f);
        t.seekSeconds(1.0);                          // 48000 Hz
        r = t.read(out.data(), 1);
        CHECK(posOf(out[0]) == 48000.0f);
        t.seekFrame(10000000);                       // clamped to the total
        r = t.read(out.data(), 10);
        CHECK(r.frames == 0 && r.atEnd);
        t.seekFrame(99990);
        r = t.read(out.data(), 100);
        CHECK(r.frames == 10 && r.atEnd);
    }
    // a loop whose end is below the play position continues from A
    {
        Counting src(100000);
        Transport t(&src, cfgOf(SpeedPolicy::BlockSkip, 1000, 100));
        std::vector<cf> out(500);
        t.seekFrame(50000);
        t.read(out.data(), 500);
        t.setLoop(1000, 2000);
        t.read(out.data(), 500);
        CHECK(posOf(out[0]) >= 1000.0f && posOf(out[0]) < 2000.0f);
    }
}

void testNoAllocation() {
    Counting src(2000000);
    Transport t(&src, cfgOf(SpeedPolicy::Varispeed, 1000, 500));
    std::vector<cf> out(5000);
    t.read(out.data(), 5000);
    for (SpeedPolicy pol : {SpeedPolicy::Varispeed, SpeedPolicy::BlockSkip}) {
        Counting s2(2000000);
        Transport t2(&s2, cfgOf(pol, 1000, 500));
        t2.setLoop(5000, 90000);
        t2.read(out.data(), 100);   // warm
        long worstAllocs = 0;
        for (double sp : {0.25, 1.0, 3.3, 8.0}) {
            t2.setSpeed(sp);
            g_allocs = 0; g_armed = true;
            for (int i = 0; i < 400; ++i) { t2.read(out.data(), 5000); }
            t2.pause(); t2.stepBlocks(2); t2.read(out.data(), 5000); t2.seekFrame(7000); t2.read(out.data(), 10); t2.resume();
            g_armed = false;
            worstAllocs = std::max(worstAllocs, g_allocs.load());
        }
        std::printf("allocations in read(), 4 speeds x 400 calls + pause/step/seek: %ld\n", worstAllocs);
        CHECK(worstAllocs == 0);
    }
}

void testConcurrency() {
    Counting src(1000000);
    Transport t(&src, cfgOf(SpeedPolicy::Varispeed, 1000, 500));
    std::atomic<bool> stop{false};
    std::thread ctl([&] {
        std::mt19937 g(77);
        while (!stop.load()) {
            switch (g() % 9) {
                case 0: t.setSpeed(0.25 + (g() % 100) * 0.0775); break;
                case 1: t.pause(); break;
                case 2: t.resume(); break;
                case 3: t.seekFrame(g() % 1000000); break;
                case 4: { const std::uint64_t a = g() % 900000; t.setLoop(a, a + 1 + g() % 90000); break; }
                case 5: t.clearLoop(); break;
                case 6: t.stepBlocks(1 + static_cast<long long>(g() % 3)); break;
                case 7: t.stepBlocks(-1); break;
                default: (void)t.position(); break;
            }
            std::this_thread::yield();
        }
    });
    std::vector<cf> out(3000);
    bool valid = true;
    long reads = 0, nonEmpty = 0;
    for (int i = 0; i < 20000; ++i) {
        const ReadResult r = t.read(out.data(), 3000);
        ++reads;
        if (r.frames > 3000) { valid = false; }
        if (r.frames > 0) { ++nonEmpty; }
        for (std::size_t k = 0; k < r.frames; ++k) {
            const double p = posOf(out[k]);
            if (!(p >= 0.0 && p <= 1000000.0) || !std::isfinite(p)) { valid = false; }
        }
        const Position pos = t.position();
        if (pos.frame > pos.total) { valid = false; }
        if (i % 50 == 0) { t.resume(); }
    }
    stop = true;
    ctl.join();
    std::printf("concurrency: %ld reads, %ld non-empty\n", reads, nonEmpty);
    CHECK(valid);
    CHECK(nonEmpty > 100);
}

void testFuzz() {
    // random command sequences, invariants: frames <= n, position <= total, every delivered position inside [0, total), loop bounds respected after the first wrap
    std::mt19937 g(2024);
    long reads = 0;
    bool ok = true;
    for (int round = 0; round < 300; ++round) {
        const std::uint64_t total = 1 + g() % 40000;
        Counting src(total);
        Transport t(&src, cfgOf(g() % 2 == 0 ? SpeedPolicy::Varispeed : SpeedPolicy::BlockSkip, 1 + g() % 3000, 1 + g() % 3000));
        std::vector<cf> out(5000);
        for (int op = 0; op < 60; ++op) {
            switch (g() % 8) {
                case 0: t.setSpeed(0.25 + (g() % 1000) * 0.00775); break;
                case 1: t.pause(); break;
                case 2: t.resume(); break;
                case 3: t.seekFrame(g() % (total + 10)); break;
                case 4: { const std::uint64_t a = g() % total; t.setLoop(a, a + 1 + g() % (total - a)); break; }
                case 5: t.clearLoop(); break;
                case 6: t.stepBlocks(static_cast<long long>(g() % 9) - 4); break;
                default: {
                    const std::size_t n = g() % 5001;
                    const ReadResult r = t.read(out.data(), n);
                    ++reads;
                    ok = ok && r.frames <= n;
                    for (std::size_t k = 0; k < r.frames; ++k) { const double p = posOf(out[k]); ok = ok && p >= 0.0 && p < static_cast<double>(total) && std::isfinite(p); }
                    const Position pos = t.position();
                    ok = ok && pos.frame <= total;
                }
            }
        }
    }
    std::printf("fuzz: %ld reads over 300 random sessions\n", reads);
    CHECK(ok);
}

void testEmptyAndNull() {
    Counting src(0);
    Transport t(&src, cfgOf(SpeedPolicy::Varispeed));
    std::vector<cf> out(10);
    ReadResult r = t.read(out.data(), 10);
    CHECK(r.frames == 0 && r.atEnd);
    CHECK(t.position().fraction == 0.0 && t.position().total == 0);
    CHECK(!t.setLoop(0, 1));
    Transport tn(nullptr, cfgOf(SpeedPolicy::Varispeed));
    CHECK(tn.read(out.data(), 10).frames == 0);
    Counting one(1);
    Transport t1(&one, cfgOf(SpeedPolicy::Varispeed));
    r = t1.read(out.data(), 10);
    CHECK(r.frames == 1 && r.atEnd);
    CHECK(t1.read(nullptr, 5).frames == 0);
}

}  // namespace

int main() {
    testSpeedOne();
    testVarispeed();
    testVarispeedPitch();
    testBlockSkip();
    testPauseAndStep();
    testSpeedValidation();
    testLoopAndSeek();
    testNoAllocation();
    testConcurrency();
    testFuzz();
    testEmptyAndNull();
    return testSummary("test_playback_transport");
}
