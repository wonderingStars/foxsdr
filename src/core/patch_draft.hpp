// patch_draft.hpp - the patch graph as one command, and the window's draft of
// it put back on top of the engine's (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 6, "Design A").
//
// THE SHAPE. The Engine owns the patch graph (Engine::patchGraph_) and runs it.
// The Patch page edits a DRAFT - a plain copy it draws from and writes to - and
// hands the whole draft over as ONE command, FOXAPP_OP_PATCH_SET_GRAPH, whose
// text is written and read here. No new format: the text is the document
// core::patch::serialise writes for config.json, read back by the same
// core::patch::parse, which offers every wire to connect() - so a malformed
// graph can never reach the engine at all.
//
// WHAT THE COMMAND ADDS TO THE DOCUMENT, and why only this. A saved document
// is a new patch: ids are the file's own and remapped, and a Radio's
// centreChosen is session-only and never saved (see core::patch::Node). The
// command is not a new patch; it is the SAME graph, a moment later, so both
// must survive it:
//   - ids, through parse(text, Ids::Keep);
//   - two lines of a kind parse() already skips as "a line from the future":
//       next-id <n>          the graph's next id, so a node deleted last does
//                            not free its id for the next one added;
//       centre-chosen <id>   a node whose Node::centreChosen is set.
// The view line is always "0 0 1": pan and zoom are the window's, not the
// engine's, and are not carried.
//
// THE REBASE. The engine changes the graph on its own while the page is open:
// the patch take-over names a device and a centre, a running radio reports its
// centre, an I/Q recording its rate, START and ALL OFF throw the radios'
// switches. An edit the window makes while one of those lands (a node being
// dragged across several frames, say) must not send the window's stale copy
// of the rest of the graph back over it. rebaseDraft() puts the window's own
// changes - what differs between the draft and the graph it was copied from -
// on top of the engine's graph as it is now, field by field.
//
// Pure functions of graphs, header-only like patch_io.hpp, so
// tests/test_patch_graph_command.cpp pins them without a window or an engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_DRAFT_HPP
#define CASCADE_CORE_PATCH_DRAFT_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"

namespace cascade::core::patch {

inline constexpr const char* kNextIdWord = "next-id";
inline constexpr const char* kCentreChosenWord = "centre-chosen";

// The text FOXAPP_OP_PATCH_SET_GRAPH carries for `g`. Also the window's way of
// asking "is this the same graph?" - two graphs with the same text are the same
// graph to everything that runs or draws it.
inline std::string graphCommandText(const Graph& g) {
    std::string t = serialise(g, 0.0f, 0.0f, 1.0f);
    t += kNextIdWord;
    t += ' ';
    t += std::to_string(g.nextId());
    t += '\n';
    for (const Node& n : g.nodes()) {
        if (!n.centreChosen) { continue; }
        t += kCentreChosenWord;
        t += ' ';
        t += std::to_string(n.id);
        t += '\n';
    }
    return t;
}

// The inverse. False, with `why` a sentence for the user, when the text is not
// a patch or ANY of it could not be honoured: a graph the engine runs is the
// whole draft or nothing, never a quietly repaired part of it. `out` is left
// alone on a refusal.
inline bool graphFromCommandText(const std::string& text, Graph& out, std::string& why) {
    LoadResult r = parse(text, Ids::Keep);
    if (!r.ok) {
        why = "the patch graph was refused: it is not a patch document";
        return false;
    }
    if (r.dropped != 0) {
        why = "the patch graph was refused: " + std::to_string(r.dropped) +
              " of its lines could not be read";
        return false;
    }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string what;
        if (!(s >> what)) { continue; }
        if (what == kNextIdWord) {
            unsigned long next = 0;
            if (!(s >> next) || next > std::numeric_limits<NodeId>::max()) {
                why = "the patch graph was refused: its next id could not be read";
                return false;
            }
            r.graph.reserveIds(static_cast<NodeId>(next));
        } else if (what == kCentreChosenWord) {
            unsigned long id = 0;
            Node* n = nullptr;
            if ((s >> id) && id <= std::numeric_limits<NodeId>::max()) {
                n = r.graph.mutableNode(static_cast<NodeId>(id));
            }
            if (n == nullptr) {
                why = "the patch graph was refused: it marks a centre on a node it does not hold";
                return false;
            }
            n->centreChosen = true;
        }
    }
    out = std::move(r.graph);
    return true;
}

namespace detail {

// Equal, with NaN equal to NaN: a field that holds NaN in both copies has not
// been changed by anyone.
template <typename T>
inline bool sameValue(T a, T b) {
    return a == b || (std::isnan(a) && std::isnan(b));
}

// What a node's first port carries: the value portsFor() was given, exactly
// as serialise() writes it.
inline PortType feedOf(const Node& n) {
    if (!n.inputs.empty()) { return n.inputs[0]; }
    if (!n.outputs.empty()) { return n.outputs[0]; }
    return PortType::Iq;
}

inline bool hasWire(const Graph& g, const Wire& w) {
    return std::find(g.wires().begin(), g.wires().end(), w) != g.wires().end();
}

}  // namespace detail

// The window's edit, put on top of the engine's graph as it is NOW.
//
// `base` is the engine's graph as the draft was copied from it, `draft` is the
// window's copy with its edits, `now` is the engine's graph at this moment.
// What the window changed (draft against base) wins; everything else is
// `now`'s. Field by field, grouped where two fields are one setting: a
// position (x, y), a size (w, h), a centre (freqHz with centreChosen).
//   - a node the window deleted is deleted;
//   - a node the window added is added, under the draft's id;
//   - a node the ENGINE deleted stays deleted, and the window's edits to it
//     go with it;
//   - wires likewise: the window's cuts are cut, its new wires offered to
//     connect() (a refusal drops that wire - the engine's graph stands).
// With `now` equal to `base` the answer is `draft` itself.
inline Graph rebaseDraft(const Graph& base, const Graph& draft, const Graph& now) {
    Graph out = now;
    for (const Node& b : base.nodes()) {
        if (draft.find(b.id) == nullptr) { out.removeNode(b.id); }
    }
    for (const Node& d : draft.nodes()) {
        const Node* b = base.find(d.id);
        if (b == nullptr) { continue; }  // added: below
        Node* o = out.mutableNode(d.id);
        if (o == nullptr) { continue; }  // the engine removed it: that stands
        if (d.name != b->name) { o->name = d.name; }
        if (!detail::sameValue(d.x, b->x) || !detail::sameValue(d.y, b->y)) {
            o->x = d.x;
            o->y = d.y;
        }
        if (!detail::sameValue(d.w, b->w) || !detail::sameValue(d.h, b->h)) {
            o->w = d.w;
            o->h = d.h;
        }
        if (!detail::sameValue(d.freqHz, b->freqHz) || d.centreChosen != b->centreChosen) {
            o->freqHz = d.freqHz;
            o->centreChosen = d.centreChosen;
        }
        if (d.mode != b->mode) { o->mode = d.mode; }
        if (d.plugin != b->plugin) { o->plugin = d.plugin; }
        if (d.device != b->device) { o->device = d.device; }
        if (!detail::sameValue(d.rateHz, b->rateHz)) { o->rateHz = d.rateHz; }
        if (d.squelch != b->squelch) { o->squelch = d.squelch; }
        if (!detail::sameValue(d.squelchDb, b->squelchDb)) { o->squelchDb = d.squelchDb; }
        if (d.on != b->on) { o->on = d.on; }
    }
    for (const Node& d : draft.nodes()) {
        if (base.find(d.id) != nullptr) { continue; }
        const NodeId made = out.addNodeAs(d.id, d.kind, d.name, detail::feedOf(d), d.x, d.y);
        if (made == kNoNode) { continue; }  // the id is taken, or a sixth radio: the engine's graph stands
        if (Node* o = out.mutableNode(made)) { *o = d; }
    }
    for (const Wire& w : base.wires()) {
        if (!detail::hasWire(draft, w)) { out.disconnect(w); }
    }
    for (const Wire& w : draft.wires()) {
        if (!detail::hasWire(base, w) && !detail::hasWire(out, w)) {
            (void)out.connect(w.from, w.fromPort, w.to, w.toPort);
        }
    }
    out.reserveIds(draft.nextId());
    return out;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_DRAFT_HPP
