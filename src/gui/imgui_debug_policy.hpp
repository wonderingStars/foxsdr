// imgui_debug_policy.hpp - when Dear ImGui's "Programmer error: N visible items with conflicting
// ID!" box may be drawn, and why it is not in a shipped build.
//
// THE REPORT (a US tester, 2026-10-13, 0.99.75). The DECODE bank listed two TETRA sections and
// ImGui's red box was open over the waterfall, outlining both sections' "+ Save" keys. Dear ImGui
// 1.92.8 turns io.ConfigDebugHighlightIdConflicts ON by default, and the box it draws says so
// itself: "Set io.ConfigDebugHighlightIdConflicts=false to disable this warning in non-programmers
// builds." The application never set it, so a user saw a message addressed to a programmer, over
// the picture, for a fault (two loaded copies of one plugin) the host now resolves on its own.
//
// ON in a Debug build, where a duplicate widget id is a defect to be seen. ON in a bounded
// --frames run that has a test hook up (FOXSDR_UI_CENSUS or FOXSDR_INPUT_SCRIPT), so a test can
// still surface a conflict: the application notes `imgui:idconflict` in the census when ImGui
// reports one. OFF everywhere else, which is every shipped run. An interactive session is never
// switched by a stray variable: the hooks count only in a bounded run, like every other hook here.
//
// PURE, so tests/test_imgui_debug_policy.cpp can walk the whole table without a window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

// WHY the box is on, in words for the log, or "" when it is off.
//   debugBuild   the build has assertions on (no NDEBUG);
//   frames       the --frames count of a bounded run, negative for an interactive session;
//   censusOn     FOXSDR_UI_CENSUS names a file;
//   scriptSet    FOXSDR_INPUT_SCRIPT names a file.
inline const char* idConflictBoxReason(bool debugBuild, int frames, bool censusOn, bool scriptSet) {
    if (debugBuild) { return "a debug build"; }
    if (frames >= 0 && (censusOn || scriptSet)) { return "a test hook is up"; }
    return "";
}

}  // namespace cascade::gui
