// FM noise squelch (src/dsp/noise_squelch.hpp): open and close delay, false-open rate on noise, hysteresis, tail, cost.
// Signals are generated: discriminator-shaped noise (white noise differentiated: what an FM discriminator makes of no
// signal) and a carrier-present state (a 1 kHz tone with the noise 20 dB lower, what quieting does). No recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <limits>
#include <random>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/noise_squelch.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

using cascade::dsp::NoiseSquelch;

namespace {
constexpr double kFs = 48000.0;
constexpr double kSigma = 0.3;  // "no signal": discriminator noise standard deviation

// Streaming discriminator noise, any length: differentiated white noise scaled to `sigma`.
class DiscNoise {
public:
    DiscNoise(double sigma, std::uint32_t seed) : g_(seed), d_(0.0, 1.0), sigma_(sigma / std::sqrt(2.0)) { prev_ = d_(g_); }
    float next() {
        const double w = d_(g_);
        const double v = (w - prev_) * sigma_;
        prev_ = w;
        return static_cast<float>(v);
    }
    void fill(std::vector<float>& v, std::size_t from, std::size_t to, double scale = 1.0) {
        for (std::size_t i = from; i < to; ++i) { v[i] = static_cast<float>(next() * scale); }
    }
private:
    std::mt19937 g_;
    std::normal_distribution<double> d_;
    double sigma_, prev_;
};

// Band level of noise-only input (mean over 10 s): the reference the thresholds are set against.
double noiseOnlyLevelDb() {
    NoiseSquelch s(kFs);
    s.setThresholdDb(-300.0);  // never opens: only the level matters
    DiscNoise n(kSigma, 1);
    std::vector<float> in(4800), a(4800);
    double sum = 0;
    int cnt = 0;
    for (int k = 0; k < 100; ++k) {
        n.fill(in, 0, in.size());
        a = in;
        s.process(in.data(), in.size(), a.data());
        if (k >= 2) { sum += std::pow(10.0, s.levelDb() / 10.0); ++cnt; }
    }
    return 10.0 * std::log10(sum / cnt);
}

struct Scenario {
    std::vector<float> det;
    std::size_t t0, t1;  // carrier present in [t0, t1)
};

// 1 s noise, carrier from t0 for `carrierS`, then noise again for 1 s. Carrier state: 1 kHz tone 0.3 and noise 20 dB lower.
Scenario makeScenario(std::uint32_t seed, double carrierS) {
    Scenario sc;
    sc.t0 = static_cast<std::size_t>(1.0 * kFs);
    sc.t1 = sc.t0 + static_cast<std::size_t>(carrierS * kFs);
    const std::size_t total = sc.t1 + static_cast<std::size_t>(1.0 * kFs);
    sc.det.assign(total, 0.0f);
    DiscNoise n(kSigma, seed);
    n.fill(sc.det, 0, sc.t0);
    n.fill(sc.det, sc.t0, sc.t1, 0.1);
    for (std::size_t i = sc.t0; i < sc.t1; ++i) {
        sc.det[i] += static_cast<float>(0.3 * std::sin(blk4::kTwoPi * 1000.0 * static_cast<double>(i) / kFs));
    }
    n.fill(sc.det, sc.t1, total);
    return sc;
}

// Runs the scenario sample by sample on audio = 1.0 and returns the open delay (first sample at/after t0 where the gain
// reaches 0.5) and close delay (first sample at/after t1 where it falls to 0.5), in ms; -1 when it never happens.
void delays(NoiseSquelch& sq, const Scenario& sc, double& openMs, double& closeMs, bool& openedBeforeT0) {
    openMs = closeMs = -1;
    openedBeforeT0 = false;
    for (std::size_t i = 0; i < sc.det.size(); ++i) {
        float a = 1.0f;
        sq.process(&sc.det[i], 1, &a);
        if (i < sc.t0 && a > 0.0f) { openedBeforeT0 = true; }
        if (i >= sc.t0 && i < sc.t1 && openMs < 0 && a >= 0.5f) { openMs = 1000.0 * static_cast<double>(i - sc.t0) / kFs; }
        if (i >= sc.t1 && closeMs < 0 && a <= 0.5f && openMs >= 0) { closeMs = 1000.0 * static_cast<double>(i - sc.t1) / kFs; }
    }
}

void testDelays() {
    const double ref = noiseOnlyLevelDb();
    std::printf("MEAS noise-only band level (5 kHz and up, discriminator noise sigma %.2f): %.2f dB re full scale\n", kSigma, ref);
    double minO = 1e9, maxO = 0, minC = 1e9, maxC = 0;
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        NoiseSquelch sq(kFs);
        sq.setThresholdDb(NoiseSquelch::thresholdForQuietingDb(ref, 10.0));
        const Scenario sc = makeScenario(seed, 1.5);
        double o, c;
        bool early;
        delays(sq, sc, o, c, early);
        CHECK(!early);
        CHECK(o > 0 && c > 0);
        minO = std::min(minO, o); maxO = std::max(maxO, o);
        minC = std::min(minC, c); maxC = std::max(maxC, c);
    }
    std::printf("MEAS 10 dB quieting, defaults (open delay 20 ms, tail 100 ms), 20 noise seeds: open delay %.1f to %.1f ms, close delay %.1f to %.1f ms\n",
                minO, maxO, minC, maxC);
    CHECK(minO > 25.0 && maxO < 50.0);
    CHECK(minC > 95.0 && maxC < 115.0);
    // Timing settings do what they say: tail 0 and open delay 0.
    NoiseSquelch fast(kFs);
    fast.setThresholdDb(NoiseSquelch::thresholdForQuietingDb(ref, 10.0));
    fast.setTailMs(0.0);
    fast.setOpenDelayMs(0.0);
    double o, c;
    bool early;
    delays(fast, makeScenario(3, 1.5), o, c, early);
    std::printf("MEAS tail 0 ms, open delay 0 ms: open %.1f ms, close %.1f ms\n", o, c);
    CHECK(o > 5.0 && o < 25.0);
    CHECK(c > 0.0 && c < 12.0);
    NoiseSquelch slow(kFs);
    slow.setThresholdDb(NoiseSquelch::thresholdForQuietingDb(ref, 10.0));
    slow.setTailMs(500.0);
    delays(slow, makeScenario(3, 1.5), o, c, early);
    std::printf("MEAS tail 500 ms: close %.1f ms\n", c);
    CHECK(c > 495.0 && c < 520.0);
}

long countOpens(double quietingDb, double openDelayMs, double seconds, std::uint32_t seed, double ref) {
    NoiseSquelch s(kFs);
    s.setThresholdDb(NoiseSquelch::thresholdForQuietingDb(ref, quietingDb));
    s.setOpenDelayMs(openDelayMs);
    DiscNoise n(kSigma, seed);
    std::vector<float> in(4800), a(4800);
    long opens = 0;
    bool was = false;
    const long chunks = static_cast<long>(seconds * 10.0);
    for (long k = 0; k < chunks; ++k) {
        n.fill(in, 0, in.size());
        a = in;
        for (std::size_t i = 0; i < in.size(); i += 16) {
            s.process(in.data() + i, 16, a.data() + i);
            const bool now = s.isOpen();
            if (now && !was) { ++opens; }
            was = now;
        }
    }
    return opens;
}

void testFalseOpens() {
    const double ref = noiseOnlyLevelDb();
    const double secs = 3600.0;
    const long at10 = countOpens(10.0, 20.0, secs, 11, ref);
    const long at6 = countOpens(6.0, 20.0, secs, 12, ref);
    const long at3 = countOpens(3.0, 20.0, secs, 13, ref);
    const long at3nodelay = countOpens(3.0, 0.0, 600.0, 14, ref) * 6;
    std::printf("MEAS false opens on one hour of noise only (20 ms open delay): needs 10 dB quieting: %ld, 6 dB: %ld, 3 dB: %ld; 3 dB with NO open delay: about %ld per hour (600 s x 6)\n",
                at10, at6, at3, at3nodelay);
    CHECK(at10 == 0);
    CHECK(at6 <= 1);
    CHECK(at3nodelay > at3);  // the debounce is doing something
}

void testHysteresisAndTail() {
    const double ref = noiseOnlyLevelDb();
    const double thr = NoiseSquelch::thresholdForQuietingDb(ref, 10.0);
    // Noise at thr + 1.5 dB is inside the hysteresis band (3 dB wide): an open gate stays open, a closed one stays closed.
    const double midScale = std::pow(10.0, (thr + 1.5 - ref) / 20.0);
    for (int startOpen = 0; startOpen < 2; ++startOpen) {
        NoiseSquelch s(kFs);
        s.setThresholdDb(thr);
        DiscNoise n(kSigma, 21);
        std::vector<float> a(48000);
        if (startOpen) {  // carrier state first
            std::vector<float> q(48000);
            n.fill(q, 0, q.size(), 0.1);
            a = q;
            s.process(q.data(), q.size(), a.data());
            CHECK(s.isOpen());
        }
        bool changed = false;
        std::vector<float> in(480);
        for (int k = 0; k < 100; ++k) {  // 10 s
            n.fill(in, 0, in.size(), midScale);
            a.assign(in.begin(), in.end());
            s.process(in.data(), in.size(), a.data());
            if (s.isOpen() != (startOpen != 0)) { changed = true; }
        }
        std::printf("MEAS noise 1.5 dB above the open threshold for 10 s, started %s: state %s\n", startOpen ? "open" : "closed", changed ? "CHANGED" : "held");
        CHECK(!changed);
    }
    // Tail: noise bursts shorter than the tail do not close the gate, longer ones do.
    for (double burstMs : {60.0, 90.0, 160.0}) {
        NoiseSquelch s(kFs);
        s.setThresholdDb(thr);
        DiscNoise n(kSigma, 22);
        std::vector<float> det(static_cast<std::size_t>(5.0 * kFs));
        n.fill(det, 0, det.size(), 0.1);
        for (std::size_t i = 0; i < det.size(); ++i) {
            const double t = static_cast<double>(i) / kFs;
            if (t >= 2.0 && t < 2.0 + burstMs * 1e-3) { det[i] = static_cast<float>(det[i] * 10.0); }  // back to full noise
        }
        std::vector<float> a(det.size(), 1.0f);
        s.process(det.data(), det.size(), a.data());
        // look at the gain from 1.0 s (open) to 5 s: did it ever fall under 0.5?
        bool closed = false;
        for (std::size_t i = static_cast<std::size_t>(1.0 * kFs); i < a.size(); ++i) { if (a[i] < 0.5f) { closed = true; } }
        std::printf("MEAS %.0f ms noise burst with the 100 ms tail: gate %s\n", burstMs, closed ? "closed" : "stayed open");
        CHECK(closed == (burstMs > 100.0));
    }
}

void testSpeechStaysOpen() {
    // Speech-like audio limited to 3 kHz (a transmitter's audio filter), as FM deviation, with the carrier's own noise 20 dB down.
    const double ref = noiseOnlyLevelDb();
    std::vector<float> sp = sbgen::speechLike(static_cast<std::size_t>(20 * kFs), kFs, 0.4, 5);
    cascade::dsp::abq::State st[2];
    const auto lp0 = cascade::dsp::abq::lowpass(kFs, 3000.0, 0.5411961), lp1 = cascade::dsp::abq::lowpass(kFs, 3000.0, 1.3065630);
    DiscNoise n(kSigma, 23);
    for (auto& v : sp) {
        v = static_cast<float>(cascade::dsp::abq::run(lp1, st[1], cascade::dsp::abq::run(lp0, st[0], v)) + 0.1 * n.next());
    }
    NoiseSquelch s(kFs);
    s.setThresholdDb(NoiseSquelch::thresholdForQuietingDb(ref, 10.0));
    std::vector<float> a(sp.size(), 1.0f);
    s.process(sp.data(), sp.size(), a.data());
    std::size_t closed = 0;
    for (std::size_t i = static_cast<std::size_t>(0.5 * kFs); i < a.size(); ++i) { if (a[i] < 0.999f) { ++closed; } }
    const double frac = static_cast<double>(closed) / (static_cast<double>(a.size()) - 0.5 * kFs);
    std::printf("MEAS speech-like with carrier present, 19.5 s: gate below unity for %.4f of the time\n", frac);
    CHECK(frac < 0.001);
}

void testBasics() {
    // Starts closed: the first samples are silent.
    NoiseSquelch s(kFs);
    std::vector<float> in(480, 0.0f), a(480, 1.0f);
    s.process(in.data(), in.size(), a.data());
    CHECK(a[0] == 0.0f && !s.isOpen());
    // Disabled: opens over 5 ms, then a bit-exact no-op.
    NoiseSquelch d(kFs);
    d.setEnabled(false);
    std::vector<float> x = blk4::tone(2400, kFs, 700.0, 0.3), y = x, noise(2400, 0.5f);
    d.process(noise.data(), 2400, y.data());
    std::vector<float> z = x;
    d.process(noise.data(), 2400, z.data());
    CHECK(z == x);
    CHECK(d.gain() == 1.0);
    // The ramp is the 5 ms: from closed it takes 240 samples to reach unity when disabled.
    std::size_t reach = 0;
    {
        NoiseSquelch r(kFs);
        r.setEnabled(false);
        std::vector<float> one(480, 1.0f), nn(480, 0.0f);
        r.process(nn.data(), nn.size(), one.data());
        while (reach < one.size() && one[reach] < 1.0f) { ++reach; }
    }
    CHECK(reach >= 235 && reach <= 245);
    // Clamps and NaN.
    NoiseSquelch c(kFs);
    c.setThresholdDb(std::nan(""));
    c.setTailMs(1e9);
    c.setHysteresisDb(-4);
    c.setBandLowHz(100.0);
    std::vector<float> bad(1000, std::numeric_limits<float>::quiet_NaN()), au(1000, 1.0f);
    c.process(bad.data(), bad.size(), au.data());
    bool fin = true;
    for (float v : au) { fin = fin && std::isfinite(v); }
    CHECK(fin);
    CHECK(std::isfinite(c.levelDb()));
    // Chunk invariance, and in-place detector (detector == audio).
    const Scenario sc = makeScenario(31, 1.0);
    NoiseSquelch w(kFs);
    w.setThresholdDb(-35.0);
    std::vector<float> whole(sc.det.size(), 1.0f);
    w.process(sc.det.data(), sc.det.size(), whole.data());
    for (std::size_t blk : {1u, 17u, 480u}) {
        NoiseSquelch v(kFs);
        v.setThresholdDb(-35.0);
        std::vector<float> o(sc.det.size(), 1.0f);
        for (std::size_t p = 0; p < o.size(); p += blk) { v.process(sc.det.data() + p, std::min<std::size_t>(blk, o.size() - p), o.data() + p); }
        CHECK(o == whole);
    }
    NoiseSquelch ip(kFs);
    ip.setThresholdDb(-35.0);
    std::vector<float> same = sc.det;
    ip.process(same.data(), same.size(), same.data());  // detector aliasing the audio
    bool inplaceOk = true;
    for (std::size_t i = 0; i < same.size(); ++i) { inplaceOk = inplaceOk && std::fabs(same[i] - sc.det[i] * whole[i]) < 1e-6f; }
    CHECK(inplaceOk);
    // Gate never makes a step bigger than the ramp allows: audio 1.0 constant, largest change per sample <= 1/240.
    double maxd = 0;
    for (std::size_t i = 1; i < whole.size(); ++i) { maxd = std::max(maxd, std::fabs(static_cast<double>(whole[i]) - whole[i - 1])); }
    CHECK(maxd <= 1.0 / 240.0 + 1e-6);
    // No allocation; cost.
    NoiseSquelch al(kFs);
    std::vector<float> blk(480, 0.1f), ab(480, 1.0f);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe p;
        al.setEnabled(i % 5 != 0);
        al.setThresholdDb(-30.0 - i);
        al.setBandLowHz(4000.0 + 50.0 * i);
        al.process(blk.data(), blk.size(), ab.data());
        news += p.news();
    }
    std::printf("MEAS operator new in 40 process calls with setting changes: %ld\n", news);
    CHECK(news == 0);
    std::vector<float> big = blk4::tone(480000, kFs, 7000.0, 0.2), bigA(480000, 1.0f);
    NoiseSquelch on(kFs), off(kFs);
    off.setEnabled(false);
    off.process(big.data(), 480, bigA.data());
    off.process(big.data(), 480, bigA.data());
    auto t0 = std::chrono::steady_clock::now();
    off.process(big.data(), big.size(), bigA.data());
    auto t1 = std::chrono::steady_clock::now();
    on.process(big.data(), big.size(), bigA.data());
    auto t2 = std::chrono::steady_clock::now();
    const double nsOff = std::chrono::duration<double, std::nano>(t1 - t0).count() / 480000.0;
    const double nsOn = std::chrono::duration<double, std::nano>(t2 - t1).count() / 480000.0;
    std::printf("MEAS cost per sample: disabled %.3f ns, running %.1f ns\n", nsOff, nsOn);
    CHECK(nsOff * 20.0 < nsOn);
}
}  // namespace

// EXACT DIGITAL SILENCE (engine wiring lane 1: the guard the block's record asked for). After a second of noise, 20 s of all-zero input: the cost per sample stays that of
// ordinary processing (the unguarded high-pass state decays into denormals at about 68 ns a sample), the level reads silence and the gate behaves.
void testSilenceCost() {
    NoiseSquelch s(kFs);
    s.setThresholdDb(-30.0);
    DiscNoise noise(kSigma, 5);
    std::vector<float> loud(static_cast<std::size_t>(kFs));
    noise.fill(loud, 0, loud.size());
    std::vector<float> a(loud.size(), 1.0f);
    s.process(loud.data(), loud.size(), a.data());
    std::vector<float> zeros(static_cast<std::size_t>(kFs) * 20, 0.0f), b(zeros.size(), 1.0f);
    const auto t0 = std::chrono::steady_clock::now();
    s.process(zeros.data(), zeros.size(), b.data());
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / static_cast<double>(zeros.size());
    // for comparison: the same amount of ordinary (noisy) input
    NoiseSquelch r(kFs);
    std::vector<float> nz(zeros.size());
    noise.fill(nz, 0, nz.size());
    std::vector<float> c(nz.size(), 1.0f);
    const auto t1 = std::chrono::steady_clock::now();
    r.process(nz.data(), nz.size(), c.data());
    const double nsNoise = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t1).count() / static_cast<double>(zeros.size());
    std::printf("  silence: %.1f ns a sample; ordinary noise: %.1f ns a sample (the unguarded high-pass measured 68 ns in silence)\n", ns, nsNoise);
    CHECK(ns < 30.0);
    CHECK(ns <= nsNoise * 1.5 + 5.0);
    CHECK(s.levelDb() == -200.0);
    CHECK(s.isOpen());   // the level is below any threshold: the gate is open (a receiver with no input is "quiet")
    // the gate's output is finite and exact: nothing denormal reaches the audio
    for (float v : b) {
        if (!std::isfinite(v) || (v != 0.0f && std::fabs(v) < 1e-30f)) {
            CHECK(false);
            break;
        }
    }
    // and the guard changes nothing for ordinary input: the same noise through a fresh block gives the same decisions as before the guard (open fraction pinned by testFalseOpens)
}

int main() {
    testSilenceCost();
    testDelays();
    testHysteresisAndTail();
    testSpeechStaysOpen();
    testBasics();
    testFalseOpens();
    return testSummary("test_noise_squelch");
}
