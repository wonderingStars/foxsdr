// test_crash_upload_poll_app.cpp - AN ISSUE REPORT IS SENT IN THE SESSION THAT WROTE IT: THE WINDOW'S SWEEP, ASKED ONCE A FRAME
// (master 0.99.101, a33a9d5f, AppWindow::crashUploadPoll; re-expressed in the 0.99.104 merge, docs/engine-merge-0.99.104.md).
//
// On master the window filed the report (its own recovery poll) and swept the reports folder from the same function. On the engine-extraction
// branch the report is filed by the ENGINE's pass, where the word is counted, and the sweep is still the window's (the uploader, the policy state
// and the install id are the interface's half), so the window asks once a frame in its look whether the pass has filed anything
// (core::issue::takeNewlyFiled, a process-wide count of files written) and starts the next sweep as soon as the one before it has finished.
// master's own tests hold the pieces (test_issue_report: the count; test_crash_upload: the uploader's running() and a second sweep; test_viewport_
// fallback_app: the whole thing through the real binary). What only a test of the window's function can hold is the branch's adaptation:
//
//   A  NOTHING FILED, NOTHING SWEPT: a hundred polls of a window whose engine filed nothing start no sweep
//   B  A SWEEP THAT CANNOT BE STARTED YET IS STILL WANTED: before the engine's first publish the telemetry switch the sweep's install id is read
//      from is unanswered (crashUploadStart answers false, "not yet"); a report filed meanwhile is not forgotten, and the first look that can answer
//      starts the sweep
//   C  A REPORT FILED AFTER A SWEEP FINISHED STARTS THE NEXT ONE, and that sweep finds it (considered == 1) - the whole point of the poll
//   D  THE START-UP SWEEP IS NOT RACED: while it has not started (crashUploadStartPending_, armed by run()) a poll starts nothing and spends the
//      count - what was filed is in the folder the start-up sweep is about to list; the start-up sweep drains the count itself, so it does not
//      ask for a second one
//   E  ONCE A SESSION IS NOT ONCE A SWEEP: the same word filed again writes nothing, counts nothing and so starts nothing
//
// Break-it (the 0.99.104 merge): the poll never asks for a sweep -> C and B go RED; the poll starts the next sweep without first checking that the
// count moved -> A goes RED; the poll ignores crashUploadStartPending_ -> D goes RED; crashUploadStart's false answer dropped (the wish forgotten) ->
// B goes RED; fileIssueReport counting before the write succeeds is held by test_issue_report.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/crash_handler.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::health::Recovered;

namespace cascade::gui {
struct AppWindowTestAccess {
    static void poll(AppWindow& a) { a.crashUploadPoll(); }
    static bool wanted(AppWindow& a) { return a.crashUploadWanted_; }
    static bool swept(AppWindow& a) { return a.crashUploadSwept_; }
    static bool& pending(AppWindow& a) { return a.crashUploadStartPending_; }
    static bool start(AppWindow& a) { return a.crashUploadStart(); }
    static void finish(AppWindow& a) { a.crashUploadFinish(); }
    static std::string& crashDir(AppWindow& a) { return a.diagCrashDir_; }
    static bool& diagnostics(AppWindow& a) { return a.diagnosticsEnabled_; }
    static cascade::core::CrashUploader& uploader(AppWindow& a) { return a.crashUploader_; }
    // One pass of the engine, as the engine's own frame runs it: the first one publishes the facts a sweep reads (fact.telemetry.enabled).
    static void pump(AppWindow& a) { passseam::whole(th::engine(a), [&] { th::engine(a).pump(); }); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// How many lines of the log contain `text`.
int logLinesWith(const std::string& text) {
    int n = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(text) != std::string::npos) { ++n; }
    }
    return n;
}

std::size_t issueFiles(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("issue-", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) { ++n; }
    }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_crash_upload_poll_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_crash_upload_poll_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The crash address is the discard port: a sweep that starts really tries and fails, and nothing leaves the machine. FOXSDR_DIAG_DIR is the
    // other seam (core/crash_upload.cpp, crashUploadEndpoint): a redirected diagnostics tree has NOWHERE to send unless FOXSDR_CRASH_URL says so,
    // so even a lost FOXSDR_CRASH_URL could not reach the live endpoint from this test.
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9/crash");
    setEnv("FOXSDR_DIAG_DIR", scratch.string());
    ImGui::CreateContext();

    // Capture on, into a folder of this test's: fileIssueReport writes only with consent and a directory (core/issue_report.hpp).
    const fs::path reports = scratch / "crashes";
    fs::create_directories(reports, ec);
    {
        cascade::core::CrashHandlerConfig on;
        on.crashDir = reports.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        cascade::core::installCrashHandlers(on);
        CHECK(!cascade::core::activeCrashDir().empty());
        cascade::core::issue::resetIssueReportsForTest();
    }

    th::TestWindow app;
    A::crashDir(app) = reports.string();
    A::diagnostics(app) = true;
    A::pending(app) = false;   // (run() arms it; a test window never runs)

    // A. NOTHING FILED, NOTHING SWEPT
    for (int i = 0; i < 100; ++i) { A::poll(app); }
    CHECK(!A::swept(app));
    CHECK(!A::uploader(app).busy());
    CHECK(!A::wanted(app));

    // B. A REPORT FILED BEFORE THE ENGINE'S FIRST PUBLISH IS WANTED, NOT FORGOTTEN
    CHECK(!cascade::core::issue::fileIssueReport(Recovered::Audio).empty());
    CHECK(issueFiles(reports) == 1);
    A::poll(app);
    const bool factsNotYet = !A::swept(app);
    std::printf("  before the engine's first publish: sweep %s, still wanted %s\n", A::swept(app) ? "started" : "not started",
                A::wanted(app) ? "yes" : "no");
    if (factsNotYet) {
        CHECK(A::wanted(app));                     // "not yet" is not "no": the report is not forgotten
        CHECK(!A::uploader(app).busy());
        A::poll(app);                              // and it is asked again, every look, until the engine can answer
        CHECK(A::wanted(app));
        A::pump(app);                              // the engine's first publish
    } else {
        // A host whose restore had already published the switch (not this one): the sweep started at once. The wish is satisfied either way.
        CHECK(!A::wanted(app));
    }
    A::poll(app);
    CHECK(A::swept(app));                          // the first look that can answer starts the sweep
    CHECK(!A::wanted(app));
    CHECK(waitFor([&] { return !A::uploader(app).running(); }));
    A::finish(app);                                // joins it, journals its state, logs its outcome
    CHECK(A::uploader(app).outcome().considered == 1);   // it found the report that was filed before it
    CHECK(!A::uploader(app).busy());

    // E. THE SAME WORD AGAIN WRITES NOTHING, COUNTS NOTHING, STARTS NOTHING
    CHECK(cascade::core::issue::fileIssueReport(Recovered::Audio).empty());
    A::poll(app);
    CHECK(!A::uploader(app).busy());
    CHECK(!A::wanted(app));

    // C. A REPORT FILED AFTER A SWEEP FINISHED STARTS THE NEXT ONE, AND THAT SWEEP FINDS IT
    CHECK(!cascade::core::issue::fileIssueReport(Recovered::Viewport).empty());
    CHECK(issueFiles(reports) == 2);
    A::poll(app);
    CHECK(A::uploader(app).busy());                // the next sweep is under way
    CHECK(!A::wanted(app));
    CHECK(waitFor([&] { return !A::uploader(app).running(); }));
    A::finish(app);
    // The second sweep listed both files; the first was settled by the first sweep (a failed send is retried by policy, so it is considered
    // again), the new one was not seen before: at least the new one is considered.
    std::printf("  the second sweep considered %d report(s)\n", A::uploader(app).outcome().considered);
    CHECK(A::uploader(app).outcome().considered >= 1);

    // D. THE START-UP SWEEP IS NOT RACED
    A::pending(app) = true;                        // run() has armed the start-up sweep and no look has started it yet
    CHECK(!cascade::core::issue::fileIssueReport(Recovered::Reopen).empty());
    CHECK(issueFiles(reports) == 3);
    A::poll(app);
    CHECK(!A::uploader(app).busy());               // nothing started on top of the start-up sweep
    CHECK(!A::wanted(app));                        // and nothing is wanted on top of it
    CHECK(cascade::core::issue::takeNewlyFiled() == 0);   // the count was spent by the poll
    CHECK(A::start(app));                          // the start-up sweep starts (what the look does)...
    A::pending(app) = false;
    CHECK(A::uploader(app).busy());
    CHECK(waitFor([&] { return !A::uploader(app).running(); }));
    // ...and, with nothing filed since, no second sweep is asked for: a poll that restarted would first join this one, and joining it logs its
    // outcome (crashUploadFinish), so the log is the witness that the poll left it alone.
    const int outcomesBefore = logLinesWith("crash upload: ");
    A::poll(app);
    CHECK(logLinesWith("crash upload: ") == outcomesBefore);
    CHECK(A::uploader(app).busy());                // (still the start-up sweep's thread, not joined: running() is false, busy() is not)
    A::finish(app);
    CHECK(logLinesWith("crash upload: ") > outcomesBefore);   // (the outcome line, and a line for each note the sweep left)
    CHECK(!A::wanted(app));
    CHECK(A::uploader(app).outcome().considered >= 1);   // it listed the report filed while it was pending

    fs::remove_all(scratch, ec);
    return testSummary("test_crash_upload_poll_app");
}
