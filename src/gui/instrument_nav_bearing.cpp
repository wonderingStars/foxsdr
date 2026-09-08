// instrument_nav_bearing.cpp - the CASCADE_INSTRUMENT_NAV_BEARING face: the
// course indicator out of an aeroplane's instrument panel.
//
// ============================ THE EQUIPMENT =================================
//
// A panel-mounted VOR course deviation indicator, of the family every light
// aircraft built since about 1970 has one of: a round black dial behind glass
// in a square brass case, a compass card graduated every five degrees and
// lettered every thirty (N, 3, 6, E, 12, 15, S, 21, 24, W, 30, 33), a fixed
// index at the top, a vertical deviation bar swinging across a row of dots, a
// TO/FROM indication either side of the middle, a red-and-white barber-pole
// NAV flag that swings across the face the moment the signal stops being
// trustworthy, and the omni bearing selector knob in the lower left corner.
//
// The specific unit reproduced is the Bendix/King KI 208, the VOR half of the
// Silver Crown KI 208 / KI 209 pair. Where a number here is the real unit's, it
// came from Honeywell's own installation manual 006-00140-0004 revision 4
// (August 2002), table 1-1: "+/-10 degrees off course gives full scale
// deflection", "OMNI Accuracy +/-2 degrees max error, +/-1 degree typical", and
// the TO-OFF-FROM flag and NAV warning flag it describes. The card's lettering
// convention and the two-degrees-per-dot scale were checked against the FAA
// Aeronautical Information Manual chapter 1 section 1 and AOPA's "How it works:
// course deviation indicator". Photographs of KI 208, KI 209 and KI 206 units
// (BendixKing's own product pages, Aircraft Spruce, Gulf Coast Avionics,
// AllAvionics) were looked at for proportion, bezel and knob placement. None of
// that is code: every line below is written here, in the bench's own materials.
//
// ================== WHERE THIS DIVERGES FROM THE REAL UNIT ==================
//
// ON A REAL CDI THE OBS KNOB TURNS THE CARD. There is no receiver in the
// aeroplane telling the instrument which radial it is on - the pilot turns the
// card until the bar centres, and THAT is how the radial is read. This decoder
// measures the radial directly, so the card is driven by the measurement and
// sits with the radial under the top index, the way a radio magnetic indicator
// carries its card. The OBS knob keeps its real job, which is the other half of
// the instrument: it selects a course, the bar shows how far off it you are at
// two degrees a dot, and the TO/FROM indication says which way that course
// runs. Everything the knob drives is computed from the radial the plugin
// measured (instrument_nav_bearing_math.hpp), so it is a control on a real
// quantity and not a picture of one - which is the only condition on which
// instrument_face.hpp permits a knob to be drawn at all.
//
// AND WITH NO SIGNAL THERE IS NO CARD. A real card is a physical wheel and
// stays where it was left; this one is the reading, so drawing it at some
// angle when nothing has been measured would put a radial on the dial that
// nobody reported. The NAV flag comes across the face, the bar and the TO/FROM
// go away, and the figures beneath blank - which is rule two of
// instrument_face.hpp, and the shape a real NAV flag already has.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "gui/fonts.hpp"
#include "gui/instrument_nav_bearing_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;
namespace nb = cascade::gui::navbearing;

namespace {

constexpr float kPiF = 3.14159265f;

// A point on the dial: `deg` clockwise from twelve o'clock, `r` from the middle.
ImVec2 onDial(const ImVec2& c, float r, double deg) {
    const float a = static_cast<float>(deg) * kPiF / 180.0f;
    return ImVec2(c.x + std::sin(a) * r, c.y - std::cos(a) * r);
}

// A caption cut into the panel: the legend face, the shadow of the cut under
// it, muted ink in it. Returns the width used.
float engrave(ImDrawList* dl, const ImVec2& at, const char* s, float px) {
    ImFont* f = fonts::legend();
    dl->AddText(f, px, ImVec2(at.x, at.y + 1.0f), theme::withAlpha(theme::kVoid, 0.65f), s);
    dl->AddText(f, px, at, theme::kInkMuted, s);
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// A figure on glass, in the monospaced reading face, with the nixie halo the
// frequency drums wear. Amber, because theme.hpp's rule is that amber is a
// NUMBER and nothing else may use it.
void figure(ImDrawList* dl, const ImVec2& at, const char* s, float px, bool lit) {
    ImFont* f = fonts::reading();
    if (lit) {
        const ImU32 halo = theme::withAlpha(theme::kAmber, 0.16f);
        dl->AddText(f, px, ImVec2(at.x - 1.0f, at.y), halo, s);
        dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y), halo, s);
        dl->AddText(f, px, ImVec2(at.x, at.y - 1.0f), halo, s);
        dl->AddText(f, px, ImVec2(at.x, at.y + 1.0f), halo, s);
    }
    dl->AddText(f, px, at, lit ? theme::kAmber : theme::kAmberDim, s);
}

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// --- the barber pole --------------------------------------------------------
//
// A NAV warning flag is a painted metal shutter on a spring: when the signal
// fails it swings across the face, and the diagonal red-and-white hatching is
// there so it is unmistakable at a glance in a moving cockpit. Drawn as the
// stripes it is, clipped to its own shutter, rather than as a red box with a
// word on it - the hatching is what makes it read as the flag rather than as
// an error message.
void drawNavFlag(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    if (w < 10.0f || h < 6.0f) { return; }
    dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                      theme::withAlpha(theme::kVoid, 0.60f), 2.0f);
    dl->AddRectFilled(tl, br, theme::kCream, 2.0f);
    dl->PushClipRect(tl, br, true);
    const float pitch = std::max(5.0f, h * 0.46f);
    for (float x = tl.x - h; x < br.x + h; x += pitch * 2.0f) {
        dl->AddQuadFilled(ImVec2(x, br.y), ImVec2(x + h, tl.y), ImVec2(x + h + pitch, tl.y),
                          ImVec2(x + pitch, br.y), theme::kAlarm);
    }
    dl->PopClipRect();
    dl->AddRect(tl, br, theme::kEnamelDark, 2.0f, 0, 1.0f);

    // The word, on the flag, in the ivory the rest of the cabinet is lettered
    // in with the dark cut under it - readable over both stripes, which a plain
    // white or a plain black word is not.
    ImFont* f = fonts::legend();
    const float px = std::min(fonts::kTinySize, h * 0.72f);
    if (px < 8.0f) { return; }
    const float tw = textW(f, px, "NAV");
    const ImVec2 at(tl.x + (w - tw) * 0.5f, tl.y + (h - px) * 0.5f - 1.0f);
    dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f), theme::withAlpha(theme::kVoid, 0.85f),
                "NAV");
    dl->AddText(f, px, at, theme::kIvory, "NAV");
}

// --- the omni bearing selector ----------------------------------------------
//
// A REAL CONTROL, and the only reason it is drawn at all. It is an ImGui item,
// so it hovers, takes focus, joins the same input arbitration as every other
// widget, and answers the keyboard as well as the hand: the left and right
// arrows step one degree while it is focused, which is what a knob alone
// cannot do and what scope_face.hpp requires of anything shaped like one.
//
// The knob TWISTS - the angle from its middle to the cursor is tracked frame to
// frame and the difference accumulated - so it never jumps to wherever the hand
// grabbed it. A real one does not either.
//
// Returns the course after this frame's input, in [0, 360).
double obsKnob(ImDrawList* dl, const ImVec2& centre, float radius, double course) {
    if (radius < 7.0f) { return course; }
    const ImVec2 saved = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
    ImGui::InvisibleButton("##vor_obs", ImVec2(radius * 2.0f, radius * 2.0f),
                           ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();

    double out = course;
    // The hand.
    static float lastAngle = 0.0f;
    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float a =
            std::atan2(m.x - centre.x, centre.y - m.y) * 180.0f / kPiF;
        if (ImGui::IsItemActivated()) { lastAngle = a; }
        float d = a - lastAngle;
        while (d > 180.0f) { d -= 360.0f; }
        while (d < -180.0f) { d += 360.0f; }
        lastAngle = a;
        out += static_cast<double>(d);
    }
    // The wheel, one degree a notch, and the wheel is TAKEN from the window
    // while the pointer is on the knob - otherwise turning the course also
    // scrolls the page out from under it.
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) { out += 1.0; }
        if (wheel < 0.0f) { out -= 1.0; }
    }
    // The keyboard, for the hand that is not on a mouse. Ten degrees with
    // shift, which is the coarse detent every selector like this has.
    if (focused) {
        const double step = ImGui::GetIO().KeyShift ? 10.0 : 1.0;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { out += step; }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) { out -= step; }
    }
    out = nb::wrap360(out);

    // The knob: a knurled brass disc lit from the upper left like every other
    // proud thing on this bench, with one cream index line from the middle out
    // that points at the course it has selected. A dial whose index never moves
    // is a picture of a dial.
    dl->AddCircleFilled(ImVec2(centre.x, centre.y + radius * 0.10f), radius * 1.10f,
                        theme::withAlpha(theme::kVoid, 0.55f), 32);
    dl->AddCircleFilled(centre, radius, theme::kEnamelDark, 32);
    dl->AddCircleFilled(centre, radius * 0.96f, theme::kBrassDark, 32);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.10f, centre.y - radius * 0.12f),
                        radius * 0.82f, theme::kBrassMid, 32);
    dl->AddCircleFilled(ImVec2(centre.x - radius * 0.18f, centre.y - radius * 0.22f),
                        radius * 0.50f, theme::kBrassBright, 32);
    // The knurl: short radial cuts round the rim, which is what a knob a
    // gloved hand turns actually has.
    for (int i = 0; i < 24; ++i) {
        const double deg = i * 15.0;
        const ImVec2 a0 = onDial(centre, radius * 0.86f, deg);
        const ImVec2 a1 = onDial(centre, radius * 0.99f, deg);
        dl->AddLine(a0, a1, theme::withAlpha(theme::kVoid, 0.45f), 1.0f);
    }
    dl->AddCircle(centre, radius,
                  (hovered || active) ? theme::kIvory
                                      : theme::withAlpha(theme::kBrassTint, 0.85f),
                  32, 1.4f);
    const ImVec2 p0 = onDial(centre, radius * 0.10f, out);
    const ImVec2 p1 = onDial(centre, radius * 0.84f, out);
    dl->AddLine(p0, p1, theme::kVoid, std::max(2.6f, radius * 0.20f));
    dl->AddLine(p0, p1, theme::kIvory, std::max(1.6f, radius * 0.13f));

    ImGui::SetCursorScreenPos(saved);
    return out;
}

// --- the dial ---------------------------------------------------------------

// The compass card. Graduated every five degrees, a longer graduation every
// ten, a longer one still and a numeral every thirty, turned so `underIndex`
// sits beneath the fixed index at the top.
void drawCard(ImDrawList* dl, const ImVec2& c, float r, double underIndex) {
    ImFont* nf = fonts::ui();
    const float npx = std::max(9.0f, std::min(fonts::kUiSize, r * 0.17f));
    for (int step = 0; step < 72; ++step) {
        const int deg = step * 5;
        const double screen = nb::cardScreenDeg(deg, underIndex);
        const bool major = (deg % 30) == 0;
        const bool mid = (deg % 10) == 0;
        const float inner = major ? r * 0.80f : (mid ? r * 0.85f : r * 0.89f);
        dl->AddLine(onDial(c, r * 0.96f, screen), onDial(c, inner, screen),
                    major ? theme::kIvory : theme::kCream, major ? 2.0f : 1.0f);
        if (!major) { continue; }
        char lab[4];
        if (!nb::cardLabel(deg, lab, sizeof lab)) { continue; }
        const ImVec2 sz = nf->CalcTextSizeA(npx, FLT_MAX, 0.0f, lab);
        const ImVec2 at = onDial(c, r * 0.68f, screen);
        const ImVec2 tp(at.x - sz.x * 0.5f, at.y - sz.y * 0.5f);
        dl->AddText(nf, npx, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    theme::withAlpha(theme::kVoid, 0.75f), lab);
        dl->AddText(nf, npx, tp, theme::kIvory, lab);
    }
}

}  // namespace

float drawNavBearingFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    if (!(w > 0.0f) || !(h > 0.0f)) { return 0.0f; }
    if (w < 80.0f || h < 60.0f) { return 0.0f; }

    // The plate the whole instrument is bolted to, titled by the plugin's own
    // window name.
    const float bodyTop = addBenchPlate(dl, tl, br, in.title.c_str());
    const float bodyBottom = br.y - 8.0f;
    const nb::Layout L = nb::layout(w, bodyBottom - bodyTop);

    // WHAT IS AND IS NOT A READING, decided once, here, and honoured by
    // everything below. A course indicator's whole dial is the bearing: the
    // card, the bar and the TO/FROM all mean nothing without one, so they are
    // all gated on the same fact rather than each guessing for itself.
    const bool lock = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u;
    const bool haveRadial = lock && std::isfinite(in.state.values[0]);
    const double radial = haveRadial ? nb::wrap360(in.state.values[0]) : 0.0;

    if (!L.drawAnything) {
        // Too small to be an instrument. Say what is on the air in words rather
        // than draw a dial nobody can read.
        char line[32];
        if (haveRadial) {
            char b[4];
            nb::formatBearing(radial, b, sizeof b);
            std::snprintf(line, sizeof line, "RADIAL %s", b);
        } else {
            std::snprintf(line, sizeof line, "NO SIGNAL");
        }
        engrave(dl, ImVec2(tl.x + 10.0f, bodyTop + 4.0f), line, fonts::kTinySize);
        return bodyTop + fonts::kTinySize + 10.0f - tl.y;
    }

    // The OBS is the user's setting, not a measurement, so it lives with the
    // window rather than with the plugin - a real selector stays where the hand
    // left it whether or not anything is being received.
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID obsKey = ImGui::GetID("##vor_obs_course");
    double course = static_cast<double>(store->GetFloat(obsKey, 0.0f));

    const ImVec2 caseC(tl.x + L.dialCx, bodyTop + L.dialCy);
    const float caseR = L.dialR;              // half the square case
    const float bezelR = caseR * 0.96f;       // the brass ring
    const float glassR = caseR * 0.76f;       // the dark dial behind the glass

    // --- the case ------------------------------------------------------------
    const ImVec2 kTL(caseC.x - caseR, caseC.y - caseR);
    const ImVec2 kBR(caseC.x + caseR, caseC.y + caseR);
    dl->AddRectFilled(kTL, kBR, theme::kBrassMid, 4.0f);
    dl->AddRectFilledMultiColor(ImVec2(kTL.x + 4.0f, kTL.y), ImVec2(kBR.x - 4.0f, kBR.y),
                                theme::kBrassBright, theme::kBrassBright, theme::kBrassDark,
                                theme::kBrassDark);
    addBenchBevel(dl, kTL, kBR, 4.0f, true);
    // Three screws, in the three corners the selector does not occupy - which
    // is what the real case looks like, the knob taking the fourth.
    const float screwR = std::max(2.5f, caseR * 0.055f);
    const float inset = caseR * 0.86f;
    addCabinetScrew(dl, ImVec2(caseC.x - inset, caseC.y - inset), screwR, 24.0f);
    addCabinetScrew(dl, ImVec2(caseC.x + inset, caseC.y - inset), screwR, -61.0f);
    addCabinetScrew(dl, ImVec2(caseC.x + inset, caseC.y + inset), screwR, 12.0f);

    // --- the bezel and the glass --------------------------------------------
    //
    // THE RING HAS TO BE A DIFFERENT METAL FROM THE PANEL BEHIND IT, or it is
    // not a bezel. Drawn in the case's own tone it vanished, and all that
    // remained of it was the hairline of its bevel - which read as a scratch
    // across the brass rather than as the edge of a ring. So: a light ring
    // struck against a dark outer lip, the eccentric inner disc doing the
    // lighting the way every other round thing on this bench is lit, and the
    // glass sunk into a dark lip inside it.
    dl->AddCircleFilled(ImVec2(caseC.x, caseC.y + bezelR * 0.025f), bezelR * 1.02f,
                        theme::withAlpha(theme::kVoid, 0.45f), 96);
    dl->AddCircleFilled(caseC, bezelR, theme::kBrassShade, 96);
    dl->AddCircleFilled(ImVec2(caseC.x - bezelR * 0.030f, caseC.y - bezelR * 0.040f),
                        bezelR * 0.980f, theme::kBrassTint, 96);
    dl->AddCircle(caseC, bezelR, theme::withAlpha(theme::kVoid, 0.55f), 96, 1.2f);
    // The lip the glass is set into.
    dl->AddCircleFilled(caseC, glassR * 1.13f, theme::kBrassDark, 96);
    dl->AddCircleFilled(caseC, glassR * 1.05f, theme::kEnamelDark, 96);
    dl->AddCircleFilled(caseC, glassR, theme::kVoid, 96);
    addBenchBevel(dl, ImVec2(caseC.x - glassR, caseC.y - glassR),
                  ImVec2(caseC.x + glassR, caseC.y + glassR), glassR, false);

    // --- the card, when there is one -----------------------------------------
    if (haveRadial) { drawCard(dl, caseC, glassR, radial); }

    // --- the fixed index at the top, and its reciprocal at the bottom ---------
    // Furniture, not a reading: the index is where the card is read, and it is
    // engraved on the glass whether or not a card is turning under it.
    {
        const float ix = caseC.x;
        const float iy = caseC.y - glassR;
        const float s = std::max(5.0f, glassR * 0.10f);
        dl->AddTriangleFilled(ImVec2(ix - s, iy - s * 0.2f), ImVec2(ix + s, iy - s * 0.2f),
                              ImVec2(ix, iy + s * 1.1f), theme::kGold);
        dl->AddTriangle(ImVec2(ix - s, iy - s * 0.2f), ImVec2(ix + s, iy - s * 0.2f),
                        ImVec2(ix, iy + s * 1.1f), theme::kVoid, 1.0f);
        const float by = caseC.y + glassR;
        dl->AddTriangleFilled(ImVec2(ix - s * 0.6f, by + s * 0.2f),
                              ImVec2(ix + s * 0.6f, by + s * 0.2f),
                              ImVec2(ix, by - s * 0.7f), theme::kInkFaint);
    }

    // --- the course the selector has chosen, as a bug on the card's rim ------
    //
    // ON THE RIM AND NOT ACROSS THE FACE. A course pointer drawn as an arrow
    // through the middle of the dial - which is what this was - lies across the
    // card's own numerals and across the deviation scale, and the one it
    // happens to cover is the one you wanted to read. A bug riding the
    // graduations is the same information in the one band of the dial that
    // carries nothing else, and it is what a heading bug or an RMI's course
    // index already is. The hollow one opposite is the reciprocal - the same
    // line read from the other end.
    if (haveRadial) {
        const double cs = nb::cardScreenDeg(course, radial);
        const float r0 = glassR * 0.79f;
        const float r1 = glassR * 0.99f;
        const double half = 3.6;
        const ImVec2 a0 = onDial(caseC, r0, cs - half);
        const ImVec2 a1 = onDial(caseC, r1, cs - half);
        const ImVec2 a2 = onDial(caseC, r1, cs + half);
        const ImVec2 a3 = onDial(caseC, r0, cs + half);
        dl->AddQuadFilled(a0, a1, a2, a3, theme::kGold);
        dl->AddQuad(a0, a1, a2, a3, theme::kVoid, 1.0f);
        const double rs = cs + 180.0;
        const ImVec2 b0 = onDial(caseC, glassR * 0.84f, rs - half * 0.7);
        const ImVec2 b1 = onDial(caseC, r1, rs - half * 0.7);
        const ImVec2 b2 = onDial(caseC, r1, rs + half * 0.7);
        const ImVec2 b3 = onDial(caseC, glassR * 0.84f, rs + half * 0.7);
        dl->AddQuad(b0, b1, b2, b3, theme::kGold, 1.4f);
    }

    // --- the deviation scale and its bar -------------------------------------
    //
    // Five dots a side at two degrees each, ten degrees to full scale - the KI
    // 208's own figure. The dots are engraved on the glass and are there
    // whatever happens; the BAR is the reading and is drawn only when there is
    // one, exactly as drawBenchMeter draws no needle without a measurement.
    const float dotStep = glassR * 0.095f;
    const float fullScale = dotStep * static_cast<float>(nb::kDots);
    const float dotR = std::max(1.6f, glassR * 0.038f);
    for (int i = 1; i <= nb::kDots; ++i) {
        const float dx = dotStep * static_cast<float>(i);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const ImVec2 p(caseC.x + dx * static_cast<float>(sgn), caseC.y);
            dl->AddCircleFilled(p, dotR, theme::kEnamel, 16);
            dl->AddCircle(p, dotR, theme::kInkFaint, 16, 1.2f);
        }
    }
    dl->AddCircle(caseC, dotR * 1.35f, theme::kInkFaint, 20, 1.2f);

    nb::CourseSolution sol;
    if (haveRadial) {
        sol = nb::solveCourse(radial, course);
        const float bx = caseC.x + static_cast<float>(sol.barFrac) * fullScale;
        const float y0 = caseC.y - glassR * 0.36f;
        const float y1 = caseC.y + glassR * 0.36f;
        const float bw = std::max(2.4f, glassR * 0.055f);
        dl->AddRectFilled(ImVec2(bx - bw * 0.5f + 1.0f, y0 + 1.5f),
                          ImVec2(bx + bw * 0.5f + 1.0f, y1 + 1.5f),
                          theme::withAlpha(theme::kVoid, 0.70f), bw * 0.5f);
        dl->AddRectFilled(ImVec2(bx - bw * 0.5f, y0), ImVec2(bx + bw * 0.5f, y1),
                          theme::kPhosphor, bw * 0.5f);
        // A bar on its stop is a bar that has stopped measuring, so it says so
        // rather than sitting there looking like a reading of exactly ten
        // degrees.
        if (sol.offScale) {
            dl->AddRectFilled(ImVec2(bx - bw * 0.9f, y0 - bw * 0.9f),
                              ImVec2(bx + bw * 0.9f, y0 + bw * 0.9f), theme::kAlarm,
                              bw * 0.5f);
            dl->AddRectFilled(ImVec2(bx - bw * 0.9f, y1 - bw * 0.9f),
                              ImVec2(bx + bw * 0.9f, y1 + bw * 0.9f), theme::kAlarm,
                              bw * 0.5f);
        }
    }

    // --- TO and FROM ---------------------------------------------------------
    //
    // Both triangles are engraved on the glass and one of them lights, so the
    // dial says which indications it HAS even when it is showing neither -
    // which is the same reason drawBenchLamp letters an unlit lamp.
    {
        ImFont* f = fonts::legend();
        const float px = std::max(8.0f, std::min(fonts::kTinySize, glassR * 0.15f));
        const float s = std::max(4.0f, glassR * 0.080f);
        const float ty = caseC.y - glassR * 0.50f;
        const float fy = caseC.y + glassR * 0.50f;
        const bool toLit = haveRadial && sol.to;
        const bool fromLit = haveRadial && !sol.to;
        const ImU32 toCol = toLit ? theme::kPhosphor : theme::withAlpha(theme::kInkFaint, 0.55f);
        const ImU32 fromCol =
            fromLit ? theme::kPhosphor : theme::withAlpha(theme::kInkFaint, 0.55f);
        dl->AddTriangleFilled(ImVec2(caseC.x - s, ty + s * 0.7f),
                              ImVec2(caseC.x + s, ty + s * 0.7f),
                              ImVec2(caseC.x, ty - s * 0.8f), toCol);
        dl->AddTriangleFilled(ImVec2(caseC.x - s, fy - s * 0.7f),
                              ImVec2(caseC.x + s, fy - s * 0.7f),
                              ImVec2(caseC.x, fy + s * 0.8f), fromCol);
        // THE WORDS TUCK INSIDE THEIR OWN TRIANGLES, towards the middle of the
        // dial. Set beside them they reached the card's numeral ring and ran
        // through whichever bearing happened to be there - and the numeral a
        // legend is sitting on is the one you were trying to read.
        const float tw = textW(f, px, "TO");
        const float fw = textW(f, px, "FROM");
        dl->AddText(f, px, ImVec2(caseC.x - tw * 0.5f, ty + s * 0.9f), toCol, "TO");
        dl->AddText(f, px, ImVec2(caseC.x - fw * 0.5f, fy - s * 0.9f - px), fromCol, "FROM");
    }

    // --- the NAV flag --------------------------------------------------------
    if (!haveRadial) {
        const float fw = glassR * 0.92f;
        const float fh = std::max(12.0f, glassR * 0.28f);
        drawNavFlag(dl, ImVec2(caseC.x - fw * 0.5f, caseC.y - glassR * 0.52f),
                    ImVec2(caseC.x + fw * 0.5f, caseC.y - glassR * 0.52f + fh));
    }

    // The glass itself, as the one reflection that reads AS glass: a bright
    // arc along the inner upper-left rim, where a curved cover catches the
    // light. The diagonal wash this replaced had a straight edge across the
    // dial and rendered as a smudge on the card rather than as a reflection.
    dl->PathArcTo(caseC, glassR - 1.5f, kPiF * 1.05f, kPiF * 1.62f, 24);
    dl->PathStroke(IM_COL32(255, 255, 255, 26), ImDrawFlags_None, 2.0f);

    // --- the selector, in the corner it lives in -----------------------------
    // A REAL PROTRUDING KNOB SITS OVER ITS OWN BEZEL, which is why this one is
    // allowed to overlap the brass ring: on the unit itself the selector's body
    // stands proud of the corner. It stops short of the glass, so nothing it
    // does hides a graduation.
    {
        const float kr = std::max(8.0f, caseR * 0.150f);
        const ImVec2 kc(caseC.x - caseR * 0.70f, caseC.y + caseR * 0.70f);
        course = obsKnob(dl, kc, kr, course);
        store->SetFloat(obsKey, static_cast<float>(course));
        // THE LEGEND IS DRAWN ONLY IF IT FITS INSIDE THE CASE. Written
        // unconditionally it ran off the bottom edge and was sliced in half by
        // the readout below - a caption cut in half is worse than none, because
        // it looks like a fault in the panel rather than a legend that did not
        // have room.
        ImFont* f = fonts::legend();
        const float px = std::min(fonts::kTinySize, kr * 0.60f);
        const float top = kc.y + kr + 2.0f;
        if (px >= 8.0f && top + px <= caseC.y + caseR - 2.0f) {
            const float tw = textW(f, px, "OBS");
            engrave(dl, ImVec2(kc.x - tw * 0.5f, top), "OBS", px);
        }
    }

    // --- the glass readout strip ---------------------------------------------
    //
    // BOTH BEARINGS, because a VOR reading is used in two directions and
    // confusing them is the classic error: the RADIAL is where you are as seen
    // from the station, the INBOUND course is its reciprocal. The plugin's own
    // text output prints both for the same reason.
    if (L.drawReadout) {
        const ImVec2 rTL(tl.x + 6.0f, bodyTop + L.readoutY0);
        const ImVec2 rBR(L.drawGauges ? tl.x + L.gaugeX0 - 6.0f : br.x - 6.0f,
                         bodyTop + L.readoutY1);
        if (rBR.x - rTL.x > 60.0f && rBR.y - rTL.y > 24.0f) {
            drawFreqDrumWell(dl, rTL, rBR);
            const float cellW = (rBR.x - rTL.x) / 3.0f;
            const float capPx = fonts::kTinySize;
            const float figPx = std::min(fonts::kReadingSize + 6.0f,
                                         (rBR.y - rTL.y) - capPx - 10.0f);
            char rad[4] = "---";
            char inb[4] = "---";
            char crs[4];
            if (haveRadial) {
                nb::formatBearing(radial, rad, sizeof rad);
                nb::formatBearing(nb::reciprocal(nb::roundedDeg(radial)), inb, sizeof inb);
            }
            nb::formatBearing(course, crs, sizeof crs);
            const char* caps[3] = {"RADIAL FROM", "INBOUND CRS", "OBS COURSE"};
            const char* vals[3] = {rad, inb, crs};
            const bool litv[3] = {haveRadial, haveRadial, true};
            for (int i = 0; i < 3; ++i) {
                const float cx = rTL.x + cellW * (static_cast<float>(i) + 0.5f);
                if (i > 0) {
                    addBenchDivider(dl, rTL.x + cellW * static_cast<float>(i), rTL.y + 4.0f,
                                    rBR.y - 4.0f);
                }
                const float cw = textW(fonts::legend(), capPx, caps[i]);
                engrave(dl, ImVec2(cx - cw * 0.5f, rTL.y + 5.0f), caps[i], capPx);
                const float vw = textW(fonts::reading(), figPx, vals[i]);
                figure(dl, ImVec2(cx - vw * 0.5f, rTL.y + 5.0f + capPx + 2.0f), vals[i],
                       figPx, litv[i]);
            }
        }
    }

    // --- the station, and its Morse ------------------------------------------
    if (L.drawIdent) {
        const ImVec2 iTL(tl.x + 6.0f, bodyTop + L.readoutY1 + 4.0f);
        const ImVec2 iBR(L.drawGauges ? tl.x + L.gaugeX0 - 6.0f : br.x - 6.0f,
                         bodyTop + L.readoutY1 + nb::kIdentH - 2.0f);
        if (iBR.x - iTL.x > 60.0f && iBR.y - iTL.y > 20.0f) {
            drawFreqDrumWell(dl, iTL, iBR);
            const char* ident = in.have ? in.state.text[0] : "";
            const float capPx = fonts::kTinySize;
            engrave(dl, ImVec2(iTL.x + 8.0f, iTL.y + (iBR.y - iTL.y - capPx) * 0.5f),
                    "IDENT", capPx);
            const float x0 = iTL.x + 8.0f + textW(fonts::legend(), capPx, "IDENT") + 12.0f;
            if (ident[0] == '\0') {
                // NO STATION IS DRAWN AS NO STATION. Not "---", which on a dial
                // full of three-figure bearings reads as one of them.
                engrave(dl, ImVec2(x0, iTL.y + (iBR.y - iTL.y - capPx) * 0.5f), "NO IDENT",
                        capPx);
            } else {
                ImFont* uf = fonts::ui();
                const float px = std::min(fonts::kUiSize + 2.0f, (iBR.y - iTL.y) * 0.52f);
                dl->AddText(uf, px, ImVec2(x0, iTL.y + 3.0f), theme::kPhosphor, ident,
                            nullptr, iBR.x - x0 - 8.0f);
                // The pattern under the letters, drawn as the marks it is. A
                // dot is a disc and a dash is a bar, three dots long, which is
                // what makes it a Morse group rather than a row of dashes.
                char code[96];
                nb::identMorse(ident, code, sizeof code);
                const float unit = std::max(2.0f, (iBR.y - iTL.y) * 0.10f);
                float mx = x0;
                const float my = iTL.y + 3.0f + px + 2.0f;
                const float maxX = iBR.x - 6.0f;
                for (const char* p = code; *p != '\0' && mx < maxX; ++p) {
                    if (*p == ' ') {
                        mx += unit * 3.0f;
                    } else if (*p == '.') {
                        dl->AddCircleFilled(ImVec2(mx + unit * 0.5f, my + unit * 0.5f),
                                            unit * 0.5f, theme::kPhosphorDim, 10);
                        mx += unit * 2.0f;
                    } else {
                        dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + unit * 3.0f, my + unit),
                                          theme::kPhosphorDim, unit * 0.5f);
                        mx += unit * 4.0f;
                    }
                }
            }
        }
    }

    // --- the column of gauges, and the two lamps above it --------------------
    if (L.drawGauges) {
        const float gx0 = tl.x + L.gaugeX0;
        const float gx1 = tl.x + L.gaugeX1;
        float gy = bodyTop + 4.0f;

        const float lampR = 6.0f;
        const ImVec2 lockAt(gx0 + (gx1 - gx0) * 0.28f, gy + lampR + 2.0f);
        const ImVec2 newAt(gx0 + (gx1 - gx0) * 0.72f, gy + lampR + 2.0f);
        drawBenchLamp(dl, lockAt, lampR, theme::kPhosphor, lock, "LOCK");
        drawBenchLamp(dl, newAt, lampR, theme::kGold, cue.unread, "NEW");
        gy += lampR * 2.0f + 4.0f + ImGui::GetTextLineHeight() + 8.0f;

        addBenchGroupCaption(dl, ImVec2(gx0, gy), gx1 - gx0, "SIGNAL");
        gy += fonts::kTinySize + 8.0f;

        const float gh = bodyBottom - gy;
        if (gh >= 44.0f) {
            // THE THREE FIGURES THE SLOT MAP NAMES, and each one is drawn as
            // "no reading" when the plugin left its slot alone - a slot at zero
            // is a slot nobody filled, and the ABI says so in as many words.
            const double conf = in.state.values[1];
            const double ref = in.state.values[2];
            const double var = in.state.values[3];
            const struct {
                const char* cap;
                double v;
            } bays[3] = {{"CONF", conf}, {"REF", ref}, {"VAR", var}};
            const float bw = (gx1 - gx0 - 8.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                const float x = gx0 + (bw + 4.0f) * static_cast<float>(i);
                const bool got = in.have && std::isfinite(bays[i].v) && bays[i].v > 0.0;
                char rd[16];
                if (got) {
                    std::snprintf(rd, sizeof rd, "%d %%",
                                  static_cast<int>(std::lround(bays[i].v * 100.0)));
                } else {
                    rd[0] = '\0';
                }
                drawScopeGauge(dl, ImVec2(x, gy), ImVec2(x + bw, bodyBottom), bays[i].cap,
                               bays[i].v, got, got ? rd : nullptr);
            }
        }
    }

    return bodyBottom + 8.0f - tl.y;
}

}  // namespace cascade::gui
