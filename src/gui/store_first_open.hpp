// store_first_open.hpp - when opening the plugin store reads the catalogue by
// itself. No ImGui.
//
// WHY THIS EXISTS (0.99.16). Until now the store window opened on "CATALOGUE
// NOT READ" and waited for CHECK NOW, so the first thing every user saw in a
// store was an empty one. The owner asked for it to look on first entry. It
// still never looks at STARTUP - the window does not reopen by itself (0.79.1)
// - so the catalogue is only ever contacted because the user opened the store
// or pressed CHECK NOW.
//
// ONCE PER SESSION. The first time the window is open with no catalogue read
// and nothing in flight, it asks. After that it never asks again on its own:
// a check that failed is shown as failed, with its reason, and asking again
// every time the window is reopened would be retrying a broken network behind
// the user's back. CHECK NOW is still there for that.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_STORE_FIRST_OPEN_HPP
#define CASCADE_GUI_STORE_FIRST_OPEN_HPP

namespace cascade::gui {

struct StoreFirstOpen {
    bool settled = false;   // the session's one automatic chance has been used or made moot
};

// Called every frame the store could be drawn. True exactly once per session:
// on the first frame the window is open, no catalogue has been read and no
// transfer is running. A catalogue already read, or a transfer already in
// flight, settles it without asking, because the question it would ask has
// already been asked.
inline bool storeShouldCheckOnOpen(StoreFirstOpen& s, bool windowOpen, bool haveCatalogue,
                                   bool transferRunning) {
    if (s.settled || !windowOpen) { return false; }
    s.settled = true;
    return !haveCatalogue && !transferRunning;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_STORE_FIRST_OPEN_HPP
