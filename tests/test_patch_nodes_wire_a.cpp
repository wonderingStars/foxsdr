// test_patch_nodes_wire_a.cpp - THE FOUR NEW PATCH PARTS AND THE BYPASS (wire-a second pass, item 5), on the graph itself.
//   A  the ports of Mixer, IfFilter, Generator and Scope, and what connects to what;  B  THE GENERATOR IS REFUSED ON EVERY TRANSMIT SINK: a list of device names that transmit, and the empty and unknown ones, all
//   give Connect::TransmitRefused, while the receive-side destinations are accepted, and the refusal is also what the document loader does with such a wire;  C  bypass: only a part whose input 0 and output 0 are
//   the same type, effective() splices it out (chains, a bypassed part with nothing in, other ports dropped) and leaves the document alone;  D  the document: format 7 only when a part needs it, a patch without the
//   new features is written as format 6 byte for byte, the round trip keeps bypass and settings, the engine's command text is exactly what it writes;  E  the plan says a part this build does not run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"
#include "core/patch_plan.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;

namespace {

pc::NodeId add(pc::Graph& g, pc::NodeKind k, const char* name, pc::PortType feed = pc::PortType::Iq) { return g.addNode(k, name, feed); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_patch_nodes_wire_a\n");

    std::printf("A: ports and connections\n");
    {
        pc::Graph g;
        const pc::NodeId radio = add(g, pc::NodeKind::Radio, "radio");
        const pc::NodeId mix = add(g, pc::NodeKind::Mixer, "mixer");
        const pc::NodeId iff = add(g, pc::NodeKind::IfFilter, "if");
        const pc::NodeId gen = add(g, pc::NodeKind::Generator, "gen");
        const pc::NodeId scope = add(g, pc::NodeKind::Scope, "scope");
        const pc::NodeId scopeAudio = add(g, pc::NodeKind::Scope, "scope audio", pc::PortType::Audio);
        const pc::NodeId chan = add(g, pc::NodeKind::Channel, "chan");
        const pc::NodeId demod = add(g, pc::NodeKind::Demod, "demod");
        const pc::NodeId demod2 = add(g, pc::NodeKind::Demod, "demod2");
        CHECK(g.find(mix)->inputs.size() == pc::kMixerInputs && g.find(mix)->outputs.size() == 1);
        for (pc::PortType t : g.find(mix)->inputs) { CHECK(t == pc::PortType::Audio); }
        CHECK(g.find(mix)->outputs[0] == pc::PortType::Audio);
        CHECK(g.find(iff)->inputs.size() == 1 && g.find(iff)->inputs[0] == pc::PortType::Iq && g.find(iff)->outputs[0] == pc::PortType::Iq);
        CHECK(g.find(gen)->inputs.empty() && g.find(gen)->outputs.size() == 1 && g.find(gen)->outputs[0] == pc::PortType::Iq);
        CHECK(g.find(scope)->inputs[0] == pc::PortType::Iq && g.find(scope)->outputs.empty());
        CHECK(g.find(scopeAudio)->inputs[0] == pc::PortType::Audio);
        // the ids of the new kinds do not disturb the old numbering
        CHECK(static_cast<unsigned>(pc::NodeKind::Map) == 6u && static_cast<unsigned>(pc::NodeKind::Mixer) == 7u && static_cast<unsigned>(pc::NodeKind::Scope) == 10u);
        CHECK(g.connect(radio, 0, iff, 0) == pc::Connect::Ok);
        CHECK(g.connect(iff, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(gen, 0, scope, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, mix, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod2, 0, mix, 0) == pc::Connect::InputOccupied);   // each mixer input takes one wire
        CHECK(g.connect(demod2, 0, mix, 1) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, scopeAudio, 0) == pc::Connect::Ok);
        CHECK(g.connect(mix, 0, scope, 0) == pc::Connect::TypeMismatch);       // audio into an I/Q scope
        CHECK(g.connect(mix, 0, mix, 2) == pc::Connect::SelfLoop);
        CHECK(g.connect(mix, 0, demod, 0) == pc::Connect::TypeMismatch);
        CHECK(g.connect(mix, 0, scopeAudio, 7) == pc::Connect::NoSuchPort);
    }

    std::printf("B: the generator is refused on every transmit sink\n");
    {
        const std::vector<std::string> transmitting = {"tx", "tx:hackrf", "transmit", "transmit:0", "hackrf_tx", "pluto|tx", "soapy|driver=hackrf,tx=1", "audio:tx loop", "audio:Transmit Loop", "Transmit", "TX", "rf-out", "antenna", "network:broadcast", "udp:239.0.0.1", "fm-transmitter"};
        for (const std::string& dev : transmitting) {
            pc::Graph g;
            const pc::NodeId gen = add(g, pc::NodeKind::Generator, "gen");
            const pc::NodeId sink = add(g, pc::NodeKind::Sink, "sink", pc::PortType::Iq);
            g.mutableNode(sink)->device = dev;
            const pc::Connect r = g.connect(gen, 0, sink, 0);
            if (r != pc::Connect::TransmitRefused) { std::printf("  NOT refused: \"%s\" -> %d\n", dev.c_str(), static_cast<int>(r)); }
            CHECK(r == pc::Connect::TransmitRefused);
            CHECK(g.wires().empty());
            // and not through the document loader either: a saved patch cannot smuggle the wire in
            pc::Graph same = g;
            const std::string doc = pc::serialise(same, 0, 0, 1) + "wire " + std::to_string(gen) + " 0 " + std::to_string(sink) + " 0\n";
            const pc::LoadResult lr = pc::parse(doc, pc::Ids::Keep);
            CHECK(lr.graph.wires().empty() && lr.dropped >= 1);
        }
        for (const std::string& dev : std::vector<std::string>{"", "wav", "mp3", "speakers", "iq", "iqfile", "audio:Speakers (Realtek)"}) {
            pc::Graph g;
            const pc::NodeId gen = add(g, pc::NodeKind::Generator, "gen");
            const pc::NodeId sink = add(g, pc::NodeKind::Sink, "sink", pc::PortType::Iq);
            g.mutableNode(sink)->device = dev;
            CHECK(g.connect(gen, 0, sink, 0) == pc::Connect::Ok);
        }
        // a radio's wire to the same sink is unaffected (the rule is about the generator)
        pc::Graph g;
        const pc::NodeId radio = add(g, pc::NodeKind::Radio, "radio");
        const pc::NodeId sink = add(g, pc::NodeKind::Sink, "sink", pc::PortType::Iq);
        g.mutableNode(sink)->device = "tx";
        CHECK(g.connect(radio, 0, sink, 0) == pc::Connect::Ok);
    }

    std::printf("C: bypass\n");
    {
        pc::Graph g;
        const pc::NodeId radio = add(g, pc::NodeKind::Radio, "radio");
        const pc::NodeId a = add(g, pc::NodeKind::IfFilter, "a");
        const pc::NodeId b = add(g, pc::NodeKind::IfFilter, "b");
        const pc::NodeId chan = add(g, pc::NodeKind::Channel, "chan");
        const pc::NodeId demod = add(g, pc::NodeKind::Demod, "demod");
        const pc::NodeId disp = add(g, pc::NodeKind::Display, "disp");   // (an I/Q display: no output)
        CHECK(g.connect(radio, 0, a, 0) == pc::Connect::Ok && g.connect(a, 0, b, 0) == pc::Connect::Ok && g.connect(b, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(!g.setBypass(radio, true));     // no input
        CHECK(!g.setBypass(demod, true));     // Iq in, Audio out
        CHECK(!g.setBypass(disp, true));      // no output
        CHECK(!g.setBypass(999, true));
        CHECK(g.setBypass(chan, false) && g.setBypass(a, true));
        const std::size_t wires = g.wires().size();
        pc::Graph e = g.effective();
        CHECK(e.find(a) == nullptr && e.find(b) != nullptr);
        CHECK(e.wires().size() == wires - 1);
        bool radioToB = false;
        for (const pc::Wire& w : e.wires()) { radioToB = radioToB || (w.from == radio && w.to == b); }
        CHECK(radioToB);
        CHECK(g.find(a) != nullptr && g.wires().size() == wires);   // the document keeps it
        // a chain of bypassed parts resolves; the whole path still connects
        CHECK(g.setBypass(b, true));
        e = g.effective();
        bool radioToChan = false;
        for (const pc::Wire& w : e.wires()) { radioToChan = radioToChan || (w.from == radio && w.to == chan); }
        CHECK(radioToChan && e.nodes().size() == g.nodes().size() - 2);
        // a bypassed part with nothing on input 0 cuts what it fed
        pc::Graph h;
        const pc::NodeId f = add(h, pc::NodeKind::IfFilter, "f");
        const pc::NodeId c2 = add(h, pc::NodeKind::Channel, "c2");
        CHECK(h.connect(f, 0, c2, 0) == pc::Connect::Ok);
        CHECK(h.setBypass(f, true));
        CHECK(h.effective().wires().empty() && h.effective().find(c2) != nullptr);
        // no bypassed part: the same graph
        pc::Graph k;
        add(k, pc::NodeKind::Radio, "r");
        CHECK(pc::graphsEqual(k, k.effective()));
        // and the switch back
        CHECK(g.setBypass(a, false) && g.setBypass(b, false));
        CHECK(pc::graphsEqual(g, g.effective()));
    }

    std::printf("D: the document\n");
    {
        // a patch with none of the new features is written as format 6, exactly as before
        pc::Graph old;
        const pc::NodeId r = add(old, pc::NodeKind::Radio, "radio");
        const pc::NodeId ch = add(old, pc::NodeKind::Channel, "chan");
        CHECK(old.connect(r, 0, ch, 0) == pc::Connect::Ok);
        const std::string t6 = pc::serialise(old, 1.0f, 2.0f, 1.5f);
        CHECK(t6.rfind("foxsdr-patch 6\n", 0) == 0);
        const pc::LoadResult back6 = pc::parse(t6);
        CHECK(back6.ok && back6.dropped == 0 && pc::graphsEqual(old, back6.graph));
        // a bypassed old part, or one of the new kinds, makes it format 7 and everything survives
        pc::Graph g = old;
        const pc::NodeId iff = add(g, pc::NodeKind::IfFilter, "if");
        g.mutableNode(iff)->settings = "low=-3000;high=3000";
        const pc::NodeId mix = add(g, pc::NodeKind::Mixer, "mix");
        g.mutableNode(mix)->settings = "gains=1,0.5,0,0";
        CHECK(g.setBypass(ch, true));
        const std::string t7 = pc::serialise(g, 0, 0, 1);
        CHECK(t7.rfind("foxsdr-patch 7\n", 0) == 0);
        const pc::LoadResult back7 = pc::parse(t7, pc::Ids::Keep);
        CHECK(back7.ok && back7.dropped == 0);
        CHECK(back7.graph.find(ch)->bypass && !back7.graph.find(r)->bypass);
        CHECK(back7.graph.find(iff)->settings == "low=-3000;high=3000" && back7.graph.find(mix)->settings == "gains=1,0.5,0,0");
        CHECK(back7.graph.find(iff)->kind == pc::NodeKind::IfFilter && back7.graph.find(mix)->kind == pc::NodeKind::Mixer);
        // the engine's command text is exactly what the writer writes (graphFromCommandText refuses anything else)
        const std::string cmd = pc::graphCommandText(g);
        pc::Graph viaCmd;
        std::string why;
        CHECK(pc::graphFromCommandText(cmd, viaCmd, why));
        CHECK(pc::graphsEqual(g, viaCmd));
        // a settings text with spaces and odd characters survives
        g.mutableNode(iff)->settings = "a b;c=100%;d=é";
        pc::Graph viaCmd2;
        CHECK(pc::graphFromCommandText(pc::graphCommandText(g), viaCmd2, why));
        CHECK(viaCmd2.find(iff)->settings == "a b;c=100%;d=é");
        // an unknown kind is dropped, not mis-read
        std::string bad = t7;
        const std::size_t p = bad.find("node " + std::to_string(iff) + " 8 ");
        CHECK(p != std::string::npos);
        bad.replace(p, std::string("node " + std::to_string(iff) + " 8 ").size(), "node " + std::to_string(iff) + " 99 ");
        CHECK(pc::parse(bad, pc::Ids::Keep).dropped >= 1);
        // the draft rebase carries the two new fields
        pc::Graph base = g, draft = g, now = g;
        draft.mutableNode(iff)->settings = "changed";
        CHECK(draft.setBypass(iff, true));
        const pc::Graph merged = pc::rebaseDraft(base, draft, now);
        CHECK(merged.find(iff)->settings == "changed" && merged.find(iff)->bypass);
        CHECK(!pc::graphsEqual(base, draft));
    }

    std::printf("E: the plan says what this build does not run\n");
    {
        pc::Graph g;
        const pc::NodeId radio = add(g, pc::NodeKind::Radio, "radio");
        g.mutableNode(radio)->device = "siggen";
        const pc::NodeId iff = add(g, pc::NodeKind::IfFilter, "if");
        CHECK(g.connect(radio, 0, iff, 0) == pc::Connect::Ok);
        const pc::Plan plan = pc::compile(g, 2.4e6, 100.0e6);
        bool said = false;
        for (const pc::NodeProblem& p : plan.problems) { said = said || (p.node == iff && p.problem == pc::Problem::NodeNotRun); }
        CHECK(said);
        CHECK(pc::isAdvisory(pc::Problem::NodeNotRun));
        CHECK(std::string(pc::problemText(pc::Problem::NodeNotRun)).find("does not run") != std::string::npos);
        // an old patch has no such problem
        pc::Graph o;
        add(o, pc::NodeKind::Radio, "r");
        for (const pc::NodeProblem& p : pc::compile(o, 2.4e6, 100.0e6).problems) { CHECK(p.problem != pc::Problem::NodeNotRun); }
    }
    return testSummary("test_patch_nodes_wire_a");
}
