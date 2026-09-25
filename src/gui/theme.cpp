// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/theme.hpp"

#include <algorithm>
#include <cmath>

#include "core/i18n.hpp"

namespace cascade::gui::theme {

namespace {

constexpr ImU32 rgb(unsigned hex) {
    return IM_COL32((hex >> 16) & 0xFFu, (hex >> 8) & 0xFFu, hex & 0xFFu, 0xFFu);
}
constexpr Surface flat(unsigned hex) { return Surface{rgb(hex), rgb(hex)}; }
constexpr Surface grad(unsigned top, unsigned bottom) { return Surface{rgb(top), rgb(bottom)}; }

// --- THE SIX PRESETS -----------------------------------------------------------
//
// TODAY IS MEASURED FROM THE APPLICATION, NOT FROM THE MOCKUP. The mockups'
// P.today was drawn by eye to look like the bench; where it and 0.99.35 differ
// (the deck's gradient, muted ink, the alarm rust, the menu, the Nixie digit),
// the value here is what the application actually draws, because these values
// are the reference every other theme's shades are placed against (tone()).
// The five others are the mockups' palettes (Receiver.dc.html, renderVals),
// transcribed: a CSS linear-gradient(180deg, A, B) is {A, B}, a radial one its
// inner and outer stops, and an rgba() grid or selection tint keeps its RGB -
// the alpha is the drawing site's, as it always was.
Preset makeToday() {
    Preset p;
    p.key = "today";
    p.name = FOX_TR_NOOP("Today's bench");
    Surface* c = p.colors;
    c[int(Role::Bg)] = flat(0x1F1B14);
    c[int(Role::Frame)] = flat(0x7D7360);
    c[int(Role::FrameText)] = flat(0x3B3529);
    c[int(Role::Deck)] = grad(0x7D7360, 0x6E6552);
    c[int(Role::DeckInk)] = flat(0x3B3529);
    c[int(Role::Panel)] = flat(0x2A251C);
    c[int(Role::PanelHead)] = flat(0x1F1B14);
    c[int(Role::Border)] = flat(0x4A4234);
    c[int(Role::Well)] = flat(0x0D0B07);
    c[int(Role::Label)] = flat(0xEFE7D2);
    c[int(Role::Text)] = flat(0xD8CFB4);
    c[int(Role::Muted)] = flat(0x9C9078);
    c[int(Role::Ctrl)] = grad(0x8B8069, 0x6E6552);
    c[int(Role::CtrlBorder)] = flat(0x4A4234);
    c[int(Role::CtrlText)] = flat(0xEFE7D2);
    c[int(Role::ActiveBg)] = grad(0xD8CFB4, 0x8B8069);
    c[int(Role::ActiveText)] = flat(0x2A251C);
    c[int(Role::ActiveLine)] = flat(0x8FD9A0);
    c[int(Role::Tag)] = flat(0x14110C);
    c[int(Role::TagText)] = flat(0xF0A840);
    c[int(Role::Digit)] = flat(0xFFB347);
    c[int(Role::DigitDim)] = flat(0x5A3A18);
    c[int(Role::DigitBg)] = grad(0x2A1D10, 0x050403);
    c[int(Role::Trace)] = flat(0x8FD9A0);
    c[int(Role::Grid)] = flat(0xFFFFFF);
    c[int(Role::WfLow)] = flat(0x06140A);
    c[int(Role::WfMid)] = flat(0x1E8C3C);
    c[int(Role::WfHigh)] = flat(0x96C83C);
    c[int(Role::WfTop)] = flat(0xF0EBB4);
    c[int(Role::Accent)] = flat(0xF0A840);
    c[int(Role::Reading)] = flat(0xF0A840);
    c[int(Role::Ok)] = flat(0x8FD9A0);
    c[int(Role::Off)] = flat(0x3B3529);
    c[int(Role::Bad)] = flat(0xB8552F);
    c[int(Role::StopBg)] = grad(0xE07A4E, 0xB8552F);
    c[int(Role::StopRing)] = flat(0x6E6552);
    c[int(Role::StopText)] = flat(0x1F1B14);
    // foxsdr-ui/1 keeps meterFace flat; the face's own shading to #D8CFB4 at
    // its foot is the meter's drawing (a tone), not the role.
    c[int(Role::MeterFace)] = flat(0xF3ECD6);
    c[int(Role::MeterInk)] = flat(0x3B3529);
    c[int(Role::MeterNeedle)] = flat(0xB8552F);
    c[int(Role::Knob)] = grad(0x2F2A21, 0x0D0B07);
    c[int(Role::KnobCap)] = flat(0xEFE7D2);
    c[int(Role::Plate)] = grad(0x5D6247, 0x3E422F);
    c[int(Role::PlateInk)] = flat(0xB9B99F);
    c[int(Role::MenuBg)] = flat(0x2A251C);
    c[int(Role::MenuText)] = flat(0xEFE7D2);
    c[int(Role::MenuBorder)] = flat(0x8B8069);
    c[int(Role::MenuHi)] = flat(0x6E6552);
    c[int(Role::Sel)] = flat(0xF0A840);
    p.fonts = PresetFonts{"bench-serif", "bench-serif", "bench-serif", "nixie"};
    p.counter = PresetCounter{"nixie", 1.0f, true, 0.75f};
    p.controls = PresetControls{"raised", "round", "bakelite", "needle", "round", 3.0f};
    p.sizes = PresetSizes{1.0f, 1.0f};
    return p;
}

Preset makeClassicXl() {
    // TODAY'S BENCH, READ FROM ACROSS THE ROOM: the same palette, the counter's
    // figures doubled and its tuner switches put away, and every other live
    // figure 1.4 times its size (foxsdr-ui/1's own definition of the preset).
    Preset p = makeToday();
    p.key = "classic-xl";
    p.name = FOX_TR_NOOP("Bench Classic XL");
    p.counter = PresetCounter{"nixie", 2.0f, false, 0.75f};
    p.sizes = PresetSizes{1.0f, 1.4f};
    return p;
}

Preset makeNight() {
    Preset p;
    p.key = "night";
    p.name = FOX_TR_NOOP("Night Watch");
    Surface* c = p.colors;
    c[int(Role::Bg)] = flat(0x070303);
    c[int(Role::Frame)] = flat(0x140606);
    c[int(Role::FrameText)] = flat(0xE87766);
    c[int(Role::Deck)] = grad(0x1C0A09, 0x120606);
    c[int(Role::DeckInk)] = flat(0xE87766);
    c[int(Role::Panel)] = flat(0x100505);
    c[int(Role::PanelHead)] = flat(0x170808);
    c[int(Role::Border)] = flat(0x3A1410);
    c[int(Role::Well)] = flat(0x040101);
    c[int(Role::Label)] = flat(0xFF8A72);
    c[int(Role::Text)] = flat(0xE87766);
    c[int(Role::Muted)] = flat(0xCF5D4D);
    c[int(Role::Ctrl)] = flat(0x1F0B09);
    c[int(Role::CtrlBorder)] = flat(0x4A1A14);
    c[int(Role::CtrlText)] = flat(0xF08A76);
    c[int(Role::ActiveBg)] = flat(0x5A1A12);
    c[int(Role::ActiveText)] = flat(0xFFD2C6);
    c[int(Role::ActiveLine)] = flat(0xFF5A3C);
    c[int(Role::Tag)] = flat(0x2A0C09);
    c[int(Role::TagText)] = flat(0xFF8A70);
    c[int(Role::Digit)] = flat(0xFF5A3C);
    c[int(Role::DigitDim)] = flat(0x3A100A);
    c[int(Role::DigitBg)] = flat(0x0A0202);
    c[int(Role::Trace)] = flat(0xFF6A4C);
    c[int(Role::Grid)] = flat(0xFF5A46);
    c[int(Role::WfLow)] = flat(0x140303);
    c[int(Role::WfMid)] = flat(0x4A0C07);
    c[int(Role::WfHigh)] = flat(0xC0281A);
    c[int(Role::WfTop)] = flat(0xFF9A70);
    c[int(Role::Accent)] = flat(0xFF6A4C);
    c[int(Role::Reading)] = flat(0xFF8A70);
    c[int(Role::Ok)] = flat(0xFF7A62);
    c[int(Role::Off)] = flat(0x3A1410);
    c[int(Role::Bad)] = flat(0xFFE1D8);
    c[int(Role::StopBg)] = grad(0x8A2014, 0x3A0806);
    c[int(Role::StopRing)] = flat(0x5A1A12);
    c[int(Role::StopText)] = flat(0xFFD2C6);
    c[int(Role::MeterFace)] = flat(0x140606);
    c[int(Role::MeterInk)] = flat(0xE87766);
    c[int(Role::MeterNeedle)] = flat(0xFF5A3C);
    c[int(Role::Knob)] = grad(0x3A1410, 0x0A0202);
    c[int(Role::KnobCap)] = flat(0xFF7A62);
    c[int(Role::Plate)] = flat(0x0C0303);
    c[int(Role::PlateInk)] = flat(0xCF5D4D);
    c[int(Role::MenuBg)] = flat(0x1C0A09);
    c[int(Role::MenuText)] = flat(0xF08A76);
    c[int(Role::MenuBorder)] = flat(0x5A1A12);
    c[int(Role::MenuHi)] = flat(0x3A1410);
    c[int(Role::Sel)] = flat(0xFF5A3C);
    p.fonts = PresetFonts{"mono", "mono", "mono", "mono"};
    p.counter = PresetCounter{"neon", 1.0f, true, 0.7f};
    p.controls = PresetControls{"flat", "round", "bakelite", "needle", "round", 2.0f};
    p.sizes = PresetSizes{1.0f, 1.0f};
    // A white highlight is exactly the light a night-adapted eye cannot have.
    p.sheen = rgb(0xFF8A72);
    return p;
}

Preset makeGlass() {
    Preset p;
    p.key = "glass";
    p.name = FOX_TR_NOOP("Glass Cockpit");
    Surface* c = p.colors;
    c[int(Role::Bg)] = flat(0x070B0F);
    c[int(Role::Frame)] = flat(0x0D141A);
    c[int(Role::FrameText)] = flat(0x9FC3D9);
    c[int(Role::Deck)] = grad(0x131E27, 0x0E161D);
    c[int(Role::DeckInk)] = flat(0x9FC3D9);
    c[int(Role::Panel)] = flat(0x0E161D);
    c[int(Role::PanelHead)] = flat(0x111C25);
    c[int(Role::Border)] = flat(0x223340);
    c[int(Role::Well)] = flat(0x05090C);
    c[int(Role::Label)] = flat(0xA9CDE3);
    c[int(Role::Text)] = flat(0xDDE9F1);
    c[int(Role::Muted)] = flat(0x7D97A9);
    c[int(Role::Ctrl)] = flat(0x16222C);
    c[int(Role::CtrlBorder)] = flat(0x28404F);
    c[int(Role::CtrlText)] = flat(0xCFE0EC);
    c[int(Role::ActiveBg)] = flat(0x0E6A85);
    c[int(Role::ActiveText)] = flat(0xF2FCFF);
    c[int(Role::ActiveLine)] = flat(0x7DF3FF);
    c[int(Role::Tag)] = flat(0x0A2A36);
    c[int(Role::TagText)] = flat(0x7DF3FF);
    c[int(Role::Digit)] = flat(0x7DF3FF);
    c[int(Role::DigitDim)] = flat(0x1C3A44);
    c[int(Role::DigitBg)] = flat(0x05090C);
    c[int(Role::Trace)] = flat(0x35E0A1);
    c[int(Role::Grid)] = flat(0x78B4D2);
    c[int(Role::WfLow)] = flat(0x0A1433);
    c[int(Role::WfMid)] = flat(0x1D4F85);
    c[int(Role::WfHigh)] = flat(0x25B39A);
    c[int(Role::WfTop)] = flat(0xF2E35C);
    c[int(Role::Accent)] = flat(0x7DF3FF);
    c[int(Role::Reading)] = flat(0xFFB547);
    c[int(Role::Ok)] = flat(0x35E0A1);
    c[int(Role::Off)] = flat(0x223340);
    c[int(Role::Bad)] = flat(0xFF5C5C);
    c[int(Role::StopBg)] = grad(0x3A1418, 0x22090C);
    c[int(Role::StopRing)] = flat(0xFF5C5C);
    c[int(Role::StopText)] = flat(0xFFD6D6);
    c[int(Role::MeterFace)] = flat(0x0B1218);
    c[int(Role::MeterInk)] = flat(0x9FC3D9);
    c[int(Role::MeterNeedle)] = flat(0x7DF3FF);
    c[int(Role::Knob)] = grad(0x2A3A47, 0x0B1218);
    c[int(Role::KnobCap)] = flat(0x7DF3FF);
    c[int(Role::Plate)] = flat(0x0A1117);
    c[int(Role::PlateInk)] = flat(0x7D97A9);
    c[int(Role::MenuBg)] = flat(0x131E27);
    c[int(Role::MenuText)] = flat(0xDDE9F1);
    c[int(Role::MenuBorder)] = flat(0x28404F);
    c[int(Role::MenuHi)] = flat(0x1D2F3C);
    c[int(Role::Sel)] = flat(0x7DF3FF);
    p.fonts = PresetFonts{"sans", "condensed", "sans", "mono"};
    // foxsdr-ui/1's "vfd" face; drawn plain until the application has one.
    p.counter = PresetCounter{"vfd", 1.0f, true, 0.45f};
    p.controls = PresetControls{"flat", "round", "chrome", "needle", "round", 8.0f};
    p.sizes = PresetSizes{1.0f, 1.0f};
    return p;
}

Preset makeDaylight() {
    Preset p;
    p.key = "daylight";
    p.name = FOX_TR_NOOP("Daylight Lab");
    Surface* c = p.colors;
    c[int(Role::Bg)] = flat(0xCFD5DC);
    c[int(Role::Frame)] = flat(0xE8ECF0);
    c[int(Role::FrameText)] = flat(0x1F2A37);
    c[int(Role::Deck)] = flat(0xF5F7F9);
    c[int(Role::DeckInk)] = flat(0x374151);
    c[int(Role::Panel)] = flat(0xFFFFFF);
    c[int(Role::PanelHead)] = flat(0xEEF1F5);
    c[int(Role::Border)] = flat(0xC3CAD3);
    c[int(Role::Well)] = flat(0xFFFFFF);
    c[int(Role::Label)] = flat(0x111827);
    c[int(Role::Text)] = flat(0x1F2A37);
    c[int(Role::Muted)] = flat(0x4B5563);
    c[int(Role::Ctrl)] = flat(0xEEF1F5);
    c[int(Role::CtrlBorder)] = flat(0xB9C1CC);
    c[int(Role::CtrlText)] = flat(0x1F2A37);
    c[int(Role::ActiveBg)] = flat(0x1D4ED8);
    c[int(Role::ActiveText)] = flat(0xFFFFFF);
    c[int(Role::ActiveLine)] = flat(0x1D4ED8);
    c[int(Role::Tag)] = flat(0xE0E7FF);
    c[int(Role::TagText)] = flat(0x1E3A8A);
    c[int(Role::Digit)] = flat(0x0F172A);
    c[int(Role::DigitDim)] = flat(0xC3CAD3);
    c[int(Role::DigitBg)] = flat(0xFFFFFF);
    c[int(Role::Trace)] = flat(0x1D4ED8);
    c[int(Role::Grid)] = flat(0x0F172A);
    c[int(Role::WfLow)] = flat(0xF4F7FB);
    c[int(Role::WfMid)] = flat(0xBCD4F6);
    c[int(Role::WfHigh)] = flat(0x3B6FD8);
    c[int(Role::WfTop)] = flat(0x0B1F5C);
    c[int(Role::Accent)] = flat(0x1D4ED8);
    c[int(Role::Reading)] = flat(0x1E3A8A);
    c[int(Role::Ok)] = flat(0x15803D);
    c[int(Role::Off)] = flat(0xD1D5DB);
    c[int(Role::Bad)] = flat(0xB91C1C);
    c[int(Role::StopBg)] = flat(0xB91C1C);
    c[int(Role::StopRing)] = flat(0x7F1D1D);
    c[int(Role::StopText)] = flat(0xFFFFFF);
    c[int(Role::MeterFace)] = flat(0xFFFFFF);
    c[int(Role::MeterInk)] = flat(0x1F2A37);
    c[int(Role::MeterNeedle)] = flat(0xB91C1C);
    c[int(Role::Knob)] = grad(0xFFFFFF, 0xB9C1CC);
    c[int(Role::KnobCap)] = flat(0x1D4ED8);
    c[int(Role::Plate)] = flat(0xFFFFFF);
    c[int(Role::PlateInk)] = flat(0x4B5563);
    c[int(Role::MenuBg)] = flat(0xFFFFFF);
    c[int(Role::MenuText)] = flat(0x111827);
    c[int(Role::MenuBorder)] = flat(0xC3CAD3);
    c[int(Role::MenuHi)] = flat(0xEEF1F5);
    c[int(Role::Sel)] = flat(0x1D4ED8);
    p.fonts = PresetFonts{"sans", "sans", "sans", "sans"};
    p.counter = PresetCounter{"plain", 1.0f, true, 0.0f};
    p.controls = PresetControls{"flat", "round", "chrome", "needle", "round", 6.0f};
    p.sizes = PresetSizes{1.0f, 1.0f};
    return p;
}

Preset makeField() {
    Preset p;
    p.key = "field";
    p.name = FOX_TR_NOOP("Field Radio");
    Surface* c = p.colors;
    c[int(Role::Bg)] = flat(0x23271A);
    c[int(Role::Frame)] = flat(0x3A4128);
    c[int(Role::FrameText)] = flat(0xF1EEDD);
    c[int(Role::Deck)] = grad(0x56603C, 0x454D30);
    c[int(Role::DeckInk)] = flat(0xF1EEDD);
    c[int(Role::Panel)] = flat(0x343B24);
    c[int(Role::PanelHead)] = flat(0x2C321E);
    c[int(Role::Border)] = flat(0x5D6641);
    c[int(Role::Well)] = flat(0x0F140B);
    c[int(Role::Label)] = flat(0xF1EEDD);
    c[int(Role::Text)] = flat(0xE3DFC9);
    c[int(Role::Muted)] = flat(0xB8B797);
    c[int(Role::Ctrl)] = grad(0x646E45, 0x525A37);
    c[int(Role::CtrlBorder)] = flat(0x2C321E);
    c[int(Role::CtrlText)] = flat(0xF1EEDD);
    c[int(Role::ActiveBg)] = flat(0xD6BF5E);
    c[int(Role::ActiveText)] = flat(0x1C1F12);
    c[int(Role::ActiveLine)] = flat(0xD6BF5E);
    c[int(Role::Tag)] = flat(0x1C2012);
    c[int(Role::TagText)] = flat(0xD6BF5E);
    c[int(Role::Digit)] = flat(0x1A2010);
    c[int(Role::DigitDim)] = flat(0x8F9D58);
    c[int(Role::DigitBg)] = grad(0xBCCB7C, 0xA4B465);
    c[int(Role::Trace)] = flat(0xB6E36A);
    c[int(Role::Grid)] = flat(0xB6E36A);
    c[int(Role::WfLow)] = flat(0x0F140B);
    c[int(Role::WfMid)] = flat(0x2C4414);
    c[int(Role::WfHigh)] = flat(0x79A52C);
    c[int(Role::WfTop)] = flat(0xE0EE7A);
    c[int(Role::Accent)] = flat(0xD6BF5E);
    c[int(Role::Reading)] = flat(0xD6BF5E);
    c[int(Role::Ok)] = flat(0x9FE870);
    c[int(Role::Off)] = flat(0x2C321E);
    c[int(Role::Bad)] = flat(0xE0603F);
    c[int(Role::StopBg)] = grad(0xD9563A, 0x93301C);
    c[int(Role::StopRing)] = flat(0x1C2012);
    c[int(Role::StopText)] = flat(0xFFF4E6);
    c[int(Role::MeterFace)] = flat(0xE9E4CF);
    c[int(Role::MeterInk)] = flat(0x1C1F12);
    c[int(Role::MeterNeedle)] = flat(0x93301C);
    c[int(Role::Knob)] = grad(0x5A5A4A, 0x1A1A12);
    c[int(Role::KnobCap)] = flat(0xF1EEDD);
    c[int(Role::Plate)] = flat(0x1C2012);
    c[int(Role::PlateInk)] = flat(0xB8B797);
    c[int(Role::MenuBg)] = flat(0xE9E4CF);
    c[int(Role::MenuText)] = flat(0x1C1F12);
    c[int(Role::MenuBorder)] = flat(0x2C321E);
    c[int(Role::MenuHi)] = flat(0xD0CBB2);
    c[int(Role::Sel)] = flat(0xD6BF5E);
    p.fonts = PresetFonts{"mono", "stencil", "mono", "mono"};
    // foxsdr-ui/1 defines an "lcd" face; the application draws nixie, neon and
    // plain today and, as the format says a reader must, draws plain for it.
    p.counter = PresetCounter{"lcd", 1.0f, true, 0.0f};
    p.controls = PresetControls{"raised", "round", "bakelite", "needle", "round", 2.0f};
    p.sizes = PresetSizes{1.0f, 1.0f};
    return p;
}

// counter.digit is foxsdr-ui/1's figure colour: "either alone sets both" with
// colors.digit, so every preset carries the one value in both places.
Preset withCounterDigit(Preset p) {
    p.counter.digit = p.colors[static_cast<int>(Role::Digit)].top;
    return p;
}

const Preset* presets() {
    static const Preset table[kThemeCount] = {
        withCounterDigit(makeToday()), withCounterDigit(makeClassicXl()),
        withCounterDigit(makeNight()), withCounterDigit(makeGlass()),
        withCounterDigit(makeDaylight()), withCounterDigit(makeField())};
    return table;
}

// The palette in force. Starts as Today, so a test or a tool that never calls
// setTheme sees the bench exactly as it has always been.
ThemeId gId = ThemeId::Today;
const Preset* gCur = nullptr;
std::uint32_t gGeneration = 1;
// Whether the palette in force is today's - worked out once per setTheme,
// because shadowOf/sheenOf/applyTheme ask on every draw.
bool gIsToday = true;

bool sameAsToday(const Preset& a) {
    const Preset& b = presets()[0];
    if (&a == &b) { return true; }
    for (int i = 0; i < kRoleCount; ++i) {
        if (a.colors[i].top != b.colors[i].top || a.colors[i].bottom != b.colors[i].bottom) {
            return false;
        }
    }
    return a.sheen == b.sheen && a.shadow == b.shadow;
}

const Preset& cur() {
    if (gCur == nullptr) { gCur = &presets()[0]; }
    return *gCur;
}

int channel(ImU32 c, int shift) { return static_cast<int>((c >> shift) & 0xFFu); }

ImU32 surfaceMean(const Surface& s) {
    const auto mid = [&](int shift) {
        return static_cast<ImU32>((channel(s.top, shift) + channel(s.bottom, shift) + 1) / 2);
    };
    return (mid(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) |
           (mid(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
           (mid(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) | (0xFFu << IM_COL32_A_SHIFT);
}

// THE PLACEMENT. The literal L is projected onto the segment from today's X
// to today's Y (how far along: t), and the same t is taken between the theme
// in force's X' and Y'. t is held to [-1, 2]: a literal a little beyond an
// anchor (a highlight brighter than the lit face, say) keeps being a little
// beyond it, but a badly chosen pair cannot throw a colour off to nowhere.
ImU32 place(int r, int g, int b, int a, Ink x, Ink y) {
    const Preset& base = presets()[0];
    const Preset& now = cur();
    const ImU32 bx = inkValue(base, x);
    const ImU32 by = inkValue(base, y);
    const ImU32 nx = inkValue(now, x);
    const ImU32 ny = inkValue(now, y);
    if (nx == bx && ny == by) {
        // TODAY'S BENCH (or any palette that agrees with it on these two
        // anchors): the literal, bit for bit.
        return IM_COL32(r, g, b, a);
    }
    const float lx[3] = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
    const float ax[3] = {static_cast<float>(channel(bx, IM_COL32_R_SHIFT)),
                         static_cast<float>(channel(bx, IM_COL32_G_SHIFT)),
                         static_cast<float>(channel(bx, IM_COL32_B_SHIFT))};
    const float ay[3] = {static_cast<float>(channel(by, IM_COL32_R_SHIFT)),
                         static_cast<float>(channel(by, IM_COL32_G_SHIFT)),
                         static_cast<float>(channel(by, IM_COL32_B_SHIFT))};
    float num = 0.0f;
    float den = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = ay[k] - ax[k];
        num += (lx[k] - ax[k]) * d;
        den += d * d;
    }
    float t = den > 1e-3f ? num / den : 0.0f;
    t = std::clamp(t, -1.0f, 2.0f);
    const float cx[3] = {static_cast<float>(channel(nx, IM_COL32_R_SHIFT)),
                         static_cast<float>(channel(nx, IM_COL32_G_SHIFT)),
                         static_cast<float>(channel(nx, IM_COL32_B_SHIFT))};
    const float cy[3] = {static_cast<float>(channel(ny, IM_COL32_R_SHIFT)),
                         static_cast<float>(channel(ny, IM_COL32_G_SHIFT)),
                         static_cast<float>(channel(ny, IM_COL32_B_SHIFT))};
    int out[3];
    for (int k = 0; k < 3; ++k) {
        const float v = cx[k] + (cy[k] - cx[k]) * t;
        out[k] = static_cast<int>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    }
    return IM_COL32(out[0], out[1], out[2], std::clamp(a, 0, 255));
}

// --- the named constants: which role each always meant --------------------------
struct LegacyEntry {
    ImU32 detail::Legacy::*field;
    unsigned today;
    Ink x;
    Ink y;
};
const LegacyEntry kLegacyMap[] = {
    // The metal: keys and the rails a hand works.
    {&detail::Legacy::brassBright, 0x8B8069, ink::CtrlTop, ink::CtrlTop},
    {&detail::Legacy::brassMid, 0x6E6552, ink::CtrlBot, ink::CtrlBot},
    {&detail::Legacy::brassDark, 0x4A4234, ink::CtrlBorder, ink::CtrlBorder},
    // The deck's own brass, and its lit edge.
    {&detail::Legacy::brassTint, 0x9C9078, ink::DeckTop, ink::White},
    {&detail::Legacy::brassShade, 0x7D7360, ink::DeckTop, ink::DeckTop},
    // The enamel and the glass.
    {&detail::Legacy::enamel, 0x2A251C, ink::Panel, ink::Panel},
    {&detail::Legacy::enamelDark, 0x1F1B14, ink::Bg, ink::Bg},
    {&detail::Legacy::well, 0x14110C, ink::Well, ink::Panel},
    {&detail::Legacy::voidInk, 0x0D0B07, ink::Well, ink::Well},
    // The inks: engraved into the deck, muted and faint on the enamel.
    {&detail::Legacy::engraved, 0x3B3529, ink::DeckInk, ink::DeckInk},
    {&detail::Legacy::inkMuted, 0x9C9078, ink::Muted, ink::Muted},
    {&detail::Legacy::inkFaint, 0x7D7360, ink::Muted, ink::Panel},
    {&detail::Legacy::ivory, 0xEFE7D2, ink::Label, ink::Label},
    {&detail::Legacy::cream, 0xD8CFB4, ink::Text, ink::Text},
    // A reading, and a reading's accents.
    {&detail::Legacy::amber, 0xF0A840, ink::Reading, ink::Reading},
    {&detail::Legacy::amberDim, 0x8A5A2A, ink::Reading, ink::Panel},
    {&detail::Legacy::gold, 0xD9B23C, ink::Accent, ink::Label},
    // What the radio received, and trouble.
    {&detail::Legacy::phosphor, 0x8FD9A0, ink::Ok, ink::Ok},
    {&detail::Legacy::phosphorDim, 0x5F8A55, ink::Ok, ink::Panel},
    {&detail::Legacy::alarm, 0xB8552F, ink::Bad, ink::Bad},
    {&detail::Legacy::alarmHot, 0xE07A4E, ink::Bad, ink::White},
};

void recomputeLegacy() {
    for (const LegacyEntry& e : kLegacyMap) {
        detail::gLegacy.*(e.field) = toneHex(e.today, 255, e.x, e.y);
    }
    // A key's corner is two thirds of a panel's: today's 2 and 3.
    const float r = std::clamp(cur().controls.radius, 0.0f, 12.0f);
    detail::gMetrics.panelRounding = r;
    detail::gMetrics.keyRounding = r * 2.0f / 3.0f;
}

double linear(int c8) {
    const double c = static_cast<double>(c8) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(ImU32 c) {
    return 0.2126 * linear(channel(c, IM_COL32_R_SHIFT)) +
           0.7152 * linear(channel(c, IM_COL32_G_SHIFT)) +
           0.0722 * linear(channel(c, IM_COL32_B_SHIFT));
}

}  // namespace

// CONSTANT-INITIALISED WITH TODAY'S VALUES, not filled in by code at start-up.
// Every translation unit may read these from its own static initialisers (a
// namespace-scope `const ImU32 k = theme::withAlpha(theme::kInkMuted, .95f)`
// is an ordinary thing to write), and C++ does not order dynamic
// initialisation across files: filled in by a constructor, the palette read
// back as zero whenever the reader happened to start first. Constant
// initialisation happens before any code runs at all. (Field order is
// detail::Legacy's; test_theme pins every value.)
namespace detail {
Legacy gLegacy{rgb(0x8B8069), rgb(0x6E6552), rgb(0x4A4234), rgb(0x9C9078), rgb(0x7D7360),
               rgb(0x2A251C), rgb(0x1F1B14), rgb(0x14110C), rgb(0x0D0B07),
               rgb(0x3B3529), rgb(0x9C9078), rgb(0x7D7360),
               rgb(0xEFE7D2), rgb(0xD8CFB4),
               rgb(0xF0A840), rgb(0x8A5A2A), rgb(0xD9B23C),
               rgb(0x8FD9A0), rgb(0x5F8A55),
               rgb(0xB8552F), rgb(0xE07A4E)};
Metrics gMetrics{2.0f, 3.0f};
}  // namespace detail

const char* roleKey(Role role) {
    static const char* const kKeys[kRoleCount] = {
        "bg",         "frame",      "frameText",  "deck",      "deckInk",    "panel",
        "panelHead",  "border",     "well",       "label",     "text",       "muted",
        "ctrl",       "ctrlBorder", "ctrlText",   "activeBg",  "activeText", "activeLine",
        "tag",        "tagText",    "digit",      "digitDim",  "digitBg",    "trace",
        "grid",       "wfLow",      "wfMid",      "wfHigh",    "wfTop",      "accent",
        "reading",    "ok",         "off",        "bad",       "stopBg",     "stopRing",
        "stopText",   "meterFace",  "meterInk",   "meterNeedle", "knob",     "knobCap",
        "plate",      "plateInk",   "menuBg",     "menuText",  "menuBorder", "menuHi",
        "sel"};
    const int i = static_cast<int>(role);
    return (i >= 0 && i < kRoleCount) ? kKeys[i] : "";
}

bool roleIsSurface(Role role) {
    switch (role) {
        case Role::Deck:
        case Role::Panel:
        case Role::Ctrl:
        case Role::ActiveBg:
        case Role::StopBg:
        case Role::DigitBg:
        case Role::Plate:
        case Role::Knob:
            return true;
        default:
            return false;
    }
}

const Preset& preset(ThemeId id) {
    const int i = static_cast<int>(id);
    return presets()[(i >= 0 && i < kThemeCount) ? i : 0];
}

const char* themeKey(ThemeId id) { return preset(id).key; }
const char* themeLabel(ThemeId id) { return preset(id).name; }

ThemeId themeFromKey(const std::string& key) {
    for (int i = 0; i < kThemeCount; ++i) {
        if (key == presets()[i].key) { return static_cast<ThemeId>(i); }
    }
    return ThemeId::Today;
}

void setTheme(ThemeId id) {
    const int i = static_cast<int>(id);
    gId = (i >= 0 && i < kThemeCount) ? id : ThemeId::Today;
    gCur = &presets()[static_cast<int>(gId)];
    gIsToday = sameAsToday(*gCur);
    recomputeLegacy();
    ++gGeneration;
}

ThemeId currentTheme() { return gId; }
const Preset& current() { return cur(); }
std::uint32_t generation() { return gGeneration; }

ImU32 role(Role r) { return surfaceMean(cur().colors[static_cast<int>(r)]); }
ImU32 roleTop(Role r) { return cur().colors[static_cast<int>(r)].top; }
ImU32 roleBottom(Role r) { return cur().colors[static_cast<int>(r)].bottom; }

ImU32 inkValue(const Preset& p, Ink i) {
    if (i.role == kInkBlack) { return IM_COL32(0, 0, 0, 255); }
    if (i.role == kInkWhite) { return IM_COL32(255, 255, 255, 255); }
    if (i.role == kInkSheen) { return p.sheen | (0xFFu << IM_COL32_A_SHIFT); }
    if (i.role >= kRoleCount) { return IM_COL32(255, 0, 255, 255); }
    const Surface& s = p.colors[i.role];
    if (i.part == 1) { return s.top; }
    if (i.part == 2) { return s.bottom; }
    return surfaceMean(s);
}

ImU32 tone(int r, int g, int b, int a, Ink x, Ink y) { return place(r, g, b, a, x, y); }
ImU32 tone(int r, int g, int b, int a, Ink x) { return place(r, g, b, a, x, x); }
ImU32 toneHex(unsigned hex, int a, Ink x, Ink y) {
    return place(static_cast<int>((hex >> 16) & 0xFFu), static_cast<int>((hex >> 8) & 0xFFu),
                 static_cast<int>(hex & 0xFFu), a, x, y);
}
ImU32 toneHex(unsigned hex, int a, Ink x) { return toneHex(hex, a, x, x); }

ImU32 sheen(int alpha) {
    const ImU32 c = cur().sheen;
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) |
           (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

ImU32 shadow(int alpha) {
    const ImU32 c = cur().shadow;
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) |
           (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

bool isTodayPalette() { return gIsToday; }

namespace {
float gReadingsScale = 1.0f;
}  // namespace

void setReadingsScale(float scale) {
    gReadingsScale = (scale >= 1.0f) ? std::min(scale, 3.0f) : 1.0f;
}
float readingsScale() { return gReadingsScale; }

ImU32 shadowOf(int r, int g, int b, int a) {
    return isTodayPalette() ? IM_COL32(r, g, b, a) : shadow(a);
}

ImU32 sheenOf(int r, int g, int b, int a) {
    return isTodayPalette() ? IM_COL32(r, g, b, a) : sheen(a);
}

ImU32 toneMix(int r, int g, int b, int a, Ink x, Ink y, float t) {
    const Preset& base = presets()[0];
    const Preset& now = cur();
    const ImU32 nx = inkValue(now, x);
    const ImU32 ny = inkValue(now, y);
    if (nx == inkValue(base, x) && ny == inkValue(base, y)) { return IM_COL32(r, g, b, a); }
    const float f = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [&](int shift) {
        const float p = static_cast<float>(channel(nx, shift));
        const float q = static_cast<float>(channel(ny, shift));
        return static_cast<int>(p + (q - p) * f + 0.5f);
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT),
                    std::clamp(a, 0, 255));
}

ImVec4 vec(ImU32 packed) {
    // IM_COL32 packs as ABGR on every platform ImGui supports, which is why
    // this shifts rather than memcpy-ing a struct: the byte order of an ImU32
    // is defined by the macro, not by the machine.
    const float s = 1.0f / 255.0f;
    return ImVec4(static_cast<float>((packed >> IM_COL32_R_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_G_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_B_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_A_SHIFT) & 0xFFu) * s);
}

ImU32 withAlpha(ImU32 packed, float alpha) {
    float a = alpha;
    if (!(a >= 0.0f)) { a = 0.0f; }
    if (a > 1.0f) { a = 1.0f; }
    const ImU32 rgbPart = packed & ~(0xFFu << IM_COL32_A_SHIFT);
    return rgbPart | (static_cast<ImU32>(a * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}

double contrastRatio(ImU32 a, ImU32 b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

ImVec4 warning() { return vec(kAmber); }
ImVec4 good() { return vec(kPhosphor); }
ImVec4 bad() { return vec(kAlarm); }

WfStops waterfallStops() {
    // Today's five stops exactly (waterfall_view.cpp's table since the
    // phosphor restyle); elsewhere wfLow, a shade between wfLow and wfMid at
    // the same place today's 0.20 stop sits, then wfMid, wfHigh and wfTop.
    WfStops s;
    s.c[0] = tone(6, 20, 10, 255, ink::WfLow);
    s.c[1] = tone(10, 70, 35, 255, ink::WfLow, ink::WfMid);
    s.c[2] = tone(30, 140, 60, 255, ink::WfMid);
    s.c[3] = tone(150, 200, 60, 255, ink::WfHigh);
    s.c[4] = tone(240, 235, 180, 255, ink::WfTop);
    return s;
}

const char* preferredFontPair(ThemeId id) {
    // bench-serif is the application's own choice (Georgia where the machine
    // has it, the embedded Saira pair where it does not) - today, untouched.
    // sans maps to the embedded Noto Sans Condensed pair; condensed, mono and
    // stencil to Saira Condensed, whose SemiBold is the nearest thing bundled
    // to a stencil caption and whose Medium stays legible at the rail's small
    // sizes where the monospaced Nova Mono closes its capitals up. Figures
    // keep Nova Mono everywhere.
    const std::string ui = preset(id).fonts.ui;
    if (ui == "bench-serif") { return ""; }
    if (ui == "sans") { return "Noto Sans Condensed"; }
    return "Saira Condensed";
}

namespace {

// TODAY'S STYLE, VERBATIM from 0.99.35: every ImGuiCol_ exactly as it was set.
void applyBenchStyleColours(ImVec4* c) {
    // The ground everything sits on. Alpha 1.0 on WindowBg is load-bearing.
    c[ImGuiCol_WindowBg] = vec(kEnamelDark);
    c[ImGuiCol_ChildBg] = vec(withAlpha(kWell, 0.55f));
    c[ImGuiCol_PopupBg] = vec(kEnamel);
    c[ImGuiCol_MenuBarBg] = vec(kBrassDark);

    // Lettering. Ivory for anything operable; the muted tone for prose.
    c[ImGuiCol_Text] = vec(kIvory);
    c[ImGuiCol_TextDisabled] = vec(kInkFaint);
    c[ImGuiCol_TextSelectedBg] = vec(withAlpha(kAmber, 0.35f));

    // Borders are brass hairlines, not black gaps.
    c[ImGuiCol_Border] = vec(withAlpha(kBrassBright, 0.55f));
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // A FIELD IS A WELL - something cut into the panel, so it is darker than
    // the panel and never lighter. Fields hold readings, and a reading lives
    // on glass.
    c[ImGuiCol_FrameBg] = vec(kEnamel);
    c[ImGuiCol_FrameBgHovered] = vec(kEngraved);
    c[ImGuiCol_FrameBgActive] = vec(kBrassDark);

    // A KEY IS BRASS AND SLIGHTLY PROUD: mid at rest, bright under the hand,
    // dark when pressed. That ordering is what makes it read as metal being
    // pushed rather than a rectangle changing colour.
    c[ImGuiCol_Button] = vec(kBrassMid);
    c[ImGuiCol_ButtonHovered] = vec(kBrassBright);
    c[ImGuiCol_ButtonActive] = vec(kBrassDark);

    // Headers - the collapsing sections of the left rail - are plates screwed
    // to the panel, so they read as brass too.
    c[ImGuiCol_Header] = vec(kBrassDark);
    c[ImGuiCol_HeaderHovered] = vec(kBrassMid);
    c[ImGuiCol_HeaderActive] = vec(kBrassBright);

    c[ImGuiCol_TitleBg] = vec(kEnamelDark);
    c[ImGuiCol_TitleBgActive] = vec(kBrassDark);
    c[ImGuiCol_TitleBgCollapsed] = vec(kEnamelDark);

    // Anything that MOVES to show a value takes the amber a reading uses: the
    // slider grab, the check mark, the progress fill. The eye then learns one
    // rule - amber means a number - instead of a colour per widget.
    c[ImGuiCol_CheckMark] = vec(kAmber);
    c[ImGuiCol_SliderGrab] = vec(kAmber);
    c[ImGuiCol_SliderGrabActive] = vec(kGold);
    c[ImGuiCol_PlotHistogram] = vec(kAmber);
    c[ImGuiCol_PlotHistogramHovered] = vec(kGold);

    // The trace of something received is phosphor, not amber - it is a picture
    // the radio made, not a figure the application computed.
    c[ImGuiCol_PlotLines] = vec(kPhosphor);
    c[ImGuiCol_PlotLinesHovered] = vec(kPhosphorDim);

    c[ImGuiCol_ScrollbarBg] = vec(withAlpha(kVoid, 0.6f));
    c[ImGuiCol_ScrollbarGrab] = vec(kBrassDark);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kBrassMid);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kBrassBright);

    c[ImGuiCol_Separator] = vec(withAlpha(kBrassBright, 0.45f));
    c[ImGuiCol_SeparatorHovered] = vec(kBrassBright);
    c[ImGuiCol_SeparatorActive] = vec(kGold);

    c[ImGuiCol_ResizeGrip] = vec(withAlpha(kBrassMid, 0.5f));
    c[ImGuiCol_ResizeGripHovered] = vec(kBrassBright);
    c[ImGuiCol_ResizeGripActive] = vec(kGold);

    c[ImGuiCol_Tab] = vec(kBrassDark);
    c[ImGuiCol_TabHovered] = vec(kBrassBright);
    c[ImGuiCol_TabSelected] = vec(kBrassMid);
    c[ImGuiCol_TabSelectedOverline] = vec(kAmber);
    c[ImGuiCol_TabDimmed] = vec(kEnamel);
    c[ImGuiCol_TabDimmedSelected] = vec(kBrassDark);

    c[ImGuiCol_TableHeaderBg] = vec(kBrassDark);
    c[ImGuiCol_TableBorderStrong] = vec(withAlpha(kBrassBright, 0.55f));
    c[ImGuiCol_TableBorderLight] = vec(withAlpha(kBrassBright, 0.25f));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = vec(withAlpha(kBrassBright, 0.05f));

    c[ImGuiCol_DragDropTarget] = vec(kGold);
    c[ImGuiCol_NavCursor] = vec(kAmber);
    c[ImGuiCol_NavWindowingHighlight] = vec(withAlpha(kIvory, 0.7f));
    c[ImGuiCol_NavWindowingDimBg] = vec(withAlpha(kVoid, 0.55f));
    c[ImGuiCol_ModalWindowDimBg] = vec(withAlpha(kVoid, 0.6f));
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const auto ch = [&](int shift) {
        const float x = static_cast<float>(channel(a, shift));
        const float y = static_cast<float>(channel(b, shift));
        return static_cast<ImU32>(x + (y - x) * t + 0.5f) & 0xFFu;
    };
    return (ch(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) | (ch(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
           (ch(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) | (ch(IM_COL32_A_SHIFT) << IM_COL32_A_SHIFT);
}

// THE READABILITY FLOOR the five other themes' own style is built to: WCAG's
// 4.5:1 for body text, with a little headroom so a colour rounded to eight
// bits a channel cannot land a hair under it.
constexpr double kReadable = 4.55;

// `c` moved along the line to `pole` just far enough that `ink` reads on it
// at `min` (found on the eight-bit colour actually drawn). Returns `c` when it
// already does, and the pole itself when nothing short of it would.
ImU32 moveUntil(ImU32 c, ImU32 pole, double min, ImU32 ink, bool moveInk) {
    const auto ok = [&](ImU32 x) {
        return contrastRatio(moveInk ? x : ink, moveInk ? c : x) >= min;
    };
    const ImU32 start = moveInk ? ink : c;
    if (ok(start)) { return start; }
    if (!ok(mix(start, pole, 1.0f))) { return mix(start, pole, 1.0f); }
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (ok(mix(start, pole, mid))) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return mix(start, pole, hi);
}

// A SURFACE THE INK CAN BE READ ON: `surface`, moved away from `ink` - darker
// under a light ink, lighter under a dark one - only as far as it has to be.
// This is what keeps ImGui's own state colours (a hovered key, a pressed one,
// the selected tab) readable: ImGui letters every one of them in the single
// ImGuiCol_Text, so the ground has to give way, not the ink.
ImU32 surfaceFor(ImU32 ink, ImU32 surface, double min = kReadable) {
    const ImU32 pole = luminance(ink) > luminance(surface) ? IM_COL32(0, 0, 0, 255)
                                                            : IM_COL32(255, 255, 255, 255);
    return moveUntil(surface, pole, min, ink, false) | (0xFFu << IM_COL32_A_SHIFT);
}

// An INK that reads on every one of `grounds`: `ink`, moved toward `toward`
// (the stronger ink of the same family) until the worst ground reaches `min`.
ImU32 inkFor(ImU32 ink, ImU32 toward, const ImU32* grounds, int n, double min = kReadable) {
    ImU32 worst = grounds[0];
    for (int i = 1; i < n; ++i) {
        if (contrastRatio(ink, grounds[i]) < contrastRatio(ink, worst)) { worst = grounds[i]; }
    }
    ImU32 out = moveUntil(worst, toward, min, ink, true);
    // The worst ground decided it; the rest are no harder, but check them all.
    for (int i = 0; i < n; ++i) {
        if (contrastRatio(out, grounds[i]) < min) {
            out = moveUntil(grounds[i], toward, min, out, true);
        }
    }
    return out;
}

// --- THE POPUP PALETTE ------------------------------------------------------------
//
// IMGUI LETTERS A POPUP IN THE SAME ImGuiCol_Text AS A WINDOW, and foxsdr-ui/1
// gives menus their own ground and ink (menuBg, menuText, menuHi, menuBorder).
// On a theme whose menus are the same polarity as its panels that is invisible;
// on Field Radio - cream menus over olive panels - it put cream words on a
// cream list, 1.09:1, in every combo, tooltip and context menu. So every
// popup-like window (a popup, a combo's list, a menu, a modal, a tooltip) is
// drawn with this table pushed over the style for its whole extent: the vendored
// ImGui does the pushing (FOXSDR PATCH popup-colours), so no call site - there
// are over a hundred tooltips alone - can forget it. Everything a popup can
// letter or hold is here: its words and quiet words, and the grounds of the
// keys, fields, list rows, title bars, tabs and table headers inside it, each
// made readable under menuText. Today's bench pushes nothing: its menus are
// the same enamel its windows are, exactly as 0.99.35 drew them.
constexpr int kPopupColourMax = 32;

int buildPopupColours(ImGuiCol* idx, ImVec4* col) {
    if (isTodayPalette()) { return 0; }
    const ImU32 mb = role(Role::MenuBg);
    const ImU32 mt = role(Role::MenuText);
    const ImU32 mh = role(Role::MenuHi);
    const ImU32 mbd = role(Role::MenuBorder);
    const ImU32 rest = surfaceFor(mt, mh);
    const ImU32 hover = surfaceFor(mt, mix(mh, mt, 0.12f));
    const ImU32 held = surfaceFor(mt, mix(mh, mt, 0.22f));
    int n = 0;
    const auto put = [&](ImGuiCol i, ImU32 c) {
        idx[n] = i;
        col[n] = vec(c);
        ++n;
    };
    put(ImGuiCol_Text, mt);
    // Quieter than the words, and still read: halfway to the ground, then back
    // toward menuText as far as the popup's ground needs.
    put(ImGuiCol_TextDisabled, inkFor(mix(mt, mb, 0.45f), mt, &mb, 1));
    put(ImGuiCol_Border, mbd);
    put(ImGuiCol_Separator, mbd);
    put(ImGuiCol_CheckMark, mt);
    // A child inside a popup is the popup's own ground, not a dark well in it.
    put(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    put(ImGuiCol_Button, rest);
    put(ImGuiCol_ButtonHovered, hover);
    put(ImGuiCol_ButtonActive, held);
    put(ImGuiCol_Header, rest);
    put(ImGuiCol_HeaderHovered, hover);
    put(ImGuiCol_HeaderActive, held);
    put(ImGuiCol_FrameBg, surfaceFor(mt, mix(mb, mh, 0.5f)));
    put(ImGuiCol_FrameBgHovered, rest);
    put(ImGuiCol_FrameBgActive, hover);
    put(ImGuiCol_TitleBg, rest);
    put(ImGuiCol_TitleBgActive, rest);
    put(ImGuiCol_TitleBgCollapsed, rest);
    put(ImGuiCol_MenuBarBg, rest);
    put(ImGuiCol_Tab, rest);
    put(ImGuiCol_TabHovered, hover);
    put(ImGuiCol_TabSelected, held);
    put(ImGuiCol_TabDimmed, rest);
    put(ImGuiCol_TabDimmedSelected, hover);
    put(ImGuiCol_TableHeaderBg, rest);
    put(ImGuiCol_TableRowBgAlt, withAlpha(mt, 0.04f));
    put(ImGuiCol_TableBorderStrong, mbd);
    put(ImGuiCol_TableBorderLight, withAlpha(mbd, 0.5f));
    return n;
}

// EVERY OTHER THEME: the same widget roles, read from foxsdr-ui/1's names -
// a field is a well holding text, a button is a control, a header a control
// plate, a menu the menu roles, anything that moves to show a value the accent.
void applyRoleStyleColours(ImVec4* c) {
    const ImU32 bg = role(Role::Bg);
    const ImU32 panel = role(Role::Panel);
    const ImU32 well = role(Role::Well);
    const ImU32 border = role(Role::Border);
    const ImU32 ctrl = role(Role::Ctrl);
    const ImU32 ctrlBorder = role(Role::CtrlBorder);
    const ImU32 active = role(Role::ActiveBg);
    const ImU32 accent = role(Role::Accent);
    const ImU32 label = role(Role::Label);
    const ImU32 muted = role(Role::Muted);
    const ImU32 menuHi = role(Role::MenuHi);

    c[ImGuiCol_WindowBg] = vec(bg);
    c[ImGuiCol_ChildBg] = vec(withAlpha(well, 0.55f));
    // A popup's GROUND is menuBg; its words, and everything else it letters,
    // are the popup palette (buildPopupColours), pushed over this style for
    // every popup-like window.
    c[ImGuiCol_PopupBg] = vec(role(Role::MenuBg));
    c[ImGuiCol_MenuBarBg] = vec(surfaceFor(label, role(Role::PanelHead)));

    c[ImGuiCol_Text] = vec(label);
    // QUIET, BUT READ. This application prints information in TextDisabled
    // ("RDS: no data", the MONO flag, the target list's ids), so it is held to
    // 4.5:1 on the window and on a child's well like any other words - muted
    // ink a third of the way to the panel, brought back toward the label only
    // as far as that takes (Daylight Lab's drew at 2.15:1 before this).
    {
        const ImU32 grounds[2] = {bg, mix(bg, well, 0.55f)};
        c[ImGuiCol_TextDisabled] = vec(inkFor(mix(muted, panel, 0.35f), label, grounds, 2));
    }
    c[ImGuiCol_TextSelectedBg] = vec(withAlpha(role(Role::Sel), 0.35f));

    c[ImGuiCol_Border] = vec(border);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // EVERY GROUND IMGUI LETTERS ON is made readable under the label
    // (surfaceFor): a key hovered or held, a selected row, the selected tab.
    // The lit key's own foxsdr-ui/1 colour (activeBg) carries activeText, and
    // ImGui cannot letter one state of a key in a different ink - so its
    // PRESSED state is the lit key's colour taken as dark (or as light) as the
    // label needs, and a key that is LATCHED lit is drawn with activeText on
    // the true activeBg by pushLitKeyColours(). Field Radio's pressed key was
    // cream on gold at 1.57:1 before this.
    c[ImGuiCol_FrameBg] = vec(surfaceFor(label, well));
    c[ImGuiCol_FrameBgHovered] = vec(surfaceFor(label, mix(well, ctrl, 0.45f)));
    c[ImGuiCol_FrameBgActive] = vec(surfaceFor(label, mix(well, active, 0.35f)));

    c[ImGuiCol_Button] = vec(surfaceFor(label, ctrl));
    c[ImGuiCol_ButtonHovered] = vec(surfaceFor(label, mix(ctrl, active, 0.35f)));
    c[ImGuiCol_ButtonActive] = vec(surfaceFor(label, active));

    c[ImGuiCol_Header] = vec(surfaceFor(label, ctrl));
    c[ImGuiCol_HeaderHovered] = vec(surfaceFor(label, menuHi));
    c[ImGuiCol_HeaderActive] = vec(surfaceFor(label, mix(menuHi, active, 0.5f)));

    c[ImGuiCol_TitleBg] = vec(surfaceFor(label, role(Role::PanelHead)));
    c[ImGuiCol_TitleBgActive] = vec(surfaceFor(label, role(Role::Frame)));
    c[ImGuiCol_TitleBgCollapsed] = vec(surfaceFor(label, role(Role::PanelHead)));

    c[ImGuiCol_CheckMark] = vec(accent);
    c[ImGuiCol_SliderGrab] = vec(accent);
    c[ImGuiCol_SliderGrabActive] = vec(mix(accent, label, 0.3f));
    c[ImGuiCol_PlotHistogram] = vec(accent);
    c[ImGuiCol_PlotHistogramHovered] = vec(mix(accent, label, 0.3f));

    c[ImGuiCol_PlotLines] = vec(role(Role::Trace));
    c[ImGuiCol_PlotLinesHovered] = vec(mix(role(Role::Trace), panel, 0.35f));

    c[ImGuiCol_ScrollbarBg] = vec(withAlpha(well, 0.6f));
    c[ImGuiCol_ScrollbarGrab] = vec(ctrlBorder);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(mix(ctrlBorder, active, 0.4f));
    c[ImGuiCol_ScrollbarGrabActive] = vec(active);

    c[ImGuiCol_Separator] = vec(withAlpha(border, 0.9f));
    c[ImGuiCol_SeparatorHovered] = vec(ctrlBorder);
    c[ImGuiCol_SeparatorActive] = vec(accent);

    c[ImGuiCol_ResizeGrip] = vec(withAlpha(ctrlBorder, 0.5f));
    c[ImGuiCol_ResizeGripHovered] = vec(ctrlBorder);
    c[ImGuiCol_ResizeGripActive] = vec(accent);

    c[ImGuiCol_Tab] = vec(surfaceFor(label, ctrl));
    c[ImGuiCol_TabHovered] = vec(surfaceFor(label, mix(ctrl, active, 0.35f)));
    c[ImGuiCol_TabSelected] = vec(surfaceFor(label, active));
    c[ImGuiCol_TabSelectedOverline] = vec(role(Role::ActiveLine));
    c[ImGuiCol_TabDimmed] = vec(surfaceFor(label, panel));
    c[ImGuiCol_TabDimmedSelected] = vec(surfaceFor(label, ctrl));

    c[ImGuiCol_TableHeaderBg] = vec(surfaceFor(label, role(Role::PanelHead)));
    c[ImGuiCol_TableBorderStrong] = vec(border);
    c[ImGuiCol_TableBorderLight] = vec(withAlpha(border, 0.5f));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = vec(withAlpha(label, 0.04f));

    c[ImGuiCol_DragDropTarget] = vec(accent);
    c[ImGuiCol_NavCursor] = vec(accent);
    c[ImGuiCol_NavWindowingHighlight] = vec(withAlpha(label, 0.7f));
    c[ImGuiCol_NavWindowingDimBg] = vec(withAlpha(shadow(255), 0.55f));
    c[ImGuiCol_ModalWindowDimBg] = vec(withAlpha(shadow(255), 0.6f));
}

}  // namespace

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // --- shape ---------------------------------------------------------------
    // A machined key has a small radius. Nothing in this design is round except
    // the things that are actually round - knobs, lamps, screws, the scope.
    s.WindowRounding = 0.0f;  // load-bearing: see the header
    s.ChildRounding = kPanelRounding;
    s.FrameRounding = kKeyRounding;
    s.PopupRounding = kPanelRounding;
    s.ScrollbarRounding = kKeyRounding;
    s.GrabRounding = kKeyRounding;
    s.TabRounding = kKeyRounding;

    s.WindowBorderSize = kHairline;
    s.ChildBorderSize = kHairline;
    s.PopupBorderSize = kHairline;
    s.FrameBorderSize = kHairline;

    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;

    // --- colour --------------------------------------------------------------
    if (isTodayPalette()) {
        applyBenchStyleColours(s.Colors);
    } else {
        applyRoleStyleColours(s.Colors);
    }

    // A TORN-OFF WINDOW IS A REAL OS WINDOW in this application, so its
    // background must be fully opaque. ImGui uses WindowBg for viewports; the
    // alpha set above is what keeps a map page from rendering see-through.
    s.Colors[ImGuiCol_WindowBg].w = 1.0f;

    // Every popup-like window's own palette (none under today's bench).
    ImGuiCol idx[kPopupColourMax];
    ImVec4 col[kPopupColourMax];
    const int n = buildPopupColours(idx, col);
    ImGui::FoxSetPopupColors(idx, col, n);
}

int popupColours(ImGuiCol* idx, ImVec4* col, int max) {
    ImGuiCol i[kPopupColourMax];
    ImVec4 c[kPopupColourMax];
    const int n = std::min(buildPopupColours(i, c), std::max(max, 0));
    for (int k = 0; k < n; ++k) {
        idx[k] = i[k];
        col[k] = c[k];
    }
    return n;
}

ImU32 legible(ImU32 ink, ImU32 surface, double minRatio) {
    // TODAY IS FROZEN: its inks are exactly what 0.99.35 drew, readable or not.
    if (isTodayPalette()) { return ink; }
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const ImU32 black = IM_COL32(0, 0, 0, 255);
    const ImU32 pole = contrastRatio(white, surface) >= contrastRatio(black, surface) ? white : black;
    const ImU32 a = ink & (0xFFu << IM_COL32_A_SHIFT);
    return (moveUntil(surface, pole, minRatio, ink | (0xFFu << IM_COL32_A_SHIFT), true) &
            ~(0xFFu << IM_COL32_A_SHIFT)) |
           a;
}

double relativeLuminance(ImU32 c) { return luminance(c); }

int pushLitKeyColours() {
    if (isTodayPalette()) {
        // 0.99.35's lit key, exactly: the pressed brass, lettered in the style's ivory.
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        return 1;
    }
    // foxsdr-ui/1's lit key: activeText on activeBg, whatever the hand is doing.
    const ImU32 ink = role(Role::ActiveText);
    const ImU32 face = surfaceFor(ink, role(Role::ActiveBg));
    ImGui::PushStyleColor(ImGuiCol_Button, vec(face));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(face));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(face));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(ink));
    return 4;
}

}  // namespace cascade::gui::theme
