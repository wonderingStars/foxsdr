// THE ORDER OF THE ENGINE'S LIFE, held against the window's and the host's, in ONE table (engine extraction stage 7c; rewritten by shell retirement R8).
//
// The window application restores its settings, starts the receiver, and tears down in an order that has been
// broken by an innocent-looking edit before: the 0.99.84 merge lost whole statements from the teardown (the
// plugin unload put after the clean-exit marker, the marker's rewrite built from a fresh snapshot, the drain's
// deadline taken after the wait it shares) and found them only by running tests. Until R8 there were TWO copies of
// the order - the window's run() and the headless service's - and a copy that is trusted to stay in step with the
// other is the bug waiting to happen. SINCE R8 THERE IS ONE COPY OF THE ENGINE'S LIFE: src/engine/engine_service.cpp
// starts and stops the engine for both hosts, and what this test holds is that the window really has given it up, that
// the host calls it in the one order that is safe, and that what is left of the window's shutdown is in the order the
// engine's depends on. It reads the files, with comments blanked, and holds each phase's statements against one table,
// row by row:
//
//   RESTORE  AppWindow::AppWindow (the constructor)          against  EngineService::restore   (shell retirement R7: the window no longer
//                                                             restores - the host's restore is the service's; the rows that moved are
//                                                             SERVICE ONLY, and the window's constructor must not have them)
//   APPLY    AppWindow::applyWindowHalf                       against  EngineService::restore (the start-up state first; the window applies
//                                                             only ITS half, from the table, so the engine's two statements are SERVICE ONLY)
//   START    AppWindow::run, before the frame loop            against  EngineService::start   (R8: every row is SERVICE ONLY but the diagnostics'
//                                                             switch, which each side applies to what it owns; the window must not start the engine)
//   PRELUDE  AppWindow::run, from the grace's release         (WINDOW ONLY rows, in time order: the window's part of the shutdown, done while the
//            to the end                                       engine is still running and the GL context is current, before run() returns)
//   STOP     AppWindow::run, same region                      against  EngineService::stop    (R8: SERVICE ONLY - the window must not stop the engine)
//   CLOSE    AppWindow::closeWindow                           (WINDOW ONLY rows: the GL teardown, the watchdog stopped LAST)
//
// Each row names a statement by the text it has in each file; every named statement appears once in its
// region and the rows come in the table's order in BOTH. Statements one side has and the other does not are rows too,
// marked WINDOW ONLY or SERVICE ONLY with the reason: they must be present where they live (so the table cannot go
// stale unnoticed) and absent where they do not (so one cannot creep in unlisted). A SERVICE ONLY row may also name the
// spelling the window USED to have (`gone`), which must be absent from the window's region: the statement did not just
// move, it left.
//
// THE THIRD FILE IN THE ORDER IS src/main.cpp, the window application's HOST (checkHostMain): it builds the service with the window application's
// options, takes the lock (a real session only), restores, constructs the window, STARTS the service, runs the window, STOPS the service, and only
// then closes the window - and does nothing else with the engine in between. The fourth is the options it builds the service with
// (src/engine/window_service_options.cpp, checkFactory): the keeper thread, the breadcrumbs, the best-effort lock, and what a bounded run changes.
//
// And the properties the order exists for, beyond the order itself: the marker is written once and only after the
// plugins are released; it is the last REQUESTED snapshot that is rewritten, so no fresh snapshot is published after
// the release; the service publishes a snapshot in exactly two places (the restore's baseline and the save that must
// be current); nobody calls Engine::shutdown() (the four phases in a row, with nothing between them) but the engine's
// own table; the window calls none of the engine's lifecycle (checkWindowProperties); and foxsdr-engine's main takes
// the lock, restores, starts, keeps and stops in that order and does nothing else with the engine in between.
//
// A scan that finds nothing proves nothing, so each region it cannot find FAILS, and the checker is run a second
// time on MUTANTS of the service's, the window's, the host's and the factory's text: each must come back RED, or the
// checker itself is broken. The R8 break-it list (docs/engine-stage4.md 6.25, the ordering table) maps each statement of the order to the
// mutant that proves its position, applied by hand to the real sources and run against the real binary as well.
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
// `gone` is, for a SERVICE ONLY row, the spelling the window used to have: it must not be in the window's region.
struct Row {
    const char* what;
    const char* window;
    const char* service;
    const char* why;  // WINDOW ONLY rows: why the service does without it; others: why it is where it is
    const char* gone = "";
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
             {"the engine's own start-up first", "", "engine_.initialise()",
              "SERVICE ONLY (shell retirement R7): the host's restore does it, before the window exists - a config restore needs something to restore onto"},
             {"the writer bound", "", "writer_.bind(",
              "SERVICE ONLY (R8b): before anything can ask it to write. The window's own writer, with the rest of its copy of the keeper, was deleted", "configWriter_.bind("},
             {"the file loaded", "", "ConfigStore::load(",
              "SERVICE ONLY (R7): missing: defaults and loaded; damaged: defaults, not loaded, nothing applied. The window reads its half from the table"},
             {"the receiver's half applied", "", "engine_.applyConfig(",
              "SERVICE ONLY (R7): the start-up state and Engine::applyConfig (APPLY below)"},
             {"the usage journal's start-up", "", "engine_.telemetryStartup(",
              "SERVICE ONLY (R7): AFTER the apply - the crash count and the pending report are read from the file, not from defaults"},
             {"a damaged file kept aside", "", "setAsideDamaged(",
              "SERVICE ONLY (R7): before anything can save over it; a damaged file that cannot be kept aside forbids the SERVICE's own writer (the window keeps none since R8b)"},
             {"the first snapshot (the debounce's baseline)", "", "engine_.publishConfig()",
              "SERVICE ONLY (R8b): no pass has run yet: the engine hands its half over first. The window's baseline of its own keeper was deleted with the keeper",
              "savedCfg_ = currentConfig()"},
             {"the bookmarks", "", "engine_.loadBookmarks()",
              "SERVICE ONLY (R7): under the settings' gate: a hermetic run never reads them"},
             {"the status lines", "", "engine_.publishStatusText()",
              "SERVICE ONLY (shell retirement R3): with or without a settings file, so the first read says what the restore said. The window no longer forces "
              "the publish: its first pass publishes the lines and its first look reads them as fact.status.* (a stepped run's first frame runs the pass's front "
              "part before the look; a free-running run makes the first publish in the service's start before the loop)"},
         }},
        {"APPLY",
         {
             {"the start-up state", "", "startupState(",
              "SERVICE ONLY (R7): the file records what was showing at the last exit; the application starts on the bench whatever that was - applied "
              "ONCE, by the host's restore, to the engine's half and (as fact.config.interface) to the window's"},
             {"the receiver's half", "", "engine_.applyConfig(",
              "SERVICE ONLY (R7): everything below it reads the start-up state, never the file as saved. The window applies its own half only"},
         }},
        {"START",
         {
             {"the hold seam in force first", "", "options_.holdAudioLockMs > 0",
              "SERVICE ONLY (R8; ServiceOptions::holdAudioLockMs): FOXSDR_HOLD_AUDIO_LOCK, a bounded-run test seam, must be in force before ANY command can reach the "
              "engine - its web server answers from before the window exists, and a browser's stop could arrive before the window's submit", "FOXAPP_OP_TEST_HOLD_AUDIO_LOCK"},
             {"the receiver started", "", "FOXAPI_OP_RUN",
              "SERVICE ONLY (shell retirement R8): the service's RUN command is Engine::startReceiver and a test fault cleared (engine.cpp, FOXAPI_OP_RUN); "
              "ServiceOptions::receive is main's decision (a real session, or a bounded run that asked for CASCADE_DECODE_TEST)",
              "engine_.startReceiver()"},
             {"the usage heartbeat", "", "engine_.configureHeartbeat()",
              "SERVICE ONLY (R8): ServiceOptions::heartbeat, on for a real session only (a bounded run would double-count one machine) - and right after "
              "the receiver, as the window's run() armed it (the window's update check, which is its own, stays in run() and is not a row)"},
             {"the diagnostics decided", "applyDiagnosticsEnabled(diagnosticsEnabled_)", "diagnosticsEnabled_ ? options_.diagnosticsDir",
              "each side applies the stored switch to what it owns: the window to its hang watchdog, the crash handler and the log; the service to the "
              "control thread's watchdog directory"},
             {"THE FORCED FIRST SAVE (the unclean-exit marker reaches the disk)", "", "saveNow()",
              "SERVICE ONLY (R8): after everything this thread does to the engine, before the pass thread exists to be raced", "saveConfigNow()"},
             {"the first pass", "", "engine_.controlPass()",
              "SERVICE ONLY (R8): made in the start so the first read is a real state (a stepped run has none: its first frame's front part is the first pass)"},
             {"the control thread", "", "engine_.startControlThread(",
              "SERVICE ONLY (R8): after the first save: from here the engine is read through its thread-safe surface only"},
         }},
        {"PRELUDE",
         {
             {"the presentation grace released", "presentGrace.release()", "",
              "WINDOW ONLY: a minimised window's pause must be released before the shutdown budget starts, or the teardown would run unwatched"},
             {"the sentinel's first shutdown step", "Phase::ShutdownBegun", "",
              "WINDOW ONLY: the service's three steps (stopping the receiver, unloading the plugins, writing the marker) follow it; ShutdownClosingWindow is closeWindow's"},
             {"the shutdown clock started", "watchdog_.beginShutdown()", "",
              "WINDOW ONLY: the window's hang watchdog judges everything from here against the shutdown budget - the engine's stop included, because closeWindow stops it last"},
             {"the plugin report's plugin half handed over", "handFrontEndFacts(true)", "",
              "WINDOW ONLY (shell retirement R4): a bounded-run diagnostic that needs the window's plugin poll and picture copy, and the port's tracks, paths and panels - "
              "so before the plugin UI is released; the ENGINE prints the report at the end of shutdownQuiesce"},
             {"the crash-report sweep stopped", "crashUploadFinish()", "",
              "WINDOW ONLY: the sweep is the window's (survey O6: no UI has consented on a headless service). BEFORE the last half of the settings, so the sweep's "
              "memory is in it, and before anything else on this path, so a transfer in flight is cancelled rather than waited out"},
             {"the feature-request sender cancelled", "featureRequestSender_.cancel()", "", "WINDOW ONLY: the window's sender"},
             {"the problem-report sender cancelled", "problemReportSender_.cancel()", "", "WINDOW ONLY: the window's sender"},
             {"THE LAST HALF OF THE SETTINGS handed to the engine and acknowledged", "flushInterfaceHalf()", "",
              "WINDOW ONLY: the engine's final-state save (STOP below) saves what the engine HOLDS, so the window's last second of changes must be in it first; "
              "after the sweep and the senders (their memory is part of the half), before the plugin UI's release"},
             {"THE PLUGIN UI RELEASED on this thread with the GL context current", "ui_->release()", "",
              "WINDOW ONLY: the map pages folded into the saved list, every instance destroyed, the basemap's tiles let go - BEFORE the engine's release and its "
              "clean-exit marker (a GUI-class plugin's destroy() is third-party code that can wedge, and a marker written before it reads a kill inside it as clean)"},
         }},
        {"STOP",
         {
             {"ONE WHOLE PASS, to take what the interface submitted last", "", "engine_.runPassNow(",
              "SERVICE ONLY (R8; ServiceOptions::finalPass): the window's last half of the settings is submitted as its frame loop ends, and a stepped run's pass "
              "only runs when someone steps it - so the stop makes one, bounded, while the control thread still runs and before anything is quiesced or saved"},
             {"NO SOUND CARD CLOSE WAITED FOR", "", "SoundCardSource::setCloseWaitEnabled(false)",
              "SERVICE ONLY (R8): a card whose close hangs must not spend the budget the radios' destroy is inside", "setCloseWaitEnabled("},
             {"quiesce: servers, the pass joined, the TRANSMITTER, GPS, recordings", "", "engine_.shutdownQuiesce()",
              "SERVICE ONLY (R8): a keyed radio cannot wait; everything the final save reads is settled", "engine_.shutdownQuiesce()"},
             {"THE FINAL-STATE SAVE (the clean-exit flag still false)", "", "saveNow()",
              "SERVICE ONLY (R8): before the pipeline stops so the snapshot reads live state; a death during the join is a death", "saveConfigNow()"},
             {"the shutdown wedge", "", "if (options_.shutdownStallMs > 0)",
              "SERVICE ONLY (R8): the bounded-run seam for the --diag-shutdown-stall test (ServiceOptions::shutdownStallMs): between the final-state save and "
              "the receiver's stop", "if (shutdownStallMs > 0)"},
             {"the sentinel's step: stopping the receiver", "", "Phase::ShutdownStoppingReceiver",
              "SERVICE ONLY (R8; ServiceOptions::breadcrumbs): set where the stop begins: a freeze in the stop is 'stopped in step 7' to the watcher",
              "Phase::ShutdownStoppingReceiver"},
             {"stop: the bookmark flush, the pipeline joined", "", "engine_.shutdownStop()",
              "SERVICE ONLY (R8): where the bounded driver waits are spent", "engine_.shutdownStop()"},
             {"the sentinel's step: unloading the plugins", "", "Phase::ShutdownUnloadingPlugins",
              "SERVICE ONLY (R8): set where the plugins come down", "Phase::ShutdownUnloadingPlugins"},
             {"RELEASE: the plugins destroyed BEFORE the marker", "", "engine_.shutdownRelease()",
              "SERVICE ONLY (R8): a plugin's destroy() is third-party code that can wedge; a marker written before it reads a kill inside it as clean",
              "engine_.shutdownRelease()"},
             {"the unload wedge", "", "if (options_.unloadStallMs > 0)",
              "SERVICE ONLY (R8): the place a plugin's destroy() runs, where the marker's own test stages its kill", "if (unloadStallMs > 0)"},
             {"the sentinel's step: writing the marker", "", "Phase::ShutdownWritingMarker",
              "SERVICE ONLY (R8): set where the marker stage begins; the window's ShutdownClosingWindow follows the drain and is closeWindow's",
              "Phase::ShutdownWritingMarker"},
             {"THE MARKER", "", "engine_.markCleanExit()", "SERVICE ONLY (R8): after the last of the third-party code", "engine_.markCleanExit()"},
             {"the marker's rewrite of the LAST REQUESTED snapshot", "", "marked.telemetryCleanExit = true",
              "SERVICE ONLY (R8): not a fresh snapshot: half torn down, its clocks are dead", "marked.telemetryCleanExit = true"},
             {"the marker queued", "", "requestSave(marked)", "SERVICE ONLY (R8): nothing has asked for another save in between", "requestConfigSave(marked)"},
             {"THE DEADLINE, taken before the wait it shares", "", "drainDeadline =",
              "SERVICE ONLY (R8): what the last save's wait leaves of it is what the drain gets", "drainDeadline ="},
             {"the last save waited for, bounded", "", "writer_.finishOrAbandon(",
              "SERVICE ONLY (R8): the one place that waits: bounded, then abandoned with a line in the log", "configWriter_.finishOrAbandon("},
             {"the drain", "", "engine_.shutdownDrain(drainDeadline)",
              "SERVICE ONLY (R8): the two lists' last saves and the takes' files, against the same deadline", "engine_.shutdownDrain("},
         }},
        {"CLOSE",
         {
             {"the plugin UI DISCONNECTED (after the engine's stop, not before)", "leavePluginUi()", "",
              "WINDOW ONLY: the engine's pass runs until the host's stop joins it, and an engine with no front end attached is a window-less one that wakes every "
              "asleep AUTO plugin - so the port stays connected through the stop (its release, posted before the engine unmaps, is applied inline by the port's own "
              "steps, idempotently) and is let go of here, with the GL context still current for the basemap's step"},
             {"the sentinel's last shutdown step", "Phase::ShutdownClosingWindow", "",
              "WINDOW ONLY: the marker stage is over (saved or abandoned) and the plugins are down: from here a normal exit is a clean one to the sentinel"},
             {"the GL textures the window owns, released with the context current", "releasePatchPictureTextures()", "",
              "WINDOW ONLY: a texture's deletion requires the creating context to be current"},
             {"the waterfall's texture", "waterfall_.reset()", "", "WINDOW ONLY: the same, and the window outlives the context"},
             {"ImGui's OpenGL backend", "ImGui_ImplOpenGL3_Shutdown()", "", "WINDOW ONLY: before its context and the GLFW window it draws into"},
             {"the ImGui context", "ImGui::DestroyContext()", "", "WINDOW ONLY"},
             {"the GLFW window", "glfwDestroyWindow(mainWindow_)", "", "WINDOW ONLY"},
             {"GLFW", "glfwTerminate()", "", "WINDOW ONLY"},
             {"THE WATCHDOG STOPPED LAST", "watchdog_.stop()", "",
              "WINDOW ONLY: everything the host did before this call - the whole of the engine's stop - and the GL teardown ran with no heartbeat on purpose, so a "
              "shutdown that wedges is reported like any other hang; stopping the watchdog before it would make the 120 s CAT freeze invisible again"},
             {"the sentinel's end", "Phase::Finished", "", "WINDOW ONLY: after the last line a clean end prints"},
         }},
    };
    return t;
}

// The regions of the files the table is held against. `ok` is false (and says which) when one cannot be cut.
struct Regions {
    std::string winRestore, winApply, winStart, winPrelude, winClose;
    std::string svcRestore, svcStart, svcStop;
    bool ok = true;
};

Regions cut(const std::string& winBlank, const std::string& svcBlank) {
    Regions r;
    r.winRestore = bodyOf(winBlank, "AppWindow::AppWindow(const WindowHost& host)");
    r.winApply = bodyOf(winBlank, "AppWindow::applyWindowHalf(");
    const std::string run = bodyOf(winBlank, "int AppWindow::run(int frames)");
    // Everything before the frame loop is where the engine used to be started; from the grace's release to the end is the window's prelude.
    const std::size_t loopAt = run.find("while (!glfwWindowShouldClose(window)");
    if (loopAt != std::string::npos) { r.winStart = run.substr(0, loopAt); }
    r.winPrelude = between(run, "presentGrace.release();", "return 0;");
    r.winClose = bodyOf(winBlank, "void AppWindow::closeWindow()");
    r.svcRestore = bodyOf(svcBlank, "EngineService::Restored EngineService::restore()");
    r.svcStart = bodyOf(svcBlank, "bool EngineService::start(");
    r.svcStop = bodyOf(svcBlank, "void EngineService::stop()");
    for (const std::string* s : {&r.winRestore, &r.winApply, &r.winStart, &r.winPrelude, &r.winClose, &r.svcRestore, &r.svcStart, &r.svcStop}) {
        if (s->empty()) { r.ok = false; }
    }
    return r;
}

// Hold the files against the table. Returns the number of things wrong, and says each.
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
            std::printf("  a region could not be cut: restore %zu/%zu apply %zu start %zu/%zu prelude %zu stop %zu close %zu bytes (window/service)\n",
                        r.winRestore.size(), r.svcRestore.size(), r.winApply.size(), r.winStart.size(), r.svcStart.size(), r.winPrelude.size(),
                        r.svcStop.size(), r.winClose.size());
        }
        return bad;
    }
    for (const Table& t : table()) {
        const std::string phase = t.phase;
        // APPLY has no region of its own in the service: it is the head of restore. PRELUDE and CLOSE are the window's alone: the service is held
        // against them only for absence (a WINDOW ONLY statement must not be in the service's stop).
        const std::string& win = phase == "RESTORE" ? r.winRestore
                                 : phase == "APPLY" ? r.winApply
                                 : phase == "START" ? r.winStart
                                 : phase == "CLOSE" ? r.winClose
                                                    : r.winPrelude;   // PRELUDE and STOP: the window's end of run()
        const std::string& svc = phase == "RESTORE" || phase == "APPLY" ? r.svcRestore : phase == "START" ? r.svcStart : r.svcStop;
        std::size_t lastWin = 0;
        std::size_t lastSvc = 0;
        const char* lastWhatWin = "";
        const char* lastWhatSvc = "";
        for (const Row& row : t.rows) {
            const std::string w = row.window;
            const std::string s = row.service;
            const std::string g = row.gone;
            // THE WINDOW's column. A SERVICE ONLY row has none: the window must not have grown the service's statement unlisted, nor kept its old spelling.
            if (w.empty()) {
                if (!s.empty() && win.find(s) != std::string::npos) {
                    fail("  %s: '%s' is SERVICE ONLY in the table but the window has it\n", t.phase, row.service);
                }
                if (!g.empty() && win.find(g) != std::string::npos) {
                    fail("  %s: the window still has '%s' - the statement left the window (the service has it)\n", t.phase, row.gone);
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
                        if (say) { std::printf("  %s: in the WINDOW '%s' comes before '%s' - the table's order is the contract\n", t.phase, row.what, lastWhatWin); }
                    }
                    lastWin = wAt;
                    lastWhatWin = row.what;
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
                fail("  %s: the service lost '%s' - the window had it\n", t.phase, row.service);
            } else if (countOf(svc, s) != 1) {
                fail("  %s: '%s' appears more than once in the service's region\n", t.phase, row.service);
            } else {
                if (sAt < lastSvc) {
                    ++bad;
                    if (say) { std::printf("  %s: in the SERVICE '%s' comes BEFORE '%s' - the window did not do that\n", t.phase, row.what, lastWhatSvc); }
                }
                lastSvc = sAt;
                lastWhatSvc = row.what;
            }
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
        // THE PLUGINS ARE RELEASED BEFORE THE CLEAN-EXIT MARKER IS WRITTEN (master 0.99.60; a merge once put them after it and test_clean_exit_marker found out): a plugin's
        // destroy() is third-party code that can wedge, and a marker written before it already said "clean". Stated directly here, beside the chain of rows that implies it.
        {
            const std::size_t releaseAt = stop.find("engine_.shutdownRelease()");
            const std::size_t markerAt = stop.find("engine_.markCleanExit()");
            need(releaseAt != std::string::npos && markerAt != std::string::npos && releaseAt < markerAt,
                 "RELEASE: the plugins are released BEFORE the clean-exit marker is written (a kill inside a plugin's destroy() must not read as a clean exit)");
            const std::size_t saveAt = stop.find("saveNow()");
            const std::size_t stopAt = stop.find("engine_.shutdownStop()");
            need(saveAt != std::string::npos && stopAt != std::string::npos && saveAt < stopAt,
                 "THE FINAL-STATE SAVE is taken BEFORE the pipeline stops (the snapshot reads live state; a death during the join is a death)");
        }
        // THE KEEPER THREAD (ServiceOptions::keeperThread, shell retirement R6) IS JOINED AT THE HEAD OF stop(): before the quiesce and before any save, so that
        // everything stop() does with the writer and the keeper's memory (the final saves, the marker, the waits) is on one thread again.
        {
            const std::size_t joinAt = stop.find("stopKeeper()");
            const std::size_t quiesceAt = stop.find("engine_.shutdownQuiesce()");
            const std::size_t saveAt = stop.find("saveNow()");
            const std::size_t passAt = stop.find("engine_.runPassNow(");
            need(joinAt != std::string::npos && quiesceAt != std::string::npos && saveAt != std::string::npos && joinAt < quiesceAt && joinAt < saveAt,
                 "the keeper thread is joined at the head of stop(), before the quiesce and the final-state save");
            need(passAt != std::string::npos && joinAt < passAt && passAt < quiesceAt,
                 "the final pass is made after the keeper is joined and BEFORE the quiesce (the control thread still runs; what the interface submitted last is applied)");
            const std::string destructor = bodyOf(svcBlank, "EngineService::~EngineService()");
            need(!destructor.empty() && destructor.find("stopKeeper()") != std::string::npos,
                 "the destructor joins the keeper thread of a service that was never stopped");
        }
        // The marker is not on any path that runs before the release: nothing in start() or keep() writes it.
        const std::string start = bodyOf(svcBlank, "bool EngineService::start(");
        need(start.find("telemetryCleanExit") == std::string::npos && keep.find("telemetryCleanExit") == std::string::npos,
             "start() and keep() never touch the clean-exit flag");
        // THE KEEPER IS STARTED BY THE START, after the control thread (R8: the window's frame loop no longer keeps the settings, so the thread must exist
        // for as long as the engine does): the last statement of start() that does work is the keeper's.
        {
            const std::size_t threadAt = start.find("engine_.startControlThread(");
            const std::size_t keeperAt = start.find("startKeeper()");
            need(threadAt != std::string::npos && keeperAt != std::string::npos && threadAt < keeperAt,
                 "the keeper thread is started by start(), after the control thread");
        }
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

// THE WINDOW HAS GIVEN UP THE ENGINE'S LIFE (shell retirement R8): nowhere in app_window.cpp is the engine started, kept running by a pass the window makes,
// or stopped - those are the service's, and the host calls them. What is left of the window's shutdown is the prelude and closeWindow (the table), the
// watchdog stops in exactly one place and it is closeWindow, and the plugin UI's disconnect is the one function both the prelude and the destructor call.
int checkWindowProperties(const std::string& winBlank, bool say) {
    int bad = 0;
    const auto need = [&](bool cond, const char* what) {
        if (!cond) {
            ++bad;
            if (say) { std::printf("  WINDOW: %s\n", what); }
        }
    };
    for (const char* call : {"engine_.startReceiver(", "engine_.controlPass(", "engine_.startControlThread(", "engine_.configureHeartbeat(", "engine_.markCleanExit(",
                             "engine_.shutdownQuiesce(", "engine_.shutdownStop(", "engine_.shutdownRelease(", "engine_.shutdownDrain(", "engine_.stopTransfers(",
                             "engine_.teardown("}) {
        const bool absent = winBlank.find(call) == std::string::npos;
        if (!absent) {
            ++bad;
            if (say) { std::printf("  WINDOW: app_window.cpp calls %s - the engine's life is the service's, and the host calls it\n", call); }
        }
    }
    const std::string run = bodyOf(winBlank, "int AppWindow::run(int frames)");
    const std::string close = bodyOf(winBlank, "void AppWindow::closeWindow()");
    need(!run.empty() && !close.empty(), "run() and closeWindow() found in src/gui/app_window.cpp");
    need(countOf(winBlank, "watchdog_.stop()") == 1 && close.find("watchdog_.stop()") != std::string::npos && run.find("watchdog_.stop()") == std::string::npos,
         "the hang watchdog is stopped in exactly one place, closeWindow(), and never in run()");
    need(run.find("glfwDestroyWindow(") == std::string::npos || run.find("glfwDestroyWindow(window)") < run.find("while (!glfwWindowShouldClose(window)"),
         "run() destroys the GL window only on its failure paths, before the frame loop (the normal end is closeWindow's)");
    need(run.find("glfwTerminate()") == std::string::npos || run.rfind("glfwTerminate()") < run.find("while (!glfwWindowShouldClose(window)"),
         "run() terminates GLFW only on its failure paths, before the frame loop");
    need(close.find("ui_->") == std::string::npos && close.find("leavePluginUi()") != std::string::npos && run.find("leavePluginUi()") == std::string::npos,
         "run() releases the plugin UI port but does NOT disconnect it (the engine's pass runs until the host's stop: disconnected, it is window-less and wakes asleep plugins); closeWindow() does");
    // THE WINDOW'S COPY OF THE KEEPER IS GONE (R8b): none of its names is in the window's code, and the window names the settings file's writer nowhere.
    for (const char* name : {"configWriter_", "savedCfg_", "pendingCfg_", "lastRequestedConfig_", "maybeSaveConfig", "saveConfigNow", "requestConfigSave",
                             "pollConfigWriter", "configsEqual", "currentConfig(", "noteConfigWrite", "configRetryHeld", "ConfigStore::writeFile"}) {
        if (winBlank.find(name) != std::string::npos) {
            ++bad;
            if (say) { std::printf("  WINDOW: app_window.cpp names %s - the settings file's keeper is the engine service's alone\n", name); }
        }
    }
    const std::string dtor = bodyOf(winBlank, "AppWindow::~AppWindow()");
    need(!dtor.empty() && dtor.find("leavePluginUi()") != std::string::npos, "~AppWindow disconnects the plugin UI port for the paths that never reached the prelude");
    return bad;
}

// THE WINDOW APPLICATION'S HOST (src/main.cpp; shell retirement R7, R8): the service is built with the window application's options, the lock is taken (a
// real session only), the settings are restored into the engine, THEN the window is constructed on it, THEN the service is started (after the window's
// plugin UI has attached - a control thread running before the attach would race it - and before the GL window exists), THEN the window runs, THEN the
// service is stopped (with the window still on screen and its watchdog armed), THEN the window's GL context is closed - and the host touches the engine
// nowhere in between. Held from the text of main(), with comments and strings blanked.
int checkHostMain(const std::string& hostBlank, bool say) {
    int bad = 0;
    const auto need = [&](bool cond, const char* what) {
        if (!cond) {
            ++bad;
            if (say) { std::printf("  HOST: %s\n", what); }
        }
    };
    const std::string body = bodyOf(hostBlank, "int main(");
    need(!body.empty(), "main() found in src/main.cpp");
    std::size_t last = 0;
    bool inOrder = true;
    for (const char* call : {"EngineService service(", "service.acquireConfig(", "service.restore()", "AppWindow app(host)", "service.start(", "app.run(",
                             "service.stop()", "app.closeWindow()", "relaunchWithSoftwareGl()"}) {
        const std::size_t at = body.find(call);
        need(at != std::string::npos, call);
        if (at != std::string::npos) {
            if (at < last) { inOrder = false; }
            last = at;
        }
    }
    need(inOrder,
         "main builds the service, takes the lock, restores, constructs the window, STARTS the service, runs the window, STOPS the service and closes the window - in that order");
    need(countOf(body, "service.restore()") == 1 && countOf(body, "AppWindow app(") == 1, "main restores once and constructs one window");
    need(countOf(body, "service.start(") == 1 && countOf(body, "service.stop()") == 1 && countOf(body, "app.closeWindow()") == 1 && countOf(body, "app.run(") == 1,
         "main starts, stops and closes once, and runs the window once");
    // The lock is a real session's only: a bounded run takes none.
    {
        const std::size_t lockAt = body.find("service.acquireConfig(");
        const std::size_t gate = body.rfind("boot.frames < 0", lockAt == std::string::npos ? 0 : lockAt);
        need(lockAt != std::string::npos && gate != std::string::npos && lockAt - gate < 60, "the lock is taken only when boot.frames < 0 (a bounded run takes none)");
    }
    // THE STOP IS REACHED WHETHER OR NOT THE RUN HAPPENED (the window could not be created, or is to be handed to the software renderer; the start refused):
    // it sits at the scope's own level, beside closeWindow, not inside the branch that runs the window.
    need(body.find("\n        service.stop();") != std::string::npos && body.find("\n        app.closeWindow();") != std::string::npos,
         "service.stop() and app.closeWindow() are at the scope's own level, reached on every path out of run()");
    // Between the start and the stop the host touches the engine only through the window's run: nothing else is said to the service or the engine.
    {
        const std::size_t a = body.find("service.start(");
        const std::size_t b = body.find("service.stop()");
        need(a != std::string::npos && b != std::string::npos && a < b &&
                 body.substr(a, b - a).find("service.engine()") == std::string::npos && body.substr(a, b - a).find(".applyCommand(") == std::string::npos &&
                 body.substr(a, b - a).find("service.keep(") == std::string::npos && body.substr(a, b - a).find("shutdown") == std::string::npos,
             "between the start and the stop the host does nothing else with the engine");
    }
    // The options are the window application's, from the one function the test host uses too; the measurement run decides the pass and is read once.
    need(body.find("windowServiceOptions(") != std::string::npos && body.find("serviceRun.frames = boot.frames;") != std::string::npos &&
             body.find("serviceRun.measuring = measure != nullptr;") != std::string::npos,
         "main builds the service's options with windowServiceOptions(), from boot.frames and the measurement run");
    need(countOf(body, "EngineMeasure::fromEnvironment()") == 1 && body.find("boot.frames >= 0") != std::string::npos &&
             body.find("app.setMeasurement(") == std::string::npos,
         "the measurement run is read once, in a bounded run only, and handed to the bounded run's hooks (not to the window: R9)");
    // THE BOUNDED RUN'S HOOKS (R9): built from the engine and the measurement in a bounded run ONLY, on the one line that tests boot.frames, and handed to the
    // window through WindowHost - before the window is constructed (the window copies them in its constructor).
    {
        const std::size_t hooks = body.find("makeBoundedHooks(service.engine(), std::move(measure))");
        const std::size_t gate = hooks == std::string::npos ? std::string::npos : body.rfind("boot.frames >= 0", hooks);
        const std::size_t winAt = body.find("AppWindow app(");
        need(hooks != std::string::npos && gate != std::string::npos && hooks - gate < 60 && countOf(body, "makeBoundedHooks(") == 1 && winAt != std::string::npos &&
                 hooks < winAt && body.find("host.bounded = ") != std::string::npos,
             "the bounded run's hooks are built only when boot.frames >= 0, from the service's engine and the measurement, and handed over in WindowHost before the window exists");
    }
    // What the restore found reaches the window by main (the window reads no file): every field of WindowHost is filled from the restore, none dropped.
    for (const char* field : {"host.configLoaded = restored.loaded;",
                              "host.lastRunUnclean = restored.lastRunUnclean;", "host.sourceKind = restored.sourceKind;",
                              "host.iqFilePath = restored.iqFilePath;", "host.api = ", "host.handle = ", "host.configPath = "}) {
        need(body.find(field) != std::string::npos, field);
    }
    // ...and the window is handed NO engine of its own (shell retirement R10: WindowHost::engine, the shell view's pointer, is deleted): the table and the handle are the whole of it.
    need(body.find("host.engine") == std::string::npos, "main.cpp hands the window no Engine pointer (WindowHost has none)");
    // The service is built inside the window's scope and before the window: destroyed after it, and before the software-GL relaunch.
    {
        const std::size_t svcAt = body.find("EngineService service(");
        const std::size_t winAt = body.find("AppWindow app(");
        const std::size_t relaunchAt = body.find("relaunchAndWait(");
        need(svcAt != std::string::npos && winAt != std::string::npos && relaunchAt != std::string::npos && svcAt < winAt && winAt < relaunchAt,
             "the service is built before the window and the relaunch comes after both");
    }
    return bad;
}

// THE OPTIONS THE WINDOW APPLICATION'S SERVICE IS BUILT WITH (src/engine/window_service_options.cpp; shell retirement R8): every decision the window's run() used
// to make for itself, held from the text. A real session starts the receiver and arms the heartbeat and is never stepped; a bounded run does neither
// (unless CASCADE_DECODE_TEST) and is stepped unless FOXSDR_PASS=free or a measurement; the wedges are read in a bounded run only; the keeper thread, the
// breadcrumbs, the best-effort lock, the window hermetic default and the front end that follows are the window application's in every run.
int checkFactory(const std::string& factoryBlank, bool say) {
    int bad = 0;
    const auto need = [&](bool cond, const char* what) {
        if (!cond) {
            ++bad;
            if (say) { std::printf("  OPTIONS: %s\n", what); }
        }
    };
    const std::string body = bodyOf(factoryBlank, "ServiceOptions windowServiceOptions(");
    need(!body.empty(), "windowServiceOptions() found in src/engine/window_service_options.cpp");
    const std::size_t bounded = body.find("if (!bounded) {");
    const std::size_t realEnd = body.find("return o;", bounded == std::string::npos ? 0 : bounded);
    need(bounded != std::string::npos && realEnd != std::string::npos, "the real-session branch (if (!bounded) { ... return o; }) is found");
    const std::string common = bounded == std::string::npos ? std::string() : body.substr(0, bounded);
    const std::string real = (bounded == std::string::npos || realEnd == std::string::npos) ? std::string() : body.substr(bounded, realEnd - bounded);
    const std::string boundedRun = realEnd == std::string::npos ? std::string() : body.substr(realEnd);
    for (const char* line : {"o.finalPass = true;", "o.lock = ServiceLock::BestEffort;", "o.hermetic = ServiceHermetic::Window;", "o.label = ", "o.diagToStdout = true;",
                             "o.frontEndFollows = true;", "o.breadcrumbs = true;", "o.keeperThread = true;"}) {
        need(common.find(line) != std::string::npos, line);
    }
    for (const char* line : {"o.receive = true;", "o.heartbeat = true;", "o.stepped = false;"}) {
        need(real.find(line) != std::string::npos, line);
    }
    need(boundedRun.find("o.heartbeat = false;") != std::string::npos && boundedRun.find("decodeHook != nullptr") != std::string::npos &&
             boundedRun.find("o.stepped = !wantFree && !run.measuring;") != std::string::npos,
         "a bounded run starts no receiver (unless CASCADE_DECODE_TEST), arms no heartbeat and is stepped unless FOXSDR_PASS=free or a measurement");
    need(boundedRun.find("o.shutdownStallMs = stallFromEnvironment(") != std::string::npos && boundedRun.find("o.unloadStallMs = stallFromEnvironment(") != std::string::npos &&
             common.find("stallFromEnvironment(") == std::string::npos && real.find("stallFromEnvironment(") == std::string::npos,
         "the two wedges are read in a bounded run only");
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
    const std::string hostText = readFile(root / "src" / "main.cpp");
    const std::string factoryText = readFile(root / "src" / "engine" / "window_service_options.cpp");
    CHECK(!winText.empty() && !svcText.empty() && !mainText.empty() && !hostText.empty() && !factoryText.empty());
    if (winText.empty() || svcText.empty() || mainText.empty() || hostText.empty() || factoryText.empty()) { return testSummary("test_engine_service_order"); }
    const std::string win = blanked(winText);
    const std::string svc = blanked(svcText);
    const std::string mn = blanked(mainText);
    const std::string host = blanked(hostText);
    const std::string factory = blanked(factoryText);

    // --- the table, the window's column and the service's, row by row -----------------------------------------------
    int rows = 0;
    for (const Table& t : table()) { rows += static_cast<int>(t.rows.size()); }
    std::printf("  %d rows in %zu phases\n", rows, table().size());
    CHECK(checkTable(win, svc, true) == 0);
    CHECK(checkProperties(svc, mn, true) == 0);
    CHECK(checkWindowProperties(win, true) == 0);
    CHECK(checkHostMain(host, true) == 0);
    CHECK(checkFactory(factory, true) == 0);

    // --- the checker is not blind: each mutant of the service's text must come back RED ------------------------------
    struct Mutant { const char* name; bool (*make)(std::string&); };
    const Mutant mutants[] = {
        {"the marker written BEFORE the plugins are released",
         [](std::string& s) { return swapStatements(s, "engine_.shutdownRelease();", "engine_.markCleanExit();"); }},
        {"the final-state save AFTER the pipeline is stopped",
         [](std::string& s) {
             return replaceOnce(s, "if (persistent()) { saveNow(); }\n\n    // The deliberate wedge in the place the real one lives", "// The deliberate wedge in the place the real one lives") &&
                    replaceOnce(s, "engine_.shutdownStop();", "engine_.shutdownStop();\n    if (persistent()) { saveNow(); }");
         }},
        {"the shutdown wedge moved BEFORE the final-state save",
         [](std::string& s) {
             const std::string wedgeLine = "if (options_.shutdownStallMs > 0) { wedge(\"shutdown-stall\", \"the teardown\", options_.shutdownStallMs); }";
             return replaceOnce(s, wedgeLine, "") && replaceOnce(s, "engine_.shutdownQuiesce();", "engine_.shutdownQuiesce();\n    " + wedgeLine);
         }},
        {"the keeper thread joined AFTER the quiesce (R6)",
         [](std::string& s) {
             return replaceOnce(s, "    stopKeeper();\n\n    // ONE WHOLE PASS", "    // ONE WHOLE PASS") &&
                    replaceOnce(s, "engine_.shutdownQuiesce();", "engine_.shutdownQuiesce();\n    stopKeeper();");
         }},
        {"the keeper thread never started by the start (R8)",
         [](std::string& s) { return replaceOnce(s, "if (options_.keeperThread && persistent()) { startKeeper(); }", ""); }},
        {"the keeper thread started BEFORE the control thread (R8)",
         [](std::string& s) {
             return replaceOnce(s, "if (options_.keeperThread && persistent()) { startKeeper(); }", "") &&
                    replaceOnce(s, "engine_.startControlThread(opt);", "if (options_.keeperThread && persistent()) { startKeeper(); }\n    engine_.startControlThread(opt);");
         }},
        {"the sentinel's unload step set AFTER the plugins are released",
         [](std::string& s) { return swapStatements(s, "phase(cascade::core::breadcrumb::Phase::ShutdownUnloadingPlugins);", "engine_.shutdownRelease();"); }},
        {"the usage heartbeat armed AFTER the forced first save",
         [](std::string& s) {
             const std::string hb = "if (options_.heartbeat) { engine_.configureHeartbeat(); }";
             return replaceOnce(s, hb, "") &&
                    replaceOnce(s, "if (persistent()) { saveNow(); }\n\n    cascade::engine::ControlThreadOptions opt;",
                                "if (persistent()) { saveNow(); }\n    " + hb + "\n\n    cascade::engine::ControlThreadOptions opt;");
         }},
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
        {"the final pass made AFTER the quiesce (the control thread already joined: nothing applies the window's last half)",
         [](std::string& s) {
             return replaceOnce(s, "if (options_.finalPass) { (void)engine_.runPassNow(kFinalPassBound); }", "") &&
                    replaceOnce(s, "engine_.shutdownQuiesce();", "engine_.shutdownQuiesce();\n    if (options_.finalPass) { (void)engine_.runPassNow(kFinalPassBound); }");
         }},
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
    // The window's text, mutated (R8): each statement of the window's part of the shutdown has a position, and each wrong one must come back RED.
    {
        struct WindowMutant { const char* name; bool (*make)(std::string&); };
        const WindowMutant windowMutants[] = {
            {"the plugin UI released AFTER the engine's stop (moved into closeWindow: after the clean-exit marker)",
             [](std::string& s) {
                 return replaceOnce(s, "    ui_->release();\n    return 0;", "    return 0;") &&
                        replaceOnce(s, "    glUp_ = false;\n", "    glUp_ = false;\n    ui_->release();\n");
             }},
            {"the plugin UI released BEFORE the last half is handed over",
             [](std::string& s) { return swapStatements(s, "(void)flushInterfaceHalf();", "ui_->release();"); }},
            {"the last half of the settings never flushed",
             [](std::string& s) { return replaceOnce(s, "    (void)flushInterfaceHalf();\n", ""); }},
            {"the crash-report sweep stopped AFTER the last half is handed over",
             [](std::string& s) { return swapStatements(s, "    crashUploadFinish();\n", "    (void)flushInterfaceHalf();\n"); }},
            {"the hang watchdog stopped in run(), BEFORE the engine's stop",
             [](std::string& s) {
                 return replaceOnce(s, "    watchdog_.stop();\n\n    // HOW LONG THE TEARDOWN", "\n    // HOW LONG THE TEARDOWN") &&
                        replaceOnce(s, "    ui_->release();\n    return 0;", "    ui_->release();\n    watchdog_.stop();\n    return 0;");
             }},
            {"the plugin UI never disconnected by closeWindow",
             [](std::string& s) { return replaceOnce(s, "    leavePluginUi();\n    // THE MARKER STAGE IS OVER", "    // THE MARKER STAGE IS OVER"); }},
            {"the plugin UI disconnected by the PRELUDE (the engine goes window-less while its pass still runs: asleep plugins woken in the middle of a shutdown)",
             [](std::string& s) { return replaceOnce(s, "    ui_->release();\n    return 0;", "    ui_->release();\n    leavePluginUi();\n    return 0;"); }},
            {"the window starts the receiver itself again",
             [](std::string& s) { return replaceOnce(s, "    // Bounded-run plugin-catalogue hook", "    engine_.startReceiver();\n    // Bounded-run plugin-catalogue hook"); }},
            {"the window stops the engine itself again (the quiesce in the prelude)",
             [](std::string& s) { return replaceOnce(s, "    (void)flushInterfaceHalf();\n", "    (void)flushInterfaceHalf();\n    engine_.shutdownQuiesce();\n"); }},
            {"the window saves the settings itself at the end again",
             [](std::string& s) { return replaceOnce(s, "    (void)flushInterfaceHalf();\n", "    saveConfigNow();\n    (void)flushInterfaceHalf();\n"); }},
            {"the window keeps its own copy of the settings writer again (the dead keeper)",
             [](std::string& s) { return replaceOnce(s, "    attachPluginUi();", "    configWriter_.bind(cascade::core::ConfigStore::writeFile);\n    attachPluginUi();"); }},
            {"the window saves the settings file itself again (a second writer beside the service's keeper)",
             [](std::string& s) { return replaceOnce(s, "    (void)flushInterfaceHalf();\n", "    (void)flushInterfaceHalf();\n    requestConfigSave(currentConfig());\n"); }},
            {"the destructor no longer disconnects the plugin UI",
             [](std::string& s) { return replaceOnce(s, "    leavePluginUi();\n}\n\nnamespace {", "}\n\nnamespace {"); }},
        };
        for (const WindowMutant& m : windowMutants) {
            std::string mutated = winText;
            const bool made = m.make(mutated);
            if (!made) { std::printf("  window mutant '%s' could not be made (its anchor text is gone from app_window.cpp): the test needs updating\n", m.name); }
            CHECK(made);
            if (!made) { continue; }
            const std::string mb = blanked(mutated);
            const int bad = checkTable(mb, svc, false) + checkWindowProperties(mb, false);
            std::printf("  window mutant '%s': %d finding(s)\n", m.name, bad);
            CHECK(bad > 0);
        }
    }
    // The window application's host (src/main.cpp), mutated: each must come back RED.
    {
        struct HostMutant { const char* name; bool (*make)(std::string&); };
        const HostMutant hostMutants[] = {
            {"the service restored AFTER the window is constructed (the survey's break-it)",
             [](std::string& s) {
                 return swapStatements(s, "const cascade::engine::EngineService::Restored restored = service.restore();", "cascade::gui::AppWindow app(host);");
             }},
            {"the lock taken AFTER the restore",
             [](std::string& s) {
                 return swapStatements(s, "if (boot.frames < 0) { (void)service.acquireConfig(lockSentence); }",
                                       "const cascade::engine::EngineService::Restored restored = service.restore();");
             }},
            {"the lock taken in a bounded run too",
             [](std::string& s) { return replaceOnce(s, "if (boot.frames < 0) { (void)service.acquireConfig(lockSentence); }", "(void)service.acquireConfig(lockSentence);"); }},
            {"the service STARTED BEFORE the window is constructed (the late-attach race)",
             [](std::string& s) {
                 return replaceOnce(s, "cascade::gui::AppWindow app(host);", "std::string earlyWhy; (void)service.start(earlyWhy);\n        cascade::gui::AppWindow app(host);") &&
                        replaceOnce(s, "if (service.start(startWhy)) {", "if (true) {");
             }},
            {"the service never started by main",
             [](std::string& s) { return replaceOnce(s, "if (service.start(startWhy)) {", "if (true) {"); }},
            {"the window closed BEFORE the service is stopped",
             [](std::string& s) { return swapStatements(s, "service.stop();", "app.closeWindow();"); }},
            {"the service never stopped by main",
             [](std::string& s) { return replaceOnce(s, "        service.stop();\n", ""); }},
            {"the service stopped only when the window ran (inside the branch)",
             [](std::string& s) {
                 return replaceOnce(s, "        service.stop();\n", "") && replaceOnce(s, "exitCode = app.run(boot.frames);", "exitCode = app.run(boot.frames);\n            service.stop();");
             }},
            {"the window never closed by main",
             [](std::string& s) { return replaceOnce(s, "        app.closeWindow();\n", ""); }},
            {"the engine driven by main between the start and the stop",
             [](std::string& s) { return replaceOnce(s, "exitCode = app.run(boot.frames);", "(void)service.engine().applyCommand(FoxCommand{});\n            exitCode = app.run(boot.frames);"); }},
            {"the options built by hand again (no windowServiceOptions)",
             [](std::string& s) { return replaceOnce(s, "windowServiceOptions(serviceRun)", "ServiceOptions()"); }},
            {"the measurement run read twice",
             [](std::string& s) { return replaceOnce(s, "serviceRun.measuring = measure != nullptr;", "serviceRun.measuring = cascade::core::EngineMeasure::fromEnvironment() != nullptr;"); }},
            {"the bounded run's hooks installed in a product run too (R9: the gate removed)",
             [](std::string& s) {
                 return replaceOnce(s, "if (boot.frames >= 0) { host.bounded = cascade::engine::makeBoundedHooks(service.engine(), std::move(measure)); }",
                                    "host.bounded = cascade::engine::makeBoundedHooks(service.engine(), std::move(measure));");
             }},
            {"the bounded run's hooks built AFTER the window is constructed (R9: the window copies them in its constructor)",
             [](std::string& s) {
                 return swapStatements(s, "if (boot.frames >= 0) { host.bounded = cascade::engine::makeBoundedHooks(service.engine(), std::move(measure)); }",
                                       "cascade::gui::AppWindow app(host);");
             }},
            {"the measurement handed to the window again (R9: it is the harness's)",
             [](std::string& s) { return replaceOnce(s, "app.setLinkTesterReveal(boot.linkTesterReveal);", "app.setLinkTesterReveal(boot.linkTesterReveal);\n        app.setMeasurement(nullptr);"); }},
            {"the saved I/Q path not handed to the window",
             [](std::string& s) { return replaceOnce(s, "host.iqFilePath = restored.iqFilePath;", ""); }},
            {"the last run's marker not handed to the window",
             [](std::string& s) { return replaceOnce(s, "host.lastRunUnclean = restored.lastRunUnclean;", ""); }},
        };
        for (const HostMutant& m : hostMutants) {
            std::string mutated = hostText;
            const bool made = m.make(mutated);
            if (!made) { std::printf("  host mutant '%s' could not be made (its anchor text is gone from main.cpp): the test needs updating\n", m.name); }
            CHECK(made);
            if (!made) { continue; }
            const int bad = checkHostMain(blanked(mutated), false);
            std::printf("  host mutant '%s': %d finding(s)\n", m.name, bad);
            CHECK(bad > 0);
        }
    }
    // The options' text, mutated: each must come back RED.
    {
        struct FactoryMutant { const char* name; bool (*make)(std::string&); };
        const FactoryMutant factoryMutants[] = {
            {"the keeper thread off (the window's frame loop no longer keeps the settings: nothing would)",
             [](std::string& s) { return replaceOnce(s, "o.keeperThread = true;", "o.keeperThread = false;"); }},
            {"no final pass (the window's last half of the settings never applied in a stepped run)",
             [](std::string& s) { return replaceOnce(s, "o.finalPass = true;", "o.finalPass = false;"); }},
            {"the lock strict (a second window refused)",
             [](std::string& s) { return replaceOnce(s, "o.lock = ServiceLock::BestEffort;", "o.lock = ServiceLock::Strict;"); }},
            {"the hermetic run headless (muted, the generator chosen)",
             [](std::string& s) { return replaceOnce(s, "o.hermetic = ServiceHermetic::Window;", "o.hermetic = ServiceHermetic::Headless;"); }},
            {"no breadcrumbs (the sentinel blind to the engine's stop)",
             [](std::string& s) { return replaceOnce(s, "o.breadcrumbs = true;", "o.breadcrumbs = false;"); }},
            {"the stall lines on stderr",
             [](std::string& s) { return replaceOnce(s, "o.diagToStdout = true;", "o.diagToStdout = false;"); }},
            {"no front end follows (the window-less restore: every fitted plugin started)",
             [](std::string& s) { return replaceOnce(s, "o.frontEndFollows = true;", "o.frontEndFollows = false;"); }},
            {"a bounded run arms the heartbeat",
             [](std::string& s) { return replaceOnce(s, "o.heartbeat = false;", "o.heartbeat = true;"); }},
            {"a real session not receiving",
             [](std::string& s) { return replaceOnce(s, "o.receive = true;", "o.receive = false;"); }},
            {"a real session stepped",
             [](std::string& s) { return replaceOnce(s, "o.stepped = false;", "o.stepped = true;"); }},
            {"a bounded run free-running by default",
             [](std::string& s) { return replaceOnce(s, "o.stepped = !wantFree && !run.measuring;", "o.stepped = false;"); }},
            {"the wedges read in a real session too",
             [](std::string& s) { return replaceOnce(s, "if (!bounded) {", "o.shutdownStallMs = stallFromEnvironment(\"CASCADE_DIAG_SHUTDOWN_STALL_MS\");\n    if (!bounded) {"); }},
        };
        for (const FactoryMutant& m : factoryMutants) {
            std::string mutated = factoryText;
            const bool made = m.make(mutated);
            if (!made) { std::printf("  options mutant '%s' could not be made (its anchor text is gone from window_service_options.cpp): the test needs updating\n", m.name); }
            CHECK(made);
            if (!made) { continue; }
            const int bad = checkFactory(blanked(mutated), false);
            std::printf("  options mutant '%s': %d finding(s)\n", m.name, bad);
            CHECK(bad > 0);
        }
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
