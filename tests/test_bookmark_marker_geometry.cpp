// Tests for gui/bookmark_marker_geometry.hpp - the row a clashing bookmark
// name stacks onto instead of being hidden ("Stack close names", an Italian
// user's request: two bookmarks a couple of kHz apart on a wide span used to
// drop the second name entirely once its extent touched the first's).
//
// What each leg proves:
//  1. No clash against anything already placed -> row 0, the row the
//     un-stacked code always used.
//  2. A clash on row 0 alone -> row 1, trying the next row down rather than
//     giving up.
//  3. All three rows full -> -1, exactly the "hide it, the tick still shows"
//     rule the un-stacked code applied to its one row.
//  4. Stacking off is a one-row budget: a candidate that would have found
//     room on row 1 with stacking on finds none with only row 0 offered.
//  5. bookmarkNameRowCapacity: three rows when there is height for all of
//     them, fewer on a short spectrum, never less than one, never more than
//     the cap the caller passes (letting the same function turn stacking off
//     by capping at 1).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/bookmark_marker_geometry.hpp"
#include "test_check.hpp"

int main() {
    using cascade::gui::bookmarkNameRowCapacity;
    using cascade::gui::chooseBookmarkNameRow;
    using cascade::gui::kBookmarkNameGapPx;
    using cascade::gui::kBookmarkNameMaxRows;

    static_assert(kBookmarkNameMaxRows == 3, "the request was three rows");

    // --- 1. No clash -> row 0. -----------------------------------------------
    {
        const std::vector<std::vector<std::pair<float, float>>> placed = {{}, {}, {}};
        CHECK(chooseBookmarkNameRow(placed, 100.0f, 150.0f) == 0);
    }
    {
        // Room to the side of something already on row 0 is still row 0 -
        // this is the ordinary "two names far enough apart" case, unchanged
        // from before stacking existed.
        const std::vector<std::vector<std::pair<float, float>>> placed = {
            {{0.0f, 40.0f}}, {}, {}};
        CHECK(chooseBookmarkNameRow(placed, 200.0f, 260.0f) == 0);
    }

    // --- 2. A clash on row 0 alone -> row 1. ----------------------------------
    {
        const std::vector<std::vector<std::pair<float, float>>> placed = {
            {{90.0f, 160.0f}}, {}, {}};
        // [100, 150] is inside the gap of [90, 160] on row 0, clear on row 1.
        CHECK(chooseBookmarkNameRow(placed, 100.0f, 150.0f) == 1);
    }
    {
        // Row 0 AND row 1 both taken at this x -> row 2.
        const std::vector<std::vector<std::pair<float, float>>> placed = {
            {{90.0f, 160.0f}}, {{95.0f, 155.0f}}, {}};
        CHECK(chooseBookmarkNameRow(placed, 100.0f, 150.0f) == 2);
    }
    {
        // The gap itself: within kBookmarkNameGapPx of a placed extent is a
        // clash; just clear of it is not - the same boundary the single-row
        // code used, now checked per row.
        const std::vector<std::vector<std::pair<float, float>>> placed = {
            {{0.0f, 100.0f}}, {}, {}};
        CHECK(chooseBookmarkNameRow(placed, 100.0f + kBookmarkNameGapPx - 0.5f, 140.0f) == 1);
        CHECK(chooseBookmarkNameRow(placed, 100.0f + kBookmarkNameGapPx + 0.5f, 140.0f) == 0);
    }

    // --- 3. Rows 0-2 all full -> -1. -------------------------------------------
    {
        const std::vector<std::vector<std::pair<float, float>>> placed = {
            {{90.0f, 160.0f}}, {{90.0f, 160.0f}}, {{90.0f, 160.0f}}};
        CHECK(chooseBookmarkNameRow(placed, 100.0f, 150.0f) == -1);
    }

    // --- 4. Stacking off: only row 0 is ever offered. -------------------------
    {
        // The exact candidate that resolved to row 1 in case 2 above, but
        // with a one-row budget (what drawBookmarkMarkers passes when the
        // "Stack close names" checkbox is off) - it must come back -1, not
        // silently fall through to a row nobody offered.
        const std::vector<std::vector<std::pair<float, float>>> placed = {{{90.0f, 160.0f}}};
        CHECK(chooseBookmarkNameRow(placed, 100.0f, 150.0f) == -1);
        // An empty one-row budget still finds row 0, so turning stacking off
        // does not touch the ordinary "one name, no clash" case.
        const std::vector<std::vector<std::pair<float, float>>> empty1 = {{}};
        CHECK(chooseBookmarkNameRow(empty1, 100.0f, 150.0f) == 0);
    }

    // --- 5. bookmarkNameRowCapacity ---------------------------------------------
    {
        // Comfortably tall: all three rows fit, capped at the cap given.
        CHECK(bookmarkNameRowCapacity(200.0f, 14.0f, 3) == 3);
        CHECK(bookmarkNameRowCapacity(200.0f, 14.0f, 1) == 1);  // stacking off
        // Room for exactly one extra row's pitch -> two rows.
        CHECK(bookmarkNameRowCapacity(14.0f, 14.0f, 3) == 2);
        // Just short of a second row's pitch -> row 0 only, never zero.
        CHECK(bookmarkNameRowCapacity(13.0f, 14.0f, 3) == 1);
        CHECK(bookmarkNameRowCapacity(0.0f, 14.0f, 3) == 1);
        CHECK(bookmarkNameRowCapacity(-40.0f, 14.0f, 3) == 1);  // the short-spectrum case
        // Two full extra rows' worth of height -> three rows, not four: the
        // cap wins even when the height would allow more.
        CHECK(bookmarkNameRowCapacity(500.0f, 14.0f, 3) == 3);
        // A degenerate pitch never divides by zero or goes negative.
        CHECK(bookmarkNameRowCapacity(200.0f, 0.0f, 3) == 1);
    }

    return testSummary("test_bookmark_marker_geometry");
}
