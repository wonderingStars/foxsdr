// instrument_fax.cpp - the CASCADE_INSTRUMENT_FAX face: a marine radiofax
// recorder, drawn in the bench's own materials.
//
// THE EQUIPMENT. A shipboard weather-fax recorder: the Furuno FAX-408 and
// FAX-410 and the JRC JAX-9B. All three are the same object - a flat painted
// slab about 380 mm across and 100 mm thick that screws to a desk or a
// bulkhead, with a raised control-panel plate on the upper face and, across
// the whole width of the front, a paper slot closed by a SERRATED TEAR BAR
// held on by two round-head screws, out of which the chart creeps and drapes
// forward down the cabinet. The FAX-410's cabinet is Munsell 2.5GY5/1.5, a
// mid grey with a green cast; the FAX-408 is Munsell N2.5, near black; the
// JAX-9B is N4 on a black steel base. The panel carries a two-line green LCD,
// a POWER rocker, VOLUME and SYNC knobs, a grid of pale square keys, and the
// lamps: TUNE, TIMER and an orange RCD that flashes on the start signal and
// goes steady while the chart records.
//
// TWO THINGS HERE ARE TRANSLATIONS RATHER THAN COPIES, and both are honest
// about it. First, the real TUNE indicator is not a meter: it is a column of
// three LEDs - red high, GREEN ON FREQUENCY, red low (FAX-408 manual, 1.5.2).
// It is already a centre-zero offset indicator, and the plugin measures the
// error in hertz rather than in three steps, so this face spends the extra
// resolution on the bench's own moving-coil meter with red at both stops.
// Second, no machine in this family has a LINE COUNTER - their LCDs show
// channel, call sign, frequency, speed and IOC - but the decoder counts lines
// and how much of a ten-minute chart has landed is exactly what the paper
// coming out of the slot tells an operator across the room, so the count is
// given drums of its own.
//
// Reference: the Furuno FAX-410 operator's manual and specification sheet
// (outline drawing D-1, coating colour, IOC and speed tables), the FAX-408
// operator's manual (panel artwork, LED colours, 216 mm paper), the JRC
// JAX-9B instruction manual (panel, 260 mm paper), the Bureau of Meteorology's
// technical characteristics page for the start / phasing / picture / stop
// sequence, and NOAA/NWS's "Worldwide Marine Radiofacsimile Broadcast
// Schedules" for 120 lpm at IOC 576 being the working standard and for the
// 1.9 kHz USB rule the tuning error exists to serve. The URLs are listed in
// tests/test_instrument_fax.cpp. The look is translated into this
// application's brass, dark enamel, amber glass and ivory engraving rather
// than copied - it has to stand beside the radar scope, not beside a
// photograph.
//
// WHAT IS DELIBERATELY NOT HERE: KEYS. The real panel is a five-by-four grid
// of them - POWER, IOC, SPD, PRG, CH, RCD, PAPER FEED and the ten-key - and
// every one would be a picture of a control on this face, because nothing in
// the instrument contract lets a host command a decoder. instrument_face.hpp
// forbids exactly that, so the deck carries only things that are driven by a
// measurement: lamps that light for a reason, readouts that show what the
// decoder measured, a counter that counts real lines, and a needle that
// points at a real error.
//
// AND THE PICTURE IS NOT HERE EITHER, for a reason worth stating rather than
// engraving on the panel: a radiofax plugin delivers its chart through
// CASCADE_CAP_IMAGE_DECODER and the host shows it in its own picture window.
// So this face is built to be useful WITHOUT the pixels, which is what a
// recorder's own panel is for: the tuning meter tells you whether the chart
// arriving will be readable at all, the phase lamps tell you where in the
// ten-minute transmission you are, and the counter and the paper feeding out
// of the slot tell you how much of it has landed.
//
// The arithmetic - the phase word to a lamp, the needle's travel, how far the
// paper has fed, how the deck divides at whatever size the window has been
// dragged to - is in instrument_fax_math.hpp, without ImGui, and is pinned by
// tests/test_instrument_fax.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/instrument_fax_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;
namespace fx = cascade::gui::faxmath;

namespace {

constexpr float kPiF = 3.14159265358979323846f;

// The largest size at which `s` fits in `room`, never below a nine pixel
// floor. Every word on this face is fitted rather than clipped, because the
// window is the user's to make narrow.
float fitPx(ImFont* f, float sizePx, const char* s, float room) {
    if (f == nullptr || s == nullptr || s[0] == '\0' || !(room > 0.0f)) { return sizePx; }
    const float w = f->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, s).x;
    if (w <= room) { return sizePx; }
    const float scaled = sizePx * room / w;
    const float floor = cascade::gui::px(9.0f);
    return scaled < floor ? floor : scaled;
}

// A caption cut into a dark deck: the legend face, the void beneath it and
// the muted ink on top. Returns the width used.
float engrave(ImDrawList* dl, const ImVec2& at, const char* s, float px) {
    ImFont* f = fonts::legend();
    dl->AddText(f, px, ImVec2(at.x, at.y + 1.0f), theme::kVoid, s);
    dl->AddText(f, px, at, theme::kInkMuted, s);
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// --- the glass a figure sits on ---------------------------------------------
//
// One selector cell: a machined aperture with a number behind it, lit when
// that position is the one in use and drained when it is not. Both states are
// drawn, because a selector that hid its unused positions would stop being a
// selector and become a bare figure - the panel's own statement of what the
// format offers is half of what makes it readable.
void drawSelectorCell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                      const char* text, bool lit) {
    if (br.x - tl.x < cascade::gui::px(8.0f) || br.y - tl.y < cascade::gui::px(8.0f)) {
        return;
    }
    dl->AddRectFilled(tl, br, theme::kWell, 2.0f);
    dl->AddRect(tl, br, theme::kBrassDark, 2.0f, 0, theme::kHairline);
    addBenchBevel(dl, tl, br, 2.0f, false);
    if (text == nullptr || text[0] == '\0') { return; }
    ImFont* f = fonts::reading();
    const float room = br.x - tl.x - cascade::gui::px(6.0f);
    const float px = fitPx(f, (br.y - tl.y) * 0.62f, text, room);
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 at((tl.x + br.x) * 0.5f - sz.x * 0.5f,
                    (tl.y + br.y) * 0.5f - sz.y * 0.5f);
    if (lit) {
        // The bloom of a lit aperture, as offset copies - the same trick the
        // frequency counter's drums use, because ImGui has no blur.
        const ImU32 halo = theme::withAlpha(theme::kAmber, 0.16f);
        dl->AddText(f, px, ImVec2(at.x - 1.0f, at.y), halo, text);
        dl->AddText(f, px, ImVec2(at.x + 1.0f, at.y), halo, text);
        dl->AddText(f, px, ImVec2(at.x, at.y - 1.0f), halo, text);
        dl->AddText(f, px, ImVec2(at.x, at.y + 1.0f), halo, text);
    }
    dl->AddText(f, px, at, lit ? theme::kAmber : theme::kAmberDim, text);
}

// A whole selector: its engraved name and the ladder of positions under it.
// Returns the x it ended at, so the next group can be laid beside it.
float drawSelector(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                   const char* caption, const fx::Selector& sel) {
    const float capPx = cascade::gui::px(fonts::kTinySize) * 0.82f;
    engrave(dl, tl, caption, capPx);
    const float y = tl.y + capPx + cascade::gui::px(4.0f);
    const float gap = cascade::gui::px(3.0f);
    float x = tl.x;
    for (int i = 0; i < sel.count; ++i) {
        char txt[16];
        fx::formatSelector(sel.v[i], txt, sizeof txt);
        drawSelectorCell(dl, ImVec2(x, y), ImVec2(x + cellW, y + cellH), txt,
                         i == sel.lit);
        x += cellW + gap;
    }
    return x - gap;
}

// The line counter, in the same drums the tuned-frequency counter uses. With
// no reading it shows DASHES rather than 0000: a fax that has received no
// lines and a fax nobody has asked yet are different claims, and four zeroes
// would make them look identical.
void drawLineCounter(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                     int digits, bool have, double lines) {
    const float capPx = cascade::gui::px(fonts::kTinySize) * 0.82f;
    engrave(dl, tl, "LINES", capPx);
    const float y = tl.y + capPx + cascade::gui::px(4.0f);
    // THE DRUMS ARE BUTTED, THE SELECTORS ARE SPACED, and that is the whole
    // difference between a counter and a ladder of positions at a glance:
    // spaced apart and the same width as the cells beside them, the four
    // figures read as a fifth selector. Narrower and touching, they read as
    // one mechanism, which is what they are.
    const float dW = cellW * 0.74f;
    const float groupW = static_cast<float>(digits) * dW;
    const float wellPad = cascade::gui::px(5.0f);
    const float wellPad2 = cascade::gui::px(4.0f);
    drawFreqDrumWell(dl, ImVec2(tl.x - wellPad, y - wellPad2),
                     ImVec2(tl.x + groupW + wellPad, y + cellH + wellPad2));
    char cells[fx::kDrumDigits + 1] = {0};
    int firstSig = digits;
    if (have) { fx::drumCells(fx::drumValue(lines), cells, digits, &firstSig); }
    const float px = cellH * 0.66f;
    for (int i = 0; i < digits; ++i) {
        const float x = tl.x + static_cast<float>(i) * dW;
        const char c = have ? cells[i] : '-';
        drawFreqDrumCell(dl, ImVec2(x, y), ImVec2(x + dW, y + cellH), c,
                         have && i >= firstSig, px);
    }
}

// --- the tuning meter --------------------------------------------------------
//
// CENTRE ZERO, AND ALARM AT BOTH ENDS, which is why this is drawn here rather
// than handed to drawBenchMeter: that meter's scale is a quantity with a
// comfortable bottom and an uncomfortable top, and reddening only its
// clockwise end would say a receiver 250 Hz high is in trouble and one 250 Hz
// low is fine. On a tuning indicator both ends are equally wrong and the
// middle is the only good place to be, so the ticks are red at both stops and
// the centre index is the heavy one. The materials are the bench's own -
// cream tombstone, brass bezel, engraved ticks, rust needle - so it belongs
// to the same instrument as the meters on the receiver's bar.
//
// WITH NO MEASUREMENT THERE IS NO NEEDLE. See instrument_fax_math.hpp: the
// error is measured from the phasing signal and does not exist before it, and
// a needle resting on the centre index would be the face claiming a perfectly
// tuned receiver on the strength of an empty slot.
void drawTuningMeter(ImDrawList* dl, const ImVec2& tl, float width, float height,
                     float frac01, bool haveReading, const char* valueLine) {
    if (width < cascade::gui::px(56.0f) || height < cascade::gui::px(46.0f)) { return; }
    ImFont* cf = fonts::legend();
    ImFont* vf = fonts::ui();
    const float tiny = cascade::gui::px(fonts::kTinySize);
    const char* cap = "TUNING";
    const char* val = (valueLine != nullptr) ? valueLine : "";
    const float textRoom = width - cascade::gui::px(4.0f);
    const float cpx = fitPx(cf, tiny, cap, textRoom);
    const float vpx = fitPx(vf, tiny, val, textRoom);
    const ImVec2 cs = cf->CalcTextSizeA(cpx, FLT_MAX, 0.0f, cap);
    const ImVec2 vs = vf->CalcTextSizeA(vpx, FLT_MAX, 0.0f, val);
    const float valH = (val[0] != '\0') ? vs.y : 0.0f;

    engrave(dl, ImVec2(tl.x + width * 0.5f - cs.x * 0.5f, tl.y), cap, cpx);

    const float faceTop = tl.y + cs.y + cascade::gui::px(3.0f);
    const float faceH = height - cs.y - valH - cascade::gui::px(8.0f);
    if (faceH < cascade::gui::px(22.0f)) { return; }
    const ImVec2 fTL(tl.x, faceTop);
    const ImVec2 fBR(tl.x + width, faceTop + faceH);

    dl->AddRectFilledMultiColor(fTL, fBR, IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xF3, 0xEC, 0xD6, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255),
                                IM_COL32(0xD8, 0xCF, 0xB4, 255));
    dl->AddRect(fTL, fBR, theme::kBrassBright, 3.0f, 0, 2.0f);

    const ImVec2 pivot(tl.x + width * 0.5f, fBR.y - cascade::gui::px(4.0f));
    constexpr float kHalfSweepDeg = 52.0f;
    const float armByHeight = faceH * 0.78f;
    const float reach = std::sin(kHalfSweepDeg * kPiF / 180.0f) * 0.94f;
    const float armByWidth =
        (width * 0.5f - cascade::gui::px(3.0f)) / std::max(0.01f, reach);
    const float armR = std::min(armByHeight, armByWidth);

    for (int i = 0; i < 9; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * t;
        const float a = deg * kPiF / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        const bool stop = (i <= 1 || i >= 7);
        const bool centre = (i == 4);
        const ImU32 col = stop ? theme::kAlarm : theme::kEngraved;
        const float inner = centre ? 0.72f : 0.80f;
        dl->AddLine(ImVec2(pivot.x + sx * armR * inner, pivot.y + sy * armR * inner),
                    ImVec2(pivot.x + sx * armR * 0.94f, pivot.y + sy * armR * 0.94f),
                    col, centre ? 2.4f : (stop ? 1.6f : 1.0f));
    }

    // The two ends named, so the scale says which way the needle is leaning
    // rather than leaving the sign to the line underneath.
    const float endPx = std::max(cascade::gui::px(9.0f), tiny * 0.72f);
    ImFont* ef = fonts::ui();
    const float ea = kHalfSweepDeg * kPiF / 180.0f;
    const float ex = std::sin(ea) * armR * 0.99f;
    const float ey = -std::cos(ea) * armR * 0.99f;
    const ImVec2 lo = ef->CalcTextSizeA(endPx, FLT_MAX, 0.0f, "-");
    dl->AddText(ef, endPx, ImVec2(pivot.x - ex - lo.x * 0.5f, pivot.y + ey - lo.y),
                theme::kEngraved, "-");
    const ImVec2 hi = ef->CalcTextSizeA(endPx, FLT_MAX, 0.0f, "+");
    dl->AddText(ef, endPx, ImVec2(pivot.x + ex - hi.x * 0.5f, pivot.y + ey - hi.y),
                theme::kEngraved, "+");

    if (haveReading) {
        float f = frac01;
        if (!(f >= 0.0f)) { f = 0.0f; }
        if (f > 1.0f) { f = 1.0f; }
        const float deg = -kHalfSweepDeg + 2.0f * kHalfSweepDeg * f;
        const float a = deg * kPiF / 180.0f;
        const float sx = std::sin(a);
        const float sy = -std::cos(a);
        dl->AddLine(pivot, ImVec2(pivot.x + sx * armR * 0.88f, pivot.y + sy * armR * 0.88f),
                    theme::kAlarm, 1.8f);
        dl->AddCircleFilled(pivot, cascade::gui::px(3.4f), theme::kEnamel, 12);
    } else {
        dl->AddCircleFilled(pivot, cascade::gui::px(3.4f), theme::kInkMuted, 12);
    }

    // The unit beside the pivot rather than above it: above is where the
    // needle sweeps through the only reading that matters.
    const float upx = std::max(cascade::gui::px(9.0f), tiny * 0.72f);
    const ImVec2 us = ef->CalcTextSizeA(upx, FLT_MAX, 0.0f, "Hz");
    const float ux = pivot.x + armR * 0.16f;
    if (ux + us.x < fBR.x - cascade::gui::px(3.0f)) {
        dl->AddText(ef, upx, ImVec2(ux, pivot.y - us.y - cascade::gui::px(2.0f)),
                    theme::kEngraved, "Hz");
    }

    if (val[0] != '\0') {
        dl->AddText(vf, vpx,
                    ImVec2(tl.x + width * 0.5f - vs.x * 0.5f, fBR.y + cascade::gui::px(3.0f)),
                    haveReading ? theme::kIvory : theme::kCream, val);
    }
}

// --- the paper ---------------------------------------------------------------
//
// The slot across the front of the machine, its tear bar, and whatever chart
// has come out of it. The LENGTH of the strip is the progress indication and
// the ruling creeping down it is the feed; both are driven by values[2], the
// count of lines the decoder has actually laid down. WITH NO LINES THERE IS
// NO PAPER - an empty slot, which is exactly what the machine looks like
// before a transmission starts.
void drawPaperSlot(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool have,
                   double lines) {
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    if (w < cascade::gui::px(40.0f) || h < cascade::gui::px(22.0f)) { return; }

    // The well the paper comes out into: cut into the panel, not painted on,
    // and darkest just under the lip so it reads as a cavity with a machine
    // behind it rather than as a black rectangle.
    dl->AddRectFilled(tl, br, theme::kEnamelDark, 4.0f);
    dl->AddRectFilledMultiColor(ImVec2(tl.x + 2.0f, tl.y + 2.0f),
                                ImVec2(br.x - 2.0f, br.y - 2.0f), theme::kVoid,
                                theme::kVoid, theme::withAlpha(theme::kVoid, 0.35f),
                                theme::withAlpha(theme::kVoid, 0.35f));
    addBenchBevel(dl, tl, br, 4.0f, false);

    // The exit slot and its tear bar, across the top of the well - the one
    // feature every machine in this family shares: a serrated cutter screwed
    // across a full-width slot, with the chart emerging from behind it.
    const float barH = std::min(cascade::gui::px(11.0f), h * 0.24f);
    const ImVec2 barTL(tl.x + cascade::gui::px(4.0f), tl.y + cascade::gui::px(3.0f));
    const ImVec2 barBR(br.x - cascade::gui::px(4.0f), tl.y + cascade::gui::px(3.0f) + barH);
    dl->AddRectFilled(barTL, barBR, theme::kBrassMid, 2.0f);
    addBenchBevel(dl, barTL, barBR, 2.0f, true);
    // The serrations of the cutting edge, along the bar's lower lip.
    const float toothPitch = cascade::gui::px(6.0f);
    for (float x = barTL.x + 2.0f; x < barBR.x - 2.0f; x += toothPitch) {
        dl->AddTriangleFilled(ImVec2(x, barBR.y),
                              ImVec2(std::min(x + toothPitch * 0.5f, barBR.x), barBR.y),
                              ImVec2(x, barBR.y + 2.0f),
                              theme::withAlpha(theme::kBrassTint, 0.55f));
    }
    // The two round-head screws that hold it on, one at each end.
    const float screwR = std::min(cascade::gui::px(3.4f), barH * 0.34f);
    if (screwR >= 2.0f) {
        const float scy = (barTL.y + barBR.y) * 0.5f;
        addCabinetScrew(dl, ImVec2(barTL.x + screwR + cascade::gui::px(3.0f), scy), screwR,
                        24.0f);
        addCabinetScrew(dl, ImVec2(barBR.x - screwR - cascade::gui::px(3.0f), scy), screwR,
                        -62.0f);
    }

    const float paperTop = barBR.y + 1.0f;
    const float room = br.y - cascade::gui::px(5.0f) - paperTop;
    if (room < cascade::gui::px(6.0f)) { return; }
    const float frac = have ? fx::paperFrac(lines) : 0.0f;
    if (!(frac > 0.0f)) { return; }
    // A PEDESTAL UNDER THE PROGRESS, and only once there is progress to show.
    // The first lines of a chart are a strip two pixels deep, which reads as
    // a scratch on the panel rather than as paper; five pixels is the least
    // that reads as a sheet, and it is added only when the count is genuinely
    // above zero, so an empty machine still shows an empty slot.
    const float pedestal = cascade::gui::px(5.0f);
    const float len = pedestal + frac * std::max(0.0f, room - pedestal);

    const ImVec2 pTL(tl.x + cascade::gui::px(14.0f), paperTop);
    const ImVec2 pBR(br.x - cascade::gui::px(14.0f), paperTop + len);
    if (pBR.x - pTL.x < cascade::gui::px(12.0f)) { return; }

    // The chart. Paper white at the lip, going to the cream of a thermal roll
    // further down, with the slot's own shadow across the top of it.
    dl->AddRectFilledMultiColor(pTL, pBR, IM_COL32(0xF6, 0xF1, 0xE2, 255),
                                IM_COL32(0xF6, 0xF1, 0xE2, 255),
                                IM_COL32(0xDE, 0xD6, 0xBE, 255),
                                IM_COL32(0xDE, 0xD6, 0xBE, 255));
    const float shade = std::min(cascade::gui::px(6.0f), len * 0.4f);
    dl->AddRectFilledMultiColor(pTL, ImVec2(pBR.x, pTL.y + shade),
                                IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    // The ruling, sliding down as lines arrive. One rule per fifty lines is
    // the same interval the plugin's own log reports at, so a rule crossing
    // the lip and a row appearing in the memory below are the same event.
    dl->PushClipRect(pTL, pBR, true);
    const float pitch = cascade::gui::px(7.0f);
    const float off = fx::ruleOffset(lines, pitch, 50.0);
    const float ruleInset = cascade::gui::px(4.0f);
    for (float y = pTL.y + off; y < pBR.y; y += pitch) {
        dl->AddLine(ImVec2(pTL.x + ruleInset, y), ImVec2(pBR.x - ruleInset, y),
                    IM_COL32(0x6E, 0x65, 0x52, 64), 1.0f);
    }
    dl->PopClipRect();

    // The sheet's own edges: a hairline of shade down each side, which is
    // what stops a pale rectangle from looking painted on the panel.
    dl->AddLine(ImVec2(pTL.x, pTL.y), ImVec2(pTL.x, pBR.y), IM_COL32(0, 0, 0, 90), 1.0f);
    dl->AddLine(ImVec2(pBR.x, pTL.y), ImVec2(pBR.x, pBR.y), IM_COL32(0, 0, 0, 90), 1.0f);

    // The cut edge, and the shadow it throws into the well.
    dl->AddLine(ImVec2(pTL.x, pBR.y), ImVec2(pBR.x, pBR.y),
                IM_COL32(0x9C, 0x90, 0x78, 220), 1.0f);
    dl->AddRectFilledMultiColor(ImVec2(pTL.x, pBR.y), ImVec2(pBR.x, pBR.y + 5.0f),
                                IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
}

}  // namespace

float drawFaxFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                  const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float hAll = br.y - tl.y;
    if (w < cascade::gui::px(80.0f) || hAll < cascade::gui::px(60.0f)) { return 0.0f; }

    float y = addBenchPlate(dl, tl, br, in.title.c_str());

    const float bx0 = tl.x + cascade::gui::px(10.0f);
    const float bx1 = br.x - cascade::gui::px(10.0f);
    const float by0 = y + cascade::gui::px(4.0f);
    const float by1 = br.y - cascade::gui::px(8.0f);
    // fx::layoutAtScale, not fx::layout: the deck's own ceilings and floors -
    // 210 px, 420 px, the meter's 104..168 - are the desktop's own pixels,
    // and left unscaled they stop the equipment growing once the tablet's
    // much larger body has room to spare. See instrument_fax_math.hpp.
    const fx::Layout L = fx::layoutAtScale(bx1 - bx0, by1 - by0);
    if (!L.ok) { return y - tl.y; }

    // --- what the plugin actually said ---------------------------------------
    //
    // Read once, here, and NOTHING below invents any of it. An absent word is
    // an absent lamp; an untouched value slot is an aperture with no figure.
    const bool have = in.have;
    const int phase = have ? fx::phaseIndex(in.state.text[0]) : fx::kPhaseUnknown;
    const bool lock = have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u;
    const double ioc = have ? in.state.values[0] : 0.0;
    const double lpm = have ? in.state.values[1] : 0.0;
    const double lines = have ? in.state.values[2] : 0.0;
    const double offsetHz = have ? in.state.values[3] : 0.0;
    const bool haveTuning = have && fx::tuningIsMeasured(phase);
    // A COUNT OF ZERO IS NOT A READING. plugin_abi.h's own rule for this
    // contract is that an untouched slot arrives as a zero and must be drawn
    // as "no reading", so the counter shows dashes and the slot stays empty
    // until a line has actually landed - which is also the truthful reading
    // for a machine that has received nothing.
    const bool haveLines = have && lines > 0.0;

    // --- the control deck ----------------------------------------------------
    const ImVec2 dTL(bx0, by0);
    const ImVec2 dBR(bx1, by0 + L.deckH);
    addScopeBay(dl, dTL, dBR, false);
    const float ix0 = dTL.x + cascade::gui::px(10.0f);
    const float ix1 = dBR.x - cascade::gui::px(10.0f);
    float iy = dTL.y + cascade::gui::px(8.0f);
    const float iyEnd = dBR.y - cascade::gui::px(8.0f);
    const float col60 = cascade::gui::px(60.0f);

    float leftX1 = ix1;
    if (L.meterW > 0.0f) {
        char offText[24];
        if (haveTuning) {
            fx::formatOffset(offsetHz, offText, sizeof offText);
        } else {
            std::snprintf(offText, sizeof offText, "--");
        }
        const float mx0 = ix1 - L.meterW;
        drawTuningMeter(dl, ImVec2(mx0, iy), L.meterW, iyEnd - iy,
                        fx::tuningFrac(offsetHz), haveTuning, offText);
        addBenchDivider(dl, mx0 - cascade::gui::px(9.0f), iy, iyEnd);
        leftX1 = mx0 - cascade::gui::px(18.0f);
    }
    const float leftW = leftX1 - ix0;

    if (L.groupCaption && leftW > col60) {
        addBenchGroupCaption(dl, ImVec2(ix0, iy), leftW, "RECEPTION");
        iy += cascade::gui::px(fonts::kTinySize) + cascade::gui::px(6.0f);
    }

    // The phase ladder. Exactly one lamp lights, and only for a word this
    // host knows - a transmission that has not started yet, or a plugin
    // sending something else, leaves every lamp cold.
    // NEW and LOCK stand at the right-hand end of the same row, behind a
    // divider: they report on the WINDOW and the link rather than on the
    // machine, and the groove is what says so.
    const int statusCols = L.statusLamps ? 2 : 0;
    if (L.lampLadder && leftW > col60) {
        const int cols = fx::kPhaseCount + statusCols;
        const float pitch = leftW / static_cast<float>(cols);
        const float lampR = std::min(cascade::gui::px(7.0f), pitch * 0.16f);
        const float floorPx = cascade::gui::px(9.0f);
        float capPx = std::max(floorPx, std::min(cascade::gui::px(fonts::kTinySize), pitch * 0.30f));
        // THE WIDEST WORD MUST FIT ITS PITCH. pitch * 0.30 was a proportion
        // fitted to a condensed face; in Georgia (0.84.0) PHASING at that
        // size ran into PICTURE on either side. Measure the longest phase
        // name at the chosen size and take the size down, never below the
        // nine-pixel floor, until it sits inside the pitch with clear metal.
        {
            ImGui::PushFont(fonts::ui(), capPx);
            float widest = 0.0f;
            for (int i = 0; i < fx::kPhaseCount; ++i) {
                widest = std::max(widest, ImGui::CalcTextSize(fx::phaseName(i)).x);
            }
            widest = std::max(widest, ImGui::CalcTextSize("LOCK").x);
            ImGui::PopFont();
            const float room = pitch - cascade::gui::px(6.0f);
            if (widest > room && widest > 0.0f) {
                capPx = std::max(floorPx, capPx * room / widest);
            }
        }
        const float lampGap = cascade::gui::px(2.0f);
        const float rowH = lampR * 2.0f + capPx * 1.40f + cascade::gui::px(4.0f);
        ImGui::PushFont(fonts::ui(), capPx);
        for (int i = 0; i < fx::kPhaseCount; ++i) {
            const ImVec2 c(ix0 + pitch * (static_cast<float>(i) + 0.5f), iy + lampR + lampGap);
            // PICTURE is the phosphor one: it is the only position on the
            // ladder that means the radio is putting a picture on paper.
            const ImU32 col = (i == fx::kPhasePicture) ? theme::kPhosphor : theme::kGold;
            drawBenchLamp(dl, c, lampR, col, i == phase, fx::phaseName(i));
        }
        if (statusCols > 0) {
            addBenchDivider(dl, ix0 + pitch * static_cast<float>(fx::kPhaseCount), iy,
                            iy + rowH);
            const ImVec2 cn(ix0 + pitch * (static_cast<float>(fx::kPhaseCount) + 0.5f),
                            iy + lampR + lampGap);
            drawBenchLamp(dl, cn, lampR, theme::kGold, cue.unread, "NEW");
            const ImVec2 cl(ix0 + pitch * (static_cast<float>(fx::kPhaseCount) + 1.5f),
                            iy + lampR + lampGap);
            drawBenchLamp(dl, cl, lampR, theme::kPhosphor, lock, "LOCK");
        }
        ImGui::PopFont();
        iy += rowH + cascade::gui::px(4.0f);
    } else if (leftW > col60) {
        // Too narrow for the ladder: the same fact in fewer pixels, the phase
        // word itself on glass, and a blank well when there is no word.
        const float wellH = std::min(cascade::gui::px(26.0f), (iyEnd - iy) * 0.42f);
        const float wellCap = cascade::gui::px(150.0f);
        const float wellW = statusCols > 0 ? std::min(leftW - cascade::gui::px(116.0f), wellCap)
                                           : std::min(leftW, wellCap);
        const ImVec2 wTL(ix0, iy);
        const ImVec2 wBR(ix0 + wellW, iy + wellH);
        drawFreqDrumWell(dl, wTL, wBR);
        if (phase != fx::kPhaseUnknown) {
            ImFont* f = fonts::ui();
            const char* s = fx::phaseName(phase);
            const float wordPx =
                fitPx(f, wellH * 0.62f, s, wBR.x - wTL.x - cascade::gui::px(8.0f));
            const ImVec2 sz = f->CalcTextSizeA(wordPx, FLT_MAX, 0.0f, s);
            dl->AddText(f, wordPx,
                        ImVec2((wTL.x + wBR.x) * 0.5f - sz.x * 0.5f,
                               (wTL.y + wBR.y) * 0.5f - sz.y * 0.5f),
                        theme::kPhosphor, s);
        }
        if (statusCols > 0) {
            const float lampR = cascade::gui::px(6.0f);
            const float lampGap = cascade::gui::px(2.0f);
            const float capPx = cascade::gui::px(fonts::kTinySize) * 0.82f;
            ImGui::PushFont(fonts::ui(), capPx);
            drawBenchLamp(dl, ImVec2(wBR.x + cascade::gui::px(32.0f), iy + lampR + lampGap),
                          lampR, theme::kGold, cue.unread, "NEW");
            drawBenchLamp(dl, ImVec2(wBR.x + cascade::gui::px(88.0f), iy + lampR + lampGap),
                          lampR, theme::kPhosphor, lock, "LOCK");
            ImGui::PopFont();
        }
        iy += wellH + cascade::gui::px(6.0f);
    }

    // The selectors and the counter, along the bottom of the deck.
    const float rowH = iyEnd - iy;
    if (rowH >= cascade::gui::px(26.0f) && leftW > col60) {
        const float capPx = cascade::gui::px(fonts::kTinySize) * 0.82f;
        const float cellH = std::min(cascade::gui::px(24.0f), rowH - capPx - cascade::gui::px(4.0f));
        if (cellH >= cascade::gui::px(12.0f)) {
            const fx::Selector iocSel = fx::iocSelector(ioc, have);
            const fx::Selector lpmSel = fx::lpmSelector(lpm, have);
            // Every cell the same width, so the two ladders and the counter
            // read as one row of apertures rather than three sizes of window.
            const int cells = iocSel.count + lpmSel.count + L.drumDigits;
            const int groups = (L.drumDigits > 0) ? 3 : 2;
            const float groupGap = cascade::gui::px(14.0f);
            const float cellGap = cascade::gui::px(3.0f);
            const float spare = leftW - static_cast<float>(groups - 1) * groupGap -
                                static_cast<float>(cells - groups) * cellGap;
            float cellW = spare / static_cast<float>(cells);
            const float cellWCap = cascade::gui::px(32.0f);
            if (cellW > cellWCap) { cellW = cellWCap; }
            if (cellW >= cascade::gui::px(11.0f)) {
                float x = ix0;
                x = drawSelector(dl, ImVec2(x, iy), cellW, cellH, "IOC", iocSel) + groupGap;
                x = drawSelector(dl, ImVec2(x, iy), cellW, cellH, "LPM", lpmSel) + groupGap;
                if (L.drumDigits > 0) {
                    drawLineCounter(dl, ImVec2(x + cellGap, iy), cellW, cellH, L.drumDigits,
                                    haveLines, lines);
                }
            }
        }
    }

    // --- the paper slot ------------------------------------------------------
    const float paperGap = cascade::gui::px(6.0f);
    const float bottomGap = cascade::gui::px(8.0f);
    if (L.paperH > 0.0f) {
        drawPaperSlot(dl, ImVec2(bx0, dBR.y + paperGap), ImVec2(bx1, dBR.y + paperGap + L.paperH),
                      haveLines, lines);
        return dBR.y + paperGap + L.paperH + bottomGap - tl.y;
    }
    return dBR.y + bottomGap - tl.y;
}

}  // namespace cascade::gui
