// test_airspy_app.cpp - the Airspy R2 / Mini controls in the REAL application
// (0.99.41): the gain mode the Source section's buttons choose, the web
// remote's gain by name, the decimation the combo chooses and the chain
// following it, and all of it put back at the next launch - AppWindow's own
// members, with a byte-exact fake radio behind the device hook, so no radio
// on the desk is ever enumerated or opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "airspy_fake_usb.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/airspy_panel.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "net/web_control.hpp"
#include "source/airspy_source.hpp"
#include "test_check.hpp"

namespace {

constexpr const char* kSerial = "644866c83f1a51df";
const std::string kArgs = std::string("serial=") + kSerial;
const std::string kKey = std::string("airspy|serial=") + kSerial;

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind != "airspy") { return nullptr; }
    auto src = std::make_unique<cascade::source::AirspySource>();
    cascade::usb::UsbDeviceInfo d;
    d.vid = 0x1D50;
    d.pid = 0x60A1;
    d.path = "\\\\?\\usb#vid_1d50&pid_60a1#fake#{a5dcbf10}";
    d.serial = kSerial;
    d.description = "AIRSPY";
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(
        std::make_unique<cascade::test::FakeAirspyUsb>());
    src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
        if (*holder == nullptr) { error = "fake: already handed out"; }
        return std::move(*holder);
    });
    return src;
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"airspy", "Airspy R2", kArgs}};
}

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

bool refused(const FoxCommandResult& r, std::int32_t status) {
    return r.status == status && (r.flags & FOXAPI_RESULT_REFUSED) != 0u;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

// The open radio's entry in a saved config - a default-constructed one (mode
// "free", decimation 1) when there is none, so a missing entry fails the
// check that reads it by name instead of throwing out of the test.
cascade::core::AirspySetting memOf(const cascade::core::AppConfig& c) {
    const auto it = c.airspy.find(kKey);
    if (it == c.airspy.end()) {
        std::printf("     no memory entry for %s\n", kKey.c_str());
        cascade::core::AirspySetting none;
        none.mode = "(none)";
        none.decimation = 0;
        return none;
    }
    return it->second;
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_airspy_app_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }
    static bool selectAirspy(AppWindow& a) {
        a.engine_.scanNative();
        for (std::size_t i = 0; i < a.engine_.nativeDevices_.size(); ++i) {
            if (a.engine_.nativeDevices_[i].args != kArgs) { continue; }
            a.engine_.selectSource(AppWindow::kNativeRowBase + static_cast<int>(i));
            const auto t0 = std::chrono::steady_clock::now();
            while (a.engine_.deviceOpenPending_) {
                a.engine_.pollSourceAsync();
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return asAirspy(a.engine_.device_) != nullptr;
        }
        return false;
    }
    static cascade::source::AirspySource* radio(AppWindow& a) { return asAirspy(a.engine_.device_); }
    static void selectGenerator(AppWindow& a) { a.engine_.selectSource(0); }
    static const std::vector<std::string>& gainNames(AppWindow& a) { return a.engine_.deviceGainNames_; }
    static const std::vector<float>& gainValues(AppWindow& a) { return a.engine_.deviceGainsDb_; }
    static const std::vector<double>& rates(AppWindow& a) { return a.engine_.deviceRatesHz_; }
    static const std::vector<std::string>& rateLabels(AppWindow& a) { return a.engine_.deviceRateLabels_; }
    static double chainRate(AppWindow& a) { return a.engine_.pipeline_.inputRateHz(); }
    static const std::string& sourceError(AppWindow& a) { return a.engine_.sourceError_; }
    static bool decimate(AppWindow& a, unsigned d) { return a.engine_.chooseAirspyDecimation(d); }
    static bool mode(AppWindow& a, cascade::source::AirspySource::GainMode m) {
        return a.engine_.chooseAirspyGainMode(m);
    }
    static bool agc(AppWindow& a, bool lna, bool on) { return a.engine_.chooseAirspyAgc(lna, on); }
    static bool agcMirror(AppWindow& a) { return a.engine_.deviceAgc_; }
    static void control(AppWindow& a, const cascade::net::ControlRequest& r) {
        a.engine_.applyControlRequest(r);
    }
    static cascade::core::AppConfig config(AppWindow& a) {
        a.engine_.publishConfig();   // the engine's frame hands its half over (OPEN 2)
        return a.currentConfig();
    }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
    // The Source panel's Airspy gain slider's own call (engine/stage3b-pre
    // M2, 2026-09-28): queued, exactly as ImGui::SliderFloat's edit does in
    // drawAirspyControls, then drained by the same pollSourceAsync poll a
    // real frame would run.
    static void sliderSetGain(AppWindow& a, const std::string& name, double db) {
        a.engine_.submitCommand(
            cascade::core::cmd::makeText(FOXAPI_OP_SET_GAIN, name, 0, 0, db));
        a.engine_.drainLocalCommands();
    }
    // The op the branch used by mistake (FOXAPP_OP_SET_GAIN_NO_READBACK, the
    // radar scope's knob): kept here only so this test can show what it
    // would have done, for contrast with the fix.
    static void sliderSetGainNoReadback(AppWindow& a, const std::string& name, double db) {
        a.engine_.submitCommand(
            cascade::core::cmd::makeText(FOXAPP_OP_SET_GAIN_NO_READBACK, name, 0, 0, db));
        a.engine_.drainLocalCommands();
    }
    // engine/stage3b-pre Airspy round (OPEN 2/3): the panel's commands, and
    // the published state drawAirspyControls now reads instead of the raw
    // device pointer above (`radio`, kept in this test ONLY for verification -
    // the application itself no longer holds onto it).
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) { return a.engine_.applyCommand(c); }
    static cascade::core::AppStateExt publishedAirspy(AppWindow& a) {
        (void)a.engine_.publishReceiverState();
        cascade::core::PublishedState s;
        if (!a.engine_.receiverSnapshot_->read(s)) { s = a.engine_.receiverSnapshot_->readFull()->state; }
        return s.app;
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Mode = cascade::source::AirspySource::GainMode;

int main() {
    std::printf("test_airspy_app\n");
    isolate();
    Access::installHooks();

    cascade::core::AppConfig saved;
    {
        cascade::gui::AppWindow app;
        CHECK(Access::selectAirspy(app));
        // --- as it opens: Free mode, its three stages, the board's rates -------
        CHECK(Access::gainNames(app) == std::vector<std::string>({"LNA", "MIXER", "VGA"}));
        CHECK(Access::rateLabels(app) ==
              std::vector<std::string>({"2.500 MS/s", "10.000 MS/s"}));
        std::printf("  opened: chain at %.0f S/s\n", Access::chainRate(app));
        CHECK(Access::chainRate(app) == Access::radio(app)->sampleRateHz());

        // --- the Linear button: ONE slider, and the memory says so -----------
        const cascade::core::AppConfig before = Access::config(app);
        CHECK(Access::mode(app, Mode::Linearity));
        CHECK(Access::gainNames(app) == std::vector<std::string>({"LINEARITY"}));
        CHECK(Access::gainValues(app).size() == 1 && Access::gainValues(app)[0] == 10.0f);
        const cascade::core::AppConfig afterMode = Access::config(app);
        CHECK(!cascade::gui::configsEqual(before, afterMode));  // the save notices
        CHECK(memOf(afterMode).mode == "linear");

        // --- the web remote names a Free-mode gain: Free mode, and the list
        //     and the memory follow ---------------------------------------------
        cascade::net::ControlRequest r;
        r.gainName = "LNA";
        r.gainDb = 5.0;
        Access::control(app, r);
        CHECK(Access::radio(app)->gainMode() == Mode::Free);
        CHECK(Access::gainNames(app) == std::vector<std::string>({"LNA", "MIXER", "VGA"}));
        CHECK(Access::gainValues(app).size() == 3 && Access::gainValues(app)[0] == 5.0f);
        CHECK(memOf(Access::config(app)).lna == 5);
        CHECK(memOf(Access::config(app)).mode == "free");

        // --- the Source panel's own Airspy slider: SET_GAIN, not
        //     NO_READBACK (engine/stage3b-pre M2). setGainDb rounds to the
        //     nearest whole step (airspy_source.cpp), so a request of 7.6
        //     is NOT what the radio ends up at - 8 is - and the mirror must
        //     read the radio back, not keep the raw drag value. -----------
        std::printf("  [RED, named] the op the branch used (NO_READBACK) keeps the raw request:\n");
        Access::sliderSetGainNoReadback(app, "LNA", 7.6);
        CHECK(Access::radio(app)->gainDb("LNA") == 8.0);      // the radio itself is correct
        CHECK(Access::gainValues(app)[0] == 7.6f);            // but the panel lies - the bug
        std::printf("      radio=%.1f, panel(NO_READBACK)=%.1f\n", Access::radio(app)->gainDb("LNA"),
                    Access::gainValues(app)[0]);

        std::printf("  [GREEN] SET_GAIN corrects it to the readback:\n");
        Access::sliderSetGain(app, "LNA", 7.6);
        CHECK(Access::radio(app)->gainDb("LNA") == 8.0);
        CHECK(Access::gainValues(app)[0] == 8.0f);            // the readback, not 7.6
        std::printf("      radio=%.1f, panel(SET_GAIN)=%.1f\n", Access::radio(app)->gainDb("LNA"),
                    Access::gainValues(app)[0]);

        // Put LNA back at the exact 5 dB the web-remote check above set and
        // the rest of this test (the save/relaunch checks further down)
        // depend on - an exact integer needs no rounding, so this is not
        // itself testing SET_GAIN, only restoring state for what follows.
        Access::sliderSetGain(app, "LNA", 5.0);
        CHECK(Access::gainValues(app)[0] == 5.0f);
        CHECK(memOf(Access::config(app)).lna == 5);

        // --- one AGC: the auto-gain mirror reads both-on only ------------------
        CHECK(Access::agc(app, false, true));
        CHECK(!Access::agcMirror(app));
        CHECK(memOf(Access::config(app)).mixerAgc);
        CHECK(Access::agc(app, false, false));

        // --- the decimation: the radio, the Rate combo, the chain -------------
        CHECK(Access::radio(app)->setSampleRateHz(10.0e6));
        CHECK(Access::decimate(app, 4));
        std::printf("  /4: chain at %.0f S/s, radio at %.0f\n", Access::chainRate(app),
                    Access::radio(app)->hardwareSampleRateHz());
        CHECK(Access::chainRate(app) == 2.5e6);
        CHECK(Access::radio(app)->hardwareSampleRateHz() == 10.0e6);
        CHECK(Access::rates(app) == std::vector<double>({625000.0, 2.5e6}));
        CHECK(Access::rateLabels(app) ==
              std::vector<std::string>({"625.000 kS/s", "2.500 MS/s"}));
        CHECK(memOf(Access::config(app)).decimation == 4);
        // A factor this board cannot do is refused, with the reason on the
        // panel, and nothing moves.
        CHECK(!Access::decimate(app, 64));
        CHECK(Access::sourceError(app).find("cannot decimate by 64") != std::string::npos);
        CHECK(Access::chainRate(app) == 2.5e6);
        saved = Access::config(app);
    }

    // --- a later launch: the same radio comes back as it was left ------------
    {
        cascade::gui::AppWindow app;
        Access::restore(app, saved);
        CHECK(Access::selectAirspy(app));
        cascade::source::AirspySource* a = Access::radio(app);
        CHECK(a != nullptr);
        if (a != nullptr) {
            std::printf("  relaunch: mode %d, lna %.0f, /%u, radio %.0f, chain %.0f\n",
                        static_cast<int>(a->gainMode()), a->gainDb("LNA"), a->decimation(),
                        a->hardwareSampleRateHz(), Access::chainRate(app));
            CHECK(a->gainMode() == Mode::Free);
            CHECK(a->gainDb("LNA") == 5.0);
            CHECK(a->gainDb("LINEARITY") == 10.0);
            CHECK(a->decimation() == 4);
            CHECK(Access::chainRate(app) == a->sampleRateHz());
            CHECK(Access::rateLabels(app).size() == 2 &&
                  Access::rateLabels(app)[0] == "625.000 kS/s");
        }
        CHECK(Access::gainNames(app) == std::vector<std::string>({"LNA", "MIXER", "VGA"}));
    }

    // --- engine/stage3b-pre Airspy round (OPEN 2/3): the COMMANDS
    //     drawAirspyControls now submits, and the PUBLISHED STATE it now
    //     reads instead of the raw device pointer. Its own fresh session -
    //     everything above already proved the underlying
    //     chooseAirspyDecimation/GainMode/Agc and the save/relaunch
    //     round-trip; this proves the command wrapper and the publish agree
    //     with the radio, without disturbing that round-trip's own state. ---
    {
        cascade::gui::AppWindow app;
        CHECK(Access::selectAirspy(app));
        namespace cmd = cascade::core::cmd;
        CHECK(Access::radio(app)->setSampleRateHz(2.5e6));
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_DECIMATION, 2))));
        cascade::core::AppStateExt e = Access::publishedAirspy(app);
        CHECK(e.airspyOpen);
        CHECK(e.airspyDecimation == Access::radio(app)->decimation());
        CHECK(e.airspyDecimation == 2u);
        // THE CHOICES THE COMBO LISTS (round 3 fix): drawAirspyControls reads
        // only these, so a publish that dropped them would leave an empty
        // Decimation list. Every one the radio offers, in its order.
        {
            const std::vector<unsigned> offered = Access::radio(app)->decimationChoices();
            CHECK(!offered.empty());
            CHECK(e.airspyDecimationChoiceCount == offered.size());
            for (std::size_t i = 0; i < offered.size() && i < e.airspyDecimationChoiceCount; ++i) {
                CHECK(e.airspyDecimationChoices[i] == offered[i]);
            }
            CHECK(e.airspyHardwareSampleRateHz == Access::radio(app)->hardwareSampleRateHz());
        }
        // An out-of-range refusal changes neither the radio nor the publish.
        CHECK(refused(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_DECIMATION, 64)), FOXAPI_FAILED));
        e = Access::publishedAirspy(app);
        CHECK(e.airspyDecimation == 2u);

        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_GAIN_MODE,
                                                 static_cast<std::int64_t>(Mode::Sensitivity)))));
        e = Access::publishedAirspy(app);
        CHECK(e.airspyGainMode == static_cast<std::uint32_t>(Mode::Sensitivity));
        CHECK(Access::radio(app)->gainMode() == Mode::Sensitivity);
        CHECK(refused(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_GAIN_MODE, 99)), FOXAPI_OUT_OF_RANGE));

        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_GAIN_MODE,
                                                 static_cast<std::int64_t>(Mode::Free)))));
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 0, 1))));  // LNA on
        e = Access::publishedAirspy(app);
        CHECK(e.airspyLnaAgc);
        CHECK(!e.airspyMixerAgc);
        CHECK(Access::agcMirror(app) == (e.airspyLnaAgc && e.airspyMixerAgc));
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 1, 1))));  // Mixer on too
        e = Access::publishedAirspy(app);
        CHECK(e.airspyLnaAgc);
        CHECK(e.airspyMixerAgc);
        CHECK(refused(Access::apply(app, cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 2, 1)), FOXAPI_OUT_OF_RANGE));
    }

    // --- A LAUNCH THAT RESTORES A DECIMATED RATE (the capture that found it):
    //     the saved 1.25 MS/s is the radio at 10 MS/s under /8, and the
    //     remembered decimation must be on the radio BEFORE that rate is asked
    //     for - asked first, it was matched against 2.5 and 10 MS/s, "coerced"
    //     to 2.5, and the panel kept a red line about it even after the
    //     decimation had put the rate right. Both open paths: the startup
    //     restore (synchronous) and a pick from the Source list (the worker).
    {
        cascade::core::AppConfig cfg = saved;
        cfg.sourceKind = "airspy";
        cfg.nativeArgs = kArgs;
        cfg.sampleRateHz = 1.25e6;
        cascade::core::AirspySetting s;
        s.mode = "linear";
        s.linearity = 15;
        s.decimation = 8;
        cfg.airspy[kKey] = s;
        cascade::gui::AppWindow app;
        Access::restore(app, cfg);
        cascade::source::AirspySource* a = Access::radio(app);
        CHECK(a != nullptr);
        if (a != nullptr) {
            std::printf("  restore at 1.25 MS/s under /8: radio %.0f, chain %.0f, error \"%s\"\n",
                        a->hardwareSampleRateHz(), Access::chainRate(app),
                        Access::sourceError(app).c_str());
            CHECK(a->decimation() == 8);
            CHECK(a->hardwareSampleRateHz() == 10.0e6);
            CHECK(Access::chainRate(app) == 1.25e6);
            CHECK(Access::sourceError(app).empty());
        }
        // The worker path: the list pick asks for the rate the chain runs at.
        Access::selectGenerator(app);
        CHECK(Access::selectAirspy(app));
        a = Access::radio(app);
        CHECK(a != nullptr);
        if (a != nullptr) {
            std::printf("  list pick: radio %.0f /%u, chain %.0f, error \"%s\"\n",
                        a->hardwareSampleRateHz(), a->decimation(), Access::chainRate(app),
                        Access::sourceError(app).c_str());
            CHECK(a->decimation() == 8);
            CHECK(Access::chainRate(app) == a->sampleRateHz());
            // The pick asks for the generator's 2 MS/s, which no Airspy rate
            // is, so a coercion line is right - but it must name a DECIMATED
            // rate (1.25), proving /8 was on the radio before the request.
            // Before the fix it named the undecimated 2.5.
            CHECK(Access::sourceError(app).find("2.5 MS/s") == std::string::npos);
            CHECK(Access::sourceError(app).find("1.25 MS/s") != std::string::npos);
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_airspy_app");
}
