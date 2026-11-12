// The patch canvas's audio mixer node (src/dsp/audio_mixer.hpp): gain accuracy, the balance / pan laws, mute and solo, clipping and the limiter,
// click-free gain moves, chunking, bad data, no allocation, concurrent setters. Generated signals only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>

#include "dsp/audio_mixer.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using cascade::dsp::AudioMixer;
using cascade::dsp::PanLaw;

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kN = 48000;

struct Out {
    std::vector<float> l, r;
};

Out mixStereo(AudioMixer& m, const std::vector<std::vector<float>>& left, const std::vector<std::vector<float>>* right = nullptr) {
    std::vector<const float*> pl, pr;
    for (const auto& v : left) { pl.push_back(v.data()); }
    if (right != nullptr) { for (const auto& v : *right) { pr.push_back(v.data()); } }
    Out o;
    const std::size_t n = left.empty() ? 0 : left[0].size();
    o.l.assign(n, 0.0f);
    o.r.assign(n, 0.0f);
    m.process(pl.data(), right != nullptr ? pr.data() : nullptr, n, o.l.data(), o.r.data());
    return o;
}

Out mixSame(AudioMixer& m, const std::vector<std::vector<float>>& v) { return mixStereo(m, v, &v); }

double amp(const std::vector<float>& v, double hz) { return dspm::toneAmp(v, kN / 4, kN * 3 / 4, kFs, hz); }

void testGainAccuracy() {
    // Stereo inputs at the centre have unity side gains: the output is exactly input x gain.
    const std::vector<float> t = dspm::tone(kN, kFs, 1000.0, 0.1);
    for (double gdb : {-60.0, -20.0, -6.0, 0.0, 6.0, 12.0, 20.0}) {
        AudioMixer m(1, kFs);
        m.setGainDb(0, gdb);
        const Out o = mixSame(m, {t});
        const double got = dspm::db(amp(o.l, 1000.0) / 0.1);
        CHECK_NEAR(got, gdb, 0.001);
        CHECK_NEAR(dspm::db(amp(o.r, 1000.0) / 0.1), gdb, 0.001);
    }
    std::printf("MEAS gain accuracy: -60 .. +20 dB steps within 0.001 dB (stereo input at the centre)\n");
    // Two inputs sum: +6.0206 dB for two equal in-phase tones, and a second tone at another frequency is untouched by the first one's gain.
    AudioMixer two(2, kFs);
    const std::vector<float> t2 = dspm::tone(kN, kFs, 1000.0, 0.1), u = dspm::tone(kN, kFs, 2500.0, 0.1);
    two.setGainDb(0, 0.0);
    two.setGainDb(1, -20.0);
    const std::vector<std::vector<float>> ins = {t2, u};
    const Out o = mixStereo(two, ins, &ins);
    CHECK_NEAR(amp(o.l, 1000.0), 0.1, 1e-4);
    CHECK_NEAR(amp(o.l, 2500.0), 0.01, 1e-5);
    // Master gain.
    AudioMixer mm(1, kFs);
    mm.setMasterDb(-12.0);
    const Out om = mixSame(mm, {t});
    CHECK_NEAR(dspm::db(amp(om.l, 1000.0) / 0.1), -12.0, 0.001);
    // Eight equal in-phase inputs add by 8; eight uncorrelated noises add in POWER (x sqrt(8) in RMS).
    AudioMixer eight(8, kFs);
    std::vector<std::vector<float>> same(8, t);
    CHECK_NEAR(amp(mixStereo(eight, same, &same).l, 1000.0), 0.8, 1e-4);
    std::mt19937_64 rng(4);
    std::normal_distribution<double> g(0.0, 0.05);
    std::vector<std::vector<float>> noise(8, std::vector<float>(kN));
    for (auto& v : noise) { for (auto& s : v) { s = static_cast<float>(g(rng)); } }
    const Out on = mixStereo(eight, noise, &noise);
    double p = 0;
    for (float s : on.l) { p += static_cast<double>(s) * s; }
    const double rms = std::sqrt(p / kN);
    std::printf("MEAS 8 uncorrelated noises: output RMS %.4f (pin %.4f)\n", rms, 0.05 * std::sqrt(8.0));
    CHECK_NEAR(rms, 0.05 * std::sqrt(8.0), 0.05 * std::sqrt(8.0) * 0.03);
}

void testLaws() {
    // Mono pan, constant power: gL^2 + gR^2 = 1 at every position; hard left is 1 / 0; centre is 1/sqrt(2) each.
    double worst = 0;
    for (int i = -100; i <= 100; ++i) {
        double gl, gr;
        AudioMixer::sideGains(i / 100.0, false, PanLaw::ConstantPower, &gl, &gr);
        worst = std::max(worst, std::fabs(gl * gl + gr * gr - 1.0));
    }
    std::printf("MEAS constant-power pan: worst |gL^2 + gR^2 - 1| over 201 positions = %.2e\n", worst);
    CHECK(worst < 1e-12);
    double gl, gr;
    AudioMixer::sideGains(-1.0, false, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gl, 1.0, 1e-12);
    CHECK_NEAR(gr, 0.0, 1e-12);
    AudioMixer::sideGains(0.0, false, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gl, 0.70710678118654752, 1e-12);
    CHECK_NEAR(gr, 0.70710678118654752, 1e-12);
    AudioMixer::sideGains(1.0, false, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gl, 0.0, 1e-12);
    CHECK_NEAR(gr, 1.0, 1e-12);
    // Linear mono law: gL + gR = 1.
    for (int i = -100; i <= 100; ++i) {
        AudioMixer::sideGains(i / 100.0, false, PanLaw::Linear, &gl, &gr);
        CHECK_NEAR(gl + gr, 1.0, 1e-12);
    }
    AudioMixer::sideGains(0.0, false, PanLaw::Linear, &gl, &gr);
    CHECK_NEAR(gl, 0.5, 1e-12);
    // Stereo balance: linear, unity at the centre, the louder side never above 1.
    AudioMixer::sideGains(0.0, true, PanLaw::ConstantPower, &gl, &gr);
    CHECK(gl == 1.0 && gr == 1.0);
    AudioMixer::sideGains(0.5, true, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gl, 0.5, 1e-12);
    CHECK_NEAR(gr, 1.0, 1e-12);
    AudioMixer::sideGains(-1.0, true, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gl, 1.0, 1e-12);
    CHECK_NEAR(gr, 0.0, 1e-12);
    // Out of range balance clamps.
    AudioMixer::sideGains(7.0, false, PanLaw::ConstantPower, &gl, &gr);
    CHECK_NEAR(gr, 1.0, 1e-12);

    // Through the mixer: a mono tone panned hard right has nothing on the left; the centre is -3.01 dB on each side; a mono input at the centre in the
    // MONO output is unity.
    const std::vector<float> t = dspm::tone(kN, kFs, 1000.0, 0.2);
    AudioMixer m(1, kFs);
    m.setBalance(0, 1.0);
    Out o = mixStereo(m, {t});
    CHECK(dspm::maxAbs(o.l, 0, kN) < 1e-6);
    CHECK_NEAR(amp(o.r, 1000.0), 0.2, 1e-4);
    AudioMixer c(1, kFs);
    o = mixStereo(c, {t});
    CHECK_NEAR(dspm::db(amp(o.l, 1000.0) / 0.2), -3.0103, 0.001);
    CHECK_NEAR(dspm::db(amp(o.r, 1000.0) / 0.2), -3.0103, 0.001);
    std::vector<float> mono(kN);
    const float* pl[1] = {t.data()};
    c.processMono(pl, nullptr, kN, mono.data());
    CHECK_NEAR(amp(mono, 1000.0), 0.2, 1e-4);
    // Stereo input with balance -1 (hard left): the right channel is gone, the left stays at its own level.
    AudioMixer bal(1, kFs);
    bal.setBalance(0, -1.0);
    const std::vector<float> rr = dspm::tone(kN, kFs, 1500.0, 0.2);
    const std::vector<std::vector<float>> rrv = {rr};
    o = mixStereo(bal, {t}, &rrv);
    CHECK(dspm::maxAbs(o.r, 0, kN) < 1e-6);
    CHECK_NEAR(amp(o.l, 1000.0), 0.2, 1e-4);
    // Pan law switch.
    AudioMixer lin(1, kFs);
    lin.setPanLaw(PanLaw::Linear);
    o = mixStereo(lin, {t});
    CHECK_NEAR(amp(o.l, 1000.0), 0.1, 1e-4);
}

void testMuteSolo() {
    const std::vector<float> a = dspm::tone(kN, kFs, 1000.0, 0.1), b = dspm::tone(kN, kFs, 2500.0, 0.1), c = dspm::tone(kN, kFs, 4000.0, 0.1);
    const std::vector<std::vector<float>> ins = {a, b, c};
    AudioMixer m(3, kFs);
    m.setMute(1, true);
    Out o = mixStereo(m, ins, &ins);
    CHECK_NEAR(amp(o.l, 1000.0), 0.1, 1e-4);
    CHECK(amp(o.l, 2500.0) < 1e-6);
    CHECK_NEAR(amp(o.l, 4000.0), 0.1, 1e-4);
    // Solo plays only the soloed input; soloing two plays both; mute beats solo; clearing the solo brings the rest back.
    AudioMixer s(3, kFs);
    s.setSolo(2, true);
    o = mixStereo(s, ins, &ins);
    CHECK(amp(o.l, 1000.0) < 1e-6 && amp(o.l, 2500.0) < 1e-6);
    CHECK_NEAR(amp(o.l, 4000.0), 0.1, 1e-4);
    s.setSolo(0, true);
    o = mixStereo(s, ins, &ins);
    CHECK_NEAR(amp(o.l, 1000.0), 0.1, 1e-4);
    CHECK(amp(o.l, 2500.0) < 1e-6);
    s.setMute(2, true);
    o = mixStereo(s, ins, &ins);
    CHECK(amp(o.l, 4000.0) < 1e-6);
    s.setMute(2, false);
    s.setSolo(0, false);
    s.setSolo(2, false);
    o = mixStereo(s, ins, &ins);
    CHECK_NEAR(amp(o.l, 2500.0), 0.1, 1e-4);
}

void testClippingAndLimiter() {
    const std::vector<float> t = dspm::tone(kN, kFs, 1000.0, 0.9);
    const std::vector<std::vector<float>> ins = {t, t, t};
    AudioMixer raw(3, kFs);
    Out o = mixStereo(raw, ins, &ins);
    // Without the limiter nothing is clipped here: three in-phase 0.9 tones reach 2.7 and pass as they are; the clip counter says so.
    CHECK_NEAR(dspm::maxAbs(o.l, 0, kN), 2.7, 1e-3);
    CHECK(raw.clipCount() > 0);
    const std::uint64_t first = raw.clipCount();
    raw.resetClipCount();
    CHECK(raw.clipCount() == 0);
    (void)first;
    // With it: never above 1, a quiet signal is untouched BIT-EXACTLY, and the curve is continuous and strictly rising.
    AudioMixer lim(3, kFs);
    lim.setLimiter(true);
    o = mixStereo(lim, ins, &ins);
    CHECK(dspm::maxAbs(o.l, 0, kN) <= 1.0);
    CHECK(dspm::maxAbs(o.l, 0, kN) > 0.99);
    AudioMixer quiet(1, kFs);
    quiet.setLimiter(true);
    const std::vector<float> q = dspm::tone(kN, kFs, 1000.0, 0.7);
    const Out oq = mixSame(quiet, {q});
    bool exact = true;
    for (std::size_t i = 0; i < kN; ++i) { exact = exact && oq.l[i] == q[i]; }
    CHECK(exact);
    float prev = -1.0f;
    double maxJump = 0;
    for (int i = -4000; i <= 4000; ++i) {
        const float x = static_cast<float>(i) / 1000.0f;
        const float y = AudioMixer::limit(x, 0.8f);
        CHECK((std::fabs(x) < 1.2f) ? (y > prev || i == -4000) : (y >= prev));  // strictly rising where float can show it
        if (i > -4000) { maxJump = std::max(maxJump, static_cast<double>(std::fabs(y - prev))); }
        prev = y;
        CHECK(std::fabs(y) <= 1.0f);
    }
    CHECK(maxJump < 0.0015);  // no step anywhere, the knee included
    CHECK_NEAR(AudioMixer::limit(0.8f, 0.8f), 0.8, 1e-7);
    CHECK_NEAR(AudioMixer::limit(1.8f, 0.8f), 0.8 + 0.2 * std::tanh(5.0), 1e-6);
    CHECK(AudioMixer::limit(-1.8f, 0.8f) == -AudioMixer::limit(1.8f, 0.8f));
    // Threshold is clamped.
    lim.setLimiterThreshold(5.0);
    lim.setLimiterThreshold(std::numeric_limits<double>::quiet_NaN());
    o = mixStereo(lim, ins, &ins);
    CHECK(dspm::maxAbs(o.l, 0, kN) <= 1.0);
    // The mono output limits the downmix.
    std::vector<float> mono(kN);
    std::vector<const float*> pl = {t.data(), t.data(), t.data()};
    lim.processMono(pl.data(), nullptr, kN, mono.data());
    CHECK(dspm::maxAbs(mono, 0, kN) <= 1.0);
}

void testClickFree() {
    // A gain change from 0 dB to silence mid-tone moves over 5 ms: the largest sample step stays near the tone's own slope.
    const std::vector<float> t = dspm::tone(2 * kN, kFs, 1000.0, 0.5);
    AudioMixer m(1, kFs);
    std::vector<const float*> pl = {t.data()};
    std::vector<const float*> pr = {t.data()};
    std::vector<float> l(2 * kN), r(2 * kN);
    // The mute lands at a PEAK of the tone (12 samples past a whole number of cycles = a quarter cycle), where an instant mute is a step of 0.5.
    constexpr std::size_t kFirst = kN + 12;
    m.process(pl.data(), pr.data(), kFirst, l.data(), r.data());
    m.setMute(0, true);
    pl[0] = t.data() + kFirst;
    pr[0] = t.data() + kFirst;
    m.process(pl.data(), pr.data(), 2 * kN - kFirst, l.data() + kFirst, r.data() + kFirst);
    const double slope = 0.5 * dspm::kTwoPi * 1000.0 / kFs;
    const double step = dspm::maxStep(l, 0, 2 * kN);
    std::printf("MEAS mute mid-tone: largest sample step %.4f (tone slope %.4f, instant mute would give ~0.5)\n", step, slope);
    CHECK(step < slope + 0.5 / 240.0 + 0.002);
    CHECK(dspm::maxAbs(l, kFirst + 400, 2 * kN) == 0.0);  // fully silent after the ramp
    // The first block starts at its settings (no fade-in from silence).
    AudioMixer first(1, kFs);
    first.setGainDb(0, -6.0);
    std::vector<const float*> p1 = {t.data()};
    std::vector<float> fl(64), fr(64);
    first.process(p1.data(), p1.data(), 64, fl.data(), fr.data());
    CHECK_NEAR(fl[10], t[10] * std::pow(10.0, -6.0 / 20.0), 1e-6);
}

void testChunkingAndBadData() {
    const std::vector<float> a = dspm::tone(kN, kFs, 1000.0, 0.3), b = dspm::tone(kN, kFs, 3100.0, 0.3);
    const std::vector<std::vector<float>> ins = {a, b};
    auto setup = [](AudioMixer& m) {
        m.setGainDb(0, -3.0);
        m.setBalance(1, -0.4);
        m.setMasterDb(2.0);
        m.setLimiter(true);
    };
    AudioMixer whole(2, kFs);
    setup(whole);
    const Out w = mixStereo(whole, ins);
    for (std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{1000}}) {
        AudioMixer m(2, kFs);
        setup(m);
        std::vector<float> l(kN), r(kN);
        for (std::size_t p = 0; p < kN; p += block) {
            const std::size_t n = std::min(block, kN - p);
            const float* pl[2] = {a.data() + p, b.data() + p};
            m.process(pl, nullptr, n, l.data() + p, r.data() + p);
        }
        CHECK(l == w.l && r == w.r);
    }
    // A change in the middle ramps the same way however the input is chunked.
    auto withChange = [&](std::size_t block) {
        AudioMixer m(2, kFs);
        std::vector<float> l(kN), r(kN);
        for (std::size_t p = 0; p < kN; p += block) {
            if (p == 24000) { m.setGainDb(0, -30.0); }
            const std::size_t n = std::min(block, kN - p);
            const float* pl[2] = {a.data() + p, b.data() + p};
            m.process(pl, nullptr, n, l.data() + p, r.data() + p);
        }
        return l;
    };
    CHECK(withChange(1000) == withChange(24000 / 3));
    // Non-finite samples read as 0; a null input is silent; a null array is silent; out-of-range settings clamp; bad indices are ignored.
    std::vector<float> bad = a;
    bad[100] = std::numeric_limits<float>::quiet_NaN();
    bad[101] = std::numeric_limits<float>::infinity();
    AudioMixer m(2, kFs);
    const float* pl[2] = {bad.data(), nullptr};
    std::vector<float> l(kN), r(kN);
    m.process(pl, nullptr, kN, l.data(), r.data());
    CHECK(l[100] == 0.0f && l[101] == 0.0f && std::isfinite(l[102]));
    m.process(nullptr, nullptr, 16, l.data(), r.data());
    CHECK(l[3] == 0.0f);
    m.process(pl, nullptr, 0, l.data(), r.data());
    m.process(pl, nullptr, 8, nullptr, r.data());
    m.setGainDb(0, 1e9);
    m.setGainDb(5, 3.0);
    m.setGainDb(-1, 3.0);
    m.setBalance(0, std::numeric_limits<double>::quiet_NaN());
    m.setMasterDb(-1e9);
    m.setMute(9, true);
    m.process(pl, nullptr, 600, l.data(), r.data());
    CHECK(l[500] == 0.0f);  // master at -90 dB or less: silent once the 5 ms ramp has run
    CHECK(AudioMixer(0).inputCount() == 1 && AudioMixer(99).inputCount() == AudioMixer::kMaxInputs);
}

void testNoAllocAndThreads() {
    const std::vector<float> a = dspm::tone(4096, kFs, 1000.0, 0.3), b = dspm::tone(4096, kFs, 3100.0, 0.3);
    AudioMixer m(2, kFs);
    m.setLimiter(true);
    std::vector<float> l(4096), r(4096), mono(4096);
    const float* pl[2] = {a.data(), b.data()};
    m.process(pl, nullptr, 4096, l.data(), r.data());
    {
        DspmAllocProbe probe;
        for (int i = 0; i < 20; ++i) {
            m.setGainDb(0, -1.0 * (i % 5));
            m.setBalance(1, 0.1 * (i % 7) - 0.3);
            m.process(pl, nullptr, 4096, l.data(), r.data());
            m.processMono(pl, nullptr, 4096, mono.data());
        }
        CHECK(probe.news() == 0);
    }
    // Setters from another thread while the audio thread mixes: every output stays finite and in range.
    std::atomic<bool> stop{false};
    std::thread setter([&] {
        int i = 0;
        while (!stop.load()) {
            m.setGainDb(0, -40.0 + (i % 50));
            m.setBalance(1, (i % 21) / 10.0 - 1.0);
            m.setMute(0, (i % 3) == 0);
            m.setSolo(1, (i % 5) == 0);
            ++i;
        }
    });
    bool finite = true;
    for (int i = 0; i < 400; ++i) {
        m.process(pl, nullptr, 4096, l.data(), r.data());
        for (std::size_t s = 0; s < 4096; ++s) { finite = finite && std::isfinite(l[s]) && std::isfinite(r[s]) && std::fabs(l[s]) <= 1.0f; }
    }
    stop.store(true);
    setter.join();
    CHECK(finite);
}

}  // namespace

int main() {
    testGainAccuracy();
    testLaws();
    testMuteSolo();
    testClippingAndLimiter();
    testClickFree();
    testChunkingAndBadData();
    testNoAllocAndThreads();
    return testSummary("test_audio_mixer");
}
