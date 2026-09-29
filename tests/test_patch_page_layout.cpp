// Tests for the patch page's layout rule (gui/patch_view_math.hpp, "the patch
// page's layout"): 0.99.49 beta feedback - on a small laptop the patch page's
// information column took a full-height strip of the canvas's width while the
// top right of the page sat empty. The information pane is now a fixed-height
// band at the top right; the canvas has the whole width below it unless a node
// is selected, when the inspector drawer holds that node's controls and can be
// folded to a strip. tests/test_patch_info_pane.cpp checks the real page.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>

#include "gui/patch_view_math.hpp"
#include "test_check.hpp"

namespace pv = cascade::gui::patch;

int main() {
    std::printf("test_patch_page_layout\n");

    // --- the information pane's width ------------------------------------------
    // A small laptop's page (1366 x 768 with the rail open): a share of it.
    CHECK_NEAR(pv::patchInfoPaneWidth(915.0f, 1.0f), 915.0f * pv::kPatchInfoShare, 0.01);
    // Never narrower than its floor, nor wider than its ceiling...
    CHECK_NEAR(pv::patchInfoPaneWidth(700.0f, 1.0f), pv::kPatchInfoMinW, 0.01);
    CHECK_NEAR(pv::patchInfoPaneWidth(3000.0f, 1.0f), pv::kPatchInfoMaxW, 0.01);
    // ...both of which grow with the interface size...
    CHECK_NEAR(pv::patchInfoPaneWidth(1000.0f, 1.5f), pv::kPatchInfoMinW * 1.5f, 0.01);
    CHECK_NEAR(pv::patchInfoPaneWidth(3000.0f, 2.0f), pv::kPatchInfoMaxW * 2.0f, 0.01);
    // ...and never more than half the page, whatever the floor says: the
    // transport, parts and decoder rows keep the other half.
    CHECK_NEAR(pv::patchInfoPaneWidth(400.0f, 1.0f), 200.0f, 0.01);
    CHECK(pv::patchInfoPaneWidth(0.0f, 1.0f) == 0.0f);

    // --- the inspector drawer ----------------------------------------------------
    CHECK(pv::patchInspectorWidth(false, false, 1.0f) == 0.0f);  // nothing selected: none
    CHECK(pv::patchInspectorWidth(false, true, 1.0f) == 0.0f);
    CHECK_NEAR(pv::patchInspectorWidth(true, false, 1.0f), pv::kPatchInspectorW, 0.01);
    CHECK_NEAR(pv::patchInspectorWidth(true, true, 1.0f), pv::kPatchInspectorFoldedW, 0.01);
    CHECK_NEAR(pv::patchInspectorWidth(true, false, 2.0f), pv::kPatchInspectorW * 2.0f, 0.01);

    // --- the canvas ---------------------------------------------------------------
    // THE WHOLE WIDTH when nothing is selected - the point of the change.
    CHECK_NEAR(pv::patchCanvasWidth(915.0f, 0.0f), 915.0f, 0.01);
    // Beside the drawer, less the drawer and one gap.
    CHECK_NEAR(pv::patchCanvasWidth(915.0f, 236.0f), 915.0f - 236.0f - pv::kPatchPaneGap, 0.01);
    CHECK_NEAR(pv::patchCanvasWidth(915.0f, 30.0f), 915.0f - 30.0f - pv::kPatchPaneGap, 0.01);
    // Never below its floor.
    CHECK_NEAR(pv::patchCanvasWidth(200.0f, 236.0f), pv::kPatchCanvasMinW, 0.01);

    return testSummary("test_patch_page_layout");
}
