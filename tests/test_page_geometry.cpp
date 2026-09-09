// Tests for gui/page_geometry.hpp - the floor a torn-off page cannot be
// dragged under, and the reset generation that puts an already-wrong one back.
//
// The fault these guard against, in the beta tester's own words: "somehow I
// messed up the window size of the ACARS printer and cannot get it back"
// (2026-09-09). The page had been dragged down to its title strip; beginPage
// draws no body below 80 px, and the resize grip on these windows is invisible
// by design, so there was almost nothing left to take hold of.
//
// What each leg proves:
//  1. A size under the floor clamps up ON BOTH AXES - including the tester's
//     own shape, wide enough and far too short, where only one axis moves.
//  2. A size above the floor is untouched: this is a floor, not a resize.
//  3. The floor itself is KEPT, not nudged. The boundary is the one value a
//     >= / > slip would move, and moving it would fight every window that
//     legitimately sits at the minimum.
//  4. A page whose seen generation is behind the current one is re-placed
//     EXACTLY ONCE - the second frame must not re-place it again, or a reset
//     would pin the window at its default and no drag would ever hold.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>

#include "gui/page_geometry.hpp"
#include "test_check.hpp"

int main() {
    using cascade::gui::clampPageSize;
    using cascade::gui::kPageMinH;
    using cascade::gui::kPageMinW;
    using cascade::gui::pageNeedsReset;

    // --- 1. Under the floor clamps up, on both axes. ------------------------
    {
        float w = 10.0f;
        float h = 4.0f;
        clampPageSize(w, h);
        CHECK(w == kPageMinW);
        CHECK(h == kPageMinH);
    }
    {
        // THE TESTER'S OWN WINDOW: a full-width page dragged down to its title
        // strip. Only the height is wrong, and a clamp that moved the width as
        // well would throw away a rectangle the user chose.
        float w = 980.0f;
        float h = 30.0f;
        clampPageSize(w, h);
        CHECK(w == 980.0f);
        CHECK(h == kPageMinH);
    }
    {
        // ...and the other way round, so neither axis is carrying the other.
        float w = 96.0f;
        float h = 620.0f;
        clampPageSize(w, h);
        CHECK(w == kPageMinW);
        CHECK(h == 620.0f);
    }

    // --- 2. Over the floor is untouched. ------------------------------------
    {
        float w = 720.0f;
        float h = 520.0f;
        clampPageSize(w, h);
        CHECK(w == 720.0f);
        CHECK(h == 520.0f);
    }

    // --- 3. The floor itself is kept. ---------------------------------------
    {
        float w = kPageMinW;
        float h = kPageMinH;
        clampPageSize(w, h);
        CHECK(w == kPageMinW);
        CHECK(h == kPageMinH);
    }
    {
        // One pixel under is still under.
        float w = kPageMinW - 1.0f;
        float h = kPageMinH - 1.0f;
        clampPageSize(w, h);
        CHECK(w == kPageMinW);
        CHECK(h == kPageMinH);
    }

    // The floor has to be above the size at which beginPage stops drawing a
    // body (80 px in either direction) or it is not a floor at all. Both
    // numbers are compile-time constants, so this is checked where a lowered
    // one would be caught by the build rather than by a run.
    static_assert(kPageMinW > 80.0f, "the floor must clear the strip fallback");
    static_assert(kPageMinH > 80.0f, "the floor must clear the strip fallback");

    // --- 4. A reset happens exactly once per generation. ---------------------
    {
        std::uint32_t seen = 0u;
        // Nothing has been asked for: the page keeps whatever the user gave it.
        CHECK(pageNeedsReset(seen, 0u) == false);
        CHECK(seen == 0u);

        // The key is pressed.
        CHECK(pageNeedsReset(seen, 1u) == true);
        CHECK(seen == 1u);
        // ...and the next frame leaves the window alone, so a drag holds.
        CHECK(pageNeedsReset(seen, 1u) == false);
        CHECK(pageNeedsReset(seen, 1u) == false);

        // Pressed again: another single re-placement.
        CHECK(pageNeedsReset(seen, 2u) == true);
        CHECK(seen == 2u);
        CHECK(pageNeedsReset(seen, 2u) == false);
    }
    {
        // A PAGE THAT WAS NOT OPEN WHEN THE KEY WAS PRESSED. Its counter is
        // several generations behind and it catches up on its first frame,
        // once - this is why the generation is a counter and not a flag the
        // presser has to deliver to every window it can find.
        std::uint32_t seen = 0u;
        CHECK(pageNeedsReset(seen, 7u) == true);
        CHECK(seen == 7u);
        CHECK(pageNeedsReset(seen, 7u) == false);
    }
    {
        // A page created AFTER the key was pressed starts at the current
        // generation, so it opens where its call site put it and is not
        // re-placed on top of that.
        std::uint32_t seen = 4u;
        CHECK(pageNeedsReset(seen, 4u) == false);
        CHECK(seen == 4u);
    }

    return testSummary("test_page_geometry");
}
