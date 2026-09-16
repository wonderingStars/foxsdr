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

}  // namespace

int main() {
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

    if (loaded) {
        testModuleRowColumnsNeverOverlap();
    } else {
        std::printf("fonts::load() failed - the measurements below were not run\n");
    }

    ImGui::DestroyContext();
    return testSummary("test_plugin_store_row");
}
