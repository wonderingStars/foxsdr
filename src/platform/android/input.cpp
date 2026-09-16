// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "input.hpp"

#include <time.h>

#include <cmath>

#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_impl_android.h"

namespace cascade::platform::android {
namespace {

// How long a finger must stay down, and how still, before it becomes a right
// click. 450 ms is Android's own ViewConfiguration default for a long press
// (LONG_PRESS_TIMEOUT = 500 ms) rounded down a little: a user who has learned
// the platform gesture expects roughly this, and a value the platform would
// call a long press but this application would not is the worst of both.
constexpr double kLongPressSeconds = 0.45;

double nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1.0e9;
}

// The mean of every finger currently down. Using the centroid rather than one
// nominated pointer means a two-finger drag does not jump when the fingers are
// not perfectly parallel, and it degrades gracefully if a third finger lands.
void centroid(const AInputEvent* event, float& x, float& y) {
    const std::size_t count = AMotionEvent_getPointerCount(event);
    if (count == 0) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    float sx = 0.0f;
    float sy = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        sx += AMotionEvent_getX(event, i);
        sy += AMotionEvent_getY(event, i);
    }
    x = sx / static_cast<float>(count);
    y = sy / static_cast<float>(count);
}

}  // namespace

void TouchInput::configure(float densityScale) {
    densityScale_ = densityScale;
    // 8 dp, which is Android's own touch slop rounded down. A finger resting
    // on glass wanders by a pixel or two; anything inside this is "still".
    slopPx_ = 8.0f * densityScale;
    // One notch of ImGui's wheel scrolls a window by 5 * font size (imgui.cpp,
    // ImGui::UpdateMouseWheel), so a finger that has travelled that far should
    // have produced exactly one notch. That is what makes the content track
    // the finger instead of flying off at some arbitrary gain.
    scrollStepPx_ = 5.0f * cascade::gui::fonts::kUiSize * densityScale;
}

void TouchInput::cancelLongPress() { pressTracking_ = false; }

void TouchInput::releaseRightButton() {
    if (!rightButtonHeld_) { return; }
    ImGui::GetIO().AddMouseButtonEvent(1, false);
    rightButtonHeld_ = false;
}

void TouchInput::newFrame() {
    if (!pressTracking_ || rightButtonHeld_) { return; }
    if (nowSeconds() - pressStart_ < kLongPressSeconds) { return; }

    // The finger has been still for long enough. Let go of the left button and
    // take hold of the right one.
    //
    // THE LEFT BUTTON WAS ALREADY PRESSED, and that is a deliberate trade. The
    // alternative - withhold the press until the long-press window has expired
    // - would put 450 ms of lag on every single tap in the application to buy
    // a cleaner long press, which is the wrong way round. The cost is that a
    // control the finger is resting on sees a 450 ms click before the context
    // menu opens, which is exactly what a long press does on the desktop when
    // a user holds the left button before deciding to use the right one.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseButtonEvent(0, false);
    io.AddMouseButtonEvent(1, true);
    rightButtonHeld_ = true;
    pressTracking_ = false;
}

int32_t TouchInput::handleEvent(AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        // Keys, and anything else, are the backend's business entirely.
        return ImGui_ImplAndroid_HandleInputEvent(event);
    }

    ImGuiIO& io = ImGui::GetIO();
    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const std::size_t pointers = AMotionEvent_getPointerCount(event);

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        // A fresh gesture: whatever the previous one left behind is over.
        suppressUntilAllUp_ = false;
        twoFingerActive_ = false;
        releaseRightButton();
        if (pointers == 1) {
            pressTracking_ = true;
            pressStart_ = nowSeconds();
            pressX_ = AMotionEvent_getX(event, 0);
            pressY_ = AMotionEvent_getY(event, 0);
        }
        break;

    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (pointers >= 2) {
            // A second finger turns this into a scroll. Everything the first
            // finger started has to be undone: the left button it pressed is
            // released here rather than left held through the drag, which
            // would otherwise drag whatever it landed on while the view
            // scrolls under it.
            cancelLongPress();
            releaseRightButton();
            io.AddMouseButtonEvent(0, false);
            twoFingerActive_ = true;
            suppressUntilAllUp_ = true;
            centroid(event, lastCentroidX_, lastCentroidY_);
            return 1;
        }
        break;

    case AMOTION_EVENT_ACTION_MOVE:
        if (twoFingerActive_ && pointers >= 2) {
            float cx = 0.0f;
            float cy = 0.0f;
            centroid(event, cx, cy);
            const float dx = cx - lastCentroidX_;
            const float dy = cy - lastCentroidY_;
            lastCentroidX_ = cx;
            lastCentroidY_ = cy;
            // SIGN: direct manipulation - the content follows the fingers.
            // ImGui applies a wheel notch as `Scroll -= wheel * step`
            // (imgui.cpp, UpdateMouseWheel), so a positive wheel moves the
            // scroll position towards the start of the content, which is what
            // dragging downwards on a page does. Both axes therefore take the
            // finger delta with its own sign, unnegated.
            io.AddMouseWheelEvent(dx / scrollStepPx_, dy / scrollStepPx_);
            return 1;
        }
        if (pressTracking_) {
            const float dx = AMotionEvent_getX(event, 0) - pressX_;
            const float dy = AMotionEvent_getY(event, 0) - pressY_;
            if (std::sqrt(dx * dx + dy * dy) > slopPx_) {
                // It is a drag, not a press. The backend is already feeding
                // ImGui the movement with the left button held, which is
                // exactly right for dragging the spectrum or a window.
                cancelLongPress();
            }
        }
        break;

    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (twoFingerActive_) {
            // Down to one finger. The gesture is over as far as scrolling
            // goes, and suppressUntilAllUp_ keeps the survivor from being
            // read as a new press.
            twoFingerActive_ = false;
            return 1;
        }
        break;

    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL: {
        const bool suppressed = suppressUntilAllUp_;
        cancelLongPress();
        twoFingerActive_ = false;
        suppressUntilAllUp_ = false;
        if (rightButtonHeld_) {
            // The finger that became a right-click is leaving. Release button
            // 1 ourselves and let the backend release button 0 as usual - it
            // is already up, and a redundant release is harmless where a
            // missing one would leave the menu stuck open.
            releaseRightButton();
            break;
        }
        if (suppressed) {
            // The last finger of a multi-finger gesture. Make sure no button
            // is left held and swallow the event.
            io.AddMouseButtonEvent(0, false);
            return 1;
        }
        break;
    }

    default:
        break;
    }

    if (suppressUntilAllUp_) {
        // Still inside a gesture the backend must not see.
        return 1;
    }
    return ImGui_ImplAndroid_HandleInputEvent(event);
}

}  // namespace cascade::platform::android
