// The two signal blocks of the tuning lane: the tone-burst generator (a tone only; a source scan keeps it away from any transmit path) and the CW stereo
// placement by pitch.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/cw_surround.hpp"
#include "dsp/tone_burst.hpp"
#include "test_check.hpp"

namespace cd = cascade::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint32_t u32() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<std::uint32_t>(s >> 32); }
    double next() { return static_cast<double>(u32() >> 8) / 8388608.0 - 1.0; }   // [-1, 1)
};

std::vector<float> renderAll(cd::ToneBurst& t, std::size_t total, std::size_t chunk) {
    std::vector<float> out(total, -9.0f);
    std::size_t i = 0;
    while (i < total) {
        const std::size_t n = std::min(chunk, total - i);
        t.render(out.data() + i, n);
        i += n;
    }
    return out;
}

// Power of the tone at fHz in x (Goertzel), as an amplitude estimate.
double toneAmplitude(const std::vector<float>& x, double fHz, double fs) {
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double ph = 2.0 * kPi * fHz * static_cast<double>(i) / fs;
        re += x[i] * std::cos(ph);
        im += x[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

void testToneBurstShape() {
    cd::ToneBurst t;                                      // 48 kHz, 1750 Hz, 500 ms, 10 ms edges, amplitude 0.5
    CHECK(t.frequencyHz() == 1750.0 && t.totalSamples() == 24000 && t.edgeSamples() == 480 && !t.active());
    std::vector<float> quiet(100, 1.0f);
    CHECK(t.render(quiet.data(), quiet.size()) == 0);     // nothing playing: zeros, nothing produced
    for (float v : quiet) { CHECK(v == 0.0f); }
    CHECK(t.trigger() && t.active());
    CHECK(!t.trigger());                                  // a held key does not stack bursts
    std::vector<float> x(30000, -9.0f);
    const std::size_t made = t.render(x.data(), x.size());
    CHECK(made == 24000 && !t.active());                  // exactly the burst length
    for (std::size_t i = 24000; i < x.size(); ++i) { CHECK(x[i] == 0.0f); }
    // no click: the first and last samples are nearly zero; the peak reaches the amplitude
    CHECK(std::fabs(x[0]) < 0.001 && std::fabs(x[23999]) < 0.001);
    float peak = 0.0f;
    for (std::size_t i = 0; i < 24000; ++i) { peak = std::max(peak, std::fabs(x[i])); }
    CHECK_NEAR(peak, 0.5, 0.002);
    // the envelope rises monotonically over the attack: the sine's envelope sampled at its crests
    double lastCrest = 0.0;
    int crestsChecked = 0;
    for (std::size_t i = 1; i < 480; ++i) {
        if (x[i] > x[i - 1] && x[i] >= x[i + 1]) { CHECK(x[i] > lastCrest); lastCrest = x[i]; ++crestsChecked; }
    }
    CHECK(crestsChecked >= 8);
    // no step anywhere: the largest sample-to-sample change is within what a 1750 Hz sine of that amplitude can do
    double maxStep = 0.0;
    for (std::size_t i = 1; i < 24000; ++i) { maxStep = std::max(maxStep, static_cast<double>(std::fabs(x[i] - x[i - 1]))); }
    CHECK(maxStep <= 0.5 * 2.0 * kPi * 1750.0 / 48000.0 * 1.02);
    // the frequency: 1750 Hz stands out of a 40 Hz neighbourhood by orders of magnitude, and by zero crossings
    const double a1750 = toneAmplitude(x, 1750.0, 48000.0);
    CHECK(a1750 > 0.4 * 0.9 && a1750 < 0.5);
    CHECK(toneAmplitude(x, 1710.0, 48000.0) < 0.02 * a1750 && toneAmplitude(x, 1790.0, 48000.0) < 0.02 * a1750);
    int crossings = 0;
    for (std::size_t i = 1; i < 24000; ++i) { if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) { ++crossings; } }
    CHECK(std::abs(crossings - 2 * 875) <= 4);            // 0.5 s x 1750 Hz = 875 cycles
    // chunking never changes a sample: 1 sample, 7, 480, 4800, 24000 at a time
    for (const std::size_t chunk : {1u, 7u, 480u, 4800u, 24000u}) {
        cd::ToneBurst c;
        c.trigger();
        const std::vector<float> y = renderAll(c, 24000, chunk);
        CHECK(std::equal(y.begin(), y.end(), x.begin()));
    }
    // the same burst again after it ends; phase restarts at zero each time
    CHECK(t.trigger());
    std::vector<float> again(24000);
    t.render(again.data(), again.size());
    CHECK(std::equal(again.begin(), again.end(), x.begin()));
}

void testToneBurstSettings() {
    cd::ToneBurst t(44100.0);
    // other sample rates: the length is the exact rounded count
    CHECK(t.totalSamples() == 22050);
    cd::ToneBurst t96(96000.0);
    CHECK(t96.setLengthMs(333.0) && t96.totalSamples() == 31968);
    cd::ToneBurst t8(8000.0);
    CHECK(t8.setLengthMs(50.0) && t8.totalSamples() == 400 && t8.frequencyHz() == 1750.0);
    // limits, each at the edge and beyond; a refusal keeps the old value
    CHECK(t.setFrequencyHz(300.0) && t.setFrequencyHz(3400.0) && !t.setFrequencyHz(299.9) && !t.setFrequencyHz(3400.1) && !t.setFrequencyHz(NAN) && t.frequencyHz() == 3400.0);
    cd::ToneBurst low(8000.0);
    CHECK(!low.setFrequencyHz(3000.0) && low.setFrequencyHz(2600.0));   // under a third of the sample rate
    CHECK(t.setLengthMs(50.0) && t.setLengthMs(5000.0) && !t.setLengthMs(49.0) && !t.setLengthMs(5001.0) && !t.setLengthMs(NAN) && t.totalSamples() == 220500);
    CHECK(t.setEdgeMs(1.0) && t.setEdgeMs(100.0) && !t.setEdgeMs(0.5) && !t.setEdgeMs(101.0) && !t.setEdgeMs(INFINITY));
    CHECK(t.setAmplitude(0.0) && t.setAmplitude(1.0) && !t.setAmplitude(-0.1) && !t.setAmplitude(1.1) && !t.setAmplitude(NAN));
    // an edge longer than half the burst is cut to half (50 ms burst, 100 ms edge: 25 ms each side, no flat top)
    cd::ToneBurst s;
    CHECK(s.setLengthMs(50.0) && s.setEdgeMs(100.0) && s.edgeSamples() == 1200 && s.totalSamples() == 2400);
    s.trigger();
    std::vector<float> y(2400);
    CHECK(s.render(y.data(), y.size()) == 2400);
    CHECK(std::fabs(y[0]) < 0.001 && std::fabs(y[2399]) < 0.001);
    // amplitude scales; zero amplitude is silence of the right length
    cd::ToneBurst z;
    CHECK(z.setAmplitude(0.0));
    z.trigger();
    std::vector<float> zero(24000, 1.0f);
    CHECK(z.render(zero.data(), zero.size()) == 24000);
    for (float v : zero) { CHECK(v == 0.0f); }
    cd::ToneBurst full;
    CHECK(full.setAmplitude(1.0));
    full.trigger();
    std::vector<float> f(24000);
    full.render(f.data(), f.size());
    float pk = 0.0f;
    for (float v : f) { pk = std::max(pk, std::fabs(v)); }
    CHECK_NEAR(pk, 1.0, 0.003);
    // settings do not change while a burst plays
    cd::ToneBurst busy;
    busy.trigger();
    CHECK(!busy.setFrequencyHz(2000.0) && !busy.setLengthMs(100.0) && !busy.setEdgeMs(5.0) && !busy.setAmplitude(0.1));
    // abort stops it at once and a new trigger works
    std::vector<float> some(1000);
    busy.render(some.data(), some.size());
    busy.abort();
    CHECK(!busy.active());
    std::vector<float> after(100, 5.0f);
    CHECK(busy.render(after.data(), after.size()) == 0);
    CHECK(busy.trigger() && busy.setFrequencyHz(2000.0) == false);
    // a bad sample rate falls back to 48 kHz
    cd::ToneBurst odd(100.0);
    CHECK(odd.totalSamples() == 24000);
    cd::ToneBurst odd2(NAN);
    CHECK(odd2.totalSamples() == 24000);
    // a different tone: 2100 Hz measured
    cd::ToneBurst d;
    CHECK(d.setFrequencyHz(2100.0) && d.setLengthMs(200.0));
    d.trigger();
    std::vector<float> dd(9600);
    d.render(dd.data(), dd.size());
    CHECK(toneAmplitude(dd, 2100.0, 48000.0) > 0.4 * 0.85 && toneAmplitude(dd, 1750.0, 48000.0) < 0.05);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testToneBurstSourceScan() {
    // THE TONE REACHES NO TRANSMIT PATH: the header includes only <cmath> and <cstddef>, and names nothing that sends, keys or opens anything.
    std::string here = __FILE__;
    for (char& c : here) { if (c == '\\') { c = '/'; } }
    const std::string root = here.substr(0, here.rfind("/tests/"));
    const std::string src = readFile(root + "/src/dsp/tone_burst.hpp");
    CHECK(src.size() > 1000);
    std::istringstream in(src);
    std::string line;
    int includes = 0;
    while (std::getline(in, line)) {
        if (line.rfind("#include", 0) == 0) {
            ++includes;
            CHECK(line.find("<cmath>") != std::string::npos || line.find("<cstddef>") != std::string::npos);
        }
    }
    CHECK(includes == 2);
    // outside comments: no identifier of a transmitter, a sink, a socket or a file
    std::string code;
    std::istringstream in2(src);
    while (std::getline(in2, line)) {
        const std::size_t c = line.find("//");
        code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
    for (const char* bad : {"transmit", "Transmit", "ptt", "PTT", "key(", "Key", "socket", "send", "Sink", "sink", "fopen", "ofstream", "CreateFile", "Pluto", "pluto", "tx_"}) {
        CHECK(code.find(bad) == std::string::npos);
        if (code.find(bad) != std::string::npos) { std::printf("  found: %s\n", bad); }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) { s += static_cast<double>(v[i]) * v[i]; }
    return std::sqrt(s / static_cast<double>(to - from));
}

// A tone with frequency offset error, optional noise at `snrDb` (tone power over noise power), phase 0.
std::vector<float> tone(double fHz, double seconds, double snrDb, std::uint64_t seed, double fs = 48000.0) {
    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> x(n);
    Lcg rng(seed);
    const double noiseAmp = snrDb >= 200.0 ? 0.0 : std::sqrt(3.0 * 0.5 * std::pow(10.0, -snrDb / 10.0));   // uniform noise: variance a^2/3; tone power 0.5
    for (std::size_t i = 0; i < n; ++i) { x[i] = static_cast<float>(std::sin(2.0 * kPi * fHz * static_cast<double>(i) / fs) + noiseAmp * rng.next()); }
    return x;
}

void testCwPanLaw() {
    CHECK_NEAR(cd::CwSurround::panForPitch(700, 700, 400), 0.0, 1e-12);
    CHECK_NEAR(cd::CwSurround::panForPitch(300, 700, 400), -1.0, 1e-12);
    CHECK_NEAR(cd::CwSurround::panForPitch(1100, 700, 400), 1.0, 1e-12);
    CHECK_NEAR(cd::CwSurround::panForPitch(500, 700, 400), -0.5, 1e-12);
    CHECK_NEAR(cd::CwSurround::panForPitch(100, 700, 400), -1.0, 1e-12);       // clamped
    CHECK_NEAR(cd::CwSurround::panForPitch(5000, 700, 400), 1.0, 1e-12);
    CHECK_NEAR(cd::CwSurround::panForPitch(500, 700, 400, true), 0.5, 1e-12);   // invert
    CHECK(cd::CwSurround::panForPitch(NAN, 700, 400) == 0.0 && cd::CwSurround::panForPitch(500, 700, 0.0) == 0.0 && cd::CwSurround::panForPitch(500, 700, -4.0) == 0.0);
    for (double pan = -1.0; pan <= 1.0001; pan += 0.125) {
        double l, r;
        cd::CwSurround::gainsForPan(pan, l, r);
        CHECK_NEAR(l * l + r * r, 2.0, 1e-12);                                  // constant power
        if (pan < -0.001) { CHECK(l > r); }
        if (pan > 0.001) { CHECK(r > l); }
    }
    double l, r;
    cd::CwSurround::gainsForPan(0.0, l, r);
    CHECK_NEAR(l, 1.0, 1e-12);
    CHECK_NEAR(r, 1.0, 1e-12);                                                  // centred is unity in each ear
    cd::CwSurround::gainsForPan(-1.0, l, r);
    CHECK_NEAR(l, std::sqrt(2.0), 1e-12);
    CHECK_NEAR(r, 0.0, 1e-12);
    cd::CwSurround::gainsForPan(7.0, l, r);                                     // beyond range: clamped
    CHECK_NEAR(l, 0.0, 1e-12);
}

void testCwPlacement() {
    // Tones of different pitch, with noise (10 dB), land at different places; the pitch estimate is within a few hertz.
    struct Case { double f; double expectedPan; };
    const Case cases[] = {{437.3, -0.6575}, {612.9, -0.2178}, {702.1, 0.0053}, {823.4, 0.3085}, {1023.1, 0.8078}};
    double lastPan = -2.0;
    for (const Case& c : cases) {
        cd::CwSurround w;
        const std::vector<float> in = tone(c.f, 1.5, 10.0, 11);
        std::vector<float> l(in.size()), r(in.size());
        w.process(in.data(), l.data(), r.data(), in.size());
        CHECK_NEAR(w.pitchHz(), c.f, 6.0);
        CHECK_NEAR(w.pan(), c.expectedPan, 0.03);
        CHECK(w.pan() > lastPan);                      // higher pitch, further right
        lastPan = w.pan();
        const double rl = rms(l, 36000, 72000), rr = rms(r, 36000, 72000);
        if (c.f < 650.0) { CHECK(rl > rr * 1.05); }
        if (c.f > 750.0) { CHECK(rr > rl * 1.05); }
        if (std::fabs(c.f - 700.0) < 5.0) { CHECK_NEAR(rl / rr, 1.0, 0.02); }
    }
    // a clean tone: exact enough to place within 0.01
    cd::CwSurround w;
    const std::vector<float> in = tone(500.0, 1.0, 300.0, 1);
    std::vector<float> l(in.size()), r(in.size());
    w.process(in.data(), l.data(), r.data(), in.size());
    CHECK_NEAR(w.pitchHz(), 500.0, 1.0);
    CHECK_NEAR(w.pan(), -0.5, 0.01);
    // constant power at the output: L^2 + R^2 = 2 x^2 sample by sample once the pan has settled (the gains are the pan's, the pan moves slowly)
    double maxErr = 0.0;
    for (std::size_t i = 30000; i < in.size(); ++i) { maxErr = std::max(maxErr, std::fabs(static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i] - 2.0 * in[i] * in[i])); }
    CHECK(maxErr < 1e-4);
    // invert swaps the sides
    cd::CwSurround inv;
    cd::CwSurroundConfig cfg;
    cfg.invert = true;
    CHECK(inv.setConfig(cfg));
    std::vector<float> l2(in.size()), r2(in.size());
    inv.process(in.data(), l2.data(), r2.data(), in.size());
    CHECK_NEAR(inv.pan(), 0.5, 0.01);
    CHECK(rms(r2, 36000, 48000) > rms(l2, 36000, 48000));   // 500 Hz is left of the centre, inverted: right
}

void testCwHoldAndNoise() {
    // noise alone never moves the image: no tone found, pitch 0, pan 0
    cd::CwSurround w;
    Lcg rng(5);
    std::vector<float> noise(96000);
    for (float& v : noise) { v = static_cast<float>(0.3 * rng.next()); }
    std::vector<float> l(noise.size()), r(noise.size());
    w.process(noise.data(), l.data(), r.data(), noise.size());
    CHECK_NEAR(w.pitchHz(), 0.0, 0.0);
    CHECK_NEAR(w.pan(), 0.0, 1e-9);
    // a tone, then silence: the pitch is held for holdMs, then the image glides back to the centre
    cd::CwSurround h;
    const std::vector<float> t = tone(450.0, 1.0, 300.0, 3);
    std::vector<float> tl(t.size()), tr(t.size());
    h.process(t.data(), tl.data(), tr.data(), t.size());
    const double panAtEnd = h.pan();
    CHECK(panAtEnd < -0.5);
    std::vector<float> sil(4800, 0.0f), sl(4800), sr(4800);        // 100 ms of silence: still held (hold 400 ms)
    h.process(sil.data(), sl.data(), sr.data(), sil.size());
    CHECK(h.pan() < -0.5);
    for (int i = 0; i < 12; ++i) { h.process(sil.data(), sl.data(), sr.data(), sil.size()); }   // 1.3 s in all: well past the hold and the glide
    CHECK(std::fabs(h.pan()) < 0.05);
    // a tone in the middle of noise at 0 dB SNR is still found (the tone ratio is on the bank mean)
    cd::CwSurround n;
    const std::vector<float> tn = tone(900.0, 2.0, 0.0, 9);
    std::vector<float> nl(tn.size()), nr(tn.size());
    n.process(tn.data(), nl.data(), nr.data(), tn.size());
    CHECK_NEAR(n.pitchHz(), 900.0, 10.0);
    // a signal weaker than the absolute floor is not a tone
    cd::CwSurround q;
    std::vector<float> weak(48000);
    for (std::size_t i = 0; i < weak.size(); ++i) { weak[i] = static_cast<float>(1e-8 * std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / 48000.0)); }
    std::vector<float> wl(weak.size()), wr(weak.size());
    q.process(weak.data(), wl.data(), wr.data(), weak.size());
    CHECK(q.pitchHz() == 0.0);
}

void testCwChunkingDisabledAndRefusals() {
    const std::vector<float> in = tone(560.0, 1.0, 20.0, 21);
    cd::CwSurround whole;
    std::vector<float> wl(in.size()), wr(in.size());
    whole.process(in.data(), wl.data(), wr.data(), in.size());
    for (const std::size_t chunk : {1u, 13u, 512u, 1000u, 4096u}) {
        cd::CwSurround c;
        std::vector<float> cl(in.size()), cr(in.size());
        for (std::size_t i = 0; i < in.size(); i += chunk) {
            const std::size_t n = std::min<std::size_t>(chunk, in.size() - i);
            c.process(in.data() + i, cl.data() + i, cr.data() + i, n);
        }
        CHECK(cl == wl && cr == wr);
    }
    // disabled: bit-exact copy
    cd::CwSurround off;
    off.setEnabled(false);
    CHECK(!off.enabled());
    std::vector<float> ol(in.size()), orr(in.size());
    off.process(in.data(), ol.data(), orr.data(), in.size());
    CHECK(ol == in && orr == in);
    // non-finite input reads as zero and does not poison later samples
    cd::CwSurround nf;
    std::vector<float> bad(2000, 0.5f);
    bad[10] = NAN; bad[11] = INFINITY; bad[12] = -INFINITY;
    std::vector<float> bl(bad.size()), br(bad.size());
    nf.process(bad.data(), bl.data(), br.data(), bad.size());
    for (std::size_t i = 0; i < bad.size(); ++i) { CHECK(std::isfinite(bl[i]) && std::isfinite(br[i])); }
    // config limits: each edge and a step beyond; a refusal keeps the old config
    cd::CwSurround c;
    cd::CwSurroundConfig cfg;
    CHECK(c.setConfig(cfg));
    auto edit = [&](auto f) { cd::CwSurroundConfig k; f(k); return c.setConfig(k); };
    CHECK(!edit([](auto& k) { k.centrePitchHz = 199.9; }) && !edit([](auto& k) { k.centrePitchHz = 1500.1; }) && !edit([](auto& k) { k.centrePitchHz = NAN; }));
    CHECK(!edit([](auto& k) { k.spreadHz = 49.9; }) && !edit([](auto& k) { k.spreadHz = 2000.1; }));
    CHECK(!edit([](auto& k) { k.toneRatio = 1.9; }) && !edit([](auto& k) { k.toneRatio = 1000.1; }));
    CHECK(!edit([](auto& k) { k.holdMs = -1.0; }) && !edit([](auto& k) { k.holdMs = 5000.1; }));
    CHECK(c.config().centrePitchHz == 700.0 && c.config().spreadHz == 400.0);
    CHECK(edit([](auto& k) { k.centrePitchHz = 200.0; k.spreadHz = 50.0; k.toneRatio = 2.0; k.holdMs = 0.0; }));
    CHECK(edit([](auto& k) { k.centrePitchHz = 1500.0; k.spreadHz = 2000.0; k.toneRatio = 1000.0; k.holdMs = 5000.0; }));
    // a different centre pitch moves where a tone sits: 500 Hz is centre when the centre is 500
    cd::CwSurround m;
    cd::CwSurroundConfig mc;
    mc.centrePitchHz = 500.0;
    CHECK(m.setConfig(mc));
    const std::vector<float> t5 = tone(500.0, 1.0, 300.0, 4);
    std::vector<float> ml(t5.size()), mr(t5.size());
    m.process(t5.data(), ml.data(), mr.data(), t5.size());
    CHECK_NEAR(m.pan(), 0.0, 0.01);
    // a bad sample rate falls back to 48 kHz and still places a tone
    cd::CwSurround odd(NAN);
    const std::vector<float> t4 = tone(500.0, 1.0, 300.0, 4);
    std::vector<float> ol2(t4.size()), or2(t4.size());
    odd.process(t4.data(), ol2.data(), or2.data(), t4.size());
    CHECK_NEAR(odd.pan(), -0.5, 0.02);
}

}  // namespace

int main() {
    testToneBurstShape();
    testToneBurstSettings();
    testToneBurstSourceScan();
    testCwPanLaw();
    testCwPlacement();
    testCwHoldAndNoise();
    testCwChunkingDisabledAndRefusals();
    return testSummary("test_tuning_dsp");
}
