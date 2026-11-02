// test_engine_api_tx_frequency_hold.cpp - THE TRANSMIT FREQUENCY IS FROZEN WHILE KEYED (the owner's decision of 2026-11-02, deep review of engine/stage4 batch A;
// docs/engine-stage4.md 6.31; core/transmitter.hpp retuneHz).
//
// While the radio is keyed - by the PTT, the latch, the web remote's hold or a plugin's key - the frequency it transmits on is the one it had at key-down, and
// it does not move until the key is up, whatever moves the RECEIVER: a scanner stepping, a plugin's tune, a web page's centre, the operator's own dial. The
// receiver stays free to tune. At key-up the transmitter follows the dial again. SPLIT is what it always was. (Before this, Engine::followTransmitFrequency
// retuned the transmitter once a pass whatever the key was doing: a scanner or a plugin could walk a keyed carrier across the band - off the frequency the
// operator keyed on, into somebody else's.) It is settled in ONE place, Transmitter::retuneHz, because that is the only place the sink's frequency changes:
// the passive follow holds silently (no error, nothing to see), and an operation that was ASKED to set the frequency is refused, with the reason.
//
//   A  THE TRANSMITTER, with a fake sink that counts retunes: every kind of key holds the frequency (PTT, latch, remote, plugin); the refusal is not an error;
//      the tail counts as keyed - the thread that has played its ramp and ended is still "keyed" until tick() has acknowledged it; a retune is accepted again
//      once the key is up; a sink that refuses is a Failure, and says why.
//   B  THE ENGINE, keyed with SPLIT off, the receiver moved by each source in turn: a web page's SET_CENTRE, the operator's dial (SET_FREQUENCY, STEP_TUNE,
//      SET_VFO_OFFSET), a running scanner's steps, a plugin's request_tune with the TUNE grant. After each: the receiver HAS moved (the premise: otherwise the
//      test proves nothing), the transmit sink has NOT, the published txFrequencyHz is where the radio is, and no error was recorded. At key-up the dial is
//      followed again, in the pass that un-keys.
//   C  THE EXPLICIT OPERATIONS ARE REFUSED WITH A REASON while keyed (TX_SET_FREQUENCY and TX_SET_SPLIT): BUSY, a message that says why, nothing changed; and
//      accepted again after the key-up.
//   D  SPLIT is unaffected: a split transmitter sits on its split frequency whatever the dial does, keyed or not; the held dial moves do not touch the
//      split frequency; and after the key-up the split frequency can be set again.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/plugin_api.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "source/tx_sink.hpp"

namespace cc = cascade::core;
using cascade::engine::Engine;
using testing::cmd;
using testing::kAllGrants;

namespace cascade::gui {
// The friend the Engine names for its tests.
struct AppWindowTestAccess {
    static double sinkHz(Engine& e) { return e.transmitter_.frequencyHz(); }
    static bool held(Engine& e) { return e.transmitter_.frequencyHeld(); }
    static std::string transmitError(Engine& e) { return e.transmitError_; }
    // A PLUGIN THAT TUNES THE RECEIVER with the TUNE grant, by the engine's own route: the host API core holds a client for the module (live, granted), the
    // plugin's request_tune queues a retune on it, and the pass applies it (Engine::applyPluginApi, after checking the grant again). The module itself is not
    // needed - what is under test is what the pass does with a retune that came from a plugin.
    static std::int32_t pluginTune(Engine& e, const std::string& key, double hz) {
        cc::PluginApiCore& core = *e.pluginApi_;
        core.client(key, "Tuner", CASCADE_CAP_HOST_CLIENT);
        core.setLiveSet({key});
        core.setTuneGranted(key, true);
        return core.queueCentreRetune(key, hz);
    }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- A: the transmitter -----------------------------------------------------------------------------------------------------------
class HoldSink final : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    void finish() override {
        finished_ = true;
        stop();
    }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return hz_.load(); }
    bool setCenterFrequencyHz(double hz) override {
        ++retunes;
        if (refuse) {
            error_ = "this test radio refuses to retune";
            return false;
        }
        hz_.store(hz);
        return true;
    }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return -89.75; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "hold sink"; }
    const char* lastError() const override { return error_.c_str(); }

    std::atomic<int> retunes{0};
    std::atomic<bool> refuse{false};
    bool finished() const { return finished_; }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<double> hz_{145.0e6};
    std::string error_;
};

struct TxRig {
    cc::Transmitter tx;
    HoldSink* sink = nullptr;
    TxRig() {
        auto s = std::make_unique<HoldSink>();
        sink = s.get();
        tx.setSink(std::move(s));
    }
    // (A hand on the key - the PTT, the latch - is held by a window that stamps every frame: frontEndAlive(), as the key holder's heartbeat does; a thread
    // that has not been stamped for a second lets go of it.)
    // Keys with `key`, ticks until the radio is on the air; returns whether it is.
    template <class F>
    bool keyWith(F&& key) {
        tx.frontEndAlive();
        key();
        for (int i = 0; i < 100 && !tx.transmitting(); ++i) {
            tx.frontEndAlive();
            tx.tick();
            sleepMs(2);
        }
        return tx.transmitting();
    }
    template <class F>
    bool unkeyWith(F&& unkey) {
        unkey();
        for (int i = 0; i < 200 && tx.transmitting(); ++i) {
            tx.frontEndAlive();
            tx.tick();
            sleepMs(2);
        }
        return !tx.transmitting();
    }
};

void testTransmitterHoldsWhileKeyed() {
    std::printf("  A: the transmitter\n");
    struct Kind {
        const char* name;
        std::function<void(TxRig&)> down;
        std::function<void(TxRig&)> up;
    };
    const Kind kinds[] = {
        {"PTT", [](TxRig& r) { r.tx.setPttHeld(true); }, [](TxRig& r) { r.tx.setPttHeld(false); }},
        {"latch", [](TxRig& r) { r.tx.setLatched(true); }, [](TxRig& r) { r.tx.setLatched(false); }},
        {"remote hold", [](TxRig& r) { r.tx.keyRemote(); }, [](TxRig& r) { r.tx.releaseRemote("test"); }},
        {"plugin key", [](TxRig& r) { r.tx.setPluginKey(true); }, [](TxRig& r) { r.tx.setPluginKey(false); }},
    };
    for (const Kind& k : kinds) {
        TxRig r;
        // idle: a retune is a retune
        CHECK(!r.tx.frequencyHeld());
        CHECK(r.tx.retuneHz(146.0e6) == cc::Transmitter::Retune::Done);
        CHECK(r.tx.frequencyHz() == 146.0e6);
        const int before = r.sink->retunes;
        CHECK(r.keyWith([&] { k.down(r); }));
        CHECK(r.tx.frequencyHeld());
        // ...keyed: nothing moves, nothing is wrong
        const cc::Transmitter::Retune held = r.tx.retuneHz(147.0e6);
        if (held != cc::Transmitter::Retune::Held) { std::printf("    FAIL a retune while keyed by the %s answered %d\n", k.name, static_cast<int>(held)); }
        CHECK(held == cc::Transmitter::Retune::Held);
        CHECK(!r.tx.setFrequencyHz(147.0e6));
        CHECK(r.tx.frequencyHz() == 146.0e6);
        CHECK(r.sink->retunes == before);   // the sink was not even asked
        CHECK(r.tx.lastError().empty());
        // an un-keyed transmitter takes it again
        CHECK(r.unkeyWith([&] { k.up(r); }));
        CHECK(!r.tx.frequencyHeld());
        CHECK(r.tx.retuneHz(147.0e6) == cc::Transmitter::Retune::Done);
        CHECK(r.tx.frequencyHz() == 147.0e6);
    }
}

void testTailCountsAsKeyed() {
    std::printf("  A: the tail counts as keyed\n");
    TxRig r;
    CHECK(r.tx.retuneHz(146.0e6) == cc::Transmitter::Retune::Done);
    CHECK(r.keyWith([&] { r.tx.setPttHeld(true); }));
    // Let go of the key and DO NOT TICK: the TX thread plays its ramp by itself and ends, silencing the radio - but nothing has acknowledged it yet.
    r.tx.setPttHeld(false);
    bool ended = false;
    for (int i = 0; i < 300 && !ended; ++i) {
        ended = r.sink->finished();
        if (!ended) { sleepMs(2); }
    }
    CHECK(ended);   // (the premise: the thread has played the tail and let the radio go)
    if (!r.tx.frequencyHeld()) { std::printf("    FAIL the frequency was released before the key-up was acknowledged\n"); }
    CHECK(r.tx.frequencyHeld());
    CHECK(r.tx.retuneHz(148.0e6) == cc::Transmitter::Retune::Held);
    CHECK(r.tx.frequencyHz() == 146.0e6);
    r.tx.tick();   // the acknowledgement
    CHECK(!r.tx.transmitting());
    CHECK(!r.tx.frequencyHeld());
    CHECK(r.tx.retuneHz(148.0e6) == cc::Transmitter::Retune::Done);
    CHECK(r.tx.frequencyHz() == 148.0e6);
}

void testRefusingSinkIsAFailure() {
    std::printf("  A: a sink that refuses is a failure with a reason; no sink is a failure\n");
    TxRig r;
    r.sink->refuse = true;
    CHECK(r.tx.retuneHz(146.0e6) == cc::Transmitter::Retune::Failed);
    CHECK(!r.tx.lastError().empty());
    cc::Transmitter none;
    CHECK(none.retuneHz(146.0e6) == cc::Transmitter::Retune::Failed);
    CHECK(!none.frequencyHeld());
}

// ---- B, C, D: the engine ------------------------------------------------------------------------------------------------------------
struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    Rig() {
        eng.configureApi("testHooks=1");
        eng.initialise();
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(s->ok());
        pass(2);
    }
    static const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }
    void pass(int n = 1) {
        for (int i = 0; i < n; ++i) {
            s->beat();
            eng.controlPass();
            sleepMs(3);
        }
    }
    testing::Outcome run(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < 40 && !o.landed; ++i) {
            pass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[k];
                }
            }
        }
        return o;
    }
    FoxReceiverState st() { return s->state(); }
    double dial() { return st().tunedHz; }
    double sink() { return Access::sinkHz(eng); }
    bool keyed() { return (st().flags & FOXAPI_RX_TX_KEYED) != 0u; }
    void open() {
        CHECK_EQ(run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).status(), FOXAPI_OK);
        pass(3);
    }
    void key() {
        CHECK_EQ(run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        pass(3);
        CHECK(keyed());
        CHECK(Access::held(eng));
    }
    void unkey() {
        CHECK_EQ(run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
        pass(3);
        CHECK(!keyed());
        CHECK(!Access::held(eng));
    }
};

bool sameHz(double a, double b) { return std::fabs(a - b) < 1.0; }

FoxCommand scanRun(long long on, double startHz, double stopHz, double stepHz) {
    FoxCommand c = cmd(FOXAPI_OP_SCANNER_RUN, startHz, on);
    c.num[1] = stopHz;
    c.num[2] = stepHz;
    return c;
}
FoxCommand scanConfig(double dwell, double hold, double resume, double listen) {
    FoxCommand c = cmd(FOXAPI_OP_SCANNER_CONFIG, dwell);
    c.num[1] = hold;
    c.num[2] = resume;
    c.num[3] = listen;
    return c;
}

// The receiver moved to somewhere new, the transmitter did not.
void expectHeld(Rig& r, double heldHz, double dialBefore, const char* what) {
    const double dialNow = r.dial();
    if (sameHz(dialNow, dialBefore)) { std::printf("    the receiver did not move for %s (the test would prove nothing)\n", what); }
    CHECK(!sameHz(dialNow, dialBefore));   // the premise
    if (!sameHz(r.sink(), heldHz)) { std::printf("    FAIL %s moved the keyed transmitter: %.1f Hz -> %.1f Hz\n", what, heldHz, r.sink()); }
    CHECK(sameHz(r.sink(), heldHz));
    const FoxReceiverState st = r.st();
    if (!sameHz(st.txFrequencyHz, heldHz)) { std::printf("    FAIL the published TX frequency (%.1f Hz) is not where the radio is (%.1f Hz) after %s\n", st.txFrequencyHz, heldHz, what); }
    CHECK(sameHz(st.txFrequencyHz, heldHz));
    CHECK(Access::transmitError(r.eng).empty());   // holding is silent
    CHECK(st.txError[0] == '\0');
}

void testEngineHoldsWhileKeyed() {
    std::printf("  B: keyed, SPLIT off: nothing that moves the receiver moves the transmitter\n");
    Rig r;
    r.open();
    // not keyed: the transmitter follows the dial (the control)
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 101.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), r.dial()));

    r.key();
    const double heldHz = r.sink();
    CHECK(sameHz(heldHz, r.dial()));   // the frequency it had at key-down: where the dial was
    double dialBefore = r.dial();

    // a web page's centre
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 102.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    expectHeld(r, heldHz, dialBefore, "SET_CENTRE");
    dialBefore = r.dial();

    // the operator's dial, three ways
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_FREQUENCY, 103.5e6)).status(), FOXAPI_OK);
    r.pass(3);
    expectHeld(r, heldHz, dialBefore, "SET_FREQUENCY");
    dialBefore = r.dial();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_STEP_TUNE, 25000.0, 3)).status(), FOXAPI_OK);
    r.pass(3);
    expectHeld(r, heldHz, dialBefore, "STEP_TUNE");
    dialBefore = r.dial();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_VFO_OFFSET, 50000.0)).status(), FOXAPI_OK);
    r.pass(3);
    expectHeld(r, heldHz, dialBefore, "SET_VFO_OFFSET");
    dialBefore = r.dial();

    // a plugin's tune, with the TUNE grant
    CHECK_EQ(Access::pluginTune(r.eng, "tuner-1.0.0-abi3-win-x64.dll", 104.0e6), static_cast<std::int32_t>(CASCADE_TUNE_OK));
    r.pass(4);
    expectHeld(r, heldHz, dialBefore, "a plugin's request_tune");
    dialBefore = r.dial();

    // a running scanner's steps: the receiver walks the range, the transmitter does not
    CHECK_EQ(r.run(scanConfig(5, 0, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.run(scanRun(1, 110.0e6, 110.5e6, 100000.0)).status(), FOXAPI_OK);
    std::vector<double> seen;
    for (int i = 0; i < 80; ++i) {
        r.pass();
        const double d = r.dial();
        if (seen.empty() || !sameHz(seen.back(), d)) { seen.push_back(d); }
        CHECK(sameHz(r.sink(), heldHz));   // at every pass
    }
    if (seen.size() < 3) { std::printf("    the scanner took %zu distinct frequencies (the test would prove little)\n", seen.size()); }
    CHECK(seen.size() >= 3);
    CHECK(!sameHz(r.dial(), dialBefore));
    CHECK(sameHz(r.st().txFrequencyHz, heldHz));
    CHECK(Access::transmitError(r.eng).empty());
    CHECK_EQ(r.run(scanRun(0, 110.0e6, 110.5e6, 100000.0)).status(), FOXAPI_OK);

    // THE KEY-UP: the dial is followed again, in the pass that un-keys
    const double dialAtRelease = r.dial();
    CHECK(!sameHz(dialAtRelease, heldHz));
    r.unkey();
    if (!sameHz(r.sink(), dialAtRelease)) { std::printf("    FAIL after the key-up the transmitter is at %.1f Hz and the dial at %.1f Hz\n", r.sink(), dialAtRelease); }
    CHECK(sameHz(r.sink(), dialAtRelease));
    CHECK(sameHz(r.st().txFrequencyHz, dialAtRelease));
    // ...and keeps following
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 105.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), r.dial()));
}

void testExplicitOpsAreRefused() {
    std::printf("  C: the operations that SET the transmit frequency are refused while keyed\n");
    Rig r;
    r.open();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 101.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    r.key();
    const double heldHz = r.sink();
    const testing::Outcome freq = r.run(cmd(FOXAPI_OP_TX_SET_FREQUENCY, 110.0e6));
    if (freq.status() != FOXAPI_BUSY) { std::printf("    FAIL TX_SET_FREQUENCY while keyed answered %d (message \"%s\")\n", freq.status(), freq.result.message); }
    CHECK_EQ(freq.status(), FOXAPI_BUSY);
    CHECK(freq.landed);
    const std::string said = freq.result.message;
    CHECK(said.find("keyed") != std::string::npos);
    CHECK(said.find("held") != std::string::npos);
    const testing::Outcome split = r.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 111.0e6, 1));
    CHECK_EQ(split.status(), FOXAPI_BUSY);
    CHECK((split.landed ? std::string(split.result.message) : std::string("?")).find("keyed") != std::string::npos);
    r.pass(3);
    CHECK(sameHz(r.sink(), heldHz));
    CHECK((r.st().txFlags & FOXAPI_TX_SPLIT) == 0u);   // the split switch did not move
    CHECK(sameHz(r.st().txFrequencyHz, heldHz));
    // after the key-up they are accepted again
    r.unkey();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 111.0e6, 1)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK((r.st().txFlags & FOXAPI_TX_SPLIT) != 0u);
    CHECK(sameHz(r.sink(), 111.0e6));
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_FREQUENCY, 112.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), 112.0e6));
}

void testSplitIsUnaffected() {
    std::printf("  D: SPLIT is what it was\n");
    Rig r;
    r.open();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 101.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 145.8e6, 1)).status(), FOXAPI_OK);   // not keyed: accepted
    r.pass(3);
    CHECK(sameHz(r.sink(), 145.8e6));          // the transmitter is on its split frequency, the receiver elsewhere
    CHECK(sameHz(r.st().txSplitHz, 145.8e6));
    r.key();
    CHECK(sameHz(r.sink(), 145.8e6));
    const double dialBefore = r.dial();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 103.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(!sameHz(r.dial(), dialBefore));
    CHECK(sameHz(r.sink(), 145.8e6));          // split: the dial never mattered
    CHECK(sameHz(r.st().txSplitHz, 145.8e6));  // and the held moves did not touch the split frequency
    CHECK(sameHz(r.st().txFrequencyHz, 145.8e6));
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_FREQUENCY, 146.0e6)).status(), FOXAPI_BUSY);   // an explicit set is refused while keyed, split or not
    CHECK(sameHz(r.st().txSplitHz, 145.8e6));
    r.unkey();
    CHECK(sameHz(r.sink(), 145.8e6));
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_FREQUENCY, 146.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), 146.0e6));
    CHECK(sameHz(r.st().txSplitHz, 146.0e6));
    // coming out of split follows the receiver again
    CHECK_EQ(r.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 0.0, 0)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), r.dial()));
}

// ---- E: a dial move and a key press in ONE batch -------------------------------------------------------------------------------------
// The transmitter follows the dial at the END of the pass (pumpPublish); the key drive (ApiHostImpl::txPass) runs before it, at the end of beginPass. A batch that moves the
// dial and presses the key - a web client's two messages in one tick, a script - keyed the radio while the transmitter was still on the dial's previous place, and the freeze
// would hold it there for the whole transmission: a carrier on a frequency nobody chose. The follow is made before the tick that can key, so the frequency frozen at key-down
// is the one the dial HAS at key-down.
void testKeyPressInTheSameBatchAsADialMove() {
    std::printf("  E: a dial move and a key press in one batch: the carrier starts where the dial is\n");
    Rig r;
    r.open();
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SET_CENTRE, 101.0e6)).status(), FOXAPI_OK);
    r.pass(3);
    CHECK(sameHz(r.sink(), r.dial()));
    const double before = r.dial();
    FoxCommand batch[2] = {cmd(FOXAPI_OP_SET_FREQUENCY, 107.5e6), cmd(FOXAPI_OP_TX_LATCH, 0, 1)};
    FoxSubmitResult results[2] = {};
    for (auto& x : results) { x.structSize = sizeof(x); }
    CHECK(Rig::api()->submit(r.s->raw(), batch, 2, results) >= 0);
    CHECK_EQ(results[0].status, FOXAPI_OK);
    CHECK_EQ(results[1].status, FOXAPI_OK);
    r.pass(1);   // the pass that applies both
    CHECK(Access::held(r.eng));
    CHECK(!sameHz(r.dial(), before));   // the premise: the dial moved in that pass
    if (!sameHz(r.sink(), r.dial())) { std::printf("    FAIL keyed on %.1f Hz while the dial is at %.1f Hz\n", r.sink(), r.dial()); }
    CHECK(sameHz(r.sink(), r.dial()));
    const double heldHz = r.sink();
    r.pass(3);
    CHECK(sameHz(r.sink(), heldHz));    // and it stays there
    r.unkey();
    CHECK(sameHz(r.sink(), r.dial()));
}

}  // namespace

int main() {
    engine_api_test::hermetic("tx_frequency_hold");
    std::printf("test_engine_api_tx_frequency_hold\n");
    testTransmitterHoldsWhileKeyed();
    testTailCountsAsKeyed();
    testRefusingSinkIsAFailure();
    testEngineHoldsWhileKeyed();
    testExplicitOpsAreRefused();
    testSplitIsUnaffected();
    testKeyPressInTheSameBatchAsADialMove();
    return check::finish("test_engine_api_tx_frequency_hold");
}
