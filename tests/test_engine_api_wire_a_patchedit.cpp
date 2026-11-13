// test_engine_api_wire_a_patchedit.cpp - THE PATCH PAGE'S EDIT HISTORY THROUGH THE ENGINE (wire-a second pass, item 5): PATCH_UNDO 0x882A, PATCH_REDO 0x882B, PATCH_SELECT 0x882C, PATCH_COPY 0x882D, PATCH_PASTE 0x882E, PATCH_BYPASS 0x882F.
//   A  every graph the page sends is a step: undo gives the earlier graph back (as a new version), redo the later one, a new edit clears what could be redone, the ends are NO_CHANGE;  B  select (replace, add, toggle, clear, all) and
//   its refusals;  C  copy and paste: new ids, the inside wires made again, the pasted set selected, ONE step, an empty clipboard and a sixth radio refused with nothing changed;  D  bypass: a part that can be bypassed is, one that cannot is
//   refused, the document says so (format 7) and keeps the part, off puts it back;  E  the running radios' own state is not a step.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;
namespace pc = cascade::core::patch;

namespace {

FoxCommand op(std::uint32_t o, std::int64_t i0 = 0, double n0 = 0.0, double n1 = 0.0) {
    FoxCommand c = testing::cmd(o, n0, i0, nullptr);
    c.num[1] = n1;
    return c;
}

// The graph the engine holds now, as the page reads it.
pc::Graph engineGraph(Rig& r) {
    r.eng.controlPass();
    pc::Graph g;
    std::string why;
    const std::string text = r.fact("patch.graph");
    CHECK(pc::graphFromCommandText(text, g, why));
    return g;
}

testing::Outcome sendGraph(Rig& r, const pc::Graph& g) {
    const std::string text = pc::graphCommandText(g);
    return r.send(testing::cmd(FOXAPP_OP_PATCH_SET_GRAPH, 0.0, 0), &text);
}

std::string ids(const pc::Graph& g, pc::NodeKind k) {
    std::string out;
    for (const pc::Node& n : g.nodes()) { if (n.kind == k) { out += std::to_string(n.id) + ","; } }
    return out;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_patchedit\n");
    engine_api_test::hermetic("wire_a_patchedit");
    Rig r;
    r.settle(200);

    std::printf("A: undo and redo\n");
    pc::Graph a = engineGraph(r);   // whatever the engine starts with
    const std::size_t startNodes = a.nodes().size();
    const pc::NodeId radio = a.addNode(pc::NodeKind::Radio, "radio", pc::PortType::Iq, 10, 10);
    a.mutableNode(radio)->device = "siggen";
    const pc::NodeId chan = a.addNode(pc::NodeKind::Channel, "chan", pc::PortType::Iq, 300, 10);
    CHECK(a.connect(radio, 0, chan, 0) == pc::Connect::Ok);
    CHECK_EQ(r.fact("patchedit.undo"), std::string("0"));
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(sendGraph(r, a).status(), FOXAPI_OK);
    r.settle(100);
    CHECK_EQ(r.fact("patchedit.undo"), std::string("1"));
    pc::Graph b = engineGraph(r);
    const pc::NodeId demod = b.addNode(pc::NodeKind::Demod, "demod", pc::PortType::Iq, 600, 10);
    CHECK(b.connect(chan, 0, demod, 0) == pc::Connect::Ok);
    CHECK_EQ(sendGraph(r, b).status(), FOXAPI_OK);
    r.settle(100);
    CHECK_EQ(r.fact("patchedit.undo"), std::string("2"));
    CHECK_EQ(engineGraph(r).nodes().size(), startNodes + 3);
    const std::string versionB = r.fact("patch.version");
    // undo: the graph before the demod, as a NEW version
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);
    pc::Graph g = engineGraph(r);
    CHECK_EQ(g.nodes().size(), startNodes + 2);
    CHECK(g.find(demod) == nullptr && g.find(chan) != nullptr);
    CHECK(r.fact("patch.version") != versionB);
    CHECK_EQ(r.fact("patchedit.undo"), std::string("1"));
    CHECK_EQ(r.fact("patchedit.redo"), std::string("1"));
    r.settle(100);   // (a pass goes by: the undo is not recorded as an edit of its own)
    CHECK_EQ(r.fact("patchedit.undo"), std::string("1"));
    CHECK_EQ(r.fact("patchedit.redo"), std::string("1"));
    // redo: the demod is back, wire and all
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_OK);
    g = engineGraph(r);
    CHECK(g.find(demod) != nullptr && g.wires().size() == 2u);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_NO_CHANGE);
    // undo twice reaches the start; one more is NO_CHANGE
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);
    CHECK_EQ(engineGraph(r).nodes().size(), startNodes);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.fact("patchedit.redo"), std::string("2"));
    // a new edit after an undo clears the redo
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_OK);   // (both steps: the radio, the channel and the demod)
    pc::Graph c = engineGraph(r);
    c.mutableNode(chan)->name = "renamed";
    CHECK_EQ(sendGraph(r, c).status(), FOXAPI_OK);
    r.settle(100);
    CHECK_EQ(r.fact("patchedit.redo"), std::string("0"));
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_REDO)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);
    CHECK(engineGraph(r).find(chan)->name == "chan");

    std::printf("B: selection\n");
    {
        const std::string both = "{\"ids\":[" + std::to_string(radio) + "," + std::to_string(chan) + "]}";
        const std::string one = "{\"ids\":[" + std::to_string(demod) + "]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_COPY)).status(), FOXAPI_NO_CHANGE);   // nothing selected
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &both).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("patchedit.selection"), std::to_string(radio) + "," + std::to_string(chan));
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &both).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 1), &one).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 2), &one).status(), FOXAPI_OK);   // toggled off again
        CHECK_EQ(r.fact("patchedit.selection"), std::to_string(radio) + "," + std::to_string(chan));
        const std::string gone = "{\"ids\":[4000]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &gone).status(), FOXAPI_BAD_ARGUMENT);
        const std::string junk = "{nope";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &junk).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 7), &both).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.fact("patchedit.selection"), std::to_string(radio) + "," + std::to_string(chan));   // the refusals changed nothing
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("patchedit.selection"), std::string());
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 4)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &both).status(), FOXAPI_OK);
    }

    std::printf("C: copy and paste\n");
    {
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_PASTE, 0, 500, 500)).status(), FOXAPI_NO_CHANGE);   // empty clipboard
        const testing::Outcome copy = r.send(op(FOXAPP_OP_PATCH_COPY));
        CHECK_EQ(copy.status(), FOXAPI_OK);
        CHECK_EQ(copy.result.applied[0], 2.0);
        CHECK_EQ(copy.result.applied[1], 1.0);   // the wire inside the selection
        CHECK_EQ(r.fact("patchedit.clipboard"), std::string("2"));
        const std::size_t before = engineGraph(r).nodes().size();
        const std::string undoBefore = r.fact("patchedit.undo");
        const testing::Outcome paste = r.send(op(FOXAPP_OP_PATCH_PASTE, 0, 500, 400));
        CHECK_EQ(paste.status(), FOXAPI_OK);
        CHECK_EQ(paste.result.applied[0], 2.0);
        CHECK_EQ(paste.result.applied[1], 0.0);
        pc::Graph p = engineGraph(r);
        CHECK_EQ(p.nodes().size(), before + 2);
        CHECK_EQ(p.wires().size(), static_cast<std::size_t>(2) + 1u);
        // the new ids are new, the radio keeps its device, the positions are the origin plus the relative place
        std::vector<pc::NodeId> fresh;
        for (const pc::Node& n : p.nodes()) { if (n.id != radio && n.id != chan && n.id != demod && n.id > demod) { fresh.push_back(n.id); } }
        CHECK_EQ(fresh.size(), static_cast<std::size_t>(2));
        if (fresh.size() == 2) {
            const pc::Node* nr = p.find(fresh[0]);
            const pc::Node* nc = p.find(fresh[1]);
            CHECK(nr->kind == pc::NodeKind::Radio && nr->device == "siggen" && nc->kind == pc::NodeKind::Channel);
            CHECK(std::fabs(nr->x - 500.0f) < 1e-3f && std::fabs(nr->y - 400.0f) < 1e-3f);
            CHECK(std::fabs(nc->x - (500.0f + 290.0f)) < 1e-3f);
            CHECK_EQ(r.fact("patchedit.selection"), std::to_string(fresh[0]) + "," + std::to_string(fresh[1]));
            bool wired = false;
            for (const pc::Wire& w : p.wires()) { wired = wired || (w.from == fresh[0] && w.to == fresh[1]); }
            CHECK(wired);
        }
        // ONE step: undo takes the whole paste away
        r.settle(100);
        CHECK_EQ(r.fact("patchedit.undo"), std::to_string(std::stoi(undoBefore) + 1));
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);
        CHECK_EQ(engineGraph(r).nodes().size(), before);
        // a sixth radio: five radios in the patch, copy one and paste it: refused, nothing changes
        pc::Graph five = engineGraph(r);
        while (five.count(pc::NodeKind::Radio) < pc::kMaxRadios) { five.addNode(pc::NodeKind::Radio, "more", pc::PortType::Iq, 0, 900); }
        CHECK_EQ(sendGraph(r, five).status(), FOXAPI_OK);
        // the channel first and a radio second: the channel is pasted before the radio is refused, so the rollback has something to take away
        pc::NodeId extra = pc::kNoNode;
        for (const pc::Node& n : five.nodes()) { if (n.kind == pc::NodeKind::Radio && n.id > chan) { extra = n.id; } }
        const std::string rr = "{\"ids\":[" + std::to_string(chan) + "," + std::to_string(extra) + "]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &rr).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_COPY)).status(), FOXAPI_OK);
        const std::size_t nodes5 = engineGraph(r).nodes().size();
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_PASTE, 0, 10, 10)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(engineGraph(r).nodes().size(), nodes5);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_PASTE, 0, std::nan(""), 0)).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_UNDO)).status(), FOXAPI_OK);   // back to the three-part patch
    }

    std::printf("D: bypass\n");
    {
        const std::string chanOnly = "{\"ids\":[" + std::to_string(chan) + "]}";
        const std::string radioOnly = "{\"ids\":[" + std::to_string(radio) + "]}";
        const std::string demodOnly = "{\"ids\":[" + std::to_string(demod) + "]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1), &radioOnly).status(), FOXAPI_OUT_OF_RANGE);   // a radio has no input
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1), &demodOnly).status(), FOXAPI_OUT_OF_RANGE);   // I/Q in, audio out
        const std::string gone = "{\"ids\":[4000]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1), &gone).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1)).status(), FOXAPI_NO_CHANGE);   // nothing named, nothing selected
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 3), &chanOnly).status(), FOXAPI_OUT_OF_RANGE);
        CHECK(!engineGraph(r).find(chan)->bypass);
        const std::string undoBefore = r.fact("patchedit.undo");
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1), &chanOnly).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1), &chanOnly).status(), FOXAPI_NO_CHANGE);
        CHECK(engineGraph(r).find(chan)->bypass);
        CHECK(r.fact("patch.graph").rfind("foxsdr-patch 7", 0) == 0);   // the document says so, and keeps the part
        CHECK(engineGraph(r).find(chan) != nullptr);
        r.settle(100);
        CHECK_EQ(r.fact("patchedit.undo"), std::to_string(std::stoi(undoBefore) + 1));
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 0), &chanOnly).status(), FOXAPI_OK);
        CHECK(!engineGraph(r).find(chan)->bypass);
        CHECK(r.fact("patch.graph").rfind("foxsdr-patch 6", 0) == 0);   // nothing needs format 7 any more
        // by the selection
        const std::string sel = "{\"ids\":[" + std::to_string(chan) + "]}";
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_SELECT, 0), &sel).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op(FOXAPP_OP_PATCH_BYPASS, 1)).status(), FOXAPI_OK);
        CHECK(engineGraph(r).find(chan)->bypass);
    }

    std::printf("E: the radios' own state is not a step\n");
    {
        r.settle(200);
        const std::string before = r.fact("patchedit.undo");
        pc::Graph g2 = engineGraph(r);
        // a runtime change of a radio (what START and a learnt centre do) goes through the same command text
        g2.mutableNode(radio)->on = !g2.find(radio)->on;
        g2.mutableNode(radio)->freqHz = 123456789.0;
        CHECK_EQ(sendGraph(r, g2).status(), FOXAPI_OK);
        r.settle(100);
        CHECK_EQ(r.fact("patchedit.undo"), before);
    }
    return check::finish("test_engine_api_wire_a_patchedit");
}
