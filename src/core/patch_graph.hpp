// patch_graph.hpp - what a patch IS, with no opinion about how it is drawn.
//
// The Patch page lets a radio, some channels, the decoders we already ship and
// a few displays be wired together on a canvas. This header is the half that
// decides; gui/patch_view.cpp is the half that draws. The split is the same one
// instrument_meter_math.hpp keeps from instrument_meter.cpp, and for the same
// reason: everything below is a pure function of the graph, so
// tests/test_patch_graph.cpp can pin the connection rules, the cycle check and
// the evaluation order without a graphics context.
//
// WHY THE RULES LIVE HERE AND NOT IN THE CANVAS. A patcher has exactly one
// interesting failure mode: a connection that LOOKS made and does nothing. Text
// dropped into an I/Q port, two sources feeding one input, a loop that starves
// the audio thread - each of those draws a perfectly convincing wire. So
// connect() refuses with a REASON rather than returning a bool, the canvas
// shows that reason while the drag is still in the air, and the refusal is
// tested here where it cannot depend on where the pointer happened to be.
//
// WHAT A PORT TYPE MEANS. The four types are not decoration; they are the four
// things that actually move between stages in this product:
//
//   Iq       complex baseband, what a radio produces and a channel narrows
//   Audio    real demodulated samples, what a speaker or a text decoder eats
//   Text     decoded lines, what a decoder emits
//   Control  a tuning request or a squelch gate, which carries no samples
//
// The Iq/Audio distinction is not ours to invent - it is already in the plugin
// ABI, which has CASCADE_CAP_IQ_DECODER for decoders that want baseband and
// CASCADE_CAP_DECODER for those that want demodulated audio. ADS-B is the
// first, ACARS the second. A Decoder node therefore declares which it is, and
// wiring the wrong one is refused here rather than discovered as silence.
//
// POSITIONS LIVE IN THE MODEL, and that is deliberate. A patch is a document
// the user saves and reopens; where they put a node is part of what they
// arranged, not a detail of this frame's rendering.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_GRAPH_HPP
#define CASCADE_CORE_PATCH_GRAPH_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core::patch {

// --- identity ----------------------------------------------------------------
//
// Ids are handed out and never reused within a graph, so a wire cannot end up
// pointing at a different node than the one it was drawn to after a delete and
// an add. Zero is "no node" so a default-constructed reference is invalid
// rather than pointing at whatever was created first.
using NodeId = std::uint32_t;
inline constexpr NodeId kNoNode = 0u;

using PortIndex = std::uint32_t;

// Track (0.99.18): a decoder's map targets - aircraft, ships, stations -
// on their way to a Map part.
enum class PortType : std::uint8_t { Iq, Audio, Text, Control, Track };

enum class NodeKind : std::uint8_t {
    Radio,    // one device; the only node with no input
    Channel,  // mix to DC, low-pass, decimate: Iq in, Iq out
    Demod,    // Iq in, Audio out
    Decoder,  // a plugin: Iq OR Audio in, Text out
    Display,  // a view: consumes, produces nothing
    Sink,     // speaker, recorder, network: consumes, produces nothing
    Map,      // targets from up to kMapInputs decoders on one map (0.99.18)
};

// HOW MANY DECODERS ONE MAP TAKES (owner, 2026-09-23: "allow one map to take
// up to 5 radio inputs") - one per radio a patch can have, so aircraft from
// one radio, ships from another and APRS stations from a third share a map.
inline constexpr std::size_t kMapInputs = 5;

// Why a connection was refused. The canvas shows these while the wire is still
// being dragged, so each one has to name a cause a person can act on.
enum class Connect : std::uint8_t {
    Ok,
    UnknownNode,     // an id that is not in this graph
    NoSuchPort,      // port index past the end of that node's list
    TypeMismatch,    // Text into an Iq port, and so on
    InputOccupied,   // an input takes exactly one wire
    AlreadyWired,    // this exact pair is already connected
    SelfLoop,        // a node wired to itself
    WouldCycle,      // the wire would close a loop
};

struct Wire {
    NodeId from = kNoNode;
    PortIndex fromPort = 0;
    NodeId to = kNoNode;
    PortIndex toPort = 0;
};

inline bool operator==(const Wire& a, const Wire& b) {
    return a.from == b.from && a.fromPort == b.fromPort && a.to == b.to && a.toPort == b.toPort;
}

struct Node {
    NodeId id = kNoNode;
    NodeKind kind = NodeKind::Radio;
    std::string name;
    std::vector<PortType> inputs;
    std::vector<PortType> outputs;
    float x = 0.0f;
    float y = 0.0f;

    // --- how big the node is --------------------------------------------------
    //
    // PER NODE, not per kind, and saved with the patch. A node on this canvas
    // is not a label for an instrument, it IS the instrument - its whole
    // control surface is on its face - so the user sizes it the way they size
    // any other panel: a demodulator they are working can be opened out, and
    // one they are only monitoring squeezed down to its reading.
    //
    // Zero means "this came from a file that predates sizes"; the loader fills
    // it with the kind's default rather than leaving a zero-area node that
    // could never be clicked.
    float w = 0.0f;
    float h = 0.0f;

    // --- what the node is SET to ---------------------------------------------
    //
    // Two fields rather than a generic property bag. A bag is tempting - every
    // kind wants something different - but it pushes every validation to
    // run time and spells each key twice, once where it is written and once
    // where it is read. These are the only settings a node currently has, and
    // a third can be added the day something needs one.
    //
    // WHICH KIND USES WHICH, and an unused field is simply ignored:
    //   Channel  freqHz  the ABSOLUTE frequency the strip is centred on, so a
    //                    patch keeps meaning the same thing when the receiver
    //                    is retuned. The offset from the radio is arithmetic,
    //                    not stored - storing the offset would silently move
    //                    every channel the moment the dial did.
    //   Demod    mode    which demodulator: an index into the host's mode list.
    //   Decoder  plugin  WHICH plugin the node runs: the module key the plugin
    //                    host loaded it under (its file name). A key and not
    //                    a display name, because two plugins may share a
    //                    display name and one module never shares a file.
    //                    Empty means "no plugin chosen", which is a node that
    //                    cannot run and says so.
    //   Radio    device  WHICH radio this node opens (0.99.17): a device key,
    //                    "siggen" for the signal generator or
    //                    "<driver>|<args>" for hardware - see patch_devices.hpp.
    //                    Empty means none chosen yet. freqHz is the radio's
    //                    centre and rateHz its requested sample rate.
    //   Sink     device  WHERE an audio sink's sound goes: "wav", "mp3",
    //                    "speakers" or "audio:<device name>". Empty is "wav",
    //                    which is the owner's rule: sound goes to a file unless
    //                    it is set to a speaker or another device.
    double freqHz = 0.0;
    int mode = 0;
    std::string plugin;
    std::string device;
    double rateHz = 0.0;
    //   Demod    squelch/squelchDb  (0.99.18) whether this demodulator's
    //                    sound is gated, and at what channel power. On by
    //                    default at the receiver's own default of -50 dB: an
    //                    ungated FM demodulator on an empty channel is full-
    //                    scale noise, and every speaker records to a file.
    bool squelch = true;
    float squelchDb = -50.0f;
    //   Radio    on      (0.99.18) this radio's own switch: off, it stays
    //                    closed even while the patch runs. Saved, so a patch
    //                    of five radios comes back with the same ones on.
    bool on = true;
    //   Radio    centreChosen  THIS SESSION ONLY, never saved: freqHz is a
    //                    centre even though it is 0. The file keeps 0 for
    //                    "none chosen" (a format-1 node has no settings), but
    //                    live, 0 Hz on the air is a real centre - a dongle on
    //                    125 MHz behind a 125 MHz up-converter - and the patch
    //                    take-over, the starter patch and a typed centre set
    //                    this so they never go through the 0 sentinel. A
    //                    patch saved with such a node reads back as "none",
    //                    and the radio then reports where it is.
    bool centreChosen = false;
};

// WHETHER A RADIO NODE HAS A CENTRE YET. The document stores 0 for "none
// chosen" (a format-1 node has no settings at all), and that is the only
// value that means it: a Radio's centre is an AIR frequency, and with an up-
// converter in front of the radio it may lie below 0 Hz - a 16.4 kHz station
// with the band centred 300 kHz below it is -283.6 kHz on the air and
// 124.7164 MHz at a radio behind a 125 MHz converter. Reading "<= 0" as
// "none" threw that centre away and left the radio at its driver's default.
// (Channel nodes are stations, and keep their own "> 0" rule.) A live 0 Hz
// centre is marked with Node::centreChosen rather than read off the value.
inline bool radioCentreSet(const Node& n) {
    return std::isfinite(n.freqHz) && (n.centreChosen || n.freqHz != 0.0);
}

// The squelch range the controls offer, in dB of channel power.
inline constexpr float kSquelchMinDb = -120.0f;
inline constexpr float kSquelchMaxDb = 0.0f;

// AT MOST FIVE RADIOS IN ONE PATCH (owner, 2026-09-23: "the ability to add up
// to 5 sdrs"). Each one is a device open, a reader thread and a sample stream
// of its own, so the cap is a promise about what the machine is asked to
// carry, not a limit of the model.
inline constexpr std::size_t kMaxRadios = 5;

// --- the port tables ---------------------------------------------------------
//
// Stated once, here, rather than at each construction site. A node kind whose
// ports are decided in two places is a node kind that will eventually disagree
// with itself.
//
// `feed` says what a Decoder wants and is ignored for every other kind. It
// exists because that is a real distinction in the plugin ABI rather than a
// convenience: an I/Q decoder handed demodulated audio does not degrade, it
// decodes nothing at all.
inline void portsFor(NodeKind kind, PortType feed, std::vector<PortType>& in,
                     std::vector<PortType>& out) {
    in.clear();
    out.clear();
    switch (kind) {
        case NodeKind::Radio:
            out.push_back(PortType::Iq);
            break;
        case NodeKind::Channel:
            in.push_back(PortType::Iq);
            out.push_back(PortType::Iq);
            break;
        case NodeKind::Demod:
            in.push_back(PortType::Iq);
            out.push_back(PortType::Audio);
            break;
        case NodeKind::Decoder:
            in.push_back(feed == PortType::Iq ? PortType::Iq : PortType::Audio);
            out.push_back(PortType::Text);
            // ...and what it puts on a map, for a module that has a track
            // source (the plan says so when one that has none is wired).
            out.push_back(PortType::Track);
            break;
        case NodeKind::Display:
            in.push_back(feed);
            break;
        case NodeKind::Sink:
            in.push_back(feed);
            break;
        case NodeKind::Map:
            for (std::size_t i = 0; i < kMapInputs; ++i) { in.push_back(PortType::Track); }
            break;
    }
}

// --- how big a node starts ---------------------------------------------------
//
// A node opens at the size its own controls need, not at one size for all of
// them: a speaker has a switch and a meter, a demodulator has a mode, a filter
// and a reading, and a display is mostly picture. Starting them all equal
// means every useful node opens too small and every simple one too large, and
// the user's first action on a fresh patch is resizing things.
//
// In world units - logical pixels at zoom 1. Here rather than in the view
// header because the LOADER needs them too, and the loader must not have to
// include anything that draws.
inline void defaultNodeSize(NodeKind kind, float& w, float& h) {
    switch (kind) {
        case NodeKind::Radio: w = 232.0f; h = 128.0f; return;
        case NodeKind::Channel: w = 232.0f; h = 104.0f; return;
        case NodeKind::Demod: w = 232.0f; h = 132.0f; return;
        case NodeKind::Decoder: w = 216.0f; h = 108.0f; return;
        case NodeKind::Display: w = 300.0f; h = 188.0f; return;
        case NodeKind::Sink: w = 216.0f; h = 116.0f; return;
        case NodeKind::Map: w = 420.0f; h = 320.0f; return;
    }
    w = 216.0f;
    h = 108.0f;
}

// The floor a resize may not go below. Small enough to squeeze a node down to
// its title and its reading, large enough that the title bar keeps a grab
// handle and a close key - a node dragged to nothing cannot be dragged back.
inline constexpr float kMinNodeW = 132.0f;
inline constexpr float kMinNodeH = 56.0f;

class Graph {
public:
    // --- building ------------------------------------------------------------

    // A sixth Radio is refused (kNoNode) - here, so the parts bin, the loader
    // and anything written later all meet the same limit.
    NodeId addNode(NodeKind kind, const std::string& name, PortType feed = PortType::Iq,
                   float x = 0.0f, float y = 0.0f) {
        if (kind == NodeKind::Radio && count(NodeKind::Radio) >= kMaxRadios) { return kNoNode; }
        Node n;
        n.id = nextId_++;
        n.kind = kind;
        n.name = name;
        n.x = x;
        n.y = y;
        defaultNodeSize(kind, n.w, n.h);
        portsFor(kind, feed, n.inputs, n.outputs);
        nodes_.push_back(std::move(n));
        return nodes_.back().id;
    }

    // Removing a node takes its wires with it. A wire whose endpoint no longer
    // exists is the one piece of state that would let the canvas draw a line to
    // nowhere and the rebuild dereference a hole, so it is not allowed to
    // survive for even one frame.
    bool removeNode(NodeId id) {
        const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                     [id](const Node& n) { return n.id == id; });
        if (it == nodes_.end()) { return false; }
        nodes_.erase(it);
        wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                    [id](const Wire& w) { return w.from == id || w.to == id; }),
                     wires_.end());
        return true;
    }

    Connect connect(NodeId from, PortIndex fromPort, NodeId to, PortIndex toPort) {
        const Node* src = find(from);
        const Node* dst = find(to);
        if (src == nullptr || dst == nullptr) { return Connect::UnknownNode; }
        if (from == to) { return Connect::SelfLoop; }
        if (fromPort >= src->outputs.size() || toPort >= dst->inputs.size()) {
            return Connect::NoSuchPort;
        }
        if (src->outputs[fromPort] != dst->inputs[toPort]) { return Connect::TypeMismatch; }

        // FAN-IN IS A PROPERTY OF THE TYPE, not of the port.
        //
        // Two sample streams into one input is not a mix, it is an argument
        // about which one wins, and nothing downstream could tell you which
        // did - so Iq, Audio and Control take exactly one wire each. TEXT is
        // genuinely different: merging decoded lines is defined (interleave
        // them, tag each with where it came from) and it is the thing the
        // workbench already does when six ACARS channels land in one window.
        // Refusing it here would have made the canvas unable to express the
        // one arrangement the page exists for.
        const bool mergeable = dst->inputs[toPort] == PortType::Text;

        const Wire want{from, fromPort, to, toPort};
        for (const Wire& w : wires_) {
            if (w == want) { return Connect::AlreadyWired; }
            if (!mergeable && w.to == to && w.toPort == toPort) {
                return Connect::InputOccupied;
            }
        }
        if (reaches(to, from)) { return Connect::WouldCycle; }

        wires_.push_back(want);
        return Connect::Ok;
    }

    bool disconnect(const Wire& w) {
        const auto it = std::find(wires_.begin(), wires_.end(), w);
        if (it == wires_.end()) { return false; }
        wires_.erase(it);
        return true;
    }

    // --- reading -------------------------------------------------------------

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Wire>& wires() const { return wires_; }

    const Node* find(NodeId id) const {
        for (const Node& n : nodes_) {
            if (n.id == id) { return &n; }
        }
        return nullptr;
    }

    // The writable one, DELIBERATELY under a different name rather than as a
    // non-const overload of find(). An overload pair would resolve to the
    // mutable one for every call on a non-const Graph - including the ones
    // inside connect() that only read - and a private overload of it silently
    // makes find() uncallable from outside on a non-const object, which is
    // exactly the error this replaced. Dragging a node on the canvas moves its
    // x/y, so this is the call that does it.
    Node* mutableNode(NodeId id) {
        for (Node& n : nodes_) {
            if (n.id == id) { return &n; }
        }
        return nullptr;
    }

    std::size_t count(NodeKind kind) const {
        std::size_t n = 0;
        for (const Node& node : nodes_) {
            if (node.kind == kind) { ++n; }
        }
        return n;
    }

    // The order the DSP must be built and run in: every node appears after
    // everything that feeds it.
    //
    // DETERMINISTIC BY ID, not by whatever order the ready set happened to fill.
    // The rebuild that follows a rewire runs off the audio thread and is swapped
    // in atomically, so two rebuilds of the same patch producing two different
    // orders would make a fault reproducible only by luck.
    //
    // A graph with a cycle cannot happen through connect(), which refuses one.
    // If one is ever present anyway the unreachable remainder is dropped rather
    // than looping forever - the caller compares size() against nodes().size()
    // to notice.
    std::vector<NodeId> evaluationOrder() const {
        std::vector<NodeId> order;
        order.reserve(nodes_.size());

        std::vector<NodeId> pending;
        pending.reserve(nodes_.size());
        for (const Node& n : nodes_) { pending.push_back(n.id); }
        std::sort(pending.begin(), pending.end());

        std::vector<NodeId> done;
        while (!pending.empty()) {
            NodeId chosen = kNoNode;
            for (NodeId id : pending) {
                bool ready = true;
                for (const Wire& w : wires_) {
                    if (w.to != id) { continue; }
                    if (std::find(done.begin(), done.end(), w.from) == done.end()) {
                        ready = false;
                        break;
                    }
                }
                if (ready) {
                    chosen = id;  // pending is sorted, so this is the lowest ready id
                    break;
                }
            }
            if (chosen == kNoNode) { break; }  // a cycle; stop rather than spin
            done.push_back(chosen);
            order.push_back(chosen);
            pending.erase(std::find(pending.begin(), pending.end(), chosen));
        }
        return order;
    }

private:
    // Is `target` reachable from `start` by following wires forwards? Used to
    // refuse a wire that would close a loop, asked BEFORE the wire is added.
    bool reaches(NodeId start, NodeId target) const {
        std::vector<NodeId> stack{start};
        std::vector<NodeId> seen;
        while (!stack.empty()) {
            const NodeId id = stack.back();
            stack.pop_back();
            if (id == target) { return true; }
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) { continue; }
            seen.push_back(id);
            for (const Wire& w : wires_) {
                if (w.from == id) { stack.push_back(w.to); }
            }
        }
        return false;
    }

    std::vector<Node> nodes_;
    std::vector<Wire> wires_;
    NodeId nextId_ = 1u;  // 0 is kNoNode
};

// THE PATCH TRANSPORT'S TWO RULES about the radios' own switches (0.99.18).
//
// START with no radio switched on switches every radio on: after ALL OFF that
// is the only useful thing START can mean, and a START that runs nothing would
// be a key that does not work. With any radio on, START leaves the switches
// exactly as the user set them. Returns whether a switch changed.
inline bool switchOnForStart(Graph& g) {
    for (const Node& n : g.nodes()) {
        if (n.kind == NodeKind::Radio && n.on) { return false; }
    }
    bool changed = false;
    std::vector<NodeId> radios;
    for (const Node& n : g.nodes()) {
        if (n.kind == NodeKind::Radio) { radios.push_back(n.id); }
    }
    for (const NodeId id : radios) {
        if (Node* n = g.mutableNode(id)) {
            n->on = true;
            changed = true;
        }
    }
    return changed;
}

// ALL OFF switches every radio off - the switches then all read OFF, which is
// what the red key says it does. Returns whether a switch changed.
inline bool switchAllRadiosOff(Graph& g) {
    std::vector<NodeId> on;
    for (const Node& n : g.nodes()) {
        if (n.kind == NodeKind::Radio && n.on) { on.push_back(n.id); }
    }
    for (const NodeId id : on) {
        if (Node* n = g.mutableNode(id)) { n->on = false; }
    }
    return !on.empty();
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_GRAPH_HPP
