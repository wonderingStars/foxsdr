// ONE CHIP, ONE ROUTE through the REAL AppWindow: the two field reports
// (0.99.59, one Linux RSP1A with the SDRplay API installed), end to end.
//
//   B. The SDRplay API session was lost, then the PATCH PAGE opened the radio
//      through SoapySDR's `sdrplay` module. Here the page's own radio worker
//      is given a Radio node on "soapy|driver=sdrplay,..." under a lost
//      session: the SoapySDR module (registered below under its real name)
//      must never be asked to make a device, and the node must say why.
//   A. The API manages the radio, and the Source list still offered the
//      SoapySDR `miri` row for it. The rows the scan lists must not include a
//      row that can only fail - in the Source list and in the patch page's
//      device list alike - and a saved config naming one must be refused
//      with the reason.
//
// The decision itself is pinned in test_one_chip_one_route; this proves the
// application asks it: from the scan merge, from the native scan, from the
// patch page's worker and from the startup restore.
//
// Hermetic like test_ppm_app: no config file, per-user directories in a scratch
// folder, no USB walk (the native list is the test's own), no real driver.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "source/rsp_rows.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::source::MiricsSoapyRefusal;
using cascade::source::NativeDeviceInfo;
using cascade::source::SdrPlayApiState;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_one_chip_app_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

NativeDeviceInfo row(const std::string& driver, const std::string& label, const std::string& args) {
    NativeDeviceInfo r;
    r.driver = driver;
    r.label = label;
    r.args = args;
    return r;
}

// The native list the next scan answers with: the test's, set per scenario.
std::vector<NativeDeviceInfo> g_native;
std::vector<NativeDeviceInfo> fakeScan() { return g_native; }

SdrPlayApiState state(bool installed, bool lost, bool abandoned) {
    SdrPlayApiState s;
    s.installed = installed;
    s.sessionLost = lost;
    s.workerAbandoned = abandoned;
    return s;
}

// --- SoapySDR modules under their real names, counting what they are asked ---
std::atomic<int> g_makesSdrplay{0};
std::atomic<int> g_makesMiri{0};

class FakeChip : public SoapySDR::Device {
public:
    explicit FakeChip(std::string key) : key_(std::move(key)) {}
    std::string getDriverKey() const override { return key_; }
    std::string getHardwareKey() const override { return "fake " + key_; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    void setSampleRate(const int, const size_t, const double) override {}
    double getFrequency(const int, const size_t) const override { return 100.0e6; }
    void setFrequency(const int, const size_t, const double, const SoapySDR::Kwargs&) override {}
    int readStream(SoapySDR::Stream*, void* const*, const size_t, int&, long long&,
                   const long) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return SOAPY_SDR_TIMEOUT;  // the retry answer: a quiet radio, not a broken one
    }

private:
    std::string key_;
};

SoapySDR::KwargsList findNamed(const SoapySDR::Kwargs& args, const char* name) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second == name) { return SoapySDR::KwargsList{args}; }
    return {};
}
SoapySDR::KwargsList findSdrplay(const SoapySDR::Kwargs& a) { return findNamed(a, "sdrplay"); }
SoapySDR::KwargsList findMiri(const SoapySDR::Kwargs& a) { return findNamed(a, "miri"); }
SoapySDR::Device* makeSdrplay(const SoapySDR::Kwargs&) {
    g_makesSdrplay.fetch_add(1);
    return new FakeChip("sdrplay");
}
SoapySDR::Device* makeMiri(const SoapySDR::Kwargs&) {
    g_makesMiri.fetch_add(1);
    return new FakeChip("miri");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.nativeScan = &fakeScan; }

    static void setSoapyRows(AppWindow& a, std::vector<cascade::source::SoapyDeviceInfo> rows) {
        a.soapyDevices_ = std::move(rows);
        a.soapyScanned_ = true;
        a.soapyScanPending_ = false;
        a.deviceOpenPending_ = false;
    }
    static std::vector<std::string> soapyArgsOf(const AppWindow& a) {
        std::vector<std::string> out;
        for (const auto& d : a.soapyDevices_) { out.push_back(d.args); }
        return out;
    }
    static void scan(AppWindow& a) { a.scanNative(); }
    static std::vector<std::string> patchChoiceKeys(const AppWindow& a) {
        std::vector<std::string> out;
        for (const auto& c : a.patchDeviceChoices()) { out.push_back(c.key); }
        return out;
    }

    static cascade::core::patch::NodeId addRadioNode(AppWindow& a, const std::string& device) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;
        const pc::NodeId id = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        if (pc::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = device;
            n->freqHz = 100.0e6;
            n->centreChosen = true;
            n->rateHz = 2.0e6;
            n->on = true;
        }
        return id;
    }
    // Reconciles until the node has either a running radio or an answer that
    // it would not open. True when one of those happened in time.
    static bool runPatchUntilAnswered(AppWindow& a, cascade::core::patch::NodeId id) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;
        const auto t0 = std::chrono::steady_clock::now();
        while (a.patchRadios_.count(id) == 0 && a.patchRadioError_.count(id) == 0) {
            a.patchReconcile();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool radioRunning(const AppWindow& a, cascade::core::patch::NodeId id) {
        return a.patchRadios_.count(id) != 0;
    }
    static std::string nodeError(const AppWindow& a, cascade::core::patch::NodeId id) {
        const auto it = a.patchRadioError_.find(id);
        return it == a.patchRadioError_.end() ? std::string() : it->second;
    }
    static void stopPatch(AppWindow& a) {
        a.patchRunning_ = false;
        a.patchStopAll(true);
    }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static std::string sourceError(const AppWindow& a) { return a.sourceError_; }
    static bool deviceOpen(const AppWindow& a) { return a.device_ != nullptr; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_one_chip_app\n");
    isolate();
    Access::installHooks();

    SoapySDR::Registry regSdrplay("sdrplay", &findSdrplay, &makeSdrplay, SOAPY_SDR_ABI_VERSION);
    SoapySDR::Registry regMiri("miri", &findMiri, &makeMiri, SOAPY_SDR_ABI_VERSION);

    const std::string lostSentence =
        cascade::source::miricsSoapyRefusalSentence(MiricsSoapyRefusal::SessionLost);
    const std::string managedSentence =
        cascade::source::miricsSoapyRefusalSentence(MiricsSoapyRefusal::ManagedByApi);
    CHECK(!lostSentence.empty() && !managedSentence.empty());

    const auto soapyRows = [] {
        std::vector<cascade::source::SoapyDeviceInfo> rows;
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
                 {"SDRplay RSP1A", "driver=sdrplay,serial=1811003EFB"},
                 {"Mirics", "driver=miri"},
                 {"B200", "driver=uhd,serial=31"}}) {
            cascade::source::SoapyDeviceInfo d;
            d.label = pair.first;
            d.args = pair.second;
            rows.push_back(std::move(d));
        }
        return rows;
    };
    using Args = std::vector<std::string>;

    cascade::gui::AppWindow app;

    // =========================================================================
    // THE LISTS: a row that can only fail is not offered (Source list AND patch
    // page device list - both are built from the same scan result)
    // =========================================================================
    g_native = {row("mirisdr", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB"),
                row("sdrplay", "SDRplay RSP1A (serial 1811003EFB)", "serial=1811003EFB")};

    // The API manages the RSP: SoapyMiri's row for it goes, the SDRplay
    // module's row and everyone else's stay.
    cascade::source::setSdrPlayApiStateForTest(state(true, false, false));
    Access::setSoapyRows(app, soapyRows());
    Access::scan(app);
    CHECK(Access::soapyArgsOf(app) ==
          (Args{"driver=sdrplay,serial=1811003EFB", "driver=uhd,serial=31"}));
    {
        const Args keys = Access::patchChoiceKeys(app);
        const auto has = [&keys](const std::string& k) {
            for (const std::string& x : keys) {
                if (x == k) { return true; }
            }
            return false;
        };
        CHECK(!has("soapy|driver=miri"));
        CHECK(has("soapy|driver=uhd,serial=31"));
    }

    // The session is lost: both Mirics-family rows go.
    cascade::source::setSdrPlayApiStateForTest(state(true, true, false));
    Access::setSoapyRows(app, soapyRows());
    Access::scan(app);
    CHECK(Access::soapyArgsOf(app) == (Args{"driver=uhd,serial=31"}));
    {
        const Args keys = Access::patchChoiceKeys(app);
        for (const std::string& k : keys) {
            CHECK(k.find("driver=sdrplay") == std::string::npos);
            CHECK(k.find("driver=miri") == std::string::npos);
        }
    }

    // NO SDRPLAY API ON THIS MACHINE: a genuine Mirics dongle's row stays,
    // even beside a row that looks like an RSP.
    cascade::source::setSdrPlayApiStateForTest(state(false, false, false));
    Access::setSoapyRows(app, soapyRows());
    Access::scan(app);
    CHECK(Access::soapyArgsOf(app).size() == 3u);

    // =========================================================================
    // REPORT B: THE PATCH PAGE'S RADIO, session lost
    // =========================================================================
    g_native.clear();
    cascade::source::setSdrPlayApiStateForTest(state(true, false, false));
    {
        // CONTROL: the same node, session healthy - the page opens the fake
        // SoapySDR module and the radio runs. Without this the refusal below
        // could be the harness.
        const int before = g_makesSdrplay.load();
        const auto id = Access::addRadioNode(app, "soapy|driver=sdrplay,serial=1811003EFB");
        CHECK(Access::runPatchUntilAnswered(app, id));
        CHECK(Access::radioRunning(app, id));
        CHECK(g_makesSdrplay.load() == before + 1);
        Access::stopPatch(app);
    }
    {
        cascade::gui::AppWindow lostApp;
        cascade::source::setSdrPlayApiStateForTest(state(true, true, false));
        const int before = g_makesSdrplay.load();
        const auto id = Access::addRadioNode(lostApp, "soapy|driver=sdrplay,serial=1811003EFB");
        CHECK(Access::runPatchUntilAnswered(lostApp, id));
        CHECK(!Access::radioRunning(lostApp, id));
        // The module was NEVER ASKED to make a device - the open was refused
        // before it - and the node says why, in the sentence the user gets.
        CHECK(g_makesSdrplay.load() == before);
        CHECK(Access::nodeError(lostApp, id) == lostSentence);
        Access::stopPatch(lostApp);
    }

    // =========================================================================
    // THE STARTUP RESTORE: a saved SoapySDR Mirics-family radio
    // =========================================================================
    {
        cascade::gui::AppWindow restoreApp;
        cascade::source::setSdrPlayApiStateForTest(state(true, true, false));
        const int before = g_makesSdrplay.load();
        cascade::core::AppConfig cfg;
        cfg.sourceKind = "soapy";
        cfg.soapyArgs = "driver=sdrplay,serial=1811003EFB";
        Access::restore(restoreApp, cfg);
        CHECK(!Access::deviceOpen(restoreApp));
        CHECK(g_makesSdrplay.load() == before);
        CHECK(Access::sourceError(restoreApp).find(lostSentence) != std::string::npos);
    }
    {
        // The same saved radio through SoapyMiri, the session lost: refused,
        // and SoapyMiri is never asked either.
        cascade::gui::AppWindow restoreApp;
        cascade::source::setSdrPlayApiStateForTest(state(true, true, false));
        const int before = g_makesMiri.load();
        cascade::core::AppConfig cfg;
        cfg.sourceKind = "soapy";
        cfg.soapyArgs = "driver=miri";
        Access::restore(restoreApp, cfg);
        CHECK(!Access::deviceOpen(restoreApp));
        CHECK(g_makesMiri.load() == before);
        CHECK(Access::sourceError(restoreApp).find(lostSentence) != std::string::npos);
    }

    cascade::source::setSdrPlayApiStateForTest(std::nullopt);
    cascade::source::sdrPlayPublishNativeRows({});
    return testSummary("test_one_chip_app");
}
