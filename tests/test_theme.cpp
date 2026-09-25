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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gui/theme.hpp"
#include "gui/tune_control.hpp"
#include "gui/tuner_ink.hpp"
#include "test_check.hpp"

namespace th = cascade::gui::theme;
using th::Role;
using th::ThemeId;

namespace {

constexpr ImU32 rgb(unsigned hex) {
    return IM_COL32((hex >> 16) & 0xFFu, (hex >> 8) & 0xFFu, hex & 0xFFu, 0xFFu);
}

const th::Surface& col(const th::Preset& p, Role r) { return p.colors[static_cast<int>(r)]; }

// --- THE PAIRS IMGUI REALLY DRAWS ------------------------------------------------
//
// WHY THIS IS READ FROM A LIVE FRAME (repair round, 2026-09-25). The first cut
// of this test held menuText to 4.5:1 on menuBg - and Field Radio's combo
// lists, tooltips and menus drew at 1.09:1, because ImGui letters a popup in
// ImGuiCol_Text, not in anything called menuText. A test of the palette's
// intended pairs cannot see the pair the library actually draws. So these are
// read from the style STACK inside a real (headless) frame: an ordinary
// window, a tooltip, a popup and a modal, each asked for the colours it would
// draw with at that moment - whatever theme.cpp or anything else pushed.
struct Pair {
    std::string scope;    // "window", "tooltip", "popup", "modal"
    std::string ink;      // "Text" / "TextDisabled"
    std::string surface;  // the ImGuiCol_ the ink is drawn on
    double ratio = 0.0;
};

ImU32 opaque(const ImVec4& v) {
    const auto c = [](float f) {
        return static_cast<int>(std::lround(std::fmin(std::fmax(f, 0.0f), 1.0f) * 255.0f));
    };
    return IM_COL32(c(v.x), c(v.y), c(v.z), 255);
}

// `top` composited over the opaque `base`, as the GPU blends it.
ImU32 over(const ImVec4& top, ImU32 base) {
    const ImVec4 b = th::vec(base);
    const float a = top.w;
    return opaque(ImVec4(top.x * a + b.x * (1.0f - a), top.y * a + b.y * (1.0f - a),
                         top.z * a + b.z * (1.0f - a), 1.0f));
}

// --- A CONTROL'S STATES MUST LOOK DIFFERENT (repair round 2, 2026-09-25) -------------
//
// Round 1 made every state ground readable under the label by darkening (or
// lightening) it until the label reached 4.5:1 - and in Field Radio that
// stopped a key's hovered and pressed grounds at the same place: 1.006:1
// apart in luminance (CIEDE2000 4.4), where today's bench moves 23. A press
// that does not show is a press the user repeats. Every pair of states of a
// key, a selectable row, a tab and a field - idle, hovered, pressed/selected -
// is read from the same live frame and held apart.
//
// THE RULE IS LUMINANCE CONTRAST >= 1.2 between any two states. Luminance,
// because it is what survives colour-blindness and glare - a change of hue
// alone is invisible to one man in twelve; 1.2, because it is just under the
// weakest step today's own bench takes between states (a field hovered to
// held, 1.227:1), so every theme gives at least today's feedback. CIEDE2000
// is printed alongside as the perceptual check, not required: it compresses
// lightness steps in the darks, where Night Watch's red-on-black lives.
constexpr double kStateStep = 1.2;
struct Step {
    std::string scope;    // "window", "tooltip", "popup", "modal", "litkey"
    std::string widget;   // "Button", "Header", "Tab", "TabDimmed", "Frame"
    std::string a, b;     // the two states
    double ratio = 0.0;   // luminance contrast between their grounds
    double de00 = 0.0;    // CIEDE2000 between them
};
std::vector<Step> g_steps;

// CIEDE2000 (Sharma, Wu & Dalal 2005) between two sRGB colours, D65.
double deltaE2000(ImU32 x, ImU32 y) {
    const auto lab = [](ImU32 c, double* L, double* A, double* B) {
        const auto lin = [](double v) {
            v /= 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        const double r = lin((c >> IM_COL32_R_SHIFT) & 0xFF);
        const double g = lin((c >> IM_COL32_G_SHIFT) & 0xFF);
        const double b = lin((c >> IM_COL32_B_SHIFT) & 0xFF);
        const double X = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
        const double Y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
        const double Z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
        const auto f = [](double t) {
            return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
        };
        *L = 116.0 * f(Y) - 16.0;
        *A = 500.0 * (f(X) - f(Y));
        *B = 200.0 * (f(Y) - f(Z));
    };
    double L1, a1, b1, L2, a2, b2;
    lab(x, &L1, &a1, &b1);
    lab(y, &L2, &a2, &b2);
    const double kPi = 3.14159265358979323846;
    const double deg = kPi / 180.0;
    const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2);
    const double Cm = 0.5 * (C1 + C2);
    const double G = 0.5 * (1.0 - std::sqrt(std::pow(Cm, 7) / (std::pow(Cm, 7) + std::pow(25.0, 7))));
    const double ap1 = (1.0 + G) * a1, ap2 = (1.0 + G) * a2;
    const double Cp1 = std::hypot(ap1, b1), Cp2 = std::hypot(ap2, b2);
    const auto hue = [&](double bb, double ap) {
        if (bb == 0.0 && ap == 0.0) { return 0.0; }
        double h = std::atan2(bb, ap) / deg;
        return h < 0.0 ? h + 360.0 : h;
    };
    const double hp1 = hue(b1, ap1), hp2 = hue(b2, ap2);
    const double dLp = L2 - L1, dCp = Cp2 - Cp1;
    double dhp = 0.0;
    if (Cp1 * Cp2 != 0.0) {
        dhp = hp2 - hp1;
        if (dhp > 180.0) { dhp -= 360.0; } else if (dhp < -180.0) { dhp += 360.0; }
    }
    const double dHp = 2.0 * std::sqrt(Cp1 * Cp2) * std::sin(dhp * deg / 2.0);
    const double Lpm = 0.5 * (L1 + L2), Cpm = 0.5 * (Cp1 + Cp2);
    double hpm = hp1 + hp2;
    if (Cp1 * Cp2 != 0.0) {
        if (std::fabs(hp1 - hp2) > 180.0) { hpm += (hp1 + hp2 < 360.0) ? 360.0 : -360.0; }
        hpm *= 0.5;
    }
    const double T = 1.0 - 0.17 * std::cos((hpm - 30.0) * deg) + 0.24 * std::cos(2.0 * hpm * deg) +
                     0.32 * std::cos((3.0 * hpm + 6.0) * deg) - 0.20 * std::cos((4.0 * hpm - 63.0) * deg);
    const double dTheta = 30.0 * std::exp(-std::pow((hpm - 275.0) / 25.0, 2.0));
    const double Rc = 2.0 * std::sqrt(std::pow(Cpm, 7) / (std::pow(Cpm, 7) + std::pow(25.0, 7)));
    const double Sl = 1.0 + 0.015 * std::pow(Lpm - 50.0, 2.0) / std::sqrt(20.0 + std::pow(Lpm - 50.0, 2.0));
    const double Sc = 1.0 + 0.045 * Cpm;
    const double Sh = 1.0 + 0.015 * Cpm * T;
    const double Rt = -std::sin(2.0 * dTheta * deg) * Rc;
    return std::sqrt(std::pow(dLp / Sl, 2.0) + std::pow(dCp / Sc, 2.0) + std::pow(dHp / Sh, 2.0) +
                     Rt * (dCp / Sc) * (dHp / Sh));
}

// Every pair of states of one widget, over the scope's ground.
void readStates(const char* scope, const char* widget,
                const std::vector<std::pair<const char*, ImGuiCol>>& states, ImU32 ground) {
    for (std::size_t i = 0; i < states.size(); ++i) {
        for (std::size_t j = i + 1; j < states.size(); ++j) {
            const ImU32 a = over(ImGui::GetStyleColorVec4(states[i].second), ground);
            const ImU32 b = over(ImGui::GetStyleColorVec4(states[j].second), ground);
            g_steps.push_back({scope, widget, states[i].first, states[j].first,
                               th::contrastRatio(a, b), deltaE2000(a, b)});
        }
    }
}

// Every text-bearing surface a scope can draw, with the ink ImGui letters on it.
void readScope(const char* scope, bool popupLike, std::vector<Pair>& out) {
    const auto c = [](ImGuiCol i) { return ImGui::GetStyleColorVec4(i); };
    const ImU32 ground = opaque(c(popupLike ? ImGuiCol_PopupBg : ImGuiCol_WindowBg));
    readStates(scope, "Button",
               {{"Button", ImGuiCol_Button}, {"ButtonHovered", ImGuiCol_ButtonHovered},
                {"ButtonActive", ImGuiCol_ButtonActive}}, ground);
    readStates(scope, "Header",
               {{"Header", ImGuiCol_Header}, {"HeaderHovered", ImGuiCol_HeaderHovered},
                {"HeaderActive", ImGuiCol_HeaderActive}}, ground);
    readStates(scope, "Tab",
               {{"Tab", ImGuiCol_Tab}, {"TabHovered", ImGuiCol_TabHovered},
                {"TabSelected", ImGuiCol_TabSelected}}, ground);
    readStates(scope, "TabDimmed",
               {{"TabDimmed", ImGuiCol_TabDimmed}, {"TabDimmedSelected", ImGuiCol_TabDimmedSelected}},
               ground);
    readStates(scope, "Frame",
               {{"FrameBg", ImGuiCol_FrameBg}, {"FrameBgHovered", ImGuiCol_FrameBgHovered},
                {"FrameBgActive", ImGuiCol_FrameBgActive}}, ground);
    const ImU32 text = opaque(c(ImGuiCol_Text));
    const ImU32 disabled = opaque(c(ImGuiCol_TextDisabled));
    struct S {
        const char* name;
        ImGuiCol idx;
    };
    const S surfaces[] = {
        {"ChildBg", ImGuiCol_ChildBg},
        {"FrameBg", ImGuiCol_FrameBg},
        {"FrameBgHovered", ImGuiCol_FrameBgHovered},
        {"FrameBgActive", ImGuiCol_FrameBgActive},
        {"Button", ImGuiCol_Button},
        {"ButtonHovered", ImGuiCol_ButtonHovered},
        {"ButtonActive", ImGuiCol_ButtonActive},
        {"Header", ImGuiCol_Header},
        {"HeaderHovered", ImGuiCol_HeaderHovered},
        {"HeaderActive", ImGuiCol_HeaderActive},
        {"TitleBg", ImGuiCol_TitleBg},
        {"TitleBgActive", ImGuiCol_TitleBgActive},
        {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
        {"MenuBarBg", ImGuiCol_MenuBarBg},
        {"Tab", ImGuiCol_Tab},
        {"TabHovered", ImGuiCol_TabHovered},
        {"TabSelected", ImGuiCol_TabSelected},
        {"TabDimmed", ImGuiCol_TabDimmed},
        {"TabDimmedSelected", ImGuiCol_TabDimmedSelected},
        {"TableHeaderBg", ImGuiCol_TableHeaderBg},
        {"TableRowBgAlt", ImGuiCol_TableRowBgAlt},
    };
    out.push_back({scope, "Text", popupLike ? "PopupBg" : "WindowBg",
                   th::contrastRatio(text, ground)});
    for (const S& s : surfaces) {
        out.push_back({scope, "Text", s.name, th::contrastRatio(text, over(c(s.idx), ground))});
    }
    // DISABLED TEXT IS HELD TO 4.5:1 TOO, not to a lower bar. WCAG exempts an
    // INACTIVE CONTROL from contrast, but this application prints information
    // in ImGuiCol_TextDisabled - "RDS: no data", the MONO flag, the target
    // list's id column, a track's secondary lines - so it is secondary text a
    // user reads, not a control that is off. It only has to look quieter than
    // Text, which is checked separately.
    out.push_back({scope, "TextDisabled", popupLike ? "PopupBg" : "WindowBg",
                   th::contrastRatio(disabled, ground)});
    out.push_back({scope, "TextDisabled", "ChildBg",
                   th::contrastRatio(disabled, over(c(ImGuiCol_ChildBg), ground))});
}

// One headless frame of the theme in force: a window, and inside it a tooltip,
// an open popup and an open modal - the four scopes anything is lettered in.
std::vector<Pair> drawnPairs() {
    std::vector<Pair> out;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
    ImGui::Begin("pairs-window");
    readScope("window", false, out);
    {
        // A KEY LATCHED LIT (the selected demodulator): at rest, under the
        // hand and held, its legend on its face.
        const int n = th::pushLitKeyColours();
        CHECK(n >= 1);
        const ImU32 ground = opaque(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        const ImU32 ink = opaque(ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const std::pair<const char*, ImGuiCol> faces[] = {{"Button", ImGuiCol_Button},
                                                          {"ButtonHovered", ImGuiCol_ButtonHovered},
                                                          {"ButtonActive", ImGuiCol_ButtonActive}};
        for (const auto& f : faces) {
            out.push_back({"litkey", "Text", f.first,
                           th::contrastRatio(ink, over(ImGui::GetStyleColorVec4(f.second), ground))});
        }
        // And it answers the hand: rest, under the hand and held all differ.
        readStates("litkey", "LitKey",
                   {{"Button", ImGuiCol_Button}, {"ButtonHovered", ImGuiCol_ButtonHovered},
                    {"ButtonActive", ImGuiCol_ButtonActive}}, ground);
        ImGui::PopStyleColor(n);
    }
    ImGui::BeginTooltip();
    readScope("tooltip", true, out);
    ImGui::EndTooltip();
    ImGui::OpenPopup("pairs-popup");
    if (ImGui::BeginPopup("pairs-popup")) {
        readScope("popup", true, out);
        ImGui::EndPopup();
    } else {
        std::printf("  the probe popup did not open - no popup pairs read\n");
        CHECK(false);
    }
    ImGui::OpenPopup("pairs-modal");
    if (ImGui::BeginPopupModal("pairs-modal")) {
        readScope("modal", true, out);
        ImGui::EndPopup();
    } else {
        std::printf("  the probe modal did not open - no modal pairs read\n");
        CHECK(false);
    }
    // After every scope has closed, the window's own ink is back.
    const ImU32 after = opaque(ImGui::GetStyleColorVec4(ImGuiCol_Text));
    CHECK(after == opaque(ImGui::GetStyle().Colors[ImGuiCol_Text]));
    ImGui::End();
    ImGui::Render();
    return out;
}

// TODAY'S BENCH MAY NOT MOVE BY ONE PIXEL (the owner's rule for this change),
// and some of the pairs it has always drawn are below 4.5:1: ivory legends
// engraved on brass keys, headers and tabs - captions cut into metal, which
// theme.hpp allows below 4.5 - and its faint disabled ink. They are pinned
// here at exactly the ratio 0.99.35 draws, so today cannot get WORSE either,
// and every pair not listed must reach 4.5 like every other theme's.
struct Frozen {
    const char* scope;
    const char* ink;
    const char* surface;
    double ratio;
};
// Measured from 0.99.35's style (applyBenchStyleColours) in this test's own
// frame: ivory #EFE7D2 on the lit brass #8B8069 a hovered key, a held header
// and a hovered tab show, and the faint ink #7D7360 on the enamel.
const std::vector<Frozen> kTodayFrozen = {
    {"window", "Text", "ButtonHovered", 3.1601},
    {"window", "Text", "HeaderActive", 3.1601},
    {"window", "Text", "TabHovered", 3.1601},
    {"window", "TextDisabled", "WindowBg", 3.6675},
    {"window", "TextDisabled", "ChildBg", 3.8615},
    {"tooltip", "Text", "ButtonHovered", 3.1601},
    {"tooltip", "Text", "HeaderActive", 3.1601},
    {"tooltip", "Text", "TabHovered", 3.1601},
    {"tooltip", "TextDisabled", "PopupBg", 3.2563},
    {"tooltip", "TextDisabled", "ChildBg", 3.7061},
    {"popup", "Text", "ButtonHovered", 3.1601},
    {"popup", "Text", "HeaderActive", 3.1601},
    {"popup", "Text", "TabHovered", 3.1601},
    {"popup", "TextDisabled", "PopupBg", 3.2563},
    {"popup", "TextDisabled", "ChildBg", 3.7061},
    {"modal", "Text", "ButtonHovered", 3.1601},
    {"modal", "Text", "HeaderActive", 3.1601},
    {"modal", "Text", "TabHovered", 3.1601},
    {"modal", "TextDisabled", "PopupBg", 3.2563},
    {"modal", "TextDisabled", "ChildBg", 3.7061},
    // The lit mode key under the hand: 0.99.35 pushed only its rest colour.
    {"litkey", "Text", "ButtonHovered", 3.1601},
};

bool frozenMatch(const Pair& p, double* want) {
    for (const Frozen& f : kTodayFrozen) {
        if (p.scope == f.scope && p.ink == f.ink && p.surface == f.surface) {
            *want = f.ratio;
            return true;
        }
    }
    return false;
}

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

    // --- what ImGui really letters, in every scope, in every theme -----------------------
    {
        std::printf("  every pair ImGui draws - window, tooltip, popup, modal - is readable\n");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
        for (int i = 0; i < th::kThemeCount; ++i) {
            const ThemeId id = static_cast<ThemeId>(i);
            th::setTheme(id);
            th::applyTheme();
            g_steps.clear();
            const std::vector<Pair> pairs = drawnPairs();
            // EVERY STATE OF A CONTROL DIFFERS FROM EVERY OTHER, by kStateStep
            // (today's bench is exactly 0.99.35's and is only reported).
            {
                double worst = 1.0e9;
                double worstDe = 1.0e9;
                std::string worstWhat;
                for (const Step& s : g_steps) {
                    if (std::getenv("FOXSDR_THEME_PRINT_STATES") != nullptr) {
                        std::printf("STATE %s %s %s %s/%s %.3f dE00 %.1f\n", th::themeKey(id),
                                    s.scope.c_str(), s.widget.c_str(), s.a.c_str(), s.b.c_str(),
                                    s.ratio, s.de00);
                    }
                    if (!th::isTodayPalette() && !(s.ratio >= kStateStep)) {
                        std::printf("  %s: %s %s %s vs %s is %.3f:1 apart (dE00 %.1f)\n",
                                    th::themeKey(id), s.scope.c_str(), s.widget.c_str(),
                                    s.a.c_str(), s.b.c_str(), s.ratio, s.de00);
                    }
                    if (!th::isTodayPalette()) { CHECK(s.ratio >= kStateStep); }
                    if (s.ratio < worst) {
                        worst = s.ratio;
                        worstWhat = s.scope + " " + s.a + "/" + s.b;
                    }
                    worstDe = std::min(worstDe, s.de00);
                }
                CHECK(g_steps.size() >= 4u * 13u + 3u);
                std::printf("  %-10s states: %zu pairs, closest %.3f:1 (%s), smallest dE00 %.1f\n",
                            th::themeKey(id), g_steps.size(), worst, worstWhat.c_str(), worstDe);
            }
            CHECK(pairs.size() >= 4u * 24u);
            const bool today = th::isTodayPalette();
            // Today's bench pushes nothing over its popups (0.99.35 exactly);
            // every other theme letters them in its own menuText.
            {
                ImGuiCol idx[32];
                ImVec4 pc[32];
                const int n = th::popupColours(idx, pc, 32);
                CHECK(today ? n == 0 : n > 0);
                bool textIsMenuText = false;
                for (int k = 0; k < n; ++k) {
                    if (idx[k] == ImGuiCol_Text) {
                        textIsMenuText = opaque(pc[k]) == th::role(Role::MenuText);
                    }
                }
                CHECK(today || textIsMenuText);
            }
            for (const Pair& p : pairs) {
                double want = 0.0;
                if (today && frozenMatch(p, &want)) {
                    if (!(std::fabs(p.ratio - want) < 0.005)) {
                        std::printf("  %s: frozen pair %s %s on %s moved: %.3f, pinned %.3f\n",
                                    th::themeKey(id), p.scope.c_str(), p.ink.c_str(),
                                    p.surface.c_str(), p.ratio, want);
                    }
                    CHECK(std::fabs(p.ratio - want) < 0.005);
                    continue;
                }
                if (!(p.ratio >= 4.5)) {
                    std::printf("  %s: %s %s on %s is %.2f:1\n", th::themeKey(id),
                                p.scope.c_str(), p.ink.c_str(), p.surface.c_str(), p.ratio);
                    if (std::getenv("FOXSDR_THEME_PRINT_FROZEN") != nullptr && today) {
                        std::printf("FROZEN {\"%s\", \"%s\", \"%s\", %.4f},\n", p.scope.c_str(),
                                    p.ink.c_str(), p.surface.c_str(), p.ratio);
                    }
                }
                CHECK(p.ratio >= 4.5);
            }
            // DISABLED MUST STILL LOOK DISABLED: quieter than Text on the same
            // ground, by a margin an eye can see (not today's, which is frozen).
            if (!today) {
                for (const Pair& d : pairs) {
                    if (d.ink != "TextDisabled" || d.surface == "ChildBg") { continue; }
                    for (const Pair& t : pairs) {
                        if (t.scope == d.scope && t.ink == "Text" && t.surface == d.surface) {
                            if (!(t.ratio >= d.ratio * 1.2)) {
                                std::printf("  %s: %s disabled text %.2f is not quieter than text "
                                            "%.2f\n",
                                            th::themeKey(id), d.scope.c_str(), d.ratio, t.ratio);
                            }
                            CHECK(t.ratio >= d.ratio * 1.2);
                        }
                    }
                }
            }
        }
        th::setTheme(ThemeId::Today);
        th::applyTheme();
        ImGui::DestroyContext();
    }

    // --- the counter: a figure hotter than its glow, and switch words that read ---------
    //
    // NIGHT WATCH'S NEON SMEARED (repair round, 2026-09-25): the figure's
    // near-white core and its glow were both anchored to the digit role alone,
    // so outside today both became the digit colour and the figure was the
    // same colour as its halo. And Field Radio's UP / DN stencils were plate
    // ink - cream - on a bezel that in Field is the LCD's own light green.
    // These read the painters' own inks (gui/tuner_ink.hpp).
    {
        std::printf("  every glowing figure is hotter than its glow, and readable on its cell\n");
        using cascade::gui::TunerStyle;
        for (int i = 0; i < th::kThemeCount; ++i) {
            const ThemeId id = static_cast<ThemeId>(i);
            th::setTheme(id);
            for (const TunerStyle style : {TunerStyle::Nixie, TunerStyle::Neon, TunerStyle::Plain}) {
                const cascade::gui::TunerCellPaint p = cascade::gui::tunerCellPaint(style);
                const ImU32 figure = cascade::gui::tunerFigureInk(p, 255);
                const double onCell =
                    th::contrastRatio(figure, cascade::gui::tunerCellGround(p));
                if (!(onCell >= 4.5)) {
                    std::printf("  %s %s: the figure is %.2f:1 on its cell\n", th::themeKey(id),
                                cascade::gui::tunerStyleName(style), onCell);
                }
                CHECK(onCell >= 4.5);
                if (p.glowLayers == 0) { continue; }
                // Every glow the painters draw around it: the style's own ring
                // colour, and the Nixie's two reference shadows.
                const ImU32 glows[3] = {cascade::gui::tunerGlowInk(p, 255),
                                        th::tone(255, 106, 0, 255, th::ink::Digit),
                                        th::tone(255, 90, 0, 255, th::ink::Digit)};
                for (int g = 0; g < (style == TunerStyle::Nixie ? 3 : 1); ++g) {
                    const double fl = th::relativeLuminance(figure);
                    const double gl = th::relativeLuminance(glows[g]);
                    if (!(fl > gl + 0.01)) {
                        std::printf("  %s %s: the figure (L %.3f) is not hotter than glow %d "
                                    "(L %.3f)\n",
                                    th::themeKey(id), cascade::gui::tunerStyleName(style), fl, g, gl);
                    }
                    CHECK(fl > gl + 0.01);
                }
            }
            const double stencil = th::contrastRatio(cascade::gui::tunerStencilInk(),
                                                     cascade::gui::tunerBezelGround());
            if (!(stencil >= 4.5)) {
                std::printf("  %s: UP / DN are %.2f:1 on the bezel\n", th::themeKey(id), stencil);
            }
            CHECK(stencil >= 4.5);
        }
        // TODAY'S COUNTER, BIT FOR BIT, in all three faces.
        th::setTheme(ThemeId::Today);
        using cascade::gui::tunerCellPaint;
        CHECK(cascade::gui::tunerFigureInk(tunerCellPaint(TunerStyle::Neon), 255) ==
              IM_COL32(0xD6, 0xFE, 0xFF, 255));
        CHECK(cascade::gui::tunerFigureInk(tunerCellPaint(TunerStyle::Neon), 70) ==
              IM_COL32(0xD6, 0xFE, 0xFF, 70));
        CHECK(cascade::gui::tunerGlowInk(tunerCellPaint(TunerStyle::Neon), 90) ==
              IM_COL32(0x00, 0xD0, 0xFF, 90));
        CHECK(cascade::gui::tunerFigureInk(tunerCellPaint(TunerStyle::Nixie), 255) ==
              IM_COL32(0xFF, 0xB3, 0x47, 255));
        CHECK(cascade::gui::tunerGlowInk(tunerCellPaint(TunerStyle::Nixie), 70) ==
              IM_COL32(0xFF, 0x8A, 0x1F, 70));
        CHECK(cascade::gui::tunerFigureInk(tunerCellPaint(TunerStyle::Plain), 255) ==
              IM_COL32(0xFF, 0xFF, 0xFF, 255));
        CHECK(cascade::gui::tunerStencilInk() == IM_COL32(0xD8, 0xD3, 0xB8, 255));
        CHECK(cascade::gui::tunerBezelGround() == IM_COL32(0x0B, 0x0B, 0x09, 255));
        th::setTheme(ThemeId::ClassicXl);
        CHECK(cascade::gui::tunerFigureInk(tunerCellPaint(TunerStyle::Neon), 255) ==
              IM_COL32(0xD6, 0xFE, 0xFF, 255));
        th::setTheme(ThemeId::Today);
    }

    // --- legible(): the lit keys' legends --------------------------------------------------
    // The transmit and scope pages letter a lit key phosphor on dark brass, and
    // SPLIT / LATCH / PTT ivory on rust - pairs no preset was checked for, and
    // under Night Watch and Glass Cockpit ivory-on-"bad" stood at 1.8:1. They
    // are lettered through legible() now: today's inks exactly, every other
    // theme's readable.
    {
        std::printf("  a lit key's legend reads on its face, and today's is untouched\n");
        for (int i = 0; i < th::kThemeCount; ++i) {
            const ThemeId id = static_cast<ThemeId>(i);
            th::setTheme(id);
            const std::pair<ImU32, ImU32> keys[] = {{th::kPhosphor, th::kBrassDark},
                                                    {th::kIvory, th::kAlarm},
                                                    {th::kIvory, th::kAlarmHot}};
            for (const auto& k : keys) {
                const ImU32 ink = th::legible(k.first, k.second);
                if (th::isTodayPalette()) {
                    CHECK(ink == k.first);
                    continue;
                }
                const double r = th::contrastRatio(ink, k.second);
                if (!(r >= 4.5)) {
                    std::printf("  %s: a lit legend is %.2f:1 on its key\n", th::themeKey(id), r);
                }
                CHECK(r >= 4.5);
                // An ink that already reads is left exactly as it is.
                if (th::contrastRatio(k.first, k.second) >= 4.5) { CHECK(ink == k.first); }
            }
            // Alpha is the caller's.
            CHECK((th::legible(th::withAlpha(th::kIvory, 0.5f), th::kAlarm) >> IM_COL32_A_SHIFT) ==
                  (th::withAlpha(th::kIvory, 0.5f) >> IM_COL32_A_SHIFT));
        }
        th::setTheme(ThemeId::Today);
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
