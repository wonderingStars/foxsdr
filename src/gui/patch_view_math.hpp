// patch_view_math.hpp - where everything on the patch canvas sits, and what
// the pointer is over. No ImGui.
//
// gui/patch_view.cpp draws the canvas; this decides the geometry it draws and
// answers every question a click asks. The split is the one
// instrument_meter_math.hpp keeps from instrument_meter.cpp, and the reason is
// sharper here than usual: hit-testing is the part of a patcher that goes
// wrong in a way nobody reports as a bug. A port whose clickable circle sits a
// few pixels from where it is drawn just feels like a fiddly application, and
// no amount of staring at the rendering finds it. Pinned here, it is arithmetic
// with an expected answer.
//
// ONE COORDINATE RULE, because mixing these up is the classic patcher bug.
// Node positions, port positions and wires are all in WORLD space - the
// canvas's own units, unaffected by where the user has scrolled or how far
// they have zoomed. Only the drawing converts to screen space, at the last
// moment, through worldToScreen(). Anything that hit-tests does it in world
// space by converting the pointer ONCE. A function here that took a screen
// position and a world position together would be a bug waiting to happen, so
// none of them do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PATCH_VIEW_MATH_HPP
#define CASCADE_GUI_PATCH_VIEW_MATH_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/patch_graph.hpp"

namespace cascade::gui::patch {

using cascade::core::patch::Graph;
using cascade::core::patch::kNoNode;
using cascade::core::patch::Node;
using cascade::core::patch::NodeId;
using cascade::core::patch::PortIndex;

// A two-float point. Deliberately NOT ImVec2: this header is tested without a
// graphics context, and taking the type from ImGui would drag imgui.h into
// every test that includes it.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline bool operator==(const Vec2& a, const Vec2& b) { return a.x == b.x && a.y == b.y; }

// --- the box ------------------------------------------------------------------
//
// Stated once so the drawing and the hit-testing cannot disagree about where a
// port is. Every value is in world units, which are logical pixels at zoom 1.
inline constexpr float kNodeWidth = 150.0f;
inline constexpr float kHeaderHeight = 22.0f;
inline constexpr float kPortPitch = 18.0f;
inline constexpr float kFirstPortY = kHeaderHeight + 14.0f;
inline constexpr float kBottomPad = 10.0f;
inline constexpr float kMinNodeHeight = 54.0f;

// How close the pointer must be to a port to grab it. Generous on purpose: a
// port is a 4 px dot and nobody should have to aim at one.
inline constexpr float kPortGrabRadius = 9.0f;

inline constexpr float kMinZoom = 0.35f;
inline constexpr float kMaxZoom = 2.50f;

inline float nodeHeight(std::size_t inputs, std::size_t outputs) {
    const std::size_t rows = std::max(inputs, outputs);
    const float needed = kFirstPortY + static_cast<float>(rows) * kPortPitch + kBottomPad;
    return std::max(kMinNodeHeight, needed);
}

inline float nodeHeight(const Node& n) { return nodeHeight(n.inputs.size(), n.outputs.size()); }

inline Vec2 nodeSize(const Node& n) { return Vec2{kNodeWidth, nodeHeight(n)}; }

// Ports sit ON the edge, not inside it, so a wire meets the box exactly where
// the dot is drawn.
inline Vec2 inputPortPos(const Node& n, PortIndex i) {
    return Vec2{n.x, n.y + kFirstPortY + static_cast<float>(i) * kPortPitch};
}

inline Vec2 outputPortPos(const Node& n, PortIndex i) {
    return Vec2{n.x + kNodeWidth, n.y + kFirstPortY + static_cast<float>(i) * kPortPitch};
}

inline bool pointInNode(const Node& n, Vec2 p) {
    const float h = nodeHeight(n);
    return p.x >= n.x && p.x <= n.x + kNodeWidth && p.y >= n.y && p.y <= n.y + h;
}

// The title bar, which is the part a drag moves the node by. Dragging from the
// body would fight with any control that later lives there.
inline bool pointInHeader(const Node& n, Vec2 p) {
    return p.x >= n.x && p.x <= n.x + kNodeWidth && p.y >= n.y && p.y <= n.y + kHeaderHeight;
}

// --- the view -----------------------------------------------------------------

struct View {
    Vec2 pan;             // screen offset, in screen pixels
    float zoom = 1.0f;    // screen pixels per world unit
};

inline Vec2 worldToScreen(const View& v, Vec2 w) {
    return Vec2{w.x * v.zoom + v.pan.x, w.y * v.zoom + v.pan.y};
}

inline Vec2 screenToWorld(const View& v, Vec2 s) {
    return Vec2{(s.x - v.pan.x) / v.zoom, (s.y - v.pan.y) / v.zoom};
}

// Zoom about a fixed SCREEN point, so the thing under the pointer stays under
// the pointer. Zooming about the origin instead is the version that makes a
// canvas feel like it is fighting you.
inline View zoomAbout(const View& v, Vec2 screenAnchor, float factor) {
    View out = v;
    out.zoom = std::clamp(v.zoom * factor, kMinZoom, kMaxZoom);
    const Vec2 w = screenToWorld(v, screenAnchor);
    out.pan.x = screenAnchor.x - w.x * out.zoom;
    out.pan.y = screenAnchor.y - w.y * out.zoom;
    return out;
}

// --- what is under the pointer ------------------------------------------------

// The topmost node containing `p`, or kNoNode. LAST in the list wins, because
// the drawing paints in order and the last one painted is the one on top - the
// eye and the hit-test must agree about which that is.
inline NodeId nodeAt(const Graph& g, Vec2 p) {
    NodeId hit = kNoNode;
    for (const Node& n : g.nodes()) {
        if (pointInNode(n, p)) { hit = n.id; }
    }
    return hit;
}

struct PortHit {
    NodeId node = kNoNode;
    PortIndex port = 0;
    bool input = false;
    bool found = false;
};

// The NEAREST port within the grab radius, searched across every node. Nearest
// rather than first: two ports can be within the radius at once where boxes are
// close, and picking the first would grab whichever happened to be created
// earlier - which looks, from the outside, like the click landing at random.
inline PortHit portAt(const Graph& g, Vec2 p, float radius = kPortGrabRadius) {
    PortHit best;
    float bestDistSq = radius * radius;
    for (const Node& n : g.nodes()) {
        for (PortIndex i = 0; i < static_cast<PortIndex>(n.inputs.size()); ++i) {
            const Vec2 q = inputPortPos(n, i);
            const float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
            if (d <= bestDistSq) {
                bestDistSq = d;
                best = PortHit{n.id, i, true, true};
            }
        }
        for (PortIndex i = 0; i < static_cast<PortIndex>(n.outputs.size()); ++i) {
            const Vec2 q = outputPortPos(n, i);
            const float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
            if (d <= bestDistSq) {
                bestDistSq = d;
                best = PortHit{n.id, i, false, true};
            }
        }
    }
    return best;
}

// --- wires --------------------------------------------------------------------

// Control points for the curve between two ports. The tangents are HORIZONTAL
// so a wire always leaves an output to the right and enters an input from the
// left, which is what makes a patch readable as a signal path rather than as a
// bundle of string. The reach grows with distance but is clamped, so short
// wires do not bulge and long ones do not flatten into something that hides
// which port they came from.
inline constexpr float kWireMinReach = 40.0f;
inline constexpr float kWireMaxReach = 140.0f;

inline void wireCurve(Vec2 a, Vec2 b, Vec2& c1, Vec2& c2) {
    const float reach = std::clamp(std::fabs(b.x - a.x) * 0.5f, kWireMinReach, kWireMaxReach);
    c1 = Vec2{a.x + reach, a.y};
    c2 = Vec2{b.x - reach, b.y};
}

inline Vec2 bezier(Vec2 a, Vec2 c1, Vec2 c2, Vec2 b, float t) {
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * t;
    const float w2 = 3.0f * u * t * t;
    const float w3 = t * t * t;
    return Vec2{w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x,
                w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y};
}

// Distance from a point to the wire between two ports, by sampling. Used to let
// a click select or cut a wire.
//
// Sampled rather than solved because an exact point-to-cubic distance is a
// quartic root-find, and the answer only has to be good to a few pixels to pick
// the wire a user aimed at. 24 segments is well under a pixel of chord error at
// the longest wire the clamp above allows.
inline constexpr int kWireSamples = 24;

inline float distanceToWire(Vec2 a, Vec2 b, Vec2 p) {
    Vec2 c1, c2;
    wireCurve(a, b, c1, c2);
    float best = 1e30f;
    Vec2 prev = a;
    for (int i = 1; i <= kWireSamples; ++i) {
        const Vec2 cur = bezier(a, c1, c2, b, static_cast<float>(i) / kWireSamples);
        // Distance from p to the segment prev..cur.
        const float vx = cur.x - prev.x;
        const float vy = cur.y - prev.y;
        const float len2 = vx * vx + vy * vy;
        float t = 0.0f;
        if (len2 > 0.0f) {
            t = std::clamp(((p.x - prev.x) * vx + (p.y - prev.y) * vy) / len2, 0.0f, 1.0f);
        }
        const float dx = prev.x + t * vx - p.x;
        const float dy = prev.y + t * vy - p.y;
        best = std::min(best, std::sqrt(dx * dx + dy * dy));
        prev = cur;
    }
    return best;
}

// The two endpoints of a wire, looked up in the graph. `found` is false when
// either end has gone - which the graph does not allow to persist, but a caller
// holding a stale Wire for one frame must not dereference a hole.
struct WireEnds {
    Vec2 from;
    Vec2 to;
    bool found = false;
};

// What a node is reading right now. Here rather than beside the drawing for
// the same reason Interaction is: it holds no ImGui types, and
// app_window.hpp owns a vector of them and does not include imgui.h.
//
// Kept OUT of Plan, which is a pure function of the graph: a level is live,
// arrives from the radio, and would make compile() untestable folded in.
struct NodeReading {
    cascade::core::patch::NodeId node = kNoNode;
    float db = 0.0f;
};

// --- what the canvas remembers between frames ---------------------------------
//
// All interaction, no document: the document is the Graph. It lives in this
// header rather than beside the drawing because it holds no ImGui types and
// app_window.hpp, which owns one, deliberately does not include imgui.h.
struct Interaction {
    View view;

    // Dragging a node by its header. `grab` is the offset from the node's
    // origin to the pointer, in world units, so the node does not jump to
    // centre itself under the cursor on the first frame.
    NodeId dragNode = kNoNode;
    Vec2 grab;

    // Dragging a wire out of a port.
    bool wiring = false;
    PortHit wireFrom;

    bool panning = false;

    // Set whenever the canvas changes the graph or the view. The owner
    // re-serialises on it and clears it, so the document is rebuilt when
    // it actually moved rather than once a frame forever.
    bool dirty = false;

    // Exactly one of these is set at a time: selecting a node clears the wire
    // and the other way round. Two selections at once would make one Delete
    // key mean two things, and which it meant would depend on what was clicked
    // before whatever you are looking at.
    NodeId selected = kNoNode;
    cascade::core::patch::Wire selectedWire;
    bool wireSelected = false;

    // WHY A REFUSAL IS REMEMBERED WITH A TIME. connect() answers instantly and
    // the wire simply does not appear, which on its own tells the user nothing
    // about why. Holding the reason for a couple of seconds and lettering it
    // by the pointer turns "it did not work" into "those two carry different
    // things" - which is the whole reason connect() returns a named reason
    // rather than a bool.
    cascade::core::patch::Connect refusal = cascade::core::patch::Connect::Ok;
    double refusedAt = 0.0;
    Vec2 refusedNear;
};

inline WireEnds wireEnds(const Graph& g, const cascade::core::patch::Wire& w) {
    const Node* a = g.find(w.from);
    const Node* b = g.find(w.to);
    if (a == nullptr || b == nullptr) { return WireEnds{}; }
    if (w.fromPort >= a->outputs.size() || w.toPort >= b->inputs.size()) { return WireEnds{}; }
    return WireEnds{outputPortPos(*a, w.fromPort), inputPortPos(*b, w.toPort), true};
}

}  // namespace cascade::gui::patch

#endif  // CASCADE_GUI_PATCH_VIEW_MATH_HPP
