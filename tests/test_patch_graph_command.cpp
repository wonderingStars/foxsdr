// Tests for core/patch_draft.hpp - the patch graph as one command
// (FOXAPP_OP_PATCH_SET_GRAPH) and the Patch page's draft put back on top of
// the engine's graph (docs/engine-stage3.md OPEN 6, Design A).
//
// WHAT IS PINNED HERE, without a window or an engine:
//   1. the document round trip keeps every position and frequency EXACTLY -
//      a channel on 144.80000025 MHz is eleven significant digits, and a
//      stream's six would put it 250 Hz away (patch_io.hpp's own history);
//   2. the command's text keeps what a document does not: ids (the runtime
//      and the page key everything by them), the next id (a deleted node's
//      id is never handed out again) and a session-only chosen centre;
//   3. the rebase keeps both halves of a race - the page's edit and whatever
//      the engine did meanwhile - and with nothing raced it is the draft,
//      byte for byte;
//   4. Graph::addNodeAs refuses what addNode would never produce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_draft.hpp"

#include <cstdio>
#include <string>

#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;

namespace {

// A patch whose every field is off its default, with an id gap (a node
// removed from the middle) and one of each port type wired. The FIRST node's
// position needs every digit too: serialise() sets a field's precision as it
// goes, and the first node's numbers are the ones written before any of that.
pc::Graph buildPatch(pc::NodeId& radio, pc::NodeId& chan, pc::NodeId& demod, pc::NodeId& sink) {
    pc::Graph g;
    radio = g.addNode(pc::NodeKind::Radio, "Radio A", pc::PortType::Iq, 61.2345678f, -80.00390625f);
    const pc::NodeId gap = g.addNode(pc::NodeKind::Display, "gone", pc::PortType::Iq, 1.0f, 1.0f);
    chan = g.addNode(pc::NodeKind::Channel, " 2 m calling", pc::PortType::Iq, 0.1f, -7.3e-5f);
    demod = g.addNode(pc::NodeKind::Demod, "FM", pc::PortType::Iq, 1234.5678f, 98765.4321f);
    sink = g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 1.0f / 3.0f, 2.0f / 3.0f);
    (void)g.removeNode(gap);
    if (pc::Node* n = g.mutableNode(radio)) {
        n->device = "rtlsdr|serial=00000001 label=Desk dongle";
        n->freqHz = 145.0e6 + 1.0 / 3.0;
        n->rateHz = 2.048e6;
        n->on = false;
        n->w = 301.25f;
        n->h = 177.75f;
    }
    if (pc::Node* n = g.mutableNode(chan)) { n->freqHz = 144800000.25; }
    if (pc::Node* n = g.mutableNode(demod)) {
        n->mode = 3;
        n->squelch = false;
        n->squelchDb = -73.5f;
    }
    if (pc::Node* n = g.mutableNode(sink)) { n->device = "audio:Line Out (USB)"; }
    CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
    CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
    CHECK(g.connect(demod, 0, sink, 0) == pc::Connect::Ok);
    return g;
}

bool sameNode(const pc::Node& a, const pc::Node& b) {
    return a.id == b.id && a.kind == b.kind && a.name == b.name && a.inputs == b.inputs &&
           a.outputs == b.outputs && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
           a.freqHz == b.freqHz && a.mode == b.mode && a.plugin == b.plugin && a.device == b.device &&
           a.rateHz == b.rateHz && a.squelch == b.squelch && a.squelchDb == b.squelchDb &&
           a.on == b.on && a.centreChosen == b.centreChosen;
}

bool sameGraph(const pc::Graph& a, const pc::Graph& b) {
    if (a.nodes().size() != b.nodes().size() || a.wires() != b.wires() || a.nextId() != b.nextId()) {
        return false;
    }
    for (std::size_t i = 0; i < a.nodes().size(); ++i) {
        if (!sameNode(a.nodes()[i], b.nodes()[i])) { return false; }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("test_patch_graph_command\n");

    // --- 1. the DOCUMENT round trip (serialise/parse, as config.json keeps it):
    //        positions and frequencies exact, 144800000.25 Hz included ---------
    {
        pc::NodeId radio = 0, chan = 0, demod = 0, sink = 0;
        const pc::Graph g = buildPatch(radio, chan, demod, sink);
        const pc::LoadResult r = pc::parse(pc::serialise(g, 12.345678f, -3.14159274f, 0.8f));
        CHECK(r.ok);
        CHECK(r.dropped == 0);
        CHECK(r.graph.nodes().size() == g.nodes().size());
        for (std::size_t i = 0; i < g.nodes().size() && i < r.graph.nodes().size(); ++i) {
            const pc::Node& a = g.nodes()[i];
            const pc::Node& b = r.graph.nodes()[i];
            CHECK(a.x == b.x);
            CHECK(a.y == b.y);
            CHECK(a.w == b.w);
            CHECK(a.h == b.h);
            CHECK(a.freqHz == b.freqHz);
            CHECK(a.rateHz == b.rateHz);
            CHECK(a.squelchDb == b.squelchDb);
            CHECK(a.name == b.name);
        }
        const pc::Node* c = r.graph.nodes().size() > 1 ? &r.graph.nodes()[1] : nullptr;
        CHECK(c != nullptr && c->freqHz == 144800000.25);
        // ...but a DOCUMENT renumbers: the gap closes. That is right for a
        // file and wrong for the engine's copy of a live graph - hence Keep.
        CHECK(c != nullptr && c->id == 2u);
        CHECK(r.panX == 12.345678f && r.panY == -3.14159274f && r.zoom == 0.8f);
    }

    // --- 2. the COMMAND's text: the same graph, ids and all ------------------
    {
        pc::NodeId radio = 0, chan = 0, demod = 0, sink = 0;
        pc::Graph g = buildPatch(radio, chan, demod, sink);
        if (pc::Node* n = g.mutableNode(radio)) {
            n->freqHz = 0.0;
            n->centreChosen = true;  // 0 Hz on the air is a centre - and a document forgets it
        }
        const pc::NodeId last = g.addNode(pc::NodeKind::Decoder, "ACARS", pc::PortType::Audio, 5.0f, 5.0f);
        (void)g.removeNode(last);  // the next id is past a node that is gone
        const std::string text = pc::graphCommandText(g);
        pc::Graph back;
        std::string why;
        CHECK(pc::graphFromCommandText(text, back, why));
        CHECK(why.empty());
        CHECK(sameGraph(g, back));
        CHECK(back.find(chan) != nullptr && back.find(chan)->freqHz == 144800000.25);
        CHECK(back.find(radio) != nullptr && back.find(radio)->centreChosen);
        CHECK(back.nextId() == last + 1u);
        CHECK(pc::graphCommandText(back) == text);
        // The next node added to the copy gets the id the original would.
        CHECK(back.addNode(pc::NodeKind::Map, "Map") == g.addNode(pc::NodeKind::Map, "Map"));

        // Refused whole, `out` untouched, for any part that cannot be read.
        pc::Graph kept = back;
        const std::string before = pc::graphCommandText(kept);
        const std::string bad[] = {
            std::string(),
            "foxsdr-patch x\n",
            text + "node 99 0 0 nope\n",
            text + "wire 1 0 1 0\n",                         // a self-loop connect() refuses
            text + "centre-chosen 77\n",
            text + "next-id -\n",
        };
        for (const std::string& t : bad) {
            std::string w;
            CHECK(!pc::graphFromCommandText(t, kept, w));
            CHECK(!w.empty());
            CHECK(pc::graphCommandText(kept) == before);
        }

        // NOT REPAIRED EITHER. The document loader quietly repairs a value it
        // cannot keep (a squelch outside -120..0 dB becomes -50, a rate past
        // 10 GHz becomes 0, a size past 4000 is clamped, one of 0 or below
        // becomes the kind's default) - right for a damaged config, wrong for
        // a command: the engine would run a graph the page never drew. Each
        // is a graph the Graph type itself allows, written by the command's
        // own writer, and each is refused whole.
        struct Bad {
            const char* what;
            void (*set)(pc::Node&);
        };
        const Bad values[] = {
            {"squelch -200 dB", [](pc::Node& n) { n.squelchDb = -200.0f; }},
            {"squelch +3 dB", [](pc::Node& n) { n.squelchDb = 3.0f; }},
            {"rate 2e10 Hz", [](pc::Node& n) { n.rateHz = 2.0e10; }},
            {"rate -1 Hz", [](pc::Node& n) { n.rateHz = -1.0; }},
            {"width 5000", [](pc::Node& n) { n.w = 5000.0f; }},
            {"height 0", [](pc::Node& n) { n.h = 0.0f; }},
        };
        for (const Bad& v : values) {
            pc::Graph odd = back;
            if (pc::Node* n = odd.mutableNode(demod)) { v.set(*n); }
            std::string w;
            const bool took = pc::graphFromCommandText(pc::graphCommandText(odd), kept, w);
            if (took) { std::printf("FAIL: the command took %s\n", v.what); }
            CHECK(!took);
            CHECK(!w.empty());
            CHECK(pc::graphCommandText(kept) == before);
        }
        // ...while the edges themselves are values, and are taken.
        {
            pc::Graph edge = back;
            if (pc::Node* n = edge.mutableNode(demod)) {
                n->squelchDb = pc::kSquelchMinDb;
                n->w = pc::kMaxLoadedNodeSize;
            }
            if (pc::Node* n = edge.mutableNode(radio)) { n->rateHz = 1.0e10; }
            pc::Graph got;
            std::string w;
            CHECK(pc::graphFromCommandText(pc::graphCommandText(edge), got, w));
            CHECK(sameGraph(edge, got));
        }
    }

    // --- 3. the rebase -------------------------------------------------------
    {
        pc::NodeId radio = 0, chan = 0, demod = 0, sink = 0;
        const pc::Graph base = buildPatch(radio, chan, demod, sink);

        // Nothing raced: the answer IS the draft.
        {
            pc::Graph draft = base;
            if (pc::Node* n = draft.mutableNode(chan)) {
                n->x = 400.5f;
                n->y = 20.25f;
            }
            const pc::NodeId added = draft.addNode(pc::NodeKind::Display, "Scope", pc::PortType::Iq, 9.0f, 9.0f);
            CHECK(draft.connect(chan, 0, added, 0) == pc::Connect::Ok);
            CHECK(draft.disconnect(pc::Wire{demod, 0, sink, 0}));
            (void)draft.removeNode(sink);
            CHECK(pc::graphCommandText(pc::rebaseDraft(base, draft, base)) == pc::graphCommandText(draft));
        }

        // A drag of the channel while the engine learnt the radio's centre and
        // threw its switch, and set the channel's own name: all three kept.
        {
            pc::Graph draft = base;
            if (pc::Node* n = draft.mutableNode(chan)) {
                n->x = 777.0f;
                n->y = -12.5f;
            }
            pc::Graph now = base;
            if (pc::Node* n = now.mutableNode(radio)) {
                n->freqHz = 145.5e6;
                n->centreChosen = true;
                n->on = true;
            }
            if (pc::Node* n = now.mutableNode(chan)) { n->name = "renamed by the engine"; }
            const pc::Graph out = pc::rebaseDraft(base, draft, now);
            const pc::Node* c = out.find(chan);
            const pc::Node* r = out.find(radio);
            CHECK(c != nullptr && c->x == 777.0f && c->y == -12.5f);
            CHECK(c != nullptr && c->name == "renamed by the engine");
            CHECK(c != nullptr && c->freqHz == 144800000.25);
            CHECK(r != nullptr && r->freqHz == 145.5e6 && r->centreChosen && r->on);
            CHECK(out.wires() == base.wires());
        }

        // The same field both ways: the page's (the user's latest act) wins.
        {
            pc::Graph draft = base;
            if (pc::Node* n = draft.mutableNode(radio)) { n->on = true; }
            pc::Graph now = base;
            if (pc::Node* n = now.mutableNode(radio)) {
                n->on = true;
                n->rateHz = 2.4e6;   // the engine's other change stays
            }
            if (pc::Node* n = draft.mutableNode(radio)) {
                n->freqHz = 1.0;
                n->centreChosen = true;
            }
            if (pc::Node* n = now.mutableNode(radio)) {
                n->freqHz = 2.0;
                n->centreChosen = false;
            }
            const pc::Graph out = pc::rebaseDraft(base, draft, now);
            const pc::Node* r = out.find(radio);
            CHECK(r != nullptr && r->freqHz == 1.0 && r->centreChosen && r->rateHz == 2.4e6 && r->on);
        }

        // Deletions: the page's delete wins over the engine's edit of that
        // node; the engine's delete wins over the page's edit of it.
        {
            pc::Graph draft = base;
            (void)draft.removeNode(sink);
            if (pc::Node* n = draft.mutableNode(demod)) { n->mode = 5; }
            pc::Graph now = base;
            if (pc::Node* n = now.mutableNode(sink)) { n->device = "speakers"; }
            (void)now.removeNode(demod);
            const pc::Graph out = pc::rebaseDraft(base, draft, now);
            CHECK(out.find(sink) == nullptr);
            CHECK(out.find(demod) == nullptr);
            CHECK(out.find(radio) != nullptr && out.find(chan) != nullptr);
            CHECK(out.wires().size() == 1u);  // radio -> channel, the only one left
        }

        // Added by the page: the node keeps its id, the wire is made, and the
        // next id carries on from the draft's.
        {
            pc::Graph draft = base;
            const pc::NodeId added = draft.addNode(pc::NodeKind::Display, "Scope", pc::PortType::Iq, 3.0f, 4.0f);
            CHECK(draft.connect(radio, 0, added, 0) == pc::Connect::Ok);
            pc::Graph now = base;
            if (pc::Node* n = now.mutableNode(radio)) { n->rateHz = 2.4e6; }
            const pc::Graph out = pc::rebaseDraft(base, draft, now);
            const pc::Node* d = out.find(added);
            CHECK(d != nullptr && d->x == 3.0f && d->y == 4.0f && d->name == "Scope");
            CHECK(std::find(out.wires().begin(), out.wires().end(), pc::Wire{radio, 0, added, 0}) !=
                  out.wires().end());
            CHECK(out.nextId() == draft.nextId());
            CHECK(out.find(radio) != nullptr && out.find(radio)->rateHz == 2.4e6);
        }
    }

    // --- 4. addNodeAs ----------------------------------------------------------
    {
        pc::Graph g;
        CHECK(g.addNodeAs(pc::kNoNode, pc::NodeKind::Channel, "zero") == pc::kNoNode);
        CHECK(g.addNodeAs(7u, pc::NodeKind::Channel, "seven") == 7u);
        CHECK(g.addNodeAs(7u, pc::NodeKind::Channel, "seven again") == pc::kNoNode);
        CHECK(g.nextId() == 8u);
        CHECK(g.addNodeAs(3u, pc::NodeKind::Channel, "three") == 3u);
        CHECK(g.nextId() == 8u);  // never moves back
        CHECK(g.addNode(pc::NodeKind::Channel, "next") == 8u);
        for (pc::NodeId id = 20u; id < 20u + pc::kMaxRadios; ++id) {
            CHECK(g.addNodeAs(id, pc::NodeKind::Radio, "R") == id);
        }
        CHECK(g.addNodeAs(40u, pc::NodeKind::Radio, "a sixth") == pc::kNoNode);
        g.reserveIds(5u);
        CHECK(g.nextId() == 20u + pc::kMaxRadios);
    }

    return testSummary("test_patch_graph_command");
}
