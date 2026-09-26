// test_airspy_panel.cpp - the Airspy R2 / Mini memory (0.99.41): what the
// Source section writes when the user changes the gain mode, a gain, an AGC
// or the decimation, and what it puts back when that radio opens again -
// against the byte-exact fake radio, so "put back" is read off the USB
// transcript and the driver's own readbacks, not off the memory itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "airspy_fake_usb.hpp"
#include "core/airspy_settings.hpp"
#include "core/pipeline.hpp"
#include "gui/airspy_panel.hpp"
#include "source/airspy_source.hpp"
#include "test_check.hpp"

using cascade::core::AirspySetting;
using cascade::source::AirspySource;
using cascade::test::FakeAirspyUsb;

namespace {

constexpr const char* kSerial = "26a464dc28593e93";

FakeAirspyUsb* attach(AirspySource& src, std::vector<std::uint32_t> rates = {10000000u, 2500000u}) {
    cascade::usb::UsbDeviceInfo d;
    d.vid = cascade::source::airspy::kUsbVid;
    d.pid = cascade::source::airspy::kUsbPid;
    d.path = "\\\\?\\usb#vid_1d50&pid_60a1#fake";
    d.serial = kSerial;
    d.description = "AIRSPY";
    auto owned = std::make_unique<FakeAirspyUsb>();
    owned->sampleRates = std::move(rates);
    FakeAirspyUsb* raw = owned.get();
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
    src.setTransportForTest({d}, [holder](const std::string&, std::string& error) {
        if (*holder == nullptr) { error = "fake: already handed out"; }
        return std::move(*holder);
    });
    return raw;
}

}  // namespace

int main() {
    const std::string args = std::string("serial=") + kSerial;
    std::map<std::string, AirspySetting> memory;

    // --- the key: the bias tee's, so one radio is one entry ------------------
    CHECK(cascade::core::airspyRadioKey(args) == "airspy|serial=26a464dc28593e93");
    CHECK(cascade::core::airspyRadioKey("index=0") == "airspy|index=0");

    // --- a radio nothing is remembered for opens as its driver opens it -------
    {
        AirspySource a;
        FakeAirspyUsb* fake = attach(a);
        CHECK(a.open(args));
        fake->clearControls();
        CHECK(!cascade::gui::airspyApplyRemembered(memory, args, a));
        CHECK(fake->controlCount() == 0);
        CHECK(a.gainMode() == AirspySource::GainMode::Free);
        CHECK(a.decimation() == 1);

        // THE USER CHANGES THINGS, and each change is remembered as the radio
        // now reads back.
        CHECK(a.setDecimation(8));
        CHECK(a.setGainDb("LINEARITY", 17.0));
        CHECK(a.setGainDb("SENSITIVITY", 4.0));
        CHECK(a.setGainMode(AirspySource::GainMode::Linearity));
        cascade::gui::airspyRemember(memory, args, a);
        a.closeDevice();
    }
    {
        const auto it = memory.find("airspy|serial=26a464dc28593e93");
        CHECK(it != memory.end());
        if (it != memory.end()) {
            CHECK(it->second.mode == "linear");
            CHECK(it->second.linearity == 17);
            CHECK(it->second.sensitivity == 4);
            CHECK(it->second.lna == 8 && it->second.mixer == 8 && it->second.vga == 8);
            CHECK(it->second.decimation == 8);
        }
    }

    // --- the same radio opens again: everything is back, programmed ONCE ----
    {
        AirspySource a;
        FakeAirspyUsb* fake = attach(a);
        CHECK(a.open(args));
        fake->clearControls();
        CHECK(cascade::gui::airspyApplyRemembered(memory, args, a));
        CHECK(a.decimation() == 8);
        CHECK(a.gainMode() == AirspySource::GainMode::Linearity);
        CHECK(a.gainDb("LINEARITY") == 17.0);
        CHECK(a.gainDb("SENSITIVITY") == 4.0);
        // Decimation is this end's arithmetic (no transfer); the gain state
        // is ONE linearity programming: both AGCs off, VGA, MIXER, LNA - not
        // the three modes stepped through in turn.
        const auto c = fake->controls();
        CHECK(c.size() == 5);
        // The saved rate is a DECIMATED one: 1.25 MS/s is the radio at 10 MS/s
        // under /8 (the application asks for it again after the decimation is
        // back, and nearest-matching it against 312.5 kS/s and 1.25 MS/s
        // lands on the radio's 10 MS/s).
        CHECK(a.setSampleRateHz(1.25e6));
        CHECK(a.hardwareSampleRateHz() == 10.0e6);
        CHECK(a.sampleRateHz() == 1.25e6);
        // The combo's rows for those rates.
        const std::vector<double> rates = a.supportedSampleRatesHz();
        CHECK(rates.size() == 2);
        if (rates.size() == 2) {
            CHECK(cascade::gui::airspyRateLabel(rates[0]) == "312.500 kS/s");
            CHECK(cascade::gui::airspyRateLabel(rates[1]) == "1.250 MS/s");
        }
        CHECK(cascade::gui::airspyRateLabel(10.0e6) == "10.000 MS/s");
        CHECK(cascade::gui::airspyRateLabel(78125.0) == "78.125 kS/s");
        a.closeDevice();
    }

    // --- Free mode with one AGC comes back with that AGC on ----------------
    {
        AirspySource a;
        attach(a);
        CHECK(a.open(args));
        CHECK(a.setGainDb("VGA", 12.0));
        CHECK(a.setMixerAgc(true));
        CHECK(a.setDecimation(1));
        cascade::gui::airspyRemember(memory, args, a);
        a.closeDevice();

        AirspySource b;
        FakeAirspyUsb* fake = attach(b);
        CHECK(b.open(args));
        fake->clearControls();
        CHECK(cascade::gui::airspyApplyRemembered(memory, args, b));
        CHECK(b.gainMode() == AirspySource::GainMode::Free);
        CHECK(b.mixerAgc() && !b.lnaAgc());
        CHECK(b.decimation() == 1);
        const auto c = fake->controls();
        // Mixer AGC on, LNA AGC off, VGA 12, LNA 8 - and NO mixer gain,
        // because the mixer's AGC has it.
        CHECK(c.size() == 4);
        if (c.size() == 4) {
            CHECK(c[0].request == 18 && c[0].index == 1);
            CHECK(c[1].request == 17 && c[1].index == 0);
            CHECK(c[2].request == 16 && c[2].index == 12);
            CHECK(c[3].request == 14 && c[3].index == 8);
        }
        b.closeDevice();
    }

    // --- a remembered decimation this board cannot do is skipped, the rest
    //     still goes back (a Mini's 64 on an R2) ------------------------------
    {
        std::map<std::string, AirspySetting> m;
        AirspySetting s;
        s.mode = "sensitive";
        s.sensitivity = 19;
        s.decimation = 64;
        m[cascade::core::airspyRadioKey(args)] = s;
        AirspySource a;
        attach(a);
        CHECK(a.open(args));
        CHECK(cascade::gui::airspyApplyRemembered(m, args, a));
        CHECK(a.decimation() == 1);
        CHECK(a.gainMode() == AirspySource::GainMode::Sensitivity);
        CHECK(a.gainDb("SENSITIVITY") == 19.0);
        a.closeDevice();
    }

    // --- the cap: a new radio past it is not remembered, a known one is ------
    {
        std::map<std::string, AirspySetting> m;
        for (std::size_t i = 0; i < cascade::core::kMaxAirspyRadios; ++i) {
            m["airspy|serial=" + std::to_string(i)] = AirspySetting{};
        }
        AirspySource a;
        attach(a);
        CHECK(a.open(args));
        CHECK(a.setDecimation(4));
        cascade::gui::airspyRemember(m, args, a);
        CHECK(m.size() == cascade::core::kMaxAirspyRadios);
        CHECK(m.count(cascade::core::airspyRadioKey(args)) == 0);
        m.erase(m.begin());
        cascade::gui::airspyRemember(m, args, a);
        CHECK(m.count(cascade::core::airspyRadioKey(args)) == 1);
        m[cascade::core::airspyRadioKey(args)].decimation = 2;
        m["airspy|serial=extra"] = AirspySetting{};  // back at the cap
        cascade::gui::airspyRemember(m, args, a);    // known: still written
        CHECK(m[cascade::core::airspyRadioKey(args)].decimation == 4);
        a.closeDevice();
    }

    // --- EVERY RATE A DECIMATION CAN DELIVER IS ONE THE RECEIVER RUNS AT -----
    // The chain needs a whole-hertz channel rate; a factor that produced a
    // rate it refuses would leave the radio streaming at one rate and the
    // spectrum, the demodulator and every decoder on another. So for an R2
    // (10 and 2.5 MS/s) and a Mini (6 and 3 MS/s), every rate the driver
    // lists under every factor it offers is put to the real pipeline.
    {
        cascade::core::Pipeline::Config pc;
        pc.sampleRateHz = 2.0e6;
        pc.fftSize = 1024;
        pc.audioEnabled = false;
        cascade::core::Pipeline pipe(pc);
        int tried = 0;
        for (const std::vector<std::uint32_t>& board :
             {std::vector<std::uint32_t>{10000000u, 2500000u},
              std::vector<std::uint32_t>{6000000u, 3000000u}}) {
            AirspySource a;
            attach(a, board);
            CHECK(a.open(args));
            for (const unsigned d : a.decimationChoices()) {
                CHECK(a.setDecimation(d));
                for (const double r : a.supportedSampleRatesHz()) {
                    ++tried;
                    const bool ok = pipe.setInputRateHz(r);
                    if (!ok) { std::printf("     the chain refuses %.1f S/s (/%u)\n", r, d); }
                    CHECK(ok);
                }
            }
            a.closeDevice();
        }
        std::printf("decimated rates the chain accepts: %d of %d tried\n", tried, tried);
        CHECK(tried == 2 * 6 + 2 * 7);
    }

    return testSummary("test_airspy_panel");
}
