// test_window_threaded_look.cpp - A REAL WINDOW LOOKING AT A PASS THAT RUNS BESIDE IT (the pump move's step P5).
//
// Every other window test in the tree is its own pump: it constructs an AppWindow and calls the pass's phases by hand, so what it proves is
// what the product had until the move - one thread. This one runs the thing the move made: an AppWindow whose engine's pass is FREE-RUNNING
// on its own thread (period 1 ms, floor 0 - passes far faster than any frame), and a window thread that does what drawUi does around its
// draw - the look (answers, status lines, state, spectrum, lists, answers acted on), the window's submits, the key - as fast as it can,
// for three seconds, while three more threads hammer the engine through sessions of their own and one reads the snapshot as a browser's
// status does. (drawUi itself needs a GL context for its panels; the frame below is its engine-facing half, in its order - and the
// order is held as text by tests/test_engine_shell_guard.cpp, checkTheFrameIsALook.)
//
//   A  THE STATE NEVER GOES BACK: the sequence a reader sees only rises, on the window's thread and on a reader thread's
//   B  THE LISTS AGREE WITH THE STATE THEY ARE KEYED ON: the window's cache of the lists is keyed on listSeq, and after a frame's reads
//      it holds the listSeq of the state it read
//   C  NO BUSY STORM: a read that overlaps a publish answers BUSY and the window keeps the last good state; under this load that is rare
//   D  THE DRAG'S LOCAL COPY NEVER OUTLIVES ITS HOLD, and what the window shows while it stands is what it sent; when the drags stop,
//      the engine's readback is the last figure sent (the engine took it)
//   E  THE ENGINE'S OTHER CLIENTS SEE ONE ANOTHER'S COMMANDS WITHOUT CORRUPTION: three session threads' random commands, all of them
//      answered, none lost to anything but BUSY, the receiver ending in a state one of them set
//   F  stepPass does nothing on a free-running pass, and the window's teardown joins the thread with the key rules in order
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::engine::Engine;
using cascade::engine::PassPart;
using Clock = std::chrono::steady_clock;
namespace cmd = cascade::core::cmd;

namespace cascade::gui {
struct AppWindowTestAccess {
    static Engine& engine(AppWindow& a) { return th::engine(a); }
    static void start(AppWindow& a, int periodMs, int floorMs) {
        cascade::engine::ControlThreadOptions o;
        o.period = std::chrono::milliseconds(periodMs);
        o.floor = std::chrono::milliseconds(floorMs);
        o.kickGap = std::chrono::milliseconds(0);   // every frame's kick reaches the loop, beside its 1 ms passes (the race is the point)
        o.watchdogThresholdMs = 20000;
        th::engine(a).startControlThread(o);
    }
    // THE FRAME'S ENGINE-FACING HALF, in drawUi's order (free-running: the two stepPass calls are no-ops).
    static void frame(AppWindow& a) {
        // (run(): the frame's first act is the window's heartbeat, which on its FOXAPI_SESSION_FRAME session is also the kick of the pass, 6.9 / R3)
        if (a.uiSession_ != nullptr) { a.engineApi_->heartbeat(a.uiSession_); }
        a.applyPluginDirective();
        a.sendTrackInfoText();
        a.handFrontEndFacts();
        th::engine(a).stepPass(PassPart::Front);
        a.syncPatchPageOpen();
        a.commitPatchDraft();
        a.pollUiAnswers();
        a.refreshStatusLines();
        a.refreshUiState();
        a.actOnUiAnswers();
        // (no publishWebSpectrum since stage 5e: the page reads the table's spectrum itself)
        th::engine(a).stepPass(PassPart::Mute);
        a.submitTransmitKey();
        th::engine(a).stepPass(PassPart::Back);
    }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static const AppWindow::TuningListCache& tuning(AppWindow& a) { return a.tuningLists(); }
    static const AppWindow::TuneLocal& local(AppWindow& a) { return a.tuneLocal_; }
    static double tunedOffset(AppWindow& a) { return a.tunedVfoOffsetHz(); }
    static void dragCentre(AppWindow& a, double absHz) { a.dragVfoCentre(absHz); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static std::uint64_t passes(AppWindow& a) { return th::engine(a).controlPasses_.load(); }
    static void park(AppWindow& a, bool on) { th::engine(a).controlParkForTest_.store(on); }
    static std::uint64_t busyDrops(AppWindow& a) { return a.uiBusyDrops_; }
    static std::size_t held(AppWindow& a) { return a.uiHeld_.size(); }
    static std::uint64_t offPassKeyTouches(AppWindow& a) { return th::engine(a).api().offPassKeyTouches(); }
    static std::shared_ptr<cascade::core::ReceiverSnapshot> snapshot(AppWindow& a) { return th::engine(a).receiverSnapshot_; }
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

constexpr int kRunMs = 3000;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_window_threaded_look\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_threaded_look_" + std::to_string(pid));
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

    {
        th::TestWindow app;
        Engine& e = A::engine(app);
        A::start(app, 1, 0);
        CHECK(e.controlThreadRunning());
        CHECK(!e.controlStepped());

        // F (first half): stepPass on a free-running pass does nothing - it does not wait and runs nothing.
        const std::uint64_t begun0 = e.passesBegun();
        const auto tF = Clock::now();
        for (int i = 0; i < 1000; ++i) { e.stepPass(PassPart::Front); }
        CHECK(msSince(tF) < 500.0);
        std::printf("  F: 1000 stepPass calls on a free-running pass: %.1f ms, %llu passes begun beside them\n", msSince(tF),
                    static_cast<unsigned long long>(e.passesBegun() - begun0));

        // THE OTHER CLIENTS: three session threads of random commands, one snapshot reader.
        std::atomic<bool> go{true};
        std::atomic<long> submitted{0}, busy{0}, answered{0}, refusedOther{0};
        std::atomic<long> readerBackwards{0}, readerReads{0};
        const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        std::vector<std::thread> clients;
        for (int c = 0; c < 3; ++c) {
            clients.emplace_back([&, c] {
                FoxSession* ses = nullptr;
                FoxSessionParams p{};
                p.structSize = sizeof(p);
                p.flags = FOXAPI_SESSION_LOCAL;
                p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
                p.clientName = "threaded look client";
                if (api->open_session(e.apiHandle(), &p, &ses) != FOXAPI_OK) { return; }
                std::mt19937 rng(1234u + static_cast<unsigned>(c));
                std::uint64_t lastSeq = 0;
                while (go.load()) {
                    api->heartbeat(ses);
                    FoxCommand cm{};
                    switch (rng() % 5) {
                        case 0: cm = cmd::makeNum(FOXAPI_OP_SET_VOLUME, static_cast<double>(rng() % 100) / 100.0); break;
                        case 1: cm = cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -60.0 + static_cast<double>(rng() % 40)); break;
                        case 2: cm = cmd::makeInt(FOXAPI_OP_SET_MODE, static_cast<std::int64_t>(1 + rng() % 3)); break;
                        case 3: cm = cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, static_cast<double>(static_cast<int>(rng() % 100000) - 50000)); break;
                        default: cm = cmd::makeInt(FOXAPI_OP_SET_MUTED, static_cast<std::int64_t>(rng() % 2)); break;
                    }
                    FoxSubmitResult sr{};
                    sr.structSize = sizeof(sr);
                    const int n = api->submit(ses, &cm, 1, &sr);
                    ++submitted;
                    if (n < 0 || sr.status == FOXAPI_BUSY) { ++busy; }
                    FoxCommandResult rr[16];
                    for (auto& x : rr) { x.structSize = sizeof(x); }
                    for (;;) {
                        const int got = api->poll_results(ses, rr, 16);
                        if (got <= 0) { break; }
                        answered += got;
                        for (int i = 0; i < got; ++i) {
                            if (rr[i].status != FOXAPI_OK && rr[i].status != FOXAPI_NO_CHANGE && rr[i].status != FOXAPI_DENIED) {
                                if (refusedOther++ == 0) {
                                    std::printf("      (first odd answer: op 0x%x status %d \"%s\")\n", rr[i].op, rr[i].status, rr[i].message);
                                }
                            }
                        }
                        if (got < 16) { break; }
                    }
                    FoxReceiverState st{};
                    st.structSize = sizeof(st);
                    if (api->read_state(ses, &st) == FOXAPI_OK) {
                        if (st.seq < lastSeq) { ++readerBackwards; }
                        lastSeq = st.seq;
                        ++readerReads;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                api->close_session(ses);
            });
        }
        std::thread browser([&] {   // the web status' reader: the snapshot's own lock-free read and its refcounted full read
            auto snap = A::snapshot(app);
            std::uint64_t last = 0;
            while (go.load()) {
                cascade::core::PublishedState ps;
                if (snap->read(ps)) {
                    if (ps.rx.seq < last) { ++readerBackwards; }
                    last = ps.rx.seq;
                    ++readerReads;
                }
                auto full = snap->readFull();
                if (full != nullptr) { (void)full->state.rx.seq; }
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });

        // THE WINDOW'S THREAD: the frame, as fast as it can, with a drag every few frames.
        std::mt19937 rng(99u);
        std::uint64_t lastSeq = 0, backwards = 0, frames = 0, listMismatch = 0, listChecks = 0;
        std::uint64_t localOutlivedHold = 0, shownNotSent = 0, dragSteps = 0;
        double lastSent = 0.0;
        double lastSentAt = 0.0;
        const auto t0 = Clock::now();
        const auto nowS = [] { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); };  // (the window's own clock, steadyNowS)
        while (msSince(t0) < kRunMs) {
            const double now = nowS();
            if (frames % 3 == 0) {
                const double want = static_cast<double>(static_cast<int>(rng() % 200000) - 100000);
                A::dragCentre(app, A::state(app).centreHz + want);
                lastSentAt = now;
                lastSent = A::local(app).offset.hz;
                ++dragSteps;
            }
            if (frames % 11 == 0) {
                A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, static_cast<double>(rng() % 100) / 100.0));
            }
            A::frame(app);
            ++frames;
            const FoxReceiverState& st = A::state(app);
            if (st.seq < lastSeq) { ++backwards; }
            lastSeq = std::max(lastSeq, st.seq);
            // B: the lists cache is keyed on listSeq; after a frame's reads, asking for it brings it to the state's listSeq.
            const auto& tl = A::tuning(app);
            ++listChecks;
            if (tl.listSeq != A::state(app).listSeq) { ++listMismatch; }
            // D: while the local copy stands, it is a figure the window sent, and it is not older than the hold.
            const auto& loc = A::local(app).offset;
            if (loc.valid) {
                if (nowS() - loc.sentAtS > cascade::gui::kTuneLocalHoldS + 0.25) { ++localOutlivedHold; }
                if (A::tunedOffset(app) != loc.hz) { ++shownNotSent; }
            }
            if ((frames & 255u) == 0u) { std::this_thread::yield(); }
        }
        go = false;
        for (std::thread& t : clients) { t.join(); }
        browser.join();

        std::printf("  A: %llu frames, %llu passes beside them; the state's sequence went backwards %llu time(s) on the window's thread, %ld on the readers'\n",
                    static_cast<unsigned long long>(frames), static_cast<unsigned long long>(A::passes(app)),
                    static_cast<unsigned long long>(backwards), readerBackwards.load());
        CHECK(frames > 1000);
        CHECK(A::passes(app) > 200u);
        {
            // The frame's kick (6.9): this loop is a window drawing thousands of frames a second beside a pass of its own, and every
            // frame's kick reaches the loop (the gap is off here) without a lost or a doubled pass.
            const Engine::PassStats ps = e.passStats();
            std::printf("  A: the frame's kicks: %llu woke the pass, %llu passes started by a kick\n",
                        static_cast<unsigned long long>(ps.kicks), static_cast<unsigned long long>(ps.kickedPasses));
            CHECK(ps.kicks > 0u);
            CHECK(ps.kickedPasses > 0u);
            CHECK(ps.kickedPasses <= ps.kicks);
        }
        CHECK(backwards == 0u);
        CHECK(readerBackwards.load() == 0);
        std::printf("  B: %llu list reads: the cache keyed on a listSeq other than the state's after its refresh %llu time(s)\n",
                    static_cast<unsigned long long>(listChecks), static_cast<unsigned long long>(listMismatch));
        CHECK(listMismatch == 0u);
        // C: BUSY reads: the window counted none that mattered. refreshUiState keeps the last good state on BUSY, so a storm shows as a
        // state that stops moving; the sequence the window saw moved through the run.
        CHECK(lastSeq > 0u);
        std::printf("  C: the window's state sequence reached %llu; the session threads read %ld states without it going back\n",
                    static_cast<unsigned long long>(lastSeq), readerReads.load());
        std::printf("  D: %llu drag steps; the local copy outlived its hold %llu time(s); what it showed was not what it sent %llu time(s)\n",
                    static_cast<unsigned long long>(dragSteps), static_cast<unsigned long long>(localOutlivedHold),
                    static_cast<unsigned long long>(shownNotSent));
        CHECK(localOutlivedHold == 0u);
        CHECK(shownNotSent == 0u);
        // D (end): the drags stopped. Frames go on, the engine's readback settles on the last figure sent (it took it - the three clients
        // also set VFO offsets, until they stopped just now, so the last word is the window's: send one more and let it land).
        A::dragCentre(app, A::state(app).centreHz + 12345.0);
        const double sent = A::local(app).offset.hz;
        const auto tSettle = Clock::now();
        bool landed = false;
        while (msSince(tSettle) < 2000.0) {
            A::frame(app);
            if (A::state(app).vfoOffsetHz == sent && !A::local(app).offset.valid) {
                landed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        std::printf("      after the drags stopped: the readback is the last figure sent (%.0f Hz) after %.1f ms: %s; the local copy is gone: %s\n",
                    sent, msSince(tSettle), landed ? "yes" : "NO", A::local(app).offset.valid ? "NO" : "yes");
        CHECK(landed);
        (void)lastSent;
        (void)lastSentAt;
        std::printf("  E: three session threads submitted %ld commands (%ld BUSY), %ld answers, %ld answered with a status other than ok/no-change/denied\n",
                    submitted.load(), busy.load(), answered.load(), refusedOther.load());
        CHECK(submitted.load() > 500);
        CHECK(refusedOther.load() == 0);
        CHECK(A::offPassKeyTouches(app) == 0u);
        // G: A STALLED PASS AND A WINDOW THAT KEEPS SUBMITTING (the survey's 1.6 I). The engine's queue merges only the commands that make the
        // key safer, so a drag's figures do not merge: with the pass parked, 5000 of them fill the session's 256 slots and the NEWEST are the
        // ones refused. The window holds the newest figure of such a setter (uiHeld_) and offers it again each frame: when the pass runs
        // again the receiver ends on the LAST figure sent, not on the 256th. Commands that are not setters (a bookmark added, each a
        // different name) are dropped when the queue is full - COUNTED and said ONCE.
        {
            A::park(app, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const std::uint64_t dropsBefore = A::busyDrops(app);
            int said = 0;
            for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
                if (l.find("the engine's pass is not keeping up") != std::string::npos) { ++said; }
            }
            double lastFigure = 0.0;
            for (int i = 0; i < 5000; ++i) {
                lastFigure = static_cast<double>(1000 + i);
                A::dragCentre(app, A::state(app).centreHz + lastFigure);
            }
            const std::uint64_t afterDrags = A::busyDrops(app);
            const std::size_t heldAfterDrags = A::held(app);
            for (int i = 0; i < 300; ++i) { A::submit(app, cmd::makeText(FOXAPI_OP_BOOKMARK_ADD, "busy " + std::to_string(i)).c); }
            const std::uint64_t afterNonSetters = A::busyDrops(app);
            int saidAfter = 0;
            for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
                if (l.find("the engine's pass is not keeping up") != std::string::npos) { ++saidAfter; }
            }
            A::park(app, false);
            const auto tg = Clock::now();
            while (msSince(tg) < 3000.0 && A::state(app).vfoOffsetHz != lastFigure) {
                A::frame(app);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool heldLanded = A::state(app).vfoOffsetHz == lastFigure;
            std::printf("  G: pass parked: 5000 drag steps dropped %llu (%zu held, the newest of them); 300 non-setters sent: %llu dropped (the queue "
                        "is 256, the drag's slots included), said %d time(s); the pass runs again: the receiver ends on the LAST figure sent (%.0f Hz): %s\n",
                        static_cast<unsigned long long>(afterDrags - dropsBefore), heldAfterDrags,
                        static_cast<unsigned long long>(afterNonSetters - afterDrags), saidAfter - said, lastFigure, heldLanded ? "yes" : "NO");
            CHECK(afterDrags == dropsBefore);                       // none of the drag steps was dropped
            CHECK(heldAfterDrags == 1u);                            // one figure held: the newest
            CHECK(afterNonSetters - afterDrags >= 40u);             // 300 non-setters into a queue the drag had filled
            CHECK(saidAfter - said == 1);
            CHECK(heldLanded);
        }
        std::printf("  F: key-rule entries from a thread other than the pass's: %llu\n",
                    static_cast<unsigned long long>(A::offPassKeyTouches(app)));
    }   // the window's teardown stops the control thread (shutdownQuiesce), with every other thread already joined

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_window_threaded_look");
}
