// Tests for the patch canvas's navigation maths (gui/patch_view_math.hpp,
// "moving round the canvas"): 0.99.49 beta feedback from a tester on a small
// laptop, whose touchpad has no middle button - and a middle-button drag was
// the only way the canvas panned.
//
//   - drag-pan: the world moves with the pointer;
//   - the wheel: a plain wheel pans (vertical, horizontal, Shift sideways),
//     Ctrl+wheel zooms about the pointer, fractional touchpad notches move a
//     fraction;
//   - the "+" and "-" keys zoom about the canvas centre;
//   - "Fit" puts every node in view, and an empty patch goes home;
//   - what a press lands on, and that ONLY empty canvas pans - a drag that
//     starts on a node, a port or a wire does not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>

#include "core/patch_graph.hpp"
#include "gui/patch_view_math.hpp"
#include "test_check.hpp"

using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::Node;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::PortType;

namespace pv = cascade::gui::patch;
using pv::PressOn;
using pv::Vec2;
using pv::View;

namespace {

Vec2 onScreen(const View& v, Vec2 world) { return pv::worldToScreen(v, world); }

void testDragPan() {
    std::printf("  drag-pan: the world moves with the pointer\n");
    const View v{Vec2{10.0f, -20.0f}, 1.5f};
    const Vec2 world{40.0f, 30.0f};
    const Vec2 before = onScreen(v, world);
    const View moved = pv::panBy(v, Vec2{25.0f, -7.0f});
    const Vec2 after = onScreen(moved, world);
    CHECK_NEAR(after.x - before.x, 25.0, 1e-4);
    CHECK_NEAR(after.y - before.y, -7.0, 1e-4);
    CHECK_NEAR(moved.zoom, 1.5, 1e-6);  // a drag never zooms
    // A drag of nothing is nothing.
    const View still = pv::panBy(v, Vec2{0.0f, 0.0f});
    CHECK(still.pan == v.pan);
}

void testWheelPan() {
    std::printf("  a plain wheel pans; Shift turns it sideways\n");
    const View v{Vec2{100.0f, 50.0f}, 0.8f};
    const Vec2 anchor{300.0f, 200.0f};

    // Two fingers up one notch: the patch moves DOWN, the way a page scrolls.
    pv::WheelInput up;
    up.y = 1.0f;
    const View a = pv::wheelView(v, anchor, up);
    CHECK_NEAR(a.pan.y - v.pan.y, pv::kWheelPanPx, 1e-4);
    CHECK_NEAR(a.pan.x, v.pan.x, 1e-6);
    CHECK_NEAR(a.zoom, v.zoom, 1e-6);  // a plain wheel no longer zooms

    // Down two notches.
    pv::WheelInput down;
    down.y = -2.0f;
    CHECK_NEAR(pv::wheelView(v, anchor, down).pan.y - v.pan.y, -2.0f * pv::kWheelPanPx, 1e-4);

    // Sideways (io.MouseWheelH > 0 scrolls left: the patch moves right).
    pv::WheelInput left;
    left.x = 1.0f;
    const View b = pv::wheelView(v, anchor, left);
    CHECK_NEAR(b.pan.x - v.pan.x, pv::kWheelPanPx, 1e-4);
    CHECK_NEAR(b.pan.y, v.pan.y, 1e-6);

    // Both at once - a diagonal two-finger swipe.
    pv::WheelInput diag;
    diag.x = -0.5f;
    diag.y = 0.25f;
    const View c = pv::wheelView(v, anchor, diag);
    CHECK_NEAR(c.pan.x - v.pan.x, -0.5f * pv::kWheelPanPx, 1e-4);
    CHECK_NEAR(c.pan.y - v.pan.y, 0.25f * pv::kWheelPanPx, 1e-4);

    // Shift sends a vertical wheel sideways (a mouse with no tilt wheel).
    pv::WheelInput shifted;
    shifted.y = 1.0f;
    shifted.shift = true;
    const View d = pv::wheelView(v, anchor, shifted);
    CHECK_NEAR(d.pan.x - v.pan.x, pv::kWheelPanPx, 1e-4);
    CHECK_NEAR(d.pan.y, v.pan.y, 1e-6);

    // No wheel, no change.
    const View e = pv::wheelView(v, anchor, pv::WheelInput{});
    CHECK(e.pan == v.pan);
    CHECK_NEAR(e.zoom, v.zoom, 1e-6);
}

void testCtrlWheelZoomsAbout() {
    std::printf("  Ctrl+wheel zooms about the pointer\n");
    const View v{Vec2{100.0f, 50.0f}, 1.0f};
    const Vec2 anchor{321.0f, 187.0f};
    const Vec2 under = pv::screenToWorld(v, anchor);

    pv::WheelInput in;
    in.y = 1.0f;
    in.ctrl = true;
    const View a = pv::wheelView(v, anchor, in);
    CHECK_NEAR(a.zoom, pv::kWheelZoomStep, 1e-5);
    // The thing under the pointer stays under the pointer.
    const Vec2 back = onScreen(a, under);
    CHECK_NEAR(back.x, anchor.x, 1e-3);
    CHECK_NEAR(back.y, anchor.y, 1e-3);

    // Out one notch is the inverse.
    pv::WheelInput out = in;
    out.y = -1.0f;
    CHECK_NEAR(pv::wheelView(a, anchor, out).zoom, 1.0, 1e-5);

    // A touchpad's fractional notch zooms by a fraction, not a whole step.
    pv::WheelInput half = in;
    half.y = 0.5f;
    CHECK_NEAR(pv::wheelView(v, anchor, half).zoom, std::sqrt(pv::kWheelZoomStep), 1e-5);

    // One huge event is held to kWheelMaxNotches, and the zoom to its limits.
    pv::WheelInput huge = in;
    huge.y = 400.0f;
    CHECK_NEAR(pv::wheelView(v, anchor, huge).zoom,
               std::min(pv::kMaxZoom, std::pow(pv::kWheelZoomStep, pv::kWheelMaxNotches)), 1e-4);
    View deep = v;
    for (int i = 0; i < 60; ++i) { deep = pv::wheelView(deep, anchor, in); }
    CHECK_NEAR(deep.zoom, pv::kMaxZoom, 1e-5);
    View shallow = v;
    for (int i = 0; i < 60; ++i) { shallow = pv::wheelView(shallow, anchor, out); }
    CHECK_NEAR(shallow.zoom, pv::kMinZoom, 1e-5);
    // Ctrl with only a sideways wheel does nothing rather than guess.
    pv::WheelInput side;
    side.x = 1.0f;
    side.ctrl = true;
    CHECK_NEAR(pv::wheelView(v, anchor, side).zoom, v.zoom, 1e-6);
    CHECK(pv::wheelView(v, anchor, side).pan == v.pan);
}

void testZoomKeys() {
    std::printf("  \"+\" and \"-\" zoom about the canvas centre\n");
    const View v{Vec2{-40.0f, 30.0f}, 1.0f};
    const float w = 800.0f, h = 500.0f;
    const Vec2 centre{w * 0.5f, h * 0.5f};
    const Vec2 under = pv::screenToWorld(v, centre);
    const View in = pv::zoomStepView(v, w, h, true);
    CHECK_NEAR(in.zoom, pv::kKeyZoomStep, 1e-5);
    CHECK_NEAR(onScreen(in, under).x, centre.x, 1e-3);
    CHECK_NEAR(onScreen(in, under).y, centre.y, 1e-3);
    const View out = pv::zoomStepView(in, w, h, false);
    CHECK_NEAR(out.zoom, 1.0, 1e-5);
    CHECK_NEAR(out.pan.x, v.pan.x, 1e-3);
    CHECK_NEAR(out.pan.y, v.pan.y, 1e-3);
}

// Every node's box, on screen through `v` drawn at `s`, inside the canvas.
bool allInView(const Graph& g, const View& v, float s, float w, float h) {
    const View dv = pv::drawView(v, s);
    for (const Node& n : g.nodes()) {
        const Vec2 size = pv::nodeSize(n);
        const Vec2 a = onScreen(dv, Vec2{n.x, n.y});
        const Vec2 b = onScreen(dv, Vec2{n.x + size.x, n.y + size.y});
        if (a.x < -0.01f || a.y < -0.01f || b.x > w + 0.01f || b.y > h + 0.01f) { return false; }
    }
    return true;
}

void testFit() {
    std::printf("  Fit: every node in view, centred; an empty patch goes home\n");
    // EMPTY: the home view, whatever the canvas.
    {
        const Graph g;
        const View v = pv::fitView(g, 900.0f, 600.0f, 1.0f);
        CHECK_NEAR(v.pan.x, 0.0, 1e-6);
        CHECK_NEAR(v.pan.y, 0.0, 1e-6);
        CHECK_NEAR(v.zoom, 1.0, 1e-6);
    }
    // A spread-out patch on a small canvas: zoomed out until it fits, the
    // margin kept, and centred.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "Radio", PortType::Iq, -300.0f, 40.0f);
        g.addNode(NodeKind::Channel, "Channel", PortType::Iq, 900.0f, 700.0f);
        const float w = 700.0f, h = 400.0f;
        const View v = pv::fitView(g, w, h, 1.0f);
        CHECK(v.zoom < 1.0f);
        CHECK(v.zoom >= pv::kMinZoom);
        CHECK(allInView(g, v, 1.0f, w, h));
        // Centred: the bounding box's middle is the canvas's middle.
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const Node& n : g.nodes()) {
            const Vec2 s = pv::nodeSize(n);
            x0 = std::min(x0, n.x);
            y0 = std::min(y0, n.y);
            x1 = std::max(x1, n.x + s.x);
            y1 = std::max(y1, n.y + s.y);
        }
        const Vec2 mid = onScreen(v, Vec2{(x0 + x1) * 0.5f, (y0 + y1) * 0.5f});
        CHECK_NEAR(mid.x, w * 0.5f, 0.5);
        CHECK_NEAR(mid.y, h * 0.5f, 0.5);
        // And it is the TIGHT fit on one axis: the margin, not more.
        const float drawnW = (x1 - x0) * v.zoom;
        const float drawnH = (y1 - y0) * v.zoom;
        const bool tightX = std::fabs(drawnW - (w - 2.0f * pv::kFitMarginPx)) < 0.5f;
        const bool tightY = std::fabs(drawnH - (h - 2.0f * pv::kFitMarginPx)) < 0.5f;
        CHECK(tightX || tightY);
    }
    // ONE small node on a big canvas: centred, and not blown up past 1:1.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 2000.0f, -500.0f);
        const View v = pv::fitView(g, 1600.0f, 900.0f, 1.0f);
        CHECK_NEAR(v.zoom, pv::kFitMaxZoom, 1e-6);
        CHECK(allInView(g, v, 1.0f, 1600.0f, 900.0f));
    }
    // AT AN INTERFACE SIZE: the drawn view (zoom x S) is the one that fits,
    // and the persisted zoom carries no S in it.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 0.0f, 0.0f);
        // Wide enough to need zooming out at both sizes, narrow enough that
        // neither reaches kMinZoom.
        g.addNode(NodeKind::Channel, "Channel", PortType::Iq, 800.0f, 300.0f);
        const View v1 = pv::fitView(g, 800.0f, 500.0f, 1.0f);
        const View v2 = pv::fitView(g, 800.0f, 500.0f, 2.0f);
        CHECK(v1.zoom < pv::kFitMaxZoom);
        CHECK(v2.zoom > pv::kMinZoom);
        CHECK(allInView(g, v2, 2.0f, 800.0f, 500.0f));
        CHECK_NEAR(v2.zoom * 2.0f, v1.zoom, 1e-4);
    }
    // A patch too big even at kMinZoom: held there, and centred anyway.
    {
        Graph g;
        g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 0.0f, 0.0f);
        g.addNode(NodeKind::Channel, "Channel", PortType::Iq, 20000.0f, 0.0f);
        const View v = pv::fitView(g, 400.0f, 300.0f, 1.0f);
        CHECK_NEAR(v.zoom, pv::kMinZoom, 1e-6);
        const Node& a = g.nodes().front();
        const Node& b = g.nodes().back();
        const float midX = (a.x + b.x + pv::nodeSize(b).x) * 0.5f;
        CHECK_NEAR(onScreen(v, Vec2{midX, 0.0f}).x, 200.0, 0.5);
    }
}

void testPressTarget() {
    std::printf("  what a press lands on, and only empty canvas pans\n");
    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 0.0f, 0.0f);
    const NodeId chan = g.addNode(NodeKind::Channel, "Channel", PortType::Iq, 400.0f, 200.0f);
    CHECK(g.connect(radio, 0, chan, 0) == Connect::Ok);
    const Node& r = *g.find(radio);
    const Vec2 rs = pv::nodeSize(r);
    constexpr float kZ = 1.0f;
    constexpr float kGrab = 7.0f;

    // THE TITLE BAR: a drag there moves the node - it must not pan.
    const Vec2 header{r.x + 20.0f, r.y + pv::kHeaderHeight * 0.5f};
    CHECK(pv::pressTarget(g, header, kZ, kGrab) == PressOn::Header);
    CHECK(!pv::pressPans(pv::pressTarget(g, header, kZ, kGrab)));
    // The face under it: selects, does not pan.
    const Vec2 body{r.x + rs.x * 0.5f, r.y + rs.y - 20.0f};
    CHECK(pv::pressTarget(g, body, kZ, kGrab) == PressOn::Body);
    CHECK(!pv::pressPans(PressOn::Body));
    // The close key and the grip.
    const pv::Rect ck = pv::closeKeyRect(r);
    CHECK(pv::pressTarget(g, Vec2{(ck.x0 + ck.x1) * 0.5f, (ck.y0 + ck.y1) * 0.5f}, kZ, kGrab) ==
          PressOn::CloseKey);
    CHECK(pv::pressTarget(g, Vec2{r.x + rs.x - 2.0f, r.y + rs.y - 2.0f}, kZ, kGrab) ==
          PressOn::Grip);
    // A port, even a few units off its dot - and the port wins over the node.
    const Vec2 out = pv::outputPortPos(r, 0);
    CHECK(pv::pressTarget(g, Vec2{out.x - 3.0f, out.y + 2.0f}, kZ, kGrab) == PressOn::Port);
    CHECK(!pv::pressPans(PressOn::Port));
    // Halfway along the wire: selects it, does not pan.
    const pv::WireEnds e = pv::wireEnds(g, g.wires().front());
    Vec2 c1, c2;
    pv::wireCurve(e.from, e.to, c1, c2);
    const Vec2 onWire = pv::bezier(e.from, c1, c2, e.to, 0.5f);
    CHECK(pv::pressTarget(g, onWire, kZ, kGrab) == PressOn::Wire);
    CHECK(!pv::pressPans(PressOn::Wire));
    // ...and the grab is in SCREEN pixels: far out, a near miss is a miss.
    const Vec2 nearWire{onWire.x, onWire.y + 10.0f};
    const float miss = pv::distanceToWire(e.from, e.to, nearWire);
    CHECK(miss * 1.0f <= kGrab && miss * 3.0f > kGrab);  // the premise
    CHECK(pv::pressTarget(g, nearWire, 1.0f, kGrab) == PressOn::Wire);
    CHECK(pv::pressTarget(g, nearWire, 3.0f, kGrab) == PressOn::Empty);
    // EMPTY CANVAS pans.
    const Vec2 empty{-500.0f, -500.0f};
    CHECK(pv::pressTarget(g, empty, kZ, kGrab) == PressOn::Empty);
    CHECK(pv::pressPans(pv::pressTarget(g, empty, kZ, kGrab)));
    // An empty patch is all empty canvas.
    CHECK(pv::pressTarget(Graph{}, Vec2{0.0f, 0.0f}, kZ, kGrab) == PressOn::Empty);
}

}  // namespace

int main() {
    std::printf("test_patch_view_nav\n");
    testDragPan();
    testWheelPan();
    testCtrlWheelZoomsAbout();
    testZoomKeys();
    testFit();
    testPressTarget();
    return testSummary("test_patch_view_nav");
}
