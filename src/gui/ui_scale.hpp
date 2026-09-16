// ui_scale.hpp - HOW BIG THE DESKTOP LAYOUT IS DRAWN ON A SCREEN THAT IS NOT
// A DESKTOP.
//
// WHY THIS IS A DECISION AND NOT A CONSTANT. Every dimension in this interface
// is a raw pixel count measured against a desktop window: the function rail's
// column is kMenuWidth = 384 px, the tuning deck needs kDeckMinWindowW = 624 px
// of client width before its volume dial is clipped away, the faces are
// lettered at 21/19/20/17 px, and the window the desktop opens is 1280 x 720.
// Those figures came off a ~96 dpi monitor at arm's length.
//
// A phone is neither of those things twice over. It has far MORE pixels than
// the layout was drawn for (a Pixel 7 is 1080 x 2400 at 420 dpi) and far LESS
// room, because they are packed at four times the density: drawn 1:1, a rail
// key comes out about 4 mm across - under the 9 mm Android's own guidance puts
// on a touch target, so unusable rather than merely small. Drawn at the full
// density (x2.62 here) the layout is the right SIZE for a finger and no longer
// FITS: 1080 physical pixels of height is 412 logical pixels at that scale,
// against a layout that wants 720.
//
// So there are two numbers and the answer is between them, which is what this
// file computes: THE LARGEST SCALE AT WHICH THE WHOLE DESKTOP LAYOUT STILL
// FITS THE SCREEN, never magnified past the screen's own density (there is no
// point drawing a 9 mm key as a 15 mm one) and never shrunk below 1:1 (below
// that the lettering is smaller than it is on a monitor, on a screen held
// closer, which is the one outcome that helps nobody).
//
// PURE ARITHMETIC ON FIVE NUMBERS IN, ONE OUT. No ImGui, no platform, no
// getenv of its own - the override string is passed in - so the rule can be
// checked without a phone, which is what tests/test_ui_scale.cpp does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_UI_SCALE_HPP
#define CASCADE_GUI_UI_SCALE_HPP

#include <cstdlib>

namespace cascade::gui {

// THE SIZE THE LAYOUT WAS DRAWN FOR, which is the window src/main.cpp asks
// for: PlatformWindow::CreateInfo's 1280 x 720 defaults. It is deliberately
// NOT kMinWindowW x kMinWindowH (624 x 400) - that pair is the floor below
// which controls are CLIPPED AWAY, not the size at which the interface is
// whole. Fitting to the floor would report "it fits" for a screen on which
// every panel is cramped and scrolling, which is exactly the complaint this
// scale exists to answer.
inline constexpr float kLayoutRefW = 1280.0f;
inline constexpr float kLayoutRefH = 720.0f;

// Never smaller than the desktop's own pixels (see the header note), and
// never larger than any shipping screen's density - a scale above this says
// the configuration is being misread rather than that the screen is enormous.
inline constexpr float kUiScaleMin = 1.0f;
inline constexpr float kUiScaleMax = 4.0f;

// FOXSDR_UI_SCALE, as getenv returns it: null when unset. Out-of-range and
// unparseable values are REFUSED rather than clamped, and refused silently
// enough that a typo cannot make the interface unusable - the caller falls
// back to the fitted scale. Returns 0.0f for "no usable override", which is
// never a legal scale.
inline float uiScaleOverride(const char* env) {
    if (env == nullptr || env[0] == '\0') { return 0.0f; }
    char* end = nullptr;
    const double v = std::strtod(env, &end);
    if (end == env) { return 0.0f; }  // nothing numeric at all
    // NaN fails both comparisons, which is the reading wanted here.
    if (!(v >= static_cast<double>(kUiScaleMin)) ||
        !(v <= static_cast<double>(kUiScaleMax))) {
        return 0.0f;
    }
    return static_cast<float>(v);
}

// THE RULE.
//
//   fbW, fbH      the drawable, in physical pixels (framebufferSize()).
//   densityCap    the screen's own density scale (dpi / 160), which is the
//                 largest magnification worth applying. Values below 1 are
//                 treated as 1: a screen that reports less than the Android
//                 baseline is reporting nothing useful.
//   env           FOXSDR_UI_SCALE, or null.
//
// A valid override wins outright, so the owner can put the fitted scale and
// the density scale side by side on one device without a rebuild.
inline float fittedUiScale(int fbW, int fbH, float densityCap, const char* env) {
    if (const float forced = uiScaleOverride(env); forced > 0.0f) { return forced; }

    // A framebuffer of zero is a window that has not been measured yet; 1:1 is
    // the answer that cannot be wrong in a way that hides the interface.
    if (fbW <= 0 || fbH <= 0) { return kUiScaleMin; }

    const float byWidth = static_cast<float>(fbW) / kLayoutRefW;
    const float byHeight = static_cast<float>(fbH) / kLayoutRefH;
    // The smaller of the two: a layout that fits the width and overflows the
    // height does not fit.
    float scale = byWidth < byHeight ? byWidth : byHeight;

    float cap = densityCap;
    if (!(cap >= kUiScaleMin)) { cap = kUiScaleMin; }  // NaN-safe
    if (!(cap <= kUiScaleMax)) { cap = kUiScaleMax; }
    if (scale > cap) { scale = cap; }
    if (!(scale >= kUiScaleMin)) { scale = kUiScaleMin; }  // NaN-safe
    return scale;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_UI_SCALE_HPP
