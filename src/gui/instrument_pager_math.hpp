// instrument_pager_math.hpp - the pager face's arithmetic, with no ImGui in it.
//
// SEPARATE FROM instrument_pager.cpp FOR THE REASON scope_view.hpp IS SEPARATE
// FROM scope_face.hpp: everything here is a pure function of numbers and
// characters, so tests/test_instrument_pager.cpp can exercise it without a
// graphics context, a window or a font atlas. The drawing file keeps the
// ImDrawList calls and nothing else worth pinning.
//
// WHAT IS IN HERE IS THE PART THAT CAN BE WRONG QUIETLY. A wrapped line that
// drops a word, a counter that shows 0 for "we have not counted", a capcode
// pushed one drum to the left - none of those crash and none of them look like
// a fault in a screenshot. They are exactly what a test can hold still.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_PAGER_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_PAGER_MATH_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cascade::gui::pager {

// The Motorola Advisor's screen, and the two numbers the whole face is laid
// out from: four lines of twenty characters, eighty characters a screen
// (Motorola's own Advisor II specification sheet, "Displays up to 4 Lines of
// Text", and IEEE Spectrum's Hall of Fame entry on the 1990 Advisor, "up to
// four lines of text with up to 20 characters per line"). The face draws a
// character LCD of exactly this shape rather than a text box that happens to
// hold some words.
inline constexpr int kLcdCols = 20;
inline constexpr int kLcdRows = 4;

// A character LCD has ONE FONT AND NO CODE PAGE, so everything outside its
// printable range is a space rather than a glyph. Drawing a byte the display
// could not have shown would be inventing a character, and the FLEX decoder
// upstream already replaces control codes for the same reason.
inline bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
inline char printable(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u < 0x20u || u > 0x7Eu) ? ' ' : c;
}

// One screen of characters, already wrapped. `lines` may be fewer than `rows`
// and is zero when there was nothing to show - which is how "no message" is
// carried through to the drawing without a sentinel string that could be
// mistaken for one.
struct LcdText {
    char line[kLcdRows][kLcdCols + 1] = {};
    int lines = 0;
    // The text ran past the last row. The real pager flashes an arrow in the
    // bottom right corner for exactly this ("The flashing arrow at the bottom
    // right corner of the screen indicates the message you received continues
    // beyond the first two lines of the display" - Advisor Pro User's Guide),
    // and the face draws one, so a truncated message never reads as a complete
    // short one.
    bool more = false;
};

// Word-wraps `s` into at most `rows` rows of kLcdCols characters.
//
//   - A word that fits on the next row is moved to it whole; a word longer
//     than a row is hard-split, because the alternative is an empty row
//     followed by the same problem.
//   - Runs of whitespace collapse to one space and leading spaces on a
//     wrapped row are dropped, so a message padded out to a word boundary -
//     which every FLEX alphanumeric page is - does not spend a row on air.
//   - Anything below 0x20 or above 0x7E becomes a space: this is a character
//     LCD with one font and no code page, and a control byte rendered as a
//     glyph would be an invented character.
//   - A NULL or empty string yields lines == 0, never a row of spaces.
//
// `rows` is clamped to [0, kLcdRows].
inline LcdText wrapLcd(const char* s, int rows) {
    LcdText out;
    if (rows > kLcdRows) { rows = kLcdRows; }
    if (s == nullptr) { return out; }
    if (rows <= 0) {
        // No screen to write on, but there IS text: the caller must still know
        // that something was not shown, or a page vanishes silently.
        std::size_t k = 0;
        while (s[k] != '\0' && isBlank(s[k])) { ++k; }
        out.more = s[k] != '\0';
        return out;
    }

    // One pass, one row at a time: fill the row, then decide where it breaks.
    // `i` is the read cursor into the message and never goes backwards, so a
    // pathological input cannot make this loop forever.
    std::size_t i = 0;
    while (s[i] != '\0' && out.lines < rows) {
        // Skip the whitespace this row would otherwise open with.
        while (s[i] != '\0' && isBlank(s[i])) { ++i; }
        if (s[i] == '\0') { break; }

        // How much of what follows fits, and where the last word boundary
        // inside it was.
        int n = 0;
        int lastSpace = -1;      // column of the last blank laid down
        std::size_t afterSpace = i;  // read position just past it
        std::size_t j = i;
        while (s[j] != '\0' && n < kLcdCols) {
            const char c = s[j];
            if (isBlank(c)) {
                // A run of blanks collapses to one, and a run at the end of a
                // row is simply eaten by the skip at the top of the next.
                lastSpace = n;
                ++j;
                while (s[j] != '\0' && isBlank(s[j])) { ++j; }
                afterSpace = j;
                if (s[j] == '\0') { break; }
                out.line[out.lines][n++] = ' ';
                continue;
            }
            out.line[out.lines][n++] = printable(c);
            ++j;
        }

        if (s[j] != '\0' && !isBlank(s[j]) && lastSpace > 0) {
            // The row stopped mid-word and there is an earlier break to use.
            // A word longer than a whole row has lastSpace <= 0 and is
            // hard-split instead, which is the only way it can ever be shown.
            n = lastSpace;
            j = afterSpace;
        }
        for (int k = n; k < kLcdCols; ++k) { out.line[out.lines][k] = '\0'; }
        out.line[out.lines][n] = '\0';
        if (n > 0) { ++out.lines; }
        i = j;
    }

    // Anything left over - after the trailing blanks a padded page carries -
    // is what the arrow in the corner is for.
    while (s[i] != '\0' && isBlank(s[i])) { ++i; }
    out.more = s[i] != '\0';
    return out;
}

// The status row: `left` at column 0 and `right` flush against the last
// column, at least one space between them. Returns false, and writes an empty
// string, when BOTH are empty - the caller then draws no row at all rather
// than a row of blanks, because a blank row on a character LCD is a row that
// was written with spaces and this one never existed.
//
// WHEN THEY WILL NOT BOTH FIT the RIGHT one is truncated first. On this face
// the left is the capcode - the identity of the page, and the one thing on the
// screen that cannot be guessed from the rest - and the right is the message
// kind, which the body of the message itself usually makes obvious. Truncating
// a capcode to make room for the word ALPHANUMERIC would be the wrong half to
// lose. If the left alone is longer than the row it is truncated too; nothing
// is ever written past `cap - 1` characters.
inline bool composeStatusLine(const char* left, const char* right, char* out,
                              std::size_t cap) {
    if (out == nullptr || cap == 0u) { return false; }
    out[0] = '\0';
    const char* l = (left != nullptr) ? left : "";
    const char* r = (right != nullptr) ? right : "";
    int ln = 0;
    while (l[ln] != '\0') { ++ln; }
    int rn = 0;
    while (r[rn] != '\0') { ++rn; }
    if (ln == 0 && rn == 0) { return false; }

    int cols = kLcdCols;
    if (static_cast<int>(cap) - 1 < cols) { cols = static_cast<int>(cap) - 1; }
    if (cols <= 0) { return false; }

    // The right-hand word gives way first, then the left. One space between
    // them is the minimum; below that the right is gone entirely.
    if (ln + 1 + rn > cols) { rn = cols - ln - 1; }
    if (rn < 0) { rn = 0; }
    if (ln > cols) { ln = cols; }

    int n = 0;
    for (int k = 0; k < ln; ++k) { out[n++] = printable(l[k]); }
    const int rightStart = cols - rn;
    while (n < rightStart) { out[n++] = ' '; }
    for (int k = 0; k < rn; ++k) { out[n++] = printable(r[k]); }
    // Trailing blanks are not characters anybody wrote; the row ends where the
    // last one does.
    while (n > 0 && out[n - 1] == ' ') { --n; }
    out[n] = '\0';
    return n > 0;
}

// The unread counter's drum cells, right-aligned into `cells` of them.
//
// NO READING IS BLANK, NOT ZERO. `have == false` leaves every cell ' ', and so
// does a negative count, because a pager that has heard nothing has not
// counted zero messages - that is the confusion instrument_face.hpp's second
// rule exists to stop, and a drum showing 00 is the most convincing way to
// make it.
//
// A count past what the drums can show is CLAMPED to all nines rather than
// rolled over. A mechanical counter does roll, but a face that showed 43 for
// 143 would be reporting a smaller number than it measured; all nines is at
// least honestly "more than this fits".
//
// `out` must hold `cells` characters; no NUL is written.
inline void counterCells(double value, bool have, char* out, int cells) {
    if (out == nullptr || cells <= 0) { return; }
    for (int k = 0; k < cells; ++k) { out[k] = ' '; }
    // NaN fails every comparison, so it lands here with the blanks - which is
    // the right answer for it too.
    if (!have || !(value >= 0.0)) { return; }

    double limit = 1.0;
    for (int k = 0; k < cells; ++k) { limit *= 10.0; }
    double v = std::floor(value + 0.5);
    if (v >= limit) {
        for (int k = 0; k < cells; ++k) { out[k] = '9'; }
        return;
    }
    long long n = static_cast<long long>(v);
    for (int k = cells - 1; k >= 0; --k) {
        out[k] = static_cast<char>('0' + static_cast<int>(n % 10));
        n /= 10;
        if (n == 0) { break; }
    }
}

// The same drums, fed a STRING - the capcode, which arrives from the plugin as
// text and may be seven digits, or fewer, or something that is not a number at
// all when the decoder could not convert an address into a published capcode.
// Right-aligned, blank-padded on the left, and the leading end is what is lost
// if it is too long, because the low digits of an identifier are the ones that
// distinguish two of them.
//
// An empty or NULL string leaves every cell blank. `out` must hold `cells`
// characters; no NUL is written.
inline void drumCells(const char* text, char* out, int cells) {
    if (out == nullptr || cells <= 0) { return; }
    for (int k = 0; k < cells; ++k) { out[k] = ' '; }
    if (text == nullptr) { return; }
    int n = 0;
    while (text[n] != '\0') { ++n; }
    if (n == 0) { return; }
    const int take = (n < cells) ? n : cells;
    for (int k = 0; k < take; ++k) {
        out[cells - 1 - k] = printable(text[n - 1 - k]);
    }
}

// The LCD's geometry inside the space the case can spare for it.
//
// A character cell is 5 dots wide and 7 tall on this kind of display, plus a
// dot of gap, so the cell's own aspect is 6:8 - and the glass is sized from
// the CELL rather than the cell from the glass, so twenty columns always
// occupy exactly twenty identical cells and the last one cannot fall off the
// right-hand edge through rounding.
struct LcdMetrics {
    float cellW = 0.0f;
    float cellH = 0.0f;
    float glassW = 0.0f;   // cellW * cols
    float glassH = 0.0f;   // cellH * (rows + status strip)
    float fontPx = 0.0f;   // the size one character is lettered at
    bool fits = false;     // false when the cell came out too small to letter
};

// `maxW` x `maxH` is what the case has to give. `stripRows` is the extra rows
// of height the icon strip above the characters costs (the envelope, the
// count and the time), expressed in character rows so it scales with them.
inline LcdMetrics lcdMetrics(float maxW, float maxH, int rows, int cols,
                             float stripRows) {
    LcdMetrics m;
    if (!(maxW > 0.0f) || !(maxH > 0.0f) || rows <= 0 || cols <= 0) { return m; }
    if (!(stripRows >= 0.0f)) { stripRows = 0.0f; }

    // 6 x 8 is a 5 x 7 dot cell with its dot of gap on two sides, which is what
    // every character LCD of this era used.
    constexpr float kCellAspect = 6.0f / 8.0f;
    const float totalRows = static_cast<float>(rows) + stripRows;
    const float byWidth = maxW / static_cast<float>(cols);
    const float byHeight = (maxH / totalRows) * kCellAspect;
    m.cellW = std::min(byWidth, byHeight);
    if (!(m.cellW > 0.0f)) { return m; }
    m.cellH = m.cellW / kCellAspect;
    m.glassW = m.cellW * static_cast<float>(cols);
    m.glassH = m.cellH * totalRows;
    // LARGER THAN THE CELL IS TALL, ON PURPOSE, and the number was measured
    // rather than chosen. A 5 x 7 dot character fills five of its cell's six
    // columns; the condensed face this bench letters words in fills barely
    // half of one at a size that fits the cell's height, and the first drawing
    // of this face came out reading as l e t t e r - s p a c e d text rather
    // than as a character display. The glyph is therefore set from the cell's
    // WIDTH: at 1.05 of the cell height a capital measures about two thirds of
    // the cell, which is as close to a dot matrix as a proportional face gets.
    // The drawing centres each glyph on its cap height rather than on its line
    // box, so a size past the cell height still sits where it belongs.
    m.fontPx = m.cellH * 1.05f;
    // Below about four pixels a character is a smudge, and a smudge that
    // claims to be a message is worse than an empty screen.
    m.fits = m.cellW >= 4.0f;
    return m;
}

// The pager's own outline, at the case's real proportions - 55 mm wide by
// 81 mm tall, from Motorola's specification sheet ("Dimension L x W x H:
// 81 x 55 x 18.5mm") - fitted into `availW` x `availH` and never larger than
// either. Height-led, because the case is taller than it is wide and the face
// is always wider than it is tall.
struct CaseSize {
    float w = 0.0f;
    float h = 0.0f;
};
inline CaseSize caseSize(float availW, float availH) {
    CaseSize c;
    if (!(availW > 0.0f) || !(availH > 0.0f)) { return c; }
    constexpr float kAspect = 55.0f / 81.0f;  // width over height, from the sheet
    c.h = availH;
    c.w = c.h * kAspect;
    if (c.w > availW) {
        c.w = availW;
        c.h = c.w / kAspect;
    }
    return c;
}

}  // namespace cascade::gui::pager

#endif  // CASCADE_GUI_INSTRUMENT_PAGER_MATH_HPP
