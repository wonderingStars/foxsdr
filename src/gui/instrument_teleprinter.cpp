// instrument_teleprinter.cpp - the CASCADE_INSTRUMENT_TELEPRINTER face: an
// aircraft cockpit printer, with a strip of thermal paper coming out of it.
//
// THE EQUIPMENT. A flight-deck message printer of the ARINC 744/744A "full
// format printer" family - the box on the rear of an airliner's pedestal that
// prints ACARS, AIDS, FMGC and CFDIU reports onto a roll of thermal paper for
// the crew to tear off. A Dzus-mounted panel not much wider than a sheet of
// paper, a latched paper door, a SLEW switch to feed the roll after loading,
// a status lamp, and a slot with a serrated tear bar that the paper comes out
// of and is torn against.
//
// WHERE I LOOKED (design reference only - every line of code here is my own):
//   - AstroNova Aerospace, "ToughWriter 4 Airborne Printer" flyer,
//     https://aerospace.astronovainc.com/wp-content/uploads/tw4flyer.pdf -
//     the specification sheet for the printer fitted to B767/B787/A380 flight
//     decks: DIRECT THERMAL printing, 300 x 300 dpi, "Both top/bottom and
//     side/side Dzus mounting are available", "A variety of front face colours
//     are available", 5.75 in wide by 9.75 in high. That is where the
//     proportions, the fasteners and the printing process come from.
//   - AstroNova Aerospace airborne printers / Miltope TP4840,
//     https://aerospace.astronovainc.com/products/airborne-printers/miltope-series/tp4840/
//     - "Fully ARINC744 compliant", which is the standard this class of box is
//     built to.
//   - FlyByWire Simulations A32NX flight-deck documentation, pedestal printer,
//     https://docs.flybywiresim.com/pilots-corner/a32nx/a32nx-briefing/flight-deck/pedestal/printer/
//     and AviationHunt's A320 cockpit printer page - the SLEW switch that
//     advances the paper after a new roll, the latched paper door, and the
//     list of systems (ACARS, AIDS, FMGC, CFDIU, EVMU) whose reports it
//     prints.
//   - SAE ITC / ARINC 744A "Full-Format Printer with Graphics Capability" -
//     the standard's title and scope.
//
// TRANSLATED ONTO THE BENCH. The machine is the bench's own brass and dark
// enamel - the same plate, bevels, lamps and counter drums the radar scope's
// enclosure wears - so it stands beside the rest of the equipment rather than
// being pasted in. THE PAPER IS THE ONE THING HERE THAT IS NOT GLASS: an
// ivory strip with a warm dark-grey print, feed perforations down both edges
// and the horizontal dot-line banding a thermal head actually leaves, because
// a phosphor-green readout would say "this is a display" when what the
// instrument has is a piece of paper.
//
// THE THREE RULES.
//   1. The captions - PRINTER, STATUS, MESSAGES, the lamp words - are
//      engraved into the metal; the live figures are the counter drums on
//      glass and the print on the paper. Nothing on this face is a picture of
//      a control: PAPER lights when there is something printed on the strip,
//      READY lights when the feed is alive, NEW while a message has arrived
//      that this window has not shown, and ALERT blinks while the plugin
//      holds the alert flag.
//   2. NO READING IS DRAWN AS NO READING. With nothing decoded the counter's
//      apertures carry dashes rather than 0000, the lamps are dark, and the
//      slot shows a blank leader of paper with the legend NO MESSAGE PRINTED
//      cut into the machine BELOW it - never onto the paper, because nothing
//      was printed.
//   3. It fits the rectangle it is given: the control column is dropped
//      before the paper is squeezed, the type shrinks to a floor, and the
//      strip is clipped to its own bay.
//
// THE PAPER ADVANCES. On a new event the strip feeds a short distance out of
// the slot over half a second (instrument_teleprinter_math.hpp:
// paperFeedOffset), which is the machine doing the one thing a printer does.
// The elapsed time comes from a tiny GUI-thread-only table keyed on the
// plugin's identity, because the face is handed `unread` but not WHEN.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "gui/fonts.hpp"
#include "gui/instrument_teleprinter_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;
namespace tp = cascade::gui::teleprinter;

namespace {

// --- the paper --------------------------------------------------------------
//
// Deliberately NOT in theme.hpp: the palette there is the bench's metal, ink
// and phosphor, and paper is none of those. These five tones exist only
// inside this instrument and naming them anywhere else would invite the next
// face to put a reading on parchment.
constexpr ImU32 kPaper = IM_COL32(0xE8, 0xE1, 0xCC, 0xFF);
constexpr ImU32 kPaperLit = IM_COL32(0xF2, 0xEC, 0xDA, 0xFF);
constexpr ImU32 kPaperEdge = IM_COL32(0xC3, 0xBA, 0xA1, 0xFF);
// Thermal print is a warm near-black, not ink-black, and it fades: the older
// message on the strip is printed in the faint tone.
constexpr ImU32 kPrint = IM_COL32(0x2E, 0x2A, 0x22, 0xFF);
constexpr ImU32 kPrintFaint = IM_COL32(0x6E, 0x66, 0x57, 0xFF);

// --- when the last message arrived -----------------------------------------
//
// The face is told THAT an event is unread, not when it landed, and a feed
// that takes half a second needs the difference. GUI THREAD ONLY - every
// instrument face is drawn from drawPluginWindows on the one thread - so this
// needs no lock; it is a fixed table so it cannot allocate, and an instrument
// that falls out of it simply draws its paper settled.
struct FeedRecord {
    std::uint32_t key = 0u;
    std::uint32_t seq = 0u;
    double atSec = 0.0;
    bool used = false;
};
constexpr int kFeedSlots = 8;
FeedRecord g_feed[kFeedSlots];

std::uint32_t identityHash(const HostInstrument& in) {
    // FNV-1a over plugin+title. Two instruments of the same kind from the
    // same plugin with the same window name are indistinguishable to the
    // host's own window identity too, so this is exactly as unique as the
    // thing it is standing in for.
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](const std::string& s) {
        for (const char c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 16777619u;
        }
        h ^= 0x2Fu;
        h *= 16777619u;
    };
    mix(in.plugin);
    mix(in.title);
    return h;
}

// Seconds since this instrument's sequence last advanced, or a large number
// when it has not (which settles the paper).
double secondsSinceFeed(const HostInstrument& in, double nowSec) {
    if (!in.have) { return 1.0e6; }
    const std::uint32_t key = identityHash(in);
    FeedRecord* free1 = nullptr;
    FeedRecord* oldest = &g_feed[0];
    for (FeedRecord& r : g_feed) {
        if (r.used && r.key == key) {
            if (r.seq != in.state.seq) {
                r.seq = in.state.seq;
                r.atSec = nowSec;
            }
            return nowSec - r.atSec;
        }
        if (!r.used && free1 == nullptr) { free1 = &r; }
        if (r.used && r.atSec < oldest->atSec) { oldest = &r; }
    }
    FeedRecord* slot = (free1 != nullptr) ? free1 : oldest;
    slot->used = true;
    slot->key = key;
    slot->seq = in.state.seq;
    slot->atSec = nowSec;
    return 0.0;
}

// --- small drawing helpers --------------------------------------------------

// A caption cut into the metal, in the legend face, at the small engraving
// size: the dark pass first, the muted ink over it, which is the cut.
void engrave(ImDrawList* dl, const ImVec2& at, const char* s, float px, ImU32 ink) {
    ImFont* f = fonts::legend();
    dl->AddText(f, px, ImVec2(at.x, at.y + 1.0f), theme::withAlpha(theme::kVoid, 0.60f), s);
    dl->AddText(f, px, at, ink, s);
}

// THE MESSAGE COUNTER, as the bench's own odometer drums rather than the
// scope's: four apertures in a machined well, amber figures on glass.
//
// WITH NO READING THE APERTURES CARRY DASHES. A counter reading 0000 says "we
// have printed nothing", which is a measurement; a printer that has not been
// switched on has not measured anything, and the two are opposite claims.
void drawMessageCounter(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                        int digits, double value, bool haveReading) {
    const float groupW = static_cast<float>(digits) * cellW + static_cast<float>(digits - 1) * 2.0f;
    drawFreqDrumWell(dl, ImVec2(tl.x - 4.0f, tl.y - 4.0f),
                     ImVec2(tl.x + groupW + 4.0f, tl.y + cellH + 4.0f));

    // Clamped to all-nines rather than wrapped: a four-digit counter shown a
    // five-digit number must read "as high as I go", not the low four digits,
    // which would be a smaller number than the truth.
    double v = value;
    if (!(v >= 0.0)) { v = 0.0; }
    double cap = 1.0;
    for (int i = 0; i < digits; ++i) { cap *= 10.0; }
    if (v > cap - 1.0) { v = cap - 1.0; }
    const long shown = static_cast<long>(v);

    bool leading = true;
    for (int i = 0; i < digits; ++i) {
        const float x = tl.x + static_cast<float>(i) * (cellW + 2.0f);
        long place = 1;
        for (int k = 0; k < digits - 1 - i; ++k) { place *= 10; }
        const int d = static_cast<int>((shown / place) % 10);
        if (d != 0 || i == digits - 1) { leading = false; }
        drawFreqDrumCell(dl, ImVec2(x, tl.y), ImVec2(x + cellW, tl.y + cellH),
                         haveReading ? static_cast<char>('0' + d) : '-',
                         haveReading && !leading, cellH * 0.66f);
    }
}

// The slot and its tear bar, drawn OVER the paper so the strip reads as
// coming out from behind them. The serrations are what makes a brass strip a
// tear bar rather than a trim piece.
void drawSlotAndTearBar(ImDrawList* dl, float x0, float x1, float y, float barH) {
    if (x1 - x0 < 8.0f) { return; }
    // The mouth: a dark aperture the paper leaves through.
    dl->AddRectFilled(ImVec2(x0, y - barH * 0.9f), ImVec2(x1, y), theme::kVoid, 1.0f);
    // The bar itself.
    const ImVec2 bTL(x0, y);
    const ImVec2 bBR(x1, y + barH);
    dl->AddRectFilled(bTL, ImVec2(bBR.x, (bTL.y + bBR.y) * 0.5f), theme::kBrassBright, 0.0f);
    dl->AddRectFilled(ImVec2(bTL.x, (bTL.y + bBR.y) * 0.5f), bBR, theme::kBrassMid, 0.0f);
    addBenchBevel(dl, bTL, bBR, 1.0f, true);
    // The serrations along its lower edge.
    const float pitch = 7.0f;
    const float depth = std::min(5.0f, barH * 0.55f);
    for (float x = x0; x + pitch <= x1; x += pitch) {
        dl->AddTriangleFilled(ImVec2(x, bBR.y), ImVec2(x + pitch, bBR.y),
                              ImVec2(x + pitch * 0.5f, bBR.y + depth), theme::kBrassShade);
        dl->AddLine(ImVec2(x, bBR.y), ImVec2(x + pitch * 0.5f, bBR.y + depth),
                    theme::withAlpha(theme::kBrassTint, 0.70f), theme::kHairline);
    }
}

// The strip's own texture: feed perforations down both edges and the
// horizontal banding a thermal head leaves, which is what turns a lettered
// rectangle into printed paper. The banding is drawn in the paper's own tone,
// so it is invisible on blank paper and breaks the print into dot rows.
void drawPaperTexture(ImDrawList* dl, const ImVec2& pTL, const ImVec2& pBR, float scrollPx) {
    for (float y = pTL.y + 2.0f; y < pBR.y; y += 2.0f) {
        dl->AddLine(ImVec2(pTL.x, y), ImVec2(pBR.x, y), theme::withAlpha(kPaperLit, 0.55f),
                    1.0f);
    }
    // The perforations travel with the paper, or they are printed on the
    // machine rather than on the roll.
    const float pitch = 13.0f;
    float y0 = pTL.y + std::fmod(scrollPx, pitch);
    if (y0 < pTL.y) { y0 += pitch; }
    for (float y = y0; y < pBR.y - 1.0f; y += pitch) {
        dl->AddCircleFilled(ImVec2(pTL.x + 4.5f, y), 1.6f, kPaperEdge, 8);
        dl->AddCircleFilled(ImVec2(pBR.x - 4.5f, y), 1.6f, kPaperEdge, 8);
    }
    // Both long edges, slightly in shade - a strip of paper is not a flat
    // rectangle of one colour.
    dl->AddRectFilled(pTL, ImVec2(pTL.x + 2.0f, pBR.y), kPaperEdge);
    dl->AddRectFilled(ImVec2(pBR.x - 2.0f, pTL.y), pBR, kPaperEdge);
}

// The printed LINE a row carries. A teleprinter's rows are the paper, one row
// per printed line, and the line itself is the row's LAST column - the demo's
// Time/Reg/Flight/Text and the ACARS feed's Time/Reg/Flight/Label/Text both
// put it there. The rest of the columns are the memory table's business,
// where they have headings to explain them.
//
// THE COLUMN COUNT COMES FROM THE HEADINGS, not from hunting for the last
// cell that happens to be filled: a block that carried no text has an EMPTY
// last cell, and hunting backwards would print its label onto the paper as
// though that were the message. An empty cell prints nothing, which is what
// "the aircraft sent no text" looks like.
const char* printedLine(const HostInstrument& in, const CascadePanelRow& row) {
    std::size_t cols = in.headings.size();
    if (cols == 0u || cols > CASCADE_PANEL_MAX_COLUMNS) { cols = CASCADE_PANEL_MAX_COLUMNS; }
    return row.cells[cols - 1u];
}

}  // namespace

float drawTeleprinterFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                          const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    if (w < 80.0f || h < 60.0f) { return 0.0f; }

    // The machine's own plate, titled by the plugin's window name.
    float y = addBenchPlate(dl, tl, br, in.title.c_str());

    const float pad = 9.0f;
    const ImVec2 bodyTL(tl.x + pad, y + 2.0f);
    const ImVec2 bodyBR(br.x - pad, br.y - pad);
    const float bodyW = bodyBR.x - bodyTL.x;
    const float bodyH = bodyBR.y - bodyTL.y;
    if (bodyW < 60.0f || bodyH < 36.0f) { return y - tl.y; }

    const bool alert = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
    const bool blinkOn = std::fmod(cue.nowSec, 0.5) < 0.25;

    // --- the control column ------------------------------------------------
    //
    // DROPPED BEFORE THE PAPER IS. A printer with no paper visible is not a
    // printer; a printer with no counter is a printer whose counter you
    // cannot see, and the memory table under the window still says how many
    // messages there have been. So in a narrow window the column goes and the
    // lamps move onto the bay's shoulder.
    float colW = std::clamp(bodyW * 0.32f, 134.0f, 196.0f);
    if (bodyW - colW < 210.0f || bodyH < 104.0f) { colW = 0.0f; }

    if (colW > 0.0f) {
        // Two Dzus quarter-turn fasteners, top and bottom of the column: this
        // panel is rack-mounted in a pedestal, and the fasteners are how it
        // is held there.
        addCabinetScrew(dl, ImVec2(bodyTL.x + 6.0f, bodyTL.y + 7.0f), 4.5f, 28.0f);
        addCabinetScrew(dl, ImVec2(bodyTL.x + 6.0f, bodyBR.y - 7.0f), 4.5f, -14.0f);

        const float cx0 = bodyTL.x + 18.0f;
        const float capW = colW - 20.0f;
        float cy = bodyTL.y + 2.0f;

        addBenchGroupCaption(dl, ImVec2(cx0, cy), capW, "STATUS");
        cy += fonts::kTinySize + 8.0f;

        // The lamps, TWO TO A ROW and in the legend face at the engraving
        // size rather than the ambient one: drawBenchLamp letters its caption
        // in whatever is bound, and the application's ordinary 21 px would
        // run PAPER into READY inside a column this wide.
        //
        // PAPER: there is print on the strip to read. Not the same claim as
        // READY - a live feed that has not yet decoded a block has a blank
        // leader hanging out of the slot and nothing on it.
        const bool havePrint =
            in.have && (in.state.text[0][0] != '\0' || in.state.text[1][0] != '\0' ||
                        in.state.text[2][0] != '\0' || !in.rows.empty());
        struct Lamp {
            ImU32 colour;
            bool lit;
            const char* caption;
        };
        const Lamp lamps[4] = {{theme::kAmber, havePrint, "PAPER"},
                               {theme::kPhosphor, in.have, "READY"},
                               {theme::kGold, cue.unread, "NEW"},
                               // An alerting lamp BLINKS. One that is merely
                               // on is a lamp somebody has stopped seeing.
                               {theme::kAlarmHot, alert && blinkOn, "ALERT"}};
        ImGui::PushFont(fonts::legend(), fonts::kTinySize - 3.0f);
        const float lampR = 6.0f;
        const float lampPitch = capW * 0.5f;
        const float lampRowH = lampR * 2.0f + fonts::kTinySize - 3.0f + 5.0f;
        for (int i = 0; i < 4; ++i) {
            // Room is checked PER ROW, so a short window keeps the first two
            // lamps and the counter rather than losing the counter to lamps
            // drawn over the bottom of the plate.
            if ((i % 2) == 0 && cy + lampRowH > bodyBR.y - 4.0f) { break; }
            const float lx = cx0 + lampPitch * (0.5f + static_cast<float>(i % 2));
            const float ly = cy + lampR + 1.0f;
            drawBenchLamp(dl, ImVec2(lx, ly), lampR, lamps[i].colour, lamps[i].lit,
                          lamps[i].caption);
            if ((i % 2) == 1) { cy += lampRowH; }
        }
        ImGui::PopFont();
        cy += 3.0f;

        if (cy + fonts::kTinySize + 34.0f < bodyBR.y) {
            addBenchGroupCaption(dl, ImVec2(cx0, cy), capW, "MESSAGES");
            cy += fonts::kTinySize + 9.0f;
            const float cellW = std::min(26.0f, (capW - 14.0f) / 4.0f);
            const float cellH = std::min(30.0f, bodyBR.y - cy - 6.0f);
            if (cellW > 8.0f && cellH > 12.0f) {
                drawMessageCounter(dl, ImVec2(cx0 + 4.0f, cy), cellW, cellH, 4,
                                   in.state.values[0], in.have);
            }
        }
    }

    // --- the paper bay ------------------------------------------------------
    const ImVec2 bayTL(bodyTL.x + (colW > 0.0f ? colW + 8.0f : 0.0f), bodyTL.y);
    const ImVec2 bayBR(bodyBR.x, bodyBR.y);
    const float bayW = bayBR.x - bayTL.x;
    const float bayH = bayBR.y - bayTL.y;
    if (bayW < 48.0f || bayH < 30.0f) { return bodyBR.y + pad - tl.y; }

    // The inside of the machine, seen through the slot: a well cut into the
    // panel rather than a dark rectangle drawn on it.
    dl->AddRectFilled(bayTL, bayBR, theme::kWell, theme::kPanelRounding);
    dl->AddRect(bayTL, bayBR, theme::kBrassDark, theme::kPanelRounding, 0, theme::kHairline);
    addBenchBevel(dl, bayTL, bayBR, theme::kPanelRounding, false);

    // With no column, the lamps ride on the bay's shoulder so the state is
    // still readable in a small window.
    float bayTop = bayTL.y + 4.0f;
    if (colW <= 0.0f && bayW > 150.0f) {
        ImGui::PushFont(fonts::legend(), fonts::kTinySize - 3.0f);
        const float lampR = 5.0f;
        float lx = bayBR.x - 26.0f;
        drawBenchLamp(dl, ImVec2(lx, bayTop + lampR + 1.0f), lampR, theme::kAlarmHot,
                      alert && blinkOn, "ALERT");
        lx -= 46.0f;
        drawBenchLamp(dl, ImVec2(lx, bayTop + lampR + 1.0f), lampR, theme::kGold, cue.unread,
                      "NEW");
        lx -= 46.0f;
        drawBenchLamp(dl, ImVec2(lx, bayTop + lampR + 1.0f), lampR, theme::kPhosphor, in.have,
                      "READY");
        ImGui::PopFont();
        bayTop += lampR * 2.0f + fonts::kTinySize + 2.0f;
    }

    // --- what is on the paper ----------------------------------------------
    //
    // The heading from the kind's slot map, then the latest printed line,
    // then - if the strip is long enough - the one before it, fading away
    // towards the tear bar. Nothing here is invented: a slot the plugin left
    // empty prints nothing at all.
    const float paperInset = std::min(14.0f, bayW * 0.10f);
    // The mouth of the slot is drawn ABOVE the paper's top edge, so the strip
    // starts a mouth's depth into the bay rather than under the bevel.
    const float slotBarH = std::clamp(bayH * 0.10f, 6.0f, 9.0f);
    const float mouthH = slotBarH * 0.9f;
    const ImVec2 pTL(bayTL.x + paperInset, bayTop + mouthH);
    const ImVec2 pBR(bayBR.x - paperInset, bayBR.y - 3.0f);
    const float paperW = pBR.x - pTL.x;
    if (paperW < 40.0f || pBR.y - pTL.y < 24.0f) { return bodyBR.y + pad - tl.y; }

    ImFont* pf = fonts::reading();
    const float textInset = 9.0f;
    const float textW = paperW - textInset * 2.0f;
    // The type is chosen so a useful number of characters fits across the
    // strip, and shrunk rather than allowed to spill: a printer that could
    // only ever show fourteen characters of a message is not showing the
    // message.
    float px = fonts::kReadingSize;
    float charW = pf->CalcTextSizeA(px, FLT_MAX, 0.0f, "M").x;
    while (px > 11.0f && tp::paperColumns(textW, charW) < 26) {
        px -= 1.0f;
        charW = pf->CalcTextSizeA(px, FLT_MAX, 0.0f, "M").x;
    }
    const int columns = tp::paperColumns(textW, charW);
    const float lineH = px + 3.0f;

    tp::PaperLine lines[tp::kMaxPaperLines];
    ImU32 inks[tp::kMaxPaperLines];
    bool ruleUnder[tp::kMaxPaperLines] = {false};
    int count = 0;

    const CascadeInstrumentState& s = in.state;
    if (in.have && columns > 0) {
        const int headLines =
            tp::paperHeading(s.text[0], s.text[1], s.text[2], s.text[3], s.text[4], columns,
                             lines, 2);
        for (int i = 0; i < headLines; ++i) { inks[count + i] = kPrint; }
        count += headLines;
        if (count > 0) { ruleUnder[count - 1] = true; }

        const std::size_t nRows = in.rows.size();
        if (nRows > 0 && count < tp::kMaxPaperLines) {
            // Rows are the paper, OLDEST FIRST (the kind's slot comment), so
            // the newest print is the last row.
            const char* newest = printedLine(in, in.rows[nRows - 1]);
            const int body = tp::wrapPaper(newest, columns, lines + count,
                                           std::min(4, tp::kMaxPaperLines - count));
            for (int i = 0; i < body; ++i) { inks[count + i] = kPrint; }
            count += body;
        }
        if (nRows > 1 && count > 0 && count + 1 < tp::kMaxPaperLines) {
            const char* prev = printedLine(in, in.rows[nRows - 2]);
            const int body = tp::wrapPaper(prev, columns, lines + count,
                                           std::min(3, tp::kMaxPaperLines - count));
            for (int i = 0; i < body; ++i) { inks[count + i] = kPrintFaint; }
            if (body > 0) { ruleUnder[count - 1] = true; }
            count += body;
        }
    }

    // How far the sheet still has to travel out of the slot.
    const float feed =
        tp::paperFeedOffset(secondsSinceFeed(in, cue.nowSec), tp::kFeedSeconds,
                            tp::kFeedDistancePx);

    const float firstLineY = pTL.y + slotBarH + 7.0f;

    // WITH NOTHING PRINTED, A BLANK LEADER. The roll is loaded and the paper
    // is out; there is simply nothing on it, which is what "no reading" looks
    // like on a printer. The legend saying so is cut into the MACHINE below
    // the leader - putting it on the paper would claim the printer printed it.
    const bool blank = (count == 0);
    const float paperBottom =
        blank ? std::min(pBR.y, firstLineY + 26.0f) : pBR.y;

    // The strip itself: lit where it leaves the slot and settling to its own
    // tone further out, which is the light falling into the bay.
    dl->AddRectFilled(ImVec2(pTL.x + 2.0f, pTL.y + 3.0f),
                      ImVec2(std::min(pBR.x + 3.0f, bayBR.x - 2.0f),
                             std::min(paperBottom + 3.0f, bayBR.y - 2.0f)),
                      theme::withAlpha(theme::kVoid, 0.55f), 2.0f);
    dl->AddRectFilledMultiColor(pTL, ImVec2(pBR.x, paperBottom), kPaperLit, kPaperLit, kPaper,
                                kPaper);
    drawPaperTexture(dl, pTL, ImVec2(pBR.x, paperBottom), feed);

    // The print, clipped to the strip so a feed in progress is genuinely
    // still inside the machine rather than drawn over the tear bar.
    if (!blank) {
        dl->PushClipRect(ImVec2(pTL.x, pTL.y + slotBarH), ImVec2(pBR.x, paperBottom), true);
        float ty = firstLineY + feed;
        for (int i = 0; i < count; ++i) {
            if (ty > paperBottom) { break; }
            dl->AddText(pf, px, ImVec2(pTL.x + textInset, ty), inks[i], lines[i].text);
            if (ruleUnder[i]) {
                // The printed rule between one message and the last, dotted
                // the way a thermal head lays it down.
                const float ry = ty + lineH - 3.0f;
                for (float rx = pTL.x + textInset; rx < pBR.x - textInset; rx += 4.0f) {
                    dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + 2.0f, ry + 1.0f),
                                      kPrintFaint);
                }
            }
            ty += lineH;
        }
        dl->PopClipRect();
    } else {
        const char* legend = in.have ? "NO MESSAGE PRINTED" : "PRINTER IDLE";
        const float ly = paperBottom + 10.0f;
        if (ly + fonts::kTinySize < bayBR.y) {
            ImFont* lf = fonts::legend();
            const ImVec2 sz = lf->CalcTextSizeA(fonts::kTinySize, FLT_MAX, 0.0f, legend);
            engrave(dl, ImVec2((bayTL.x + bayBR.x) * 0.5f - sz.x * 0.5f, ly), legend,
                    fonts::kTinySize, theme::kInkFaint);
        }
    }

    // The shadow the tear bar throws onto the paper, then the bar itself over
    // the strip's top edge so the paper reads as coming out from behind it.
    dl->AddRectFilledMultiColor(ImVec2(pTL.x, pTL.y + slotBarH),
                                ImVec2(pBR.x, pTL.y + slotBarH + 7.0f),
                                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    drawSlotAndTearBar(dl, bayTL.x + 5.0f, bayBR.x - 5.0f, pTL.y, slotBarH);

    return bodyBR.y + pad - tl.y;
}

}  // namespace cascade::gui
