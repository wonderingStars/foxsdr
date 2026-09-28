// Does the interface-scale factor do exactly what gui/ui_scale.hpp claims,
// and nothing else?
//
// WHY THIS FILE EXISTS. A tester on a 4K monitor: "the fonts are too small,
// I tried other settings in the View tab but couldn't make it bigger." The
// fix threads one factor, S, through every font size and every hard-coded
// layout pixel in the application - and the ONE THING that fix must never do
// is move anything for the tester who is NOT on a 4K monitor: a 96 dpi
// display with no override must render bit-identically to every release
// before this one. That is the S=1 guarantee, and it rests entirely on the
// arithmetic this file pins: effectiveScale("auto", 96) must be exactly
// 1.0f, and px(v) at that factor must be exactly v, for every v a caller
// might pass.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>

#include "gui/theme.hpp"
#include "gui/ui_scale.hpp"
#include "gui/ui_style_compose.hpp"
#include "test_check.hpp"

using namespace cascade::gui::uiscale;

namespace {

// --- 1. THE S=1 GUARANTEE, pinned as a fact rather than trusted from the
// arithmetic that happens to produce it. This is the check a future edit to
// effectiveScale() must not break, ever - it is the whole reason a scale
// factor can be threaded through hundreds of call sites without a single
// screenshot changing at the default.
void testS1IsExact() {
    std::printf("  effectiveScale(\"auto\", 96) is exactly 1.0f\n");
    CHECK(effectiveScale("auto", 96) == 1.0f);
    // And px(v) at that factor is v, bit-for-bit, for a spread of values -
    // not just round ones, because a layout constant is rarely round.
    for (const float v : {0.0f, 1.0f, -1.0f, 3.5f, 384.0f, 22.0f, 8.0f, 0.20f,
                          152.0f, -97.0f, 1e6f}) {
        setChoice("auto");
        setMonitorDpi(96);
        CHECK(factor() == 1.0f);
        CHECK(px(v) == v);
    }
}

// --- 2. "auto" FOLLOWS THE MONITOR, continuously, never snapped to a step.
void testAutoFollowsMonitorContinuously() {
    std::printf("  \"auto\" is monitorDpi/96, unsnapped\n");
    CHECK_NEAR(effectiveScale("auto", 144), 1.5, 1e-6);   // 150% Windows scaling
    CHECK_NEAR(effectiveScale("auto", 192), 2.0, 1e-6);   // 200%
    CHECK_NEAR(effectiveScale("auto", 120), 1.25, 1e-6);  // 125%
    // An odd percentage some laptops offer (110%) is followed exactly, not
    // pulled to the nearest of the six offered STEPS (nearestStep is a
    // different function, for a different job - see test 5).
    CHECK_NEAR(effectiveScale("auto", 106), 106.0 / 96.0, 1e-6);
    // dpi == 0 (never detected) reads as 96, not a division by zero.
    CHECK(effectiveScale("auto", 0) == 1.0f);
}

// --- 3. A FIXED CHOICE OVERRIDES THE MONITOR ENTIRELY, on every monitor.
void testFixedChoiceIgnoresMonitor() {
    std::printf("  a fixed step ignores what the monitor reports\n");
    for (const unsigned dpi : {unsigned{96}, unsigned{144}, unsigned{192}, unsigned{240}}) {
        CHECK(effectiveScale("200", dpi) == 2.0f);
        CHECK(effectiveScale("100", dpi) == 1.0f);
        CHECK(effectiveScale("150", dpi) == 1.5f);
    }
}

// --- 4. NORMALIZATION: anything not "auto" or one of the six steps is
// "auto" - a hand-edited config, an older or newer build's spelling, empty
// text, never a refusal to start.
void testNormalizeChoiceDegradesToAuto() {
    std::printf("  an unrecognised choice normalises to auto\n");
    CHECK(normalizeChoice("auto") == "auto");
    for (const int step : kSteps) {
        CHECK(normalizeChoice(std::to_string(step)) == std::to_string(step));
    }
    for (const char* bad : {"", "110", "Auto", "AUTO", "100%", "2", "-100", "1000",
                            "one hundred", "100.0"}) {
        if (normalizeChoice(bad) != "auto") {
            std::printf("      \"%s\" normalised to \"%s\", want \"auto\"\n", bad,
                        normalizeChoice(bad).c_str());
        }
        CHECK(normalizeChoice(bad) == "auto");
    }
    // effectiveScale itself normalises too - a bad choice reads as auto
    // rather than as std::stoi throwing.
    CHECK(effectiveScale("bogus", 96) == 1.0f);
    CHECK(effectiveScale("", 192) == 2.0f);
}

// --- 5. nearestStep: used only by Ctrl+=/Ctrl+- to find where an arbitrary
// "auto" percentage sits among the six offered steps - never by effectiveScale
// itself (test 2 above is what proves "auto" is NOT snapped).
void testNearestStep() {
    std::printf("  nearestStep finds the closest offered percentage\n");
    CHECK(nearestStep(100) == 100);
    CHECK(nearestStep(96) == 100);   // below the lowest step: pulled up to it
    CHECK(nearestStep(0) == 100);
    CHECK(nearestStep(112) == 100);  // closer to 100 than to 125
    CHECK(nearestStep(113) == 125);  // closer to 125 than to 100
    CHECK(nearestStep(250) == 250);
    CHECK(nearestStep(9999) == 250);  // above the highest step: pulled down
    CHECK(nearestStep(-50) == 100);
    // dpiToPercent feeds nearestStep in the keyboard-stepping code: a 140 dpi
    // monitor is 146% (140/96, rounded), 4 points from 150 and 21 from 125 -
    // Ctrl+= from there should offer 150 next, not 125.
    CHECK(dpiToPercent(140) == 146);
    CHECK(nearestStep(dpiToPercent(140)) == 150);
}

void testDpiToPercent() {
    std::printf("  dpiToPercent is dpi/96 as a rounded whole percent\n");
    CHECK(dpiToPercent(96) == 100);
    CHECK(dpiToPercent(0) == 100);   // undetected reads as 96, not 0%
    CHECK(dpiToPercent(144) == 150);
    CHECK(dpiToPercent(192) == 200);
    CHECK(dpiToPercent(120) == 125);
    CHECK(dpiToPercent(240) == 250);
}

// --- 6. THE LIVE STATE MACHINE: setChoice/setMonitorDpi/factor/consumeChanged.
// This is what gui/app_window.cpp's applyPendingUiScale relies on to rebuild
// the ImGui style exactly once per real change - never on every frame
// (compounding ScaleAllSizes), and never missing a change either.
void testLiveStateChangeTracking() {
    std::printf("  consumeChanged() fires exactly once per real change\n");
    setChoice("auto");
    setMonitorDpi(96);
    (void)consumeChanged();  // drain whatever the previous test left pending

    // No-op: same choice, same dpi -> no change reported.
    setChoice("auto");
    setMonitorDpi(96);
    CHECK(!consumeChanged());

    // A monitor move to a scaled display: reported once, then quiet.
    setMonitorDpi(144);
    CHECK(factor() == 1.5f);
    CHECK(consumeChanged());
    CHECK(!consumeChanged());
    // Setting the SAME dpi again is not a change.
    setMonitorDpi(144);
    CHECK(!consumeChanged());

    // A user override: reported once.
    setChoice("200");
    CHECK(factor() == 2.0f);
    CHECK(choice() == "200");
    CHECK(consumeChanged());
    CHECK(!consumeChanged());
    // The monitor moving again while a fixed choice is in force changes
    // monitorDpi() (for the Display combo's "100% here" hint) but NOT the
    // live factor, and so is not reported as a change.
    setMonitorDpi(96);
    CHECK(monitorDpi() == 96);
    CHECK(factor() == 2.0f);  // unchanged: "200" still overrides
    CHECK(!consumeChanged());

    // Back to auto, on the 96 dpi monitor just set: factor moves 2.0 -> 1.0,
    // reported once.
    setChoice("auto");
    CHECK(factor() == 1.0f);
    CHECK(consumeChanged());
    CHECK(!consumeChanged());

    // An invalid choice normalises to auto and, being already auto, is not a
    // change.
    setChoice("bogus");
    CHECK(choice() == "auto");
    CHECK(!consumeChanged());
}

// --- 7. THEME AND SCALE COMPOSE, IN EITHER ORDER (an Opus review, round 4,
// "B1"). gui/app_window.cpp's applyPendingTheme and applyPendingUiScale both
// funnel through gui::uiscale::composeStyle(baseline, factor) - see
// gui/ui_style_compose.hpp for the full story - re-baselining on whatever
// theme::applyTheme() just wrote every time either one runs. This is that
// composition, checked against the REAL theme module (not a stand-in), for
// both orders: picking a theme then changing the scale, and changing the
// scale then picking a theme. Before the fix, composeStyle() itself did not
// exist - applyPendingUiScale kept a function-local static baseline captured
// ONCE, ever, so "theme then scale" lost the theme's colours (reverted to
// whichever theme was live the very first time the function ran) and "scale
// then theme" reset the padding to 100% (applyTheme's own unscaled numbers
// overwrote the live style with nothing downstream ever rescaling them).
void testThemeAndScaleComposeRegardlessOfOrder() {
    std::printf("  theme-then-scale and scale-then-theme converge on the same "
               "style: this theme's colours, sizes at the CURRENT S\n");
    namespace th = cascade::gui::theme;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    // Ground truth: Daylight Lab's own colours and this application's own
    // unscaled sizes, straight from the real theme module with nothing else
    // in play (S is not a concept theme.cpp knows about).
    th::setTheme(th::ThemeId::Daylight);
    th::applyTheme();
    const ImGuiStyle daylightUnscaled = ImGui::GetStyle();
    const ImVec4 daylightWindowBg = daylightUnscaled.Colors[ImGuiCol_WindowBg];
    const ImVec4 daylightText = daylightUnscaled.Colors[ImGuiCol_Text];
    // The premise: Daylight Lab is a genuinely different palette from Today's
    // (the default this process would otherwise still be in), or this test
    // would pass even with B1 present.
    th::setTheme(th::ThemeId::Today);
    th::applyTheme();
    const ImVec4 todayWindowBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    CHECK(daylightWindowBg.x != todayWindowBg.x || daylightWindowBg.y != todayWindowBg.y ||
         daylightWindowBg.z != todayWindowBg.z);

    const float s = 1.5f;

    // ORDER A: theme, then scale. This is applyPendingTheme's own sequence
    // (theme::applyTheme(), then re-baseline, then compose) followed by
    // applyPendingUiScale's (compose again from the SAME baseline, S changed).
    th::setTheme(th::ThemeId::Daylight);
    th::applyTheme();
    ImGuiStyle baseA = ImGui::GetStyle();  // "refreshUiStyleBase()"
    ImGui::GetStyle() = composeStyle(baseA, 1.0f);   // applyPendingTheme's own re-compose, S unchanged
    ImGui::GetStyle() = composeStyle(baseA, s);      // applyPendingUiScale, S -> 1.5
    const ImGuiStyle themeThenScale = ImGui::GetStyle();

    // ORDER B: scale, then theme. Start back at Today so the scale change has
    // something concrete to compose against first.
    th::setTheme(th::ThemeId::Today);
    th::applyTheme();
    ImGuiStyle baseToday = ImGui::GetStyle();
    ImGui::GetStyle() = composeStyle(baseToday, s);  // applyPendingUiScale, S -> 1.5, Today's colours
    // Now the theme changes WHILE S is already 1.5 - applyPendingTheme's real
    // sequence: theme::applyTheme() overwrites the live style (colours AND
    // unscaled sizes) regardless of what was live a moment ago, then
    // re-baseline on THAT, then re-compose at the CURRENT (unchanged) factor.
    th::setTheme(th::ThemeId::Daylight);
    th::applyTheme();
    ImGuiStyle baseB = ImGui::GetStyle();
    ImGui::GetStyle() = composeStyle(baseB, s);
    const ImGuiStyle scaleThenTheme = ImGui::GetStyle();

    // BOTH ORDERS must land on Daylight Lab's colours...
    const auto sameColour = [](const ImVec4& a, const ImVec4& b) {
        return std::fabs(a.x - b.x) < 1e-6f && std::fabs(a.y - b.y) < 1e-6f &&
              std::fabs(a.z - b.z) < 1e-6f && std::fabs(a.w - b.w) < 1e-6f;
    };
    CHECK(sameColour(themeThenScale.Colors[ImGuiCol_WindowBg], daylightWindowBg));
    CHECK(sameColour(scaleThenTheme.Colors[ImGuiCol_WindowBg], daylightWindowBg));
    CHECK(sameColour(themeThenScale.Colors[ImGuiCol_Text], daylightText));
    CHECK(sameColour(scaleThenTheme.Colors[ImGuiCol_Text], daylightText));
    // ...and sizes scaled by 1.5, not left at 100% - both padding fields
    // ScaleAllSizes touches, and FontScaleMain, which is the field a theme
    // apply used to reset to whatever theme.cpp left it (never touched at
    // all, i.e. stuck at the CreateContext default of 1.0).
    CHECK_NEAR(themeThenScale.WindowPadding.x, daylightUnscaled.WindowPadding.x * s, 0.01f);
    CHECK_NEAR(themeThenScale.FramePadding.x, daylightUnscaled.FramePadding.x * s, 0.01f);
    CHECK_NEAR(themeThenScale.FontScaleMain, s, 0.001f);
    CHECK_NEAR(scaleThenTheme.WindowPadding.x, daylightUnscaled.WindowPadding.x * s, 0.01f);
    CHECK_NEAR(scaleThenTheme.FramePadding.x, daylightUnscaled.FramePadding.x * s, 0.01f);
    CHECK_NEAR(scaleThenTheme.FontScaleMain, s, 0.001f);

    // PROVEN TO GO RED against the pre-fix design: reproduce round 3's
    // function-local-static baseline (captured once, from Today, and never
    // refreshed by a theme change) and show it fails exactly what the fix
    // above passes.
    {
        const ImGuiStyle staleBase = baseToday;  // "captured once, at Today"
        // "theme then scale" under the OLD code: theme::applyTheme() runs
        // (Daylight's colours land in the live style token this line), but
        // the STALE (Today) baseline is what the scale change composes from -
        // so the colours it produces come from staleBase, i.e. Today's, not
        // Daylight's.
        th::setTheme(th::ThemeId::Daylight);
        th::applyTheme();
        const ImGuiStyle staleComposed = composeStyle(staleBase, s);
        CHECK(!sameColour(staleComposed.Colors[ImGuiCol_WindowBg], daylightWindowBg));
        CHECK(sameColour(staleComposed.Colors[ImGuiCol_WindowBg], todayWindowBg));
        // "scale then theme" under the OLD code: applyPendingTheme had no
        // re-baseline/re-compose step at all, so a theme pick left the style
        // exactly as theme::applyTheme() wrote it - unscaled padding, even
        // though FontScaleMain (a field applyTheme() never touches) is still
        // sitting at `s` from the line before - the inconsistent, half-scaled
        // result an Opus review's B1 actually described.
        ImGui::GetStyle() = composeStyle(staleBase, s);  // scale applied once, correctly
        th::setTheme(th::ThemeId::Daylight);
        th::applyTheme();  // OLD CODE: nothing composes afterwards
        CHECK(std::fabs(ImGui::GetStyle().WindowPadding.x - daylightUnscaled.WindowPadding.x * s) > 0.5f);
        CHECK_NEAR(ImGui::GetStyle().WindowPadding.x, daylightUnscaled.WindowPadding.x, 0.01f);
    }

    ImGui::DestroyContext();
}

}  // namespace

int main() {
    testS1IsExact();
    testAutoFollowsMonitorContinuously();
    testFixedChoiceIgnoresMonitor();
    testNormalizeChoiceDegradesToAuto();
    testNearestStep();
    testDpiToPercent();
    testLiveStateChangeTracking();
    testThemeAndScaleComposeRegardlessOfOrder();
    // Leave the global state at its default for any test binary that link-
    // shares this translation unit's statics with another (it does not here -
    // one executable per test_*.cpp - but the habit costs nothing).
    setChoice("auto");
    setMonitorDpi(96);
    (void)consumeChanged();
    return testSummary("test_ui_scale");
}
