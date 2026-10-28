// test_look_order.cpp - THE ORDER IN WHICH A FRAME LOOKS AT WHAT THE PASS PUBLISHED (the pump move's step P4; the survey's 1.6 B and C).
//
// With the pass on a thread of its own, the window's look is a series of separate reads that a publish can fall between. The rule the
// look follows (AppWindow::drawUi; tests/test_engine_shell_guard.cpp holds the order of the calls as text): the session's results and
// events FIRST, the status lines next, THEN the state, the spectrum and the lists keyed on its sequence, and only then are the answers
// ACTED ON - against that state. Two properties, each with its control (the opposite order, which the test can see fail):
//
//   (Since shell retirement R3 the window's status lines are the table's fact.status.* - refreshStatusLines - and not Engine::statusText's copy. A and B hold
//   what the ENGINE publishes and what the window ACTS on; D holds that the window's lines ARE the engine's, key for key, and E the store's URL box; the
//   window's own order of the two reads is held as text by the guard. A below is still true of Engine::statusText, which the diagnostics report reads.)
//
//   A  THE STATUS LINES ARE NEVER FROM A LATER PASS THAN THE STATE. The pass publishes the state and then the lines
//      (Engine::pumpPublish); a reader that takes the lines and then the state therefore sees lines at most one pass OLD. A hook in the
//      gap between the two publishes holds the pass there; a reader thread checks, in the gap, that the lines it read are not newer
//      than the state it read. The control reads the state first and then the lines: there the lines are newer in the gap, every time.
//   B  AN ANSWER IS ACTED ON AGAINST A STATE AT LEAST AS NEW AS THE SNAPSHOT THAT RAISED IT. The table never shows a result before the
//      snapshot that carries its effect, so results read BEFORE the state are shown by it. The window's own pollUiAnswers /
//      refreshUiState / actOnUiAnswers, with the pass free-running at 1 ms and a stream of VFO-offset commands, rising, whose result names
//      the figure applied: when the window acts on that result, the state it has read shows that figure (or a later one). The control polls
//      the answers AFTER the state is read, with a pass allowed to run in between: the state is older than the result, often.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::engine::Engine;
using Clock = std::chrono::steady_clock;
namespace cmd = cascade::core::cmd;

namespace cascade::gui {
struct AppWindowTestAccess {
    static Engine& engine(AppWindow& a) { return th::engine(a); }
    static void betweenPublishes(void (*f)()) { Engine::testHooks_.betweenPublishes = f; }
    static void poll(AppWindow& a) { a.pollUiAnswers(); }
    // The window's own two reads, in drawUi's order: the status lines (the table's facts since shell retirement R3), then the state.
    static void look(AppWindow& a) {
        a.refreshStatusLines();
        a.refreshUiState();
    }
    static const cascade::engine::EngineStatusText& lines(AppWindow& a) { return a.statusLines_; }
    // R3: the engine's 22 sentences set, one distinct text per key (the key is in the text), the ADD ALL run failed or not, the fault sentence longer than the
    // window's first 512-byte read; and cleared again. Set with no pass running (the caller steps the pass itself).
    static void say(AppWindow& a, bool addAllFailed) {
        Engine& e = th::engine(a);
        e.sourceError_ = "text of sourceError";
        e.gpsRefusal_ = "text of gpsRefusal";
        e.catalogError_ = "text of catalogError";
        e.bandPlanError_ = "text of bandPlanError";
        e.tuneMismatchNote_ = "text of tuneMismatchNote";
        e.transmitError_ = "text of transmitError";
        e.soundCardMissing_ = "text of soundCardMissing";
        e.sdrPlayApiDetail_ = "text of sdrPlayApiDetail";
        e.sdrPlayAdvice_ = "text of sdrPlayAdvice";
        e.recordNotice_ = "text of recordNotice";
        e.recordError_ = "text of recordError";
        e.presetNote_ = "text of presetNote";
        e.pluginEnforceError_ = "text of pluginEnforceError";
        e.restoreKeepLabel_ = "text of restoreKeepLabel";
        e.audioHealthNote_ = "text of audioHealth";
        e.bookmarkImportNote_ = "text of bookmarkNote";
        e.bookmarkError_ = "text of bookmarkError";
        e.installReport_ = "text of installReport";
        e.installError_ = "text of installError";
        e.catalogStatus_ = "text of catalogStatus";
        e.addAllSummary_ = "text of addAllSummary";
        e.addAllFailed_ = addAllFailed;
        e.testFault_ = std::string("text of deviceFault ") + std::string(900, 'f');
    }
    static void hush(AppWindow& a) {
        Engine& e = th::engine(a);
        for (std::string* t : {&e.sourceError_, &e.gpsRefusal_, &e.catalogError_, &e.bandPlanError_, &e.tuneMismatchNote_, &e.transmitError_,
                               &e.soundCardMissing_, &e.sdrPlayApiDetail_, &e.sdrPlayAdvice_, &e.recordNotice_, &e.recordError_, &e.presetNote_,
                               &e.pluginEnforceError_, &e.restoreKeepLabel_, &e.audioHealthNote_, &e.bookmarkImportNote_, &e.bookmarkError_,
                               &e.installReport_, &e.installError_, &e.catalogStatus_, &e.addAllSummary_, &e.testFault_}) {
            t->clear();
        }
        e.addAllFailed_ = false;
    }
    static void pass(AppWindow& a) { th::engine(a).controlPass(); }
    static std::string url(AppWindow& a) { return a.pluginUrlBuf_; }
    static void typeUrl(AppWindow& a, const std::string& v) { std::snprintf(a.pluginUrlBuf_, sizeof(a.pluginUrlBuf_), "%s", v.c_str()); }
    static bool seeded(AppWindow& a) { return a.pluginUrlSeeded_; }
    static void unseed(AppWindow& a) { a.pluginUrlSeeded_ = false; }   // (what AppWindow::applyConfig does)
    static std::string engineUrl(AppWindow& a) { return th::engine(a).pluginCatalogueUrl_; }
    static void setEngineUrl(AppWindow& a, const std::string& v) { th::engine(a).pluginCatalogueUrl_ = v; }
    static void act(AppWindow& a) { a.actOnUiAnswers(); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static std::uint64_t busyReads(AppWindow& a) { return a.uiStateBusyReads_; }
    // A look whose state read came back BUSY: the last good state stands and the flag says so (what refreshUiState does itself).
    static void pretendBusy(AppWindow& a) { a.uiStateFresh_ = false; }
    static int pending(AppWindow& a) { return a.uiAnswers_.nResults; }
    static void tap(AppWindow& a, std::function<void(const FoxCommandResult&)>* f) { a.uiAnswerTap_ = f; }
    static void start(AppWindow& a, int periodMs) {
        cascade::engine::ControlThreadOptions o;
        o.period = std::chrono::milliseconds(periodMs);
        o.floor = std::chrono::milliseconds(0);
        o.watchdogThresholdMs = 20000;
        th::engine(a).startControlThread(o);
    }
    static cascade::core::PublishedState published(Engine& e) {
        cascade::core::PublishedState ps;
        for (int i = 0; i < 100; ++i) {
            if (e.receiverSnapshot_->read(ps)) { break; }
        }
        return ps;
    }
};
}  // namespace cascade::gui
using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
void spinUs(int us) {
    const auto end = Clock::now() + std::chrono::microseconds(us);
    while (Clock::now() < end) { std::this_thread::yield(); }
}

// ---- A -------------------------------------------------------------------------------------------------------------------------
std::atomic<long> g_gapCalls{0};
void gapHook() {   // hold the pass between the state's publish and the lines'
    ++g_gapCalls;
    spinUs(1500);
}

int idxOfStatus(const std::string& s) { return s.rfind("n=", 0) == 0 ? std::atoi(s.c_str() + 2) : -1; }
int idxOfVolume(double v) { return static_cast<int>(std::lround(v * 1000.0)); }

struct Seen {
    long reads = 0;
    long linesNewerThanState = 0;     // the lines carried a later round than the state in the same pair of reads
    int maxSkewBehind = 0;            // how far the lines were behind the state, in rounds
};

Seen readerRun(Engine& e, bool linesFirst, std::atomic<bool>& go) {
    Seen out;
    while (go.load()) {
        int lines = -1;
        double vol = 0.0;
        if (linesFirst) {
            lines = idxOfStatus(e.statusText().sourceError);
            vol = A::published(e).rx.volume;
        } else {
            vol = A::published(e).rx.volume;
            lines = idxOfStatus(e.statusText().sourceError);
        }
        const int state = idxOfVolume(vol);
        ++out.reads;
        if (lines > state) { ++out.linesNewerThanState; }
        if (state - lines > out.maxSkewBehind && lines >= 0) { out.maxSkewBehind = state - lines; }
        std::this_thread::yield();
    }
    return out;
}

Seen statusRun(bool linesFirst) {
    Engine e;
    e.initialise();
    cascade::engine::ControlThreadOptions o;
    o.period = std::chrono::milliseconds(1);
    o.floor = std::chrono::milliseconds(0);
    o.watchdogThresholdMs = 20000;
    A::betweenPublishes(&gapHook);   // (a test hook is set before the pass that reads it exists, and cleared after it is gone)
    e.startControlThread(o);
    // The two changes go in ONE submit on a session of their own, so the pass that applies one applies both (two submitCommand calls
    // wake the pass separately and a pass can run between them).
    const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    FoxSession* ses = nullptr;
    {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_ADMIN;
        p.clientName = "look order";
        CHECK(api->open_session(e.apiHandle(), &p, &ses) == FOXAPI_OK);
    }
    std::atomic<bool> go{true};
    Seen seen;
    std::thread reader([&] { seen = readerRun(e, linesFirst, go); });
    for (int i = 1; i <= 120; ++i) {
        // BOTH CHANGES IN ONE DRAIN, so they land in the same pass: the lines say "n=i" and the state says volume i/1000.
        FoxCommand batch[2];
        batch[0] = cmd::makeText(FOXAPP_OP_SET_SOURCE_ERROR, "n=" + std::to_string(i)).c;
        batch[1] = cmd::makeNum(FOXAPI_OP_SET_VOLUME, static_cast<double>(i) / 1000.0);
        CHECK(api->submit(ses, batch, 2, nullptr) >= 0);
        FoxCommandResult rr[16];
        for (auto& x : rr) { x.structSize = sizeof(x); }
        while (api->poll_results(ses, rr, 16) == 16) {}
        const auto t0 = Clock::now();
        while (idxOfVolume(A::published(e).rx.volume) < i && msSince(t0) < 3000.0) { std::this_thread::yield(); }
        while (idxOfStatus(e.statusText().sourceError) < i && msSince(t0) < 3000.0) { std::this_thread::yield(); }
    }
    go = false;
    reader.join();
    api->close_session(ses);
    e.stopControlThread();
    A::betweenPublishes(nullptr);
    return seen;
}

void testStatusThenState() {
    std::printf("  A: the status lines are never from a later pass than the state\n");
    g_gapCalls = 0;
    const Seen good = statusRun(/*linesFirst=*/true);
    std::printf("      lines then state (the window's order): %ld pairs read over %ld held passes, lines newer than the state in %ld, "
                "at most %d round(s) behind it\n",
                good.reads, g_gapCalls.load(), good.linesNewerThanState, good.maxSkewBehind);
    CHECK(good.reads > 100);
    CHECK(g_gapCalls.load() >= 100);            // the hook held the pass in the gap, so the reader was there to see it
    CHECK(good.maxSkewBehind >= 1);             // ...and did see lines one round BEHIND the state: the lag the order allows
    CHECK(good.linesNewerThanState == 0);       // never ahead. (Its control is the mutant that publishes the lines first: docs/engine-stage4.md.)
}

// ---- B -------------------------------------------------------------------------------------------------------------------------
struct AnswerRun {
    long answers = 0;
    long stateOlderThanAnswer = 0;
    unsigned long long busyReads = 0;   // state reads that came back BUSY: their answers were held to the next look
};

AnswerRun answerRun(bool answersFirst) {
    th::TestWindow app;
    A::start(app, 1);
    AnswerRun out;
    std::function<void(const FoxCommandResult&)> tap = [&](const FoxCommandResult& r) {
        if (r.op != FOXAPI_OP_SET_VFO_OFFSET || r.status != FOXAPI_OK) { return; }
        ++out.answers;
        // The result names the figure that was applied; the state read for this look must show it (or a later one).
        if (A::state(app).vfoOffsetHz < r.applied[0]) { ++out.stateOlderThanAnswer; }
    };
    A::tap(app, &tap);
    const auto t0 = Clock::now();
    int n = 0;
    while (msSince(t0) < 1200.0) {
        A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, static_cast<double>(++n)));       // strictly rising offsets (clamped by the span: still never falling)
        if (answersFirst) {
            A::poll(app);       // the answers first...
            A::look(app);       // ...then the state...
            A::act(app);        // ...and only then are they acted on
        } else {
            A::look(app);       // THE CONTROL: the state first...
            spinUs(300);        // ...a pass may publish here...
            A::poll(app);       // ...and the answers it raised are read after...
            A::act(app);        // ...and acted on against the state that does not show them yet
        }
        std::this_thread::yield();
    }
    A::tap(app, nullptr);
    out.busyReads = A::busyReads(app);
    return out;
}

void testAnswersAfterTheState() {
    std::printf("  B: an answer is acted on against a state at least as new as the snapshot that raised it\n");
    const AnswerRun good = answerRun(true);
    const AnswerRun control = answerRun(false);
    std::printf("      answers, state, act (the window's order): %ld offset answers, the state older than the answer in %ld\n", good.answers,
                good.stateOlderThanAnswer);
    std::printf("      state, answers, act (the control):         %ld offset answers, the state older than the answer in %ld\n",
                control.answers, control.stateOlderThanAnswer);
    std::printf("      state reads that came back BUSY (their answers waited for the next look): %llu in the window's order, %llu in the control\n",
                good.busyReads, control.busyReads);
    CHECK(good.answers > 50);
    CHECK(good.stateOlderThanAnswer == 0);
    CHECK(control.stateOlderThanAnswer > 0);
}

// ---- C -------------------------------------------------------------------------------------------------------------------------
// A STATE READ THAT CAME BACK BUSY HOLDS THE ANSWERS BACK. Found by the P6 soak of case B: a read that keeps overlapping a publish answers
// BUSY, the last good state stands, and the answers the frame had just polled were acted on against it - a state OLDER than the figure the
// answer names, in about one answer in 100 000 (4 runs of 50). Deterministic here: the look is declared BUSY, the answers must stay in
// the buffer (not acted on, not lost), and the next look that did read acts on them, against a state that shows them.
void testBusyHoldsAnswersBack() {
    std::printf("  C: a BUSY state read holds the answers back until a look that read\n");
    th::TestWindow app;
    A::start(app, 1);
    long acted = 0;
    long olderThanAnswer = 0;
    std::function<void(const FoxCommandResult&)> tap = [&](const FoxCommandResult& r) {
        if (r.op != FOXAPI_OP_SET_VFO_OFFSET || r.status != FOXAPI_OK) { return; }
        ++acted;
        if (A::state(app).vfoOffsetHz < r.applied[0]) { ++olderThanAnswer; }
    };
    A::tap(app, &tap);
    A::look(app);
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 777.0));
    const auto t0 = Clock::now();
    while (A::pending(app) == 0 && msSince(t0) < 3000.0) {
        A::poll(app);
        std::this_thread::yield();
    }
    CHECK(A::pending(app) >= 1);              // the answer is in the buffer
    A::pretendBusy(app);                      // ...and the look that follows could not read the state
    A::act(app);
    CHECK(acted == 0);                        // not acted on against the old state
    CHECK(A::pending(app) >= 1);              // ...and not lost
    for (int i = 0; i < 200 && acted == 0; ++i) {   // the next look that reads (the answer is a pass old by now: the state shows it)
        A::look(app);
        A::act(app);
    }
    CHECK(acted >= 1);
    CHECK(olderThanAnswer == 0);
    CHECK(A::pending(app) == 0);
    A::tap(app, nullptr);
}

// ---- D -------------------------------------------------------------------------------------------------------------------------
// THE WINDOW'S STATUS LINES ARE THE ENGINE'S, KEY FOR KEY (shell retirement R3). The window read Engine::statusText()'s copy; it reads fact.status.<key> now,
// through refreshStatusLines, and a key read into the wrong field, a key missing, or a flag lost would put a sentence on the wrong panel. The engine's 22
// sentences are each set to a text that names its key; after the pass has published them the window's lines equal the copy field by field, the fault sentence
// (900 bytes, past the first 512-byte read) whole, and ADD ALL's level follows the engine's flag. Said no more, they are empty in the window the next look.
void testWindowLinesAreTheEngines() {
    std::printf("  D: the window's status lines (fact.status.*) are the engine's, key for key\n");
    th::TestWindow app;
    // BEFORE THE FIRST PUBLISH a fact answers nothing: the lines are empty and the look says so.
    A::look(app);
    CHECK(A::lines(app).sourceError.empty() && A::lines(app).deviceFault.empty() && !A::lines(app).addAllFailed);
    for (const bool failed : {false, true}) {
        A::say(app, failed);
        A::pass(app);
        A::look(app);
        const cascade::engine::EngineStatusText copy = th::engine(app).statusText();
        const cascade::engine::EngineStatusText& w = A::lines(app);
        const auto same = [&](const char* key, const std::string& window, const std::string& engine) {
            const bool ok = window == engine && window.rfind(std::string("text of ") + key, 0) == 0;
            if (!ok) { std::printf("      FAIL %s: window '%.40s', engine '%.40s'\n", key, window.c_str(), engine.c_str()); }
            CHECK(ok);
        };
        same("sourceError", w.sourceError, copy.sourceError);
        same("gpsRefusal", w.gpsRefusal, copy.gpsRefusal);
        same("catalogError", w.catalogError, copy.catalogError);
        same("bandPlanError", w.bandPlanError, copy.bandPlanError);
        same("tuneMismatchNote", w.tuneMismatchNote, copy.tuneMismatchNote);
        same("transmitError", w.transmitError, copy.transmitError);
        same("soundCardMissing", w.soundCardMissing, copy.soundCardMissing);
        same("sdrPlayApiDetail", w.sdrPlayApiDetail, copy.sdrPlayApiDetail);
        same("sdrPlayAdvice", w.sdrPlayAdvice, copy.sdrPlayAdvice);
        same("recordNotice", w.recordNotice, copy.recordNotice);
        same("recordError", w.recordError, copy.recordError);
        same("presetNote", w.presetNote, copy.presetNote);
        same("pluginEnforceError", w.pluginEnforceError, copy.pluginEnforceError);
        same("restoreKeepLabel", w.restoreKeepLabel, copy.restoreKeepLabel);
        same("audioHealth", w.audioHealth, copy.audioHealth);
        same("bookmarkNote", w.bookmarkNote, copy.bookmarkNote);
        same("bookmarkError", w.bookmarkError, copy.bookmarkError);
        same("installReport", w.installReport, copy.installReport);
        same("installError", w.installError, copy.installError);
        same("catalogStatus", w.catalogStatus, copy.catalogStatus);
        same("addAllSummary", w.addAllSummary, copy.addAllSummary);
        same("deviceFault", w.deviceFault, copy.deviceFault);
        CHECK(w.deviceFault.size() > 800u);
        CHECK((w.addAllFailed) == (failed));
        CHECK((copy.addAllFailed) == (failed));
    }
    A::hush(app);
    A::pass(app);
    A::look(app);
    CHECK(A::lines(app).sourceError.empty() && A::lines(app).deviceFault.empty() && A::lines(app).addAllSummary.empty() && !A::lines(app).addAllFailed);
}

// ---- E -------------------------------------------------------------------------------------------------------------------------
// THE STORE'S URL BOX IS SEEDED AT THE FIRST LOOK THAT FINDS THE FACT (shell retirement R3; it was read from the engine in the constructor and again in
// applyConfig). Before the first publish there is no fact and the box is empty and not yet seeded; the first look after the pass has published seeds it from
// fact.store.url; what the person types after that is theirs - a later look, and a later change of the engine's URL, do not overwrite it; applyConfig (which clears
// the flag) puts the engine's URL in the box again at the next look.
void testUrlBoxIsSeededOnce() {
    std::printf("  E: the store's URL box is seeded from fact.store.url at the first look that finds it, once\n");
    th::TestWindow app;
    CHECK(!A::seeded(app));
    CHECK(A::url(app).empty());
    A::look(app);                                  // no publish yet: the fact is not there
    CHECK(!A::seeded(app));
    CHECK(A::url(app).empty());
    A::pass(app);
    A::look(app);
    CHECK(A::seeded(app));
    CHECK(!A::url(app).empty());
    CHECK((A::url(app)) == (A::engineUrl(app)));      // the engine's default index
    A::typeUrl(app, "typed by the person");
    A::setEngineUrl(app, "https://example.test/other/index.json");
    A::pass(app);
    A::look(app);
    CHECK((A::url(app)) == (std::string("typed by the person")));   // seeded once: the box is theirs now
    A::unseed(app);                                // applyConfig's act
    A::look(app);                                  // the engine has published its URL already (the last pass): the next look puts it in the box
    CHECK((A::url(app)) == (std::string("https://example.test/other/index.json")));
    CHECK(A::seeded(app));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_look_order\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_look_order_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testStatusThenState();
    testAnswersAfterTheState();
    testBusyHoldsAnswersBack();
    testWindowLinesAreTheEngines();
    testUrlBoxIsSeededOnce();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_look_order");
}
