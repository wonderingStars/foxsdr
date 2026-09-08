// instrument_pager.cpp - the CASCADE_INSTRUMENT_PAGER face.
//
// THE EQUIPMENT: a 1990s alphanumeric pager of the Motorola Advisor family -
// the object a FLEX or POCSAG decoder is a software copy of. A small dark
// plastic brick, 81 x 55 x 18.5 mm on one AA cell, with a backlit four-line
// dot-matrix liquid-crystal display of twenty characters a line, eighty to a
// screen; an envelope symbol and a message count on the glass; the time along
// the top; and a lamp that flashes for a page that has arrived and not been
// read.
//
// WHERE THOSE FIGURES COME FROM. Every proportion and every legend below was
// taken from Motorola's own documents rather than from memory:
//   - ADVISOR II Flex Alphanumeric Pager specification sheet (Motorola
//     Solutions), for "Displays up to 4 Lines of Text", "52 Message Slots for
//     Storing Multiple Pages", the four capcodes, the message time and date
//     stamp, the graphic battery gauge, the 929-932 MHz band and
//     "Dimension L x W x H: 81 x 55 x 18.5mm":
//     https://www.motorolasolutions.com/content/dam/msi/docs/business/products/pagers/advisor_ii/_documents/static_files/advisor_ii_flex_spec_sheet.pdf
//   - Motorola Advisor Pro User's Guide, for what the screen actually shows -
//     the incoming-message symbol, "A flashing symbol indicates a message has
//     been received, but has not yet been read", the flashing arrow in the
//     bottom right corner for a message that continues past the display, the
//     back light, and the padlock on a locked message:
//     https://ia801805.us.archive.org/8/items/manualzz-id-932564/932564.pdf
//   - IEEE Spectrum, Consumer Electronics Hall of Fame: Motorola Advisor, for
//     the 1990 original, "up to four lines of text with up to 20 characters
//     per line", the dimensions and the single AA cell:
//     https://spectrum.ieee.org/the-consumer-electronics-hall-of-fame-motorola-advisor-pager
// The drawing is written here from those descriptions; nothing was copied.
//
// NO MAKER'S NAME IS LETTERED ON THE CASE. The reference is a trademarked
// product and this is not one, so the moulded plate carries the bench's own
// name and the model line says what the thing is. Naming Motorola on a face
// Motorola did not make would be the one kind of lie this file is otherwise
// written to avoid.
//
// AND NO BUTTONS. A real Advisor has READ/RESET, FUNCTION/SELECT and a pair of
// arrows under the screen, and every one of them would be a picture of a
// control here - there is nothing for them to do, and instrument_face.hpp's
// third rule says a control that invites a hand and then refuses it is worse
// than no control. So the lower half of the case is a moulded plate and an
// acoustic grille, which are parts of the object rather than affordances.
//
// TWO MATERIALS THAT ARE NOT THE BENCH'S, and both are deliberate. The case is
// a cool near-neutral grey plastic and the display is a warm yellow-green
// liquid crystal with dark segments - a consumer object of its decade, which
// is what the equipment IS, set into the brass cradle and engraved captions
// the rest of the bench is made of. A phosphor-green readout in the enamel
// well every other face uses would have been easier and would have drawn a
// small radar screen instead of a pager. The colours live in this file rather
// than in theme.hpp because exactly one object in the product is made of them;
// a second liquid-crystal face is when they move.
//
// SLOT MAP (plugin_abi.h, CASCADE_INSTRUMENT_PAGER):
//   text[0] latest message   -> the LCD's character rows, wrapped at 20
//   text[1] capcode          -> the LCD's status row, and the CAPCODE drums
//   text[2] time (HH:MM)     -> the LCD's top strip, and RECEIVED
//   text[3] kind             -> the LCD's status row, and TYPE
//   values[0] unread count   -> the envelope's counter, and the UNREAD drums
//   flags ALERT -> the lamp flashes; LOCK -> SYNC; LOW_BATT -> BATT
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/instrument_pager_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;

namespace {

// --- the two materials this face owns ---------------------------------------
//
// The mouldings. Cool where the bench is warm, because the object is.
constexpr ImU32 kCaseHi = IM_COL32(0x3C, 0x3C, 0x40, 0xFF);
constexpr ImU32 kCaseLo = IM_COL32(0x20, 0x20, 0x24, 0xFF);
constexpr ImU32 kCaseEdge = IM_COL32(0x55, 0x55, 0x5A, 0xFF);
constexpr ImU32 kCaseCut = IM_COL32(0x14, 0x14, 0x17, 0xFF);
constexpr ImU32 kCasePlate = IM_COL32(0x8A, 0x8A, 0x90, 0xFF);

// The liquid crystal. Lit is the electroluminescent back light on; unlit is the
// same glass in ambient light; dead is an unaddressed panel, which is what a
// pager that has heard nothing shows and is NOT a screen full of zeros.
constexpr ImU32 kLcdLit = IM_COL32(0xA6, 0xB8, 0x76, 0xFF);
constexpr ImU32 kLcdIdle = IM_COL32(0x86, 0x96, 0x6C, 0xFF);
constexpr ImU32 kLcdDead = IM_COL32(0x5C, 0x64, 0x54, 0xFF);
constexpr ImU32 kLcdSeg = IM_COL32(0x18, 0x20, 0x14, 0xFF);
constexpr ImU32 kLcdSegSoft = IM_COL32(0x18, 0x20, 0x14, 0x66);

// --- small lettering helpers ------------------------------------------------

// A caption cut into the metal, in the legend face. Returns its width.
float engrave(ImDrawList* dl, const ImVec2& at, const char* s, ImU32 ink = theme::kInkMuted,
              float px = fonts::kTinySize) {
    ImFont* f = fonts::legend();
    dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y + 1.0f),
                theme::withAlpha(theme::kVoid, 0.55f), s);
    dl->AddText(f, px, at, ink, s);
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// A live word on glass, in the ui face, clipped to `maxW`.
void onGlass(ImDrawList* dl, const ImVec2& at, const char* s, float maxW,
             float px = fonts::kUiSize, ImU32 ink = theme::kPhosphor) {
    dl->AddText(fonts::ui(), px, at, ink, s, nullptr, maxW);
}

// --- the pager's own parts --------------------------------------------------

// The moulded shell: a shadow, a two-stop plastic gradient, a rim and the
// bevel every other object on this bench wears, so it is lit from the same
// upper left as the brass it stands in.
void drawCaseShell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, float round) {
    dl->AddRectFilled(ImVec2(tl.x + 3.0f, tl.y + 4.0f), ImVec2(br.x + 3.0f, br.y + 4.0f),
                      theme::withAlpha(theme::kVoid, 0.45f), round);
    dl->AddRectFilled(tl, br, kCaseHi, round);
    if (br.x - tl.x > round * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + round, tl.y), ImVec2(br.x - round, br.y),
                                    kCaseHi, kCaseHi, kCaseLo, kCaseLo);
    }
    dl->AddRect(tl, br, kCaseEdge, round, 0, theme::kHairline);
    addBenchBevel(dl, tl, br, round, true);
}

// The acoustic grille: the slots the alert sounds through. A moulding, not a
// control - see the header.
void drawGrille(ImDrawList* dl, const ImVec2& tl, float w, float h, int slots) {
    if (slots < 1 || w < 6.0f || h < 3.0f) { return; }
    const float pitch = w / static_cast<float>(slots);
    const float bar = std::max(1.0f, pitch * 0.45f);
    for (int i = 0; i < slots; ++i) {
        const float x = tl.x + static_cast<float>(i) * pitch;
        dl->AddRectFilled(ImVec2(x, tl.y), ImVec2(x + bar, tl.y + h), kCaseCut, 1.0f);
        dl->AddLine(ImVec2(x + bar, tl.y), ImVec2(x + bar, tl.y + h),
                    theme::withAlpha(theme::kBrassTint, 0.18f), theme::kHairline);
    }
}

// The envelope on the glass - the incoming-message symbol the manual names.
// Drawn in the display's own segments, so it is part of the picture the LCD is
// making rather than an icon laid over it.
void drawEnvelope(ImDrawList* dl, const ImVec2& tl, float w, float h, ImU32 ink) {
    if (w < 5.0f || h < 4.0f) { return; }
    const ImVec2 br(tl.x + w, tl.y + h);
    dl->AddRect(tl, br, ink, 0.0f, 0, std::max(1.0f, h * 0.10f));
    dl->AddLine(tl, ImVec2(tl.x + w * 0.5f, tl.y + h * 0.62f), ink,
                std::max(1.0f, h * 0.10f));
    dl->AddLine(ImVec2(br.x, tl.y), ImVec2(tl.x + w * 0.5f, tl.y + h * 0.62f), ink,
                std::max(1.0f, h * 0.10f));
}

// One drum row: `cells` apertures in a well, fed a string of characters. A
// blank cell is drawn dark and empty, which is how the counter says it has not
// counted rather than counted nothing.
float drawDrumRow(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                  const char* cells, int count) {
    const float w = cellW * static_cast<float>(count);
    drawFreqDrumWell(dl, ImVec2(tl.x - 3.0f, tl.y - 3.0f),
                     ImVec2(tl.x + w + 3.0f, tl.y + cellH + 3.0f));
    for (int i = 0; i < count; ++i) {
        const ImVec2 ctl(tl.x + static_cast<float>(i) * cellW, tl.y);
        const ImVec2 cbr(ctl.x + cellW - 1.0f, tl.y + cellH);
        const char c = cells[i];
        drawFreqDrumCell(dl, ctl, cbr, c == ' ' ? ' ' : c, c != ' ', cellH * 0.72f);
    }
    return w;
}

}  // namespace

float drawPagerFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float hAll = br.y - tl.y;
    // A rectangle too small to hold the equipment gets nothing drawn in it
    // rather than a compressed drawing over its edges.
    if (!(w > 0.0f) || !(hAll > 0.0f) || w < 120.0f || hAll < 90.0f) { return 0.0f; }

    const float plateY = addBenchPlate(dl, tl, br, in.title.c_str());

    // THE CRADLE. The pager is a separate object standing in the bench, so the
    // bench gives it a machined bay to stand in - and the bay is what stops at
    // the bottom of the face, with the lower part of the case inside it.
    const ImVec2 bayTL(tl.x + 10.0f, plateY + 4.0f);
    // Capped, so a window given the whole height does not stretch one pager to
    // fill it; the face returns the smaller number and the caller keeps the
    // rest.
    const float bayH = std::min(br.y - 10.0f - bayTL.y, 430.0f);
    const ImVec2 bayBR(br.x - 10.0f, bayTL.y + bayH);
    if (bayH < 60.0f || bayBR.x - bayTL.x < 100.0f) { return plateY - tl.y; }
    addScopeBay(dl, bayTL, bayBR, true);
    dl->PushClipRect(bayTL, bayBR, true);

    const bool have = in.have;
    const bool alert = have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
    const bool lock = have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u;
    const bool lowBatt = have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOW_BATT) != 0u;
    // ALERT BLINKS; it is not merely on. The manual's own word for an unread
    // page is "flashing", and this is the same 2 Hz the rest of the bench uses.
    const bool blink = std::fmod(cue.nowSec, 0.5) < 0.25;
    const bool ringing = alert || cue.unread;

    // --- the case ----------------------------------------------------------
    const float bayW = bayBR.x - bayTL.x;
    // THE BENCH'S COLUMN IS THE PART THAT GIVES WAY. Below about 150 px it
    // cannot letter a lamp caption without the words running into each other,
    // so at that point it is dropped and the equipment takes the whole bay -
    // which is the third rule working as intended: less is drawn, smaller,
    // rather than the same drawing pushed over the edge.
    constexpr float kColumnMin = 150.0f;
    const float wantW = std::clamp(bayW * 0.46f, 120.0f, 560.0f);
    const bool haveColumn = bayW - wantW - 28.0f >= kColumnMin;
    const float caseW = haveColumn ? wantW : std::max(120.0f, bayW - 24.0f);
    const ImVec2 caseTL(bayTL.x + 12.0f, bayTL.y + 8.0f);
    // The body at its true proportions, which is TALLER than the bay: the last
    // third of the pager is down inside the cradle, exactly as it would be.
    const pager::CaseSize cs = pager::caseSize(caseW, caseW / (55.0f / 81.0f));
    const ImVec2 caseBR(caseTL.x + cs.w, caseTL.y + cs.h);
    const float caseRound = cs.w * 0.075f;
    drawCaseShell(dl, caseTL, caseBR, caseRound);

    // The shoulder: the new-message lamp on the left, the alert grille right.
    // EVERY MOULDING IS CAPPED BY THE BAY'S HEIGHT AS WELL AS THE CASE'S
    // WIDTH. A short wide bay makes the case wide, and mouldings sized from
    // the width alone then eat the whole of it: the first version of this
    // widened the case when the bench's column would not fit and pushed its
    // own display below the minimum in the same move, so a small window showed
    // a pager with a dead screen. Height is the scarce dimension here; these
    // give way to it.
    const float sideInset = std::clamp(cs.w * 0.075f, 6.0f, 16.0f);
    const float shoulderH = std::clamp(cs.w * 0.11f, 12.0f, bayH * 0.17f);
    const float lampR = std::max(3.0f, shoulderH * 0.22f);
    const ImVec2 lampC(caseTL.x + sideInset + lampR, caseTL.y + shoulderH * 0.55f);
    drawBenchLamp(dl, lampC, lampR, theme::kAlarmHot, ringing && blink, nullptr);
    engrave(dl, ImVec2(lampC.x + lampR + 5.0f, lampC.y - fonts::kTinySize * 0.52f), "MSG",
            theme::kCream, std::min(fonts::kTinySize, shoulderH * 0.72f));
    {
        const float gw = std::min(cs.w * 0.30f, 60.0f);
        drawGrille(dl, ImVec2(caseBR.x - sideInset - gw, caseTL.y + shoulderH * 0.28f), gw,
                   shoulderH * 0.52f, 6);
    }

    // --- the display -------------------------------------------------------
    //
    // The glass is sized from the CHARACTER CELL, so twenty columns are twenty
    // identical cells whatever the window is doing.
    const float bezelPad = std::clamp(cs.w * 0.028f, 3.0f, 8.0f);
    const float lcdMaxW = cs.w - sideInset * 2.0f - bezelPad * 2.0f;
    const float lcdTop = caseTL.y + shoulderH;
    // What is left for the display once the moulded plate under it is kept
    // clear, measured against the BAY: the case runs past the bay's bottom and
    // the display may not.
    const float plateRoom = std::clamp(cs.w * 0.16f, 16.0f, bayH * 0.24f);
    const float lcdMaxH = (bayBR.y - 8.0f) - lcdTop - bezelPad * 2.0f - plateRoom;
    constexpr float kStripRows = 1.25f;  // the icon strip, in character rows
    const pager::LcdMetrics m =
        pager::lcdMetrics(lcdMaxW, lcdMaxH, pager::kLcdRows, pager::kLcdCols, kStripRows);

    float lcdBottom = lcdTop;
    if (m.fits) {
        const ImVec2 glassTL(caseTL.x + (cs.w - m.glassW) * 0.5f, lcdTop + bezelPad);
        const ImVec2 glassBR(glassTL.x + m.glassW, glassTL.y + m.glassH);
        // The bezel the panel is set into: a brass surround, because the bench
        // holds the glass even though the case is plastic.
        const ImVec2 bezTL(glassTL.x - bezelPad, glassTL.y - bezelPad);
        const ImVec2 bezBR(glassBR.x + bezelPad, glassBR.y + bezelPad);
        dl->AddRectFilled(bezTL, bezBR, theme::kBrassDark, 2.0f);
        addBenchBevel(dl, bezTL, bezBR, 2.0f, false);

        // The panel itself. Lit while something is ringing - the back light on
        // an Advisor comes up with the page - idle otherwise, and DEAD when
        // the plugin has given us nothing at all.
        const ImU32 ground = !have ? kLcdDead : (ringing ? kLcdLit : kLcdIdle);
        dl->AddRectFilled(glassTL, glassBR, ground, 0.0f);
        // A liquid-crystal panel is darker towards the bottom of its viewing
        // cone; two flat stops are enough to stop it reading as paper.
        dl->AddRectFilledMultiColor(glassTL, glassBR, IM_COL32(255, 255, 255, 16),
                                    IM_COL32(255, 255, 255, 10), IM_COL32(0, 0, 0, 26),
                                    IM_COL32(0, 0, 0, 30));

        // The pixel grid. This is what separates a liquid-crystal panel from a
        // box with text in it: the same faint lattice runs across the whole
        // glass whether a character is lit there or not.
        const float dotW = m.cellW / 6.0f;
        const float dotH = m.cellH / 8.0f;
        if (dotW >= 1.6f) {
            const ImU32 grid = IM_COL32(0, 0, 0, 20);
            for (float x = glassTL.x + dotW; x < glassBR.x - 0.5f; x += dotW) {
                dl->AddLine(ImVec2(x, glassTL.y), ImVec2(x, glassBR.y), grid, 1.0f);
            }
            for (float y = glassTL.y + dotH; y < glassBR.y - 0.5f; y += dotH) {
                dl->AddLine(ImVec2(glassTL.x, y), ImVec2(glassBR.x, y), grid, 1.0f);
            }
        }

        dl->PushClipRect(glassTL, glassBR, true);
        const float stripH = m.cellH * kStripRows;
        if (have) {
            // The icon strip: the envelope and its count on the left, the time
            // on the right. The envelope FLASHES for an unread page, which is
            // the manual's own behaviour and not decoration.
            const float ey = glassTL.y + stripH * 0.22f;
            const float eh = stripH * 0.46f;
            float ex = glassTL.x + m.cellW * 0.4f;
            if (!cue.unread || blink) {
                drawEnvelope(dl, ImVec2(ex, ey), eh * 1.45f, eh, kLcdSeg);
            }
            ex += eh * 1.45f + m.cellW * 0.3f;
            char cnt[8];
            char cells[2];
            pager::counterCells(in.state.values[0], true, cells, 2);
            std::snprintf(cnt, sizeof cnt, "%c%c", cells[0], cells[1]);
            dl->AddText(fonts::ui(), m.fontPx, ImVec2(ex, glassTL.y + stripH * 0.10f),
                        kLcdSeg, cnt);
            if (in.state.text[2][0] != '\0') {
                ImFont* f = fonts::ui();
                const float tw =
                    f->CalcTextSizeA(m.fontPx, FLT_MAX, 0.0f, in.state.text[2]).x;
                dl->AddText(f, m.fontPx,
                            ImVec2(glassBR.x - m.cellW * 0.4f - tw,
                                   glassTL.y + stripH * 0.10f),
                            kLcdSeg, in.state.text[2]);
            }
            dl->AddLine(ImVec2(glassTL.x + m.cellW * 0.3f, glassTL.y + stripH - 1.0f),
                        ImVec2(glassBR.x - m.cellW * 0.3f, glassTL.y + stripH - 1.0f),
                        kLcdSegSoft, 1.0f);
        }

        // The character rows. Every glyph is placed in its own cell rather than
        // run as a string, which is what makes twenty columns line up whatever
        // face is bound - and is what a character display physically does.
        if (have) {
            char status[pager::kLcdCols + 1];
            const bool haveStatus = pager::composeStatusLine(
                in.state.text[1], in.state.text[3], status, sizeof status);
            const pager::LcdText body =
                pager::wrapLcd(in.state.text[0], pager::kLcdRows - (haveStatus ? 1 : 0));

            ImFont* f = fonts::ui();
            int row = 0;
            auto putRow = [&](const char* s, ImU32 ink) {
                const float ry = glassTL.y + stripH + static_cast<float>(row) * m.cellH;
                // CENTRED ON THE CAP HEIGHT, NOT ON THE LINE BOX. AddText puts
                // the top of the line box at the y it is given, and a line box
                // is taller than the capitals inside it by the room kept for
                // descenders - so centring on it drops every row a fraction of
                // a cell and, at a glyph size chosen from the cell's width
                // rather than its height, pushes the last row off the glass.
                // A capital's top sits about 0.22 of the size below the box and
                // its baseline about 0.80, so its middle is 0.51 down.
                const float capMid = ry + m.cellH * 0.5f - m.fontPx * 0.51f;
                for (int c = 0; s[c] != '\0' && c < pager::kLcdCols; ++c) {
                    if (s[c] == ' ') { continue; }
                    const char one[2] = {s[c], '\0'};
                    const float gw = f->CalcTextSizeA(m.fontPx, FLT_MAX, 0.0f, one).x;
                    dl->AddText(f, m.fontPx,
                                ImVec2(glassTL.x + (static_cast<float>(c) + 0.5f) * m.cellW
                                           - gw * 0.5f,
                                       capMid),
                                ink, one);
                }
                ++row;
            };
            if (haveStatus) { putRow(status, kLcdSeg); }
            for (int i = 0; i < body.lines; ++i) { putRow(body.line[i], kLcdSeg); }

            // The flashing arrow in the bottom right corner: the message runs
            // past the screen. Without it a cut-off page reads as a short one.
            if (body.more && blink) {
                const float ax = glassBR.x - m.cellW * 0.5f;
                const float ay = glassBR.y - m.cellH * 0.55f;
                const float a = m.cellW * 0.30f;
                dl->AddTriangleFilled(ImVec2(ax - a, ay - a), ImVec2(ax + a, ay - a),
                                      ImVec2(ax, ay + a * 0.9f), kLcdSeg);
            }
        }
        dl->PopClipRect();
        lcdBottom = bezBR.y;
    } else {
        // Too small to letter twenty columns. The panel is still drawn, dark
        // and empty, because a pager with an unreadable screen is still a
        // pager and a face that silently omitted its display would be lying
        // about the size it was given.
        const ImVec2 gTL(caseTL.x + sideInset, lcdTop + bezelPad);
        const ImVec2 gBR(caseBR.x - sideInset,
                         std::min(bayBR.y - 8.0f - plateRoom, lcdTop + bezelPad + 24.0f));
        if (gBR.y > gTL.y + 6.0f) {
            dl->AddRectFilled(gTL, gBR, kLcdDead, 1.0f);
            addBenchBevel(dl, gTL, gBR, 1.0f, false);
            lcdBottom = gBR.y;
        }
    }

    // --- the moulded plate under the screen --------------------------------
    //
    // WHERE THE BUTTONS WOULD BE. The header says why there are none; what
    // fills the lower shell instead is the plate, the parting seam between the
    // two halves of the moulding, and then the cradle.
    {
        const float bandH = bayBR.y - lcdBottom;
        const float ph = std::clamp(cs.w * 0.11f, 12.0f, 28.0f);
        const float py = lcdBottom + std::max(5.0f, (bandH - ph) * 0.30f);
        if (py + ph < bayBR.y - 2.0f) {
            const ImVec2 pTL(caseTL.x + sideInset, py);
            const ImVec2 pBR(caseBR.x - sideInset, py + ph);
            dl->AddRectFilled(pTL, pBR, kCaseCut, 2.0f);
            addBenchBevel(dl, pTL, pBR, 2.0f, false);
            const float px = std::min(fonts::kTinySize, ph * 0.68f);
            engrave(dl, ImVec2(pTL.x + 6.0f, pTL.y + (ph - px) * 0.5f - 1.0f), "FOXSDR",
                    kCasePlate, px);
            ImFont* f = fonts::legend();
            const char* model = "ALPHANUMERIC PAGER";
            const float mw = f->CalcTextSizeA(px * 0.86f, FLT_MAX, 0.0f, model).x;
            if (mw < (pBR.x - pTL.x) * 0.62f) {
                engrave(dl, ImVec2(pBR.x - 6.0f - mw, pTL.y + (ph - px) * 0.5f), model,
                        theme::kInkFaint, px * 0.86f);
            }
            // The parting seam between the front and back shells: a cut with
            // its lit far wall, the same groove addBenchDivider draws lying
            // down. It is what stops the lower half reading as a flat panel.
            const float sy = pBR.y + std::max(8.0f, cs.w * 0.06f);
            if (sy < bayBR.y - 4.0f) {
                dl->AddLine(ImVec2(caseTL.x + 2.0f, sy), ImVec2(caseBR.x - 2.0f, sy),
                            theme::withAlpha(theme::kVoid, 0.65f), theme::kHairline);
                dl->AddLine(ImVec2(caseTL.x + 2.0f, sy + 1.0f),
                            ImVec2(caseBR.x - 2.0f, sy + 1.0f),
                            theme::withAlpha(kCaseEdge, 0.55f), theme::kHairline);
            }
        }
    }

    // --- the bench's own column, beside the cradle -------------------------
    //
    // Everything here is the same reading in the bench's materials: engraved
    // captions and amber drums, so the figures can be read across the room
    // without squinting at a 1990s display.
    const float colX = caseBR.x + 18.0f;
    const float colW = bayBR.x - 10.0f - colX;
    if (haveColumn && colW >= kColumnMin) {
        float y = bayTL.y + 12.0f;

        // The lamps. Every one is drawn whether lit or not, so a cold panel
        // still says which lamps it has.
        {
            ImGui::PushFont(fonts::legend(), fonts::kTinySize);
            const float r = 6.0f;
            const float pitch = std::min(colW / 4.0f, 78.0f);
            float lx = colX + pitch * 0.5f;
            const float ly = y + r + 2.0f;
            drawBenchLamp(dl, ImVec2(lx, ly), r, theme::kAlarmHot, alert && blink, "ALERT");
            lx += pitch;
            drawBenchLamp(dl, ImVec2(lx, ly), r, theme::kGold, cue.unread, "NEW");
            lx += pitch;
            drawBenchLamp(dl, ImVec2(lx, ly), r, theme::kPhosphor, lock, "SYNC");
            lx += pitch;
            drawBenchLamp(dl, ImVec2(lx, ly), r, theme::kAmber, lowBatt, "BATT");
            ImGui::PopFont();
            y = ly + r + fonts::kTinySize + 7.0f;
        }

        // The drums: the capcode that was addressed, and how many pages have
        // not been looked at. Seven cells because a FLEX capcode is printed as
        // seven digits; two because the equipment holds 52 message slots and
        // never more.
        // Capped by the COLUMN'S WIDTH as well as its height: nine drums and a
        // gap have to stand side by side, and a tall narrow column would
        // otherwise grow them until the UNREAD pair was pushed off the panel -
        // a reading the plugin supplied, gone, with nothing to say it went.
        const float drumRoom = (colW - 34.0f) / 9.0f / 0.66f;
        const float rowH =
            std::clamp(std::min((bayBR.y - y - 34.0f) * 0.30f, drumRoom), 0.0f, 44.0f);
        if (rowH >= 14.0f) {
            const float cellW = rowH * 0.66f;
            const float capW = cellW * 7.0f;
            engrave(dl, ImVec2(colX, y), "CAPCODE");
            const float uCapX = colX + capW + 22.0f;
            const bool roomForUnread = uCapX + cellW * 2.0f + 8.0f < bayBR.x - 10.0f;
            if (roomForUnread) { engrave(dl, ImVec2(uCapX, y), "UNREAD"); }
            y += fonts::kTinySize + 3.0f;
            char cap[8];
            pager::drumCells(have ? in.state.text[1] : "", cap, 7);
            drawDrumRow(dl, ImVec2(colX + 3.0f, y), cellW, rowH, cap, 7);
            if (roomForUnread) {
                char un[2];
                pager::counterCells(in.state.values[0], have, un, 2);
                drawDrumRow(dl, ImVec2(uCapX + 3.0f, y), cellW, rowH, un, 2);
            }
            y += rowH + 8.0f;
        }

        // The two words: what kind of page it was and when it came in. Words,
        // so they take the ui face rather than the counter's - fonts.hpp is
        // explicit that Nova Mono is for figures and merges its capitals.
        // The two word rows share whatever height is left rather than sitting
        // in a fixed clump at the top of a tall column.
        // SHARED BETWEEN THE TWO OF THEM, not taken by the first. Sizing each
        // row from the whole remaining height gave a first row so tall that the
        // second fell off the bottom of the bay and simply was not drawn - a
        // slot the plugin filled, silently missing.
        // SHARED BETWEEN THE TWO OF THEM, and measured against what is left
        // once the EVENT line at the bottom has been kept clear. Sizing each
        // row from the whole remaining height gave a first row so tall that the
        // second fell off the bottom of the bay and simply was not drawn - a
        // slot the plugin filled, silently missing, which is the failure this
        // face is least able to afford.
        const float wordRoom = bayBR.y - y - (fonts::kTinySize + 8.0f);
        const float lineH =
            std::clamp(wordRoom * 0.5f - 3.0f, fonts::kUiSize + 2.0f, 52.0f);
        const float labelW = std::min(96.0f, colW * 0.40f);
        struct Pair {
            const char* caption;
            const char* value;
        };
        const Pair pairs[2] = {
            {"TYPE", have ? in.state.text[3] : ""},
            {"RECEIVED", have ? in.state.text[2] : ""},
        };
        for (int i = 0; i < 2; ++i) {
            if (y + lineH > bayBR.y - (fonts::kTinySize + 6.0f)) { break; }
            engrave(dl, ImVec2(colX, (y + y + lineH) * 0.5f - fonts::kTinySize * 0.6f),
                    pairs[i].caption);
            const ImVec2 wTL(colX + labelW, y - 1.0f);
            // The well is as wide as the words need and no wider: run out to
            // the edge of a large column and a five-character time sits in a
            // bar the width of the panel, which reads as a missing value
            // rather than a short one.
            const ImVec2 wBR(std::min(bayBR.x - 10.0f, wTL.x + 300.0f), y + lineH - 3.0f);
            if (wBR.x > wTL.x + 20.0f) {
                drawFreqDrumWell(dl, wTL, wBR);
                // AN EMPTY SLOT IS AN EMPTY WELL. No dash, no "n/a", nothing
                // that could be read as a value the plugin sent.
                if (pairs[i].value[0] != '\0') {
                    onGlass(dl, ImVec2(wTL.x + 7.0f, wTL.y + 1.0f), pairs[i].value,
                            wBR.x - wTL.x - 14.0f);
                }
            }
            y += lineH + 3.0f;
        }

        // What the face is counting from: the plugin's own event number.
        if (have) {
            char seq[32];
            std::snprintf(seq, sizeof seq, "EVENT %u",
                          static_cast<unsigned>(in.state.seq));
            engrave(dl, ImVec2(colX, bayBR.y - fonts::kTinySize - 6.0f), seq,
                    theme::kInkFaint);
        } else {
            engrave(dl, ImVec2(colX, bayBR.y - fonts::kTinySize - 6.0f),
                    "NO PAGE RECEIVED", theme::kInkFaint);
        }
    }

    dl->PopClipRect();
    return bayBR.y + 10.0f - tl.y;
}

}  // namespace cascade::gui
