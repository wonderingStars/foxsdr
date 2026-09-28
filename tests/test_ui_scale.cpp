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

// --- 8. A THEME PICK MUST NOT COMPOUND THE FIELDS IT NEVER TOUCHES (an Opus
// review, round 5, finding 1). theme::applyTheme() writes only the ~20 size
// fields it explicitly assigns (WindowPadding, FramePadding, the
// rounding/border fields, ...); the other ~25 ScaleAllSizes() also scales
// (IndentSpacing, WindowMinSize, CellPadding, TabMinWidthBase, ...) are left
// exactly as the live style already had them. This drives
// gui::uiscale::applyThemeComposed (ui_style_compose.hpp) - the SAME
// function AppWindow::applyPendingTheme calls, not a hand-copied
// reimplementation of its sequence (finding 2: the previous version of this
// test copied the sequence, so deleting the fix from the real
// applyPendingTheme left it green) - against the REAL theme::applyTheme(),
// through three theme picks at 200% and back to 100%, in both possible
// orders relative to the first scale change.
void testThemePickDoesNotCompoundUntouchedFields() {
    std::printf("  three theme picks at 200%% do not compound IndentSpacing/"
               "WindowMinSize/CellPadding/TabMinWidthBase; 100%% restores the "
               "pristine ImGui defaults\n");
    namespace th = cascade::gui::theme;

    const auto freshContext = []() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    };

    // ImGui's own compiled-in defaults (imgui.cpp's ImGuiStyle constructor) -
    // theme::applyTheme() never touches these four, so at S=1, after any
    // number of theme picks and scale changes, they must be EXACTLY these,
    // not merely close.
    constexpr float kDefaultIndent = 21.0f;
    constexpr float kDefaultWindowMinSize = 32.0f;
    constexpr float kDefaultCellPadding = 4.0f;
    constexpr float kDefaultTabMinWidthBase = 1.0f;
    // theme.cpp's own literals - touched by applyTheme, so at S=1 they must
    // come back to exactly these (the assignment is unconditional and
    // identical for every ThemeId).
    constexpr float kThemeWindowPaddingX = 10.0f;
    constexpr float kThemeFramePaddingX = 8.0f;

    const float s = 2.0f;

    // --- ORDER A: the interface size is ALREADY 200% before the first theme
    // ever applies (a saved config loaded before the user picks a theme) -
    // exactly applyPendingUiScale's own scale-only sequence, composeStyle
    // straight from whatever the live style already is.
    {
        freshContext();
        ImGuiStyle base = ImGui::GetStyle();
        bool baseCaptured = true;
        ImGui::GetStyle() = composeStyle(base, s);

        for (int i = 0; i < 3; ++i) {
            const th::ThemeId id = (i % 2 == 0) ? th::ThemeId::Daylight : th::ThemeId::Today;
            cascade::gui::uiscale::applyThemeComposed(
                [&]() { th::setTheme(id); th::applyTheme(); }, base, baseCaptured, s);
            CHECK_NEAR(ImGui::GetStyle().IndentSpacing, kDefaultIndent * s, 0.01f);
            CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, kDefaultWindowMinSize * s, 0.01f);
            CHECK_NEAR(ImGui::GetStyle().CellPadding.x, kDefaultCellPadding * s, 0.01f);
            CHECK_NEAR(ImGui::GetStyle().TabMinWidthBase, kDefaultTabMinWidthBase * s, 0.01f);
        }
        ImGui::GetStyle() = composeStyle(base, 1.0f);  // back to 100%
        CHECK_NEAR(ImGui::GetStyle().IndentSpacing, kDefaultIndent, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, kDefaultWindowMinSize, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.y, kDefaultWindowMinSize, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().CellPadding.x, kDefaultCellPadding, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().TabMinWidthBase, kDefaultTabMinWidthBase, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowPadding.x, kThemeWindowPaddingX, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().FramePadding.x, kThemeFramePaddingX, 0.01f);
        ImGui::DestroyContext();
    }

    // --- ORDER B: a theme is picked once at S=1 (a normal startup), THEN the
    // interface size jumps to 200%, THEN the user browses two more themes -
    // the sequence an actual session produces.
    {
        freshContext();
        ImGuiStyle base{};
        bool baseCaptured = false;
        cascade::gui::uiscale::applyThemeComposed(
            [&]() { th::setTheme(th::ThemeId::Today); th::applyTheme(); }, base, baseCaptured, 1.0f);
        ImGui::GetStyle() = composeStyle(base, s);  // the scale-only path, S -> 2.0
        for (int i = 0; i < 2; ++i) {
            const th::ThemeId id = (i == 0) ? th::ThemeId::Daylight : th::ThemeId::Today;
            cascade::gui::uiscale::applyThemeComposed(
                [&]() { th::setTheme(id); th::applyTheme(); }, base, baseCaptured, s);
        }
        CHECK_NEAR(ImGui::GetStyle().IndentSpacing, kDefaultIndent * s, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, kDefaultWindowMinSize * s, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().CellPadding.x, kDefaultCellPadding * s, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().TabMinWidthBase, kDefaultTabMinWidthBase * s, 0.01f);
        ImGui::GetStyle() = composeStyle(base, 1.0f);
        CHECK_NEAR(ImGui::GetStyle().IndentSpacing, kDefaultIndent, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, kDefaultWindowMinSize, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().CellPadding.x, kDefaultCellPadding, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().TabMinWidthBase, kDefaultTabMinWidthBase, 0.01f);
        ImGui::DestroyContext();
    }

    // --- PROVEN TO GO RED against round 4 (c7082d5): the same three-pick
    // sequence at 200%, but with round 4's OWN sequence inlined - re-baseline
    // and compose AFTER applyTheme(), with nothing restoring the live style
    // to the unscaled baseline FIRST. Not called through applyThemeComposed,
    // because that omission is exactly what round 4 shipped.
    {
        freshContext();
        // base starts as the fresh, UN-composed default (21/32/...) - the
        // live style has not been scaled at all yet, matching the review's
        // own "Indent 21 -> 42 -> ..." starting point exactly.
        ImGuiStyle base = ImGui::GetStyle();
        for (int i = 0; i < 3; ++i) {
            const th::ThemeId id = (i % 2 == 0) ? th::ThemeId::Daylight : th::ThemeId::Today;
            th::setTheme(id);
            th::applyTheme();  // round 4: nothing restores the baseline first
            base = ImGui::GetStyle();
            ImGui::GetStyle() = composeStyle(base, s);
        }
        // The review's own numbers: 21 -> 42 -> 84 -> 168 for IndentSpacing,
        // 32 -> 64 -> 128 -> 256 for WindowMinSize.
        CHECK_NEAR(ImGui::GetStyle().IndentSpacing, 168.0f, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, 256.0f, 0.01f);
        ImGui::GetStyle() = composeStyle(base, 1.0f);
        // Round 4 leaves 84 here, not the pristine 21.
        CHECK_NEAR(ImGui::GetStyle().IndentSpacing, 84.0f, 0.01f);
        CHECK_NEAR(ImGui::GetStyle().WindowMinSize.x, 128.0f, 0.01f);
        ImGui::DestroyContext();
    }
}

// --- 9. STARTUP WINDOW GEOMETRY: THE WORK AREA WINS, EVEN OVER THE MINIMUM
// (an Opus review, round 5, finding 5). See firstLaunchWindowGeometry's own
// comment (gui/ui_scale.hpp) for the full mechanism; this pins the review's
// own repro numbers.
void testFirstLaunchWindowGeometryWorkAreaWins() {
    std::printf("  the startup window never exceeds its monitor's work area, "
               "even at a scaled minimum bigger than a small laptop screen\n");
    // 200% on a 1366x768 laptop: kMinWindowW (624) and kMinWindowH (400)
    // doubled to 1248x800; work area roughly 1366x728 with the taskbar taken
    // out. Desired size is 1280x720 doubled, 2560x1440.
    constexpr int areaX = 0, areaY = 0, areaW = 1366, areaH = 728;
    constexpr int desiredW = 2560, desiredH = 1440;
    constexpr int minW = 1248, minH = 800;

    const WindowGeometry geom =
        firstLaunchWindowGeometry(100, 50, areaX, areaY, areaW, areaH, desiredW, desiredH, minW, minH);
    CHECK(geom.w <= areaW);
    CHECK(geom.h <= areaH);
    CHECK(geom.h == areaH);  // the work area wins outright: 728, not 800
    CHECK(geom.x >= areaX && geom.x + geom.w <= areaX + areaW);
    CHECK(geom.y >= areaY && geom.y + geom.h <= areaY + areaH);

    // PROVEN TO GO RED against round 4's own order (clamp to the area, THEN
    // apply the floor, never re-clamped): the height that sequence produces.
    {
        int w = desiredW, h = desiredH;
        w = std::min(w, areaW);
        h = std::min(h, areaH);
        w = std::max(w, minW);
        h = std::max(h, minH);
        CHECK(h == 800);   // round 4's own number: past the work area
        CHECK(h > areaH);  // ...and this is exactly the bug finding 5 names
    }

    // A big desktop with no minimum conflict is untouched either way - the
    // fix must not shrink a window that already fits.
    const WindowGeometry roomy =
        firstLaunchWindowGeometry(200, 100, 0, 0, 2560, 1440, 2560, 1440, 1248, 800);
    CHECK(roomy.w == 2560);
    CHECK(roomy.h == 1440);

    // A window GLFW placed near the right/bottom edge of its monitor is
    // pulled fully back inside it, not merely resized in place.
    const WindowGeometry edge =
        firstLaunchWindowGeometry(1300, 700, areaX, areaY, areaW, areaH, desiredW, desiredH, minW, minH);
    CHECK(edge.x + edge.w <= areaX + areaW);
    CHECK(edge.y + edge.h <= areaY + areaH);
    CHECK(edge.x >= areaX && edge.y >= areaY);

    // An area of 0 (not yet known) disables that axis's clamp, matching
    // every existing monitorWorkareaForWindow caller's convention.
    const WindowGeometry noArea = firstLaunchWindowGeometry(0, 0, 0, 0, 0, 0, desiredW, desiredH, minW, minH);
    CHECK(noArea.w == desiredW);
    CHECK(noArea.h == desiredH);
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
    testThemePickDoesNotCompoundUntouchedFields();
    testFirstLaunchWindowGeometryWorkAreaWins();
    // Leave the global state at its default for any test binary that link-
    // shares this translation unit's statics with another (it does not here -
    // one executable per test_*.cpp - but the habit costs nothing).
    setChoice("auto");
    setMonitorDpi(96);
    (void)consumeChanged();
    return testSummary("test_ui_scale");
}
