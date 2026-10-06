// test_hydrasdr_app.cpp - the HydraSDR RFOne in the REAL application: the Source
// list row it appears under, the open that follows, the panel it is drawn with
// (it is an Airspy driver with a HydraSDR's profile, so the Airspy panel and its
// per-radio memory apply), the config that brings it back, the web remote that
// may name it, the patch page's one-radio rule, and the two anonymous counts it
// makes (radio_open.hydrasdr, radio_fail.hydrasdr.<why>) - AppWindow's own
// members, with a byte-exact fake radio behind the device hook, so no radio on
// the desk is ever enumerated or opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "core/config.hpp"
#include "core/health_events.hpp"
#include "core/patch_devices.hpp"
#include "core/telemetry.hpp"
#include "gui/app_window.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/tune_control.hpp"
#include "hydrasdr_fake_usb.hpp"
#include "net/web_control.hpp"
#include "source/airspy_source.hpp"
#include "source/hydrasdr_source.hpp"
#include "test_check.hpp"

namespace {

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using health::Counts;

constexpr const char* kSerial = "HYDRASDR_SN:0123456789ABCDEF";
const std::string kArgs = std::string("serial=") + kSerial;
const std::string kKey = std::string("hydrasdr|serial=") + kSerial;

// When set, the device hook hands out a source with NO fake behind it, so the
// open goes to this machine's real USB enumeration and meets a serial that is
// not there - the driver's own refusal, counted by the path that opened it.
std::atomic<bool> g_real{false};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind != "hydrasdr") { return nullptr; }
    auto src = std::make_unique<cascade::source::HydraSdrSource>();
    if (g_real.load()) { return src; }
    cascade::usb::UsbDeviceInfo d;
    d.vid = 0x38AF;
    d.pid = 0x0001;
    d.path = "\\\\?\\usb#vid_38af&pid_0001#fake#{a5dcbf10}";
    d.serial = kSerial;
    d.description = "HydraSDR RFOne";
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(
        std::make_unique<cascade::test::FakeHydraSdrUsb>());
    src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
        if (*holder == nullptr) { error = "fake: already handed out"; }
        return std::move(*holder);
    });
    return src;
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    if (g_real.load()) {
        return {{"hydrasdr", "HydraSDR RFOne", "serial=HEALTHTESTNOSUCHRADIO"}};
    }
    return {{"hydrasdr", "HydraSDR RFOne", kArgs}};
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_hydrasdr_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::shared_ptr<health::HealthLedger> fresh() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    return g;
}

cascade::core::AirspySetting memOf(const cascade::core::AppConfig& c, const std::string& key) {
    const auto it = c.airspy.find(key);
    if (it == c.airspy.end()) {
        std::printf("     no memory entry for %s\n", key.c_str());
        cascade::core::AirspySetting none;
        none.mode = "(none)";
        none.decimation = 0;
        return none;
    }
    return it->second;
}

bool writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
    return static_cast<bool>(f);
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }
    // Picks the one native row and waits for the worker's answer.
    static bool select(AppWindow& a) {
        a.scanNative();
        for (std::size_t i = 0; i < a.nativeDevices_.size(); ++i) {
            if (a.nativeDevices_[i].driver != "hydrasdr") { continue; }
            a.selectSource(AppWindow::kNativeRowBase + static_cast<int>(i));
            const auto t0 = std::chrono::steady_clock::now();
            while (a.deviceOpenPending_) {
                a.pollSourceAsync();
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return a.device_ != nullptr;
        }
        return false;
    }
    static cascade::source::AirspySource* radio(AppWindow& a) { return asAirspy(a.device_); }
    static cascade::source::DeviceSource* device(AppWindow& a) { return a.device_; }
    static const std::vector<std::string>& gainNames(AppWindow& a) { return a.deviceGainNames_; }
    static const std::vector<std::string>& antennas(AppWindow& a) { return a.deviceAntennas_; }
    static const std::vector<std::string>& rateLabels(AppWindow& a) { return a.deviceRateLabels_; }
    static const std::string& sourceError(AppWindow& a) { return a.sourceError_; }
    static const std::string& sourceKind(AppWindow& a) { return a.sourceKind_; }
    static bool mode(AppWindow& a, cascade::source::AirspySource::GainMode m) {
        return a.chooseAirspyGainMode(m);
    }
    static bool decimate(AppWindow& a, unsigned d) { return a.chooseAirspyDecimation(d); }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Mode = cascade::source::AirspySource::GainMode;

int main() {
    std::printf("test_hydrasdr_app\n");
    isolate();
    Access::installHooks();

    // --- the Source list row, and the open it leads to -------------------------
    cascade::core::AppConfig saved;
    {
        auto g = fresh();
        cascade::gui::AppWindow app;
        CHECK(Access::select(app));
        cascade::source::DeviceSource* dev = Access::device(app);
        CHECK(dev != nullptr);
        CHECK(dev != nullptr && std::string(dev->driverKey()) == "hydrasdr");
        CHECK(Access::sourceKind(app) == "hydrasdr");
        // THE AIRSPY PANEL APPLIES: it is an Airspy driver with a HydraSDR's
        // profile, so asAirspy() answers and the gain-mode panel is drawn.
        CHECK(Access::radio(app) != nullptr);
        CHECK(Access::gainNames(app) == std::vector<std::string>({"LNA", "MIXER", "VGA"}));
        // THE DEVICE'S OWN THREE RATES, in the Rate combo's words.
        CHECK(Access::rateLabels(app) ==
              std::vector<std::string>({"2.500 MS/s", "5.000 MS/s", "10.000 MS/s"}));
        // THE THREE RECEIVE PORTS, offered by the generic antenna combo.
        CHECK(Access::antennas(app) == std::vector<std::string>({"ANT", "CABLE1", "CABLE2"}));
        // THE COUNT: one radio opened, counted under its own word.
        CHECK(g->counts() == (Counts{{"radio_open.hydrasdr", 1}}));

        // The memory is the radio's own, under ITS key - not an Airspy's.
        CHECK(Access::mode(app, Mode::Linearity));
        CHECK(Access::decimate(app, 4));
        saved = Access::config(app);
        CHECK(memOf(saved, kKey).mode == "linear");
        CHECK(memOf(saved, kKey).decimation == 4);
        bool leakedUnderAirspy = false;
        for (const auto& e : saved.airspy) {
            if (e.first.rfind("airspy|", 0) == 0) { leakedUnderAirspy = true; }
        }
        CHECK(!leakedUnderAirspy);
        g->reset();
    }

    // --- a later launch: the saved radio comes back as it was left ----------------
    {
        auto g = fresh();
        cascade::core::AppConfig cfg = saved;
        cfg.sourceKind = "hydrasdr";
        cfg.nativeArgs = kArgs;
        cascade::gui::AppWindow app;
        Access::restore(app, cfg);
        cascade::source::AirspySource* a = Access::radio(app);
        CHECK(a != nullptr);
        if (a != nullptr) {
            std::printf("  relaunch: mode %d, /%u, radio %.0f\n", static_cast<int>(a->gainMode()),
                        a->decimation(), a->hardwareSampleRateHz());
            CHECK(std::string(a->driverKey()) == "hydrasdr");
            CHECK(a->gainMode() == Mode::Linearity);
            CHECK(a->decimation() == 4);
        }
        CHECK(Access::sourceKind(app) == "hydrasdr");
        // THE RESTORE IS COUNTED LIKE ANY OTHER OPEN.
        CHECK(g->counts() == (Counts{{"radio_open.hydrasdr", 1}}));
        g->reset();
    }

    // --- a PICK FROM THE SOURCE LIST (the worker's open) puts the memory back too -----------
    //     It is a different code path from the startup restore above: the GUI thread copies the
    //     radio's remembered state into the request (launchDeviceOpen) and the worker applies it
    //     before the rate is asked for. Kept under the radio's own key, so it is the one place a
    //     HydraSDR's memory could be silently missed.
    {
        auto g = fresh();
        cascade::core::AppConfig cfg = saved;
        cfg.sourceKind = "siggen";
        cascade::core::AirspySetting s;
        s.mode = "sensitive";
        s.sensitivity = 7;
        s.decimation = 8;
        cfg.airspy.clear();
        cfg.airspy[kKey] = s;
        cascade::gui::AppWindow app;
        Access::restore(app, cfg);
        CHECK(Access::select(app));
        cascade::source::AirspySource* a = Access::radio(app);
        CHECK(a != nullptr);
        if (a != nullptr) {
            CHECK(a->gainMode() == Mode::Sensitivity);
            CHECK(a->gainDb("SENSITIVITY") == 7.0);
            CHECK(a->decimation() == 8);
        }
        g->reset();
    }

    // --- the driver's own refusal, through the path that opened it ---------------------
    {
        auto g = fresh();
        g_real.store(true);
        cascade::gui::AppWindow app;
        CHECK(!Access::select(app));
        g_real.store(false);
        std::string only;
        for (const auto& kv : g->counts()) { only += kv.first; }
        std::printf("  the real driver, serial not there: %s\n", only.c_str());
        CHECK(g->counts().size() == 1);
        CHECK(only == "radio_fail.hydrasdr.bind" || only == "radio_fail.hydrasdr.absent");
        CHECK(g->counts().count("radio_open.hydrasdr") == 0);
        g->reset();
    }

    // --- the saved config: the kind survives a load, the memory key too ------------------
    {
        const fs::path path = g_scratch / "config.json";
        CHECK(writeText(path,
                        "{\"schemaVersion\":1,\"sourceKind\":\"hydrasdr\","
                        "\"nativeArgs\":\"serial=HYDRASDR_SN:0123456789ABCDEF\","
                        "\"airspy\":{\"hydrasdr|serial=HYDRASDR_SN:0123456789ABCDEF\":"
                        "{\"mode\":\"sensitive\",\"decimation\":8,\"lna\":99},"
                        "\"banana|serial=1\":{\"mode\":\"linear\"}}}\n"));
        cascade::core::AppConfig out;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(path.string(), out, err));
        CHECK(out.sourceKind == "hydrasdr");
        CHECK(out.nativeArgs == "serial=HYDRASDR_SN:0123456789ABCDEF");
        CHECK(out.airspy.count(kKey) == 1);
        CHECK(out.airspy.count("banana|serial=1") == 0);   // an unknown driver's entry is dropped
        CHECK(memOf(out, kKey).mode == "sensitive");
        CHECK(memOf(out, kKey).decimation == 8);
        CHECK(memOf(out, kKey).lna == 14);                 // clamped, as an Airspy's is
        // ...and it round-trips through a save.
        const fs::path again = g_scratch / "config2.json";
        CHECK(cascade::core::ConfigStore::save(again.string(), out, err));
        cascade::core::AppConfig back;
        CHECK(cascade::core::ConfigStore::load(again.string(), back, err));
        CHECK(back.sourceKind == "hydrasdr");
        CHECK(back.airspy.count(kKey) == 1);
        // The Airspy's own key is untouched by all of it.
        CHECK(cascade::core::airspyRadioKey("serial=X") == "airspy|serial=X");
        CHECK(cascade::core::airspyRadioKey("serial=X", "hydrasdr") == "hydrasdr|serial=X");
    }

    // --- the web remote may name it ------------------------------------------------------------
    {
        cascade::net::ControlRequest r;
        std::string error;
        CHECK(cascade::net::parseControlRequest(
            "{\"sourceKind\":\"hydrasdr\",\"soapyArgs\":\"serial=HYDRASDR_SN:0123456789ABCDEF\"}", r,
            error));
        CHECK(r.sourceKind.value_or("") == "hydrasdr");
        // ...and the sentence that lists the kinds it may name now lists it.
        cascade::net::ControlRequest bad;
        CHECK(!cascade::net::parseControlRequest("{\"sourceKind\":\"hydra\"}", bad, error));
        CHECK(error.find("\"hydrasdr\"") != std::string::npos);
    }

    // --- the rules that group radios by family -----------------------------------------------------
    {
        CHECK(cascade::gui::isNativeSourceKind("hydrasdr"));
        CHECK(cascade::gui::isNativeSourceKind("airspy"));
        CHECK(!cascade::gui::isNativeSourceKind("hydra"));
        // SoapyHydraSDR registers itself as "hydrasdr", so a scan beside an open
        // native radio leaves that module out (it would reset the open radio).
        bool known = false;
        CHECK(cascade::gui::soapyModulesForFamily("hydrasdr", "", known) ==
                  std::vector<std::string>({"hydrasdr"}) &&
              known);
        // The patch page's one-device-one-radio rule: the native row and the
        // SoapySDR row of the same radio are the same radio.
        CHECK(cascade::core::patch::nativeFamilyForSoapyDriver("hydrasdr") == "hydrasdr");
        CHECK(cascade::core::patch::nativeFamilyForSoapyDriver("hydra") == "");
        CHECK(cascade::gui::nativeKeyForSoapyDriver("hydrasdr") == "hydrasdr");
        // An Airspy and a HydraSDR are NOT the same family: one is never
        // answered with the other.
        CHECK(cascade::core::patch::nativeFamilyForSoapyDriver("airspy") == "airspy");
        std::vector<cascade::source::NativeDeviceInfo> native = {
            {"airspy", "Airspy R2", "serial=644866c83f1a51df"},
            {"hydrasdr", "HydraSDR RFOne", kArgs}};
        const auto pick = cascade::gui::preferNativeFor(
            "soapy", "driver=hydrasdr,serial=0123456789ABCDEF", native);
        CHECK(pick.has_value());
        CHECK(pick.has_value() && pick->driver == "hydrasdr");
        const auto none = cascade::gui::preferNativeFor(
            "soapy", "driver=hydrasdr,serial=FFFFFFFFFFFFFFFF", native);
        CHECK(!none.has_value());   // a different serial: a different radio
    }

    // --- the registrations no device hook reaches -------------------------------------------
    //
    // scanNative() asks the real USB transport, which a test cannot put a radio
    // on, so the two lines that make the RFOne appear in the Source list (and
    // an unbound one be reported) are pinned as text, the way
    // test_health_events pins PRIVACY.md. So are the Linux permission rule and
    // the README's statement that the driver is unconfirmed: a rule that is
    // only a comment is a rule nothing holds.
    {
        const auto readText = [](const char* relative) {
            std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / relative, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string window = readText("src/gui/app_window.cpp");
        CHECK(!window.empty());
        CHECK(window.find("cascade::source::enumerateHydraSdr()") != std::string::npos);
        CHECK(window.find("cascade::source::hydraSdrUsbIds()") != std::string::npos);
        CHECK(window.find("kind == \"hydrasdr\"") != std::string::npos);
        const std::string rules = readText("installer/linux/99-foxsdr-sdr.rules");
        CHECK(rules.find("ATTR{idVendor}==\"38af\", ATTR{idProduct}==\"0001\"") != std::string::npos);
        const std::string readme = readText("README.md");
        CHECK(readme.find("The native HydraSDR RFOne driver (0.99.66, NOT YET TESTED ON HARDWARE)") !=
              std::string::npos);
        CHECK(readText("PRIVACY.md").find("| `hydrasdr` | HydraSDR RFOne |") != std::string::npos);
    }

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_hydrasdr_app");
}
