// Tests for gui/store_first_open.hpp - the plugin store reads the catalogue by
// itself the first time it is opened in a session, and never again on its own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/store_first_open.hpp"

#include "test_check.hpp"

using cascade::gui::StoreFirstOpen;
using cascade::gui::storeShouldCheckOnOpen;

int main() {
    // [1] Closed: nothing, however many frames, and the chance is kept for
    // when it IS opened. This is the "never at startup" half of the promise.
    {
        StoreFirstOpen s;
        for (int i = 0; i < 100; ++i) { CHECK(!storeShouldCheckOnOpen(s, false, false, false)); }
        CHECK(!s.settled);
        // First open asks...
        CHECK(storeShouldCheckOnOpen(s, true, false, false));
        // ...and exactly once: the next frame, and every frame after, do not.
        CHECK(!storeShouldCheckOnOpen(s, true, false, true));    // fetch now running
        CHECK(!storeShouldCheckOnOpen(s, true, false, false));   // fetch FAILED: no retry
        // Closed and reopened in the same session: still no retry.
        CHECK(!storeShouldCheckOnOpen(s, false, false, false));
        CHECK(!storeShouldCheckOnOpen(s, true, false, false));
    }

    // [2] A catalogue already read before the first open (CHECK NOW is only in
    // the window, but a later change could add another route): no second read,
    // and the chance is spent.
    {
        StoreFirstOpen s;
        CHECK(!storeShouldCheckOnOpen(s, true, true, false));
        CHECK(s.settled);
        CHECK(!storeShouldCheckOnOpen(s, true, false, false));
    }

    // [3] A transfer already running on first open: do not start another.
    {
        StoreFirstOpen s;
        CHECK(!storeShouldCheckOnOpen(s, true, false, true));
        CHECK(!storeShouldCheckOnOpen(s, true, false, false));
    }

    return testSummary("test_store_first_open");
}
