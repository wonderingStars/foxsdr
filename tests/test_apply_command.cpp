// test_apply_command.cpp - EVERY op AppWindow::applyCommand implements,
// applied to the real application (engine extraction stage 1,
// docs/engine-stage1.md).
//
// ONE CASE PER OP. Each case prints the op's name, applies the command through
// AppWindow::applyCommand - the one function every desktop widget, key, the
// web remote, CAT and plugins now end in - and checks what it did to the
// receiver: the pipeline's own readback where the pipeline has one, the
// window's mirror where it does not, and the result (status, REFUSED,
// CLAMPED, applied[]). The list of ops is core/app_commands.cpp's; a case for
// an op that list does not mark implemented, or an implemented op with no
// case, fails the coverage check at the end - so a new op cannot be added
// without a test, and an op cannot quietly stop being implemented.
//
// HERMETIC, like test_converter_app_paths and test_bias_key_app: no config is
// read or written, every per-user folder is a scratch directory, no USB
// enumeration runs (testHooks_.nativeScan), no SoapySDR probe runs
// (testHooks_.soapyScan), no radio, sound card or transmitter is real
// (testHooks_.makeDevice, testHooks_.soundCardBackend, a fake iiod daemon on
// loopback), and the one plugin is tests/fixture_stage1_plugin.cpp, loaded
// from the scratch directory (testHooks_.pluginDir). Network targets are
// 127.0.0.1:9, and telemetry is pointed there too.
//
// WHAT IS NOT COVERED HERE, and why: SET_DEVICE_OPTION's switches exist only
// on an SDRplay RSP or an RX888 (their refusal on a radio without them is
// checked; the switch bodies are the Source panel's own, moved); TX_SET_INPUT
// to the MICROPHONE would open a real input device (the tone input is
// checked); AUDIO_DEVICE is checked against an index PortAudio does not have,
// so no speaker is opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "iiod_fake_server.hpp"
#include "imgui.h"
#include "net/web_control.hpp"
#include "source/device_source.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;

// --- the scratch world -----------------------------------------------------------

std::filesystem::path g_scratch;
std::filesystem::path g_plugins;

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
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_apply_command_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The fixture plugin, copied into a plugin directory of this test's own.
    g_plugins = g_scratch / "plugins";
    std::filesystem::create_directories(g_plugins);
    const std::filesystem::path fixture(FIXTURE_STAGE1_PLUGIN);
    std::error_code ec;
    std::filesystem::copy_file(fixture, g_plugins / fixture.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { std::printf("could not copy the fixture plugin: %s\n", ec.message().c_str()); }
}

std::string pluginDirHook() { return g_plugins.string(); }

// --- a radio that records nothing and quantises its gains to whole dB -----------

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
    const char* lastError() const override { return error_.c_str(); }
    const char* driverKey() const override { return kind_.c_str(); }
    bool open(const std::string& args) override {
        std::lock_guard<std::mutex> lk(argsMutex());
        lastArgs() = args;
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override {
        cascade::source::GainInfo lna;
        lna.name = "LNA";
        lna.minDb = 0.0;
        lna.maxDb = 40.0;
        cascade::source::GainInfo vga = lna;
        vga.name = "VGA";
        return {lna, vga};
    }
    bool setGainDb(const std::string& name, double db) override {
        if (name == "NOPE") {
            error_ = "fake: no such stage";
            return false;
        }
        (name == "LNA" ? lna_ : vga_) = std::round(db);  // whole dB, like a real tuner's steps
        return true;
    }
    double gainDb(const std::string& name) const override { return name == "LNA" ? lna_ : vga_; }
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override {
        agc_ = on;
        return true;
    }
    bool autoGain() const override { return agc_; }
    std::vector<std::string> antennas() const override { return {"A", "B"}; }
    bool setAntenna(const std::string& name) override {
        if (name != "A" && name != "B") {
            error_ = "fake: no such antenna";
            return false;
        }
        antenna_ = name;
        return true;
    }
    std::string antenna() const override { return antenna_; }
    std::vector<double> supportedSampleRatesHz() const override { return {1.024e6, 2.048e6, 2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

    static std::mutex& argsMutex() {
        static std::mutex m;
        return m;
    }
    static std::string& lastArgs() {
        static std::string s;
        return s;
    }

private:
    std::string kind_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.048e6;
    double centre_ = 100.0e6;
    double lna_ = 20.0;
    double vga_ = 10.0;
    bool agc_ = false;
    std::string antenna_ = "A";
    bool open_ = false;
    std::string error_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    return std::make_unique<FakeRadio>(kind);
}

std::vector<cascade::source::NativeDeviceInfo> fakeNativeScan() {
    return {{"rtlsdr", "Fake RTL", "serial=0001"}};
}

std::vector<cascade::source::SoapyDeviceInfo> fakeSoapyScan() {
    return {{"Fake Soapy", "driver=fake,serial=9"}};
}

// --- a sound card that opens and says nothing -------------------------------------

class FakeCard final : public cascade::source::SoundCardBackend {
public:
    std::vector<cascade::source::SoundCardDevice> listDevices() override {
        cascade::source::SoundCardDevice d;
        d.index = 0;
        d.name = "Fake Card";
        d.hostApi = "Fake API";
        d.maxInputChannels = 2;
        d.defaultRateHz = 48000.0;
        d.isDefault = true;
        d.rates = {{48000.0, false}};
        return {d};
    }
    bool open(const cascade::source::SoundCardDevice&, int, double, bool, PushFn, void*,
              std::string&) override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool alive() override { return open_.load(); }

private:
    std::atomic<bool> open_{false};
};

std::shared_ptr<cascade::source::SoundCardBackend> makeCard() { return std::make_shared<FakeCard>(); }

// A two-channel 16-bit WAV of silence: the I/Q file source's own format.
std::filesystem::path writeIqWav(const std::string& name) {
    const std::filesystem::path p = g_scratch / name;
    std::ofstream f(p, std::ios::binary);
    const std::uint32_t rate = 250000;
    const std::uint32_t frames = 4096;
    const std::uint32_t dataBytes = frames * 4u;
    const auto u32 = [&f](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&f](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36u + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16u);
    u16(1u);      // PCM
    u16(2u);      // I and Q
    u32(rate);
    u32(rate * 4u);
    u16(4u);
    u16(16u);
    f.write("data", 4);
    u32(dataBytes);
    const std::vector<char> zeros(dataBytes, 0);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    return p;
}

template <typename Fn>
bool waitFor(Fn fn, int ms = 20000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!fn()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for its tests (see AppWindow::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeNativeScan;
        AppWindow::testHooks_.soapyScan = &fakeSoapyScan;
        AppWindow::testHooks_.pluginDir = &pluginDirHook;
        AppWindow::testHooks_.soundCardBackend = &makeCard;
    }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c, const std::string& lt = {}) {
        return a.applyCommand(c, lt);
    }
    static FoxCommandResult apply(AppWindow& a, const cmd::QueuedCommand& q) {
        return a.applyCommand(q.c, q.longText);
    }
    static void submit(AppWindow& a, const FoxCommand& c) { a.submitCommand(c); }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.submitCommand(q); }
    static void drain(AppWindow& a) { a.drainLocalCommands(); }
    static std::size_t queued(AppWindow& a) { return a.localCommands_.size(); }

    // The receiver.
    static bool running(AppWindow& a) { return a.pipeline_.running(); }
    static double centre(AppWindow& a) { return a.pipeline_.activeSource().centerFrequencyHz(); }
    static double tuned(AppWindow& a) { return a.currentAbsoluteHz(); }
    static double vfo(AppWindow& a) { return a.pipeline_.vfoOffsetHz(); }
    static float vfoKhz(AppWindow& a) { return a.vfoOffsetKhz_; }
    static double inputRate(AppWindow& a) { return a.pipeline_.inputRateHz(); }
    static double channelRate(AppWindow& a) { return a.pipeline_.channelRateHz(); }
    static int modeIndex(AppWindow& a) { return a.modeIndex_; }
    static cascade::dsp::DemodMode demod(AppWindow& a) { return a.pipeline_.demodMode(); }
    static double bw(AppWindow& a) { return a.vfoBandwidthHz_; }
    static int bwIndex(AppWindow& a) { return a.bandwidthIndex_; }
    static float squelch(AppWindow& a) { return a.squelchDb_; }
    static float volume(AppWindow& a) { return a.volume_; }
    static bool muted(AppWindow& a) { return a.userMuted_; }
    static int deemph(AppWindow& a) { return a.deemphIndex_; }
    static double deemphUs(AppWindow& a) { return a.pipeline_.deemphasisUs(); }
    static bool stereo(AppWindow& a) { return a.stereoEnabled_ && a.pipeline_.stereoEnabled(); }
    static bool stereoOff(AppWindow& a) { return !a.stereoEnabled_ && !a.pipeline_.stereoEnabled(); }
    static bool nr(AppWindow& a) { return a.nrEnabled_; }
    static bool nrPipe(AppWindow& a) { return a.pipeline_.noiseReductionEnabled(); }
    static float nrStrength(AppWindow& a) { return a.nrStrength_; }
    static float nrStrengthPipe(AppWindow& a) { return a.pipeline_.noiseReductionStrength(); }
    static bool notch(AppWindow& a) { return a.notchEnabled_ && a.pipeline_.notchEnabled(); }
    static bool notchOff(AppWindow& a) { return !a.notchEnabled_ && !a.pipeline_.notchEnabled(); }
    static float notchHz(AppWindow& a) { return a.notchFreqHz_; }
    static double notchHzPipe(AppWindow& a) { return a.pipeline_.notchFrequencyHz(); }
    static float notchQ(AppWindow& a) { return a.notchQ_; }
    static double notchQPipe(AppWindow& a) { return a.pipeline_.notchQ(); }
    static bool autoNotch(AppWindow& a) { return a.autoNotch_ && a.pipeline_.autoNotchEnabled(); }
    static float dbMin(AppWindow& a) { return a.dbMin_; }
    static float dbMax(AppWindow& a) { return a.dbMax_; }
    static const std::string& bandPlan(AppWindow& a) { return a.bandPlanSelection_; }

    // The recorder.
    static void setRecordDir(AppWindow& a, const std::string& d) { a.recordDir_ = d; }
    static bool recIq(AppWindow& a) { return a.iqRecorder_.recording(); }
    static bool recAudio(AppWindow& a) { return a.audioRecorder_.recording(); }

    // The source.
    static std::size_t nativeCount(AppWindow& a) { return a.nativeDevices_.size(); }
    static void clearNative(AppWindow& a) { a.nativeDevices_.clear(); }
    static std::size_t soapyCount(AppWindow& a) { return a.soapyDevices_.size(); }
    static void clearSoapy(AppWindow& a) {
        a.soapyDevices_.clear();
        a.soapyScanned_ = false;
        a.soapyScanPartial_ = false;
    }
    static void markSoapyScanned(AppWindow& a) {
        a.soapyScanned_ = true;
        a.soapyScanPartial_ = false;
    }
    static bool soapyPending(AppWindow& a) { return a.soapyScanPending_; }
    static bool waitSoapy(AppWindow& a) {
        return waitFor([&a] {
            a.pollSourceAsync();
            return !a.soapyScanPending_;
        });
    }
    static bool networkUsrps(AppWindow& a) { return a.lookForNetworkUsrps_; }
    static bool waitOpen(AppWindow& a) {
        return waitFor([&a] {
            a.pollSourceAsync();
            return !a.deviceOpenPending_;
        });
    }
    static bool waitCard(AppWindow& a) {
        return waitFor([&a] {
            a.pollSoundCard();
            return !a.soundCardOpenPending_ && !a.soundCardScanPending_;
        });
    }
    static const std::string& kind(AppWindow& a) { return a.sourceKind_; }
    static int sourceSel(AppWindow& a) { return a.sourceSel_; }
    static bool haveDevice(AppWindow& a) { return a.device_ != nullptr; }
    static const std::string& sourceError(AppWindow& a) { return a.sourceError_; }
    static void clearSourceError(AppWindow& a) { a.sourceError_.clear(); }
    static float gain(AppWindow& a, const char* name) {
        for (std::size_t i = 0; i < a.deviceGainNames_.size() && i < a.deviceGainsDb_.size(); ++i) {
            if (a.deviceGainNames_[i] == name) { return a.deviceGainsDb_[i]; }
        }
        return -999.0f;
    }
    // NULL-SAFE, every one: a build in which the source was never selected
    // must report the failure, not crash the harness in the run that has
    // something to say.
    static double radioGain(AppWindow& a, const char* name) {
        return a.device_ != nullptr ? a.device_->gainDb(name) : -999.0;
    }
    static bool agc(AppWindow& a) { return a.device_ != nullptr && a.deviceAgc_ && a.device_->autoGain(); }
    static bool agcOff(AppWindow& a) {
        return a.device_ != nullptr && !a.deviceAgc_ && !a.device_->autoGain();
    }
    static const std::string& antenna(AppWindow& a) { return a.deviceAntenna_; }
    static double rate(AppWindow& a) { return a.pipeline_.activeSource().sampleRateHz(); }
    static int rateIndex(AppWindow& a) { return a.deviceRateIndex_; }
    static void setBiasStandIn(AppWindow& a) { a.biasStandIn_ = cascade::gui::BiasStandIn::Accept; }
    static bool biasOn(AppWindow& a) { return a.biasStandInOn_; }
    static cascade::core::ConverterSetting converter(AppWindow& a) { return a.pipeline_.converter(); }
    static void setSoundCardForm(AppWindow& a) {
        a.soundCard_ = cascade::source::SoundCardSettings{};
        a.soundCard_.device = "Fake Card";
        a.soundCard_.hostApi = "Fake API";
        a.soundCard_.cardRateHz = 48000.0;
    }
    static void setSoundCardFormIq(AppWindow& a) {
        setSoundCardForm(a);
        a.soundCard_.format = cascade::source::SoundCardFormat::IqStereo;
        a.soundCard_.iqCentreHz = 7.0e6;
    }
    static double liveIqCentre(AppWindow& a) { return a.soundCardLive_.iqCentreHz; }

    // Bookmarks and scanner.
    static const std::vector<cascade::core::Bookmark>& bookmarks(AppWindow& a) { return a.freqMgr_.list(); }
    static std::uint64_t addBookmark(AppWindow& a, const std::string& name, double hz, const char* mode,
                                     double bwHz, const std::string& group = {}) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = mode;
        b.bandwidthHz = bwHz;
        b.group = group;
        const int at = a.freqMgr_.add(b);
        return a.freqMgr_.list()[static_cast<std::size_t>(at)].id;
    }
    static const std::string& importNote(AppWindow& a) { return a.bookmarkImportNote_; }
    // The web remote's side: the snapshot a browser reads (and the row map it
    // leaves), and a request applied as applyWebControls applies it.
    static void publishWeb(AppWindow& a) { a.publishWebSnapshot(); }
    // The row a browser sees a bookmark on: read off the PUBLISHED snapshot
    // (what /api/status serves), never off the row map under test.
    static int webRowOf(AppWindow& a, const std::string& name) {
        std::lock_guard<std::mutex> lock(a.webMutex_);
        const auto& rows = a.webStatus_.bookmarks;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].name == name) { return static_cast<int>(r); }
        }
        return -1;
    }
    static void webRequest(AppWindow& a, const cascade::net::ControlRequest& r) { a.applyControlRequest(r); }
    static bool scanning(AppWindow& a) { return a.scanner_.active(); }
    static double scanStartMhz(AppWindow& a) { return a.scanStartMhz_; }
    static double scanStopMhz(AppWindow& a) { return a.scanStopMhz_; }
    static double scanStepKhz(AppWindow& a) { return a.scanStepKhz_; }
    static double scanDwell(AppWindow& a) { return a.scanDwellMs_; }
    static double scanListen(AppWindow& a) { return a.scanListenMs_; }

    // Plugins and the store.
    static const cascade::core::LoadedPlugin* fixture(AppWindow& a) {
        for (const cascade::core::LoadedPlugin& p : a.pluginHost_.plugins()) {
            if (p.loaded && p.name == "Stage One Fixture") { return &p; }
        }
        return nullptr;
    }
    static std::string fixtureKey(AppWindow& a) {
        const cascade::core::LoadedPlugin* p = fixture(a);
        return p != nullptr ? cascade::core::pluginKey(*p) : std::string();
    }
    static bool stopped(AppWindow& a, const std::string& key) { return a.pluginIsStopped(key); }
    static bool mutes(AppWindow& a) {
        const cascade::core::LoadedPlugin* p = fixture(a);
        return p != nullptr && a.pluginMutes(*p);
    }
    static bool tuneGranted(AppWindow& a, const std::string& key) {
        for (const std::string& k : a.pluginTuneAllowed_) {
            if (k == key) { return true; }
        }
        return false;
    }
    static bool settingsGranted(AppWindow& a, const std::string& key) {
        for (const std::string& k : a.pluginSettingsAllowed_) {
            if (k == key) { return true; }
        }
        return false;
    }
    static std::size_t userPresetCount(AppWindow& a) { return a.userPresets_.size(); }
    static const std::string& presetNote(AppWindow& a) { return a.presetNote_; }
    static void setCatalogueUrl(AppWindow& a, const std::string& u) { a.pluginCatalogueUrl_ = u; }
    static bool storeBusy(AppWindow& a) { return a.catalogPending_ || a.installPending_; }
    static bool waitStore(AppWindow& a) {
        return waitFor([&a] {
            a.pollPluginAsync();
            return !a.catalogPending_ && !a.installPending_;
        });
    }
    static const std::string& installError(AppWindow& a) { return a.installError_; }
    static void clearInstallError(AppWindow& a) { a.installError_.clear(); }
    static void injectCatalogue(AppWindow& a) {
        cascade::core::PluginCatalogEntry e;
        e.id = "stage1-catalogue-only";
        e.name = "Catalogue Only";
        e.version = "1.0.0";
        e.licence = "MIT";
        e.legalNotice = "a notice to acknowledge";
        e.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        cascade::core::PluginPlatform pf;
        pf.os = cascade::core::PluginRepo::hostOs();
        pf.arch = cascade::core::PluginRepo::hostArch();
        pf.file = "catalogue_only.dll";
        pf.url = "https://127.0.0.1:9/catalogue_only.dll";
        pf.sha256 = std::string(64, 'a');
        e.platforms.push_back(pf);
        e.compatible = true;
        a.catalog_.clear();
        a.catalog_.push_back(e);
    }
    static bool addAllActive(AppWindow& a) { return a.addAllRun_.active; }
    static void stopAddAll(AppWindow& a) { a.addAllRun_ = AppWindow::AddAllRun{}; }
    static const std::string& pluginDirNow(AppWindow& a) { return a.pluginDir_; }

    // The patch page, the transmitter, audio, position, GPS.
    static bool patchRunning(AppWindow& a) { return a.patchRunning_; }
    static void setTransmitOpen(AppWindow& a, bool on) { a.transmitOpen_ = on; }
    static bool haveTx(AppWindow& a) { return a.transmitter_.haveSink(); }
    static std::int64_t remoteHoldMs(AppWindow& a) { return a.transmitter_.remoteHoldRemainingMs(); }
    static int txMode(AppWindow& a) { return a.transmitModeIndex_; }
    static bool txSplit(AppWindow& a) { return a.transmitSplit_; }
    static double txSplitHz(AppWindow& a) { return a.transmitSplitHz_; }
    static double txPower(AppWindow& a) { return a.transmitPowerDb_; }
    static int txInput(AppWindow& a) { return a.transmitInputIndex_; }
    static double txTone(AppWindow& a) { return a.transmitToneHz_; }
    static bool txMonitor(AppWindow& a) { return a.transmitMonitor_; }
    static const std::string& txArgs(AppWindow& a) { return a.transmitArgs_; }
    static double txFrequency(AppWindow& a) { return a.transmitter_.frequencyHz(); }
    static void addAudioDevice(AppWindow& a, int paIndex, const char* name) {
        a.devices_.push_back({paIndex, name, false});
    }
    static int audioDeviceIndex(AppWindow& a) { return a.deviceIndex_; }
    static bool positionSet(AppWindow& a, double lat, double lon) {
        return a.rxSet_ && a.rxLat_ == lat && a.rxLon_ == lon;
    }
    static bool gpsListening(AppWindow& a) { return a.gpsReader_.listening(); }
};

}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

std::set<std::uint32_t> g_tested;

void covering(std::uint32_t op) {
    g_tested.insert(op);
    std::printf("  [%s]\n", cmd::opName(op));
}

bool refused(const FoxCommandResult& r, std::int32_t status) {
    return r.status == status && (r.flags & FOXAPI_RESULT_REFUSED) != 0u;
}

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

FoxCommand num(std::uint32_t op, double n0, double n1 = 0.0) { return cmd::makeNum(op, n0, n1); }
FoxCommand ints(std::uint32_t op, std::int64_t v0, std::int64_t v1 = 0) { return cmd::makeInt(op, v0, v1); }
FoxCommand numInt(std::uint32_t op, double n0, std::int64_t v0) { return cmd::makeNumInt(op, n0, v0); }
cmd::QueuedCommand text(std::uint32_t op, const std::string& t, std::int64_t v0 = 0, std::int64_t v1 = 0,
                        double n0 = 0.0) {
    return cmd::makeText(op, t, v0, v1, n0);
}

// --- the receiver -----------------------------------------------------------------------

void receiverOps(AppWindow& a) {
    covering(FOXAPI_OP_RUN);
    {
        A::setRecordDir(a, (g_scratch / "rec").string());
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 1))));
        CHECK(A::running(a));
        // A take is running when the stop arrives: RUN 0 is the dome's stop,
        // recordings ended BEFORE the pipeline stops (the 0.99.36 fix, now
        // the only stop there is).
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_IQ, 1))));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_AUDIO, 1))));
        CHECK(A::recIq(a) && A::recAudio(a));
        const FoxCommandResult r = A::apply(a, ints(FOXAPI_OP_RUN, 0));
        CHECK(ok(r));
        CHECK(!A::running(a));
        CHECK(r.applied[0] == 0.0);
        CHECK(!A::recIq(a));
        CHECK(!A::recAudio(a));
    }

    covering(FOXAPI_OP_SET_CENTRE);
    {
        const FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_CENTRE, 101.0e6));
        CHECK(ok(r));
        CHECK(A::centre(a) == 101.0e6);
        CHECK(r.applied[0] == 101.0e6);
    }

    covering(FOXAPI_OP_SET_VFO_OFFSET);
    {
        // The band stays inside the baseband: +/-(rate/2 - bw/2).
        const double lim = 0.5 * A::inputRate(a) - 0.5 * A::bw(a);
        std::printf("      input %.0f, bw %.0f -> limit %.0f\n", A::inputRate(a), A::bw(a), lim);
        CHECK(lim > 100e3);
        FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_VFO_OFFSET, 100e3));
        CHECK(ok(r));
        CHECK(A::vfo(a) == 100e3);
        CHECK(A::vfoKhz(a) == 100.0f);
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) == 0u);
        r = A::apply(a, num(FOXAPI_OP_SET_VFO_OFFSET, 50e6));
        CHECK(ok(r));
        CHECK(A::vfo(a) == lim);
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        CHECK(r.applied[0] == lim);
    }

    covering(FOXAPP_OP_SET_VFO_OFFSET_FREE);
    {
        // The rail's slider: NOT clamped into the band.
        const double lim = 0.5 * A::inputRate(a) - 0.5 * A::bw(a);
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_VFO_OFFSET_FREE, lim + 20e3))));
        CHECK(A::vfo(a) == lim + 20e3);
        CHECK(A::vfoKhz(a) == static_cast<float>((lim + 20e3) / 1000.0));
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_VFO_OFFSET_FREE, 50e3))));
        CHECK(A::vfo(a) == 50e3);
    }

    covering(FOXAPI_OP_SET_FREQUENCY);
    {
        // The counter's rule: the VFO offset kept, the centre follows.
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 102.0e6))));
        CHECK(A::tuned(a) == 102.0e6);
        CHECK(A::vfo(a) == 50e3);
        CHECK(A::centre(a) == 102.0e6 - 50e3);
    }

    covering(FOXAPP_OP_VFO_TO_ABSOLUTE);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_WFM))));  // 100 kHz raster
        const double centre = A::centre(a);
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_VFO_TO_ABSOLUTE, centre + 261e3, 1))));
        CHECK(A::vfo(a) == std::round((centre + 261e3) / 100e3) * 100e3 - centre);
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_VFO_TO_ABSOLUTE, centre + 261e3, 0))));
        CHECK(A::vfo(a) == 261e3);
        CHECK(A::centre(a) == centre);  // the VFO moved, never the radio
    }

    covering(FOXAPI_OP_STEP_TUNE);
    {
        const double before = A::tuned(a);
        FoxCommand c = ints(FOXAPI_OP_STEP_TUNE, 3);
        c.num[0] = 10e3;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::tuned(a) == before + 30e3);
        c.ival[0] = -1;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::tuned(a) == before + 20e3);
        c.ival[0] = 0;
        CHECK(refused(A::apply(a, c), FOXAPI_OUT_OF_RANGE));
        CHECK(A::tuned(a) == before + 20e3);
        // Floored at the counter's floor: with no converter, the VFO offset
        // itself (the centre may never be asked for less than 0 Hz).
        c.ival[0] = -1;
        c.num[0] = 1.0e9;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::tuned(a) == std::max(0.0, A::vfo(a)));
        CHECK(A::centre(a) == 0.0);
    }

    covering(FOXAPI_OP_SET_MODE);
    {
        // All eight, in the keys' order, each moving the bandwidth to its
        // mode's default (the keys' own setModeIndex).
        const char* names[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
        const int defaultBw[8] = {2, 1, 3, 3, 5, 5, 5, 0};
        for (int i = 0; i < 8; ++i) {
            const FoxCommandResult r = A::apply(a, ints(FOXAPI_OP_SET_MODE, i + 1));
            CHECK(ok(r));
            CHECK(A::modeIndex(a) == i);
            CHECK(std::string(cascade::dsp::modeName(A::demod(a))) == names[i]);
            CHECK(A::bwIndex(a) == defaultBw[i]);
            CHECK(A::bw(a) == cascade::gui::kBwHz[defaultBw[i]]);
            CHECK(r.applied[0] == i + 1);
        }
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_SET_MODE, 0)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_SET_MODE, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(A::modeIndex(a) == 7);  // unchanged by the refusals
    }

    covering(FOXAPI_OP_SET_BANDWIDTH);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_WFM))));
        FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_BANDWIDTH, 40e3));
        CHECK(ok(r));
        CHECK(A::bw(a) == 40e3);
        CHECK(A::bwIndex(a) == -1);  // no such step: the combo ticks nothing
        r = A::apply(a, num(FOXAPI_OP_SET_BANDWIDTH, 12.5e3));
        CHECK(A::bwIndex(a) == 2);
        const double hi = 0.9 * A::channelRate(a);
        r = A::apply(a, num(FOXAPI_OP_SET_BANDWIDTH, 1.0e7));
        CHECK(ok(r));
        CHECK(A::bw(a) == hi);
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        r = A::apply(a, num(FOXAPI_OP_SET_BANDWIDTH, 10.0));
        CHECK(A::bw(a) == 3000.0);  // the 3 kHz floor
    }

    covering(FOXAPP_OP_SET_BANDWIDTH_STEP);
    {
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_BANDWIDTH_STEP, 1))));
        CHECK(A::bwIndex(a) == 1);
        CHECK(A::bw(a) == cascade::gui::kBwHz[1]);
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SET_BANDWIDTH_STEP, cascade::gui::kBwCount)),
                      FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SET_BANDWIDTH_STEP, -1)), FOXAPI_OUT_OF_RANGE));
        CHECK(A::bwIndex(a) == 1);
    }

    covering(FOXAPP_OP_SET_BANDWIDTH_DRAG);
    {
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_BANDWIDTH_STEP, 2))));
        const FoxCommandResult r = A::apply(a, num(FOXAPP_OP_SET_BANDWIDTH_DRAG, 44e3));
        CHECK(ok(r));
        CHECK(A::bw(a) == 44e3);
        CHECK(A::bwIndex(a) == 2);  // the combo's step left where it was
        const FoxCommandResult c = A::apply(a, num(FOXAPP_OP_SET_BANDWIDTH_DRAG, 1.0));
        CHECK(A::bw(a) == 3000.0);
        CHECK((c.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    }

    covering(FOXAPI_OP_SET_SQUELCH);
    {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_SQUELCH, -33.0))));
        CHECK(A::squelch(a) == -33.0f);
    }

    covering(FOXAPI_OP_SET_VOLUME);
    {
        const FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_VOLUME, 0.4));
        CHECK(ok(r));
        CHECK(A::volume(a) == 0.4f);
        CHECK(r.applied[0] == static_cast<double>(0.4f));
    }

    covering(FOXAPI_OP_SET_MUTED);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MUTED, 1))));
        CHECK(A::muted(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MUTED, 0))));
        CHECK(!A::muted(a));
    }

    covering(FOXAPI_OP_SET_DEEMPHASIS);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEEMPHASIS, 1))));
        CHECK(A::deemph(a) == 1);
        CHECK(A::deemphUs(a) == 75.0);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEEMPHASIS, 0))));
        CHECK(A::deemphUs(a) == 50.0);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_SET_DEEMPHASIS, 3)), FOXAPI_OUT_OF_RANGE));
        CHECK(A::deemph(a) == 0);
    }

    covering(FOXAPI_OP_SET_STEREO);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_STEREO, 0))));
        CHECK(A::stereoOff(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_STEREO, 1))));
        CHECK(A::stereo(a));
    }

    covering(FOXAPI_OP_SET_NR);
    {
        CHECK(ok(A::apply(a, [] {
            FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NR, 1, 1);
            c.num[0] = 0.7;
            return c;
        }())));
        CHECK(A::nr(a) && A::nrPipe(a));
        CHECK(A::nrStrength(a) == 0.7f);
        CHECK(A::nrStrengthPipe(a) == 0.7f);
        // The switch alone leaves the strength alone.
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_NR, 0, 0))));
        CHECK(!A::nr(a) && !A::nrPipe(a));
        CHECK(A::nrStrength(a) == 0.7f);
    }

    covering(FOXAPP_OP_SET_NR_STRENGTH);
    {
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_NR_STRENGTH, 0.2))));
        CHECK(A::nrStrength(a) == 0.2f);
        CHECK(A::nrStrengthPipe(a) == 0.2f);
        CHECK(!A::nr(a));  // the switch left alone
    }

    covering(FOXAPI_OP_SET_NOTCH);
    {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NOTCH, 1, 1);
        c.num[0] = 1200.0;
        c.num[1] = 30.0;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::notch(a));
        CHECK(A::notchHz(a) == 1200.0f);
        CHECK(A::notchHzPipe(a) == 1200.0);
        CHECK(A::notchQ(a) == 30.0f);
        CHECK(A::notchQPipe(a) == 30.0);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_NOTCH, 0, 0))));
        CHECK(A::notchOff(a));
        CHECK(A::notchHz(a) == 1200.0f);  // untouched without ival[1]
    }

    covering(FOXAPP_OP_SET_NOTCH_FREQUENCY);
    {
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_NOTCH_FREQUENCY, 800.0))));
        CHECK(A::notchHz(a) == 800.0f);
        CHECK(A::notchHzPipe(a) == 800.0);
        CHECK(A::notchQ(a) == 30.0f);
        CHECK(A::notchOff(a));
    }

    covering(FOXAPP_OP_SET_NOTCH_Q);
    {
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_NOTCH_Q, 12.0))));
        CHECK(A::notchQ(a) == 12.0f);
        CHECK(A::notchQPipe(a) == 12.0);
        CHECK(A::notchHz(a) == 800.0f);
    }

    covering(FOXAPI_OP_SET_AUTO_NOTCH);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_AUTO_NOTCH, 1))));
        CHECK(A::autoNotch(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_AUTO_NOTCH, 0))));
        CHECK(!A::autoNotch(a));
    }

    // Not a finite number: refused before anything is touched.
    {
        const double v = A::vfo(a);
        FoxCommand c = num(FOXAPI_OP_SET_VFO_OFFSET, std::nan(""));
        CHECK(refused(A::apply(a, c), FOXAPI_BAD_ARGUMENT));
        c = num(FOXAPI_OP_SET_VOLUME, 0.5);
        c.num[3] = HUGE_VAL;  // an unused slot counts too
        CHECK(refused(A::apply(a, c), FOXAPI_BAD_ARGUMENT));
        CHECK(A::vfo(a) == v);
    }
    // An op nothing implements is UNSUPPORTED, and says so.
    {
        const FoxCommandResult r = A::apply(a, cmd::make(FOXAPI_OP_SERVER_CONFIG));
        CHECK(refused(r, FOXAPI_UNSUPPORTED));
        CHECK(std::strlen(r.message) > 0u);
        CHECK(refused(A::apply(a, cmd::make(0x7FFFu)), FOXAPI_UNSUPPORTED));
    }
}

// --- the display ---------------------------------------------------------------------------

void displayOps(AppWindow& a) {
    covering(FOXAPI_OP_SET_DISPLAY_RANGE);
    {
        FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_DISPLAY_RANGE, -100.0, -20.0));
        CHECK(ok(r));
        CHECK(A::dbMin(a) == -100.0f && A::dbMax(a) == -20.0f);
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) == 0u);
        // Too narrow: with both ends given, the minimum yields.
        r = A::apply(a, num(FOXAPI_OP_SET_DISPLAY_RANGE, -50.0, -45.0));
        CHECK(A::dbMax(a) == -45.0f);
        CHECK(A::dbMin(a) == -55.0f);
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    }

    covering(FOXAPP_OP_SET_DISPLAY_MIN);
    {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_DISPLAY_RANGE, -100.0, -20.0))));
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_DISPLAY_MIN, -80.0))));
        CHECK(A::dbMin(a) == -80.0f && A::dbMax(a) == -20.0f);
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_DISPLAY_MIN, -15.0))));
        CHECK(A::dbMax(a) == -20.0f);  // the end that did not move stays
        CHECK(A::dbMin(a) == -30.0f);
    }

    covering(FOXAPP_OP_SET_DISPLAY_MAX);
    {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_DISPLAY_RANGE, -100.0, -20.0))));
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_DISPLAY_MAX, -40.0))));
        CHECK(A::dbMax(a) == -40.0f && A::dbMin(a) == -100.0f);
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SET_DISPLAY_MAX, -105.0))));
        CHECK(A::dbMin(a) == -100.0f);  // the end that did not move stays
        CHECK(A::dbMax(a) == -90.0f);
    }

    covering(FOXAPI_OP_SET_BAND_PLAN);
    {
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SET_BAND_PLAN, "stage1-no-such-plan"))));
        CHECK(A::bandPlan(a) == "stage1-no-such-plan");
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_BAND_PLAN, "stage1-no-such-plan")),
                      FOXAPI_NO_CHANGE));
    }
}

// --- the source and the radio -----------------------------------------------------------------

void sourceOps(AppWindow& a) {
    covering(FOXAPI_OP_SCAN_DEVICES);
    {
        A::clearNative(a);
        A::clearSoapy(a);
        CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_SCAN_DEVICES))));
        CHECK(A::nativeCount(a) == 1u);
        CHECK(A::waitSoapy(a));
        CHECK(A::soapyCount(a) == 1u);
    }

    covering(FOXAPP_OP_SCAN_DEVICES_ON_OPEN);
    {
        // Scanned wholly already: the native list is re-read, SoapySDR is not
        // asked again.
        A::clearNative(a);
        A::markSoapyScanned(a);
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN))));
        CHECK(A::nativeCount(a) == 1u);
        CHECK(!A::soapyPending(a));
        // Never scanned: the lazy first scan runs.
        A::clearSoapy(a);
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN))));
        CHECK(A::waitSoapy(a));
        CHECK(A::soapyCount(a) == 1u);
    }

    covering(FOXAPP_OP_SET_NETWORK_USRP_SCAN);
    {
        A::clearSoapy(a);
        A::markSoapyScanned(a);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 1))));
        CHECK(A::networkUsrps(a));
        CHECK(A::soapyPending(a));  // ticking it rescans at once
        CHECK(A::waitSoapy(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 0))));
        CHECK(!A::networkUsrps(a));
        CHECK(!A::soapyPending(a));  // unticking does not
    }

    covering(FOXAPI_OP_SELECT_SOURCE);
    {
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "rtlsdr:serial=0001"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "rtlsdr");
        CHECK(A::haveDevice(a));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "siggen"))));
        CHECK(A::kind(a) == "siggen");
        CHECK(!A::haveDevice(a));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "soapy:driver=fake,serial=9"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "soapy");
        // A device nobody scanned is refused and said, never passed to a
        // driver.
        A::clearSourceError(a);
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "rtlsdr:serial=nobody")), FOXAPI_NOT_FOUND));
        CHECK(A::sourceError(a).find("no scanned device") != std::string::npos);
        CHECK(A::kind(a) == "soapy");
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "nonsense")), FOXAPI_NOT_FOUND));
        // The two panel rows only show their panel.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "row:iqfile"))));
        CHECK(A::sourceSel(a) == 1);
        CHECK(A::kind(a) == "soapy");
        // The IQ file's Open, with an id longer than FoxCommand::text holds
        // ("file:" + a 254-character path: past the 255-byte slot, inside
        // Windows' 260-character path limit).
        const std::size_t dirLen = (g_scratch.string() + "/").size();
        const std::size_t nameLen = (dirLen < 240u) ? 254u - dirLen - 4u : 8u;
        const std::filesystem::path wav = writeIqWav(std::string(nameLen, 'q') + ".wav");
        std::printf("      I/Q file path: %zu characters\n", wav.string().size());
        const cmd::QueuedCommand fileCmd = text(FOXAPI_OP_SELECT_SOURCE, "file:" + wav.string());
        CHECK(!fileCmd.longText.empty());
        A::clearSourceError(a);
        CHECK(ok(A::apply(a, fileCmd)));
        if (A::kind(a) != "file") { std::printf("      source error: %s\n", A::sourceError(a).c_str()); }
        CHECK(A::kind(a) == "file");
        CHECK(A::rate(a) == 250000.0);
        // The Pluto panel's Open: the typed address reaches the driver.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "open-pluto:uri=ip:192.0.2.7"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "pluto");
        {
            std::lock_guard<std::mutex> lk(FakeRadio::argsMutex());
            CHECK(FakeRadio::lastArgs() == "uri=ip:192.0.2.7");
        }
        // The sound card panel's Open.
        A::setSoundCardForm(a);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "row:soundcard"))));
        CHECK(A::waitCard(a));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"))));
        CHECK(A::waitCard(a));
        CHECK(A::kind(a) == "soundcard");

        covering(FOXAPP_OP_SOUNDCARD_IQ_CENTRE);
        // A card running in REAL mode keeps a new centre for the next Open.
        CHECK(refused(A::apply(a, num(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, 7.1e6)), FOXAPI_NO_CHANGE));
        // One running in I/Q mode takes it at once, and the receiver follows.
        A::setSoundCardFormIq(a);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"))));
        CHECK(A::waitCard(a));
        CHECK(A::kind(a) == "soundcard");
        CHECK(ok(A::apply(a, num(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, 7.1e6))));
        CHECK(A::liveIqCentre(a) == 7.1e6);
        CHECK(A::centre(a) == 7.1e6);
        // Back to a radio for the device ops below.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "rtlsdr:serial=0001"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "rtlsdr");
    }

    covering(FOXAPI_OP_SET_SAMPLE_RATE);
    {
        const FoxCommandResult r = A::apply(a, num(FOXAPI_OP_SET_SAMPLE_RATE, 2.4e6));
        CHECK(ok(r));
        CHECK(A::rate(a) == 2.4e6);
        CHECK(A::rateIndex(a) == 2);  // the combo follows the readback
        CHECK(A::inputRate(a) == 2.4e6);  // the DSP chain follows the rate
    }

    covering(FOXAPI_OP_SET_GAIN);
    {
        const FoxCommandResult r = A::apply(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 21.4));
        CHECK(ok(r));
        CHECK(A::radioGain(a, "LNA") == 21.0);
        CHECK(A::gain(a, "LNA") == 21.0f);  // THE READBACK, not the request
        CHECK(r.applied[0] == 21.0);
        A::clearSourceError(a);
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_GAIN, "NOPE", 0, 0, 3.0)), FOXAPI_FAILED));
        CHECK(!A::sourceError(a).empty());
    }

    covering(FOXAPP_OP_SET_GAIN_NO_READBACK);
    {
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_GAIN_NO_READBACK, "LNA", 0, 0, 23.4))));
        CHECK(A::radioGain(a, "LNA") == 23.0);
        CHECK(A::gain(a, "LNA") == 23.4f);  // the knob keeps what it asked for
        CHECK(refused(A::apply(a, text(FOXAPP_OP_SET_GAIN_NO_READBACK, "XYZ", 0, 0, 3.0)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_SET_DEVICE_AGC);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEVICE_AGC, 1))));
        CHECK(A::agc(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEVICE_AGC, 0))));
        CHECK(A::agcOff(a));
    }

    covering(FOXAPI_OP_SET_ANTENNA);
    {
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SET_ANTENNA, "B"))));
        CHECK(A::antenna(a) == "B");
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_ANTENNA, "Z")), FOXAPI_FAILED));
        CHECK(A::antenna(a) == "B");
    }

    covering(FOXAPI_OP_SET_DEVICE_OPTION);
    {
        // The fake radio is neither an RSP nor an RX888: every switch is
        // refused, by the op's own refusal (not the unknown-op one), and
        // nothing changes. (The switch bodies are the Source panel's own,
        // moved; an RSP or RX888 on a fake transport is not built here.)
        const FoxCommandResult r = A::apply(a, text(FOXAPI_OP_SET_DEVICE_OPTION, "rf_notch", 1));
        CHECK(refused(r, FOXAPI_UNSUPPORTED));
        CHECK(std::string(r.message) == "this radio has no such switch");
        CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_DEVICE_OPTION, "dither", 1)), FOXAPI_UNSUPPORTED));
    }

    covering(FOXAPI_OP_SET_BIAS_TEE);
    {
        A::setBiasStandIn(a);  // FOXSDR_FORCE_BIAS_KEY=accept's stand-in driver
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_BIAS_TEE, 1))));
        CHECK(A::biasOn(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_BIAS_TEE, 0))));
        CHECK(!A::biasOn(a));
    }

    covering(FOXAPP_OP_SET_CONVERTER);
    {
        FoxCommand c = ints(FOXAPP_OP_SET_CONVERTER, 1, 0);  // up
        c.num[0] = 125.0e6;
        CHECK(ok(A::apply(a, c)));
        const cascade::core::ConverterSetting s = A::converter(a);
        CHECK(s.mode == cascade::core::ConverterMode::Up);
        CHECK(s.loHz == 125.0e6);
        CHECK(!s.inverted);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_CONVERTER, 0, 0))));
        CHECK(A::converter(a).mode == cascade::core::ConverterMode::Off);
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SET_CONVERTER, 3, 0)), FOXAPI_OUT_OF_RANGE));
    }

    // A QUEUED RADIO COMMAND NEVER LANDS ON A DIFFERENT RADIO. A gain asked of
    // the open radio, then that radio replaced (a new open finishing between
    // the widget and the drain): the drain drops the gain rather than set it
    // on the newcomer - while a receiver command queued beside it still
    // applies. With no change of radio, the same queued gain does land.
    std::printf("  [queued radio commands and a change of radio]\n");
    {
        CHECK(A::haveDevice(a));
        A::submit(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 17.0));
        A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -44.0));
        // (Another radio: selecting the one already open reopens nothing.)
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "soapy:driver=fake,serial=9"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "soapy");
        CHECK(A::haveDevice(a));
        const double before = A::radioGain(a, "LNA");
        CHECK(before != 17.0);
        A::drain(a);
        CHECK(A::queued(a) == 0u);
        CHECK(A::radioGain(a, "LNA") == before);  // dropped, not applied to the new radio
        CHECK(A::squelch(a) == -44.0f);             // the receiver's own command applied
        // The same radio throughout: applied.
        A::submit(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 17.0));
        A::drain(a);
        CHECK(A::radioGain(a, "LNA") == 17.0);
    }

    // No radio open: the radio's own ops say so and change nothing.
    CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "siggen"))));
    CHECK(refused(A::apply(a, num(FOXAPI_OP_SET_SAMPLE_RATE, 1.024e6)), FOXAPI_NO_DEVICE));
    CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 3.0)), FOXAPI_NO_DEVICE));
    CHECK(refused(A::apply(a, ints(FOXAPI_OP_SET_DEVICE_AGC, 1)), FOXAPI_NO_DEVICE));
    CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_ANTENNA, "A")), FOXAPI_NO_DEVICE));
    CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_DEVICE_OPTION, "hdr", 1)), FOXAPI_NO_DEVICE));
}

// --- the recorder -----------------------------------------------------------------------------

void recorderOps(AppWindow& a) {
    A::setRecordDir(a, (g_scratch / "rec").string());
    covering(FOXAPI_OP_RECORD_IQ);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_IQ, 1))));
        CHECK(A::recIq(a));
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_RECORD_IQ, 1)), FOXAPI_NO_CHANGE));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_IQ, 0))));
        CHECK(!A::recIq(a));
    }
    covering(FOXAPI_OP_RECORD_AUDIO);
    {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_AUDIO, 1))));
        CHECK(A::recAudio(a));
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_RECORD_AUDIO, 1)), FOXAPI_NO_CHANGE));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RECORD_AUDIO, 0))));
        CHECK(!A::recAudio(a));
    }
    // Takes are files in the scratch folder, never the user's recordings.
    std::error_code ec;
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(g_scratch / "rec", ec)) {
        (void)e;
        ++files;
    }
    CHECK(files >= 2u);
}

// --- bookmarks and the scanner ---------------------------------------------------------------

void bookmarkOps(AppWindow& a) {
    covering(FOXAPI_OP_BOOKMARK_ADD);
    {
        const std::size_t before = A::bookmarks(a).size();
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 145.5e6))));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_BOOKMARK_ADD, "Repeater"))));
        CHECK(A::bookmarks(a).size() == before + 1u);
        bool found = false;
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
            if (b.name == "Repeater") {
                found = true;
                CHECK(b.freqHz == 145.5e6);
                CHECK(b.id != 0u);
            }
        }
        CHECK(found);
        // A nameless add is named by its frequency (the desktop's rule).
        CHECK(ok(A::apply(a, text(FOXAPI_OP_BOOKMARK_ADD, ""))));
        found = false;
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) { found = found || b.name == "145.5000 MHz"; }
        CHECK(found);
    }

    covering(FOXAPI_OP_BOOKMARK_TUNE);
    {
        const std::uint64_t id = A::addBookmark(a, "Airband", 124.2e6, "AM", 8000.0);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_WFM))));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_BOOKMARK_TUNE, static_cast<std::int64_t>(id)))));
        CHECK(A::tuned(a) == 124.2e6);
        CHECK(std::string(cascade::dsp::modeName(A::demod(a))) == "AM");
        CHECK(A::modeIndex(a) == 2);
        CHECK(A::bw(a) == 8000.0);
        CHECK(A::bwIndex(a) == -1);  // 8 kHz is none of the steps
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_BOOKMARK_TUNE, 0)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_BOOKMARK_TUNE, 999999)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_BOOKMARK_FAVOURITE);
    {
        const std::uint64_t id = A::addBookmark(a, "Star", 99.9e6, "WFM", 150000.0);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_BOOKMARK_FAVOURITE, static_cast<std::int64_t>(id), 1))));
        bool fav = false;
        std::uint64_t idAfter = 0;
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
            if (b.name == "Star") {
                fav = b.favourite;
                idAfter = b.id;
            }
        }
        CHECK(fav);
        CHECK(idAfter == id);  // an edited entry keeps its identity
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_BOOKMARK_FAVOURITE, static_cast<std::int64_t>(id), 0))));
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
            if (b.name == "Star") { CHECK(!b.favourite); }
        }
    }

    covering(FOXAPI_OP_BOOKMARK_REMOVE);
    {
        const std::uint64_t id = A::addBookmark(a, "Gone", 88.8e6, "WFM", 150000.0);
        // An entry inserted BELOW it moves its index; the id still finds it.
        (void)A::addBookmark(a, "Lower", 50.0e6, "NFM", 12500.0);
        const std::size_t before = A::bookmarks(a).size();
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_BOOKMARK_REMOVE, static_cast<std::int64_t>(id)))));
        CHECK(A::bookmarks(a).size() == before - 1u);
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) { CHECK(b.name != "Gone"); }
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_BOOKMARK_REMOVE, static_cast<std::int64_t>(id))),
                      FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_BOOKMARK_REMOVE_GROUP);
    {
        (void)A::addBookmark(a, "G1", 1.0e6, "AM", 9000.0, "Stage1Group");
        (void)A::addBookmark(a, "G2", 2.0e6, "AM", 9000.0, "Stage1Group");
        const std::size_t before = A::bookmarks(a).size();
        const FoxCommandResult r = A::apply(a, text(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, "Stage1Group"));
        CHECK(ok(r));
        CHECK(r.applied[0] == 2.0);
        CHECK(A::bookmarks(a).size() == before - 2u);
        CHECK(A::importNote(a).find("Stage1Group") != std::string::npos);
    }

    covering(FOXAPP_OP_BOOKMARK_IMPORT_FILE);
    {
        const std::filesystem::path csv = g_scratch / "list.csv";
        {
            std::ofstream f(csv, std::ios::binary);
            f << "frequency,name\n100100000,Imported One\n100200000,Imported Two\n";
        }
        const std::size_t before = A::bookmarks(a).size();
        CHECK(ok(A::apply(a, text(FOXAPP_OP_BOOKMARK_IMPORT_FILE, csv.string()))));
        CHECK(A::bookmarks(a).size() == before + 2u);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_IMPORT_FILE, "")), FOXAPI_BAD_ARGUMENT));
    }
}

// THE WEB REMOTE'S BOOKMARK ROWS, END TO END. A browser names a bookmark by
// its ROW in the last snapshot; applyControlRequest turns the row into the id
// that snapshot published. The case that matters: a desktop bookmark command
// drained at the top of the SAME frame as the web request (the drain runs
// before applyWebControls) shifts every list index after it - the row must
// still reach the bookmark the browser showed, never its new neighbour.
void webBookmarkRows(AppWindow& a) {
    std::printf("  [web bookmark rows]\n");
    const std::uint64_t idA = A::addBookmark(a, "Web A", 30.0e6, "NFM", 12500.0);
    (void)A::addBookmark(a, "Web B", 31.0e6, "NFM", 12500.0);
    (void)A::addBookmark(a, "Web C", 32.0e6, "AM", 9000.0);
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 31.0e6))));
    A::publishWeb(a);  // what the browser reads
    const int rowA = A::webRowOf(a, "Web A");
    const int rowB = A::webRowOf(a, "Web B");
    const int rowC = A::webRowOf(a, "Web C");
    std::printf("      published rows: A %d, B %d, C %d\n", rowA, rowB, rowC);
    CHECK(rowA >= 0 && rowB >= 0 && rowC >= 0);

    // Frame N+1: the desktop's x on A (submitted last frame) drains first...
    A::submit(a, ints(FOXAPI_OP_BOOKMARK_REMOVE, static_cast<std::int64_t>(idA)));
    A::drain(a);
    // ...then the browser's "tune to row C" arrives.
    cascade::net::ControlRequest tune;
    tune.bookmarkTune = rowC;
    A::webRequest(a, tune);
    std::printf("      after the web tune to row C: %.0f Hz, mode %s\n", A::tuned(a),
                cascade::dsp::modeName(A::demod(a)));
    CHECK(A::tuned(a) == 32.0e6);
    CHECK(std::string(cascade::dsp::modeName(A::demod(a))) == "AM");

    // A row whose bookmark has gone since the snapshot changes nothing.
    cascade::net::ControlRequest gone;
    gone.bookmarkTune = rowA;
    A::webRequest(a, gone);
    CHECK(A::tuned(a) == 32.0e6);

    // Frame N+2: the browser reads a fresh snapshot; a desktop ADD (the
    // bookmark lands at the tuned frequency, below B in the list) drains
    // first, then the browser removes the row it saw B on: B goes, and
    // nothing else does.
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 29.0e6))));
    A::publishWeb(a);
    const int rowB2 = A::webRowOf(a, "Web B");
    CHECK(rowB2 >= 0);
    A::submit(a, cmd::makeText(FOXAPI_OP_BOOKMARK_ADD, "Web D").c);
    A::drain(a);
    const std::size_t before = A::bookmarks(a).size();
    cascade::net::ControlRequest rm;
    rm.bookmarkRemove = rowB2;
    A::webRequest(a, rm);
    CHECK(A::bookmarks(a).size() == before - 1u);
    bool haveB = false, haveC = false, haveD = false;
    for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
        haveB = haveB || b.name == "Web B";
        haveC = haveC || b.name == "Web C";
        haveD = haveD || b.name == "Web D";
    }
    CHECK(!haveB);
    CHECK(haveC);
    CHECK(haveD);
}

void scannerOps(AppWindow& a) {
    covering(FOXAPP_OP_SCANNER_RANGE);
    {
        FoxCommand c = cmd::make(FOXAPP_OP_SCANNER_RANGE);
        c.ival[0] = 1 | 4;
        c.num[0] = 150.0e6;
        c.num[1] = 999.0e6;  // not in the mask: left alone
        c.num[2] = 25.0e3;
        const double stopBefore = A::scanStopMhz(a);
        CHECK(ok(A::apply(a, c)));
        CHECK(A::scanStartMhz(a) == 150.0);
        CHECK(A::scanStepKhz(a) == 25.0);
        CHECK(A::scanStopMhz(a) == stopBefore);
        CHECK(!A::scanning(a));
    }

    covering(FOXAPI_OP_SCANNER_RUN);
    {
        FoxCommand c = ints(FOXAPI_OP_SCANNER_RUN, 1);
        c.num[0] = 144.0e6;
        c.num[1] = 146.0e6;
        c.num[2] = 12.5e3;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::scanning(a));
        CHECK(A::scanStartMhz(a) == 144.0);
        CHECK(A::scanStopMhz(a) == 146.0);
        CHECK(A::scanStepKhz(a) == 12.5);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SCANNER_RUN, 0))));
        CHECK(!A::scanning(a));
    }

    covering(FOXAPI_OP_SCANNER_SKIP);
    {
        CHECK(refused(A::apply(a, cmd::make(FOXAPI_OP_SCANNER_SKIP)), FOXAPI_NO_CHANGE));
        FoxCommand c = ints(FOXAPI_OP_SCANNER_RUN, 1);
        c.num[0] = 144.0e6;
        c.num[1] = 146.0e6;
        c.num[2] = 12.5e3;
        CHECK(ok(A::apply(a, c)));
        CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_SCANNER_SKIP))));
        CHECK(A::scanning(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SCANNER_RUN, 0))));
    }

    covering(FOXAPI_OP_SCANNER_CONFIG);
    {
        FoxCommand c = cmd::make(FOXAPI_OP_SCANNER_CONFIG);
        c.num[0] = 150.0;
        c.num[1] = 900.0;
        c.num[2] = 1500.0;
        c.num[3] = 4000.0;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::scanDwell(a) == 150.0);
        CHECK(A::scanListen(a) == 4000.0);
    }
}

// --- plugins and the store -------------------------------------------------------------------

void pluginOps(AppWindow& a) {
    const std::string key = A::fixtureKey(a);
    std::printf("      fixture plugin: \"%s\" from %s\n", key.c_str(), A::pluginDirNow(a).c_str());
    CHECK(!key.empty());
    if (key.empty()) { return; }

    covering(FOXAPI_OP_DECODER_STOP);
    CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_STOP, key))));
    CHECK(A::stopped(a, key));
    CHECK(refused(A::apply(a, text(FOXAPI_OP_DECODER_STOP, "")), FOXAPI_BAD_ARGUMENT));

    covering(FOXAPI_OP_DECODER_START);
    CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_START, key))));
    CHECK(!A::stopped(a, key));

    covering(FOXAPI_OP_DECODER_STOP_ALL);
    {
        // Stops every decoder being FED: run the receiver on the fixture's
        // own preset so it is.
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 1))));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_DECODER_STOP_ALL, 0))));
        CHECK(A::stopped(a, key));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_START, key))));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 0))));
    }

    covering(FOXAPP_OP_DECODER_STOP_LIST);
    {
        CHECK(ok(A::apply(a, text(FOXAPP_OP_DECODER_STOP_LIST, key + "\nnot-loaded.dll", 0))));
        CHECK(A::stopped(a, key));
        CHECK(A::stopped(a, "not-loaded.dll"));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_START, key))));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_START, "not-loaded.dll"))));
        // The mute dialog's form: the same stop, and the mute it caused ends.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_DECODER_STOP_LIST, key, 1))));
        CHECK(A::stopped(a, key));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_DECODER_START, key))));
    }

    covering(FOXAPI_OP_PLUGIN_PRESET);
    {
        // By file key (the preset bars) and by display name (the web remote).
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, key, 1))));
        CHECK(A::tuned(a) == 145.5e6);
        CHECK(std::string(cascade::dsp::modeName(A::demod(a))) == "NFM");
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, "Stage One Fixture", 0))));
        CHECK(A::tuned(a) == 100.1e6);
        CHECK(std::string(cascade::dsp::modeName(A::demod(a))) == "WFM");
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, key, 2)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, key, -1)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, "nobody", 0)), FOXAPI_NOT_FOUND));
        CHECK(A::tuned(a) == 100.1e6);
    }

    covering(FOXAPI_OP_USER_PRESET_SAVE);
    {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 146.0e6))));
        const std::size_t before = A::userPresetCount(a);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_USER_PRESET_SAVE, key))));
        CHECK(A::userPresetCount(a) == before + 1u);
        CHECK(A::presetNote(a).find("146.0000") != std::string::npos);
    }

    covering(FOXAPP_OP_USER_PRESET_APPLY);
    {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 100.0e6))));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_USER_PRESET_APPLY, key, 0))));
        CHECK(A::tuned(a) == 146.0e6);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_USER_PRESET_APPLY, key, 5)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPP_OP_USER_PRESET_FORGET_AT);
    {
        const std::size_t before = A::userPresetCount(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_USER_PRESET_FORGET_AT, key, 0))));
        CHECK(A::userPresetCount(a) == before - 1u);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_USER_PRESET_FORGET_AT, key, -1)), FOXAPI_OUT_OF_RANGE));
    }

    covering(FOXAPP_OP_PLUGIN_AUTO_PRESET);
    {
        // Opening its window, away from both presets: the first is applied.
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 433.0e6))));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_AUTO_PRESET, key, 1))));
        CHECK(A::tuned(a) == 100.1e6);
        // Already on one of them: nothing moves.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_PRESET, key, 1))));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_AUTO_PRESET, key, 0))));
        CHECK(A::tuned(a) == 145.5e6);
    }

    covering(FOXAPI_OP_PLUGIN_GRANT);
    {
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_GRANT, key, 1, 1))));
        CHECK(A::tuneGranted(a, key));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_GRANT, key, 2, 1))));
        CHECK(A::settingsGranted(a, key));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_GRANT, key, 1, 0))));
        CHECK(!A::tuneGranted(a, key));
        CHECK(A::settingsGranted(a, key));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_GRANT, key, 2, 0))));
        CHECK(!A::settingsGranted(a, key));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_GRANT, key, 3, 1)), FOXAPI_OUT_OF_RANGE));
    }

    covering(FOXAPI_OP_PLUGIN_COMMAND);
    {
        // The fixture added key 7 when it was attached.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_COMMAND, key, 7))));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_COMMAND, key, 8)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_COMMAND, "nobody", 7)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_PLUGIN_MUTE);
    {
        const bool before = A::mutes(a);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_MUTE, key, before ? 0 : 1))));
        CHECK(A::mutes(a) == !before);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_PLUGIN_MUTE, key, before ? 1 : 0))));
        CHECK(A::mutes(a) == before);
        CHECK(refused(A::apply(a, text(FOXAPI_OP_PLUGIN_MUTE, "nobody", 1)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_PLUGIN_RESCAN);
    {
        CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_PLUGIN_RESCAN))));
        CHECK(A::fixtureKey(a) == key);  // unloaded and loaded again
    }
}

void storeOps(AppWindow& a) {
    A::setCatalogueUrl(a, "https://127.0.0.1:9/index.json");

    covering(FOXAPI_OP_STORE_FETCH);
    {
        CHECK(!A::storeBusy(a));
        CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_STORE_FETCH))));
        CHECK(A::storeBusy(a));  // the fetch is in flight
        CHECK(A::waitStore(a));
    }

    covering(FOXAPI_OP_STORE_CANCEL);
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_STORE_CANCEL))));

    covering(FOXAPI_OP_STORE_INSTALL);
    {
        A::injectCatalogue(a);
        A::clearInstallError(a);
        CHECK(refused(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "stage1-catalogue-only", 0)), FOXAPI_DENIED));
        CHECK(A::installError(a).find("legal notice") != std::string::npos);
        CHECK(!A::storeBusy(a));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "no-such-id", 1)), FOXAPI_NOT_FOUND));
        CHECK(A::installError(a).find("no catalogue entry") != std::string::npos);
        CHECK(ok(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "stage1-catalogue-only", 1))));
        CHECK(A::storeBusy(a));  // the download is in flight (and cannot land)
        CHECK(A::waitStore(a));
    }

    covering(FOXAPI_OP_STORE_UPDATE_ALL);
    {
        A::injectCatalogue(a);
        A::clearInstallError(a);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_STORE_UPDATE_ALL, 1))));
        CHECK(A::addAllActive(a));
        A::stopAddAll(a);
    }

    covering(FOXAPP_OP_STORE_UPDATE);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_UPDATE, "stage1-catalogue-only")), FOXAPI_NOT_FOUND));

    covering(FOXAPP_OP_STORE_REMOVE_BLOCKED);
    {
        // A module name in THIS platform's form: the sanitiser refuses any
        // other ending (".dll" here is not a plugin on Linux).
#if defined(_WIN32)
        const std::string blocked = "blocked_stage1.dll";
#else
        const std::string blocked = "blocked_stage1.so";
#endif
        const std::filesystem::path aside = g_plugins / (blocked + ".disabled");
        { std::ofstream(aside, std::ios::binary) << "not a module"; }
        CHECK(std::filesystem::exists(aside));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_REMOVE_BLOCKED, blocked))));
        CHECK(!std::filesystem::exists(aside));
    }

    covering(FOXAPI_OP_STORE_REMOVE);
    {
        const std::string key = A::fixtureKey(a);
        CHECK(!key.empty());
        CHECK(ok(A::apply(a, text(FOXAPI_OP_STORE_REMOVE, key))));
        CHECK(A::fixture(a) == nullptr);
        CHECK(!std::filesystem::exists(g_plugins / key));
    }
}

// --- the patch page, the transmitter, audio, position, GPS -------------------------------------

void patchOps(AppWindow& a) {
    covering(FOXAPI_OP_PATCH_RUN);
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_PATCH_RUN, 1))));
    CHECK(A::patchRunning(a));
    CHECK(refused(A::apply(a, ints(FOXAPI_OP_PATCH_RUN, 1)), FOXAPI_NO_CHANGE));
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_PATCH_RUN, 0))));
    CHECK(!A::patchRunning(a));

    covering(FOXAPI_OP_PATCH_ALL_OFF);
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_PATCH_RUN, 1))));
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_PATCH_ALL_OFF))));
    CHECK(!A::patchRunning(a));
}

void transmitterOps(AppWindow& a) {
    cascade::test::FakeTxIiod d;
    std::string err;
    CHECK(d.start(err));
    cascade::test::stockTxBoard(d);
    const std::string args = "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(d.port()));

    covering(FOXAPI_OP_TX_OPEN);
    CHECK(ok(A::apply(a, text(FOXAPI_OP_TX_OPEN, args))));
    CHECK(A::haveTx(a));
    CHECK(A::txArgs(a) == args);

    covering(FOXAPI_OP_TX_SET_MODE);
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_SET_MODE, FOXAPI_TX_MODE_USB))));
    CHECK(A::txMode(a) == 3);
    CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_SET_MODE, 5)), FOXAPI_OUT_OF_RANGE));

    covering(FOXAPI_OP_TX_SET_POWER);
    {
        const FoxCommandResult r = A::apply(a, num(FOXAPI_OP_TX_SET_POWER, -20.0));
        CHECK(ok(r));
        CHECK(A::txPower(a) == r.applied[0]);
        CHECK(std::fabs(A::txPower(a) - -20.0) < 0.5);  // the board's own step, read back
    }

    covering(FOXAPI_OP_TX_SET_SPLIT);
    {
        CHECK(ok(A::apply(a, numInt(FOXAPI_OP_TX_SET_SPLIT, 435.0e6, 1))));
        CHECK(A::txSplit(a));
        CHECK(A::txSplitHz(a) == 435.0e6);
        CHECK(std::fabs(A::txFrequency(a) - 435.0e6) < 1.0);
    }

    covering(FOXAPI_OP_TX_SET_FREQUENCY);
    CHECK(ok(A::apply(a, num(FOXAPI_OP_TX_SET_FREQUENCY, 436.0e6))));
    CHECK(A::txSplitHz(a) == 436.0e6);
    CHECK(std::fabs(A::txFrequency(a) - 436.0e6) < 1.0);
    CHECK(ok(A::apply(a, numInt(FOXAPI_OP_TX_SET_SPLIT, 0.0, 0))));
    CHECK(!A::txSplit(a));

    covering(FOXAPI_OP_TX_SET_INPUT);
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_SET_INPUT, 1))));  // the tone: opens nothing
    CHECK(A::txInput(a) == 1);
    CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_SET_INPUT, 2)), FOXAPI_OUT_OF_RANGE));

    covering(FOXAPI_OP_TX_SET_TONE);
    {
        const FoxCommandResult r = A::apply(a, num(FOXAPI_OP_TX_SET_TONE, 1500.0));
        CHECK(ok(r));
        CHECK(A::txTone(a) == 1500.0);
        CHECK(r.applied[0] == 1500.0);
    }

    covering(FOXAPI_OP_TX_SET_MONITOR);
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_SET_MONITOR, 1))));
    CHECK(A::txMonitor(a));
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_SET_MONITOR, 0))));
    CHECK(!A::txMonitor(a));

    covering(FOXAPI_OP_TX_PTT);
    {
        // The REMOTE key: refused while the page is shut, a bounded hold
        // while it is open, released by PTT 0.
        A::setTransmitOpen(a, false);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_PTT, 1)), FOXAPI_NO_DEVICE));
        CHECK(A::remoteHoldMs(a) == 0);
        A::setTransmitOpen(a, true);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_PTT, 1))));
        CHECK(A::remoteHoldMs(a) > 0);
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_TX_PTT, 0))));
        CHECK(A::remoteHoldMs(a) == 0);
        A::setTransmitOpen(a, false);
    }

    covering(FOXAPI_OP_TX_CLOSE);
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_TX_CLOSE))));
    CHECK(!A::haveTx(a));
    // An open that finds nothing there says so.
    CHECK(refused(A::apply(a, text(FOXAPI_OP_TX_OPEN, "uri=ip:127.0.0.1:9")), FOXAPI_FAILED));
    CHECK(!A::haveTx(a));
}

void otherOps(AppWindow& a) {
    covering(FOXAPI_OP_AUDIO_DEVICE);
    {
        // An index PortAudio does not have: the list's pick moves, the open
        // fails, and no speaker is ever opened by this test.
        A::addAudioDevice(a, 9999, "Stage1 phantom output");
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_AUDIO_DEVICE, 9999))));
        CHECK(A::audioDeviceIndex(a) >= 0);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_AUDIO_DEVICE, 12345)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPI_OP_SET_POSITION);
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_POSITION, 53.8, -1.55))));
    CHECK(A::positionSet(a, 53.8, -1.55));
    CHECK(refused(A::apply(a, num(FOXAPI_OP_SET_POSITION, 95.0, 0.0)), FOXAPI_OUT_OF_RANGE));
    CHECK(refused(A::apply(a, num(FOXAPI_OP_SET_POSITION, 0.0, 0.0)), FOXAPI_OUT_OF_RANGE));
    CHECK(A::positionSet(a, 53.8, -1.55));

    covering(FOXAPI_OP_GPS);
    {
        CHECK(refused(A::apply(a, text(FOXAPI_OP_GPS, "", 1, 0, 9600.0)), FOXAPI_BAD_ARGUMENT));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_GPS, "STAGE1-NO-SUCH-PORT", 1, 0, 9600.0))));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_GPS, "", 0))));
        CHECK(!A::gpsListening(a));
    }
}

// --- the queue ------------------------------------------------------------------------------------

void queueDrainsInOrder(AppWindow& a) {
    std::printf("  [the local queue]\n");
    // A submitted command changes nothing until the drain; the drain applies
    // every one, in order, and empties the queue.
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_SQUELCH, -10.0))));
    A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -20.0));
    A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -30.0));
    CHECK(A::queued(a) == 2u);
    CHECK(A::squelch(a) == -10.0f);
    A::drain(a);
    CHECK(A::queued(a) == 0u);
    CHECK(A::squelch(a) == -30.0f);
}

void everyOpHasACase() {
    std::printf("  [coverage] every op applyCommand implements has a case, and only those\n");
    std::size_t n = 0;
    const std::uint32_t* ops = cmd::knownOps(n);
    std::set<std::uint32_t> known(ops, ops + n);
    std::printf("      %zu ops implemented, %zu tested\n", known.size(), g_tested.size());
    for (const std::uint32_t op : known) {
        if (g_tested.count(op) == 0) { std::printf("FAIL: no case for %s\n", cmd::opName(op)); }
        CHECK(g_tested.count(op) != 0);
    }
    for (const std::uint32_t op : g_tested) {
        if (known.count(op) == 0) { std::printf("FAIL: a case for %s, which is not implemented\n", cmd::opName(op)); }
        CHECK(known.count(op) != 0);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_apply_command\n");
    isolate();
    A::installHooks();
    ImGui::CreateContext();
    {
        AppWindow app;
        receiverOps(app);
        displayOps(app);
        sourceOps(app);
        recorderOps(app);
        bookmarkOps(app);
        webBookmarkRows(app);
        scannerOps(app);
        pluginOps(app);
        storeOps(app);
        patchOps(app);
        transmitterOps(app);
        otherOps(app);
        queueDrainsInOrder(app);
    }
    everyOpHasACase();
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_apply_command");
}
