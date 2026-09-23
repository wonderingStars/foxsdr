// Tests for core/patch_graph.hpp - the rules a patch obeys, with nothing drawn.
//
// The whole value of a patcher is that the picture and the signal path are the
// same thing. Every test here is aimed at the one way that stops being true: a
// connection the canvas would happily draw and the DSP could not honour. Those
// are silent by nature - a wire into the wrong port type decodes nothing, a
// second wire into one input picks a winner nobody chose, a loop starves the
// audio thread - so each is refused with a NAMED reason and each reason is
// pinned here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_graph.hpp"

#include <algorithm>
#include <vector>

#include "test_check.hpp"

using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::kNoNode;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::PortIndex;
using cascade::core::patch::PortType;
using cascade::core::patch::switchAllRadiosOff;
using cascade::core::patch::switchOnForStart;
using cascade::core::patch::Wire;

namespace {

// Where a node came out in the evaluation order. Returns -1 when absent, which
// is a real answer and not an error: a node dropped from the order is exactly
// what a cycle looks like.
int positionOf(const std::vector<NodeId>& order, NodeId id) {
    const auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

// The property the order exists to guarantee, asked of the whole graph rather
// than of one pair: nothing is evaluated before something that feeds it.
bool everyFeederComesFirst(const Graph& g) {
    const std::vector<NodeId> order = g.evaluationOrder();
    for (const Wire& w : g.wires()) {
        const int a = positionOf(order, w.from);
        const int b = positionOf(order, w.to);
        if (a < 0 || b < 0 || a >= b) { return false; }
    }
    return true;
}

// The patch from the mockup, built twice in the tests below: one radio feeding
// two ACARS channels and a spectrum, the two decoders merging into one text
// sink.
struct AcarsPatch {
    Graph g;
    NodeId radio, ch1, ch2, spectrum, dec1, dec2, text;

    AcarsPatch() {
        radio = g.addNode(NodeKind::Radio, "Radio A");
        ch1 = g.addNode(NodeKind::Channel, "131.725");
        ch2 = g.addNode(NodeKind::Channel, "130.025");
        spectrum = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
        dec1 = g.addNode(NodeKind::Decoder, "ACARS 1", PortType::Audio);
        dec2 = g.addNode(NodeKind::Decoder, "ACARS 2", PortType::Audio);
        text = g.addNode(NodeKind::Sink, "Text out", PortType::Text);
    }
};

}  // namespace

int main() {
    // [1] The port tables. A node kind whose ports are wrong is wrong
    // everywhere at once, so they are stated rather than inferred.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "R");
        const NodeId chan = g.addNode(NodeKind::Channel, "C");
        const NodeId demod = g.addNode(NodeKind::Demod, "D");
        const NodeId iqDec = g.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq);
        const NodeId auDec = g.addNode(NodeKind::Decoder, "ACARS", PortType::Audio);

        CHECK(g.find(radio)->inputs.empty());
        CHECK(g.find(radio)->outputs == std::vector<PortType>{PortType::Iq});

        CHECK(g.find(chan)->inputs == std::vector<PortType>{PortType::Iq});
        CHECK(g.find(chan)->outputs == std::vector<PortType>{PortType::Iq});

        CHECK(g.find(demod)->inputs == std::vector<PortType>{PortType::Iq});
        CHECK(g.find(demod)->outputs == std::vector<PortType>{PortType::Audio});

        // The ABI's own distinction: an I/Q decoder and an audio decoder are
        // not interchangeable, and the graph must know which it has.
        CHECK(g.find(iqDec)->inputs == std::vector<PortType>{PortType::Iq});
        CHECK(g.find(auDec)->inputs == std::vector<PortType>{PortType::Audio});
        // Text, and (0.99.18) its map targets for a Map part - output 0 stays
        // Text, so every patch saved before a decoder had a map output keeps
        // its wires.
        CHECK(g.find(iqDec)->outputs == std::vector<PortType>({PortType::Text, PortType::Track}));
    }

    // [MAP] A MAP TAKES UP TO FIVE DECODERS (0.99.18): five Track inputs, one
    // wire each, from decoders' map outputs - never from their text output,
    // and never a sixth.
    {
        Graph g;
        const NodeId map = g.addNode(NodeKind::Map, "Map", PortType::Track);
        CHECK(g.find(map)->inputs ==
              std::vector<PortType>(cascade::core::patch::kMapInputs, PortType::Track));
        CHECK(g.find(map)->outputs.empty());
        NodeId decs[6] = {};
        for (int i = 0; i < 6; ++i) {
            decs[i] = g.addNode(NodeKind::Decoder, "D", PortType::Iq);
        }
        CHECK(g.connect(decs[0], 0, map, 0) == Connect::TypeMismatch);   // text is not a target
        for (PortIndex i = 0; i < 5; ++i) {
            CHECK(g.connect(decs[i], 1, map, i) == Connect::Ok);
        }
        CHECK(g.connect(decs[5], 1, map, 0) == Connect::InputOccupied);   // one wire per input
        CHECK(g.connect(decs[5], 1, map, 5) == Connect::NoSuchPort);      // and there is no sixth
        CHECK(g.wires().size() == 5u);
    }

    // [2] A type mismatch is refused, and named. This is the one a canvas would
    // otherwise draw perfectly and the decoder would answer with silence.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "R");
        const NodeId acars = g.addNode(NodeKind::Decoder, "ACARS", PortType::Audio);
        CHECK(g.connect(radio, 0, acars, 0) == Connect::TypeMismatch);
        CHECK(g.wires().empty());

        // ...and the same pair through a demod is fine, which proves the
        // refusal was about the TYPE and not about those two nodes.
        const NodeId demod = g.addNode(NodeKind::Demod, "AM");
        CHECK(g.connect(radio, 0, demod, 0) == Connect::Ok);
        CHECK(g.connect(demod, 0, acars, 0) == Connect::Ok);
        CHECK(g.wires().size() == 2u);
    }

    // [3] An output fans out; an input does not fan in.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "R");
        const NodeId a = g.addNode(NodeKind::Channel, "A");
        const NodeId b = g.addNode(NodeKind::Channel, "B");
        const NodeId c = g.addNode(NodeKind::Channel, "C");

        CHECK(g.connect(radio, 0, a, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, b, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, c, 0) == Connect::Ok);
        CHECK(g.wires().size() == 3u);

        // Two sources into one input is not a mix; it is an argument about
        // which one wins.
        CHECK(g.connect(a, 0, c, 0) == Connect::InputOccupied);
        CHECK(g.wires().size() == 3u);
    }

    // [4] Duplicates, self-loops, unknown nodes and bad ports each have their
    // own reason, because "it did not connect" is not an answer a user can act
    // on.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "R");
        const NodeId chan = g.addNode(NodeKind::Channel, "C");

        CHECK(g.connect(radio, 0, chan, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, chan, 0) == Connect::AlreadyWired);
        CHECK(g.connect(chan, 0, chan, 0) == Connect::SelfLoop);
        CHECK(g.connect(9999u, 0, chan, 0) == Connect::UnknownNode);
        CHECK(g.connect(radio, 0, 9999u, 0) == Connect::UnknownNode);
        CHECK(g.connect(radio, 7, chan, 0) == Connect::NoSuchPort);
        CHECK(g.connect(radio, 0, chan, 7) == Connect::NoSuchPort);
        // A radio has no inputs at all, so nothing may be wired INTO one.
        CHECK(g.connect(chan, 0, radio, 0) == Connect::NoSuchPort);
        CHECK(g.wires().size() == 1u);
    }

    // [5] A loop is refused before it exists. A cycle in a running DSP graph
    // does not draw badly, it starves the audio thread.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Channel, "A");
        const NodeId b = g.addNode(NodeKind::Channel, "B");
        const NodeId c = g.addNode(NodeKind::Channel, "C");
        CHECK(g.connect(a, 0, b, 0) == Connect::Ok);
        CHECK(g.connect(b, 0, c, 0) == Connect::Ok);

        CHECK(g.connect(c, 0, a, 0) == Connect::WouldCycle);
        CHECK(g.wires().size() == 2u);

        // The graph is still usable and still ordered afterwards - a refused
        // wire must leave nothing behind.
        CHECK(everyFeederComesFirst(g));
        CHECK(g.evaluationOrder().size() == 3u);
    }

    // [6] Deleting a node takes its wires with it, and never leaves one
    // pointing at a hole.
    {
        AcarsPatch p;
        CHECK(p.g.connect(p.radio, 0, p.ch1, 0) == Connect::Ok);
        CHECK(p.g.connect(p.radio, 0, p.ch2, 0) == Connect::Ok);
        CHECK(p.g.connect(p.radio, 0, p.spectrum, 0) == Connect::Ok);
        CHECK(p.g.wires().size() == 3u);

        CHECK(p.g.removeNode(p.ch1));
        CHECK(p.g.wires().size() == 2u);
        for (const Wire& w : p.g.wires()) {
            CHECK(p.g.find(w.from) != nullptr);
            CHECK(p.g.find(w.to) != nullptr);
        }

        CHECK(!p.g.removeNode(p.ch1));    // already gone
        CHECK(!p.g.removeNode(kNoNode));  // and the invalid id is not a node
    }

    // [7] Ids are never reused, so a stale reference cannot silently become a
    // reference to a different node.
    {
        Graph g;
        const NodeId first = g.addNode(NodeKind::Channel, "A");
        CHECK(g.removeNode(first));
        const NodeId second = g.addNode(NodeKind::Channel, "B");
        CHECK(second != first);
        CHECK(g.find(first) == nullptr);
    }

    // [8] disconnect removes exactly the wire named, and reports a miss.
    {
        Graph g;
        const NodeId r = g.addNode(NodeKind::Radio, "R");
        const NodeId a = g.addNode(NodeKind::Channel, "A");
        const NodeId b = g.addNode(NodeKind::Channel, "B");
        CHECK(g.connect(r, 0, a, 0) == Connect::Ok);
        CHECK(g.connect(r, 0, b, 0) == Connect::Ok);

        CHECK(g.disconnect(Wire{r, 0, a, 0}));
        CHECK(g.wires().size() == 1u);
        CHECK(!g.disconnect(Wire{r, 0, a, 0}));
        CHECK(g.wires()[0].to == b);

        // The input it freed accepts a wire again.
        CHECK(g.connect(r, 0, a, 0) == Connect::Ok);
    }

    // [9] The real patch from the mockup: the order is valid, complete, and
    // puts the radio first and the text sink last.
    {
        AcarsPatch p;
        CHECK(p.g.connect(p.radio, 0, p.ch1, 0) == Connect::Ok);
        CHECK(p.g.connect(p.radio, 0, p.ch2, 0) == Connect::Ok);
        CHECK(p.g.connect(p.radio, 0, p.spectrum, 0) == Connect::Ok);

        // Each channel through its own demod into its own decoder.
        const NodeId am1 = p.g.addNode(NodeKind::Demod, "AM 1");
        const NodeId am2 = p.g.addNode(NodeKind::Demod, "AM 2");
        CHECK(p.g.connect(p.ch1, 0, am1, 0) == Connect::Ok);
        CHECK(p.g.connect(p.ch2, 0, am2, 0) == Connect::Ok);
        CHECK(p.g.connect(am1, 0, p.dec1, 0) == Connect::Ok);
        CHECK(p.g.connect(am2, 0, p.dec2, 0) == Connect::Ok);

        // Both decoders merge into one text sink - the thing a single-channel
        // receiver cannot express at all, and the reason the page exists.
        // Text is the one type that may fan in, because merging lines is
        // defined where merging samples is not.
        CHECK(p.g.connect(p.dec1, 0, p.text, 0) == Connect::Ok);
        CHECK(p.g.connect(p.dec2, 0, p.text, 0) == Connect::Ok);
        CHECK(p.g.wires().size() == 9u);

        const std::vector<NodeId> order = p.g.evaluationOrder();
        CHECK(order.size() == p.g.nodes().size());
        CHECK(everyFeederComesFirst(p.g));
        CHECK(order.front() == p.radio);
        CHECK(positionOf(order, p.dec1) > positionOf(order, am1));
        CHECK(positionOf(order, am1) > positionOf(order, p.ch1));
    }

    // [10] The order is deterministic. The rebuild after a rewire happens off
    // the audio thread and is swapped in; two rebuilds of the same patch giving
    // two different orders would make any fault in it reproducible only by
    // luck.
    {
        AcarsPatch a;
        AcarsPatch b;
        for (AcarsPatch* p : {&a, &b}) {
            CHECK(p->g.connect(p->radio, 0, p->ch1, 0) == Connect::Ok);
            CHECK(p->g.connect(p->radio, 0, p->ch2, 0) == Connect::Ok);
            CHECK(p->g.connect(p->radio, 0, p->spectrum, 0) == Connect::Ok);
        }
        CHECK(a.g.evaluationOrder() == b.g.evaluationOrder());

        // ...and repeated calls on one graph agree with each other.
        CHECK(a.g.evaluationOrder() == a.g.evaluationOrder());
    }

    // [11] Nodes with nothing wired to them still appear. An unconnected node
    // is a node the user has placed and not yet wired, not an error, and
    // dropping it from the order would make it vanish from the rebuild.
    {
        Graph g;
        const NodeId lonely = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
        const NodeId r = g.addNode(NodeKind::Radio, "R");
        const NodeId c = g.addNode(NodeKind::Channel, "C");
        CHECK(g.connect(r, 0, c, 0) == Connect::Ok);

        const std::vector<NodeId> order = g.evaluationOrder();
        CHECK(order.size() == 3u);
        CHECK(positionOf(order, lonely) >= 0);
        CHECK(everyFeederComesFirst(g));
    }

    // [12] An empty graph is empty, not undefined.
    {
        Graph g;
        CHECK(g.evaluationOrder().empty());
        CHECK(g.nodes().empty());
        CHECK(g.wires().empty());
        CHECK(g.find(kNoNode) == nullptr);
        CHECK(g.count(NodeKind::Radio) == 0u);
    }

    // [13] Counting radios, which is what the device limit will be enforced
    // against once there is more than one pipeline.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "A");
        g.addNode(NodeKind::Radio, "B");
        g.addNode(NodeKind::Channel, "C");
        CHECK(g.count(NodeKind::Radio) == 2u);
        CHECK(g.count(NodeKind::Channel) == 1u);
        CHECK(g.count(NodeKind::Sink) == 0u);
    }

    // [14] Positions are part of the document. A patch is saved and reopened,
    // and where a node was put is what the user arranged.
    {
        Graph g;
        const NodeId n = g.addNode(NodeKind::Radio, "R", PortType::Iq, 120.5f, -40.25f);
        CHECK(g.find(n)->x == 120.5f);
        CHECK(g.find(n)->y == -40.25f);
        CHECK(g.find(n)->name == "R");

        // Dragging a node moves it, and the move survives in the document.
        g.mutableNode(n)->x = 300.0f;
        g.mutableNode(n)->y = 12.0f;
        CHECK(g.find(n)->x == 300.0f);
        CHECK(g.find(n)->y == 12.0f);
        CHECK(g.mutableNode(9999u) == nullptr);
    }

    // [15] The fan-in asymmetry, pinned deliberately rather than as a
    // side-effect of test [9]. Text merges; samples do not. Both halves are
    // asserted together so a future edit cannot quietly make them the same
    // rule again in either direction.
    {
        Graph g;
        const NodeId d1 = g.addNode(NodeKind::Decoder, "ACARS 1", PortType::Audio);
        const NodeId d2 = g.addNode(NodeKind::Decoder, "ACARS 2", PortType::Audio);
        const NodeId text = g.addNode(NodeKind::Sink, "Text out", PortType::Text);
        CHECK(g.connect(d1, 0, text, 0) == Connect::Ok);
        CHECK(g.connect(d2, 0, text, 0) == Connect::Ok);
        CHECK(g.wires().size() == 2u);
        // ...but the SAME wire twice is still a duplicate, merge or not.
        CHECK(g.connect(d2, 0, text, 0) == Connect::AlreadyWired);

        // Audio into one speaker: the second is refused, because that would be
        // a mix and this graph has no mixer.
        const NodeId a1 = g.addNode(NodeKind::Demod, "AM 1");
        const NodeId a2 = g.addNode(NodeKind::Demod, "AM 2");
        const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        CHECK(g.connect(a1, 0, spk, 0) == Connect::Ok);
        CHECK(g.connect(a2, 0, spk, 0) == Connect::InputOccupied);

        // And a merged text sink still orders correctly - both feeders before
        // it, which is the property a fan-in could most easily break.
        CHECK(everyFeederComesFirst(g));
    }

    // [R5] AT MOST FIVE RADIOS (0.99.17). The sixth is refused by the graph
    // itself, other kinds are not limited, and removing a radio makes room.
    {
        Graph g;
        NodeId first = kNoNode;
        for (std::size_t i = 0; i < cascade::core::patch::kMaxRadios; ++i) {
            const NodeId id = g.addNode(NodeKind::Radio, "R");
            CHECK(id != kNoNode);
            if (i == 0) { first = id; }
        }
        CHECK(g.addNode(NodeKind::Radio, "sixth") == kNoNode);
        CHECK(g.count(NodeKind::Radio) == cascade::core::patch::kMaxRadios);
        CHECK(g.addNode(NodeKind::Channel, "C") != kNoNode);   // other kinds unaffected
        CHECK(g.removeNode(first));
        CHECK(g.addNode(NodeKind::Radio, "again") != kNoNode);
        CHECK(g.addNode(NodeKind::Radio, "over") == kNoNode);
    }

    // [SW] THE RADIOS' OWN SWITCHES (0.99.18). A new radio is on. START leaves
    // the user's choice alone while any radio is on, and switches them all on
    // when none is; ALL OFF switches every radio off and touches nothing else.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Radio, "A");
        const NodeId b = g.addNode(NodeKind::Radio, "B");
        const NodeId c = g.addNode(NodeKind::Channel, "C");
        CHECK(g.find(a)->on);
        CHECK(g.find(b)->on);

        g.mutableNode(b)->on = false;
        CHECK(!switchOnForStart(g));            // A is on: B stays off
        CHECK(g.find(a)->on);
        CHECK(!g.find(b)->on);

        CHECK(switchAllRadiosOff(g));
        CHECK(!g.find(a)->on);
        CHECK(!g.find(b)->on);
        CHECK(g.find(c)->on);                   // not a radio: untouched
        CHECK(!switchAllRadiosOff(g));          // nothing left to switch

        CHECK(switchOnForStart(g));             // none on: all on
        CHECK(g.find(a)->on);
        CHECK(g.find(b)->on);

        Graph empty;                            // no radios: nothing to do
        CHECK(!switchOnForStart(empty));
        CHECK(!switchAllRadiosOff(empty));
    }

    return testSummary("test_patch_graph");
}
