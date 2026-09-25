// THE DECK'S BIAS TEE KEY, THROUGH THE REAL AppWindow AND A REAL DRIVER.
//
// The owner asked for the bias tee on the main panel (a tester, 2026-09-25).
// This drives the AppWindow members the key and its dialog call - the same
// ones the drawing code calls on a press and on each answer - with the
// SHIPPING HackRF driver behind them on a fake USB transport, so every "on"
// and "off" below is a control transfer the driver actually sent, and every
// lamp state is the driver's own readback:
//
//   * the key exists only while the open radio has a bias tee: not over the
//     generator, not over a SoapySDR radio, yes over the HackRF;
//   * ON needs the dialog's "Turn it on" the first time for a radio: a press
//     alone sends nothing, Cancel sends nothing, and only the answer switches;
//   * OFF is immediate, every time;
//   * once confirmed for a serial-named radio the next ON is immediate; a
//     radio named only by position (index=0) is asked every time;
//   * the key and the Source panel's checkbox are one state: a tick lights the
//     key and a key press unticks the box;
//   * a refused change (the fake failing the bias-T request) leaves the key,
//     the box and the driver where they were, with the driver's error in
//     sourceError_ - where the Source panel prints refusals;
//   * a dialog whose radio went away switches nothing when answered.
//
// Hermetic, like test_converter_app_paths: no config is read or written, the
// per-user folders point at a scratch directory, no USB enumeration runs
// (testHooks_.nativeScan) and no real device is ever opened
// (testHooks_.makeDevice hands out fakes).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "gui/app_window.hpp"
#include "gui/bias_tee.hpp"
#include "hackrf_fake_usb.hpp"
#include "source/device_source.hpp"
#include "source/hackrf_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::test::FakeHackRfUsb;

// --- the radios ------------------------------------------------------------------

// The HackRF's bias-T request (libhackrf's ANTENNA_ENABLE), as the fake sees it.
constexpr std::uint8_t kAntennaEnable = 23;
constexpr const char* kSerial = "0000000000000000457863c8";
const std::string kHackArgs = std::string("serial=") + kSerial;
const std::string kHackIndexArgs = "index=0";

struct Registry {
    std::mutex m;
    // The fake behind the most recently made HackRF (owned by that driver).
    FakeHackRfUsb* hack = nullptr;
    int hackMade = 0;
};
Registry g_reg;

// A radio with no bias tee at all: what a SoapySDR-opened radio is to the key.
class PlainRadio final : public cascade::source::DeviceSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return false; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2.4e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>*, std::size_t) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return 0;
    }
    const char* name() const override { return "Plain radio"; }
    const char* lastError() const override { return ""; }
    const char* driverKey() const override { return "soapy"; }
    bool open(const std::string&) override {
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    double centre_ = 100.0e6;
    bool open_ = false;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind == "hackrf") {
        // THE SHIPPING DRIVER on a fake wire: its open() enumerates this list
        // instead of WinUSB and gets this device instead of a real handle.
        auto src = std::make_unique<cascade::source::HackRfSource>();
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x6089;
        d.path = "\\\\?\\usb#vid_1d50&pid_6089#fake#{a5dcbf10}";
        d.serial = kSerial;
        d.description = "HackRF One";
        auto owned = std::make_unique<FakeHackRfUsb>();
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            g_reg.hack = owned.get();
            ++g_reg.hackMade;
        }
        auto holder =
            std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
            if (*holder == nullptr) { error = "fake: already handed out"; }
            return std::move(*holder);
        });
        return src;
    }
    return std::make_unique<PlainRadio>();
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"hackrf", "HackRF One", kHackArgs}, {"hackrf", "HackRF One (by index)", kHackIndexArgs}};
}

FakeHackRfUsb* hack() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.hack;
}

// Every bias-T request the driver sent since the fake's transcript was last
// cleared, as the values it asked for (1 on, 0 off).
std::vector<int> biasRequests() {
    std::vector<int> out;
    if (hack() == nullptr) { return out; }
    for (const cascade::test::ControlRecord& c : hack()->controls()) {
        if (!c.in && c.request == kAntennaEnable) { out.push_back(c.value); }
    }
    return out;
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

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_bias_key_app_" + std::to_string(pid));
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

// The friend AppWindow names for its tests (see AppWindow::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }
    static bool waitOpen(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (a.deviceOpenPending_) {
            a.pollSourceAsync();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool selectNative(AppWindow& a, const std::string& args) {
        a.scanNative();
        for (std::size_t i = 0; i < a.nativeDevices_.size(); ++i) {
            if (a.nativeDevices_[i].args == args) {
                a.selectSource(AppWindow::kNativeRowBase + static_cast<int>(i));
                return waitOpen(a);
            }
        }
        return false;
    }
    static bool selectSoapy(AppWindow& a, const std::string& args, const std::string& label) {
        // The native rows first, so the SoapySDR row's number is the one it
        // keeps: a row picked before the scan is renumbered by it, and the
        // next native pick would land on the "same" row and do nothing.
        a.scanNative();
        a.soapyDevices_.clear();
        a.soapyDevices_.push_back({label, args});
        a.selectSource(a.soapyRowBase());
        return waitOpen(a);
    }
    static void selectGenerator(AppWindow& a) { a.selectSource(0); }

    // THE KEY, as drawToolbar draws it: shown and lit from biasKeyPanel().
    static bool keyShown(AppWindow& a) { return a.biasKeyPanel().present; }
    static bool keyLit(AppWindow& a) { return a.biasKeyPanel().shown; }
    // THE CHECKBOX, as the Source panel draws it: shown and ticked from
    // biasTeePanel_ (the stand-in never reaches it).
    static bool boxShown(AppWindow& a) { return a.biasTeePanel_.present; }
    static bool boxTicked(AppWindow& a) { return a.biasTeePanel_.shown; }
    // A press of the key, and the two answers - the members the deck and the
    // dialog call.
    static void press(AppWindow& a) { a.biasKeyPressed(); }
    static void answer(AppWindow& a, bool turnOn) { a.biasKeyAnswered(turnOn); }
    // The dialog is asked for (queued for the top level to open).
    static bool asking(AppWindow& a) { return a.biasKeyAskQueued_ && a.biasKeyGate_.asking; }
    // What the dialog would do if it were drawn now: the question still stands.
    static void clearQueued(AppWindow& a) { a.biasKeyAskQueued_ = false; }
    // A tick or untick of the Source panel's checkbox.
    static void tick(AppWindow& a, bool want) { a.switchBiasTee(want); }
    static const std::string& sourceError(AppWindow& a) { return a.sourceError_; }
    static const std::string& kind(AppWindow& a) { return a.sourceKind_; }
    static const std::string& args(AppWindow& a) { return a.deviceArgs_; }
    static void clearSourceError(AppWindow& a) { a.sourceError_.clear(); }
    // The HackRF driver the application opened, for its readback.
    static bool driverBiasT(AppWindow& a) {
        auto* h = dynamic_cast<cascade::source::HackRfSource*>(a.device_);
        return h != nullptr && h->biasT();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

void testNoKeyWithoutABiasTee() {
    std::printf("  [1] the key is drawn only for a radio with a bias tee\n");
    cascade::gui::AppWindow app;
    // The generator at start.
    std::printf("      generator: shown %d\n", Access::keyShown(app));
    CHECK(!Access::keyShown(app));
    Access::press(app);  // a press with no key does nothing, asks nothing
    CHECK(!Access::asking(app));

    // A SoapySDR radio: no bias tee this application reaches.
    CHECK(Access::selectSoapy(app, "driver=uhd,serial=31E0000", "B200"));
    std::printf("      SoapySDR radio: shown %d\n", Access::keyShown(app));
    CHECK(!Access::keyShown(app));
    CHECK(!Access::boxShown(app));

    // The HackRF: it has one, opened OFF (the driver switches it off at open).
    CHECK(Access::selectNative(app, kHackArgs));
    std::printf("      HackRF: shown %d lit %d (kind %s, args %s, error \"%s\")\n",
                Access::keyShown(app), Access::keyLit(app), Access::kind(app).c_str(),
                Access::args(app).c_str(), Access::sourceError(app).c_str());
    CHECK(Access::keyShown(app));
    CHECK(Access::boxShown(app));
    CHECK(!Access::keyLit(app));
    CHECK(!Access::driverBiasT(app));

    // ...and back to the generator: gone again.
    Access::selectGenerator(app);
    CHECK(!Access::keyShown(app));
}

void testOnAsksOffDoesNot() {
    std::printf("  [2] on needs the dialog's yes; off is immediate; the yes is remembered\n");
    cascade::gui::AppWindow app;
    CHECK(Access::selectNative(app, kHackArgs));
    CHECK(hack() != nullptr);
    hack()->clearControls();

    // A press ASKS and switches nothing.
    Access::press(app);
    std::printf("      press: asking %d, requests sent %zu, lit %d\n", Access::asking(app),
                biasRequests().size(), Access::keyLit(app));
    CHECK(Access::asking(app));
    CHECK(biasRequests().empty());
    CHECK(!Access::keyLit(app));
    CHECK(!Access::driverBiasT(app));

    // Cancel: still nothing sent, nothing remembered - the next press asks again.
    Access::clearQueued(app);
    Access::answer(app, false);
    CHECK(biasRequests().empty());
    CHECK(!Access::keyLit(app));
    Access::press(app);
    CHECK(Access::asking(app));
    CHECK(biasRequests().empty());

    // "Turn it on": ONE request for on, and the lamp is the driver's readback.
    Access::clearQueued(app);
    Access::answer(app, true);
    std::printf("      confirmed: requests %zu (last %d), driver %d, key %d, box %d\n",
                biasRequests().size(), biasRequests().empty() ? -1 : biasRequests().back(),
                Access::driverBiasT(app), Access::keyLit(app), Access::boxTicked(app));
    CHECK(biasRequests() == std::vector<int>{1});
    CHECK(Access::driverBiasT(app));
    CHECK(Access::keyLit(app));
    CHECK(Access::boxTicked(app));  // the checkbox shows the same state

    // OFF: immediate, no dialog.
    hack()->clearControls();
    Access::press(app);
    std::printf("      off: asking %d, requests %zu (last %d), key %d\n", Access::asking(app),
                biasRequests().size(), biasRequests().empty() ? -1 : biasRequests().back(),
                Access::keyLit(app));
    CHECK(!Access::asking(app));
    CHECK(biasRequests() == std::vector<int>{0});
    CHECK(!Access::driverBiasT(app));
    CHECK(!Access::keyLit(app));
    CHECK(!Access::boxTicked(app));

    // ON AGAIN, this session, same serial-named radio: no second question.
    hack()->clearControls();
    Access::press(app);
    std::printf("      on again: asking %d, requests %zu\n", Access::asking(app),
                biasRequests().size());
    CHECK(!Access::asking(app));
    CHECK(biasRequests() == std::vector<int>{1});
    CHECK(Access::keyLit(app));
}

void testKeyAndBoxAreOneState() {
    std::printf("  [3] the key and the checkbox are one state\n");
    cascade::gui::AppWindow app;
    CHECK(Access::selectNative(app, kHackArgs));
    // A tick of the box lights the key...
    Access::tick(app, true);
    CHECK(Access::driverBiasT(app));
    CHECK(Access::boxTicked(app));
    CHECK(Access::keyLit(app));
    // ...and a press of the key unticks the box (off: no dialog).
    Access::press(app);
    CHECK(!Access::asking(app));
    CHECK(!Access::driverBiasT(app));
    CHECK(!Access::boxTicked(app));
    CHECK(!Access::keyLit(app));
}

void testRefusalChangesNothing() {
    std::printf("  [4] a refused change leaves the key, the box and the radio where they were\n");
    cascade::gui::AppWindow app;
    CHECK(Access::selectNative(app, kHackArgs));
    // The radio refuses every bias-T request from here on.
    hack()->failingRequests.push_back(kAntennaEnable);
    Access::clearSourceError(app);
    Access::press(app);
    CHECK(Access::asking(app));
    Access::clearQueued(app);
    Access::answer(app, true);
    std::printf("      refused on: driver %d key %d box %d error \"%s\"\n",
                Access::driverBiasT(app), Access::keyLit(app), Access::boxTicked(app),
                Access::sourceError(app).c_str());
    CHECK(!Access::driverBiasT(app));
    CHECK(!Access::keyLit(app));
    CHECK(!Access::boxTicked(app));
    CHECK(!Access::sourceError(app).empty());
    hack()->failingRequests.clear();

    // On, then the radio refuses the OFF: the lamp stays lit, because the
    // power is still on the port. A FRESH radio: on a HackRF a failed control
    // transfer is a radio that stopped answering, and the driver treats the
    // one above as dead from then on (the error says so).
    cascade::gui::AppWindow app2;
    CHECK(Access::selectNative(app2, kHackArgs));
    Access::tick(app2, true);
    CHECK(Access::keyLit(app2));
    Access::clearSourceError(app2);
    hack()->failingRequests.push_back(kAntennaEnable);
    Access::press(app2);
    std::printf("      refused off: asking %d driver %d key %d box %d error \"%s\"\n",
                Access::asking(app2), Access::driverBiasT(app2), Access::keyLit(app2),
                Access::boxTicked(app2), Access::sourceError(app2).c_str());
    CHECK(!Access::asking(app2));  // off is never asked about, refused or not
    CHECK(Access::driverBiasT(app2));
    CHECK(Access::keyLit(app2));
    CHECK(Access::boxTicked(app2));
    CHECK(!Access::sourceError(app2).empty());
    hack()->failingRequests.clear();
}

void testQuestionThatNoLongerApplies() {
    std::printf("  [5] a dialog whose radio went away switches nothing\n");
    cascade::gui::AppWindow app;
    CHECK(Access::selectNative(app, kHackArgs));
    Access::press(app);
    CHECK(Access::asking(app));
    Access::clearQueued(app);
    // The radio is closed (the generator selected) while the dialog is up.
    Access::selectGenerator(app);
    Access::answer(app, true);
    CHECK(!Access::keyShown(app));
    // Back on the HackRF (a fresh open, off): the stale yes was not
    // remembered, so the next press asks rather than switching on.
    CHECK(Access::selectNative(app, kHackArgs));
    hack()->clearControls();
    CHECK(!Access::keyLit(app));
    Access::press(app);
    std::printf("      after the stale yes: asking %d, requests %zu\n", Access::asking(app),
                biasRequests().size());
    CHECK(Access::asking(app));
    CHECK(biasRequests().empty());
    Access::clearQueued(app);
    Access::answer(app, false);

    // THE DANGEROUS CASE: the dialog asked about one radio, and ANOTHER radio
    // with a bias tee is open when it is answered. The yes was given for the
    // first; it must not put power on the second, which nobody was asked
    // about. (With a closed radio there is nothing to switch either way, so
    // only this case shows a yes that ignores which radio it was for.)
    Access::press(app);  // asks about the serial-named HackRF
    CHECK(Access::asking(app));
    Access::clearQueued(app);
    CHECK(Access::selectNative(app, kHackIndexArgs));  // a different radio, off
    CHECK(Access::keyShown(app));
    hack()->clearControls();
    Access::answer(app, true);
    std::printf("      yes for the other radio: key %d, driver %d, requests %zu\n",
                Access::keyLit(app), Access::driverBiasT(app), biasRequests().size());
    CHECK(!Access::keyLit(app));
    CHECK(!Access::driverBiasT(app));
    CHECK(biasRequests().empty());
}

void testPositionNamedRadioIsAlwaysAsked() {
    std::printf("  [6] a radio named by position (index=0) is asked every time\n");
    cascade::gui::AppWindow app;
    CHECK(Access::selectNative(app, kHackIndexArgs));
    CHECK(Access::keyShown(app));
    Access::press(app);
    CHECK(Access::asking(app));
    Access::clearQueued(app);
    Access::answer(app, true);
    CHECK(Access::keyLit(app));
    Access::press(app);  // off
    CHECK(!Access::keyLit(app));
    hack()->clearControls();
    Access::press(app);  // on again: asked again
    std::printf("      on again: asking %d, requests %zu\n", Access::asking(app),
                biasRequests().size());
    CHECK(Access::asking(app));
    CHECK(biasRequests().empty());
}

}  // namespace

int main() {
    std::printf("test_bias_key_app\n");
    isolate();
    Access::installHooks();

    testNoKeyWithoutABiasTee();
    testOnAsksOffDoesNot();
    testKeyAndBoxAreOneState();
    testRefusalChangesNothing();
    testQuestionThatNoLongerApplies();
    testPositionNamedRadioIsAlwaysAsked();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_bias_key_app");
}
