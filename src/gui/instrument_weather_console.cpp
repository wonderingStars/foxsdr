// instrument_weather_console.cpp - the CASCADE_INSTRUMENT_WEATHER_CONSOLE
// face: a home weather station's base unit.
//
// THE EQUIPMENT. A late-1990s/2000s Oregon Scientific multi-channel base unit
// of the BAR388HG / BAR628HG family - the silver-and-grey plastic desk console
// that ships with a THGR122NX or THN132N outdoor sensor, which is exactly what
// the plugin behind this window decodes. The whole front of it is one large
// segmented LCD divided into fixed rectangular zones separated by rules etched
// into the mask, each zone carrying its own reading, and around it a moulded
// case with a fold-out stand.
//
// Reference, all read for this face: the manufacturer's own factory manuals
// for the BAR388HG (docs.rs-online.com/5710 and /b69b), the BAR628HG - the one
// that ships with the THGR122NX - the WMR86 and the WMR968 Cable Free, plus
// photographs of a real WMR80 and WMR100 console and its LCD close up
// (commons.wikimedia.org). Design reference only; every line below is written
// here.
//
// WHAT THOSE MANUALS SETTLED, and it is worth writing down because three of
// them contradict what "everybody knows" about these consoles:
//
//   THE FIELD IS THREE AND A HALF DIGITS. The all-segments artwork reads
//   "-188.8" - a dedicated minus bar, a half digit that can only form a 1, two
//   whole digits, the point, the tenths. So the face holds -199.9 to 199.9 and
//   the cells here are that field exactly.
//
//   A LOST SENSOR SHOWS DASHES. The BAR388HG prints it as "- - . -" and the
//   BAR628HG as "- - -", described as "the sensor cannot be found". Rule two
//   of instrument_face.hpp, arrived at independently by somebody selling a
//   thermometer.
//
//   OUT OF RANGE IS "LLL" OR "HHH", from the BAR628HG's troubleshooting page -
//   which is a DIFFERENT statement from the dashes, and this face keeps them
//   different.
//
//   THERE IS NO "CH" AND NO "RH" ANYWHERE ON THE GLASS. The channel is a
//   pictogram - a sensor with one, two or three arcs radiating from it - and
//   humidity carries a bare "%". Both are drawn here as the equipment draws
//   them; the channel's digit beside the pictogram is this face's own addition
//   and the next paragraph says why.
//
//   THE REAL CONSOLE SHOWS ONE CHANNEL AT A TIME. Every manual read says the
//   CHANNEL key cycles 1-2-3, with an auto-scan holding each for three or four
//   seconds. THIS FACE SHOWS ALL THREE AT ONCE and is therefore a composite,
//   not a copy: a window on a bench has no CHANNEL key to press and a reading
//   that appears for three seconds in nine is not a reading anybody can use.
//   The compartments are the WMR968's separately-addressed display windows
//   laid side by side, and the channel digit is there because three
//   compartments on one glass need telling apart in a way one never did.
//
//   WHAT IS DELIBERATELY ABSENT. The equipment's compartments also carry a
//   temperature TREND ARROW, a humidity trend arrow, a reception icon and
//   MAX/MIN legends. The slot map carries none of those, and a trend drawn
//   from a single reading, or a signal bar with nothing measuring signal,
//   would be exactly the invented figure this application's faces are
//   forbidden. They are left off rather than guessed at.
//
// TRANSLATED ONTO THE BENCH. The real housing is white and silver injection
// moulding, which would sit on this bench like a plastic ruler in a drawer of
// brass instruments. So the case is the bench's own metal - a brass-toned
// moulding with a bevelled edge and four countersunk screws, the LCD sunk into
// a dark enamel bezel - and only the GLASS is reproduced literally, because
// the glass is the part that carries meaning:
//
//   A NEMATIC LCD IS DARK-ON-PALE, which is the one place this file departs
//   from theme.hpp's "a live figure is on glass" rule, and it departs from it
//   deliberately and in the safe direction. The bench's rule exists because a
//   caption engraved dark into brass measures about 2.3:1 and a figure has to
//   do better than that. Dark segments on this face's grey-green polariser
//   measure about 5.8:1 - better than the rule was written to protect - so the
//   reading is MORE legible here, not less, and it looks like the equipment
//   instead of like a phosphor tube pretending to be a thermometer. Everything
//   engraved on the metal around the glass still follows the bench exactly.
//
//   THE GHOST SEGMENTS ARE NOT DECORATION. A real LCD's unlit segments are
//   faintly visible against the polariser, and drawing them is what makes a
//   blank cell read as an empty digit position rather than as nothing at all -
//   which is precisely the distinction rule two of instrument_face.hpp is
//   about. A channel with no sensor shows its dashes sitting in a full set of
//   ghosts, so the eye can see there is a place for a reading and no reading
//   in it.
//
// THE SLOT MAP, from plugin_abi.h's CASCADE_INSTRUMENT_WEATHER_CONSOLE:
//   values[0..2] temperature C on channels 1..3
//   values[3..5] humidity % on the same channels
//   values[6]    channel mask, bit n-1 set when channel n has a reading
//   values[7]    low-battery mask
//   text[0..2]   sensor model per channel
//   flags        LOW_BATT when any sensor says so
// Nothing else is read, and a channel outside the mask is drawn blank however
// tempting the zero sitting in its temperature slot looks. The arithmetic that
// enforces that is in instrument_weather_console_math.hpp, where it is tested.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "gui/fonts.hpp"
#include "gui/instrument_weather_console_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

namespace {

using namespace cascade::gui::wxface;

// --- the glass ---------------------------------------------------------------
//
// These four tones are the only colours in this application that are not in
// theme.hpp, and the header above says why: the bench has no role for "a pale
// polariser with dark segments on it" because nothing else on the bench is a
// liquid-crystal display. They are named for what they are so a later reader
// can see they are one material and not four strays.
constexpr ImU32 kGlassTop = IM_COL32(0x9A, 0xA6, 0x89, 0xFF);
constexpr ImU32 kGlassBottom = IM_COL32(0x84, 0x90, 0x77, 0xFF);
constexpr ImU32 kGlassInk = IM_COL32(0x15, 0x19, 0x11, 0xFF);
// The unlit segment: the same ink at the alpha a real cell's off state shows
// against its polariser. Low enough not to be read as a figure, high enough
// that the empty cell is visibly a cell.
// MEASURED DOWN FROM 30/255 AFTER LOOKING AT IT. A compartment is a third
// of the glass, so a digit gets about forty points of height, and at that
// size a ghost at 30 was reading as a lit segment's neighbour - a 1 with its
// unlit segments showing came out as a blurred 8. Eighteen keeps the empty
// cell visible as a cell, which is the job, without competing with the ink.
constexpr ImU32 kGlassGhost = IM_COL32(0x15, 0x19, 0x11, 0x12);

// A live LCD is very slightly italic, which is most of what separates a real
// segment display from a font pretending to be one.
constexpr float kSlant = 0.075f;

// --- segment primitives ------------------------------------------------------
//
// Both bars are hexagons rather than rectangles: a segment's ends are mitred
// so the corner between two lit segments closes cleanly, and square ends leave
// a visible notch at every join.

void hbar(ImDrawList* dl, float x0, float x1, float yc, float t, float shearBase,
          ImU32 col) {
    if (x1 - x0 < t) { return; }
    const float sh = (shearBase - yc) * kSlant;
    const float h = t * 0.5f;
    const ImVec2 p[6] = {ImVec2(x0 + sh, yc),          ImVec2(x0 + h + sh, yc - h),
                         ImVec2(x1 - h + sh, yc - h),  ImVec2(x1 + sh, yc),
                         ImVec2(x1 - h + sh, yc + h),  ImVec2(x0 + h + sh, yc + h)};
    dl->AddConvexPolyFilled(p, 6, col);
}

void vbar(ImDrawList* dl, float xc, float y0, float y1, float t, float shearBase,
          ImU32 col) {
    if (y1 - y0 < t) { return; }
    const float h = t * 0.5f;
    const float s0 = (shearBase - y0) * kSlant;
    const float s1 = (shearBase - y1) * kSlant;
    const float sm = (shearBase - (y0 + y1) * 0.5f) * kSlant;
    const ImVec2 p[6] = {ImVec2(xc + s0, y0),
                         ImVec2(xc + h + sm, y0 + h),
                         ImVec2(xc + h + sm, y1 - h),
                         ImVec2(xc + s1, y1),
                         ImVec2(xc - h + sm, y1 - h),
                         ImVec2(xc - h + sm, y0 + h)};
    dl->AddConvexPolyFilled(p, 6, col);
}

// One cell of the display. `segs` is the mask from segmentsFor(); every
// segment is drawn, lit ones in the ink and the rest as ghosts, which is what
// the equipment does and what makes an empty position legible as one.
void drawCell(ImDrawList* dl, float x, float y, float w, float h, unsigned segs,
              bool ghosts) {
    const float t = std::max(2.0f, h * 0.135f);
    const float gap = t * 0.55f;
    const float xL = x + t * 0.5f;
    const float xR = x + w - t * 0.5f;
    const float yT = y + t * 0.5f;
    const float yM = y + h * 0.5f;
    const float yB = y + h - t * 0.5f;
    const float base = y + h;  // the shear pivots on the baseline
    if (xR - xL < t * 1.5f || yB - yT < t * 1.5f) { return; }

    struct Seg {
        unsigned bit;
        bool horizontal;
        float a, b, c;  // horizontal: x0, x1, yc; vertical: xc, y0, y1
    };
    const Seg segments[7] = {
        {kSegA, true, xL + gap, xR - gap, yT},
        {kSegG, true, xL + gap, xR - gap, yM},
        {kSegD, true, xL + gap, xR - gap, yB},
        {kSegF, false, xL, yT + gap, yM - gap},
        {kSegB, false, xR, yT + gap, yM - gap},
        {kSegE, false, xL, yM + gap, yB - gap},
        {kSegC, false, xR, yM + gap, yB - gap},
    };
    for (const Seg& s : segments) {
        const bool on = (segs & s.bit) != 0u;
        if (!on && !ghosts) { continue; }
        const ImU32 col = on ? kGlassInk : kGlassGhost;
        if (s.horizontal) {
            hbar(dl, s.a, s.b, s.c, t, base, col);
        } else {
            vbar(dl, s.a, s.b, s.c, t, base, col);
        }
    }
}

// The decimal point, in its own narrow cell, sitting on the baseline.
void drawPoint(ImDrawList* dl, float x, float y, float w, float h, bool lit) {
    const float t = std::max(2.0f, h * 0.135f);
    const float cy = y + h - t * 0.5f;
    const float cx = x + w * 0.5f + (h * 0.5f) * kSlant;
    dl->AddRectFilled(ImVec2(cx - t * 0.5f, cy - t * 0.5f), ImVec2(cx + t * 0.5f, cy + t * 0.5f),
                      lit ? kGlassInk : kGlassGhost, t * 0.25f);
}

// The degree ring and its C, drawn the way the glass draws them: a small open
// square-ish ring and a four-segment C, not a typeset "°C".
void drawDegreeC(ImDrawList* dl, float x, float y, float h) {
    const float t = std::max(1.5f, h * 0.11f);
    const float r = h * 0.15f;
    // The ring sits at the TOP of the legend and the C beneath it, clear of
    // each other: crowded together the two read as one broken glyph.
    const float cx = x + r + t * 0.5f;
    const float cy = y + r + t;
    dl->AddCircle(ImVec2(cx + (h * 0.62f) * kSlant, cy), r, kGlassInk, 14, t);
    const float ch = h * 0.50f;
    const float cw = h * 0.46f;
    drawCell(dl, x, y + h - ch, cw, ch, segmentsFor('C'), false);
}

// The reception pictogram these consoles use INSTEAD of the letters "CH":
// arcs radiating from a sensor, one arc for channel 1, two for channel 2,
// three for channel 3. The channel's digit is lettered beside it, which the
// equipment does not do - it has no need to, because it only ever shows one
// channel at a time - and this face does because it shows all three at once.
void drawChannelGlyph(ImDrawList* dl, float x, float y, float h, int channel, bool live) {
    const ImU32 col = live ? kGlassInk
                           : ((kGlassInk & 0x00FFFFFFu) | (0x55u << IM_COL32_A_SHIFT));
    const float t = std::max(1.2f, h * 0.10f);
    const float cx = x + t;
    const float cy = y + h * 0.5f;
    dl->AddRectFilled(ImVec2(cx - t, cy - t), ImVec2(cx + t, cy + t), col, t * 0.4f);
    for (int i = 1; i <= 3; ++i) {
        const float r = h * (0.18f * static_cast<float>(i) + 0.10f);
        const bool on = i <= channel;
        dl->PathArcTo(ImVec2(cx, cy), r, -0.9f, 0.9f, 10);
        dl->PathStroke(on ? col
                          : ((kGlassInk & 0x00FFFFFFu) | (0x1Eu << IM_COL32_A_SHIFT)),
                       ImDrawFlags_None, t);
    }
}

// The battery symbol the console shows inside a channel's compartment when
// that sensor reports its cells are low. DRAWN ONLY WHEN IT IS TRUE: a
// permanently-present outline with nothing in it would be one more thing to
// interpret, and this is a warning, not a gauge.
void drawBatteryLow(ImDrawList* dl, float x, float y, float w, float h) {
    if (w < 8.0f || h < 5.0f) { return; }
    const float bodyW = w - w * 0.16f;
    dl->AddRect(ImVec2(x, y), ImVec2(x + bodyW, y + h), kGlassInk, 1.0f, 0, 1.4f);
    dl->AddRectFilled(ImVec2(x + bodyW, y + h * 0.28f), ImVec2(x + w, y + h * 0.72f),
                      kGlassInk, 0.5f);
    // One bar left of three: an empty battery outline reads as "no battery",
    // and a full one would say the opposite of what the flag means.
    dl->AddRectFilled(ImVec2(x + 2.0f, y + 2.0f), ImVec2(x + 2.0f + (bodyW - 4.0f) * 0.3f,
                                                        y + h - 2.0f),
                      kGlassInk, 0.5f);
}

// Small lettering on the glass: the panel captions the console silk-screens
// onto its own polariser (CH 1, %RH, the sensor's name). The UI face, in the
// glass ink, so it belongs to the display rather than to the metal.
float glassText(ImDrawList* dl, float x, float y, const char* s, float px, float alpha,
                float maxX) {
    ImFont* f = fonts::ui();
    const ImU32 col = (kGlassInk & 0x00FFFFFFu) |
                      (static_cast<ImU32>(std::lround(255.0f * alpha)) << IM_COL32_A_SHIFT);
    // ONE POINT OF SLACK, and it is not a fudge. A caller that measures a
    // string with CalcTextSizeA and then lays it out flush against maxX hands
    // this function a wrap width equal to the string's own width, and ImGui
    // wraps on >= rather than > - so "2/3" came out as "2/" with the 3 on the
    // line below, over the rule beneath it. The slack is smaller than a pixel
    // of ink and removes the tie.
    const float wrap = maxX - x + 1.0f;
    if (wrap <= 1.0f) { return 0.0f; }
    dl->AddText(f, px, ImVec2(x, y), col, s, nullptr, wrap);
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
    return std::min(sz.x, wrap);
}

// The console's clock, which is the one thing on this face that is not the
// plugin's: it is the machine's own wall clock, exactly as the base unit's
// clock strip is the console's own. Recomputed once a second rather than once
// a frame - localtime consults the time zone and this is a frame loop.
const char* consoleClock(double nowSec, bool* colonOn) {
    static char text[8] = "--:--";
    static double lastAt = -1000.0;
    if (nowSec - lastAt >= 1.0 || lastAt < 0.0) {
        lastAt = nowSec;
        const std::time_t t = std::time(nullptr);
        std::tm tmv;
        std::memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
        const bool ok = (localtime_s(&tmv, &t) == 0);
#else
        const bool ok = (localtime_r(&t, &tmv) != nullptr);
#endif
        if (ok) {
            std::snprintf(text, sizeof(text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            std::snprintf(text, sizeof(text), "--:--");
        }
    }
    // The colon blinks at 1 Hz, which is what a console clock does and what
    // says the display is live rather than frozen.
    if (colonOn != nullptr) { *colonOn = std::fmod(nowSec, 1.0) < 0.5; }
    return text;
}

// --- one channel compartment -------------------------------------------------

void drawChannelPanel(ImDrawList* dl, float x0, float x1, float y0, float y1, int channel,
                      const cascade::core::HostInstrument& in, const PanelStyle& st) {
    const double mask = in.have ? in.state.values[6] : 0.0;
    const double lowMask = in.have ? in.state.values[7] : 0.0;
    const bool reporting = channelReporting(mask, channel);
    const bool lowBatt = channelLowBattery(mask, lowMask, channel);
    const double tempC = in.have ? in.state.values[channel - 1] : 0.0;
    const double hum = in.have ? in.state.values[3 + channel - 1] : 0.0;

    // THE STACK IS CENTRED IN ITS COMPARTMENT, not hung from the top. The
    // face takes the whole window when the plugin has no memory feed, and a
    // tall window then left four rows of glass at the top and half a hand of
    // empty polariser below them. The height is the sum of what is actually
    // going to be drawn, so a compartment that has given up its humidity row
    // recentres on what is left rather than keeping a hole where it was.
    const float stackH = panelStackHeight(st);
    float y = y0 + std::max(0.0f, (y1 - y0 - stackH) * 0.5f);

    // The compartment's own header: the channel number, always, so an empty
    // compartment still says WHICH channel is empty; and the battery symbol
    // where the flag is set.
    if (st.headerPx > 0.0f) {
        char ch[8];
        std::snprintf(ch, sizeof ch, "%d", channel);
        const float glyphW = st.headerPx * 0.95f;
        drawChannelGlyph(dl, x0 + 2.0f, y, st.headerPx, channel, reporting);
        glassText(dl, x0 + 2.0f + glyphW + 3.0f, y, ch, st.headerPx,
                  reporting ? 0.95f : 0.55f, x1);
        if (lowBatt) {
            const float bh = st.headerPx * 0.62f;
            const float bw = bh * 1.9f;
            drawBatteryLow(dl, x1 - bw - 2.0f, y + (st.headerPx - bh) * 0.5f, bw, bh);
        }
        y += st.headerPx + kHeaderGap;
    }

    // The temperature, in the compartment's large digits.
    const TempCells t = temperatureCells(reporting, tempC);
    if (st.digitH > 0.0f) {
        const float cell = digitCellWidth(st.digitH);
        const float pitch = digitPitch(st.digitH);
        const float signW = cell * 0.50f;
        const float halfW = cell * 0.50f;
        const float pointW = cell * 0.30f;
        const float degW = cell * 0.58f;
        const float total =
            signW + halfW + cell * 3.0f + pointW + degW + pitch * 5.0f;
        float x = x0 + std::max(0.0f, (x1 - x0 - total) * 0.5f);
        // The minus bar has its own cell and is lit only for a negative
        // reading. It is drawn without ghosts, because on the equipment it is
        // a bar and not a segment of a digit - and because a permanent faint
        // minus in front of every positive reading would be a thing to
        // misread.
        if (t.negative) {
            drawCell(dl, x, y, signW + cell * 0.25f, st.digitH, kSegG, false);
        }
        x += signW + pitch;
        // The half digit: the b and c strokes only, so it can form a 1 and
        // nothing else, which is what a 3.5-digit field is.
        drawCell(dl, x - halfW * 0.5f, y, cell, st.digitH,
                 (t.hundreds == '1') ? (kSegB | kSegC) : 0u, false);
        x += halfW + pitch;
        drawCell(dl, x, y, cell, st.digitH, segmentsFor(t.tens), true);
        x += cell + pitch;
        drawCell(dl, x, y, cell, st.digitH, segmentsFor(t.units), true);
        x += cell;
        // The point is lit for a real figure. HHH and LLL are a message, not a
        // number, so nothing about them carries a decimal.
        drawPoint(dl, x, y, pointW, st.digitH, !t.blank && !t.over);
        x += pointW;
        drawCell(dl, x, y, cell, st.digitH, segmentsFor(t.tenths), true);
        x += cell + pitch;
        drawDegreeC(dl, x, y, st.digitH);
        y += st.digitH + kDigitGap;
    }

    // The humidity, in the compartment's small digits, with its own legend.
    if (st.humH > 0.0f) {
        const HumCells h = humidityCells(reporting, hum);
        const float dh = st.humH;
        const float cw = digitCellWidth(dh);
        const float pitch = dh * 0.16f;
        ImFont* f = fonts::ui();
        const float legendPx = std::max(11.0f, dh * 0.62f);
        const float legendW = f->CalcTextSizeA(legendPx, FLT_MAX, 0.0f, "%").x;
        const float total = cw * 3.0f + pitch * 3.0f + legendW;
        float x = x0 + std::max(0.0f, (x1 - x0 - total) * 0.5f);
        drawCell(dl, x, y, cw, dh, segmentsFor(h.hundreds), h.hundreds != ' ');
        x += cw + pitch;
        drawCell(dl, x, y, cw, dh, segmentsFor(h.tens), true);
        x += cw + pitch;
        drawCell(dl, x, y, cw, dh, segmentsFor(h.units), true);
        x += cw + pitch;
        glassText(dl, x, y + dh - legendPx, "%", legendPx, h.blank ? 0.4f : 0.85f, x1);
        y += dh + kHumGap;
    }

    // The sensor's own name, printed under its compartment. Only where there
    // IS a sensor: a model name under an empty compartment would say the
    // console has heard something it has not.
    if (st.modelPx > 0.0f && reporting && y + st.modelPx <= y1) {
        const char* model = in.state.text[channel - 1];
        if (model[0] != '\0') {
            ImFont* f = fonts::ui();
            const float per = std::max(3.0f, f->CalcTextSizeA(st.modelPx, FLT_MAX, 0.0f, "n").x);
            const std::size_t fits =
                static_cast<std::size_t>(std::max(0.0f, (x1 - x0 - 4.0f) / per));
            char label[CASCADE_INSTRUMENT_TEXT_CHARS];
            fitLabel(model, label, sizeof label, fits);
            if (label[0] != '\0') {
                const float w = f->CalcTextSizeA(st.modelPx, FLT_MAX, 0.0f, label).x;
                glassText(dl, x0 + std::max(0.0f, (x1 - x0 - w) * 0.5f), y, label, st.modelPx,
                          0.72f, x1);
            }
        }
    }
}

}  // namespace

float drawWeatherConsoleFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                             const cascade::core::HostInstrument& in,
                             const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    // Below this there is no drawing that would be honest at the size - so
    // nothing is drawn and nothing is claimed, rather than a smear over the
    // edge of a window somebody has dragged shut.
    if (w < 120.0f || h < 70.0f) { return 0.0f; }

    // The window's own plate, titled by the plugin.
    const float plateBottom = addBenchPlate(dl, tl, br, in.title.c_str());

    // --- the console's case ---------------------------------------------------
    const float hx0 = tl.x + 8.0f;
    const float hx1 = br.x - 8.0f;
    const float hy0 = plateBottom + 4.0f;
    const float hy1 = br.y - 8.0f;
    if (hx1 - hx0 < 90.0f || hy1 - hy0 < 50.0f) { return br.y - tl.y; }

    const float round = theme::kPanelRounding + 2.0f;
    dl->AddRectFilled(ImVec2(hx0, hy0), ImVec2(hx1, hy1), theme::kBrassMid, round);
    if (hx1 - hx0 > round * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(hx0 + round, hy0), ImVec2(hx1 - round, hy1),
                                    theme::kBrassBright, theme::kBrassBright,
                                    theme::kBrassDark, theme::kBrassDark);
    }
    dl->AddRect(ImVec2(hx0, hy0), ImVec2(hx1, hy1), theme::kBrassDark, round, 0,
                theme::kHairline);
    addBenchBevel(dl, ImVec2(hx0, hy0), ImVec2(hx1, hy1), round, true);

    const bool roomForScrews = (hx1 - hx0) > 200.0f && (hy1 - hy0) > 110.0f;
    if (roomForScrews) {
        const float sr = 3.6f;
        const float si = 9.0f;
        addCabinetScrew(dl, ImVec2(hx0 + si, hy0 + si), sr, 24.0f);
        addCabinetScrew(dl, ImVec2(hx1 - si, hy0 + si), sr, -61.0f);
        addCabinetScrew(dl, ImVec2(hx0 + si, hy1 - si), sr, 78.0f);
        addCabinetScrew(dl, ImVec2(hx1 - si, hy1 - si), sr, 12.0f);
    }

    // --- the strip of metal beneath the glass ---------------------------------
    //
    // The counter and the NEW lamp. Both are engraved-and-on-glass in the
    // bench's own way: the caption is cut into the brass, the figure sits in a
    // sunk well. It is the readings COUNTER - the plugin's own event sequence,
    // which for this kind advances once per reading received - so it counts
    // something real.
    const float caseInset = roomForScrews ? 14.0f : 8.0f;
    float stripTop = hy1;
    const float stripH = 34.0f;
    const bool haveStrip = (hy1 - hy0) >= 150.0f;
    if (haveStrip) {
        stripTop = hy1 - stripH - 4.0f;
        // The caption is cut into the metal BESIDE the well, not above it:
        // above it there is only the glass bezel, and that is where it was
        // being drawn and clipped.
        ImFont* lf = fonts::legend();
        const char* cap = "READINGS";
        const float capW = lf->CalcTextSizeA(fonts::kTinySize, FLT_MAX, 0.0f, cap).x;
        const float wy0 = stripTop + 5.0f;
        const float wy1 = wy0 + 24.0f;
        const float capX = hx0 + caseInset;
        dl->AddText(lf, fonts::kTinySize, ImVec2(capX + 1.0f, wy0 + 5.0f),
                    theme::withAlpha(theme::kVoid, 0.55f), cap);
        dl->AddText(lf, fonts::kTinySize, ImVec2(capX, wy0 + 4.0f), theme::kInkMuted, cap);
        const float wx0 = capX + capW + 8.0f;
        const float wx1 = wx0 + std::min(72.0f, (hx1 - hx0) * 0.22f);
        drawFreqDrumWell(dl, ImVec2(wx0, wy0), ImVec2(wx1, wy1));
        char count[16];
        if (in.have) {
            std::snprintf(count, sizeof count, "%u", static_cast<unsigned>(in.state.seq));
        } else {
            // No reading has ever arrived: the counter shows the dashes a
            // counter with nothing to count shows, not a zero.
            std::snprintf(count, sizeof count, "---");
        }
        ImFont* rf = fonts::reading();
        const ImVec2 csz = rf->CalcTextSizeA(fonts::kReadingSize, FLT_MAX, 0.0f, count);
        dl->AddText(rf, fonts::kReadingSize,
                    ImVec2(wx1 - csz.x - 8.0f, (wy0 + wy1) * 0.5f - csz.y * 0.5f),
                    in.have ? theme::kAmber : theme::kAmberDim, count);

        // The lamps. NEW is the host's cue - an event has arrived that this
        // window has not shown - and BATT repeats the state flag on the metal
        // so a glance at the case says a sensor needs cells even when the
        // compartment carrying the symbol is scrolled out of a narrow window.
        const bool lowBatt =
            in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOW_BATT) != 0u;
        const float lampR = 6.0f;
        // Clear of the corner screw: the lamps' captions are lettered under
        // them and the bottom right screw sits where BATT's T would be.
        float lx = hx1 - caseInset - lampR - 12.0f;
        const float ly = stripTop + 8.0f;
        ImGui::PushFont(fonts::legend(), fonts::kTinySize);
        drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kAmber, lowBatt, "BATT");
        lx -= 64.0f;
        drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kGold, cue.unread, "NEW");
        ImGui::PopFont();
    }

    // --- the glass ------------------------------------------------------------
    const float gx0 = hx0 + caseInset;
    const float gx1 = hx1 - caseInset;
    const float gy0 = hy0 + (roomForScrews ? 14.0f : 8.0f);
    const float gy1 = (haveStrip ? stripTop : hy1) - 8.0f;
    if (gx1 - gx0 < 60.0f || gy1 - gy0 < 34.0f) { return br.y - tl.y; }

    // The bezel it is sunk into, then the polariser itself.
    dl->AddRectFilled(ImVec2(gx0 - 4.0f, gy0 - 4.0f), ImVec2(gx1 + 4.0f, gy1 + 4.0f),
                      theme::kEnamelDark, 4.0f);
    addBenchBevel(dl, ImVec2(gx0 - 4.0f, gy0 - 4.0f), ImVec2(gx1 + 4.0f, gy1 + 4.0f), 4.0f,
                  false);
    dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), kGlassTop, 2.0f);
    dl->AddRectFilledMultiColor(ImVec2(gx0 + 2.0f, gy0), ImVec2(gx1 - 2.0f, gy1), kGlassTop,
                                kGlassTop, kGlassBottom, kGlassBottom);
    // The sheen down the top of the cover glass. Two flat quads, because this
    // draw list has no gradient that is not axis-aligned and a real cover
    // glass reflects the room in a band, not a blur.
    dl->AddRectFilledMultiColor(ImVec2(gx0, gy0), ImVec2(gx1, gy0 + (gy1 - gy0) * 0.18f),
                                IM_COL32(255, 255, 255, 34), IM_COL32(255, 255, 255, 18),
                                IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));

    dl->PushClipRect(ImVec2(gx0, gy0), ImVec2(gx1, gy1), true);

    const float insetX = 6.0f;
    const float ix0 = gx0 + insetX;
    const float ix1 = gx1 - insetX;
    float iy0 = gy0 + 4.0f;
    const float iy1 = gy1 - 4.0f;

    // The clock strip along the top, exactly where the console keeps it: the
    // time on the left, and on the right how many of the three channels are
    // being heard - which is the console's own "how many sensors have I got"
    // and is the honest form of a signal indicator.
    const float insideH = iy1 - iy0;
    if (insideH >= 96.0f) {
        const float px = fonts::kTinySize;
        bool colonOn = true;
        const char* clock = consoleClock(cue.nowSec, &colonOn);
        char shown[8];
        std::snprintf(shown, sizeof shown, "%s", clock);
        if (!colonOn) {
            for (char& c : shown) {
                if (c == ':') { c = ' '; }
            }
        }
        glassText(dl, ix0, iy0, shown, px, 0.85f, ix1);
        char chans[24];
        if (in.have) {
            std::snprintf(chans, sizeof chans, "%d/3",
                          reportingCount(in.state.values[6]));
        } else {
            std::snprintf(chans, sizeof chans, "-/3");
        }
        ImFont* f = fonts::ui();
        const float cw = f->CalcTextSizeA(px, FLT_MAX, 0.0f, chans).x;
        glassText(dl, ix1 - cw, iy0, chans, px, in.have ? 0.85f : 0.5f, ix1);
        iy0 += px + 3.0f;
        // The rule printed across the glass between the clock strip and the
        // channel area.
        dl->AddLine(ImVec2(ix0, iy0), ImVec2(ix1, iy0), kGlassGhost, 1.0f);
        dl->AddLine(ImVec2(ix0, iy0), ImVec2(ix1, iy0),
                    (kGlassInk & 0x00FFFFFFu) | (0x40u << IM_COL32_A_SHIFT), 1.0f);
        iy0 += 4.0f;
    }

    // --- the three compartments ----------------------------------------------
    const float panelH = iy1 - iy0;
    const float panelGap = 8.0f;
    // The compartment's vertical budget, which is pure arithmetic and lives in
    // the math header where a test sweeps it over every size a window can be.
    const PanelBox first = panelBox(ix0, ix1, 0, 3, panelGap);
    const float panelW = first.valid ? (first.x1 - first.x0) : 0.0f;
    const PanelStyle st = panelStyle(panelW, panelH, fonts::kTinySize);

    for (int i = 0; i < 3; ++i) {
        const PanelBox b = panelBox(ix0, ix1, i, 3, panelGap);
        if (!b.valid) { continue; }
        if (i > 0) {
            // The printed division between compartments.
            const float dx = b.x0 - panelGap * 0.5f;
            dl->AddLine(ImVec2(dx, iy0 + 1.0f), ImVec2(dx, iy1 - 1.0f),
                        (kGlassInk & 0x00FFFFFFu) | (0x30u << IM_COL32_A_SHIFT), 1.0f);
        }
        drawChannelPanel(dl, b.x0, b.x1, iy0, iy1, i + 1, in, st);
    }

    // Nothing has ever been decoded: the glass says so once, across the
    // compartments, rather than leaving three sets of dashes to be read as a
    // fault. The dashes stay - they are what the equipment shows - and this is
    // the sentence explaining them.
    if (!in.have && st.headerPx > 0.0f) {
        ImFont* f = fonts::ui();
        const char* msg = "NO SENSOR HEARD YET";
        const float px = fonts::kTinySize;
        const float mw = f->CalcTextSizeA(px, FLT_MAX, 0.0f, msg).x;
        if (mw < ix1 - ix0) {
            glassText(dl, (ix0 + ix1) * 0.5f - mw * 0.5f, iy1 - px - 1.0f, msg, px, 0.8f, ix1);
        }
    }

    dl->PopClipRect();
    return br.y - tl.y;
}

}  // namespace cascade::gui
