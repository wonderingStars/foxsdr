// gui_desktop_guard.hpp - THE RULE "NO TEST WINDOW ON THE VISIBLE DESKTOP", as a pure decision (engine fixes 4, item 3).
//
// A `gui`-labelled test opens a window on whatever desktop its process runs on (the drive runner starts foxsdr-slint with _popen; nothing creates or switches a desktop), so
// the only protection is where the process is started: the hidden desktop made by ~/.claude/scripts/run_on_hidden_desktop.ps1 (default name "FoxSDRTests", merge lanes use
// "FoxSDRTestsEM"). tests/gui_desktop_guard.cpp is a program that reads the name of the desktop it runs on; every gui test requires it as a CTest fixture
// (FIXTURES_REQUIRED gui_desktop), so when it refuses, no gui test starts and ctest counts them as failed.
//
// The decision: the interactive desktop is called "Default" by Windows. A process on it is REFUSED, and so is one whose desktop name cannot be read. Any other desktop is a
// desktop somebody made for this purpose. The override variable FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1 allows a run on the visible desktop on purpose (a person watching).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cctype>
#include <string>

namespace cascade::testguard {

struct Verdict {
    bool allowed = false;
    std::string message;   // one line
};

inline bool isVisibleDesktopName(const std::string& name) {
    static const char kVisible[] = "default";
    if (name.size() != sizeof(kVisible) - 1) { return false; }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != kVisible[i]) { return false; }
    }
    return true;
}

// OFF WINDOWS there are no named desktops, so the name is derived from the display the process would open its windows on. Only one display is PROVEN virtual: the one `xvfb-run` makes
// (it runs Xvfb for the command and points XAUTHORITY at <tmp>/xvfb-run.XXXXXX/Xauthority, a path nothing else creates). Everything else is a display somebody may be sitting at:
//   * WAYLAND_DISPLAY set: a Wayland-capable client may reach the person's compositor whatever DISPLAY says, so the name is "Default" (refused);
//   * DISPLAY set but not an xvfb-run one (a real X session, or an Xvfb nobody here started): "Default" (refused; name the virtual desktop with FOXSDR_GUARD_DESKTOP_NAME instead);
//   * neither set: "" (could not be read: refused).
// Arguments are the three environment values, nullptr when unset.
inline std::string linuxDesktopName(const char* display, const char* waylandDisplay, const char* xauthority) {
    if (waylandDisplay != nullptr && *waylandDisplay != '\0') { return "Default"; }
    if (display == nullptr || *display == '\0') { return std::string(); }
    const std::string xa = xauthority != nullptr ? std::string(xauthority) : std::string();
    const std::size_t at = xa.rfind("/xvfb-run.");
    const bool xvfbRun = at != std::string::npos && xa.size() > at + 21 && xa.compare(xa.size() - 11, 11, "/Xauthority") == 0 && xa.find('/', at + 1) == xa.size() - 11;
    return xvfbRun ? "xvfb-run" + std::string(display) : std::string("Default");
}

// `desktopName`: the name of the desktop this process runs on, "" when it could not be read. `overrideValue`: the value of FOXSDR_ALLOW_VISIBLE_GUI_TESTS, nullptr when unset.
inline Verdict judge(const std::string& desktopName, const char* overrideValue) {
    Verdict v;
    const bool override = overrideValue != nullptr && std::string(overrideValue) == "1";
    if (override) {
        v.allowed = true;
        v.message = "gui tests allowed on this desktop by FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1";
        return v;
    }
    if (desktopName.empty()) {
        v.message = "REFUSED: the desktop this process runs on could not be read, so gui-labelled tests do not start; run them through run_on_hidden_desktop.ps1 "
                    "(or set FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1 to run them on a desktop you are watching)";
        return v;
    }
    if (isVisibleDesktopName(desktopName)) {
        v.message = "REFUSED: this is the visible desktop ('" + desktopName + "'); gui-labelled tests open windows and must run on the hidden test desktop through "
                    "run_on_hidden_desktop.ps1 (or set FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1 to run them on a desktop you are watching)";
        return v;
    }
    v.allowed = true;
    v.message = "gui tests allowed: this process runs on the desktop '" + desktopName + "', not the visible one";
    return v;
}

}  // namespace cascade::testguard
