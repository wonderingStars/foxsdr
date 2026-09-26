// test_airspy_app.cpp - the Airspy R2 / Mini controls in the REAL application
// (0.99.40): the gain mode the Source section's buttons choose, the web
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
#include "core/config.hpp"
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
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }
    static bool selectAirspy(AppWindow& a) {
        a.scanNative();
        for (std::size_t i = 0; i < a.nativeDevices_.size(); ++i) {
            if (a.nativeDevices_[i].args != kArgs) { continue; }
            a.selectSource(AppWindow::kNativeRowBase + static_cast<int>(i));
            const auto t0 = std::chrono::steady_clock::now();
            while (a.deviceOpenPending_) {
                a.pollSourceAsync();
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return asAirspy(a.device_) != nullptr;
        }
        return false;
    }
    static cascade::source::AirspySource* radio(AppWindow& a) { return asAirspy(a.device_); }
    static void selectGenerator(AppWindow& a) { a.selectSource(0); }
    static const std::vector<std::string>& gainNames(AppWindow& a) { return a.deviceGainNames_; }
    static const std::vector<float>& gainValues(AppWindow& a) { return a.deviceGainsDb_; }
    static const std::vector<double>& rates(AppWindow& a) { return a.deviceRatesHz_; }
    static const std::vector<std::string>& rateLabels(AppWindow& a) { return a.deviceRateLabels_; }
    static double chainRate(AppWindow& a) { return a.pipeline_.inputRateHz(); }
    static const std::string& sourceError(AppWindow& a) { return a.sourceError_; }
    static bool decimate(AppWindow& a, unsigned d) { return a.chooseAirspyDecimation(d); }
    static bool mode(AppWindow& a, cascade::source::AirspySource::GainMode m) {
        return a.chooseAirspyGainMode(m);
    }
    static bool agc(AppWindow& a, bool lna, bool on) { return a.chooseAirspyAgc(lna, on); }
    static bool agcMirror(AppWindow& a) { return a.deviceAgc_; }
    static void control(AppWindow& a, const cascade::net::ControlRequest& r) {
        a.applyControlRequest(r);
    }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
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
