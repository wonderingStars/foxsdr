// instrument_meter_math.hpp - the arithmetic behind the utility-meter face.
//
// NO ImGui. Everything here is a pure function of the plugin's state, so
// tests/test_instrument_meter.cpp can pin the segment table, the digit
// splitting, the commodity word, the tamper unpacking and the roster lookup
// without a graphics context - the same separation scope_view.hpp keeps from
// scope_face.hpp, and for the same reason.
//
// WHAT LIVES HERE AND WHY. Three of these are the places a meter face can
// quietly start lying:
//
//   - the SEGMENT TABLE. A seven-segment cell that lights the wrong bar for a
//     6 reads as a plausible different digit rather than as a fault, so the
//     table is stated once and asserted digit by digit.
//   - the READING's decomposition. "We have not heard this meter" and "this
//     meter reads zero" are opposite claims and must not produce the same
//     picture, so this returns a STATUS as well as digits, and the caller
//     draws no lit segment at all for the first of them.
//   - the LEADING-ZERO boundary. A register showing 00048213 with all eight
//     figures equally lit is a number nobody can read at arm's length; the
//     index of the first significant figure is computed here so the face can
//     ghost what comes before it, exactly as the frequency counter's drums do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_METER_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_METER_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::gui::meter {

// --- the register -----------------------------------------------------------

// Eight cells, because that is what the widest reading this kind can carry
// needs: the ERT SCM consumption field is 24 bits, and 2^24 - 1 is 16777215 -
// eight figures. A meter face with fewer cells than its slot can hold would
// have to drop a digit off a real reading, which is the worst failure a
// register can have.
inline constexpr int kDigits = 8;
inline constexpr double kMaxReading = 99999999.0;

// Segment bits, in the usual clock order from the top bar.
inline constexpr unsigned kSegA = 0x01u;  // top
inline constexpr unsigned kSegB = 0x02u;  // upper right
inline constexpr unsigned kSegC = 0x04u;  // lower right
inline constexpr unsigned kSegD = 0x08u;  // bottom
inline constexpr unsigned kSegE = 0x10u;  // lower left
inline constexpr unsigned kSegF = 0x20u;  // upper left
inline constexpr unsigned kSegG = 0x40u;  // middle
inline constexpr unsigned kSegAll = 0x7Fu;

// Which bars a figure lights. Anything that is not '0'..'9' lights nothing,
// which is how a blank cell is asked for.
inline unsigned segments(char digit) {
    switch (digit) {
        case '0': return kSegA | kSegB | kSegC | kSegD | kSegE | kSegF;
        case '1': return kSegB | kSegC;
        case '2': return kSegA | kSegB | kSegD | kSegE | kSegG;
        case '3': return kSegA | kSegB | kSegC | kSegD | kSegG;
        case '4': return kSegB | kSegC | kSegF | kSegG;
        case '5': return kSegA | kSegC | kSegD | kSegF | kSegG;
        case '6': return kSegA | kSegC | kSegD | kSegE | kSegF | kSegG;
        case '7': return kSegA | kSegB | kSegC;
        case '8': return kSegAll;
        case '9': return kSegA | kSegB | kSegC | kSegD | kSegF | kSegG;
        default: return 0u;
    }
}

// What the register has to say about the figure it was handed.
enum class Reading {
    None,   // nothing was measured: draw the unlit glass, never a zero
    Ok,     // digits[] and firstSignificant are meaningful
    Over,   // a real figure, too wide for the cells this register has
};

struct Register {
    Reading status = Reading::None;
    char digits[kDigits + 1] = {0};  // NUL-terminated, always kDigits long
    int firstSignificant = kDigits - 1;
};

// Splits a reading into cells.
//
// `have` is the plugin's own "there is a state at all"; a face with have=false
// must not draw a figure whatever is in the slot, and passing it in here
// rather than checking it at the call site is what stops that rule being
// forgotten at one of them.
//
// A reading of exactly zero is a READING - a register that has never turned -
// and shows a single lit 0, not a blank. That is not the case rule two is
// about: the blank is for have=false and for a figure that is not a figure
// (NaN, negative, infinite), which no meter register can produce.
inline Register decompose(bool have, double reading) {
    Register r;
    for (int i = 0; i < kDigits; ++i) { r.digits[i] = ' '; }
    r.digits[kDigits] = '\0';
    if (!have) { return r; }
    if (!(reading >= 0.0)) { return r; }  // NaN and negatives land here
    if (reading > kMaxReading) {
        r.status = Reading::Over;
        return r;
    }
    long long v = static_cast<long long>(reading + 0.5);
    if (v < 0) { v = 0; }
    for (int i = kDigits - 1; i >= 0; --i) {
        r.digits[i] = static_cast<char>('0' + static_cast<int>(v % 10));
        v /= 10;
    }
    int first = kDigits - 1;  // a zero reading is one lit figure, not none
    for (int i = 0; i < kDigits; ++i) {
        if (r.digits[i] != '0') {
            first = i;
            break;
        }
    }
    r.firstSignificant = first;
    r.status = Reading::Ok;
    return r;
}

// --- the commodity ----------------------------------------------------------

enum class Commodity { Unknown, Electric, Gas, Water };

// text[1] per the slot map is ELECTRIC / GAS / WATER. Matched case-insensitively
// and by leading word, so a plugin that sends "electric" or "Electric SCM"
// still lights the right lamp; anything else lights none rather than guessing.
inline Commodity commodity(const char* text) {
    if (text == nullptr) { return Commodity::Unknown; }
    // Skip leading space, then compare the first word without allocating.
    const char* p = text;
    while (*p == ' ' || *p == '\t') { ++p; }
    auto leads = [p](const char* word) {
        std::size_t i = 0;
        for (; word[i] != '\0'; ++i) {
            const char a = p[i];
            if (a == '\0') { return false; }
            const char up = (a >= 'a' && a <= 'z') ? static_cast<char>(a - 32) : a;
            if (up != word[i]) { return false; }
        }
        const char after = p[i];
        return after == '\0' || after == ' ' || after == '\t';
    };
    if (leads("ELECTRIC")) { return Commodity::Electric; }
    if (leads("GAS")) { return Commodity::Gas; }
    if (leads("WATER")) { return Commodity::Water; }
    return Commodity::Unknown;
}

// The annunciator printed beside the figures.
//
// ONLY ELECTRIC GETS A REAL UNIT. An electricity endpoint's register is in
// whole kWh and the slot map says so. A gas or water endpoint's SCM
// consumption field is the INDEX COUNT of whatever register the module was
// fitted to - cubic feet, hundreds of cubic feet, gallons, tens of gallons -
// and which one is a property of that installation, not of the message. So
// the lamp says what the commodity is and the annunciator says UNITS, because
// printing "ft3" against a number that might be CCF is inventing a figure by
// changing its scale, which is the same fault as inventing the figure.
inline const char* unitWord(Commodity c) {
    switch (c) {
        case Commodity::Electric: return "kWh";
        case Commodity::Gas:
        case Commodity::Water:
        case Commodity::Unknown: return "UNITS";
    }
    return "UNITS";
}

// --- tamper -----------------------------------------------------------------
//
// values[1] carries both of the ERT frame's tamper counters packed into one
// slot: bits 0..1 the physical (meter inversion / cover removal) counter and
// bits 2..3 the encoder one, each 0..3, and zero for "no tamper reported".
// Unpacked here so the face and the plugin cannot drift on which half is
// which.
struct Tamper {
    bool have = false;  // false when there is no state, NOT when the count is 0
    int physical = 0;
    int encoder = 0;
    bool any() const { return have && (physical != 0 || encoder != 0); }
};

inline Tamper tamper(bool haveState, double bits) {
    Tamper t;
    if (!haveState) { return t; }
    if (!(bits >= 0.0) || bits > 4294967295.0) { return t; }
    const unsigned v = static_cast<unsigned>(bits + 0.5);
    t.have = true;
    t.physical = static_cast<int>(v & 0x3u);
    t.encoder = static_cast<int>((v >> 2) & 0x3u);
    return t;
}

// --- the roster -------------------------------------------------------------

// The "last heard" note under the lamps, taken from the roster the plugin
// already publishes rather than from a slot of its own: the host draws those
// rows beneath this face, so the age shown on the face and the age shown in
// the table are one number by construction and cannot disagree.
//
// Returns an EMPTY string when the roster has no such column or no such
// meter, and the face then draws a blank cell. It never manufactures an age.
inline std::string heardText(const std::vector<std::string>& headings,
                             const std::vector<CascadePanelRow>& rows, const char* id) {
    if (id == nullptr || id[0] == '\0') { return std::string(); }
    std::size_t cols = headings.size();
    if (cols > static_cast<std::size_t>(CASCADE_PANEL_MAX_COLUMNS)) {
        cols = static_cast<std::size_t>(CASCADE_PANEL_MAX_COLUMNS);
    }
    std::size_t heardCol = cols;
    for (std::size_t c = 0; c < cols; ++c) {
        const std::string& h = headings[c];
        if (h.size() != 5) { continue; }
        std::string up;
        up.reserve(5);
        for (char ch : h) {
            up.push_back((ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 32) : ch);
        }
        if (up == "HEARD") {
            heardCol = c;
            break;
        }
    }
    if (heardCol >= cols) { return std::string(); }
    for (const CascadePanelRow& r : rows) {
        if (r.kind != CASCADE_ROW_CELLS) { continue; }
        // The identity column is the first one - the roster's "Meter" heading -
        // which is the same cell the face's own label prints.
        if (std::strncmp(r.cells[0], id, CASCADE_PANEL_CELL_CHARS) != 0) { continue; }
        const char* cell = r.cells[heardCol];
        if (cell[0] == '\0') { return std::string(); }
        // Bounded by hand rather than with strnlen: the cell is a fixed array
        // the plugin filled and is not promised to be terminated.
        std::size_t n = 0;
        while (n < static_cast<std::size_t>(CASCADE_PANEL_CELL_CHARS) && cell[n] != '\0') {
            ++n;
        }
        return std::string(cell, n);
    }
    return std::string();
}

// --- the rail chip ----------------------------------------------------------
//
// What the rail's row says for a meter instrument when there is nothing new:
// how many meters are on the roster, because a neighbourhood has dozens and
// the count is the thing worth knowing without opening the window. With no
// roster it says only what kind of instrument it is - "0 MTR" would be a zero
// standing in for "we have not counted", which is the conflation rule two
// exists to stop.
inline void chipWord(bool have, std::size_t rowCount, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    if (!have) {
        std::snprintf(out, cap, "WAIT");
        return;
    }
    if (rowCount == 0u) {
        std::snprintf(out, cap, "MTR");
        return;
    }
    std::snprintf(out, cap, "%d MTR", static_cast<int>(rowCount));
}

}  // namespace cascade::gui::meter

#endif  // CASCADE_GUI_INSTRUMENT_METER_MATH_HPP
