// THE PATHS THAT CARRY A FREQUENCY FROM ONE RADIO TO THE NEXT, through the
// real AppWindow code, with a converter in front of the radio.
//
// test_converter_routing proves the translation layer (Pipeline::activeSource
// and PatchRadio) converts what it is handed. This proves the application
// HANDS IT OVER: that every place a radio is opened - a device switch, a
// re-select, the startup restore, the reopen after a driver fault, the patch
// page's take-over and hand-back, and the SoapySDR fallback for a dongle the
// native driver refuses - sends the air frequency the user was on through
// THAT radio's converter, before anything else is sent to it.
//
// THE RADIOS ARE FAKES THAT RECORD EVERY setCenterFrequencyHz THEY RECEIVE
// (AppWindow::testHooks_ constructs them wherever the application would
// construct a driver), so "the very first frequency the radio sees" is a
// fact read off the fake, not an inference from where it ended up.
//
// THE CASE THAT FOUND THE BUG: a 16.4 kHz station with the VFO parked 300 kHz
// up puts the band centre at -283.6 kHz ON THE AIR, which through a 125 MHz
// up-converter is 124.7164 MHz at the radio - an ordinary tune. Every carry
// path used to read "centre <= 0" as "nothing to carry" and left the new
// radio at its driver's default instead.
//
// Hermetic: no config file is read or written (empty config path), the
// per-user directories point at a scratch folder, no USB enumeration runs
// (testHooks_.nativeScan) and no real driver is ever constructed.
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

#include "core/freq_converter.hpp"
#include "gui/app_window.hpp"
#include "gui/tune_control.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

using cascade::core::ConverterMode;
using cascade::core::ConverterSetting;

namespace {

ConverterSetting up(double lo) { return {ConverterMode::Up, lo, false}; }

// --- The recording radios ------------------------------------------------------

// One per radio the application constructs, owned by the TEST so what a radio
// was told survives the application destroying it.
struct Record {
    std::string kind;
    std::string args;              // what open() was given
    std::vector<double> tunes;     // every setCenterFrequencyHz, in order
    // How many tunes had arrived when the stream was first started; -1 =
    // never started. A radio whose tune comes only AFTER its stream starts
    // has streamed at its driver's default first, however quickly it was
    // corrected (the patch page's follow corrects it a frame later).
    int tunesAtStart = -1;
};

struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Record>> made;
    // A native open that fails the way the RTL-SDR driver refuses an E4000:
    // with the sentence gui::nativeOpenShouldFallBack recognises.
    std::atomic<bool> nativeRtlRefuses{false};
    std::vector<cascade::source::NativeDeviceInfo> native;
};
Registry g_reg;

class FakeRadio final : public cascade::source::DeviceSource {
public:
    FakeRadio(std::string kind, std::shared_ptr<Record> rec)
        : kind_(std::move(kind)), rec_(std::move(rec)) {}

    // IqSource
    bool start() override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            if (rec_->tunesAtStart < 0) { rec_->tunesAtStart = static_cast<int>(rec_->tunes.size()); }
        }
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
        // An RTL-SDR's range: a driver refuses what it cannot tune.
        if (!(hz >= 24.0e6 && hz <= 1766.0e6)) { return false; }
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
    const char* lastError() const override { return error_.c_str(); }

    // DeviceSource
    const char* driverKey() const override { return kind_.c_str(); }
    bool open(const std::string& args) override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->args = args;
        }
        if (kind_ == "rtlsdr" && g_reg.nativeRtlRefuses.load()) {
            error_ = std::string("this dongle's ") + cascade::gui::kTunerUnsupportedMarker +
                     " yet";
            return false;
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
    double centre_ = 100.0e6;  // where an RTL-SDR's driver leaves it
    bool open_ = false;
    std::string error_;
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

// The radio made at index i, copied under the lock.
Record made(std::size_t i) {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return i < g_reg.made.size() ? *g_reg.made[i] : Record{};
}

// The first frequency radio i was ever told, or NaN when it was told nothing.
double firstTune(std::size_t i) {
    const Record r = made(i);
    return r.tunes.empty() ? std::nan("") : r.tunes.front();
}

void resetRegistry() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    g_reg.made.clear();
    g_reg.native.clear();
    g_reg.nativeRtlRefuses = false;
}

// Per-user directories pointed at a scratch folder, before any AppWindow
// exists: nothing this test does may reach the user's real FoxSDR folders.
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
                ("foxsdr_converter_app_paths_" + std::to_string(pid));
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

// The friend AppWindow names for this test (see AppWindow::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }

    static void setConverter(AppWindow& a, const std::string& key, const ConverterSetting& s) {
        a.converters_[key] = s;
    }

    // Waits for the worker open to resolve and applies it, as the frame loop
    // does. False when it never resolved.
    static bool waitOpen(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (a.deviceOpenPending_) {
            a.pollSourceAsync();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    static int nativeRow(AppWindow& a, const std::string& args) {
        for (std::size_t i = 0; i < a.nativeDevices_.size(); ++i) {
            if (a.nativeDevices_[i].args == args) {
                return AppWindow::kNativeRowBase + static_cast<int>(i);
            }
        }
        return -1;
    }

    // Selects a native row as the Source combo would, and waits for it.
    static bool selectNative(AppWindow& a, const std::string& args) {
        a.scanNative();
        const int row = nativeRow(a, args);
        if (row < 0) { return false; }
        a.selectSource(row);
        return waitOpen(a);
    }

    static bool selectSoapy(AppWindow& a, const std::string& args, const std::string& label) {
        a.soapyDevices_.clear();
        a.soapyDevices_.push_back({label, args});
        a.selectSource(a.soapyRowBase());
        return waitOpen(a);
    }

    // A tune by the counter, applied at once (the coalescer's pacing is not
    // what is under test).
    static void tune(AppWindow& a, double airCentreHz) { a.applyRetuneNow(airCentreHz); }

    static double airCentre(AppWindow& a) { return a.pipeline_.activeSource().centerFrequencyHz(); }
    static double counter(AppWindow& a) { return a.currentAbsoluteHz(); }
    static void setVfo(AppWindow& a, double hz) { a.pipeline_.setVfoOffsetHz(hz); }
    static double vfo(AppWindow& a) { return a.pipeline_.vfoOffsetHz(); }
    static ConverterSetting live(AppWindow& a) { return a.pipeline_.converter(); }
    static const std::string& kind(AppWindow& a) { return a.sourceKind_; }

    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static void reopen(AppWindow& a) { a.reopenAfterDriverFault(); }
    static void changeConverter(AppWindow& a, const ConverterSetting& s) { a.changeConverter(s); }
    static void scanNativeForTest(AppWindow& a) { a.scanNative(); }
    static std::string aliasNote(AppWindow& a) { return a.converterAliasNote(); }

    // --- the patch page ---------------------------------------------------------
    static cascade::core::patch::NodeId addRadioNode(AppWindow& a, const std::string& device,
                                                     double freqHz) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;  // no starter patch: this test builds its own
        const pc::NodeId id = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        if (pc::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = device;
            n->freqHz = freqHz;
            n->rateHz = 2.4e6;
            n->on = true;
        }
        return id;
    }
    static const cascade::core::patch::Node* node(AppWindow& a, cascade::core::patch::NodeId id) {
        return a.patchGraph_.find(id);
    }
    // One reconcile per frame, until the node's radio runs (or 20 s).
    static bool runPatchUntilOpen(AppWindow& a, cascade::core::patch::NodeId id) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;  // the page's first-frame scan is not under test
        const auto t0 = std::chrono::steady_clock::now();
        while (a.patchRadios_.count(id) == 0) {
            a.patchReconcile();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // A few more frames: the running radio's follow must not move it.
        for (int i = 0; i < 5; ++i) { a.patchReconcile(); }
        return true;
    }
    static double patchRadioAir(AppWindow& a, cascade::core::patch::NodeId id) {
        const auto it = a.patchRadios_.find(id);
        return it == a.patchRadios_.end() ? std::nan("") : it->second->centreHz();
    }
    // STOP on the patch page: every patch radio closes and the receiver gets
    // its radio back.
    static bool stopPatch(AppWindow& a) {
        a.patchRunning_ = false;
        a.patchStopAll(true);
        return waitOpen(a);
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr double kAirCentre = -283600.0;  // 16.4 kHz with the VFO 300 kHz up
constexpr double kVfo = 300000.0;
constexpr double kStation = 16400.0;

// Radio A: an RTL-SDR behind the 125 MHz up-converter. Radio B: a second one
// behind a 100 MHz converter, so B's figure can only come from B's setting.
const std::string kArgsA = "serial=0000000A";
const std::string kArgsB = "serial=0000000B";
const std::string kKeyA = "rtlsdr|" + kArgsA;
const std::string kKeyB = "rtlsdr|" + kArgsB;

void twoDongles() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kArgsA},
                    {"rtlsdr", "Generic RTL2832U B", kArgsB}};
}

// The receiver on radio A, tuned so the band centre is `airCentre` on the
// air. Returns the index of A's record.
std::size_t onRadioA(cascade::gui::AppWindow& app, double airCentre, double vfo) {
    Access::setConverter(app, kKeyA, up(125.0e6));
    Access::setConverter(app, kKeyB, up(100.0e6));
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    CHECK(made(idx).args == kArgsA);
    Access::setVfo(app, vfo);
    Access::tune(app, airCentre);
    CHECK(Access::airCentre(app) == airCentre);
    return idx;
}

void testDeviceSwitch(double airCentre, double vfo) {
    std::printf("  a device switch carries the air frequency through the NEW radio's converter "
                "(air centre %.0f Hz)\n",
                airCentre);
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    const std::size_t a = onRadioA(app, airCentre, vfo);
    CHECK(made(a).tunes.back() == airCentre + 125.0e6);

    // A -> B: B's very first frequency is the air one through B's 100 MHz LO.
    const std::size_t b = madeCount();
    CHECK(Access::selectNative(app, kArgsB));
    CHECK(madeCount() == b + 1);
    CHECK(made(b).args == kArgsB);
    CHECK(firstTune(b) == airCentre + 100.0e6);
    CHECK(Access::live(app) == up(100.0e6));
    CHECK(Access::airCentre(app) == airCentre);
    CHECK(Access::counter(app) == airCentre + vfo);

    // ...and back to A (the re-select): A's own 125 MHz LO again.
    const std::size_t a2 = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    CHECK(made(a2).args == kArgsA);
    CHECK(firstTune(a2) == airCentre + 125.0e6);
    CHECK(Access::airCentre(app) == airCentre);
    CHECK(Access::counter(app) == airCentre + vfo);
}

void testStartupRestore(double airCentre) {
    std::printf("  the startup restore tells the saved radio the air frequency converted "
                "(air centre %.0f Hz)\n",
                airCentre);
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "rtlsdr";
    cfg.nativeArgs = kArgsA;
    cfg.centerHz = airCentre;
    cfg.vfoOffsetHz = kVfo;
    cfg.converters[kKeyA] = up(125.0e6);
    const std::size_t idx = madeCount();
    Access::restore(app, cfg);
    CHECK(madeCount() == idx + 1);
    CHECK(made(idx).args == kArgsA);
    CHECK(firstTune(idx) == airCentre + 125.0e6);
    CHECK(Access::kind(app) == "rtlsdr");
    CHECK(Access::live(app) == up(125.0e6));
    CHECK(Access::airCentre(app) == airCentre);
    CHECK(Access::counter(app) == airCentre + Access::vfo(app));
    CHECK(Access::vfo(app) == kVfo);
}

void testReopenAfterDriverFault(double airCentre, double vfo) {
    std::printf("  the reopen after a driver fault carries the air frequency through the "
                "converter (air centre %.0f Hz)\n",
                airCentre);
    resetRegistry();
    cascade::gui::AppWindow app;
    // A SoapySDR radio (no native family, so no prefer-native swap).
    const std::string args = "driver=uhd,serial=31E0000";
    Access::setConverter(app, "soapy|" + args, up(125.0e6));
    const std::size_t first = madeCount();
    CHECK(Access::selectSoapy(app, args, "B200"));
    CHECK(made(first).kind == "soapy");
    Access::setVfo(app, vfo);
    Access::tune(app, airCentre);
    CHECK(Access::airCentre(app) == airCentre);

    const std::size_t again = madeCount();
    Access::reopen(app);
    CHECK(Access::waitOpen(app));
    CHECK(madeCount() == again + 1);
    CHECK(made(again).args == args);
    CHECK(firstTune(again) == airCentre + 125.0e6);
    CHECK(Access::live(app) == up(125.0e6));
    CHECK(Access::airCentre(app) == airCentre);
    CHECK(Access::counter(app) == airCentre + vfo);
}

void testPatchTakeOverAndHandBack() {
    std::printf("  the patch page takes the radio at a negative air centre and hands it back\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    onRadioA(app, kAirCentre, kVfo);
    CHECK(Access::counter(app) == kStation);

    // A patch radio with no device yet takes the receiver's radio - and the
    // band the receiver was on, which is -283.6 kHz on the air.
    const auto id = Access::addRadioNode(app, std::string(), 0.0);
    const std::size_t patchIdx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, id));
    CHECK(Access::kind(app) == "siggen");  // the receiver stands in on the generator
    const cascade::core::patch::Node* n = Access::node(app, id);
    CHECK(n != nullptr);
    if (n != nullptr) {
        CHECK(n->device == kKeyA);
        CHECK(n->freqHz == kAirCentre);
    }
    CHECK(madeCount() == patchIdx + 1);
    CHECK(made(patchIdx).args == kArgsA);
    CHECK(firstTune(patchIdx) == kAirCentre + 125.0e6);
    // ...told by the OPEN, before its stream started, not a frame later by
    // the running radio's follow.
    CHECK(made(patchIdx).tunesAtStart >= 1);
    // Told once, and never moved by the running radio's follow.
    for (const double t : made(patchIdx).tunes) { CHECK(t == kAirCentre + 125.0e6); }
    CHECK(Access::patchRadioAir(app, id) == kAirCentre);

    // STOP: the receiver gets radio A back, on the same band.
    const std::size_t back = madeCount();
    CHECK(Access::stopPatch(app));
    CHECK(madeCount() == back + 1);
    CHECK(made(back).args == kArgsA);
    CHECK(firstTune(back) == kAirCentre + 125.0e6);
    CHECK(Access::kind(app) == "rtlsdr");
    CHECK(Access::live(app) == up(125.0e6));
    CHECK(Access::airCentre(app) == kAirCentre);
    CHECK(Access::counter(app) == kStation);
}

void testPatchOpen() {
    std::printf("  a patch radio is told its node's air frequency through its converter\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyB, up(100.0e6));
    const auto id = Access::addRadioNode(app, kKeyB, kStation);
    const std::size_t idx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, id));
    CHECK(madeCount() == idx + 1);
    CHECK(made(idx).args == kArgsB);
    CHECK(firstTune(idx) == kStation + 100.0e6);
    CHECK(made(idx).tunesAtStart >= 1);  // by the open, before the stream
    CHECK(Access::patchRadioAir(app, id) == kStation);
}

void testSoapyFallbackKeepsTheConverter(bool viaRestore) {
    std::printf("  a native open that falls back to SoapySDR keeps the radio's converter (%s)\n",
                viaRestore ? "startup restore" : "Source combo");
    resetRegistry();
    const std::string nativeArgs = "serial=0000E400";
    const std::string soapyArgs = "driver=rtlsdr,serial=0000E400";
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.native = {{"rtlsdr", "Generic RTL2832U (E4000)", nativeArgs}};
    }
    g_reg.nativeRtlRefuses = true;
    cascade::gui::AppWindow app;
    // Set for the radio the user picked - reached natively, which is the key
    // the Source section stores it under.
    Access::setConverter(app, "rtlsdr|" + nativeArgs, up(125.0e6));
    const std::size_t idx = madeCount();
    if (viaRestore) {
        cascade::core::AppConfig cfg;
        cfg.sourceKind = "soapy";
        cfg.soapyArgs = soapyArgs;
        cfg.centerHz = kStation;
        cfg.vfoOffsetHz = 0.0;
        cfg.converters["rtlsdr|" + nativeArgs] = up(125.0e6);
        Access::restore(app, cfg);
    } else {
        Access::setVfo(app, 0.0);
        Access::tune(app, kStation);  // the generator, carried across
        CHECK(Access::airCentre(app) == kStation);
        Access::scanNativeForTest(app);
        CHECK(Access::selectSoapy(app, soapyArgs, "Generic RTL2832U"));
    }
    // The native driver was tried and refused; SoapySDR opened the dongle.
    CHECK(madeCount() == idx + 2);
    CHECK(made(idx).kind == "rtlsdr");
    CHECK(made(idx).tunes.empty());
    CHECK(made(idx + 1).kind == "soapy");
    CHECK(made(idx + 1).args == soapyArgs);
    CHECK(Access::kind(app) == "soapy");
    // ...and the converter came with it: the air frequency was NOT sent raw.
    CHECK(firstTune(idx + 1) == kStation + 125.0e6);
    CHECK(Access::live(app) == up(125.0e6));
    CHECK(Access::airCentre(app) == kStation);
    CHECK(!Access::aliasNote(app).empty());
}

void testSwitchingOnRelabelsANegativeCentre() {
    std::printf("  switching a converter on relabels a band whose centre is below 0 Hz on the "
                "air\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    // Radio A with NO converter yet, already on the converter's output: the
    // radio at 124.7164 MHz with the VFO 300 kHz up - the tester's dongle,
    // tuned by hand to hear 16.4 kHz.
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    Access::setVfo(app, kVfo);
    Access::tune(app, 124716400.0);
    const std::size_t toldBefore = made(idx).tunes.size();
    Access::changeConverter(app, up(125.0e6));
    // The radio stays where it is; the counter now says what it hears.
    CHECK(made(idx).tunes.size() == toldBefore);
    CHECK(Access::airCentre(app) == kAirCentre);
    CHECK(Access::counter(app) == kStation);
}

}  // namespace

int main() {
    std::printf("test_converter_app_paths\n");
    isolate();
    Access::installHooks();

    testDeviceSwitch(kAirCentre, kVfo);   // the negative air centre
    testDeviceSwitch(kStation, 0.0);      // and an ordinary positive one
    testStartupRestore(kAirCentre);
    testStartupRestore(kStation);
    testReopenAfterDriverFault(kAirCentre, kVfo);
    testReopenAfterDriverFault(kStation, 0.0);
    testPatchTakeOverAndHandBack();
    testPatchOpen();
    testSoapyFallbackKeepsTheConverter(false);
    testSoapyFallbackKeepsTheConverter(true);
    testSwitchingOnRelabelsANegativeCentre();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_converter_app_paths");
}
