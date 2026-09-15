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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "gui/transmit_page.hpp"
#include "test_check.hpp"

using cascade::gui::TxLamp;

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

    return testSummary("test_transmit_page");
}
