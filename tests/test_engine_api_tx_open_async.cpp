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

class LedgerSink final : public cascade::source::IqSink {
public:
    LedgerSink() { ++g_sinksAlive; }
    ~LedgerSink() override { --g_sinksAlive; }
    bool start() override { return false; }   // never keys: this ledger cannot be started
    void stop() override {}
    bool running() const override { return false; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.083e6; hi = 61.44e6; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return -89.75; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t) override { return 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "ledger transmit radio"; }
    const char* lastError() const override { return ""; }
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
    std::uint64_t submit(std::uint32_t op, const char* text = nullptr) {
        const FoxCommand c = testing::cmd(op, 0.0, 0, text);
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
    g_connects = 0;
    g_connectDone = 0;
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
    return check::finish("test_engine_api_tx_open_async");
}
