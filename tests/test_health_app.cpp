// THE FAILURE COUNTS INSIDE THE REAL APPLICATION (core/health_events.hpp, 0.99.64).
//
// test_health_events.cpp holds the vocabulary and the ledger; test_health_paths.cpp
// drives the failures that live below the window (a sound output, a driver, the
// updater, the plugin installer, the plugin host, a recording). What only the
// application can show is the WIRING: that the places AppWindow decides things -
// the worker that opens a radio, the restore of the saved radio, the patch page's
// radios, the scan, the update and the plugin rescan - are the places that count,
// that "samples flowed" and "the speakers played" are read off state the window
// already has, and that the real binary arms, journals, keeps and clears the
// ledger, and does not lose a failure to a process that is ended. It drives the
// real AppWindow through the same friend and hooks the other *_app tests use
// (AppWindow::testHooks_), with the radio a REAL RTL-SDR driver asked for a serial that is
// not there where a refusal is wanted and a fake that opens where a success is,
// and the real cascade binary for the file and the crash.
//
// Hermetic like test_converter_app_paths: per-user directories pointed at a
// scratch folder, telemetry at a black hole, and nothing reaches a network.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/health_events.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/telemetry.hpp"
#include "gui/app_window.hpp"
#include "sink/audio_out.hpp"
#include "source/device_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using health::Counts;
using health::HealthLedger;

namespace {

// --- the radios -------------------------------------------------------------------

struct Script {
    // The real RTL-SDR driver, asked for a serial that is not there: its refusal is the driver's own.
    std::atomic<bool> realRtl{false};
};
Script g_script;

class FakeRadio final : public cascade::source::DeviceSource {
public:
    explicit FakeRadio(std::string kind) : kind_(std::move(kind)) {}
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
    const char* lastError() const override { return ""; }
    const char* driverKey() const override { return kind_.c_str(); }
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
    std::string kind_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
};

std::unique_ptr<cascade::source::DeviceSource> makeRadio(const std::string& kind) {
    if (kind == "rtlsdr" && g_script.realRtl.load()) {
        return std::make_unique<cascade::source::RtlSdrSource>();
    }
    return std::make_unique<FakeRadio>(kind);
}

// --- the sound cards -----------------------------------------------------------------

struct CardWorld {
    std::mutex m;
    std::vector<cascade::source::SoundCardDevice> devices;
    std::atomic<bool> refuse{false};
};
CardWorld g_cards;

class FakeCard final : public cascade::source::SoundCardBackend {
public:
    ~FakeCard() override { close(); }
    std::vector<cascade::source::SoundCardDevice> listDevices() override {
        std::lock_guard<std::mutex> lk(g_cards.m);
        return g_cards.devices;
    }
    bool open(const cascade::source::SoundCardDevice&, int, double, bool, PushFn, void*,
              std::string& error) override {
        if (g_cards.refuse.load()) {
            error = "fake refused";
            return false;
        }
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool alive() override { return open_.load(); }

private:
    std::atomic<bool> open_{false};
};

std::shared_ptr<cascade::source::SoundCardBackend> makeCard() { return std::make_shared<FakeCard>(); }

std::mutex g_nativeMutex;
std::vector<cascade::source::NativeDeviceInfo> g_native;

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_nativeMutex);
    return g_native;
}
void setNative(std::vector<cascade::source::NativeDeviceInfo> v) {
    std::lock_guard<std::mutex> lk(g_nativeMutex);
    g_native = std::move(v);
}

const std::string kArgs = "serial=0000000A";
// A count by token, 0 when it is not there - a missing token is a failed check,
// never an exception that ends the run (and hides every check after it).
std::uint32_t countOf(const Counts& c, const std::string& token) {
    const auto it = c.find(token);
    return it == c.end() ? 0u : it->second;
}


// --- isolation and small helpers --------------------------------------------------

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) { ::setenv(name, value, 1); } else { ::unsetenv(name); }
#endif
}

fs::path g_scratch;

unsigned long pidNow() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

void isolate() {
    g_scratch = fs::temp_directory_path() / ("foxsdr_health_app_" + std::to_string(pidNow()));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s.c_str());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/api/update");   // refused before any socket
    // The Soapy scan's child is the real application, beside this test.
    const std::string helper = std::string(CASCADE_APP_BINDIR) +
#if defined(_WIN32)
                               "\\cascade.exe";
#else
                               "/cascade";
#endif
    setEnv("CASCADE_ENUM_HELPER", helper.c_str());
}

std::shared_ptr<HealthLedger> fresh() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    return g;
}

// What the ledger holds under `prefix` - a driver's refusal is the driver's own
// sentence, so its reason is bind or absent depending on what the machine has.
std::string only(const std::shared_ptr<HealthLedger>& g, const char* prefix) {
    std::string found;
    for (const auto& kv : g->counts()) {
        if (kv.first.rfind(prefix, 0) == 0) {
            found += (found.empty() ? "" : ",") + kv.first + "=" + std::to_string(kv.second);
        }
    }
    return found;
}

// A count that is not what was wanted says what it WAS.
#define EXPECT_COUNTS(ledger, ...)                                                       \
    do {                                                                                 \
        const Counts want_ = __VA_ARGS__;                                                \
        const Counts got_ = (ledger)->counts();                                          \
        if (got_ != want_) {                                                             \
            std::printf("FAIL %s:%d counted \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, \
                        health::encode(got_).c_str(), health::encode(want_).c_str());    \
        }                                                                                \
        CHECK(got_ == want_);                                                            \
    } while (0)

bool isBindOrAbsent(const std::string& s, const char* driver) {
    return s == std::string("radio_fail.") + driver + ".bind=1" ||
           s == std::string("radio_fail.") + driver + ".absent=1";
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeRadio;
        AppWindow::testHooks_.nativeScan = &fakeScan;
        AppWindow::testHooks_.soundCardBackend = &makeCard;
    }
    // The Source section's Open on the Sound card row, settled as the frame loop settles it.
    static bool openCard(AppWindow& a, const cascade::source::SoundCardSettings& s) {
        a.sourceSel_ = AppWindow::kSoundCardRow;
        a.soundCard_ = s;
        a.launchSoundCardOpen(false, s);
        const auto t0 = std::chrono::steady_clock::now();
        while (a.soundCardOpenPending_ || a.soundCardScanPending_) {
            a.pollSoundCard();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
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
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static bool watching(AppWindow& a) { return a.healthWatching_; }
    static void poll(AppWindow& a) { a.healthPoll(); }
    static void start(AppWindow& a) { a.startReceiver(); }
    static void stop(AppWindow& a) { a.stopReceiver(); }
    static cascade::sink::AudioOut& audio(AppWindow& a) { return a.pipeline_.audio(); }

    // The patch page's radio.
    static cascade::core::patch::NodeId addRadioNode(AppWindow& a, const std::string& device) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;
        const pc::NodeId id = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        if (pc::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = device;
            n->freqHz = 100.0e6;
            n->rateHz = 2.4e6;
            n->on = true;
        }
        return id;
    }
    // One reconcile per frame until the radio runs or its open has failed.
    static bool patchUntilSettled(AppWindow& a, cascade::core::patch::NodeId id) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;
        const auto t0 = std::chrono::steady_clock::now();
        while (a.patchRadios_.count(id) == 0 && a.patchRadioError_.count(id) == 0) {
            a.patchReconcile();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static void stopPatch(AppWindow& a) {
        a.patchRunning_ = false;
        a.patchStopAll(true);
        waitOpen(a);
    }

    // The scan (both halves, as the Source list's Refresh asks).
    static bool scanBoth(AppWindow& a) {
        a.scanNative();
        a.scanSoapy();
        const auto t0 = std::chrono::steady_clock::now();
        while (a.soapyScanPending_) {
            a.pollSourceAsync();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
    static bool listed(AppWindow& a) { return !a.soapyDevices_.empty() || !a.nativeDevices_.empty(); }

    // The update check.
    static bool check(AppWindow& a) {
        a.updateCheckEnabled_ = true;
        a.startUpdateCheck();
        const auto t0 = std::chrono::steady_clock::now();
        while (a.updatePending_) {
            a.pollUpdateAsync();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
    static bool launch(AppWindow& a, const std::string& path) { return a.launchInstaller(path); }

    static void rescan(AppWindow& a) { a.rescanPlugins(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// ---------------------------------------------------------------------------
// A RADIO THAT WOULD NOT OPEN, on every way the application opens one.
// ---------------------------------------------------------------------------
void testTheSourceListsOpenCountsFailureAndSuccess() {
    // THE REAL RTL-SDR DRIVER, asked for a serial that is not there: the worker that
    // opens the radio the Source list chose counts its own refusal.
    {
        auto g = fresh();
        g_script.realRtl = true;
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        CHECK(Access::selectNative(app, kArgs));
        const std::string fail = only(g, "radio_fail.");
        std::printf("health: the real RTL-SDR driver, serial not there: %s\n", fail.c_str());
        CHECK(isBindOrAbsent(fail, "rtlsdr"));
        CHECK(only(g, "radio_open.").empty());
        CHECK(!Access::watching(app));    // nothing opened, nothing to watch for
        g_script.realRtl = false;
    }
    // A RADIO THAT OPENS is the denominator, and the watch for its samples starts.
    {
        auto g = fresh();
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        CHECK(Access::selectNative(app, kArgs));
        EXPECT_COUNTS(g, (Counts{{"radio_open.rtlsdr", 1}}));
        CHECK(Access::watching(app));
    }
    g_script.realRtl = false;
}

void testSamplesFlowedIsCountedOnceTheRadioDelivers() {
    // THE RECEIVER STOPPED: a radio is open and nothing arrives - not counted, however long we look.
    {
        auto g = fresh();
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        CHECK(Access::selectNative(app, kArgs));
        CHECK(Access::watching(app));
        for (int i = 0; i < 80; ++i) {
            Access::poll(app);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(only(g, "radio_data.").empty());
        CHECK(Access::watching(app));
    }
    // THE RECEIVER RUNNING: the first spectrum picture made from the radio's samples counts, once.
    {
        auto g = fresh();
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        CHECK(Access::selectNative(app, kArgs));
        Access::start(app);
        bool seen = false;
        for (int i = 0; i < 2000 && !seen; ++i) {
            Access::poll(app);
            seen = !Access::watching(app);
            if (!seen) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        }
        Access::stop(app);
        CHECK(seen);
        EXPECT_COUNTS(g, (Counts{{"radio_open.rtlsdr", 1}, {"radio_data.rtlsdr", 1}}));
        // Polling on does not count it again.
        for (int i = 0; i < 20; ++i) { Access::poll(app); }
        EXPECT_COUNTS(g, (Counts{{"radio_open.rtlsdr", 1}, {"radio_data.rtlsdr", 1}}));
    }
}

void testTheRestoreOfTheSavedRadioIsCounted() {
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "rtlsdr";
    cfg.nativeArgs = kArgs;
    cfg.sampleRateHz = 2.4e6;
    // The saved radio would not open (the real driver, a serial that is not there).
    {
        auto g = fresh();
        g_script.realRtl = true;
        setNative({});
        cascade::gui::AppWindow app;
        Access::restore(app, cfg);
        const std::string fail = only(g, "radio_fail.");
        std::printf("health: the saved radio, serial not there: %s\n", fail.c_str());
        CHECK(isBindOrAbsent(fail, "rtlsdr"));
        CHECK(only(g, "radio_open.").empty());
        g_script.realRtl = false;
    }
    // The saved radio opened.
    {
        auto g = fresh();
        setNative({});
        cascade::gui::AppWindow app;
        Access::restore(app, cfg);
        EXPECT_COUNTS(g, (Counts{{"radio_open.rtlsdr", 1}}));
        CHECK(Access::watching(app));
    }
}

void testThePatchPagesRadiosAreCounted() {
    const std::string device = "rtlsdr|" + kArgs;
    {
        auto g = fresh();
        g_script.realRtl = true;
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        const auto id = Access::addRadioNode(app, device);
        CHECK(Access::patchUntilSettled(app, id));
        const std::string fail = only(g, "radio_fail.");
        std::printf("health: the patch page's radio, serial not there: %s\n", fail.c_str());
        CHECK(isBindOrAbsent(fail, "rtlsdr"));
        CHECK(only(g, "radio_open.").empty());
        Access::stopPatch(app);
        g_script.realRtl = false;
    }
    {
        auto g = fresh();
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        const auto id = Access::addRadioNode(app, device);
        CHECK(Access::patchUntilSettled(app, id));
        EXPECT_COUNTS(g, (Counts{{"radio_open.rtlsdr", 1}}));
        Access::stopPatch(app);
    }
}

void testASoundCardInputIsCountedLikeARadio() {
    cascade::source::SoundCardDevice d;
    d.index = 0;
    d.name = "Line In (Fake Audio)";
    d.hostApi = "Windows WASAPI";
    d.maxInputChannels = 2;
    d.defaultRateHz = 48000.0;
    d.isDefault = true;
    d.rates = {{48000.0, false}, {96000.0, false}};
    cascade::source::SoundCardSettings s;
    s.device = d.name;
    s.hostApi = d.hostApi;
    s.cardRateHz = 48000.0;
    s.format = cascade::source::SoundCardFormat::RealMono;
    s.pickedFromList = true;
    // THE HOST API REFUSES the stream.
    {
        auto g = fresh();
        {
            std::lock_guard<std::mutex> lk(g_cards.m);
            g_cards.devices = {d};
        }
        g_cards.refuse = true;
        cascade::gui::AppWindow app;
        CHECK(Access::openCard(app, s));
        EXPECT_COUNTS(g, (Counts{{"radio_fail.soundcard.other", 1}}));
        // The card's name is never in a count.
        CHECK(health::encode(g->counts()).find("Line In") == std::string::npos);
    }
    // THE CARD IS NOT THERE any more.
    {
        auto g = fresh();
        {
            std::lock_guard<std::mutex> lk(g_cards.m);
            g_cards.devices.clear();
        }
        g_cards.refuse = false;
        cascade::gui::AppWindow app;
        CHECK(Access::openCard(app, s));
        EXPECT_COUNTS(g, (Counts{{"radio_fail.soundcard.absent", 1}}));
    }
    // IT OPENS.
    {
        auto g = fresh();
        {
            std::lock_guard<std::mutex> lk(g_cards.m);
            g_cards.devices = {d};
        }
        g_cards.refuse = false;
        cascade::gui::AppWindow app;
        CHECK(Access::openCard(app, s));
        EXPECT_COUNTS(g, (Counts{{"radio_open.soundcard", 1}}));
    }
}

// ---------------------------------------------------------------------------
// THE SCAN THAT FOUND NOTHING.
// ---------------------------------------------------------------------------
void testAScanThatFindsNoRadioIsCountedOnceASession() {
    // Nothing listed natively.
    {
        auto g = fresh();
        setNative({});
        cascade::gui::AppWindow app;
        CHECK(Access::scanBoth(app));
        const bool anything = Access::listed(app);
        std::printf("health: the whole-bus scan listed %s\n", anything ? "something" : "nothing");
        if (!anything) {
            EXPECT_COUNTS(g, (Counts{{"scan_none", 1}}));
            // A Refresh that finds nothing again is not a second.
            CHECK(Access::scanBoth(app));
            EXPECT_COUNTS(g, (Counts{{"scan_none", 1}}));
        } else {
            // A machine with a SoapySDR radio on it: the count must stay out of it.
            CHECK(only(g, "scan_none").empty());
        }
    }
    // A radio listed natively: a scan that found one found a radio.
    {
        auto g = fresh();
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        cascade::gui::AppWindow app;
        CHECK(Access::scanBoth(app));
        CHECK(only(g, "scan_none").empty());
    }
    setNative({});
}

// ---------------------------------------------------------------------------
// THE SPEAKERS THAT PLAYED.
// ---------------------------------------------------------------------------
void testSoundOkIsCountedWhenTheOutputHasPlayed() {
    auto g = fresh();
    cascade::gui::AppWindow app;
    cascade::sink::AudioOut& out = Access::audio(app);
    if (!out.open(-1, 48000.0, 1)) {
        std::printf("SKIP the speakers that played: no output device on this machine\n");
        ++g_checksSkipped;
        return;
    }
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    // BEFORE ANYTHING IS PLAYED the output is open and not primed: not counted.
    for (int i = 0; i < 10; ++i) { Access::poll(app); }
    CHECK(only(g, "sound_ok.").empty());
    // Feed it a lead; the device's own callback primes it and plays.
    std::vector<float> lead(cascade::sink::AudioOut::kPrimeFrames * 2, 0.0f);
    out.write(lead.data(), lead.size());
    bool primed = false;
    for (int i = 0; i < 400 && !primed; ++i) {
        primed = out.primed();
        if (!primed) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    CHECK(primed);
    Access::poll(app);
    const std::string api = out.openedHostApi();
    const health::AudioApi kind = health::audioApiFromName(api);
    std::printf("health: the output played through \"%s\" -> sound_ok.%s\n", api.c_str(),
                health::audioApiWord(kind));
    EXPECT_COUNTS(g, (Counts{{std::string("sound_ok.") + health::audioApiWord(kind), 1}}));
    // Once a session.
    for (int i = 0; i < 20; ++i) { Access::poll(app); }
    CHECK(health::sumOf(g->counts(), "sound_ok") == 1);
    out.close();
}

// ---------------------------------------------------------------------------
// THE UPDATE.
// ---------------------------------------------------------------------------
void testAnUpdateCheckThatFailsIsCounted() {
    auto g = fresh();
    cascade::gui::AppWindow app;
    CHECK(Access::check(app));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    g->reset();
}

void testAnInstallerThatCannotBeStartedIsCounted() {
#if defined(_WIN32)
    auto g = fresh();
    cascade::gui::AppWindow app;
    // The path the Install key was given is not a path the shell is asked to run:
    // nothing starts, and that is a failure the user caused by pressing the key.
    CHECK(!Access::launch(app, std::string()));
    EXPECT_COUNTS(g, (Counts{{"upd_run", 1}}));
    g->reset();
#else
    std::printf("SKIP the installer that cannot be started: there is no installer on this platform\n");
    ++g_checksSkipped;
#endif
}

// ---------------------------------------------------------------------------
// PLUGINS refused before they are loaded: retired by the catalogue, or built for another ABI.
// ---------------------------------------------------------------------------
void testPluginsBlockedBeforeLoadAreCounted() {
    auto g = fresh();
    cascade::gui::AppWindow app;
    const fs::path dir = cascade::core::PluginHost::defaultPluginDir();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string ext =
#if defined(_WIN32)
        ".dll";
#else
        ".so";
#endif
    std::vector<cascade::core::InstalledPlugin> plugins;
    std::vector<cascade::core::CachedPolicy> policies;
    auto add = [&](const std::string& id, std::uint32_t abi, const std::string& floor) {
        const std::string file = id + ext;
        const std::string bytes = "not a module, only a file the manifest names: " + id;
        {
            std::ofstream out(dir / file, std::ios::binary);
            out << bytes;
        }
        cascade::core::InstalledPlugin p;
        p.id = id;
        p.name = id;
        p.version = "1.0.0";
        p.file = file;
        p.abiVersion = abi;
        std::string err;
        CHECK(cascade::core::PluginRepo::sha256Hex(bytes.data(), bytes.size(), p.sha256, err));
        plugins.push_back(p);
        cascade::core::CachedPolicy c;
        c.id = id;
        c.known = true;
        c.minSupportedVersion = floor;
        c.catalogueVersion = "2.0.0";
        c.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
        policies.push_back(c);
    };
    add("healthretired", static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION), "2.0.0");  // below the floor
    add("healthabi", static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION) + 5u, "");      // another ABI
    add("healthfine", static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION), "");          // neither
    std::string err;
    CHECK(cascade::core::PluginRepo::saveManifest(dir.string(), plugins, policies, err));
    Access::rescan(app);
    // The reason class of each blocked plugin, once each a session; a plugin that is not blocked counts nothing.
    const Counts c = g->counts();
    std::printf("health: the rescan counted %s\n", health::encode(c).c_str());
    CHECK(c.count("plug_load.retired") == 1 && countOf(c, "plug_load.retired") == 1);
    CHECK(c.count("plug_load.abi") == 1 && countOf(c, "plug_load.abi") == 1);
    // (the file the manifest names is not a module, so the host's own load may add a "load" - never more)
    for (const auto& kv : c) {
        CHECK(kv.first == "plug_load.retired" || kv.first == "plug_load.abi" || kv.first == "plug_load.load");
    }
    // A rescan meets them again: still one of each.
    Access::rescan(app);
    CHECK(countOf(g->counts(), "plug_load.retired") == 1);
    CHECK(countOf(g->counts(), "plug_load.abi") == 1);
    g->reset();
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// THE REAL BINARY: arms the ledger, journals it, keeps it, clears it, and
// does not lose a failure to a process that is ended.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define APP_EXE "\\cascade.exe"
#else
#define APP_EXE "/cascade"
#endif

struct AppRun {
    int frames = 30;
    std::string args;
};

void setAppEnv(const fs::path& config) {
    setEnv("CASCADE_CONFIG_TEST", config.string().c_str());
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

void clearAppEnv() {
    setEnv("CASCADE_CONFIG_TEST", nullptr);
}

void runApp(const fs::path& config, const AppRun& run = AppRun()) {
    setAppEnv(config);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + APP_EXE;
    const std::string tail =
        " --frames " + std::to_string(run.frames) + (run.args.empty() ? std::string() : " " + run.args);
#if defined(_WIN32)
    const std::string cmd = "\"\"" + exe + "\"" + tail + " 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "\"" + exe + "\"" + tail + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof buf, p) != nullptr) {}
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    clearAppEnv();
}

fs::path scratchDir(const char* tag) {
    const fs::path d = g_scratch / tag;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A config whose saved radio is an RTL-SDR with a serial that is not there.
void writeConfig(const fs::path& config, const std::string& id, bool reporting, bool diagnostics) {
    cascade::core::AppConfig cfg;
    cfg.telemetryEnabled = reporting;
    cfg.telemetryInstallId = reporting ? id : std::string();
    cfg.telemetryLaunches = 4;
    cfg.telemetryCleanExit = true;
    cfg.diagnosticsEnabled = diagnostics;
    cfg.sourceKind = "rtlsdr";
    cfg.nativeArgs = kArgs;
    cfg.sampleRateHz = 2.4e6;
    std::string err;
    CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                cascade::core::ConfigStore::serialize(cfg), err));
}

std::string pendingHealth(const fs::path& config) {
    cascade::core::AppConfig back;
    std::string err;
    if (!cascade::core::ConfigStore::load(config.string(), back, err)) { return "(unreadable)"; }
    const nlohmann::json p = nlohmann::json::parse(back.telemetryPending, nullptr, false);
    if (p.is_discarded() || !p.is_object() || !p.contains("health")) { return "(none)"; }
    return p["health"].get<std::string>();
}

bool bindOrAbsentCount(const Counts& c, std::uint32_t n) {
    return c.size() == 1 &&
           ((c.count("radio_fail.rtlsdr.bind") == 1 && countOf(c, "radio_fail.rtlsdr.bind") == n) ||
            (c.count("radio_fail.rtlsdr.absent") == 1 && countOf(c, "radio_fail.rtlsdr.absent") == n));
}

void testTheRealApplicationCountsKeepsAndClears() {
    const std::string id = cascade::core::newInstallId();

    // ON: the saved radio would not open - the very first thing the application does, before
    // it has read whether reporting is on - and the failure is in the ledger's file AND in the
    // record the session journals, under this run's own identity.
    {
        const fs::path dir = scratchDir("app-on");
        const fs::path config = dir / "config.json";
        writeConfig(config, id, true, true);
        runApp(config);
        const std::string file = HealthLedger::pathIn(u8(dir));
        Counts onDisk;
        const std::string text = readFile(file);
        CHECK(HealthLedger::parseFileText(text, id, onDisk));
        std::printf("health: the real application left %s\n", health::encode(onDisk).c_str());
        CHECK(bindOrAbsentCount(onDisk, 1));
        // The journalled record carries it too (as of that save; what is SENT is the ledger's at the next start).
        Counts journalled;
        CHECK(health::decode(pendingHealth(config), journalled));
        CHECK(journalled == onDisk);
    }

    // OFF: nothing counted, nothing kept, nothing journalled, and a file an earlier opted-in run left is removed.
    {
        const fs::path dir = scratchDir("app-off");
        const fs::path config = dir / "config.json";
        writeConfig(config, id, false, true);
        {
            std::ofstream(HealthLedger::pathIn(u8(dir)), std::ios::binary)
                << HealthLedger::fileText(id, Counts{{"upd_dl", 3}});
        }
        runApp(config);
        CHECK(!fs::exists(HealthLedger::pathIn(u8(dir))));
        CHECK(pendingHealth(config) == "(none)");
        CHECK(readFile(config).find("\"health\"") == std::string::npos);
    }

    // DIAGNOSTICS OFF (the second gate): reporting is on and nothing new is counted.
    {
        const fs::path dir = scratchDir("app-nodiag");
        const fs::path config = dir / "config.json";
        writeConfig(config, id, true, false);
        runApp(config);
        const std::string file = HealthLedger::pathIn(u8(dir));
        CHECK(!fs::exists(file));
        CHECK(pendingHealth(config) == "");
    }

    // EARLIER SESSIONS' COUNTS ARE KEPT and this run's are added to them (the send is refused by
    // the black hole, so nothing is taken off).
    {
        const fs::path dir = scratchDir("app-prior");
        const fs::path config = dir / "config.json";
        writeConfig(config, id, true, true);
        {
            std::ofstream(HealthLedger::pathIn(u8(dir)), std::ios::binary)
                << HealthLedger::fileText(id, Counts{{"upd_dl", 2}});
        }
        cascade::core::AppConfig cfg;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(config.string(), cfg, err));
        cfg.telemetryPending = "{\"id\":\"" + id + "\",\"v\":\"0.0.0\",\"sessionSec\":9}";
        CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                    cascade::core::ConfigStore::serialize(cfg), err));
        runApp(config);
        Counts onDisk;
        CHECK(HealthLedger::parseFileText(readFile(HealthLedger::pathIn(u8(dir))), id, onDisk));
        CHECK(onDisk.size() == 2 && countOf(onDisk, "upd_dl") == 2);
        std::uint32_t radio = 0;
        for (const auto& kv : onDisk) {
            if (kv.first.rfind("radio_fail.rtlsdr.", 0) == 0) { radio = kv.second; }
        }
        CHECK(radio == 1);
    }

    // A LEDGER LEFT BY ANOTHER IDENTITY is not attributed to this one.
    {
        const fs::path dir = scratchDir("app-other");
        const fs::path config = dir / "config.json";
        writeConfig(config, id, true, true);
        {
            std::ofstream(HealthLedger::pathIn(u8(dir)), std::ios::binary)
                << HealthLedger::fileText(cascade::core::newInstallId(), Counts{{"upd_dl", 7}});
        }
        runApp(config);
        Counts onDisk;
        CHECK(HealthLedger::parseFileText(readFile(HealthLedger::pathIn(u8(dir))), id, onDisk));
        CHECK(bindOrAbsentCount(onDisk, 1));   // no upd_dl in it
    }
}

#if defined(_WIN32)
struct Child {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE readEnd = nullptr;
    std::string seen;
    bool started = false;
};

Child startFrozenApp(const fs::path& config) {
    Child c;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&c.readEnd, &writeEnd, &sa, 0)) { return c; }
    ::SetHandleInformation(c.readEnd, HANDLE_FLAG_INHERIT, 0);
    setAppEnv(config);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    // 60 s: far longer than this test needs, so the process is still frozen when it is ended.
    std::string cmd = "\"" + exe + "\" --frames 600 --diag-stall 60000";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    clearAppEnv();
    ::CloseHandle(writeEnd);
    if (ok) {
        c.process = pi.hProcess;
        c.thread = pi.hThread;
        c.started = true;
    }
    return c;
}

bool waitForOutput(Child& c, const std::string& needle, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (::PeekNamedPipe(c.readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (::ReadFile(c.readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) { c.seen.append(buf, got); }
        } else if (::WaitForSingleObject(c.process, 0) == WAIT_OBJECT_0) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (c.seen.find(needle) != std::string::npos) { return true; }
    }
    return c.seen.find(needle) != std::string::npos;
}

// THE PATH A FAILURE IS MOST AT RISK ON: the radio would not open, the window then froze, and
// the user ended it from the taskbar. End task is TerminateProcess - nothing runs, the GUI
// thread never saves again - and the failure is still reported, by the next start.
void testAFailureSurvivesEndTask() {
    const std::string id = cascade::core::newInstallId();
    const fs::path dir = scratchDir("app-endtask");
    const fs::path config = dir / "config.json";
    writeConfig(config, id, true, true);
    const std::string file = HealthLedger::pathIn(u8(dir));

    Child c = startFrozenApp(config);
    CHECK(c.started);
    if (c.started) {
        CHECK(waitForOutput(c, "--diag-stall wedging the frame loop", 60000));
        // STILL FROZEN: the GUI thread has not come back from its wedge.
        CHECK(::WaitForSingleObject(c.process, 0) == WAIT_TIMEOUT);
        // The ledger's own thread wrote the file; wait for it, not for a time.
        Counts onDisk;
        bool there = false;
        for (int i = 0; i < 200 && !there; ++i) {
            there = HealthLedger::parseFileText(readFile(file), id, onDisk) && !onDisk.empty();
            if (!there) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        }
        CHECK(there);
        CHECK(bindOrAbsentCount(onDisk, 1));
        // End task.
        ::TerminateProcess(c.process, 1);
        ::WaitForSingleObject(c.process, 10000);
    }
    if (c.thread != nullptr) { ::CloseHandle(c.thread); }
    if (c.process != nullptr) { ::CloseHandle(c.process); }
    if (c.readEnd != nullptr) { ::CloseHandle(c.readEnd); }

    // The dead process saved nothing more, and the failure is still on disk...
    Counts after;
    CHECK(HealthLedger::parseFileText(readFile(file), id, after));
    CHECK(bindOrAbsentCount(after, 1));
    // ...so THE NEXT START finds it as an earlier session's, and its own failure is added.
    runApp(config);
    Counts next;
    CHECK(HealthLedger::parseFileText(readFile(file), id, next));
    CHECK(next.size() == 1 && next.begin()->second == 2);   // the dead session's, and this one's
}
#endif  // _WIN32

}  // namespace

int main() {
    isolate();
    Access::installHooks();
    testTheSourceListsOpenCountsFailureAndSuccess();
    testSamplesFlowedIsCountedOnceTheRadioDelivers();
    testTheRestoreOfTheSavedRadioIsCounted();
    testThePatchPagesRadiosAreCounted();
    testASoundCardInputIsCountedLikeARadio();
    testAScanThatFindsNoRadioIsCountedOnceASession();
    testSoundOkIsCountedWhenTheOutputHasPlayed();
    testAnUpdateCheckThatFailsIsCounted();
    testAnInstallerThatCannotBeStartedIsCounted();
    testPluginsBlockedBeforeLoadAreCounted();
    testTheRealApplicationCountsKeepsAndClears();
#if defined(_WIN32)
    testAFailureSurvivesEndTask();
#else
    SKIP_LINUX("the End-task run uses CreateProcess and TerminateProcess");
#endif
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_health_app");
}
