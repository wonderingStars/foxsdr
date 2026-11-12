// Graph editing on an abstract interface (src/core/graph_edit.hpp): undo / redo of every edit, copy / paste with new ids and preserved internal
// links, multi-select, bypass splicing - and RANDOM EDIT SEQUENCES checked against a reference model (every undo lands on the state the model says,
// undoing everything returns to the start, redoing everything to the end).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "core/graph_edit.hpp"
#include "test_check.hpp"

using namespace cascade::core::edit;

namespace {

// A reference graph with the refusals a real one has: eight radios at most, no self-loop, no unknown ends, one wire per input port, no duplicate.
class MemGraph : public IEditableGraph {
public:
    std::vector<NodeId> nodeIds() const override {
        std::vector<NodeId> v;
        for (const auto& kv : nodes_) { v.push_back(kv.first); }
        return v;
    }
    bool getNode(NodeId id, NodeData* out) const override {
        const auto it = nodes_.find(id);
        if (it == nodes_.end()) { return false; }
        *out = it->second;
        return true;
    }
    std::vector<WireData> wires() const override { return std::vector<WireData>(wires_.begin(), wires_.end()); }
    NodeId addNode(const NodeData& d, NodeId wanted) override {
        if (d.kind == "radio") {
            int radios = 0;
            for (const auto& kv : nodes_) { radios += kv.second.kind == "radio" ? 1 : 0; }
            if (radios >= 8) { return kNoNodeId; }
        }
        NodeId id = wanted;
        if (id == kNoNodeId) { id = next_++; }
        else if (nodes_.count(id) != 0) { return kNoNodeId; }
        else if (id >= next_) { next_ = id + 1; }
        NodeData n = d;
        n.id = id;
        nodes_[id] = n;
        return id;
    }
    bool removeNode(NodeId id) override {
        if (nodes_.erase(id) == 0) { return false; }
        for (auto it = wires_.begin(); it != wires_.end();) {
            if (it->from == id || it->to == id) { it = wires_.erase(it); } else { ++it; }
        }
        return true;
    }
    bool setNode(const NodeData& d) override {
        const auto it = nodes_.find(d.id);
        if (it == nodes_.end()) { return false; }
        it->second = d;
        return true;
    }
    bool connect(const WireData& w) override {
        if (nodes_.count(w.from) == 0 || nodes_.count(w.to) == 0 || w.from == w.to) { return false; }
        if (wires_.count(w) != 0) { return false; }
        for (const WireData& o : wires_) {
            if (o.to == w.to && o.toPort == w.toPort) { return false; }
        }
        wires_.insert(w);
        return true;
    }
    bool disconnect(const WireData& w) override { return wires_.erase(w) != 0; }

    std::string snapshot() const {
        std::ostringstream o;
        for (const auto& kv : nodes_) {
            const NodeData& n = kv.second;
            o << "N" << n.id << ' ' << n.kind << ' ' << n.name << ' ' << n.payload << ' ' << n.x << ' ' << n.y << ' ' << n.w << ' ' << n.h << ' ' << n.bypass << '\n';
        }
        for (const WireData& w : wires_) { o << "W" << w.from << ':' << w.fromPort << ">" << w.to << ':' << w.toPort << '\n'; }
        return o.str();
    }

    std::map<NodeId, NodeData> nodes_;
    std::set<WireData> wires_;
    NodeId next_ = 1;
};

NodeData mk(const std::string& kind, float x, float y, const std::string& name = "n") {
    NodeData d;
    d.kind = kind;
    d.x = x;
    d.y = y;
    d.name = name;
    d.payload = kind + "-payload";
    return d;
}

void testBasicEdits() {
    MemGraph g;
    GraphEditor e(g);
    const NodeId a = e.addNode(mk("radio", 0, 0, "A"));
    const NodeId b = e.addNode(mk("channel", 200, 0, "B"));
    const NodeId c = e.addNode(mk("sink", 400, 0, "C"));
    CHECK(a != 0 && b != 0 && c != 0 && a != b && b != c);
    CHECK(e.connect({a, 0, b, 0}) && e.connect({b, 0, c, 0}));
    CHECK(!e.connect({a, 0, b, 0}));    // duplicate: refused, not recorded
    CHECK(!e.connect({a, 0, a, 0}));    // self-loop
    CHECK(!e.connect({a, 0, 999, 0}));  // unknown end
    CHECK(e.history().undoCount() == 5);
    const std::string full = g.snapshot();
    // Delete the middle node: its two wires go with it, and undo brings back the node (same id) and both wires.
    CHECK(e.removeNodes({b}));
    CHECK(g.nodes_.size() == 2 && g.wires_.empty());
    CHECK(e.undo());
    CHECK(g.snapshot() == full);
    CHECK(e.redo() && g.nodes_.size() == 2);
    CHECK(e.undo() && g.snapshot() == full);
    // Removing a node that is not there changes nothing.
    CHECK(!e.removeNodes({777}));
    CHECK(!e.removeNodes({}));
    // Disconnect and its undo.
    CHECK(e.disconnect({a, 0, b, 0}));
    CHECK(!e.disconnect({a, 0, b, 0}));
    CHECK(e.undo() && g.wires_.count({a, 0, b, 0}) == 1);
    // Set data: name, payload, size; an identical node is no edit; an unknown node is refused.
    NodeData d;
    g.getNode(b, &d);
    NodeData d2 = d;
    CHECK(!e.setNode(d));
    d2.name = "renamed";
    d2.payload = "new payload";
    d2.w = 150;
    CHECK(e.setNode(d2));
    NodeData got;
    g.getNode(b, &got);
    CHECK(got == d2);
    CHECK(e.undo());
    g.getNode(b, &got);
    CHECK(got == d);
    NodeData ghost = d;
    ghost.id = 4242;
    CHECK(!e.setNode(ghost));
    // The radio cap refuses the ninth radio and records nothing.
    MemGraph g2;
    GraphEditor e2(g2);
    for (int i = 0; i < 8; ++i) { CHECK(e2.addNode(mk("radio", 10.0f * i, 0)) != 0); }
    const std::size_t n = e2.history().undoCount();
    CHECK(e2.addNode(mk("radio", 0, 0)) == 0);
    CHECK(e2.history().undoCount() == n && g2.nodes_.size() == 8);
    // Undo of an add, then a redo, brings back the SAME id.
    const NodeId x = e2.addNode(mk("sink", 5, 5));
    CHECK(x != 0);
    CHECK(e2.undo() && g2.nodes_.count(x) == 0);
    CHECK(e2.redo() && g2.nodes_.count(x) == 1);
}

void testMoveMerges() {
    MemGraph g;
    GraphEditor e(g);
    const NodeId a = e.addNode(mk("radio", 10, 10)), b = e.addNode(mk("sink", 50, 50));
    const std::size_t before = e.history().undoCount();
    for (int i = 1; i <= 30; ++i) { CHECK(e.moveNodes({a, b}, {{10.0f + i, 10.0f}, {50.0f + i, 50.0f}})); }
    CHECK(e.history().undoCount() == before + 1);  // one drag, one entry
    CHECK(!e.moveNodes({a, b}, {{40.0f, 10.0f}, {80.0f, 50.0f}}));  // not a move at all: nothing recorded
    CHECK(e.undo());
    NodeData n;
    g.getNode(a, &n);
    CHECK(n.x == 10 && n.y == 10);  // back to where the drag began
    g.getNode(b, &n);
    CHECK(n.x == 50);
    CHECK(e.redo());
    g.getNode(a, &n);
    CHECK(n.x == 40);
    // Another set of nodes is another drag.
    CHECK(e.moveNodes({a}, {{1, 1}}));
    CHECK(e.history().undoCount() == before + 2);
    CHECK(!e.moveNodes({a, 999}, {{1, 1}, {2, 2}}));
    CHECK(!e.moveNodes({a}, {}));
}

void testCopyPaste() {
    MemGraph g;
    GraphEditor e(g);
    const NodeId r = e.addNode(mk("radio", 100, 100, "R")), c = e.addNode(mk("channel", 300, 120, "C")), d = e.addNode(mk("demod", 500, 140, "D")),
                 out = e.addNode(mk("sink", 700, 100, "S"));
    e.connect({r, 0, c, 0});
    e.connect({c, 0, d, 0});
    e.connect({d, 0, out, 0});
    // Copy radio + channel + demod: two internal wires, the one to the sink is not inside the set.
    const Clipboard clip = e.copy({r, c, d});
    CHECK(clip.nodes.size() == 3 && clip.wires.size() == 2);
    float minX = 1e9f;
    for (const NodeData& n : clip.nodes) { minX = std::min(minX, n.x); }
    CHECK(minX == 0.0f);  // relative to the top-left of the set
    const std::size_t entries = e.history().undoCount();
    const PasteResult p = e.paste(clip, 100, 400);
    CHECK(p.ok && p.newIds.size() == 3 && p.wiresRefused == 0);
    CHECK(e.history().undoCount() == entries + 1);  // one entry
    CHECK(g.nodes_.size() == 7 && g.wires_.size() == 5);
    std::set<NodeId> old = {r, c, d, out};
    for (NodeId id : p.newIds) { CHECK(old.count(id) == 0 && g.nodes_.count(id) == 1); }  // new ids
    // Internal links preserved on the new ids, none to the old nodes, positions = origin + relative.
    CHECK(g.wires_.count({p.newIds[0], 0, p.newIds[1], 0}) == 1 && g.wires_.count({p.newIds[1], 0, p.newIds[2], 0}) == 1);
    for (const WireData& w : g.wires_) {
        const bool newFrom = std::find(p.newIds.begin(), p.newIds.end(), w.from) != p.newIds.end();
        const bool newTo = std::find(p.newIds.begin(), p.newIds.end(), w.to) != p.newIds.end();
        CHECK(newFrom == newTo);  // a pasted node is never wired to an old one
    }
    NodeData pn;
    g.getNode(p.newIds[0], &pn);
    CHECK(pn.x == 100 && pn.y == 400 && pn.name == "R" && pn.payload == "radio-payload");
    g.getNode(p.newIds[1], &pn);
    CHECK(pn.x == 300 - 100 + 100 && pn.y == 400 + (120 - 100));
    // The pasted set is the selection.
    CHECK(e.selection() == std::set<NodeId>(p.newIds.begin(), p.newIds.end()));
    // Undo removes the whole paste; redo puts the SAME ids back with the same wires.
    const std::string after = g.snapshot();
    CHECK(e.undo());
    CHECK(g.nodes_.size() == 4 && g.wires_.size() == 3);
    CHECK(e.selection().empty());  // pruned: those nodes are gone
    CHECK(e.redo());
    CHECK(g.snapshot() == after);
    // A second paste takes a different set of ids again.
    const PasteResult p2 = e.paste(clip, 0, 600);
    CHECK(p2.ok);
    for (NodeId id : p2.newIds) { CHECK(std::find(p.newIds.begin(), p.newIds.end(), id) == p.newIds.end()); }
    // Duplicate (selection copy at an offset).
    e.select(r);
    e.select(c, SelectMode::Add);
    const PasteResult dup = e.pasteSelectionCopy(10, 10);
    CHECK(dup.ok && dup.newIds.size() == 2);
    NodeData dn;
    g.getNode(dup.newIds[0], &dn);
    CHECK(dn.x == 110 && dn.y == 110);
    CHECK(g.wires_.count({dup.newIds[0], 0, dup.newIds[1], 0}) == 1);
    // Empty clipboard, empty selection.
    CHECK(!e.paste(Clipboard{}, 0, 0).ok);
    e.clearSelection();
    CHECK(!e.pasteSelectionCopy().ok);
    CHECK(e.copy({}).empty() && e.copy({9999}).empty());
}

void testPasteIsAtomic() {
    // Copy two radios while seven radios exist: the second pasted radio would be the ninth. The paste must leave NOTHING behind and record nothing.
    MemGraph g;
    GraphEditor e(g);
    std::vector<NodeId> radios;
    for (int i = 0; i < 7; ++i) { radios.push_back(e.addNode(mk("radio", 100.0f * i, 0))); }
    const NodeId t = e.addNode(mk("text", 0, 300));
    (void)t;
    const Clipboard clip = e.copy({radios[0], radios[1]});
    const std::string before = g.snapshot();
    const std::size_t entries = e.history().undoCount();
    const PasteResult p = e.paste(clip, 0, 500);
    CHECK(!p.ok && p.nodesRefused == 1);
    CHECK(g.snapshot() == before);
    CHECK(e.history().undoCount() == entries);
    // One radio fits.
    const Clipboard one = e.copy({radios[0]});
    CHECK(e.paste(one, 0, 500).ok);
    CHECK(g.nodes_.size() == 9);
}

void testSelection() {
    MemGraph g;
    GraphEditor e(g);
    const NodeId a = e.addNode(mk("radio", 0, 0)), b = e.addNode(mk("sink", 300, 0)), c = e.addNode(mk("sink", 0, 300));
    e.select(a);
    CHECK(e.selection() == std::set<NodeId>({a}));
    e.select(b, SelectMode::Add);
    CHECK(e.selection() == std::set<NodeId>({a, b}));
    e.select(a, SelectMode::Toggle);
    CHECK(e.selection() == std::set<NodeId>({b}));
    e.select(c);
    CHECK(e.selection() == std::set<NodeId>({c}));
    e.select(999, SelectMode::Add);  // unknown id: nothing added
    CHECK(e.selection() == std::set<NodeId>({c}));
    e.select(999);                   // Replace with an unknown id clears
    CHECK(e.selection().empty());
    e.selectAll();
    CHECK(e.selection().size() == 3);
    e.clearSelection();
    // Rectangle: boxes are 100 x 60. A band over the top row meets a and b; touching an edge counts; corners in any order.
    e.selectRect(-10, -10, 400, 50);
    CHECK(e.selection() == std::set<NodeId>({a, b}));
    e.selectRect(400, 50, -10, -10);
    CHECK(e.selection() == std::set<NodeId>({a, b}));
    e.selectRect(100, 0, 200, 10);  // touches a's right edge (x = 100)
    CHECK(e.selection() == std::set<NodeId>({a}));
    e.selectRect(101, 0, 200, 10);
    CHECK(e.selection().empty());
    e.selectRect(-10, 290, 50, 350);
    CHECK(e.selection() == std::set<NodeId>({c}));
    e.selectRect(-10, -10, 50, 50, SelectMode::Add);
    CHECK(e.selection() == std::set<NodeId>({a, c}));
    e.selectRect(-10, -10, 350, 50, SelectMode::Toggle);  // a and b are hit: a leaves, b joins
    CHECK(e.selection() == std::set<NodeId>({b, c}));
    // Delete the selection: it empties itself; undo restores the nodes but not the selection.
    CHECK(e.removeSelection());
    CHECK(e.selection().empty() && g.nodes_.size() == 1);
    CHECK(e.undo());
    CHECK(g.nodes_.size() == 3 && e.selection().empty());
    CHECK(!e.removeSelection());
}

void testBypass() {
    MemGraph g;
    GraphEditor e(g);
    const NodeId r = e.addNode(mk("radio", 0, 0)), f1 = e.addNode(mk("filter", 100, 0)), f2 = e.addNode(mk("filter", 200, 0)), s1 = e.addNode(mk("sink", 300, 0)),
                 s2 = e.addNode(mk("sink", 300, 100));
    e.connect({r, 0, f1, 0});
    e.connect({f1, 0, f2, 0});
    e.connect({f2, 0, s1, 0});
    e.connect({f2, 0, s2, 0});
    // Nothing bypassed: the wires are the wires.
    CHECK(e.effectiveWires() == g.wires());
    // One filter bypassed: radio -> f2 directly.
    CHECK(e.setBypass({f1}, true));
    std::vector<WireData> w = e.effectiveWires();
    CHECK((w == std::vector<WireData>{{r, 0, f2, 0}, {f2, 0, s1, 0}, {f2, 0, s2, 0}}));
    // Both bypassed: radio feeds both sinks.
    CHECK(e.setBypass({f2}, true));
    w = e.effectiveWires();
    CHECK((w == std::vector<WireData>{{r, 0, s1, 0}, {r, 0, s2, 0}}));
    CHECK(!e.setBypass({f1, f2}, true));  // already: no edit
    // Undo / redo the flag.
    CHECK(e.undo());
    NodeData d;
    g.getNode(f2, &d);
    CHECK(!d.bypass);
    CHECK(e.redo());
    g.getNode(f2, &d);
    CHECK(d.bypass);
    // A bypassed node with nothing on input 0 cuts what it fed; a bypassed source feeds nothing.
    CHECK(e.disconnect({r, 0, f1, 0}));
    CHECK(e.effectiveWires().empty());
    CHECK(e.undo());
    CHECK(e.setBypass({r}, true));
    CHECK(e.effectiveWires().empty());
    CHECK(e.setBypass({r}, false));
    // Only port 0 passes: a wire from another output port of a bypassed node is dropped.
    MemGraph g2;
    GraphEditor e2(g2);
    const NodeId a = e2.addNode(mk("radio", 0, 0)), m = e2.addNode(mk("filter", 100, 0)), z = e2.addNode(mk("sink", 200, 0));
    e2.connect({a, 0, m, 0});
    e2.connect({m, 1, z, 0});
    e2.setBypass({m}, true);
    CHECK(e2.effectiveWires().empty());
    // A ring of bypassed nodes terminates and yields nothing.
    MemGraph g3;
    GraphEditor e3(g3);
    const NodeId p = e3.addNode(mk("filter", 0, 0)), q = e3.addNode(mk("filter", 100, 0)), t = e3.addNode(mk("sink", 200, 0));
    e3.connect({p, 0, q, 0});
    e3.connect({q, 0, p, 0});
    e3.connect({q, 0, t, 0});
    e3.setBypass({p, q}, true);
    CHECK(e3.effectiveWires().empty());
    // Toggle on the selection: any not bypassed -> bypass all; all bypassed -> release.
    e3.select(p);
    e3.select(q, SelectMode::Add);
    CHECK(e3.toggleBypassSelection());
    NodeData nd;
    g3.getNode(p, &nd);
    CHECK(!nd.bypass);
    CHECK(e3.toggleBypassSelection());
    g3.getNode(p, &nd);
    CHECK(nd.bypass);
}

// ------------------------------------------------------------- the random test

struct Model {
    std::vector<std::string> states;  // states[0] is the start; the stack's cursor is `cur`
    std::size_t cur = 0;
};

void testRandomEdits() {
    std::size_t totalOps = 0, totalUndos = 0, totalRedos = 0, merged = 0, groups = 0, pastes = 0, refused = 0;
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        std::mt19937_64 rng(seed * 7919u);
        MemGraph g;
        GraphEditor e(g, 10000);
        // Start from a small graph that is NOT created through the editor (so "undo everything" has a real starting point).
        for (int i = 0; i < 3; ++i) { g.addNode(mk("channel", 50.0f * i, 20.0f * i, "s" + std::to_string(i)), kNoNodeId); }
        g.connect({1, 0, 2, 0});
        Model m;
        m.states.push_back(g.snapshot());
        auto ids = [&]() { return g.nodeIds(); };
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        auto randomSubset = [&]() {
            std::vector<NodeId> all = ids(), out;
            for (NodeId id : all) { if (rng() % 3 == 0) { out.push_back(id); } }
            return out;
        };
        std::vector<NodeId> lastDrag;
        const char* kinds[] = {"radio", "channel", "demod", "sink", "text"};
        // One operation; returns true when it should have been recorded as exactly one new entry or merged into the last.
        auto oneOp = [&]() {
            const int k = static_cast<int>(rng() % 12);
            switch (k) {
                case 0: case 1: return e.addNode(mk(kinds[pick(5)], static_cast<float>(rng() % 900), static_cast<float>(rng() % 600), "n" + std::to_string(rng() % 99))) != 0;
                case 2: { auto s = randomSubset(); return e.removeNodes(s); }
                case 3: case 4: {
                    auto all = ids();
                    if (all.size() < 2) { return false; }
                    return e.connect({all[pick(all.size())], static_cast<std::uint32_t>(rng() % 2), all[pick(all.size())], static_cast<std::uint32_t>(rng() % 2)});
                }
                case 5: {
                    auto w = g.wires();
                    if (w.empty()) { return false; }
                    return e.disconnect(w[pick(w.size())]);
                }
                case 6: {
                    // a DRAG: several moves of the same nodes (sometimes the same nodes as the drag before it, which merges into it)
                    std::vector<NodeId> s;
                    if (!lastDrag.empty() && rng() % 2) {
                        for (NodeId id : lastDrag) { NodeData tmp; if (g.getNode(id, &tmp)) { s.push_back(id); } }
                    }
                    if (s.empty()) { s = randomSubset(); }
                    if (s.empty()) { return false; }
                    lastDrag = s;
                    bool any = false;
                    for (int i = 0, n = 1 + static_cast<int>(rng() % 5); i < n; ++i) {
                        std::vector<std::pair<float, float>> to;
                        for (std::size_t j = 0; j < s.size(); ++j) { to.push_back({static_cast<float>(rng() % 900), static_cast<float>(rng() % 600)}); }
                        any = e.moveNodes(s, to) || any;
                    }
                    return any;
                }
                case 7: {
                    auto all = ids();
                    if (all.empty()) { return false; }
                    NodeData d;
                    g.getNode(all[pick(all.size())], &d);
                    d.name += "x";
                    d.payload = "p" + std::to_string(rng() % 9);
                    return e.setNode(d);
                }
                case 8: return e.setBypass(randomSubset(), (rng() % 2) == 0);
                case 9: {
                    e.selectRect(0, 0, static_cast<float>(rng() % 900), static_cast<float>(rng() % 600), SelectMode::Replace);
                    if (rng() % 2) { const PasteResult p = e.pasteSelectionCopy(30, 30); return p.ok; }
                    const Clipboard c = e.copySelection();
                    return e.paste(c, static_cast<float>(rng() % 500), static_cast<float>(rng() % 500)).ok;
                }
                default: return false;
            }
        };
        std::size_t unrecorded = 0;
        for (int step = 0; step < 220; ++step) {
            const int what = static_cast<int>(rng() % 10);
            if (what == 0 && e.history().canUndo()) {
                CHECK(e.undo());
                CHECK(m.cur > 0);
                --m.cur;
                CHECK(g.snapshot() == m.states[m.cur]);
                ++totalUndos;
            } else if (what == 1 && e.history().canRedo()) {
                CHECK(e.redo());
                ++m.cur;
                CHECK(m.cur < m.states.size());
                CHECK(g.snapshot() == m.states[m.cur]);
                ++totalRedos;
            } else if (what == 2) {
                // a group of several edits is ONE entry
                const std::size_t u0 = e.history().undoCount();
                e.history().beginGroup("group");
                int done = 0;
                for (int i = 0; i < 2 + static_cast<int>(rng() % 3); ++i) { done += oneOp() ? 1 : 0; }
                e.history().endGroup();
                ++groups;
                if (done > 0) {
                    CHECK(e.history().undoCount() == u0 + 1);
                    m.states.resize(m.cur + 1);
                    m.states.push_back(g.snapshot());
                    ++m.cur;
                } else {
                    CHECK(e.history().undoCount() == u0);
                    CHECK(g.snapshot() == m.states[m.cur]);
                }
            } else {
                const std::size_t u0 = e.history().undoCount();
                const std::size_t r0 = e.history().redoCount();
                const bool recorded = oneOp();
                ++totalOps;
                if (recorded) {
                    if (e.history().undoCount() == u0) {
                        // merged into the newest entry (a drag): the state moved, the number of entries did not
                        CHECK(m.cur == m.states.size() - 1);
                        m.states[m.cur] = g.snapshot();
                        ++merged;
                    } else {
                        CHECK(e.history().undoCount() == u0 + 1);
                        m.states.resize(m.cur + 1);
                        m.states.push_back(g.snapshot());
                        ++m.cur;
                    }
                } else {
                    ++refused;
                    CHECK(e.history().undoCount() == u0 && e.history().redoCount() == r0);
                    CHECK(g.snapshot() == m.states[m.cur]);
                    ++unrecorded;
                }
            }
            for (NodeId id : e.selection()) {
                NodeData tmp;
                CHECK(g.getNode(id, &tmp));  // the selection never holds a node that is gone
            }
        }
        pastes += 0;
        // Undo EVERYTHING: the start state, exactly. Then redo everything: the end state, exactly.
        const std::string end = g.snapshot();
        const std::size_t depthNow = e.history().undoCount();
        std::size_t n = 0;
        while (e.undo()) { ++n; }
        CHECK(n == depthNow);
        CHECK(g.snapshot() == m.states[0]);
        while (e.redo()) {}
        CHECK(g.snapshot() == m.states.back());
        (void)end;
        (void)unrecorded;
    }
    std::printf("MEAS random edit test: 300 sequences, %zu single ops (%zu refused), %zu groups, %zu merged drags, %zu undos and %zu redos checked step by step\n", totalOps,
                refused, groups, merged, totalUndos, totalRedos);
    CHECK(totalUndos > 2000 && totalRedos > 500 && merged > 100 && groups > 1000);
    (void)pastes;
}

void testDepthCapOnGraph() {
    MemGraph g;
    GraphEditor e(g, 5);
    std::vector<std::string> states;
    states.push_back(g.snapshot());
    for (int i = 0; i < 12; ++i) {
        e.addNode(mk("sink", 10.0f * i, 0));
        states.push_back(g.snapshot());
    }
    CHECK(e.history().undoCount() == 5);
    int n = 0;
    while (e.undo()) { ++n; }
    CHECK(n == 5);
    CHECK(g.snapshot() == states[12 - 5]);  // five steps back, no further
}

}  // namespace

int main() {
    testBasicEdits();
    testMoveMerges();
    testCopyPaste();
    testPasteIsAtomic();
    testSelection();
    testBypass();
    testRandomEdits();
    testDepthCapOnGraph();
    return testSummary("test_graph_edit");
}
