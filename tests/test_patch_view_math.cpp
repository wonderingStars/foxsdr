// Tests for gui/patch_view_math.hpp - where things sit on the patch canvas and
// what the pointer is over.
//
// Hit-testing is the part of a patcher that fails without anyone filing a bug.
// A port whose clickable circle sits four pixels from where it is drawn reads
// as a fiddly application rather than as a defect, and staring at the rendering
// never finds it. So the geometry the drawing uses and the geometry the click
// uses come from the same functions, and those functions are pinned here with
// expected numbers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/patch_view_math.hpp"

#include "core/patch_graph.hpp"
#include "test_check.hpp"

using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::kNoNode;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::PortType;
using cascade::core::patch::Wire;

using cascade::gui::patch::bezier;
using cascade::gui::patch::distanceToWire;
using cascade::gui::patch::inputPortPos;
using cascade::gui::patch::kBottomPad;
using cascade::gui::patch::kFirstPortY;
using cascade::gui::patch::kHeaderHeight;
using cascade::gui::patch::kMaxZoom;
using cascade::gui::patch::kMinNodeHeight;
using cascade::gui::patch::kMinZoom;
using cascade::gui::patch::kNodeWidth;
using cascade::gui::patch::kPortPitch;
using cascade::gui::patch::kWireMaxReach;
using cascade::gui::patch::kWireMinReach;
using cascade::gui::patch::nodeAt;
using cascade::gui::patch::nodeHeight;
using cascade::gui::patch::nodeWidth;
using cascade::gui::patch::kResizeGrip;
using cascade::gui::patch::nodeSize;
using cascade::gui::patch::pointInResizeGrip;
using cascade::gui::patch::resizeNodeTo;
using cascade::gui::patch::outputPortPos;
using cascade::gui::patch::pointInHeader;
using cascade::gui::patch::pointInNode;
using cascade::gui::patch::portAt;
using cascade::gui::patch::PortHit;
using cascade::gui::patch::screenToWorld;
using cascade::gui::patch::Vec2;
using cascade::gui::patch::View;
using cascade::gui::patch::wireCurve;
using cascade::gui::patch::wireEnds;
using cascade::gui::patch::worldToScreen;
using cascade::gui::patch::zoomAbout;

namespace {

// Commas inside BRACES are not protected from the preprocessor, so
// CHECK(f(Vec2{a, b})) is two macro arguments and MSVC quietly expands it with
// only the first - an assertion that compiles, runs and checks the wrong
// thing. Commas inside PARENTHESES are protected, so every point and wire in
// this file is built through a function.
constexpr Vec2 V(float x, float y) { return Vec2{x, y}; }

constexpr Wire W(NodeId from, unsigned fromPort, NodeId to, unsigned toPort) {
    return Wire{from, fromPort, to, toPort};
}

}  // namespace

int main() {
    // [1] A box is tall enough for its ports, and never shorter than the
    // minimum. A node whose ports fall outside its own outline is the most
    // visible way this can be wrong.
    {
        // A one-port node is header + offset + one pitch + pad = 64, NOT the
        // 54 floor. Worth stating plainly because the floor therefore governs
        // only a node with no ports at all - every real kind has at least one,
        // so it is a guard against a future portless node collapsing, not a
        // height anything currently uses.
        const float one = kFirstPortY + kPortPitch + kBottomPad;
        CHECK_NEAR(nodeHeight(0, 1), one, 0.001f);   // a radio: one output
        CHECK_NEAR(nodeHeight(1, 1), one, 0.001f);   // a channel
        CHECK(one > kMinNodeHeight);
        CHECK(nodeHeight(0, 0) == kMinNodeHeight);   // where the floor applies

        const float four = nodeHeight(4, 1);
        const float six = nodeHeight(6, 1);
        CHECK(four > kMinNodeHeight);
        CHECK(six > four);
        CHECK_NEAR(six - four, 2.0f * kPortPitch, 0.001f);
        // Height follows whichever side has MORE ports.
        CHECK(nodeHeight(1, 5) == nodeHeight(5, 1));
    }

    // [2] Ports sit on the edges, evenly spaced, and inside the outline.
    {
        Graph g;
        const NodeId id = g.addNode(NodeKind::Demod, "AM", PortType::Iq, 100.0f, 40.0f);
        const auto& n = *g.find(id);

        CHECK(inputPortPos(n, 0).x == 100.0f);                 // left edge
        CHECK(outputPortPos(n, 0).x == 100.0f + nodeWidth(n)); // right edge
        CHECK(inputPortPos(n, 0).y == outputPortPos(n, 0).y);  // row 0 lines up

        // Every port is vertically within the box, and below the header.
        const float h = nodeHeight(n);
        CHECK(inputPortPos(n, 0).y > n.y + kHeaderHeight);
        CHECK(inputPortPos(n, 0).y < n.y + h);

        // Spacing is the stated pitch. Node is a plain struct, so a
        // many-ported one can be built directly rather than waiting for a node
        // kind that happens to have several - the arithmetic is what is under
        // test, not the port table (test_patch_graph covers that).
        cascade::core::patch::Node wide;
        wide.x = 5.0f;
        wide.y = 7.0f;
        wide.inputs = {PortType::Iq, PortType::Audio, PortType::Text, PortType::Control};
        wide.outputs = {PortType::Text};

        for (unsigned i = 1; i < 4; ++i) {
            CHECK_NEAR(inputPortPos(wide, i).y - inputPortPos(wide, i - 1).y, kPortPitch, 0.001f);
        }
        // All four still fit inside a box sized for them.
        const float wh = nodeHeight(wide);
        CHECK(inputPortPos(wide, 3).y < wide.y + wh);
        CHECK(inputPortPos(wide, 0).x == wide.x);
        CHECK(outputPortPos(wide, 0).x == wide.x + kNodeWidth);
    }

    // [3] The outline, and the header a drag moves the node by.
    {
        Graph g;
        const NodeId id = g.addNode(NodeKind::Channel, "C", PortType::Iq, 10.0f, 20.0f);
        const auto& n = *g.find(id);
        const float h = nodeHeight(n);

        CHECK(pointInNode(n, V(10.0f, 20.0f)));                       // top-left corner
        CHECK(pointInNode(n, V(10.0f + nodeWidth(n), 20.0f + h)));    // bottom-right
        CHECK(pointInNode(n, V(60.0f, 40.0f)));                       // inside
        CHECK(!pointInNode(n, V(9.0f, 40.0f)));                       // just left
        CHECK(!pointInNode(n, V(60.0f, 20.0f + h + 1.0f)));           // just below

        CHECK(pointInHeader(n, V(60.0f, 21.0f)));
        CHECK(!pointInHeader(n, V(60.0f, 20.0f + kHeaderHeight + 1.0f)));
        // The header is part of the node, so anything in it is in both.
        CHECK(pointInNode(n, V(60.0f, 21.0f)));
    }

    // [4] World and screen round-trip. Getting this wrong puts every click a
    // scroll-distance away from where it looked.
    {
        const View v{V(-120.0f, 45.0f), 1.75f};
        for (const Vec2 w : {V(0.0f, 0.0f), V(300.0f, -80.0f), V(-42.5f, 900.0f)}) {
            const Vec2 back = screenToWorld(v, worldToScreen(v, w));
            CHECK_NEAR(back.x, w.x, 0.001f);
            CHECK_NEAR(back.y, w.y, 0.001f);
        }
        // At zoom 1 with no pan the two spaces coincide.
        const View unit{V(0.0f, 0.0f), 1.0f};
        CHECK(worldToScreen(unit, V(7.0f, 9.0f)) == V(7.0f, 9.0f));
    }

    // [5] Zooming keeps whatever is under the pointer under the pointer, and
    // stops at the limits. A canvas that drifts while you zoom feels broken
    // even though nothing is.
    {
        const View v{V(30.0f, -10.0f), 1.0f};
        const Vec2 anchor{400.0f, 250.0f};
        const Vec2 worldUnder = screenToWorld(v, anchor);

        const View zoomed = zoomAbout(v, anchor, 1.6f);
        CHECK_NEAR(zoomed.zoom, 1.6f, 0.0001f);
        const Vec2 nowAt = worldToScreen(zoomed, worldUnder);
        CHECK_NEAR(nowAt.x, anchor.x, 0.01f);
        CHECK_NEAR(nowAt.y, anchor.y, 0.01f);

        // ...and the same holds after zooming out.
        const View out = zoomAbout(zoomed, anchor, 0.25f);
        const Vec2 stillAt = worldToScreen(out, worldUnder);
        CHECK_NEAR(stillAt.x, anchor.x, 0.01f);
        CHECK_NEAR(stillAt.y, anchor.y, 0.01f);

        // Limits hold however hard it is pushed.
        View deep = v;
        for (int i = 0; i < 40; ++i) { deep = zoomAbout(deep, anchor, 2.0f); }
        CHECK_NEAR(deep.zoom, kMaxZoom, 0.0001f);
        View shallow = v;
        for (int i = 0; i < 40; ++i) { shallow = zoomAbout(shallow, anchor, 0.5f); }
        CHECK_NEAR(shallow.zoom, kMinZoom, 0.0001f);
    }

    // [6] Which node the pointer is over, including the overlap rule: the one
    // drawn last is on top, so it must be the one hit.
    {
        Graph g;
        const NodeId under = g.addNode(NodeKind::Channel, "under", PortType::Iq, 0.0f, 0.0f);
        const NodeId over = g.addNode(NodeKind::Channel, "over", PortType::Iq, 20.0f, 10.0f);

        CHECK(nodeAt(g, V(5.0f, 5.0f)) == under);      // only the first covers this
        CHECK(nodeAt(g, V(30.0f, 20.0f)) == over);     // both cover it; last wins
        CHECK(nodeAt(g, V(-50.0f, -50.0f)) == kNoNode);

        Graph empty;
        CHECK(nodeAt(empty, V(0.0f, 0.0f)) == kNoNode);
    }

    // [7] Which PORT the pointer is over. Nearest within the radius, not first
    // found, and inputs are told from outputs.
    {
        Graph g;
        const NodeId a = g.addNode(NodeKind::Radio, "R", PortType::Iq, 0.0f, 0.0f);
        const NodeId b = g.addNode(NodeKind::Channel, "C", PortType::Iq, 300.0f, 0.0f);
        const auto& na = *g.find(a);
        const auto& nb = *g.find(b);

        const PortHit onOut = portAt(g, outputPortPos(na, 0));
        CHECK(onOut.found);
        CHECK(onOut.node == a);
        CHECK(!onOut.input);

        const PortHit onIn = portAt(g, inputPortPos(nb, 0));
        CHECK(onIn.found);
        CHECK(onIn.node == b);
        CHECK(onIn.input);

        // Well away from anything.
        CHECK(!portAt(g, V(160.0f, 400.0f)).found);
        // Just outside the grab radius of a real port.
        const Vec2 near = outputPortPos(na, 0);
        CHECK(!portAt(g, V(near.x + 30.0f, near.y)).found);

        // Two ports within reach at once: the nearer must win. Placed so the
        // input of `c` is close to the output of `a`.
        Graph t;
        const NodeId src = t.addNode(NodeKind::Radio, "R", PortType::Iq, 0.0f, 0.0f);
        const NodeId dst = t.addNode(NodeKind::Channel, "C", PortType::Iq,
                                     nodeWidth(*t.find(src)) + 6.0f, 0.0f);
        const Vec2 srcOut = outputPortPos(*t.find(src), 0);
        const Vec2 dstIn = inputPortPos(*t.find(dst), 0);
        CHECK(portAt(t, V(srcOut.x + 1.0f, srcOut.y)).node == src);
        CHECK(portAt(t, V(dstIn.x - 1.0f, dstIn.y)).node == dst);
    }

    // [8] Wire tangents are horizontal and the reach is clamped at both ends.
    // Horizontal is what makes a patch read left-to-right as a signal path.
    {
        Vec2 c1, c2;

        // A short wire: reach clamped UP to the minimum so it still bulges.
        wireCurve(V(0.0f, 0.0f), V(10.0f, 0.0f), c1, c2);
        CHECK(c1.y == 0.0f);
        CHECK(c2.y == 0.0f);
        CHECK_NEAR(c1.x, kWireMinReach, 0.001f);

        // A long one: clamped DOWN to the maximum so it does not flatten.
        wireCurve(V(0.0f, 0.0f), V(2000.0f, 0.0f), c1, c2);
        CHECK_NEAR(c1.x, kWireMaxReach, 0.001f);
        CHECK_NEAR(c2.x, 2000.0f - kWireMaxReach, 0.001f);

        // Tangents stay horizontal even when the ends are far apart vertically.
        wireCurve(V(0.0f, 0.0f), V(300.0f, 500.0f), c1, c2);
        CHECK(c1.y == 0.0f);
        CHECK(c2.y == 500.0f);

        // ...and when the destination is to the LEFT, which happens whenever a
        // node is dragged back past its source. The curve must still leave to
        // the right and arrive from the left.
        wireCurve(V(400.0f, 0.0f), V(100.0f, 0.0f), c1, c2);
        CHECK(c1.x > 400.0f);
        CHECK(c2.x < 100.0f);
    }

    // [9] The curve actually starts and ends on its ports. A wire that misses
    // the dot it belongs to is the single most visible drawing bug here.
    {
        const Vec2 a{10.0f, 20.0f};
        const Vec2 b{400.0f, 300.0f};
        Vec2 c1, c2;
        wireCurve(a, b, c1, c2);
        const Vec2 start = bezier(a, c1, c2, b, 0.0f);
        const Vec2 end = bezier(a, c1, c2, b, 1.0f);
        CHECK_NEAR(start.x, a.x, 0.0001f);
        CHECK_NEAR(start.y, a.y, 0.0001f);
        CHECK_NEAR(end.x, b.x, 0.0001f);
        CHECK_NEAR(end.y, b.y, 0.0001f);
        // The midpoint of a level wire sits on the same line, by symmetry.
        Vec2 d1, d2;
        const Vec2 la{0.0f, 50.0f};
        const Vec2 lb{400.0f, 50.0f};
        wireCurve(la, lb, d1, d2);
        CHECK_NEAR(bezier(la, d1, d2, lb, 0.5f).y, 50.0f, 0.0001f);
    }

    // [10] Clicking a wire. Near the curve is near; away from it is not.
    {
        const Vec2 a{0.0f, 0.0f};
        const Vec2 b{400.0f, 0.0f};
        CHECK(distanceToWire(a, b, V(0.0f, 0.0f)) < 0.5f);       // on an endpoint
        CHECK(distanceToWire(a, b, V(200.0f, 0.0f)) < 1.0f);     // on the middle
        CHECK(distanceToWire(a, b, V(200.0f, 60.0f)) > 40.0f);   // well off it
        CHECK(distanceToWire(a, b, V(200.0f, 4.0f)) < 6.0f);     // just beside it

        // A wire between two coincident points is a point, not a crash.
        CHECK(distanceToWire(V(5.0f, 5.0f), V(5.0f, 5.0f), V(5.0f, 5.0f)) < 60.0f);

        // A sloping wire: a point on the straight chord is NOT necessarily on
        // the curve, which is the whole reason this samples the curve.
        const float chordMid = distanceToWire(V(0.0f, 0.0f), V(300.0f, 200.0f),
                                              V(150.0f, 100.0f));
        CHECK(chordMid < 30.0f);
    }

    // [11] Endpoints looked up through the graph, and the stale-wire guard.
    {
        Graph g;
        const NodeId r = g.addNode(NodeKind::Radio, "R", PortType::Iq, 0.0f, 0.0f);
        const NodeId c = g.addNode(NodeKind::Channel, "C", PortType::Iq, 300.0f, 60.0f);
        CHECK(g.connect(r, 0, c, 0) == Connect::Ok);

        const auto ends = wireEnds(g, g.wires()[0]);
        CHECK(ends.found);
        CHECK(ends.from == outputPortPos(*g.find(r), 0));
        CHECK(ends.to == inputPortPos(*g.find(c), 0));

        // A wire naming a node that is gone, or a port that does not exist,
        // answers "not found" rather than reading past the end.
        CHECK(!wireEnds(g, W(9999u, 0, c, 0)).found);
        CHECK(!wireEnds(g, W(r, 0, 9999u, 0)).found);
        CHECK(!wireEnds(g, W(r, 7, c, 0)).found);
        CHECK(!wireEnds(g, W(r, 0, c, 7)).found);
    }

    // [12] Moving a node moves its ports and therefore its wires - the
    // property that makes dragging look right.
    {
        Graph g;
        const NodeId r = g.addNode(NodeKind::Radio, "R", PortType::Iq, 0.0f, 0.0f);
        const NodeId c = g.addNode(NodeKind::Channel, "C", PortType::Iq, 300.0f, 0.0f);
        CHECK(g.connect(r, 0, c, 0) == Connect::Ok);
        const auto before = wireEnds(g, g.wires()[0]);

        g.mutableNode(c)->x += 120.0f;
        g.mutableNode(c)->y -= 35.0f;
        const auto after = wireEnds(g, g.wires()[0]);

        CHECK(after.found);
        CHECK(after.from == before.from);                  // the source did not move
        CHECK_NEAR(after.to.x, before.to.x + 120.0f, 0.001f);
        CHECK_NEAR(after.to.y, before.to.y - 35.0f, 0.001f);
    }

    // [S1] EVERY KIND OPENS AT ITS OWN SIZE, and the geometry reads the
    // node's size rather than one width for all. A speaker and a display are
    // not the same shape of instrument.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "R", PortType::Iq, 0.0f, 0.0f);
        const NodeId disp = g.addNode(NodeKind::Display, "S", PortType::Iq, 0.0f, 0.0f);
        const auto& r = *g.find(radio);
        const auto& d = *g.find(disp);
        float rw = 0.0f, rh = 0.0f, dw = 0.0f, dh = 0.0f;
        cascade::core::patch::defaultNodeSize(NodeKind::Radio, rw, rh);
        cascade::core::patch::defaultNodeSize(NodeKind::Display, dw, dh);
        CHECK(r.w == rw);
        CHECK(r.h == rh);
        CHECK(nodeWidth(r) == rw);
        CHECK(nodeHeight(r) == rh);
        CHECK(nodeWidth(d) == dw);
        CHECK(dw > rw);                          // a display is wider than a radio
        CHECK(dw > kNodeWidth);                  // and nothing is stuck at the old 150
        // Every default is at or above the floors, or the floor would be
        // silently resizing a node the first time it is drawn.
        for (const NodeKind k : {NodeKind::Radio, NodeKind::Channel, NodeKind::Demod,
                                 NodeKind::Decoder, NodeKind::Display, NodeKind::Sink}) {
            float w = 0.0f, h = 0.0f;
            cascade::core::patch::defaultNodeSize(k, w, h);
            CHECK(w >= cascade::core::patch::kMinNodeW);
            CHECK(h >= cascade::core::patch::kMinNodeH);
        }
    }

    // [S2] A RESIZED NODE TAKES ITS OUTPUTS WITH IT. The inputs stay on the
    // left edge; the outputs sit on whatever the right edge now is, so a wire
    // still meets the dot exactly - the failure this guards against is a node
    // drawn wide with its ports hit-tested at the old width.
    {
        Graph g;
        const NodeId id = g.addNode(NodeKind::Demod, "AM", PortType::Iq, 40.0f, 30.0f);
        cascade::core::patch::Node& n = *g.mutableNode(id);
        const Vec2 inBefore = inputPortPos(n, 0);
        resizeNodeTo(n, V(40.0f + 400.0f, 30.0f + 300.0f));
        CHECK(n.w == 400.0f);
        CHECK(n.h == 300.0f);
        CHECK(outputPortPos(n, 0).x == 440.0f);
        CHECK(inputPortPos(n, 0).x == inBefore.x);
        CHECK(inputPortPos(n, 0).y == inBefore.y);   // ports do not slide down
        // The grab radius follows the port to its new place.
        CHECK(portAt(g, V(439.0f, outputPortPos(n, 0).y)).found);
        CHECK(!portAt(g, V(40.0f + nodeWidth(n) - 250.0f, outputPortPos(n, 0).y)).found);
        // And the outline answers at the new size.
        CHECK(pointInNode(n, V(435.0f, 325.0f)));
        CHECK(!pointInNode(n, V(445.0f, 325.0f)));
    }

    // [S3] A resize cannot take a node below its floors - not below the
    // minimum size, and not so short that a port falls off its own bottom.
    {
        Graph g;
        const NodeId id = g.addNode(NodeKind::Channel, "C", PortType::Iq, 100.0f, 100.0f);
        cascade::core::patch::Node& n = *g.mutableNode(id);
        resizeNodeTo(n, V(90.0f, 90.0f));        // dragged up and past the origin
        CHECK(n.w == cascade::core::patch::kMinNodeW);
        CHECK(n.h >= cascade::core::patch::kMinNodeH);
        CHECK(n.h >= nodeHeight(n.inputs.size(), n.outputs.size()));
        CHECK(inputPortPos(n, 0).y < n.y + nodeHeight(n));

        // A many-ported node's port floor beats a small requested height.
        cascade::core::patch::Node many;
        many.x = 0.0f;
        many.y = 0.0f;
        many.inputs.assign(8, PortType::Text);
        resizeNodeTo(many, V(200.0f, 60.0f));
        const float floor = nodeHeight(many.inputs.size(), many.outputs.size());
        CHECK(floor > 60.0f);
        CHECK(many.h == floor);
        CHECK(inputPortPos(many, 7).y < many.y + nodeHeight(many));
    }

    // [S4] The grip is the bottom-right corner and nowhere else - in
    // particular not the right EDGE, where the output ports are.
    {
        Graph g;
        const NodeId id = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio, 0.0f, 0.0f);
        const auto& n = *g.find(id);
        const Vec2 s = nodeSize(n);
        CHECK(pointInResizeGrip(n, V(s.x - 2.0f, s.y - 2.0f)));
        CHECK(pointInResizeGrip(n, V(s.x - kResizeGrip, s.y - kResizeGrip)));
        CHECK(!pointInResizeGrip(n, V(s.x - kResizeGrip - 1.0f, s.y - 2.0f)));
        CHECK(!pointInResizeGrip(n, V(s.x - 2.0f, kFirstPortY)));   // beside the port rows
        CHECK(!pointInResizeGrip(n, V(s.x + 3.0f, s.y - 2.0f)));    // outside
        CHECK(!pointInResizeGrip(n, V(2.0f, 2.0f)));
    }

    // [S4b] The close key is in the title bar at the right, and nowhere near
    // a port: a click aimed at an output port must never close the node.
    {
        using cascade::gui::patch::closeKeyRect;
        using cascade::gui::patch::faceRect;
        using cascade::gui::patch::kCloseKey;
        using cascade::gui::patch::pointInCloseKey;
        Graph g;
        const NodeId id = g.addNode(NodeKind::Demod, "AM", PortType::Iq, 50.0f, 70.0f);
        cascade::core::patch::Node& n = *g.mutableNode(id);
        const auto r = closeKeyRect(n);
        CHECK(r.x1 <= n.x + nodeWidth(n));
        CHECK(r.x1 - r.x0 == kCloseKey);
        CHECK(r.y0 >= n.y);
        CHECK(r.y1 <= n.y + kHeaderHeight);          // inside the title bar
        CHECK(pointInCloseKey(n, V((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f)));
        const Vec2 port = outputPortPos(n, 0);
        CHECK(!pointInCloseKey(n, port));
        CHECK(port.y > r.y1);                          // ports start below it
        CHECK(!pointInCloseKey(n, V(n.x + 10.0f, n.y + 10.0f)));   // left of the title
        // It follows the right edge through a resize.
        resizeNodeTo(n, V(n.x + 480.0f, n.y + 200.0f));
        CHECK(pointInCloseKey(n, V(n.x + 480.0f - 10.0f, (r.y0 + r.y1) * 0.5f)));
        // The face is inside the node, below the title, and grows with it.
        const auto f = faceRect(n);
        CHECK(f.x0 > n.x && f.x1 < n.x + nodeWidth(n));
        CHECK(f.y0 >= n.y + kHeaderHeight - 2.0f);
        CHECK(f.y1 < n.y + nodeHeight(n));
        CHECK(f.x1 - f.x0 > 400.0f);
        CHECK(!pointInResizeGrip(n, V(f.x0 + 10.0f, f.y0 + 10.0f)));   // a face click is not a resize
    }

    // [S5] A node with no size - a plain struct, or a file from before sizes
    // - still has a usable outline rather than a zero-area one.
    {
        cascade::core::patch::Node bare;
        bare.inputs = {PortType::Iq};
        bare.outputs = {PortType::Audio};
        CHECK(bare.w == 0.0f);
        CHECK(nodeWidth(bare) == kNodeWidth);
        CHECK(nodeHeight(bare) == nodeHeight(1, 1));
        CHECK(pointInNode(bare, V(kNodeWidth - 1.0f, 10.0f)));
    }

    // [S6] A PART PRESSED IN THE BIN LANDS ON THE CANVAS IN VIEW. 0.99.15 fed
    // the key's desktop position through the canvas-relative view, and nodes
    // appeared off the canvas ("the buttons do nothing"). Whatever the pan,
    // the zoom and the press count, the new node's title bar must be inside
    // the canvas.
    {
        using cascade::gui::patch::keyWraps;
        using cascade::gui::patch::newPartPosition;
        const View views[] = {
            View{V(0.0f, 0.0f), 1.0f},        // a fresh page
            View{V(-2400.0f, -900.0f), 1.0f}, // panned far right and down
            View{V(1800.0f, 700.0f), 1.0f},   // panned the other way
            View{V(-300.0f, 250.0f), kMinZoom},
            View{V(150.0f, -600.0f), kMaxZoom},
        };
        const float sizes[][2] = {{620.0f, 440.0f}, {1400.0f, 900.0f}, {180.0f, 90.0f}};
        int inView = 0;
        int cases = 0;
        for (const View& v : views) {
            for (const auto& sz : sizes) {
                for (int placed = 0; placed < 20; ++placed) {
                    ++cases;
                    const Vec2 w = newPartPosition(v, sz[0], sz[1], placed);
                    const Vec2 s = worldToScreen(v, w);   // canvas-local pixels
                    const Vec2 title = worldToScreen(v, V(w.x + 10.0f, w.y + kHeaderHeight * 0.5f));
                    if (s.x >= 0.0f && s.y >= 0.0f && title.x < sz[0] && title.y < sz[1]) {
                        ++inView;
                    }
                }
            }
        }
        CHECK(cases == 300);
        CHECK(inView == cases);
        // A fresh page puts the first part near the top-left, not at a spot
        // that depends on anything but the canvas.
        const Vec2 first = newPartPosition(View{V(0.0f, 0.0f), 1.0f}, 620.0f, 440.0f, 0);
        CHECK(std::fabs(first.x - 28.0f) < 1e-3f && std::fabs(first.y - 28.0f) < 1e-3f);
        // Presses step, so a stack of new parts can be told apart...
        const Vec2 second = newPartPosition(View{V(0.0f, 0.0f), 1.0f}, 620.0f, 440.0f, 1);
        CHECK(second.x > first.x && second.y > first.y);
        // ...and a negative count (never produced, but an int) is still in view.
        const Vec2 neg = newPartPosition(View{V(0.0f, 0.0f), 1.0f}, 620.0f, 440.0f, -3);
        CHECK(neg.x >= 0.0f && neg.y >= 0.0f && neg.x < 620.0f);

        // The key row wraps at the edge and not before it.
        CHECK(!keyWraps(100.0f, 8.0f, 80.0f, 188.0f));   // ends exactly at the edge
        CHECK(keyWraps(100.0f, 8.0f, 81.0f, 188.0f));    // one pixel over
        CHECK(!keyWraps(0.0f, 8.0f, 50.0f, 1000.0f));
    }

    return testSummary("test_patch_view_math");
}
