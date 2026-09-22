// Tests for core/patch_io.hpp - a patch written down and read back.
//
// The thing worth testing here is not the happy round trip, which is easy; it
// is what a DAMAGED file does. This text lives inside the user's config, which
// is hand-editable, merged across machines, and occasionally truncated by a
// power cut mid-write. A loader that trusted it could produce a graph the
// canvas itself could never have built - a cycle, a type mismatch, an input
// with two sample sources - and the DSP would answer that with silence rather
// than an error. So every wire is loaded through connect(), and these tests
// are mostly about proving that holds.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_io.hpp"

#include <string>

#include "core/patch_graph.hpp"
#include "test_check.hpp"

using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::kPatchMagic;
using cascade::core::patch::LoadResult;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::parse;
using cascade::core::patch::PortType;
using cascade::core::patch::sanitiseName;
using cascade::core::patch::serialise;

namespace {

// The patch from the mockup: one radio into two channels and a spectrum, two
// audio decoders merging into one text sink.
Graph buildRealPatch() {
    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "Radio A", PortType::Iq, 60.0f, 80.0f);
    const NodeId ch1 = g.addNode(NodeKind::Channel, "131.725", PortType::Iq, 260.0f, 40.0f);
    const NodeId ch2 = g.addNode(NodeKind::Channel, "130.025", PortType::Iq, 260.0f, 160.0f);
    const NodeId spec = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq, 260.0f, 280.0f);
    const NodeId am1 = g.addNode(NodeKind::Demod, "AM 1", PortType::Iq, 460.0f, 40.0f);
    const NodeId am2 = g.addNode(NodeKind::Demod, "AM 2", PortType::Iq, 460.0f, 160.0f);
    const NodeId d1 = g.addNode(NodeKind::Decoder, "ACARS 1", PortType::Audio, 660.0f, 40.0f);
    const NodeId d2 = g.addNode(NodeKind::Decoder, "ACARS 2", PortType::Audio, 660.0f, 160.0f);
    const NodeId text = g.addNode(NodeKind::Sink, "Text out", PortType::Text, 860.0f, 100.0f);

    g.connect(radio, 0, ch1, 0);
    g.connect(radio, 0, ch2, 0);
    g.connect(radio, 0, spec, 0);
    g.connect(ch1, 0, am1, 0);
    g.connect(ch2, 0, am2, 0);
    g.connect(am1, 0, d1, 0);
    g.connect(am2, 0, d2, 0);
    g.connect(d1, 0, text, 0);
    g.connect(d2, 0, text, 0);
    return g;
}

// Compare two graphs by what they MEAN rather than by their ids, which are
// deliberately not preserved across a save and load.
bool sameShape(const Graph& a, const Graph& b) {
    if (a.nodes().size() != b.nodes().size()) { return false; }
    if (a.wires().size() != b.wires().size()) { return false; }
    for (std::size_t i = 0; i < a.nodes().size(); ++i) {
        const auto& x = a.nodes()[i];
        const auto& y = b.nodes()[i];
        if (x.kind != y.kind || x.name != y.name) { return false; }
        if (x.x != y.x || x.y != y.y) { return false; }
        if (x.inputs != y.inputs || x.outputs != y.outputs) { return false; }
    }
    return true;
}

std::string header() { return std::string(kPatchMagic) + " 1\n"; }

// Indexing guarded only by a preceding CHECK is an out-of-bounds read in
// exactly the run that has something to report: the test dies with an access
// violation instead of naming the broken expectation, so the red check meant
// to prove the test works proves nothing. A mutation run did exactly that
// here. These return a value no test expects instead.
std::string nameAt(const Graph& g, std::size_t i) {
    return i < g.nodes().size() ? g.nodes()[i].name : std::string("<no such node>");
}

NodeKind kindOf(const Graph& g, NodeId id) {
    const auto* n = g.find(id);
    return n != nullptr ? n->kind : NodeKind::Sink;
}

}  // namespace

int main() {
    // [1] A real patch survives the round trip: every node, every wire, every
    // position, and the view.
    {
        const Graph g = buildRealPatch();
        const std::string text = serialise(g, -120.0f, 45.5f, 1.25f);
        const LoadResult r = parse(text);

        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(sameShape(g, r.graph));
        CHECK(r.graph.nodes().size() == 9u);
        CHECK(r.graph.wires().size() == 9u);
        CHECK_NEAR(r.panX, -120.0f, 0.01f);
        CHECK_NEAR(r.panY, 45.5f, 0.01f);
        CHECK_NEAR(r.zoom, 1.25f, 0.001f);

        // ...and saving what was loaded gives the same text again, which is
        // the property that stops a patch drifting a little on every launch.
        CHECK(serialise(r.graph, r.panX, r.panY, r.zoom) == text);
    }

    // [2] Names with spaces survive; names with newlines cannot be allowed to,
    // because the format is line-based and one would inject a line.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "Radio A  (the good one)", PortType::Iq, 0.0f, 0.0f);
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.graph.nodes().size() == 1u);
        CHECK(nameAt(r.graph, 0) == "Radio A  (the good one)");

        CHECK(sanitiseName("a\nb") == "a b");
        CHECK(sanitiseName("a\r\nb") == "a  b");
        CHECK(sanitiseName("a\tb") == "a b");

        Graph nasty;
        nasty.addNode(NodeKind::Radio, "evil\nwire 1 0 1 0", PortType::Iq, 0.0f, 0.0f);
        const LoadResult nr = parse(serialise(nasty, 0.0f, 0.0f, 1.0f));
        CHECK(nr.graph.nodes().size() == 1u);   // one node, not two lines' worth
        CHECK(nr.graph.wires().empty());        // and no wire was injected
    }

    // [3] An empty patch is a patch. Saving nothing and loading it back must
    // give nothing, not a failure.
    {
        Graph g;
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.ok);
        CHECK(r.graph.nodes().empty());
        CHECK(r.graph.wires().empty());
        CHECK(r.dropped == 0);
    }

    // [4] Rubbish in the header is refused outright, and refusing means an
    // EMPTY graph rather than a half-built one.
    {
        CHECK(!parse("").ok);
        CHECK(!parse("nonsense\n").ok);
        CHECK(!parse("foxsdr-patch\n").ok);           // no version
        CHECK(!parse("foxsdr-patch 0\n").ok);         // version 0
        CHECK(!parse("{\"json\": true}\n").ok);       // somebody's config
        const LoadResult r = parse("nonsense\nnode 1 0 0 0 0 Radio\n");
        CHECK(!r.ok);
        CHECK(r.graph.nodes().empty());
    }

    // [5] A LATER format loses what this build cannot read and keeps the rest.
    // Refusing the whole file would make a patch unusable after a downgrade.
    {
        const std::string text = std::string(kPatchMagic) + " 9\n"
                                 "node 1 0 0 10 20 Radio\n"
                                 "flux 7 capacitor\n"          // from the future
                                 "node 2 1 0 30 40 Channel\n"
                                 "wire 1 0 2 0\n";
        const LoadResult r = parse(text);
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().size() == 1u);
        CHECK(r.dropped == 0);   // an unknown LINE is not a dropped item
    }

    // [6] A wire the rules forbid is dropped, not honoured. This is the whole
    // reason loading goes through connect().
    {
        // A type mismatch: a radio's I/Q straight into an audio decoder.
        const std::string bad = header() +
                                "node 1 0 0 0 0 Radio\n"
                                "node 2 3 1 200 0 ACARS\n"   // Decoder, audio feed
                                "wire 1 0 2 0\n";
        const LoadResult r = parse(bad);
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().empty());
        CHECK(r.dropped == 1);
    }

    // [7] A cycle in the file cannot become a cycle in the graph. A hand-edit
    // is the only way to write one, and a loop in a running DSP graph does not
    // draw badly - it starves the audio thread.
    {
        const std::string loop = header() +
                                 "node 1 1 0 0 0 A\n"
                                 "node 2 1 0 200 0 B\n"
                                 "node 3 1 0 400 0 C\n"
                                 "wire 1 0 2 0\n"
                                 "wire 2 0 3 0\n"
                                 "wire 3 0 1 0\n";
        const LoadResult r = parse(loop);
        CHECK(r.graph.wires().size() == 2u);
        CHECK(r.dropped == 1);
        // The survivor is still a valid, orderable graph.
        CHECK(r.graph.evaluationOrder().size() == r.graph.nodes().size());
    }

    // [8] Two sample sources into one input: the second is dropped.
    {
        const std::string twice = header() +
                                  "node 1 0 0 0 0 R1\n"
                                  "node 2 0 0 0 100 R2\n"
                                  "node 3 1 0 200 0 C\n"
                                  "wire 1 0 3 0\n"
                                  "wire 2 0 3 0\n";
        const LoadResult r = parse(twice);
        CHECK(r.graph.wires().size() == 1u);
        CHECK(r.dropped == 1);
    }

    // [9] Wires naming nodes that are not in the file are dropped rather than
    // creating them. A truncated file is exactly this shape.
    {
        const std::string cut = header() +
                                "node 1 0 0 0 0 Radio\n"
                                "wire 1 0 99 0\n"
                                "wire 99 0 1 0\n";
        const LoadResult r = parse(cut);
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 1u);
        CHECK(r.graph.wires().empty());
        CHECK(r.dropped == 2);
    }

    // [10] Unreadable numbers and out-of-range enums are dropped, not
    // truncated into something plausible - a node kind read as 0 would appear
    // as a Radio the user never placed.
    {
        const std::string junk = header() +
                                 "node one two three four five Name\n"   // no numbers
                                 "node 2 99 0 0 0 BadKind\n"             // no such kind
                                 "node 3 0 99 0 0 BadFeed\n"             // no such type
                                 "node 4 0 0 10 20 Fine\n";
        const LoadResult r = parse(junk);
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 1u);
        CHECK(nameAt(r.graph, 0) == "Fine");
        CHECK(r.dropped == 3);
    }

    // [11] A zoom of zero would divide by zero in screenToWorld and put every
    // node at infinity, so it is replaced rather than trusted.
    {
        CHECK_NEAR(parse(header() + "view 0 0 0\n").zoom, 1.0f, 0.001f);
        CHECK_NEAR(parse(header() + "view 0 0 -3\n").zoom, 1.0f, 0.001f);
        CHECK_NEAR(parse(header() + "view 5 6 2\n").zoom, 2.0f, 0.001f);
        // A view line that is short keeps the defaults rather than half-applying.
        const LoadResult r = parse(header() + "view 5\n");
        CHECK_NEAR(r.panX, 0.0f, 0.001f);
        CHECK_NEAR(r.zoom, 1.0f, 0.001f);
    }

    // [12] Ids are remapped, so a patch loaded into a graph does not depend on
    // the file's own numbering being anything in particular.
    {
        const std::string odd = header() +
                                "node 5000 0 0 0 0 Radio\n"
                                "node 17 1 0 200 0 Channel\n"
                                "wire 5000 0 17 0\n";
        const LoadResult r = parse(odd);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().size() == 1u);
        // Whatever ids it got, the wire joins the radio to the channel.
        CHECK(!r.graph.wires().empty());
        if (!r.graph.wires().empty()) {
            const auto& w = r.graph.wires()[0];
            CHECK(kindOf(r.graph, w.from) == NodeKind::Radio);
            CHECK(kindOf(r.graph, w.to) == NodeKind::Channel);
        }
        CHECK(r.dropped == 0);
    }

    return testSummary("test_patch_io");
}
