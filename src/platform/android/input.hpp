// input.hpp - touch, translated into the two-button mouse the desktop
// interface was built for.
//
// WHY A TRANSLATION LAYER AT ALL. imgui_impl_android's HandleInputEvent maps
// one finger onto the left mouse button and stops there: it has no right
// button and no wheel from a touchscreen, because a touchscreen has neither.
// The FoxSDR interface uses both - a right-click opens the context menus on
// the spectrum, the map and the plugin rows, and the wheel scrolls every list
// and zooms the spectrum - so on a phone those two gestures have to come from
// somewhere or the corresponding features are simply unreachable.
//
// THE TWO GESTURES, and they are the platform conventions rather than
// inventions:
//   long press (one finger held still)  -> right button
//   two-finger drag                     -> wheel, both axes
//
// Everything else - taps, drags, a real mouse or a stylus over USB or
// Bluetooth - is passed straight through to the ImGui backend untouched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_PLATFORM_ANDROID_INPUT_HPP
#define CASCADE_PLATFORM_ANDROID_INPUT_HPP

#include <android/input.h>

#include <cstdint>

namespace cascade::platform::android {

class TouchInput {
public:
    // densityScale scales the movement slop and the scroll pitch, so "held
    // still" and "one notch of wheel" mean the same distance in millimetres on
    // a 320 dpi screen and a 560 dpi one.
    void configure(float densityScale);

    // Returns 1 when the event was consumed (the android_app onInputEvent
    // contract), 0 when it was not. Called on the native app thread, which is
    // the same thread the frame loop runs on, so no synchronisation is needed
    // between this and newFrame().
    int32_t handleEvent(AInputEvent* event);

    // Called once per frame, BEFORE ImGui::NewFrame. A long press is a timeout
    // rather than an event - nothing arrives from the system when a finger
    // stops moving - so the press that becomes a right-click has to be noticed
    // here.
    void newFrame();

private:
    void cancelLongPress();
    void releaseRightButton();

    float densityScale_ = 1.0f;
    float slopPx_ = 12.0f;        // movement that still counts as "held still"
    float scrollStepPx_ = 85.0f;  // finger travel per notch of wheel

    // --- long press ---
    bool pressTracking_ = false;   // a single finger is down and still eligible
    bool rightButtonHeld_ = false; // the long press fired and button 1 is down
    double pressStart_ = 0.0;      // CLOCK_MONOTONIC seconds
    float pressX_ = 0.0f;
    float pressY_ = 0.0f;

    // --- two-finger drag ---
    bool twoFingerActive_ = false;
    float lastCentroidX_ = 0.0f;
    float lastCentroidY_ = 0.0f;
    // Set when a second finger joins, cleared when the last finger leaves.
    // Without it, lifting one finger of a two-finger drag leaves the other
    // mid-gesture and the backend sees it as a fresh press - a stray click on
    // whatever the remaining finger happens to be over.
    bool suppressUntilAllUp_ = false;
};

}  // namespace cascade::platform::android

#endif  // CASCADE_PLATFORM_ANDROID_INPUT_HPP
