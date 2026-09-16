// Tests for gui/plugin_store_view.hpp's moduleRowColumns() - the module
// list's card split between the kind tag, the text column (name, version,
// summary, maker/licence) and the action column (the FIT/UPDATE/FITTED key,
// the install word, the running state word).
//
// WHY THIS TEST EXISTS. At UI scale 2.0 on the docked tablet's real module
// list width (~712-727 px, not the desktop store's own 1280 px this row was
// first drawn for), the OLD formula's std::max(cascade::gui::px(120.0f),
// cw - kTagW - kActW - kCardPad * 3.0f) floor was ITSELF scaled - px(120.0f)
// is 240 device px, not 120 - and on a card narrower than the desktop's own
// that claimed comfort could exceed the room the tag and action columns
// actually left behind. Nothing clipped the difference: the row's own
// version/install line and its maker/licence line were drawn with no wrap or
// clip bound at all, so the text simply ran on past where the action column
// began and printed through "FITTED"/"INSTALLED"/"STARTED". moduleRowColumns()
// now answers honestly - `mx + midW` is always exactly `ax`, by construction,
// never a floor that can claim more than `cw` leaves once the tag and action
// columns have taken their own share - and the row's own drawing clips its
// single-line text (railPlateLabel's and centreDockTabLabel's own technique)
// to whatever midW that honestly is.
//
// A PURE FUNCTION OF `cw`. Font metrics come from the loaded typefaces
// (fonts::load() below), not from an open ImGui frame or a window size, so
// this is checked directly rather than through a full draw().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/ui_scale.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::gui::ModuleRowColumns;
using cascade::gui::moduleRowColumns;

namespace {

// THE PROPERTY: the text column ends exactly where the action column
// begins, never later, at ANY width - never overlapping, whatever `cw` the
// module list's own layout hands it.
void checkNoOverlap(float cw, const char* label) {
    const ModuleRowColumns c = moduleRowColumns(cw);
    CHECK(c.mx >= 0.0f);
    CHECK(c.midW >= 0.0f);
    CHECK(c.ax >= c.mx);
    CHECK_NEAR(c.mx + c.midW, c.ax, 0.02f);
    std::printf("  %-26s cw=%.1f mx=%.1f midW=%.1f ax=%.1f\n", label, cw, c.mx, c.midW, c.ax);
}

void testModuleRowColumnsNeverOverlap() {
    // THE DESKTOP STORE, at both scales it can actually run at. Measured
    // module-list content widths from a real draw() at the desktop's own
    // default open size and a maximised one.
    cascade::gui::setUiScale(1.0f);
    checkNoOverlap(827.0f, "desktop @1.0x");
    cascade::gui::setUiScale(2.0f);
    checkNoOverlap(1256.0f, "desktop @2.0x (equiv.)");

    // THE REAL DOCKED BODY'S OWN MODULE LIST WIDTH - read back from a real
    // draw() at the tablet's real body (1174x906, UI scale 2.0): the
    // module list's content width there is ~712-727 px depending on the
    // exact catalogue state, comfortably inside the range this test pins
    // both ends of.
    cascade::gui::setUiScale(2.0f);
    checkNoOverlap(712.0f, "real docked body @2.0x");
    checkNoOverlap(727.0f, "real docked body @2.0x (b)");

    cascade::gui::setUiScale(1.0f);
}

// ---------------------------------------------------------------------------
// ROUND 4: the row's over-correction. moduleRowColumns() stopped the text
// running under the action column, but at the docked tablet's real 712 px
// module list width x2 it did that by giving the action column
// moduleActionColumnWidth()'s own figure - sized from the WIDEST install
// word ("NOT INSTALLED", 13 characters) even on the common row whose key is
// just "FIT" (3) - which left the text column under a quarter of the card:
// every sample line clipped mid-word ("1.0.0 INST", "FoxSDR projec").
//
// ROUND 5: the coordinator's own Windows run of the round-4 fix (and-deck at
// 8992c91) found it still too narrow under Georgia specifically - 30.0% of
// the card, both sample lines still overrunning it - because the kind tag
// chip (moduleKindTagWidth(), a global worst-case max across all seven kind
// words, exactly the fault the action column had) was wider under Georgia
// than the narrowed action column by the time both were measured.
// moduleKindTagWidthNarrow() and moduleActionColumnWidthNarrowest() (this
// file's own fix, see plugin_store_view.cpp) are what closes it; this test
// measures the SAME two sample lines a screenshot review actually found
// clipped, under BOTH faces.
// ---------------------------------------------------------------------------
void testModuleRowTextColumnAtRealBodyIsWideEnough() {
    cascade::gui::setUiScale(2.0f);
    const float cw = 712.0f;
    const ModuleRowColumns c = moduleRowColumns(cw);
    const float ratio = c.midW / cw;
    std::printf("  real docked body @2.0x text column: midW=%.1f cw=%.1f ratio=%.1f%% narrow=%d\n",
                c.midW, cw, ratio * 100.0f, c.narrow ? 1 : 0);
    CHECK(c.narrow);

    // THE TWO SAMPLE LINES A SCREENSHOT REVIEW ACTUALLY FOUND CLIPPED,
    // measured at the exact fonts and size the row draws them at (see
    // plugin_store_view.cpp's version+install and maker/licence lines):
    // both in the ui face, "1.0.0" in the reading face, `prose()` sized -
    // `storeProsePx()` is exported for exactly this (see moduleKindTagWidth
    // and storeCheckKeyWidth's own tests for the same pattern).
    ImFont* uf = cascade::gui::fonts::ui();
    ImFont* rf = cascade::gui::fonts::reading();
    const float tiny = cascade::gui::px(cascade::gui::storeProsePx());
    const float versionW = rf->CalcTextSizeA(tiny, FLT_MAX, 0.0f, "1.0.0").x;
    const float instW = uf->CalcTextSizeA(tiny, FLT_MAX, 0.0f, "INSTALLED").x;
    const float line1W = versionW + 16.0f + instW;
    std::printf("  '1.0.0 INSTALLED' width=%.1f vs midW=%.1f\n", line1W, c.midW);
    CHECK(line1W <= c.midW);

    const float line2W =
        uf->CalcTextSizeA(tiny, FLT_MAX, 0.0f, "FoxSDR project \xc2\xb7 MIT").x;
    std::printf("  'FoxSDR project . MIT' width=%.1f vs midW=%.1f\n", line2W, c.midW);
    CHECK(line2W <= c.midW);

    // THE COORDINATOR'S OWN 35% TARGET, now comfortably cleared under BOTH
    // faces (measured: 68.3% under the embedded faces, 51.1% under Georgia)
    // once the kind tag chip narrowed alongside the action column and the
    // column gap between them tightened too (kColGapNarrow, px(4.0f) in
    // place of the wide px(14.0f) - see moduleRowColumns()'s own comment).
    CHECK(ratio >= 0.35f);

    cascade::gui::setUiScale(1.0f);
}

}  // namespace

namespace {

// ONE PASS: a fresh ImGui context, fonts::load() (which reads
// FOXSDR_SYSTEM_FONT_DIR / %WINDIR% exactly as the product does - see the
// note on fonts::systemFontPath()), the checks above, then torn down again
// so the next pass starts clean. Returns which face this pass measured
// against (fonts::usingSystemSerif()).
bool runOnePass(const char* label) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2000.0f, 1200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // THE LOAD ITSELF IS THE FIRST ASSERTION, exactly as test_bench_text_fits
    // does it: measuring against ImGui's own fallback face would tell us
    // nothing about what the product draws.
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);
    const bool serif = cascade::gui::fonts::usingSystemSerif();
    std::printf("  [%s] fonts::usingSystemSerif() = %d (%s)\n", label, serif ? 1 : 0,
                serif ? "Georgia" : "embedded Saira");

    if (loaded) {
        testModuleRowColumnsNeverOverlap();
        testModuleRowTextColumnAtRealBodyIsWideEnough();
    } else {
        std::printf("fonts::load() failed - the measurements below were not run\n");
    }

    ImGui::DestroyContext();
    return serif;
}

}  // namespace

int main() {
    // THE SEAM'S STARTING VALUE, COPIED - not just pointed at, because the
    // env-mutating calls below (setenv/_putenv_s) can invalidate the pointer
    // getenv() handed back.
    const char* seamRaw = std::getenv("FOXSDR_SYSTEM_FONT_DIR");
    const bool seamSetAtStart = seamRaw != nullptr && seamRaw[0] != '\0';
    const std::string seamAtStart = seamRaw != nullptr ? seamRaw : "";

    // PASS 1: whatever the environment already says - native Georgia on
    // Windows with no seam needed, the seam's own directory if set, or the
    // embedded Saira fallback otherwise.
    const bool pass1Serif = runOnePass("pass 1, native");

    // PASS 2: the OTHER face, run under BOTH faces whenever that is
    // reachable from here - the coordinator's own instruction, "like the
    // deck test does" (test_plugin_store_deck.cpp's own SKIPPED convention
    // when it genuinely cannot be reached).
    if (pass1Serif) {
        // FORCE THE EMBEDDED FALLBACK, on ANY platform including Windows:
        // point the seam at a directory that cannot hold georgia.ttf (the
        // current directory almost certainly does not), so
        // fonts::addSystem()'s own std::filesystem::exists() check fails and
        // load() falls back to the embedded Saira pair - see fonts.cpp. This
        // does not depend on the seam having been set for pass 1 at all:
        // native Windows Georgia (no seam) is forced back to Saira exactly
        // the same way.
#ifdef _WIN32
        _putenv_s("FOXSDR_SYSTEM_FONT_DIR", ".");
#else
        setenv("FOXSDR_SYSTEM_FONT_DIR", ".", 1);
#endif
        runOnePass("pass 2, embedded (forced)");
    } else if (seamSetAtStart) {
        // The seam was set but pass 1 still came back Saira - the named
        // directory does not actually hold georgia.ttf/georgiab.ttf.
        std::printf(
            "  SKIPPED pass 2: FOXSDR_SYSTEM_FONT_DIR is set but did not load Georgia - "
            "check the directory holds georgia.ttf and georgiab.ttf.\n");
    } else {
        std::printf(
            "  SKIPPED pass 2: no system serif loaded and FOXSDR_SYSTEM_FONT_DIR is not set - "
            "Georgia was not checked on this run. Set FOXSDR_SYSTEM_FONT_DIR to a directory "
            "holding georgia.ttf and georgiab.ttf to check it here, or run this binary on "
            "Windows.\n");
    }

    // RESTORE - this test's own environment tampering must not leak past it.
#ifdef _WIN32
    _putenv_s("FOXSDR_SYSTEM_FONT_DIR", seamAtStart.c_str());
#else
    if (seamSetAtStart) {
        setenv("FOXSDR_SYSTEM_FONT_DIR", seamAtStart.c_str(), 1);
    } else {
        unsetenv("FOXSDR_SYSTEM_FONT_DIR");
    }
#endif

    return testSummary("test_plugin_store_row");
}
