// A RADIO'S AUTOMATIC-GAIN SWITCH IS REMEMBERED ACROSS RUNS (0.99.73), through the
// real AppWindow code. The owner: "also add a remember last state of auto gain ...
// add that so it always does that". Before this every radio but the Airspy was
// forced back to manual gain at every open (adoptDeviceMirrors), so a radio left
// on auto gain came back on manual gain after every restart.
//
// What is proved here, on fake radios that record every setAutoGain they receive:
//   * remembered ON + a radio with auto gain: setAutoGain(true) is called ONCE after
//     the manual-gain probe, and the box is on;
//   * remembered ON + a radio without auto gain: it is never asked, the box stays
//     off, and the one log line says so (the entry is kept for the next open);
//   * remembered ON + a radio that refuses it: the box stays off, the same line;
//   * remembered OFF, or nothing remembered: the radio opens on manual gain exactly
//     as before (the probe's setAutoGain(false) and nothing else);
//   * the memory is PER RADIO: another radio is not affected and nothing is erased
//     at open;
//   * ticking and unticking the box (AppWindow::changeAutoGain, the box's own path)
//     writes the memory and the saved config; a refusal writes nothing;
//   * a restart: the saved config, written to a file and read back into a new
//     window, brings the radio back on auto gain - and back on manual after an
//     untick;
//   * the reopen after a driver fault still re-applies the state the user had (and
//     not the memory's).
//
// Hermetic like test_ppm_app: no config file of the user's, the per-user directories
// in a scratch folder, no USB walk, no real driver (AppWindow::testHooks_ builds the
// fakes wherever the application would construct a driver).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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

#include "core/bias_tee_memory.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "gui/app_window.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace cc = cascade::core;

namespace {

struct Record {
    std::string kind;
    std::string args;
    std::vector<bool> agcCalls;  // every setAutoGain(on), in order
};

struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Record>> made;
    std::vector<cascade::source::NativeDeviceInfo> native;
    // A radio that has auto gain but refuses to switch it ON (it accepts OFF, which
    // is what the open's manual-gain probe asks).
    std::atomic<bool> refuseAgcOn{false};
};
Registry g_reg;

// The HackRF fake has NO auto gain (autoGainSupported() false), like the real one
// does not; every other kind has it.
bool kindHasAutoGain(const std::string& kind) { return kind != "hackrf"; }

class FakeRadio final : public cascade::source::DeviceSource {
public:
    FakeRadio(std::string kind, std::shared_ptr<Record> rec)
        : kind_(std::move(kind)), rec_(std::move(rec)) {}

    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abort_) { return 0; }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return err_.c_str(); }

    const char* driverKey() const override { return kind_.c_str(); }
    bool open(const std::string& args) override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->args = args;
        }
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return kindHasAutoGain(kind_); }
    bool setAutoGain(bool on) override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->agcCalls.push_back(on);
        }
        if (!kindHasAutoGain(kind_)) { return false; }
        if (on && g_reg.refuseAgcOn.load()) {
            err_ = "the radio refused automatic gain";
            return false;
        }
        agc_ = on;
        return true;
    }
    bool autoGain() const override { return agc_; }
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
    std::string kind_;
    std::shared_ptr<Record> rec_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    bool agc_ = false;
    std::string err_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    auto rec = std::make_shared<Record>();
    rec->kind = kind;
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.made.push_back(rec);
    }
    return std::make_unique<FakeRadio>(kind, rec);
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.native;
}

std::size_t madeCount() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.made.size();
}

Record made(std::size_t i) {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return i < g_reg.made.size() ? *g_reg.made[i] : Record{};
}

const std::string kRtlA = "serial=0000000A";
const std::string kRtlB = "serial=0000000B";
const std::string kHack = "serial=abc";
const std::string kSoapyArgs = "driver=uhd,serial=31E0000";
const std::string kKeyA = cc::biasTeeRadioKey("rtlsdr", kRtlA);
const std::string kKeyB = cc::biasTeeRadioKey("rtlsdr", kRtlB);
const std::string kKeyHack = cc::biasTeeRadioKey("hackrf", kHack);
const std::string kKeySoapy = cc::biasTeeRadioKey("soapy", kSoapyArgs);

// The one log line, and how many times the ring holds it.
const char* const kRefusedLine = "source: auto gain remembered on but the radio refused it";

int refusedLines() {
    int n = 0;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(kRefusedLine) != std::string::npos) { ++n; }
    }
    return n;
}

void resetRegistry() {
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.made.clear();
        g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kRtlA},
                        {"rtlsdr", "Generic RTL2832U B", kRtlB},
                        {"hackrf", "HackRF One", kHack}};
    }
    g_reg.refuseAgcOn = false;
    cc::DiagLog::instance().resetForTest();
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
    g_scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_autogain_app_" + std::to_string(pid));
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
    static bool selectSoapy(AppWindow& a, const std::string& args) {
        a.soapyDevices_.clear();
        a.soapyDevices_.push_back({"B200", args});
        a.selectSource(a.soapyRowBase());
        return waitOpen(a);
    }
    static void reopen(AppWindow& a) { a.reopenAfterDriverFault(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static cascade::core::AppConfig saved(AppWindow& a) { return a.currentConfig(); }

    // The window's state: the box, whether it is offered, and the memory.
    static bool agc(const AppWindow& a) { return a.deviceAgc_; }
    static bool agcSupported(const AppWindow& a) { return a.deviceAgcSupported_; }
    static const std::map<std::string, bool>& memory(const AppWindow& a) {
        return a.autoGainMemory_;
    }
    static void remember(AppWindow& a, const std::string& key, bool on) {
        a.autoGainMemory_[key] = on;
    }
    // The box's own path.
    static bool toggle(AppWindow& a, bool want) { return a.changeAutoGain(want); }
    static void clearError(AppWindow& a) { a.sourceError_.clear(); }
    static std::string error(const AppWindow& a) { return a.sourceError_; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

bool callsAre(const Record& r, const std::vector<bool>& want) { return r.agcCalls == want; }

// What a memory holds for a radio: 1 on, 0 off, -1 no entry. Never throws, so a
// missing entry is a failed CHECK and not a terminated test.
int entry(const std::map<std::string, bool>& m, const std::string& key) {
    const auto it = m.find(key);
    return it == m.end() ? -1 : (it->second ? 1 : 0);
}

std::size_t openNative(cascade::gui::AppWindow& app, const std::string& args) {
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, args));
    CHECK(madeCount() == idx + 1);
    CHECK(made(idx).args == args);
    return idx;
}

void testNothingRemembered() {
    std::printf("  nothing remembered: the radio opens on manual gain, as before\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    const std::size_t r = openNative(app, kRtlA);
    // The probe's manual-gain call and nothing else.
    CHECK(callsAre(made(r), {false}));
    CHECK(Access::agcSupported(app));
    CHECK(!Access::agc(app));
    CHECK(Access::memory(app).empty());
    CHECK(refusedLines() == 0);
}

void testRememberedOnRestores() {
    std::printf("  remembered ON on a radio with auto gain: put back once, box on\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::remember(app, kKeyA, true);
    const std::size_t r = openNative(app, kRtlA);
    // The probe (manual), then exactly one switch ON.
    CHECK(callsAre(made(r), {false, true}));
    CHECK(Access::agcSupported(app));
    CHECK(Access::agc(app));
    CHECK(refusedLines() == 0);
    // An open never rewrites the memory.
    CHECK(Access::memory(app).size() == 1 && entry(Access::memory(app), kKeyA) == 1);
}

void testRememberedOnUnsupported() {
    std::printf("  remembered ON on a radio WITHOUT auto gain: not asked, box off, one line\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::remember(app, kKeyHack, true);
    const std::size_t r = openNative(app, kHack);
    CHECK(made(r).agcCalls.empty());  // setAutoGain was never called at all
    CHECK(!Access::agcSupported(app));
    CHECK(!Access::agc(app));
    CHECK(refusedLines() == 1);
    // Kept for the next open (a different radio of that key may support it).
    CHECK(Access::memory(app).size() == 1 && entry(Access::memory(app), kKeyHack) == 1);
}

void testRememberedOnRefused() {
    std::printf("  remembered ON on a radio that refuses it: tried once, box off, one line\n");
    resetRegistry();
    g_reg.refuseAgcOn = true;
    cascade::gui::AppWindow app;
    Access::remember(app, kKeyA, true);
    const std::size_t r = openNative(app, kRtlA);
    CHECK(callsAre(made(r), {false, true}));
    CHECK(Access::agcSupported(app));
    CHECK(!Access::agc(app));
    CHECK(refusedLines() == 1);
    CHECK(Access::memory(app).size() == 1 && entry(Access::memory(app), kKeyA) == 1);
}

void testRememberedOffStaysManual() {
    std::printf("  remembered OFF: manual gain, nothing asked, no line, entry kept\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::remember(app, kKeyA, false);
    const std::size_t r = openNative(app, kRtlA);
    CHECK(callsAre(made(r), {false}));
    CHECK(!Access::agc(app));
    CHECK(refusedLines() == 0);
    CHECK(Access::memory(app).size() == 1 && entry(Access::memory(app), kKeyA) == 0);
}

void testPerRadio() {
    std::printf("  the memory is per radio: another radio is untouched, nothing is erased\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::remember(app, kKeyA, true);
    // Radio B has no entry: manual gain, and A's entry survives its open.
    const std::size_t b = openNative(app, kRtlB);
    CHECK(callsAre(made(b), {false}));
    CHECK(!Access::agc(app));
    CHECK(Access::memory(app).size() == 1 && entry(Access::memory(app), kKeyA) == 1);
    // ...and A, opened after B, is still put back on.
    const std::size_t a = openNative(app, kRtlA);
    CHECK(callsAre(made(a), {false, true}));
    CHECK(Access::agc(app));
    // B is then switched on: both radios are in the memory, each its own.
    const std::size_t b2 = openNative(app, kRtlB);
    CHECK(callsAre(made(b2), {false}));
    CHECK(Access::toggle(app, true));
    CHECK(Access::memory(app).size() == 2);
    CHECK(entry(Access::memory(app), kKeyA) == 1 && entry(Access::memory(app), kKeyB) == 1);
}

void testToggleWritesMemoryAndSavedConfig() {
    std::printf("  ticking the box writes the memory and the saved config; a refusal writes "
                "nothing\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    const std::size_t r = openNative(app, kRtlA);
    CHECK(Access::memory(app).empty());

    CHECK(Access::toggle(app, true));
    CHECK(Access::agc(app));
    CHECK(callsAre(made(r), {false, true}));
    CHECK(entry(Access::memory(app), kKeyA) == 1);
    {
        const cascade::core::AppConfig cfg = Access::saved(app);
        CHECK(entry(cfg.autoGainByRadio, kKeyA) == 1);
    }

    // UNTICKED: an "off" is remembered too, so a radio set back to manual stays so.
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
    CHECK(entry(Access::memory(app), kKeyA) == 0);
    {
        const cascade::core::AppConfig cfg = Access::saved(app);
        CHECK(entry(cfg.autoGainByRadio, kKeyA) == 0);
    }

    // A REFUSAL: the box stays where it was, the memory is not touched, and the
    // driver's words reach the panel's error line.
    g_reg.refuseAgcOn = true;
    Access::clearError(app);
    CHECK(!Access::toggle(app, true));
    CHECK(!Access::agc(app));
    CHECK(entry(Access::memory(app), kKeyA) == 0);
    CHECK(!Access::error(app).empty());
    g_reg.refuseAgcOn = false;

    // A radio with no auto gain: the call is refused up front, nothing is written.
    const std::size_t h = openNative(app, kHack);
    (void)h;
    CHECK(!Access::toggle(app, true));
    CHECK(Access::memory(app).count(kKeyHack) == 0);
}

void testRestart() {
    std::printf("  a restart: auto gain comes back on, and manual gain stays manual after an "
                "untick\n");
    resetRegistry();
    const std::string path = (g_scratch / "restart.json").string();
    std::string err;

    // Session 1: the user ticks Auto gain on radio A and closes the program.
    {
        cascade::gui::AppWindow app;
        openNative(app, kRtlA);
        CHECK(Access::toggle(app, true));
        const cascade::core::AppConfig cfg = Access::saved(app);
        CHECK(cfg.sourceKind == "rtlsdr" && cfg.nativeArgs == kRtlA);
        CHECK(cascade::core::ConfigStore::save(path, cfg, err));
    }
    // Session 2: the file is read and the window restores the saved radio.
    {
        cascade::core::AppConfig cfg;
        CHECK(cascade::core::ConfigStore::load(path, cfg, err));
        CHECK(entry(cfg.autoGainByRadio, kKeyA) == 1);
        cascade::gui::AppWindow app;
        const std::size_t idx = madeCount();
        Access::restore(app, cfg);
        CHECK(madeCount() == idx + 1);
        CHECK(made(idx).args == kRtlA);
        CHECK(callsAre(made(idx), {false, true}));
        CHECK(Access::agc(app));
        // The user unticks it and closes the program again.
        CHECK(Access::toggle(app, false));
        CHECK(cascade::core::ConfigStore::save(path, Access::saved(app), err));
    }
    // Session 3: manual gain, and it stays manual.
    {
        cascade::core::AppConfig cfg;
        CHECK(cascade::core::ConfigStore::load(path, cfg, err));
        CHECK(entry(cfg.autoGainByRadio, kKeyA) == 0);
        cascade::gui::AppWindow app;
        const std::size_t idx = madeCount();
        Access::restore(app, cfg);
        CHECK(madeCount() == idx + 1);
        CHECK(callsAre(made(idx), {false}));
        CHECK(!Access::agc(app));
    }
}

void testRecoveryReopen() {
    std::printf("  the reopen after a driver fault re-applies the state the user had, not the "
                "memory's\n");
    resetRegistry();
    // AUTO GAIN ON when the driver faults: the reopen puts it back, once.
    {
        cascade::gui::AppWindow app;
        CHECK(Access::selectSoapy(app, kSoapyArgs));
        CHECK(Access::toggle(app, true));
        CHECK(entry(Access::memory(app), kKeySoapy) == 1);
        const std::size_t again = madeCount();
        Access::reopen(app);
        CHECK(Access::waitOpen(app));
        CHECK(madeCount() == again + 1);
        CHECK(made(again).args == kSoapyArgs);
        // The probe, then the recovery's own switch - and not a second one from the
        // memory (the open does not restore it on a recovery).
        CHECK(callsAre(made(again), {false, true}));
        CHECK(Access::agc(app));
        CHECK(entry(Access::memory(app), kKeySoapy) == 1);
    }
    // AUTO GAIN OFF when the driver faults, whatever the memory says: stays off.
    {
        cascade::gui::AppWindow app;
        CHECK(Access::selectSoapy(app, kSoapyArgs));
        CHECK(!Access::agc(app));
        Access::remember(app, kKeySoapy, true);
        const std::size_t again = madeCount();
        Access::reopen(app);
        CHECK(Access::waitOpen(app));
        CHECK(madeCount() == again + 1);
        CHECK(callsAre(made(again), {false}));
        CHECK(!Access::agc(app));
    }
}

}  // namespace

int main() {
    std::printf("test_autogain_app\n");
    isolate();
    Access::installHooks();

    testNothingRemembered();
    testRememberedOnRestores();
    testRememberedOnUnsupported();
    testRememberedOnRefused();
    testRememberedOffStaysManual();
    testPerRadio();
    testToggleWritesMemoryAndSavedConfig();
    testRestart();
    testRecoveryReopen();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_autogain_app");
}
