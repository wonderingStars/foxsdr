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

#include <cmath>
#include <string>

using namespace foxsdr::slintlook;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void skins() {
    CHECK(skinOfTheme("night") == SkinId::NightWatch);
    for (const char* key : {"today", "classic-xl", "glass", "daylight", "field", "", "NIGHT", "nigh", "bogus"}) { CHECK(skinOfTheme(key) == SkinId::Workbench); }
    CHECK(std::string(skinName(SkinId::NightWatch)) == "night");
    CHECK(std::string(skinName(SkinId::Workbench)) == "workbench");
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

}  // namespace

int main() {
    skins();
    faces();
    wanted();
    resizing();
    follower();
    return testSummary("test_slint_host_look");
}
