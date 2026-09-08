// The pager face's arithmetic: the character LCD's wrapping, its status row,
// the counter drums and the geometry the case and the glass are laid out from.
//
// NO IMGUI HERE, and that is the point of instrument_pager_math.hpp: everything
// below runs without a window, a font atlas or a graphics context, so the parts
// of the face that can be wrong QUIETLY - a dropped word, a counter reading 0
// when it has counted nothing, a capcode pushed one drum left - are held still
// by a test rather than by a screenshot.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstring>
#include <string>

#include "gui/instrument_pager_math.hpp"
#include "test_check.hpp"

namespace {

using cascade::gui::pager::caseSize;
using cascade::gui::pager::CaseSize;
using cascade::gui::pager::composeStatusLine;
using cascade::gui::pager::counterCells;
using cascade::gui::pager::drumCells;
using cascade::gui::pager::kLcdCols;
using cascade::gui::pager::kLcdRows;
using cascade::gui::pager::LcdMetrics;
using cascade::gui::pager::lcdMetrics;
using cascade::gui::pager::LcdText;
using cascade::gui::pager::wrapLcd;

// A bounds-safe read of one wrapped row. Indexing straight into `line` after a
// separate CHECK on `lines` is how a harness that RECORDS AND CONTINUES turns a
// failed expectation into an access violation - the run that has something to
// report is the one that crashes instead of naming it (test_config, 2026-08-16).
std::string row(const LcdText& t, int i) {
    if (i < 0 || i >= t.lines || i >= kLcdRows) { return std::string("<no row>"); }
    return std::string(t.line[i]);
}

// The drum helpers write no NUL, so every comparison goes through this.
std::string cells(const char* buf, int n) { return std::string(buf, buf + n); }

}  // namespace

int main() {
    // ---- the screen's own shape ------------------------------------------
    // Motorola's specification sheet and IEEE Spectrum both say four lines of
    // twenty characters, eighty to a screen. If these ever move, every layout
    // measurement in the face moves with them.
    // Read into plain locals so MSVC does not fold the comparison away and
    // warn about a constant condition (C4127) on a check that is doing its job.
    int cols = kLcdCols;
    int rows = kLcdRows;
    CHECK(cols == 20);
    CHECK(rows == 4);
    CHECK(cols * rows == 80);

    // ---- wrapping ---------------------------------------------------------
    {
        // Nothing at all is NOT a row of spaces: the face draws no rows and the
        // glass stays empty, which is what "we have not received a page" looks
        // like.
        const LcdText none = wrapLcd(nullptr, 4);
        CHECK(none.lines == 0);
        CHECK(!none.more);
        const LcdText empty = wrapLcd("", 4);
        CHECK(empty.lines == 0);
        CHECK(!empty.more);
        const LcdText blanks = wrapLcd("     ", 4);
        CHECK(blanks.lines == 0);
        CHECK(!blanks.more);
    }
    {
        // Exactly twenty characters is one row and no continuation arrow: the
        // off-by-one that would put a lone word on a second row, or claim more
        // text than exists, lives here.
        const LcdText t = wrapLcd("ABCDEFGHIJKLMNOPQRST", 4);
        CHECK(t.lines == 1);
        CHECK(row(t, 0) == "ABCDEFGHIJKLMNOPQRST");
        CHECK(!t.more);
    }
    {
        const LcdText t = wrapLcd("ABCDEFGHIJKLMNOPQRSTU", 4);
        CHECK(t.lines == 2);
        CHECK(row(t, 0) == "ABCDEFGHIJKLMNOPQRST");
        CHECK(row(t, 1) == "U");
        CHECK(!t.more);
    }
    {
        // The demonstration page, and the one every screenshot of this face is
        // taken with: three full rows and text still to come.
        const LcdText t =
            wrapLcd("CALL DISPATCH RE UNIT 4 ETA 20 MIN BRING SPARE ANTENNA AND LOG", 3);
        CHECK(t.lines == 3);
        CHECK(row(t, 0) == "CALL DISPATCH RE");
        CHECK(row(t, 1) == "UNIT 4 ETA 20 MIN");
        CHECK(row(t, 2) == "BRING SPARE ANTENNA");
        CHECK(t.more);
        // No row may exceed the screen, ever.
        for (int i = 0; i < t.lines; ++i) { CHECK(row(t, i).size() <= 20u); }
    }
    {
        // The same page with all four rows: it fits, and the arrow goes out.
        const LcdText t =
            wrapLcd("CALL DISPATCH RE UNIT 4 ETA 20 MIN BRING SPARE ANTENNA AND LOG", 4);
        CHECK(t.lines == 4);
        CHECK(row(t, 3) == "AND LOG");
        CHECK(!t.more);
    }
    {
        // A word longer than the whole row is hard-split rather than dropped -
        // the alternative is an empty row followed by the same problem for ever.
        const LcdText t = wrapLcd("SUPERCALIFRAGILISTICEXPIALIDOCIOUS", 4);
        CHECK(t.lines == 2);
        CHECK(row(t, 0) == "SUPERCALIFRAGILISTIC");
        CHECK(row(t, 1) == "EXPIALIDOCIOUS");
        CHECK(!t.more);
    }
    {
        // A FLEX alphanumeric page is padded out to a word boundary, so runs of
        // blanks are the normal case and must not cost a row.
        const LcdText t = wrapLcd("  HELLO     THERE   ", 4);
        CHECK(t.lines == 1);
        CHECK(row(t, 0) == "HELLO THERE");
        CHECK(!t.more);
    }
    {
        // Control bytes become spaces: this is a character LCD with one font
        // and no code page, and a glyph for 0x07 would be an invented character.
        const LcdText t = wrapLcd("A\x01" "B\tC\x7F" "D", 4);
        CHECK(t.lines == 1);
        CHECK(row(t, 0) == "A B C D");
    }
    {
        // The largest thing the slot can hold - CASCADE_INSTRUMENT_TEXT_CHARS
        // is 64 - and rows must still be full, in order, and bounded.
        std::string big;
        for (int i = 0; i < 63; ++i) { big.push_back(static_cast<char>('A' + (i % 26))); }
        const LcdText t = wrapLcd(big.c_str(), 4);
        CHECK(t.lines == 4);
        CHECK(row(t, 0) == big.substr(0, 20));
        CHECK(row(t, 1) == big.substr(20, 20));
        CHECK(row(t, 2) == big.substr(40, 20));
        CHECK(row(t, 3) == big.substr(60, 3));
        CHECK(!t.more);
    }
    {
        // Zero rows asked for, and more rows than the screen has: neither may
        // write outside the array.
        const LcdText none = wrapLcd("ANYTHING", 0);
        CHECK(none.lines == 0);
        CHECK(none.more);
        const LcdText clamped = wrapLcd("ANYTHING", 99);
        CHECK(clamped.lines == 1);
        CHECK(row(clamped, 0) == "ANYTHING");
    }

    // ---- the status row ---------------------------------------------------
    {
        char out[kLcdCols + 1];
        CHECK(composeStatusLine("1234567", "ALPHA", out, sizeof out));
        // Built rather than typed: a literal with the wrong number of spaces
        // in it fails for a reason that has nothing to do with the code.
        CHECK(std::string(out) == std::string("1234567") + std::string(8, ' ') + "ALPHA");
        CHECK(std::strlen(out) == 20u);
    }
    {
        char out[kLcdCols + 1];
        // One side only: still a row, and no phantom blanks after it.
        CHECK(composeStatusLine("1234567", "", out, sizeof out));
        CHECK(std::string(out) == "1234567");
        CHECK(composeStatusLine("", "TONE", out, sizeof out));
        CHECK(std::string(out) == std::string(16, ' ') + "TONE");
    }
    {
        char out[kLcdCols + 1];
        // Neither: NO ROW. A row of spaces would be a row somebody wrote.
        CHECK(!composeStatusLine("", "", out, sizeof out));
        CHECK(out[0] == '\0');
        CHECK(!composeStatusLine(nullptr, nullptr, out, sizeof out));
        CHECK(out[0] == '\0');
    }
    {
        char out[kLcdCols + 1];
        // Too long together: the RIGHT gives way, because the capcode is the
        // identity of the page and the kind is not.
        CHECK(composeStatusLine("0123456789ABCDEF", "NUMERIC", out, sizeof out));
        CHECK(std::string(out) == "0123456789ABCDEF NUM");
        // Too long alone: cut to the row, never past it.
        CHECK(composeStatusLine("0123456789ABCDEFGHIJKLMNOP", "X", out, sizeof out));
        CHECK(std::strlen(out) == 20u);
        CHECK(std::string(out) == "0123456789ABCDEFGHIJ");
    }
    {
        // A caller with no room at all must be refused rather than written to.
        char tiny[1] = {'Z'};
        CHECK(!composeStatusLine("A", "B", tiny, 1));
        CHECK(tiny[0] == '\0');
        CHECK(!composeStatusLine("A", "B", nullptr, 8));
    }

    // ---- the counter drums ------------------------------------------------
    {
        char c[2];
        // THE RULE THIS FACE EXISTS TO KEEP: no reading is blank, not zero.
        counterCells(0.0, false, c, 2);
        CHECK(cells(c, 2) == "  ");
        counterCells(7.0, false, c, 2);
        CHECK(cells(c, 2) == "  ");
        // A real zero IS a reading and is shown.
        counterCells(0.0, true, c, 2);
        CHECK(cells(c, 2) == " 0");
        counterCells(3.0, true, c, 2);
        CHECK(cells(c, 2) == " 3");
        counterCells(52.0, true, c, 2);  // the equipment's own message capacity
        CHECK(cells(c, 2) == "52");
        counterCells(99.0, true, c, 2);
        CHECK(cells(c, 2) == "99");
        // Past the drums: clamped, not rolled over. 143 shown as 43 would be a
        // smaller number than was measured.
        counterCells(143.0, true, c, 2);
        CHECK(cells(c, 2) == "99");
        counterCells(1e18, true, c, 2);
        CHECK(cells(c, 2) == "99");
        // Nonsense is no reading, not a reading of nonsense.
        counterCells(-1.0, true, c, 2);
        CHECK(cells(c, 2) == "  ");
        counterCells(std::nan(""), true, c, 2);
        CHECK(cells(c, 2) == "  ");
        // Rounding, not truncation.
        counterCells(2.6, true, c, 2);
        CHECK(cells(c, 2) == " 3");
    }
    {
        // Zero cells and a null buffer must not write anything.
        char c[4] = {'a', 'b', 'c', 'd'};
        counterCells(5.0, true, c, 0);
        CHECK(cells(c, 4) == "abcd");
        counterCells(5.0, true, nullptr, 4);
        drumCells("1", nullptr, 4);
        drumCells(nullptr, c, 4);
        CHECK(cells(c, 4) == "    ");
    }
    {
        char c[7];
        drumCells("1234567", c, 7);
        CHECK(cells(c, 7) == "1234567");
        drumCells("77", c, 7);
        CHECK(cells(c, 7) == "     77");
        drumCells("", c, 7);
        CHECK(cells(c, 7) == "       ");
        // Longer than the drums: the LOW digits survive, because they are what
        // distinguishes two identifiers.
        drumCells("1234567890", c, 7);
        CHECK(cells(c, 7) == "4567890");
        // Not a number at all - the decoder prints a raw address when there is
        // no published capcode conversion - passed through as it stands.
        drumCells("A1B2C3D", c, 7);
        CHECK(cells(c, 7) == "A1B2C3D");
        // And an unprintable byte is a blank, not a glyph.
        drumCells("12\x01" "456", c, 7);
        CHECK(cells(c, 7) == " 12 456");
    }

    // ---- the geometry -----------------------------------------------------
    {
        // A zero-size rectangle must not crash and must not claim to fit.
        const LcdMetrics z = lcdMetrics(0.0f, 0.0f, 4, 20, 1.25f);
        CHECK(!z.fits);
        CHECK(z.cellW == 0.0f);
        const LcdMetrics neg = lcdMetrics(-40.0f, 100.0f, 4, 20, 1.25f);
        CHECK(!neg.fits);
        const LcdMetrics noRows = lcdMetrics(300.0f, 200.0f, 0, 20, 1.25f);
        CHECK(!noRows.fits);
    }
    {
        // Width-led: twenty cells fill the glass exactly, and the glass never
        // exceeds what it was given.
        const LcdMetrics m = lcdMetrics(240.0f, 400.0f, 4, 20, 1.25f);
        CHECK(m.fits);
        CHECK_NEAR(m.cellW, 12.0f, 1e-4);
        CHECK_NEAR(m.glassW, 240.0f, 1e-3);
        CHECK(m.glassW <= 240.0f + 1e-3f);
        CHECK(m.glassH <= 400.0f + 1e-3f);
        CHECK_NEAR(m.cellH, 16.0f, 1e-4);  // a 6 x 8 dot cell
        // The glyph is set from the cell's WIDTH, so it is deliberately a
        // little taller than the cell; the drawing centres it on its cap
        // height. What must not happen is a size that could never fit a row.
        CHECK(m.fontPx > m.cellW);
        CHECK(m.fontPx < m.cellH * 1.3f);
    }
    {
        // Height-led: the same, the other way round.
        const LcdMetrics m = lcdMetrics(600.0f, 105.0f, 4, 20, 1.25f);
        CHECK(m.fits);
        CHECK(m.glassW <= 600.0f + 1e-3f);
        CHECK(m.glassH <= 105.0f + 1e-3f);
        CHECK_NEAR(m.glassH, 105.0f, 1e-3);
    }
    {
        // Too small to letter: the face is told so rather than drawing a
        // smudge that claims to be a message.
        const LcdMetrics m = lcdMetrics(60.0f, 200.0f, 4, 20, 1.25f);
        CHECK(!m.fits);
        CHECK(m.cellW > 0.0f);  // still measured, just not usable
    }
    {
        // The case keeps the equipment's own proportions - 55 x 81 mm - and
        // fits inside what it is given, whichever way round the space is.
        const CaseSize tall = caseSize(1000.0f, 405.0f);
        CHECK_NEAR(tall.h, 405.0f, 1e-3);
        CHECK_NEAR(tall.w, 275.0f, 1e-2);
        CHECK(tall.w <= 1000.0f);
        const CaseSize wide = caseSize(110.0f, 1000.0f);
        CHECK_NEAR(wide.w, 110.0f, 1e-3);
        CHECK_NEAR(wide.h, 162.0f, 1e-2);
        CHECK(wide.h <= 1000.0f);
        const CaseSize none = caseSize(0.0f, 0.0f);
        CHECK(none.w == 0.0f && none.h == 0.0f);
        const CaseSize bad = caseSize(-5.0f, 40.0f);
        CHECK(bad.w == 0.0f && bad.h == 0.0f);
    }

    return testSummary("test_instrument_pager");
}
