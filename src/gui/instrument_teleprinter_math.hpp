// instrument_teleprinter_math.hpp - the arithmetic behind the cockpit
// printer's face: how many characters of paper fit across the strip, what the
// printed heading says, how a message wraps on it, how far the paper has fed,
// and the word the rail's chip carries.
//
// NO ImGui, ON PURPOSE, and for the reason scope_view.hpp gives: a header the
// drawing includes cannot be included by a test without a graphics context,
// and the parts of this face that can be WRONG - a heading that runs off the
// edge of the paper, a wrap that loses a word, a feed that never settles - are
// all arithmetic. They live here so tests/test_instrument_teleprinter.cpp can
// exercise them, and instrument_teleprinter.cpp does nothing but draw what
// these functions decide.
//
// Everything here is pure and allocation-free: fixed-size line buffers, no
// std::string, no heap. It runs once per frame on the GUI thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_TELEPRINTER_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_TELEPRINTER_MATH_HPP

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cascade::gui::teleprinter {

// A printed line of thermal paper. The width is the machine's, not the
// window's: real cockpit printers print a fixed number of columns and a
// narrow window shows fewer of them, so a line longer than this is a line
// this instrument could not have printed.
inline constexpr int kMaxPaperCols = 72;

// The strip never carries more than this many lines at once - the rest has
// already fed past the tear bar and lives in the memory table beneath the
// face, which is where the host draws it.
inline constexpr int kMaxPaperLines = 12;

// How long a sheet of paper takes to feed out when a message arrives, and how
// far it travels. Short enough that it reads as a machine advancing rather
// than as an animation being watched.
inline constexpr double kFeedSeconds = 0.5;
inline constexpr float kFeedDistancePx = 26.0f;

struct PaperLine {
    char text[kMaxPaperCols + 1];
};

// How many characters of `charWidthPx` fit across `widthPx` of paper.
// Clamped to [0, kMaxPaperCols]; a zero-width strip prints nothing rather
// than dividing by zero.
inline int paperColumns(float widthPx, float charWidthPx) {
    if (!(widthPx > 0.0f) || !(charWidthPx > 0.0f)) { return 0; }
    const float n = widthPx / charWidthPx;
    if (!(n >= 1.0f)) { return 0; }
    if (n >= static_cast<float>(kMaxPaperCols)) { return kMaxPaperCols; }
    return static_cast<int>(n);
}

namespace detail {

// Copies at most `columns` characters, always NUL-terminating. A line the
// paper is too narrow for is cut off at the edge of the paper, which is what
// a real printer does with an over-wide line and is why nothing here wraps a
// heading field.
inline void setLine(PaperLine& out, const char* s, int columns) {
    int n = 0;
    if (s != nullptr) {
        while (s[n] != '\0' && n < columns && n < kMaxPaperCols) { ++n; }
    }
    if (n > 0) { std::memcpy(out.text, s, static_cast<std::size_t>(n)); }
    out.text[n] = '\0';
}

// "REG G-EZBX" - a field and its value, or nothing at all when the plugin did
// not fill that slot. NO PLACEHOLDER: an empty slot means the block did not
// carry that field, and printing "REG -" would put a dash on the paper that
// the aircraft never sent.
inline int makeToken(char* out, std::size_t cap, const char* caption, const char* value) {
    if (out == nullptr || cap == 0) { return 0; }
    out[0] = '\0';
    if (value == nullptr || value[0] == '\0') { return 0; }
    return std::snprintf(out, cap, "%s %s", caption, value);
}

}  // namespace detail

// THE PRINTED HEADING, from the kind's slot map: registration, flight, label,
// mode and block, in that order, packed onto as few lines as `columns` allows
// with two spaces between fields - the way a message header comes off a
// cockpit printer rather than as a column of labelled boxes.
//
// Returns the number of lines written (0 when the plugin filled none of the
// five slots, which is the "no reading" the face must draw as no reading).
inline int paperHeading(const char* reg, const char* flight, const char* label,
                        const char* mode, const char* block, int columns,
                        PaperLine* out, int maxLines) {
    if (out == nullptr || maxLines <= 0 || columns <= 0) { return 0; }
    // A caller asking for more columns than the machine has gets the
    // machine's: every buffer below is sized to kMaxPaperCols, and clamping
    // here is what keeps the packing arithmetic inside them.
    if (columns > kMaxPaperCols) { columns = kMaxPaperCols; }
    struct Field {
        const char* caption;
        const char* value;
    };
    const Field fields[5] = {{"REG", reg},
                             {"FLT", flight},
                             {"LBL", label},
                             {"MODE", mode},
                             {"BLK", block}};

    int lines = 0;
    char line[kMaxPaperCols + 1];
    int used = 0;
    line[0] = '\0';
    for (int i = 0; i < 5; ++i) {
        char token[40];
        if (detail::makeToken(token, sizeof token, fields[i].caption, fields[i].value) <= 0) {
            continue;
        }
        const int tn = static_cast<int>(std::strlen(token));
        const int need = (used == 0) ? tn : used + 2 + tn;
        if (used != 0 && need > columns) {
            if (lines >= maxLines) { return lines; }
            detail::setLine(out[lines++], line, columns);
            used = 0;
            line[0] = '\0';
        }
        if (used != 0 && used + 2 <= kMaxPaperCols) {
            line[used++] = ' ';
            line[used++] = ' ';
        }
        int k = 0;
        while (token[k] != '\0' && used < kMaxPaperCols) { line[used++] = token[k++]; }
        line[used] = '\0';
    }
    if (used != 0 && lines < maxLines) { detail::setLine(out[lines++], line, columns); }
    return lines;
}

// Word-wraps `text` at `columns`. A word longer than the paper is broken at
// the edge rather than dropped - an ACARS free-text field legitimately
// carries unbroken runs (a route string, a waypoint list) and losing one
// would be losing the message.
//
// Returns the number of lines written, at most `maxLines`; text that does not
// fit is simply not printed, because paper is finite and the whole message is
// in the memory table below.
inline int wrapPaper(const char* text, int columns, PaperLine* out, int maxLines) {
    if (out == nullptr || maxLines <= 0 || columns <= 0 || text == nullptr) { return 0; }
    if (columns > kMaxPaperCols) { columns = kMaxPaperCols; }
    int lines = 0;
    std::size_t i = 0;
    const std::size_t n = std::strlen(text);
    while (i < n && lines < maxLines) {
        while (i < n && text[i] == ' ') { ++i; }
        if (i >= n) { break; }
        const std::size_t start = i;
        std::size_t end = start + static_cast<std::size_t>(columns);
        if (end >= n) {
            end = n;
        } else {
            // Back up to the last space inside the window, if there is one.
            std::size_t brk = end;
            while (brk > start && text[brk] != ' ') { --brk; }
            if (brk > start) { end = brk; }
        }
        int k = 0;
        for (std::size_t j = start; j < end && k < columns && k < kMaxPaperCols; ++j) {
            out[lines].text[k++] = text[j];
        }
        out[lines].text[k] = '\0';
        ++lines;
        i = end;
    }
    return lines;
}

// HOW FAR THE PAPER STILL HAS TO TRAVEL, in pixels, as a displacement to add
// to the strip's settled position: -`distancePx` the instant a message
// arrives and 0 once the sheet is out. Negative because the new print starts
// inside the machine and comes DOWN out of the slot.
//
// Smoothstep rather than linear: a stepper motor starts and stops, and a
// constant-velocity slide reads as a scrolling text box.
//
// Anything nonsensical - a negative or NaN elapsed time, a zero duration, a
// zero distance - settles immediately. A face that animated forever because
// the clock went backwards would be worse than one that never animated.
inline float paperFeedOffset(double elapsedSec, double durationSec, float distancePx) {
    if (!(distancePx > 0.0f) || !(durationSec > 0.0)) { return 0.0f; }
    if (!(elapsedSec >= 0.0)) { return 0.0f; }  // negative, or NaN
    if (elapsedSec >= durationSec) { return 0.0f; }
    const double t = elapsedSec / durationSec;
    const double eased = t * t * (3.0 - 2.0 * t);
    return -distancePx * static_cast<float>(1.0 - eased);
}

// THE WORD ON THE RAIL'S CHIP when nothing is unread. A printer's idle state
// is the last thing it printed, so the chip carries the REGISTRATION - which
// says what was heard - and falls back to the message count when the plugin
// filled no registration. `cap` is at least 16 by the instrumentChip contract.
//
// The count is clamped rather than wrapped: a counter shown more messages
// than it can print must read "as many as I can say", never the low digits of
// the truth, which would be a smaller number than the truth.
inline void chipWord(const char* reg, double messages, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) { return; }
    if (reg != nullptr && reg[0] != '\0') {
        std::snprintf(out, cap, "%s", reg);
        return;
    }
    double m = messages;
    if (!(m >= 0.0)) { m = 0.0; }  // negative, or NaN
    if (m > 99999.0) { m = 99999.0; }
    std::snprintf(out, cap, "%d MSG", static_cast<int>(m));
}

}  // namespace cascade::gui::teleprinter

#endif  // CASCADE_GUI_INSTRUMENT_TELEPRINTER_MATH_HPP
