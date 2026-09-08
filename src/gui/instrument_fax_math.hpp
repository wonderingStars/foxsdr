// instrument_fax_math.hpp - the arithmetic behind the radiofax face.
//
// EVERYTHING IN THIS FILE IS PURE, and that is the point: no ImGui, no
// drawing, no state. The phase word to lamp index, the needle's position on a
// centre-zero scale, how far the chart has fed out of the slot, where the
// ruling on that chart has crept to, what the drum counter shows and how the
// deck is divided at whatever size the user has dragged the window to - all of
// it is decided here, so tests/test_instrument_fax.cpp can pin it without a
// graphics context. instrument_fax.cpp then only paints what this returns.
//
// THE SLOT MAP IT SERVES is plugin_abi.h's CASCADE_INSTRUMENT_FAX:
//   values[0] IOC          values[1] lines per minute
//   values[2] lines received   values[3] tuning offset Hz
//   text[0]   phase - IDLE / START / PHASING / PICTURE / STOP
//   flags     LOCK while phased
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_FAX_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_FAX_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace cascade::gui::faxmath {

// --- the phase ladder --------------------------------------------------------
//
// The five words the contract names, in the order a transmission goes through
// them. The index IS the lamp: the ladder is drawn left to right in this
// order and exactly one of them lights, or none when the plugin has sent a
// word this host does not know.

inline constexpr int kPhaseIdle = 0;
inline constexpr int kPhaseStart = 1;
inline constexpr int kPhasePhasing = 2;
inline constexpr int kPhasePicture = 3;
inline constexpr int kPhaseStop = 4;
inline constexpr int kPhaseCount = 5;
inline constexpr int kPhaseUnknown = -1;

inline const char* phaseName(int index) {
    switch (index) {
        case kPhaseIdle: return "IDLE";
        case kPhaseStart: return "START";
        case kPhasePhasing: return "PHASING";
        case kPhasePicture: return "PICTURE";
        case kPhaseStop: return "STOP";
        default: return "";
    }
}

// The plugin's word to its lamp. EXACT, and deliberately so: the contract
// prints the five words it may send, and a host that quietly accepted
// "picture" or "PIC" would be guessing at which lamp to light. An unknown or
// empty word lights nothing, which is what "no reading" looks like on a lamp.
inline int phaseIndex(const char* s) {
    if (s == nullptr || s[0] == '\0') { return kPhaseUnknown; }
    for (int i = 0; i < kPhaseCount; ++i) {
        const char* n = phaseName(i);
        int k = 0;
        while (n[k] != '\0' && s[k] == n[k]) { ++k; }
        if (n[k] == '\0' && s[k] == '\0') { return i; }
    }
    return kPhaseUnknown;
}

// --- the tuning meter --------------------------------------------------------
//
// A radiofax operator's one continuous job is keeping the dial where the
// subcarrier lands on 1900 Hz, because the mapping from frequency to grey is
// published and fixed: a receiver off tune produces a chart whose shades are
// all wrong for no visible reason. So the meter is CENTRE ZERO - the needle
// sits mid-scale when the error is nothing, and which way it leans says which
// way to turn the dial.
//
// PLUS AND MINUS 250 Hz FULL SCALE. The signal's whole deviation is +/-400 Hz
// about the subcarrier, so an error approaching 250 Hz is already putting
// black where white belongs; a wider scale would leave every usable error
// crowded around the centre pin where it could not be read.
inline constexpr double kTuningFullScaleHz = 250.0;

// 0.5 at zero error, 0 at the anticlockwise stop, 1 at the clockwise one.
// Clamped, because a signal far enough off tune to peg the meter must peg it
// rather than sweep the needle off the face.
inline float tuningFrac(double hz) {
    if (!(hz == hz)) { return 0.5f; }  // NaN: nothing to point at
    double f = 0.5 + hz / (2.0 * kTuningFullScaleHz);
    if (f < 0.0) { f = 0.0; }
    if (f > 1.0) { f = 1.0; }
    return static_cast<float>(f);
}

// WHETHER THERE IS A TUNING READING AT ALL, and this is the one place the
// face has to reason rather than transcribe. values[3] carries a signed error
// in Hz, so zero is a legitimate reading - a receiver exactly on tune - and
// cannot be used as "no reading" the way an empty string can. The measurement
// itself comes from the PHASING signal (95 % of a phasing line is black, and
// black is a published frequency), so it does not exist until the
// transmission has reached phasing. Before that the meter shows no needle.
inline bool tuningIsMeasured(int phaseIdx) {
    return phaseIdx == kPhasePhasing || phaseIdx == kPhasePicture ||
           phaseIdx == kPhaseStop;
}

// "-23 Hz", "+7 Hz", "0 Hz". Clamped at four figures so a corrupt slot cannot
// run a 300-character double through a 16-byte buffer; NaN reads as no
// figure at all.
inline void formatOffset(double hz, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    if (!(hz == hz)) {
        std::snprintf(out, cap, "--");
        return;
    }
    if (hz > 9999.0) {
        std::snprintf(out, cap, ">+9999 Hz");
        return;
    }
    if (hz < -9999.0) {
        std::snprintf(out, cap, "<-9999 Hz");
        return;
    }
    const long v = std::lround(hz);
    if (v == 0) {
        std::snprintf(out, cap, "0 Hz");
        return;
    }
    std::snprintf(out, cap, "%+ld Hz", v);
}

// --- the selector readouts ---------------------------------------------------
//
// IOC and line rate are not free numbers. The format has two indices of
// cooperation and four line rates, the decoder measures which is in use and
// snaps to it, and a real recorder's panel prints all the positions with the
// live one illuminated - which is a better readout than a bare figure,
// because it says what the machine COULD be doing as well as what it is.
//
// THE OFF-LIST CASE IS SHOWN, NOT SWALLOWED. A plugin that reports something
// not on the ladder gets an extra cell holding its figure, lit. Refusing to
// display it would be the face inventing a "no reading" out of a reading.

inline constexpr int kSelectorMax = 5;

struct Selector {
    int count = 0;
    double v[kSelectorMax] = {0.0, 0.0, 0.0, 0.0, 0.0};
    int lit = -1;  // index into v, or -1 for nothing lit
};

namespace detail {

inline Selector build(const double* std_, int stdCount, double value, bool have) {
    Selector s;
    for (int i = 0; i < stdCount && i < kSelectorMax; ++i) {
        s.v[s.count++] = std_[i];
    }
    if (!have || !(value == value) || value <= 0.0) { return s; }
    for (int i = 0; i < s.count; ++i) {
        // A quarter of a unit: the decoder snaps to the published rates, so
        // anything further out is genuinely a different figure and belongs in
        // its own cell rather than lighting a position it is not at.
        if (std::fabs(s.v[i] - value) < 0.25) {
            s.lit = i;
            return s;
        }
    }
    if (s.count < kSelectorMax) {
        s.v[s.count] = value;
        s.lit = s.count;
        ++s.count;
    }
    return s;
}

}  // namespace detail

// The two indices of cooperation radiofax uses: 576 for the ordinary weather
// charts, 288 for the coarser transmissions.
inline Selector iocSelector(double value, bool have) {
    static const double kStd[2] = {576.0, 288.0};
    return detail::build(kStd, 2, value, have);
}

// The four published drum speeds, in lines per minute.
inline Selector lpmSelector(double value, bool have) {
    static const double kStd[4] = {60.0, 90.0, 120.0, 240.0};
    return detail::build(kStd, 4, value, have);
}

// A selector cell's legend: whole numbers plainly, anything else to one
// decimal, and a figure too large to be one of these at all as no figure.
inline void formatSelector(double v, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    if (!(v == v) || v <= 0.0 || v >= 100000.0) {
        std::snprintf(out, cap, "--");
        return;
    }
    if (std::fabs(v - std::floor(v + 0.5)) < 0.05) {
        std::snprintf(out, cap, "%ld", std::lround(v));
        return;
    }
    std::snprintf(out, cap, "%.1f", v);
}

// --- the line counter --------------------------------------------------------
//
// Four drums, because the decoder stops at 1600 lines and a chart at 120 lpm
// runs to about 1200: four figures hold every picture this can receive and a
// fifth would be a permanent leading zero.
inline constexpr int kDrumDigits = 4;

// CLAMPED TO ALL NINES rather than wrapped, the same rule the scope's own
// drums follow: a counter shown a number it cannot hold must read "as high as
// I go", never the bottom four digits of it, which would be a smaller number
// than the truth.
inline int drumValue(double lines) {
    if (!(lines == lines) || lines <= 0.0) { return 0; }
    if (lines > 9999.0) { return 9999; }
    return static_cast<int>(lines + 0.5);
}

// Fills `digits` characters, right-aligned and zero-padded, and reports which
// cell holds the first significant figure so the leading zeros can be dimmed
// the way the frequency counter dims its own. `firstSignificant` is `digits`
// for a value of zero - every cell is a leading zero and none is live.
inline void drumCells(int value, char* out, int digits, int* firstSignificant) {
    if (out == nullptr || digits <= 0) {
        if (firstSignificant != nullptr) { *firstSignificant = 0; }
        return;
    }
    int v = value;
    if (v < 0) { v = 0; }
    int cap = 1;
    for (int i = 0; i < digits; ++i) { cap *= 10; }
    if (v > cap - 1) { v = cap - 1; }
    int firstSig = digits;
    for (int i = 0; i < digits; ++i) {
        int place = 1;
        for (int k = 0; k < digits - 1 - i; ++k) { place *= 10; }
        const int d = (v / place) % 10;
        out[i] = static_cast<char>('0' + d);
        if (d != 0 && firstSig == digits) { firstSig = i; }
    }
    if (firstSignificant != nullptr) { *firstSignificant = firstSig; }
}

// --- the paper ---------------------------------------------------------------
//
// The chart itself is delivered by the plugin's IMAGE capability and lives in
// the host's own picture window; this face does not have the pixels. What it
// has is the line count, and what a recorder shows across the room is how
// much paper has come out of the slot - so the strip's LENGTH is the progress
// indication, and it is honest because it is driven by a real count.
//
// A ten-minute chart at 120 lpm is about 1200 lines, which is what "a full
// page" means here. Longer transmissions run the strip to the bottom of the
// well and hold it there rather than drawing outside the face.
inline constexpr double kNominalChartLines = 1200.0;

inline float paperFrac(double lines) {
    if (!(lines == lines) || lines <= 0.0) { return 0.0f; }
    double f = lines / kNominalChartLines;
    if (f > 1.0) { f = 1.0; }
    return static_cast<float>(f);
}

// Where the faint ruling across the paper has crept to, in pixels, so the
// strip visibly FEEDS rather than merely growing: one rule per
// `linesPerRule` received lines, sliding down its own pitch as they arrive.
// Always in [0, pitchPx).
inline float ruleOffset(double lines, float pitchPx, double linesPerRule) {
    if (!(lines == lines) || lines <= 0.0 || !(pitchPx > 0.0f) ||
        !(linesPerRule > 0.0)) {
        return 0.0f;
    }
    double t = lines / linesPerRule;
    t -= std::floor(t);
    float o = static_cast<float>(t) * pitchPx;
    if (!(o >= 0.0f)) { o = 0.0f; }
    if (o >= pitchPx) { o = 0.0f; }
    return o;
}

// --- the deck's layout -------------------------------------------------------
//
// The window is the user's to drag, so what the deck can hold is a decision
// and not an assumption. Everything below is measured in the rectangle the
// host hands the face, and each element is dropped in the order it can most
// afford to be lost: the group caption first, then the meter, then the ladder
// of phase lamps (which becomes the phase WORD on glass - fewer pixels, same
// fact), then the counter.
struct Layout {
    bool ok = false;      // false: too small to draw anything honestly
    float deckH = 0.0f;   // the control deck
    float paperH = 0.0f;  // the slot and the chart below it; 0 = no room
    float meterW = 0.0f;  // the tuning meter's column; 0 = no meter
    bool groupCaption = false;
    bool lampLadder = false;   // false: the phase word on glass instead
    bool statusLamps = false;  // the NEW and LOCK lamps at the end of the row
    int drumDigits = 0;        // 0 = no counter
};

inline Layout layout(float w, float h) {
    Layout L;
    if (!(w >= 80.0f) || !(h >= 56.0f)) { return L; }
    L.ok = true;

    // The paper takes a little over two fifths. Measured rather than
    // guessed: at a third of the 240 px the host gives this face when the
    // plugin also has a log, the well came out 64 px deep and a third-of-a-
    // chart strip inside it was fifteen pixels of solid cream with room for
    // one rule - it read as a lit bar rather than as paper. Two fifths is
    // where the ruling and the tear bar both survive that rectangle.
    constexpr float kGap = 6.0f;
    float paper = h * 0.42f;
    if (paper > 150.0f) { paper = 150.0f; }
    if (paper < 40.0f) { paper = 40.0f; }
    float deck = h - kGap - paper;
    if (deck < 92.0f) {
        // Squeeze the paper to its minimum before taking anything off the
        // deck: the lamps, the counter and the meter are what an operator
        // actually watches.
        paper = 40.0f;
        deck = h - kGap - paper;
    }
    if (deck < 64.0f) {
        paper = 0.0f;
        deck = h;
    }
    // AND THE DECK STOPS GROWING. Lamps, apertures and a meter do not get
    // better for being spread over four hundred pixels - a tall window with
    // an unbounded deck put a third of its height of empty enamel under the
    // selectors - so past this the surplus goes to the paper, which is the
    // part of a fax machine that is genuinely mostly paper. Past 420 px of
    // well the face simply returns less than it was offered, which the
    // contract in instrument_face.hpp explicitly allows.
    if (deck > 210.0f) {
        deck = 210.0f;
        paper = h - kGap - deck;
        if (paper > 420.0f) { paper = 420.0f; }
    }
    L.deckH = deck;
    L.paperH = paper;

    L.meterW = 0.0f;
    if (w >= 360.0f && deck >= 84.0f) {
        float m = w * 0.26f;
        if (m < 104.0f) { m = 104.0f; }
        if (m > 168.0f) { m = 168.0f; }
        L.meterW = m;
    }

    const float leftW = w - L.meterW - 24.0f;
    L.groupCaption = deck >= 132.0f;
    L.lampLadder = leftW >= 260.0f && deck >= 104.0f;
    // NEW and LOCK - the two lamps that belong to the WINDOW rather than to
    // the machine - stand at the end of the same row as the phase ladder,
    // behind a divider. They were tried on a strip of their own above the
    // deck and it cost 38 px that the paper could not spare: at the 240 px
    // the host gives this face when the plugin also has a log, the well came
    // out 40 px deep and the chart in it was a scratch. Seven lamps in one
    // row want 44 px each to keep PHASING legible under them.
    L.statusLamps = L.lampLadder ? (leftW >= 308.0f) : (leftW >= 260.0f);
    L.drumDigits = (leftW >= 150.0f) ? kDrumDigits : 0;
    return L;
}

}  // namespace cascade::gui::faxmath

#endif  // CASCADE_GUI_INSTRUMENT_FAX_MATH_HPP
