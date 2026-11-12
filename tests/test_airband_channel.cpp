// Per-channel airband settings (src/core/airband_channel.hpp): validation, the text form (round trip and fuzz), the audio chain (high-pass, low-pass,
// notch against their closed forms), the per-channel squelch (each channel its own threshold) and the processor's pre-gate tap.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "core/airband_channel.hpp"
#include "dspmodels_alloc.hpp"
#include "dspmodels_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::airband;
using Cf = std::complex<float>;

namespace {

constexpr double kFs = 48000.0;

AirbandChannelConfig base() {
    AirbandChannelConfig c;
    c.freqHz = 118700000;
    c.bandwidthHz = 10000;
    return c;
}

void testDefaultsAndValidation() {
    const AirbandChannelConfig c = base();
    CHECK(validate(c) == Problem::None);
    CHECK(c.output == OutputKind::None);  // OFF by default
    CHECK(c.squelchKind == SquelchKind::Level && c.highPassHz == 0 && c.lowPassHz == 0);
    struct Case { int field; std::int64_t value; Problem want; };
    auto with = [&](auto mutate) { AirbandChannelConfig x = base(); mutate(x); return validate(x); };
    CHECK(with([](AirbandChannelConfig& x) { x.freqHz = 0; }) == Problem::BadFrequency);
    CHECK(with([](AirbandChannelConfig& x) { x.freqHz = 7000000000ll; }) == Problem::BadFrequency);
    CHECK(with([](AirbandChannelConfig& x) { x.bandwidthHz = 500; }) == Problem::BadBandwidth);
    CHECK(with([](AirbandChannelConfig& x) { x.bandwidthHz = 300000; }) == Problem::BadBandwidth);
    CHECK(with([](AirbandChannelConfig& x) { x.mode = 2; }) == Problem::BadMode);
    CHECK(with([](AirbandChannelConfig& x) { x.squelchTenthsDb = 10; }) == Problem::BadSquelch);
    CHECK(with([](AirbandChannelConfig& x) { x.squelchTenthsDb = -1300; }) == Problem::BadSquelch);
    CHECK(with([](AirbandChannelConfig& x) { x.snrOpenTenthsDb = 5; }) == Problem::BadSquelch);
    CHECK(with([](AirbandChannelConfig& x) { x.holdMs = 20000; }) == Problem::BadSquelch);
    CHECK(with([](AirbandChannelConfig& x) { x.highPassHz = 10; }) == Problem::BadFilter);
    CHECK(with([](AirbandChannelConfig& x) { x.lowPassHz = 100; }) == Problem::BadFilter);
    CHECK(with([](AirbandChannelConfig& x) { x.highPassHz = 800; x.lowPassHz = 700; }) == Problem::BadFilter);
    CHECK(with([](AirbandChannelConfig& x) { x.filterOrder = 3; }) == Problem::FilterOrder);
    CHECK(with([](AirbandChannelConfig& x) { x.notches[1].hz = 20; }) == Problem::BadNotch);
    CHECK(with([](AirbandChannelConfig& x) { x.notches[2].hz = 1000; x.notches[2].qTenths = 5; }) == Problem::BadNotch);
    CHECK(with([](AirbandChannelConfig& x) { x.output = static_cast<OutputKind>(9); }) == Problem::BadOutput);
    CHECK(with([](AirbandChannelConfig& x) { x.maxFileMb = 0; }) == Problem::BadFileSize);
    CHECK(with([](AirbandChannelConfig& x) { x.udpPort = 80; }) == Problem::BadPort);
    CHECK(with([](AirbandChannelConfig& x) { x.output = OutputKind::Udp; }) == Problem::PortMissing);
    CHECK(with([](AirbandChannelConfig& x) { x.output = OutputKind::Udp; x.udpPort = 7355; }) == Problem::None);
    for (Problem p : {Problem::BadFrequency, Problem::BadBandwidth, Problem::BadMode, Problem::BadSquelch, Problem::BadFilter, Problem::FilterOrder,
                      Problem::BadNotch, Problem::BadOutput, Problem::BadFileSize, Problem::BadPort, Problem::PortMissing}) {
        CHECK(problemText(p)[0] != '\0');
    }
}

void testSanitise() {
    AirbandChannelConfig c;
    c.freqHz = -5;
    c.bandwidthHz = 99999999;
    c.mode = 7;
    c.squelchTenthsDb = 500;
    c.snrOpenTenthsDb = 0;
    c.snrHysteresisTenthsDb = -3;
    c.holdMs = -9;
    c.highPassHz = 900;
    c.lowPassHz = 800;
    c.filterOrder = 9;
    c.notches[0] = {5, 1};
    c.notches[1] = {50000, 99999};
    c.output = OutputKind::Udp;  // no port
    c.maxFileMb = 99999;
    c.udpPort = 0;
    const AirbandChannelConfig s = sanitise(c);
    CHECK(validate(s) == Problem::None);
    CHECK(s.output == OutputKind::None);  // a stream with no port is no output
    CHECK(s.highPassHz == 0 && s.lowPassHz == 800);
    CHECK(sanitise(s) == s);  // idempotent
    AirbandChannelConfig d = base();
    d.output = OutputKind::Udp;
    d.udpPort = 99999999;
    CHECK(sanitise(d).udpPort == 65535);
    // Conversion to the monitor's own channel type.
    d = base();
    d.mode = 1;
    const cascade::core::MonitorChannel m = toMonitorChannel(d);
    CHECK(m.freqHz == 118700000.0 && m.bandwidthHz == 10000.0 && m.mode == cascade::core::MonitorMode::Nfm);
}

void testTextForm() {
    AirbandChannelConfig c = base();
    c.mode = 1;
    c.squelchKind = SquelchKind::Snr;
    c.squelchTenthsDb = -655;
    c.snrOpenTenthsDb = 85;
    c.holdMs = 1200;
    c.highPassHz = 300;
    c.lowPassHz = 3400;
    c.filterOrder = 2;
    c.notches[0] = {1000, 450};
    c.notches[2] = {2400, 80};
    c.output = OutputKind::WavFile;
    c.dailyFolder = false;
    c.maxFileMb = 250;
    const std::string t = serialise(c);
    CHECK(t == "FOXAIRCH1|118700000|10000|1|1|-655|85|30|1200|300|3400|2|1000|450|0|300|2400|80|1|0|250|0");
    const ParseResult r = parse(t);
    CHECK(r.ok && r.config == c && serialise(r.config) == t);
    CHECK(serialise(base()) == "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0");
    // Refusals.
    const char* bad[] = {"",
                         "FOXAIRCH2|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0",
                         "FOXAIRCH1|118700000|10000",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0|5",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|x",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|",
                         "FOXAIRCH1|118700000|10000|0|2|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|3|1|100|0",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|2|100|0",
                         "FOXAIRCH1|0|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0",
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|2|1|100|0",    // udp output, no port
                         "FOXAIRCH1|118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|99999999999999999999",
                         "FOXAIRCH1|1187.5|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0",
                         "FOXAIRCH1| 118700000|10000|0|0|-500|100|30|400|0|0|4|0|300|0|300|0|300|0|1|100|0"};
    for (const char* b : bad) {
        const ParseResult p = parse(b);
        CHECK(!p.ok);
        CHECK(p.why[0] != '\0' || p.problem != Problem::None);
    }
    CHECK(!parse(std::string(kMaxTextLen + 1, '1')).ok);
    // Fuzz: mutated strings and random bytes never crash; what is accepted validates and round-trips.
    std::mt19937_64 rng(31);
    int ok = 0;
    for (int i = 0; i < 40000; ++i) {
        std::string m = t;
        for (int e = 0; e < 1 + static_cast<int>(rng() % 4); ++e) {
            const std::size_t at = rng() % (m.size() + 1);
            switch (rng() % 4) {
                case 0: if (at < m.size()) { m[at] = static_cast<char>('0' + rng() % 10); } break;
                case 1: if (at < m.size()) { m[at] = static_cast<char>(rng()); } break;
                case 2: if (at < m.size()) { m.erase(at, 1); } break;
                default: m.insert(at, 1, static_cast<char>('0' + rng() % 10)); break;
            }
        }
        const ParseResult p = parse(m);
        if (p.ok) {
            ++ok;
            CHECK(validate(p.config) == Problem::None);
            CHECK(parse(serialise(p.config)).ok && parse(serialise(p.config)).config == p.config);
        }
    }
    for (int i = 0; i < 5000; ++i) {
        std::string m(rng() % 120, '\0');
        for (char& ch : m) { ch = static_cast<char>(rng()); }
        CHECK(!parse(m).ok);
    }
    std::printf("MEAS channel text fuzz: 40000 mutated strings (%d accepted, all valid and exact on re-serialise), 5000 random byte strings\n", ok);
    CHECK(ok > 50);
}

double toneGainDb(ChannelAudioChain& chain, double hz) {
    std::vector<float> x = dspm::tone(48000, kFs, hz, 0.1);
    chain.process(x.data(), x.size());
    return dspm::db(dspm::toneAmp(x, 24000, 48000, kFs, hz) / 0.1);
}

void testAudioChain() {
    // 4th-order Butterworth high-pass at 300 Hz: -3.01 dB at 300, -24.1 dB at 150, ~0 dB at 3 kHz.
    AirbandChannelConfig c = base();
    c.highPassHz = 300;
    ChannelAudioChain hp(kFs);
    hp.configure(c);
    const double g150 = toneGainDb(hp, 150.0);
    ChannelAudioChain hp2(kFs);
    hp2.configure(c);
    const double g300 = toneGainDb(hp2, 300.0);
    ChannelAudioChain hp3(kFs);
    hp3.configure(c);
    const double g3k = toneGainDb(hp3, 3000.0);
    std::printf("MEAS high-pass 300 Hz order 4: 150 Hz %.2f dB (pin -24.1), 300 Hz %.2f (pin -3.01), 3 kHz %.3f\n", g150, g300, g3k);
    CHECK_NEAR(g150, -24.1, 0.5);
    CHECK_NEAR(g300, -3.01, 0.15);
    CHECK_NEAR(g3k, 0.0, 0.05);
    CHECK_NEAR(g150, hp.responseDb(150.0), 0.1);
    // Order 2: 12 dB per octave.
    c.filterOrder = 2;
    ChannelAudioChain h2(kFs);
    h2.configure(c);
    CHECK_NEAR(toneGainDb(h2, 150.0), -12.3, 0.5);  // 10 log10(1 + 2^4) = 12.3 dB
    // Low-pass at 3 kHz order 4.
    c = base();
    c.lowPassHz = 3000;
    ChannelAudioChain lp(kFs);
    lp.configure(c);
    const double l6 = toneGainDb(lp, 6000.0);
    ChannelAudioChain lp2(kFs);
    lp2.configure(c);
    const double l500 = toneGainDb(lp2, 500.0);
    std::printf("MEAS low-pass 3 kHz order 4: 6 kHz %.2f dB (pin -24.1), 500 Hz %.3f dB\n", l6, l500);
    CHECK_NEAR(l6, -24.1, 1.6);  // (the bilinear transform bends the digital response down a little at 0.125 of the sample rate)
    CHECK_NEAR(l6, lp.responseDb(6000.0), 0.1);
    CHECK_NEAR(l500, 0.0, 0.05);
    // Notch 1 kHz Q 30: deep at the centre, flat away from it.
    c = base();
    c.notches[0] = {1000, 300};
    ChannelAudioChain nt(kFs);
    nt.configure(c);
    const double n1000 = toneGainDb(nt, 1000.0);
    ChannelAudioChain nt2(kFs);
    nt2.configure(c);
    const double n1200 = toneGainDb(nt2, 1200.0);
    std::printf("MEAS notch 1 kHz Q 30: centre %.1f dB, 1.2 kHz %.3f dB\n", n1000, n1200);
    CHECK(n1000 < -50.0);
    CHECK_NEAR(n1200, 0.0, 0.15);
    // Three notches at once, with HP and LP: each notch bites, the speech band passes.
    c = base();
    c.highPassHz = 300;
    c.lowPassHz = 3400;
    c.notches[0] = {1000, 300};
    c.notches[1] = {1800, 300};
    c.notches[2] = {2600, 300};
    for (double f : {1000.0, 1800.0, 2600.0}) {
        ChannelAudioChain a(kFs);
        a.configure(c);
        CHECK(toneGainDb(a, f) < -45.0);
    }
    ChannelAudioChain mid(kFs);
    mid.configure(c);
    CHECK(std::fabs(toneGainDb(mid, 1400.0)) < 1.5);
    // Bypass: everything off is bit-exact and cheap.
    ChannelAudioChain off(kFs);
    off.configure(base());
    CHECK(off.isBypassed());
    std::vector<float> in = dspm::tone(1000, kFs, 777.0, 0.4), x = in;
    off.process(x.data(), x.size());
    CHECK(x == in);
    // NaN input reads as 0; null is ignored; no allocation.
    ChannelAudioChain a(kFs);
    a.configure(c);
    std::vector<float> bad(100, std::numeric_limits<float>::quiet_NaN());
    a.process(bad.data(), bad.size());
    for (float v : bad) { CHECK(std::isfinite(v)); }
    a.process(nullptr, 5);
    std::vector<float> y = dspm::tone(4096, kFs, 500.0, 0.3);
    DspmAllocProbe probe;
    for (int i = 0; i < 20; ++i) { a.process(y.data(), y.size()); }
    CHECK(probe.news() == 0);
}

void testPerChannelSquelch() {
    // The SAME signal into two channels with different thresholds: the one set above the signal stays closed, the one below opens. (A level squelch
    // judges channel power in dB re full scale; this carrier plus noise is about -41 dB.)
    std::mt19937_64 rng(5);
    std::normal_distribution<double> g(0.0, 0.002);
    std::vector<Cf> iq(48000);
    for (std::size_t i = 0; i < iq.size(); ++i) {
        const double ph = dspm::kTwoPi * 1500.0 * static_cast<double>(i) / kFs;
        iq[i] = Cf(static_cast<float>(0.01 * std::cos(ph) + g(rng)), static_cast<float>(0.01 * std::sin(ph) + g(rng)));
    }
    AirbandChannelConfig lowT = base(), highT = base();
    lowT.squelchTenthsDb = -600;   // -60 dB: the signal is above it
    highT.squelchTenthsDb = -300;  // -30 dB: the signal is below it
    ChannelSquelch a(kFs), b(kFs);
    a.configure(lowT);
    b.configure(highT);
    std::vector<float> audioA(iq.size(), 1.0f), audioB(iq.size(), 1.0f);
    const bool openA = a.process(iq.data(), iq.size(), audioA.data());
    const bool openB = b.process(iq.data(), iq.size(), audioB.data());
    std::printf("MEAS same signal, two thresholds: -60 dB channel open %d, -30 dB channel open %d, meter reads %.1f dB\n", openA ? 1 : 0, openB ? 1 : 0, a.meterDb());
    CHECK(openA && !openB);
    CHECK(audioA.back() == 1.0f && audioB.back() == 0.0f);
    CHECK(a.meterDb() > -50.0 && a.meterDb() < -35.0);
    // SNR kind: the same two thresholds mean nothing; the SNR threshold decides. Noise only (closed), then a carrier 15 dB up.
    AirbandChannelConfig snr = base();
    snr.squelchKind = SquelchKind::Snr;
    snr.snrOpenTenthsDb = 100;
    snr.holdMs = 100;
    ChannelSquelch s(kFs);
    s.configure(snr);
    CHECK(s.kind() == SquelchKind::Snr);
    std::vector<Cf> noise(96000);
    for (auto& v : noise) { v = Cf(static_cast<float>(g(rng)), static_cast<float>(g(rng))); }
    std::vector<float> audio(noise.size(), 1.0f);
    CHECK(!s.process(noise.data(), noise.size(), audio.data()));
    std::vector<Cf> sig(48000);
    for (std::size_t i = 0; i < sig.size(); ++i) {
        const double ph = dspm::kTwoPi * 1500.0 * static_cast<double>(i) / kFs;
        const double amp = 0.002 * 2.0 * std::sqrt(30.0);  // carrier power 30 x the noise power (noise power = 2 sigma^2): total +14.9 dB
        sig[i] = Cf(static_cast<float>(amp * std::cos(ph) + g(rng)), static_cast<float>(amp * std::sin(ph) + g(rng)));
    }
    std::vector<float> a2(sig.size(), 1.0f);
    CHECK(s.process(sig.data(), sig.size(), a2.data()));
    CHECK(s.meterDb() > 10.0);
}

void testProcessorPreGateTap() {
    AirbandChannelConfig c = base();
    c.highPassHz = 300;
    c.squelchTenthsDb = -300;  // closed for this signal
    ChannelProcessor p(kFs);
    p.configure(c);
    std::vector<Cf> iq(4800, Cf(0.001f, 0.0f));
    std::vector<float> audio = dspm::tone(4800, kFs, 1000.0, 0.3), copy = audio, tap(4800, -9.0f);
    ChannelAudioChain ref(kFs);
    ref.configure(c);
    ref.process(copy.data(), copy.size());
    const bool open = p.process(iq.data(), audio.data(), audio.size(), tap.data());
    CHECK(!open);
    CHECK(tap == copy);                 // the tap is the audio after the filter, before the gate
    CHECK(audio.back() == 0.0f);        // and the gate has closed the audio
    CHECK(dspm::maxAbs(tap, 2000, 4800) > 0.1);
    // Without a tap it behaves the same.
    ChannelProcessor q(kFs);
    q.configure(c);
    std::vector<float> a2 = dspm::tone(4800, kFs, 1000.0, 0.3);
    CHECK(!q.process(iq.data(), a2.data(), a2.size()));
    CHECK(a2 == audio);
}

}  // namespace

int main() {
    testDefaultsAndValidation();
    testSanitise();
    testTextForm();
    testAudioChain();
    testPerChannelSquelch();
    testProcessorPreGateTap();
    return testSummary("test_airband_channel");
}
