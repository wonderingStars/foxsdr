// instrument_beacon.cpp - the CASCADE_INSTRUMENT_BEACON face: the alarm panel
// of a 406 MHz COSPAS-SARSAT distress-beacon receiver.
//
// THE EQUIPMENT. Two real things, taken together, because neither alone is what
// this window is:
//
//   A 406 MHz BEACON TEST RECEIVER. WS Technologies' BT200 beacon tester is the
//   instrument this face borrows its VOCABULARY from - its results screen names
//   exactly the fields the plugin sends up here: "15 Hex ID", the beacon
//   information the protocol decides, the burst mode (normal or self test), the
//   channel frequency, and a frequency measurement quoted in hertz of error.
//   (WS Technologies BT200 Operator's Manual v1.30, "Measurement Results",
//   "Summary Section" and the specification table, read at
//   avionteq.com/document/newdocument/WS-Technologies-BT200-Operators-Manual.pdf;
//   product pages at wst.ca/bt200. The current BT200 is a ruggedised handset,
//   so what is borrowed is what it MEASURES, not how it looks.)
//
//   A BRIDGE DISTRESS ALARM PANEL. The look is the older, better-shaped
//   ancestor: a dark panel with one large red DISTRESS lamp under a clear
//   guard. The guard is the detail worth copying - on a GMDSS alarm panel the
//   distress control is under a spring-loaded transparent cover precisely
//   because it is the one thing on the bridge that must never be operated by
//   accident, and a panel that puts a lens under a guard is a panel saying
//   "this lamp is not decoration". (Cobham SAILOR 6103 GMDSS alarm panel; the
//   requirement that the distress alert control be protected against
//   inadvertent activation is in the GMDSS carriage rules, 47 CFR 80 subpart W
//   / IMO SOLAS IV. Ground-station context from COSPAS-SARSAT's own LUT
//   description, cospas-sarsat.int - a real LUT is an unmanned computer in a
//   shed, so the panel it feeds is what is drawn here, not the LUT itself.)
//
// Translated into this bench's materials: brass and dark enamel, ivory
// engraving, a phosphor-green well for the words the radio received and an
// amber segment display for the figures. The identity is drawn on SEVEN
// SEGMENTS rather than in type because that is what every beacon tester and
// every alarm annunciator of this vintage shows a hexadecimal identity on, and
// because it is the one place on the bench where a number is not simply
// lettered - see instrument_beacon_math.hpp for the table, and for why B and D
// are lower case on it.
//
// WHAT IS DRAWN, AND WHY EACH THING IS HERE (rule 1: nothing is a picture of a
// control, and every lamp has a reason to light):
//
//   DISTRESS      the ALERT flag, and it BLINKS - a beacon alert that merely
//                 glowed would be indistinguishable from a panel left switched
//                 on. Dark with no reading, dark for a declared self test.
//   the identity  text[0], the 15 Hex ID: the single thing a rescue
//                 co-ordination centre needs from this window.
//   NEW           cue.unread: a burst arrived that this window has not shown.
//   COUNTRY       text[1]   PROTOCOL text[2]   POSITION text[3]
//   CARRIER ERROR values[0], on a centre-zero meter, because zero is the
//                 meaningful reading and both directions are equally wrong.
//   SINCE LAST    values[1]: with a mean burst period near 50 s, the age of the
//                 last burst is what says whether the beacon is still there.
//   BURSTS LOGGED how many bursts the plugin's own log is holding.
//   the legend    engraved across the foot, permanently.
//
// AND RULE 2, WHICH THIS FACE OF ALL FACES MUST NOT BREAK: with no reading the
// segments are drawn UNLIT rather than blank - a dark readout with its ghosts
// faintly visible is what an idle display looks like - the lamp is dark, the
// needle is absent (not resting at zero), the counter apertures are empty and
// the plates carry captions with nothing on their glass. "No beacon has been
// heard" and "a beacon is transmitting zeroes" are opposite claims.
//
// WHY THE CENTRE-ZERO METER IS DRAWN HERE AND NOT TAKEN FROM scope_face.hpp.
// drawBenchMeter is a 0..1 travel meter and says so: its scale runs from a
// bottom to a top and its last two ticks are red because "the top of any
// meter's travel is where it should be uncomfortable to sit". A carrier error
// is not like that - zero is the good reading, both ends are equally bad, and
// the red belongs at BOTH stops. Borrowing the wrong meter and letting the
// caller pretend 0.5 means zero would have put a red band on one side of a
// symmetric scale, which is a meter that lies about which way is wrong. The
// materials below - cream tombstone, brass bezel, rust needle, no needle
// without a reading - are deliberately the same as that function's, so the two
// meters read as coming off the same bench.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/instrument_beacon_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;

namespace {

constexpr float kPiF = 3.14159265358979323846f;

// --- type ------------------------------------------------------------------

// The size at which this string fits `room`, never below the nine pixel floor
// the rest of the bench uses. Re-measured after each pass because glyph
// advances are rounded per size, so one division is close and not exact - the
// same loop scope_view.cpp's own fitter runs, and for the same reason.
float fitPx(ImFont* font, float px, const char* text, float room) {
    if (font == nullptr || text == nullptr || text[0] == '\0') { return px; }
    if (!(room > 0.0f) || !(px > 0.0f)) { return px; }
    float out = px;
    for (int pass = 0; pass < 4; ++pass) {
        const float w = font->CalcTextSizeA(out, FLT_MAX, 0.0f, text).x;
        if (!(w > room) || !(w > 0.0f)) { break; }
        const float next = std::max(9.0f, out * room / w - 0.05f);
        if (!(next < out)) { break; }
        out = next;
    }
    return out;
}

// An engraved caption, cut into whatever it is written on: the dark pass one
// pixel down and the muted ink over it. Fitted to `room`, and clipped there if
// even the floor size will not fit, because a caption running off its own plate
// is drawn over the control standing next to it.
void engrave(ImDrawList* dl, const ImVec2& at, const char* s, float room) {
    if (dl == nullptr || s == nullptr || s[0] == '\0') { return; }
    ImFont* f = fonts::legend();
    const float px = fitPx(f, fonts::kTinySize, s, room);
    dl->PushClipRect(ImVec2(at.x - 1.0f, at.y - 2.0f),
                     ImVec2(at.x + room + 1.0f, at.y + px + 6.0f), true);
    dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f), theme::withAlpha(theme::kVoid, 0.6f),
                s);
    dl->AddText(f, px, at, theme::kInkMuted, s);
    dl->PopClipRect();
}

// A live word on glass. Fitted, then clipped, then - if the clip would eat
// letters - cut short with two dots, so a value longer than its cell SAYS it
// was longer rather than appearing to end where it was trimmed.
void onGlass(ImDrawList* dl, const ImVec2& at, const char* s, float room, ImU32 col) {
    if (dl == nullptr || s == nullptr || s[0] == '\0' || !(room > 4.0f)) { return; }
    ImFont* f = fonts::ui();
    const float px = fitPx(f, fonts::kUiSize, s, room);
    char cut[CASCADE_INSTRUMENT_TEXT_CHARS + 4];
    const char* draw = s;
    if (f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x > room) {
        std::snprintf(cut, sizeof cut, "%s", s);
        std::size_t n = std::strlen(cut);
        while (n > 2u) {
            cut[n - 1] = '\0';
            cut[n - 2] = '.';
            char probe[CASCADE_INSTRUMENT_TEXT_CHARS + 6];
            std::snprintf(probe, sizeof probe, "%s.", cut);
            if (f->CalcTextSizeA(px, FLT_MAX, 0.0f, probe).x <= room) {
                std::snprintf(cut, sizeof cut, "%s.", probe);
                break;
            }
            --n;
        }
        draw = cut;
    }
    // AND CLIPPED ANYWAY. The trimming above is what makes an over-long value
    // READ correctly; this is what guarantees it cannot be drawn over the cell
    // beside it whatever the type does, which is a promise the fitting loop
    // alone cannot make once it has hit its nine pixel floor.
    dl->PushClipRect(ImVec2(at.x - 1.0f, at.y - 2.0f),
                     ImVec2(at.x + room + 1.0f, at.y + px + 6.0f), true);
    dl->AddText(f, px, at, col, draw);
    dl->PopClipRect();
}

// A figure on glass, in the monospaced face so it stops jittering sideways as
// it counts.
void figure(ImDrawList* dl, const ImVec2& at, const char* s, float px, ImU32 col) {
    if (dl == nullptr || s == nullptr || s[0] == '\0') { return; }
    dl->AddText(fonts::reading(), px, at, col, s);
}

// --- the distress lamp under its guard --------------------------------------

// A lens the size of a thumb, in a brass bezel, under a clear polycarbonate
// guard on two hinge lugs. `lit` is the alert; `bright` is the blink phase, so
// a held alert flashes rather than glowing steadily. Dark means dark: the lens
// keeps its own hue at low alpha, which is how a cold red lamp reads as a red
// lamp that is off rather than as a grey disc.
void drawDistressLamp(ImDrawList* dl, const ImVec2& c, float r, bool lit, bool bright) {
    if (dl == nullptr || !(r > 6.0f)) { return; }

    // The panel cut-out and the bezel standing proud of it.
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.10f), r * 1.22f,
                        theme::withAlpha(theme::kVoid, 0.55f), 0);
    dl->AddCircleFilled(c, r * 1.16f, theme::kBrassMid, 0);
    addBenchBevel(dl, ImVec2(c.x - r * 1.16f, c.y - r * 1.16f),
                  ImVec2(c.x + r * 1.16f, c.y + r * 1.16f), r * 1.16f, true);
    dl->AddCircleFilled(c, r * 1.02f, theme::kEnamelDark, 0);

    if (lit && bright) {
        // The bloom, as grown discs - a draw list has no blur. Its outermost
        // ring is 1.28 r, which is the figure instrument_beacon_math.hpp places
        // the lamp's centre against.
        for (int i = 4; i >= 1; --i) {
            dl->AddCircleFilled(c, r + static_cast<float>(i) * r * 0.07f,
                                theme::withAlpha(theme::kAlarmHot,
                                                 0.11f - static_cast<float>(i) * 0.020f),
                                0);
        }
        dl->AddCircleFilled(c, r, theme::kAlarmHot, 0);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.10f, c.y - r * 0.12f), r * 0.72f,
                            IM_COL32(0xFF, 0xB0, 0x80, 220), 0);
    } else if (lit) {
        // The dark half of the blink: still plainly a lit lamp between
        // flashes, not the same thing as an alarm that has cleared.
        dl->AddCircleFilled(c, r, theme::withAlpha(theme::kAlarm, 0.55f), 0);
    } else {
        // COLD, AND STILL PLAINLY A RED LAMP. The lens keeps its own hue taken
        // down rather than going grey: an unlit red lamp and an unlit green one
        // are different objects, and a panel a user can read cold is a panel
        // they can trust when it lights.
        dl->AddCircleFilled(c, r, theme::withAlpha(theme::kAlarm, 0.30f), 0);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.10f, c.y - r * 0.12f), r * 0.66f,
                            theme::withAlpha(theme::kVoid, 0.30f), 0);
    }

    // THE GUARD. A ring of clear plastic standing off the lens on two lugs,
    // with one specular streak across it. Drawn whether the lamp is lit or not,
    // because it is part of the panel and not part of the alarm.
    dl->AddCircle(c, r * 1.02f, IM_COL32(0xEF, 0xE7, 0xD2, 70), 0, 2.0f);
    dl->AddCircle(c, r * 0.94f, IM_COL32(0xEF, 0xE7, 0xD2, 34), 0, 1.0f);
    const float lug = r * 0.30f;
    for (int s = -1; s <= 1; s += 2) {
        const float lx = c.x + static_cast<float>(s) * r * 0.86f;
        dl->AddRectFilled(ImVec2(lx - lug * 0.35f, c.y - r * 1.20f),
                          ImVec2(lx + lug * 0.35f, c.y - r * 0.86f), theme::kBrassShade,
                          1.0f);
    }
    // The streak: a chord of light across the upper left of the cover.
    const float a0 = kPiF * 1.08f;
    const float a1 = kPiF * 1.42f;
    dl->PathArcTo(c, r * 0.80f, a0, a1, 12);
    dl->PathStroke(IM_COL32(255, 255, 255, lit && bright ? 90 : 55), ImDrawFlags_None,
                   std::max(1.5f, r * 0.10f));
}

// --- one seven-segment cell --------------------------------------------------

// `mask` is the segment table's answer for this character. Unlit segments are
// drawn too, faintly, which is both what a real display looks like from the
// side and the honest way to show a cell with nothing in it.
//
// THE SLANT is the small forward lean every LED display of this kind has. It is
// applied as a shear about the middle of the digit, so a cell leans without
// growing: the top of the first character moves right and the bottom moves
// left, by a twentieth of the digit height, which the glass's own padding
// covers.
void drawSegments(ImDrawList* dl, const ImVec2& centre, float digitH, float cellW,
                  std::uint8_t mask, bool poweredOn) {
    if (dl == nullptr || !(digitH > 6.0f)) { return; }
    const float dw = std::min(cellW * 0.94f, digitH * 0.60f);
    const float t = std::max(1.4f, digitH * 0.125f);
    const float left = centre.x - dw * 0.5f;
    const float right = centre.x + dw * 0.5f;
    const float top = centre.y - digitH * 0.5f;
    const float bot = centre.y + digitH * 0.5f;
    const float mid = centre.y;
    constexpr float kSlant = 0.10f;

    const ImU32 lit = theme::kAmber;
    const ImU32 glow = theme::withAlpha(theme::kAmber, 0.16f);
    // The ghost of an unlit segment. Faint enough to read as off, present
    // enough that an idle readout is visibly a readout.
    const ImU32 off = theme::withAlpha(theme::kAmberDim, poweredOn ? 0.30f : 0.22f);

    auto shear = [&](float x, float y) {
        return ImVec2(x + (centre.y - y) * kSlant, y);
    };

    auto horiz = [&](float y, bool on) {
        ImVec2 p[6] = {shear(left + t * 0.5f, y),        shear(left + t, y - t * 0.5f),
                       shear(right - t, y - t * 0.5f),   shear(right - t * 0.5f, y),
                       shear(right - t, y + t * 0.5f),   shear(left + t, y + t * 0.5f)};
        if (on) { dl->AddConvexPolyFilled(p, 6, glow); }
        dl->AddConvexPolyFilled(p, 6, on ? lit : off);
    };
    auto vert = [&](float x, float ya, float yb, bool on) {
        ImVec2 p[6] = {shear(x, ya + t * 0.5f),          shear(x + t * 0.5f, ya + t),
                       shear(x + t * 0.5f, yb - t),      shear(x, yb - t * 0.5f),
                       shear(x - t * 0.5f, yb - t),      shear(x - t * 0.5f, ya + t)};
        if (on) { dl->AddConvexPolyFilled(p, 6, glow); }
        dl->AddConvexPolyFilled(p, 6, on ? lit : off);
    };

    horiz(top, (mask & 0x01u) != 0u);                 // a
    vert(right, top, mid, (mask & 0x02u) != 0u);      // b
    vert(right, mid, bot, (mask & 0x04u) != 0u);      // c
    horiz(bot, (mask & 0x08u) != 0u);                 // d
    vert(left, mid, bot, (mask & 0x10u) != 0u);       // e
    vert(left, top, mid, (mask & 0x20u) != 0u);       // f
    horiz(mid, (mask & 0x40u) != 0u);                 // g
}

// --- the centre-zero meter ---------------------------------------------------

void drawCentreZeroMeter(ImDrawList* dl, const ImVec2& tl, float width, float height,
                         const char* caption, float frac01, bool haveReading,
                         const char* valueLine) {
    if (dl == nullptr || width < 40.0f || height < 40.0f) { return; }
    ImFont* cf = fonts::legend();
    ImFont* vf = fonts::ui();
    const float tiny = fonts::kTinySize;
    const char* cap = (caption != nullptr) ? caption : "";
    const char* val = (valueLine != nullptr && valueLine[0] != '\0') ? valueLine : "--";
    const float cpx = fitPx(cf, tiny, cap, width - 4.0f);
    const float vpx = fitPx(vf, tiny, val, width - 4.0f);
    const ImVec2 cs = cf->CalcTextSizeA(cpx, FLT_MAX, 0.0f, cap);
    const ImVec2 vs = vf->CalcTextSizeA(vpx, FLT_MAX, 0.0f, val);
    const float capH = (cap[0] != '\0') ? cs.y : 0.0f;

    if (cap[0] != '\0') {
        // CUT THE WAY THE OTHER TWO BAYS' CAPTIONS ARE. drawBenchMeter letters
        // its caption dark-into-brass, which is right on the bench's metal and
        // wrong here: this meter stands on the plate's dark ENAMEL, where a
        // void-coloured caption over a pale lower lip is a caption nobody can
        // find - the first photograph of this panel had CARRIER ERROR as a
        // smudge between two perfectly readable neighbours. Same treatment as
        // engrave() above, so the three captions on this deck read as one row.
        const ImVec2 at(tl.x + width * 0.5f - cs.x * 0.5f, tl.y);
        dl->AddText(cf, cpx, ImVec2(at.x + 1.0f, at.y + 1.0f),
                    theme::withAlpha(theme::kVoid, 0.6f), cap);
        dl->AddText(cf, cpx, at, theme::kInkMuted, cap);
    }

    const float faceTop = tl.y + capH + 3.0f;
    const float faceH = height - capH - vs.y - 8.0f;
    if (faceH < 20.0f) { return; }
    const ImVec2 fTL(tl.x, faceTop);
    const ImVec2 fBR(tl.x + width, faceTop + faceH);

    dl->AddRectFilledMultiColor(fTL, fBR, IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255));
    dl->AddRect(fTL, fBR, theme::kBrassBright, 3.0f, 0, 2.0f);

    const ImVec2 pivot(tl.x + width * 0.5f, fBR.y - 4.0f);
    constexpr float kHalfSweepDeg = 52.0f;
    const float armByHeight = faceH * 0.78f;
    const float reach = std::sin(kHalfSweepDeg * kPiF / 180.0f) * 0.94f;
    const float armByWidth = (width * 0.5f - 3.0f) / std::max(0.01f, reach);
    const float armR = std::min(armByHeight, armByWidth);

    // NINE TICKS, AND THE RED IS AT BOTH ENDS. On a centre-zero scale the far
    // stops are equally wrong in opposite directions, and the heavy tick in the
    // middle is the reading the instrument exists to find.
    for (int i = 0; i < 9; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * t;
        const float a = deg * kPiF / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        const bool edge = (i <= 1 || i >= 7);
        const ImU32 col = edge ? theme::kAlarm : theme::kEngraved;
        const float th = (i == 4) ? 2.4f : (edge ? 1.6f : 1.0f);
        const float in = (i == 4) ? 0.72f : 0.80f;
        dl->AddLine(ImVec2(pivot.x + sx * armR * in, pivot.y + sy * armR * in),
                    ImVec2(pivot.x + sx * armR * 0.94f, pivot.y + sy * armR * 0.94f), col,
                    th);
    }

    if (haveReading) {
        const float f = std::clamp(frac01, 0.0f, 1.0f);
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * f;
        const float a = deg * kPiF / 180.0f;
        dl->AddLine(pivot,
                    ImVec2(pivot.x + std::sin(a) * armR * 0.88f,
                           pivot.y - std::cos(a) * armR * 0.88f),
                    theme::kAlarm, 1.8f);
        dl->AddCircleFilled(pivot, 3.4f, theme::kEnamel, 12);
    } else {
        // No needle at all: a needle resting on the centre tick would read as
        // "measured, and the beacon is exactly on frequency".
        dl->AddCircleFilled(pivot, 3.4f, theme::kInkMuted, 12);
    }

    // The unit, printed beside the pivot the way a moving-coil meter names its
    // own scale - beside, because above is where the needle sweeps.
    const float upx = std::min(tiny, faceH * 0.34f);
    const ImVec2 us = vf->CalcTextSizeA(upx, FLT_MAX, 0.0f, "kHz");
    if (pivot.x + armR * 0.16f + us.x < fBR.x - 3.0f) {
        dl->AddText(vf, upx, ImVec2(pivot.x + armR * 0.16f, pivot.y - us.y - 2.0f),
                    theme::kEngraved, "kHz");
    }

    dl->AddText(vf, vpx, ImVec2(tl.x + width * 0.5f - vs.x * 0.5f, fBR.y + 3.0f),
                haveReading ? theme::kIvory : theme::kCream, val);
}

// --- one engraved caption over one strip of glass ---------------------------

void drawPlateCell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* caption,
                   const char* value) {
    if (dl == nullptr || br.x - tl.x < 24.0f || br.y - tl.y < 20.0f) { return; }
    const float capH = fonts::kTinySize + 2.0f;
    engrave(dl, ImVec2(tl.x, tl.y), caption, br.x - tl.x);
    const ImVec2 gTL(tl.x, tl.y + capH);
    if (br.y - gTL.y < 12.0f) { return; }
    drawFreqDrumWell(dl, gTL, br);
    const float pad = 5.0f;
    onGlass(dl, ImVec2(gTL.x + pad, gTL.y + (br.y - gTL.y - fonts::kUiSize) * 0.5f + 1.0f),
            value, br.x - gTL.x - pad * 2.0f, theme::kPhosphor);
}

// --- the counter drums -------------------------------------------------------

// Three apertures of the bench's own amber counter. `have` false leaves the
// drums BLANK rather than showing 000 - the same distinction the whole face
// turns on.
void drawCounter(ImDrawList* dl, const ImVec2& tl, float width, float height, int value,
                 bool have) {
    if (dl == nullptr || width < 24.0f || height < 14.0f) { return; }
    constexpr int kDigits = 3;
    const float gap = 2.0f;
    const float cw = (width - gap * static_cast<float>(kDigits - 1)) /
                     static_cast<float>(kDigits);
    drawFreqDrumWell(dl, ImVec2(tl.x - 3.0f, tl.y - 3.0f),
                     ImVec2(tl.x + width + 3.0f, tl.y + height + 3.0f));
    int v = value;
    if (v < 0) { v = 0; }
    if (v > 999) { v = 999; }
    const float px = std::min(height * 0.78f, cw * 1.25f);
    for (int i = 0; i < kDigits; ++i) {
        const ImVec2 cTL(tl.x + static_cast<float>(i) * (cw + gap), tl.y);
        const ImVec2 cBR(cTL.x + cw, tl.y + height);
        int place = 1;
        for (int k = 0; k < kDigits - 1 - i; ++k) { place *= 10; }
        const int digit = (v / place) % 10;
        // A leading zero is dimmed, exactly as the tuned-frequency counter
        // dims its own, so the eye reads only the live figure.
        const bool bright = have && (digit != 0 || v >= place);
        drawFreqDrumCell(dl, cTL, cBR, have ? static_cast<char>('0' + digit) : ' ', bright,
                         px);
    }
}

}  // namespace

float drawBeaconFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                     const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    if (br.x - tl.x < 80.0f || br.y - tl.y < 60.0f) { return 0.0f; }

    const float headerY = addBenchPlate(dl, tl, br, in.title.c_str());
    const beacon::Layout L = beacon::layout(tl.x, headerY, br.x, br.y);
    if (!L.any) { return headerY - tl.y; }

    const CascadeInstrumentState& s = in.state;
    const bool alert = in.have && (s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
    // 2 Hz, which is what tells a held alarm apart from a lamp that is simply
    // on. cue.nowSec is the host's clock, so every blinking thing in the
    // application flashes together.
    const bool blinkOn = std::fmod(cue.nowSec, 0.5) < 0.28;

    dl->PushClipRect(tl, br, true);

    // ---- deck 1: the lamp, the identity, and NEW ---------------------------
    drawDistressLamp(dl, ImVec2(L.lampCx, L.lampCy), L.lampR, alert, blinkOn);
    {
        ImFont* f = fonts::legend();
        const float px = fitPx(f, fonts::kTinySize, "DISTRESS", L.lampR * 2.6f);
        const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "DISTRESS");
        const ImVec2 at(L.lampCx - sz.x * 0.5f, L.lampCy + L.lampR * 1.2f + 3.0f);
        if (at.y + sz.y < L.readY1 + 2.0f) {
            // Clipped to the lamp's own column: at the smallest lamp this word
            // is wider than the lens it names even at the floor size, and what
            // it must not do is run under the identity's glass.
            dl->PushClipRect(ImVec2(tl.x, at.y - 2.0f),
                             ImVec2(L.wellX0 - 2.0f, at.y + px + 6.0f), true);
            dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                        theme::withAlpha(theme::kVoid, 0.6f), "DISTRESS");
            dl->AddText(f, px, at, alert ? theme::kIvory : theme::kCream, "DISTRESS");
            dl->PopClipRect();
        }
    }
    if (L.newLamp) {
        drawBenchLamp(dl, ImVec2(L.newCx, L.newCy), L.newR, theme::kGold, cue.unread,
                      nullptr);
        ImFont* f = fonts::legend();
        const float px = fitPx(f, fonts::kTinySize, "NEW", L.newR * 4.0f);
        const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "NEW");
        const ImVec2 at(L.newCx - sz.x * 0.5f, L.newCy + L.newR + 4.0f);
        dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                    theme::withAlpha(theme::kVoid, 0.6f), "NEW");
        dl->AddText(f, px, at, cue.unread ? theme::kIvory : theme::kCream, "NEW");
    }

    engrave(dl, ImVec2(L.wellX0 + 2.0f, L.readY0), "BEACON 15 HEX ID",
            L.wellX1 - L.wellX0 - 4.0f);
    const float glassTop = L.readY0 + fonts::kTinySize + 3.0f;
    if (L.readY1 - glassTop > 10.0f) {
        drawFreqDrumWell(dl, ImVec2(L.wellX0, glassTop), ImVec2(L.wellX1, L.readY1));
        char cells[beacon::kHexIdChars + 1];
        beacon::layHexId(in.have ? s.text[0] : "", cells);
        const float cy = (glassTop + L.readY1) * 0.5f;
        const float row = L.wellX0 + 8.0f;
        for (int i = 0; i < beacon::kHexIdChars; ++i) {
            const float cx = row + (L.cellW + L.cellGap) * static_cast<float>(i) +
                             L.cellW * 0.5f;
            drawSegments(dl, ImVec2(cx, cy), L.digitH, L.cellW,
                         beacon::segments(cells[i]), in.have);
        }
    }

    // ---- deck 2: country, protocol, position -------------------------------
    if (L.plates) {
        const float w = L.x1 - L.x0;
        const float gap = 8.0f;
        // The country name is the longest of the three by a wide margin (the
        // ITU MID table's own wording runs past fifty characters), so it gets
        // the widest cell rather than an equal third.
        const float wc = (w - gap * 2.0f) * 0.38f;
        const float wp = (w - gap * 2.0f) * 0.34f;
        const float wq = (w - gap * 2.0f) - wc - wp;
        float x = L.x0;
        drawPlateCell(dl, ImVec2(x, L.plateY0), ImVec2(x + wc, L.plateY1), "COUNTRY",
                      in.have ? s.text[1] : "");
        x += wc + gap;
        drawPlateCell(dl, ImVec2(x, L.plateY0), ImVec2(x + wp, L.plateY1), "PROTOCOL",
                      in.have ? s.text[2] : "");
        x += wp + gap;
        drawPlateCell(dl, ImVec2(x, L.plateY0), ImVec2(x + wq, L.plateY1), "POSITION",
                      in.have ? s.text[3] : "");
    }

    // ---- deck 3: carrier error, age, bursts --------------------------------
    if (L.gauges > 0) {
        const float w = L.x1 - L.x0;
        const float gap = 10.0f;
        const float h = L.gaugeY1 - L.gaugeY0;
        const int n = L.gauges;
        const float bay = (w - gap * static_cast<float>(n - 1)) / static_cast<float>(n);

        // The frequency slot is only a reading when the plugin filled it, and
        // an exact zero is what an unfilled double looks like. A carrier error
        // of exactly 0.000 Hz does not happen on a real measurement, so the
        // face treats it as an empty slot - which draws no needle rather than
        // claiming a perfect beacon.
        const bool haveErr = in.have && s.values[0] != 0.0 && std::isfinite(s.values[0]);
        char errLine[32];
        beacon::formatError(s.values[0], haveErr, errLine, sizeof errLine);
        // A TOMBSTONE METER IS ABOUT TWICE AS WIDE AS ITS FACE IS TALL, and a
        // bay wider than that does not make a better meter - it makes a letter
        // box with a short needle stranded in the middle of it, which is what
        // the first photograph of this panel showed. The bay keeps its width;
        // the meter takes only what it can use.
        const float meterW = std::min(bay, (h - 42.0f) * 3.4f + 40.0f);
        drawCentreZeroMeter(dl, ImVec2(L.x0, L.gaugeY0), meterW, h, "CARRIER ERROR",
                            static_cast<float>(beacon::errorFraction(s.values[0])),
                            haveErr, errLine);

        if (n >= 2) {
            const float x = L.x0 + bay + gap;
            engrave(dl, ImVec2(x, L.gaugeY0), "SINCE LAST BURST", bay);
            const ImVec2 gTL(x, L.gaugeY0 + fonts::kTinySize + 3.0f);
            const ImVec2 gBR(x + bay, L.gaugeY1 - 2.0f);
            if (gBR.y - gTL.y > 14.0f) {
                drawFreqDrumWell(dl, gTL, gBR);
                const bool haveAge = in.have && std::isfinite(s.values[1]);
                char age[32];
                beacon::formatAge(s.values[1], haveAge, age, sizeof age);
                if (age[0] != '\0') {
                    const float px = std::min(fonts::kReadingSize * 1.6f,
                                              (gBR.y - gTL.y) * 0.62f);
                    ImFont* rf = fonts::reading();
                    const ImVec2 sz = rf->CalcTextSizeA(px, FLT_MAX, 0.0f, age);
                    figure(dl, ImVec2((gTL.x + gBR.x) * 0.5f - sz.x * 0.5f,
                                      (gTL.y + gBR.y) * 0.5f - sz.y * 0.5f),
                           age, px, theme::kAmber);
                }
            }
        }
        if (n >= 3) {
            const float x = L.x0 + (bay + gap) * 2.0f;
            engrave(dl, ImVec2(x, L.gaugeY0), "BURSTS LOGGED", bay);
            const float dy = L.gaugeY0 + fonts::kTinySize + 6.0f;
            const float dh = std::min(L.gaugeY1 - dy - 4.0f, 44.0f);
            if (dh > 12.0f) {
                const float dw = std::min(bay - 8.0f, dh * 1.9f);
                drawCounter(dl, ImVec2(x + 4.0f, dy), dw, dh,
                            static_cast<int>(in.rows.size()),
                            in.have || !in.rows.empty());
            }
        }
    }

    // ---- deck 4: the legend ------------------------------------------------
    if (L.legend) {
        const float w = L.x1 - L.x0;
        ImFont* f = fonts::legend();
        dl->PushClipRect(ImVec2(L.x0 - 1.0f, L.legY0 - 1.0f),
                         ImVec2(L.x1 + 1.0f, L.legY1 + 1.0f), true);
        const char* whole = beacon::warningLine(0);
        const float px = fitPx(f, fonts::kTinySize, whole, w);
        // ONE LINE ONLY WHILE ONE LINE IS STILL READABLE. Below twelve pixels
        // the engraving stops being a warning and becomes a texture, so the
        // legend goes to two lines instead - it may not be dropped and it may
        // not be truncated.
        if (px >= 12.0f) {
            const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, whole);
            const ImVec2 at(L.x0 + (w - sz.x) * 0.5f,
                            (L.legY0 + L.legY1) * 0.5f - sz.y * 0.5f);
            dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                        theme::withAlpha(theme::kVoid, 0.6f), whole);
            dl->AddText(f, px, at, theme::kInkMuted, whole);
        } else {
            for (int half = 1; half <= 2; ++half) {
                const char* line = beacon::warningLine(half);
                const float lpx = fitPx(f, fonts::kTinySize * 0.9f, line, w);
                const ImVec2 sz = f->CalcTextSizeA(lpx, FLT_MAX, 0.0f, line);
                const float rowH = (L.legY1 - L.legY0) * 0.5f;
                const ImVec2 at(L.x0 + (w - sz.x) * 0.5f,
                                L.legY0 + rowH * static_cast<float>(half - 1) +
                                    (rowH - sz.y) * 0.5f);
                dl->AddText(f, lpx, ImVec2(at.x + 1.0f, at.y + 1.0f),
                            theme::withAlpha(theme::kVoid, 0.6f), line);
                dl->AddText(f, lpx, at, theme::kInkMuted, line);
            }
        }
        dl->PopClipRect();
    }

    dl->PopClipRect();
    return (headerY - tl.y) + L.used;
}

}  // namespace cascade::gui
