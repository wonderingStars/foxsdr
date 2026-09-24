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

using FrameFn = std::function<void(Transmitter&, const PageFrame&)>;

// THE FRAME LOOP AS IT IS WIRED NOW: txPageKey, applied every frame whether or
// not the page was drawn, then tick() - the order drawUi runs them in.
void fixedFrame(Transmitter& tx, const PageFrame& f) {
    const cascade::gui::TxPageKey k =
        cascade::gui::txPageKey(f.pageLive, tx.latched(), f.latchPressed, f.pttHeld);
    tx.setLatched(k.latched);
    tx.setPttHeld(k.pttHeld);
    tx.tick();
}

// THE FRAME LOOP AS IT WAS WIRED IN 0.99.34, copied from app_window.cpp so
// the scenarios below can prove they SEE the defect: the page remembered its
// own latch, wrote it and read it straight back, and did all of it only when
// the page body was drawn. Kept as a control, not as behaviour anybody wants.
struct PreFixPage {
    bool latched = false;  // transmitLatched_
    void frame(Transmitter& tx, const PageFrame& f) {
        if (f.pageLive) {
            if (f.latchPressed) { latched = !latched; }
            tx.setLatched(latched);
            latched = tx.latched();
            tx.setPttHeld(f.pttHeld);
        }
        tx.tick();
    }
};

void frames(Transmitter& tx, const FrameFn& fn, const PageFrame& f, int n) {
    for (int i = 0; i < n; ++i) {
        fn(tx, f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct KeyOutcome {
    bool latchClosedPageReleases = false;    // LATCH on, page closed
    bool latchRolledUpPageReleases = false;  // LATCH on, page rolled up (same input)
    bool pttClosedPageReleases = false;      // PTT down, page closed
    bool latchFailsafeStays = false;         // the minute passes: no re-latch
    bool frozenWindowStays = false;          // dead-man's handle: no re-key on thaw
    bool pressesWork = false;                // LATCH keys and unkeys; PTT keys
};

// Every scenario on its own transmitter, radio AND PAGE (makePage is called
// afresh for each, because a page that remembers its latch must not carry one
// scenario's into the next), driven only through that page's frame.
KeyOutcome runKeyScenarios(const std::function<FrameFn()>& makePage) {
    KeyOutcome o;
    const PageFrame idleLive{true, false, false};
    const PageFrame pressLatch{true, true, false};
    const PageFrame holdPtt{true, false, true};
    const PageFrame notDrawn{false, false, false};

    // Latched, then the page stops being drawn (closed, or rolled up - both
    // arrive here as a frame in which the page body did not run).
    for (int variant = 0; variant < 2; ++variant) {
        Transmitter tx;
        const FrameFn fn = makePage();
        auto s = std::make_unique<CountingSink>();
        tx.setSink(std::move(s));
        tx.setInput(cascade::core::TxInput::Tone);
        frames(tx, fn, pressLatch, 1);
        const bool keyed = tx.transmitting();
        frames(tx, fn, idleLive, 5);
        frames(tx, fn, notDrawn, 1);
        const bool released = keyed && !tx.transmitting() && !tx.latched();
        frames(tx, fn, notDrawn, 20);
        (variant == 0 ? o.latchClosedPageReleases : o.latchRolledUpPageReleases) =
            released && !tx.transmitting();
    }

    // The PTT held down at the moment the page goes.
    {
        Transmitter tx;
        const FrameFn fn = makePage();
        tx.setSink(std::make_unique<CountingSink>());
        tx.setInput(cascade::core::TxInput::Tone);
        frames(tx, fn, holdPtt, 5);
        const bool keyed = tx.transmitting();
        frames(tx, fn, notDrawn, 1);
        const bool released = keyed && !tx.transmitting() && !tx.pttHeld();
        frames(tx, fn, notDrawn, 20);
        o.pttClosedPageReleases = released && !tx.transmitting();
    }

    // The latch's own failsafe, with the page open and nobody touching it.
    {
        Transmitter tx;
        const FrameFn fn = makePage();
        auto s = std::make_unique<CountingSink>();
        CountingSink* raw = s.get();
        tx.setSink(std::move(s));
        tx.setInput(cascade::core::TxInput::Tone);
        tx.setLatchTimeoutForTest(std::chrono::milliseconds(150));
        frames(tx, fn, pressLatch, 1);
        const bool keyed = tx.transmitting();
        // Well past the deadline, one frame at a time the way drawUi runs.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < until) { frames(tx, fn, idleLive, 1); }
        std::printf("  latch failsafe: keyed %d, starts %d, transmitting %d, latched %d\n",
                    keyed ? 1 : 0, raw->starts.load(), tx.transmitting() ? 1 : 0,
                    tx.latched() ? 1 : 0);
        o.latchFailsafeStays =
            keyed && raw->starts.load() == 1 && !tx.transmitting() && !tx.latched();
    }

    // The frozen window: nothing calls anything for longer than the handle,
    // then the frames come back with the page open and nobody touching it.
    {
        Transmitter tx;
        const FrameFn fn = makePage();
        auto s = std::make_unique<CountingSink>();
        CountingSink* raw = s.get();
        tx.setSink(std::move(s));
        tx.setInput(cascade::core::TxInput::Tone);
        frames(tx, fn, pressLatch, 1);
        const bool keyed = tx.transmitting();
        std::this_thread::sleep_for(Transmitter::kKeyAliveWait + std::chrono::milliseconds(300));
        frames(tx, fn, idleLive, 20);
        std::printf("  frozen window: keyed %d, starts %d, transmitting %d, latched %d\n",
                    keyed ? 1 : 0, raw->starts.load(), tx.transmitting() ? 1 : 0,
                    tx.latched() ? 1 : 0);
        o.frozenWindowStays =
            keyed && raw->starts.load() == 1 && !tx.transmitting() && !tx.latched();
    }

    // And the controls still do what they are for.
    {
        Transmitter tx;
        const FrameFn fn = makePage();
        tx.setSink(std::make_unique<CountingSink>());
        tx.setInput(cascade::core::TxInput::Tone);
        frames(tx, fn, idleLive, 3);
        const bool quietAtRest = !tx.transmitting();
        frames(tx, fn, pressLatch, 1);
        frames(tx, fn, idleLive, 5);
        const bool latchKeys = tx.transmitting() && tx.latched();
        frames(tx, fn, pressLatch, 1);
        const bool latchUnkeys = !tx.transmitting() && !tx.latched();
        frames(tx, fn, holdPtt, 5);
        const bool pttKeys = tx.transmitting();
        frames(tx, fn, idleLive, 1);
        const bool pttReleases = !tx.transmitting();
        o.pressesWork = quietAtRest && latchKeys && latchUnkeys && pttKeys && pttReleases;
    }
    return o;
}

void printOutcome(const char* who, const KeyOutcome& o) {
    std::printf("%s: latch+closed releases %d, latch+rolled-up releases %d, ptt+closed "
                "releases %d, failsafe stays %d, frozen window stays %d, presses work %d\n",
                who, o.latchClosedPageReleases ? 1 : 0, o.latchRolledUpPageReleases ? 1 : 0,
                o.pttClosedPageReleases ? 1 : 0, o.latchFailsafeStays ? 1 : 0,
                o.frozenWindowStays ? 1 : 0, o.pressesWork ? 1 : 0);
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
    // 7. THE KEY: WHAT THE PAGE ASKS FOR, FRAME BY FRAME
    //
    //    Until 0.99.35 the page wrote its key to the transmitter from inside
    //    its own body, so a page that was closed or rolled up simply stopped
    //    writing: a LATCH stayed closed for up to a minute with no control on
    //    screen, and a PTT held at that moment stayed held for good. And the
    //    page wrote its REMEMBERED latch back every frame, so the latch's own
    //    failsafe and the frozen-window handle each released the radio for
    //    exactly one frame before the page re-latched it.
    // =====================================================================
    {
        // The rule itself.
        using cascade::gui::txPageKey;
        // No page, no key - whatever the transmitter or the hand is doing.
        CHECK(!txPageKey(false, true, false, true).latched);
        CHECK(!txPageKey(false, true, false, true).pttHeld);
        CHECK(!txPageKey(false, false, true, false).latched);
        // The transmitter's latch is the truth; the page only sends presses.
        CHECK(txPageKey(true, true, false, false).latched);
        CHECK(!txPageKey(true, false, false, false).latched);
        CHECK(txPageKey(true, false, true, false).latched);
        CHECK(!txPageKey(true, true, true, false).latched);
        CHECK(txPageKey(true, false, false, true).pttHeld);
        CHECK(!txPageKey(true, false, false, false).pttHeld);

        // The same scenarios against a real transmitter, through the frame
        // loop as it is wired now and as it was wired before.
        const KeyOutcome fixed = runKeyScenarios([] { return FrameFn(fixedFrame); });
        printOutcome("frame loop now", fixed);
        CHECK(fixed.latchClosedPageReleases);
        CHECK(fixed.latchRolledUpPageReleases);
        CHECK(fixed.pttClosedPageReleases);
        CHECK(fixed.latchFailsafeStays);
        CHECK(fixed.frozenWindowStays);
        CHECK(fixed.pressesWork);

        // THE CONTROL: the 0.99.34 wiring fails every one of the defect
        // scenarios, which is what proves they can see the defect at all -
        // and passes the ordinary one, so it is the same page being driven.
        const KeyOutcome before = runKeyScenarios([] {
            auto page = std::make_shared<PreFixPage>();
            return FrameFn([page](Transmitter& tx, const PageFrame& f) { page->frame(tx, f); });
        });
        printOutcome("0.99.34 wiring", before);
        CHECK(!before.latchClosedPageReleases);
        CHECK(!before.pttClosedPageReleases);
        CHECK(!before.latchFailsafeStays);
        CHECK(!before.frozenWindowStays);
        CHECK(before.pressesWork);
    }

    // =====================================================================
    // 8. ...AND THE APPLICATION IS WIRED THAT WAY
    //
    //    The rule above is only true of the product if app_window.cpp applies
    //    it once a frame, outside the page, between the page being drawn and
    //    the transmitter being ticked - and says the page is live only once
    //    beginPage has actually put its controls on screen (a rolled-up page
    //    returns false there). Read from the source that ships, the device
    //    tests/test_shutdown_budget.cpp uses for its teardown ordering.
    // =====================================================================
    {
        const std::filesystem::path src =
            std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
        const std::string text = readFile(src);
        CHECK(!text.empty());

        // Exactly one place writes the key into the transmitter...
        const std::vector<std::size_t> latchSets = liveHits(text, "transmitter_.setLatched(");
        const std::vector<std::size_t> pttSets = liveHits(text, "transmitter_.setPttHeld(");
        const std::vector<std::size_t> keyCalls = liveHits(text, "cascade::gui::txPageKey(");
        const std::vector<std::size_t> ticks = liveHits(text, "transmitter_.tick();");
        std::printf("wiring: setLatched x%zu, setPttHeld x%zu, txPageKey x%zu, tick x%zu\n",
                    latchSets.size(), pttSets.size(), keyCalls.size(), ticks.size());
        CHECK(latchSets.size() == 1u);
        CHECK(pttSets.size() == 1u);
        CHECK(keyCalls.size() == 1u);
        CHECK(ticks.size() == 1u);

        const std::size_t drawUi = text.find("void AppWindow::drawUi()");
        const std::size_t pluginWindows = liveHits(text, "    drawPluginWindows();").empty()
                                              ? std::string::npos
                                              : liveHits(text, "    drawPluginWindows();")[0];
        const std::size_t pageBody = text.find("void AppWindow::drawTransmitPage()");
        const std::size_t pageEnd =
            pageBody == std::string::npos ? std::string::npos
                                          : text.find("\nvoid AppWindow::", pageBody + 1);
        const bool anchors = latchSets.size() == 1u && pttSets.size() == 1u &&
                             keyCalls.size() == 1u && ticks.size() == 1u &&
                             drawUi != std::string::npos && pluginWindows != std::string::npos &&
                             pageBody != std::string::npos && pageEnd != std::string::npos;
        CHECK(anchors);
        if (anchors) {
            // ...in the frame loop, after the page has been drawn (it is drawn
            // inside drawPluginWindows) and before the tick that acts on it.
            CHECK(drawUi < pluginWindows);
            CHECK(pluginWindows < keyCalls[0]);
            CHECK(keyCalls[0] < latchSets[0]);
            CHECK(keyCalls[0] < pttSets[0]);
            CHECK(latchSets[0] < ticks[0]);
            CHECK(pttSets[0] < ticks[0]);
            // NOT in the page body, which is the one place that stops running
            // exactly when the key most needs releasing.
            CHECK(!(latchSets[0] > pageBody && latchSets[0] < pageEnd));
            CHECK(!(pttSets[0] > pageBody && pttSets[0] < pageEnd));

            // The page is live only AFTER beginPage has said it is drawing -
            // the live flag set before that would count a rolled-up page.
            const std::string page = text.substr(pageBody, pageEnd - pageBody);
            const std::size_t begin = page.find("if (!beginPage(\"Transmit###transmitwindow\"");
            const std::size_t live = page.find("transmitPageLive_ = true;");
            CHECK(begin != std::string::npos);
            CHECK(live != std::string::npos);
            CHECK(begin != std::string::npos && live != std::string::npos && begin < live);
            // And the flag starts every frame false, beside the PTT's own reset.
            CHECK(!liveHits(text, "transmitPageLive_ = false;").empty());
        }
    }

    return testSummary("test_transmit_page");
}
