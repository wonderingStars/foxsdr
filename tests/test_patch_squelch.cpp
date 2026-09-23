// Tests for the patch demodulator's squelch (0.99.18): patch_runner.hpp's
// RunningChannel::squelch, Runner::setSquelchDb and Runner::squelchState.
//
// WHAT IT MUST DO, in the order a user meets it:
//   - an empty channel records SILENCE, not full-scale noise (the owner's
//     complaint that asked for it);
//   - a signal opens it, and the speaker hears the signal;
//   - switching it off, or moving the threshold, takes effect on the next
//     block without rebuilding anything;
//   - a decoder behind the same demodulator still gets EVERY sample - a
//     squelch that opens late clips the burst a decoder is waiting for.
//
// The test thread is the runner's DSP thread (process() is called directly),
// so nothing here depends on timing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"
#include "test_check.hpp"

using namespace cascade::core::patch;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 1.0e6;
constexpr double kCentre = 100.0e6;
constexpr double kOffset = 200000.0;

class Capture final : public AudioDest {
public:
    void write(const float* s, std::size_t n) override {
        note(s, n);
        got.insert(got.end(), s, s + n);
    }
    std::string describe() const override { return "capture"; }
    std::string error() const override { return {}; }
    std::vector<float> got;
};

double rms(const std::vector<float>& x, std::size_t from) {
    double e = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < x.size(); ++i) {
        e += static_cast<double>(x[i]) * x[i];
        ++n;
    }
    return n == 0 ? 0.0 : std::sqrt(e / static_cast<double>(n));
}

// An audio decoder that only measures what it is given.
struct Meter {
    double energy = 0.0;
    std::uint64_t frames = 0;
};
void* meterCreate(std::uint32_t) { return new Meter; }
void meterProcess(void* h, const float* s, std::size_t n) {
    Meter* m = static_cast<Meter*>(h);
    for (std::size_t i = 0; i < n; ++i) { m->energy += static_cast<double>(s[i]) * s[i]; }
    m->frames += n;
}
std::int32_t meterPoll(void*, char*, std::size_t) { return 0; }
Meter* g_last = nullptr;
void meterDestroy(void* h) {
    Meter* m = static_cast<Meter*>(h);
    if (g_last == m) { g_last = nullptr; }
    delete m;
}
void* meterCreateTracked(std::uint32_t r) {
    g_last = static_cast<Meter*>(meterCreate(r));
    return g_last;
}

// Noise at `sigma` per component, plus (optionally) an AM carrier at kOffset
// modulated by 1 kHz.
struct Gen {
    std::uint32_t s = 12345u;
    double t = 0.0;
    float uniform() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFFFF) / 16777216.0f - 0.5f;
    }
    void block(std::vector<std::complex<float>>& out, float sigma, float carrier) {
        for (auto& z : out) {
            // Sum of four uniforms: close enough to Gaussian for a noise floor.
            float i = 0.0f, q = 0.0f;
            for (int k = 0; k < 4; ++k) {
                i += uniform();
                q += uniform();
            }
            z = std::complex<float>(i * sigma * 1.7f, q * sigma * 1.7f);
            if (carrier > 0.0f) {
                const double env = 1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
                const double ph = 2.0 * kPi * kOffset * t;
                z += std::complex<float>(static_cast<float>(carrier * env * std::cos(ph)),
                                         static_cast<float>(carrier * env * std::sin(ph)));
            }
            t += 1.0 / kRate;
        }
    }
};

}  // namespace

int main() {
    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "R", PortType::Iq);
    const NodeId chan = g.addNode(NodeKind::Channel, "C", PortType::Iq);
    const NodeId demod = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
    const NodeId spk = g.addNode(NodeKind::Sink, "S", PortType::Audio);
    const NodeId dec = g.addNode(NodeKind::Decoder, "Meter", PortType::Audio);
    g.mutableNode(chan)->freqHz = kCentre + kOffset;
    g.mutableNode(demod)->mode = 2;   // AM
    // A new demodulator starts with the squelch ON, at the receiver's -50 dB.
    CHECK(g.find(demod)->squelch);
    CHECK(g.find(demod)->squelchDb == -50.0f);
    g.mutableNode(demod)->squelchDb = -30.0f;
    g.mutableNode(dec)->plugin = "meter.dll";
    CHECK(g.connect(radio, 0, chan, 0) == Connect::Ok);
    CHECK(g.connect(chan, 0, demod, 0) == Connect::Ok);
    CHECK(g.connect(demod, 0, spk, 0) == Connect::Ok);
    CHECK(g.connect(demod, 0, dec, 0) == Connect::Ok);

    CascadeDecoderApi api{};
    api.structSize = sizeof(CascadeDecoderApi);
    api.requiredRateHz = 0;
    api.create = meterCreateTracked;
    api.process = meterProcess;
    api.poll_text = meterPoll;
    api.destroy = meterDestroy;
    const std::vector<DecoderInfo> cat{{"meter.dll", "Meter", PortType::Audio, 0.0, false}};
    const std::vector<PluginApis> apis{{&api, nullptr, nullptr}};

    const Plan plan = compile(g, kRate, kCentre, &cat);
    CHECK(plan.runnable);
    auto cap = std::make_shared<Capture>();
    const DestTable dests{{spk, cap}};
    Runner runner;
    runner.publish(buildRadioSet(plan, g, radio, kRate, &cat, &apis, dests));

    Gen gen;
    std::vector<std::complex<float>> blk(10000);
    const auto run = [&](int blocks, float sigma, float carrier) {
        for (int b = 0; b < blocks; ++b) {
            gen.block(blk, sigma, carrier);
            runner.process(blk.data(), blk.size());
        }
    };

    // [1] AN EMPTY CHANNEL RECORDS SILENCE. Half a second of noise alone, well
    // under a -30 dB threshold: after the gate has settled, nothing but zeros.
    run(50, 0.01f, 0.0f);
    float level = 0.0f;
    bool open = true;
    CHECK(runner.squelchState(chan, level, open));
    std::printf("noise: level %.1f dB, open %d, speaker rms %.6f\n", level, open ? 1 : 0,
                rms(cap->got, cap->got.size() / 2));
    CHECK(!open);
    CHECK(level < -40.0f && level > -120.0f);
    CHECK(cap->got.size() > 20000u);
    CHECK(rms(cap->got, cap->got.size() / 2) < 1e-6);
    // ...while the decoder behind the same demodulator was fed the noise.
    CHECK(g_last != nullptr);
    const double decNoise = g_last != nullptr ? g_last->energy : 0.0;
    std::printf("decoder energy over the noise: %.3f\n", decNoise);
    CHECK(decNoise > 0.0);

    // [2] A SIGNAL OPENS IT, and the speaker hears it.
    std::size_t mark = cap->got.size();
    run(30, 0.01f, 0.3f);
    CHECK(runner.squelchState(chan, level, open));
    const double sigRms = rms(cap->got, mark + (cap->got.size() - mark) / 2);
    std::printf("signal: level %.1f dB, open %d, speaker rms %.4f\n", level, open ? 1 : 0, sigRms);
    CHECK(open);
    CHECK(level > -30.0f);
    CHECK(sigRms > 0.01);

    // [3] IT CLOSES AGAIN when the signal goes (after its hold).
    mark = cap->got.size();
    run(30, 0.01f, 0.0f);
    CHECK(runner.squelchState(chan, level, open));
    CHECK(!open);
    CHECK(rms(cap->got, mark + (cap->got.size() - mark) / 2) < 1e-6);

    // [4] SWITCHED OFF LIVE: the next blocks pass the noise - no rebuild.
    runner.setSquelchDb(chan, kSquelchOffDb);
    mark = cap->got.size();
    run(30, 0.01f, 0.0f);
    CHECK(runner.squelchState(chan, level, open));
    const double offRms = rms(cap->got, mark + (cap->got.size() - mark) / 2);
    std::printf("squelch off: open %d, speaker rms %.6f\n", open ? 1 : 0, offRms);
    CHECK(open);
    CHECK(offRms > 1e-4);

    // [5] AND BACK ON at a threshold under the noise: open as well.
    runner.setSquelchDb(chan, -110.0f);
    run(10, 0.01f, 0.0f);
    CHECK(runner.squelchState(chan, level, open));
    CHECK(open);
    // Over it: closed.
    runner.setSquelchDb(chan, -30.0f);
    mark = cap->got.size();
    run(30, 0.01f, 0.0f);
    CHECK(runner.squelchState(chan, level, open));
    CHECK(!open);
    CHECK(rms(cap->got, mark + (cap->got.size() - mark) / 2) < 1e-6);

    // [6] A channel that has not run is not reported.
    float l2 = 0.0f;
    bool o2 = false;
    CHECK(!runner.squelchState(static_cast<NodeId>(9999), l2, o2));

    // [7] A demodulator saved with the squelch OFF builds with it off.
    g.mutableNode(demod)->squelch = false;
    Runner r2;
    auto cap2 = std::make_shared<Capture>();
    r2.publish(buildRadioSet(compile(g, kRate, kCentre, &cat), g, radio, kRate, &cat, &apis,
                             DestTable{{spk, cap2}}));
    for (int b = 0; b < 30; ++b) {
        gen.block(blk, 0.01f, 0.0f);
        r2.process(blk.data(), blk.size());
    }
    CHECK(rms(cap2->got, cap2->got.size() / 2) > 1e-4);

    runner.flushNow();
    r2.flushNow();
    return testSummary("test_patch_squelch");
}
