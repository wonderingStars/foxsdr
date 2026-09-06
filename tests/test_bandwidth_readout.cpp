/*
 * THE BANDWIDTH THE COMBO SAYS MUST BE THE BANDWIDTH THE RECEIVER IS RUNNING.
 *
 * WHY THIS EXISTS. The NOAA APT preset asks for 40 kHz, deliberately: an APT
 * signal is about 34 kHz wide and 12.5 kHz slices its video sidebands off, so
 * a picture taken at 12.5 kHz is ruined in a way that looks like poor
 * reception rather than like a setting. The Bandwidth combo carries six steps
 * and 40 kHz is not one of them. The code used to point the combo at the
 * NEAREST step, which is 12.5 kHz, and two things followed:
 *
 *   - the combo lettered "12.5k" while the receiver ran 40 kHz, so the reading
 *     was simply false; and
 *   - every control that writes that index back writes kBwHz[index] with it,
 *     so the next touch of the combo or of any mode button really did narrow
 *     the receiver to the figure the combo had been wrongly showing. The
 *     display was not merely wrong, it was wrong in a way that came true.
 *
 * The fix makes the preview a rendering of the ACTUAL bandwidth and redefines
 * the index as "the offered step, or -1 for none of them". These two pure
 * functions are that fix, and until this file they had no test: they were
 * private to app_window.cpp, and nothing in the suite can construct an
 * AppWindow, so the only check they got was running the application and
 * reading the combo. They were moved into app_window.hpp so this file can see
 * them, for the same reason railRowHeight and railChipReserve live there.
 *
 * WHAT THIS CANNOT CHECK: that the widget draws this preview, or that
 * BeginCombo does not write on a click that changes nothing. Those need a live
 * ImGui frame and an AppWindow. What is pinned here is the arithmetic that
 * decides what the widget is asked to say, and the boundary the fault lived on.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstring>
#include <string>

#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace {

std::string shown(double hz) {
    char buf[32];
    cascade::gui::formatBandwidth(hz, buf, sizeof(buf));
    return std::string(buf);
}

// Every step the combo offers reads exactly as its own label does. If these
// two ever disagree the combo shows one word for the value and another for the
// step beside it, which is the confusion this whole change exists to end.
void testEveryOfferedStepReadsAsItsLabel() {
    const char* const labels[] = {"200k", "150k", "12.5k", "10k", "6k", "3k"};
    for (int i = 0; i < cascade::gui::kBwCount; ++i) {
        CHECK(shown(cascade::gui::kBwHz[i]) == std::string(labels[i]));
        // ...and each one is recognised as itself, not as a neighbour.
        CHECK(cascade::gui::bandwidthStepIndex(cascade::gui::kBwHz[i]) == i);
    }
}

// THE CASE THE FAULT WAS. 40 kHz is not an offered step; it must read as
// itself and must NOT be claimed to be one of them.
void testAPresetBandwidthReadsAsItselfAndIsNotAStep() {
    CHECK(shown(40000.0) == "40k");
    CHECK(cascade::gui::bandwidthStepIndex(40000.0) == -1);

    // The specific wrong answer the old code gave, named so a regression is
    // unmistakable: 12.5 kHz is the nearest step to 40 kHz, and returning its
    // index here is exactly what made the combo lie and then come true.
    const int nearestWrongAnswer = 2;  // kBwHz[2] == 12500
    CHECK(cascade::gui::bandwidthStepIndex(40000.0) != nearestWrongAnswer);
}

// Other bandwidths a preset might reasonably ask for, none of them in the
// table, each of which must survive being displayed.
void testOffTableValuesRenderReadably() {
    CHECK(shown(34000.0) == "34k");
    CHECK(shown(25000.0) == "25k");
    CHECK(shown(8000.0) == "8k");
    // A value carrying hertz keeps them rather than being rounded into a lie.
    CHECK(shown(12345.0) == "12.345k");
    // One hertz is the resolution the three decimals buy, and it is kept.
    CHECK(shown(3001.0) == "3.001k");
    for (double hz : {40000.0, 34000.0, 25000.0, 8000.0, 12345.0, 3001.0}) {
        CHECK(cascade::gui::bandwidthStepIndex(hz) == -1);
    }
}

// The tolerance exists for a config round-trip, not for rounding a genuinely
// different bandwidth onto a step. An eighth of a hertz either side counts;
// a whole hertz does not.
void testTheToleranceIsForARoundTripAndNothingWider() {
    CHECK(cascade::gui::bandwidthStepIndex(12500.05) == 2);
    CHECK(cascade::gui::bandwidthStepIndex(12499.95) == 2);
    CHECK(cascade::gui::bandwidthStepIndex(12501.0) == -1);
    CHECK(cascade::gui::bandwidthStepIndex(12499.0) == -1);
}

// formatBandwidth writes through a caller's buffer. A buffer too small to hold
// the answer must leave something terminated rather than run past the end -
// the project has been bitten before by a test whose own out-of-bounds write
// was the fault it was chasing.
void testItNeverRunsPastTheBufferItIsGiven() {
    char small[4];
    std::memset(small, 'X', sizeof(small));
    cascade::gui::formatBandwidth(200000.0, small, sizeof(small));
    bool terminated = false;
    for (char c : small) {
        if (c == '\0') { terminated = true; }
    }
    CHECK(terminated);

    // The smallest buffer that can hold the longest ordinary answer.
    char exact[8];
    cascade::gui::formatBandwidth(150000.0, exact, sizeof(exact));
    CHECK(std::string(exact) == "150k");
}

}  // namespace

int main() {
    testEveryOfferedStepReadsAsItsLabel();
    testAPresetBandwidthReadsAsItselfAndIsNotAStep();
    testOffTableValuesRenderReadably();
    testTheToleranceIsForARoundTripAndNothingWider();
    testItNeverRunsPastTheBufferItIsGiven();
    return testSummary("test_bandwidth_readout");
}
