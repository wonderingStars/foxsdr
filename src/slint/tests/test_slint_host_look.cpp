// The window's look (src/slint/look_logic.hpp, docs 6.61): which skin a saved theme is drawn in, which counter face a saved tuner style names, and what scale
// a saved interface size asks of the window. Pure: no Slint, no engine, no window. The reference for the scale is the ImGui window's gui/ui_scale.hpp
// (effectiveScale: "auto" follows the monitor, a fixed step pins it); for the theme, gui/theme.cpp (six keys, an unknown key is Today).
//
// What this holds equal: every one of the six classic theme keys names a skin and only "night" names Night Watch; "auto" follows the OS number and a step
// ignores it; a hand-edited choice degrades to auto; the follower treats a number it did not dispatch as the OS's new scale (a drag to another monitor)
// and a number it did dispatch as its own; resizing for a scale keeps the logical size.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "look_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

using namespace foxsdr::slintlook;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void skins() {
    CHECK(skinOfTheme("night") == SkinId::NightWatch);
    for (const char* key : {"today", "classic-xl", "", "NIGHT", "nigh", "bogus"}) { CHECK(skinOfTheme(key) == SkinId::Workbench); }
    CHECK(skinOfTheme("glass") == SkinId::GlassCockpit && skinOfTheme("daylight") == SkinId::DaylightLab && skinOfTheme("field") == SkinId::FieldRadio);
    CHECK(std::string(skinName(SkinId::GlassCockpit)) == "glass" && std::string(skinName(SkinId::DaylightLab)) == "daylight" && std::string(skinName(SkinId::FieldRadio)) == "field");
    CHECK(std::string(skinName(SkinId::NightWatch)) == "night");
    CHECK(std::string(skinName(SkinId::Workbench)) == "workbench");
}

void steps() {
    // Ctrl+= and Ctrl+- (window-gaps-2, row 145): the next offered step through 100 to 250 %, none past either end, and a number between steps lands on its neighbour
    CHECK(stepUiSizePercent(100, 1) == 125 && stepUiSizePercent(125, 1) == 150 && stepUiSizePercent(200, 1) == 250 && stepUiSizePercent(250, 1) == 0);
    CHECK(stepUiSizePercent(250, -1) == 200 && stepUiSizePercent(125, -1) == 100 && stepUiSizePercent(100, -1) == 0);
    CHECK(stepUiSizePercent(110, 1) == 125 && stepUiSizePercent(110, -1) == 100 && stepUiSizePercent(300, 1) == 0 && stepUiSizePercent(50, -1) == 0);
}

void counterFit() {
    // Bench Classic XL at 1600 px (window-gaps-2): the doubled figures left the lanes 221 px of 600 and pushed the antenna key out of the window; the scale drawn is what the strip holds.
    const float at1600 = fitCounterScale(2.0f, 1.4f, 1600.0f);
    CHECK(at1600 >= 1.0f && at1600 < 1.2f);                                   // the asked doubling cannot be drawn: the lanes keep their 525 + 60 px
    CHECK(fitCounterScale(1.0f, 1.0f, 1600.0f) == 1.0f && fitCounterScale(1.0f, 1.4f, 800.0f) == 1.0f);   // normal size is never touched, whatever the width
    CHECK(fitCounterScale(2.0f, 1.0f, 640.0f) == 1.0f && fitCounterScale(2.0f, 1.0f, 0.0f) == 1.0f);   // a window far too narrow: never below normal
    CHECK(fitCounterScale(2.0f, 1.0f, 1600.0f) > at1600);                       // the readings scale takes some of the room too
    CHECK(fitCounterScale(2.0f, 1.0f, 1759.0f) > fitCounterScale(2.0f, 1.0f, 1600.0f));   // a wider window holds larger figures
    CHECK(fitCounterScale(2.0f, 1.4f, 2400.0f) == 2.0f && fitCounterScale(2.0f, 1.0f, 2216.0f) == 2.0f);   // wide enough: the full doubling
    // the lanes left at the answer never fall under what they need: 600 - 379 * (s - 1) at 1600 px
    for (float w = 1000.0f; w <= 3000.0f; w += 7.0f) {
        const float s = fitCounterScale(2.0f, 1.4f, w);
        const float lanes = (w >= 1760.0f ? w - 1307.0f : w - 1000.0f) - 379.0f * (s - 1.0f);
        CHECK(s >= 1.0f && s <= 2.0f);
        CHECK(s == 1.0f || lanes >= 585.0f - 0.01f);
    }
}

void faces() {
    CHECK(faceOfStyle("nixie") == CounterFace::Nixie);
    CHECK(faceOfStyle("neon") == CounterFace::Drum);
    CHECK(faceOfStyle("plain") == CounterFace::Plain);
    CHECK(faceOfStyle("") == CounterFace::Nixie);   // an unknown style is Nixie, as the classic window reads it
    CHECK(faceOfStyle("vfd") == CounterFace::Nixie);
    CHECK(std::string(faceStyleName(CounterFace::Drum)) == "neon");
    CHECK(std::string(faceStyleName(CounterFace::Plain)) == "plain");
    CHECK(std::string(faceStyleName(CounterFace::Nixie)) == "nixie");
}

void wanted() {
    // auto follows the OS number exactly
    CHECK(near(scaleWanted("auto", 1.0f), 1.0f));
    CHECK(near(scaleWanted("auto", 1.5f), 1.5f));
    CHECK(near(scaleWanted("auto", 1.25f), 1.25f));
    // a step ignores it, on every monitor
    CHECK(near(scaleWanted("100", 1.5f), 1.0f));
    CHECK(near(scaleWanted("125", 1.0f), 1.25f));
    CHECK(near(scaleWanted("150", 2.0f), 1.5f));
    CHECK(near(scaleWanted("175", 1.0f), 1.75f));
    CHECK(near(scaleWanted("200", 1.0f), 2.0f));
    CHECK(near(scaleWanted("250", 1.0f), 2.5f));
    // a hand-edited choice (not a step) degrades to auto
    CHECK(near(scaleWanted("130", 1.5f), 1.5f));
    CHECK(near(scaleWanted("", 1.25f), 1.25f));
    CHECK(near(scaleWanted("big", 1.0f), 1.0f));
    CHECK(near(scaleWanted("-200", 1.0f), 1.0f));
    // a wild OS number is bounded
    CHECK(near(scaleWanted("auto", 0.0f), 0.5f));
    CHECK(near(scaleWanted("auto", 9.0f), 4.0f));
}

void resizing() {
    auto s = sizeForScale(1000, 700, 1.0f, 2.0f);
    CHECK(s.first == 2000 && s.second == 1400);
    s = sizeForScale(2000, 1400, 2.0f, 1.0f);
    CHECK(s.first == 1000 && s.second == 700);
    s = sizeForScale(1000, 700, 1.0f, 1.25f);
    CHECK(s.first == 1250 && s.second == 875);
    s = sizeForScale(1000, 700, 1.0f, 1.0f);
    CHECK(s.first == 1000 && s.second == 700);
    s = sizeForScale(1, 1, 4.0f, 0.5f);   // never below one pixel
    CHECK(s.first == 1 && s.second == 1);
    s = sizeForScale(800, 600, 0.0f, 2.0f);   // an unknown "from" changes nothing
    CHECK(s.first == 800 && s.second == 600);
}

// A window that fills the screen cannot grow: the system clamps the grow request, so the way back must not be "the logical size stays" from the clamped size
// (slint_drive_system-look: 200 % then Auto read width 800 instead of 1600). Engine merge 6.
void clampedGrowRoundTrip() {
    {   // the system honours the grow: the old behaviour, unchanged
        ScaleFollower f;
        auto s = f.sizeAfter(1000, 700, 1.0f, 2.0f);
        CHECK(s.first == 2000 && s.second == 1400);
        s = f.sizeAfter(2000, 1400, 2.0f, 1.0f);
        CHECK(s.first == 1000 && s.second == 700);
    }
    {   // the grow is clamped to the screen (1600 x 900): back at 100 % the window is asked for what it was, not for half of it
        ScaleFollower f;
        auto s = f.sizeAfter(1600, 900, 1.0f, 2.0f);
        CHECK(s.first == 3200 && s.second == 1800);
        s = f.sizeAfter(1600, 900, 2.0f, 1.0f);   // the window still reports the clamped 1600 x 900
        CHECK(s.first == 1600 && s.second == 900);
        // and nothing is remembered once it is back: the next grow starts from here
        s = f.sizeAfter(1600, 900, 1.0f, 1.5f);
        CHECK(s.first == 2400 && s.second == 1350);
        s = f.sizeAfter(1600, 900, 1.5f, 1.0f);
        CHECK(s.first == 1600 && s.second == 900);
    }
    {   // a chain of grows (100 -> 150 -> 200) and the way back in two steps
        ScaleFollower f;
        (void)f.sizeAfter(1600, 900, 1.0f, 1.5f);
        (void)f.sizeAfter(1600, 900, 1.5f, 2.0f);
        auto s = f.sizeAfter(1600, 900, 2.0f, 1.5f);
        CHECK(s.first == 2400 && s.second == 1350);   // 1600 x 900 at 100 % is 2400 x 1350 at 150 %: what the system will clamp again
        s = f.sizeAfter(1600, 900, 1.5f, 1.0f);
        CHECK(s.first == 1600 && s.second == 900);
    }
    {   // the person made the window bigger than the grow asked for: their size is theirs, the logical size stays as before
        ScaleFollower f;
        (void)f.sizeAfter(1000, 700, 1.0f, 2.0f);   // asked 2000 x 1400
        const auto s = f.sizeAfter(2400, 1500, 2.0f, 1.0f);
        CHECK(s.first == 1200 && s.second == 750);
    }
    {   // a shrink with nothing remembered is the plain rule
        ScaleFollower f;
        const auto s = f.sizeAfter(2000, 1400, 2.0f, 1.0f);
        CHECK(s.first == 1000 && s.second == 700);
    }
}

void follower() {
    ScaleFollower f;
    // first look on a 125% monitor, saved size auto: nothing to do
    ScalePlan p = f.plan("auto", 1.25f);
    CHECK(near(p.os, 1.25f) && near(p.want, 1.25f) && !p.change);
    // the user picks 200%: the window must move to 2.0, and the OS number is kept
    p = f.plan("200", 1.25f);
    CHECK(near(p.os, 1.25f) && near(p.want, 2.0f) && p.change);
    f.applied(p.want);
    // the window now reports what was dispatched: settled
    p = f.plan("200", 2.0f);
    CHECK(near(p.os, 1.25f) && near(p.want, 2.0f) && !p.change);
    // back to auto: the OS's own number is remembered, not the pinned 2.0
    p = f.plan("auto", 2.0f);
    CHECK(near(p.os, 1.25f) && near(p.want, 1.25f) && p.change);
    f.applied(p.want);
    // the window is dragged to a 150% monitor: the OS reports 1.5, which this follower did not dispatch
    p = f.plan("auto", 1.5f);
    CHECK(near(p.os, 1.5f) && near(p.want, 1.5f) && !p.change);
    f.applied(1.5f);
    // ...with a pinned size the drag changes the OS number but not the window
    p = f.plan("175", 1.5f);
    CHECK(near(p.os, 1.5f) && near(p.want, 1.75f) && p.change);
    f.applied(p.want);
    p = f.plan("175", 2.0f);   // dragged to a 200% monitor: the OS reset the window to 2.0
    CHECK(near(p.os, 2.0f) && near(p.want, 1.75f) && p.change);
    f.applied(p.want);
    CHECK(near(f.os(), 2.0f));
    p = f.plan("auto", 1.75f);   // pinned 1.75 applied, then back to auto: the monitor's 2.0 returns
    CHECK(near(p.os, 2.0f) && near(p.want, 2.0f) && p.change);
}

// THE WINDOW'S SIZE AT EVERY INTERFACE SIZE (drive fixes 1, rows L135 / L145 / L153): Ctrl+=, Ctrl+Shift+= and "125 %" made a 1921 x 1081 window 150 x 90, 6 x 6 or 505 x 285 pixels and Ctrl+0
// did not bring it back. The machine below is the window as the drive measured it: the host mirrors the window's size into win-w / win-h (win-w = physical / scale), and a CHANGED win-w is
// carried to the system as a logical size that the system converts with ITS scale, one step later (physical = win-w * os, whatever scale this window is drawn at); a size the host asks for
// (set_size) arrives one step later too. "naive" is the mirror as it was (write every tick); the other is SizeMirror.
struct Machine {
    std::uint32_t w, h;                 // the window's real physical size
    int x = 0, y = 0;                   // where its top-left is (drive fixes 6, item 2)
    float os;                           // the system's scale
    float scale;                        // the scale the window is drawn at
    float winW = 0.0f, winH = 0.0f;     // what the host last wrote
    bool pushPending = false;
    float pushW = 0.0f, pushH = 0.0f;
    bool setPending = false;
    std::uint32_t setW = 0, setH = 0;
    bool pushFirst = true;              // the order the two requests reach the system in: both are tried
    bool naive = false;
    SizeMirror mirror;
    void ask(std::uint32_t nw, std::uint32_t nh) { setPending = true; setW = nw; setH = nh; if (!naive) { mirror.asked(nw, nh); } }
    void host() {                       // one 33 ms tick of the host's mirror
        if (naive) {
            const float nw = static_cast<float>(w) / scale, nh = static_cast<float>(h) / scale;
            if (std::fabs(nw - winW) > 0.001f || std::fabs(nh - winH) > 0.001f) { pushPending = true; pushW = nw; pushH = nh; }
            winW = nw; winH = nh;
            return;
        }
        const SizeMirror::Step s = mirror.tick(w, h, scale, os);
        if (s.write) {
            if (std::fabs(s.winW - winW) > 0.001f || std::fabs(s.winH - winH) > 0.001f) { pushPending = true; pushW = s.winW; pushH = s.winH; }
            winW = s.winW; winH = s.winH;
        }
        if (s.repair) { setPending = true; setW = s.repairW; setH = s.repairH; }
    }
    void system() {                     // the event loop's pass between two ticks
        const auto doPush = [&] { if (pushPending) { w = static_cast<std::uint32_t>(std::max(1.0f, std::floor(pushW * os + 0.5f))); h = static_cast<std::uint32_t>(std::max(1.0f, std::floor(pushH * os + 0.5f))); pushPending = false; } };
        const auto doSet = [&] { if (setPending) { w = setW; h = setH; setPending = false; } };
        if (pushFirst) { doPush(); doSet(); } else { doSet(); doPush(); }
    }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) { host(); system(); } }
};

void windowSizeAtEverySize(int screenW, int screenH) {   // the work area of the screen: width and height in physical pixels, its top-left the screen's (drive fixes 6: run for four screens)
    const float kMinW = 1024.0f, kMinH = 640.0f;
    const WorkRect work{0, 0, screenW, screenH};
    const float steps[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f};
    for (const float os : {1.0f, 1.5f}) {
        for (const bool pushFirst : {true, false}) {
            for (const auto& start : {std::pair<std::uint32_t, std::uint32_t>{1300, 780}, {1921, 1081}, {1024, 640}, {1600, 900}}) {
                Machine m;
                m.os = os; m.scale = os; m.pushFirst = pushFirst;
                m.w = static_cast<std::uint32_t>(std::lround(start.first * os)); m.h = static_cast<std::uint32_t>(std::lround(start.second * os));
                m.x = std::max(0, (screenW - static_cast<int>(m.w)) / 2); m.y = std::max(0, (screenH - static_cast<int>(m.h)) / 2);   // the system centres a new window
                const std::uint32_t w0 = m.w, h0 = m.h;
                m.run(6);
                CHECK(m.w == w0 && m.h == h0);   // at the system's own scale nothing moves
                ScaleFollower f;
                (void)f.plan("auto", os);
                f.applied(os);
                const auto go = [&](float to) {   // the follower's grow: dispatch the scale, ask for the size
                    const float from = m.scale;
                    m.scale = to;
                    const auto ns = f.sizeAfter(m.w, m.h, from, to);
                    const WindowFit fit = fitWindowToScreen(ns.first, ns.second, to, kMinW, kMinH, screenW, screenH);
                    m.ask(fit.w, fit.h);
                    const WindowPlace at = placeInWorkArea(m.x, m.y, fit.w, fit.h, work);   // main.cpp moves the window inside the work area after it grows
                    m.x = at.x; m.y = at.y;
                    f.applied(to);
                    m.run(16);
                    if (std::getenv("DBGLOOK") != nullptr) { std::printf("   go os %.2f pf %d to %.2f start %ux%u ns %ux%u fit %ux%u -> %ux%u\n", static_cast<double>(os), pushFirst ? 1 : 0, static_cast<double>(to), w0, h0, ns.first, ns.second, fit.w, fit.h, m.w, m.h); }
                    // what the user sees: at least the smallest window, unless the screen is smaller; never larger than the screen; never collapsed
                    const std::uint32_t needW = std::min(static_cast<std::uint32_t>(std::ceil(kMinW * to)), static_cast<std::uint32_t>(screenW));
                    const std::uint32_t needH = std::min(static_cast<std::uint32_t>(std::ceil(kMinH * to)), static_cast<std::uint32_t>(screenH));
                    CHECK(m.w >= needW && m.h >= needH);
                    CHECK(m.w <= static_cast<std::uint32_t>(screenW) && m.h <= static_cast<std::uint32_t>(screenH));
                    // and the whole window is on the screen: its right and bottom edges are inside the work area, not only its size (the drive saw the bottom 74 px below a 1440 px screen at 150 %)
                    if (m.x < 0 || m.y < 0 || m.x + static_cast<int>(m.w) > screenW || m.y + static_cast<int>(m.h) > screenH) { std::printf("  off the screen: %dx%d screen, os %.2f scale %.2f window %ux%u at %d,%d\n", screenW, screenH, static_cast<double>(os), static_cast<double>(to), m.w, m.h, m.x, m.y); }
                    CHECK(m.x >= 0 && m.y >= 0 && m.x + static_cast<int>(m.w) <= screenW && m.y + static_cast<int>(m.h) <= screenH);
                    const std::uint32_t wSeen = m.w, hSeen = m.h;
                    m.run(12);
                    CHECK(m.w == wSeen && m.h == hSeen);   // and it stays put: nothing moves it a tick later
                };
                for (const float s : steps) { if (s > os + 0.001f) { go(s); } }                                   // every size up
                for (int i = 5; i >= 0; --i) { if (steps[i] > os + 0.001f && steps[i] < 2.5f) { go(steps[i]); } }   // and every size down
                go(os);                                                                                          // Ctrl+0: Auto
                const std::uint32_t needW = std::min(static_cast<std::uint32_t>(std::ceil(kMinW * os)), static_cast<std::uint32_t>(screenW));
                CHECK(m.w >= needW);
                if (w0 <= static_cast<std::uint32_t>(screenW) && h0 <= static_cast<std::uint32_t>(screenH) && w0 >= needW) { if (!(m.w == w0 && m.h == h0)) { std::printf("  back at auto: os %.2f pushFirst %d start %ux%u now %ux%u\n", static_cast<double>(os), pushFirst ? 1 : 0, w0, h0, m.w, m.h); } CHECK(m.w == w0 && m.h == h0); }   // Ctrl+0 returns to the automatic size
            }
        }
    }
    // the fault itself: the mirror as it was shrinks a 1300 x 780 window to a few pixels at 150 % with the system at 100 % (what the drive saw: 1921 x 1081 became 150 x 90)
    {
        Machine m;
        m.naive = true; m.os = 1.0f; m.scale = 1.0f; m.w = 1300; m.h = 780;
        m.run(4);
        m.scale = 1.5f;
        m.ask(1950, 1170);
        m.run(40);
        CHECK(m.w < 100 && m.h < 100);
    }
    // the same step with the mirror: the window is where it was asked to be
    {
        Machine m;
        m.os = 1.0f; m.scale = 1.0f; m.w = 1300; m.h = 780;
        m.run(4);
        m.scale = 1.5f;
        m.ask(1950, 1170);
        m.run(40);
        CHECK(m.w == 1950 && m.h == 1170);
    }
    // a size the system never gives (a maximised window ignores the ask): the mirror waits ten ticks and then carries on, and does not hold the screen at a stale size for ever
    {
        SizeMirror mirror;
        mirror.asked(2400, 1350);
        int silent = 0;
        for (int i = 0; i < 30; ++i) { if (mirror.tick(1600, 900, 1.5f, 1.0f).write) { break; } ++silent; }
        CHECK(silent == 10);   // ten ticks of waiting, then the first write
    }
    // where the window sits: the least move that brings it inside the work area; a window as big as the area sits at its corner
    {
        const WorkRect r{0, 0, 5120, 1392};
        WindowPlace p = placeInWorkArea(1360, 270, 2400, 1350, r);
        CHECK(p.x == 1360 && p.y == 42);   // 1600 x 900 centred on 5120 x 1440, grown to 150 %: its bottom was 270 + 1350 = 1620; now 1392
        p = placeInWorkArea(4000, 100, 2400, 1350, r);
        CHECK(p.x == 2720 && p.y == 42);
        p = placeInWorkArea(-300, -50, 800, 600, r);
        CHECK(p.x == 0 && p.y == 0);
        p = placeInWorkArea(500, 300, 800, 600, r);
        CHECK(p.x == 500 && p.y == 300);   // a window already inside is left alone
        p = placeInWorkArea(80, 90, 5120, 1392, r);
        CHECK(p.x == 0 && p.y == 0);
        const WorkRect second{1920, 0, 3840, 1040};   // a second monitor: the area's own origin
        p = placeInWorkArea(1000, 500, 1000, 800, second);
        CHECK(p.x == 1920 && p.y == 240);
        p = placeInWorkArea(100, 200, 500, 500, WorkRect{});
        CHECK(p.x == 100 && p.y == 200);   // no area known: where it is
    }
    // the work-area fit: the screen wins over the minimum; the minimum wins over a small grow
    {
        WindowFit f = fitWindowToScreen(800, 500, 1.0f, 1024.0f, 640.0f, 1920, 1040);
        CHECK(f.w == 1024 && f.h == 640);
        f = fitWindowToScreen(2401, 1351, 1.25f, 1024.0f, 640.0f, 1920, 1040);
        CHECK(f.w == 1920 && f.h == 1040);
        f = fitWindowToScreen(1000, 600, 2.5f, 1024.0f, 640.0f, 1920, 1040);
        CHECK(f.w == 1920 && f.h == 1040);   // 2560 x 1600 would not fit the screen
        f = fitWindowToScreen(1000, 600, 2.5f, 1024.0f, 640.0f, 0, 0);
        CHECK(f.w == 2560 && f.h == 1600);   // no screen known: the minimum alone
        f = fitWindowToScreen(1300, 780, 1.0f, 1024.0f, 640.0f, 1920, 1040);
        CHECK(f.w == 1300 && f.h == 780);    // a window already inside both is left alone
    }
    // USER BUG C: the window at start, fitted to the work area of the monitor it opens on (taskbar already out of the work area; physical pixels)
    {
        const auto fit = [](float scale, int w, int h, int cx, int cy) { return fitStartWindow(1600.0f, 900.0f, 1024.0f, 640.0f, scale, WorkRect{0, 0, w, h}, cx, cy); };
        StartFit s = fit(1.0f, 1366, 728, 500, 400);   // 1366 x 768 laptop, 100 %
        CHECK(s.w == 1366 && s.h == 728 && s.x == 0 && s.y == 0 && !s.maximise && s.minW == 1024.0f && s.minH == 640.0f);
        s = fit(1.0f, 1280, 680, 300, 300);            // 1280 x 720 screen, 100 %
        CHECK(s.w == 1280 && s.h == 680 && s.x == 0 && s.y == 0 && !s.maximise);
        s = fit(1.25f, 1920, 1040, 400, 100);          // 1920 x 1080 at 125 % (1536 x 864 logical): 2000 x 1125 was asked of a 1920 x 1040 area
        CHECK(s.w == 1920 && s.h == 1040 && s.x == 0 && s.y == 0 && !s.maximise);
        s = fit(1.5f, 1920, 1040, 10, 10);             // 1920 x 1080 at 150 % (1280 x 720 logical): the smallest window, 1536 x 960, still fits
        CHECK(s.w == 1920 && s.h == 1040 && !s.maximise && s.minW == 1024.0f && s.minH == 640.0f);
        s = fit(1.0f, 1024, 560, 0, 0);                // 1024 x 600 screen: 560 high is under the 640 minimum - maximised, the minimum lowered to what the screen has
        CHECK(s.maximise && s.w == 1024 && s.h == 560 && s.x == 0 && s.y == 0 && s.minW == 1024.0f && s.minH == 560.0f);
        s = fit(1.25f, 1366, 728, 0, 0);               // 1366 x 768 at 125 %: the work area is 1093 x 582 logical, under the 640 minimum height: maximised, minimum 1024 x 582.4
        CHECK(s.maximise && s.w == 1366 && s.h == 728 && s.minW == 1024.0f && s.minH > 582.3f && s.minH < 582.5f);
        s = fit(1.0f, 5120, 1392, 1360, 270);          // the 5120 x 1440 desk: 1600 x 900 fits and stays where it is
        CHECK(s.w == 1600 && s.h == 900 && s.x == 1360 && s.y == 270 && !s.maximise);
        s = fit(1.0f, 5120, 1392, 4000, 800);          // opened near the right and bottom edges: moved in, never out of the area
        CHECK(s.w == 1600 && s.h == 900 && s.x == 3520 && s.y == 492);
        s = fit(2.5f, 5120, 1392, 100, 100);           // 250 %: 4000 x 2250 asked; the area is 1392 high, under the 1600 minimum: maximised, minimum 556.8 logical high
        CHECK(s.maximise && s.w == 5120 && s.h == 1392 && s.minW == 1024.0f && s.minH > 556.7f && s.minH < 556.9f);
        s = fitStartWindow(1600.0f, 900.0f, 1024.0f, 640.0f, 1.0f, WorkRect{1920, 0, 3840, 1040}, 100, 100);   // a second monitor: placed inside ITS area
        CHECK(s.w == 1600 && s.h == 900 && s.x == 1920 && s.y == 100);
        s = fitStartWindow(1600.0f, 900.0f, 1024.0f, 640.0f, 1.0f, WorkRect{}, 77, 88);   // no work area known: the wanted size, left where it is
        CHECK(s.w == 1600 && s.h == 900 && s.x == 77 && s.y == 88 && !s.maximise && s.minW == 1024.0f);
        for (int w : {1024, 1280, 1366, 1536, 1920, 2560, 5120}) {   // whatever the area: the window is inside it
            for (int h : {560, 680, 728, 864, 1040, 1392}) {
                for (float sc : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f}) {
                    s = fit(sc, w, h, 3000, 3000);
                    CHECK(s.x >= 0 && s.y >= 0 && s.x + static_cast<int>(s.w) <= w && s.y + static_cast<int>(s.h) <= h);
                }
            }
        }
    }
    // THE HOLD AFTER THE FIT (review C1 + C2): the fit is held while the system settles, and let go of the moment the USER has the window
    {
        const StartFit fit = fitStartWindow(1600.0f, 900.0f, 1024.0f, 640.0f, 1.0f, WorkRect{0, 0, 1366, 728}, 104, 104);   // 1366 x 728 at 0,0
        const HoldLook before{1600, 900, 104, 104, false, false};
        const auto fresh = [&](const StartFit& f, const HoldLook& b) { StartHold h; h.begin(b, f); return h; };
        {   // a normal start: asked; the system maps the window at ITS size and place after the first ask (not the user's: asked again); then it takes the fit; the hold runs out
            StartHold h = fresh(fit, before);
            HoldAction a = h.look(before, 0);
            CHECK(a.kind == HoldAction::Kind::Reassert && a.setSize && a.setPlace && !a.setMaximised);
            a = h.look(HoldLook{1600, 900, 156, 156, false, false}, 33);   // mapped at the system's default place: not the user's
            CHECK(a.kind == HoldAction::Kind::Reassert && a.setSize && a.setPlace);
            a = h.look(HoldLook{1366, 728, 156, 156, false, false}, 66);   // the size was taken, the place not yet
            CHECK(a.kind == HoldAction::Kind::Reassert && !a.setSize && a.setPlace);
            a = h.look(HoldLook{1366, 728, 0, 0, false, false}, 99);       // both taken: nothing more to ask
            CHECK(a.kind == HoldAction::Kind::Reassert && !a.setSize && !a.setPlace);
            bool ended = false;
            for (int i = 0; i < 40 && !ended; ++i) { ended = h.look(HoldLook{1366, 728, 0, 0, false, false}, 130 + i * 33).kind == HoldAction::Kind::Finished; }
            CHECK(ended && h.over());   // 20 looks, then it lets go
        }
        {   // the user moves the window once the fit has been taken (two looks in a row): never undone, and the hold stays over
            StartHold h = fresh(fit, before);
            (void)h.look(before, 0);
            (void)h.look(HoldLook{1366, 728, 0, 0, false, false}, 33);
            (void)h.look(HoldLook{1366, 728, 0, 0, false, false}, 66);
            HoldAction a = h.look(HoldLook{1366, 728, 60, 40, false, false}, 99);
            CHECK(a.kind == HoldAction::Kind::User && h.over());
            a = h.look(before, 132);
            CHECK(a.kind == HoldAction::Kind::User && !a.setSize && !a.setPlace && !a.setMaximised);   // later looks are answered "user", nothing is asked
        }
        {   // the user (a Win+Arrow snap, a system drag) sizes it after the fit was taken: a size that is neither the fit's nor the old one
            StartHold h = fresh(fit, before);
            (void)h.look(before, 0);
            (void)h.look(HoldLook{1366, 728, 0, 0, false, false}, 33);
            (void)h.look(HoldLook{1366, 728, 0, 0, false, false}, 66);
            const HoldAction a = h.look(HoldLook{1246, 648, 0, 0, false, false}, 99);
            CHECK(a.kind == HoldAction::Kind::User);
        }
        {   // the user maximises it (not what the fit asked for)
            StartHold h = fresh(fit, before);
            (void)h.look(before, 0);
            const HoldAction a = h.look(HoldLook{1366, 728, 0, 0, true, false}, 33);
            CHECK(a.kind == HoldAction::Kind::User);
        }
        {   // the user minimises it
            StartHold h = fresh(fit, before);
            (void)h.look(before, 0);
            const HoldAction a = h.look(HoldLook{1600, 900, -32000, -32000, false, true}, 33);
            CHECK(a.kind == HoldAction::Kind::User);
        }
        {   // a window that starts minimised or maximised is left exactly as it is
            StartHold h = fresh(fit, HoldLook{1600, 900, -32000, -32000, false, true});
            CHECK(h.look(HoldLook{1600, 900, -32000, -32000, false, true}, 0).kind == HoldAction::Kind::User);
            StartHold m = fresh(fit, HoldLook{1920, 1040, 0, 0, true, false});
            CHECK(m.look(HoldLook{1920, 1040, 0, 0, true, false}, 0).kind == HoldAction::Kind::User);
        }
        {   // the time bound: a slow start (a look every 200 ms) is held for 1.5 s at most, not for 20 looks
            StartHold h = fresh(fit, before);
            HoldAction a;
            int looks = 0;
            for (; looks < 20; ++looks) { a = h.look(before, looks * 200); if (a.kind != HoldAction::Kind::Reassert) { break; } }
            CHECK(a.kind == HoldAction::Kind::Finished && looks < 10);
        }
        {   // the fit maximises (a work area under the smallest window): the size and the place go FIRST, the maximise in the same ask, so Restore gives the fitted window
            const StartFit tiny = fitStartWindow(1600.0f, 900.0f, 1024.0f, 640.0f, 1.0f, WorkRect{0, 0, 1024, 560}, 104, 104);
            CHECK(tiny.maximise && tiny.w == 1024 && tiny.h == 560 && tiny.x == 0 && tiny.y == 0);
            StartHold h = fresh(tiny, before);
            HoldAction a = h.look(before, 0);
            CHECK(a.kind == HoldAction::Kind::Reassert && a.setSize && a.setPlace && !a.setMaximised);   // NOT maximised yet: the restore rectangle must be the fitted one
            a = h.look(HoldLook{1600, 900, 156, 156, false, false}, 33);   // the system mapped the window at its own size and place: asked again, still no maximise
            CHECK(a.kind == HoldAction::Kind::Reassert && a.setSize && a.setPlace && !a.setMaximised);
            a = h.look(HoldLook{1024, 560, 0, 0, false, false}, 66);       // taken (first look)
            CHECK(a.kind == HoldAction::Kind::Reassert && !a.setSize && !a.setPlace && !a.setMaximised);
            a = h.look(HoldLook{1024, 560, 0, 0, false, false}, 99);       // taken (second look in a row): NOW the maximise, after the size and the place
            CHECK(a.kind == HoldAction::Kind::Reassert && !a.setSize && !a.setPlace && a.setMaximised);
            a = h.look(HoldLook{5120, 1392, -8, -8, true, false}, 132);    // maximised: done, never touched again
            CHECK(a.kind == HoldAction::Kind::Finished);
            a = h.look(HoldLook{1024, 560, 0, 0, false, false}, 165);      // (a Restore by the user afterwards is not undone)
            CHECK(a.kind == HoldAction::Kind::Finished && !a.setMaximised);
        }
    }
}

// MAP FOCUS: the torn-off page windows follow the main window's rule (look_logic.hpp tornResizeAt / tornMoveAt): the size and the place are a function of the press and of the pointer's
// total offset from it, never of the size the frame has now. The rule they had (kit_publish.cpp, now removed) added every report to the current size, so a frame one answer late ran away.
void tornWindow() {
    {   // the old rule, as the test of the main window keeps its own: "size now plus this report", one answer late
        float late = 700.f, asked = 700.f;
        for (int i = 0; i < 6; ++i) { const float next = late + 20.f; late = asked; asked = next; }   // the pointer stands 20 px right of the press; six reports arrive
        CHECK(asked != 720.f);
    }
    EdgeDrag d;
    d.begin(1, 0.f, 0.f, 700, 500);                   // the right edge of a 700 x 500 frame
    for (int i = 0; i < 1000; ++i) {                  // a thousand reports of one offset: one size
        const WindowFit f = tornResizeAt(d, 20.f, 300.f);
        CHECK(f.w == 720 && f.h == 500);
    }
    WindowFit f = tornResizeAt(d, -1000.f, 0.f);
    CHECK(f.w == 240 && f.h == 500);                  // never narrower than 240
    f = tornResizeAt(d, 0.f, 0.f);
    CHECK(f.w == 700);                                // back at the press: the size it had
    d.begin(2, 0.f, 0.f, 700, 500);                   // the bottom edge ignores across and stops at 140
    f = tornResizeAt(d, 300.f, 80.f);
    CHECK(f.w == 700 && f.h == 580);
    f = tornResizeAt(d, 0.f, -1000.f);
    CHECK(f.h == 140);
    d.begin(3, 0.f, 0.f, 700, 500);                   // the corner: both
    f = tornResizeAt(d, 100.f, 80.f);
    CHECK(f.w == 800 && f.h == 580);
    f = tornResizeAt(d, 90000.f, 90000.f);
    CHECK(f.w == 16384 && f.h == 16384);              // the limit
    // moving: from where the window was at the press
    TornPlace p = tornMoveAt(80.f, 60.f, 25.f, -10.f);
    CHECK(p.x == 105.f && p.y == 50.f);
    p = tornMoveAt(80.f, 60.f, -500.f, -500.f);
    CHECK(p.x == 0.f && p.y == 0.f);                  // never above or left of the main window
    for (int i = 0; i < 1000; ++i) { p = tornMoveAt(80.f, 60.f, 25.f, -10.f); }
    CHECK(p.x == 105.f && p.y == 50.f);               // the same offset, the same place
}

// DRIVE FIXES 7: the drag of a window edge. The fault was a size that depended on the size the window had when a move arrived (see look_logic.hpp EdgeDrag): the same pointer
// position, reported again before the window had resized, grew the window again. So the first thing held here is that it cannot: any number of moves at one pointer position ask for
// one size, whatever the window's size is by then.
void edgeDrag() {
    // the rule it replaces, kept here so the test shows the fault it guards against: "the size now plus the distance" with a window that is one answer late
    {
        std::uint32_t late = 1107;                    // the size the window reports: always the one asked for a move ago
        std::uint32_t asked = 1107;
        for (int i = 0; i < 6; ++i) { const std::uint32_t next = late + 20; late = asked; asked = next; }   // the pointer stands 20 px right of the press, six moves arrive
        CHECK(asked != 1127);                         // the old rule did not stay at press + 20: this is the runaway
    }
    EdgeDrag d;
    CHECK(!d.active());
    d.begin(1, 1104.0f, 300.0f, 1107, 673);           // the right edge of a 1107 x 673 window, pressed 3 px inside it
    CHECK(d.active() && d.edge() == 1);
    for (int i = 0; i < 1000; ++i) {                  // a thousand moves at one place: one size, every time
        const WindowFit f = d.at(1124.0f, 300.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
        CHECK(f.w == 1127 && f.h == 673);
    }
    WindowFit f = d.at(1404.0f, 900.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1407 && f.h == 673);                 // the right edge follows the pointer across and ignores it down
    f = d.at(1104.0f, 300.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1107 && f.h == 673);                 // back at the press: the size it had
    f = d.at(200.0f, 300.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1024 && f.h == 673);                 // never narrower than the smallest window
    f = d.at(90000.0f, 300.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 16384);                              // never wider than the limit
    d.end();
    CHECK(!d.active());
    d.begin(2, 500.0f, 670.0f, 1107, 673);            // the bottom edge
    f = d.at(900.0f, 770.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1107 && f.h == 773);                 // follows down, ignores across
    f = d.at(500.0f, 10.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.h == 640);
    d.begin(3, 1102.0f, 668.0f, 1107, 673);           // the corner: both
    f = d.at(1202.0f, 718.0f, 1.0f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1207 && f.h == 723);
    // a scale that is not 1: the pointer is in logical pixels, the size in physical ones; the smallest window grows with the scale
    d.begin(3, 1000.0f, 600.0f, 1600, 1000);
    f = d.at(1100.0f, 640.0f, 1.5f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1750 && f.h == 1060);
    f = d.at(0.0f, 0.0f, 1.5f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1536 && f.h == 960);
    // a pointer position that is not a number changes nothing
    f = d.at(std::nanf(""), 640.0f, 1.5f, 1024.0f, 640.0f, 16384, 16384);
    CHECK(f.w == 1600 && f.h == 1060);
    // a limit below the smallest window is not a limit
    d.begin(1, 10.0f, 10.0f, 1024, 640);
    f = d.at(10.0f, 10.0f, 1.0f, 1024.0f, 640.0f, 800, 800);
    CHECK(f.w == 1024 && f.h == 640);
}

}  // namespace

int main() {
    edgeDrag();
    windowSizeAtEverySize(1920, 1040);   // a 1920 x 1080 screen less its task bar
    windowSizeAtEverySize(5120, 1440);   // drive fixes 6: the owner's screen, and the same less a 48 px task bar
    windowSizeAtEverySize(5120, 1392);
    windowSizeAtEverySize(1366, 768);    // the test machine, and the same less a 40 px task bar
    windowSizeAtEverySize(1366, 728);
    skins();
    steps();
    counterFit();
    faces();
    wanted();
    tornWindow();
    resizing();
    clampedGrowRoundTrip();
    follower();
    return testSummary("test_slint_host_look");
}
