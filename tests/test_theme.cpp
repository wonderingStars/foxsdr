/*
 * THE SIX THEMES: readable, true to foxsdr-ui/1, and today's bench untouched.
 *
 * gui/theme.hpp turned the palette from constants into state (2026-09-25).
 * This pins what that must not cost:
 *
 *   READABILITY. Every preset's reading, text, label and digit ink reaches
 *   WCAG's 4.5:1 against the surfaces it sits on (well, panel, panel head,
 *   and the counter cell's two stops) - foxsdr-ui/1's own rule, and the one
 *   theme.hpp's header states for the bench: a live figure must be on glass.
 *
 *   THE FORMAT. The roles are foxsdr-ui/1's 49, by its names, in its order; a
 *   preset's key is its "base" value; unknown keys are today.
 *
 *   TODAY. Under Today (and Bench Classic XL, which shares its palette) every
 *   named constant is the value 0.99.35 drew and tone() returns its literal
 *   bit for bit; under another theme a single-anchor tone IS that role.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "gui/theme.hpp"
#include "test_check.hpp"

namespace th = cascade::gui::theme;
using th::Role;
using th::ThemeId;

namespace {

constexpr ImU32 rgb(unsigned hex) {
    return IM_COL32((hex >> 16) & 0xFFu, (hex >> 8) & 0xFFu, hex & 0xFFu, 0xFFu);
}

const th::Surface& col(const th::Preset& p, Role r) { return p.colors[static_cast<int>(r)]; }

void requireContrast(const th::Preset& p, Role ink, Role surface, bool bothStops) {
    const ImU32 fg = col(p, ink).top;
    const ImU32 stops[2] = {col(p, surface).top, col(p, surface).bottom};
    for (int s = 0; s < (bothStops ? 2 : 1); ++s) {
        const double ratio = th::contrastRatio(fg, stops[s]);
        if (!(ratio >= 4.5)) {
            std::printf("  %s: %s on %s%s is %.2f:1\n", p.key, th::roleKey(ink),
                        th::roleKey(surface), s == 1 ? " (bottom stop)" : "", ratio);
        }
        CHECK(ratio >= 4.5);
    }
}

}  // namespace

int main() {
    // --- the WCAG formula itself, against its textbook values -------------------
    {
        std::printf("  the contrast formula\n");
        CHECK_NEAR(th::contrastRatio(rgb(0x000000), rgb(0xFFFFFF)), 21.0, 1e-9);
        CHECK_NEAR(th::contrastRatio(rgb(0xFFFFFF), rgb(0xFFFFFF)), 1.0, 1e-9);
        // #767676 on white is the classic "just passes" grey: 4.54:1.
        CHECK_NEAR(th::contrastRatio(rgb(0x767676), rgb(0xFFFFFF)), 4.54, 0.01);
        // Order does not matter.
        CHECK_NEAR(th::contrastRatio(rgb(0x1E3A8A), rgb(0xFFFFFF)),
                   th::contrastRatio(rgb(0xFFFFFF), rgb(0x1E3A8A)), 1e-12);
    }

    // --- readability, every preset ------------------------------------------------
    {
        std::printf("  every preset's figures and words reach 4.5:1 on their surfaces\n");
        for (int i = 0; i < th::kThemeCount; ++i) {
            const th::Preset& p = th::preset(static_cast<ThemeId>(i));
            for (const Role ink : {Role::Reading, Role::Text, Role::Label}) {
                requireContrast(p, ink, Role::Well, false);
                requireContrast(p, ink, Role::Panel, true);
            }
            requireContrast(p, Role::Label, Role::PanelHead, false);
            requireContrast(p, Role::Digit, Role::DigitBg, true);
            // The menus and the rail's chips carry words that are read too.
            requireContrast(p, Role::MenuText, Role::MenuBg, false);
            requireContrast(p, Role::TagText, Role::Tag, false);
            // KEY LEGENDS ARE REPORTED, NOT REQUIRED: today's bench letters its
            // keys ivory on brass (3.2:1) and the lit key dark on pale brass
            // (3.9:1 at its foot) - captions engraved in metal, which theme.hpp
            // allows below 4.5 - and today may not move by a pixel. The other
            // five are printed so a regression there is visible.
            for (const auto& pr : {std::make_pair(Role::CtrlText, Role::Ctrl),
                                   std::make_pair(Role::ActiveText, Role::ActiveBg)}) {
                for (const ImU32 s : {col(p, pr.second).top, col(p, pr.second).bottom}) {
                    const double r = th::contrastRatio(col(p, pr.first).top, s);
                    if (r < 4.5) {
                        std::printf("  (legend) %s: %s on %s is %.2f:1\n", p.key,
                                    th::roleKey(pr.first), th::roleKey(pr.second), r);
                    }
                }
            }
        }
    }

    // --- the format -----------------------------------------------------------------
    {
        std::printf("  the roles are foxsdr-ui/1's, by name and in order\n");
        const char* spec[] = {
            "bg",       "frame",     "frameText", "deck",       "deckInk",     "panel",
            "panelHead", "border",   "well",      "label",      "text",        "muted",
            "ctrl",     "ctrlBorder", "ctrlText", "activeBg",   "activeText",  "activeLine",
            "tag",      "tagText",   "digit",     "digitDim",   "digitBg",     "trace",
            "grid",     "wfLow",     "wfMid",     "wfHigh",     "wfTop",       "accent",
            "reading",  "ok",        "off",       "bad",        "stopBg",      "stopRing",
            "stopText", "meterFace", "meterInk",  "meterNeedle", "knob",       "knobCap",
            "plate",    "plateInk",  "menuBg",    "menuText",   "menuBorder",  "menuHi",
            "sel"};
        CHECK(sizeof(spec) / sizeof(spec[0]) == static_cast<std::size_t>(th::kRoleCount));
        for (int i = 0; i < th::kRoleCount; ++i) {
            CHECK(std::strcmp(th::roleKey(static_cast<Role>(i)), spec[i]) == 0);
        }
        // Gradients only where foxsdr-ui/1 allows them, and every flat role flat.
        int surfaces = 0;
        for (int i = 0; i < th::kRoleCount; ++i) {
            if (th::roleIsSurface(static_cast<Role>(i))) { ++surfaces; }
        }
        CHECK(surfaces == 8);
        for (int t = 0; t < th::kThemeCount; ++t) {
            const th::Preset& p = th::preset(static_cast<ThemeId>(t));
            for (int i = 0; i < th::kRoleCount; ++i) {
                const Role r = static_cast<Role>(i);
                if (!th::roleIsSurface(r)) { CHECK(col(p, r).top == col(p, r).bottom); }
                // Opaque: an alpha is the drawing site's, never the palette's.
                CHECK(((col(p, r).top >> IM_COL32_A_SHIFT) & 0xFFu) == 0xFFu);
            }
        }

        std::printf("  the six preset keys, and unknown keys are today\n");
        const char* keys[] = {"today", "classic-xl", "night", "glass", "daylight", "field"};
        for (int i = 0; i < th::kThemeCount; ++i) {
            CHECK(std::strcmp(th::themeKey(static_cast<ThemeId>(i)), keys[i]) == 0);
            CHECK(th::themeFromKey(keys[i]) == static_cast<ThemeId>(i));
            CHECK(th::themeLabel(static_cast<ThemeId>(i))[0] != '\0');
        }
        for (const char* bad : {"", "Night", "night ", "Night Watch", "classic_xl", "TODAY"}) {
            CHECK(th::themeFromKey(bad) == ThemeId::Today);
        }
        // Bench Classic XL is today's palette with the counter doubled, its
        // switches put away and every reading 1.4 (foxsdr-ui/1's definition).
        const th::Preset& xl = th::preset(ThemeId::ClassicXl);
        const th::Preset& today = th::preset(ThemeId::Today);
        for (int i = 0; i < th::kRoleCount; ++i) {
            CHECK(xl.colors[i].top == today.colors[i].top);
            CHECK(xl.colors[i].bottom == today.colors[i].bottom);
        }
        CHECK_NEAR(xl.counter.scale, 2.0, 1e-9);
        CHECK(!xl.counter.switches);
        CHECK_NEAR(xl.sizes.readings, 1.4, 1e-6);
        CHECK_NEAR(today.counter.scale, 1.0, 1e-9);
        CHECK(today.counter.switches);
        CHECK_NEAR(today.sizes.readings, 1.0, 1e-9);
        // THE FIELDS THE SPEC LEFT OPEN, as the website builder's canonical
        // presets.json fills them (coordinated 2026-09-25): the three readers of
        // foxsdr-ui/1 must agree on what each preset's face, glow, knob, lamp
        // and fonts are.
        {
            const char* face[] = {"nixie", "nixie", "neon", "vfd", "plain", "lcd"};
            const float glow[] = {0.75f, 0.75f, 0.7f, 0.45f, 0.0f, 0.0f};
            const char* knob[] = {"bakelite", "bakelite", "bakelite", "chrome", "chrome",
                                  "bakelite"};
            const char* keysStyle[] = {"raised", "raised", "flat", "flat", "flat", "raised"};
            const float radius[] = {3.0f, 3.0f, 2.0f, 8.0f, 6.0f, 2.0f};
            const char* ui[] = {"bench-serif", "bench-serif", "mono", "sans", "sans", "mono"};
            const char* caption[] = {"bench-serif", "bench-serif", "mono", "condensed", "sans",
                                     "stencil"};
            const char* reading[] = {"bench-serif", "bench-serif", "mono", "sans", "sans", "mono"};
            const char* counterFont[] = {"nixie", "nixie", "mono", "mono", "sans", "mono"};
            for (int i = 0; i < th::kThemeCount; ++i) {
                const th::Preset& p = th::preset(static_cast<ThemeId>(i));
                CHECK(std::strcmp(p.counter.face, face[i]) == 0);
                CHECK_NEAR(p.counter.glow, glow[i], 1e-6);
                CHECK(std::strcmp(p.controls.knob, knob[i]) == 0);
                CHECK(std::strcmp(p.controls.keys, keysStyle[i]) == 0);
                CHECK(std::strcmp(p.controls.lamp, "round") == 0);
                CHECK(std::strcmp(p.controls.stop, "round") == 0);
                CHECK(std::strcmp(p.controls.meter, "needle") == 0);
                CHECK_NEAR(p.controls.radius, radius[i], 1e-6);
                CHECK(std::strcmp(p.fonts.ui, ui[i]) == 0);
                CHECK(std::strcmp(p.fonts.caption, caption[i]) == 0);
                CHECK(std::strcmp(p.fonts.reading, reading[i]) == 0);
                CHECK(std::strcmp(p.fonts.counter, counterFont[i]) == 0);
                // counter.digit and colors.digit: "either alone sets both".
                CHECK(p.counter.digit == col(p, Role::Digit).top);
            }
        }
        // The mockups' own values, spot-checked (Receiver.dc.html, renderVals).
        CHECK(col(th::preset(ThemeId::Night), Role::Digit).top == rgb(0xFF5A3C));
        CHECK(col(th::preset(ThemeId::Glass), Role::Accent).top == rgb(0x7DF3FF));
        CHECK(col(th::preset(ThemeId::Daylight), Role::Panel).top == rgb(0xFFFFFF));
        CHECK(col(th::preset(ThemeId::Field), Role::DigitBg).top == rgb(0xBCCB7C));
        CHECK(col(th::preset(ThemeId::Field), Role::DigitBg).bottom == rgb(0xA4B465));
    }

    // --- today untouched, and the others really re-coloured ---------------------------
    {
        std::printf("  today's named constants and tones are 0.99.35's, bit for bit\n");
        for (const ThemeId todayish : {ThemeId::Today, ThemeId::ClassicXl}) {
            th::setTheme(todayish);
            CHECK(th::isTodayPalette());
            CHECK(th::kBrassBright == rgb(0x8B8069));
            CHECK(th::kBrassMid == rgb(0x6E6552));
            CHECK(th::kBrassDark == rgb(0x4A4234));
            CHECK(th::kBrassTint == rgb(0x9C9078));
            CHECK(th::kBrassShade == rgb(0x7D7360));
            CHECK(th::kEnamel == rgb(0x2A251C));
            CHECK(th::kEnamelDark == rgb(0x1F1B14));
            CHECK(th::kWell == rgb(0x14110C));
            CHECK(th::kVoid == rgb(0x0D0B07));
            CHECK(th::kEngraved == rgb(0x3B3529));
            CHECK(th::kInkMuted == rgb(0x9C9078));
            CHECK(th::kInkFaint == rgb(0x7D7360));
            CHECK(th::kIvory == rgb(0xEFE7D2));
            CHECK(th::kCream == rgb(0xD8CFB4));
            CHECK(th::kAmber == rgb(0xF0A840));
            CHECK(th::kAmberDim == rgb(0x8A5A2A));
            CHECK(th::kGold == rgb(0xD9B23C));
            CHECK(th::kPhosphor == rgb(0x8FD9A0));
            CHECK(th::kPhosphorDim == rgb(0x5F8A55));
            CHECK(th::kAlarm == rgb(0xB8552F));
            CHECK(th::kAlarmHot == rgb(0xE07A4E));
            CHECK_NEAR(th::kKeyRounding, 2.0, 1e-9);
            CHECK_NEAR(th::kPanelRounding, 3.0, 1e-9);
            // A literal on any pair of anchors, and at any alpha, comes back as
            // itself - including one no pair brackets.
            CHECK(th::tone(20, 21, 15, 255, th::ink::Well, th::ink::Panel) ==
                  IM_COL32(20, 21, 15, 255));
            CHECK(th::tone(134, 214, 74, 36, th::ink::Trace) == IM_COL32(134, 214, 74, 36));
            CHECK(th::toneHex(0xD9D9D2, 255, th::ink::KnobCap, th::ink::KnobBot) ==
                  rgb(0xD9D9D2));
            CHECK(th::shadowOf(13, 11, 7, 115) == IM_COL32(13, 11, 7, 115));
            CHECK(th::sheenOf(232, 218, 184, 144) == IM_COL32(232, 218, 184, 144));
            CHECK(th::toneMix(239, 231, 210, 255, th::ink::CtrlTop, th::ink::ActiveBgTop, 0.35f) ==
                  IM_COL32(239, 231, 210, 255));
            // Today's waterfall stops, exactly.
            const th::WfStops w = th::waterfallStops();
            CHECK(w.c[0] == IM_COL32(6, 20, 10, 255));
            CHECK(w.c[1] == IM_COL32(10, 70, 35, 255));
            CHECK(w.c[2] == IM_COL32(30, 140, 60, 255));
            CHECK(w.c[3] == IM_COL32(150, 200, 60, 255));
            CHECK(w.c[4] == IM_COL32(240, 235, 180, 255));
            CHECK(std::string(th::preferredFontPair(todayish)).empty());
        }

        std::printf("  under the other five every named constant and tone follows its role\n");
        for (const ThemeId other : {ThemeId::Night, ThemeId::Glass, ThemeId::Daylight,
                                    ThemeId::Field}) {
            const std::uint32_t before = th::generation();
            th::setTheme(other);
            CHECK(th::generation() != before);
            CHECK(!th::isTodayPalette());
            const th::Preset& p = th::preset(other);
            // One anchor: exactly the role, at the literal's alpha.
            const ImU32 trace = col(p, Role::Trace).top;
            CHECK(th::tone(134, 214, 74, 36, th::ink::Trace) ==
                  ((trace & ~(0xFFu << IM_COL32_A_SHIFT)) | (36u << IM_COL32_A_SHIFT)));
            CHECK(th::kAmber == col(p, Role::Reading).top);
            CHECK(th::kIvory == col(p, Role::Label).top);
            CHECK(th::kVoid == col(p, Role::Well).top);
            CHECK(th::kPhosphor == col(p, Role::Ok).top);
            CHECK(th::kAlarm == col(p, Role::Bad).top);
            // Two anchors: a literal AT an anchor lands on the theme's anchor,
            // and one halfway lands halfway (to rounding).
            CHECK(th::toneHex(0x0D0B07, 255, th::ink::Well, th::ink::Panel) ==
                  col(p, Role::Well).top);
            const ImU32 mid = th::tone((0x0D + 0x2A) / 2, (0x0B + 0x25) / 2, (0x07 + 0x1C) / 2, 255,
                                       th::ink::Well, th::ink::Panel);
            const ImU32 w = col(p, Role::Well).top;
            const ImU32 q = col(p, Role::Panel).top;
            for (const int shift : {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT}) {
                const int want = (static_cast<int>((w >> shift) & 0xFFu) +
                                  static_cast<int>((q >> shift) & 0xFFu)) / 2;
                CHECK(std::abs(static_cast<int>((mid >> shift) & 0xFFu) - want) <= 2);
            }
            // The waterfall's ends are the theme's own.
            const th::WfStops ws = th::waterfallStops();
            CHECK(ws.c[0] == col(p, Role::WfLow).top);
            CHECK(ws.c[4] == col(p, Role::WfTop).top);
            // Radii follow controls.radius (a key two thirds of a panel).
            CHECK_NEAR(th::kPanelRounding, p.controls.radius, 1e-6);
            CHECK_NEAR(th::kKeyRounding, p.controls.radius * 2.0f / 3.0f, 1e-5);
            CHECK(std::string(th::preferredFontPair(other)) != "");
        }
        th::setTheme(ThemeId::Today);
        CHECK(th::kAmber == rgb(0xF0A840));
        CHECK(th::isTodayPalette());
    }

    // --- the readings size ----------------------------------------------------------------
    {
        std::printf("  the readings size stays inside foxsdr-ui/1's 1.0 - 3.0\n");
        th::setReadingsScale(1.4f);
        CHECK_NEAR(th::readingsScale(), 1.4, 1e-6);
        th::setReadingsScale(0.2f);
        CHECK_NEAR(th::readingsScale(), 1.0, 1e-9);
        th::setReadingsScale(9.0f);
        CHECK_NEAR(th::readingsScale(), 3.0, 1e-9);
        th::setReadingsScale(1.0f);
    }

    return testSummary("test_theme");
}
