// test_patch_mixer_wire_a.cpp - THE MIXER PART RUNS (wire-a second pass, item 5, second part): two demodulated channels summed into one speaker through a Mixer part, in a real radio set driven by the real runner.
//   A  the settings text;  B  the plan: the sink lists both channels with their gains (a muted one at 0), a mixer that feeds anything but sinks still says it is not run;  C  what the speaker is given: a 1 kHz tone from the
//   first channel and a 2 kHz tone from the second sit together in the sum, a gain of 0.5 halves one, a mute removes one, a gain of 2 doubles it, and a sink with no mixer plays its one channel as before;  D  the signature follows
//   the mixer's settings.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/patch_audio.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_nodes.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 2400000.0;
constexpr double kCentre = 100000000.0;

class Capture final : public pc::AudioDest {
public:
    void write(const float* s, std::size_t n) override { v.insert(v.end(), s, s + n); }
    std::string describe() const override { return "capture"; }
    std::string error() const override { return std::string(); }
    std::vector<float> v;
};

// Two AM carriers: +200 kHz modulated at 1 kHz, -300 kHz modulated at 2 kHz.
std::vector<std::complex<float>> two(std::size_t n, std::size_t from) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(from + i) / kRate;
        const double a = (1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t)) * 0.5, b = (1.0 + 0.5 * std::sin(2.0 * kPi * 2000.0 * t)) * 0.5;
        const std::complex<double> v = std::polar(a, 2.0 * kPi * 200000.0 * t) + std::polar(b, -2.0 * kPi * 300000.0 * t);
        out[i] = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    return out;
}

struct Built {
    pc::Graph g;
    pc::NodeId radio = pc::kNoNode, chA = pc::kNoNode, chB = pc::kNoNode, mixer = pc::kNoNode, sink = pc::kNoNode;
};

// Radio -> Channel A (+200 kHz) -> AM demod -> mixer input 0; Radio -> Channel B (-300 kHz) -> AM demod -> mixer input 1; mixer -> speaker. Without a mixer, channel A's demod goes straight to the speaker.
Built build(const std::string& mixerSettings, bool withMixer = true) {
    Built b;
    b.radio = b.g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq);
    b.chA = b.g.addNode(pc::NodeKind::Channel, "A", pc::PortType::Iq);
    b.chB = b.g.addNode(pc::NodeKind::Channel, "B", pc::PortType::Iq);
    b.g.mutableNode(b.chA)->freqHz = kCentre + 200000.0;
    b.g.mutableNode(b.chB)->freqHz = kCentre - 300000.0;
    const pc::NodeId dA = b.g.addNode(pc::NodeKind::Demod, "dA", pc::PortType::Iq);
    const pc::NodeId dB = b.g.addNode(pc::NodeKind::Demod, "dB", pc::PortType::Iq);
    for (pc::NodeId d : {dA, dB}) {
        b.g.mutableNode(d)->mode = 2;
        b.g.mutableNode(d)->squelch = false;
    }
    b.sink = b.g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio);
    CHECK(b.g.connect(b.radio, 0, b.chA, 0) == pc::Connect::Ok);
    CHECK(b.g.connect(b.radio, 0, b.chB, 0) == pc::Connect::Ok);
    CHECK(b.g.connect(b.chA, 0, dA, 0) == pc::Connect::Ok);
    CHECK(b.g.connect(b.chB, 0, dB, 0) == pc::Connect::Ok);
    if (withMixer) {
        b.mixer = b.g.addNode(pc::NodeKind::Mixer, "Mixer", pc::PortType::Audio);
        b.g.mutableNode(b.mixer)->settings = mixerSettings;
        CHECK(b.g.connect(dA, 0, b.mixer, 0) == pc::Connect::Ok);
        CHECK(b.g.connect(dB, 0, b.mixer, 1) == pc::Connect::Ok);
        CHECK(b.g.connect(b.mixer, 0, b.sink, 0) == pc::Connect::Ok);
    } else {
        CHECK(b.g.connect(dA, 0, b.sink, 0) == pc::Connect::Ok);
    }
    return b;
}

// What the speaker is given for one second of the two carriers: the amplitude of the 1 kHz and 2 kHz tones in it (Goertzel over the last 0.5 s).
struct Heard {
    double a = 0.0, b = 0.0;
    std::size_t samples = 0;
};

double goertzel(const std::vector<float>& x, std::size_t from, double hz) {
    const double w = 2.0 * kPi * hz / 48000.0, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = from; i < x.size(); ++i) {
        const double s = static_cast<double>(x[i]) + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double n = static_cast<double>(x.size() - from);
    return 2.0 * std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

Heard run(const Built& b, std::string* signature = nullptr) {
    const pc::Plan plan = pc::compile(b.g, kRate, kCentre);
    auto cap = std::make_shared<Capture>();
    pc::DestTable dests;
    dests.emplace_back(b.sink, cap);
    if (signature != nullptr) { *signature = pc::radioSignature(plan, b.g, b.radio, kRate, nullptr, nullptr, dests); }
    std::shared_ptr<pc::StripSet> set = pc::buildRadioSet(plan, b.g, b.radio, kRate, nullptr, nullptr, dests);
    CHECK(set != nullptr);
    pc::Runner runner;
    runner.publish(set);
    const std::size_t block = 24000;
    for (int blk = 0; blk < 100; ++blk) {
        const std::vector<std::complex<float>> in = two(block, static_cast<std::size_t>(blk) * block);
        runner.process(in.data(), in.size());
    }
    Heard h;
    h.samples = cap->v.size();
    if (cap->v.size() > 30000) {
        const std::size_t from = cap->v.size() - 24000;
        h.a = goertzel(cap->v, from, 1000.0);
        h.b = goertzel(cap->v, from, 2000.0);
    }
    return h;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_patch_mixer_wire_a\n");

    std::printf("A: the settings text\n");
    {
        pc::MixerSettings m;
        std::string why;
        CHECK(pc::parseMixerSettings("", m, why) && m.gain[0] == 1.0f && !m.mute[3]);
        CHECK(pc::parseMixerSettings("gain=0.5,2;mute=0,0,1", m, why) && m.gain[0] == 0.5f && m.gain[1] == 2.0f && m.gain[2] == 1.0f && m.mute[2] && !m.mute[0]);
        for (const char* bad : {"gain=5", "gain=-1", "gain=a", "mute=2", "gain=1,1,1,1,1", "level=1", "gain", "mute=0,0,0,0,0"}) {
            pc::MixerSettings keep = m;
            CHECK(!pc::parseMixerSettings(bad, m, why) && !why.empty());
            CHECK(m == keep);
        }
    }

    std::printf("B: the plan\n");
    {
        const Built b = build("gain=1,1;mute=0,1");
        const pc::Plan plan = pc::compile(b.g, kRate, kCentre);
        CHECK(plan.sinks.size() == 1u && plan.sinks[0].mixChannels.size() == 2u);
        if (plan.sinks.size() == 1u && plan.sinks[0].mixChannels.size() == 2u) {
            CHECK(plan.sinks[0].mixChannels[0] == b.chA && plan.sinks[0].mixChannels[1] == b.chB);
            CHECK(plan.sinks[0].mixGains[0] == 1.0f && plan.sinks[0].mixGains[1] == 0.0f);   // muted: gain 0
        }
        for (const pc::NodeProblem& p : plan.problems) { CHECK(p.problem != pc::Problem::NodeNotRun); }
        const Built plain = build("", false);
        const pc::Plan pp = pc::compile(plain.g, kRate, kCentre);
        CHECK(pp.sinks.size() == 1u && pp.sinks[0].mixChannels.empty() && pp.sinks[0].channel == plain.chA);
        // a mixer wired to a scope or anything but a sink is not run
        pc::Graph g;
        const pc::NodeId mx = g.addNode(pc::NodeKind::Mixer, "M", pc::PortType::Audio);
        const pc::NodeId sc = g.addNode(pc::NodeKind::Scope, "S", pc::PortType::Audio);
        CHECK(g.connect(mx, 0, sc, 0) == pc::Connect::Ok);
        bool said = false;
        for (const pc::NodeProblem& p : pc::compile(g, kRate, kCentre).problems) { said = said || (p.node == mx && p.problem == pc::Problem::NodeNotRun); }
        CHECK(said);
    }

    std::printf("C: what the speaker is given\n");
    {
        const Heard one = run(build("", false));
        const Heard both = run(build(""));
        std::printf("  channel A alone: 1 kHz %.4f, 2 kHz %.4f; through the mixer: 1 kHz %.4f, 2 kHz %.4f (%zu samples)\n", one.a, one.b, both.a, both.b, both.samples);
        CHECK(one.samples > 30000 && one.a > 0.05 && one.b < 0.1 * one.a);   // alone: the 1 kHz tone, none of the 2 kHz
        CHECK(both.a > 0.05 && both.b > 0.05);                               // summed: both tones
        CHECK(std::fabs(both.a - one.a) < 0.1 * one.a);                       // the first is what it was alone
        const Heard half = run(build("gain=0.5,1"));
        CHECK(std::fabs(half.a - 0.5 * both.a) < 0.1 * both.a && std::fabs(half.b - both.b) < 0.1 * both.b);
        const Heard dbl = run(build("gain=1,2"));
        CHECK(std::fabs(dbl.b - 2.0 * both.b) < 0.1 * both.b && std::fabs(dbl.a - both.a) < 0.1 * both.a);
        const Heard muted = run(build("mute=1,0"));
        CHECK(muted.a < 0.1 * both.a && std::fabs(muted.b - both.b) < 0.1 * both.b);
        const Heard mutedB = run(build("mute=0,1"));
        CHECK(mutedB.b < 0.1 * both.b && std::fabs(mutedB.a - both.a) < 0.1 * both.a);
    }

    std::printf("D: the signature follows the mixer's settings\n");
    {
        auto sig = [&](const std::string& settings) {
            const Built b = build(settings);
            static auto cap = std::make_shared<Capture>();
            pc::DestTable dests;
            dests.emplace_back(b.sink, cap);
            return pc::radioSignature(pc::compile(b.g, kRate, kCentre), b.g, b.radio, kRate, nullptr, nullptr, dests);
        };
        CHECK(sig("gain=1,1") == sig("gain=1,1"));
        CHECK(sig("gain=1,1") != sig("gain=1,0.5"));
        CHECK(sig("gain=1,1") != sig("mute=0,1"));
    }
    return testSummary("test_patch_mixer_wire_a");
}
