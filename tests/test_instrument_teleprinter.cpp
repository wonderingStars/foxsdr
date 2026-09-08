// The cockpit printer face's arithmetic: how many characters fit across the
// paper, what the printed heading says, how a message wraps on the strip, how
// far the sheet has fed, and the word the rail's chip carries.
//
// WHAT IS PINNED HERE IS WHAT CAN BE WRONG WITHOUT LOOKING WRONG. A heading
// that quietly drops the flight number, a wrap that swallows a waypoint, a
// feed that never settles because the clock went backwards, a counter word
// that reads "0 MSG" when nothing has been measured - none of those show in a
// screenshot, and every one of them is a lie printed on a piece of paper.
//
// THE DRAWING IS TESTED TOO, but only for the one thing a screenshot cannot
// prove: that it FITS. The window is user-resizable, and the third rule every
// face obeys is that a face given less room draws less rather than drawing
// over the edge - so the face is rendered headless through a real Dear ImGui
// context at a ladder of sizes, from smaller than it can usefully draw to
// larger than any monitor, and every vertex it emits is required to be inside
// the rectangle it was handed. That is the tests/test_scope_face.cpp pattern
// and the reason it exists: no GL, no window, ImGui builds its vertex buffers
// on the CPU and those are what is measured.
//
// instrumentChip is tested directly, because it touches no ImGui - it is
// snprintf over a struct - and it is what the rail shows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "gui/fonts.hpp"
#include "gui/instrument_face.hpp"
#include "gui/instrument_teleprinter_math.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace {

namespace tp = cascade::gui::teleprinter;
using cascade::core::HostInstrument;

// The line at `i`, or "" when the call produced fewer - so an assertion on a
// line that was never written FAILS with an empty string rather than reading
// uninitialised storage. (The harness records and continues, so `lines[i]`
// guarded only by a preceding CHECK on the count is an out-of-bounds read in
// exactly the run that has something to report.)
std::string at(const tp::PaperLine* lines, int count, int i) {
    if (lines == nullptr || i < 0 || i >= count) { return std::string(); }
    return std::string(lines[i].text);
}

HostInstrument makeInstrument() {
    HostInstrument h;
    h.plugin = "acars-decoder";
    h.title = "ACARS printer";
    h.kind = CASCADE_INSTRUMENT_TELEPRINTER;
    h.have = true;
    h.state.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
    h.state.seq = 4u;
    std::snprintf(h.state.text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "G-EZBX");
    h.state.values[0] = 137.0;
    return h;
}

void addRow(HostInstrument& h, const char* a, const char* b, const char* c, const char* d) {
    CascadePanelRow r{};
    r.kind = CASCADE_ROW_CELLS;
    const char* cells[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        std::snprintf(r.cells[i], CASCADE_PANEL_CELL_CHARS, "%s", cells[i]);
    }
    h.rows.push_back(r);
}

// A full instrument: header slots, a counter, and a paper history with a
// newest and a previous message, which is the state that puts the most on the
// strip and therefore the most at risk of running off it.
HostInstrument loadedInstrument() {
    HostInstrument h = makeInstrument();
    std::snprintf(h.state.text[1], CASCADE_INSTRUMENT_TEXT_CHARS, "EZY83U");
    std::snprintf(h.state.text[2], CASCADE_INSTRUMENT_TEXT_CHARS, "H1");
    std::snprintf(h.state.text[3], CASCADE_INSTRUMENT_TEXT_CHARS, "2");
    std::snprintf(h.state.text[4], CASCADE_INSTRUMENT_TEXT_CHARS, "4");
    h.headings = {"Time", "Reg", "Flight", "Text"};
    addRow(h, "14:31:05", "EI-DWA", "RYR4MK", "POS N5340 W00145 FL360 M78");
    addRow(h, "14:31:50", "G-EZBX", "EZY83U", "ETA EGNM 1455 GATE 12 FUEL 4.1T");
    return h;
}

// --- headless geometry ------------------------------------------------------
//
// Everything ONE call drew, and nothing else's. The window is deliberately far
// bigger than the rectangle under test: ImGui culls glyphs against the current
// clip rectangle as it emits them, so a face overflowing inside a tight window
// would be trimmed by the window and the overflow would never reach the vertex
// buffer - the test would pass by being unable to see the fault.
struct Extent {
    int verts = 0;
    bool bad = false;  // a NaN or an infinity anywhere in the range
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    float used = 0.0f;
};

Extent drawFaceInto(const HostInstrument& in, const cascade::gui::InstrumentCue& cue,
                    const ImVec2& tl, const ImVec2& br) {
    Extent e;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(2600.0f, 1800.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("bench", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Anti-aliased lines drawn through the atlas carry a glyph-like UV; the
    // extent does not care, but clearing it keeps this reading the geometry
    // rather than the renderer's shortcut.
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    const int mark = dl->VtxBuffer.Size;
    e.used = cascade::gui::drawTeleprinterFace(dl, tl, br, in, cue);
    for (int i = mark; i < dl->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y)) {
            e.bad = true;
            continue;
        }
        if (e.verts == 0) {
            e.x0 = e.x1 = v.pos.x;
            e.y0 = e.y1 = v.pos.y;
        } else {
            e.x0 = (v.pos.x < e.x0) ? v.pos.x : e.x0;
            e.x1 = (v.pos.x > e.x1) ? v.pos.x : e.x1;
            e.y0 = (v.pos.y < e.y0) ? v.pos.y : e.y0;
            e.y1 = (v.pos.y > e.y1) ? v.pos.y : e.y1;
        }
        ++e.verts;
    }
    ImGui::End();
    ImGui::Render();
    return e;
}

// Half a pixel of anti-aliasing on a filled edge and a pixel of the shadow
// pass a cut letter sits over. A face that does not fit overflows by tens of
// pixels, so this cannot hide the fault it is looking for.
constexpr float kSlack = 2.0f;

void checkFits(const char* what, const HostInstrument& in,
               const cascade::gui::InstrumentCue& cue, float w, float h) {
    const ImVec2 tl(200.0f, 150.0f);
    const ImVec2 br(tl.x + w, tl.y + h);
    const Extent e = drawFaceInto(in, cue, tl, br);
    const bool ok = !e.bad && (e.verts == 0 ||
                               (e.x0 >= tl.x - kSlack && e.x1 <= br.x + kSlack &&
                                e.y0 >= tl.y - kSlack && e.y1 <= br.y + kSlack));
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at %.0fx%.0f: drew x %.1f..%.1f y %.1f..%.1f, box "
                    "%.1f..%.1f / %.1f..%.1f (%d verts%s)\n",
                    what, static_cast<double>(w), static_cast<double>(h),
                    static_cast<double>(e.x0), static_cast<double>(e.x1),
                    static_cast<double>(e.y0), static_cast<double>(e.y1),
                    static_cast<double>(tl.x), static_cast<double>(br.x),
                    static_cast<double>(tl.y), static_cast<double>(br.y), e.verts,
                    e.bad ? ", NON-FINITE" : "");
    }
    // The height it reports is what the host lays the memory table beneath, so
    // a face that claimed more than it was given would push the table off the
    // window.
    const bool heightOk = e.used >= 0.0f && e.used <= h + kSlack;
    CHECK(heightOk);
    if (!heightOk) {
        std::printf("      %s at %.0fx%.0f: returned height %.1f for a %.0f box\n", what,
                    static_cast<double>(w), static_cast<double>(h),
                    static_cast<double>(e.used), static_cast<double>(h));
    }
}

}  // namespace

int main() {
    // ================================================================
    // 1. HOW MUCH PAPER THERE IS.
    // ================================================================
    CHECK(tp::paperColumns(300.0f, 10.0f) == 30);
    CHECK(tp::paperColumns(305.0f, 10.0f) == 30);  // a partial column is not a column
    CHECK(tp::paperColumns(9.0f, 10.0f) == 0);     // narrower than one character
    CHECK(tp::paperColumns(10.0f, 10.0f) == 1);
    // A zero-size rectangle must not divide by zero, and must print nothing.
    CHECK(tp::paperColumns(0.0f, 10.0f) == 0);
    CHECK(tp::paperColumns(300.0f, 0.0f) == 0);
    CHECK(tp::paperColumns(-300.0f, 10.0f) == 0);
    CHECK(tp::paperColumns(300.0f, -10.0f) == 0);
    CHECK(tp::paperColumns(std::numeric_limits<float>::quiet_NaN(), 10.0f) == 0);
    // The machine's own width is the ceiling however wide the window gets.
    CHECK(tp::paperColumns(100000.0f, 1.0f) == tp::kMaxPaperCols);

    // ================================================================
    // 2. THE PRINTED HEADING, from the kind's slot map.
    // ================================================================
    {
        tp::PaperLine l[4];
        const int n = tp::paperHeading("G-EZBX", "EZY83U", "H1", "2", "4", 40, l, 4);
        CHECK(n == 2);
        CHECK(at(l, n, 0) == "REG G-EZBX  FLT EZY83U  LBL H1  MODE 2");
        CHECK(at(l, n, 1) == "BLK 4");
        CHECK(std::strlen(l[0].text) <= 40u);
    }
    {
        // A wide strip takes the whole header on one line.
        tp::PaperLine l[4];
        const int n = tp::paperHeading("G-EZBX", "EZY83U", "H1", "2", "4", 60, l, 4);
        CHECK(n == 1);
        CHECK(at(l, n, 0) == "REG G-EZBX  FLT EZY83U  LBL H1  MODE 2  BLK 4");
    }
    {
        // AN EMPTY SLOT PRINTS NOTHING. An uplink block carries no flight
        // number, and "FLT -" on the paper would be a field the aircraft
        // never sent.
        tp::PaperLine l[4];
        const int n = tp::paperHeading("G-EZBX", "", "SA", "2", "", 40, l, 4);
        CHECK(n == 1);
        CHECK(at(l, n, 0) == "REG G-EZBX  LBL SA  MODE 2");
    }
    {
        // NO SLOTS FILLED IS NO HEADING - the face draws blank paper, not a
        // row of captions with nothing after them.
        tp::PaperLine l[4];
        CHECK(tp::paperHeading("", "", "", "", "", 40, l, 4) == 0);
        CHECK(tp::paperHeading(nullptr, nullptr, nullptr, nullptr, nullptr, 40, l, 4) == 0);
    }
    {
        // A strip too narrow for even one field cuts the field off at the
        // edge of the paper rather than over it.
        tp::PaperLine l[4];
        const int n = tp::paperHeading("G-EZBX", "EZY83U", "", "", "", 6, l, 4);
        CHECK(n == 2);
        CHECK(at(l, n, 0) == "REG G-");
        CHECK(at(l, n, 1) == "FLT EZ");
        for (int i = 0; i < n; ++i) {
            CHECK(std::strlen(l[i].text) <= 6u);
        }
    }
    {
        // maxLines is a hard stop; nothing is written past it.
        tp::PaperLine l[2];
        l[1].text[0] = '\0';
        const int n = tp::paperHeading("G-EZBX", "EZY83U", "H1", "2", "4", 12, l, 1);
        CHECK(n == 1);
        CHECK(at(l, n, 0) == "REG G-EZBX");
        CHECK(l[1].text[0] == '\0');
    }
    {
        // THE LARGEST THE SLOTS CAN HOLD: 63 characters in every one of the
        // five. Nothing may run past the paper's own width.
        char big[CASCADE_INSTRUMENT_TEXT_CHARS];
        for (int i = 0; i < CASCADE_INSTRUMENT_TEXT_CHARS - 1; ++i) { big[i] = 'W'; }
        big[CASCADE_INSTRUMENT_TEXT_CHARS - 1] = '\0';
        CHECK(std::strlen(big) == 63u);
        tp::PaperLine l[tp::kMaxPaperLines];
        const int n = tp::paperHeading(big, big, big, big, big, 40, l, tp::kMaxPaperLines);
        CHECK(n == 5);
        for (int i = 0; i < n; ++i) { CHECK(std::strlen(l[i].text) <= 40u); }
        // A slot value longer than one field token can hold is cut at the
        // token buffer, which is still inside the paper's own width.
        CHECK(at(l, n, 0) == std::string("REG ") + std::string(35, 'W'));
        // And at the machine's full width, still inside the line buffer.
        const int m = tp::paperHeading(big, big, big, big, big, tp::kMaxPaperCols * 4, l,
                                       tp::kMaxPaperLines);
        CHECK(m == 5);
        for (int i = 0; i < m; ++i) {
            CHECK(std::strlen(l[i].text) <= static_cast<std::size_t>(tp::kMaxPaperCols));
        }
    }

    // ================================================================
    // 3. THE MESSAGE ON THE PAPER.
    // ================================================================
    {
        tp::PaperLine l[6];
        const int n = tp::wrapPaper("POS N5340 W00145 FL360 M78", 16, l, 6);
        CHECK(n == 2);
        CHECK(at(l, n, 0) == "POS N5340 W00145");
        CHECK(at(l, n, 1) == "FL360 M78");
        // Nothing is lost: every word is still there, in order.
        CHECK(at(l, n, 0) + " " + at(l, n, 1) == "POS N5340 W00145 FL360 M78");
    }
    {
        // A word longer than the paper is BROKEN, not dropped: an ACARS free
        // text field carries unbroken route strings and losing one loses the
        // message.
        tp::PaperLine l[6];
        const int n = tp::wrapPaper("EGLLDET2JDETUL9KONANORTA5EGNM", 10, l, 6);
        CHECK(n == 3);
        CHECK(at(l, n, 0) == "EGLLDET2JD");
        CHECK(at(l, n, 1) == "ETUL9KONAN");
        CHECK(at(l, n, 2) == "ORTA5EGNM");
    }
    {
        tp::PaperLine l[6];
        CHECK(tp::wrapPaper("", 20, l, 6) == 0);
        CHECK(tp::wrapPaper(nullptr, 20, l, 6) == 0);
        CHECK(tp::wrapPaper("   ", 20, l, 6) == 0);
        CHECK(tp::wrapPaper("HELLO", 0, l, 6) == 0);
        CHECK(tp::wrapPaper("HELLO", 20, l, 0) == 0);
        CHECK(tp::wrapPaper("HELLO", 20, nullptr, 6) == 0);
    }
    {
        // AN OVERLONG MESSAGE - the 220 characters ACARS allows - fills the
        // paper it has and stops. It is not truncated silently to one line
        // and it does not run off the strip.
        std::string long220;
        for (int i = 0; i < 22; ++i) { long220 += "WAYPOINT10"; }
        CHECK(long220.size() == 220u);
        tp::PaperLine l[4];
        const int n = tp::wrapPaper(long220.c_str(), 30, l, 4);
        CHECK(n == 4);
        for (int i = 0; i < n; ++i) { CHECK(std::strlen(l[i].text) <= 30u); }
        CHECK(at(l, n, 0) == "WAYPOINT10WAYPOINT10WAYPOINT10");
    }

    // ================================================================
    // 4. THE FEED - the paper coming out of the slot.
    // ================================================================
    {
        // At the instant of the message the sheet is fully inside the
        // machine; half a second later it is out and stays out.
        CHECK_NEAR(tp::paperFeedOffset(0.0, 0.5, 26.0f), -26.0f, 1e-4);
        CHECK(tp::paperFeedOffset(0.5, 0.5, 26.0f) == 0.0f);
        CHECK(tp::paperFeedOffset(9.0, 0.5, 26.0f) == 0.0f);
        CHECK_NEAR(tp::paperFeedOffset(0.25, 0.5, 26.0f), -13.0f, 1e-4);
        // Monotone: the paper never goes back into the machine.
        float prev = tp::paperFeedOffset(0.0, 0.5, 26.0f);
        for (int i = 1; i <= 50; ++i) {
            const float v = tp::paperFeedOffset(i * 0.01, 0.5, 26.0f);
            CHECK(v >= prev - 1e-5f);
            CHECK(v <= 0.0f);
            CHECK(v >= -26.0f);
            prev = v;
        }
        CHECK(prev == 0.0f);
        // Nonsense settles rather than animating forever.
        CHECK(tp::paperFeedOffset(-1.0, 0.5, 26.0f) == 0.0f);
        CHECK(tp::paperFeedOffset(std::numeric_limits<double>::quiet_NaN(), 0.5, 26.0f) ==
              0.0f);
        CHECK(tp::paperFeedOffset(0.1, 0.0, 26.0f) == 0.0f);
        CHECK(tp::paperFeedOffset(0.1, -1.0, 26.0f) == 0.0f);
        CHECK(tp::paperFeedOffset(0.1, 0.5, 0.0f) == 0.0f);
        CHECK(tp::paperFeedOffset(0.1, 0.5, -5.0f) == 0.0f);
    }

    // ================================================================
    // 5. THE WORD ON THE RAIL, through the host's own instrumentChip.
    // ================================================================
    {
        char chip[16];
        HostInstrument h = makeInstrument();

        // NEW beats everything while an event has not been shown.
        cascade::gui::instrumentChip(h, true, chip, sizeof chip);
        CHECK(std::string(chip) == "NEW");

        // Idle: the registration, because that says WHAT was heard.
        cascade::gui::instrumentChip(h, false, chip, sizeof chip);
        CHECK(std::string(chip) == "G-EZBX");

        // No station slot: the count is the fallback.
        h.state.text[0][0] = '\0';
        cascade::gui::instrumentChip(h, false, chip, sizeof chip);
        CHECK(std::string(chip) == "137 MSG");

        // NOTHING MEASURED IS NOT A COUNT OF NOTHING. With have=false the
        // chip must say it is waiting, never "0 MSG".
        h.have = false;
        cascade::gui::instrumentChip(h, false, chip, sizeof chip);
        CHECK(std::string(chip) == "WAIT");
        CHECK(std::string(chip) != "0 MSG");
    }
    {
        // The pure helper's own boundaries, which instrumentChip inherits.
        char out[16];
        tp::chipWord("", 0.0, out, sizeof out);
        CHECK(std::string(out) == "0 MSG");
        tp::chipWord(nullptr, 3.0, out, sizeof out);
        CHECK(std::string(out) == "3 MSG");
        // Clamped, never wrapped: a counter shown more than it can say must
        // read as high as it goes, not the low digits of the truth.
        tp::chipWord("", 1.0e12, out, sizeof out);
        CHECK(std::string(out) == "99999 MSG");
        tp::chipWord("", -5.0, out, sizeof out);
        CHECK(std::string(out) == "0 MSG");
        tp::chipWord("", std::numeric_limits<double>::quiet_NaN(), out, sizeof out);
        CHECK(std::string(out) == "0 MSG");
        // A registration longer than the chip is cut, not overrun.
        char small[8];
        tp::chipWord("G-ABCDEFGH", 0.0, small, sizeof small);
        CHECK(std::string(small) == "G-ABCDE");
        CHECK(small[7] == '\0');
        // A refusing buffer must not be written to.
        tp::chipWord("G-EZBX", 1.0, nullptr, 16);
        tp::chipWord("G-EZBX", 1.0, out, 0);
    }

    // ================================================================
    // 6. THE FACE ITSELF REFUSES AN IMPOSSIBLE RECTANGLE.
    //
    // No draw list and no room are both legal calls - the window is
    // user-resizable and can be dragged to nothing - and both must return a
    // used height of zero rather than dereference or draw over the edge.
    // ================================================================
    {
        const HostInstrument h = makeInstrument();
        cascade::gui::InstrumentCue cue;
        cue.nowSec = 12.5;
        CHECK(cascade::gui::drawTeleprinterFace(nullptr, ImVec2(0, 0), ImVec2(600, 400), h,
                                                cue) == 0.0f);
        CHECK(cascade::gui::drawTeleprinterFace(nullptr, ImVec2(0, 0), ImVec2(0, 0), h, cue) ==
              0.0f);
    }

    // ================================================================
    // 7. IT FITS THE RECTANGLE IT IS GIVEN, at every size.
    //
    // The window is user-resizable, so this is not a hypothetical: the
    // control column is dropped before the paper is squeezed, the type on the
    // strip shrinks to a floor, and the strip is clipped to its own bay.
    // Every vertex must land inside the box, and the height reported back -
    // which is where the host puts the memory table - must not exceed it.
    // ================================================================
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(2600.0f, 1800.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        // 1.92 lets the backend own texture uploads; saying so is what makes a
        // context with no renderer behind it legal for a frame.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        // The real typefaces at the real sizes, because how wide a line of
        // print comes out is the whole subject. A refusal is not a failure of
        // these checks - every face falls back to the bound one and the
        // fitting arithmetic is unchanged - so it is reported and the run
        // continues.
        if (!cascade::gui::fonts::load()) {
            std::printf("  note: font atlas refused a face; measuring the fallback\n");
        }

        const HostInstrument loaded = loadedInstrument();
        HostInstrument empty = makeInstrument();
        empty.have = false;
        empty.state.text[0][0] = '\0';
        empty.state.values[0] = 0.0;

        cascade::gui::InstrumentCue cue;
        cue.nowSec = 100.25;  // mid-blink, so the ALERT lamp is drawn lit
        cue.unread = true;

        // The size the host opens the window at, then a ladder either side of
        // it: below where the face can draw anything at all, through the sizes
        // that drop the control column, up past any monitor.
        const float sizes[][2] = {{600.0f, 460.0f},  {600.0f, 240.0f}, {580.0f, 190.0f},
                                  {420.0f, 200.0f},  {330.0f, 160.0f}, {260.0f, 140.0f},
                                  {200.0f, 120.0f},  {140.0f, 90.0f},  {96.0f, 70.0f},
                                  {81.0f, 61.0f},    {79.0f, 59.0f},   {40.0f, 400.0f},
                                  {900.0f, 120.0f},  {1400.0f, 900.0f},
                                  {3000.0f, 1600.0f}};
        for (const auto& sz : sizes) {
            checkFits("loaded", loaded, cue, sz[0], sz[1]);
            checkFits("no reading", empty, cue, sz[0], sz[1]);
        }

        // A ZERO-SIZE AND AN INVERTED RECTANGLE MUST NOT CRASH and must draw
        // nothing at all - a window dragged to nothing still gets a frame.
        {
            const ImVec2 o(200.0f, 150.0f);
            CHECK(drawFaceInto(loaded, cue, o, o).verts == 0);
            CHECK(drawFaceInto(loaded, cue, o, ImVec2(o.x - 100.0f, o.y - 100.0f)).verts == 0);
            CHECK(drawFaceInto(loaded, cue, o, ImVec2(o.x + 600.0f, o.y)).verts == 0);
        }

        // The feed settles: drawn again a second after the same event, the
        // paper is no longer moving, so two frames a long way apart with the
        // same seq must put the print in the same place.
        {
            const ImVec2 tl(200.0f, 150.0f);
            const ImVec2 br(tl.x + 600.0f, tl.y + 300.0f);
            cascade::gui::InstrumentCue a;
            a.nowSec = 500.0;
            const Extent first = drawFaceInto(loaded, a, tl, br);
            a.nowSec = 502.0;
            const Extent settled1 = drawFaceInto(loaded, a, tl, br);
            a.nowSec = 509.0;
            const Extent settled2 = drawFaceInto(loaded, a, tl, br);
            CHECK(first.verts > 0);
            CHECK(settled1.verts == settled2.verts);
            CHECK_NEAR(settled1.y1, settled2.y1, 0.01);
        }

        ImGui::DestroyContext();
    }

    return testSummary("test_instrument_teleprinter");
}
