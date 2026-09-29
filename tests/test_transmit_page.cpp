// test_transmit_page.cpp - the TRANSMIT page's arithmetic.
//
// Small, and one of these checks is the most important in the whole
// change-set: the power control on that page reads LEFT TO RIGHT as quiet to
// loud, while the number under it is an ATTENUATION where 0 dB is FULL
// OUTPUT. That inversion happens once, here, and if it is ever inverted back
// the panel will show a slider at the bottom of its travel over a radio at
// full power.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/transmit_page.hpp"
#include "test_check.hpp"

using cascade::core::Transmitter;
using cascade::gui::TxLamp;

namespace {

// A radio that only counts how often it was started - which is the number of
// times something KEYED it.
class CountingSink : public cascade::source::IqSink {
public:
    bool start() override {
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
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
    std::size_t write(const std::complex<float>*, std::size_t n) override {
        return running_ ? n : 0;
    }
    bool faulted() const override { return false; }
    const char* name() const override { return "counting radio"; }
    const char* lastError() const override { return ""; }

    std::atomic<int> starts{0};

private:
    std::atomic<bool> running_{false};
};

// One frame's worth of what the page saw.
struct PageFrame {
    bool pageLive = false;
    bool latchPressed = false;
    bool pttHeld = false;
};

// A PAGE: what one frame of it submits on the window's engine API session,
// given what the page saw and the state the session read. Since engine stage
// 4 the page keys the transmitter only through commands (API 0.2's THE KEY).
using PageFn = std::function<std::uint32_t(const PageFrame&, const FoxReceiverState&, FoxCommand* out)>;

// THE PAGE AS IT IS WIRED NOW: gui::txKeyCommands, called once a frame by the
// frame loop (AppWindow::pumpTransmitKey) whether or not the page was drawn.
PageFn fixedPage() {
    auto mem = std::make_shared<cascade::gui::TxKeyMemory>();
    return [mem](const PageFrame& f, const FoxReceiverState& st, FoxCommand* out) {
        cascade::gui::TxKeyFrame k;
        k.pageLive = f.pageLive;
        k.pttHeld = f.pttHeld;
        k.latchPressed = f.latchPressed;
        k.latchedShown = (st.flags & FOXAPI_RX_TX_LATCHED) != 0u;
        k.latchMine = k.latchedShown && (st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
        return cascade::gui::txKeyCommands(k, *mem, out);
    };
}

// THE CONTROL: the 0.99.34 page's shape, carried over to commands - a page
// that REMEMBERS its latch and sends its key as a LEVEL every frame it is
// drawn (LATCH of what it remembers, PTT of what is held), and nothing when
// it is not drawn. Kept as a control, not as behaviour anybody wants: it
// proves the scenarios below can see a page that does not let go.
PageFn levelPage() {
    auto latched = std::make_shared<bool>(false);
    return [latched](const PageFrame& f, const FoxReceiverState&, FoxCommand* out) {
        std::uint32_t n = 0;
        if (!f.pageLive) { return n; }
        if (f.latchPressed) { *latched = !*latched; }
        auto add = [&](std::uint32_t op, long long v) {
            FoxCommand c{};
            c.structSize = sizeof(c);
            c.op = op;
            c.ival[0] = v;
            out[n++] = c;
        };
        add(FOXAPI_OP_TX_LATCH, *latched ? 1 : 0);
        add(FOXAPI_OP_TX_PTT, f.pttHeld ? 1 : 0);
        return n;
    };
}

struct KeyOutcome {
    bool latchClosedPageReleases = false;    // LATCH on, page closed
    bool latchRolledUpPageReleases = false;  // LATCH on, page rolled up (same input)
    bool pttClosedPageReleases = false;      // PTT down, page closed
    bool latchFailsafeStays = false;         // the latch timed out: no re-latch
    bool frozenWindowStays = false;          // dead-man's handle: no re-key on thaw
    bool pressesWork = false;                // LATCH keys and unkeys; PTT keys
};

}  // namespace

// The engine's transmitter, for the rig below (Engine's test friend).
namespace cascade::gui {
struct AppWindowTestAccess {
    static CountingSink* installSink(cascade::engine::Engine& e) {
        auto sink = std::make_unique<CountingSink>();
        CountingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        e.transmitter_.setInput(cascade::core::TxInput::Tone);
        return raw;
    }
    static Transmitter& tx(cascade::engine::Engine& e) { return e.transmitter_; }
};
}  // namespace cascade::gui

namespace {

// ONE ENGINE, PUMPED AS THE WINDOW PUMPS IT (stage 3a: its frame IS the
// engine's pass), a LOCAL session on it as the window opens one, and a radio
// that counts its starts. frame(): the heartbeat, the pass (which applies the
// batch the previous frame submitted, then ticks), the state read, and the
// page's batch submitted - the frame loop's order.
struct Rig {
    explicit Rig(const std::string& options = std::string()) {
        if (!options.empty()) { e.configureApi(options); }
        e.initialise();
        raw = cascade::gui::AppWindowTestAccess::installSink(e);
        api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TRANSMIT;
        p.clientName = "test page";
        api->open_session(e.apiHandle(), &p, &s);
    }
    ~Rig() {
        if (s != nullptr) { api->close_session(s); }
    }
    Transmitter& tx() { return cascade::gui::AppWindowTestAccess::tx(e); }
    void frame(const PageFn& page, const PageFrame& f) {
        api->heartbeat(s);
        e.pump();
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        api->read_state(s, &st);
        FoxCommand cmds[8];
        const std::uint32_t n = page(f, st, cmds);
        if (n > 0) { api->submit(s, cmds, n, nullptr); }
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        while (api->poll_results(s, r, 16) == 16) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    void frames(const PageFn& page, const PageFrame& f, int n) {
        for (int i = 0; i < n; ++i) { frame(page, f); }
    }

    cascade::engine::Engine e;
    CountingSink* raw = nullptr;
    const FoxEngineApi* api = nullptr;
    FoxSession* s = nullptr;
};

// Every scenario on its own engine, radio AND PAGE (makePage is called afresh
// for each, because a page that remembers its latch must not carry one
// scenario's into the next), driven only through that page's frames. A
// control lands ONE FRAME after it is submitted (engine-stage4.md decision 2:
// the window's batch is applied by the next pass), so every "released" is
// read two frames on.
KeyOutcome runKeyScenarios(const std::function<PageFn()>& makePage) {
    KeyOutcome o;
    const PageFrame idleLive{true, false, false};
    const PageFrame pressLatch{true, true, false};
    const PageFrame holdPtt{true, false, true};
    const PageFrame notDrawn{false, false, false};

    // Latched, then the page stops being drawn (closed, or rolled up - both
    // arrive here as a frame in which the page body did not run).
    for (int variant = 0; variant < 2; ++variant) {
        Rig r;
        const PageFn page = makePage();
        r.frames(page, idleLive, 1);
        r.frames(page, pressLatch, 1);
        r.frames(page, idleLive, 5);
        const bool keyed = r.tx().transmitting();
        r.frames(page, notDrawn, 3);
        const bool released = keyed && !r.tx().transmitting() && !r.tx().latched();
        r.frames(page, notDrawn, 20);
        (variant == 0 ? o.latchClosedPageReleases : o.latchRolledUpPageReleases) = released && !r.tx().transmitting();
    }

    // The PTT held down at the moment the page goes.
    {
        Rig r;
        const PageFn page = makePage();
        r.frames(page, holdPtt, 5);
        const bool keyed = r.tx().transmitting();
        r.frames(page, notDrawn, 3);
        const bool released = keyed && !r.tx().transmitting() && !r.tx().pttHeld();
        r.frames(page, notDrawn, 20);
        o.pttClosedPageReleases = released && !r.tx().transmitting();
    }

    // The latch's own failsafe (the engine's key rules; 150 ms here), with the
    // page open and nobody touching it.
    {
        Rig r("latchTimeoutMs=150");
        const PageFn page = makePage();
        r.frames(page, idleLive, 1);
        r.frames(page, pressLatch, 1);
        r.frames(page, idleLive, 3);
        const bool keyed = r.tx().transmitting();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::chrono::steady_clock::now() < until) { r.frames(page, idleLive, 1); }
        std::printf("  latch failsafe: keyed %d, starts %d, transmitting %d, latched %d\n", keyed ? 1 : 0,
                    r.raw->starts.load(), r.tx().transmitting() ? 1 : 0, r.tx().latched() ? 1 : 0);
        o.latchFailsafeStays = keyed && r.raw->starts.load() == 1 && !r.tx().transmitting() && !r.tx().latched();
    }

    // The frozen window: nothing calls anything - not the page, not the
    // heartbeat, and (stage 3a) not the engine's pass either - for longer
    // than the handle, then the frames come back with the page open and
    // nobody touching it.
    {
        Rig r;
        const PageFn page = makePage();
        r.frames(page, idleLive, 1);
        r.frames(page, pressLatch, 1);
        r.frames(page, idleLive, 3);
        const bool keyed = r.tx().transmitting();
        std::this_thread::sleep_for(Transmitter::kKeyAliveWait + std::chrono::milliseconds(300));
        r.frames(page, idleLive, 20);
        std::printf("  frozen window: keyed %d, starts %d, transmitting %d, latched %d\n", keyed ? 1 : 0,
                    r.raw->starts.load(), r.tx().transmitting() ? 1 : 0, r.tx().latched() ? 1 : 0);
        o.frozenWindowStays = keyed && r.raw->starts.load() == 1 && !r.tx().transmitting() && !r.tx().latched();
    }

    // And the controls still do what they are for (past the re-arm time
    // between the two latch presses: API 0.2's 1 s).
    {
        Rig r;
        const PageFn page = makePage();
        r.frames(page, idleLive, 3);
        const bool quietAtRest = !r.tx().transmitting();
        r.frames(page, pressLatch, 1);
        r.frames(page, idleLive, 5);
        const bool latchKeys = r.tx().transmitting() && r.tx().latched();
        r.frames(page, pressLatch, 1);
        r.frames(page, idleLive, 3);
        const bool latchUnkeys = !r.tx().transmitting() && !r.tx().latched();
        r.frames(page, holdPtt, 5);
        const bool pttKeys = r.tx().transmitting();
        r.frames(page, idleLive, 3);
        const bool pttReleases = !r.tx().transmitting();
        o.pressesWork = quietAtRest && latchKeys && latchUnkeys && pttKeys && pttReleases;
    }
    return o;
}

void printOutcome(const char* name, const KeyOutcome& o) {
    std::printf("%s: latch/page closed %d, latch/rolled up %d, ptt/page closed %d, latch failsafe %d, "
                "frozen window %d, presses %d\n",
                name, o.latchClosedPageReleases ? 1 : 0, o.latchRolledUpPageReleases ? 1 : 0,
                o.pttClosedPageReleases ? 1 : 0, o.latchFailsafeStays ? 1 : 0, o.frozenWindowStays ? 1 : 0,
                o.pressesWork ? 1 : 0);
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Is `at` on a line of code rather than inside a // comment?
bool isLiveCode(const std::string& text, std::size_t at) {
    if (at == std::string::npos) { return false; }
    const std::size_t bol = text.rfind('\n', at);
    const std::size_t start = (bol == std::string::npos) ? 0 : bol + 1;
    return text.substr(start, at - start).find("//") == std::string::npos;
}

// Every live-code occurrence of `needle` in `text`.
std::vector<std::size_t> liveHits(const std::string& text, const std::string& needle) {
    std::vector<std::size_t> hits;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        if (isLiveCode(text, at)) { hits.push_back(at); }
    }
    return hits;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. WHICH FREQUENCY THE TRANSMITTER IS ON
    // =====================================================================
    {
        // Not split: it follows the receiver, so a reply goes out where the
        // call came in - and moving the dial moves both.
        CHECK_NEAR(cascade::gui::txFrequencyHz(false, 432.0e6, 145.5e6), 145.5e6, 1e-6);
        // Split: unlinked, which is what a repeater and a satellite need and
        // is also the state in which somebody is not listening where they are
        // transmitting.
        CHECK_NEAR(cascade::gui::txFrequencyHz(true, 432.0e6, 145.5e6), 432.0e6, 1e-6);
    }

    // =====================================================================
    // 2. THE POWER CONTROL, AND THE DIRECTION IT RUNS IN
    //
    //    THE CHECK THIS FILE EXISTS FOR. An AD9361 publishes its transmit
    //    gain as [-89.75 .. 0], where the MINIMUM is silence and the MAXIMUM
    //    is full output. The slider has to run the other way - left quiet,
    //    right loud - and the two must agree exactly at both ends.
    // =====================================================================
    {
        constexpr double quiet = -89.75;
        constexpr double loud = 0.0;

        CHECK_NEAR(cascade::gui::txPowerFraction(quiet, quiet, loud), 0.0, 1e-6);
        CHECK_NEAR(cascade::gui::txPowerFraction(loud, quiet, loud), 1.0, 1e-6);
        // Half the SPAN is half the travel - the control is linear in the
        // board's decibels, which is what makes each step of the slider the
        // same change in power ratio.
        CHECK_NEAR(cascade::gui::txPowerFraction(-44.875, quiet, loud), 0.5, 1e-6);
        // MORE ATTENUATION IS LESS TRAVEL, which is the inversion stated as a
        // comparison rather than as a formula.
        CHECK(cascade::gui::txPowerFraction(-60.0, quiet, loud) <
              cascade::gui::txPowerFraction(-20.0, quiet, loud));

        // Out of range in either direction, and NaN, land on the QUIET end -
        // never on full output.
        CHECK_NEAR(cascade::gui::txPowerFraction(-500.0, quiet, loud), 0.0, 1e-6);
        CHECK_NEAR(cascade::gui::txPowerFraction(std::nan(""), quiet, loud), 0.0, 1e-6);
        // ...and a request above the loud end is drawn FULL, because that is
        // what the slider would be showing; the DRIVER is what refuses to
        // honour it (source::clampTxGainDb), and the two are deliberately
        // different jobs.
        CHECK_NEAR(cascade::gui::txPowerFraction(40.0, quiet, loud), 1.0, 1e-6);

        // A board that published a degenerate span is drawn quiet rather than
        // dividing by zero.
        CHECK_NEAR(cascade::gui::txPowerFraction(-10.0, 0.0, 0.0), 0.0, 1e-6);

        // The round trip: a slider position and back.
        for (int i = 0; i <= 10; ++i) {
            const float t = static_cast<float>(i) / 10.0f;
            const double db = cascade::gui::txPowerFromFraction(t, quiet, loud);
            CHECK_NEAR(cascade::gui::txPowerFraction(db, quiet, loud), t, 1e-5);
        }
        CHECK_NEAR(cascade::gui::txPowerFromFraction(0.0f, quiet, loud), quiet, 1e-9);
        CHECK_NEAR(cascade::gui::txPowerFromFraction(1.0f, quiet, loud), loud, 1e-9);
        // A slider position off either end is clamped to that end. This one
        // IS allowed to clamp to loud, because it describes where the control
        // is rather than what the radio will be given - the refusal lives in
        // the driver, where the range it is being refused against is known.
        CHECK_NEAR(cascade::gui::txPowerFromFraction(-3.0f, quiet, loud), quiet, 1e-9);
        CHECK_NEAR(cascade::gui::txPowerFromFraction(9.0f, quiet, loud), loud, 1e-9);
        CHECK_NEAR(cascade::gui::txPowerFromFraction(std::nanf(""), quiet, loud), quiet, 1e-9);
    }

    // =====================================================================
    // 3. WHAT THE READOUT SAYS
    // =====================================================================
    {
        char buf[32];
        cascade::gui::formatTxPower(buf, sizeof(buf), -20.0, -89.75);
        CHECK(std::string(buf) == "-20.0 dB");
        cascade::gui::formatTxPower(buf, sizeof(buf), 0.0, -89.75);
        CHECK(std::string(buf) == "0.0 dB");
        // THE BOTTOM OF THE BOARD'S OWN SPAN READS "QUIET", because
        // "-89.8 dB" is a number an operator has to think about and QUIET is
        // not - and the bottom is where a transmitter should be whenever
        // nobody has deliberately turned it up.
        cascade::gui::formatTxPower(buf, sizeof(buf), -89.75, -89.75);
        CHECK(std::string(buf) == "QUIET");
        // Within one attenuator step of the bottom is the bottom.
        cascade::gui::formatTxPower(buf, sizeof(buf), -89.6, -89.75);
        CHECK(std::string(buf) == "QUIET");
        // A number that is not one reads as quiet rather than as "nan dB".
        cascade::gui::formatTxPower(buf, sizeof(buf), std::nan(""), -89.75);
        CHECK(std::string(buf) == "QUIET");
        // A board with a different floor moves the word with it.
        cascade::gui::formatTxPower(buf, sizeof(buf), -40.0, -40.0);
        CHECK(std::string(buf) == "QUIET");
        cascade::gui::formatTxPower(buf, sizeof(buf), -20.0, -40.0);
        CHECK(std::string(buf) == "-20.0 dB");
        // A caller that hands it nothing gets nothing rather than a fault.
        cascade::gui::formatTxPower(nullptr, 0, -20.0, -89.75);
        cascade::gui::formatTxPower(buf, 0, -20.0, -89.75);
    }

    // =====================================================================
    // 4. THE LAMP
    //
    //    The distinction that matters is READY against NO RADIO: a page with
    //    no transmitter on it and a page with one that is not keyed look
    //    identical unless something says which, and they are a setup problem
    //    and a working transmitter respectively.
    // =====================================================================
    {
        CHECK(cascade::gui::txLampState(false, false, false) == TxLamp::NoRadio);
        CHECK(cascade::gui::txLampState(true, false, false) == TxLamp::Ready);
        CHECK(cascade::gui::txLampState(true, false, true) == TxLamp::Transmitting);
        CHECK(cascade::gui::txLampState(true, true, false) == TxLamp::Fault);
        // FAULT OUTRANKS TRANSMITTING. A sink that has faulted has already
        // stopped; a transmit lamp over it would be the panel claiming RF
        // that is not there, which is the one direction this readout must
        // never be wrong in.
        CHECK(cascade::gui::txLampState(true, true, true) == TxLamp::Fault);
        // A fault with no radio is still a fault, because the message that
        // goes with it is the useful thing on the page.
        CHECK(cascade::gui::txLampState(false, true, false) == TxLamp::Fault);

        CHECK(std::string(cascade::gui::txLampText(TxLamp::NoRadio)) == "NO RADIO");
        CHECK(std::string(cascade::gui::txLampText(TxLamp::Ready)) == "READY");
        CHECK(std::string(cascade::gui::txLampText(TxLamp::Transmitting)) == "ON AIR");
        CHECK(std::string(cascade::gui::txLampText(TxLamp::Fault)) == "FAULT");
    }

    // =====================================================================
    // 5. THE AUDIO METER
    //
    //    On a decibel scale, because a linear audio meter spends nine tenths
    //    of its travel in the top 20 dB and reads as dead for normal speech.
    // =====================================================================
    {
        CHECK_NEAR(cascade::gui::txMeterFraction(1.0f), 1.0, 1e-6);
        CHECK_NEAR(cascade::gui::txMeterFraction(0.0f), 0.0, 1e-6);
        // -50 dB is the floor, so half scale in decibels is half the travel.
        CHECK_NEAR(cascade::gui::txMeterFraction(std::pow(10.0f, -25.0f / 20.0f)), 0.5, 1e-3);
        // Ordinary speech at a tenth of full scale is -20 dB, which on a
        // linear meter would be a tenth of the bar and here is well over half.
        const float speech = cascade::gui::txMeterFraction(0.1f);
        std::printf("a -20 dB signal fills %.2f of the meter (a linear one: 0.10)\n", speech);
        CHECK(speech > 0.55f);
        CHECK(speech < 0.65f);
        // Below the floor is empty, not negative.
        CHECK_NEAR(cascade::gui::txMeterFraction(0.0001f), 0.0, 1e-6);
        CHECK_NEAR(cascade::gui::txMeterFraction(-1.0f), 0.0, 1e-6);
        // Over full scale is full, not more than full.
        CHECK_NEAR(cascade::gui::txMeterFraction(4.0f), 1.0, 1e-6);
    }

    // =====================================================================
    // 6. THE SENTENCE
    //
    //    It is on the page, in the README, and nowhere else - so it has to be
    //    the same words in both, which is why it is a constant rather than a
    //    string literal in app_window.cpp. What is pinned here is that it
    //    still SAYS the two things it exists to say.
    // =====================================================================
    {
        const std::string notice = cascade::gui::kTxLicenceNotice;
        std::printf("the page says: %s\n", notice.c_str());
        CHECK(notice.find("transmits") != std::string::npos);
        // The power figure, so somebody can decide between an antenna and an
        // attenuator without leaving the page.
        CHECK(notice.find("+7 dBm") != std::string::npos);
        // And whose responsibility it is, said plainly rather than as a
        // warning nobody reads.
        CHECK(notice.find("responsibility") != std::string::npos);
        CHECK(notice.find("licensing") != std::string::npos);
    }

    // =====================================================================
    // 7. THE KEY: WHAT THE PAGE SUBMITS, FRAME BY FRAME
    //
    //    Until 0.99.35 the page wrote its key to the transmitter from inside
    //    its own body, so a page that was closed or rolled up simply stopped
    //    writing: a LATCH stayed closed for up to a minute with no control on
    //    screen, and a PTT held at that moment stayed held for good. Since
    //    engine stage 4 the page SUBMITS its key on the window's engine API
    //    session, once a frame from the frame loop (gui::txKeyCommands, API
    //    0.2's THE KEY): the PTT a 2 s hold re-asserted every frame it is held,
    //    the LATCH edge-triggered, and a page that goes away lets go.
    // =====================================================================
    {
        using cascade::gui::TxKeyFrame;
        using cascade::gui::TxKeyMemory;
        using cascade::gui::txKeyCommands;
        FoxCommand c[cascade::gui::kTxKeyMaxCommands];
        auto is = [&](std::uint32_t n, std::initializer_list<std::pair<std::uint32_t, long long>> want) {
            if (n != want.size()) { return false; }
            std::uint32_t i = 0;
            for (const auto& w : want) {
                if (c[i].op != w.first || c[i].ival[0] != w.second) { return false; }
                ++i;
            }
            return true;
        };
        TxKeyMemory m;
        TxKeyFrame f;
        f.pageLive = true;
        // Nothing asked for, nothing sent.
        CHECK(is(txKeyCommands(f, m, c), {}));
        // The PTT: re-asserted every frame it is held, released once.
        f.pttHeld = true;
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_PTT, 1}}));
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_PTT, 1}}));
        f.pttHeld = false;
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_PTT, 0}}));
        CHECK(is(txKeyCommands(f, m, c), {}));
        // THE LATCH KEY PROTOCOL: a press on a key shown released sends LATCH 0
        // then LATCH 1 in one batch; on a key shown latched, LATCH 0.
        f.latchPressed = true;
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_LATCH, 0}, {FOXAPI_OP_TX_LATCH, 1}}));
        f.latchedShown = true;
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_LATCH, 0}}));
        f.latchPressed = false;
        // The remote-transmit switch.
        f.armRequest = 1;
        CHECK(is(txKeyCommands(f, m, c), {{FOXAPI_OP_TX_REMOTE_ARM, 1}}));
        f.armRequest = -1;
        // THE PAGE GOES AWAY holding the PTT, the latch (its own) and the
        // consent: all three let go, in one batch, once.
        f.pttHeld = true;
        f.latchMine = true;
        (void)txKeyCommands(f, m, c);
        TxKeyFrame gone;
        gone.latchedShown = true;
        gone.latchMine = true;
        CHECK(is(txKeyCommands(gone, m, c),
                 {{FOXAPI_OP_TX_PTT, 0}, {FOXAPI_OP_TX_LATCH, 0}, {FOXAPI_OP_TX_REMOTE_ARM, 0}}));
        CHECK(is(txKeyCommands(gone, m, c), {}));
        // A PRESS NOT YET LANDED, THEN THE PAGE GOES (both before any pass
        // has shown the latch): the close ends the pending press - LATCH 0
        // after the LATCH 1, so the pass applies both and never keys.
        {
            TxKeyMemory mp;
            TxKeyFrame press;
            press.pageLive = true;
            press.latchPressed = true;
            CHECK(is(txKeyCommands(press, mp, c), {{FOXAPI_OP_TX_LATCH, 0}, {FOXAPI_OP_TX_LATCH, 1}}));
            const TxKeyFrame closed;  // the state still shows nothing latched
            CHECK(is(txKeyCommands(closed, mp, c), {{FOXAPI_OP_TX_LATCH, 0}}));
            TxKeyFrame reopened;
            reopened.pageLive = true;
            CHECK(is(txKeyCommands(reopened, mp, c), {}));  // no press on the reopened page, nothing sent
            // Once the state has shown the latch as this window's, the press
            // is no longer pending: the close's release is the one for "mine".
            TxKeyMemory ml;
            (void)txKeyCommands(press, ml, c);
            TxKeyFrame shown;
            shown.pageLive = true;
            shown.latchedShown = true;
            shown.latchMine = true;
            CHECK(is(txKeyCommands(shown, ml, c), {}));
            CHECK(!ml.latchPending);
        }
        // A latch that is ANOTHER window's is not released by this page going.
        TxKeyMemory m2;
        f = TxKeyFrame{};
        f.pageLive = true;
        (void)txKeyCommands(f, m2, c);
        TxKeyFrame goneOther;
        goneOther.latchedShown = true;
        goneOther.latchMine = false;
        CHECK(is(txKeyCommands(goneOther, m2, c), {}));

        // The same scenarios against a REAL engine and transmitter, through a
        // LOCAL session, pumped as the window pumps it.
        const KeyOutcome fixed = runKeyScenarios([] { return fixedPage(); });
        printOutcome("frame loop now", fixed);
        CHECK(fixed.latchClosedPageReleases);
        CHECK(fixed.latchRolledUpPageReleases);
        CHECK(fixed.pttClosedPageReleases);
        CHECK(fixed.latchFailsafeStays);
        CHECK(fixed.frozenWindowStays);
        CHECK(fixed.pressesWork);

        // THE CONTROL: a page that sends its key as a level and only while it
        // is drawn fails both page-going scenarios - which proves they can see
        // a page that does not let go - and does the ordinary presses. The two
        // it now PASSES are the engine's doing, not the page's: API 0.2's rules
        // refuse the level page's LATCH 1 after a timeout or a frozen window
        // (its operator is marked until it sends LATCH 0 itself), where in
        // 0.99.34 the page re-latched the radio the frame after.
        const KeyOutcome before = runKeyScenarios([] { return levelPage(); });
        printOutcome("a level-triggered page", before);
        CHECK(!before.latchClosedPageReleases);
        CHECK(!before.pttClosedPageReleases);
        CHECK(before.latchFailsafeStays);
        CHECK(before.frozenWindowStays);
        CHECK(before.pressesWork);
    }

    // =====================================================================
    // 8. ...AND THE APPLICATION IS WIRED THAT WAY
    //
    //    The rule above is only true of the product if app_window.cpp submits
    //    the key once a frame, OUTSIDE the page, after the page has been drawn
    //    - and says the page is live only once beginPage has actually put its
    //    controls on screen (a rolled-up page returns false there) - and if
    //    nothing but the engine API's pass writes the key to the transmitter.
    //    Read from the source that ships.
    // =====================================================================
    {
        const std::filesystem::path src =
            std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
        const std::string text = readFile(src);
        CHECK(!text.empty());
        const std::string etext =
            readFile(std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "engine" / "engine.cpp");
        const std::string atext =
            readFile(std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "engine" / "fox_engine_api.cpp");
        CHECK(!etext.empty() && !atext.empty());

        // THE KEY IS WRITTEN IN ONE PLACE: the API host's pass (txPass), from
        // the rules, before the one tick. The window writes nothing to the
        // transmitter; the engine writes only key-UPs (the control thread's
        // stop).
        const std::vector<std::size_t> apiLatch = liveHits(atext, "tx.setLatched(rules_.latched());");
        const std::vector<std::size_t> apiPtt = liveHits(atext, "tx.setPttHeld(");
        const std::vector<std::size_t> apiTick = liveHits(atext, "tx.tick();");
        std::printf("wiring: the pass writes the latch x%zu and the PTT x%zu, ticks x%zu\n", apiLatch.size(),
                    apiPtt.size(), apiTick.size());
        CHECK(apiLatch.size() == 1u);
        CHECK(apiPtt.size() == 1u);
        CHECK(apiTick.size() == 1u);
        CHECK(!apiLatch.empty() && !apiTick.empty() && apiLatch[0] < apiTick[0]);
        CHECK(!apiPtt.empty() && !apiTick.empty() && apiPtt[0] < apiTick[0]);
        for (const std::size_t at : liveHits(etext, "transmitter_.setLatched(")) {
            CHECK(etext.compare(at, std::strlen("transmitter_.setLatched(false)"), "transmitter_.setLatched(false)") == 0);
        }
        for (const std::size_t at : liveHits(etext, "transmitter_.setPttHeld(")) {
            CHECK(etext.compare(at, std::strlen("transmitter_.setPttHeld(false)"), "transmitter_.setPttHeld(false)") ==
                  0);
        }
        CHECK(liveHits(etext, "transmitter_.tick();").empty());
        CHECK(liveHits(text, "transmitter_.setLatched(").empty());
        CHECK(liveHits(text, "transmitter_.setPttHeld(").empty());
        CHECK(liveHits(text, "transmitter_.keyRemote(").empty());
        CHECK(liveHits(text, "transmitter_.tick();").empty());

        // THE WINDOW'S HALF: pumpTransmitKey, once, in drawUi after the pages
        // are drawn (inside drawPluginWindows), never in the page body; and
        // it is the only caller of txKeyCommands.
        const std::vector<std::size_t> pumpCalls = liveHits(text, "    pumpTransmitKey();");
        const std::vector<std::size_t> ruleCalls = liveHits(text, "cascade::gui::txKeyCommands(");
        CHECK(pumpCalls.size() == 1u);
        CHECK(ruleCalls.size() == 1u);
        const std::size_t drawUi = text.find("void AppWindow::drawUi()");
        const std::size_t drawUiEnd = drawUi == std::string::npos ? std::string::npos : text.find("\n}", drawUi);
        const std::size_t pluginWindows = liveHits(text, "    drawPluginWindows();").empty()
                                              ? std::string::npos
                                              : liveHits(text, "    drawPluginWindows();")[0];
        const std::size_t pageBody = text.find("void AppWindow::drawTransmitPage()");
        const std::size_t pageEnd =
            pageBody == std::string::npos ? std::string::npos : text.find("\nvoid AppWindow::", pageBody + 1);
        const std::size_t heartbeat = text.find("if (uiSession_ != nullptr) { engineApi_->heartbeat(uiSession_); }");
        const bool anchors = pumpCalls.size() == 1u && drawUi != std::string::npos && drawUiEnd != std::string::npos &&
                             pluginWindows != std::string::npos && pageBody != std::string::npos &&
                             pageEnd != std::string::npos && heartbeat != std::string::npos;
        CHECK(anchors);
        if (anchors) {
            CHECK(drawUi < pluginWindows);
            CHECK(pluginWindows < pumpCalls[0]);
            CHECK(pumpCalls[0] < drawUiEnd);
            // The heartbeat is the frame's first act, inside drawUi.
            CHECK(drawUi < heartbeat && heartbeat < pluginWindows);
            CHECK(!(pumpCalls[0] > pageBody && pumpCalls[0] < pageEnd));
            // The page is live only AFTER beginPage has said it is drawing.
            const std::string page = text.substr(pageBody, pageEnd - pageBody);
            const std::size_t begin = page.find("if (!beginPage(\"Transmit###transmitwindow\"");
            const std::size_t live = page.find("transmitPageLive_ = true;");
            CHECK(begin != std::string::npos);
            CHECK(live != std::string::npos);
            CHECK(begin != std::string::npos && live != std::string::npos && begin < live);
            CHECK(!liveHits(text, "transmitPageLive_ = false;").empty());
        }
    }

    return testSummary("test_transmit_page");
}
