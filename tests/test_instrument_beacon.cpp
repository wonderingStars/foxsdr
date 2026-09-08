// Tests for the CASCADE_INSTRUMENT_BEACON face: the 406 MHz distress-beacon
// alarm panel in src/gui/instrument_beacon.cpp and the arithmetic behind it in
// src/gui/instrument_beacon_math.hpp.
//
// TWO HALVES, BECAUSE THE FAULTS COME IN TWO KINDS.
//
//   The PURE half asks what the face says. A seven-segment table with one wrong
//   entry turns one beacon identity into another - B into 8, D into 0 - and a
//   beacon identity reported wrong is the whole point of this window failing
//   silently. A formatter that prints "0 s" for a slot the plugin never filled
//   breaks the rule the entire face is built on. Neither is visible in a
//   screenshot and neither needs a graphics context to check.
//
//   The GEOMETRY half asks whether it stays in its box, through a real headless
//   Dear ImGui context, exactly as tests/test_scope_face.cpp does. The window
//   is user-resizable and the host hands the face anything from 160 to 360
//   points of height; ImGui does not wrap and does not clip on its own, so a
//   deck that outgrew its band would be drawn straight over the burst log
//   underneath and would look like a design decision.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "core/plugin_ui.hpp"
#include "gui/fonts.hpp"
#include "gui/instrument_beacon_math.hpp"
#include "gui/instrument_face.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace {

namespace bx = cascade::gui::beacon;
using cascade::core::HostInstrument;

// --- the segment table -------------------------------------------------------

void testSegmentTable() {
    std::printf("\n[1] the seven-segment table\n");
    // Segment bits: a=1 b=2 c=4 d=8 e=16 f=32 g=64.
    CHECK(bx::segments('0') == 0x3Fu);   // every segment but the middle bar
    CHECK(bx::segments('1') == 0x06u);   // b and c only
    CHECK(bx::segments('8') == 0x7Fu);   // all seven
    CHECK(bx::segments('7') == 0x07u);
    CHECK(bx::segments('A') == 0x77u);
    CHECK(bx::segments('F') == 0x71u);
    // Lower case reads the same as upper: a plugin sending "adcd..." must not
    // produce a blank readout.
    for (const char* p = "0123456789abcdef"; *p != '\0'; ++p) {
        const char up = (*p >= 'a') ? static_cast<char>(*p - 'a' + 'A') : *p;
        CHECK(bx::segments(*p) == bx::segments(up));
    }
    // THE FOUR CONFUSIONS A SEVEN-SEGMENT HEX DISPLAY EXISTS TO AVOID. B must
    // not look like 8, D must not look like 0, and 6 and 9 must keep their
    // tails - each of these being equal would turn one beacon identity into a
    // different, real, valid-looking one.
    CHECK(bx::segments('B') != bx::segments('8'));
    CHECK(bx::segments('D') != bx::segments('0'));
    CHECK(bx::segments('6') != bx::segments('b'));
    CHECK(bx::segments('9') != bx::segments('8'));
    // Every one of the sixteen is distinct from every other.
    const char* hex = "0123456789ABCDEF";
    int collisions = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = i + 1; j < 16; ++j) {
            if (bx::segments(hex[i]) == bx::segments(hex[j])) { ++collisions; }
        }
    }
    CHECK(collisions == 0);
    // Anything that is not a hexadecimal character lights nothing, which is the
    // blank cell the no-reading rule asks for.
    CHECK(bx::segments(' ') == 0x00u);
    CHECK(bx::segments('\0') == 0x00u);
    CHECK(bx::segments('Z') == 0x00u);
    CHECK(bx::segments('-') == 0x00u);
}

// --- laying the identity into the cells --------------------------------------

void testLayHexId() {
    std::printf("\n[2] the identity in its fifteen cells\n");
    char cells[bx::kHexIdChars + 1];

    CHECK(bx::layHexId("ADCD00800440401", cells) == 15);
    CHECK(std::string(cells) == "ADCD00800440401");

    // Nothing at all: fifteen blank cells, not a single one lit.
    CHECK(bx::layHexId("", cells) == 0);
    CHECK(std::string(cells) == "               ");
    int litCells = 0;
    for (int i = 0; i < bx::kHexIdChars; ++i) {
        if (bx::segments(cells[i]) != 0u) { ++litCells; }
    }
    CHECK(litCells == 0);

    // A null slot is the same as an empty one, and must not read past it.
    CHECK(bx::layHexId(nullptr, cells) == 0);
    CHECK(std::string(cells) == "               ");

    // Short: the rest of the row is blank rather than padded with zeroes,
    // because a padded zero is a hexadecimal character and would be read as
    // part of the identity.
    CHECK(bx::layHexId("ABC", cells) == 3);
    CHECK(std::string(cells) == "ABC            ");

    // Over-long - the longest thing the slot can hold is 63 characters - is cut
    // at fifteen and never overruns the buffer.
    char big[CASCADE_INSTRUMENT_TEXT_CHARS];
    for (std::size_t i = 0; i + 1 < sizeof big; ++i) { big[i] = 'F'; }
    big[sizeof big - 1] = '\0';
    CHECK(bx::layHexId(big, cells) == 15);
    CHECK(std::strlen(cells) == 15u);
    CHECK(std::string(cells) == "FFFFFFFFFFFFFFF");

    // Lower case is lifted; a character with no segments keeps its cell rather
    // than being filtered out, so a plugin sending prose produces a visibly
    // broken readout instead of an identity assembled from the hex letters in
    // a sentence.
    CHECK(bx::layHexId("adcd0080044040z", cells) == 15);
    CHECK(std::string(cells) == "ADCD0080044040Z");
    CHECK(bx::segments(cells[14]) == 0x00u);
}

// --- the figures -------------------------------------------------------------

void testFormatAge() {
    std::printf("\n[3] the age of the last burst\n");
    char out[32];

    // NO READING IS NOTHING, not "0 s".
    bx::formatAge(41.0, false, out, sizeof out);
    CHECK(out[0] == '\0');
    bx::formatAge(std::nan(""), true, out, sizeof out);
    CHECK(out[0] == '\0');
    bx::formatAge(std::numeric_limits<double>::infinity(), true, out, sizeof out);
    CHECK(out[0] == '\0');

    bx::formatAge(0.0, true, out, sizeof out);
    CHECK(std::string(out) == "0 s");
    bx::formatAge(12.4, true, out, sizeof out);
    CHECK(std::string(out) == "12 s");
    bx::formatAge(-5.0, true, out, sizeof out);
    CHECK(std::string(out) == "0 s");
    // Every unit boundary, in both directions.
    bx::formatAge(59.9, true, out, sizeof out);
    CHECK(std::string(out) == "59 s");
    bx::formatAge(60.0, true, out, sizeof out);
    CHECK(std::string(out) == "1:00");
    bx::formatAge(125.0, true, out, sizeof out);
    CHECK(std::string(out) == "2:05");
    bx::formatAge(3599.0, true, out, sizeof out);
    CHECK(std::string(out) == "59:59");
    bx::formatAge(3600.0, true, out, sizeof out);
    CHECK(std::string(out) == "1:00:00");
    // The largest the slot can hold saturates instead of printing a number
    // wider than the face.
    bx::formatAge(1.0e300, true, out, sizeof out);
    CHECK(std::string(out) == "99:59:59");
    CHECK(std::strlen(out) < sizeof out);
}

void testFormatError() {
    std::printf("\n[4] the carrier error\n");
    char out[32];

    bx::formatError(430.0, false, out, sizeof out);
    CHECK(out[0] == '\0');
    bx::formatError(std::nan(""), true, out, sizeof out);
    CHECK(out[0] == '\0');

    // SIGNED, ALWAYS. A carrier 430 Hz high and one 430 Hz low are different
    // measurements and the panel has to say which.
    bx::formatError(430.0, true, out, sizeof out);
    CHECK(std::string(out) == "+430 Hz");
    bx::formatError(-430.0, true, out, sizeof out);
    CHECK(std::string(out) == "-430 Hz");
    bx::formatError(0.0, true, out, sizeof out);
    CHECK(std::string(out) == "+0 Hz");
    bx::formatError(9999.0, true, out, sizeof out);
    CHECK(std::string(out) == "+9999 Hz");
    bx::formatError(12000.0, true, out, sizeof out);
    CHECK(std::string(out) == "+12.0 kHz");
    // Saturating, so a nonsense double in the slot cannot draw forty characters
    // across the meter.
    bx::formatError(1.0e300, true, out, sizeof out);
    CHECK(std::string(out) == "+999.9 kHz");
    bx::formatError(-1.0e300, true, out, sizeof out);
    CHECK(std::string(out) == "-999.9 kHz");
    CHECK(std::strlen(out) < sizeof out);
}

void testErrorFraction() {
    std::printf("\n[5] where the needle sits on a centre-zero scale\n");
    // Zero is the middle of the travel: that is the whole claim of a
    // centre-zero meter, and the one thing a 0..1 travel meter could not make.
    CHECK_NEAR(bx::errorFraction(0.0), 0.5, 1e-12);
    CHECK_NEAR(bx::errorFraction(bx::kCarrierErrorFullScaleHz), 1.0, 1e-12);
    CHECK_NEAR(bx::errorFraction(-bx::kCarrierErrorFullScaleHz), 0.0, 1e-12);
    CHECK_NEAR(bx::errorFraction(2500.0), 0.75, 1e-12);
    CHECK_NEAR(bx::errorFraction(-2500.0), 0.25, 1e-12);
    // Symmetric about the centre, at every step.
    for (int i = -60; i <= 60; ++i) {
        const double hz = static_cast<double>(i) * 100.0;
        CHECK_NEAR(bx::errorFraction(hz) + bx::errorFraction(-hz), 1.0, 1e-12);
    }
    // Both stops hold, and a non-finite figure parks on the centre rather than
    // producing a needle angle that is not a number.
    CHECK(bx::errorFraction(1.0e300) == 1.0);
    CHECK(bx::errorFraction(-1.0e300) == 0.0);
    CHECK(bx::errorFraction(std::nan("")) == 0.5);
}

// --- the legend --------------------------------------------------------------

void testWarningLegend() {
    std::printf("\n[6] the engraved warning legend\n");
    const std::string whole = bx::warningLine(0);
    const std::string first = bx::warningLine(1);
    const std::string second = bx::warningLine(2);
    CHECK(!first.empty());
    CHECK(!second.empty());
    // THE TWO-LINE FORM IS THE WHOLE LEGEND. A narrow window must not be able
    // to lose a clause of it, which is exactly what an independently written
    // "short version" would eventually do.
    CHECK(first + bx::warningJoin() + second == whole);
    CHECK(whole.find("DISTRESS ALERT") != std::string::npos);
    CHECK(whole.find("RESCUE CO-ORDINATION CENTRE") != std::string::npos);
    CHECK(whole.find("NOT A TEST") != std::string::npos);
}

// --- the rail's chip ---------------------------------------------------------

void testChipWord() {
    std::printf("\n[7] the word the rail carries\n");
    char out[16];
    bx::chipWord(true, 3.0, 7, out, sizeof out);
    CHECK(std::string(out) == "ALERT");
    // Not alerting: how long since the last burst, which is what says whether
    // the beacon is still transmitting.
    bx::chipWord(false, 45.0, 7, out, sizeof out);
    CHECK(std::string(out) == "45 s");
    bx::chipWord(false, 130.0, 7, out, sizeof out);
    CHECK(std::string(out) == "2:10");
    // No age filled: fall back to the size of the log rather than inventing one.
    bx::chipWord(false, 0.0, 7, out, sizeof out);
    CHECK(std::string(out) == "7 LOG");
    bx::chipWord(false, std::nan(""), 0, out, sizeof out);
    CHECK(std::string(out) == "0 LOG");
    CHECK(std::strlen(out) < sizeof out);

    // And the same through the host's own dispatcher, which is what the rail
    // actually calls.
    HostInstrument in;
    in.kind = CASCADE_INSTRUMENT_BEACON;
    in.have = true;
    in.state.values[1] = 45.0;
    in.state.flags = CASCADE_INSTRUMENT_FLAG_ALERT;
    cascade::gui::instrumentChip(in, false, out, sizeof out);
    CHECK(std::string(out) == "ALERT");
    in.state.flags = 0u;
    cascade::gui::instrumentChip(in, false, out, sizeof out);
    CHECK(std::string(out) == "45 s");
    // Unread and not-yet-heard still win, as they do for every kind.
    cascade::gui::instrumentChip(in, true, out, sizeof out);
    CHECK(std::string(out) == "NEW");
    in.have = false;
    cascade::gui::instrumentChip(in, false, out, sizeof out);
    CHECK(std::string(out) == "WAIT");
}

// --- the layout --------------------------------------------------------------

void checkLayoutInBox(const char* what, float w, float h) {
    const bx::Layout L = bx::layout(0.0f, 0.0f, w, h);
    if (!L.any) { return; }
    bool ok = L.used <= h + 0.01f && L.x0 >= 0.0f && L.x1 <= w + 0.01f &&
              L.readY1 <= h + 0.01f && L.wellX1 <= L.x1 + 0.01f &&
              L.wellX0 >= L.x0 - 0.01f && L.cellW > 0.0f && L.digitH > 0.0f;
    if (L.plates) { ok = ok && L.plateY1 <= h + 0.01f && L.plateY0 >= L.readY1; }
    if (L.gauges > 0) { ok = ok && L.gaugeY1 <= h + 0.01f; }
    if (L.legend) { ok = ok && L.legY1 <= h + 0.01f; }
    // The fifteen cells and their gaps have to fit the glass they sit in.
    const float span = L.cellW * 15.0f + L.cellGap * 14.0f;
    ok = ok && span <= (L.wellX1 - L.wellX0) - 15.0f;
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at %.0f x %.0f: used %.1f, read %.1f..%.1f, well "
                    "%.1f..%.1f span %.1f, cell %.2f\n",
                    what, static_cast<double>(w), static_cast<double>(h),
                    static_cast<double>(L.used), static_cast<double>(L.readY0),
                    static_cast<double>(L.readY1), static_cast<double>(L.wellX0),
                    static_cast<double>(L.wellX1), static_cast<double>(span),
                    static_cast<double>(L.cellW));
    }
}

void testLayout() {
    std::printf("\n[8] the four decks in the rectangle they are given\n");
    // A rectangle with nothing in it does not crash and does not draw.
    const bx::Layout zero = bx::layout(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(!zero.any);
    CHECK(zero.used == 0.0f);
    const bx::Layout inverted = bx::layout(100.0f, 100.0f, 0.0f, 0.0f);
    CHECK(!inverted.any);
    const bx::Layout tiny = bx::layout(0.0f, 0.0f, 60.0f, 30.0f);
    CHECK(!tiny.any);

    // The identity is never given up: any rectangle the face draws in at all
    // has a readout in it.
    for (float w = 100.0f; w <= 1800.0f; w += 37.0f) {
        for (float h = 50.0f; h <= 900.0f; h += 23.0f) {
            const bx::Layout L = bx::layout(0.0f, 0.0f, w, h);
            if (!L.any) { continue; }
            CHECK(L.readY1 > L.readY0);
            checkLayoutInBox("layout", w, h);
        }
    }

    // The decks arrive in the documented order as the window grows, and never
    // go away again once they have arrived.
    bool sawNone = false, sawPlates = false, sawGauges = false, sawLegend = false;
    for (float h = 50.0f; h <= 500.0f; h += 1.0f) {
        const bx::Layout L = bx::layout(0.0f, 0.0f, 600.0f, h);
        if (!L.any) { continue; }
        if (!L.plates) { sawNone = true; }
        if (L.plates) { sawPlates = true; }
        if (L.gauges > 0) { sawGauges = true; }
        if (L.legend) { sawLegend = true; }
        // Nothing may appear before the deck above it.
        CHECK(!(L.gauges > 0) || L.plates);
    }
    CHECK(sawNone && sawPlates && sawGauges && sawLegend);

    // The sizes the host actually hands out.
    checkLayoutInBox("host minimum", 600.0f, 160.0f);
    checkLayoutInBox("host typical", 600.0f, 276.0f);
    checkLayoutInBox("host maximum", 600.0f, 360.0f);
    checkLayoutInBox("no memory, full window", 600.0f, 420.0f);

    // At 600 x 276 - the shape the demonstration window opens at - the whole
    // panel is there.
    const bx::Layout d = bx::layout(0.0f, 0.0f, 600.0f, 276.0f);
    CHECK(d.any && d.plates && d.gauges == 3 && d.legend && d.newLamp);
    // A narrow window keeps the identity and drops what it cannot letter: the
    // NEW lamp's column goes back to the glass, and the gauge deck falls to
    // the one bay it can still draw a needle on.
    const bx::Layout narrow = bx::layout(0.0f, 0.0f, 210.0f, 276.0f);
    CHECK(narrow.any && narrow.gauges == 1 && !narrow.newLamp);
    // AND THE IDENTITY IS STILL THE THING THAT GETS THE ROOM. On a tall narrow
    // window the lamp is sized by the width rather than by the deck, so the
    // fifteen cells keep a usable share of it.
    const bx::Layout tall = bx::layout(0.0f, 0.0f, 190.0f, 560.0f);
    CHECK(tall.any);
    CHECK(tall.wellX1 - tall.wellX0 > (tall.x1 - tall.x0) * 0.45f);
    CHECK(tall.cellW > 3.0f);
}

// --- geometry, through a real headless context -------------------------------

struct Box {
    int verts = 0;
    bool bad = false;
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
};

Box extentFrom(const ImDrawList* dl, int from) {
    Box b;
    for (int i = from; i < dl->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y)) {
            b.bad = true;
            continue;
        }
        if (b.verts == 0) {
            b.x0 = b.x1 = v.pos.x;
            b.y0 = b.y1 = v.pos.y;
        } else {
            b.x0 = (v.pos.x < b.x0) ? v.pos.x : b.x0;
            b.x1 = (v.pos.x > b.x1) ? v.pos.x : b.x1;
            b.y0 = (v.pos.y < b.y0) ? v.pos.y : b.y0;
            b.y1 = (v.pos.y > b.y1) ? v.pos.y : b.y1;
        }
        ++b.verts;
    }
    return b;
}

// One frame, one window far larger than anything drawn in it, and the extent of
// exactly what the call emitted. The slack matters: ImGui culls glyphs against
// the current clip rectangle, so a face overflowing inside a tight window would
// be trimmed by the window and the test would pass by being unable to see it.
template <class F>
Box drawOne(F&& fn) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(2000.0f, 1200.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("beacon", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    const int mark = dl->VtxBuffer.Size;
    fn(dl);
    const Box b = extentFrom(dl, mark);
    ImGui::End();
    ImGui::Render();
    return b;
}

HostInstrument liveBeacon() {
    HostInstrument in;
    in.plugin = "406 MHz Beacons";
    in.title = "406 MHz beacon";
    in.kind = CASCADE_INSTRUMENT_BEACON;
    in.have = true;
    in.state.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
    in.state.seq = 3u;
    in.state.flags = CASCADE_INSTRUMENT_FLAG_ALERT;
    std::snprintf(in.state.text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "ADCD00800440401");
    std::snprintf(in.state.text[1], CASCADE_INSTRUMENT_TEXT_CHARS,
                  "232 United Kingdom of Great Britain and Northern Ireland");
    std::snprintf(in.state.text[2], CASCADE_INSTRUMENT_TEXT_CHARS,
                  "EPIRB Serial Location Protocol");
    std::snprintf(in.state.text[3], CASCADE_INSTRUMENT_TEXT_CHARS, "53.55556N 1.71444W");
    in.state.values[0] = 430.0;
    in.state.values[1] = 12.0;
    in.headings = {"Time", "Hex ID", "Country", "Channel"};
    return in;
}

void checkFaceInside(const char* what, const HostInstrument& in, float w, float h,
                     double nowSec, bool unread) {
    cascade::gui::InstrumentCue cue;
    cue.unread = unread;
    cue.nowSec = nowSec;
    float used = 0.0f;
    const ImVec2 tl(60.0f, 40.0f);
    const ImVec2 br(tl.x + w, tl.y + h);
    const Box b = drawOne([&](ImDrawList* dl) {
        used = cascade::gui::drawBeaconFace(dl, tl, br, in, cue);
    });
    // Half a pixel of anti-aliasing, plus the one pixel a shadow pass sits
    // under its letters. A deck that overflowed its band overflows by tens.
    constexpr float kSlack = 2.0f;
    const bool ok = !b.bad && used >= 0.0f && used <= h + kSlack &&
                    (b.verts == 0 ||
                     (b.x0 >= tl.x - kSlack && b.x1 <= br.x + kSlack &&
                      b.y0 >= tl.y - kSlack && b.y1 <= br.y + kSlack));
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at %.0f x %.0f: drew x %.1f..%.1f y %.1f..%.1f in "
                    "%.1f..%.1f / %.1f..%.1f, used %.1f (%d verts%s)\n",
                    what, static_cast<double>(w), static_cast<double>(h),
                    static_cast<double>(b.x0), static_cast<double>(b.x1),
                    static_cast<double>(b.y0), static_cast<double>(b.y1),
                    static_cast<double>(tl.x), static_cast<double>(br.x),
                    static_cast<double>(tl.y), static_cast<double>(br.y),
                    static_cast<double>(used), b.verts, b.bad ? ", NON-FINITE" : "");
    }
}

void testFaceFitsEverySize() {
    std::printf("\n[9] the face inside the rectangle it is handed\n");
    const HostInstrument live = liveBeacon();
    HostInstrument cold;
    cold.title = "406 MHz beacon";
    cold.kind = CASCADE_INSTRUMENT_BEACON;
    cold.headings = {"Time", "Hex ID", "Country", "Channel"};

    // The three shapes the host actually produces, and the extremes either
    // side of them.
    checkFaceInside("live, host minimum", live, 600.0f, 160.0f, 1.0, true);
    checkFaceInside("live, host typical", live, 600.0f, 276.0f, 1.0, true);
    checkFaceInside("live, host maximum", live, 600.0f, 360.0f, 1.0, false);
    checkFaceInside("live, no memory", live, 600.0f, 460.0f, 1.0, false);
    checkFaceInside("cold, host typical", cold, 600.0f, 276.0f, 1.0, false);

    for (float w = 90.0f; w <= 1700.0f; w += 53.0f) {
        for (float h = 50.0f; h <= 800.0f; h += 41.0f) {
            checkFaceInside("live", live, w, h, 1.0, true);
            checkFaceInside("cold", cold, w, h, 1.3, false);
        }
    }
    // Both halves of the blink, because one of them draws a bloom that is
    // larger than the lens.
    checkFaceInside("blink on", live, 600.0f, 276.0f, 0.10, true);
    checkFaceInside("blink off", live, 600.0f, 276.0f, 0.40, true);
}

void testFaceSurvivesNonsense() {
    std::printf("\n[10] a rectangle with nothing in it, and slots full of nonsense\n");
    cascade::gui::InstrumentCue cue;
    cue.nowSec = 2.0;
    HostInstrument in = liveBeacon();

    // A zero-size rectangle, an inverted one, and a null draw list: each must
    // return without drawing and without crashing.
    float used = -1.0f;
    drawOne([&](ImDrawList* dl) {
        used = cascade::gui::drawBeaconFace(dl, ImVec2(50.0f, 50.0f), ImVec2(50.0f, 50.0f),
                                            in, cue);
    });
    CHECK(used == 0.0f);
    drawOne([&](ImDrawList* dl) {
        used = cascade::gui::drawBeaconFace(dl, ImVec2(300.0f, 300.0f),
                                            ImVec2(100.0f, 100.0f), in, cue);
    });
    CHECK(used == 0.0f);
    CHECK(cascade::gui::drawBeaconFace(nullptr, ImVec2(0.0f, 0.0f), ImVec2(600.0f, 300.0f),
                                       in, cue) == 0.0f);

    // The largest and the least a slot can hold, and a text with no terminator
    // to spare.
    in.state.values[0] = 1.0e300;
    in.state.values[1] = -1.0e300;
    for (int i = 0; i < CASCADE_INSTRUMENT_TEXTS; ++i) {
        std::memset(in.state.text[i], 'W', CASCADE_INSTRUMENT_TEXT_CHARS - 1);
        in.state.text[i][CASCADE_INSTRUMENT_TEXT_CHARS - 1] = '\0';
    }
    checkFaceInside("every slot at its limit", in, 600.0f, 276.0f, 1.0, true);
    checkFaceInside("every slot at its limit, narrow", in, 200.0f, 200.0f, 1.0, true);
    in.state.values[0] = std::nan("");
    in.state.values[1] = std::nan("");
    checkFaceInside("not a number in both figures", in, 600.0f, 276.0f, 1.0, true);
}

// AND THE ONE THING NO GEOMETRY CHECK CAN SEE: that a cold panel is not drawing
// a reading. The face's whole rule is that "no beacon heard" looks different
// from "a beacon transmitting zeroes", and both of those emit vertices inside
// the box. What separates them is that the LIT amber of a segment, the needle
// and the counter digits are absent - so the two frames are compared, and the
// cold one must be strictly the smaller drawing.
void testColdPanelDrawsLess() {
    std::printf("\n[11] no reading is drawn as no reading\n");
    cascade::gui::InstrumentCue cue;
    cue.nowSec = 0.10;  // the bright half of the blink
    cue.unread = false;
    const HostInstrument live = liveBeacon();
    HostInstrument cold;
    cold.title = live.title;
    cold.kind = CASCADE_INSTRUMENT_BEACON;
    cold.headings = live.headings;

    const ImVec2 tl(60.0f, 40.0f);
    const ImVec2 br(660.0f, 316.0f);
    int liveVerts = 0;
    int coldVerts = 0;
    drawOne([&](ImDrawList* dl) {
        const int mark = dl->VtxBuffer.Size;
        cascade::gui::drawBeaconFace(dl, tl, br, live, cue);
        liveVerts = dl->VtxBuffer.Size - mark;
    });
    drawOne([&](ImDrawList* dl) {
        const int mark = dl->VtxBuffer.Size;
        cascade::gui::drawBeaconFace(dl, tl, br, cold, cue);
        coldVerts = dl->VtxBuffer.Size - mark;
    });
    CHECK(liveVerts > 0);
    CHECK(coldVerts > 0);          // the panel is still a panel with no signal
    CHECK(coldVerts < liveVerts);  // but it is plainly showing less
    if (!(coldVerts < liveVerts)) {
        std::printf("      cold %d verts, live %d\n", coldVerts, liveVerts);
    }
    // And the cold panel still uses the same height, so the burst log beneath
    // it does not jump when the first burst arrives.
    float usedLive = 0.0f;
    float usedCold = 0.0f;
    drawOne([&](ImDrawList* dl) {
        usedLive = cascade::gui::drawBeaconFace(dl, tl, br, live, cue);
    });
    drawOne([&](ImDrawList* dl) {
        usedCold = cascade::gui::drawBeaconFace(dl, tl, br, cold, cue);
    });
    CHECK_NEAR(usedLive, usedCold, 0.01);
}

}  // namespace

int main() {
    testSegmentTable();
    testLayHexId();
    testFormatAge();
    testFormatError();
    testErrorFraction();
    testWarningLegend();
    testChipWord();
    testLayout();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2000.0f, 1200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    if (!cascade::gui::fonts::load()) {
        std::printf("  note: font atlas refused a face; measuring the fallback\n");
    }

    testFaceFitsEverySize();
    testFaceSurvivesNonsense();
    testColdPanelDrawsLess();

    ImGui::DestroyContext();
    return testSummary("test_instrument_beacon");
}
