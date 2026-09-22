// Tests for core/patch_plan.hpp - what a patch would actually build, and
// everything that stops it.
//
// Every problem reported here describes a patch that LOOKS right. A channel
// outside the captured band, a decoder hanging off nothing, a device rate that
// cannot be divided down to 48 kHz - each one draws a perfectly convincing
// picture and then decodes nothing, which is indistinguishable from a quiet
// band. That is why they are worth naming, and why the naming is pinned here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_plan.hpp"

#include <algorithm>
#include <vector>

#include "core/patch_graph.hpp"
#include "test_check.hpp"

using cascade::core::patch::ChannelPlan;
using cascade::core::patch::compile;
using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::hasBlockingProblem;
using cascade::core::patch::isAdvisory;
using cascade::core::patch::kChannelRateHz;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::Plan;
using cascade::core::patch::PortType;
using cascade::core::patch::Problem;
using cascade::core::patch::problemsFor;
using cascade::core::patch::problemText;
using cascade::core::patch::wholeDecimation;

namespace {

constexpr double kRate = 2400000.0;    // 2.4 MS/s, which divides to 48 kHz
constexpr double kCentre = 131000000.0;

bool has(const Plan& p, NodeId id, Problem want) {
    const std::vector<Problem> got = problemsFor(p, id);
    return std::find(got.begin(), got.end(), want) != got.end();
}

// Count only the problems that stop a patch, so an advisory does not make a
// test about blocking failures look wrong.
std::size_t blocking(const Plan& p) {
    std::size_t n = 0;
    for (const auto& np : p.problems) {
        if (!isAdvisory(np.problem)) { ++n; }
    }
    return n;
}

// A radio, a channel on frequency, a demod and a speaker: the smallest patch
// that should simply work.
struct Working {
    Graph g;
    NodeId radio, chan, demod, spk;

    Working() {
        radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        chan = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        demod = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        g.mutableNode(chan)->freqHz = kCentre + 200000.0;
        g.connect(radio, 0, chan, 0);
        g.connect(chan, 0, demod, 0);
        g.connect(demod, 0, spk, 0);
    }
};

}  // namespace

int main() {
    // [1] The decimation is EXACT or it is refused. A bit-clocked decoder
    // treats its symbol phases as sample offsets within a bit, so a fractional
    // samples-per-bit does not degrade the decode, it walks off the bit.
    {
        CHECK(wholeDecimation(2400000.0) == 50u);
        CHECK(wholeDecimation(1200000.0) == 25u);
        CHECK(wholeDecimation(240000.0) == 5u);
        CHECK(wholeDecimation(48000.0) == 1u);

        // 2.048 MS/s reaches 42.667 and must be refused rather than rounded.
        CHECK(wholeDecimation(2048000.0) == 0u);
        CHECK(wholeDecimation(1000000.0) == 0u);

        // A device reporting 2399999.9 for 2.4 MS/s is reporting 2.4 MS/s;
        // refusing that would be pedantry rather than care.
        CHECK(wholeDecimation(2399999.9) == 50u);
        // But a rate that is genuinely a hundred hertz out is not.
        CHECK(wholeDecimation(2400100.0) == 0u);

        // Nonsense in, zero out - never a divide by zero or a huge count.
        CHECK(wholeDecimation(0.0) == 0u);
        CHECK(wholeDecimation(-2400000.0) == 0u);
        CHECK(wholeDecimation(2400000.0, 0.0) == 0u);
        CHECK(wholeDecimation(1000.0) == 0u);   // below the target entirely
    }

    // [2] The patch that should simply work, does - and says so.
    {
        Working w;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
        CHECK(p.channels.size() == 1u);
        CHECK(p.channels[0].node == w.chan);
        CHECK_NEAR(p.channels[0].offsetHz, 200000.0, 0.001);
        CHECK(p.channels[0].decimation == 50u);
        CHECK_NEAR(p.channels[0].outRateHz, kChannelRateHz, 0.001);
        CHECK(p.order.size() == w.g.nodes().size());
    }

    // [3] A channel with no frequency. It has been placed and wired and never
    // told where to sit, which is the most ordinary half-finished state there
    // is - and a channel at 0 Hz would otherwise plan an offset of minus the
    // whole centre frequency.
    {
        Working w;
        w.g.mutableNode(w.chan)->freqHz = 0.0;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, w.chan, Problem::NoFrequency));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);
    }

    // [4] A channel outside the band the radio is capturing. The usable band
    // is deliberately less than the full width: the edges are filter
    // roll-off, so a channel exactly at Nyquist hears the filter rather than
    // the signal.
    {
        Working w;
        // Comfortably inside.
        w.g.mutableNode(w.chan)->freqHz = kCentre + 1000000.0;
        CHECK(blocking(compile(w.g, kRate, kCentre)) == 0u);

        // Past the usable edge, though still inside the raw half-rate.
        w.g.mutableNode(w.chan)->freqHz = kCentre + 1190000.0;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, w.chan, Problem::OutOfBand));
        CHECK(p.channels.empty());

        // ...and the same distance the other way, which a signed comparison
        // would happily miss.
        w.g.mutableNode(w.chan)->freqHz = kCentre - 1190000.0;
        CHECK(has(compile(w.g, kRate, kCentre), w.chan, Problem::OutOfBand));
    }

    // [5] A device rate that cannot be divided down. This is the real
    // 2.048 MS/s case, and it is a property of the RATE, so it lands on the
    // channel that cannot be built rather than nowhere.
    {
        Working w;
        const Plan p = compile(w.g, 2048000.0, kCentre);
        CHECK(has(p, w.chan, Problem::RateUnreachable));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);

        // The same patch at a rate that divides is fine, which proves the
        // refusal was about the rate and not about the patch.
        CHECK(blocking(compile(w.g, 2400000.0, kCentre)) == 0u);
    }

    // [6] Nothing feeds it. A node placed and never wired.
    {
        Graph g;
        const NodeId lonely = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        g.mutableNode(lonely)->freqHz = kCentre;
        const Plan p = compile(g, kRate, kCentre);
        CHECK(has(p, lonely, Problem::NothingFeedsIt));
        CHECK(!p.runnable);

        // A Radio has no inputs, so it is never accused of this.
        Graph r;
        const NodeId radio = r.addNode(NodeKind::Radio, "R", PortType::Iq);
        CHECK(!has(compile(r, kRate, kCentre), radio, Problem::NothingFeedsIt));
    }

    // [7] Wired, but to a chain that starts nowhere. Every node in it is
    // wired, so "nothing feeds it" would be a lie; the honest answer is that
    // no radio is upstream.
    {
        Graph g;
        const NodeId chan = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        const NodeId demod = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        g.mutableNode(chan)->freqHz = kCentre;
        CHECK(g.connect(chan, 0, demod, 0) == Connect::Ok);
        CHECK(g.connect(demod, 0, spk, 0) == Connect::Ok);

        const Plan p = compile(g, kRate, kCentre);
        // The channel is the node to fix, so that is where the complaint goes.
        CHECK(has(p, chan, Problem::NothingFeedsIt));
        CHECK(has(p, demod, Problem::NotFedByARadio));
        CHECK(has(p, spk, Problem::NotFedByARadio));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);
    }

    // [8] Producing something nobody consumes is ADVISORY, not blocking -
    // people leave a decoder unwired while they are still building.
    {
        Working w;
        const NodeId spare = w.g.addNode(NodeKind::Channel, "spare", PortType::Iq);
        w.g.mutableNode(spare)->freqHz = kCentre + 100000.0;
        CHECK(w.g.connect(w.radio, 0, spare, 0) == Connect::Ok);

        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, spare, Problem::NothingListens));
        CHECK(isAdvisory(Problem::NothingListens));
        CHECK(!hasBlockingProblem(p, spare));
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);                  // an advisory does not stop it
        CHECK(p.channels.size() == 2u);     // and the strip is still planned
    }

    // [9] An empty patch is not "runnable". A green light over an empty canvas
    // would be the most confident possible way of saying nothing.
    {
        Graph g;
        const Plan p = compile(g, kRate, kCentre);
        CHECK(!p.runnable);
        CHECK(p.problems.empty());
        CHECK(p.channels.empty());
        CHECK(p.order.empty());

        // A radio on its own is not runnable either - nothing consumes it.
        Graph r;
        r.addNode(NodeKind::Radio, "R", PortType::Iq);
        CHECK(!compile(r, kRate, kCentre).runnable);
    }

    // [10] The per-node lookups the canvas and the inspector use.
    {
        Working w;
        w.g.mutableNode(w.chan)->freqHz = 0.0;
        const Plan p = compile(w.g, kRate, kCentre);

        CHECK(problemsFor(p, w.chan).size() == 1u);
        CHECK(hasBlockingProblem(p, w.chan));
        CHECK(!hasBlockingProblem(p, w.radio));
        CHECK(problemsFor(p, 9999u).empty());
        CHECK(!hasBlockingProblem(p, 9999u));
    }

    // [11] Every problem has words, and they are distinct - a message shared
    // between two causes tells the user the wrong thing about one of them.
    {
        const Problem all[] = {Problem::NothingFeedsIt, Problem::NotFedByARadio,
                               Problem::NoFrequency,    Problem::OutOfBand,
                               Problem::RateUnreachable, Problem::NothingListens};
        std::vector<std::string> seen;
        for (const Problem p : all) {
            const char* t = problemText(p);
            CHECK(t != nullptr && t[0] != '\0');
            seen.emplace_back(t);
        }
        std::sort(seen.begin(), seen.end());
        CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    }

    // [12] Several channels off one radio, which is the arrangement the whole
    // page exists for. Each gets its own offset and they do not interfere.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        const double freqs[] = {130025000.0, 130450000.0, 131725000.0};
        for (const double f : freqs) {
            const NodeId ch = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
            const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
            const NodeId dec = g.addNode(NodeKind::Decoder, "ACARS", PortType::Audio);
            g.mutableNode(ch)->freqHz = f;
            CHECK(g.connect(radio, 0, ch, 0) == Connect::Ok);
            CHECK(g.connect(ch, 0, dm, 0) == Connect::Ok);
            CHECK(g.connect(dm, 0, dec, 0) == Connect::Ok);
            CHECK(g.connect(dec, 0, text, 0) == Connect::Ok);
        }

        const Plan p = compile(g, kRate, 130875000.0);
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
        CHECK(p.channels.size() == 3u);
        for (const ChannelPlan& c : p.channels) {
            CHECK(c.decimation == 50u);
            CHECK(std::fabs(c.offsetHz) <= 0.5 * kRate);
        }
    }

    return testSummary("test_patch_plan");
}
