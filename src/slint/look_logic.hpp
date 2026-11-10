// look_logic.hpp - THE WINDOW'S LOOK: which skin the saved theme names, which counter face the saved tuner style names, and what scale the saved
// interface size asks of the window (docs 6.61). No Slint here and no ImGui: main.cpp reads the interface half once a tick, asks these three rules
// and applies the answers (Theme.skin, the counter face, Window::dispatch_scale_factor_change_event), so a pick in SYSTEM > Display is seen on the
// next frame and the same rules run at start-up.
//
// THE SKINS. The classic window has six looks (gui/theme.cpp). This window ports ONE besides Workbench - Night Watch, the red-black look that keeps
// a night-adapted eye - end to end as the proof that a second filling carries the whole window (theme.slint: the NightWatch value, one struct). The
// other four still wear Workbench until they are ported; skinOfTheme says so in one place.
//
// THE SCALE. "auto" follows the operating system's own scale (Window::scale_factor as the OS last set it), a fixed choice (100..250 %, the steps of
// gui/ui_scale.hpp) pins the window to that number on every monitor. The window cannot tell the OS's number from the one this code dispatched, so
// ScaleFollower remembers the last number it dispatched: a window that reports anything else was moved by the OS (a drag to another monitor) and
// that report is the new OS scale.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace foxsdr::slintlook {

enum class SkinId { Workbench, NightWatch };

// The skin a saved theme key ("today", "classic-xl", "night", "glass", "daylight", "field"; anything else is Today) is drawn in.
SkinId skinOfTheme(const std::string& uiTheme);
const char* skinName(SkinId id);   // "workbench", "night"

enum class CounterFace { Plain, Drum, Nixie };
// The counter face a saved tuner style ("nixie", "neon", "plain"; anything else is Nixie, as the classic reads it) is drawn in.
CounterFace faceOfStyle(const std::string& style);
const char* faceStyleName(CounterFace f);   // the style text the kit and the tests use: "plain", "neon", "nixie"

// The scale a saved interface size asks for when the operating system reports `osScale`: "auto" is osScale, "100".."250" (gui/ui_scale.hpp kSteps) is
// that percent / 100, anything else (a hand-edited file) degrades to auto. Bounded to 0.5 .. 4.
float scaleWanted(const std::string& choice, float osScale);

// The size the window must take to keep its LOGICAL size when the scale moves from `from` to `to`: physical * to / from, never below 1.
std::pair<std::uint32_t, std::uint32_t> sizeForScale(std::uint32_t width, std::uint32_t height, float from, float to);

struct ScalePlan {
    float want = 1.0f;     // the scale the window should have
    bool change = false;   // true when it does not have it yet
    float os = 1.0f;       // the operating system's scale as far as this follower knows
};

class ScaleFollower {
public:
    // `choice`: the saved interface size; `current`: Window::scale_factor() now.
    ScalePlan plan(const std::string& choice, float current);
    // The number the caller just dispatched to the window.
    void applied(float scale) { applied_ = scale; }
    float os() const { return os_; }

private:
    float os_ = 0.0f;        // the OS's scale; 0 until the first plan
    float applied_ = 0.0f;   // the last number dispatched; 0 = none yet
};

}  // namespace foxsdr::slintlook
