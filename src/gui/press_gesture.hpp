// press_gesture.hpp - when a left press on the waterfall is a CLICK (tune
// there) and when it is not. Pure, so tests/test_press_gesture.cpp pins it.
//
// WHY A HOLD IS NOT A CLICK (2026-09-30). The receiver's waterfall tunes on
// the release of a press that never moved. Android has no right button: its
// touch layer (src/platform/android/input.cpp on the Android branches) turns
// a finger held still for 450 ms into "left button up, right button down" -
// so a long press, meant to open the marker menu and note a frequency WITHOUT
// tuning to it, first released the left button over the waterfall and tuned
// there. The same is true on a desktop for a user who holds the left button
// before deciding on the right one. A press held still for Android's own
// long-press time is a hold, and tunes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PRESS_GESTURE_HPP
#define CASCADE_GUI_PRESS_GESTURE_HPP

#include <cmath>

namespace cascade::gui {

// Android's touch layer's long-press time (kLongPressSeconds there).
inline constexpr double kLongPressSeconds = 0.45;

// A release is a click when the press neither moved past its slop nor lasted
// as long as a long press. A NaN or negative duration (a clock that went
// backwards) is read as a click: it is what every press was before this rule.
inline bool pressIsClick(double heldSeconds, bool moved) {
    if (moved) { return false; }
    return !(heldSeconds >= kLongPressSeconds);
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PRESS_GESTURE_HPP
