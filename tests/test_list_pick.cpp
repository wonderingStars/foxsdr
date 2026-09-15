/*
 * A COMBO MAY NOT READ THE LIST IT IS ITERATING AFTER THE PICK.
 *
 * WHAT THIS GUARDS. Three "crash cascade.exe @ ImHashStr" reports (0.90.1
 * twice, 0.95.1 once) came out of the REGION picker in the Display section:
 * the range-for held an iterator into bandPlanChoices_, the click handler
 * called loadBandPlan(), loadBandPlan() assigned a fresh vector over it, and
 * the next Selectable hashed a name pointer into freed storage. gui/list_pick.hpp
 * is the rule that replaces the loop, and this is the file that holds it.
 *
 * HOW THE CHECK CAN ACTUALLY SEE THE FAULT. Reading freed memory is not
 * observable from a test: in a Release build the old block is usually still
 * readable and the run passes. So the list here is not a vector but a TRIPWIRE
 * container that counts the reads it serves and stamps each with the
 * generation it was serving - and the row callback bumps the generation when
 * it makes the pick, exactly as loadBandPlan() does. A read stamped with a
 * generation that is no longer current IS the use-after-free, named and
 * counted instead of guessed at.
 *
 * THE BREAK-IT CHECK, run while writing this (0.96.1). Changing pickFromList's
 * `return i;` to a recorded-and-continue - which is the pre-fix shape - leaves
 * the loop reading items[i] after the mutation, and this file goes red:
 *
 *     FAIL tests/test_list_pick.cpp:40  got == want
 *          the loop stopped there: 4 (want 2)
 *     FAIL tests/test_list_pick.cpp:40  got == want
 *          no row was read after the rebuild: 2 (want 0)
 *
 * That second line is the use-after-free, counted instead of guessed at.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "gui/list_pick.hpp"
#include "test_check.hpp"

namespace {

// CHECK with the number in the failure line: a bare CHECK here would print
// only the expression, and the count IS the evidence.
void checkCount(const char* what, std::size_t got, std::size_t want) {
    CHECK(got == want);
    if (got != want) { std::printf("     %s: %zu (want %zu)\n", what, got, want); }
}

// A list that remembers what was read and WHEN. `generation` stands for the
// storage: bumping it is what `bandPlanChoices_ = available(dir)` does to the
// real one, and any read that arrives afterwards with an older stamp is a read
// of storage that no longer exists.
struct Tripwire {
    std::vector<std::string> rows;
    unsigned generation = 0;
    mutable unsigned reads = 0;
    mutable unsigned readsAfterRebuild = 0;

    std::size_t size() const { return rows.size(); }

    const std::string& operator[](std::size_t i) const {
        ++reads;
        if (generation != 0) { ++readsAfterRebuild; }
        return rows[i];
    }

    // What loadBandPlan() does: a whole new list over the old one.
    void rebuild(std::vector<std::string> fresh) {
        rows = std::move(fresh);
        ++generation;
    }
};

void testPickStopsAtTheFirstClick() {
    Tripwire list;
    list.rows = {"ITU Region 1", "ITU Region 2", "ITU Region 3", "United Kingdom"};

    // The row callback is the Selectable: it reports the click and does
    // NOTHING else. The rebuild stands where loadBandPlan() stands in the real
    // combo - after the loop, which is the whole point.
    std::size_t rowsDrawn = 0;
    const std::size_t pick =
        cascade::gui::pickFromList(list, [&](const std::string&, std::size_t i) {
            ++rowsDrawn;
            return i == 1;  // the user clicks the second region
        });

    checkCount("the second row is the pick", pick, std::size_t{1});
    checkCount("the loop stopped there", rowsDrawn, std::size_t{2});

    // Now the caller applies it, which is what rebuilds the list.
    const std::string chosen = list.rows[pick];
    list.rebuild({"ITU Region 1"});

    checkCount("read the list after the handler rebuilt it",
                  static_cast<std::size_t>(list.readsAfterRebuild), std::size_t{0});
    CHECK(chosen == "ITU Region 2");  // the pick survived the rebuild
}

// The same list, with the handler run FROM INSIDE the callback - the shape the
// fault had. pickFromList still leaves immediately, so even a caller that
// rebuilds too early cannot make the loop read the new storage.
void testARebuildInsideTheRowCallbackIsStillNotReadBack() {
    Tripwire list;
    list.rows = {"a", "b", "c", "d", "e"};

    const std::size_t pick =
        cascade::gui::pickFromList(list, [&](const std::string&, std::size_t i) {
            if (i != 2) { return false; }
            list.rebuild({"only one left"});  // the loadBandPlan() of the fault
            return true;
        });

    checkCount("the third row is the pick", pick, std::size_t{2});
    checkCount("no row was read after the rebuild",
                  static_cast<std::size_t>(list.readsAfterRebuild), std::size_t{0});
}

void testNoClickWalksTheWholeListAndAnswersNoPick() {
    Tripwire list;
    list.rows = {"a", "b", "c"};
    std::size_t rowsDrawn = 0;
    const std::size_t pick =
        cascade::gui::pickFromList(list, [&](const std::string&, std::size_t) {
            ++rowsDrawn;
            return false;
        });
    CHECK(pick == cascade::gui::kNoPick);
    checkCount("every row was offered", rowsDrawn, std::size_t{3});
}

void testAnEmptyListDrawsNothing() {
    Tripwire list;
    std::size_t rowsDrawn = 0;
    const std::size_t pick =
        cascade::gui::pickFromList(list, [&](const std::string&, std::size_t) {
            ++rowsDrawn;
            return true;
        });
    CHECK(pick == cascade::gui::kNoPick);
    checkCount("nothing drawn", rowsDrawn, std::size_t{0});
    checkCount("nothing read", static_cast<std::size_t>(list.reads), std::size_t{0});
}

}  // namespace

int main() {
    testPickStopsAtTheFirstClick();
    testARebuildInsideTheRowCallbackIsStillNotReadBack();
    testNoClickWalksTheWholeListAndAnswersNoPick();
    testAnEmptyListDrawsNothing();
    return testSummary("test_list_pick");
}
