// Tests for gui/press_gesture.hpp - a still, short press on the waterfall is a
// click; a drag or a long press is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/press_gesture.hpp"

#include <cmath>
#include <cstdio>

#include "test_check.hpp"

using cascade::gui::kLongPressSeconds;
using cascade::gui::pressIsClick;

int main() {
    std::printf("  a still, short press is a click; a drag or a long press is not\n");
    CHECK(pressIsClick(0.08, false));
    CHECK(pressIsClick(kLongPressSeconds - 0.001, false));
    CHECK(!pressIsClick(kLongPressSeconds, false));  // Android's long press, exactly
    CHECK(!pressIsClick(2.0, false));
    CHECK(!pressIsClick(0.08, true));                // a pan
    CHECK(pressIsClick(std::nan(""), false));        // no clock: as before the rule
    CHECK(pressIsClick(-1.0, false));
    return testSummary("test_press_gesture");
}
