// test_engine_api_tx_open_async.cpp - THE NETWORK CONNECT OF A TRANSMIT RADIO IS NOT ON THE CONTROL THREAD (freeze diagnosis P2; stack iiod::connectTcp < PlutoTx::open <
// Engine::controlLoop, reports AD22C5C8 / 8E530EC8 / 661D00E7). TX_OPEN used to call PlutoTx::open on the pass: a connect bounded at 3 s per attempt, and Open presses
// queued behind it ran back to back, past the pass's 5 s watchdog. Now the open is on a worker and a later pass publishes the answer.
//
// NO RADIO ANYWHERE: the connect is a FAKE (Engine::txOpenForTest_ stands in for "make a PlutoTx and open it" on the worker): it sleeps, then hands back a ledger sink that
// records nothing and keys nothing, or refuses with a sentence. Nothing here opens a socket, a board or a transmitter. src/source/pluto_tx.cpp is not touched by this change.
//
//   A  A 3 s connect holds no control pass longer than 50 ms; the answer is published when it arrives (sink installed, no error);
//   B  A FAILED connect is published as before: the sink absent, the error text kept, and the log line "tx: could not open <address> - <reason>";
//   C  Two requests do not stack: TX_OPEN while one is pending is refused BUSY, and only ONE connect ran;
//   D  TX_CLOSE while one is pending returns at once, the answer is never applied, and the late board is let go;
//   E  The engine going away while one is pending returns within a bound (nothing waits for the connect) and the worker, which holds nothing of the engine, finishes safely.
//
// THE TRANSMIT-SAFETY RULES of the review of the first attempt (0e95c7e1, REVIEW-freezes-PART3 F1, F2, F3, F10). "A KEY NEVER SURVIVES A TRANSMITTER CHANGE":
//   F  With a board already installed, a key (PTT 1, LATCH 1) given while an open is pending is refused BUSY, and when the new board lands nothing is keyed and the new board
//      was never started (the fake board counts its start() calls);
//   G  PTT 0 and LATCH 0 (the safer direction) are still answered OK while an open is pending;
//   H  The plugins' arming window is refused BUSY while an open is pending and no window stands after the board lands;
//   I  SET_TRANSMIT_ARGS is refused BUSY while an open is pending, and the address after the board lands is the one that was opened;
//   J  The pending state is PUBLISHED (fact.transmit.opening: 1 from the accepted TX_OPEN until the answer, 0 after success, failure, close);
//   K  An open with a board installed keeps it installed until the new one lands, then swaps; a FAILED open leaves the installed board usable;
//   L  TX_CLOSE while an open is pending with a board installed closes both: nothing is installed later;
//   M  Open, Close, Open: the second open is refused BUSY while the abandoned connect is still running, accepted once it has ended (two connects never overlap);
//   N  std::async unable to start a thread does not leave the flag set (every later open would be BUSY): the failure is published and the next open is accepted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "core/app_commands.hpp"
#include "foxsdr_api.h"
#include "source/tx_sink.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;
using cascade::engine::Engine;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<int> g_connects{0};       // how many times the fake connect ran
std::atomic<int> g_connectDone{0};    // how many returned
std::atomic<int> g_sinksAlive{0};     // ledger sinks made and not yet destroyed
std::atomic<int> g_connectMs{3000};
std::atomic<bool> g_connectFails{false};
std::atomic<bool> g_keyable{false};   // the fake board's start() succeeds (so a key that reached it would be seen)
std::atomic<int> g_starts{0};         // how many times a fake board was started (= keyed) - nothing here may ever be

class LedgerSink final : public cascade::source::IqSink {
public:
    LedgerSink() { ++g_sinksAlive; }
    ~LedgerSink() override { --g_sinksAlive; }
    // Never radiates: write() takes the block and sends it nowhere. It can be started only when a test asks (g_keyable), to SEE a key that reached it.
    bool start() override {
        if (!g_keyable.load()) { return false; }
        ++g_starts;
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_.load(); }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.083e6; hi = 61.44e6; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return -89.75; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_.load() ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "ledger transmit radio"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
};

std::unique_ptr<cascade::source::IqSink> fakeConnect(const std::string&, std::string& err) {
    ++g_connects;
    std::this_thread::sleep_for(std::chrono::milliseconds(g_connectMs.load()));
    std::unique_ptr<cascade::source::IqSink> out;
    if (g_connectFails.load()) {
        err = "the fake daemon did not answer in time";
    } else {
        out = std::make_unique<LedgerSink>();
    }
    ++g_connectDone;
    return out;
}

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr const char* kAddress = "ip:192.0.2.77";   // a documentation address: nothing is ever dialled, the connect is the fake
fs::path g_plugins;
std::string pluginDirText() { return g_plugins.string(); }

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static void setPluginDir(std::string (*dir)()) { Engine::testHooks_.pluginDir = dir; }
    static void useFakeConnect(Engine& e) { e.txOpenForTest_ = &fakeConnect; }
    static bool haveSink(Engine& e) { return e.transmitter_.haveSink(); }
    static std::string error(Engine& e) { return e.transmitError_; }
    static bool pending(Engine& e) { return e.txOpenPending_; }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static std::string sinkName(Engine& e) { return e.transmitter_.sink() != nullptr ? e.transmitter_.sink()->name() : std::string(); }
    static std::string args(Engine& e) { return e.transmitArgs_; }
    static bool armed(Engine& e) { return e.txArming_.armed(); }
    static void noThread(Engine& e, bool on) { e.txOpenNoThreadForTest_ = on; }
    static int ending(Engine& e) { return e.txOpenEnding_->load(); }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

bool logHas(const std::string& needle) {
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

struct Rig {
    std::unique_ptr<Engine> eng = std::make_unique<Engine>();
    std::unique_ptr<testing::Session> page;
    double worstPassMs = 0.0;

    Rig() {
        cc::DiagLog::instance().resetForTest();
        Access::setPluginDir(&pluginDirText);
        eng->configureApi(kHooks);
        eng->initialise();
        eng->controlPass();
        Access::useFakeConnect(*eng);
        page = std::make_unique<testing::Session>(api(), eng->apiHandle(), FOXAPI_SESSION_LOCAL,
                                                  FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE);
        CHECK(page->ok());
        pass();
        pass();
    }
    ~Rig() { Access::setPluginDir(nullptr); }

    // One control pass, timed: the longest one is what the watchdog would have seen.
    void pass() {
        const auto t0 = Clock::now();
        eng->controlPass();
        worstPassMs = std::max(worstPassMs, ms(t0, Clock::now()));
    }
    std::uint64_t submit(std::uint32_t op, const char* text = nullptr, long long ival = 0) {
        const FoxCommand c = testing::cmd(op, 0.0, ival, text);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        CHECK_EQ(api()->submit(page->raw(), &c, 1, &sr), 1);
        return sr.ticket;
    }
    // Passes until the ticket's result lands (a few passes at most); returns its status, or -999.
    int resultOf(std::uint64_t ticket) {
        for (int i = 0; i < 20; ++i) {
            pass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(page->raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == ticket) { return r[k].status; }
            }
        }
        return -999;
    }
    // Passes (10 ms apart, as the real pass runs) until `done` or the time is up.
    template <class F>
    bool passUntil(F done, int limitMs) {
        const auto t0 = Clock::now();
        while (ms(t0, Clock::now()) < limitMs) {
            pass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done();
    }
};

void resetFakes(int connectMs, bool fails) {
    g_connectMs = connectMs;
    g_connectFails = fails;
    g_keyable = false;
    g_starts = 0;
    g_connects = 0;
    g_connectDone = 0;
}

std::string fact(Rig& r, const char* key) {
    char buf[512];
    const int32_t n = api()->get_setting(r.page->raw(), key, buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<") + std::to_string(n) + ">";
}
bool keyedNow(Rig& r) { return (r.page->state().flags & (FOXAPI_RX_TX_KEYED | FOXAPI_RX_TX_LATCHED)) != 0u; }

// Board A is installed (the table's dummy load: no RF anywhere, synchronous).
void openBoardA(Rig& r) {
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, "test:tx:dummy")), FOXAPI_OK);
    CHECK(Access::haveSink(*r.eng));
    CHECK(!Access::pending(*r.eng));
}

void waitFor(std::atomic<int>& v, int want, int limitMs) {
    const auto t0 = Clock::now();
    while (v.load() != want && ms(t0, Clock::now()) < limitMs) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}

// A  a 3 s connect holds no pass longer than 50 ms, and the answer is published when it arrives
void testSlowConnectDoesNotHoldThePass() {
    std::printf("A  a 3 s connect holds no control pass\n");
    resetFakes(3000, false);
    Rig r;
    const auto t0 = Clock::now();
    const int status = r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress));
    const double returnedMs = ms(t0, Clock::now());
    std::printf("    TX_OPEN answered %d after %.1f ms; worst pass %.1f ms; sink installed yet: %d\n", status, returnedMs, r.worstPassMs, Access::haveSink(*r.eng) ? 1 : 0);
    CHECK_EQ(status, FOXAPI_OK);
    CHECK(returnedMs < 500.0);
    CHECK(!Access::haveSink(*r.eng));   // the connect is still in flight
    CHECK(Access::pending(*r.eng));
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 6000));
    std::printf("    published after %.0f ms; worst pass %.1f ms over the whole open\n", ms(t0, Clock::now()), r.worstPassMs);
    CHECK(r.worstPassMs < 50.0);
    CHECK(!Access::pending(*r.eng));
    CHECK(Access::error(*r.eng).empty());
    CHECK_EQ(g_connects.load(), 1);
}

// B  a failed connect is published as before
void testFailedConnectIsPublished() {
    std::printf("B  a failed connect: the error text and the log line\n");
    resetFakes(300, true);
    Rig r;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 3000));
    std::printf("    error text: \"%s\"\n", Access::error(*r.eng).c_str());
    CHECK(!Access::haveSink(*r.eng));
    CHECK_EQ(Access::error(*r.eng), std::string("the fake daemon did not answer in time"));
    CHECK(logHas(std::string("tx: could not open ") + kAddress + " - the fake daemon did not answer in time"));
    CHECK(r.worstPassMs < 50.0);
    // ...and the next press is accepted again (nothing is left pending).
    g_connectFails = false;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 3000));
    CHECK(Access::error(*r.eng).empty());
}

// C  two requests do not stack
void testSecondRequestIsRefused() {
    std::printf("C  a second request while one is pending is refused\n");
    resetFakes(1200, false);
    Rig r;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    const int second = r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress));
    const int third = r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, "ip:192.0.2.78"));
    std::printf("    second -> %d, third -> %d (BUSY is %d)\n", second, third, FOXAPI_BUSY);
    CHECK_EQ(second, FOXAPI_BUSY);
    CHECK_EQ(third, FOXAPI_BUSY);
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 5000));
    CHECK_EQ(g_connects.load(), 1);   // one connect ever ran
    CHECK(r.worstPassMs < 50.0);
}

// D  close while pending
void testCloseWhilePending() {
    std::printf("D  TX_CLOSE while a connect is pending\n");
    resetFakes(1000, false);
    g_sinksAlive = 0;
    Rig r;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK(Access::pending(*r.eng));
    const auto t0 = Clock::now();
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_CLOSE)), FOXAPI_OK);
    const double closeMs = ms(t0, Clock::now());
    std::printf("    TX_CLOSE answered after %.1f ms\n", closeMs);
    CHECK(closeMs < 500.0);
    CHECK(!Access::pending(*r.eng));
    waitFor(g_connectDone, 1, 4000);
    CHECK_EQ(g_connectDone.load(), 1);
    r.passUntil([&] { return false; }, 300);   // passes after the worker is back: the answer must not be applied
    CHECK(!Access::haveSink(*r.eng));
    CHECK(Access::error(*r.eng).empty());
    waitFor(g_sinksAlive, 0, 2000);
    CHECK_EQ(g_sinksAlive.load(), 0);   // the late board was let go, not kept
    CHECK(r.worstPassMs < 50.0);
}

// E  exit while pending
void testExitWhilePending() {
    std::printf("E  the engine destroyed while a connect is pending\n");
    resetFakes(2500, false);
    g_sinksAlive = 0;
    double destroyMs = 0.0;
    {
        Rig r;
        CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
        CHECK(Access::pending(*r.eng));
        r.page.reset();
        const auto t0 = Clock::now();
        r.eng.reset();
        destroyMs = ms(t0, Clock::now());
    }
    std::printf("    ~Engine took %.1f ms with the connect (2500 ms) still in flight\n", destroyMs);
    CHECK(destroyMs < 1000.0);
    // The worker comes back after the engine is gone: it must finish and let go of what it made, touching nothing of the engine.
    waitFor(g_connectDone, 1, 6000);
    CHECK_EQ(g_connectDone.load(), 1);
    waitFor(g_sinksAlive, 0, 3000);
    CHECK_EQ(g_sinksAlive.load(), 0);
}


// F  a key given while an open is pending is refused, and nothing is keyed on the board that lands
void testNoKeyWhileOpening() {
    std::printf("F  PTT 1 / LATCH 1 while an open is pending: refused; nothing keyed after the new board lands\n");
    resetFakes(900, false);
    g_keyable = true;
    Rig r;
    openBoardA(r);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK(Access::pending(*r.eng));
    CHECK(Access::haveSink(*r.eng));   // A is still installed while B connects
    r.page->beat();
    const int latch = r.resultOf(r.submit(FOXAPI_OP_TX_LATCH, nullptr, 1));
    const int ptt = r.resultOf(r.submit(FOXAPI_OP_TX_PTT, nullptr, 1));
    std::printf("    LATCH 1 -> %d, PTT 1 -> %d (BUSY is %d); keyed now: %d\n", latch, ptt, FOXAPI_BUSY, keyedNow(r) ? 1 : 0);
    CHECK_EQ(latch, FOXAPI_BUSY);
    CHECK_EQ(ptt, FOXAPI_BUSY);
    CHECK(!keyedNow(r));
    CHECK(!Access::transmitting(*r.eng));
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 5000));
    CHECK_EQ(Access::sinkName(*r.eng), std::string("ledger transmit radio"));   // B landed
    r.passUntil([&] { return false; }, 400);   // passes after the landing: a key carried over would be down by now
    std::printf("    after the landing: keyed %d, transmitting %d, board starts %d\n", keyedNow(r) ? 1 : 0, Access::transmitting(*r.eng) ? 1 : 0, g_starts.load());
    CHECK(!keyedNow(r));
    CHECK(!Access::transmitting(*r.eng));
    CHECK_EQ(g_starts.load(), 0);   // nobody keyed B, and B was never started
}

// G  the safer direction is never refused
void testReleaseStillWorksWhileOpening() {
    std::printf("G  PTT 0 / LATCH 0 while an open is pending are answered OK\n");
    resetFakes(700, false);
    Rig r;
    openBoardA(r);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK(Access::pending(*r.eng));
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_PTT, nullptr, 0)), FOXAPI_OK);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_LATCH, nullptr, 0)), FOXAPI_OK);
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 5000));
}

// H  the plugins' arming window
void testNoArmWhileOpening() {
    std::printf("H  a plugin ARM while an open is pending is refused BUSY; no window after the board lands\n");
    resetFakes(700, false);
    Rig r;
    openBoardA(r);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    const int arm = r.resultOf(r.submit(FOXAPP_OP_TX_PLUGIN_ARM, nullptr, 5));
    std::printf("    TX_PLUGIN_ARM 5 -> %d (BUSY is %d); window open: %d\n", arm, FOXAPI_BUSY, Access::armed(*r.eng) ? 1 : 0);
    CHECK_EQ(arm, FOXAPI_BUSY);
    CHECK(!Access::armed(*r.eng));
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 5000));
    r.passUntil([&] { return false; }, 200);
    CHECK(!Access::armed(*r.eng));
}

// I  the address cannot change under a pending open
void testAddressFixedWhileOpening() {
    std::printf("I  SET_TRANSMIT_ARGS while an open is pending is refused BUSY; the address that lands is the one that was opened\n");
    resetFakes(700, false);
    Rig r;
    CHECK_EQ(r.resultOf(r.submit(FOXAPP_OP_SET_TRANSMIT_ARGS, "ip:192.0.2.1")), FOXAPI_OK);   // accepted with nothing pending
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    const int other = r.resultOf(r.submit(FOXAPP_OP_SET_TRANSMIT_ARGS, "ip:192.0.2.99"));
    std::printf("    SET_TRANSMIT_ARGS -> %d (BUSY is %d); address now \"%s\"\n", other, FOXAPI_BUSY, Access::args(*r.eng).c_str());
    CHECK_EQ(other, FOXAPI_BUSY);
    CHECK_EQ(Access::args(*r.eng), std::string(kAddress));
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 5000));
    CHECK_EQ(Access::args(*r.eng), std::string(kAddress));
    CHECK_EQ(fact(r, "fact.transmit.args"), std::string(kAddress));
}

// J  the pending state is published
void testPendingIsPublished() {
    std::printf("J  fact.transmit.opening is 1 from the accepted open until the answer\n");
    resetFakes(600, false);
    Rig r;
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("0"));
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("1"));
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 5000));
    r.pass();
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("0"));
    // a failure
    resetFakes(300, true);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_CLOSE)), FOXAPI_OK);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);   // (the first open had landed: no abandoned connect is outstanding)
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("1"));
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 3000));
    r.pass();
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("0"));
    // a close
    resetFakes(1500, false);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("1"));
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_CLOSE)), FOXAPI_OK);
    r.pass();
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("0"));
    waitFor(g_connectDone, 1, 4000);
}

// K  an open with a board installed, landing and failing
void testOpenOverInstalledBoard() {
    std::printf("K  an open with a board installed: A stays until B lands; a failed open leaves the installed board usable\n");
    resetFakes(500, false);
    g_keyable = true;
    Rig r;
    openBoardA(r);
    CHECK_EQ(Access::sinkName(*r.eng), std::string("Test transmitter (dummy load, no RF)"));
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK_EQ(Access::sinkName(*r.eng), std::string("Test transmitter (dummy load, no RF)"));   // still A
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 5000));
    CHECK_EQ(Access::sinkName(*r.eng), std::string("ledger transmit radio"));                  // now B
    // a failed open over B: B stays, and a key is accepted again
    resetFakes(300, true);
    g_keyable = true;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, "ip:192.0.2.78")), FOXAPI_OK);
    CHECK(r.passUntil([&] { return !Access::pending(*r.eng); }, 3000));
    CHECK_EQ(Access::sinkName(*r.eng), std::string("ledger transmit radio"));
    r.page->beat();
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_LATCH, nullptr, 1)), FOXAPI_OK);   // nothing pending any more: the key is allowed again
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_LATCH, nullptr, 0)), FOXAPI_OK);
}

// L  close while pending with a board installed
void testCloseWhilePendingOverBoard() {
    std::printf("L  TX_CLOSE while an open is pending with a board installed\n");
    resetFakes(900, false);
    Rig r;
    openBoardA(r);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_CLOSE)), FOXAPI_OK);
    CHECK(!Access::pending(*r.eng));
    CHECK(!Access::haveSink(*r.eng));
    waitFor(g_connectDone, 1, 4000);
    r.passUntil([&] { return false; }, 300);
    CHECK(!Access::haveSink(*r.eng));   // the late board is never installed
    waitFor(g_sinksAlive, 0, 2000);
}

// M  Open, Close, Open: two connects never overlap
void testReopenWaitsForTheAbandonedConnect() {
    std::printf("M  Open, Close, Open: the second open is refused while the abandoned connect runs\n");
    resetFakes(900, false);
    Rig r;
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_CLOSE)), FOXAPI_OK);
    const int second = r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress));
    std::printf("    the second open -> %d (BUSY is %d); connects started %d\n", second, FOXAPI_BUSY, g_connects.load());
    CHECK_EQ(second, FOXAPI_BUSY);
    CHECK_EQ(g_connects.load(), 1);
    waitFor(g_connectDone, 1, 4000);
    const auto t0 = Clock::now();
    while (Access::ending(*r.eng) > 0 && ms(t0, Clock::now()) < 2000) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK_EQ(Access::ending(*r.eng), 0);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);   // the first connect has ended: accepted
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 5000));
    CHECK_EQ(g_connects.load(), 2);
}

// N  std::async cannot start a thread
void testNoThreadDoesNotWedgeTheFlag() {
    std::printf("N  a worker that could not be started leaves nothing pending\n");
    resetFakes(200, false);
    Rig r;
    Access::noThread(*r.eng, true);
    const int first = r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress));
    std::printf("    TX_OPEN -> %d (FAILED is %d), pending %d, error \"%s\"\n", first, FOXAPI_FAILED, Access::pending(*r.eng) ? 1 : 0, Access::error(*r.eng).c_str());
    CHECK_EQ(first, FOXAPI_FAILED);
    CHECK(!Access::pending(*r.eng));
    CHECK(!Access::error(*r.eng).empty());
    CHECK_EQ(fact(r, "fact.transmit.opening"), std::string("0"));
    Access::noThread(*r.eng, false);
    CHECK_EQ(r.resultOf(r.submit(FOXAPI_OP_TX_OPEN, kAddress)), FOXAPI_OK);   // not BUSY for ever
    CHECK(r.passUntil([&] { return Access::haveSink(*r.eng); }, 3000));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_tx_open_async\n");
    engine_api_test::hermetic("txopenasync");
    g_plugins = fs::path(std::getenv("LOCALAPPDATA") != nullptr ? std::getenv("LOCALAPPDATA") : ".") / "txopenasync_plugins";
    std::error_code ec;
    fs::remove_all(g_plugins, ec);
    fs::create_directories(g_plugins, ec);
    testSlowConnectDoesNotHoldThePass();
    testFailedConnectIsPublished();
    testSecondRequestIsRefused();
    testCloseWhilePending();
    testExitWhilePending();
    testNoKeyWhileOpening();
    testReleaseStillWorksWhileOpening();
    testNoArmWhileOpening();
    testAddressFixedWhileOpening();
    testPendingIsPublished();
    testOpenOverInstalledBoard();
    testCloseWhilePendingOverBoard();
    testReopenWaitsForTheAbandonedConnect();
    testNoThreadDoesNotWedgeTheFlag();
    return check::finish("test_engine_api_tx_open_async");
}
