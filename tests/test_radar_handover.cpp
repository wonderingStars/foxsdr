/*
 * HANDING THE DISPLAY TO THE RADAR UNIT MUST NOT LOOK LIKE A CRASH.
 *
 * WHY THIS EXISTS. The radar unit is a second program, and while it is open
 * FoxSDR gets out of its way. Until 0.76.0 it did that with glfwHideWindow,
 * which takes a window off the TASKBAR as well as off the screen - so opening
 * the radar removed every trace of the receiver at once. Measured on the
 * released 0.75.0: the visible windows went from "[Decoder output] [FoxSDR
 * 0.75.0]" to none, while the process stayed healthy and rendered 13,435 more
 * frames and exited 0. There is no way to tell that apart from a crash by
 * looking, and it was reported as one.
 *
 * The fix is one word - minimise instead of hide - and it is the kind of word
 * that gets changed back by somebody tidying up, because from inside the code
 * "hide the window we are not using" reads perfectly reasonable. So it is
 * pinned here.
 *
 * WHAT THIS CAN AND CANNOT CHECK. The handover is three GLFW calls against a
 * real operating-system window; a test without a window cannot make them
 * happen. What it CAN do is read the source of the two functions that own the
 * behaviour and require the properties that matter, which is the same thing
 * tests/test_shutdown_budget.cpp does for the bounded waits. That is a weaker
 * check than exercising it, and it is stated plainly rather than dressed up:
 * it catches the regression, not every possible way of breaking the handover.
 *
 * The end-to-end behaviour was verified by hand against the installed build -
 * open the radar, confirm FoxSDR is minimised rather than gone, click its
 * taskbar button, confirm the windows come back and the radar's next renewal
 * does not take them away again.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "test_check.hpp"

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The body of a function, from its signature to the closing brace at column
// zero. Crude on purpose: every function this file looks at is written in the
// house style, one per top-level block, so a brace in the first column is the
// end of it.
std::string bodyOf(const std::string& src, const std::string& signature) {
    const std::size_t start = src.find(signature);
    if (start == std::string::npos) { return {}; }
    const std::size_t end = src.find("\n}\n", start);
    if (end == std::string::npos) { return src.substr(start); }
    return src.substr(start, end - start);
}

}  // namespace

int main() {
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
    const std::string text = readFile(src);
    CHECK(!text.empty());

    // --- the handover itself -------------------------------------------------
    const std::string hold = bodyOf(text, "void AppWindow::setRadarHoldsDisplay(");
    CHECK(!hold.empty());

    // THE WHOLE BUG, in one assertion. A hidden main window has no taskbar
    // button and is indistinguishable from a process that died.
    CHECK(hold.find("glfwHideWindow(mainWindow_)") == std::string::npos);

    // ...and the thing it must do instead.
    CHECK(hold.find("glfwIconifyWindow(mainWindow_)") != std::string::npos);

    // Coming back is a restore, not just a show: a window that was minimised
    // stays minimised however many times it is shown.
    CHECK(hold.find("glfwRestoreWindow(mainWindow_)") != std::string::npos);

    // --- taking the display back by hand -------------------------------------
    // The taskbar button is only a real control if un-minimising is honoured.
    const std::string vis = bodyOf(text, "void AppWindow::applyRadarWindowVisibility(");
    CHECK(!vis.empty());
    CHECK(vis.find("GLFW_ICONIFIED") != std::string::npos);
    CHECK(vis.find("radarDisplayTakenBack_ = true") != std::string::npos);

    // ...and only if the radar's four-second renewal cannot immediately undo
    // it. Without this guard the user loses a fight they cannot see.
    CHECK(text.find("if (!radarDisplayTakenBack_) { setRadarHoldsDisplay(true); }") !=
          std::string::npos);

    // The hand-back lapses when the radar stops renewing, or the next radar a
    // user opens would be overruled by a decision they made about a previous
    // one.
    CHECK(text.find("radarDisplayTakenBack_ = false") != std::string::npos);

    // --- the radar is reachable from the rail --------------------------------
    // Reported missing twice: the scope was buried in the Decoders section and
    // the unit was reachable only from the Start menu.
    CHECK(text.find("void AppWindow::drawRadarSection()") != std::string::npos);
    CHECK(text.find("drawRadarSection();") != std::string::npos);
    CHECK(text.find("foxsdr-radar.exe") != std::string::npos);

    std::printf("radar handover: hide banned, iconify required, take-back honoured\n");
    return testSummary("test_radar_handover");
}
