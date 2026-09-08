// instrument_weather_console_math.hpp - the arithmetic behind the weather
// console's face, with no ImGui in it so tests can pin it.
//
// WHY THIS IS A SEPARATE HEADER. Everything here is a decision the face makes
// about what a reading LOOKS LIKE - which seven segments a digit lights, where
// the minus sign goes, what a channel with no sensor shows, how three panels
// divide a strip of glass - and every one of those decisions is a place a
// "no reading" can quietly become a zero. They are pure functions of numbers
// and characters, so tests/test_instrument_weather_console.cpp exercises them
// without a graphics context, the same separation scope_view.hpp keeps.
//
// THE ONE RULE THIS FILE EXISTS TO ENFORCE. The slot map hands the face eight
// doubles, and three of them (temperature on channels 1..3) are perfectly
// valid at zero: 0.0 C is a real winter morning. The ONLY thing that says
// whether a channel has a reading is the channel mask in values[6], so every
// entry point here takes the mask - never the temperature alone - and answers
// with a "blank" cell plan when the bit is clear. A caller cannot accidentally
// print a figure the plugin did not send, because there is no path through
// this header that turns an unmasked channel into digits.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_WEATHER_CONSOLE_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_WEATHER_CONSOLE_MATH_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cascade::gui::wxface {

// --- the seven-segment table -------------------------------------------------
//
// Segment names are the industry's own, and the order matters only in that it
// is written down once here and read by the drawing code:
//
//        aaaa
//       f    b
//       f    b
//        gggg
//       e    c
//       e    c
//        dddd    (and a decimal point, drawn separately)
//
// A CONSOLE'S LCD HAS EXACTLY THESE SEVEN and no more, which is why "no
// reading" on one of these panels can only ever be a dash: segment g alone is
// the only mark a seven-segment cell can make that is not a digit. That is
// what the real base unit shows for a channel it has not heard, and it is what
// this face shows, so the two agree by construction rather than by intention.

inline constexpr unsigned kSegA = 1u << 0;
inline constexpr unsigned kSegB = 1u << 1;
inline constexpr unsigned kSegC = 1u << 2;
inline constexpr unsigned kSegD = 1u << 3;
inline constexpr unsigned kSegE = 1u << 4;
inline constexpr unsigned kSegF = 1u << 5;
inline constexpr unsigned kSegG = 1u << 6;
inline constexpr unsigned kSegAll = 0x7Fu;

// Which segments a character lights. Digits, the minus sign, a blank and the
// dash this face uses for "no reading"; anything else lights nothing, because
// a cell that cannot show a character must show an empty cell rather than an
// arbitrary one.
inline unsigned segmentsFor(char c) {
    switch (c) {
        case '0': return kSegA | kSegB | kSegC | kSegD | kSegE | kSegF;
        case '1': return kSegB | kSegC;
        case '2': return kSegA | kSegB | kSegG | kSegE | kSegD;
        case '3': return kSegA | kSegB | kSegG | kSegC | kSegD;
        case '4': return kSegF | kSegG | kSegB | kSegC;
        case '5': return kSegA | kSegF | kSegG | kSegC | kSegD;
        case '6': return kSegA | kSegF | kSegG | kSegE | kSegC | kSegD;
        case '7': return kSegA | kSegB | kSegC;
        case '8': return kSegAll;
        case '9': return kSegA | kSegB | kSegC | kSegD | kSegF | kSegG;
        case '-': return kSegG;
        // The three letters a seven-segment cell can actually form that this
        // face needs. C is the degree legend's own letter; H and L are what
        // these consoles show for a reading above or below the range the
        // field can hold (the BAR628HG's troubleshooting page states "LLL or
        // HHH" outright), and they are the equipment's way of saying "there
        // is a reading and it is not this one" - which is a different claim
        // from the dashes that mean no reading at all.
        case 'C': return kSegA | kSegF | kSegE | kSegD;
        case 'H': return kSegB | kSegC | kSegE | kSegF | kSegG;
        case 'L': return kSegD | kSegE | kSegF;
        case ' ': return 0u;
        default: return 0u;
    }
}

// --- the masks ---------------------------------------------------------------
//
// values[6] and values[7] are doubles carrying a three-bit integer, which is a
// shape worth being careful with: the slot could hold anything a plugin puts
// there, including a NaN or 1e300, and std::lround of either is undefined
// behaviour. Everything that reads a mask goes through here.

inline bool maskBit(double mask, int channel) {
    if (channel < 1 || channel > 3) { return false; }
    if (!(mask == mask)) { return false; }  // NaN: no bits, not a crash
    if (!(mask >= 0.0) || mask > 1.0e9) { return false; }
    const unsigned m = static_cast<unsigned>(mask + 0.5);
    return (m & (1u << (channel - 1))) != 0u;
}

// Channel `channel` (1..3) has a reading.
inline bool channelReporting(double channelMask, int channel) {
    return maskBit(channelMask, channel);
}

// Channel `channel` reports its battery is low. A channel with no reading at
// all cannot report anything, so the channel mask gates this too - otherwise a
// stale low-battery bit would light an icon on a blank panel.
inline bool channelLowBattery(double channelMask, double lowBattMask, int channel) {
    return maskBit(channelMask, channel) && maskBit(lowBattMask, channel);
}

inline int reportingCount(double channelMask) {
    int n = 0;
    for (int ch = 1; ch <= 3; ++ch) {
        if (maskBit(channelMask, ch)) { ++n; }
    }
    return n;
}

// --- the temperature cells ---------------------------------------------------
//
// A THREE-AND-A-HALF DIGIT FIELD, which is what these consoles actually have.
// The all-segments artwork in the BAR388HG and BAR628HG manuals reads
// "-188.8": a dedicated minus bar, then a HALF digit that can only ever form a
// 1, then two whole digits, the decimal point, then the tenths. So the field
// holds -199.9 to 199.9 and not one tenth more, and this struct is that field
// cell for cell.
//
//   have=false        `blank` - the tens, units and tenths cells are dashes and
//                     the half digit is empty, which is exactly what the
//                     equipment shows for a sensor it cannot find (the BAR388HG
//                     manual prints it as "- - . -").
//   |value| >= 200    `over` - the three cells read HHH above the field and LLL
//                     below it, which is again the equipment's own indication
//                     (the BAR628HG's troubleshooting page). This decoder's BCD
//                     digits stop at 99.9 so it cannot get here, but the slot is
//                     a double: without this, a wild figure would be drawn as
//                     digits running over the compartment beside it, and
//                     silently clamping it to 199.9 would be worse still.
struct TempCells {
    bool blank = true;      // no reading on this channel
    bool over = false;      // a reading outside the field: HHH / LLL
    bool overHigh = false;  // which end it went off
    bool negative = false;
    char hundreds = ' ';    // the half digit: '1' or ' '
    char tens = '-';        // '0'..'9', ' ' for a suppressed leading zero, '-'
    char units = '-';
    char tenths = '-';
};

inline TempCells temperatureCells(bool have, double celsius) {
    TempCells t;
    if (!have || !(celsius == celsius)) { return t; }  // NaN is not a reading
    // Rounded to a tenth FIRST, so 199.96 is treated as 200.0 and refused by
    // the range test rather than split into "99.10" by a truncating divide.
    double v = celsius;
    const bool neg = v < 0.0;
    if (neg) { v = -v; }
    if (!(v < 1.0e9)) {  // catches infinity before the cast to long
        t.blank = false;
        t.over = true;
        t.overHigh = !neg;
        t.negative = neg;
        t.hundreds = ' ';
        t.tens = t.units = t.tenths = neg ? 'L' : 'H';
        return t;
    }
    const long tenthsTotal = static_cast<long>(v * 10.0 + 0.5);
    if (tenthsTotal >= 2000) {
        t.blank = false;
        t.over = true;
        t.overHigh = !neg;
        t.negative = neg;
        t.tens = t.units = t.tenths = neg ? 'L' : 'H';
        return t;
    }
    t.blank = false;
    t.negative = neg;
    const int whole = static_cast<int>(tenthsTotal / 10);
    const int frac = static_cast<int>(tenthsTotal % 10);
    // LEADING ZEROS ARE BLANKED, which is what the glass does: 8.4 C reads
    // " 8.4", not "008.4". The cells are still there - they are where the
    // digits go when they are needed - so nothing shuffles sideways as the
    // reading crosses ten degrees.
    t.hundreds = (whole >= 100) ? '1' : ' ';
    t.tens = (whole >= 10) ? static_cast<char>('0' + ((whole / 10) % 10)) : ' ';
    t.units = static_cast<char>('0' + (whole % 10));
    t.tenths = static_cast<char>('0' + frac);
    return t;
}

// --- the humidity cells ------------------------------------------------------
//
// Two digits and, for the one value that needs it, a third. Blank when the
// channel has no reading OR when the sensor reports no humidity - which the
// slot map spells as a zero, because a hygrometer never reads 0% and a
// temperature-only sensor (THN132N, THWR800) leaves the slot untouched.
struct HumCells {
    bool blank = true;
    char hundreds = ' ';
    char tens = '-';
    char units = '-';
};

inline HumCells humidityCells(bool channelHasReading, double percent) {
    HumCells h;
    if (!channelHasReading) { return h; }
    if (!(percent == percent)) { return h; }
    if (percent < 0.5 || percent > 100.5) { return h; }  // 0 means "not measured"
    const int pct = static_cast<int>(percent + 0.5);
    h.blank = false;
    h.hundreds = (pct >= 100) ? '1' : ' ';
    h.tens = static_cast<char>('0' + ((pct / 10) % 10));
    h.units = static_cast<char>('0' + (pct % 10));
    return h;
}

// --- fitting the printed label ----------------------------------------------
//
// The sensor model is printed under its panel, and a panel is a third of the
// glass. A model name longer than the panel is cut and marked with an ellipsis
// rather than drawn into its neighbour; `maxChars` is what the caller measured
// its own panel to hold.
//
// It also copies into a fixed buffer, so an overlong text[] from a plugin - the
// slot holds 63 characters and a panel holds far fewer - cannot run off the end
// of anything.
inline void fitLabel(const char* in, char* out, std::size_t cap, std::size_t maxChars) {
    if (out == nullptr || cap == 0u) { return; }
    out[0] = '\0';
    if (in == nullptr) { return; }
    std::size_t want = maxChars;
    if (want + 1u > cap) { want = cap - 1u; }
    const std::size_t len = std::strlen(in);
    if (len <= want) {
        std::memcpy(out, in, len);
        out[len] = '\0';
        return;
    }
    if (want == 0u) { return; }
    if (want <= 3u) {
        std::memcpy(out, in, want);
        out[want] = '\0';
        return;
    }
    std::memcpy(out, in, want - 3u);
    out[want - 3u] = '.';
    out[want - 2u] = '.';
    out[want - 1u] = '.';
    out[want] = '\0';
}

// --- laying the panels out ---------------------------------------------------
//
// Three panels across a strip of glass, with a gap between them that is the
// divider the real glass is printed with. Index 0..count-1; a degenerate strip
// (a window dragged narrower than the gaps) gives an empty box rather than a
// negative-width one, which is what would draw a rectangle inside out.
struct PanelBox {
    float x0 = 0.0f;
    float x1 = 0.0f;
    bool valid = false;
};

inline PanelBox panelBox(float x0, float x1, int index, int count, float gap) {
    PanelBox b;
    if (count <= 0 || index < 0 || index >= count) { return b; }
    const float total = x1 - x0;
    const float gaps = gap * static_cast<float>(count - 1);
    const float each = (total - gaps) / static_cast<float>(count);
    // Four points is the floor, not one: below it the compartment cannot hold
    // even the thinnest segment and the caller is better off drawing nothing
    // than three slivers. A negative `each` - which is what a strip narrower
    // than its own divisions produces - is caught by the same test, and that
    // is the one that would otherwise hand back a rectangle inside out.
    if (!(each >= 4.0f)) { return b; }
    b.x0 = x0 + static_cast<float>(index) * (each + gap);
    b.x1 = b.x0 + each;
    b.valid = true;
    return b;
}

// The aspect a seven-segment cell is drawn at: a digit is about 0.55 as wide
// as it is tall, which is the proportion these displays use and is what stops
// three of them plus a sign and a point from filling more glass than a
// compartment has.
inline float digitCellWidth(float digitHeight) { return digitHeight * 0.55f; }

// The pitch between two marks of the group, and it is deliberately tight. The
// compartment is a third of the glass and the field is six marks wide, so
// every point of air between them is a point off the height of every digit -
// this group is width-bound at any window size worth using, which makes the
// pitch a legibility decision and not a spacing one.
inline float digitPitch(float digitHeight) { return digitHeight * 0.09f; }

// The width the whole temperature group needs at a given digit height: the
// minus bar's cell, the half digit, two whole digits, the decimal point's
// narrow cell, the tenths digit, and the degree legend beside it. Five pitches
// separate the six marks.
inline float temperatureGroupWidth(float digitHeight) {
    const float cell = digitCellWidth(digitHeight);
    return cell * 0.50f          // the minus bar's own cell
           + cell * 0.50f        // the half digit (a 1 and nothing else)
           + cell * 3.0f         // tens, units, tenths
           + cell * 0.30f        // the decimal point
           + cell * 0.58f        // the degree legend
           + digitPitch(digitHeight) * 5.0f;
}

// The digit height that fits `width` of glass, clamped to a floor below which
// the face draws no digits at all rather than an unreadable smear.
inline float fitDigitHeight(float width, float maxHeight, float floorHeight) {
    if (!(width > 0.0f) || !(maxHeight > 0.0f)) { return 0.0f; }
    // THE FLOOR BINDS FROM ABOVE AS WELL AS FROM BELOW, and this is the half
    // that was missing. The loop only ever shrinks a digit that is too WIDE,
    // so a caller with very little HEIGHT to spend - a compartment squeezed by
    // a window dragged shut - was handed back whatever it asked for, ten
    // points if that is what was left, and a ten-point seven-segment digit is
    // a smudge that still claims to be a temperature. Found by the size sweep
    // in tests/test_instrument_weather_console.cpp, which is what a sweep is
    // for: it asks every size, and nobody screenshots a compartment 30 points
    // tall.
    if (maxHeight < floorHeight) { return 0.0f; }
    float h = maxHeight;
    while (h > floorHeight && temperatureGroupWidth(h) > width) { h -= 1.0f; }
    if (temperatureGroupWidth(h) > width) { return 0.0f; }
    return h;
}

// --- the compartment's vertical budget ---------------------------------------
//
// A compartment carries four rows - the channel, the temperature, the humidity
// and the sensor's printed name - and at the window's own default size there
// is about a hundred points of glass for all four. So the budget is taken in
// the order of what the instrument is FOR:
//
//   the TEMPERATURE is never given up. A compartment without it is not a
//   thermometer, and a face that dropped it would be showing a channel number
//   and a battery symbol for a reading it is not displaying;
//   the HUMIDITY goes before the temperature;
//   the printed NAME goes before the humidity. It is a caption; the other two
//   are readings, and a reading lost to a caption is the wrong trade.
//
// IT IS HERE RATHER THAN IN THE DRAWING because it is the whole of the face's
// behaviour at a size other than the one it was drawn at, and a window is
// resizable. A test can sweep every height from nothing to a wall-sized panel
// and assert the stack fits, which is rule three; a screenshot can only ever
// show the two or three sizes somebody thought to try.
//
// `uiSize` and `tinySize` are passed in rather than taken from fonts.hpp so
// this header stays free of ImGui - and so the sweep can be re-run at a type
// size the application has not adopted yet.
struct PanelStyle {
    float headerPx = 0.0f;  // the channel row, 0 when there is no room
    float digitH = 0.0f;    // the temperature digits, 0 when there is no room
    float humH = 0.0f;      // the humidity digits, 0 when there is no room
    float modelPx = 0.0f;   // the printed sensor name, 0 when there is no room
};

// The gaps the stack is laid out with, named once so the budget and the
// drawing cannot drift apart.
inline constexpr float kHeaderGap = 2.0f;
inline constexpr float kDigitGap = 3.0f;
inline constexpr float kHumGap = 2.0f;
inline constexpr float kPanelAir = 8.0f;

inline float panelStackHeight(const PanelStyle& st) {
    return st.headerPx + (st.headerPx > 0.0f ? kHeaderGap : 0.0f) + st.digitH +
           (st.digitH > 0.0f ? kDigitGap : 0.0f) +
           (st.humH > 0.0f ? st.humH + kHumGap : 0.0f) + st.modelPx;
}

inline PanelStyle panelStyle(float panelW, float panelH, float tinySize) {
    PanelStyle st;
    if (!(panelW > 0.0f) || !(panelH > 0.0f)) { return st; }
    st.headerPx = (panelH >= 46.0f) ? std::min(tinySize, panelH * 0.17f) : 0.0f;
    st.modelPx = (panelH >= 92.0f) ? (tinySize - 4.0f) : 0.0f;
    float room = panelH - st.headerPx - st.modelPx - kPanelAir;
    st.humH = (room >= 54.0f) ? std::min(24.0f, std::max(14.0f, room * 0.30f)) : 0.0f;
    room -= st.humH;
    st.digitH = fitDigitHeight(panelW - 4.0f, std::min(room, 64.0f), 18.0f);
    // A compartment too narrow or too short for its temperature keeps nothing
    // but its channel number: the humidity and the name are captions on a
    // reading that is not there, and drawing them alone would say the console
    // is showing something it is not.
    if (st.digitH <= 0.0f) {
        st.humH = 0.0f;
        st.modelPx = 0.0f;
    }
    return st;
}

// --- the rail's chip ---------------------------------------------------------
//
// The word on the rail beside this instrument when the window is not showing
// NEW. A console's own answer to "what is it saying" is the outdoor reading,
// so the chip carries the lowest reporting channel's temperature; with several
// channels the count is what distinguishes "one sensor" from "three", so it is
// appended. With no channel reporting at all the chip says so rather than
// showing a figure, which is the whole of rule two in sixteen bytes.
//
// `out` must hold at least 16 bytes, which is what instrumentChip promises.
inline void chipWord(double channelMask, const double* temperatures, char* out,
                     std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    const int n = reportingCount(channelMask);
    if (n == 0 || temperatures == nullptr) {
        std::snprintf(out, cap, "NO RX");
        return;
    }
    for (int ch = 1; ch <= 3; ++ch) {
        if (!channelReporting(channelMask, ch)) { continue; }
        const TempCells t = temperatureCells(true, temperatures[ch - 1]);
        if (t.over || t.blank) {
            std::snprintf(out, cap, "%d CH", n);
            return;
        }
        if (n > 1) {
            std::snprintf(out, cap, "%.1fC +%d", temperatures[ch - 1], n - 1);
        } else {
            std::snprintf(out, cap, "%.1fC", temperatures[ch - 1]);
        }
        return;
    }
    std::snprintf(out, cap, "NO RX");
}

}  // namespace cascade::gui::wxface

#endif  // CASCADE_GUI_INSTRUMENT_WEATHER_CONSOLE_MATH_HPP
