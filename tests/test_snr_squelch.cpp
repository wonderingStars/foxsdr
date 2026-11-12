// The SNR squelch (src/dsp/snr_squelch.hpp): noise-floor estimate, opening and hang times, hysteresis, a long carrier not teaching it that it is noise,
// following a real change of noise, learning, chunking, gating the audio, bad data. Generated I/Q with noise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "dsp/snr_squelch.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using cascade::dsp::SnrSquelch;
using cascade::dsp::SnrSquelchSettings;
using Cf = std::complex<float>;

namespace {

constexpr double kFs = 48000.0;

// Noise of total complex power 10^(noiseDb/10) plus (optionally) a carrier whose power makes total/noise = snrDb above the noise.
struct Source {
    std::mt19937_64 rng{9};
    std::normal_distribution<double> g{0.0, 1.0};
    double phase = 0.0;
    std::vector<Cf> make(std::size_t n, double noiseDb, double snrDb, double carrierHz = 1700.0) {
        std::vector<Cf> v(n);
        const double sigma = std::sqrt(std::pow(10.0, noiseDb / 10.0) / 2.0);
        const double carrierPower = snrDb > -100.0 ? std::pow(10.0, noiseDb / 10.0) * (std::pow(10.0, snrDb / 10.0) - 1.0) : 0.0;
        const double amp = std::sqrt(carrierPower);
        for (std::size_t i = 0; i < n; ++i) {
            phase += dspm::kTwoPi * carrierHz * (1.0 + 20e-6) / kFs;
            v[i] = Cf(static_cast<float>(sigma * g(rng) + amp * std::cos(phase)), static_cast<float>(sigma * g(rng) + amp * std::sin(phase)));
        }
        return v;
    }
};

// Feeds `seconds` of a source in 480-sample chunks and returns the time (s) when the gate first took the stated state, or -1.
double runUntil(SnrSquelch& q, Source& s, double seconds, double noiseDb, double snrDb, bool wantOpen) {
    const std::size_t chunk = 480;
    const std::size_t total = static_cast<std::size_t>(seconds * kFs);
    for (std::size_t done = 0; done < total; done += chunk) {
        const std::vector<Cf> x = s.make(chunk, noiseDb, snrDb);
        q.process(x.data(), x.size());
        if (q.isOpen() == wantOpen) { return static_cast<double>(done + chunk) / kFs; }
    }
    return -1.0;
}

void testNoiseOnly() {
    SnrSquelch q(kFs);
    Source s;
    // 60 s of noise only: the gate never opens; the floor estimate sits within 1 dB of the truth; the SNR reads near 0 (the floor follows the minimum,
    // so a little positive).
    const std::size_t chunk = 4800;
    bool everOpen = false;
    double worstSnr = -1e9;
    for (int i = 0; i < 600; ++i) {  // 600 x 100 ms
        const std::vector<Cf> x = s.make(chunk, -60.0, -200.0);
        q.process(x.data(), x.size());
        everOpen = everOpen || q.isOpen();
        if (i > 20) { worstSnr = std::max(worstSnr, q.snrDb()); }
    }
    std::printf("MEAS noise only at -60 dB for 60 s: floor %.2f dB, power %.2f dB, worst SNR %.2f dB, ever open: %d\n", q.floorDb(), q.powerDb(), worstSnr, everOpen ? 1 : 0);
    CHECK(!everOpen);
    CHECK_NEAR(q.floorDb(), -60.0, 1.0);
    CHECK(worstSnr < 3.0);
}

void testOpenAndHang() {
    SnrSquelch q(kFs);
    Source s;
    CHECK(runUntil(q, s, 2.0, -60.0, -200.0, true) < 0);  // noise: stays closed through the learning period and after
    // A carrier 15 dB above the noise.
    const double tOpen = runUntil(q, s, 2.0, -60.0, 15.0, true);
    std::printf("MEAS carrier +15 dB over the floor: opened after %.3f s (delay 20 ms + 20 ms smoothing), SNR read %.2f dB\n", tOpen, q.snrDb());
    CHECK(tOpen > 0.0 && tOpen < 0.15);
    // Keep it on for a second: the SNR reads 15 dB.
    runUntil(q, s, 1.0, -60.0, 15.0, false);
    CHECK_NEAR(q.snrDb(), 15.0, 1.0);
    // The carrier goes: it closes after the hang (400 ms) plus the smoothing that takes the SNR under 7 dB.
    const double tClose = runUntil(q, s, 3.0, -60.0, -200.0, false);
    std::printf("MEAS carrier removed: closed after %.3f s (hang 400 ms)\n", tClose);
    CHECK(tClose > 0.38 && tClose < 0.55);
    // A longer hang is honoured.
    SnrSquelch h(kFs);
    SnrSquelchSettings st = h.settings();
    st.hangMs = 1000.0;
    h.setSettings(st);
    Source s2;
    runUntil(h, s2, 1.0, -60.0, -200.0, true);
    CHECK(runUntil(h, s2, 1.0, -60.0, 15.0, true) > 0.0);  // open
    runUntil(h, s2, 0.5, -60.0, 15.0, false);               // (stays open)
    const double t2 = runUntil(h, s2, 3.0, -60.0, -200.0, false);
    CHECK(t2 > 0.98 && t2 < 1.2);
    // Opening delay: a carrier shorter than the delay never opens it (a 10 ms burst with a 50 ms delay).
    SnrSquelch d(kFs);
    st = d.settings();
    st.openDelayMs = 50.0;
    st.smoothMs = 1.0;
    d.setSettings(st);
    Source s3;
    runUntil(d, s3, 1.0, -60.0, -200.0, true);
    std::vector<Cf> burst = s3.make(480, -60.0, 15.0);
    d.process(burst.data(), burst.size());
    CHECK(!d.isOpen());
    const std::vector<Cf> q2 = s3.make(4800, -60.0, -200.0);
    d.process(q2.data(), q2.size());
    CHECK(!d.isOpen());
}

void testHysteresis() {
    // Power values driven directly (slot()): floor learned at 1e-6, then a level whose SNR sits BETWEEN the thresholds (open 10, close 7).
    // (A power that stays above the floor with the gate closed is, by the floor's own rule, noise that is rising: it creeps up at 1 dB/s. The
    // band tests are therefore short, or run with the creep off.)
    SnrSquelch q(kFs);
    const double noise = 1e-6;
    for (int i = 0; i < 400; ++i) { q.slot(noise); }  // 2 s
    CHECK(!q.isOpen());
    CHECK_NEAR(q.floorDb(), -60.0, 0.01);
    const double mid = noise * std::pow(10.0, 0.85);  // SNR 8.5 dB
    for (int i = 0; i < 40; ++i) { q.slot(mid); }      // 0.2 s
    CHECK(!q.isOpen());                                // closed stays closed in the band
    CHECK(q.snrDb() > 8.2 && q.snrDb() < 8.5 + 0.01);
    const double high = noise * std::pow(10.0, 1.4);
    for (int i = 0; i < 20; ++i) { q.slot(high); }
    CHECK(q.isOpen());
    for (int i = 0; i < 2000; ++i) { q.slot(mid); }  // 10 s in the band
    CHECK(q.isOpen());  // open stays open in the band (while open the floor creeps 0.05 dB/s: 0.5 dB in 10 s, the SNR stays above 7)
    // Exactly at the thresholds, with the creep off so the SNR is what is written: 9.99 dB does not open, 10.01 dB does, 7.01 dB holds, 6.99 dB closes
    // once the hang has run.
    SnrSquelch e(kFs);
    SnrSquelchSettings st = e.settings();
    st.floorRiseDbPerS = 0.0;
    st.openRiseDbPerS = 0.0;
    st.smoothMs = 1.0;
    e.setSettings(st);
    for (int i = 0; i < 400; ++i) { e.slot(noise); }
    for (int i = 0; i < 400; ++i) { e.slot(noise * std::pow(10.0, 0.999)); }
    CHECK(!e.isOpen());
    for (int i = 0; i < 20; ++i) { e.slot(noise * std::pow(10.0, 1.001)); }
    CHECK(e.isOpen());
    for (int i = 0; i < 400; ++i) { e.slot(noise * std::pow(10.0, 0.701)); }
    CHECK(e.isOpen());
    for (int i = 0; i < 100; ++i) { e.slot(noise * std::pow(10.0, 0.699)); }  // 0.5 s > the 400 ms hang
    CHECK(!e.isOpen());
}

void testLongCarrierAndRealNoiseChange() {
    SnrSquelch q(kFs);
    const double noise = 1e-6;
    for (int i = 0; i < 400; ++i) { q.slot(noise); }
    const double floor0 = q.floorDb();
    // A carrier at +15 dB for 60 s: the gate stays open, the floor creeps at most 0.05 dB/s (3 dB in 60 s) and the SNR stays above the close threshold.
    for (int i = 0; i < 20; ++i) { q.slot(noise * std::pow(10.0, 1.5)); }
    CHECK(q.isOpen());
    double minSnr = 1e9;
    bool wasClosed = false;
    for (int i = 0; i < 60 * 200; ++i) {
        q.slot(noise * std::pow(10.0, 1.5));
        minSnr = std::min(minSnr, q.snrDb());
        wasClosed = wasClosed || !q.isOpen();
    }
    std::printf("MEAS 60 s carrier +15 dB: floor moved %.2f dB (limit 3.0), lowest SNR %.2f dB, gate lost: %d\n", q.floorDb() - floor0, minSnr, wasClosed ? 1 : 0);
    CHECK(!wasClosed);
    CHECK(q.floorDb() - floor0 <= 3.05 && q.floorDb() - floor0 > 2.0);
    CHECK(minSnr > 11.5);
    // Carrier gone, then the REAL noise rises by 6 dB (no carrier): the SNR reads 6 (below the open threshold), the floor follows at 1 dB/s.
    for (int i = 0; i < 400; ++i) { q.slot(noise); }  // closes
    CHECK(!q.isOpen());
    const double n2 = noise * std::pow(10.0, 0.6);
    int slotsToFollow = 0;
    for (int i = 0; i < 200 * 20; ++i) {
        q.slot(n2);
        ++slotsToFollow;
        if (std::fabs(q.floorDb() - (-60.0 + 6.0)) < 0.5) { break; }
    }
    std::printf("MEAS real noise rise of +6 dB: the floor followed in %.1f s (rate 1 dB/s), gate open: %d\n", slotsToFollow / 200.0, q.isOpen() ? 1 : 0);
    CHECK(slotsToFollow / 200.0 > 4.0 && slotsToFollow / 200.0 < 8.0);
    CHECK(!q.isOpen());
    // A noise DROP is followed down quickly (50 ms constant).
    for (int i = 0; i < 100; ++i) { q.slot(noise * 0.1); }  // 0.5 s
    CHECK(q.floorDb() < -69.0 && q.floorDb() > -71.0);  // the new -70 dB
    // maxOpenS: a gate held open by a floor that jumped up relearns by itself.
    SnrSquelch m(kFs);
    SnrSquelchSettings st = m.settings();
    st.maxOpenS = 5.0;
    m.setSettings(st);
    for (int i = 0; i < 400; ++i) { m.slot(noise); }
    for (int i = 0; i < 200; ++i) { m.slot(noise * 100.0); }  // +20 dB "signal"
    CHECK(m.isOpen());
    for (int i = 0; i < 1040 - 200; ++i) { m.slot(noise * 100.0); }  // 5.2 s open in all: the 5 s limit has run out
    CHECK(!m.isOpen() && m.learning());
    for (int i = 0; i < 200; ++i) { m.slot(noise * 100.0); }  // it has learned the new level as the floor
    CHECK_NEAR(m.floorDb(), -40.0, 0.5);
    // relearn() by hand.
    m.relearn();
    CHECK(!m.isOpen() && m.learning());
}

void testLearningAndDisabled() {
    // A strong carrier present from the very first sample: the learning period keeps the gate closed, and the carrier becomes the floor.
    SnrSquelch q(kFs);
    Source s;
    const double t = runUntil(q, s, 1.0, -60.0, 20.0, true);
    CHECK(t < 0.0);
    CHECK(q.floorDb() > -45.0);
    // Disabled: the gate reads open and audio passes at unity from the first sample, while the estimator keeps running.
    SnrSquelch d(kFs);
    SnrSquelchSettings st = d.settings();
    st.enabled = false;
    d.setSettings(st);
    std::vector<Cf> x = s.make(2400, -60.0, -200.0);
    std::vector<float> audio(2400, 0.5f);
    d.process(x.data(), x.size(), audio.data());
    CHECK(d.isOpen());
    CHECK(audio[2399] == 0.5f);  // unity once the 5 ms ramp is done
    CHECK(audio[0] < 0.5f);       // (the envelope starts from 0: a start-up ramp, no step)
    CHECK(d.floorDb() < -55.0 && d.floorDb() > -65.0);
}

void testAudioGating() {
    SnrSquelch q(kFs);
    Source s;
    std::vector<float> audio;
    std::vector<float> gains;
    auto feed = [&](double seconds, double snrDb) {
        const std::size_t total = static_cast<std::size_t>(seconds * kFs);
        for (std::size_t d = 0; d < total; d += 480) {
            const std::vector<Cf> x = s.make(480, -60.0, snrDb);
            std::vector<float> a(480, 1.0f);
            q.process(x.data(), x.size(), a.data());
            audio.insert(audio.end(), a.begin(), a.end());
        }
    };
    feed(1.5, -200.0);
    for (float v : audio) { CHECK(v == 0.0f); }  // closed: exact silence
    const std::size_t before = audio.size();
    feed(1.0, 15.0);
    double step = 0;
    for (std::size_t i = before + 1; i < audio.size(); ++i) { step = std::max(step, std::fabs(static_cast<double>(audio[i]) - audio[i - 1])); }
    std::printf("MEAS gate opening: largest audio step %.4f (a 5 ms ramp is %.4f)\n", step, 1.0 / 240.0);
    CHECK(step <= 1.0 / 240.0 + 1e-6);
    CHECK(audio.back() == 1.0f);
}

void testChunking() {
    auto run = [](std::size_t chunk) {
        SnrSquelch q(kFs);
        Source s;
        std::vector<Cf> all;
        const std::vector<Cf> a = s.make(48000, -60.0, -200.0), b = s.make(48000, -60.0, 14.0), c = s.make(96000, -60.0, -200.0);
        all.insert(all.end(), a.begin(), a.end());
        all.insert(all.end(), b.begin(), b.end());
        all.insert(all.end(), c.begin(), c.end());
        std::vector<float> audio(all.size(), 1.0f);
        std::vector<int> openAt;
        for (std::size_t p = 0; p < all.size(); p += chunk) {
            const std::size_t n = std::min(chunk, all.size() - p);
            q.process(all.data() + p, n, audio.data() + p);
        }
        return std::make_pair(audio, std::make_pair(q.snrDb(), q.floorDb()));
    };
    const auto whole = run(192000);
    CHECK(whole.first[100000] > 0.0f);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{240}, std::size_t{1000}, std::size_t{4801}}) {
        const auto r = run(chunk);
        CHECK(r.first == whole.first);
        CHECK(r.second == whole.second);
    }
}

void testBadDataAndSettings() {
    SnrSquelch q(kFs);
    std::vector<Cf> bad(5000, Cf(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()));
    q.process(bad.data(), bad.size());
    CHECK(std::isfinite(q.powerDb()) && std::isfinite(q.floorDb()) && std::isfinite(q.snrDb()));
    q.process(nullptr, 100);
    q.slot(std::numeric_limits<double>::quiet_NaN());
    q.slot(-4.0);
    CHECK(std::isfinite(q.floorDb()));
    SnrSquelchSettings w;
    w.openDb = std::numeric_limits<double>::quiet_NaN();
    w.hysteresisDb = -5;
    w.hangMs = 1e9;
    w.smoothMs = 0;
    w.floorRiseDbPerS = 1e9;
    w.learnMs = -1;
    w.maxOpenS = 1e12;
    q.setSettings(w);
    CHECK(q.settings().openDb == 10.0);
    CHECK(q.settings().hysteresisDb == 0.0 && q.settings().hangMs == 10000.0 && q.settings().smoothMs == 1.0);
    CHECK(q.settings().floorRiseDbPerS == 60.0 && q.settings().learnMs == 0.0 && q.settings().maxOpenS == 86400.0);
    // No allocation in the audio path.
    SnrSquelch a(kFs);
    Source s;
    std::vector<Cf> x = s.make(4800, -60.0, 12.0);
    std::vector<float> audio(4800, 1.0f);
    a.process(x.data(), x.size(), audio.data());
    DspmAllocProbe probe;
    for (int i = 0; i < 50; ++i) { a.process(x.data(), x.size(), audio.data()); }
    CHECK(probe.news() == 0);
}

}  // namespace

int main() {
    testNoiseOnly();
    testOpenAndHang();
    testHysteresis();
    testLongCarrierAndRealNoiseChange();
    testLearningAndDisabled();
    testAudioGating();
    testChunking();
    testBadDataAndSettings();
    return testSummary("test_snr_squelch");
}
