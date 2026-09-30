// THE CRYSTAL CORRECTION THROUGH THE REAL AppWindow CODE (0.99.56): every place
// a radio is opened - the Source combo, the startup restore, the patch page's
// own radios - applies that radio's remembered value before it is used, in the
// radio when it corrects its own crystal and by retuning otherwise; a change is
// applied live; OFF sends nothing and changes no tune; and the "answered a tune
// somewhere else" check does not fire because of the correction (while a real
// coerced tune is still reported).
//
// test_ppm_correction proves the arithmetic, the view and the drivers. This
// proves the application hands them the right value at the right moment.
//
// THE RADIOS ARE FAKES THAT RECORD EVERY TUNE AND EVERY CORRECTION THEY RECEIVE
// (AppWindow::testHooks_ constructs them wherever the application would
// construct a driver): the "rtlsdr" fake corrects its own crystal, every other
// kind does not. Hermetic like test_converter_app_paths: no config file, the
// per-user directories in a scratch folder, no USB walk, no real driver.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "core/ppm_correction.hpp"
#include "gui/app_window.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace cc = cascade::core;

namespace {

struct Record {
    std::string kind;
    std::string args;
    std::vector<double> tunes;          // every setCenterFrequencyHz, in order
    std::vector<double> corrections;    // every setFrequencyCorrectionPpm, in order
    // How many tunes had arrived when the first correction did; -1 = none.
    int tunesAtFirstCorrection = -1;
};

struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Record>> made;
    std::vector<cascade::source::NativeDeviceInfo> native;
    // A radio that CLAMPS a tune above its top to the top (and says yes), the
    // way a driver answers "somewhere else" - instead of refusing it.
    std::atomic<bool> clampAtTop{false};
};
Registry g_reg;

constexpr double kTopHz = 1766.0e6;

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
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->tunes.push_back(hz);
        }
        if (hz > kTopHz && g_reg.clampAtTop.load()) {
            centre_ = kTopHz;
            return true;
        }
        if (!(hz >= 24.0e6 && hz <= kTopHz)) { return false; }
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
    const char* lastError() const override { return ""; }
    // THE RTL-SDR FAKE CORRECTS ITS OWN CRYSTAL, as the native driver does.
    bool hasFrequencyCorrection() const override { return kind_ == "rtlsdr"; }
    bool setFrequencyCorrectionPpm(double ppm) override {
        if (kind_ != "rtlsdr") { return false; }
        std::lock_guard<std::mutex> lk(g_reg.m);
        if (rec_->tunesAtFirstCorrection < 0) {
            rec_->tunesAtFirstCorrection = static_cast<int>(rec_->tunes.size());
        }
        rec_->corrections.push_back(ppm);
        return true;
    }

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
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = kTopHz;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::string kind_;
    std::shared_ptr<Record> rec_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;   // where a driver leaves it
    bool open_ = false;
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

double lastTune(std::size_t i) {
    const Record r = made(i);
    return r.tunes.empty() ? std::nan("") : r.tunes.back();
}

const std::string kRtlArgs = "serial=0000000A";
const std::string kRtlKey = "rtlsdr|" + kRtlArgs;
const std::string kHackArgs = "serial=abc";
const std::string kHackKey = "hackrf|" + kHackArgs;
const std::string kRspArgs = "serial=RSP0000A";
const std::string kRspKey = "sdrplay|" + kRspArgs;

void resetRegistry() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    g_reg.made.clear();
    g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kRtlArgs},
                    {"hackrf", "HackRF One", kHackArgs},
                    {"sdrplay", "SDRplay RSP1A", kRspArgs}};
    g_reg.clampAtTop = false;
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
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_ppm_app_" + std::to_string(pid));
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
    static void setPpm(AppWindow& a, bool on, const std::string& key, double v) {
        a.ppmCorrectionOn_ = on;
        a.ppmValues_[key] = v;
    }
    static void setOn(AppWindow& a, bool on) { a.ppmCorrectionOn_ = on; }
    static void change(AppWindow& a, bool on, double v) { a.changePpm(on, v); }
    static double stored(AppWindow& a, const std::string& key) {
        const auto it = a.ppmValues_.find(key);
        return it == a.ppmValues_.end() ? 0.0 : it->second;
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
    static void selectGenerator(AppWindow& a) { a.selectSource(0); }
    static void tune(AppWindow& a, double hz) { a.applyRetuneNow(hz); }
    static void setVfo(AppWindow& a, double hz) { a.pipeline_.setVfoOffsetHz(hz); }
    static double airCentre(AppWindow& a) { return a.pipeline_.activeSource().centerFrequencyHz(); }
    static double counter(AppWindow& a) { return a.currentAbsoluteHz(); }
    static double softwarePpm(AppWindow& a) { return a.pipeline_.softwarePpm(); }
    static std::string tuneNote(AppWindow& a) { return a.tuneMismatchNote_; }
    static void clearNote(AppWindow& a) { a.tuneMismatchNote_.clear(); }
    static std::string card(AppWindow& a) { return a.ppmCardLine(); }
    static std::string diag(AppWindow& a) { return a.ppmDiagText(); }
    static cc::PpmMethod method(AppWindow& a) { return a.ppmMethodNow(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static cascade::core::AppConfig saved(AppWindow& a) { return a.currentConfig(); }

    static cascade::core::patch::NodeId addRadioNode(AppWindow& a, const std::string& device,
                                                     double freqHz) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;
        const pc::NodeId id = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        if (pc::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = device;
            n->freqHz = freqHz;
            n->centreChosen = true;
            n->rateHz = 2.4e6;
            n->on = true;
        }
        return id;
    }
    static bool runPatchUntilOpen(AppWindow& a, cascade::core::patch::NodeId id) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;
        const auto t0 = std::chrono::steady_clock::now();
        while (a.patchRadios_.count(id) == 0) {
            a.patchReconcile();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 5; ++i) { a.patchReconcile(); }
        return true;
    }
    static void reconcile(AppWindow& a, int frames) {
        for (int i = 0; i < frames; ++i) { a.patchReconcile(); }
    }
    static double patchAir(AppWindow& a, cascade::core::patch::NodeId id) {
        const auto it = a.patchRadios_.find(id);
        return it == a.patchRadios_.end() ? std::nan("") : it->second->centreHz();
    }
    static void stopPatch(AppWindow& a) {
        a.patchRunning_ = false;
        a.patchStopAll(true);
        waitOpen(a);
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// The generator on `hz` with the VFO centred, then the radio at `args`
// opened over it - the frequency is carried across. Returns its record index.
std::size_t openOver(cascade::gui::AppWindow& app, const std::string& args, double hz) {
    Access::setVfo(app, 0.0);
    Access::tune(app, hz);
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, args));
    CHECK(madeCount() == idx + 1);
    CHECK(made(idx).args == args);
    return idx;
}

void testOffIsUnchanged() {
    std::printf("  OFF: the same tunes, no correction sent - with and without stored values\n");
    std::vector<double> plainHack;
    std::vector<double> plainRtl;
    for (int pass = 0; pass < 2; ++pass) {
        resetRegistry();
        cascade::gui::AppWindow app;
        if (pass == 1) {
            // Values remembered for both radios, the switch OFF.
            Access::setPpm(app, false, kHackKey, 50.0);
            Access::setPpm(app, false, kRtlKey, 2.6);
        }
        const std::size_t h = openOver(app, kHackArgs, 100.0e6);
        Access::tune(app, 145.0e6);
        CHECK(!made(h).tunes.empty() && made(h).tunes.back() == 145.0e6);
        CHECK(Access::airCentre(app) == 145.0e6);
        const std::size_t r = openOver(app, kRtlArgs, 433.92e6);
        Access::tune(app, 144.8e6);
        CHECK(made(r).corrections.empty());
        CHECK(Access::softwarePpm(app) == 0.0);
        CHECK(Access::card(app).empty());
        CHECK(Access::diag(app) == "off");
        if (pass == 0) {
            plainHack = made(h).tunes;
            plainRtl = made(r).tunes;
        } else {
            CHECK(made(h).tunes == plainHack);
            CHECK(made(r).tunes == plainRtl);
        }
        CHECK(!made(r).tunes.empty() && made(r).tunes.back() == 144.8e6);
    }
}

void testRetuneFallback() {
    std::printf("  a radio without its own correction is corrected by retuning, and live\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kHackKey, 50.0);
    const std::size_t h = openOver(app, kHackArgs, 100.0e6);
    CHECK(Access::method(app) == cc::PpmMethod::Retune);
    CHECK(Access::softwarePpm(app) == 50.0);
    // The radio is told the corrected frequency; everything the user sees
    // reads the TRUE one - exactly, not a fraction of a hertz off.
    CHECK(lastTune(h) == cc::ppmRequestHz(100.0e6, 50.0));
    CHECK(Access::airCentre(app) == 100.0e6);
    CHECK(Access::counter(app) == 100.0e6);
    CHECK(made(h).corrections.empty());
    CHECK(Access::card(app) == "PPM +50.0");
    CHECK(Access::diag(app) == "+50.0 by retuning");

    // A TUNE: corrected at the radio, true on screen, and the "answered a
    // tune somewhere else" check does NOT fire because of the correction.
    Access::clearNote(app);
    Access::tune(app, 145.0e6);
    CHECK(lastTune(h) == cc::ppmRequestHz(145.0e6, 50.0));
    CHECK(Access::airCentre(app) == 145.0e6);
    CHECK(Access::tuneNote(app).empty());
    // THE REPEAT-TUNE GUARD still matches: the same tune again sends nothing.
    const std::size_t n = made(h).tunes.size();
    Access::tune(app, 145.0e6);
    CHECK(made(h).tunes.size() == n);

    // A LIVE CHANGE retunes the radio at once and keeps the station.
    Access::change(app, true, -20.0);
    CHECK(Access::stored(app, kHackKey) == -20.0);
    CHECK(made(h).tunes.size() == n + 1);
    CHECK(lastTune(h) == cc::ppmRequestHz(145.0e6, -20.0));
    CHECK(Access::airCentre(app) == 145.0e6);
    CHECK(Access::tuneNote(app).empty());
    CHECK(Access::card(app) == "PPM -20.0");

    // SWITCHED OFF: the radio goes back to the plain frequency, the value is
    // kept for next time, and the card loses its line.
    Access::change(app, false, -20.0);
    CHECK(lastTune(h) == 145.0e6);
    CHECK(Access::airCentre(app) == 145.0e6);
    CHECK(Access::softwarePpm(app) == 0.0);
    CHECK(Access::stored(app, kHackKey) == -20.0);
    CHECK(Access::card(app).empty());
    CHECK(Access::diag(app) == "off");
    // ...and on again.
    Access::change(app, true, -20.0);
    CHECK(lastTune(h) == cc::ppmRequestHz(145.0e6, -20.0));

    // Saved: the switch and the value, per radio.
    const cascade::core::AppConfig cfg = Access::saved(app);
    CHECK(cfg.ppmCorrection);
    CHECK(cfg.ppm.count(kHackKey) == 1 && cfg.ppm.at(kHackKey) == -20.0);
}

void testTuneElsewhereStillFires() {
    std::printf("  a real coerced tune is still reported with the correction on\n");
    resetRegistry();
    g_reg.clampAtTop = true;
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kHackKey, 50.0);
    const std::size_t h = openOver(app, kHackArgs, 100.0e6);
    Access::clearNote(app);
    Access::tune(app, 1800.0e6);
    CHECK(lastTune(h) == cc::ppmRequestHz(1800.0e6, 50.0));
    CHECK(!Access::tuneNote(app).empty());
}

void testInRadio() {
    std::printf("  a radio with its own correction is sent it before its first tune\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kRtlKey, 2.6);
    const std::size_t r = openOver(app, kRtlArgs, 433.92e6);
    CHECK(Access::method(app) == cc::PpmMethod::InRadio);
    const Record rec = made(r);
    CHECK(rec.corrections.size() == 1u && rec.corrections.front() == 2.6);
    // Before the carried tune - the only tune the application sent.
    CHECK(rec.tunesAtFirstCorrection == 0);
    CHECK(rec.tunes.size() == 1u);
    // IN THE RADIO: the view corrects nothing, the radio is told the plain
    // frequency.
    CHECK(Access::softwarePpm(app) == 0.0);
    CHECK(lastTune(r) == 433.92e6);
    CHECK(Access::airCentre(app) == 433.92e6);
    // The card shows what a whole-ppm radio really took.
    CHECK(Access::card(app) == "PPM +3.0");
    CHECK(Access::diag(app) == "+2.6 in the radio");

    // Live: the new value goes to the radio, and the application sends no
    // tune of its own (the driver retunes itself).
    const std::size_t n = made(r).tunes.size();
    Access::change(app, true, -1.0);
    CHECK(made(r).corrections.back() == -1.0);
    CHECK(made(r).tunes.size() == n);
    // Off: the radio is told 0.
    Access::change(app, false, -1.0);
    CHECK(made(r).corrections.back() == 0.0);
    CHECK(made(r).tunes.size() == n);
}

void testReappliedOnEveryOpen() {
    std::printf("  each radio gets its own value on every open, and no other's\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kHackKey, 50.0);
    Access::setPpm(app, true, kRtlKey, 2.6);
    const std::size_t h1 = openOver(app, kHackArgs, 145.0e6);
    CHECK(lastTune(h1) == cc::ppmRequestHz(145.0e6, 50.0));
    // HackRF -> RTL-SDR: the RTL corrects itself, the view's value is gone.
    const std::size_t r = madeCount();
    CHECK(Access::selectNative(app, kRtlArgs));
    CHECK(Access::softwarePpm(app) == 0.0);
    CHECK(made(r).corrections.size() == 1u && made(r).corrections.back() == 2.6);
    CHECK(lastTune(r) == 145.0e6);
    CHECK(Access::airCentre(app) == 145.0e6);
    // ...and back: the HackRF's value again, on a new open.
    const std::size_t h2 = madeCount();
    CHECK(Access::selectNative(app, kHackArgs));
    CHECK(Access::softwarePpm(app) == 50.0);
    CHECK(lastTune(h2) == cc::ppmRequestHz(145.0e6, 50.0));
    CHECK(Access::airCentre(app) == 145.0e6);
    // The generator: nothing to correct.
    Access::selectGenerator(app);
    CHECK(Access::method(app) == cc::PpmMethod::NotApplicable);
    CHECK(Access::softwarePpm(app) == 0.0);
    CHECK(Access::card(app).empty());
    CHECK(Access::diag(app) == "not applicable");
    Access::tune(app, 100.0e6);
    CHECK(Access::airCentre(app) == 100.0e6);
}

void testStartupRestore() {
    std::printf("  the startup restore tells the saved radio the corrected frequency once\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "hackrf";
    cfg.nativeArgs = kHackArgs;
    cfg.centerHz = 100.0e6;
    cfg.vfoOffsetHz = 0.0;
    cfg.ppmCorrection = true;
    cfg.ppm[kHackKey] = 50.0;
    const std::size_t idx = madeCount();
    Access::restore(app, cfg);
    CHECK(madeCount() == idx + 1);
    const Record rec = made(idx);
    CHECK(rec.args == kHackArgs);
    // TOLD ONCE, corrected: the install moved nothing after the pre-tune.
    CHECK(rec.tunes.size() == 1u);
    CHECK(!rec.tunes.empty() && rec.tunes.front() == cc::ppmRequestHz(100.0e6, 50.0));
    CHECK(Access::airCentre(app) == 100.0e6);
    CHECK(Access::softwarePpm(app) == 50.0);

    // The same config with the switch off: the plain frequency, as ever.
    resetRegistry();
    cascade::gui::AppWindow app2;
    cfg.ppmCorrection = false;
    const std::size_t idx2 = madeCount();
    Access::restore(app2, cfg);
    CHECK(!made(idx2).tunes.empty() && made(idx2).tunes.front() == 100.0e6);
    CHECK(Access::softwarePpm(app2) == 0.0);
}

void testRspPreTune() {
    std::printf("  an RSP's pre-Init tune is the corrected one, and nothing follows it\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kRspKey, 50.0);
    const std::size_t r = openOver(app, kRspArgs, 145.0e6);
    CHECK(made(r).kind == "sdrplay");
    // SdrPlaySource drives no correction of its own, so it is corrected by
    // retuning - and the tune it hears before its stream starts is already
    // the corrected one, so the carry-across after the install finds it there
    // and sends nothing more (an early Update is what the pre-Init tune exists
    // to avoid).
    CHECK(Access::method(app) == cc::PpmMethod::Retune);
    CHECK(made(r).tunes.size() == 1u);
    CHECK(!made(r).tunes.empty() && made(r).tunes.front() == cc::ppmRequestHz(145.0e6, 50.0));
    CHECK(Access::airCentre(app) == 145.0e6);
    CHECK(Access::softwarePpm(app) == 50.0);
}

void testPatchRadios() {
    std::printf("  the patch page's radios take the same value by the same rule\n");
    resetRegistry();
    cascade::gui::AppWindow app;
    Access::setPpm(app, true, kHackKey, 50.0);
    Access::setPpm(app, true, kRtlKey, 2.6);

    // BY RETUNING: the worker's first tune is corrected, the node reads true.
    const auto hid = Access::addRadioNode(app, kHackKey, 145.0e6);
    const std::size_t h = madeCount();
    CHECK(Access::runPatchUntilOpen(app, hid));
    CHECK(made(h).args == kHackArgs);
    CHECK(!made(h).tunes.empty() && made(h).tunes.front() == cc::ppmRequestHz(145.0e6, 50.0));
    CHECK(made(h).tunes.size() == 1u);  // the follow does not chase a fraction
    CHECK(Access::patchAir(app, hid) == 145.0e6);

    // IN THE RADIO: sent before the first tune, which is the plain frequency.
    const auto rid = Access::addRadioNode(app, kRtlKey, 433.92e6);
    const std::size_t r = madeCount();
    CHECK(Access::runPatchUntilOpen(app, rid));
    CHECK(made(r).args == kRtlArgs);
    CHECK(made(r).corrections.size() == 1u && made(r).corrections.front() == 2.6);
    CHECK(made(r).tunesAtFirstCorrection == 0);
    CHECK(!made(r).tunes.empty() && made(r).tunes.front() == 433.92e6);
    CHECK(Access::patchAir(app, rid) == 433.92e6);

    // A CHANGE while they run: each follows on the next frames.
    Access::setPpm(app, true, kHackKey, -20.0);
    Access::setPpm(app, true, kRtlKey, 1.0);
    Access::reconcile(app, 3);
    CHECK(made(h).tunes.back() == cc::ppmRequestHz(145.0e6, -20.0));
    CHECK(Access::patchAir(app, hid) == 145.0e6);
    CHECK(made(r).corrections.back() == 1.0);
    CHECK(made(r).tunes.size() == 1u);
    // OFF: the plain frequency, and 0 to the radio that corrects itself.
    Access::setOn(app, false);
    Access::reconcile(app, 3);
    CHECK(made(h).tunes.back() == 145.0e6);
    CHECK(Access::patchAir(app, hid) == 145.0e6);
    CHECK(made(r).corrections.back() == 0.0);
    const std::size_t nh = made(h).tunes.size();
    const std::size_t nc = made(r).corrections.size();
    Access::reconcile(app, 5);   // and nothing more is sent while nothing changes
    CHECK(made(h).tunes.size() == nh);
    CHECK(made(r).corrections.size() == nc);
    Access::stopPatch(app);
}

}  // namespace

int main() {
    std::printf("test_ppm_app\n");
    isolate();
    Access::installHooks();

    testOffIsUnchanged();
    testRetuneFallback();
    testTuneElsewhereStillFires();
    testInRadio();
    testReappliedOnEveryOpen();
    testStartupRestore();
    testRspPreTune();
    testPatchRadios();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_ppm_app");
}
