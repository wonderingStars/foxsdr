// test_patch_iffilter_wire_a.cpp - THE IF FILTER PART RUNS (wire-a second pass, item 5, second part): Radio -> IF filter -> Channel -> Demod in a real strip set driven by the real runner.
//   A  the settings text: what parses, what is refused, and how;  B  a channel behind an IF filter whose pass-band holds it hears what it heard without the filter, one whose pass-band does not hold it hears next to nothing,
//   two filters in a row both have to pass it, a bypassed filter is out of the path (Graph::effective);  C  the set's signature follows the settings (so a change rebuilds the set);  D  the plan: a filter that feeds
//   channels is run, one wired to anything else still says it is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

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
constexpr double kChannel = kCentre + 200000.0;

std::vector<std::complex<float>> tone(double offsetHz, std::size_t n, std::size_t from) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(from + i) / kRate;
        const double env = 1.0 + 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        const double ph = 2.0 * kPi * offsetHz * t;
        out[i] = std::complex<float>(static_cast<float>(env * std::cos(ph)), static_cast<float>(env * std::sin(ph)));
    }
    return out;
}

struct Built {
    pc::Graph g;
    pc::NodeId radio = pc::kNoNode, chan = pc::kNoNode;
    std::vector<pc::NodeId> filters;
};

// Radio -> [IF filters with the given settings] -> Channel (200 kHz above the centre) -> AM demodulator -> speaker.
Built build(const std::vector<std::string>& filterSettings) {
    Built b;
    b.radio = b.g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq);
    pc::NodeId from = b.radio;
    for (const std::string& s : filterSettings) {
        const pc::NodeId f = b.g.addNode(pc::NodeKind::IfFilter, "IF", pc::PortType::Iq);
        b.g.mutableNode(f)->settings = s;
        CHECK(b.g.connect(from, 0, f, 0) == pc::Connect::Ok);
        from = f;
        b.filters.push_back(f);
    }
    b.chan = b.g.addNode(pc::NodeKind::Channel, "Chan", pc::PortType::Iq);
    b.g.mutableNode(b.chan)->freqHz = kChannel;
    CHECK(b.g.connect(from, 0, b.chan, 0) == pc::Connect::Ok);
    const pc::NodeId dem = b.g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq);
    b.g.mutableNode(dem)->mode = 2;   // AM
    b.g.mutableNode(dem)->squelch = false;
    CHECK(b.g.connect(b.chan, 0, dem, 0) == pc::Connect::Ok);
    const pc::NodeId sink = b.g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio);
    CHECK(b.g.connect(dem, 0, sink, 0) == pc::Connect::Ok);
    return b;
}

// The mean size of the channel's demodulated audio over the last blocks of a run of the tone 200 kHz above the centre.
double heard(const pc::Graph& graph, const pc::Graph& effective) {
    (void)graph;
    const pc::Plan plan = pc::compile(effective, kRate, kCentre);
    std::shared_ptr<pc::StripSet> set = pc::buildStripSet(plan, effective, kRate, pc::kNoNode);
    CHECK(set != nullptr && set->channels.size() == 1u);
    pc::Runner runner;
    runner.publish(set);
    const std::size_t block = 24000;
    double sum = 0.0;
    std::size_t count = 0;
    for (int blk = 0; blk < 12; ++blk) {
        const std::vector<std::complex<float>> in = tone(200000.0, block, static_cast<std::size_t>(blk) * block);
        runner.process(in.data(), in.size());
        if (blk >= 8) {
            const pc::RunningChannel& rc = set->channels[0];
            for (std::size_t i = 0; i < rc.produced; ++i) { sum += std::fabs(static_cast<double>(rc.audio[i])); ++count; }
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

std::string inBand() { return "low=" + std::to_string(static_cast<long long>(kCentre + 150000.0)) + ";high=" + std::to_string(static_cast<long long>(kCentre + 250000.0)); }
std::string outBand() { return "low=" + std::to_string(static_cast<long long>(kCentre - 500000.0)) + ";high=" + std::to_string(static_cast<long long>(kCentre - 400000.0)); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_patch_iffilter_wire_a\n");

    std::printf("A: the settings text\n");
    {
        pc::IfSettings s;
        std::string why;
        CHECK(pc::parseIfSettings("", s, why) && !s.band && s.notches.empty());
        CHECK(pc::parseIfSettings("low=7000000;high=7003000", s, why) && s.band && s.lowHz == 7000000.0 && s.highHz == 7003000.0);
        CHECK(pc::parseIfSettings("notch=7001000/50,7002500/100;low=7000000;high=7003000", s, why) && s.notches.size() == 2u && s.notches[1].rfHz == 7002500.0 && s.notches[1].widthHz == 100.0);
        CHECK(pc::parseIfSettings("notch=7001000/50", s, why) && !s.band && s.notches.size() == 1u);
        const char* bad[] = {"low=7000000", "high=7003000", "low=7003000;high=7000000", "low=7000000;high=7000005", "low=abc;high=7000000", "low=-5;high=7000000", "notch=7001000", "notch=7001000/2", "notch=x/50",
                             "colour=red", "low", "notch=1/50,2/50,3/50,4/50,5/50,6/50,7/50,8/50,9/50"};
        for (const char* t : bad) {
            pc::IfSettings keep = s;
            const bool ok = pc::parseIfSettings(t, s, why);
            if (ok) { std::printf("  accepted: %s\n", t); }
            CHECK(!ok && !why.empty());
            CHECK(s == keep);   // a refusal leaves the result alone
        }
        CHECK(pc::parseIfSettings("notch=1/50,2/50,3/50,4/50,5/50,6/50,7/50,8/50", s, why) && s.notches.size() == 8u);   // eight is the most
    }

    std::printf("B: a channel behind an IF filter\n");
    {
        const Built plain = build({});
        const double base = heard(plain.g, plain.g.effective());
        const Built pass = build({inBand()});
        const double through = heard(pass.g, pass.g.effective());
        const Built stop = build({outBand()});
        const double blocked = heard(stop.g, stop.g.effective());
        std::printf("  no filter %.4f, pass-band holding the channel %.4f, pass-band elsewhere %.5f\n", base, through, blocked);
        CHECK(base > 0.2);
        CHECK(through > 0.8 * base && through < 1.2 * base);
        CHECK(blocked < 0.02 * base);
        // two in a row: both must pass it
        const Built chain = build({inBand(), outBand()});
        CHECK(heard(chain.g, chain.g.effective()) < 0.02 * base);
        const Built chain2 = build({inBand(), "low=" + std::to_string(static_cast<long long>(kCentre + 100000.0)) + ";high=" + std::to_string(static_cast<long long>(kCentre + 300000.0))});
        const double both = heard(chain2.g, chain2.g.effective());
        CHECK(both > 0.8 * base);
        // an empty filter and an unreadable one pass the samples as they are
        const Built empty = build({""});
        CHECK(std::fabs(heard(empty.g, empty.g.effective()) - base) < 0.02 * base);
        const Built junk = build({"colour=red"});
        CHECK(std::fabs(heard(junk.g, junk.g.effective()) - base) < 0.02 * base);
        // BYPASS: the blocking filter switched out of the path is not there, and the document still holds it
        Built byp = build({outBand()});
        CHECK(byp.g.setBypass(byp.filters[0], true));
        const double bypassed = heard(byp.g, byp.g.effective());
        CHECK(bypassed > 0.8 * base);
        CHECK(byp.g.find(byp.filters[0]) != nullptr);
    }

    std::printf("C: the set's signature follows the settings\n");
    {
        const Built a = build({inBand()});
        Built b = build({inBand()});
        const pc::Plan pa = pc::compile(a.g, kRate, kCentre);
        const std::string sa = pc::dspSignature(pa, a.g, kRate, pc::kNoNode, 48000.0, nullptr, nullptr);
        CHECK(sa == pc::dspSignature(pc::compile(b.g, kRate, kCentre), b.g, kRate, pc::kNoNode, 48000.0, nullptr, nullptr));
        b.g.mutableNode(b.filters[0])->settings = outBand();
        CHECK(sa != pc::dspSignature(pc::compile(b.g, kRate, kCentre), b.g, kRate, pc::kNoNode, 48000.0, nullptr, nullptr));
    }

    std::printf("D: the plan\n");
    {
        const Built a = build({inBand()});
        for (const pc::NodeProblem& p : pc::compile(a.g, kRate, kCentre).problems) { CHECK(p.problem != pc::Problem::NodeNotRun); }
        // a filter wired to a display is not run
        pc::Graph g;
        const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "R", pc::PortType::Iq);
        const pc::NodeId f = g.addNode(pc::NodeKind::IfFilter, "IF", pc::PortType::Iq);
        const pc::NodeId disp = g.addNode(pc::NodeKind::Display, "D", pc::PortType::Iq);
        CHECK(g.connect(radio, 0, f, 0) == pc::Connect::Ok && g.connect(f, 0, disp, 0) == pc::Connect::Ok);
        bool said = false;
        for (const pc::NodeProblem& p : pc::compile(g, kRate, kCentre).problems) { said = said || (p.node == f && p.problem == pc::Problem::NodeNotRun); }
        CHECK(said);
    }
    return testSummary("test_patch_iffilter_wire_a");
}
