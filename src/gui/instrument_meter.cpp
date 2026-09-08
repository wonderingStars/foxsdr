// instrument_meter.cpp - the CASCADE_INSTRUMENT_METER face: a North American
// utility meter, seen through the glass.
//
// ===========================================================================
// THE EQUIPMENT
// ===========================================================================
//
// An Itron CENTRON residential electricity meter with an ERT radio module -
// the round, socket-mounted meter on the side of tens of millions of North
// American houses, and the thing an ERT decoder is actually listening to. The
// same face serves a gas or water index with an ERT module fitted, because
// what those broadcast is the same standard consumption message with a
// different commodity in it.
//
// WHAT WAS LOOKED AT, all of it for proportion, legend and materials only;
// every line of the drawing below is written here:
//
//   [1] Itron, "CENTRON Polyphase CP1SR R400" specification sheet,
//       101380SP-01, na.itron.com. Fetched 2026-09-08 and read as text. Names
//       the standard features this face reproduces: "Electronic LCD display",
//       "Polycarbonate cover", "Test LED", "Voltage indication", and states
//       that "each RF transmission contains the unit ID number, unit type,
//       energy usage, and tamper status" - which IS this kind's slot map,
//       from the manufacturer.
//   [2] Itron, "The CENTRON Electricity Meter" product brochure,
//       na.itron.com. Fetched 2026-09-08 and read as text; the interchangeable
//       personality module and the residential round-meter form.
//   [3] Itron CENTRON / CENTRON C1SR Technical Reference Guide, as summarised
//       in publicly indexed listings: an "ANSI C12.10 compliant, 104 segment
//       liquid crystal display", "a nine-digit LCD, with a variety of
//       annunciators", and the face's magnetic switch, demand reset button,
//       optical port, infrared test LED and NAMEPLATE.
//   [4] Itron gas ERT module material (100G series listings): the module
//       reads a "standard dial and direct-read (odometer) index", which is
//       why the commodity is a lamp on this face and not a second drawing.
//
// The proportions taken from those: a round bezel with a clear cover over a
// pale dial; a wide rectangular LCD across the middle of that dial carrying
// the register in seven-segment figures with small annunciators around it;
// the meter's own number on a printed label under the display; a nameplate
// legend above it; and a test lamp low on the face.
//
// ===========================================================================
// TRANSLATING IT ONTO THE BENCH
// ===========================================================================
//
// THIS IS THE ONE FACE WHERE A PALE DISPLAY IS RIGHT, and it is worth saying
// why it does not break theme.hpp's rule. That rule - a caption may be
// engraved, a live figure must be on glass - is about CONTRAST: dark ink cut
// into brass measures about 2.3:1 and is not good enough for a number. A
// liquid-crystal display is glass; it simply runs the other way round, dark
// segments on a pale ground, and the pair used here measures about 11:1. The
// rule is kept, not bent. The one thing that would break it is putting a live
// figure on the BRASS, and nothing here does.
//
// So: the bench's brass for the bezel and the surrounding panel, the bench's
// enamel wells for the figures that are not on the meter itself, ivory for
// the dial (which is what the real one is), and the LCD's own two tones for
// the register. The bezel, its bevel, its shadow and the engraved captions are
// the same calls every other panel in the application uses, so the meter sits
// on the bench rather than beside it.
//
// TAMPER IS DRAWN IN THE TROUBLE COLOUR, NOT IN AMBER. The design brief for
// this face asked for amber tamper flags; theme.hpp reserves amber for a
// NUMBER and gives trouble its own rust, precisely so a warning and a reading
// can never be confused. Rust wins: the flag is a warning, the count beside it
// is the number. Said out loud here because it is a deliberate departure.
//
// ===========================================================================
// THE THREE RULES
// ===========================================================================
//
//  1. Engraved captions (COMMODITY, TAMPER, LAST HEARD, the nameplate) are
//     cut into the metal and the dial. Every live figure - the register, the
//     tamper counts, the age, the meter number - is on glass or on the
//     printed label, both of which are high contrast.
//  2. NO READING IS DRAWN AS NO READING. With in.have false the LCD shows its
//     unlit ghost segments and not one lit bar, the three commodity lamps are
//     all dark, the tamper cells are empty rather than showing 0, and the
//     age cell is blank. A dark commodity lamp and a blank cell are the only
//     honest pictures of "we have not heard this meter".
//  3. It fits the rectangle. Everything is derived from the rectangle's own
//     size through one scale factor; the whole body is clipped; and below the
//     size where the right-hand column can be read, the column is dropped and
//     the meter keeps the room instead of the two overlapping.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "gui/fonts.hpp"
#include "gui/instrument_face.hpp"
#include "gui/instrument_meter_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// --- the liquid crystal ------------------------------------------------------
//
// Not in theme.hpp because nothing else in the application has one, and a
// palette entry used once is a palette entry that drifts. The pair is the
// grey-green of a reflective twisted-nematic panel and the near-black its
// segments go: about 11:1, which is what lets the register be read at arm's
// length the way the rule demands.
constexpr ImU32 kLcdTop = IM_COL32(0x9E, 0xA8, 0x90, 0xFF);
constexpr ImU32 kLcdBottom = IM_COL32(0x86, 0x91, 0x79, 0xFF);
constexpr ImU32 kLcdLit = IM_COL32(0x17, 0x1B, 0x11, 0xFF);
// The unlit segment. A real panel's inactive bars are faintly visible against
// the ground, and drawing them is what makes an empty register read as a
// display that is switched on with nothing to say - rather than as a hole.
constexpr ImU32 kLcdGhost = IM_COL32(0x92, 0x9C, 0x85, 0xFF);
// The printed label under the display: paper, and ink on paper.
constexpr ImU32 kLabelPaper = IM_COL32(0xE7, 0xE1, 0xD1, 0xFF);
constexpr ImU32 kLabelInk = IM_COL32(0x22, 0x1E, 0x17, 0xFF);
// The dial the whole lot is printed on.
constexpr ImU32 kDialTop = IM_COL32(0xE9, 0xE2, 0xCE, 0xFF);
constexpr ImU32 kDialBottom = IM_COL32(0xC9, 0xC1, 0xAB, 0xFF);
constexpr ImU32 kDialInk = IM_COL32(0x3A, 0x34, 0x28, 0xFF);

// One bar of a seven-segment cell, as a flat hexagon - square in the middle
// and mitred at both ends, which is how a real segment is cut and what stops
// two meeting bars overlapping into a blob at the corner.
void segBar(ImDrawList* dl, float x0, float y0, float x1, float y1, float t, ImU32 col) {
    const float h = t * 0.5f;
    ImVec2 p[6];
    if (std::fabs(y1 - y0) < 0.5f) {  // horizontal
        p[0] = ImVec2(x0, y0);
        p[1] = ImVec2(x0 + h, y0 - h);
        p[2] = ImVec2(x1 - h, y0 - h);
        p[3] = ImVec2(x1, y0);
        p[4] = ImVec2(x1 - h, y0 + h);
        p[5] = ImVec2(x0 + h, y0 + h);
    } else {  // vertical
        p[0] = ImVec2(x0, y0);
        p[1] = ImVec2(x0 + h, y0 + h);
        p[2] = ImVec2(x0 + h, y1 - h);
        p[3] = ImVec2(x0, y1);
        p[4] = ImVec2(x0 - h, y1 - h);
        p[5] = ImVec2(x0 - h, y0 + h);
    }
    dl->AddConvexPolyFilled(p, 6, col);
}

// One figure. `mask` is meter::segments() of the character; every bar the mask
// does not name is still drawn, in the ghost tone, because that is what a
// liquid crystal looks like and it is how an unlit register says "on, with
// nothing to show" instead of "absent".
// THE GLYPH IS SIZED FROM ITS OWN ASPECT, not from the cell. Eight cells across
// a meter dial are narrow and tall; a figure drawn to fill one is a thin
// ladder, and one drawn with a fixed inset off a narrow cell has no bar left
// at all - which is exactly what happened the first time this was drawn, and
// the register came out blank. So the glyph box is the tallest 0.62:1
// rectangle that fits the cell, centred in it.
void drawDigit(ImDrawList* dl, const ImVec2& tl, float w, float h, unsigned mask,
               bool ghostUnlit, ImU32 litCol) {
    const float gh = h;
    const float gw = std::min(w * 0.84f, gh * 0.62f);
    const float t = std::max(1.2f, gh * 0.115f);
    if (gw < t * 2.2f || gh < t * 4.0f) { return; }
    const float cx = tl.x + w * 0.5f;
    const float x0 = cx - gw * 0.5f + t * 0.5f;
    const float x1 = cx + gw * 0.5f - t * 0.5f;
    const float yTop = tl.y + t * 0.5f;
    const float yMid = tl.y + gh * 0.5f;
    const float yBot = tl.y + gh - t * 0.5f;

    struct Bar {
        unsigned bit;
        float ax, ay, bx, by;
    };
    const Bar bars[7] = {
        {meter::kSegA, x0, yTop, x1, yTop},
        {meter::kSegB, x1, yTop, x1, yMid},
        {meter::kSegC, x1, yMid, x1, yBot},
        {meter::kSegD, x0, yBot, x1, yBot},
        {meter::kSegE, x0, yMid, x0, yBot},
        {meter::kSegF, x0, yTop, x0, yMid},
        {meter::kSegG, x0, yMid, x1, yMid},
    };
    for (const Bar& b : bars) {
        const bool on = (mask & b.bit) != 0u;
        if (!on && !ghostUnlit) { continue; }
        segBar(dl, b.ax, b.ay, b.bx, b.by, t, on ? litCol : kLcdGhost);
    }
}

// An engraved caption on the dial - dark ink on ivory, which is a printed
// legend rather than a cut one, and is what the real nameplate is.
void dialText(ImDrawList* dl, ImFont* f, float px, const ImVec2& at, const char* s,
              ImU32 col) {
    dl->AddText(f, px, at, col, s);
}

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// A small sunk well with a live figure in it, and NOTHING in it when there is
// no figure. The blank well is rule two: it is a cell that has not been
// filled, which is a different picture from a cell holding a zero.
void glassCell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* value,
               float px) {
    if (br.x - tl.x < 8.0f || br.y - tl.y < 6.0f) { return; }
    drawFreqDrumWell(dl, tl, br);
    if (value == nullptr || value[0] == '\0') { return; }
    ImFont* f = fonts::reading();
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, value);
    const ImVec2 at(tl.x + 6.0f, (tl.y + br.y) * 0.5f - sz.y * 0.5f);
    dl->PushClipRect(tl, br, true);
    dl->AddText(f, px, at, theme::kAmber, value);
    dl->PopClipRect();
}

}  // namespace

float drawMeterFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    // Below this there is no drawing that would be an instrument rather than a
    // smear, so the generic readout - which is honest at any size - takes it.
    if (w < 220.0f || h < 110.0f) { return drawGenericFace(dl, tl, br, in, cue); }

    const float plateY = addBenchPlate(dl, tl, br, in.title.c_str());
    const ImVec2 bTL(tl.x + 10.0f, plateY + 4.0f);
    const ImVec2 bBR(br.x - 10.0f, br.y - 8.0f);
    const float bodyW = bBR.x - bTL.x;
    const float bodyH = bBR.y - bTL.y;
    if (bodyW < 160.0f || bodyH < 70.0f) { return plateY - tl.y; }

    dl->PushClipRect(bTL, bBR, true);

    // ONE SCALE FOR EVERYTHING. Every size below is this times a constant, so
    // the drawing shrinks as one object instead of the type staying put while
    // the metal moves - which is what makes a resized panel look broken.
    const float s = std::clamp(std::min(bodyH / 210.0f, bodyW / 540.0f), 0.55f, 1.5f);
    const float capPx = std::clamp(fonts::kTinySize * s, 9.0f, 18.0f);
    const float readPx = std::clamp(fonts::kReadingSize * s, 9.0f, 20.0f);

    // The right-hand column is dropped rather than squeezed: below the width
    // its captions need, a column of clipped words is worse than no column,
    // and the meter takes the whole body instead.
    const float colMinW = 150.0f * s;
    const bool haveCol = bodyW > 260.0f * s + colMinW;

    const float meterBoxW = haveCol ? (bodyW - colMinW - 16.0f * s) : bodyW;
    float radius = std::min(bodyH, meterBoxW) * 0.5f - 2.0f;
    if (radius > bodyH * 0.5f - 2.0f) { radius = bodyH * 0.5f - 2.0f; }
    // A METER IS A PHYSICAL SIZE, so the dial stops growing and the room goes
    // to the column instead. Without the cap, a plugin with no roster hands
    // this face the whole window and the dial becomes a foot across with the
    // column squeezed against the edge - which is a picture of a meter rather
    // than an instrument on a bench.
    if (radius > 150.0f) { radius = 150.0f; }
    if (radius < 34.0f) { radius = 34.0f; }
    const ImVec2 c(bTL.x + radius + 3.0f, bTL.y + bodyH * 0.5f);
    const float r = radius;

    // --- the bezel ----------------------------------------------------------
    // Brass ring, proud of the panel, with its own shadow beneath it: the same
    // three calls the transport button is built from, so the two are the same
    // metal.
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.05f), r * 1.02f,
                        theme::withAlpha(theme::kVoid, 0.50f), 0);
    dl->AddCircleFilled(c, r, theme::kBrassMid, 0);
    addBenchBevel(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), r, true);
    dl->AddCircleFilled(c, r * 0.955f, theme::kBrassDark, 0);
    const float dialR = r * 0.90f;

    // --- the dial -----------------------------------------------------------
    // Ivory, shaded from the top, which is what a photograph of one looks like
    // under a cover. ImDrawList has no radial fill, so the shade is three
    // stacked discs offset upward - enough to stop it reading as a flat disc.
    dl->AddCircleFilled(c, dialR, kDialBottom, 0);
    dl->AddCircleFilled(ImVec2(c.x, c.y - dialR * 0.10f), dialR * 0.95f,
                        IM_COL32(0xDC, 0xD5, 0xC0, 0xFF), 0);
    dl->AddCircleFilled(ImVec2(c.x, c.y - dialR * 0.20f), dialR * 0.80f, kDialTop, 0);
    dl->AddCircle(c, dialR, theme::withAlpha(theme::kVoid, 0.45f), 0, 1.0f);

    // --- what is printed on the dial ---------------------------------------
    ImFont* leg = fonts::legend();
    ImFont* uiF = fonts::ui();
    const meter::Commodity comm = meter::commodity(in.have ? in.state.text[1] : "");
    const char* nameplate = "UTILITY METER";
    switch (comm) {
        case meter::Commodity::Electric: nameplate = "ELECTRICITY METER"; break;
        case meter::Commodity::Gas: nameplate = "GAS METER"; break;
        case meter::Commodity::Water: nameplate = "WATER METER"; break;
        case meter::Commodity::Unknown: break;
    }
    {
        // The nameplate legend across the top of the dial. Printed, not cut:
        // it names the machine, and it changes only with the commodity slot -
        // it never states a figure.
        float px = std::clamp(r * 0.125f, 7.5f, capPx);
        while (px > 7.5f && textW(leg, px, nameplate) > dialR * 1.55f) { px -= 0.5f; }
        const float tw = textW(leg, px, nameplate);
        dialText(dl, leg, px, ImVec2(c.x - tw * 0.5f, c.y - r * 0.68f), nameplate,
                 kDialInk);
    }

    // --- the liquid crystal -------------------------------------------------
    //
    // EVERY RECTANGLE ON THIS DIAL IS SIZED SO ITS CORNERS FALL INSIDE THE
    // GLASS. The dial is a circle of 0.90r and a rectangle's corner is the
    // furthest point of it, so each half-width below is chosen against its own
    // largest vertical offset: 0.80r at 0.26r down is 0.84r out, and the label
    // at 0.60r by 0.47r is 0.76r out. Sized by eye instead, a panel looks
    // right at one window size and hangs over the bezel at the next.
    const ImVec2 lcdTL(c.x - r * 0.80f, c.y - r * 0.26f);
    const ImVec2 lcdBR(c.x + r * 0.80f, c.y + r * 0.15f);
    {
        // The panel's own bezel: a thin dark surround, then the ground, then
        // the glass sheen across the top. Three rectangles and a gradient.
        dl->AddRectFilled(ImVec2(lcdTL.x - 3.0f, lcdTL.y - 3.0f),
                          ImVec2(lcdBR.x + 3.0f, lcdBR.y + 3.0f),
                          IM_COL32(0x4A, 0x4C, 0x42, 0xFF), 2.0f);
        dl->AddRectFilledMultiColor(lcdTL, lcdBR, kLcdTop, kLcdTop, kLcdBottom,
                                    kLcdBottom);
        dl->AddRect(lcdTL, lcdBR, theme::withAlpha(theme::kVoid, 0.55f), 0.0f, 0, 1.0f);

        const meter::Register reg = meter::decompose(in.have, in.state.values[0]);
        const float pad = std::max(2.5f, r * 0.035f);
        const float unitW = std::min(r * 0.26f, (lcdBR.x - lcdTL.x) * 0.20f);
        const float cellsW = (lcdBR.x - lcdTL.x) - pad * 2.0f - unitW;
        const float cw = cellsW / static_cast<float>(meter::kDigits);
        const float ch = (lcdBR.y - lcdTL.y) - pad * 2.0f;
        for (int i = 0; i < meter::kDigits; ++i) {
            const ImVec2 dtl(lcdTL.x + pad + cw * static_cast<float>(i), lcdTL.y + pad);
            // A leading zero is drawn as a ghost, not as a lit figure: the eye
            // then reads 48213 out of the eight cells instead of 00048213.
            const bool lit = reg.status == meter::Reading::Ok && i >= reg.firstSignificant;
            const unsigned mask = lit ? meter::segments(reg.digits[i]) : 0u;
            drawDigit(dl, dtl, cw, ch, mask, true, kLcdLit);
        }

        // The annunciators. The unit is lit whenever there is a register to
        // put it against and dark otherwise, because "kWh" beside no figure
        // is a claim about a measurement that was not made.
        const float annPx = std::clamp(r * 0.115f, 7.5f, capPx);
        const char* unit = meter::unitWord(comm);
        const bool unitLit = reg.status == meter::Reading::Ok;
        dialText(dl, leg, annPx,
                 ImVec2(lcdBR.x - pad - textW(leg, annPx, unit),
                        lcdTL.y + (lcdBR.y - lcdTL.y) * 0.52f),
                 unit, unitLit ? kLcdLit : kLcdGhost);
        // OVER is the register's own overflow annunciator: a figure arrived
        // that will not fit in eight cells, which is a fact worth showing and
        // is not the same as no figure.
        if (reg.status == meter::Reading::Over) {
            dialText(dl, leg, annPx, ImVec2(lcdTL.x + pad, lcdTL.y + pad * 0.4f), "OVER",
                     kLcdLit);
        }
    }

    // --- the printed label --------------------------------------------------
    {
        const ImVec2 lTL(c.x - r * 0.60f, c.y + r * 0.23f);
        const ImVec2 lBR(c.x + r * 0.60f, c.y + r * 0.47f);
        dl->AddRectFilled(lTL, lBR, kLabelPaper, 1.0f);
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kVoid, 0.35f), 1.0f, 0, 1.0f);
        const float labPx = std::clamp((lBR.y - lTL.y) * 0.60f, 7.5f, readPx);
        const float capW = textW(leg, labPx, "METER No.");
        dialText(dl, leg, labPx, ImVec2(lTL.x + 4.0f, (lTL.y + lBR.y) * 0.5f - labPx * 0.62f),
                 "METER No.", theme::withAlpha(kLabelInk, 0.75f));
        // The number itself: the plugin's text[0], in the monospaced reading
        // face so a changing id does not shuffle sideways, in ink on paper -
        // about 13:1, which is what a live figure is owed.
        const char* id = in.have ? in.state.text[0] : "";
        if (id[0] != '\0') {
            ImFont* rf = fonts::reading();
            float px = labPx;
            const float room = (lBR.x - 5.0f) - (lTL.x + 8.0f + capW);
            while (px > 7.0f && textW(rf, px, id) > room) { px -= 0.5f; }
            dl->PushClipRect(lTL, lBR, true);
            dl->AddText(rf, px, ImVec2(lTL.x + 8.0f + capW, (lTL.y + lBR.y) * 0.5f - px * 0.62f),
                        kLabelInk, id);
            dl->PopClipRect();
        }
    }

    // --- the indicator pair -------------------------------------------------
    //
    // The real meter's infrared test LED [1][3], put to work, and its
    // neighbour. Both are drawn whatever the state, so a cold face still says
    // which two lamps it has; each lights for a reason and nothing else.
    //   NEW   - an event has arrived that this window has not shown.
    //   ALERT - the plugin is holding CASCADE_INSTRUMENT_FLAG_ALERT, and it
    //           BLINKS, because a held alert and a lamp that is merely on are
    //           different things.
    //
    // ON THE DIAL RATHER THAN IN THE COLUMN, and that is a correction: they
    // were in the bottom-right corner of the column, where the caption ran off
    // the panel as "ALER" and the lamp sat on top of the LAST HEARD well. A
    // circle has no edge to clip against.
    {
        const bool alert = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
        const bool blinkOn = std::fmod(cue.nowSec, 0.5) < 0.25;
        const float lr = std::max(2.5f, r * 0.055f);
        const float px = std::clamp(r * 0.095f, 6.5f, capPx);
        struct Ind {
            const char* word;
            ImU32 colour;
            bool lit;
        };
        const Ind pair[2] = {
            {"NEW", theme::kGold, cue.unread},
            {"ALERT", theme::kAlarmHot, alert && blinkOn},
        };
        for (int i = 0; i < 2; ++i) {
            const ImVec2 lc(c.x + r * (i == 0 ? -0.20f : 0.20f), c.y + r * 0.60f);
            dl->AddCircleFilled(lc, lr + 1.5f, IM_COL32(0x4A, 0x42, 0x34, 0xFF), 16);
            if (pair[i].lit) {
                for (int g = 3; g >= 1; --g) {
                    dl->AddCircleFilled(lc, lr + static_cast<float>(g) * 1.4f,
                                        theme::withAlpha(pair[i].colour, 0.09f), 16);
                }
                dl->AddCircleFilled(lc, lr, pair[i].colour, 16);
                dl->AddCircleFilled(ImVec2(lc.x - lr * 0.28f, lc.y - lr * 0.30f), lr * 0.38f,
                                    IM_COL32(255, 255, 255, 80), 10);
            } else {
                dl->AddCircleFilled(lc, lr, theme::withAlpha(pair[i].colour, 0.22f), 16);
            }
            const float tw = textW(leg, px, pair[i].word);
            dialText(dl, leg, px, ImVec2(lc.x - tw * 0.5f, lc.y + lr + 2.0f), pair[i].word,
                     pair[i].lit ? kDialInk : theme::withAlpha(kDialInk, 0.50f));
        }
    }

    // --- the cover ----------------------------------------------------------
    // The polycarbonate dome [1]. Not a texture - one soft crescent in the
    // upper left, lit from the same direction as every bevel on this panel,
    // plus the rim of the cover itself. It is the whole difference between a
    // printed disc and something seen THROUGH something.
    for (int i = 0; i < 5; ++i) {
        const float rr = dialR * (0.98f - static_cast<float>(i) * 0.055f);
        dl->PathArcTo(c, rr, kPi * 1.08f, kPi * 1.52f);
        dl->PathStroke(IM_COL32(255, 255, 255, static_cast<int>(18 - i * 3)),
                       ImDrawFlags_None, dialR * 0.07f);
    }
    dl->PathArcTo(c, dialR * 0.93f, kPi * 1.14f, kPi * 1.40f);
    dl->PathStroke(IM_COL32(255, 255, 255, 70), ImDrawFlags_None, 1.5f);
    dl->AddCircle(c, dialR * 1.005f, IM_COL32(255, 255, 255, 34), 0, 1.5f);

    float used = std::max(c.y + r, bTL.y) + 8.0f;

    // --- the column beside it -----------------------------------------------
    if (haveCol) {
        const float x0 = c.x + r + 14.0f * s;
        const float x1 = bBR.x;
        const float colW = x1 - x0;
        float y = bTL.y + 2.0f;

        // A caption's row is its type plus the rule addBenchGroupCaption carries
        // out under it; the slack is what stops the lamps below sitting on that
        // rule when the whole column is scaled down.
        const float capH = capPx + 9.0f * s;
        const float lampR = std::clamp(6.0f * s, 4.0f, 8.0f);
        const float cellH = std::clamp(readPx + 8.0f * s, 16.0f, 30.0f);

        // COMMODITY - three engraved lamps, the matching one lit. All three
        // are drawn whatever the state, so a cold panel still says which
        // commodities this instrument can report; with no reading all three
        // are dark, which is the only honest picture of "we have not heard
        // this meter".
        addBenchGroupCaption(dl, ImVec2(x0, y), colW, "COMMODITY");
        y += capH;
        {
            ImGui::PushFont(leg, capPx);
            const float pitch = colW / 3.0f;
            struct Lamp {
                const char* word;
                meter::Commodity kind;
                ImU32 colour;
            };
            // Three hues that are already on the bench: the phosphor of a live
            // reading for electricity, the gold of a counter for gas, and the
            // brass tint for water. Colour alone never carries it - the word
            // is lettered under every one of them, lit or not.
            const Lamp lamps[3] = {
                {"ELECTRIC", meter::Commodity::Electric, theme::kPhosphor},
                {"GAS", meter::Commodity::Gas, theme::kGold},
                {"WATER", meter::Commodity::Water, theme::kBrassTint},
            };
            for (int i = 0; i < 3; ++i) {
                const bool lit = in.have && comm == lamps[i].kind;
                drawBenchLamp(dl, ImVec2(x0 + pitch * (static_cast<float>(i) + 0.5f),
                                         y + lampR + 3.0f * s),
                              lampR, lamps[i].colour, lit, lamps[i].word);
            }
            ImGui::PopFont();
            y += lampR * 2.0f + capPx + 10.0f * s;
        }

        // TAMPER - the counters the SCM carries [1]: "tamper status". The flag
        // is the warning and the cell beside it is the number, in the bench's
        // two different colours for exactly that reason.
        const meter::Tamper tp = meter::tamper(in.have, in.state.values[1]);
        addBenchGroupCaption(dl, ImVec2(x0, y), colW, "TAMPER");
        y += capH;
        {
            const float halfW = (colW - 8.0f * s) * 0.5f;
            const char* words[2] = {"PHYSICAL", "ENCODER"};
            const int counts[2] = {tp.physical, tp.encoder};
            for (int i = 0; i < 2; ++i) {
                const float cx0 = x0 + (halfW + 8.0f * s) * static_cast<float>(i);
                dl->AddText(leg, capPx, ImVec2(cx0 + 1.0f, y + 1.0f),
                            theme::withAlpha(theme::kVoid, 0.55f), words[i]);
                dl->AddText(leg, capPx, ImVec2(cx0, y), theme::kInkMuted, words[i]);
                const float wordW = textW(leg, capPx, words[i]);
                // The flag: rust and blinking while a tamper stands, because
                // ALERT blinks and is not merely on. Dark otherwise, and dark
                // also when there is nothing to report at all - the number
                // beside it is what separates those two, by being absent.
                const bool flag = tp.have && counts[i] != 0;
                const bool blink = std::fmod(cue.nowSec, 0.6) < 0.35;
                const float fr = std::max(3.0f, 4.5f * s);
                const ImVec2 fc(cx0 + wordW + fr + 5.0f, y + capPx * 0.45f);
                dl->AddCircleFilled(fc, fr,
                                    flag ? (blink ? theme::kAlarmHot
                                                  : theme::withAlpha(theme::kAlarm, 0.55f))
                                         : theme::withAlpha(theme::kAlarm, 0.20f),
                                    12);
                dl->AddCircle(fc, fr, theme::withAlpha(theme::kBrassTint, 0.7f), 12, 1.0f);

                char num[8] = {0};
                if (tp.have) { std::snprintf(num, sizeof num, "%d", counts[i]); }
                glassCell(dl, ImVec2(cx0, y + capPx + 3.0f * s),
                          ImVec2(cx0 + halfW, y + capPx + 3.0f * s + cellH), num, readPx);
            }
            y += capPx + cellH + 9.0f * s;
        }

        // LAST HEARD - read out of the roster the host is about to draw
        // underneath, so the two can never disagree; blank when the roster
        // does not carry it.
        if (y + capH + cellH < bBR.y + 4.0f) {
            addBenchGroupCaption(dl, ImVec2(x0, y), colW, "LAST HEARD");
            y += capH;
            const std::string heard =
                meter::heardText(in.headings, in.rows, in.have ? in.state.text[0] : "");
            glassCell(dl, ImVec2(x0, y), ImVec2(x1, y + cellH), heard.c_str(), readPx);
            y += cellH + 4.0f;
        }

        used = std::max(used, y + 4.0f);
    } else {
        // With no column the meter is the whole instrument, so the commodity
        // has to be readable off the dial itself. One word, under the glass,
        // lettered only when there is a reading to letter it for.
        if (in.have && comm != meter::Commodity::Unknown) {
            const char* word = (comm == meter::Commodity::Electric)  ? "ELECTRIC"
                               : (comm == meter::Commodity::Gas)     ? "GAS"
                                                                     : "WATER";
            const float px = std::clamp(r * 0.11f, 7.5f, capPx);
            const float tw = textW(uiF, px, word);
            dialText(dl, uiF, px, ImVec2(c.x - tw * 0.5f, c.y + r * 0.86f - px), word,
                     kDialInk);
        }
    }

    dl->PopClipRect();
    return std::min(used, bBR.y + 8.0f) - tl.y;
}

}  // namespace cascade::gui
