// display_power.hpp - whether the display this session is shown on is switched
// off, and the frames the application does not draw while it is.
//
// WHAT IT IS FOR. A tester's 0.99.95 and 0.99.96 on a laptop with Intel UHD
// graphics (2026-10-27): FoxSDR left running on ADS-B with nobody touching it
// "froze" - nothing in the window responded, a minimise and restore left it
// black, and a map page vanished - while the log showed the frame loop turning
// the whole time and a close from the taskbar shutting it down normally. Ten
// minutes after the start (the screen's own time-out; he had not touched it)
// a 500 ms frame began to appear every minute or two, and 29 to 91 minutes in
// the driver refused a second window ("WGL: Failed to create OpenGL context").
// His System log held no display-driver reset. The application had gone on
// drawing and presenting sixty frames a second to a display that was off.
//
// Windows says when the display goes off. GUID_SESSION_DISPLAY_STATUS (WinNT.h,
// "Power Setting GUIDs") is the one Microsoft names for user-mode applications
// in an interactive session - the display of THIS session, so a Remote Desktop
// viewer is not paused because the laptop's own screen is off, which
// GUID_CONSOLE_DISPLAY_STATE would do. Its Data is a DWORD MONITOR_DISPLAY_STATE:
// 0 PowerMonitorOff, 1 PowerMonitorOn, 2 PowerMonitorDim. The window procedure
// (gui/win_frame.cpp) receives it; this file decides what it means.
//
// ONLY "OFF" PAUSES DRAWING. Dimmed is a display somebody may still be reading,
// and a value Windows has not yet sent, or one this build does not know, is
// taken as on: failing to pause costs what it always cost, and pausing wrongly
// would hide the radio from someone looking at it.
//
// Pure, so tests/test_display_power.cpp can drive every transition without a
// window or a monitor to switch off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DISPLAY_POWER_HPP
#define CASCADE_GUI_DISPLAY_POWER_HPP

#include <cstdint>

namespace cascade::gui {

enum class DisplayPower { Unknown, Off, On, Dim };

// The MONITOR_DISPLAY_STATE value in a GUID_SESSION_DISPLAY_STATUS notification.
inline DisplayPower displayPowerFromData(std::uint32_t value) {
    switch (value) {
    case 0: return DisplayPower::Off;
    case 1: return DisplayPower::On;
    case 2: return DisplayPower::Dim;
    default: return DisplayPower::Unknown;
    }
}

inline const char* displayPowerName(DisplayPower p) {
    switch (p) {
    case DisplayPower::Off: return "off";
    case DisplayPower::On: return "on";
    case DisplayPower::Dim: return "dimmed";
    default: return "unknown";
    }
}

// THE PAUSE AND ITS TWO EDGES, read once a frame. update() returns Paused on
// the frame the display is first seen off, Resumed on the frame it is first
// seen anything else again, and None otherwise; while paused() the frame loop
// draws nothing and calls noteSkipped() for each frame it did not draw, so the
// line that says drawing resumed can say how long and how many.
class DisplayPause {
public:
    enum class Edge { None, Paused, Resumed };

    Edge update(DisplayPower p, double now) {
        const bool off = p == DisplayPower::Off;
        if (off == paused_) { return Edge::None; }
        paused_ = off;
        if (off) {
            since_ = now;
            skipped_ = 0;
            return Edge::Paused;
        }
        lastOffSeconds_ = now - since_;
        lastSkipped_ = skipped_;
        return Edge::Resumed;
    }

    bool paused() const { return paused_; }
    void noteSkipped() {
        if (paused_) { ++skipped_; }
    }
    // Frames not drawn in the pause in force now, or in the last one.
    long skipped() const { return paused_ ? skipped_ : lastSkipped_; }
    // How long the last pause lasted, read after Resumed.
    double lastOffSeconds() const { return lastOffSeconds_; }

private:
    bool paused_ = false;
    double since_ = 0.0;
    long skipped_ = 0;
    double lastOffSeconds_ = 0.0;
    long lastSkipped_ = 0;
};

// How long a frame waits while drawing is paused: the frame loop paces itself
// off the display's vertical sync, which a display that is off no longer gives,
// so without this the loop would spin. 33 ms keeps the interface's own work
// (plugin polls, the session timers) turning at about 30 a second and answers
// the "on" message within one wait.
inline constexpr int kDisplayOffFrameMs = 33;

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DISPLAY_POWER_HPP
