// android_window_logic.hpp - the parts of gui::AndroidPlatformWindow that are
// arithmetic or bookkeeping rather than EGL, so they can be checked on a
// machine with no phone attached.
//
// WHY A SEPARATE HEADER. gui/platform_window_android.cpp cannot be compiled on
// the host at all: it includes <EGL/egl.h>, <android/native_window.h>,
// <android_native_app_glue.h> and the ImGui Android backend, none of which
// exist outside an NDK sysroot. The GLFW implementation has the opposite
// problem and solved it the same way - tests/test_platform_window.cpp exercises
// what it can and the pure decisions live in headers (gui/viewport_policy.hpp,
// gui/page_geometry.hpp) - so the two decisions in the Android implementation
// that a wrong answer would be VISIBLE in go here: the screen's density, and
// the clipboard.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_ANDROID_WINDOW_LOGIC_HPP
#define CASCADE_GUI_ANDROID_WINDOW_LOGIC_HPP

#include <string>

namespace cascade::gui {

// The Android baseline, "mdpi": the density at which one logical pixel is one
// physical pixel and AConfiguration_getDensity's DEFAULT/ANY/NONE answers are
// worth nothing better than.
inline constexpr int kAndroidBaselineDpi = 160;

// dpi -> the factor a logical pixel is drawn at.
//
// Clamped to [1, 4]: below 1 would shrink an already small interface, and
// above 4 is past any shipping screen and means the configuration is being
// misread. A non-positive dpi - which is what AConfiguration_getDensity
// answers before the activity is fully attached, and on some emulators - reads
// as the baseline rather than as zero.
inline float androidDensityScale(int dpi) {
    if (dpi <= 0) { dpi = kAndroidBaselineDpi; }
    const float scale = static_cast<float>(dpi) / static_cast<float>(kAndroidBaselineDpi);
    if (scale < 1.0f) { return 1.0f; }
    if (scale > 4.0f) { return 4.0f; }
    return scale;
}

// THE CLIPBOARD THIS PORT HAS, AND IT IS NOT THE SYSTEM'S ONE.
//
// android.content.ClipboardManager is reachable only through JNI, and on
// Android 10 and later setPrimaryClip from a background thread is refused
// outright while getPrimaryClip returns nothing unless the app holds focus -
// the NativeActivity's native thread is not the UI thread, so both calls would
// have to be posted across, with a JavaVM attach, a looper handler on the Java
// side and a failure mode (a silent empty string) indistinguishable from an
// empty clipboard. That is a slice of its own.
//
// What is here instead is PROCESS-LOCAL, and it is not a placebo: every
// clipboard operation the interface actually performs - copy a frequency out
// of the tuning deck, paste a catalogue URL into the plugin field, cut a text
// field - is within this one application, and all of those work. What does not
// work is carrying text to or from another app, and PlatformWindow's contract
// is honest about the shape of that: clipboardText() returns what
// setClipboardText() last put there, and empty before anything did, exactly as
// a system clipboard holding nothing would.
class LocalClipboard {
public:
    // Null is "clear it", not "ignore this call": a caller that has just
    // failed to build a string must not leave the previous contents behind
    // for a paste to find.
    void set(const char* text) { text_ = (text != nullptr) ? std::string(text) : std::string(); }
    const std::string& get() const { return text_; }

private:
    std::string text_;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_ANDROID_WINDOW_LOGIC_HPP
