// THE ORDER OF THE ENGINE'S LIFE, held against the window's, in ONE table (engine extraction stage 7c).
//
// The window application restores its settings, starts the receiver, and tears down in an order that has been
// broken by an innocent-looking edit before: the 0.99.84 merge lost whole statements from the teardown (the
// plugin unload put after the clean-exit marker, the marker's rewrite built from a fresh snapshot, the drain's
// deadline taken after the wait it shares) and found them only by running tests. The headless service
// (src/engine/engine_service.cpp) is the same life with the window left out, so there are now TWO copies of the
// order. A copy that is trusted to stay in step with the other is the bug waiting to happen; this test is what
// stops trusting. It reads BOTH files, with comments blanked, and holds each phase's statements - the window's and
// the service's - against one table, row by row:
//
//   RESTORE  AppWindow::AppWindow (the constructor)          against  EngineService::restore
//   APPLY    AppWindow::applyConfig                           against  EngineService::restore (the start-up state first)
//   START    AppWindow::run, receiver to control thread      against  EngineService::start
//   STOP     AppWindow::run, close-wait to drain             against  EngineService::stop
//
// Each row names a statement by the text it has in each file; every named statement appears once in its
// region and the rows come in the table's order in BOTH. Statements the window has and the service does not (the
// plugin report, the crash-report sweep's stop, the feature-request senders, the diagnostics wedge, the update
// check and the usage heartbeat) are rows too, marked WINDOW ONLY with the reason: they must be present in the
// window (so the table cannot go stale unnoticed) and absent from the service (so one cannot creep in unlisted).
//
// And the properties the order exists for, beyond the order itself: the marker is written once and only after the
// plugins are released; it is the last REQUESTED snapshot that is rewritten, so no fresh snapshot is published after
// the release; the service publishes a snapshot in exactly two places (the restore's baseline and the save that must
// be current); nobody calls Engine::shutdown() (the four phases in a row, with nothing between them) but the engine's
// own table; and foxsdr-engine's main takes the lock, restores, starts, keeps and stops in that order and does
// nothing else with the engine in between.
//
// A scan that finds nothing proves nothing, so each region it cannot find FAILS, and the checker is run a second
// time on MUTANTS of the service's text (marker before release, the final save after the stop, the forced first
// save after the control thread, the writer bound after the load, the lock after the restore): each must come back
// RED, or the checker itself is broken.
//
// Break-it by hand (docs/engine-stage4.md 6.17): move `engine_.markCleanExit()` above `engine_.shutdownRelease()` in
// EngineService::stop -> RED naming both statements.
//
// The optional first argument is the repository root (CTest passes none; the build bakes in CASCADE_SOURCE_DIR).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// The file with its line ends as LF (a working tree checked out with CRLF reads the same as one checked out with LF).
std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// The source with comments blanked to spaces (positions and line ends kept) and the insides of string and
// character literals blanked as well, so braces and identifiers in CODE are all that is searched.
std::string blanked(const std::string& s) {
    std::string v = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        const auto blank = [&](std::size_t k) {
            if (s[k] != '\n' && s[k] != '\r') { v[k] = ' '; }
        };
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; blank(i); }
                else if (c == '/' && n == '*') { st = Block; blank(i); blank(i + 1); ++i; }
                else if (c == '"') { st = Str; }
                else if (c == '\'') {
                    const bool sep = i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])) &&
                                     std::isalnum(static_cast<unsigned char>(n));
                    if (!sep) { st = Chr; }
                }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { blank(i); }
                break;
            case Block:
                if (c == '*' && n == '/') { blank(i); blank(i + 1); ++i; st = Code; } else { blank(i); }
                break;
            case Str:
                if (c == '\\') { blank(i); if (i + 1 < s.size()) { blank(++i); } }
                else if (c == '"') { st = Code; }
                else { blank(i); }
                break;
            case Chr:
                if (c == '\\') { blank(i); if (i + 1 < s.size()) { blank(++i); } }
                else if (c == '\'') { st = Code; }
                else { blank(i); }
                break;
        }
    }
    return v;
}

// The text of a function body: from the first '{' after `signature` (a definition's brace, not a declaration's
// semicolon) to its match. Empty when it cannot be found.
std::string bodyOf(const std::string& blank, const std::string& signature) {
    const std::size_t at = blank.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t open = blank.find_first_of("{;", at);
    if (open == std::string::npos || blank[open] != '{') { return std::string(); }
    int depth = 0;
    for (std::size_t i = open; i < blank.size(); ++i) {
        if (blank[i] == '{') { ++depth; }
        if (blank[i] == '}' && --depth == 0) { return blank.substr(open, i - open + 1); }
    }
    return std::string();
}

// [from, through) of `text`: from the first occurrence of `begin` to the end of the first `end` after it.
std::string between(const std::string& text, const std::string& begin, const std::string& end) {
    const std::size_t a = text.find(begin);
    if (a == std::string::npos) { return std::string(); }
    const std::size_t b = text.find(end, a);
    if (b == std::string::npos) { return std::string(); }
    return text.substr(a, b + end.size() - a);
}

std::size_t countOf(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) { ++n; }
    return n;
}

// One row of the table: the statement as the window writes it, and as the service writes it. An empty column means
// "this file has no such statement" (WINDOW ONLY rows have an empty service column, SERVICE ONLY rows an empty window column, and say why).
struct Row {
    const char* what;
    const char* window;
    const char* service;
    const char* why;  // WINDOW ONLY rows: why the service does without it; others: why it is where it is
};

// The regions (each is cut from its file by the caller) and their rows, in the order of both.
struct Table {
    const char* phase;
    std::vector<Row> rows;
};

const std::vector<Table>& table() {
    static const std::vector<Table> t = {
        {"RESTORE",
         {
             {"the engine's own start-up first", "engine_.initialise()", "engine_.initialise()",
              "a config restore needs something to restore onto"},
             {"the writer bound", "configWriter_.bind(", "writer_.bind(", "before anything can ask it to write"},
             {"the file loaded", "ConfigStore::load(", "ConfigStore::load(",
              "missing: defaults and loaded; damaged: defaults, not loaded, nothing applied"},
             {"the receiver's half applied", "applyConfig(cfg)", "engine_.applyConfig(",
              "the window's applyConfig runs the start-up state and Engine::applyConfig (APPLY below)"},
             {"the usage journal's start-up", "engine_.telemetryStartup(", "engine_.telemetryStartup(",
              "AFTER the apply: the crash count and the pending report are read from the file, not from defaults"},
             {"a damaged file kept aside", "setAsideDamagedFile(", "setAsideDamaged(",
              "before anything can save over it"},
             {"the first snapshot (the debounce's baseline)", "engine_.publishConfig()", "engine_.publishConfig()",
              "no pass has run yet: the engine hands its half over first"},
             {"the bookmarks", "engine_.loadBookmarks()", "engine_.loadBookmarks()",
              "under the settings' gate: a hermetic run never reads them"},
             {"the status lines", "", "engine_.publishStatusText()",
              "SERVICE ONLY (shell retirement R3): with or without a settings file, so the first read says what the restore said. The window no longer forces "
              "the publish: its first pass publishes the lines and its first look reads them as fact.status.* (a stepped run's first frame runs the pass's front "
              "part before the look; a free-running run makes the first publish in run() before the loop)"},
         }},
        {"APPLY",
         {
             {"the start-up state", "startupState(", "startupState(",
              "the file records what was showing at the last exit; the application starts on the bench whatever that was"},
             {"the receiver's half", "engine_.applyConfig(", "engine_.applyConfig(",
              "everything below it reads the start-up state, never the file as saved"},
         }},
        {"START",
         {
             {"the receiver started", "engine_.startReceiver()", "FOXAPI_OP_RUN",
              "the service's RUN command is Engine::startReceiver and a test fault cleared (engine.cpp, FOXAPI_OP_RUN)"},
             {"the update check and the usage heartbeat", "configureHeartbeat()", "",
              "WINDOW ONLY: interactive window runs only; a service on the desktop's install id would double-count one machine"},
             {"the diagnostics decided", "applyDiagnosticsEnabled(diagnosticsEnabled_)", "diagnosticsEnabled_ ? options_.diagnosticsDir",
              "the hang watchdog's report directory follows the stored switch"},
             {"THE FORCED FIRST SAVE (the unclean-exit marker reaches the disk)", "saveConfigNow()", "saveNow()",
              "after everything this thread does to the engine, before the pass thread exists to be raced"},
             {"the first pass", "engine_.controlPass()", "engine_.controlPass()",
              "made here so the first read is a real state"},
             {"the control thread", "engine_.startControlThread(", "engine_.startControlThread(",
              "after the first save: from here the engine is read through its thread-safe surface only"},
         }},
        {"STOP",
         {
             {"NO SOUND CARD CLOSE WAITED FOR", "SoundCardSource::setCloseWaitEnabled(false)",
              "SoundCardSource::setCloseWaitEnabled(false)", "a card whose close hangs must not spend the budget the radios' destroy is inside"},
             {"the plugin report's plugin half handed over", "handFrontEndFacts(true)", "",
              "WINDOW ONLY (shell retirement R4): a bounded-run diagnostic that needs the window's plugin poll and picture copy; the ENGINE prints the report at "
              "the end of shutdownQuiesce, which the service runs too and hands no half"},
             {"quiesce: servers, the pass joined, the TRANSMITTER, GPS, recordings", "engine_.shutdownQuiesce()",
              "engine_.shutdownQuiesce()", "a keyed radio cannot wait; everything the final save reads is settled"},
             {"the crash-report sweep stopped", "crashUploadFinish()", "",
              "WINDOW ONLY: the sweep is the window's (survey O6: no UI has consented on a headless service)"},
             {"the feature-request sender cancelled", "featureRequestSender_.cancel()", "", "WINDOW ONLY: the window's sender"},
             {"the problem-report sender cancelled", "problemReportSender_.cancel()", "", "WINDOW ONLY: the window's sender"},
             {"THE FINAL-STATE SAVE (the clean-exit flag still false)", "saveConfigNow()", "saveNow()",
              "before the pipeline stops so the snapshot reads live state; a death during the join is a death"},
             {"the shutdown wedge", "if (shutdownStallMs > 0)", "",
              "WINDOW ONLY: the bounded-run seam for the --diag-shutdown-stall test; the service has the unload one only"},
             {"stop: the bookmark flush, the pipeline joined", "engine_.shutdownStop()", "engine_.shutdownStop()",
              "where the bounded driver waits are spent"},
             {"RELEASE: the plugins destroyed BEFORE the marker", "engine_.shutdownRelease()", "engine_.shutdownRelease()",
              "a plugin's destroy() is third-party code that can wedge; a marker written before it reads a kill inside it as clean"},
             {"the unload wedge", "if (unloadStallMs > 0)", "if (options_.unloadStallMs > 0)",
              "the place a plugin's destroy() runs, where the marker's own test stages its kill"},
             {"THE MARKER", "engine_.markCleanExit()", "engine_.markCleanExit()", "after the last of the third-party code"},
             {"the marker's rewrite of the LAST REQUESTED snapshot", "marked.telemetryCleanExit = true",
              "marked.telemetryCleanExit = true", "not a fresh snapshot: half torn down, its clocks are dead"},
             {"the marker queued", "requestConfigSave(marked)", "requestSave(marked)", "nothing has asked for another save in between"},
             {"THE DEADLINE, taken before the wait it shares", "drainDeadline =", "drainDeadline =",
              "what the last save's wait leaves of it is what the drain gets"},
             {"the last save waited for, bounded", "configWriter_.finishOrAbandon(", "writer_.finishOrAbandon(",
              "the one place that waits: bounded, then abandoned with a line in the log"},
             {"the drain", "engine_.shutdownDrain(drainDeadline)", "engine_.shutdownDrain(drainDeadline)",
              "the two lists' last saves and the takes' files, against the same deadline"},
         }},
    };
    return t;
}

// The regions of the two files the table is held against. `ok` is false (and says which) when one cannot be cut.
struct Regions {
    std::string winRestore, winApply, winStart, winStop;
    std::string svcRestore, svcStart, svcStop;
    bool ok = true;
};

Regions cut(const std::string& winBlank, const std::string& svcBlank) {
    Regions r;
    r.winRestore = bodyOf(winBlank, "AppWindow::AppWindow(std::string configPath, bool announceConfig)");
    r.winApply = bodyOf(winBlank, "void AppWindow::applyConfig(");
    const std::string run = bodyOf(winBlank, "int AppWindow::run(int frames)");
    r.winStart = between(run, "engine_.startReceiver()", "engine_.startControlThread(");
    r.winStop = between(run, "SoundCardSource::setCloseWaitEnabled(false)", "engine_.shutdownDrain(drainDeadline)");
    r.svcRestore = bodyOf(svcBlank, "EngineService::Restored EngineService::restore()");
    r.svcStart = bodyOf(svcBlank, "bool EngineService::start(");
    r.svcStop = bodyOf(svcBlank, "void EngineService::stop()");
    for (const std::string* s : {&r.winRestore, &r.winApply, &r.winStart, &r.winStop, &r.svcRestore, &r.svcStart, &r.svcStop}) {
        if (s->empty()) { r.ok = false; }
    }
    return r;
}

// Hold both files against the table. Returns the number of things wrong, and says each.
int checkTable(const std::string& winBlank, const std::string& svcBlank, bool say) {
    int bad = 0;
    const auto fail = [&](const char* fmt, const char* a, const char* b = "") {
        ++bad;
        if (say) { std::printf(fmt, a, b); }
    };
    const Regions r = cut(winBlank, svcBlank);
    if (!r.ok) {
        ++bad;
        if (say) {
            std::printf("  a region could not be cut: restore %zu/%zu apply %zu start %zu/%zu stop %zu/%zu bytes (window/service)\n",
                        r.winRestore.size(), r.svcRestore.size(), r.winApply.size(), r.winStart.size(), r.svcStart.size(),
                        r.winStop.size(), r.svcStop.size());
        }
        return bad;
    }
    for (const Table& t : table()) {
        const std::string phase = t.phase;
        // APPLY has no region of its own in the service: it is the head of restore.
        const std::string& win = phase == "RESTORE" ? r.winRestore : phase == "APPLY" ? r.winApply : phase == "START" ? r.winStart : r.winStop;
        const std::string& svc = phase == "RESTORE" || phase == "APPLY" ? r.svcRestore : phase == "START" ? r.svcStart : r.svcStop;
        std::size_t lastWin = 0;
        std::size_t lastSvc = 0;
        const char* lastWhat = "";
        for (const Row& row : t.rows) {
            const std::string w = row.window;
            const std::string s = row.service;
            // THE WINDOW's column. A SERVICE ONLY row has none: the window must not have grown the service's statement unlisted.
            if (w.empty()) {
                if (!s.empty() && win.find(s) != std::string::npos) {
                    fail("  %s: '%s' is SERVICE ONLY in the table but the window has it\n", t.phase, row.service);
                }
            } else {
                const std::size_t wAt = win.find(w);
                if (wAt == std::string::npos) {
                    fail("  %s: the window no longer has '%s' where the table says: the table or the window moved\n", t.phase, row.window);
                } else if (countOf(win, w) != 1) {
                    fail("  %s: '%s' appears more than once in the window's region\n", t.phase, row.window);
                } else {
                    if (wAt < lastWin) {
                        ++bad;
                        if (say) { std::printf("  %s: in the WINDOW '%s' comes before '%s' - the table's order is the contract\n", t.phase, row.what, lastWhat); }
                    }
                    lastWin = wAt;
                }
            }
            // THE SERVICE's column.
            if (s.empty()) {
                // WINDOW ONLY: the service must not have grown it unlisted. The window's text names it; the service's
                // region must not contain the same call.
                if (!w.empty() && svc.find(w) != std::string::npos) {
                    fail("  %s: '%s' is WINDOW ONLY in the table but the service has it\n", t.phase, row.window);
                }
                continue;
            }
            const std::size_t sAt = svc.find(s);
            if (sAt == std::string::npos) {
                fail("  %s: the service lost '%s' - the window has it\n", t.phase, row.service);
            } else if (countOf(svc, s) != 1) {
                fail("  %s: '%s' appears more than once in the service's region\n", t.phase, row.service);
            } else {
                if (sAt < lastSvc) {
                    ++bad;
                    if (say) { std::printf("  %s: in the SERVICE '%s' comes BEFORE '%s' - the window does not do that\n", t.phase, row.what, lastWhat); }
                }
                lastSvc = sAt;
            }
            lastWhat = row.what;
        }
    }
    // APPLY's service column is the head of restore: the start-up state is before the receiver's half there, too.
    {
        const std::size_t a = r.svcRestore.find("startupState(");
        const std::size_t b = r.svcRestore.find("engine_.applyConfig(");
        if (a == std::string::npos || b == std::string::npos || a > b) {
            ++bad;
            if (say) { std::printf("  the service does not take the start-up state before it applies the receiver's half\n"); }
        }
    }
    return bad;
}

// The properties the order exists for, beyond the order.
int checkProperties(const std::string& svcBlank, const std::string& mainBlank, bool say) {
    int bad = 0;
    const auto need = [&](bool cond, const char* what) {
        if (!cond) {
            ++bad;
            if (say) { std::printf("  PROPERTY: %s\n", what); }
        }
    };
    // The marker is written once, in one place; the service never calls the four-phase shutdown.
    need(countOf(svcBlank, "markCleanExit(") == 1, "markCleanExit() is called exactly once in the service");
    need(countOf(svcBlank, "telemetryCleanExit = true") == 1, "telemetryCleanExit = true appears exactly once in the service");
    need(svcBlank.find("engine_.shutdown()") == std::string::npos && svcBlank.find("engine_.shutdown (") == std::string::npos,
         "the service calls Engine::shutdown() (the four phases with nothing between them)");
    // A snapshot is published in exactly two places: the restore's baseline and the save that must be current. So none is
    // published after the release, and the marker's rewrite cannot be of a fresh one.
    need(countOf(svcBlank, "publishConfig(") == 2, "publishConfig() is called in exactly two places (restore's baseline, saveNow)");
    {
        const std::string save = bodyOf(svcBlank, "void EngineService::saveNow()");
        need(!save.empty() && save.find("engine_.publishConfig()") != std::string::npos, "saveNow() publishes the current snapshot");
        const std::string keep = bodyOf(svcBlank, "void EngineService::keep()");
        need(!keep.empty() && keep.find("publishConfig(") == std::string::npos,
             "keep() publishes nothing (the engine's pass does, and keep reads the published copy)");
        const std::string stop = bodyOf(svcBlank, "void EngineService::stop()");
        need(!stop.empty() && stop.find("publishConfig(") == std::string::npos && stop.find("configSnapshot(") == std::string::npos,
             "stop() takes no snapshot of its own (it saves through saveNow() and rewrites lastRequested_)");
        need(!stop.empty() && stop.find("lastRequested_") != std::string::npos, "the marker is a rewrite of lastRequested_");
        // The marker is not on any path that runs before the release: nothing in start() or keep() writes it.
        const std::string start = bodyOf(svcBlank, "bool EngineService::start(");
        need(start.find("telemetryCleanExit") == std::string::npos && keep.find("telemetryCleanExit") == std::string::npos,
             "start() and keep() never touch the clean-exit flag");
    }
    // foxsdr-engine's main: the lock, the restore, the start, the keep, the stop - and the engine touched nowhere else.
    {
        const std::string run = bodyOf(mainBlank, "int runEngine(");
        need(!run.empty(), "runEngine() found in src/engine_main.cpp");
        // The first call of each, in this order - except stop(), whose LAST call is the one that ends the run (it is also
        // called on the two early-return paths, where the run failed before it began).
        std::size_t last = 0;
        // (7e: the provisioning is between the restore and the start - after what the file said, before anything receives)
        for (const char* call : {"service.acquireConfig(", "service.restore()", "service.provision(", "service.start(", "service.keep()"}) {
            const std::size_t at = run.find(call);
            need(at != std::string::npos && at >= last, call);
            if (at != std::string::npos) { last = at; }
        }
        const std::size_t stopAt = run.rfind("service.stop()");
        need(stopAt != std::string::npos && stopAt >= last, "service.stop() ends the run, after the keep loop");
        need(run.find("engine.shutdown") == std::string::npos && run.find(".applyCommand(") == std::string::npos,
             "runEngine() drives the engine through the service only (no shutdown(), no applyCommand())");
        need(run.find("engine.startControlThread") == std::string::npos && run.find("engine.controlPass") == std::string::npos,
             "runEngine() does not start the engine's pass itself");
    }
    return bad;
}

fs::path findRoot(int argc, char** argv) {
    if (argc > 1) { return argv[1]; }
#ifdef CASCADE_SOURCE_DIR
    return CASCADE_SOURCE_DIR;
#else
    fs::path at = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(at / "src" / "engine" / "engine_service.cpp")) { return at; }
        at = at.parent_path();
    }
    return {};
#endif
}

// Replace the first occurrence of `a` with `b` in `s` (the mutants' tool). False when `a` is not there.
bool replaceOnce(std::string& s, const std::string& a, const std::string& b) {
    const std::size_t at = s.find(a);
    if (at == std::string::npos) { return false; }
    s.replace(at, a.size(), b);
    return true;
}

// Swap two statements (each a whole line's code) by exchanging their text.
bool swapStatements(std::string& s, const std::string& a, const std::string& b) {
    const std::size_t ia = s.find(a);
    const std::size_t ib = s.find(b);
    if (ia == std::string::npos || ib == std::string::npos || ia == ib) { return false; }
    const std::string placeholder = "\x01SWAP\x01";
    if (!replaceOnce(s, a, placeholder)) { return false; }
    if (!replaceOnce(s, b, a)) { return false; }
    return replaceOnce(s, placeholder, b);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash in the scan must leave what it had said
    std::printf("test_engine_service_order\n");
    const fs::path root = findRoot(argc, argv);
    const std::string winText = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::string svcText = readFile(root / "src" / "engine" / "engine_service.cpp");
    const std::string mainText = readFile(root / "src" / "engine_main.cpp");
    CHECK(!winText.empty() && !svcText.empty() && !mainText.empty());
    if (winText.empty() || svcText.empty() || mainText.empty()) { return testSummary("test_engine_service_order"); }
    const std::string win = blanked(winText);
    const std::string svc = blanked(svcText);
    const std::string mn = blanked(mainText);

    // --- the table, the window's column and the service's, row by row -----------------------------------------------
    int rows = 0;
    for (const Table& t : table()) { rows += static_cast<int>(t.rows.size()); }
    std::printf("  %d rows in %zu phases\n", rows, table().size());
    CHECK(checkTable(win, svc, true) == 0);
    CHECK(checkProperties(svc, mn, true) == 0);

    // --- the checker is not blind: each mutant of the service's text must come back RED ------------------------------
    struct Mutant { const char* name; bool (*make)(std::string&); };
    const Mutant mutants[] = {
        {"the marker written BEFORE the plugins are released",
         [](std::string& s) { return swapStatements(s, "engine_.shutdownRelease();", "engine_.markCleanExit();"); }},
        {"the final-state save AFTER the pipeline is stopped",
         [](std::string& s) { return replaceOnce(s, "if (persistent()) { saveNow(); }\n\n    engine_.shutdownStop();", "engine_.shutdownStop();\n    if (persistent()) { saveNow(); }"); }},
        {"the clean-exit flag rewritten from a FRESH snapshot (a second publish)",
         [](std::string& s) { return replaceOnce(s, "cascade::core::AppConfig marked = lastRequested_;", "engine_.publishConfig(); cascade::core::AppConfig marked = engine_.configSnapshot();"); }},
        {"the forced first save AFTER the control thread starts",
         [](std::string& s) {
             return replaceOnce(s, "if (persistent()) { saveNow(); }\n\n    cascade::engine::ControlThreadOptions opt;", "cascade::engine::ControlThreadOptions opt;") &&
                    replaceOnce(s, "engine_.startControlThread(opt);", "engine_.startControlThread(opt);\n    if (persistent()) { saveNow(); }");
         }},
        {"the writer bound AFTER the file is loaded",
         [](std::string& s) { return swapStatements(s, "writer_.bind(cascade::core::ConfigStore::writeFile);", "engine_.publishStatusText();"); }},
        {"the usage journal started BEFORE the receiver's half is applied",
         [](std::string& s) { return swapStatements(s, "engine_.applyConfig(startup);", "engine_.telemetryStartup(cfg, configDir);"); }},
        {"the sound-card close wait left on",
         [](std::string& s) { return replaceOnce(s, "cascade::source::SoundCardSource::setCloseWaitEnabled(false);", ""); }},
        {"the drain's deadline taken AFTER the last save's wait",
         [](std::string& s) { return swapStatements(s, "const auto drainDeadline = std::chrono::steady_clock::now() + cascade::gui::ConfigWriter::kSaveBound;", "if (persistent()) {\n        if (writer_.finishOrAbandon("); }},
    };
    for (const Mutant& m : mutants) {
        std::string mutated = svcText;
        const bool made = m.make(mutated);
        if (!made) { std::printf("  mutant '%s' could not be made (its anchor text is gone from the service): the test needs updating\n", m.name); }
        CHECK(made);
        if (!made) { continue; }
        const int bad = checkTable(win, blanked(mutated), false) + checkProperties(blanked(mutated), mn, false);
        std::printf("  mutant '%s': %d finding(s)\n", m.name, bad);
        CHECK(bad > 0);
    }
    // The main's order, mutated: the lock taken after the restore.
    {
        std::string mutated = mainText;
        const bool made = swapStatements(mutated, "service.acquireConfig(sentence)", "service.restore()");
        CHECK(made);
        const int bad = checkProperties(svc, blanked(mutated), false);
        std::printf("  mutant 'the lock taken after the restore': %d finding(s)\n", bad);
        CHECK(made && bad > 0);
    }
    return testSummary("test_engine_service_order");
}
