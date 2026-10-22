// test_internal_session_order.cpp - THE INTERNAL TIER: the web server's and CAT's sessions are applied where the pull has always been,
// after the operator at the machine (engine extraction stage 5a; docs/api-0.3-additions.md 8.16; the pump move's step P1 ordering
// guarantee, now structural instead of a property of a pull).
//
// WHAT THE PULL GAVE, AND WHY IT HAD TO BECOME A TIER. P1 made the browser's requests run AFTER the local and API queues of a pass so a
// local press that landed in the same pass is applied first and a remote one meets its key (rule 6). test_remote_control_pull pins the
// case where the browser ARRIVES SECOND. NOTHING pinned the case the code comment promises - the browser FIRST in wall time and the
// local press still winning the key - because with a pull there is no arrival order to speak of. A web server that submits to a session
// has one: its command is in the engine's queue before the desk's. This file pins it, and every other property of the tier:
//
//   A   ORDER, by hand (no control thread: the test is the pass). A1 THE NEW ONE: the browser's PTT is submitted FIRST, the desk's
//       press second, one pass: the desk owns the key. A2 the same the other way round. A3 the browser alone keys (so A1 is the order at
//       work, not a session that never could). A4 THE SAME-PASS RELEASE: the browser's PTT 0 releases the Transmitter in the pass that
//       applied it (the key drive runs again after the tier), not in the next. A5 the tier's order: the local queue, the API queue, then
//       the web session, then CAT's - however they arrived; within a session, in the order it submitted. A6 a closed session's commands
//       are not applied; its release is. A7 closing the key holder's session releases the key in the next pass.
//   B   THE SAME AS A1 AND A4 UNDER THE FREE-RUNNING PASS, the control thread parked so both submits are certain to be in the queues of
//       ONE pass (deterministic, not a race the test hopes to win), three rounds.
//   C   THE FLOOD. An internal session that never reads its results is answered BUSY past FOXAPI_MAX_PENDING (256) for the NEWEST
//       command - the oldest 256 stand (D9: the table's rule replaces the servers' old "drop the oldest at 64") - and the commands that
//       can only make the transmitter safer are never BUSY; read, the session is served again; 10 000 commands with the results drained
//       are never BUSY.
//   D   CAT NEVER KEYS: a CAT session's PTT is refused by its profile, with local consent given and a transmitter open.
//   E   LIVENESS. The browser's keep-alive is the session's heartbeat (a status poll), and a PTT re-assertion is not one: a browser that
//       keeps saying PTT without a poll loses the key at the 2 s remote keep-alive; one that polls keeps it.
//   F   THE STORM: producers on the web and CAT sessions, two local sessions pressing and releasing, the pass free-running; the key
//       rules are touched by the pumping thread alone (offPassKeyTouches reads 0), and nothing stays keyed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/receiver_snapshot.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "gui/transmit_page.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"

// (test_check.hpp has CHECK only: a comparison that prints what it saw)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto va_ = (a);                                                                              \
        const auto vb_ = (b);                                                                              \
        CHECK(va_ == vb_);                                                                                 \
        if (!(va_ == vb_)) {                                                                               \
            std::printf("      %s = %lld, %s = %lld\n", #a, static_cast<long long>(va_), #b, static_cast<long long>(vb_)); \
        }                                                                                                  \
    } while (0)

using cascade::core::TxInput;
using cascade::engine::Engine;
using cascade::engine::InternalKind;
using tx_test::FrontEnd;

namespace {

// A radio that is only a ledger (test_remote_control_pull.cpp's shape).
class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
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

private:
    double rate_ = 2.5e6;
    double freq_ = 146.52e6;
    double gain_ = 0.0;
    std::atomic<bool> running_{false};
};

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
    static bool remoteKeyed(Engine& e) { return e.transmitter_.remoteKeyed(); }
    static std::uint64_t passes(Engine& e) { return e.controlPasses_.load(); }
    static void park(Engine& e, bool on) { e.controlParkForTest_.store(on); }
    static bool parked(Engine& e) { return e.controlParkedForTest_.load(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

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

// Parks the control thread between passes and waits until it SAYS it is parked: no pass is in progress and none will begin until it is
// let go (a pass under a thread sanitizer can run far longer than any pause a test could take for "settled").
bool parkAndSettle(Engine& e) {
    Access::park(e, true);
    return waitUntil(60000, [&] { return Access::parked(e); });
}

// The key owner as the published block says it (a seqlock): the rules themselves are the pumping thread's alone, and reading them from a
// test thread while the control thread runs is the race the tripwire exists to forbid.
std::uint64_t ownerNow(Engine& e) {
    for (int i = 0; i < 1000; ++i) {
        cascade::core::PublishedState p;
        if (e.snapshot()->read(p)) { return p.app.txKeyOwner; }
        std::this_thread::yield();
    }
    return ~std::uint64_t{0};
}

void startThread(Engine& e, int periodMs) {
    cascade::engine::ControlThreadOptions opts;
    opts.period = std::chrono::milliseconds(periodMs);
    opts.watchdogThresholdMs = 10000;
    e.startControlThread(opts);
}

FoxCommand cmdInt(std::uint32_t op, std::int64_t v) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.ival[0] = v;
    return c;
}

FoxCommand cmdNum(std::uint32_t op, double v) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = v;
    return c;
}

// An engine-internal session, as the web server or one CAT connection holds it.
struct Internal {
    FoxSession* s = nullptr;
    Internal(Engine& e, InternalKind kind, const char* label) { s = e.api().openInternalSession(kind, label); }
    ~Internal() { close(); }
    Internal(const Internal&) = delete;
    Internal& operator=(const Internal&) = delete;
    void close() {
        if (s != nullptr) {
            api()->close_session(s);
            s = nullptr;
        }
    }
    int32_t submit(const FoxCommand& c) { return api()->submit(s, &c, 1, nullptr); }
    int32_t submitStatus(const FoxCommand& c) {
        FoxSubmitResult r{};
        r.structSize = sizeof(r);
        const int32_t n = api()->submit(s, &c, 1, &r);
        return n < 0 ? n : r.status;
    }
    void beat() { api()->heartbeat(s); }
    // Every result waiting, oldest first.
    std::vector<FoxCommandResult> results() {
        std::vector<FoxCommandResult> out;
        for (;;) {
            FoxCommandResult r[32];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s, r, 32);
            if (got <= 0) { break; }
            out.insert(out.end(), r, r + got);
            if (got < 32) { break; }
        }
        return out;
    }
    FoxReceiverState state() {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        (void)api()->read_state(s, &st);
        return st;
    }
};

// A LOCAL session of the API with the grants asked (the FrontEnd's own has no TUNE).
struct ApiSessionHandle {
    FoxSession* s = nullptr;
    ApiSessionHandle(Engine& e, std::uint64_t grants) {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = grants;
        p.clientName = "an api client";
        api()->open_session(e.apiHandle(), &p, &s);
    }
    ~ApiSessionHandle() {
        if (s != nullptr) { api()->close_session(s); }
    }
    ApiSessionHandle(const ApiSessionHandle&) = delete;
    ApiSessionHandle& operator=(const ApiSessionHandle&) = delete;
};

bool mine(FrontEnd& fe) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    (void)fe.api->read_state(fe.s, &s);
    return (s.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
}

double vfoOffset(Internal& s) { return s.state().vfoOffsetHz; }

// The desk consents to a remote key and the transmitter is published open.
void armed(Engine& e, FrontEnd& desk) {
    desk.frame(true, false, false);
    e.pump();
    desk.frame(true, false, false, true, 1);
    e.pump();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_internal_session_order\n");
    engine_api_test::hermetic("internalorder");

    // ================================================================================================================
    // A. ORDER (the pump by hand: nothing between the phases but the test)
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd desk(e, "the desk");
        Internal web(e, InternalKind::Web, "the web server");
        Internal cat(e, InternalKind::Cat, "a CAT client");
        CHECK(web.s != nullptr && cat.s != nullptr);
        armed(e, desk);

        // A1: THE NEW ONE. The browser's PTT is in the engine's queue FIRST (in wall time), the desk's press second; one pass. The desk
        // owns the key: the tier is applied after the local and API queues whatever came first.
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
        desk.frame(true, true, false);
        e.pump();
        const bool localOwns = mine(desk);
        const bool remoteOwns = Access::remoteKeyed(e);
        std::printf("A1: browser PTT first, local PTT second, one pass: local key mine=%d, remote keyed=%d\n", localOwns ? 1 : 0,
                    remoteOwns ? 1 : 0);
        CHECK(localOwns);
        CHECK(!remoteOwns);
        CHECK(raw->running());
        desk.frame(true, false, false);  // let go
        e.pump();
        e.pump();
        CHECK(!mine(desk));
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        web.results();

        // A2: the other way round (the order the pull's test pinned).
        desk.frame(true, true, false);
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
        e.pump();
        CHECK(mine(desk));
        CHECK(!Access::remoteKeyed(e));
        desk.frame(true, false, false);
        e.pump();
        e.pump();
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        web.results();

        // A3: the browser alone keys (so A1 is the order at work, not a session that never could).
        web.beat();
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
        e.pump();
        std::printf("A3: browser PTT alone: remote keyed=%d, owner=%llu\n", Access::remoteKeyed(e) ? 1 : 0,
                    static_cast<unsigned long long>(e.api().txFacts().owner));
        CHECK(Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner != 0u);
        CHECK(!mine(desk));
        // ...with the answer the browser's PTT has always had: applied, OK.
        {
            const auto rs = web.results();
            CHECK(!rs.empty() && rs.back().status == FOXAPI_OK);
        }

        // A4: THE SAME-PASS RELEASE. PTT 0 from the browser lets the Transmitter go in the pass that applied it, as webRemotePtt did - not
        // in the next one. (One pump, and the remote key is already down.)
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 0)), FOXAPI_OK);
        e.pump();
        std::printf("A4: browser PTT 0, ONE pass: remote keyed=%d, owner=%llu\n", Access::remoteKeyed(e) ? 1 : 0,
                    static_cast<unsigned long long>(e.api().txFacts().owner));
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        web.results();

        // A5: THE TIER'S ORDER. The vfo offset is a value every profile and every queue can set: the last one applied is the one that
        // stands. Local queue (1000), then the desk's session (2000), then the web session (4000), then CAT (3000): CAT's was
        // submitted FIRST in wall time and still lands last.
        CHECK_EQ(cat.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 3000.0)), FOXAPI_OK);
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 4000.0)), FOXAPI_OK);
        ApiSessionHandle client(e, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE);
        FoxCommand clientOffset = cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 2000.0);
        CHECK_EQ(api()->submit(client.s, &clientOffset, 1, nullptr), 1);
        e.submitCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 1000.0));
        e.pump();
        std::printf("A5: local 1000, desk 2000, web 4000, CAT 3000 (CAT first in wall time) -> %.0f\n", vfoOffset(web));
        CHECK(vfoOffset(web) == 3000.0);
        // ...and within a session, in the order it submitted.
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 5000.0)), FOXAPI_OK);
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 6000.0)), FOXAPI_OK);
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 7000.0)), FOXAPI_OK);
        e.pump();
        CHECK(vfoOffset(web) == 7000.0);
        // Only when there is something: an empty tier costs the pass nothing (nothing to drain, no second drive).
        e.pump();
        CHECK(vfoOffset(web) == 7000.0);
        desk.drain();
        web.results();
        cat.results();

        // A6: a closed session's commands are not applied (it has no one to tell and no right to act), its safer ones are.
        {
            Internal gone(e, InternalKind::Web, "a web session that closes");
            CHECK_EQ(gone.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 9000.0)), FOXAPI_OK);
            gone.close();
            e.pump();
            e.pump();
            CHECK(vfoOffset(web) == 7000.0);
        }

        // A7: closing the key holder's session releases the key (the next pass; the table's session rule).
        web.beat();
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
        e.pump();
        CHECK(Access::remoteKeyed(e));
        web.close();
        e.pump();
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);
        CHECK(waitUntil(300, [&] { return !raw->running(); }));
        CHECK(e.api().offPassKeyTouches() == 0u);  // no control thread: nothing is judged
        desk.close();
    }

    // ================================================================================================================
    // B. THE SAME UNDER THE FREE-RUNNING PASS, the control thread parked so both submits are in the queues of ONE pass
    // ================================================================================================================
    {
        Engine e;
        e.configureApi("testHooks=1");  // (for the key state a failure prints)
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd desk(e, "the desk");
        Internal web(e, InternalKind::Web, "the web server");
        startThread(e, 5);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        for (int round = 0; round < 3; ++round) {
            CHECK(parkAndSettle(e));
            web.beat();
            CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);  // the browser FIRST in wall time...
            desk.frame(true, true, false);                                        // ...the desk second (and its heartbeat)
            const std::uint64_t p0 = Access::passes(e);
            Access::park(e, false);
            CHECK(waitUntil(2000, [&] { return Access::passes(e) >= p0 + 1; }));
            CHECK(waitUntil(500, [&] { return mine(desk); }));
            CHECK(!Access::remoteKeyed(e));
            CHECK(raw->running());
            desk.frame(true, false, false);  // PTT 0
            CHECK(waitUntil(1000, [&] { return !mine(desk) && !raw->running(); }));
            desk.beat();
            web.results();
            // the browser alone, in a pass of its own, keys; and its PTT 0 is down within ONE pass of being applied
            web.beat();
            CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
            CHECK(waitUntil(1000, [&] { return Access::remoteKeyed(e); }));
            CHECK(parkAndSettle(e));
            CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 0)), FOXAPI_OK);
            const std::uint64_t p1 = Access::passes(e);
            Access::park(e, false);
            // (the pass counter moves when a pass ENDS; the Transmitter is down by the end of the pass that applied the release)
            CHECK(waitUntil(2000, [&] { return Access::passes(e) >= p1 + 1; }));
            if (Access::remoteKeyed(e)) {
                char ks[512] = {};
                char td[32] = {};
                (void)api()->get_setting(web.s, "test.keyState", ks, sizeof(ks));
                (void)api()->get_setting(web.s, "test.tailDepth", td, sizeof(td));
                std::printf("B round %d: STILL KEYED after the pass that should have released it: passes %llu (p1 %llu), tail depth %s, key state: %s\n",
                            round, static_cast<unsigned long long>(Access::passes(e)), static_cast<unsigned long long>(p1), td, ks);
            }
            CHECK(!Access::remoteKeyed(e));
            CHECK(waitUntil(1000, [&] { return !raw->running(); }));
            web.results();
        }
        std::printf("B (control thread parked, 3 rounds): the desk won every time with the browser first; the browser's release was down in its pass\n");
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    // ================================================================================================================
    // C. THE FLOOD (the table's rule: the NEWEST past 256 is BUSY; the safer commands never are)
    // ================================================================================================================
    {
        Engine e;
        e.configureApi("testHooks=1");
        e.initialise();
        FrontEnd desk(e, "the desk");
        Internal web(e, InternalKind::Web, "the web server");
        Internal cat(e, InternalKind::Cat, "a CAT client");
        desk.frame(true, false, false);
        e.pump();
        int ok = 0, busy = 0, other = 0;
        for (int i = 0; i < 300; ++i) {
            const int32_t st = web.submitStatus(cmdNum(FOXAPI_OP_SET_VOLUME, i / 1000.0));
            if (st == FOXAPI_OK) {
                ++ok;
            } else if (st == FOXAPI_BUSY) {
                ++busy;
            } else {
                ++other;
            }
        }
        std::printf("C: 300 volumes with no results read: %d accepted, %d BUSY, %d other\n", ok, busy, other);
        CHECK_EQ(ok, 256);
        CHECK_EQ(busy, 44);
        CHECK_EQ(other, 0);
        // The safer commands are never BUSY, however full the session is (one of a kind at a time, merged).
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 0)), FOXAPI_OK);
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 0)), FOXAPI_NO_CHANGE);
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VOLUME, 0.9)), FOXAPI_BUSY);
        e.pump();
        // The OLDEST 256 stand: the last accepted volume (i = 255) is the one in force, and the newest was refused.
        const double vol = web.state().volume;
        std::printf("C: the volume in force %.3f (the 256th accepted, 0.255; not the 300th, 0.299)\n", vol);
        CHECK(vol > 0.2545 && vol < 0.2555);
        const auto rs = web.results();
        CHECK_EQ(rs.size(), static_cast<std::size_t>(257));  // 256 volumes and the PTT 0, every ticket answered
        // Read, the session is served again.
        CHECK_EQ(web.submitStatus(cmdNum(FOXAPI_OP_SET_VOLUME, 0.5)), FOXAPI_OK);
        e.pump();
        web.results();
        // 10 000 commands, results drained as the servers will drain them: never BUSY.
        int busyCount = 0, answered = 0;
        for (int round = 0; round < 100; ++round) {
            for (int i = 0; i < 100; ++i) {
                if (cat.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, 1000.0 + i)) != FOXAPI_OK) { ++busyCount; }
            }
            e.pump();
            answered += static_cast<int>(cat.results().size());
        }
        std::printf("C: 10000 CAT commands, results drained each round: %d refused, %d answered\n", busyCount, answered);
        CHECK_EQ(busyCount, 0);
        CHECK_EQ(answered, 10000);
        // ...and the internal tier is empty again.
        char buf[32];
        CHECK(api()->get_setting(web.s, "test.tailDepth", buf, sizeof(buf)) >= 0);
        CHECK(std::string(buf) == "0");
    }

    // ================================================================================================================
    // D. CAT NEVER KEYS
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd desk(e, "the desk");
        Internal cat(e, InternalKind::Cat, "a CAT client");
        armed(e, desk);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommand ptt = cmdInt(FOXAPI_OP_TX_PTT, 1);
        CHECK_EQ(api()->submit(cat.s, &ptt, 1, &sr), 1);
        e.pump();
        const auto rs = cat.results();
        CHECK_EQ(rs.size(), static_cast<std::size_t>(1));
        CHECK(!rs.empty() && rs[0].status == FOXAPI_DENIED && (rs[0].flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(!rs.empty() && std::string(rs[0].message).find("profile") != std::string::npos);
        CHECK(!Access::remoteKeyed(e));
        CHECK(e.api().txFacts().owner == 0u);
        CHECK(!raw->running());
        // the same engine, the same consent, and the web session's PTT does key (the refusal above is CAT's profile, nothing else)
        Internal web(e, InternalKind::Web, "the web server");
        web.beat();
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 1)), FOXAPI_OK);
        e.pump();
        CHECK(Access::remoteKeyed(e));
        CHECK_EQ(web.submitStatus(cmdInt(FOXAPI_OP_TX_PTT, 0)), FOXAPI_OK);
        e.pump();
        CHECK(!Access::remoteKeyed(e));
        std::printf("D: CAT's PTT is refused by its profile; the web session's, under the same consent, keys and releases\n");
    }

    // ================================================================================================================
    // E. LIVENESS: the session's heartbeat, and a PTT re-assertion is not one
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd desk(e, "the desk");
        startThread(e, 5);
        CHECK(waitPasses(e, 3));
        desk.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        // E1: re-asserting the PTT every 400 ms with NO status poll: the key is down at the remote keep-alive (2 s from the last beat,
        // which was the session's opening), long before the 3 s the loop would otherwise hold it.
        {
            Internal web(e, InternalKind::Web, "the web server");
            const std::int64_t t0 = nowMs();
            std::int64_t keyedAt = -1, droppedAt = -1;
            while (nowMs() - t0 < 3600) {
                desk.beat();  // the desk stays alive; only the browser is silent
                (void)web.submit(cmdInt(FOXAPI_OP_TX_PTT, 1));
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                const bool keyed = Access::remoteKeyed(e);
                if (keyed && keyedAt < 0) { keyedAt = nowMs() - t0; }
                if (!keyed && keyedAt >= 0 && droppedAt < 0) { droppedAt = nowMs() - t0; }
                web.results();
            }
            std::printf("E1: PTT re-asserted, no status poll: keyed at %lld ms, down at %lld ms\n", static_cast<long long>(keyedAt),
                        static_cast<long long>(droppedAt));
            CHECK(keyedAt >= 0);
            CHECK(droppedAt >= 1800 && droppedAt <= 3000);
            web.close();
            CHECK(waitUntil(2000, [&] { return !Access::remoteKeyed(e) && !raw->running(); }));
        }
        // E2: the same, polling its status as a page does: the key stands for the whole 3 s.
        {
            Internal web(e, InternalKind::Web, "the web server");
            const std::int64_t t0 = nowMs();
            bool downEver = false;
            bool keyed = false;
            while (nowMs() - t0 < 3600) {
                desk.beat();
                web.beat();
                (void)web.submit(cmdInt(FOXAPI_OP_TX_PTT, 1));
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                web.results();
                if (nowMs() - t0 > 300) {
                    keyed = Access::remoteKeyed(e);
                    downEver = downEver || !keyed;
                }
            }
            std::printf("E2: PTT re-asserted with a status poll: down at any time after the first 300 ms: %d\n", downEver ? 1 : 0);
            CHECK(!downEver);
            (void)web.submit(cmdInt(FOXAPI_OP_TX_PTT, 0));
            CHECK(waitUntil(2000, [&] { return !Access::remoteKeyed(e) && !raw->running(); }));
        }
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    // ================================================================================================================
    // F. THE STORM
    // ================================================================================================================
    {
        Engine e;
        e.initialise();
        RecordingSink* raw = Access::installSink(e);
        FrontEnd deskA(e, "desk A");
        FrontEnd deskB(e, "desk B");
        startThread(e, 1);
        CHECK(waitPasses(e, 3));
        deskA.frame(true, false, false, true, 1);
        CHECK(waitPasses(e, 3));
        Internal web(e, InternalKind::Web, "the web server");
        Internal cat(e, InternalKind::Cat, "a CAT client");
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> sent{0}, busy{0};
        std::thread tw([&] {
            std::mt19937 rng(7);
            while (!stop.load()) {
                web.beat();
                const int pick = static_cast<int>(rng() % 4);
                const FoxCommand c = pick == 0 ? cmdInt(FOXAPI_OP_TX_PTT, 1)
                                     : pick == 1 ? cmdInt(FOXAPI_OP_TX_PTT, 0)
                                     : pick == 2 ? cmdNum(FOXAPI_OP_SET_VOLUME, (rng() % 100) / 100.0)
                                                 : cmdInt(FOXAPI_OP_RUN, 0);  // (RUN 0 ends a key; RUN 1 would open a sound device)
                const int32_t st = web.submitStatus(c);
                ++sent;
                if (st == FOXAPI_BUSY) { ++busy; }
                web.results();
                std::this_thread::yield();
            }
        });
        std::thread tc([&] {
            std::mt19937 rng(11);
            while (!stop.load()) {
                const int32_t st = cat.submitStatus(cmdNum(FOXAPI_OP_SET_VFO_OFFSET, static_cast<double>(rng() % 20000)));
                ++sent;
                if (st == FOXAPI_BUSY) { ++busy; }
                cat.results();
                std::this_thread::yield();
            }
        });
        const std::int64_t t0 = nowMs();
        std::mt19937 rng(3);
        while (nowMs() - t0 < 1500) {
            deskA.frame(true, rng() % 2 == 0, false);
            deskB.frame(true, rng() % 2 == 0, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stop.store(true);
        tw.join();
        tc.join();
        (void)web.submit(cmdInt(FOXAPI_OP_TX_PTT, 0));
        deskA.frame(true, false, false);
        deskB.frame(true, false, false);
        std::printf("F: %llu commands from two internal sessions in 1.5 s against a 1 ms pass (%llu BUSY)\n",
                    static_cast<unsigned long long>(sent.load()), static_cast<unsigned long long>(busy.load()));
        CHECK(sent.load() > 500u);
        CHECK(waitUntil(5000, [&] {
            deskA.frame(true, false, false);
            deskB.frame(true, false, false);
            return !Access::remoteKeyed(e) && ownerNow(e) == 0u && !raw->running();
        }));
        CHECK(e.api().offPassKeyTouches() == 0u);
        e.stopControlThread();
    }

    return testSummary("test_internal_session_order");
}
