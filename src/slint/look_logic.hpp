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

enum class SkinId { Workbench, NightWatch, HighContrast, GlassCockpit, DaylightLab, FieldRadio };   // HighContrast: slint-gaps.md item 11, chosen by its own option, not by the saved theme key

// The skin a saved theme key ("today", "classic-xl", "night", "glass", "daylight", "field"; anything else is Today) is drawn in.
SkinId skinOfTheme(const std::string& uiTheme);
// Ctrl+= and Ctrl+- (the classic's handleInterfaceScaleKeys): from `nowPercent` the next offered step above (dir > 0) or below (dir < 0) through 100 to 250 %; 0 when there is none (already at the end).
int stepUiSizePercent(int nowPercent, int dir);
const char* skinName(SkinId id);   // "workbench", "night"
// THE COUNTER SCALE THE STRIP CAN HOLD (window-gaps-2, the Bench Classic XL squeeze). The doubled figures take 379 px more than the normal ones, and what the receiver strip has
// over is the three lanes' (gain, squelch, volume): at 1600 px they hold 600 at normal size and the doubled counter left them 221, so the gain and squelch headers ran into each
// other and the antenna key was pushed out of the window. The scale drawn is the one asked for, less whatever leaves the lanes under what their headers need (525 px, and 150 more
// per whole step of the readings scale). Measured on the real strip (English; one radio): the lanes hold width - 1000 px below 1760 px and width - 1307 px from it (the eight mode keys).
// `requested` is 1 or 2, `readings` is 1.0 .. 3.0, `windowWidth` the window's logical width; the answer is 1.0 .. requested.
float fitCounterScale(float requested, float readings, float windowWidth);


enum class CounterFace { Plain, Drum, Nixie };
// The counter face a saved tuner style ("nixie", "neon", "plain"; anything else is Nixie, as the classic reads it) is drawn in.
CounterFace faceOfStyle(const std::string& style);
const char* faceStyleName(CounterFace f);   // the style text the kit and the tests use: "plain", "neon", "nixie"

// The scale a saved interface size asks for when the operating system reports `osScale`: "auto" is osScale, "100".."250" (gui/ui_scale.hpp kSteps) is
// that percent / 100, anything else (a hand-edited file) degrades to auto. Bounded to 0.5 .. 4.
float scaleWanted(const std::string& choice, float osScale);

// The size the window must take to keep its LOGICAL size when the scale moves from `from` to `to`: physical * to / from, never below 1.
std::pair<std::uint32_t, std::uint32_t> sizeForScale(std::uint32_t width, std::uint32_t height, float from, float to);

// THE WINDOW'S SIZE AT A SCALE (drive fixes 1, rows L135, L145, L153). The physical size the window is asked for when the interface size moves: `w` x `h` is what the follower asked for
// (the logical size kept), `scale` the scale it is drawn at. It is kept at least `minW` x `minH` LOGICAL pixels (what the user sees: the layout's smallest window, host_logic.hpp
// kMinWindowW / kMinWindowH, at the scale in force) and never larger than the screen's work area (`screenW` x `screenH`, 0 = not known): where the two cannot both hold (250 % on
// a 1920 x 1080 screen) the screen wins, because a window larger than the screen cannot be used at all.
struct WindowFit { std::uint32_t w = 0, h = 0; };
WindowFit fitWindowToScreen(std::uint32_t w, std::uint32_t h, float scale, float minW, float minH, int screenW, int screenH);

// WHERE THE WINDOW SITS AFTER IT GROWS (drive fixes 6, item 2). The fit above bounds the SIZE by the work area; a window that is the right size but still has its top-left where it was
// (a 1600 x 900 window centred on a 5120 x 1440 screen has its top at 270) ends past the bottom edge once it is 1350 high: the drive found its bottom 74 pixels below a 1440 pixel screen at
// 150 %. The place is the window's own x, y moved the least that puts the whole window inside the work area `r` (a window larger than the area sits at its top-left corner).
struct WorkRect { int left = 0, top = 0, right = 0, bottom = 0; };   // physical pixels; right and bottom are exclusive
struct WindowPlace { int x = 0, y = 0; };
WindowPlace placeInWorkArea(int x, int y, std::uint32_t w, std::uint32_t h, const WorkRect& r);

// THE MIRROR OF THE REAL WINDOW SIZE INTO THE SCREEN'S SIZE PROPERTIES (ui/main.slint win-w / win-h, bound to the window's width and height). When the scale this window is drawn at is not
// the one the operating system gave it (an interface size of 125 % to 250 %: the follower above dispatches it), a CHANGED win-w is carried to the system as a LOGICAL size and converted
// with the system's scale, not ours: the window is resized to win-w x os instead of win-w x scale, one step later, and the next tick's mirror writes the smaller number again - the
// window shrank by scale / os every tick until it was 6 x 6 pixels (found by the drive, 2026-11-17: Ctrl+= made a 1921 x 1081 window 150 x 90). So with a scale that is not the
// system's, the mirror (1) writes nothing while a size the host just asked for has not arrived, (2) when it does write a CHANGED number, remembers the size the window had and puts that
// size back on the next tick if the write moved it, and (3) writes nothing else. With the system's own scale it writes every tick, as it always did.
class SizeMirror {
public:
    struct Step {
        bool write = false;              // set win-w / win-h to winW / winH
        float winW = 0.0f, winH = 0.0f;
        bool repair = false;             // ask the window for repairW x repairH (physical) again
        std::uint32_t repairW = 0, repairH = 0;
    };
    // The host has just asked the window for w x h physical pixels (a grow for a scale, a drag of an edge): the mirror waits for it to arrive.
    void asked(std::uint32_t w, std::uint32_t h);
    // Once a tick: the window's physical size now, the scale it is drawn at (Window::scale_factor) and the system's scale.
    Step tick(std::uint32_t w, std::uint32_t h, float scale, float os);

private:
    bool want_ = false;
    std::uint32_t wantW_ = 0, wantH_ = 0;
    int waited_ = 0;
    bool guard_ = false;
    std::uint32_t guardW_ = 0, guardH_ = 0;
    int guardLeft_ = 0;
    bool wrote_ = false;
    float lastW_ = 0.0f, lastH_ = 0.0f;
};

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
    // The size to ask the window for when the scale goes from `from` to `to` and the window is not maximised (`w` x `h`: its size now, in physical pixels). The logical size stays,
    // so a window grows with the scale: 1600 x 900 at 100 % asks for 3200 x 1800 at 200 %. A window that fills the screen is held at the screen's size by the system, so the grow is
    // CLAMPED, and asking for "the logical size stays" on the way back (800 x 450 at 100 %) would shrink it to half its first size for good (slint_drive_system-look read width 800
    // after Interface size 200 % then Auto, 4 runs of 5). So the size before the first grow is remembered; a window still no bigger than the grow asked for, going back down, is
    // asked for that remembered size scaled to the new number. Nothing is remembered once the window is back at the scale it left.
    std::pair<std::uint32_t, std::uint32_t> sizeAfter(std::uint32_t w, std::uint32_t h, float from, float to);

private:
    float os_ = 0.0f;        // the OS's scale; 0 until the first plan
    float applied_ = 0.0f;   // the last number dispatched; 0 = none yet
    bool grown_ = false;     // a grow is remembered
    float growFrom_ = 0.0f;  // the scale the window had before the first grow
    std::uint32_t growW_ = 0, growH_ = 0;     // its physical size then
    std::uint32_t askedW_ = 0, askedH_ = 0;   // the size the last grow asked for
};

}  // namespace foxsdr::slintlook
