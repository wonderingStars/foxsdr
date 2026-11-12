// graph_edit.hpp - UNDO / REDO, COPY / PASTE, MULTI-SELECT and BYPASS for a node graph, on an ABSTRACT graph interface, so the patch canvas can adopt it
// without this file knowing PatchGraph (a forbidden file for this lane). Not wired: the hand-off in docs/feature-records/dspmodels.md says what an
// adapter over PatchGraph has to provide (it is five calls) and which keys and menu rows go on the canvas.
//
// THE INTERFACE. IEditableGraph is what the editor may do to a graph: list nodes and wires, add a node (optionally under a stated id), remove a node
// (its wires go with it), replace a node's data, connect and disconnect. A node is NodeData: id, an opaque `kind` and `payload` (the adapter packs
// whatever the real node holds - a device key, a frequency, a plugin name - into the payload string and unpacks it again), a name, a position, a
// size and a BYPASS flag. A graph may REFUSE any of these (the radio cap, a wire that would close a loop): the editor then records nothing.
//
// THE RULES (each tested against a reference graph with random edit sequences: undo everything returns to the start, redo everything to the end):
//   - Every edit is one undoable entry: add, remove (nodes and EVERY wire they had, restored with the same ids on undo), connect, disconnect, set data,
//     bypass, paste. Moving nodes MERGES: a drag of many small moves is one entry whose undo goes back to where the drag began.
//   - COPY takes the selected nodes and the wires with BOTH ends inside the selection; positions are stored relative to the top-left of the set.
//     PASTE adds new nodes (the graph chooses their ids), puts them at the paste origin + their relative position, re-creates the internal wires on the
//     new ids and selects the pasted set. PASTE IS ATOMIC: if the graph refuses a node, everything pasted so far is removed again and nothing is
//     recorded. A wire the graph refuses is counted in the result and skipped. Redo of a paste re-creates the SAME ids, so later history stays valid.
//     DUPLICATE is copy followed by paste at an offset.
//   - SELECTION is a set of node ids: replace, add, toggle, rectangle (nodes whose box meets the rectangle), all, clear; it is kept free of ids that
//     no longer exist. It is not part of the history.
//   - BYPASS is a flag per node. effectiveWires() answers what the wires are with bypassed nodes spliced out: a bypassed node passes its input port 0 to
//     its output port 0 (every wire that left output 0 now leaves from whatever fed input 0; a bypassed node with nothing on input 0 cuts what it fed;
//     other ports of a bypassed node are dropped). The runner builds from effectiveWires() instead of wires().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/command_stack.hpp"

namespace cascade::core::edit {

using NodeId = std::uint32_t;
inline constexpr NodeId kNoNodeId = 0;

struct NodeData {
    NodeId id = kNoNodeId;
    std::string kind;
    std::string name;
    std::string payload;
    float x = 0, y = 0, w = 100, h = 60;
    bool bypass = false;
    bool operator==(const NodeData&) const = default;
};

struct WireData {
    NodeId from = kNoNodeId;
    std::uint32_t fromPort = 0;
    NodeId to = kNoNodeId;
    std::uint32_t toPort = 0;
    bool operator==(const WireData&) const = default;
    bool operator<(const WireData& o) const {
        if (from != o.from) { return from < o.from; }
        if (fromPort != o.fromPort) { return fromPort < o.fromPort; }
        if (to != o.to) { return to < o.to; }
        return toPort < o.toPort;
    }
};

class IEditableGraph {
public:
    virtual ~IEditableGraph() = default;
    virtual std::vector<NodeId> nodeIds() const = 0;
    virtual bool getNode(NodeId id, NodeData* out) const = 0;
    virtual std::vector<WireData> wires() const = 0;
    // Adds a node. wantedId != kNoNodeId asks for that id (undo and redo need it); the graph may refuse (returns kNoNodeId).
    virtual NodeId addNode(const NodeData& data, NodeId wantedId) = 0;
    virtual bool removeNode(NodeId id) = 0;  // its wires go with it
    virtual bool setNode(const NodeData& data) = 0;  // replaces everything but the id
    virtual bool connect(const WireData& w) = 0;
    virtual bool disconnect(const WireData& w) = 0;
};

// What copy() keeps: the nodes (ids as they were; positions relative to the set's top-left) and the wires inside the set.
struct Clipboard {
    std::vector<NodeData> nodes;
    std::vector<WireData> wires;
    bool empty() const { return nodes.empty(); }
};

struct PasteResult {
    bool ok = false;
    std::vector<NodeId> newIds;      // in the order of Clipboard::nodes
    std::size_t wiresRefused = 0;
    std::size_t nodesRefused = 0;    // when ok is false: how many nodes the graph refused (0 for an empty clipboard)
};

enum class SelectMode { Replace, Add, Toggle };

class GraphEditor {
public:
    GraphEditor(IEditableGraph& g, std::size_t depth = CommandStack::kDefaultDepth) : g_(&g), stack_(depth) {}

    CommandStack& history() { return stack_; }
    const CommandStack& history() const { return stack_; }
    bool undo() { const bool r = stack_.undo(); prune(); return r; }
    bool redo() { const bool r = stack_.redo(); prune(); return r; }

    // ------------------------------------------------------------------ edits
    // Adds a node (the graph picks the id unless data.id is set); returns its id or kNoNodeId.
    NodeId addNode(const NodeData& data, const std::string& label = "Add node") {
        auto id = std::make_shared<NodeId>(kNoNodeId);  // shared with the stored closures: redo re-creates the node under the SAME id
        const NodeData d = data;
        stack_.execute(
            label,
            [this, d, id]() {
                const NodeId got = g_->addNode(d, *id == kNoNodeId ? d.id : *id);
                if (got == kNoNodeId) { return false; }
                *id = got;
                return true;
            },
            [this, id]() { g_->removeNode(*id); });
        return *id;
    }

    // Removes nodes with every wire they had.
    bool removeNodes(const std::vector<NodeId>& ids, const std::string& label = "Delete") {
        std::vector<NodeData> nodes;
        std::set<NodeId> want(ids.begin(), ids.end());
        for (NodeId id : want) {
            NodeData d;
            if (g_->getNode(id, &d)) { nodes.push_back(d); }
        }
        if (nodes.empty()) { return false; }
        std::vector<WireData> touching;
        for (const WireData& w : g_->wires()) {
            if (want.count(w.from) != 0 || want.count(w.to) != 0) { touching.push_back(w); }
        }
        const bool ok = stack_.execute(
            label,
            [this, nodes]() {
                bool any = false;
                for (const NodeData& n : nodes) { any = g_->removeNode(n.id) || any; }
                return any;
            },
            [this, nodes, touching]() {
                for (const NodeData& n : nodes) { g_->addNode(n, n.id); }
                for (const WireData& w : touching) { g_->connect(w); }
            });
        prune();
        return ok;
    }
    bool removeSelection() { return removeNodes(std::vector<NodeId>(sel_.begin(), sel_.end())); }

    bool connect(const WireData& w) {
        return stack_.execute(
            "Connect", [this, w]() { return g_->connect(w); }, [this, w]() { g_->disconnect(w); });
    }
    bool disconnect(const WireData& w) {
        return stack_.execute(
            "Disconnect", [this, w]() { return g_->disconnect(w); }, [this, w]() { g_->connect(w); });
    }

    // Moves nodes to absolute positions (the drag's merge key keeps a drag as one entry). `to` has one entry per id.
    bool moveNodes(const std::vector<NodeId>& ids, const std::vector<std::pair<float, float>>& to) {
        if (ids.size() != to.size() || ids.empty()) { return false; }
        std::vector<NodeData> before, after;
        bool changes = false;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            NodeData d;
            if (!g_->getNode(ids[i], &d)) { return false; }
            before.push_back(d);
            changes = changes || d.x != to[i].first || d.y != to[i].second;
            d.x = to[i].first;
            d.y = to[i].second;
            after.push_back(d);
        }
        if (!changes) { return false; }
        std::string key = "move";
        for (NodeId id : ids) { key += ':' + std::to_string(id); }
        return stack_.execute(
            "Move", [this, after]() { return applyAll(after); }, [this, before]() { applyAll(before); }, key);
    }

    // Replaces a node's data (name, payload, size ...). The id cannot change.
    bool setNode(const NodeData& next, const std::string& label = "Edit node") {
        NodeData before;
        if (!g_->getNode(next.id, &before) || before == next) { return false; }
        return stack_.execute(
            label, [this, next]() { return g_->setNode(next); }, [this, before]() { g_->setNode(before); });
    }

    // Bypass flag on or off for a set of nodes (one entry).
    bool setBypass(const std::vector<NodeId>& ids, bool on) {
        std::vector<NodeData> before, after;
        for (NodeId id : ids) {
            NodeData d;
            if (!g_->getNode(id, &d) || d.bypass == on) { continue; }
            before.push_back(d);
            d.bypass = on;
            after.push_back(d);
        }
        if (before.empty()) { return false; }
        return stack_.execute(
            on ? "Bypass" : "Remove bypass", [this, after]() { return applyAll(after); }, [this, before]() { applyAll(before); });
    }
    bool toggleBypassSelection() {
        bool anyOn = false;
        for (NodeId id : sel_) {
            NodeData d;
            if (g_->getNode(id, &d) && !d.bypass) { anyOn = true; }
        }
        return setBypass(std::vector<NodeId>(sel_.begin(), sel_.end()), anyOn);  // any not bypassed -> bypass them all; all bypassed -> release
    }

    // ------------------------------------------------------------ copy / paste
    Clipboard copy(const std::vector<NodeId>& ids) const {
        Clipboard c;
        std::set<NodeId> want(ids.begin(), ids.end());
        float minX = 1e30f, minY = 1e30f;
        for (NodeId id : want) {
            NodeData d;
            if (!g_->getNode(id, &d)) { continue; }
            c.nodes.push_back(d);
            minX = std::min(minX, d.x);
            minY = std::min(minY, d.y);
        }
        for (NodeData& n : c.nodes) {
            n.x -= minX;
            n.y -= minY;
        }
        std::set<NodeId> have;
        for (const NodeData& n : c.nodes) { have.insert(n.id); }
        for (const WireData& w : g_->wires()) {
            if (have.count(w.from) != 0 && have.count(w.to) != 0) { c.wires.push_back(w); }
        }
        return c;
    }
    Clipboard copySelection() const { return copy(std::vector<NodeId>(sel_.begin(), sel_.end())); }

    PasteResult paste(const Clipboard& clip, float originX, float originY) {
        PasteResult r;
        if (clip.empty()) { return r; }
        // Do it for real once, inside a group, so that a refusal can be rolled back and the whole is one entry.
        std::vector<NodeId> ids(clip.nodes.size(), kNoNodeId);
        std::vector<WireData> madeWires;
        std::vector<NodeData> placed;
        for (std::size_t i = 0; i < clip.nodes.size(); ++i) {
            NodeData d = clip.nodes[i];
            d.x += originX;
            d.y += originY;
            d.id = kNoNodeId;
            const NodeId got = g_->addNode(d, kNoNodeId);
            if (got == kNoNodeId) {
                for (std::size_t k = 0; k < i; ++k) { g_->removeNode(ids[k]); }
                r.nodesRefused = 1;
                return r;
            }
            ids[i] = got;
            d.id = got;
            placed.push_back(d);
        }
        std::map<NodeId, NodeId> remap;
        for (std::size_t i = 0; i < clip.nodes.size(); ++i) { remap[clip.nodes[i].id] = ids[i]; }
        for (const WireData& w : clip.wires) {
            WireData nw{remap[w.from], w.fromPort, remap[w.to], w.toPort};
            if (g_->connect(nw)) { madeWires.push_back(nw); } else { ++r.wiresRefused; }
        }
        // Record: undo removes the new nodes (their wires go with them); redo puts them back under the SAME ids with the same wires.
        auto firstTime = std::make_shared<bool>(true);
        stack_.execute(
            "Paste",
            [this, firstTime, placed, madeWires]() {
                if (*firstTime) { *firstTime = false; return true; }  // already done above
                for (const NodeData& n : placed) {
                    if (g_->addNode(n, n.id) == kNoNodeId) { return false; }
                }
                for (const WireData& w : madeWires) { g_->connect(w); }
                return true;
            },
            [this, placed]() {
                for (const NodeData& n : placed) { g_->removeNode(n.id); }
            });
        r.ok = true;
        r.newIds = ids;
        sel_ = std::set<NodeId>(ids.begin(), ids.end());
        return r;
    }
    PasteResult pasteSelectionCopy(float dx = 24.0f, float dy = 24.0f) {
        const Clipboard c = copySelection();
        float minX = 1e30f, minY = 1e30f;
        for (NodeId id : sel_) {
            NodeData d;
            if (g_->getNode(id, &d)) { minX = std::min(minX, d.x); minY = std::min(minY, d.y); }
        }
        if (c.empty()) { return PasteResult{}; }
        return paste(c, minX + dx, minY + dy);
    }

    // --------------------------------------------------------------- selection
    const std::set<NodeId>& selection() const { return sel_; }
    void select(NodeId id, SelectMode m = SelectMode::Replace) {
        NodeData d;
        const bool exists = g_->getNode(id, &d);
        if (m == SelectMode::Replace) { sel_.clear(); }
        if (!exists) { return; }
        if (m == SelectMode::Toggle) {
            if (sel_.erase(id) == 0) { sel_.insert(id); }
        } else {
            sel_.insert(id);
        }
    }
    void selectAll() {
        sel_.clear();
        for (NodeId id : g_->nodeIds()) { sel_.insert(id); }
    }
    void clearSelection() { sel_.clear(); }
    // Rectangle (any corner order): nodes whose box (x, y, w, h) touches it. mode Replace starts from nothing, Add keeps the old, Toggle flips.
    void selectRect(float x0, float y0, float x1, float y1, SelectMode m = SelectMode::Replace) {
        const float lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
        std::set<NodeId> hit;
        for (NodeId id : g_->nodeIds()) {
            NodeData d;
            if (!g_->getNode(id, &d)) { continue; }
            if (d.x <= hx && d.x + d.w >= lx && d.y <= hy && d.y + d.h >= ly) { hit.insert(id); }
        }
        if (m == SelectMode::Replace) { sel_ = hit; }
        else if (m == SelectMode::Add) { sel_.insert(hit.begin(), hit.end()); }
        else {
            for (NodeId id : hit) { if (sel_.erase(id) == 0) { sel_.insert(id); } }
        }
    }

    // The wires with bypassed nodes spliced out (see the header). Sorted, without duplicates.
    std::vector<WireData> effectiveWires() const {
        std::map<NodeId, bool> bypassed;
        for (NodeId id : g_->nodeIds()) {
            NodeData d;
            if (g_->getNode(id, &d) && d.bypass) { bypassed[id] = true; }
        }
        const std::vector<WireData> all = g_->wires();
        // For a bypassed node: whatever feeds its input 0 (a wire into port 0).
        auto feeder = [&](NodeId id, WireData* out) {
            for (const WireData& w : all) {
                if (w.to == id && w.toPort == 0) { *out = w; return true; }
            }
            return false;
        };
        std::set<WireData> res;
        for (const WireData& w : all) {
            if (bypassed.count(w.to) != 0) { continue; }  // wires INTO a bypassed node are replaced by the spliced ones
            WireData cur = w;
            bool alive = true;
            std::set<NodeId> seen;
            while (bypassed.count(cur.from) != 0) {
                if (cur.fromPort != 0 || seen.count(cur.from) != 0) { alive = false; break; }  // another port of a bypassed node, or a ring of them
                seen.insert(cur.from);
                WireData up;
                if (!feeder(cur.from, &up)) { alive = false; break; }
                cur = WireData{up.from, up.fromPort, w.to, w.toPort};
            }
            if (alive) { res.insert(cur); }
        }
        return std::vector<WireData>(res.begin(), res.end());
    }

private:
    bool applyAll(const std::vector<NodeData>& nodes) {
        bool any = false;
        for (const NodeData& n : nodes) { any = g_->setNode(n) || any; }
        return any;
    }
    void prune() {
        for (auto it = sel_.begin(); it != sel_.end();) {
            NodeData d;
            if (!g_->getNode(*it, &d)) { it = sel_.erase(it); } else { ++it; }
        }
    }

    IEditableGraph* g_;
    CommandStack stack_;
    std::set<NodeId> sel_;
};

}  // namespace cascade::core::edit
