// test_remote_control_pull.cpp - the web remote's control reaches the engine through ITS SESSION, in the pass's remote-controls phase, and the
// key rules are touched by the pumping thread alone (the pump move, step P1; the independent review's F3, docs/engine-stage4.md; re-pointed
// from the engine's PULL to the web server's engine-internal session in stage 5d, 6.12 - this is the suite that guards the browser's PTT, and
// every assertion it had before 5d is still made, over the new path).
//
// THE CHANGE UNDER TEST. The window's frame used to drain the two servers' queues and apply each request itself, which runs the whole
// command surface, the key rules above all (core::TxKeyRules is single-threaded by contract), from a thread that is not the one that pumps
// the engine. P1 moved that into a phase of the engine's own (Engine::pumpRemoteControls) that PULLED a source the front end installed. Stage
// 5d deleted the pull: the web server holds ONE engine-internal session while it runs (net/web_session.hpp, engine::openWebSession), its
// requests are translated on the HTTP thread and submitted in one batch, and the phase drains the session's internal tier at the same place -
// after the local and API queues and the inputs, before the plugins' requests and the scanner (ApiHost::applyTail) - and then DRIVES THE KEY
// AGAIN, so a browser's PTT 0 lets the Transmitter go in the same pass. The test stands the browser in front of the engine with
// web_test::WebSeat (tests/web_seat.hpp): the production session wrapper and the production hand-over, so it runs the product's code.
//
// WHAT THIS FILE HOLDS, on a real Engine, its API host, the real Transmitter and (D-F) the engine's own control thread:
//   A  ORDER, deterministic. A local press landed in the same pass as a browser's is applied first and the browser meets its key (rule 6);
//      the browser alone keys; the web's request is applied AFTER the local queue's and the API queue's commands, whichever it was queued
//      behind. (Break-it: the tier drained with the API queue, in arrival order, and A1, A3 and A4 fail.)
//   A4 ARRIVAL ORDER MUST NOT DECIDE (new in 5d). The browser's PTT arrives FIRST in wall time, the local press second, both before the one
//      pass that applies them: the LOCAL press wins the key, three rounds, with the control thread parked between passes. (Red against an
//      arrival-order build - the mutant is in the 6.12 record.)
//   A5 PTT 0 FROM THE BROWSER RELEASES THE TRANSMITTER IN THE SAME PASS (new in 5d): remoteKeyed is false after ONE pump. (Red with the
//      key drive's second run removed from the tier's phase.)
//   B  THE STANDING CONDITION. The server stopping - its session closing - ends a key the browser holds in the next pass; running, nothing
//      is touched; a key cannot even be asked for while the server is down (no session), and a restarted server keys again. (Break-it:
//      close_session skipped and B fails.)
//   C  REQUESTS ARE APPLIED IN THE ORDER THE SESSION GIVES THEM, in one pass; the session's bound is the table's (the NEWEST is refused
//      BUSY past 256 outstanding, a safer command never is - the pull's old "drop the oldest at 64" is gone); an engine with no web session
//      holds no remote key.
//   D  THE TRIPWIRE, and the single-thread contract under a threaded engine: producers submit browser requests from their own threads, two
//      local sessions press and release, the server's session closes and opens again and again, a stray caller (the thing the window's frame
//      would have been) proves the tripwire counts - and, with no stray caller, it reads 0 after the storm, the key is released within the
//      hold, and nothing is left keyed.
//   E  THE 2 s HOLD, unchanged, with the session in the path: a browser that keeps saying PTT keeps the key; one that stops loses it between
//      2.0 s and 2.5 s after its last word; PTT 0 releases within two passes.
//   F  THE LATCH'S OWNER MARKING AND THE 1 s RE-ARM, unchanged: a latch ended by the browser's RUN 0 (through the session) marks the local
//      operator exactly as a local session's RUN 0 does (the same three answers); an owner's own release does not mark, and a latch cannot
//      close again inside the re-arm time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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

#include "core/receiver_snapshot.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/transmit_page.hpp"
#include "net/web_control.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"
#include "web_seat.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using cascade::engine::Engine;
using cascade::net::ControlRequest;
using tx_test::FrontEnd;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_remote_pull_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_remote_pull_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// A radio that is only a ledger (test_transmit_key_race.cpp's shape).
class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override { rate_ = hz; return true; }
    double centerFrequencyHz() const override { return freq_; }
    bool setCenterFrequencyHz(double hz) override { freq_ = hz; return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.083e6; hi = 61.44e6; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return gain_; }
    bool setGainDb(double db) override { gain_ = db; return true; }
    std::size_t write(const std::complex<float>* /*samples*/, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return ""; }

    std::atomic<int> starts{0};

private:
    double rate_ = 2.5e6;
    double freq_ = 146.52e6;
    double gain_ = 0.0;
    std::atomic<bool> running_{false};
};

// THE BROWSER, as the web server presents it to the engine: the web server's one engine session (a web_test::WebSeat), opened when the
// server starts and closed when it stops, and the browser's requests handed to it as POST /api/control hands them. The server RUNNING is the
// seat being open: stopServer() is the server stopping (close_session), startServer() the server starting again, and a request pushed
// while it is down finds nobody to take it.
struct RemoteQueue {
    web_test::WebSeat seat;
    std::atomic<std::uint64_t> handed{0};   // requests the engine's session took
    Engine* eng = nullptr;

    // The browser polls its status as it presses: THAT is the web remote's keep-alive (API 0.2: only a poll counts, never the PTT
    // re-assertions), so every push beats the session's heartbeat first - any thread, as the server's own handlers do.
    bool push(ControlRequest r) {
        seat.beat();
        const bool taken = seat.push(r);
        if (taken) { handed.fetch_add(1); }
        return taken;
    }
    void attach(Engine& e) {
        eng = &e;
        seat.open(e);
    }
    void stopServer() { seat.close(); }
    void startServer() { seat.open(*eng); }
    bool serverRunning() const { return seat.isOpen(); }
};

ControlRequest ptt(bool on) {
    ControlRequest r;
    r.transmitPtt = on;
    return r;
}
ControlRequest volume(double v) {
    ControlRequest r;
    r.volume = v;
    return r;
}
ControlRequest stopReceiver() {
    ControlRequest r;
    r.running = false;
    return r;
}

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static RecordingSink* installSink(Engine& e) {
        auto sink = std::make_unique<RecordingSink>();
        RecordingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        e.transmitter_.setInput(TxInput::Tone);
        return raw;
    }
    static bool latched(Engine& e) { return e.transmitter_.latched(); }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static bool remoteKeyed(Engine& e) { return e.transmitter_.remoteKeyed(); }
    static std::uint64_t passes(Engine& e) { return e.controlPasses_.load(); }
    static void park(Engine& e, bool on) { e.controlParkForTest_.store(on); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool waitPasses(Engine& e, std::uint64_t n, int ms = 2000) {
    const std::uint64_t target = Access::passes(e) + n;
    return waitUntil(ms, [&] { return Access::passes(e) >= target; });
}

// THE KEY OWNER, from a thread that is not the pump's: the published block (a seqlock), never the rules themselves - the
// rules are the pumping thread's alone, which is what this file is about, and reading them from here would be the race.
std::uint64_t ownerNow(Engine& e) {
    for (int i = 0; i < 1000; ++i) {
        cascade::core::PublishedState p;
        if (e.snapshot()->read(p)) { return p.app.txKeyOwner; }
        std::this_thread::yield();
    }
    return ~std::uint64_t{0};
}

// The control thread, with a short period (a pass every millisecond and at once on a submit).
void startThread(Engine& e, int periodMs) {
    cascade::engine::ControlThreadOptions opts;
    opts.period = std::chrono::milliseconds(periodMs);
    opts.watchdogThresholdMs = 10000;
    e.startControlThread(opts);
}

// The state a session reads, as the desk reads it.
FoxReceiverState readState(FrontEnd& fe) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    (void)fe.api->read_state(fe.s, &s);
    return s;
}

bool mine(FrontEnd& fe) { return (readState(fe).flags & FOXAPI_RX_TX_KEY_MINE) != 0u; }

// One command on the desk's session, and the status of ITS answer once the engine has applied it.
int32_t submitStatus(Engine& e, FrontEnd& fe, const FoxCommand& c, bool threaded) {
    fe.drain();
    FoxCommand cmd = c;
    cmd.structSize = sizeof(cmd);
    if (fe.api->submit(fe.s, &cmd, 1, nullptr) != 1) { return -9999; }
    if (threaded) {
        (void)waitPasses(e, 3);
    } else {
        e.pump();
        e.pump();
    }
    FoxCommandResult r[4];
    for (auto& x : r) { x.structSize = sizeof(x); }
    const int32_t got = fe.api->poll_results(fe.s, r, 4);
    return got >= 1 ? r[got - 1].status : -9998;
}

FoxCommand cmdInt(std::uint32_t op, std::int64_t v) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.ival[0] = v;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_remote_control_pull\n");
    isolate();

    // ================================================================================================================
    // A. ORDER (the pump by hand: nothing between the phases but the test)
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        desk.frame(true, false, false);
        e.pump();  // the transmitter is PUBLISHED open
        desk.frame(true, false, false, true, 1);  // the local operator consents to a remote key
        e.pump();

        // A1: a local press and a browser's PTT land in the SAME pass - the local one is applied first and holds the key.
        desk.frame(true, true, false);  // PTT held, submitted on the session
        q.push(ptt(true));
        e.pump();
        const bool localOwns = mine(desk);
        const bool remoteOwns = Access::remoteKeyed(e);
        std::printf("A1: local PTT + browser PTT in one pass: local key mine=%d, remote keyed=%d\n", localOwns ? 1 : 0,
                    remoteOwns ? 1 : 0);
        CHECK(localOwns);
        CHECK(!remoteOwns);
        CHECK(raw->running());
        desk.frame(true, false, false);  // let go
        e.pump();
        e.pump();
        CHECK(!mine(desk));
        CHECK(waitUntil(300, [&] { return !raw->running(); }));

        // A2: the browser alone keys (so A1 above is the ORDER at work, not a browser that never could).
        q.push(ptt(true));
        e.pump();
        std::printf("A2: browser PTT alone: remote keyed=%d, owner=%llu\n", Access::remoteKeyed(e) ? 1 : 0,
                    static_cast<unsigned long long>(e.api().txFacts().owner));
        CHECK(Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner != 0u);
        CHECK(!mine(desk));
        q.push(ptt(false));
        e.pump();
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);

        // A3: the browser's request is applied AFTER the local queue's commands...
        e.submitCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.2));
        q.push(volume(0.7));
        e.pump();
        CHECK(readState(desk).volume > 0.69 && readState(desk).volume < 0.71);
        // ...and after the API queue's (a session's submit), which drains after the local one.
        FoxCommand v = cmdInt(FOXAPI_OP_SET_VOLUME, 0);
        v.num[0] = 0.3;
        CHECK(desk.api->submit(desk.s, &v, 1, nullptr) == 1);
        q.push(volume(0.8));
        e.pump();
        const double vol = readState(desk).volume;
        std::printf("A3: local 0.2 then browser 0.7 -> %.2f; session 0.3 then browser 0.8 -> %.2f\n", 0.7, vol);
        CHECK(vol > 0.79 && vol < 0.81);
        desk.drain();
        CHECK(e.api().offPassKeyTouches() == 0u);  // no control thread: nothing is judged
        desk.close();
    }

    // A5: THE BROWSER'S PTT 0 RELEASES THE TRANSMITTER IN THE SAME PASS. The table decides a key in the rules and drives the Transmitter at
    // the END of the API queue's drain; the browser's commands are applied after that, in the remote-controls phase, so the phase runs the
    // drive a second time. Without it the rules would be open and the radio would stay keyed until the NEXT pass - carrier nobody asked for,
    // from the one request (a release) that must never wait. ONE pump, by hand, and the Transmitter is let go in it.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        desk.frame(true, false, false);
        e.pump();
        desk.frame(true, false, false, true, 1);  // the local operator consents to a remote key
        e.pump();
        for (int round = 0; round < 3; ++round) {
            CHECK(q.push(ptt(true)));
            e.pump();
            CHECK(Access::remoteKeyed(e));
            CHECK(e.api().txFacts().owner != 0u);
            CHECK(waitUntil(300, [&] { return raw->running(); }));
            CHECK(q.push(ptt(false)));
            e.pump();  // ONE pass
            std::printf("A5 (round %d): browser PTT 0, one pass: remote keyed=%d, owner=%llu\n", round, Access::remoteKeyed(e) ? 1 : 0,
                        static_cast<unsigned long long>(e.api().txFacts().owner));
            CHECK(!Access::remoteKeyed(e));
            CHECK(e.api().txFacts().owner == 0u);
            CHECK(waitUntil(300, [&] { return !raw->running(); }));
        }
        desk.close();
    }

    // A, THE THREADED PASS: the same two cases with the control thread parked between passes, so BOTH arrive before the one
    // pass that applies them (deterministic, not a race the test hopes to win).
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 5);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        for (int round = 0; round < 3; ++round) {
            Access::park(e, true);
            // The loop parks at the TOP of an iteration: wait until the pass count has stopped moving.
            std::uint64_t last = ~std::uint64_t{0};
            CHECK(waitUntil(2000, [&] {
                const std::uint64_t now = Access::passes(e);
                const bool stable = now == last;
                last = now;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                return stable;
            }));
            desk.frame(true, true, false);  // the local PTT, submitted (and the desk's heartbeat: its consent stands)
            q.push(ptt(true));              // the browser's PTT, queued BEHIND it in wall time and in the pull
            const std::uint64_t p0 = Access::passes(e);
            Access::park(e, false);
            CHECK(waitUntil(2000, [&] { return Access::passes(e) >= p0 + 1; }));
            CHECK(waitUntil(500, [&] { return mine(desk); }));
            CHECK(!Access::remoteKeyed(e));
            CHECK(raw->running());
            desk.frame(true, false, false);  // PTT 0
            CHECK(waitUntil(1000, [&] { return !mine(desk) && !raw->running(); }));
            desk.beat();
            // the browser's request, alone, in a pass of its own, keys (the case above was not a browser that never could)
            q.push(ptt(true));
            CHECK(waitUntil(1000, [&] { return Access::remoteKeyed(e); }));
            q.push(ptt(false));
            CHECK(waitUntil(1000, [&] { return !Access::remoteKeyed(e) && !raw->running(); }));
        }
        std::printf("A (control thread parked, 3 rounds): the local press won every time; the browser alone keyed every time\n");
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    // A4: ARRIVAL ORDER MUST NOT DECIDE. The browser's PTT is submitted FIRST in wall time and the local press SECOND, both while the
    // control thread is parked, so one pass applies them both. A web request applied in the table's ordinary queue would be applied first
    // and hold the key, and the operator at the machine would be refused; the browser's session drains in the remote-controls phase, after
    // the API queue, so the LOCAL press wins whichever arrived first (rule 6). Three rounds. (Before 5d the pull's order was the only thing
    // that gave this, and the one case the P1 suite pinned had the browser arrive SECOND.)
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 5);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        int localWon = 0;
        for (int round = 0; round < 3; ++round) {
            Access::park(e, true);
            std::uint64_t last = ~std::uint64_t{0};
            CHECK(waitUntil(2000, [&] {
                const std::uint64_t now = Access::passes(e);
                const bool stable = now == last;
                last = now;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                return stable;
            }));
            CHECK(q.push(ptt(true)));        // the browser FIRST (and its status poll: the session is alive)
            desk.frame(true, true, false);   // ...the local press SECOND
            const std::uint64_t p0 = Access::passes(e);
            Access::park(e, false);
            CHECK(waitUntil(2000, [&] { return Access::passes(e) >= p0 + 1; }));
            CHECK(waitUntil(500, [&] { return mine(desk); }));
            const bool remote = Access::remoteKeyed(e);
            if (mine(desk) && !remote) { ++localWon; }
            CHECK(!remote);
            CHECK(raw->running());
            desk.frame(true, false, false);  // PTT 0
            CHECK(waitUntil(1000, [&] { return !mine(desk) && !raw->running(); }));
            desk.beat();
            // the browser asks again, alone, once the desk has let go: it keys (so the case above was not a browser that never could)
            CHECK(q.push(ptt(true)));
            CHECK(waitUntil(1000, [&] { return Access::remoteKeyed(e); }));
            CHECK(q.push(ptt(false)));
            CHECK(waitUntil(1000, [&] { return !Access::remoteKeyed(e) && !raw->running(); }));
        }
        std::printf("A4 (the browser FIRST in wall time, the local press second, control thread parked, 3 rounds): the local press won %d of 3\n",
                    localWon);
        CHECK(localWon == 3);
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    // ================================================================================================================
    // B. THE STANDING CONDITION
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        desk.frame(true, false, false);
        e.pump();
        desk.frame(true, false, false, true, 1);
        e.pump();
        q.push(ptt(true));
        e.pump();
        CHECK(Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner != 0u);
        // CONTROL: the server running, no new request - the key stands (a check that always released would fail here).
        e.pump();
        e.pump();
        CHECK(Access::remoteKeyed(e));
        CHECK(raw->running());
        // THE SERVER STOPS: its session closes (WebServer::stop does it after the last thread of the server has ended), and the key rules open
        // the key the closed session held at the start of the next pass - with no request anywhere.
        q.stopServer();
        e.pump();
        std::printf("B: server stopped (its session closed) -> remote keyed=%d, owner=%llu\n", Access::remoteKeyed(e) ? 1 : 0,
                    static_cast<unsigned long long>(e.api().txFacts().owner));
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        // and a key cannot even be ASKED FOR while the server is down: there is no session to hand it to (the POST would be refused)
        CHECK(!q.push(ptt(true)));
        e.pump();
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);
        // THE SERVER STARTS AGAIN (a new session): the stop was not sticky, and the browser can key as before
        q.startServer();
        CHECK(q.serverRunning());
        CHECK(q.push(ptt(true)));
        e.pump();
        CHECK(Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner != 0u);
        CHECK(q.push(ptt(false)));
        e.pump();
        CHECK(!Access::remoteKeyed(e));
        desk.close();
    }

    // ================================================================================================================
    // C. ORDER OF REQUESTS, THE SESSION'S BOUND, NO SESSION
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        FrontEnd desk(e, "the desk");
        desk.frame(true, false, false);
        e.pump();
        RemoteQueue q;
        q.attach(e);
        q.push(volume(0.1));
        q.push(volume(0.4));
        q.push(volume(0.9));
        e.pump();
        const double vol = readState(desk).volume;
        std::printf("C: three requests in one pass -> the last one stands (%.2f); handed=%llu\n", vol,
                    static_cast<unsigned long long>(q.handed.load()));
        CHECK(vol > 0.89 && vol < 0.91);
        // THE BOUND IS THE TABLE'S (it replaces the servers' "drop the OLDEST at 64"): a session holds at most FOXAPI_MAX_PENDING (256)
        // outstanding commands and answers the NEWEST BUSY past it - so when the engine is not applying (no pump here) the first 256
        // requests stand and the ones after are refused at the door - and a SAFER command (RUN 0, PTT 0) is never refused for it.
        std::uint64_t taken = 0;
        constexpr int kFlood = 300;
        for (int i = 0; i < kFlood; ++i) {
            if (q.push(volume(static_cast<double>(i + 1) / 1000.0))) { ++taken; }
        }
        // (the hand-over reports a request handed over even when the table answered one of its commands BUSY; what the engine took is what
        // it applies)
        CHECK(q.push(stopReceiver()));  // the safer command, past the bound
        e.pump();
        const double last = readState(desk).volume;
        const std::vector<FoxCommandResult> results = q.seat.take();
        int ranStops = 0;
        for (const FoxCommandResult& r : results) {
            if (r.op == FOXAPI_OP_RUN) { ++ranStops; }
        }
        std::printf("C: %d volume requests with the engine not applying: the 256th stands (volume %.3f, not the newest 0.300); the RUN 0 past the "
                    "bound was %s\n",
                    kFlood, last, ranStops >= 1 ? "taken" : "REFUSED");
        CHECK(last > 0.255 && last < 0.257);  // the 256th request (0.256) is the newest the session took; 257..300 were refused BUSY
        CHECK(ranStops >= 1);
        (void)taken;
        // ...and once the engine has applied them and the results are read, the session takes requests again
        CHECK(q.push(volume(0.55)));
        e.pump();
        CHECK(readState(desk).volume > 0.54 && readState(desk).volume < 0.56);
        // THE RANGES ARE THE TABLE'S: the strict parse lets centerHz 0 through (web_control.hpp kMinCenterHz), the table refuses it - as a
        // RESULT on the session, never to the browser (the one range the two disagreed about; docs/api-0.3-additions.md 8.16.8).
        {
            ControlRequest zero;
            zero.centerHz = 0.0;
            (void)q.seat.take();
            CHECK(q.push(zero));
            e.pump();
            CHECK(q.seat.lastStatus(FOXAPI_OP_SET_CENTRE) == FOXAPI_OUT_OF_RANGE);
        }
        // NO SESSION: a web server that is not running holds none, so a request has nowhere to go; the headless engine of the table
        // judges nothing and holds no remote key (a stopped server cannot have started one).
        q.stopServer();
        CHECK(!q.push(volume(0.3)));
        e.pump();
        e.pump();
        CHECK(readState(desk).volume > 0.54 && readState(desk).volume < 0.56);
        {
            Engine h;
            h.initialise();
            RecordingSink* hraw = Access::installSink(h);
            FrontEnd hd(h, "the desk");
            hd.frame(true, false, false);
            h.pump();
            hd.frame(true, false, false, true, 1);
            h.pump();
            h.pump();
            CHECK(!Access::remoteKeyed(h));
            CHECK(!hraw->running());
            hd.close();
        }
        desk.close();
    }

    // ================================================================================================================
    // D. THE TRIPWIRE, AND THE SINGLE-THREAD CONTRACT UNDER THE THREADED ENGINE
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        FrontEnd second(e, "the second window");
        startThread(e, 1);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));

        // D0: the tripwire is not decoration. A stray caller - the window's frame applying an engine-ended key beside the pass,
        // the very thing the review found - is COUNTED while the control thread runs. The stray call is the one that only READS
        // the rules (no key is held, so nothing is written: ApiHost::endKeyFromEngine is the RUN 0 / TX_OPEN / TX_CLOSE end, which
        // TxKeyRules::endKey makes only when a key is held): the writing kind - a PTT through the key rules, which bumps the rules'
        // change counter while the pass publishes it - is a real data race, and ThreadSanitizer reported it here the first time D0
        // used one (tx_key_rules.hpp changes() against TxKeyRules::key): the F3 defect in miniature. (Re-aimed in 5d: the probe was
        // ApiHost::webRemoteStopped, which went with the web remote's private key path; endKeyFromEngine is the same entry.)
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.api().endKeyFromEngine("the tripwire probe");
        const std::uint64_t counted = e.api().offPassKeyTouches();
        std::printf("D0: a stray key-rule entry beside the control thread is counted: %llu\n",
                    static_cast<unsigned long long>(counted));
        CHECK(counted >= 1u);
        // ...so the storm below is judged against the count as it stands, and must not add to it.
        const std::uint64_t base = counted;

        // D1: the storm. Two producers (two browsers' worth of requests, from their own threads, into the one web session), the desk
        // and a second window pressing and releasing, the server's session closing and opening again - for 1.5 s.
        std::atomic<bool> go{true};
        std::atomic<std::uint64_t> pushed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937 rng(1234u + static_cast<unsigned>(t));
                while (go.load()) {
                    switch (rng() % 6) {
                        case 0: q.push(ptt(true)); break;
                        case 1: q.push(ptt(false)); break;
                        case 2: q.push(volume(static_cast<double>(rng() % 100) / 100.0)); break;
                        case 3: q.push(stopReceiver()); break;   // RUN 0: an engine-ended key, from the pull
                        default: q.push(ptt(true)); break;
                    }
                    pushed.fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::microseconds(200 + rng() % 800));
                }
            });
        }
        for (int t = 0; t < 2; ++t) {
            FrontEnd* fe = t == 0 ? &desk : &second;
            threads.emplace_back([&, fe, t] {
                std::mt19937 rng(99u + static_cast<unsigned>(t));
                while (go.load()) {
                    const unsigned pick = rng() % 4;
                    fe->frame(true, pick == 0 || pick == 1, pick == 2);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1 + rng() % 4));
                }
            });
        }
        threads.emplace_back([&] {
            // THE SERVER FLAPS: its session closes (WebServer::stop) and a new one opens (WebServer::start), under the seat's lock, with
            // the producers pushing into whichever is there - a push into a closed seat finds nobody, as a POST to a stopped server would.
            while (go.load()) {
                if (q.serverRunning()) {
                    q.stopServer();
                } else {
                    q.startServer();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(37));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        go.store(false);
        for (std::thread& t : threads) { t.join(); }
        if (!q.serverRunning()) { q.startServer(); }

        // D2: quiesce. Every producer has stopped; the pages close (the desk's and the second window's), so any PTT, latch and
        // consent they hold goes in the pass that sees it; whatever the browser held ends by its hold (2 s) - and nothing
        // may be left keyed, with a key owner the rules cannot find.
        desk.frame(false, false, false);
        second.frame(false, false, false);
        const bool quiet = waitUntil(4000, [&] {
            return ownerNow(e) == 0u && !Access::transmitting(e) && !Access::latched(e) &&
                   !Access::remoteKeyed(e) && !raw->running();
        });
        const std::uint64_t touches = e.api().offPassKeyTouches();
        std::printf("D: %llu requests pushed, %llu handed to a session, radio started %d times; quiet after the storm: %d; "
                    "off-pass key touches added by the storm: %llu\n",
                    static_cast<unsigned long long>(pushed.load()), static_cast<unsigned long long>(q.handed.load()),
                    raw->starts.load(), quiet ? 1 : 0, static_cast<unsigned long long>(touches - base));
        CHECK(quiet);
        CHECK(touches == base);
        CHECK(pushed.load() > 50u);
        CHECK(raw->starts.load() > 0);  // the storm really keyed the radio, so quiet means something
        e.stopControlThread();
        CHECK(e.api().offPassKeyTouches() == base);  // and the control thread's last act (engineStopping) is its own
    }

    // ================================================================================================================
    // E. THE 2 s HOLD, with the session in the path (real timers)
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 2);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        // The browser keeps saying PTT every 400 ms for 1.2 s: the key must never drop between the words.
        bool dropped = false;
        std::int64_t lastWord = 0;
        q.push(ptt(true));
        lastWord = nowMs();
        const std::int64_t firstWord = lastWord;
        // Keyed AND published: the pass applies the request in its remote-controls phase and publishes the owner at its end, so the
        // Transmitter's flag precedes the published block by the rest of that pass (a loaded machine makes it long enough to see).
        CHECK(waitUntil(1000, [&] { return Access::remoteKeyed(e) && ownerNow(e) != 0u; }));
        for (int word = 1; word <= 3; ++word) {
            const std::int64_t until = nowMs() + 400;
            while (nowMs() < until) {
                if (!Access::remoteKeyed(e) || ownerNow(e) == 0u) {
                    if (!dropped) {
                        std::printf("E: key dropped %lld ms after the first word: remoteKeyed=%d owner=%llu\n",
                                    static_cast<long long>(nowMs() - firstWord), Access::remoteKeyed(e) ? 1 : 0,
                                    static_cast<unsigned long long>(ownerNow(e)));
                    }
                    dropped = true;
                }
                desk.beat();  // the desk's heartbeat keeps its consent alive; the browser's PTT is not one...
                q.seat.beat();  // ...its status poll is (the session's heartbeat)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            q.push(ptt(true));
            lastWord = nowMs();
        }
        CHECK(!dropped);
        // Silence: the hold runs out between 2.0 s and 2.5 s after the last word (stamped when it was applied, a pass
        // after the push).
        bool released = false;
        while (nowMs() - lastWord < 4000) {
            desk.beat();
            q.seat.beat();
            if (!Access::remoteKeyed(e) && ownerNow(e) == 0u) {
                released = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double held = static_cast<double>(nowMs() - lastWord);
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        std::printf("E: the browser said PTT four times 400 ms apart: never dropped=%d; after its last word the key was "
                    "released at %.0f ms\n",
                    dropped ? 0 : 1, held);
        CHECK(released);
        CHECK(held >= 2000.0 && held <= 2500.0);
        // PTT 0 releases within two passes
        q.push(ptt(true));
        CHECK(waitUntil(1000, [&] { return Access::remoteKeyed(e); }));
        q.push(ptt(false));
        // PTT 0 releases NOW (API 0.2): in the pass that applies it, which is a few milliseconds of control thread, not the hold.
        CHECK(waitUntil(400, [&] { return !Access::remoteKeyed(e) && ownerNow(e) == 0u; }));
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    // ================================================================================================================
    // F. THE LATCH'S OWNER MARKING AND THE 1 s RE-ARM (real timers)
    // ================================================================================================================
    // The latch is ended by RUN 0 - from the browser (through its session) in one run, from a local session (through the API
    // queue) in the other. The three answers a later LATCH 1 gets must be the same.
    int32_t answers[2][3] = {};
    for (int viaPull = 0; viaPull < 2; ++viaPull) {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        RemoteQueue q;
        q.attach(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 2);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false);
        CHECK(waitPasses(e, 3));
        // 1. latch: accepted
        answers[viaPull][0] = submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true);
        CHECK(waitUntil(500, [&] { return Access::latched(e); }));
        // 2. RUN 0 ends it (marking the local operator)
        if (viaPull != 0) {
            q.push(stopReceiver());
        } else {
            (void)submitStatus(e, desk, cmdInt(FOXAPI_OP_RUN, 0), true);
        }
        CHECK(waitUntil(1000, [&] { return !Access::latched(e); }));
        // 3. past the re-arm time a LATCH 1 is refused: the operator is marked, and only the owner's own LATCH 0 clears it
        std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 150));
        desk.beat();
        answers[viaPull][1] = submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true);
        CHECK(!Access::latched(e));
        // 4. the session's LATCH 0 exempts it; then a LATCH 1 is accepted again
        (void)submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 0), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 150));
        desk.beat();
        answers[viaPull][2] = submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true);
        CHECK(waitUntil(500, [&] { return Access::latched(e) && raw->running(); }));
        std::printf("F (%s): LATCH 1 -> %d; after RUN 0 from %s, LATCH 1 -> %d; after its own LATCH 0, LATCH 1 -> %d\n",
                    viaPull != 0 ? "browser, through its session" : "local session, through the API queue",
                    answers[viaPull][0], viaPull != 0 ? "the browser" : "the local session", answers[viaPull][1],
                    answers[viaPull][2]);
        CHECK(answers[viaPull][0] == FOXAPI_OK);
        CHECK(answers[viaPull][1] == FOXAPI_DENIED);
        CHECK(answers[viaPull][2] == FOXAPI_OK);
        // release for the next run
        desk.frame(false, false, false);
        CHECK(waitUntil(1000, [&] { return !Access::latched(e) && !raw->running(); }));
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }
    CHECK(answers[0][0] == answers[1][0] && answers[0][1] == answers[1][1] && answers[0][2] == answers[1][2]);

    // F2: THE RE-ARM ON ITS OWN. A latch the OWNER releases marks nobody, and no latch closes again inside the re-arm time:
    // refused straight after, accepted once the time has run out.
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 2);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false);
        CHECK(waitPasses(e, 3));
        CHECK(submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true) == FOXAPI_OK);
        CHECK(waitUntil(500, [&] { return Access::latched(e); }));
        CHECK(submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 0), true) == FOXAPI_OK);
        const int32_t tooSoon = submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 150));
        desk.beat();
        const int32_t later = submitStatus(e, desk, cmdInt(FOXAPI_OP_TX_LATCH, 1), true);
        std::printf("F2: LATCH 1 straight after the owner's LATCH 0 -> %d; after the re-arm time -> %d\n", tooSoon, later);
        CHECK(tooSoon != FOXAPI_OK);
        CHECK(later == FOXAPI_OK);
        desk.frame(false, false, false);
        CHECK(waitUntil(1000, [&] { return !Access::latched(e) && !raw->running(); }));
        e.stopControlThread();
    }

    return testSummary("test_remote_control_pull");
}
