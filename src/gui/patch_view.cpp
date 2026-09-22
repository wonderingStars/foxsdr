// patch_view.cpp - drawing the patch canvas, and the pointer on it.
//
// The geometry this uses is all in patch_view_math.hpp and every rule it obeys
// is in core/patch_graph.hpp. What is left here is paint and input, which is
// the only part that genuinely needs a graphics context.
//
// THE PALETTE IS THE PRODUCT'S, taken from gui/theme.hpp by name. Two of its
// rules shape everything below and neither is decoration:
//
//   * AMBER IS A NUMBER. Every reading on a node is amber; no wire, border,
//     lamp or caption may be, because a figure and a warning that share a
//     colour are a figure you cannot trust at a glance.
//   * A CAPTION MAY BE ENGRAVED, A READING MUST BE ON GLASS. Node titles are
//     dark ink cut into brass, which is ~2.3:1 and fine for a label at rest.
//     Anything live sits in a dark well instead.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/patch_view.hpp"

#include <cstdio>

#include "gui/theme.hpp"

namespace cascade::gui::patch {

using core::patch::Connect;
using core::patch::Graph;
using core::patch::kNoNode;
using core::patch::Node;
using core::patch::NodeId;
using core::patch::NodeKind;
using core::patch::PortIndex;
using core::patch::PortType;
using core::patch::Wire;

namespace {

constexpr float kGridWorld = 26.0f;       // grid pitch, world units
constexpr double kRefusalHoldSec = 2.5;   // how long a refusal stays lettered
constexpr float kWireGrabPx = 7.0f;       // how near a click must be to cut one

ImVec2 iv(Vec2 v) { return ImVec2{v.x, v.y}; }

Vec2 vv(ImVec2 v) { return Vec2{v.x, v.y}; }

}  // namespace

const char* kindCaption(NodeKind k) {
    switch (k) {
        case NodeKind::Radio: return "RADIO";
        case NodeKind::Channel: return "CHANNEL";
        case NodeKind::Demod: return "DEMOD";
        case NodeKind::Decoder: return "DECODER";
        case NodeKind::Display: return "DISPLAY";
        case NodeKind::Sink: return "SINK";
    }
    return "NODE";
}

// A node is drawn as a brass plate: a vertical gradient, a hairline edge, and
// a darker well under the caption for anything live. Selected gains an ivory
// edge rather than a glow, because the bench has no glowing edges.
void drawNodePlate(ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected) {
    dl->AddRectFilledMultiColor(a, b, theme::kBrassMid, theme::kBrassMid, theme::kBrassDark,
                                theme::kBrassDark);
    dl->AddRect(a, b, selected ? theme::kIvory : theme::kBrassBright, theme::kPanelRounding,
                0, theme::kHairline);
}

const char* refusalText(Connect why) {
    switch (why) {
        case Connect::Ok: return "";
        case Connect::UnknownNode: return "that node is gone";
        case Connect::NoSuchPort: return "there is no port there";
        case Connect::TypeMismatch: return "those two carry different things";
        case Connect::InputOccupied: return "that input already has a wire";
        case Connect::AlreadyWired: return "those are already wired together";
        case Connect::SelfLoop: return "a node cannot feed itself";
        case Connect::WouldCycle: return "that would make a loop";
    }
    return "that connection was refused";
}

ImU32 portColour(PortType t) {
    switch (t) {
        // Phosphor is what the radio actually received, which is exactly what
        // these two carry.
        case PortType::Iq: return theme::kPhosphor;
        case PortType::Audio: return theme::kGold;
        // These carry no samples, so they take lettering colours.
        case PortType::Text: return theme::kCream;
        case PortType::Control: return theme::kBrassTint;
    }
    return theme::kInkMuted;
}

void seedDefaultPatch(Graph& g, const char* radioName) {
    if (!g.nodes().empty()) { return; }
    g.addNode(NodeKind::Radio, radioName != nullptr ? radioName : "Radio", PortType::Iq,
              60.0f, 80.0f);
}

void drawPatchCanvas(Graph& g, Interaction& ui, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##patchcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 br{origin.x + size.x, origin.y + size.y};

    dl->PushClipRect(origin, br, true);
    dl->AddRectFilled(origin, br, theme::kWell);

    // --- pan and zoom, BEFORE anything is drawn -------------------------------
    // Handled first so the grid, the wires and the nodes are all this frame's
    // answer. Doing it afterwards draws one frame of the old view on every
    // scroll, which reads as the canvas lagging the mouse.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) { ui.panning = true; }
    if (ui.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            ui.view.pan.x += io.MouseDelta.x;
            ui.view.pan.y += io.MouseDelta.y;
            ui.dirty = true;
        } else {
            ui.panning = false;
        }
    }

    // THE STORED PAN IS CANVAS-RELATIVE, AND THE COMPOSED ONE IS NOT.
    //
    // ui.view.pan is a scroll within the canvas: (0,0) means "world origin at
    // the canvas's top-left corner". The drawing needs a transform to SCREEN,
    // so it composes that scroll with wherever the canvas happens to be in the
    // window. Storing an absolute screen offset instead is a bug that hides
    // perfectly: the first rendered check of this page showed an empty grid,
    // because the seeded node at world (60, 80) was being drawn at screen
    // (60, 80) - above and left of the canvas, and clipped away. It would also
    // have made the patch jump whenever the page was moved or resized.
    View v{Vec2{origin.x + ui.view.pan.x, origin.y + ui.view.pan.y}, ui.view.zoom};

    if (hovered && io.MouseWheel != 0.0f) {
        v = zoomAbout(v, vv(io.MousePos), io.MouseWheel > 0.0f ? 1.12f : 1.0f / 1.12f);
        ui.view.zoom = v.zoom;
        ui.view.pan = Vec2{v.pan.x - origin.x, v.pan.y - origin.y};
        ui.dirty = true;
    }

    // --- the grid -------------------------------------------------------------
    // Drawn in world units so it scrolls and scales with the patch. A grid
    // fixed to the screen instead makes a canvas feel like the nodes are
    // sliding over wallpaper rather than being somewhere.
    {
        const float step = kGridWorld * v.zoom;
        if (step >= 6.0f) {  // below this it is just a wash of lines
            const float x0 = origin.x - std::fmod(origin.x - v.pan.x, step);
            for (float x = x0; x < br.x; x += step) {
                dl->AddLine(ImVec2{x, origin.y}, ImVec2{x, br.y}, theme::kEnamelDark);
            }
            const float y0 = origin.y - std::fmod(origin.y - v.pan.y, step);
            for (float y = y0; y < br.y; y += step) {
                dl->AddLine(ImVec2{origin.x, y}, ImVec2{br.x, y}, theme::kEnamelDark);
            }
        }
    }

    const Vec2 mouseWorld = screenToWorld(v, vv(io.MousePos));

    // --- the wires ------------------------------------------------------------
    // Under the nodes, always. A wire crossing a node's face would read as
    // connecting to it.
    for (const Wire& w : g.wires()) {
        const WireEnds e = wireEnds(g, w);
        if (!e.found) { continue; }
        Vec2 c1, c2;
        wireCurve(e.from, e.to, c1, c2);
        const Node* src = g.find(w.from);
        const ImU32 col = portColour(src->outputs[w.fromPort]);
        const bool sel = ui.wireSelected && ui.selectedWire == w;
        dl->AddBezierCubic(iv(worldToScreen(v, e.from)), iv(worldToScreen(v, c1)),
                           iv(worldToScreen(v, c2)), iv(worldToScreen(v, e.to)),
                           sel ? theme::kIvory : col, (sel ? 3.0f : 1.8f) * v.zoom, 0);
    }

    // The wire being dragged, following the pointer.
    if (ui.wiring && ui.wireFrom.found) {
        const Node* n = g.find(ui.wireFrom.node);
        if (n != nullptr) {
            const Vec2 a = ui.wireFrom.input ? inputPortPos(*n, ui.wireFrom.port)
                                             : outputPortPos(*n, ui.wireFrom.port);
            Vec2 c1, c2;
            // An in-flight wire is drawn from the port towards the pointer in
            // the direction it will finally run, so it does not flip round at
            // the moment of release.
            if (ui.wireFrom.input) {
                wireCurve(mouseWorld, a, c1, c2);
                dl->AddBezierCubic(iv(worldToScreen(v, mouseWorld)),
                                   iv(worldToScreen(v, c1)), iv(worldToScreen(v, c2)),
                                   iv(worldToScreen(v, a)), theme::kInkMuted,
                                   1.6f * v.zoom, 0);
            } else {
                wireCurve(a, mouseWorld, c1, c2);
                dl->AddBezierCubic(iv(worldToScreen(v, a)), iv(worldToScreen(v, c1)),
                                   iv(worldToScreen(v, c2)),
                                   iv(worldToScreen(v, mouseWorld)), theme::kInkMuted,
                                   1.6f * v.zoom, 0);
            }
        }
    }

    // --- the nodes ------------------------------------------------------------
    ImFont* font = ImGui::GetFont();
    for (const Node& n : g.nodes()) {
        const Vec2 tl{n.x, n.y};
        const Vec2 size2 = nodeSize(n);
        const ImVec2 a = iv(worldToScreen(v, tl));
        const ImVec2 b = iv(worldToScreen(v, Vec2{tl.x + size2.x, tl.y + size2.y}));
        drawNodePlate(dl, a, b, ui.selected == n.id);

        // The caption, engraved into the brass.
        const float cap = 11.0f * v.zoom;
        if (cap >= 5.0f) {
            char title[96];
            std::snprintf(title, sizeof(title), "%s  %s", kindCaption(n.kind), n.name.c_str());
            dl->AddText(font, cap, ImVec2{a.x + 7.0f * v.zoom, a.y + 5.0f * v.zoom},
                        theme::kEngraved, title);
        }

        // The well under it. Nothing live runs through the graph yet, so it is
        // empty rather than filled with a number that would be a guess - a
        // figure nobody computed is worse than no figure.
        const ImVec2 wa{a.x + 6.0f * v.zoom, a.y + (kHeaderHeight - 2.0f) * v.zoom};
        const ImVec2 wb{b.x - 6.0f * v.zoom, b.y - 6.0f * v.zoom};
        if (wb.x > wa.x && wb.y > wa.y) {
            dl->AddRectFilled(wa, wb, theme::kWell, theme::kKeyRounding);
        }

        // The ports, on the edges where the hit test looks for them.
        const float r = 4.0f * v.zoom;
        for (PortIndex i = 0; i < static_cast<PortIndex>(n.inputs.size()); ++i) {
            const ImVec2 p = iv(worldToScreen(v, inputPortPos(n, i)));
            dl->AddCircleFilled(p, r, portColour(n.inputs[i]), 12);
            dl->AddCircle(p, r, theme::kEnamelDark, 12, 1.0f);
        }
        for (PortIndex i = 0; i < static_cast<PortIndex>(n.outputs.size()); ++i) {
            const ImVec2 p = iv(worldToScreen(v, outputPortPos(n, i)));
            dl->AddCircleFilled(p, r, portColour(n.outputs[i]), 12);
            dl->AddCircle(p, r, theme::kEnamelDark, 12, 1.0f);
        }
    }

    // --- the pointer ----------------------------------------------------------
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const PortHit port = portAt(g, mouseWorld);
        if (port.found) {
            ui.wiring = true;
            ui.wireFrom = port;
        } else {
            const NodeId hit = nodeAt(g, mouseWorld);
            if (hit != kNoNode) {
                ui.selected = hit;
                ui.wireSelected = false;
                const Node* n = g.find(hit);
                if (pointInHeader(*n, mouseWorld)) {
                    ui.dragNode = hit;
                    ui.grab = Vec2{mouseWorld.x - n->x, mouseWorld.y - n->y};
                }
            } else {
                // Empty canvas: a click near a wire selects it, otherwise the
                // selection clears.
                ui.selected = kNoNode;
                ui.wireSelected = false;
                for (const Wire& w : g.wires()) {
                    const WireEnds e = wireEnds(g, w);
                    if (!e.found) { continue; }
                    if (distanceToWire(e.from, e.to, mouseWorld) * v.zoom <= kWireGrabPx) {
                        ui.selectedWire = w;
                        ui.wireSelected = true;
                        break;
                    }
                }
            }
        }
    }

    if (ui.dragNode != kNoNode) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Node* n = g.mutableNode(ui.dragNode);
            if (n != nullptr) {
                n->x = mouseWorld.x - ui.grab.x;
                n->y = mouseWorld.y - ui.grab.y;
                ui.dirty = true;
            }
        } else {
            ui.dragNode = kNoNode;
        }
    }

    if (ui.wiring && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const PortHit drop = portAt(g, mouseWorld);
        ui.refusal = Connect::Ok;
        if (drop.found && drop.node != kNoNode) {
            // A wire may be drawn from either end; whichever end is the OUTPUT
            // is the source. Forcing the user to start at the output would be
            // an arbitrary rule they would discover by it not working.
            const PortHit& out = ui.wireFrom.input ? drop : ui.wireFrom;
            const PortHit& in = ui.wireFrom.input ? ui.wireFrom : drop;
            if (!out.input && in.input) {
                const Connect r = g.connect(out.node, out.port, in.node, in.port);
                if (r == Connect::Ok) { ui.dirty = true; }
                if (r != Connect::Ok) {
                    ui.refusal = r;
                    ui.refusedAt = ImGui::GetTime();
                    ui.refusedNear = mouseWorld;
                }
            } else {
                // Two inputs or two outputs: there is no sense in which that
                // is a connection, and saying so is better than silence.
                ui.refusal = Connect::TypeMismatch;
                ui.refusedAt = ImGui::GetTime();
                ui.refusedNear = mouseWorld;
            }
        }
        ui.wiring = false;
    }

    // Delete removes whichever ONE thing is selected.
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (ui.wireSelected) {
            g.disconnect(ui.selectedWire);
            ui.wireSelected = false;
            ui.dirty = true;
        } else if (ui.selected != kNoNode) {
            g.removeNode(ui.selected);
            ui.selected = kNoNode;
            ui.dirty = true;
        }
    }

    // --- the refusal ----------------------------------------------------------
    if (ui.refusal != Connect::Ok && ImGui::GetTime() - ui.refusedAt < kRefusalHoldSec) {
        const ImVec2 at = iv(worldToScreen(v, ui.refusedNear));
        const char* msg = refusalText(ui.refusal);
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        const ImVec2 pad{6.0f, 3.0f};
        const ImVec2 box0{at.x + 12.0f, at.y + 10.0f};
        const ImVec2 box1{box0.x + sz.x + pad.x * 2.0f, box0.y + sz.y + pad.y * 2.0f};
        dl->AddRectFilled(box0, box1, theme::kEnamel, theme::kPanelRounding);
        dl->AddRect(box0, box1, theme::kAlarm, theme::kPanelRounding, 0, theme::kHairline);
        // Rust, not amber: this is a warning and amber belongs to numbers.
        dl->AddText(ImVec2{box0.x + pad.x, box0.y + pad.y}, theme::kAlarmHot, msg);
    }

    dl->PopClipRect();
}

}  // namespace cascade::gui::patch
