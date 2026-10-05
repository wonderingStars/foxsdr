// A plugin rescan says, in the log, which of its steps took the time.
//
// THE FIELD LOG. A session of 0.99.58 (a Store package) logged
//     04:16:19.833 info plugins: <folder> (this is a Store package)
//     04:18:19.809 info plugin: loaded ...   (twenty of them, in 1 ms)
// 119.976 s apart. That is a plugin rescan, and it holds the window frozen for
// as long as it runs - the GUI thread calls every plugin's destroy(), unmaps
// every module, re-hashes the installed files, lists the folder and maps every
// module again, in that order, on its own thread. The log named none of those
// steps, so the two minutes could belong to a plugin that would not stop, to a
// disk, to the loader or to the network, and nobody could say which. Nothing in
// the source waits 120 s (there is no such constant on that path, in the
// application or in the plugins), so the figure is somebody else's timeout - and
// a freeze whose owner is unknown is a freeze nobody can fix.
//
// WHAT IS HELD, through the REAL AppWindow::rescanPlugins:
//   a. a rescan writes exactly ONE line of the shape
//      "plugins: reload took <n> s (decoders .., patch .., panels and map ..,
//      unload .., inventory .., load .., restart ..)", with the laps in the order
//      the work is done - the order is what makes "which step" answerable;
//   b. a second rescan writes a second line, and no more: the clock is the
//      rescan's own, not a member that accumulates across them;
//   c. the line is written on EVERY way out of the function - it has an early
//      return, taken after the plugins have been unloaded when the quarantine
//      cannot move a retired file - so the clock's report is a scope guard that
//      is in force before the first `return`, as DiagContextOnExit is (the
//      function's other exit-time duty);
//   d. detachAndUnloadPlugins is the one that times the first four laps, and its
//      other callers (the exit path, an installed plugin's removal, a stop and a
//      start) pass no clock and so write nothing.
// The clock itself is tests/test_phase_clock.cpp.
//
// Hermetic like test_diag_context_app: no config file, the per-user directories
// in a scratch folder, no USB walk, no real driver.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private rescan.
struct AppWindowTestAccess {
    static void rescan(AppWindow& a) { a.rescanPlugins(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::core::DiagLog;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const fs::path scratch =
        fs::temp_directory_path() / ("foxsdr_plugin_rescan_log_" + std::to_string(pid));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') { out += c; }
    }
    return out;
}

std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The lines of the log ring that announce a finished reload.
std::vector<std::string> reloadLines() {
    std::vector<std::string> out;
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find("plugins: reload took") != std::string::npos) { out.push_back(line); }
    }
    return out;
}

// Offsets of `needles` in `hay`, each searched for after the one before it, or
// npos at the first that is missing - the check that laps come in the order the
// work is done in.
bool inOrder(const std::string& hay, const std::vector<std::string>& needles) {
    std::size_t at = 0;
    for (const std::string& n : needles) {
        const std::size_t found = hay.find(n, at);
        if (found == std::string::npos) { return false; }
        at = found + n.size();
    }
    return true;
}

void testARescanSaysWhereItsTimeWent() {
    std::printf("a rescan writes one line naming its steps\n");
    cascade::gui::AppWindow app;  // its own start-up scan has already run

    DiagLog::instance().resetForTest();
    Access::rescan(app);
    std::vector<std::string> lines = reloadLines();
    CHECK(lines.size() == 1u);
    if (lines.size() != 1u) { return; }
    std::printf("  %s\n", lines[0].c_str());

    // a. the shape: one sentence, an ordinary line (a rescan of a quiet session
    //    is milliseconds, far under the frame threshold)...
    CHECK(lines[0].find(" info plugins: reload took ") != std::string::npos);
    CHECK(lines[0].find(" s (") != std::string::npos);
    // ...and every step of the work, in the order it is done in. The first four
    // belong to detachAndUnloadPlugins (stop what runs, stop the patch, stop the
    // panels and the map, unmap the modules), the next two to the rescan's own
    // disk work (re-hash and quarantine, then list and map), the last to what
    // follows (the log lines, the instances).
    CHECK(inOrder(lines[0], {"decoders ", "patch ", "panels and map ", "unload ", "inventory ",
                             "load ", "restart "}));

    // b. a second rescan, a second line - and no third.
    DiagLog::instance().resetForTest();
    Access::rescan(app);
    Access::rescan(app);
    lines = reloadLines();
    CHECK(lines.size() == 2u);
}

// c. and d. - the structure, because a rescan that returns early cannot be made
// to in a test: the quarantine only fails against a file the process may not
// rename, which a hermetic run has none of.
void testStructure() {
    std::printf("the report is made on every way out, and only the rescan asks for it\n");
    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    const std::string source = readText(gui / "app_window.cpp");
    CHECK(!source.empty());

    const std::string rescan = functionBody(source, "void AppWindow::rescanPlugins(");
    CHECK(!rescan.empty());
    const std::size_t clock = rescan.find("PhaseClock");
    const std::size_t guard = rescan.find("log(\"plugins: reload\"");
    const std::size_t firstReturn = rescan.find("return;");
    CHECK(clock != std::string::npos);
    CHECK(guard != std::string::npos);
    CHECK(firstReturn != std::string::npos);
    // The clock exists, and the guard that reports it is declared, BEFORE the
    // early return - a destructor declared after a `return` is not run by it.
    CHECK(clock < firstReturn);
    CHECK(guard < firstReturn);
    // The detach is handed the clock, so its four steps are timed.
    CHECK(rescan.find("detachAndUnloadPlugins(&") != std::string::npos);

    // Every OTHER caller of detachAndUnloadPlugins passes no clock: the exit path
    // must not write a "reload" line at every quit, and a removal or a stop is
    // not a reload.
    std::size_t withClock = 0;
    std::size_t withoutClock = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(gui)) {
        if (e.path().extension() != ".cpp") { continue; }
        const std::string text = readText(e.path());
        const std::string call = "detachAndUnloadPlugins(";
        for (std::size_t at = text.find(call); at != std::string::npos;
             at = text.find(call, at + 1)) {
            const std::size_t lineStart = text.rfind('\n', at) + 1;
            const std::string before = text.substr(lineStart, at - lineStart);
            // The definition, and a mention in a comment, are not call sites.
            if (before.find("AppWindow::") != std::string::npos ||
                before.find("//") != std::string::npos) {
                continue;
            }
            if (text.compare(at + call.size(), 1, "&") == 0) {
                ++withClock;
            } else {
                ++withoutClock;
            }
        }
    }
    std::printf("  detachAndUnloadPlugins call sites: %zu with the clock, %zu without\n",
                withClock, withoutClock);
    CHECK(withClock == 1u);
    CHECK(withoutClock >= 3u);
}

}  // namespace

int main() {
    std::printf("test_plugin_rescan_log\n");
    isolate();
    testARescanSaysWhereItsTimeWent();
    testStructure();
    return testSummary("test_plugin_rescan_log");
}
