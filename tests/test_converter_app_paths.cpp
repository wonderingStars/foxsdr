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
#include <cstdint>
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
    // Radios made while this is set read 0 Hz until a tune takes - the
    // "never tuned" readback carriedAirCentre must treat as no frequency.
    std::atomic<bool> startAtZero{false};
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
    // Where an RTL-SDR's driver leaves it, or 0 Hz for a radio made to read
    // "never tuned" (Registry::startAtZero).
    double centre_ = g_reg.startAtZero.load() ? 0.0 : 100.0e6;
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
    g_reg.startAtZero = false;
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
    static bool hasStored(AppWindow& a, const std::string& k) { return a.converters_.count(k) != 0; }
    static ConverterSetting stored(AppWindow& a, const std::string& k) {
        const auto it = a.converters_.find(k);
        return it == a.converters_.end() ? ConverterSetting{} : it->second;
    }
    static void selectGenerator(AppWindow& a) { a.selectSource(0); }
    // What the radio itself was last told and kept (the raw source).
    static double radioNow(AppWindow& a) { return a.pipeline_.rawSource().centerFrequencyHz(); }
    // The note line under the counter (a coerced, refused or unreachable tune).
    static std::string tuneNote(AppWindow& a) { return a.tuneMismatchNote_; }

    // The Pluto row: selecting it only selects (see selectSource); Open
    // closes the radio in use and opens the board at the typed address.
    static bool selectPlutoRow(AppWindow& a, const std::string& args) {
        a.scanNative();
        const int row = nativeRow(a, args);
        if (row < 0) { return false; }
        a.selectSource(row);
        return a.sourceSel_ == row;
    }
    static bool openPluto(AppWindow& a, const std::string& uri) {
        std::snprintf(a.plutoUri_, sizeof(a.plutoUri_), "%s", uri.c_str());
        a.openPlutoFromBox();
        return waitOpen(a);
    }

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
    // The page's starter patch, seeded as opening the page seeds it; returns
    // its Radio node (kNoNode when there is none).
    static cascade::core::patch::NodeId seedPatch(AppWindow& a) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = false;
        a.seedPatchIfNeeded();
        for (const pc::Node& n : a.patchGraph_.nodes()) {
            if (n.kind == pc::NodeKind::Radio) { return n.id; }
        }
        return pc::kNoNode;
    }
    // A centre typed on the node's face (or the panel): true when taken.
    static bool typeCentre(AppWindow& a, cascade::core::patch::NodeId id, double airHz) {
        cascade::core::patch::Node* n = a.patchGraph_.mutableNode(id);
        return n != nullptr && a.setPatchRadioCentre(*n, airHz);
    }
    static std::string centreNote(AppWindow& a, cascade::core::patch::NodeId id) {
        const auto it = a.patchCentreNote_.find(id);
        return it == a.patchCentreNote_.end() ? std::string() : it->second;
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

// --- A converter change keeps the AIR frequency ------------------------------
//
// THE RULE (orchestrator's decision after the second review): on ANY change -
// on, off, mode, LO, inversion - the air frequency the user is listening to
// stays, and the radio is retuned to what the new setting makes of it, when
// the radio can go there. Only when it cannot does the radio stay put and the
// counter relabel, with the sentence saying what the radio reaches.

void testSwitchingOnKeepsTheAirFrequency() {
    std::printf("  switching a converter on keeps the air frequency and moves the radio\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    // Radio A with NO converter yet: 124.7164 MHz on the air, VFO 300 kHz up.
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    Access::setVfo(app, kVfo);
    Access::tune(app, 124716400.0);
    CHECK(Access::counter(app) == 125016400.0);
    Access::changeConverter(app, up(125.0e6));
    // 124.7164 MHz on the air through a 125 MHz up-converter: 249.7164 MHz.
    CHECK(made(idx).tunes.back() == 249716400.0);
    CHECK(Access::radioNow(app) == 249716400.0);
    CHECK(Access::airCentre(app) == 124716400.0);
    CHECK(Access::counter(app) == 125016400.0);
    CHECK(Access::tuneNote(app).empty());
}

// PROBE P2 of the second review: an LO typo moved the radio, and correcting
// it only relabelled - the station was lost.
void testLoTypoRoundTrip() {
    std::printf("  an LO typo and its correction come back to the same air and radio frequency\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyA, up(125.0e6));
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    Access::setVfo(app, 0.0);
    Access::tune(app, 17200.0);
    CHECK(made(idx).tunes.back() == 125017200.0);
    Access::changeConverter(app, up(1250.0e6));    // the typo
    CHECK(Access::radioNow(app) == 1250017200.0);
    CHECK(Access::counter(app) == 17200.0);
    Access::changeConverter(app, up(125.0e6));     // corrected
    CHECK(made(idx).tunes.back() == 125017200.0);
    CHECK(Access::radioNow(app) == 125017200.0);
    CHECK(Access::counter(app) == 17200.0);
}

// The quick LO keys, both ways round: 125 -> 100 -> 125 and 100 -> 125 -> 100
// each end where they began, at the air AND at the radio.
void testQuickKeysRoundTrip(double startLo, double otherLo) {
    std::printf("  quick LO keys %.0f -> %.0f -> %.0f MHz return to the same frequencies\n",
                startLo / 1e6, otherLo / 1e6, startLo / 1e6);
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyA, up(startLo));
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    Access::setVfo(app, kVfo);
    Access::tune(app, kAirCentre);
    const double radioStart = kAirCentre + startLo;
    CHECK(Access::radioNow(app) == radioStart);
    CHECK(Access::counter(app) == kStation);
    Access::changeConverter(app, up(otherLo));
    CHECK(made(idx).tunes.back() == kAirCentre + otherLo);
    CHECK(Access::airCentre(app) == kAirCentre);
    CHECK(Access::counter(app) == kStation);
    Access::changeConverter(app, up(startLo));
    CHECK(made(idx).tunes.back() == radioStart);
    CHECK(Access::radioNow(app) == radioStart);
    CHECK(Access::airCentre(app) == kAirCentre);
    CHECK(Access::counter(app) == kStation);
}

// Where the new setting would put the radio outside what it covers, the
// radio stays and the counter relabels - and the note says what it reaches.
void testUnreachableChangeRelabels() {
    std::printf("  a converter change the radio cannot follow leaves it put, relabelled, and "
                "says why\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyA, up(125.0e6));
    const std::size_t idx = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    Access::setVfo(app, 0.0);
    Access::tune(app, 17200.0);
    const std::size_t told = made(idx).tunes.size();
    // 17.2 kHz through a 2 GHz LO is 2000.0172 MHz: past the dongle's top.
    Access::changeConverter(app, up(2000.0e6));
    CHECK(made(idx).tunes.size() == told);        // never asked
    CHECK(Access::radioNow(app) == 125017200.0);
    CHECK(Access::counter(app) == 125017200.0 - 2000.0e6);
    CHECK(Access::tuneNote(app).find("stayed where it was") != std::string::npos);
    // Back to 125 MHz: the station it could not keep is found again - the
    // radio never moved, nothing is sent, and nothing is left to say.
    Access::changeConverter(app, up(125.0e6));
    CHECK(made(idx).tunes.size() == told);
    CHECK(Access::radioNow(app) == 125017200.0);
    CHECK(Access::counter(app) == 17200.0);
    CHECK(Access::tuneNote(app).empty());
    // OFF: 17.2 kHz is below the dongle's 24 MHz, so the radio stays at
    // 125.0172 MHz and the counter now says so, with the radio's range.
    Access::changeConverter(app, ConverterSetting{});
    CHECK(made(idx).tunes.size() == told);
    CHECK(Access::radioNow(app) == 125017200.0);
    CHECK(Access::counter(app) == 125017200.0);
    CHECK(Access::tuneNote(app).find("Its range is") != std::string::npos);
    // ...and ON AGAIN: back on 17.2 kHz, the radio unmoved - not 125.0172 MHz
    // kept on the air and the radio sent to 250.0172 MHz.
    Access::changeConverter(app, up(125.0e6));
    CHECK(made(idx).tunes.size() == told);
    CHECK(Access::radioNow(app) == 125017200.0);
    CHECK(Access::counter(app) == 17200.0);
    CHECK(Access::tuneNote(app).empty());

    // WHAT IT REACHES, said: 100 MHz on the air is 225 MHz at the radio; a
    // 1700 MHz LO would need 1800 MHz, past the top - and through that LO the
    // dongle covers 0 Hz to 66 MHz.
    Access::tune(app, 100.0e6);
    CHECK(Access::radioNow(app) == 225.0e6);
    Access::changeConverter(app, up(1700.0e6));
    CHECK(Access::radioNow(app) == 225.0e6);
    CHECK(Access::counter(app) == 225.0e6 - 1700.0e6);
    CHECK(Access::tuneNote(app).find("reaches") != std::string::npos);
    Access::changeConverter(app, up(125.0e6));
    CHECK(Access::radioNow(app) == 225.0e6);
    CHECK(Access::counter(app) == 100.0e6);
    // A 125 MHz DOWN-converter would need -25 MHz at the radio.
    Access::changeConverter(app, {ConverterMode::Down, 125.0e6, false});
    CHECK(Access::radioNow(app) == 225.0e6);
    CHECK(Access::counter(app) == 350.0e6);
    CHECK(Access::tuneNote(app).find("out of reach") != std::string::npos);
    Access::changeConverter(app, up(125.0e6));
    CHECK(Access::radioNow(app) == 225.0e6);
    CHECK(Access::counter(app) == 100.0e6);
    CHECK(Access::tuneNote(app).empty());
    // A TUNE ends the hold: the relabelled figure after it is the station.
    Access::changeConverter(app, up(1700.0e6));     // unreachable: held 100 MHz
    Access::tune(app, 225.0e6 - 1700.0e6 + 10.0e6); // the radio moves to 235 MHz
    CHECK(Access::radioNow(app) == 235.0e6);
    Access::changeConverter(app, up(125.0e6));      // keeps -1465 MHz: unreachable
    CHECK(Access::radioNow(app) == 235.0e6);
    CHECK(Access::counter(app) == 110.0e6);
}

// An I/Q FILE is not a radio: its frequency is where the recording was made,
// and a converter set for it relabels - that is the whole of what it is for.
void testFileRelabels() {
    std::printf("  a converter set on an I/Q file relabels it\n");
    resetRegistry();
    const std::filesystem::path wav = g_scratch / "converter_relabel.wav";
    {
        // 16-bit stereo PCM WAV, 48 kS/s, 4800 frames of silence.
        const std::uint32_t frames = 4800;
        const std::uint32_t dataBytes = frames * 4;
        std::FILE* f = std::fopen(wav.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f == nullptr) { return; }
        const auto u32 = [f](std::uint32_t v) {
            const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                        static_cast<unsigned char>(v >> 16),
                                        static_cast<unsigned char>(v >> 24)};
            std::fwrite(b, 1, 4, f);
        };
        const auto u16 = [f](std::uint16_t v) {
            const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
            std::fwrite(b, 1, 2, f);
        };
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(2);
        u32(48000);
        u32(48000 * 4);
        u16(4);
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        const std::vector<unsigned char> zeros(dataBytes, 0);
        std::fwrite(zeros.data(), 1, zeros.size(), f);
        std::fclose(f);
    }
    cascade::gui::AppWindow app;
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "file";
    cfg.iqFilePath = wav.string();
    cfg.centerHz = 125017200.0;
    cfg.vfoOffsetHz = 0.0;
    Access::restore(app, cfg);
    CHECK(Access::kind(app) == "file");
    CHECK(Access::radioNow(app) == 125017200.0);
    Access::changeConverter(app, up(125.0e6));
    CHECK(Access::radioNow(app) == 125017200.0);   // the recording's own figure
    CHECK(Access::counter(app) == 17200.0);        // now read through the converter
}

// PROBE P1 of the second review: the receiver at EXACTLY 0 Hz on the air (the
// dongle on 125 MHz behind a 125 MHz up-converter) is a frequency, and the
// patch take-over must carry it - the node's stored 0 is not "no centre" here.
void testZeroAirCentreTakeOver() {
    std::printf("  the patch take-over carries an air centre of exactly 0 Hz\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    onRadioA(app, 0.0, kStation);
    CHECK(Access::counter(app) == kStation);
    const auto id = Access::addRadioNode(app, std::string(), 0.0);
    const std::size_t patchIdx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, id));
    CHECK(firstTune(patchIdx) == 125.0e6);
    for (const double t : made(patchIdx).tunes) { CHECK(t == 125.0e6); }
    CHECK(Access::patchRadioAir(app, id) == 0.0);
    const cascade::core::patch::Node* n = Access::node(app, id);
    CHECK(n != nullptr && n->freqHz == 0.0);
}

// The page's starter patch takes the receiver's radio at its AIR centre - a
// negative one (N13 of the second review), and exactly 0 Hz (P1 again).
void testPatchSeedCarriesTheAirCentre(double airCentre, double vfo) {
    std::printf("  the starter patch takes the receiver's air centre (%.0f Hz)\n", airCentre);
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    onRadioA(app, airCentre, vfo);
    const auto id = Access::seedPatch(app);
    const cascade::core::patch::Node* n = Access::node(app, id);
    CHECK(n != nullptr);
    if (n != nullptr) {
        CHECK(n->device == kKeyA);
        CHECK(n->freqHz == airCentre);
    }
    const std::size_t patchIdx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, id));
    CHECK(firstTune(patchIdx) == airCentre + 125.0e6);
    for (const double t : made(patchIdx).tunes) { CHECK(t == airCentre + 125.0e6); }
    CHECK(Access::patchRadioAir(app, id) == airCentre);
}

// A node that already has a centre of its own keeps it when it takes the
// receiver's radio (N6 of the second review).
void testTakeOverKeepsANodesOwnCentre() {
    std::printf("  the patch take-over keeps a node's own centre\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    onRadioA(app, kAirCentre, kVfo);
    const auto id = Access::addRadioNode(app, std::string(), kStation);
    const std::size_t patchIdx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, id));
    const cascade::core::patch::Node* n = Access::node(app, id);
    CHECK(n != nullptr);
    if (n != nullptr) {
        CHECK(n->device == kKeyA);
        CHECK(n->freqHz == kStation);
    }
    CHECK(firstTune(patchIdx) == kStation + 125.0e6);
    CHECK(Access::patchRadioAir(app, id) == kStation);
}

// A radio that reads 0 Hz has never been tuned and has nothing to carry: the
// next radio is told nothing, not "0 Hz at the radio, read through the
// converter" (N1 of the second review).
void testUntunedRadioCarriesNothing() {
    std::printf("  a radio that was never tuned carries no frequency to the next one\n");
    resetRegistry();
    twoDongles();
    g_reg.startAtZero = true;
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyA, up(125.0e6));   // B has none
    // The generator on 2 GHz: through A's converter that is 2.125 GHz, which
    // A refuses - so A stays on its never-tuned 0 Hz.
    Access::tune(app, 2.0e9);
    const std::size_t a = madeCount();
    CHECK(Access::selectNative(app, kArgsA));
    CHECK(!made(a).tunes.empty());
    CHECK(Access::radioNow(app) == 0.0);
    const std::size_t b = madeCount();
    CHECK(Access::selectNative(app, kArgsB));
    CHECK(made(b).args == kArgsB);
    CHECK(made(b).tunes.empty());
}

// The Pluto's Open key reads the frequency to carry BEFORE it closes the radio
// in use (N9 of the second review), and sends it through the Pluto's own
// converter.
void testPlutoCarriesTheAirFrequency() {
    std::printf("  the Pluto's Open carries the air frequency read before the close\n");
    resetRegistry();
    const std::string plutoArgs = "uri=ip:192.168.2.1";
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kArgsA},
                        {"rtlsdr", "Generic RTL2832U B", kArgsB},
                        {"pluto", "ADALM-Pluto", plutoArgs}};
    }
    cascade::gui::AppWindow app;
    onRadioA(app, kAirCentre, kVfo);
    Access::setConverter(app, "pluto|" + plutoArgs, up(100.0e6));
    CHECK(Access::selectPlutoRow(app, plutoArgs));
    const std::size_t pl = madeCount();
    CHECK(Access::openPluto(app, "ip:192.168.2.1"));
    CHECK(madeCount() == pl + 1);
    CHECK(made(pl).kind == "pluto");
    CHECK(firstTune(pl) == kAirCentre + 100.0e6);
    CHECK(Access::live(app) == up(100.0e6));
    CHECK(Access::airCentre(app) == kAirCentre);
    CHECK(Access::counter(app) == kStation);
}

// --- The SoapySDR fallback's alias, at its edges -----------------------------

// Opens the E4000 dongle the way the Source combo does (the Soapy row, swapped
// to the native driver, refused, reopened through SoapySDR) and returns the
// index of the SoapySDR radio.
std::size_t fallBackToSoapy(cascade::gui::AppWindow& app, const std::string& nativeArgs,
                            const std::string& soapyArgs) {
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.native = {{"rtlsdr", "Generic RTL2832U (E4000)", nativeArgs}};
    }
    g_reg.nativeRtlRefuses = true;
    Access::setVfo(app, 0.0);
    Access::tune(app, kStation);  // the generator, carried across
    Access::scanNativeForTest(app);
    const std::size_t idx = madeCount();
    CHECK(Access::selectSoapy(app, soapyArgs, "Generic RTL2832U"));
    CHECK(madeCount() == idx + 2);
    CHECK(made(idx).kind == "rtlsdr");
    CHECK(made(idx + 1).kind == "soapy");
    CHECK(Access::kind(app) == "soapy");
    return idx + 1;
}

// PROBE P3: an edit made while the alias is in force is stored under the
// NATIVE key (where the next native open looks) and takes effect at once.
void testAliasEditLandsUnderTheNativeKey() {
    std::printf("  a converter edit while aliased lands under the native key and applies\n");
    resetRegistry();
    const std::string nativeArgs = "serial=0000E400";
    const std::string soapyArgs = "driver=rtlsdr,serial=0000E400";
    cascade::gui::AppWindow app;
    Access::setConverter(app, "rtlsdr|" + nativeArgs, up(125.0e6));
    const std::size_t s = fallBackToSoapy(app, nativeArgs, soapyArgs);
    CHECK(firstTune(s) == kStation + 125.0e6);
    CHECK(Access::live(app) == up(125.0e6));
    Access::changeConverter(app, up(100.0e6));
    CHECK(Access::stored(app, "rtlsdr|" + nativeArgs) == up(100.0e6));
    CHECK(!Access::hasStored(app, "soapy|" + soapyArgs));
    CHECK(Access::live(app) == up(100.0e6));
    CHECK(made(s).tunes.back() == kStation + 100.0e6);
    CHECK(Access::counter(app) == kStation);
    // A different radio afterwards gets none of it.
    Access::selectGenerator(app);
    CHECK(Access::selectSoapy(app, "driver=uhd,serial=31E0000", "B200"));
    CHECK(Access::live(app) == ConverterSetting{});
}

// A Soapy key with an ACTIVE converter of its own keeps it (N3).
void testSoapyKeyKeepsItsOwnConverter() {
    std::printf("  a SoapySDR key with a converter of its own keeps it on the fallback\n");
    resetRegistry();
    const std::string nativeArgs = "serial=0000E400";
    const std::string soapyArgs = "driver=rtlsdr,serial=0000E400";
    cascade::gui::AppWindow app;
    Access::setConverter(app, "rtlsdr|" + nativeArgs, up(125.0e6));
    Access::setConverter(app, "soapy|" + soapyArgs, up(100.0e6));
    const std::size_t s = fallBackToSoapy(app, nativeArgs, soapyArgs);
    CHECK(firstTune(s) == kStation + 100.0e6);
    CHECK(Access::live(app) == up(100.0e6));
    CHECK(Access::aliasNote(app).empty());
}

// ...but an OFF record on the Soapy key is "none" (item 5a): the native
// converter is carried, not silently dropped.
void testSoapyOffRecordIsNone() {
    std::printf("  an Off record on the SoapySDR key does not block the radio's converter\n");
    resetRegistry();
    const std::string nativeArgs = "serial=0000E400";
    const std::string soapyArgs = "driver=rtlsdr,serial=0000E400";
    cascade::gui::AppWindow app;
    Access::setConverter(app, "rtlsdr|" + nativeArgs, up(125.0e6));
    Access::setConverter(app, "soapy|" + soapyArgs, {ConverterMode::Off, 100.0e6, false});
    const std::size_t s = fallBackToSoapy(app, nativeArgs, soapyArgs);
    CHECK(firstTune(s) == kStation + 125.0e6);
    CHECK(Access::live(app) == up(125.0e6));
    CHECK(!Access::aliasNote(app).empty());
}

// With NO serial in the Soapy args, SoapySDR may open a different dongle from
// the one the native row named (item 5b): the converter is not carried, and
// the note says so.
void testNoSerialNoAlias() {
    std::printf("  a SoapySDR fallback with no serial does not carry the converter, and says so\n");
    resetRegistry();
    const std::string nativeArgs = "serial=0000E400";
    const std::string soapyArgs = "driver=rtlsdr";
    cascade::gui::AppWindow app;
    Access::setConverter(app, "rtlsdr|" + nativeArgs, up(125.0e6));
    const std::size_t s = fallBackToSoapy(app, nativeArgs, soapyArgs);
    CHECK(firstTune(s) == kStation);               // raw: no converter applied
    CHECK(Access::live(app) == ConverterSetting{});
    const std::string note = Access::aliasNote(app);
    CHECK(note.find("not carried") != std::string::npos);
    // An edit made here is this key's own, not the native radio's.
    Access::changeConverter(app, up(100.0e6));
    CHECK(Access::stored(app, "rtlsdr|" + nativeArgs) == up(125.0e6));
    CHECK(Access::stored(app, "soapy|" + soapyArgs) == up(100.0e6));
    CHECK(Access::aliasNote(app).find("not carried") == std::string::npos);
}

// The rule under the alias, on its own: only a serial on the Soapy side that
// matches the native row's settles "the same dongle".
void testFallbackSameDongleRule() {
    std::printf("  a SoapySDR fallback is the same dongle only by a matching serial\n");
    using cascade::gui::fallbackNamesTheSameDongle;
    CHECK(fallbackNamesTheSameDongle("rtlsdr|serial=0000E400", "soapy|driver=rtlsdr,serial=0000E400"));
    // Case and the long/short forms match as the prefer-native rule matches them.
    CHECK(fallbackNamesTheSameDongle("rtlsdr|serial=0000e400", "soapy|driver=rtlsdr,serial=0000E400"));
    CHECK(fallbackNamesTheSameDongle("rtlsdr|serial=0000E400", "soapy|driver=rtlsdr,serial=E400"));
    // No serial on the Soapy side: SoapySDR picks whichever dongle it finds.
    CHECK(!fallbackNamesTheSameDongle("rtlsdr|serial=0000E400", "soapy|driver=rtlsdr"));
    // A different serial is a different dongle; none on the native side too.
    CHECK(!fallbackNamesTheSameDongle("rtlsdr|serial=0000E400", "soapy|driver=rtlsdr,serial=0000F00D"));
    CHECK(!fallbackNamesTheSameDongle("rtlsdr|", "soapy|driver=rtlsdr,serial=0000E400"));
    CHECK(!fallbackNamesTheSameDongle("", "soapy|driver=rtlsdr,serial=0000E400"));
}

// --- A centre typed on a Radio node (item 6) ----------------------------------

void testTypedNodeCentre() {
    std::printf("  a Radio node takes any centre its radio can be told, and refuses the rest\n");
    resetRegistry();
    twoDongles();
    cascade::gui::AppWindow app;
    Access::setConverter(app, kKeyA, up(125.0e6));   // B has none
    const auto a = Access::addRadioNode(app, kKeyA, kStation);
    const auto b = Access::addRadioNode(app, kKeyB, 100.0e6);
    // Behind the 125 MHz up-converter: -283.6 kHz is 124.7164 MHz at the radio.
    CHECK(Access::typeCentre(app, a, kAirCentre));
    CHECK(Access::node(app, a)->freqHz == kAirCentre);
    CHECK(Access::centreNote(app, a).empty());
    // -125 MHz would be 0 Hz at the radio, and -200 MHz below it: refused,
    // said, and the node keeps what it had.
    CHECK(!Access::typeCentre(app, a, -125.0e6));
    CHECK(!Access::centreNote(app, a).empty());
    CHECK(!Access::typeCentre(app, a, -200.0e6));
    CHECK(Access::node(app, a)->freqHz == kAirCentre);
    // 0 Hz on the air is 125 MHz at the radio: a centre, and it clears the note.
    CHECK(Access::typeCentre(app, a, 0.0));
    CHECK(Access::node(app, a)->freqHz == 0.0);
    CHECK(Access::centreNote(app, a).empty());
    // With no converter the radio frequency IS the air one: above 0 Hz only.
    CHECK(!Access::typeCentre(app, b, 0.0));
    CHECK(!Access::typeCentre(app, b, -1.0));
    CHECK(!Access::centreNote(app, b).empty());
    CHECK(Access::node(app, b)->freqHz == 100.0e6);
    CHECK(Access::typeCentre(app, b, 1.0e6));
    CHECK(Access::node(app, b)->freqHz == 1.0e6);
    // ...and the 0 Hz centre on A is what A's radio is told when it opens.
    const std::size_t idx = madeCount();
    CHECK(Access::runPatchUntilOpen(app, a));
    std::size_t aIdx = idx;
    for (std::size_t i = idx; i < madeCount(); ++i) {
        if (made(i).args == kArgsA) { aIdx = i; }
    }
    CHECK(made(aIdx).args == kArgsA);
    CHECK(firstTune(aIdx) == 125.0e6);
    CHECK(Access::patchRadioAir(app, a) == 0.0);
}

// 0.99.36 TUNES AN RSP BEFORE ITS STREAM STARTS (the pre-Init tune the service
// answers), and 0.99.37 merged that with the converter: the frequency carried
// is an AIR one, so the RSP must be told it THROUGH ITS OWN CONVERTER, before
// start - never the air figure raw. The release merge found the raw write.
void testRspPreTuneGoesThroughItsConverter(double airCentre, double vfo) {
    std::printf("  an RSP is told the carried frequency through its converter before its stream "
                "starts (air centre %.0f Hz)\n",
                airCentre);
    resetRegistry();
    const std::string rspArgs = "serial=RSP0000A";
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kArgsA},
                        {"sdrplay", "SDRplay RSP1A", rspArgs}};
    }
    cascade::gui::AppWindow app;
    onRadioA(app, airCentre, vfo);
    Access::setConverter(app, "sdrplay|" + rspArgs, up(125.0e6));
    const std::size_t r = madeCount();
    CHECK(Access::selectNative(app, rspArgs));
    CHECK(made(r).kind == "sdrplay");
    // The first thing the RSP heard is the radio frequency, and it was never
    // started before hearing it (tunesAtStart: -1 = not started in this test,
    // 0 would mean streamed at its default before the first tune).
    CHECK(firstTune(r) == airCentre + 125.0e6);
    CHECK(made(r).tunesAtStart != 0);
    CHECK(Access::airCentre(app) == airCentre);
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
    testSwitchingOnKeepsTheAirFrequency();
    testLoTypoRoundTrip();
    testQuickKeysRoundTrip(125.0e6, 100.0e6);
    testQuickKeysRoundTrip(100.0e6, 125.0e6);
    testUnreachableChangeRelabels();
    testFileRelabels();
    testZeroAirCentreTakeOver();
    testPatchSeedCarriesTheAirCentre(kAirCentre, kVfo);
    testPatchSeedCarriesTheAirCentre(0.0, kStation);
    testTakeOverKeepsANodesOwnCentre();
    testUntunedRadioCarriesNothing();
    testPlutoCarriesTheAirFrequency();
    testAliasEditLandsUnderTheNativeKey();
    testSoapyKeyKeepsItsOwnConverter();
    testSoapyOffRecordIsNone();
    testNoSerialNoAlias();
    testFallbackSameDongleRule();
    testTypedNodeCentre();
    testRspPreTuneGoesThroughItsConverter(kAirCentre, kVfo);
    testRspPreTuneGoesThroughItsConverter(kStation, 0.0);

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_converter_app_paths");
}
