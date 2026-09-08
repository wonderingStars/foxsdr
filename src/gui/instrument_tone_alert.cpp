// instrument_tone_alert.cpp - the CASCADE_INSTRUMENT_TONE_ALERT face: the
// two-tone alerting receiver that sits on a fire station wall.
//
// THE EQUIPMENT. A Plectron desktop monitor receiver, and its successor on the
// same shelf, a Motorola Minitor in its amplified charging base. One channel,
// squelched shut, listening to a dispatch output all day and doing nothing at
// all until the dispatcher sends the two tones the station's reeds are cut
// for - about a second of tone A, about three of tone B - whereupon the lamp
// comes up, the speaker opens and the crew hears the call. Plectron built them
// from the late 1950s to the late 1990s in Overton, Nebraska; hundreds of
// thousands went into the homes of volunteer firefighters across North
// America, and the two-tone sequential signalling they answer to is still on
// the air today.
//
// WHAT I LOOKED AT (design reference only; every line of drawing below is
// written here):
//   - en.wikipedia.org/wiki/Plectron - what the equipment is and its dates.
//   - scannerschool.com/plectrons-and-early-fire-alerting - the reeds, the
//     tuning, and how a station was alerted with one.
//   - motorolasolutions.com Minitor V / VI product pages, and the Minitor V
//     user guide (handbook.mteriefire.com) - the red alert lamp, the
//     charger-amplifier the pager sits in, the "SELECTIVE CALL ALERT MONITOR
//     RECEIVER" wording on its own front.
//   - Midian Electronics "Motorola Two Tone & Four Tone Paging" signalling
//     charts (sigidwiki mirror) - the tone tables the frequency scale on this
//     face is drawn to, and the 1 s / 3 s timing the duration cells expect.
//     The plugin feeding this face transcribes the same charts.
//
// WHAT THE REAL THING SHOWS, AND WHAT THIS ADDS. A Plectron's whole display is
// one lamp: it has no idea what frequencies it heard, because the reeds either
// resonate or they do not. This face keeps the lamp as the thing you see from
// across the room and then shows the measurement the decoder in front of it
// actually makes - the two tone frequencies, how long each ran, and which
// published table (if any) names the pair. That is the honest translation of a
// reed receiver onto a bench that measures: the equipment's face, plus the
// figures the equipment never had.
//
// THE THREE RULES (instrument_face.hpp) as they land here:
//   - the captions are cut into the plate; every measured figure is on glass.
//   - no reading is drawn as NO READING: with `have` false the lamp is dark,
//     the bars carry no index at all, every cell reads "--" and the glass says
//     NO PAGE. Not one zero is printed anywhere on this face, and a tone bar
//     with no tone shows an empty well rather than an index parked at the
//     bottom stop - an index at the bottom would be a measurement of 250 Hz.
//   - the drawing is scaled to the rectangle it is given, and when the
//     rectangle is too small for three bays it gives up the bays, not the
//     edges: the lamps alone still say whether the station is being called.
//
// The arithmetic is in instrument_tone_alert_math.hpp so it can be tested;
// this file is the drawing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/instrument_tone_alert_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

namespace ta = cascade::gui::tone_alert;

namespace {

// A caption cut into the panel: the legend face, the muted ink, and the dark
// pass under it that is the cut itself. Returns the width it used, so a caller
// can put a rule or a unit after it.
float cut(ImDrawList* dl, const ImVec2& at, float px, const char* s,
          ImU32 ink = theme::kInkMuted) {
    if (s == nullptr || s[0] == '\0') { return 0.0f; }
    ImFont* f = fonts::legend();
    dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                theme::withAlpha(theme::kVoid, 0.65f), s);
    dl->AddText(f, px, at, ink, s);
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// The same, centred on `cx` and shrunk to fit `maxW` rather than spilling onto
// the bay beside it.
void cutCentred(ImDrawList* dl, float cx, float y, float px, const char* s, float maxW,
                ImU32 ink = theme::kInkMuted) {
    if (s == nullptr || s[0] == '\0') { return; }
    ImFont* f = fonts::legend();
    const float w0 = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
    const float fitted = ta::fitPx(px, w0, maxW);
    const float w = f->CalcTextSizeA(fitted, FLT_MAX, 0.0f, s).x;
    cut(dl, ImVec2(cx - w * 0.5f, y), fitted, s, ink);
}

// A live figure on glass: the monospaced reading face, so a frequency stepping
// through its digits does not shuffle sideways. Centred and fitted like the
// captions. Returns the height it used.
float figureCentred(ImDrawList* dl, float cx, float y, float px, const char* s,
                    float maxW, ImU32 col) {
    if (s == nullptr || s[0] == '\0') { return 0.0f; }
    ImFont* f = fonts::reading();
    const float w0 = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
    const float fitted = ta::fitPx(px, w0, maxW);
    const ImVec2 sz = f->CalcTextSizeA(fitted, FLT_MAX, 0.0f, s);
    dl->AddText(f, fitted, ImVec2(cx - sz.x * 0.5f, y), col, s);
    return sz.y;
}

// One tone bar: the well, the graduations, and the index at the measured
// frequency. `hz` is the slot straight out of the plugin, so the decision
// about whether there IS a tone is made here, once, from the contract's own
// rule rather than by the caller remembering to check.
//
// WITH NO TONE THE WELL IS EMPTY. Not an index at the bottom stop, which is
// what "parked" would mean and what a reader would take for 250 Hz.
void drawToneBar(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, double hz) {
    if (dl == nullptr || br.x - tl.x < 6.0f || br.y - tl.y < 16.0f) { return; }
    drawFreqDrumWell(dl, tl, br);

    const float x0 = tl.x + 3.0f;
    const float x1 = br.x - 3.0f;
    const float y0 = tl.y + 4.0f;
    const float y1 = br.y - 4.0f;
    if (x1 <= x0 || y1 <= y0) { return; }

    // The graduations INSIDE the well, one per minor tick on the engraved
    // scale beside it, so the eye can read a height off the bar without
    // crossing to the numbers.
    for (double f = ta::kScaleLoHz; f <= ta::kScaleHiHz + 0.5; f += ta::kMinorTickStepHz) {
        const float t = static_cast<float>(ta::toneFrac(f));
        const float y = y1 - t * (y1 - y0);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y),
                    theme::withAlpha(theme::kBrassTint, 0.14f), 1.0f);
    }

    if (!ta::haveTone(hz)) { return; }

    const float t = static_cast<float>(ta::toneFrac(hz));
    const float y = y1 - t * (y1 - y0);

    // The column below the index, dim: the bar half of "bar or needle". It
    // shows the height at a glance from across the room, which is the job the
    // real equipment's lamp did and the reason this is not only a hairline.
    if (y < y1) {
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1),
                          theme::withAlpha(theme::kPhosphor, 0.22f), 1.0f);
    }

    // The index itself, with its glow drawn as two grown rectangles - ImGui
    // has no blur, and this is how every other lit thing on this bench is lit.
    const float th = std::max(2.0f, (y1 - y0) * 0.012f + 1.6f);
    dl->AddRectFilled(ImVec2(x0 - 1.0f, y - th), ImVec2(x1 + 1.0f, y + th),
                      theme::withAlpha(theme::kPhosphor, 0.20f), 2.0f);
    dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x1, y + th * 0.5f),
                      theme::kPhosphor, 1.0f);

    // A tone off either end of the engraved scale is pinned, and the stop it
    // is pinned against is marked so the pin is never read as a measurement.
    if (ta::overRange(hz) || ta::underRange(hz)) {
        const float ay = ta::overRange(hz) ? y0 + 2.0f : y1 - 2.0f;
        const float dir = ta::overRange(hz) ? -1.0f : 1.0f;
        const float cx = (x0 + x1) * 0.5f;
        const float s = std::min(6.0f, (x1 - x0) * 0.35f);
        dl->AddTriangleFilled(ImVec2(cx, ay + dir * s), ImVec2(cx - s, ay),
                              ImVec2(cx + s, ay), theme::kAmber);
    }
}

// THE STATION LAMP: the one thing on a Plectron's front, and the only part of
// this face a crew ever looked at from the far side of the room.
//
// Drawn here rather than through drawBenchLamp because that control is a 6 px
// indicator on a rail - its bezel is a 1.5 px ring and its bloom three grown
// discs - and blown up to thirty pixels it reads as a flat plastic counter
// rather than as glass in a brass socket. Everything here is proportional to
// the radius: the socket, the lens rim, the bloom, the specular highlight.
//
// UNLIT IS DARK GLASS, NOT GREY. An unlit red lamp and an unlit amber one are
// different objects and a cold panel should still say which is which - the
// same reason drawBenchLamp keeps the hue in its own unlit state.
void drawStationLamp(ImDrawList* dl, const ImVec2& c, float r, ImU32 colour, bool lit,
                     const char* caption, float capPx) {
    if (dl == nullptr || r < 4.0f) { return; }

    // The socket: a shadow under it, the brass ring, and the dark inner lip
    // the lens is seated in.
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.10f), r * 1.22f,
                        theme::withAlpha(theme::kVoid, 0.55f), 44);
    dl->AddCircleFilled(c, r * 1.18f, theme::kBrassMid, 44);
    addBenchBevel(dl, ImVec2(c.x - r * 1.18f, c.y - r * 1.18f),
                  ImVec2(c.x + r * 1.18f, c.y + r * 1.18f), r * 1.18f, true);
    dl->AddCircleFilled(c, r * 1.06f, theme::kBrassDark, 44);

    if (lit) {
        // The bloom, as grown discs - ImGui has no blur, and this is how every
        // other lit thing on this bench is lit.
        for (int i = 4; i >= 1; --i) {
            dl->AddCircleFilled(c, r + static_cast<float>(i) * r * 0.13f,
                                (colour & 0x00FFFFFFu) |
                                    (static_cast<ImU32>(34 - i * 7) << IM_COL32_A_SHIFT),
                                44);
        }
        dl->AddCircleFilled(c, r, colour, 48);
        // The hot centre and the specular, both off-centre to the upper left
        // like every other light on this panel.
        dl->AddCircleFilled(ImVec2(c.x - r * 0.08f, c.y - r * 0.10f), r * 0.54f,
                            theme::withAlpha(theme::kIvory, 0.20f), 36);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.36f), r * 0.17f,
                            IM_COL32(255, 255, 255, 150), 24);
    } else {
        dl->AddCircleFilled(c, r, theme::kVoid, 48);
        dl->AddCircleFilled(c, r, theme::withAlpha(colour, 0.26f), 48);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.28f, c.y - r * 0.32f), r * 0.32f,
                            theme::withAlpha(theme::kIvory, 0.08f), 24);
    }
    dl->AddCircle(c, r * 1.10f, theme::withAlpha(theme::kBrassBright, 0.75f), 44, 1.0f);

    // The word, whatever the state: a lamp whose meaning is carried by colour
    // alone cannot be read in a photograph and is unreadable to about one man
    // in twelve.
    if (caption != nullptr && caption[0] != '\0') {
        ImFont* f = fonts::legend();
        const ImVec2 sz = f->CalcTextSizeA(capPx, FLT_MAX, 0.0f, caption);
        cut(dl, ImVec2(c.x - sz.x * 0.5f, c.y + r * 1.22f + 3.0f), capPx, caption,
            lit ? theme::kIvory : theme::kCream);
    }
}

// The engraved frequency scale cut between the two bars. Ticks both ways, the
// major ones numbered in the legend face - the panel's own kHz markings.
void drawToneScale(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, float px) {
    if (dl == nullptr || br.x - tl.x < 12.0f || br.y - tl.y < 16.0f) { return; }
    const float y0 = tl.y + 4.0f;
    const float y1 = br.y - 4.0f;
    const float cx = (tl.x + br.x) * 0.5f;
    const float halfW = (br.x - tl.x) * 0.5f;

    ImFont* f = fonts::legend();
    for (double hz = ta::kScaleLoHz; hz <= ta::kScaleHiHz + 0.5;
         hz += ta::kMinorTickStepHz) {
        const float y = y1 - static_cast<float>(ta::toneFrac(hz)) * (y1 - y0);
        bool major = false;
        for (int i = 0; i < ta::kMajorTickCount; ++i) {
            if (std::fabs(hz - ta::kMajorTickHz[i]) < 0.5) { major = true; }
        }
        const float len = major ? halfW * 0.34f : halfW * 0.20f;
        // Cut on both sides, so the scale reads as belonging to both bars
        // rather than to whichever one it sits nearer.
        addBenchRail(dl, tl.x + 1.0f, tl.x + 1.0f + len, y);
        addBenchRail(dl, br.x - 1.0f - len, br.x - 1.0f, y);
        if (!major) { continue; }
        char lbl[12];
        if (hz < 1000.0) {
            std::snprintf(lbl, sizeof lbl, "%d", static_cast<int>(hz + 0.5));
        } else if (std::fabs(hz - std::floor(hz / 1000.0) * 1000.0) < 0.5) {
            std::snprintf(lbl, sizeof lbl, "%dk", static_cast<int>(hz / 1000.0 + 0.5));
        } else {
            std::snprintf(lbl, sizeof lbl, "%.1fk", hz / 1000.0);
        }
        const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, lbl);
        cut(dl, ImVec2(cx - sz.x * 0.5f, y - sz.y * 0.5f), px, lbl, theme::kCream);
    }
    // The unit, once, at the head of the scale - the scale's own name, not a
    // repeat on every tick.
    const ImVec2 us = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Hz");
    cut(dl, ImVec2(cx - us.x * 0.5f, tl.y - us.y - 1.0f), px, "Hz");
}

}  // namespace

float drawToneAlertFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                        const cascade::core::HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    // RULE 3, at its limit: a rectangle this small cannot hold a plate, let
    // alone an instrument, so nothing is drawn and nothing is claimed. A
    // zero-size or inverted rectangle lands here too.
    if (!(w >= 90.0f) || !(h >= 64.0f)) { return 0.0f; }

    const CascadeInstrumentState& s = in.state;
    const bool have = in.have;
    const bool alert = have && (s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
    const bool ringing = alert && ta::alertBlink(cue.nowSec);

    // The case, titled by the plugin's own window name.
    float y = addBenchPlate(dl, tl, br, in.title.c_str());

    const float sc = ta::typeScale(w, h);
    const float capPx = std::max(ta::kMinTextPx, fonts::kTinySize * sc);
    const float readPx = std::max(ta::kMinTextPx + 2.0f, fonts::kReadingSize * sc);
    const float uiPx = std::max(ta::kMinTextPx, fonts::kUiSize * sc);

    // The maker's legend along the bottom of the case, and the event counter
    // beside it. Both are cut into the enamel; neither is a measurement.
    const float legendH = capPx + 6.0f;
    const float legendY = br.y - 5.0f - capPx;
    ImFont* lf = fonts::legend();
    float legendRoom = w - 24.0f;
    if (have) {
        // The event counter first, so the maker's legend beside it is fitted
        // to what is actually left rather than to the whole case.
        char ev[32];
        std::snprintf(ev, sizeof ev, "EVENT %u", static_cast<unsigned>(s.seq));
        const float ew = lf->CalcTextSizeA(capPx, FLT_MAX, 0.0f, ev).x;
        cut(dl, ImVec2(br.x - 12.0f - ew, legendY), capPx, ev, theme::kInkFaint);
        legendRoom -= ew + 12.0f;
    }
    // The maker's legend, cut into the case the way the equipment letters its
    // own front: what signalling it answers to, and what it is.
    {
        const char* mark = "TWO-TONE SEQUENTIAL - SELECTIVE CALL ALERT MONITOR";
        const float w0 = lf->CalcTextSizeA(capPx, FLT_MAX, 0.0f, mark).x;
        if (w0 > legendRoom) { mark = "TWO-TONE SEQUENTIAL"; }
        const float w1 = lf->CalcTextSizeA(capPx, FLT_MAX, 0.0f, mark).x;
        cut(dl, ImVec2(tl.x + 12.0f, legendY), ta::fitPx(capPx, w1, legendRoom), mark);
    }

    // The deck: everything between the plate's rule and the legend.
    const ImVec2 dTL(tl.x + 10.0f, y + 2.0f);
    const ImVec2 dBR(br.x - 10.0f, br.y - 6.0f - legendH);
    const float dw = dBR.x - dTL.x;
    const float dh = dBR.y - dTL.y;
    if (dw < 40.0f || dh < 30.0f) { return h; }

    const ta::Columns col = ta::columnsFor(dw, dh);

    // --- the alerting lamps ---------------------------------------------------
    //
    // The bay a fireman looks at, and the only one the real equipment had. It
    // is drawn first and, on a window too small for the rest, it is drawn
    // ALONE across the whole deck rather than being shrunk with the others:
    // "is the station being called" outranks every figure on this face.
    const float lampW = col.valid ? col.lampW : dw;
    const ImVec2 lTL = dTL;
    const ImVec2 lBR(dTL.x + lampW, dBR.y);
    addScopeBay(dl, lTL, lBR, true);

    {
        const float bw = lBR.x - lTL.x;
        const float bh = lBR.y - lTL.y;
        const float cx = (lTL.x + lBR.x) * 0.5f;
        // The two lamps and their engraved words are laid out as ONE block and
        // that block is centred in the bay, so the lamps sit where a hand
        // would put them rather than hard against the top rail with a hole
        // underneath.
        const float capBlock = capPx + 6.0f;
        const float gap = 8.0f;
        float R = std::min(bw * 0.32f, (bh - 16.0f - 2.0f * capBlock - gap) / 2.68f);
        bool withNew = R >= 9.0f;
        if (!withNew) {
            // Too short for both. The ALERT lamp is the one that must survive:
            // the NEW lamp says a page arrived while you were away, and the
            // rail's own chip says that too.
            R = std::min(bw * 0.34f, (bh - 10.0f - capBlock) * 0.5f);
        }
        R = std::max(5.0f, std::min(R, 36.0f));
        const float smallR = std::max(4.0f, R * 0.34f);
        const float blockH =
            withNew ? (2.0f * R + capBlock + gap + 2.0f * smallR + capBlock)
                    : (2.0f * R + capBlock);
        float top = lTL.y + (bh - blockH) * 0.5f;
        if (top < lTL.y + 4.0f) { top = lTL.y + 4.0f; }

        // THE ALERT LAMP BLINKS, it is not merely on: a steady red lamp on a
        // station wall is a fault indicator, a blinking one is a call.
        drawStationLamp(dl, ImVec2(cx, top + R), R, theme::kAlarmHot, ringing, "ALERT",
                        capPx);
        if (withNew) {
            const float ny = top + 2.0f * R + capBlock + gap + smallR;
            drawStationLamp(dl, ImVec2(cx, ny), smallR, theme::kGold, cue.unread, "NEW",
                            capPx);
        }
    }

    if (!col.valid) {
        // Too narrow for the measuring bays. The lamps above are the face.
        return h;
    }

    // --- the two tone bars ----------------------------------------------------
    const ImVec2 bTL(lBR.x + col.gap, dTL.y);
    const ImVec2 bBR(bTL.x + col.barsW, dBR.y);
    addScopeBay(dl, bTL, bBR, true);

    const ta::BarColumns bars = ta::barsFor(col.barsW);
    {
        const float capY = bTL.y + 4.0f;
        const float capH = capPx + 4.0f;
        // Two figure lines under each bar: the frequency and the duration,
        // each with its unit cut in beside the caption above it.
        const float figH = readPx + 2.0f;
        const float wellTop = capY + capH;
        const float wellBot = bBR.y - 6.0f - figH * 2.0f;

        if (bars.valid && wellBot > wellTop + 24.0f) {
            const float xA0 = bTL.x + bars.gap;
            const float xA1 = xA0 + bars.barW;
            const float xS0 = xA1;
            const float xS1 = xS0 + bars.scaleW;
            const float xB0 = xS1;
            const float xB1 = xB0 + bars.barW;

            cutCentred(dl, (xA0 + xA1) * 0.5f, capY, capPx, "TONE A", bars.barW);
            cutCentred(dl, (xB0 + xB1) * 0.5f, capY, capPx, "TONE B", bars.barW);

            // EVERY SLOT THROUGH slotOrNone, the bars included. Gating the
            // readouts on `have` and not the bars is exactly the fault the
            // first no-reading screenshot of this face caught: two dashes
            // under two indexed bars.
            drawToneBar(dl, ImVec2(xA0, wellTop), ImVec2(xA1, wellBot),
                        ta::slotOrNone(have, s.values[0]));
            drawToneBar(dl, ImVec2(xB0, wellTop), ImVec2(xB1, wellBot),
                        ta::slotOrNone(have, s.values[1]));
            drawToneScale(dl, ImVec2(xS0, wellTop), ImVec2(xS1, wellBot), capPx);

            // The figures, on glass under the bar each belongs to. An empty
            // slot prints the dash formatHz supplies and never a zero.
            for (int i = 0; i < 2; ++i) {
                const float cx = (i == 0 ? (xA0 + xA1) : (xB0 + xB1)) * 0.5f;
                const double hz = ta::slotOrNone(have, s.values[i]);
                const double sec = ta::slotOrNone(have, s.values[2 + i]);
                char buf[32];
                ta::formatHz(buf, sizeof buf, hz);
                figureCentred(dl, cx, wellBot + 3.0f, readPx, buf, bars.barW,
                              ta::haveTone(hz) ? theme::kPhosphor : theme::kCream);
                char sb[32];
                ta::formatSec(sb, sizeof sb, sec);
                char line[40];
                std::snprintf(line, sizeof line, "%s s", sb);
                figureCentred(dl, cx, wellBot + 3.0f + figH, readPx * 0.82f, line,
                              bars.barW,
                              ta::haveDuration(sec) ? theme::kAmber : theme::kCream);
            }
        }
    }

    // --- the glass plate ------------------------------------------------------
    //
    // What the dispatcher actually sent, as far as any published table can
    // name it. The real equipment has nothing like this; the decoder in front
    // of it does, and this is where its answer goes.
    const ImVec2 gTL(bBR.x + col.gap, dTL.y);
    const ImVec2 gBR(gTL.x + col.glassW, dBR.y);
    drawFreqDrumWell(dl, gTL, gBR);
    {
        const float pad = 8.0f;
        const float x0 = gTL.x + pad;
        const float maxW = (gBR.x - pad) - x0;
        float gy = gTL.y + 6.0f;
        const ta::Result res = have ? ta::resultOf(s.text[2]) : ta::Result::kNone;

        if (!have) {
            // RULE 2. No page has been received, so the glass says so - one
            // flag, no cells, no zeroes, no invented code.
            cut(dl, ImVec2(x0, gy + (gBR.y - gTL.y) * 0.42f), uiPx, "NO PAGE",
                theme::kInkMuted);
        } else {
            cut(dl, ImVec2(x0, gy), capPx, "CODE");
            gy += capPx + 2.0f;
            if (s.text[0][0] != '\0') {
                ImFont* rf = fonts::reading();
                // THE CODE IS THE HERO FIGURE on this face - it is what a
                // watch officer reads and what the station is called by - so
                // it is set as large as the glass will carry, bounded by the
                // glass's own height rather than by the ambient reading size.
                const float codePx = std::min(readPx * 2.10f, (gBR.y - gTL.y) * 0.26f);
                const float w0 = rf->CalcTextSizeA(codePx, FLT_MAX, 0.0f, s.text[0]).x;
                const float px = ta::fitPx(codePx, w0, maxW);
                dl->AddText(rf, px, ImVec2(x0, gy), theme::kPhosphor, s.text[0]);
                gy += rf->CalcTextSizeA(px, FLT_MAX, 0.0f, s.text[0]).y + 4.0f;
            } else {
                // A page with no code is what an unmatched pair looks like:
                // the cell is left blank rather than filled with a guess.
                gy += readPx + 4.0f;
            }

            cut(dl, ImVec2(x0, gy), capPx, "FORMAT");
            gy += capPx + 2.0f;
            if (s.text[1][0] != '\0') {
                // Words, so the UI face - and wrapped inside the glass rather
                // than run out over the bevel. "Motorola Quick Call II group
                // 1" is longer than any bay this face has.
                ImFont* uf = fonts::ui();
                dl->AddText(uf, uiPx * 0.92f, ImVec2(x0, gy), theme::kPhosphor, s.text[1],
                            nullptr, maxW);
                const ImVec2 tsz =
                    uf->CalcTextSizeA(uiPx * 0.92f, FLT_MAX, maxW, s.text[1]);
                gy += tsz.y + 4.0f;
            } else {
                gy += uiPx + 4.0f;
            }

            // The result tab: phosphor for a pair a published table names,
            // amber for one it does not, the word itself for anything else the
            // plugin chose to say. It is a tab rather than a lamp because it
            // carries its meaning in a word - a colour alone is unreadable to
            // about one man in twelve.
            //
            // ANCHORED TO THE FOOT OF THE GLASS, not stacked under whatever
            // came before it: the verdict is the last thing read and it sits
            // in the same place on every page, so the eye does not have to
            // hunt for it up a column whose length depends on how long the
            // format's name happened to be.
            if (res != ta::Result::kNone) {
                const char* word = s.text[2];
                ImU32 ink = theme::kCream;
                if (res == ta::Result::kMatched) { ink = theme::kPhosphor; }
                if (res == ta::Result::kUnmatched) { ink = theme::kAmber; }
                ImFont* tabFont = fonts::legend();
                const float w0 = tabFont->CalcTextSizeA(capPx, FLT_MAX, 0.0f, word).x;
                const float px = ta::fitPx(capPx, w0, maxW - 12.0f);
                const ImVec2 tsz = tabFont->CalcTextSizeA(px, FLT_MAX, 0.0f, word);
                float ty = gBR.y - 8.0f - tsz.y;
                // ...unless the glass is so short that the foot would collide
                // with the format line, in which case it follows on instead.
                if (ty < gy) { ty = gy; }
                const ImVec2 rTL(x0 - 3.0f, ty - 2.0f);
                const ImVec2 rBR(x0 + tsz.x + 7.0f, ty + tsz.y + 3.0f);
                if (rBR.y < gBR.y && rBR.x < gBR.x) {
                    if (ty - capPx - 3.0f > gy) {
                        cut(dl, ImVec2(x0, ty - capPx - 4.0f), capPx, "RESULT");
                    }
                    dl->AddRectFilled(rTL, rBR, theme::withAlpha(ink, 0.14f), 3.0f);
                    dl->AddRect(rTL, rBR, theme::withAlpha(ink, 0.55f), 3.0f, 0, 1.0f);
                    dl->AddText(tabFont, px, ImVec2(x0, ty), ink, word);
                }
            }
        }
    }

    // The two screws in the case's top corners, driven home at different
    // angles the way real ones are. Drawn last so they sit on the plate rather
    // than under it.
    const float sr = std::max(2.5f, 4.0f * sc);
    if (w > 120.0f) {
        addCabinetScrew(dl, ImVec2(tl.x + sr + 5.0f, tl.y + sr + 5.0f), sr, 18.0f);
        addCabinetScrew(dl, ImVec2(br.x - sr - 5.0f, tl.y + sr + 5.0f), sr, -34.0f);
    }

    return h;
}

}  // namespace cascade::gui
