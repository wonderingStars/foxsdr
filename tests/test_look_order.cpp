// test_look_order.cpp - THE ORDER IN WHICH A FRAME LOOKS AT WHAT THE PASS PUBLISHED (the pump move's step P4; the survey's 1.6 B and C).
//
// With the pass on a thread of its own, the window's look is a series of separate reads that a publish can fall between. The rule the
// look follows (AppWindow::drawUi; tests/test_engine_shell_guard.cpp holds the order of the calls as text): the session's results and
// events FIRST, the status lines next, THEN the state, the spectrum and the lists keyed on its sequence, and only then are the answers
// ACTED ON - against that state. Two properties, each with its control (the opposite order, which the test can see fail):
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
    static void look(AppWindow& a) {
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
    }
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

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_look_order");
}
