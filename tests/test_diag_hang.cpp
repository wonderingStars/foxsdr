// The hang watchdog: the half a crash handler never catches.
//
// Three properties are asserted, and the second is the one that makes a hang
// report worth having:
//
//   1. A stalled frame loop is noticed and reported, and the app is left
//      running afterwards - a diagnostic that killed the session would be
//      worse than the hang.
//   2. The report contains MORE THAN ONE THREAD's stack. A deadlock is only
//      legible as a pair: "the GUI thread is blocked" names no bug, "the GUI
//      thread waits on the lock the CAT thread is holding inside a socket
//      close" is the bug. A single-threaded report would look complete and be
//      useless, so the thread count is asserted, not the presence of "a
//      stack".
//   3. It does not cry wolf: a paused window (deliberate blocking work) and a
//      suppressed state (debugger, modal loop) produce no report from exactly
//      the same stall that does produce one otherwise.
//
// The last block is the end-to-end one: the REAL application, with its real
// frame loop, made to stall past its real 5 s threshold, must leave a real
// report - and the same run reports the worst heartbeat gap it saw, which is
// the measurement the threshold is justified against. A unit test of a
// watchdog class proves the class; only that block proves the product.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
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
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-hang-") + tag + "-" + std::to_string(pid));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Every "name: " label a report emits before its first "--- section ---"
// marker, as a set. The comparison against the declared inventory is made in
// BOTH directions, so an added field fails as loudly as a documented one that
// stopped being emitted.
std::set<std::string> headerFields(const std::string& report) {
    std::set<std::string> names;
    std::size_t pos = 0;
    while (pos < report.size()) {
        const std::size_t eol = report.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? report.size() : eol;
        const std::string line = report.substr(pos, end - pos);
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { names.insert(line.substr(0, colon)); }
        pos = end + 1;
    }
    return names;
}

// Files directly in `dir`, so "off wrote nothing" can be asserted as a count
// rather than as the absence of one name a bug might have changed.
int fileCount(const fs::path& dir) {
    std::error_code ec;
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { ++n; }
    }
    return n;
}

// Beats the watchdog like a frame loop would, for `ms`, then returns.
void beatFor(HangWatchdog& w, unsigned ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Extra threads that are parked somewhere recognisable, so a report that
// walked every thread has something to find beyond the two the test itself
// needs.
struct ParkedThreads {
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    void spawn(int n) {
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this] {
                while (!stop.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
        }
    }
    void join() {
        stop.store(true, std::memory_order_relaxed);
        for (std::thread& t : threads) { t.join(); }
        threads.clear();
    }
};

#if defined(_WIN32)
// Runs the REAL application with its whole diagnostics tree redirected into
// `dir`, captures its stdout, and returns how many hang reports it left. Used
// by the toggle blocks below: the defect they exist for lived in the
// difference between what the Settings checkbox governed and what it did not,
// which no unit test of the watchdog class can see.
int runAppCountingHangs(const fs::path& dir, const std::string& args, std::string& out) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" " + args + " 2>&1\"";
    out.clear();
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);

    int n = 0;
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        if (readFile(e.path()).find("kind: hang") != std::string::npos) { ++n; }
    }
    return n;
}
#endif

}  // namespace

int main() {
    ParkedThreads parked;
    parked.spawn(3);

    // --- A stall is noticed, reported, and survived -------------------------
    {
        const fs::path dir = scratchDir("fires");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 800);
        CHECK(w.running());

        // Healthy first: a watchdog that fires on a running loop is worse
        // than none at all.
        beatFor(w, 1600);
        CHECK(w.reportsWritten() == 0u);

        // Now stall. 3x the threshold, plus the poll interval.
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 1u);

        // Exactly one, not one per poll: a wedged application must not fill
        // the user's disk with the same report 200 times.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        CHECK(w.reportsWritten() == 1u);

        const std::string path = w.lastReportPath();
        CHECK(!path.empty());
        CHECK(fs::exists(fs::path(path)));
        const std::string text = readFile(fs::path(path));
        CHECK(text.find("kind: hang") != std::string::npos);
        CHECK(text.find("stalled-ms: ") != std::string::npos);
        CHECK(text.find("signature: ") != std::string::npos);

        // THE INVENTORY, BOTH DIRECTIONS. Present-and-correct was already
        // asserted above; this asserts EXHAUSTIVE. PRIVACY.md lists what a
        // freeze report contains and says the list is held by a test in both
        // directions - it was not, and a line added to the writer (a command
        // line, a tuned frequency) would have shipped undocumented with every
        // test green. A crash or freeze report is the more revealing document
        // of the two this product writes, so it gets the stricter check, not
        // the looser one.
        const std::set<std::string> declaredHang(hangReportFieldNames().begin(),
                                                 hangReportFieldNames().end());
        CHECK(!declaredHang.empty());
        const std::set<std::string> emittedHang = headerFields(text);
        for (const std::string& f : emittedHang) { std::printf("hang field: %s\n", f.c_str()); }
        CHECK(emittedHang == declaredHang);

        // THE PROPERTY THAT MAKES IT USEFUL: a deadlock needs both halves.
        const int threads = countOccurrences(text, "--- thread ");
        std::printf("hang report thread sections: %d\n", threads);
        CHECK(threads > 1);
        // ...and each of them has to be a stack, not a heading.
        CHECK(countOccurrences(text, "+0x") > threads);
        // The stalled thread is named as such, so nobody has to guess which
        // of five stacks is the one that stopped.
        CHECK(text.find("(gui, stalled)") != std::string::npos);

        // ...AND THE MODULE TABLE, which is what makes those stacks readable
        // by anyone who was not there. A stack of module+offset with nothing
        // saying WHICH BUILD of that module is unreadable hex forever: the
        // same offset in a different link is a different function. This block
        // was absent from the freeze writer for a whole release while
        // PRIVACY.md described it as present, so it is asserted here against a
        // report from a REAL stall rather than trusted.
        CHECK(text.find("--- modules ---") != std::string::npos);
        {
            const std::size_t at = text.find("--- modules ---");
            const std::string mods =
                (at == std::string::npos) ? std::string() : text.substr(at);
            CHECK(mods.find("cascade") != std::string::npos ||
                  mods.find("test_diag_hang") != std::string::npos);
            // A build id, not the literal "(none)" for the module that
            // matters - a report whose own binary has no CodeView record is a
            // build that should never have shipped.
            CHECK(mods.find(" build=") != std::string::npos);
            const std::size_t b = mods.find(" build=");
            CHECK(b != std::string::npos && mods.compare(b, 13, " build=(none)") != 0);
        }

        // Recovery: the app carries on, and the watchdog re-arms rather than
        // going quiet for the rest of the session.
        beatFor(w, 1600);
        CHECK(w.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 2u);

        w.stop();
        CHECK(!w.running());
        fs::remove_all(dir, ec);
    }

    // --- The identifying half reaches disk BEFORE the stacks are walked -----
    //
    // Unwinding a stack means asking ntdll where a function's unwind data is,
    // and that reads the loader's inverted function table under a lock
    // LoadLibrary holds exclusively. This application calls LoadLibrary from
    // the GUI thread (a plugin rescan) and from a worker (the Soapy enumerate),
    // so a capture that runs while one of them is inside the loader can block
    // in the unwinder. crash_handler.cpp already answers that by writing
    // incrementally, in decreasing order of value; the watchdog must too, or a
    // wedged unwinder leaves NOTHING on disk - no fault kind, no signature, no
    // context - which is strictly worse than the hang it was describing.
    //
    // A wedged unwinder cannot be staged from ctest. What CAN be staged is the
    // property the incremental write exists for: stop the capture where a wedge
    // would stop it, and the file already on disk still names the bug.
    {
        const fs::path dir = scratchDir("partial");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        // The context a real session always has by the time the frame loop is
        // running, so the assertions below are about the report the product
        // would write rather than about an empty one.
        DiagContext ctx;
        ctx.version = "0.61.0-hangtest";
        ctx.commit = "cafebabe1234";
        ctx.mode = "WFM";
        ctx.sourceKind = "generator";
        setDiagContext(ctx);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.setCaptureAbortForTest(HangWatchdog::CaptureAbortForTest::AfterHeader);
        w.start(dir.string(), 800);
        beatFor(w, 900);
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        w.stop();

        const std::string path = w.lastReportPath();
        CHECK(!path.empty());
        CHECK(fs::exists(fs::path(path)));
        const std::string text = readFile(fs::path(path));
        std::printf("partial hang report: %zu bytes\n", text.size());
        // Everything that identifies the fault, written before a single thread
        // was suspended.
        CHECK(text.find("kind: hang") != std::string::npos);
        CHECK(text.find("stalled-ms: ") != std::string::npos);
        CHECK(text.find("threshold-ms: ") != std::string::npos);
        CHECK(text.find("signature: ") != std::string::npos);
        CHECK(text.find("commit: cafebabe1234") != std::string::npos);
        // ...and it really did stop where a wedge would have: no stacks.
        CHECK(text.find("--- thread ") == std::string::npos);

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- Deliberate blocking work does not produce a report -----------------
    {
        const fs::path dir = scratchDir("paused");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 800);
        beatFor(w, 900);
        {
            WatchdogPause hold(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        }
        CHECK(w.reportsWritten() == 0u);
        CHECK(fs::is_empty(dir));

        // And the pause must not have disarmed it permanently: the same stall
        // outside the pause still reports. Beat once first so the resume has
        // a fresh mark to stall from.
        beatFor(w, 900);
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 1u);

        w.stop();
        fs::remove_all(dir, ec);
    }

    // --- A suppressed state (debugger, modal loop) produces no report -------
    {
        const fs::path dir = scratchDir("suppressed");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::AlwaysSuppress);
        w.start(dir.string(), 800);
        beatFor(w, 900);
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 0u);
        CHECK(fs::is_empty(dir));
        w.stop();
        fs::remove_all(dir, ec);
    }

    // --- The switch governs a RUNNING watchdog, in both directions ----------
    //
    // start() is a no-op once running and is called once, before the frame
    // loop, so the directory handed to it used to be the only thing that ever
    // decided whether a hang report reached the disk. A user who started with
    // diagnostics on and unticked the box mid-session kept a live watchdog
    // writing hang-<pid>-N.txt on the next stall - against a promise made in
    // PRIVACY.md, README.md and docs/DIAGNOSTICS.md in the words "off means
    // off: no directory, no log file, no report".
    {
        const fs::path dir = scratchDir("toggle");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 800);
        CHECK(w.reportDir() == dir.string());

        // On: a stall reports.
        beatFor(w, 900);
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 1u);
        CHECK(fileCount(dir) == 1);

        // OFF, MID-SESSION. The same stall, on the same running watchdog, must
        // leave nothing new behind.
        w.setReportDir(std::string());
        CHECK(w.reportDir().empty());
        beatFor(w, 900);  // recover, so the watchdog re-arms
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.running());  // still running: a recovered stall is still logged
        CHECK(w.reportsWritten() == 1u);
        CHECK(fileCount(dir) == 1);

        // ON AGAIN, MID-SESSION, which is the half that was silently missing
        // in the other direction: a session switched on got the crash handler
        // and the log and no watchdog at all.
        w.setReportDir(dir.string());
        beatFor(w, 900);
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 2u);
        CHECK(fileCount(dir) == 2);

        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- No report directory means no file, and no crash --------------------
    {
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(std::string(), 800);
        CHECK(w.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 0u);
        CHECK(w.lastReportPath().empty());
        w.stop();
    }

    // The teardown's own budget (beginShutdown) and the cost of stop() itself
    // are asserted in tests/test_shutdown_budget.cpp rather than here - this
    // file already spends 90 s of its 120 s ceiling on real application runs,
    // and a suite that fails by timing out reports nothing useful about
    // either.

    parked.join();

#if defined(_WIN32)
    // --- The real application: real loop, real threshold, real report -------
    //
    // FOXSDR_DIAG_DIR redirects the whole diagnostics tree into a scratch
    // directory, so this never touches the user's %LOCALAPPDATA%\FoxSDR.
    // --frames keeps the run hermetic (no config load, no config save) and
    // --diag-stall wedges the frame loop for longer than the shipped 5 s
    // threshold - the exact fault this feature exists for.
    {
        const fs::path dir = scratchDir("app");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());

        const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
        const std::string cmd = "\"\"" + exe + "\" --frames 240 --diag-stall 7000 2>&1\"";
        std::string out;
        FILE* p = _popen(cmd.c_str(), "r");
        CHECK(p != nullptr);
        char buf[512];
        while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
        if (p != nullptr) { _pclose(p); }
        ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
        std::printf("%s", out.c_str());

        CHECK(out.find("rendered 240 frames") != std::string::npos);

        // THE MEASUREMENT THE THRESHOLD IS JUSTIFIED AGAINST. The app prints
        // the worst heartbeat gap it observed, excluding the deliberate
        // stall. If a future change makes a frame legitimately slow, this
        // goes red here rather than as a false hang report on a user's
        // machine.
        const std::size_t at = out.find("worst frame gap ");
        CHECK(at != std::string::npos);
        double gapMs = -1.0;
        if (at != std::string::npos) {
            gapMs = std::atof(out.c_str() + at + std::strlen("worst frame gap "));
        }
        std::printf("measured worst frame gap: %.1f ms (threshold %u ms)\n", gapMs,
                    HangWatchdog::kDefaultThresholdMs);
        CHECK(gapMs > 0.0);
        CHECK(gapMs < static_cast<double>(HangWatchdog::kDefaultThresholdMs) / 2.0);

        // WATCHDOGPAUSE IS ACTUALLY WIRED INTO THE PRODUCT. The header and the
        // docs offer it as the answer for blocking work the application enters
        // knowingly, and a facility that no shipped call site uses is a
        // mitigation a future maintainer will assume is already in place. The
        // real blocking work this application has on its GUI thread is the
        // plugin scan - twelve LoadLibrary calls off a possibly cold disk - so
        // the app reports how many pauses it took and this requires at least
        // one. Zero here means the class is dead code again.
        const std::size_t pat = out.find("watchdog pauses ");
        CHECK(pat != std::string::npos);
        long pauses = -1;
        if (pat != std::string::npos) {
            pauses = std::atol(out.c_str() + pat + std::strlen("watchdog pauses "));
        }
        std::printf("watchdog pauses taken by the real app: %ld\n", pauses);
        CHECK(pauses >= 1);

        // And the stall past the threshold left a report behind.
        const fs::path crashes = dir / "crashes";
        CHECK(fs::exists(crashes));
        int hangReports = 0;
        std::string sample;
        for (const auto& e : fs::directory_iterator(crashes, ec)) {
            const std::string text = readFile(e.path());
            if (text.find("kind: hang") != std::string::npos) {
                ++hangReports;
                sample = text;
            }
        }
        std::printf("app hang reports: %d\n", hangReports);
        CHECK(hangReports >= 1);
        CHECK(sample.find("--- thread ") != std::string::npos);
        CHECK(countOccurrences(sample, "--- thread ") > 1);
        // The app's own context reached the report: this is the wiring that a
        // unit test of the watchdog cannot see.
        CHECK(sample.find("mode: ") != std::string::npos);
        CHECK(sample.find("source: ") != std::string::npos);
        CHECK(sample.find("commit: ") != std::string::npos);

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- OFF MEANS OFF, in the REAL application, mid-session -----------------
    //
    // The block above is the positive control: the same binary, the same
    // stall, no toggle, one report. Here the Settings switch is unticked on
    // frame 30 - through the very function the checkbox calls - and the same
    // stall on frame 60 must leave NOTHING. That promise is made in
    // PRIVACY.md, README.md and docs/DIAGNOSTICS.md, and it was false: the
    // checkbox disarmed the crash handler and the log and never touched the
    // watchdog, so a user who turned diagnostics off went on producing hang
    // reports for the rest of the session.
    {
        const fs::path dir = scratchDir("toggleoff");
        std::string out;
        const int hangs =
            runAppCountingHangs(dir, "--frames 240 --diag-stall 7000 --diag-toggle off", out);
        std::printf("%s", out.c_str());
        CHECK(out.find("rendered 240 frames") != std::string::npos);
        // The hook really ran: without this, a --diag-toggle that silently did
        // nothing would pass the assertion below for the wrong reason.
        CHECK(out.find("--diag-toggle diagnostics off") != std::string::npos);
        CHECK(out.find("wedging the frame loop") != std::string::npos);
        std::printf("hang reports after switching diagnostics OFF: %d\n", hangs);
        CHECK(hangs == 0);

        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- ...and ON means ON, from a session that started with it off ---------
    //
    // The mirror image, and the half that would otherwise stay broken quietly:
    // a user who turns diagnostics on after a freeze got the crash handler and
    // the log armed and no watchdog at all, so the next freeze - the fault
    // they turned it on for - still wrote nothing.
    {
        const fs::path dir = scratchDir("toggleon");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path cfgPath = dir / "config.json";
        {
            std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
            cfg << "{\n  \"diagnosticsEnabled\": false\n}\n";
        }
        CHECK(fs::exists(cfgPath));
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
        std::string out;
        // runAppCountingHangs clears the directory, so the config is written
        // into a sibling that survives it.
        const fs::path run = dir / "diag";
        const int hangs =
            runAppCountingHangs(run, "--frames 240 --diag-stall 7000 --diag-toggle on", out);
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
        std::printf("%s", out.c_str());
        CHECK(out.find("rendered 240 frames") != std::string::npos);
        CHECK(out.find("--diag-toggle diagnostics on") != std::string::npos);
        std::printf("hang reports after switching diagnostics ON: %d\n", hangs);
        CHECK(hangs >= 1);

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- The unclean-exit marker reaches disk EARLY --------------------------
    //
    // telemetryCleanExit is the trigger for offering a report on the next
    // start: false on disk means "the last run never shut down". That is only
    // true if the false is written EARLY. It is written by the config save, and
    // the save is debounced behind a change - so a session that crashed before
    // the user touched anything would leave the marker still reading true and
    // look, to the next start, exactly like a clean exit. The launch counter
    // was supposed to force that first save; it does not, because the debounce
    // baseline is taken AFTER the counter is incremented, so the two agree.
    //
    // Found by running the real application, killing it, and reading the
    // config back - which is the only place a debounce interacting with a
    // start-up baseline is visible at all. Staged here with --diag-stall,
    // because that gives a known window in which the process is definitely
    // past start-up and definitely has not been touched by a user.
    {
        const fs::path dir = scratchDir("unclean");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path cfgPath = dir / "config.json";
        CHECK(!fs::exists(cfgPath));
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());

        const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
        std::string cmd = "\"" + exe + "\" --frames 600 --diag-stall 8000";
        std::vector<char> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back('\0');
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL started = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                              &pi);
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
        CHECK(started != 0);
        if (started != 0) {
            // The stall starts about a second in and lasts eight, so five
            // seconds is comfortably inside it: past start-up, nowhere near a
            // clean shutdown, and with nothing having changed in the session.
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }

        const std::string after = readFile(cfgPath);
        std::printf("unclean-exit marker: config is %zu bytes\n", after.size());
        CHECK(fs::exists(cfgPath));
        CHECK(after.find("\"telemetryCleanExit\": false") != std::string::npos);
        // ...and the diagnostics switches are on disk too, so the offer on the
        // next start can respect them.
        CHECK(after.find("\"diagnosticsEnabled\"") != std::string::npos);
        CHECK(after.find("\"diagnosticsMinidump\"") != std::string::npos);

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- ...and the CLEAN-exit marker reaches disk only after the join ------
    //
    // Since 0.66.0 telemetryCleanExit=true is written by a second save AFTER
    // pipeline_.stop() — because a death during that join (DSP threads, CAT,
    // the USB stack: where the worst shipped shutdown freeze lived) is a
    // death, and the old order (marker set before the join) counted it as a
    // clean exit. The save sits BEFORE the GL/GLFW teardown on purpose: it
    // rebuilds the pending usage report through glfwGetTime(), which returns
    // 0.0 after glfwTerminate() and would zero every session's length. This
    // block is the half that keeps the second save existing at all: without
    // it, every exit would leave false on disk and every next start would
    // count a phantom crash. The ordering half is comment-guarded at the call
    // site; the block above staying green pins the other side (false while
    // the process is alive).
    {
        const fs::path dir = scratchDir("cleanexit");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path cfgPath = dir / "config.json";
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());

        const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
        std::string cmd = "\"" + exe + "\" --frames 3";
        std::vector<char> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back('\0');
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL started = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                              &pi);
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
        CHECK(started != 0);
        if (started != 0) {
            // A 3-frame run exits by itself; 30 s is a generous ceiling.
            CHECK(::WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0);
            DWORD exitCode = 1;
            ::GetExitCodeProcess(pi.hProcess, &exitCode);
            CHECK(exitCode == 0);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }

        const std::string after = readFile(cfgPath);
        CHECK(fs::exists(cfgPath));
        CHECK(after.find("\"telemetryCleanExit\": true") != std::string::npos);

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- ...and the ORDER, which the two blocks above cannot see -----------
    //
    // The blocks above prove the marker is false mid-session and true after a
    // clean run. Both stay green whichever side of pipeline_.stop() the write
    // sits on — so on their own they do NOT pin the property this change
    // exists for: that a death during the pipeline join counts as UNCLEAN.
    // Proving that dynamically would mean killing the process inside a join
    // that a bounded run completes in milliseconds, with no seam to hold it
    // open. So the ordering is pinned STATICALLY, against the source that
    // ships — crude, but it is exactly what a revert would touch, and it goes
    // red the moment the marker moves back above the join. (Reading a source
    // file to hold a promise is the same device tests/test_crash_upload.cpp
    // uses against PRIVACY.md.)
    {
        const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
        const std::string text = readFile(src);
        CHECK(!text.empty());

        const std::size_t marker = text.find("telemetryCleanExit_ = true;");
        const std::size_t join = text.find("pipeline_.stop();");
        // The GL teardown, which must come AFTER the marker write: a save
        // performed past glfwTerminate() re-derives the pending usage report
        // with glfwGetTime() == 0.0 and reports a zero-second session.
        const std::size_t glTeardown = text.find("ImGui_ImplOpenGL3_Shutdown();");
        std::printf("clean-exit ordering: join@%zu marker@%zu glTeardown@%zu\n", join, marker,
                    glTeardown);
        CHECK(marker != std::string::npos);
        CHECK(join != std::string::npos);
        CHECK(glTeardown != std::string::npos);
        // Bounds-safe by construction: each index is checked against npos
        // above, and a comparison of two npos values would otherwise read as
        // a passing ordering.
        const bool haveAll = marker != std::string::npos && join != std::string::npos &&
                             glTeardown != std::string::npos;
        CHECK(haveAll && join < marker);
        CHECK(haveAll && marker < glTeardown);
    }
#endif

    return testSummary("test_diag_hang");
}
