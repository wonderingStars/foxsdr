// The window's last-known state in a freeze report.
//
// WHY THIS EXISTS. Field report 6A586D4DAD464BB8 (0.99.73, a Raspberry Pi 5): the
// GUI thread spent 142 s, 257 s and 70 s inside the buffer swap and recovered each
// time. "In present for minutes" reads two ways - the graphics driver stalled, or
// the window system stopped scheduling a window nobody could see - and the report
// carried nothing that could tell them apart. The frame loop now publishes the
// window's visible / minimised / focused state each frame (core/diag_report.hpp,
// publishWindowState) and every freeze report prints it with its age.
//
// WHAT THIS HOLDS:
//   - before anything is published the line says `unknown`, rather than a state
//     nobody saw;
//   - the wording is fixed plain words, and the age is whole seconds before the
//     report (pinned against an explicit clock, so no test waits 142 s);
//   - the renderer never overruns a small buffer;
//   - a REAL stall, filed by the REAL watchdog thread, leaves the line in the
//     context section of the report - between `--- context ---` and
//     `--- modules ---`, not in the header the inventory covers - with the state
//     the "frame loop" last published and an age at least as long as the stall had
//     run when the report was filed (the state stopped updating when the stall
//     began, which is the signal);
//   - a window that is HIDDEN and MINIMISED does not change whether a report is
//     filed: the state is recorded and never acted on.
//
// The frame loop's own publication (gui/app_window.cpp) is not exercised here: it
// needs a window. This test publishes the way the loop does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "core/diag_report.hpp"
#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-winstate-") + tag + "-" + std::to_string(pid));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

std::string render() {
    char buf[256];
    const std::size_t n = renderWindowStateLine(buf, sizeof(buf));
    return std::string(buf, n);
}

// The frame loop's side: beat the watchdog AND publish, every 10 ms, for `ms`.
void frameLoopFor(HangWatchdog& w, unsigned ms, bool visible, bool iconified, bool focused) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        w.heartbeat();
        publishWindowState(visible, iconified, focused);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// The section of `report` between the `--- context ---` and `--- modules ---`
// markers, or empty.
std::string contextSection(const std::string& report) {
    const std::size_t a = report.find("--- context ---\n");
    const std::size_t b = report.find("--- modules ---");
    if (a == std::string::npos || b == std::string::npos || b < a) { return std::string(); }
    return report.substr(a, b - a);
}

// The `window-state:` line of a section, without its newline, or empty.
std::string windowStateLine(const std::string& section) {
    const std::size_t at = section.find("\nwindow-state: ");
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = section.find('\n', at + 1);
    return section.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
}

// The whole seconds in "..., updated N s before this report", or -1.
long ageSeconds(const std::string& line) {
    const char* key = ", updated ";
    const std::size_t at = line.find(key);
    if (at == std::string::npos) { return -1; }
    const std::size_t from = at + std::strlen(key);
    if (from >= line.size() || line[from] < '0' || line[from] > '9') { return -1; }
    char* end = nullptr;
    const long n = std::strtol(line.c_str() + from, &end, 10);
    if (std::string(end).compare(0, std::strlen(" s before this report"),
                                 " s before this report") != 0) {
        return -1;
    }
    return n;
}

}  // namespace

int main() {
    // --- Nothing published: the line says so --------------------------------
    {
        resetWindowStateForTest();
        const std::string line = render();
        std::printf("%s", line.c_str());
        CHECK(line == "window-state: unknown (the frame loop has not published one)\n");
    }

    // --- The wording, pinned against an explicit clock ----------------------
    {
        resetWindowStateForTest();
        publishWindowState(true, false, true);
        const std::int64_t t1 = windowStateClockMs();  // the stamp is <= this
        char buf[256];
        // 142.5 s after: 142 whole seconds, whatever the millisecond the stamp took.
        std::size_t n = renderWindowStateLineAt(buf, sizeof(buf), t1 + 142500);
        CHECK(std::string(buf, n) ==
              "window-state: visible, not minimised, focused, updated 142 s before this report\n");

        publishWindowState(false, true, false);
        const std::int64_t t2 = windowStateClockMs();
        n = renderWindowStateLineAt(buf, sizeof(buf), t2 + 257500);
        CHECK(std::string(buf, n) ==
              "window-state: hidden, minimised, not focused, updated 257 s before this report\n");

        // Visible but not focused (another window in front), and not minimised.
        publishWindowState(true, false, false);
        const std::int64_t t3 = windowStateClockMs();
        n = renderWindowStateLineAt(buf, sizeof(buf), t3 + 70500);
        CHECK(std::string(buf, n) ==
              "window-state: visible, not minimised, not focused, updated 70 s before this report\n");

        // Under a second is "0 s", never negative: a clock reading from before the
        // stamp (the renderer racing a publish) clamps to zero.
        publishWindowState(true, false, true);
        n = renderWindowStateLineAt(buf, sizeof(buf), 0);
        CHECK(std::string(buf, n) ==
              "window-state: visible, not minimised, focused, updated 0 s before this report\n");
    }

    // --- The window system, as a second line ---------------------------------
    {
        resetWindowStateForTest();
        char buf[256];
        publishWindowState(true, false, true);
        const std::int64_t t = windowStateClockMs();
        setWindowSystem("wayland");
        std::size_t n = renderWindowStateLineAt(buf, sizeof(buf), t + 3500);
        CHECK(std::string(buf, n) ==
              "window-state: visible, not minimised, focused, updated 3 s before this report\n"
              "window-system: wayland\n");
        // Before the first frame the system is still known.
        resetWindowStateForTest();
        setWindowSystem("x11");
        n = renderWindowStateLine(buf, sizeof(buf));
        CHECK(std::string(buf, n) ==
              "window-state: unknown (the frame loop has not published one)\nwindow-system: x11\n");
        // Anything that is not plain lower-case words is not repeated.
        setWindowSystem("Wayland; tuned to 145.500 MHz");
        n = renderWindowStateLine(buf, sizeof(buf));
        CHECK(std::string(buf, n).find("window-system: unknown\n") != std::string::npos);
        CHECK(std::string(buf, n).find("145") == std::string::npos);
        // Truncated cleanly in the middle of the second line.
        setWindowSystem("wayland");
        char small[80];
        n = renderWindowStateLine(small, sizeof(small));
        CHECK(n == sizeof(small) - 1);
        CHECK(small[n] == '\0');
        resetWindowStateForTest();
    }

    // --- A small buffer is truncated, terminated and never overrun ----------
    {
        resetWindowStateForTest();
        publishWindowState(true, false, true);
        char buf[40];
        std::memset(buf, 'X', sizeof(buf));
        const std::size_t n = renderWindowStateLine(buf, 24);
        CHECK(n == 23);
        CHECK(buf[23] == '\0');
        CHECK(std::string(buf, n) == "window-state: visible, ");
        CHECK(buf[24] == 'X');  // nothing written past the cap
        CHECK(renderWindowStateLine(buf, 0) == 0);
        CHECK(renderWindowStateLine(nullptr, 10) == 0);
    }

    // --- A REAL stall, filed by the REAL watchdog thread ---------------------
    {
        const fs::path dir = scratchDir("fires");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        resetWindowStateForTest();
        setWindowSystem("wayland");
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 1500);
        CHECK(w.running());

        // The frame loop running and publishing: visible, focused.
        frameLoopFor(w, 1200, true, false, true);
        CHECK(w.reportsWritten() == 0u);

        // Now the loop stops publishing (it is in the swap). 1500 ms threshold,
        // 500 ms poll: the report is filed within ~2 s of the last frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        CHECK(w.reportsWritten() == 1u);

        const std::string text = readFile(fs::path(w.lastReportPath()));
        CHECK(text.find("kind: hang") != std::string::npos);
        const std::string ctx = contextSection(text);
        CHECK(!ctx.empty());
        const std::string line = windowStateLine(ctx);
        std::printf("report line: %s\n", line.c_str());
        CHECK(startsWith(line, "window-state: visible, not minimised, focused, updated "));
        // The age is how long ago the "frame loop" last published: at least the
        // 1500 ms threshold that had to elapse before the report was filed.
        const long age = ageSeconds(line);
        std::printf("window-state age: %ld s\n", age);
        CHECK(age >= 1);
        CHECK(age <= 10);
        // In the context section, not the header the field inventory covers.
        CHECK(text.find("\nwindow-state: ") > text.find("--- context ---"));
        CHECK(text.find("\nwindow-state: ") < text.find("--- modules ---"));
        // ...and the window system beside it.
        CHECK(ctx.find("\nwindow-system: wayland\n") != std::string::npos);
        // One line, once.
        CHECK(text.find("\nwindow-state: ", text.find("\nwindow-state: ") + 1) == std::string::npos);

        w.stop();
        fs::remove_all(dir, ec);
    }

    // --- Hidden and minimised is RECORDED, and changes nothing about filing --
    {
        const fs::path dir = scratchDir("hidden");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        resetWindowStateForTest();
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 1500);
        frameLoopFor(w, 800, false, true, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        // A report is filed exactly as it is for a visible window...
        CHECK(w.reportsWritten() == 1u);
        const std::string text = readFile(fs::path(w.lastReportPath()));
        CHECK(text.find("kind: hang") != std::string::npos);
        // ...and says what the window was.
        const std::string line = windowStateLine(contextSection(text));
        std::printf("report line: %s\n", line.c_str());
        CHECK(startsWith(line, "window-state: hidden, minimised, not focused, updated "));
        CHECK(ageSeconds(line) >= 1);

        w.stop();
        fs::remove_all(dir, ec);
    }

    // --- A stall before any frame published says `unknown` -------------------
    {
        const fs::path dir = scratchDir("unknown");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        resetWindowStateForTest();
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 1500);
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            while (std::chrono::steady_clock::now() < until) {
                w.heartbeat();  // beats, but never publishes a window state
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        CHECK(w.reportsWritten() == 1u);
        const std::string line =
            windowStateLine(contextSection(readFile(fs::path(w.lastReportPath()))));
        CHECK(line == "window-state: unknown (the frame loop has not published one)");

        w.stop();
        fs::remove_all(dir, ec);
    }

    return testSummary("test_diag_window_state");
}
