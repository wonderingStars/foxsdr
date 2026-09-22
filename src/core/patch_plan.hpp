// patch_plan.hpp - turning a patch into what would actually be built, and
// naming everything that stops it.
//
// The graph says what is WIRED. That is not the same as what can RUN: a
// channel can be wired perfectly and sit outside the band the radio is
// capturing, or ask for a rate no whole-number decimation of the device rate
// can reach. Both draw a completely convincing patch and then decode nothing.
//
// So this is the bridge. It walks the graph and produces two things: the
// concrete strips a DSP would build, and a list of PROBLEMS, each pinned to
// the node it belongs to. The canvas marks those nodes and the inspector reads
// the reason out. Nothing here runs any DSP or touches a thread - it is a pure
// function of the graph and two numbers, so tests/test_patch_plan.cpp can pin
// every case with expected answers.
//
// WHY THE PROBLEMS MATTER MORE THAN THE PLAN, for now. Nothing executes a plan
// yet. But a patch that cannot work is worth saying so about immediately, at
// the node responsible, rather than after the user has waited for silence and
// started doubting the aerial. Every failure this reports is one that is
// otherwise indistinguishable from a quiet band.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_PLAN_HPP
#define CASCADE_CORE_PATCH_PLAN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/patch_graph.hpp"

namespace cascade::core::patch {

// The rate a demodulated audio chain is built at. The whole product works at
// 48 kHz (Pipeline::kAudioRateHz), and the decoders that ride on it are
// written against it.
inline constexpr double kChannelRateHz = 48000.0;

// How far inside the captured band a channel must sit. The edges of a
// decimating filter are not usable, so a channel exactly at Nyquist is a
// channel that hears the filter roll-off rather than the signal.
inline constexpr double kUsableBandFraction = 0.90;

enum class Problem : std::uint8_t {
    NothingFeedsIt,    // a node with an input and no wire into it
    NotFedByARadio,    // nothing upstream is a Radio, so there are no samples
    NoFrequency,       // a Channel that has not been told where to sit
    OutOfBand,         // a Channel outside what the radio is capturing
    RateUnreachable,   // no whole decimation of the device rate reaches 48 kHz
    NothingListens,    // produces something nobody consumes - advisory only
};

// Advisory problems do not stop a patch running; they are worth saying and not
// worth refusing. A decoder wired to nothing still decodes, it just has no
// window - that is a thing people do deliberately while building a patch.
inline bool isAdvisory(Problem p) { return p == Problem::NothingListens; }

inline const char* problemText(Problem p) {
    switch (p) {
        case Problem::NothingFeedsIt: return "nothing feeds this";
        case Problem::NotFedByARadio: return "no radio upstream of this";
        case Problem::NoFrequency: return "no frequency set";
        case Problem::OutOfBand: return "outside the band the radio is receiving";
        case Problem::RateUnreachable: return "this sample rate cannot reach 48 kHz";
        case Problem::NothingListens: return "nothing is listening to this";
    }
    return "this cannot run";
}

struct NodeProblem {
    NodeId node = kNoNode;
    Problem problem = Problem::NothingFeedsIt;
};

struct ChannelPlan {
    NodeId node = kNoNode;
    double offsetHz = 0.0;   // from the radio's centre, signed
    unsigned decimation = 0;
    double outRateHz = 0.0;
};

struct Plan {
    std::vector<NodeId> order;
    std::vector<ChannelPlan> channels;
    std::vector<NodeProblem> problems;
    bool runnable = false;   // nothing blocking, and something to actually do
};

// The whole decimation from `deviceRateHz` to kChannelRateHz, or 0 when there
// is none.
//
// EXACT, NOT NEAREST. acars::Demod and every other bit-clocked decoder in this
// product treats its symbol phases as sample offsets within a bit, so a
// fractional samples-per-bit does not degrade the decode, it walks off the
// bit. 2.4 MS/s gives 50, 1.2 gives 25, 0.24 gives 5; 2.048 MS/s reaches
// 42.667 and is refused rather than approximated.
inline unsigned wholeDecimation(double deviceRateHz, double targetHz = kChannelRateHz) {
    if (!(deviceRateHz > 0.0) || !(targetHz > 0.0)) { return 0; }
    const double exact = deviceRateHz / targetHz;
    const double rounded = std::floor(exact + 0.5);
    if (rounded < 1.0) { return 0; }
    // One part in a million: a device that reports 2399999.9 for 2.4 MS/s is
    // reporting 2.4 MS/s, and refusing it would be pedantry rather than care.
    if (std::fabs(exact - rounded) > 1e-6 * rounded) { return 0; }
    return static_cast<unsigned>(rounded);
}

inline Plan compile(const Graph& g, double deviceRateHz, double radioCentreHz) {
    Plan plan;
    plan.order = g.evaluationOrder();

    const double halfBand = 0.5 * deviceRateHz * kUsableBandFraction;

    // Which nodes have a radio somewhere upstream. Walked in evaluation order,
    // so a node's feeders are always decided before it is - that ordering is
    // the whole reason evaluationOrder exists.
    std::vector<NodeId> fed;
    const auto isFed = [&fed](NodeId id) {
        return std::find(fed.begin(), fed.end(), id) != fed.end();
    };

    for (const NodeId id : plan.order) {
        const Node* n = g.find(id);
        if (n == nullptr) { continue; }

        if (n->kind == NodeKind::Radio) {
            fed.push_back(id);
            continue;
        }

        // Does anything wire into it at all, and is any of that fed?
        bool anyInput = false;
        bool anyFedInput = false;
        for (const Wire& w : g.wires()) {
            if (w.to != id) { continue; }
            anyInput = true;
            if (isFed(w.from)) { anyFedInput = true; }
        }

        if (!n->inputs.empty() && !anyInput) {
            plan.problems.push_back({id, Problem::NothingFeedsIt});
        } else if (!anyFedInput) {
            // Wired, but to something that is not itself fed - a chain hanging
            // off nothing. Reported once, here, rather than at every node
            // along it, because the node the user has to fix is this one.
            plan.problems.push_back({id, Problem::NotFedByARadio});
        } else {
            fed.push_back(id);
        }
    }

    // Channels: where they sit, and whether the rate can get there.
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Channel) { continue; }
        if (!isFed(n.id)) { continue; }  // already reported above

        if (n.freqHz <= 0.0) {
            plan.problems.push_back({n.id, Problem::NoFrequency});
            continue;
        }
        const double offset = n.freqHz - radioCentreHz;
        if (std::fabs(offset) > halfBand) {
            plan.problems.push_back({n.id, Problem::OutOfBand});
            continue;
        }
        const unsigned dec = wholeDecimation(deviceRateHz);
        if (dec == 0) {
            plan.problems.push_back({n.id, Problem::RateUnreachable});
            continue;
        }
        plan.channels.push_back({n.id, offset, dec, deviceRateHz / dec});
    }

    // Anything that produces and is heard by nobody. Advisory.
    for (const Node& n : g.nodes()) {
        if (n.outputs.empty()) { continue; }
        bool heard = false;
        for (const Wire& w : g.wires()) {
            if (w.from == n.id) {
                heard = true;
                break;
            }
        }
        if (!heard) { plan.problems.push_back({n.id, Problem::NothingListens}); }
    }

    const bool blocked = std::any_of(plan.problems.begin(), plan.problems.end(),
                                     [](const NodeProblem& p) { return !isAdvisory(p.problem); });
    // A patch with nothing in it is not "runnable" - there is nothing to run.
    // Saying otherwise would put a green light on an empty canvas.
    const bool anySink = std::any_of(g.nodes().begin(), g.nodes().end(), [](const Node& n) {
        return n.kind == NodeKind::Sink || n.kind == NodeKind::Display;
    });
    plan.runnable = !blocked && anySink;
    return plan;
}

// Every problem pinned to one node, for the canvas and the inspector.
inline std::vector<Problem> problemsFor(const Plan& plan, NodeId id) {
    std::vector<Problem> out;
    for (const NodeProblem& p : plan.problems) {
        if (p.node == id) { out.push_back(p.problem); }
    }
    return out;
}

inline bool hasBlockingProblem(const Plan& plan, NodeId id) {
    for (const NodeProblem& p : plan.problems) {
        if (p.node == id && !isAdvisory(p.problem)) { return true; }
    }
    return false;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_PLAN_HPP
