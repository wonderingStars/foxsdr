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
using cascade::core::patch::kPatchFormat;
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

// The CURRENT format. The version-1 fixtures elsewhere in this file are not
// stale: a format 1 document is still something this build must read, so
// they go on exercising that path deliberately.
std::string header2() {
    return std::string(kPatchMagic) + " " + std::to_string(kPatchFormat) + "\n";
}

// A FORMAT-3 document: node lines with no <device> <rateHz> before the name.
std::string header3() { return std::string(kPatchMagic) + " 3\n"; }
// A FORMAT-4 document: node lines with <device> <rateHz> and no squelch.
std::string header4() { return std::string(kPatchMagic) + " 4\n"; }
// A FORMAT-5 document: a squelch, and no radio switch.
std::string header5() { return std::string(kPatchMagic) + " 5\n"; }

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

    // [5] A LATER format keeps what this build can read. An unknown LINE is
    // skipped rather than counted, because it is not a dropped item - it is a
    // line from a format this build was made before.
    //
    // A later format's NODE lines are read with THIS build's layout. That is a
    // deliberate limit rather than an oversight: the name is read to the end of
    // the line so it may contain spaces, so any field a future format appends
    // before the name would be indistinguishable from part of it - and a
    // channel is routinely named "131.725", so guessing whether a leading
    // number is a setting or a name is genuinely ambiguous. Reading by version
    // and letting a future field land in the name loses a label; guessing
    // would lose a frequency.
    {
        const std::string text = std::string(kPatchMagic) + " 9\n"
                                 "node 1 0 0 10 20 232 128 0 0 - siggen 2000000 1 -50 1 Radio\n"
                                 "flux 7 capacitor\n"          // from the future
                                 "node 2 1 0 30 40 232 104 131725000 0 - - 0 1 -50 1 Channel\n"
                                 "wire 1 0 2 0\n";
        const LoadResult r = parse(text);
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().size() == 1u);
        CHECK(r.dropped == 0);   // an unknown LINE is not a dropped item
    }

    // [5b] A REAL format 1 document still loads, and its nodes take the
    // default settings rather than reading their own name as a number. This is
    // the case that actually happens - a patch saved by an earlier build.
    {
        const std::string v1 = std::string(kPatchMagic) + " 1\n"
                               "view 10 20 1.5\n"
                               "node 1 0 0 60 80 Radio A\n"
                               "node 2 1 0 260 40 131.725\n"   // a NAME that is a number
                               "wire 1 0 2 0\n";
        const LoadResult r = parse(v1);
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().size() == 1u);
        CHECK(nameAt(r.graph, 0) == "Radio A");
        // The name survives intact rather than being eaten as a frequency.
        CHECK(nameAt(r.graph, 1) == "131.725");
        for (const auto& n : r.graph.nodes()) {
            CHECK(n.freqHz == 0.0);
            CHECK(n.mode == 0);
        }
        CHECK_NEAR(r.zoom, 1.5f, 0.001f);
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

    // [15] Settings survive the round trip, and a format-2 node line that is
    // missing them is dropped rather than read with the name as a number.
    {
        Graph g;
        const NodeId ch = g.addNode(NodeKind::Channel, "131.725", PortType::Iq, 10.0f, 20.0f);
        const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq, 200.0f, 20.0f);
        g.mutableNode(ch)->freqHz = 131725000.0;
        g.mutableNode(dm)->mode = 3;

        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes()[0].freqHz == 131725000.0);
        CHECK(r.graph.nodes()[0].name == "131.725");
        CHECK(r.graph.nodes()[1].mode == 3);

        // A frequency is carried exactly, not rounded through a float - a
        // channel 500 Hz off is a channel that decodes nothing.
        CHECK(r.graph.nodes()[0].freqHz == g.nodes()[0].freqHz);

        // Truncated at the settings: dropped, not misread.
        const LoadResult bad = parse(header2() + "node 1 1 0 10 20 Channel\n");
        CHECK(bad.ok);
        CHECK(bad.graph.nodes().empty());
        CHECK(bad.dropped == 1);
    }

    // [15b] FREQUENCIES WITH MORE THAN SIX SIGNIFICANT DIGITS survive exactly.
    // [15] passes with 131.725 MHz only because that number HAS six digits,
    // and six is the stream's default precision: 446.00625 MHz (PMR446) was
    // written 4.46006e+08 and came back 250 Hz off, 118.008333 MHz (an 8.33 kHz
    // airband channel) 333 Hz off - a saved patch that decodes nothing after a
    // restart. Found from the patch document a scripted run printed in 0.99.15.
    {
        Graph g;
        // The last two need TEN or more significant digits - a 23 cm channel
        // to the hertz, and a quarter-hertz offset. Nine digits (a float's
        // round-trip count) is not enough for them, so they are what proves
        // the frequency is written at a DOUBLE's precision and not merely at
        // "more than six": a break-it pass that wrote frequencies at the float
        // setting survived every other value here.
        const double freqs[] = {446006250.0, 118008333.0, 1090000000.0, 137912500.0,
                                7074000.5, 1296123457.0, 144800000.25};
        constexpr std::size_t kCount = sizeof(freqs) / sizeof(freqs[0]);
        for (const double f : freqs) {
            const NodeId ch = g.addNode(NodeKind::Channel, "c", PortType::Iq, 1234.5678f, -98.765f);
            g.mutableNode(ch)->freqHz = f;
        }
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.graph.nodes().size() == kCount);
        for (std::size_t i = 0; i < kCount && i < r.graph.nodes().size(); ++i) {
            CHECK(r.graph.nodes()[i].freqHz == freqs[i]);
        }
        // Positions are floats and must come back as the SAME float too, or
        // a node creeps a little on every save.
        if (!r.graph.nodes().empty()) {
            CHECK(r.graph.nodes()[0].x == 1234.5678f);
            CHECK(r.graph.nodes()[0].y == -98.765f);
        }
    }

    // [16] FORMAT 3: every node's size and a decoder's plugin survive the
    // round trip - including a plugin key with a space and a percent sign in
    // it, because a key is a file name and file names hold both.
    {
        Graph g;
        const NodeId ch = g.addNode(NodeKind::Channel, "Tower", PortType::Iq, 10.0f, 20.0f);
        const NodeId dec = g.addNode(NodeKind::Decoder, "POCSAG 1200", PortType::Iq, 300.0f, 20.0f);
        g.mutableNode(ch)->w = 410.0f;
        g.mutableNode(ch)->h = 222.5f;
        g.mutableNode(dec)->plugin = "pocsag decoder 100%-1.0.2.dll";

        const std::string text = serialise(g, 0.0f, 0.0f, 1.0f);
        CHECK(text.find("pocsag%20decoder%20100%25-1.0.2.dll") != std::string::npos);
        const LoadResult r = parse(text);
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 2u);
        if (r.graph.nodes().size() == 2u) {
            CHECK(r.graph.nodes()[0].w == 410.0f);
            CHECK(r.graph.nodes()[0].h == 222.5f);
            CHECK(r.graph.nodes()[0].plugin.empty());
            CHECK(r.graph.nodes()[1].plugin == "pocsag decoder 100%-1.0.2.dll");
            CHECK(r.graph.nodes()[1].name == "POCSAG 1200");
        }
    }

    // [17] A FORMAT 2 document - what 0.99.14 wrote - still loads: every
    // node at its kind's default size and running no plugin, and nothing
    // counted as dropped, because nothing was.
    {
        const LoadResult r = parse(std::string(kPatchMagic) + " 2\n"
                                   "node 1 0 0 60 80 0 0 Radio\n"
                                   "node 2 1 0 260 40 131725000 0 Tower\n"
                                   "wire 1 0 2 0\n");
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 2u);
        CHECK(r.graph.wires().size() == 1u);
        if (r.graph.nodes().size() == 2u) {
            float w = 0.0f, h = 0.0f;
            cascade::core::patch::defaultNodeSize(NodeKind::Channel, w, h);
            CHECK(r.graph.nodes()[1].w == w);
            CHECK(r.graph.nodes()[1].h == h);
            CHECK(r.graph.nodes()[1].freqHz == 131725000.0);
            CHECK(r.graph.nodes()[1].name == "Tower");
            CHECK(r.graph.nodes()[1].plugin.empty());
        }
    }

    // [18] A size the file cannot mean is not taken: zero, negative and NaN
    // keep the default, and an absurd one is clamped rather than covering the
    // canvas.
    {
        float dw = 0.0f, dh = 0.0f;
        cascade::core::patch::defaultNodeSize(NodeKind::Channel, dw, dh);
        const LoadResult r = parse(header3() +
                                   "node 1 1 0 0 0 0 0 0 0 - Zero\n"
                                   "node 2 1 0 0 0 -50 -9 0 0 - Negative\n"
                                   "node 3 1 0 0 0 nan nan 0 0 - NotANumber\n"
                                   "node 4 1 0 0 0 1e9 1e9 0 0 - Huge\n");
        CHECK(r.ok);
        // NaN may or may not parse as a float depending on the library; if it
        // does not, the line is dropped - either way no node has a NaN size.
        for (const auto& n : r.graph.nodes()) {
            CHECK(n.w == n.w);   // not NaN
            CHECK(n.w > 0.0f);
            CHECK(n.w <= cascade::core::patch::kMaxLoadedNodeSize);
            CHECK(n.h <= cascade::core::patch::kMaxLoadedNodeSize);
            if (n.name == "Zero" || n.name == "Negative" || n.name == "NotANumber") {
                CHECK(n.w == dw);
                CHECK(n.h == dh);
            }
            if (n.name == "Huge") {
                CHECK(n.w == cascade::core::patch::kMaxLoadedNodeSize);
            }
        }
        CHECK(nameAt(r.graph, 0) == "Zero");
        CHECK(nameAt(r.graph, 1) == "Negative");
    }

    // [19] An undecodable plugin token keeps the NODE - its place, wires and
    // name are the user's - but with no plugin, and the repair is counted.
    {
        const LoadResult r = parse(header3() +
                                   "node 1 3 0 0 0 216 108 0 0 bad%4 Truncated\n"
                                   "node 2 3 0 0 0 216 108 0 0 bad%zz Nonhex\n"
                                   "node 3 3 0 0 0 216 108 0 0 good%20one Fine\n");
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 3u);
        CHECK(r.dropped == 2);
        if (r.graph.nodes().size() == 3u) {
            CHECK(r.graph.nodes()[0].plugin.empty());
            CHECK(r.graph.nodes()[1].plugin.empty());
            CHECK(r.graph.nodes()[2].plugin == "good one");
            CHECK(r.graph.nodes()[0].name == "Truncated");
        }
    }

    // [20] The key encoding is exact for EVERY byte, and "-" means none.
    {
        std::string all;
        for (int c = 1; c < 256; ++c) { all.push_back(static_cast<char>(c)); }
        const std::string enc = cascade::core::patch::encodePluginKey(all);
        CHECK(enc.find(' ') == std::string::npos);
        CHECK(enc.find('\n') == std::string::npos);
        std::string back;
        CHECK(cascade::core::patch::decodePluginKey(enc, back));
        CHECK(back == all);
        CHECK(cascade::core::patch::encodePluginKey("") == "-");
        CHECK(cascade::core::patch::decodePluginKey("-", back));
        CHECK(back.empty());
        // A real key that is just a hyphen cannot be confused with "none".
        CHECK(cascade::core::patch::encodePluginKey("-") != "-");
        CHECK(cascade::core::patch::decodePluginKey(
            cascade::core::patch::encodePluginKey("-"), back));
        CHECK(back == "-");
    }

    // [F4a] FORMAT 4 (0.99.17): a radio's device and rate, and a speaker's
    // output, come back exactly - including a SoapySDR args string, which is
    // full of commas, equals signs and sometimes spaces.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Radio, "Airband", PortType::Iq, 10.0f, 20.0f);
        const NodeId b = g.addNode(NodeKind::Radio, "Gen", PortType::Iq, 10.0f, 200.0f);
        const NodeId s = g.addNode(NodeKind::Sink, "Out", PortType::Audio, 400.0f, 20.0f);
        g.mutableNode(a)->device = "soapy|driver=uhd,serial=31E8A0B, type=b200";
        g.mutableNode(a)->rateHz = 2048000.25;
        g.mutableNode(a)->freqHz = 131725000.0;
        g.mutableNode(b)->device = "siggen";
        g.mutableNode(b)->rateHz = 2000000.0;
        g.mutableNode(s)->device = "audio:Headphones (Arctis Nova Pro Wireless)";
        const std::string text = serialise(g, 0.0f, 0.0f, 1.0f);
        CHECK(text.rfind(std::string(kPatchMagic) + " " + std::to_string(kPatchFormat), 0) == 0);
        const LoadResult r = parse(text);
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 3u);
        if (r.graph.nodes().size() == 3u) {
            CHECK(r.graph.nodes()[0].device == "soapy|driver=uhd,serial=31E8A0B, type=b200");
            CHECK(r.graph.nodes()[0].rateHz == 2048000.25);
            CHECK(r.graph.nodes()[0].freqHz == 131725000.0);
            CHECK(r.graph.nodes()[0].name == "Airband");
            CHECK(r.graph.nodes()[1].device == "siggen");
            CHECK(r.graph.nodes()[1].rateHz == 2000000.0);
            CHECK(r.graph.nodes()[2].device == "audio:Headphones (Arctis Nova Pro Wireless)");
            CHECK(r.graph.nodes()[2].name == "Out");
        }
    }

    // [F4b] A FORMAT-3 radio has no device: it loads as one still to be chosen,
    // with its name intact, rather than guessing which radio it meant.
    {
        const LoadResult r = parse(header3() + "node 1 0 0 10 20 232 128 100000000 0 - Radio\n");
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 1u);
        if (r.graph.nodes().size() == 1u) {
            CHECK(r.graph.nodes()[0].device.empty());
            CHECK(r.graph.nodes()[0].rateHz == 0.0);
            CHECK(r.graph.nodes()[0].name == "Radio");
        }
    }

    // [F4c] A SIXTH RADIO in a file is refused by the graph, counted as
    // dropped, and a wire to it goes with it - the other five load.
    {
        std::string text = header4();
        for (int i = 1; i <= 6; ++i) {
            text += "node " + std::to_string(i) + " 0 0 0 0 232 128 0 0 - siggen 2000000 R" +
                    std::to_string(i) + "\n";
        }
        text += "node 7 1 0 0 0 232 104 100000000 0 - - 0 Chan\n";
        text += "wire 6 0 7 0\n";
        const LoadResult r = parse(text);
        CHECK(r.ok);
        CHECK(r.graph.count(NodeKind::Radio) == cascade::core::patch::kMaxRadios);
        CHECK(r.graph.nodes().size() == 6u);    // five radios and the channel
        CHECK(r.graph.wires().empty());
        CHECK(r.dropped == 2);                  // the sixth radio and its wire
    }

    // [F5] FORMAT 5 (0.99.18): a demodulator's squelch - on or off, and its
    // threshold - comes back exactly; a format-4 demodulator loads with the
    // default (on, -50 dB); an absurd threshold in a file is not trusted.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Demod, "Off", PortType::Iq);
        const NodeId b = g.addNode(NodeKind::Demod, "Low", PortType::Iq);
        g.mutableNode(a)->squelch = false;
        g.mutableNode(a)->squelchDb = -72.5f;
        g.mutableNode(b)->squelchDb = -101.25f;
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 2u);
        if (r.graph.nodes().size() == 2u) {
            CHECK(!r.graph.nodes()[0].squelch);
            CHECK(r.graph.nodes()[0].squelchDb == -72.5f);
            CHECK(r.graph.nodes()[0].name == "Off");
            CHECK(r.graph.nodes()[1].squelch);
            CHECK(r.graph.nodes()[1].squelchDb == -101.25f);
        }
        const LoadResult old = parse(std::string(kPatchMagic) +
                                     " 4\nnode 1 2 0 0 0 232 132 0 1 - - 0 FM\n");
        CHECK(old.ok);
        CHECK(old.graph.nodes().size() == 1u);
        if (old.graph.nodes().size() == 1u) {
            CHECK(old.graph.nodes()[0].squelch);
            CHECK(old.graph.nodes()[0].squelchDb == -50.0f);
            CHECK(old.graph.nodes()[0].name == "FM");
        }
        const LoadResult odd = parse(header5() + "node 1 2 0 0 0 232 132 0 1 - - 0 1 55 Loud\n"
                                                 "node 2 2 0 0 0 232 132 0 1 - - 0 1 nan N\n");
        CHECK(odd.ok);
        for (const auto& n : odd.graph.nodes()) { CHECK(n.squelchDb == -50.0f); }
    }

    // [MAP] A Map part and the decoders wired to its inputs come back - the
    // node kind and the Track port are both new, and an older build refuses
    // them rather than misreading them.
    {
        Graph g;
        const NodeId d1 = g.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq);
        const NodeId d2 = g.addNode(NodeKind::Decoder, "AIS", PortType::Iq);
        const NodeId m = g.addNode(NodeKind::Map, "Sky and sea", PortType::Track);
        CHECK(g.connect(d1, 1, m, 0) == Connect::Ok);
        CHECK(g.connect(d2, 1, m, 3) == Connect::Ok);
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 3u);
        CHECK(r.graph.wires().size() == 2u);
        CHECK(r.graph.count(NodeKind::Map) == 1u);
        if (r.graph.wires().size() == 2u) {
            CHECK(r.graph.wires()[0].fromPort == 1u && r.graph.wires()[0].toPort == 0u);
            CHECK(r.graph.wires()[1].fromPort == 1u && r.graph.wires()[1].toPort == 3u);
        }
        CHECK(nameAt(r.graph, 2) == "Sky and sea");
        // A kind past Map in a file is dropped, not guessed at.
        const LoadResult bad = parse(header2() + "node 1 7 0 0 0 232 128 0 0 - - 0 1 -50 1 X\n");
        CHECK(bad.ok);
        CHECK(bad.graph.nodes().empty());
        CHECK(bad.dropped == 1);
    }

    // [F6] FORMAT 6: a radio's own switch comes back; a format-5 radio is on.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Radio, "Off radio", PortType::Iq);
        g.addNode(NodeKind::Radio, "On radio", PortType::Iq);
        g.mutableNode(a)->on = false;
        const LoadResult r = parse(serialise(g, 0.0f, 0.0f, 1.0f));
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == 2u);
        if (r.graph.nodes().size() == 2u) {
            CHECK(!r.graph.nodes()[0].on);
            CHECK(r.graph.nodes()[0].name == "Off radio");
            CHECK(r.graph.nodes()[1].on);
        }
        const LoadResult five = parse(header5() + "node 1 0 0 0 0 232 128 0 0 - siggen 2000000 1 -50 R\n");
        CHECK(five.ok);
        CHECK(five.graph.nodes().size() == 1u);
        if (five.graph.nodes().size() == 1u) {
            CHECK(five.graph.nodes()[0].on);
            CHECK(five.graph.nodes()[0].name == "R");
        }
    }

    // [F4d] A negative or absurd rate in a file is not trusted.
    {
        const LoadResult r = parse(header4() + "node 1 0 0 0 0 232 128 0 0 - siggen -5 A\n"
                                               "node 2 0 0 0 0 232 128 0 0 - siggen 1e99 B\n");
        CHECK(r.ok);
        CHECK(r.graph.nodes().size() == 2u);
        for (const auto& n : r.graph.nodes()) { CHECK(n.rateHz == 0.0); }
    }

    return testSummary("test_patch_io");
}
