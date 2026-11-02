// torn_off_ledger.hpp - which pages are their own operating system windows right
// now, how many pop-ups have been given windows over them, and the log lines
// that say when that changes (0.99.103).
//
// WHY. The 2026-10-29 tester log (Intel UHD 630, a laptop docked to a second
// monitor) had 81 minutes of half-second frames in `render` - the scope that
// draws and swaps the pages' own windows as well as the main one - and two
// refusals of a second GL context (GLFW 65543, Win32 0xc00705aa: out of
// resources). The log could not say whether a page had a window of its own at
// the time, on which monitor it sat, or how many windows the session had already
// made before the driver said no: a window that WORKS was not logged anywhere,
// only the refusal and the page brought back inside. The frame loop hands this
// ledger the windows ImGui has after UpdatePlatformWindows, every frame; a page's
// window it has not seen is logged with its page, size, place, monitor and the
// session's count, and one that is gone as back inside. The id in the line is the
// viewport's, which the slow-frame line uses too (`largest part: window-swap 440
// ms (torn-off window 0x8a3f2b1c of 2)`, core/frame_timing.hpp), so the two read
// together:
//
//     viewports: page "ADS-B Map" in its own window 0x8a3f2b1c - 1026x699 at 588,202
//         on monitor 2 of 2 (DELL U2720Q, 100%); window 3 this session, 1 outside
//     viewports: page "ADS-B Map" back in the main window (window 0x8a3f2b1c gone);
//         0 outside
//
// A PAGE NEED NOT BE TORN OFF TO HAVE A WINDOW. The map, store and fitted-modules
// pages are ImGuiViewportFlags_NoAutoMerge (gui/app_window.cpp): each is its own
// operating system window and shared GL context from the frame it opens, wherever
// it sits. So the line says "in its own window", not "torn off".
//
// POP-UPS ARE WINDOWS TOO, AND THEY BLINK. A tooltip or menu that protrudes past a
// page's own window is given a window and shared GL context of ITS OWN by ImGui
// (imgui.cpp, Begin: "Late create viewport if we don't fit"), inside
// UpdatePlatformWindows - and it is destroyed one frame after the pop-up goes.
// Over a map page, an aircraft tooltip can do that every time a target drifts
// under the pointer. That is the one mechanism of this codebase a read-only code
// hunt (2026-10-29) found that makes a GL context per event in the `render` scope
// while a page has its own window and never while every page is inside; whether
// it is what the tester's laptop does is what these lines will say. Such windows
// are COUNTED, not listed: one page line per blink would push the whole log out
// of its ring in exactly the sessions this is for. They get one summary line at
// most every kPopupGapS, with the count since the last line and the session's.
//
// PURE. No ImGui, no GLFW, no log: the caller fills a TornOffWindow per window
// and gets the lines back. Fixed-size on the normal path - comparing a handful of
// ids - and allocating only on the frame something changed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cascade::gui {

// One window ImGui has beside the main one, as the frame loop sees it this frame.
struct TornOffWindow {
    // ImGui's viewport id: the page's own, the same every time the page is outside.
    std::uint32_t id = 0;
    // The page title's visible half (before any "###"), `nameLen` characters.
    const char* name = nullptr;
    int nameLen = 0;
    // Where the window is, in screen points.
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    // The monitor ImGui placed it on: 0-based in the platform's list, -1 when it
    // could not place it (a monitor being reconfigured).
    int monitor = -1;
    int monitors = 0;
    // The display's model name as the window library reports it; null when not
    // known this frame.
    const char* monitorName = nullptr;
    // That monitor's scale: 1.25 at 120 dpi.
    float dpiScale = 1.0f;
    // A pop-up - tooltip, menu, popup - rather than a page: counted, not listed.
    bool popup = false;
};

class TornOffLedger {
public:
    // More windows than this are not followed: a real session has one or two
    // pages out, and a fixed table is what keeps the per-frame path free of
    // allocation. Pop-ups have a table of their own, the same size.
    static constexpr int kMax = 16;
    // THE LINE MUST FIT THE LOG'S LINE (core/diag_log.hpp, 192 bytes with the NUL),
    // or its tail - the counts - is what gets cut. The longest line is a page's
    // with everything at its widest (a 4-digit size and place, a 3-digit scale, a
    // 4-digit session count); with a page name cut to kNameChars - 1 and a monitor
    // name cut to kMonitorChars it is 187 characters, held by
    // tests/test_torn_off_ledger.cpp.
    static constexpr int kNameChars = 28;
    static constexpr int kMonitorChars = 20;
    // One pop-up summary line per this long, at most.
    static constexpr double kPopupGapS = 30.0;

    // The windows this frame; `now` is a clock in seconds (the window's). Appends
    // one line per change to `lines` - the pages gone first, then the new pages,
    // then at most one pop-up summary - and returns how many it added.
    int update(const TornOffWindow* windows, int count, double now, std::vector<std::string>& lines) {
        int added = 0;
        // Pages gone: in the ledger, not in the frame.
        for (int i = 0; i < n_;) {
            if (!among(entries_[i].id, windows, count, false)) {
                char line[256];
                std::snprintf(line, sizeof line,
                              "viewports: page \"%s\" back in the main window (window 0x%08x gone); %d "
                              "outside",
                              entries_[i].name, static_cast<unsigned>(entries_[i].id), n_ - 1);
                lines.emplace_back(line);
                ++added;
                entries_[i] = entries_[n_ - 1];
                --n_;
            } else {
                ++i;
            }
        }
        // Pages new: in the frame, not in the ledger.
        for (int i = 0; i < count; ++i) {
            const TornOffWindow& w = windows[i];
            if (w.popup || w.id == 0 || find(w.id) >= 0) { continue; }
            if (n_ >= kMax) { break; }
            Entry& e = entries_[n_++];
            e.id = w.id;
            const int len = w.nameLen < 0 ? 0 : (w.nameLen < kNameChars - 1 ? w.nameLen : kNameChars - 1);
            std::memcpy(e.name, w.name != nullptr ? w.name : "", static_cast<std::size_t>(len));
            e.name[len] = '\0';
            ++ownWindows_;
            char where[96];
            if (w.monitor >= 0 && w.monitor < w.monitors) {
                std::snprintf(where, sizeof where, "on monitor %d of %d (%.*s, %.0f%%)", w.monitor + 1,
                              w.monitors, kMonitorChars,
                              w.monitorName != nullptr ? w.monitorName : "unnamed", w.dpiScale * 100.0f);
            } else {
                std::snprintf(where, sizeof where, "on no monitor the interface could place (%d listed)",
                              w.monitors);
            }
            char line[256];
            std::snprintf(line, sizeof line,
                          "viewports: page \"%s\" in its own window 0x%08x - %.0fx%.0f at %.0f,%.0f %s; "
                          "window %d this session, %d outside",
                          e.name, static_cast<unsigned>(w.id), w.w, w.h, w.x, w.y, where, ownWindows_,
                          n_);
            lines.emplace_back(line);
            ++added;
        }
        // Pop-ups: the ones gone are forgotten, the new ones counted.
        for (int i = 0; i < popupN_;) {
            if (!among(popups_[i], windows, count, true)) {
                popups_[i] = popups_[popupN_ - 1];
                --popupN_;
            } else {
                ++i;
            }
        }
        for (int i = 0; i < count; ++i) {
            const TornOffWindow& w = windows[i];
            if (!w.popup || w.id == 0 || findPopup(w.id) >= 0) { continue; }
            if (popupN_ < kMax) { popups_[popupN_++] = w.id; }
            ++popupsMade_;
            ++popupsSinceLine_;
        }
        // ...and said at most once per kPopupGapS, the first one at once.
        if (popupsSinceLine_ > 0 && (popupLineAt_ < 0.0 || now - popupLineAt_ >= kPopupGapS)) {
            char line[256];
            std::snprintf(line, sizeof line,
                          "viewports: %d pop-up window%s (a tooltip or menu past a page's own window, each "
                          "a window and GL context of its own while it shows) since the last line; %d "
                          "this session",
                          popupsSinceLine_, popupsSinceLine_ == 1 ? "" : "s", popupsMade_);
            lines.emplace_back(line);
            ++added;
            popupsSinceLine_ = 0;
            popupLineAt_ = now;
        }
        return added;
    }

    // How many pages are their own windows right now (of the ones followed).
    int outside() const { return n_; }
    // How many times a page was given its own window this session.
    int ownWindows() const { return ownWindows_; }
    // How many pop-up windows were made over such pages this session.
    int popupsMade() const { return popupsMade_; }

private:
    struct Entry {
        std::uint32_t id = 0;
        char name[kNameChars] = {};
    };

    int find(std::uint32_t id) const {
        for (int i = 0; i < n_; ++i) {
            if (entries_[i].id == id) { return i; }
        }
        return -1;
    }
    int findPopup(std::uint32_t id) const {
        for (int i = 0; i < popupN_; ++i) {
            if (popups_[i] == id) { return i; }
        }
        return -1;
    }
    static bool among(std::uint32_t id, const TornOffWindow* windows, int count, bool popup) {
        for (int i = 0; i < count; ++i) {
            if (windows[i].id == id && windows[i].popup == popup) { return true; }
        }
        return false;
    }

    Entry entries_[kMax];
    int n_ = 0;
    int ownWindows_ = 0;

    std::uint32_t popups_[kMax] = {};
    int popupN_ = 0;
    int popupsMade_ = 0;
    int popupsSinceLine_ = 0;
    double popupLineAt_ = -1.0;
};

}  // namespace cascade::gui
