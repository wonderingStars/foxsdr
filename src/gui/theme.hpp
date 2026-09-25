// theme.hpp - the application's palette and its Dear ImGui style, in one place.
//
// WHY THIS FILE EXISTS. Until it did, FoxSDR's entire theme was three lines in
// AppWindow::run - ImGui::StyleColorsDark(), WindowRounding = 0, and WindowBg
// alpha = 1 - and every other colour in the product was a literal at the point
// it was drawn. An audit counted 172 IM_COL32 literals, 25 inline ImVec4
// colours and about fifty file-local constexpr colour constants sitting in
// anonymous namespaces where nothing could reach them. The warning amber alone
// was written out seventeen times and had already drifted into three slightly
// different hues, which is worse than one wrong colour because it implies a
// distinction that does not exist.
//
// So: one palette, named by ROLE rather than by hue, and one applyTheme() that
// sets the whole ImGuiStyle from it. A control added next year inherits the
// bench instead of arriving in default blue.
//
// SIX THEMES (2026-09-25). The owner approved five alternative looks beside
// today's bench - Bench Classic XL, Night Watch, Glass Cockpit, Daylight Lab
// and Field Radio - and asked for all six in the application. That turned the
// palette from constants into STATE: the theme in force is chosen at run time
// and every surface asks for its colour when it draws, so switching theme
// repaints everything on the next frame.
//
// THE MODEL IS foxsdr-ui/1 (the interface package format the website's GUI
// builder writes). Its 49 colour ROLES are the vocabulary here - bg, frame,
// deck, panel, well, label, text, reading, digit, trace, stopBg, meterFace and
// the rest - together with its counter, controls, fonts and sizes fields, so a
// package from the builder can later be loaded into exactly this structure.
// A preset IS a foxsdr-ui/1 theme block.
//
// TODAY MUST NOT MOVE BY ONE PIXEL, and the mechanism below is how that is
// kept rather than hoped for. Every colour the application draws is written
// at its call site as TODAY'S value plus the role(s) it belongs to:
//
//     theme::tone(20, 21, 15, 255, ink::Well, ink::Panel)
//
// While the roles in force are today's, tone() returns the literal unchanged -
// bit for bit what 0.99.35 drew. Under any other theme the literal is placed
// on the line between its two anchors AS TODAY DRAWS THEM (how far along, and
// its alpha), and the same position is taken between the anchors of the theme
// in force. A shade that sat a quarter of the way from today's well to today's
// panel sits a quarter of the way from Night Watch's well to its panel. One
// anchor means "this IS that role, at this alpha".
//
// THE NAMED CONSTANTS (kBrassMid, kAmber, kIvory...) STAY, as live values. The
// thousand-odd call sites that already named a role by its old name keep
// compiling; each name is now a reference into the palette in force, mapped to
// the foxsdr-ui/1 role it always meant (see theme.cpp, legacyMap).
//
// ONE RULE WORTH KNOWING BEFORE USING THESE. The reference engraves its
// CAPTIONS dark-into-brass and puts its READINGS on glass - bright ink in a
// dark well. Dark-on-brass is about 2.3:1 contrast, which is fine for a label
// at rest and not acceptable for a number somebody is trying to read at arm's
// length. So: a caption may be engraved; a live figure must be on glass.
// tests/test_theme.cpp holds every preset's reading, text, label and digit to
// 4.5:1 against the surfaces they sit on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_THEME_HPP
#define CASCADE_GUI_THEME_HPP

#include <cstdint>
#include <string>

#include "imgui.h"

namespace cascade::gui::theme {

// --- the roles (foxsdr-ui/1, in the spec's own order) -------------------------
enum class Role : std::uint8_t {
    Bg, Frame, FrameText, Deck, DeckInk, Panel, PanelHead, Border, Well, Label, Text,
    Muted, Ctrl, CtrlBorder, CtrlText, ActiveBg, ActiveText, ActiveLine, Tag, TagText,
    Digit, DigitDim, DigitBg, Trace, Grid, WfLow, WfMid, WfHigh, WfTop, Accent,
    Reading, Ok, Off, Bad, StopBg, StopRing, StopText, MeterFace, MeterInk,
    MeterNeedle, Knob, KnobCap, Plate, PlateInk, MenuBg, MenuText, MenuBorder, MenuHi,
    Sel,
    Count
};
inline constexpr int kRoleCount = static_cast<int>(Role::Count);

// The role's key in a foxsdr-ui/1 document ("bg", "frameText", "wfLow"...).
const char* roleKey(Role role);
// Whether foxsdr-ui/1 lets the role be a two-stop vertical gradient (deck,
// panel, ctrl, activeBg, stopBg, digitBg, plate, knob). The rest are flat.
bool roleIsSurface(Role role);

// A role's value: two stops, equal for a flat role. A radial gradient in the
// mockups is its inner stop (top) and outer stop (bottom).
struct Surface {
    ImU32 top = 0;
    ImU32 bottom = 0;
};

// --- where a colour is anchored -----------------------------------------------
//
// An Ink names one end of the line a literal is placed on: a role (its flat
// value - the mean of a gradient's two stops), a gradient role's top or
// bottom stop, or black or white.
struct Ink {
    std::uint8_t role = 0;  // a Role, or kInkBlack / kInkWhite
    std::uint8_t part = 0;  // 0 flat, 1 top stop, 2 bottom stop
};
inline constexpr std::uint8_t kInkBlack = 250;
inline constexpr std::uint8_t kInkWhite = 251;

namespace ink {
#define FOX_THEME_INK(name, role)                                                      \
    inline constexpr Ink name{static_cast<std::uint8_t>(Role::role), 0};               \
    inline constexpr Ink name##Top{static_cast<std::uint8_t>(Role::role), 1};          \
    inline constexpr Ink name##Bot{static_cast<std::uint8_t>(Role::role), 2};
FOX_THEME_INK(Bg, Bg)
FOX_THEME_INK(Frame, Frame)
FOX_THEME_INK(FrameText, FrameText)
FOX_THEME_INK(Deck, Deck)
FOX_THEME_INK(DeckInk, DeckInk)
FOX_THEME_INK(Panel, Panel)
FOX_THEME_INK(PanelHead, PanelHead)
FOX_THEME_INK(Border, Border)
FOX_THEME_INK(Well, Well)
FOX_THEME_INK(Label, Label)
FOX_THEME_INK(Text, Text)
FOX_THEME_INK(Muted, Muted)
FOX_THEME_INK(Ctrl, Ctrl)
FOX_THEME_INK(CtrlBorder, CtrlBorder)
FOX_THEME_INK(CtrlText, CtrlText)
FOX_THEME_INK(ActiveBg, ActiveBg)
FOX_THEME_INK(ActiveText, ActiveText)
FOX_THEME_INK(ActiveLine, ActiveLine)
FOX_THEME_INK(Tag, Tag)
FOX_THEME_INK(TagText, TagText)
FOX_THEME_INK(Digit, Digit)
FOX_THEME_INK(DigitDim, DigitDim)
FOX_THEME_INK(DigitBg, DigitBg)
FOX_THEME_INK(Trace, Trace)
FOX_THEME_INK(Grid, Grid)
FOX_THEME_INK(WfLow, WfLow)
FOX_THEME_INK(WfMid, WfMid)
FOX_THEME_INK(WfHigh, WfHigh)
FOX_THEME_INK(WfTop, WfTop)
FOX_THEME_INK(Accent, Accent)
FOX_THEME_INK(Reading, Reading)
FOX_THEME_INK(Ok, Ok)
FOX_THEME_INK(Off, Off)
FOX_THEME_INK(Bad, Bad)
FOX_THEME_INK(StopBg, StopBg)
FOX_THEME_INK(StopRing, StopRing)
FOX_THEME_INK(StopText, StopText)
FOX_THEME_INK(MeterFace, MeterFace)
FOX_THEME_INK(MeterInk, MeterInk)
FOX_THEME_INK(MeterNeedle, MeterNeedle)
FOX_THEME_INK(Knob, Knob)
FOX_THEME_INK(KnobCap, KnobCap)
FOX_THEME_INK(Plate, Plate)
FOX_THEME_INK(PlateInk, PlateInk)
FOX_THEME_INK(MenuBg, MenuBg)
FOX_THEME_INK(MenuText, MenuText)
FOX_THEME_INK(MenuBorder, MenuBorder)
FOX_THEME_INK(MenuHi, MenuHi)
FOX_THEME_INK(Sel, Sel)
#undef FOX_THEME_INK
inline constexpr Ink Black{kInkBlack, 0};
inline constexpr Ink White{kInkWhite, 0};
}  // namespace ink

// --- the six presets -----------------------------------------------------------
enum class ThemeId : std::uint8_t { Today = 0, ClassicXl, Night, Glass, Daylight, Field };
inline constexpr int kThemeCount = 6;

// The foxsdr-ui/1 fields besides the colours. Strings are the spec's own
// vocabulary (fonts: bench-serif | mono | sans | condensed | stencil | nixie;
// counter.face: nixie | neon | plain | ...; keys raised|flat|engraved|pill...).
struct PresetFonts {
    const char* ui = "bench-serif";
    const char* caption = "bench-serif";
    const char* reading = "mono";
    const char* counter = "nixie";
};
struct PresetCounter {
    const char* face = "nixie";
    float scale = 1.0f;     // 1.0 - 3.0; the application draws 1x and 2x
    bool switches = true;   // the UP/DN tuner switches under the digits
    float glow = 0.7f;      // 0 - 1
    // The figure's colour (foxsdr-ui/1 counter.digit; it wins over colors.digit
    // when a package gives both, and either alone sets both).
    ImU32 digit = 0;
};
struct PresetControls {
    const char* keys = "raised";
    const char* stop = "round";
    const char* knob = "bakelite";
    const char* meter = "needle";
    const char* lamp = "round";
    float radius = 3.0f;    // px, 0 - 12
};
struct PresetSizes {
    float ui = 1.0f;        // 0.8 - 1.5
    float readings = 1.0f;  // 1.0 - 3.0: every live figure, not just the counter
};

struct Preset {
    const char* key = "today";   // the config and foxsdr-ui/1 "base" value
    const char* name = "";       // the picker's label (a translation key)
    Surface colors[kRoleCount];
    PresetFonts fonts;
    PresetCounter counter;
    PresetControls controls;
    PresetSizes sizes;
    // Application extras the format does not name: the colour a lit edge and a
    // drop shadow are drawn in. White and black on every bench but Night
    // Watch, whose highlights are red so they cannot cost the eye its dark
    // adaptation.
    ImU32 sheen = IM_COL32(255, 255, 255, 255);
    ImU32 shadow = IM_COL32(0, 0, 0, 255);
};

const Preset& preset(ThemeId id);
const char* themeKey(ThemeId id);
// The picker's label, as an English translation key (draw it through tr()).
const char* themeLabel(ThemeId id);
// THE NAME IS WHAT THE CONFIG FILE CARRIES: an unknown, empty or wrong-case
// value is Today, never a refusal - the file is user-editable and a typo must
// leave the application looking like itself.
ThemeId themeFromKey(const std::string& key);

// --- the theme in force ----------------------------------------------------------
//
// setTheme() swaps the palette, the named constants and the metrics. It does
// NOT touch ImGui's style - call applyTheme() after it (between frames) - so a
// test can drive the palette with no ImGui context.
void setTheme(ThemeId id);
ThemeId currentTheme();
const Preset& current();
// Bumped on every setTheme, so a cache built from colours (the waterfall's
// colormap) knows when to rebuild.
std::uint32_t generation();

ImU32 role(Role r);        // the flat value (the mean of a gradient)
ImU32 roleTop(Role r);
ImU32 roleBottom(Role r);
ImU32 inkValue(const Preset& p, Ink i);

// TODAY'S colour, re-placed in the theme in force. See the header. The alpha
// always comes from the literal.
ImU32 tone(int r, int g, int b, int a, Ink x, Ink y);
ImU32 tone(int r, int g, int b, int a, Ink x);
// The same for 0xRRGGBB colour tokens (the tuner plate's reference hexes).
ImU32 toneHex(unsigned rgb, int a, Ink x, Ink y);
ImU32 toneHex(unsigned rgb, int a, Ink x);

// A NAMED tone: today's value and its anchors, kept as a constant and resolved
// against the theme in force whenever it is read. It converts to ImU32, so a
// file-local `constexpr ImU32 kFoo = IM_COL32(...)` becomes
// `constexpr theme::Tone kFoo{r, g, b, a, ink::X, ink::Y}` with every use
// left as it was.
struct Tone {
    unsigned char r = 0, g = 0, b = 0, a = 255;
    Ink x{};
    Ink y{};
    constexpr Tone(unsigned char r_, unsigned char g_, unsigned char b_, unsigned char a_,
                   Ink x_, Ink y_)
        : r(r_), g(g_), b(b_), a(a_), x(x_), y(y_) {}
    constexpr Tone(unsigned char r_, unsigned char g_, unsigned char b_, unsigned char a_,
                   Ink x_)
        : r(r_), g(g_), b(b_), a(a_), x(x_), y(x_) {}
    operator ImU32() const { return tone(r, g, b, a, x, y); }
    ImU32 withA(int alpha) const { return tone(r, g, b, alpha, x, y); }
};

// A lit edge and a drop shadow at `alpha` (0-255): white and black on today's
// bench, the preset's sheen and shadow elsewhere.
ImU32 sheen(int alpha);
ImU32 shadow(int alpha);
// The same for a shadow or a lit edge that today draws in a TINTED colour (a
// warm near-black at 45%, say): the literal on today's bench, the preset's
// shadow / sheen at the literal's alpha everywhere else.
ImU32 shadowOf(int r, int g, int b, int a);
ImU32 sheenOf(int r, int g, int b, int a);

// TODAY'S colour where today's palette is in force; otherwise a fixed blend,
// `t` of the way from X to Y in the theme in force. For a state today draws
// in a colour no pair of its roles brackets (a key's hover face, say), where
// the placement tone() would compute is not the one the state means.
ImU32 toneMix(int r, int g, int b, int a, Ink x, Ink y, float t);

// True while the palette in force is today's (Today or Bench Classic XL).
bool isTodayPalette();

// --- the size of every live figure (foxsdr-ui/1 sizes.readings) ----------------
// 1.0 is today's size; "Enlarge every reading" draws the main screen's live
// figures - the status cards' values, the spectrum's peak and span readings,
// the volume setting and the meters' value lines - this much larger. Set by the
// application once a frame (clamped to 1.0 - 3.0); the drawing sites multiply
// their reading size by it.
void setReadingsScale(float scale);
float readingsScale();

// --- the named constants (live) ---------------------------------------------
//
// Each is the role it always meant, in the theme in force (theme.cpp,
// legacyMap, says which). Under Today every one is exactly its old value.
namespace detail {
struct Legacy {
    ImU32 brassBright, brassMid, brassDark, brassTint, brassShade;
    ImU32 enamel, enamelDark, well, voidInk;
    ImU32 engraved, inkMuted, inkFaint;
    ImU32 ivory, cream;
    ImU32 amber, amberDim, gold;
    ImU32 phosphor, phosphorDim;
    ImU32 alarm, alarmHot;
};
struct Metrics {
    float keyRounding;
    float panelRounding;
};
extern Legacy gLegacy;
extern Metrics gMetrics;
}  // namespace detail

// The metal. Panels, keys, rails, plates - anything a hand operates.
inline const ImU32& kBrassBright = detail::gLegacy.brassBright;  // today #8B8069
inline const ImU32& kBrassMid = detail::gLegacy.brassMid;        // today #6E6552
inline const ImU32& kBrassDark = detail::gLegacy.brassDark;      // today #4A4234
inline const ImU32& kBrassTint = detail::gLegacy.brassTint;      // today #9C9078
inline const ImU32& kBrassShade = detail::gLegacy.brassShade;    // today #7D7360

// The enamel. Wells, recesses and the dark ground a reading sits on.
inline const ImU32& kEnamel = detail::gLegacy.enamel;            // today #2A251C
inline const ImU32& kEnamelDark = detail::gLegacy.enamelDark;    // today #1F1B14
inline const ImU32& kWell = detail::gLegacy.well;                // today #14110C
inline const ImU32& kVoid = detail::gLegacy.voidInk;             // today #0D0B07

// The ink. Engraved legends, cut into the metal.
inline const ImU32& kEngraved = detail::gLegacy.engraved;        // today #3B3529
inline const ImU32& kInkMuted = detail::gLegacy.inkMuted;        // today #9C9078
inline const ImU32& kInkFaint = detail::gLegacy.inkFaint;        // today #7D7360

// The lettering. Ivory on metal, for anything a hand operates.
inline const ImU32& kIvory = detail::gLegacy.ivory;              // today #EFE7D2
inline const ImU32& kCream = detail::gLegacy.cream;              // today #D8CFB4

// The readings. Amber is a NUMBER; nothing else may use it.
inline const ImU32& kAmber = detail::gLegacy.amber;              // today #F0A840
inline const ImU32& kAmberDim = detail::gLegacy.amberDim;        // today #8A5A2A
inline const ImU32& kGold = detail::gLegacy.gold;                // today #D9B23C

// The display. Phosphor, for what the radio actually received.
inline const ImU32& kPhosphor = detail::gLegacy.phosphor;        // today #8FD9A0
inline const ImU32& kPhosphorDim = detail::gLegacy.phosphorDim;  // today #5F8A55

// Trouble. Rust for a fault, and it is deliberately NOT the amber a reading
// uses - a number and a warning must never be the same colour.
inline const ImU32& kAlarm = detail::gLegacy.alarm;              // today #B8552F
inline const ImU32& kAlarmHot = detail::gLegacy.alarmHot;        // today #E07A4E

// --- metrics -----------------------------------------------------------------
// A machined key has a small radius, not a round one. The two radii follow the
// preset's controls.radius (today 3: a key 2, a panel 3); the hairline and
// the rail do not change with the theme. Logical pixels at scale 1.
inline const float& kKeyRounding = detail::gMetrics.keyRounding;
inline const float& kPanelRounding = detail::gMetrics.panelRounding;
inline constexpr float kHairline = 1.0f;
inline constexpr float kRailThickness = 2.0f;

// --- conversions -------------------------------------------------------------
// ImGuiStyle wants ImVec4; ImDrawList wants ImU32. One converter so a colour
// cannot be transcribed differently in the two places it is used.
ImVec4 vec(ImU32 packed);
ImU32 withAlpha(ImU32 packed, float alpha);

// --- readability -----------------------------------------------------------------
// WCAG 2 contrast ratio between two opaque colours (alpha ignored), 1..21.
double contrastRatio(ImU32 a, ImU32 b);

// --- semantics ---------------------------------------------------------------
//
// What a message MEANS, for the widget layer. These exist because the three
// states below were previously written as raw ImVec4 literals at every site:
// the warning amber appeared SEVENTEEN times, with two more near-duplicates
// that meant the same thing in a slightly different hue, and there were two
// different "good" greens and two different reds. Three subtly different
// warning colours is worse than one wrong colour, because it implies a
// distinction the product does not actually make.
ImVec4 warning();  // amber: something the user should look at
ImVec4 good();     // phosphor: working, connected, receiving
ImVec4 bad();      // rust: failed, refused, stopped

// --- the waterfall's colormap stops --------------------------------------------
// The colormap's five anchor stops (at 0, 0.20, 0.45, 0.75 and 1.0) in the
// theme in force: today's exact phosphor table, or wfLow / a derived shade /
// wfMid / wfHigh / wfTop. The waterfall rebuilds its table from these when
// generation() moves.
struct WfStops {
    ImU32 c[5];
};
WfStops waterfallStops();

// --- the typeface pair ------------------------------------------------------------
// The bundled pair the preset's "ui" font maps to: "" for the application's
// own choice (Georgia where the machine has it - today's bench), "Saira
// Condensed" or "Noto Sans Condensed". Figures keep Nova Mono in every theme.
const char* preferredFontPair(ThemeId id);

// --- the style ---------------------------------------------------------------
//
// Sets every ImGuiCol_ and the style vars from the palette in force. Call it
// after ImGui::CreateContext and before the first frame, and again (between
// frames) after every setTheme.
//
// TWO OVERRIDES ARE LOAD-BEARING AND MUST SURVIVE ANY EDIT HERE. This
// application runs with multi-viewport enabled and WITHOUT
// ConfigViewportsNoDecoration, so its torn-off windows - map pages, decoded
// images, the target details window, decoder output - are real operating
// system windows. WindowBg's alpha must stay 1.0 or those render translucent,
// and WindowRounding must stay 0 or the OS frame and the ImGui corner disagree
// visibly. Both were added deliberately and neither is cosmetic.
void applyTheme();

}  // namespace cascade::gui::theme

#endif  // CASCADE_GUI_THEME_HPP
