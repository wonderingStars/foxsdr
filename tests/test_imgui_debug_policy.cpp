/*
 * DEAR IMGUI'S "PROGRAMMER ERROR" BOX STAYS OUT OF SHIPPED BUILDS (0.99.77).
 *
 * THE REPORT (a US tester, 2026-10-13, 0.99.75): "Programmer error: 2 visible items with
 * conflicting ID!" in red over the waterfall, outlining the "+ Save" keys of two TETRA sections.
 * Dear ImGui turns io.ConfigDebugHighlightIdConflicts ON by default and the box says itself that
 * it is meant to be switched off "in non-programmers builds". The decision is the pure function
 * in gui/imgui_debug_policy.hpp; this walks its whole table:
 *
 *   a shipped run                    -> off (the answer the field report is about)
 *   an interactive run with a hook   -> off (a stray variable never switches it on)
 *   a bounded run with a hook up     -> on, so a test can still surface a conflict
 *   a Debug build                    -> on
 *
 * The call site in AppWindow::run is proved by tests/test_plugin_dup_app.cpp, which reads the
 * log line it writes and the census item `imgui:idconflict`.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <string>

#include "gui/imgui_debug_policy.hpp"
#include "test_check.hpp"

using cascade::gui::idConflictBoxReason;

int main() {
    std::printf("test_imgui_debug_policy\n");

    // A SHIPPED RUN: not a debug build, no bounded run, no hook. Off.
    CHECK(std::string(idConflictBoxReason(false, -1, false, false)).empty());
    // A bounded run with nothing up (what app_smoke is): off.
    CHECK(std::string(idConflictBoxReason(false, 3, false, false)).empty());

    // AN INTERACTIVE SESSION IS NEVER SWITCHED BY A STRAY VARIABLE: the hooks count in a bounded run only.
    CHECK(std::string(idConflictBoxReason(false, -1, true, false)).empty());
    CHECK(std::string(idConflictBoxReason(false, -1, false, true)).empty());
    CHECK(std::string(idConflictBoxReason(false, -1, true, true)).empty());

    // A BOUNDED RUN WITH A HOOK UP: on, with the reason in words for the log.
    CHECK(std::string(idConflictBoxReason(false, 0, true, false)) == "a test hook is up");
    CHECK(std::string(idConflictBoxReason(false, 40, false, true)) == "a test hook is up");
    CHECK(std::string(idConflictBoxReason(false, 40, true, true)) == "a test hook is up");

    // A DEBUG BUILD: on, whatever else.
    CHECK(std::string(idConflictBoxReason(true, -1, false, false)) == "a debug build");
    CHECK(std::string(idConflictBoxReason(true, 40, true, true)) == "a debug build");

    return testSummary("test_imgui_debug_policy");
}
