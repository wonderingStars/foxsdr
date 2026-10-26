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
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/patch_draft.hpp"
#include "core/config.hpp"
#include "engine/soundcard_panel.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "iiod_fake_server.hpp"
#include "imgui.h"
#include "net/web_control.hpp"
#include "source/device_source.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"
#include "web_request_seam.hpp"
#include "tx_front_end.hpp"

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
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeNativeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &fakeSoapyScan;
        cascade::engine::Engine::testHooks_.pluginDir = &pluginDirHook;
        cascade::engine::Engine::testHooks_.soundCardBackend = &makeCard;
    }

    // EVERY ENGINE FIELD BELOW IS THE PASS'S (the pump move's step P6): a read is made, and a write or a call is done, through
    // passseam::read/on - on the pass's own thread, with the pass out of the way, when it free-runs (FOXSDR_TEST_PASS=free); right here
    // otherwise. What a read returns is a COPY made with the pass out of the way, never a reference into an engine member. The cases
    // whose meaning needs NO pass between two of their steps (a submit and the drain that must follow a change of radio, a command and
    // the flag only the next pass acts on) run those steps as ONE block with exclusive() - a block in which the calls below run
    // directly, the pass being the thread that runs them.
    template <class F>
    static auto rd(AppWindow& a, F f) -> std::decay_t<decltype(f())> {
        using R = std::decay_t<decltype(f())>;
        return passseam::read<R>(rawEngine(a.engine_), std::function<R()>(f));
    }
    template <class F>
    static void wr(AppWindow& a, F f) { passseam::on(rawEngine(a.engine_), std::function<void()>(f)); }
    template <class F>
    static void exclusive(AppWindow& a, F f) { wr(a, f); }
    // A WAIT FOR WORK THE PASS COMPLETES ITSELF (an open, a scan, a fetch): inline and stepped the test polls it as the pass's workers
    // would (no pass runs here); free-running the pass's own workers poll it and the test only reads the flag - unless the wait is made
    // inside an exclusive() block, where the pass is the thread waiting and so polls it itself.
    template <class Poll, class Done>
    static bool waitPolled(AppWindow& a, Poll poll, Done done) {
        const bool byPass = passseam::mode() == passseam::Mode::Free && !rawEngine(a.engine_).onControlThread();  // (the CALLER's thread)
        return waitFor([&] {
            return rd(a, [&] {
                if (!byPass) { poll(); }
                return done();
            });
        });
    }

    // A RECORDING'S FILE OPENS ON A WORKER (0.99.63): a take just asked for is armed on the pass that collects the file. Waits (bounded) until no
    // start is out, polling as the pass does, so a test that asked for a take then reads it "recording".
    static bool recordStartsOut(AppWindow& a) {
        return passseam::read<bool>(rawEngine(a.engine_), [&] {
            return rawEngine(a.engine_).iqStart_.pending() || rawEngine(a.engine_).audioStart_.pending() || rawEngine(a.engine_).iqStartQueued_ ||
                   rawEngine(a.engine_).audioStartQueued_;
        });
    }
    static void settleRecordStarts(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (recordStartsOut(a) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            passseam::custom(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pollRecordStarts(rawEngine(a.engine_).clockS()); });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c, const std::string& lt = {}) {
        const FoxCommandResult r = rd(a, [&] { return rawEngine(a.engine_).applyCommand(c, lt); });
        if (c.op == FOXAPI_OP_RECORD_IQ || c.op == FOXAPI_OP_RECORD_AUDIO) { settleRecordStarts(a); }
        return r;
    }
    static FoxCommandResult apply(AppWindow& a, const cmd::QueuedCommand& q) {
        const FoxCommandResult r = rd(a, [&] { return rawEngine(a.engine_).applyCommand(q.c, q.longText); });
        if (q.c.op == FOXAPI_OP_RECORD_IQ || q.c.op == FOXAPI_OP_RECORD_AUDIO) { settleRecordStarts(a); }
        return r;
    }
    // (Engine::submitCommand is the front end's own call from any thread: the queue has its own lock, and it wakes the pass.)
    static void submit(AppWindow& a, const FoxCommand& c) { rawEngine(a.engine_).submitCommand(c); }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { rawEngine(a.engine_).submitCommand(q); }
    static void drain(AppWindow& a) {
        if (rawEngine(a.engine_).onControlThread()) {  // (inside an exclusive() block, free-running: the pass is this thread)
            rawEngine(a.engine_).drainLocalCommands();
            return;
        }
        passseam::custom(rawEngine(a.engine_), [&] { rawEngine(a.engine_).drainLocalCommands(); });
    }
    static std::size_t queued(AppWindow& a) { return rawEngine(a.engine_).localCommandsQueuedForTest(); }  // (under the queue's own lock)

    // The receiver.
    static bool running(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.running(); }); }
    static double centre(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.activeSource().centerFrequencyHz(); }); }
    static double tuned(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).currentAbsoluteHz(); }); }
    static double vfo(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.vfoOffsetHz(); }); }
    static float vfoKhz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).vfoOffsetKhz_; }); }
    static double inputRate(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.inputRateHz(); }); }
    static double channelRate(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.channelRateHz(); }); }
    static int modeIndex(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).modeIndex_; }); }
    static cascade::dsp::DemodMode demod(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.demodMode(); }); }
    static double bw(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).vfoBandwidthHz_; }); }
    static int bwIndex(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).bandwidthIndex_; }); }
    static float squelch(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).squelchDb_; }); }
    static float volume(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).volume_; }); }
    static bool muted(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).userMuted_; }); }
    static int deemph(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).deemphIndex_; }); }
    static double deemphUs(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.deemphasisUs(); }); }
    static bool stereo(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).stereoEnabled_ && rawEngine(a.engine_).pipeline_.stereoEnabled(); });
    }
    static bool stereoOff(AppWindow& a) {
        return rd(a, [&] { return !rawEngine(a.engine_).stereoEnabled_ && !rawEngine(a.engine_).pipeline_.stereoEnabled(); });
    }
    static bool nr(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).nrEnabled_; }); }
    static bool nrPipe(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.noiseReductionEnabled(); }); }
    static float nrStrength(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).nrStrength_; }); }
    static float nrStrengthPipe(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.noiseReductionStrength(); }); }
    static bool notch(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).notchEnabled_ && rawEngine(a.engine_).pipeline_.notchEnabled(); });
    }
    static bool notchOff(AppWindow& a) {
        return rd(a, [&] { return !rawEngine(a.engine_).notchEnabled_ && !rawEngine(a.engine_).pipeline_.notchEnabled(); });
    }
    static float notchHz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).notchFreqHz_; }); }
    static double notchHzPipe(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.notchFrequencyHz(); }); }
    static float notchQ(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).notchQ_; }); }
    static double notchQPipe(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.notchQ(); }); }
    static bool autoNotch(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).autoNotch_ && rawEngine(a.engine_).pipeline_.autoNotchEnabled(); });
    }
    static float dbMin(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).dbMin_; }); }
    static float dbMax(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).dbMax_; }); }
    static std::string bandPlan(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).bandPlanSelection_; }); }

    // The recorder.
    static void setRecordDir(AppWindow& a, const std::string& d) { wr(a, [&] { rawEngine(a.engine_).recordDir_ = d; }); }
    static bool recIq(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).iqRecorder_.recording(); }); }
    static bool recAudio(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).audioRecorder_.recording(); }); }

    // The source.
    static std::size_t nativeCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).nativeDevices_.size(); }); }
    static void clearNative(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).nativeDevices_.clear(); }); }
    static std::size_t soapyCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soapyDevices_.size(); }); }
    static void clearSoapy(AppWindow& a) {
        wr(a, [&] {
            rawEngine(a.engine_).soapyDevices_.clear();
            rawEngine(a.engine_).soapyScanned_ = false;
            rawEngine(a.engine_).soapyScanPartial_ = false;
        });
    }
    static void markSoapyScanned(AppWindow& a) {
        wr(a, [&] {
            rawEngine(a.engine_).soapyScanned_ = true;
            rawEngine(a.engine_).soapyScanPartial_ = false;
        });
    }
    static bool soapyPending(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soapyScanPending_; }); }
    static bool waitSoapy(AppWindow& a) {
        return waitPolled(a, [&] { rawEngine(a.engine_).pollSourceAsync(); }, [&] { return !rawEngine(a.engine_).soapyScanPending_; });
    }
    // The patch page's device-list wish (engine/stage3b-pre B2).
    static bool patchListsWanted(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchListsWanted_; }); }
    static void setPatchListsWanted(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).patchListsWanted_ = v; }); }
    static bool patchScanWanted(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchScanWanted_; }); }
    static void setDeviceOpenPending(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).deviceOpenPending_ = v; }); }
    static void patchReconcile(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).patchReconcile(); }); }
    static void setPatchScanWanted(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).patchScanWanted_ = v; }); }
    static bool networkUsrps(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).lookForNetworkUsrps_; }); }
    static bool waitOpen(AppWindow& a) {
        return waitPolled(a, [&] { rawEngine(a.engine_).pollSourceAsync(); }, [&] { return !rawEngine(a.engine_).deviceOpenPending_; });
    }
    static bool waitCard(AppWindow& a) {
        return waitPolled(a, [&] { rawEngine(a.engine_).pollSoundCard(); },
                          [&] { return !rawEngine(a.engine_).soundCardOpenPending_ && !rawEngine(a.engine_).soundCardScanPending_; });
    }
    static std::string kind(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).sourceKind_; }); }
    static int sourceSel(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).sourceSel_; }); }
    static bool haveDevice(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).device_ != nullptr; }); }
    static std::string sourceError(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).sourceError_; }); }
    static void clearSourceError(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).sourceError_.clear(); }); }
    static void setSourceError(AppWindow& a, const std::string& s) { wr(a, [&] { rawEngine(a.engine_).sourceError_ = s; }); }
    static float gain(AppWindow& a, const char* name) {
        return rd(a, [&] {
            for (std::size_t i = 0; i < rawEngine(a.engine_).deviceGainNames_.size() && i < rawEngine(a.engine_).deviceGainsDb_.size(); ++i) {
                if (rawEngine(a.engine_).deviceGainNames_[i] == name) { return rawEngine(a.engine_).deviceGainsDb_[i]; }
            }
            return -999.0f;
        });
    }
    // NULL-SAFE, every one: a build in which the source was never selected
    // must report the failure, not crash the harness in the run that has
    // something to say.
    static double radioGain(AppWindow& a, const char* name) {
        return rd(a, [&] { return rawEngine(a.engine_).device_ != nullptr ? rawEngine(a.engine_).device_->gainDb(name) : -999.0; });
    }
    static bool agc(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).device_ != nullptr && rawEngine(a.engine_).deviceAgc_ && rawEngine(a.engine_).device_->autoGain(); });
    }
    static bool agcOff(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).device_ != nullptr && !rawEngine(a.engine_).deviceAgc_ && !rawEngine(a.engine_).device_->autoGain(); });
    }
    static std::string antenna(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).deviceAntenna_; }); }
    static double rate(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.activeSource().sampleRateHz(); }); }
    static int rateIndex(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).deviceRateIndex_; }); }
    static void setBiasStandIn(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).biasStandIn_ = cascade::gui::BiasStandIn::Accept; }); }
    static bool biasOn(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).biasStandInOn_; }); }
    static cascade::core::ConverterSetting converter(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pipeline_.converter(); }); }
    static void setSoundCardForm(AppWindow& a) { wr(a, [&] { soundCardFormHere(a); }); }
    static void setSoundCardFormIq(AppWindow& a) {
        wr(a, [&] {
            soundCardFormHere(a);
            rawEngine(a.engine_).soundCard_.format = cascade::source::SoundCardFormat::IqStereo;
            rawEngine(a.engine_).soundCard_.iqCentreHz = 7.0e6;
        });
    }
    static void soundCardFormHere(AppWindow& a) {
        rawEngine(a.engine_).soundCard_ = cascade::source::SoundCardSettings{};
        rawEngine(a.engine_).soundCard_.device = "Fake Card";
        rawEngine(a.engine_).soundCard_.hostApi = "Fake API";
        rawEngine(a.engine_).soundCard_.cardRateHz = 48000.0;
    }
    static double liveIqCentre(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soundCardLive_.iqCentreHz; }); }

    // Bookmarks and scanner.
    static std::vector<cascade::core::Bookmark> bookmarks(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).freqMgr_.list(); }); }
    static std::uint64_t addBookmark(AppWindow& a, const std::string& name, double hz, const char* mode,
                                     double bwHz, const std::string& group = {}) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = mode;
        b.bandwidthHz = bwHz;
        b.group = group;
        return rd(a, [&] {
            const int at = rawEngine(a.engine_).freqMgr_.add(b);
            return rawEngine(a.engine_).freqMgr_.list()[static_cast<std::size_t>(at)].id;
        });
    }
    static std::string importNote(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).bookmarkImportNote_; }); }
    static std::string recordDir(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).recordDir_; }); }
    // The engine/stage3b-pre 2c status-line commands.
    static void setGpsRefusal(AppWindow& a, const std::string& s) { wr(a, [&] { rawEngine(a.engine_).gpsRefusal_ = s; }); }
    static std::string gpsRefusal(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).gpsRefusal_; }); }
    static void setSoundCardMissing(AppWindow& a, const std::string& s) { wr(a, [&] { rawEngine(a.engine_).soundCardMissing_ = s; }); }
    static std::string soundCardMissing(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soundCardMissing_; }); }
    static void pushDecoderLogLine(AppWindow& a) {
        wr(a, [&] { rawEngine(a.engine_).decoderLog_.push_back(cascade::core::DecodedLine{"test", "line"}); });
    }
    static std::size_t decoderLogSize(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).decoderLog_.size(); }); }
    static std::uint64_t decoderLinesTotal(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).decoderLinesTotal_; }); }
    static cascade::core::DecodedLine decoderLogBack(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).decoderLog_.back(); }); }
    static void setSoapyScanDeferredLogged(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).soapyScanDeferredLogged_ = v; }); }
    static bool soapyScanDeferredLogged(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soapyScanDeferredLogged_; }); }
    static void setMutePopupQueued(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).mutePopupQueued_ = v; }); }
    static bool mutePopupQueued(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).mutePopupQueued_; }); }
    static bool muteKeptRunning(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).muteKeptRunning_; }); }
    static void setMutePopupSubject(AppWindow& a, const std::string& name, const std::string& key) {
        wr(a, [&] {
            rawEngine(a.engine_).mutePopup_.open = true;
            rawEngine(a.engine_).mutePopup_.names = {name};
            rawEngine(a.engine_).mutePopup_.keys = {key};
        });
    }
    static bool mutePopupOpen(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).mutePopup_.open; }); }
    static std::size_t mutePopupKeyCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).mutePopup_.keys.size(); }); }
    // The web remote's side: the snapshot a browser reads (and the row map it
    // leaves), and a request applied directly (web_request_seam.hpp: the product's translator, each command through applyCommand).
    static void publishWeb(AppWindow& a) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).publishReceiverState(); }); }
    // The row a browser sees a bookmark on: read off the PUBLISHED snapshot
    // (what /api/status serves - the web provider's own call), never off the
    // row map under test.
    static int webRowOf(AppWindow& a, const std::string& name) {
        const cascade::net::RadioStatus status = rawEngine(a.engine_).webStatusNow();
        const auto& rows = status.bookmarks;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].name == name) { return static_cast<int>(r); }
        }
        return -1;
    }
    static void webRequest(AppWindow& a, const cascade::net::ControlRequest& r) { wr(a, [&] { web_test::applyRequestDirect(rawEngine(a.engine_), r); }); }
    static bool scanning(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanner_.active(); }); }
    static double scanStartMhz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanStartMhz_; }); }
    static double scanStopMhz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanStopMhz_; }); }
    static double scanStepKhz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanStepKhz_; }); }
    static double scanDwell(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanDwellMs_; }); }
    static double scanListen(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanListenMs_; }); }
    static double scanHold(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanHoldMs_; }); }
    // The Airband monitor and the scanner's list mode (stub campaign B6).
    static bool scanTicked(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanTicked_; }); }
    static bool airbandListening(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).airbandListening_; }); }
    static bool airbandPending(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).airbandStartPending_; }); }
    static std::string airbandNote(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).airbandNote_; }); }
    static float airbandSquelch(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).airbandSquelchDb_; }); }
    static double airbandHold(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).airbandHoldS_; }); }
    static double scanResume(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).scanResumeMs_; }); }

    // Plugins and the store.
    static const cascade::core::LoadedPlugin* fixtureHere(AppWindow& a) {  // (with the pass out of the way only)
        for (const cascade::core::LoadedPlugin& p : rawEngine(a.engine_).pluginHost_.plugins()) {
            if (p.loaded && p.name == "Stage One Fixture") { return &p; }
        }
        return nullptr;
    }
    static bool haveFixture(AppWindow& a) { return rd(a, [&] { return fixtureHere(a) != nullptr; }); }
    static std::string fixtureKey(AppWindow& a) {
        return rd(a, [&] {
            const cascade::core::LoadedPlugin* p = fixtureHere(a);
            return p != nullptr ? cascade::core::pluginKey(*p) : std::string();
        });
    }
    static bool stopped(AppWindow& a, const std::string& key) { return rd(a, [&] { return rawEngine(a.engine_).pluginIsStopped(key); }); }
    static bool mutes(AppWindow& a) {
        return rd(a, [&] {
            const cascade::core::LoadedPlugin* p = fixtureHere(a);
            return p != nullptr && rawEngine(a.engine_).pluginMutes(*p);
        });
    }
    static bool tuneGranted(AppWindow& a, const std::string& key) {
        return rd(a, [&] {
            for (const std::string& k : rawEngine(a.engine_).pluginTuneAllowed_) {
                if (k == key) { return true; }
            }
            return false;
        });
    }
    static bool settingsGranted(AppWindow& a, const std::string& key) {
        return rd(a, [&] {
            for (const std::string& k : rawEngine(a.engine_).pluginSettingsAllowed_) {
                if (k == key) { return true; }
            }
            return false;
        });
    }
    static std::size_t userPresetCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).userPresets_.size(); }); }
    static std::string presetNote(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).presetNote_; }); }
    static void setCatalogueUrl(AppWindow& a, const std::string& u) { wr(a, [&] { rawEngine(a.engine_).pluginCatalogueUrl_ = u; }); }
    static std::string catalogueUrl(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pluginCatalogueUrl_; }); }
    static bool telemetryEnabled(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).telemetryEnabled_; }); }
    static std::string telemetryInstallId(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).telemetryInstallId_; }); }
    static std::vector<std::string> telemetryPanels(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).telemetryPanels_; }); }
    static cascade::source::SoundCardSettings soundCardForm(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soundCard_; }); }
    // (a picture on its way and a clean-up on a worker are transfers too: stub campaign B3)
    static bool storeBusyHere(AppWindow& a) {
        return rawEngine(a.engine_).catalogPending_ || rawEngine(a.engine_).installPending_ || rawEngine(a.engine_).screenshotPending_ ||
               rawEngine(a.engine_).cleanupPending_;
    }
    static std::vector<std::string> storeSites(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pluginSites_; }); }
    static std::string siteError(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).siteError_; }); }
    static bool storeBusy(AppWindow& a) { return rd(a, [&] { return storeBusyHere(a); }); }
    static bool waitStore(AppWindow& a) {
        return waitPolled(a, [&] { rawEngine(a.engine_).pollPluginAsync(); }, [&] { return !storeBusyHere(a); });
    }
    static std::string installError(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).installError_; }); }
    static void clearInstallError(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).installError_.clear(); }); }
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
        wr(a, [&] {
            rawEngine(a.engine_).catalog_.clear();
            rawEngine(a.engine_).catalog_.push_back(e);
        });
    }
    static bool addAllActive(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).addAllRun_.active; }); }
    static void stopAddAll(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).addAllRun_ = cascade::engine::Engine::AddAllRun{}; }); }
    static std::string pluginDirNow(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).pluginDir_; }); }

    // The patch page, the transmitter, audio, position, GPS.
    static bool patchRunning(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchRunning_; }); }
    static void setTransmitOpen(AppWindow& a, bool on) { wr(a, [&] { rawEngine(a.engine_).transmitOpen_ = on; }); }
    static bool transmitOpen(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitOpen_; }); }
    // (Only for what is thread-safe by construction: the API host's handle a session is opened on.)
    static cascade::engine::Engine& engine(AppWindow& a) { return rawEngine(a.engine_); }
    static std::uint64_t serialPortsScans(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).serialPortsScans_; }); }
    // The engine's pass as far as the key needs it - the queue applied, the
    // state published - without the audio half, which reads the window's
    // ImGui clock (no ImGui context in this test).
    static void pass(AppWindow& a) {
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            (void)rawEngine(a.engine_).pumpPublish();
        });
    }
    static bool haveTx(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitter_.haveSink(); }); }
    static std::int64_t remoteHoldMs(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitter_.remoteHoldRemainingMs(); }); }
    static std::uint64_t txOwner(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).api().txFacts().owner; }); }
    static int txMode(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitModeIndex_; }); }
    static bool txSplit(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitSplit_; }); }
    static double txSplitHz(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitSplitHz_; }); }
    static double txPower(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitPowerDb_; }); }
    static int txInput(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitInputIndex_; }); }
    static double txTone(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitToneHz_; }); }
    static bool txMonitor(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitMonitor_; }); }
    static std::string txArgs(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitArgs_; }); }
    static std::string plutoUri(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).plutoUri_; }); }
    static std::string rtlTcpAddr(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).rtlTcpAddr_; }); }
    static int audioBufferMs(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).audioBufferMs_; }); }
    static bool ppmOn(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).ppmCorrectionOn_; }); }
    static std::size_t ppmValueCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).ppmValues_.size(); }); }
    static double ppmValueFor(AppWindow& a, const std::string& key) {
        return rd(a, [&] { const auto& m = rawEngine(a.engine_).ppmValues_; const auto it = m.find(key); return it == m.end() ? 0.0 : it->second; });
    }
    // The patch decoders' pictures the engine keeps (API 0.3 8.12, part 3), and the graph they are pruned against.
    static cascade::core::patch::NodeId addPatchNode(AppWindow& a) {
        return rd(a, [&] { return rawEngine(a.engine_).patchGraph_.addNode(cascade::core::patch::NodeKind::Decoder, "prune probe"); });
    }
    static void removePatchNode(AppWindow& a, cascade::core::patch::NodeId id) {
        wr(a, [&] { (void)rawEngine(a.engine_).patchGraph_.removeNode(id); });
    }
    static void putPatchImage(AppWindow& a, cascade::core::patch::NodeId id) {
        cascade::core::HostImage h;
        h.plugin = "probe";
        h.width = 2;
        h.height = 1;
        h.format = CASCADE_IMAGE_GRAY8;
        h.pixels = {1, 2};
        wr(a, [&] { rawEngine(a.engine_).takePatchImage(id, std::move(h)); });
    }
    static bool hasPatchImage(AppWindow& a, cascade::core::patch::NodeId id) {
        return rd(a, [&] { return rawEngine(a.engine_).patchImages_.count(id) != 0; });
    }
    static void syncPatchImages(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).syncPatchImages(); }); }
    // FOXAPP_OP_PATCH_SET_GRAPH (docs/engine-stage3.md OPEN 6).
    static cascade::core::patch::Graph patchGraph(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchGraph_; }); }
    template <class F>
    static void changePatchGraph(AppWindow& a, F change) { wr(a, [&] { change(rawEngine(a.engine_).patchGraph_); }); }
    static std::uint64_t patchGraphEpoch(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchGraphEpoch_; }); }
    static std::uint64_t patchGraphVersion(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchGraphVersion_; }); }
    static bool patchPageOpen(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchPageOpen_; }); }
    static std::size_t patchRecordingCount(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchRecordings_.size(); }); }
    static double patchRecordingRate(AppWindow& a, std::size_t i) { return rd(a, [&] { return rawEngine(a.engine_).patchRecordings_[i].rateHz; }); }
    static bool patchWasOpen(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).patchWasOpen_; }); }
    static void setPatchRunning(AppWindow& a, bool v) { wr(a, [&] { rawEngine(a.engine_).patchRunning_ = v; }); }
    static void pumpPatch(AppWindow& a) { passseam::custom(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pumpPatch(); }); }
    static void setSoundCardsListed(AppWindow& a, bool on) { wr(a, [&] { rawEngine(a.engine_).soundCardListed_ = on; }); }
    static bool soundCardScanPending(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).soundCardScanPending_; }); }
    static double txFrequency(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).transmitter_.frequencyHz(); }); }
    static void addAudioDevice(AppWindow& a, int paIndex, const char* name) {
        wr(a, [&] { rawEngine(a.engine_).devices_.push_back({paIndex, name, false}); });
    }
    static int audioDeviceIndex(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).deviceIndex_; }); }
    static bool positionSet(AppWindow& a, double lat, double lon) {
        return rd(a, [&] { return rawEngine(a.engine_).rxSet_ && rawEngine(a.engine_).rxLat_ == lat && rawEngine(a.engine_).rxLon_ == lon; });
    }
    static bool gpsListening(AppWindow& a) { return rd(a, [&] { return rawEngine(a.engine_).gpsReader_.listening(); }); }
    // Window port group 14, part b (FOXAPP_STATUS_GPS_RESULT): a read that ended in failure, and one that is running.
    static int gpsState(AppWindow& a) { return rd(a, [&] { return static_cast<int>(rawEngine(a.engine_).gpsReader_.status().state); }); }
    static bool gpsFail(AppWindow& a) {
        wr(a, [&] {
            rawEngine(a.engine_).gpsReader_.setOpenerForTest(
                [](const std::string&, int, std::string& error) -> std::unique_ptr<cascade::core::ByteSource> {
                    error = "could not be opened: test";
                    return nullptr;
                });
            rawEngine(a.engine_).gpsReader_.start({"BAD1", 9600, 5.0});
        });
        for (int i = 0; i < 2000 && gpsState(a) != 3; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        return gpsState(a) == 3;
    }
    struct Silent final : cascade::core::ByteSource {
        int read(char*, std::size_t) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return 0;
        }
    };
    static bool gpsListen(AppWindow& a) {
        return rd(a, [&] {
            rawEngine(a.engine_).gpsReader_.start(std::make_unique<Silent>(), {"SILENT", 9600, 30.0});
            return static_cast<int>(rawEngine(a.engine_).gpsReader_.status().state) == 1;
        });
    }
    static void gpsStop(AppWindow& a) { wr(a, [&] { rawEngine(a.engine_).gpsReader_.stop(); }); }
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
    // The servers' configuration (stage 5a): the engine records it. (Since 5b its host also starts what is switched ON, on a worker; CAT
    // stays off here, so nothing opens. The strict parse and the worker for the password are test_engine_api_05a's, the listeners
    // test_server_host's; here the op has its case.)
    covering(FOXAPI_OP_SERVER_CONFIG);
    {
        CHECK(ok(A::apply(a, cmd::makeText(FOXAPI_OP_SERVER_CONFIG, "{\"cat\":{\"port\":4999}}"))));
        CHECK(refused(A::apply(a, cmd::makeText(FOXAPI_OP_SERVER_CONFIG, "{}")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, cmd::makeText(FOXAPI_OP_SERVER_CONFIG, "{\"cat\":{\"port\":80}}")), FOXAPI_BAD_ARGUMENT));
        CHECK(ok(A::apply(a, cmd::makeText(FOXAPI_OP_SERVER_CONFIG, "{\"cat\":{\"port\":4532}}"))));
    }
    // An op nothing implements is UNSUPPORTED, and says so.
    {
        const FoxCommandResult r = A::apply(a, cmd::make(FOXAPI_OP_UPDATE_CHECK));
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
        A::exclusive(a, [&] {
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN))));
            CHECK(A::nativeCount(a) == 1u);
            CHECK(!A::soapyPending(a));
        });
        // Never scanned: the lazy first scan runs.
        A::clearSoapy(a);
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN))));
        CHECK(A::waitSoapy(a));
        CHECK(A::soapyCount(a) == 1u);
    }

    // A Radio part added (0.99.40): the native walk ONLY - never the SoapySDR probe, and it asks for nothing more. (Not in
    // the op table until window port group 13, which sends it through the session: the table refused it as unknown.)
    covering(FOXAPP_OP_SCAN_NATIVE_ONLY);
    {
        A::clearNative(a);
        A::clearSoapy(a);
        CHECK(cascade::core::cmd::isKnownOp(FOXAPP_OP_SCAN_NATIVE_ONLY));
        A::exclusive(a, [&] {
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SCAN_NATIVE_ONLY))));
            CHECK(A::nativeCount(a) == 1u);       // the walk ran
            CHECK(!A::soapyPending(a));           // the vendor probe did not
            CHECK(!A::patchListsWanted(a));       // and nothing is owed
        });
    }

    covering(FOXAPP_OP_PATCH_RADIO_LIST_OPENED);
    {
        // engine/stage3b-pre B2: a Radio's device list opening. The native
        // list is re-read EVERY time; the SoapySDR half is a WISH, set only
        // the very first time ever (never immediately) - patchReconcile runs
        // it once the scan plan allows, proven separately in
        // test_patch_scan_wanted.cpp. Here: the wish and the one-time guard.
        A::clearNative(a);
        A::clearSoapy(a);
        // (One block: the pass's own patchReconcile consumes the wish, so a free-running pass between the command and these reads
        // would answer for it.)
        A::exclusive(a, [&] {
            CHECK(!A::patchListsWanted(a));
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_PATCH_RADIO_LIST_OPENED))));
            CHECK(A::nativeCount(a) == 1u);       // scanNative ran at once
            CHECK(!A::soapyPending(a));           // never scanSoapy() directly
            CHECK(A::patchListsWanted(a));        // the wish is now on
            CHECK(A::patchScanWanted(a));         // unscanned: SoapySDR is owed
        });
        // A second opening: the native list still refreshes, but the
        // first-open guard means the wish is not re-armed once it has been
        // consumed (simulated here by clearing it, as patchReconcile would).
        A::clearNative(a);
        A::exclusive(a, [&] {
            A::setPatchScanWanted(a, false);      // as if patchReconcile ran it
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_PATCH_RADIO_LIST_OPENED))));
            CHECK(A::nativeCount(a) == 1u);       // native always re-reads
            CHECK(!A::patchScanWanted(a));        // NOT re-armed: already opened once
        });
    }

    covering(FOXAPP_OP_PATCH_LOOK_FOR_RADIOS);
    {
        // "Look for radios" (engine/stage3b-pre fields-to-commands round 2):
        // unconditional, unlike PATCH_RADIO_LIST_OPENED's one-time wish -
        // true whether it was already true or false.
        A::setPatchListsWanted(a, false);
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_PATCH_LOOK_FOR_RADIOS))));
        CHECK(A::patchListsWanted(a));
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_PATCH_LOOK_FOR_RADIOS))));
        CHECK(A::patchListsWanted(a));  // still true - a harmless repeat
    }

    covering(FOXAPP_OP_SET_NETWORK_USRP_SCAN);
    {
        CHECK(A::waitSoapy(a));  // (free-running: a scan the patch page's wish above started has been collected)
        A::clearSoapy(a);
        A::markSoapyScanned(a);
        A::exclusive(a, [&] {  // (the pass's workers collect a finished scan)
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 1))));
            CHECK(A::networkUsrps(a));
            CHECK(A::soapyPending(a));  // ticking it rescans at once
        });
        CHECK(A::waitSoapy(a));
        A::exclusive(a, [&] {
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 0))));
            CHECK(!A::networkUsrps(a));
            CHECK(!A::soapyPending(a));  // unticking does not
        });
    }

    covering(FOXAPP_OP_SET_PLUTO_URI);
    {
        // The Pluto address box, committed as typed (round 3 fix): an
        // address typed and never opened is still what config.json keeps.
        const std::string before = A::plutoUri(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_PLUTO_URI, "ip:pluto.local"))));
        CHECK(A::plutoUri(a) == "ip:pluto.local");
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_PLUTO_URI, before))));
        CHECK(A::plutoUri(a) == before);
    }

    covering(FOXAPP_OP_SET_RTLTCP_ADDR);
    {
        // The rtl_tcp address box (0.99.70), committed as typed: the Pluto box's twin.
        const std::string before = A::rtlTcpAddr(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_RTLTCP_ADDR, "shack.local:1235"))));
        CHECK(A::rtlTcpAddr(a) == "shack.local:1235");
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_RTLTCP_ADDR, before))));
        CHECK(A::rtlTcpAddr(a) == before);
    }

    covering(FOXAPP_OP_SET_AUDIO_BUFFER);
    {
        // The Sinks rail's audio buffer (0.99.73): 0 is AUTOMATIC, the four fixed depths are kept, anything else is refused and changes nothing.
        const int before = A::audioBufferMs(a);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_AUDIO_BUFFER, 480))));
        CHECK(A::audioBufferMs(a) == 480);
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SET_AUDIO_BUFFER, 333)), FOXAPI_OUT_OF_RANGE));
        CHECK(A::audioBufferMs(a) == 480);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_AUDIO_BUFFER, before))));
        CHECK(A::audioBufferMs(a) == before);
    }

    covering(FOXAPP_OP_SET_PPM);
    {
        // The crystal correction (0.99.56): the switch is global, the value is kept for the open radio (here the signal generator,
        // which has no crystal, so nothing is sent anywhere); an out-of-range value is clamped, 0 is no entry.
        const bool onBefore = A::ppmOn(a);
        const std::size_t countBefore = A::ppmValueCount(a);
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_SET_PPM, 1.5, 1))));
        CHECK(A::ppmOn(a));
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_SET_PPM, 0.0, 0))));
        CHECK(!A::ppmOn(a));
        CHECK(A::ppmValueCount(a) == countBefore);  // 0 is kept as no entry
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_SET_PPM, 0.0, onBefore ? 1 : 0))));
    }

    covering(FOXAPP_OP_BANDWIDTH_DRAG_END);
    {
        // The drag on the spectrum's band edge has settled (0.99.61): one log line for the width it ended on, only if the drag moved it.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BANDWIDTH_DRAG_END, 0))));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BANDWIDTH_DRAG_END, 0))));   // nothing moved since: nothing said, nothing refused
    }

    covering(FOXAPP_OP_TEST_HOLD_AUDIO_LOCK);
    {
        // The bounded-run seam (0.99.68): the window sends FOXSDR_HOLD_AUDIO_LOCK's milliseconds; 0 is "no seam" and is what is set here,
        // so nothing is held in this run (tests/test_stop_ends_recordings.cpp drives the real hold through the real application).
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_TEST_HOLD_AUDIO_LOCK, 0))));
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
        // The rtl_tcp panel's Open: the typed address reaches the driver, and is the address kept.
        CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "open-rtltcp:rtltcp=192.0.2.9:1234"))));
        CHECK(A::waitOpen(a));
        CHECK(A::kind(a) == "rtltcp");
        {
            std::lock_guard<std::mutex> lk(FakeRadio::argsMutex());
            CHECK(FakeRadio::lastArgs() == "rtltcp=192.0.2.9:1234");
        }
        CHECK(A::rtlTcpAddr(a) == "192.0.2.9:1234");
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
        covering(FOXAPP_OP_SOUND_CARD_FORM);
        {
            // The whole panel form in one command (engine/stage3b-pre
            // fields-to-commands round 2), replacing direct writes to
            // soundCard_ from every widget in drawSoundCardControls.
            FoxCommand c = cmd::make(FOXAPP_OP_SOUND_CARD_FORM);
            const std::string encoded = std::string("Other Card") + '\x1F' + "Other API";
            std::memcpy(c.text, encoded.c_str(), encoded.size() + 1);
            c.ival[0] = 1 | 2 | 4 | 8;  // IqStereo, channel right, swapIq, pickedFromList
            c.num[0] = 96000.0;
            c.num[1] = 5.0e6;
            CHECK(ok(A::apply(a, c)));
            const cascade::source::SoundCardSettings form = A::soundCardForm(a);
            CHECK(form.device == "Other Card");
            CHECK(form.hostApi == "Other API");
            CHECK(form.format == cascade::source::SoundCardFormat::IqStereo);
            CHECK(form.channel == 1);
            CHECK(form.swapIq);
            CHECK(form.pickedFromList);
            CHECK(form.cardRateHz == 96000.0);
            CHECK(form.iqCentreHz == 5.0e6);
            // The mask's OFF bits, and a device with no hostApi half.
            c.ival[0] = 0;
            const std::string bare = "Bare Card";
            std::memset(c.text, 0, sizeof(c.text));
            std::memcpy(c.text, bare.c_str(), bare.size() + 1);
            CHECK(ok(A::apply(a, c)));
            const cascade::source::SoundCardSettings form2 = A::soundCardForm(a);
            CHECK(form2.device == "Bare Card");
            CHECK(form2.hostApi.empty());
            CHECK(form2.format == cascade::source::SoundCardFormat::RealMono);
            CHECK(form2.channel == 0);
            CHECK(!form2.swapIq);
            CHECK(!form2.pickedFromList);

            // THE PANEL'S OWN ENCODE, ROUND-TRIPPED (round 3 fix): every
            // form drawSoundCardControls can send, built by the same
            // soundCardFormCommand its widgets use and applied through the
            // Engine, comes back field for field - each bit alone, so a bit
            // dropped or crossed on either side fails here.
            for (int bits = 0; bits < 16; ++bits) {
                cascade::source::SoundCardSettings want;
                want.device = "Card " + std::to_string(bits);
                want.hostApi = (bits % 3 == 0) ? std::string() : "API " + std::to_string(bits);
                want.format = (bits & 1) ? cascade::source::SoundCardFormat::IqStereo
                                         : cascade::source::SoundCardFormat::RealMono;
                want.channel = (bits & 2) ? 1 : 0;
                want.swapIq = (bits & 4) != 0;
                want.pickedFromList = (bits & 8) != 0;
                want.cardRateHz = 44100.0 + bits;
                want.iqCentreHz = 1.0e6 * bits;
                CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(want))));
                const cascade::source::SoundCardSettings got = A::soundCardForm(a);
                CHECK(got.device == want.device);
                CHECK(got.hostApi == want.hostApi);
                CHECK(got.format == want.format);
                CHECK(got.channel == want.channel);
                CHECK(got.swapIq == want.swapIq);
                CHECK(got.pickedFromList == want.pickedFromList);
                CHECK(got.cardRateHz == want.cardRateHz);
                CHECK(got.iqCentreHz == want.iqCentreHz);
            }

            // THE FIELD MASK (API 0.3 8.10): a command that carries only the rate leaves every other field as the
            // engine holds it, even though the rest of the command (the text, the bits, the centre) says otherwise.
            {
                cascade::source::SoundCardSettings base;
                base.device = "Held Card";
                base.hostApi = "Held API";
                base.format = cascade::source::SoundCardFormat::IqStereo;
                base.channel = 1;
                base.swapIq = true;
                base.pickedFromList = true;
                base.cardRateHz = 48000.0;
                base.iqCentreHz = 3.0e6;
                CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(base))));
                cascade::source::SoundCardSettings stale;  // everything else wrong
                stale.cardRateHz = 192000.0;
                const cascade::core::cmd::QueuedCommand onlyRate =
                    cascade::gui::soundCardFormCommand(stale, FOXAPP_SOUND_CARD_FIELD_RATE);
                CHECK(ok(A::apply(a, onlyRate)));
                const cascade::source::SoundCardSettings kept = A::soundCardForm(a);
                CHECK(kept.cardRateHz == 192000.0);
                CHECK(kept.device == "Held Card" && kept.hostApi == "Held API");
                CHECK(kept.format == cascade::source::SoundCardFormat::IqStereo && kept.channel == 1 && kept.swapIq &&
                      kept.pickedFromList && kept.iqCentreHz == 3.0e6);
                // Every bit of the mask on its own touches its field and no other.
                const std::int64_t fields[] = {FOXAPP_SOUND_CARD_FIELD_DEVICE,  FOXAPP_SOUND_CARD_FIELD_FORMAT,
                                               FOXAPP_SOUND_CARD_FIELD_CHANNEL, FOXAPP_SOUND_CARD_FIELD_SWAP_IQ,
                                               FOXAPP_SOUND_CARD_FIELD_PICKED,  FOXAPP_SOUND_CARD_FIELD_RATE,
                                               FOXAPP_SOUND_CARD_FIELD_CENTRE};
                for (const std::int64_t field : fields) {
                    CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(base))));
                    cascade::source::SoundCardSettings other;  // every field different from `base`
                    other.device = "Other";
                    other.hostApi = "Api";
                    other.format = cascade::source::SoundCardFormat::RealMono;
                    other.channel = 0;
                    other.swapIq = false;
                    other.pickedFromList = false;
                    other.cardRateHz = 96000.0;
                    other.iqCentreHz = 9.0e6;
                    CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(other, field))));
                    const cascade::source::SoundCardSettings got = A::soundCardForm(a);
                    CHECK((got.device == "Other") == ((field & FOXAPP_SOUND_CARD_FIELD_DEVICE) != 0));
                    CHECK((got.hostApi == "Api") == ((field & FOXAPP_SOUND_CARD_FIELD_DEVICE) != 0));
                    CHECK((got.format == cascade::source::SoundCardFormat::RealMono) ==
                          ((field & FOXAPP_SOUND_CARD_FIELD_FORMAT) != 0));
                    CHECK((got.channel == 0) == ((field & FOXAPP_SOUND_CARD_FIELD_CHANNEL) != 0));
                    CHECK((!got.swapIq) == ((field & FOXAPP_SOUND_CARD_FIELD_SWAP_IQ) != 0));
                    CHECK((!got.pickedFromList) == ((field & FOXAPP_SOUND_CARD_FIELD_PICKED) != 0));
                    CHECK((got.cardRateHz == 96000.0) == ((field & FOXAPP_SOUND_CARD_FIELD_RATE) != 0));
                    CHECK((got.iqCentreHz == 9.0e6) == ((field & FOXAPP_SOUND_CARD_FIELD_CENTRE) != 0));
                }
                // A mask with a bit that names no field, or a negative one, is refused and changes nothing.
                CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(base))));
                CHECK(refused(A::apply(a, cascade::gui::soundCardFormCommand(stale, 128)), FOXAPI_OUT_OF_RANGE));
                CHECK(refused(A::apply(a, cascade::gui::soundCardFormCommand(stale, -1)), FOXAPI_OUT_OF_RANGE));
                CHECK(A::soundCardForm(a).cardRateHz == 48000.0);
            }

            covering(FOXAPP_OP_SOUND_CARD_PICK);
            {
                // The list the engine took when the row was chosen: one card, "Fake Card (Fake API)", 48 kHz.
                // A form on another card at a rate the new card does not offer comes back on the card's own.
                cascade::source::SoundCardSettings before;
                before.device = "Somewhere Else";
                before.hostApi = "Other API";
                before.cardRateHz = 44100.0;
                CHECK(ok(A::apply(a, cascade::gui::soundCardFormCommand(before))));
                FoxCommand pick = cmd::make(FOXAPP_OP_SOUND_CARD_PICK);
                pick.ival[0] = 1;
                CHECK(ok(A::apply(a, pick)));
                const cascade::source::SoundCardSettings p = A::soundCardForm(a);
                CHECK(p.device == "Fake Card" && p.hostApi == "Fake API" && p.pickedFromList);
                CHECK(p.cardRateHz == 48000.0);
                pick.ival[0] = 0;
                CHECK(refused(A::apply(a, pick), FOXAPI_NOT_FOUND));
                pick.ival[0] = 2;
                CHECK(refused(A::apply(a, pick), FOXAPI_NOT_FOUND));
            }
        }

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
        // A stage the radio does not list is NOT_FOUND and never reaches the
        // driver (engine stage 4, API.md 8: "stage must be listed"), so the
        // driver's own refusal is not asked for and the line stays clear.
        A::exclusive(a, [&] {
            A::clearSourceError(a);
            CHECK(refused(A::apply(a, text(FOXAPI_OP_SET_GAIN, "NOPE", 0, 0, 3.0)), FOXAPI_NOT_FOUND));
            CHECK(A::sourceError(a).empty());
        });
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
        // (One block: the submits, the change of radio and its open with the pass out of the way. A free-running pass would drain
        // the gain BEFORE the radio changed - onto the old radio - and the check below would prove nothing.)
        A::exclusive(a, [&] {
            A::submit(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 17.0));
            A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -44.0));
            // (Another radio: selecting the one already open reopens nothing.)
            CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "soapy:driver=fake,serial=9"))));
            CHECK(A::waitOpen(a));
        });
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

    covering(FOXAPP_OP_BOOKMARK_EXPORT);
    {
        // The ids named, in any order, are written whole to a file under the record folder (the
        // engine picks the name), and the note says where. Nothing named, or nothing found, is
        // refused and writes nothing.
        const std::uint64_t idA = A::addBookmark(a, "Export A", 140.1e6, "NFM", 12500.0, "Out");
        const std::uint64_t idB = A::addBookmark(a, "Export B", 140.2e6, "ZZ-FUTURE", 777.0, "Out");
        (void)A::addBookmark(a, "Export C", 140.3e6, "AM", 9000.0, "Out");
        const std::filesystem::path dir(A::recordDir(a));
        const auto count = [&] {
            std::size_t n = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                n += e.path().filename().string().rfind("foxsdr-frequencies-", 0) == 0 ? 1u : 0u;
            }
            return n;
        };
        const std::size_t files0 = count();
        CHECK(ok(A::apply(a, text(FOXAPP_OP_BOOKMARK_EXPORT, std::to_string(idB) + " " + std::to_string(idA)))));
        CHECK(count() == files0 + 1u);
        CHECK(A::importNote(a).rfind("Exported 2 to ", 0) == 0);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_EXPORT, "")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_EXPORT, "424242424")), FOXAPI_NOT_FOUND));
        CHECK(count() == files0 + 1u);
    }
}

// THE WEB REMOTE'S BOOKMARK ROWS, END TO END. A browser names a bookmark by
// its ROW in the last snapshot; the translation (net::controlRequestToCommands, over the ids of the
// Full block) turns the row into the id that snapshot published. The case that matters: a desktop bookmark command
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

    // THE SAME FRAME is one block (the pass out of the way from the submit to the requests): a free-running pass between them would
    // drain the x and PUBLISH a new row map before the browser's rows arrive, which is a later frame, not this case.
    A::exclusive(a, [&] {
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
    });

    // Frame N+2: the browser reads a fresh snapshot; a desktop ADD (the
    // bookmark lands at the tuned frequency, below B in the list) drains
    // first, then the browser removes the row it saw B on: B goes, and
    // nothing else does.
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_FREQUENCY, 29.0e6))));
    A::publishWeb(a);
    const int rowB2 = A::webRowOf(a, "Web B");
    CHECK(rowB2 >= 0);
    A::exclusive(a, [&] {  // (the same frame, as above)
        A::submit(a, cmd::makeText(FOXAPI_OP_BOOKMARK_ADD, "Web D").c);
        A::drain(a);
        const std::size_t before = A::bookmarks(a).size();
        cascade::net::ControlRequest rm;
        rm.bookmarkRemove = rowB2;
        A::webRequest(a, rm);
        CHECK(A::bookmarks(a).size() == before - 1u);
    });
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
        A::exclusive(a, [&] {  // (the stop seen is the command's, not one the pass's own scanner step made)
            CHECK(ok(A::apply(a, ints(FOXAPI_OP_SCANNER_RUN, 0))));
            CHECK(!A::scanning(a));
        });
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
        // engine/stage3b-pre fields-to-commands round 2: this is now also
        // what drawScannerSection's timing InputDoubles commit through (a
        // from-scratch FOXAPP_OP_SCANNER_TIMING briefly duplicated this op
        // byte for byte before that was noticed) - hold/resume added to the
        // check alongside the pre-existing dwell/listen for that reason.
        FoxCommand c = cmd::make(FOXAPI_OP_SCANNER_CONFIG);
        c.num[0] = 150.0;
        c.num[1] = 900.0;
        c.num[2] = 1500.0;
        c.num[3] = 4000.0;
        CHECK(ok(A::apply(a, c)));
        CHECK(A::scanDwell(a) == 150.0);
        CHECK(A::scanHold(a) == 900.0);
        CHECK(A::scanResume(a) == 1500.0);
        CHECK(A::scanListen(a) == 4000.0);
    }
}

// --- the Airband monitor and the frequency list's ticks (stub campaign B6, docs/api-0.3-additions.md 9.5) ------------------

// The row named `name` (a zeroed one with id 0 when there is none).
cascade::core::Bookmark rowNamed(AppWindow& a, const std::string& name) {
    for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
        if (b.name == name) { return b; }
    }
    return cascade::core::Bookmark{};
}

void airbandOps(AppWindow& a) {
    const bool pageWas = A::patchPageOpen(a);

    covering(FOXAPP_OP_BOOKMARK_TICK);
    {
        const std::uint64_t id = A::addBookmark(a, "Tick me", 119.7e6, "AM", 10000.0);
        CHECK(!rowNamed(a, "Tick me").scan);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(id), 1))));
        CHECK(rowNamed(a, "Tick me").scan);
        CHECK(rowNamed(a, "Tick me").id == id);   // an edited row keeps its identity
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(id), 1))));   // already so: nothing changes
        CHECK(rowNamed(a, "Tick me").scan);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(id), 0))));
        CHECK(!rowNamed(a, "Tick me").scan);
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, 0, 1)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, 999999, 1)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPP_OP_BOOKMARK_ADD_ROWS);
    {
        // A row TYPED IN (ival[0] 0): ticked, in its group, named by its frequency when nameless.
        const std::size_t before = A::bookmarks(a).size();
        FoxCommandResult r = A::apply(a, text(FOXAPP_OP_BOOKMARK_ADD_ROWS, "121500000\tNFM\t12500\t1\tManual\t", 0));
        CHECK(ok(r));
        CHECK(r.applied[0] == 1.0);
        CHECK(A::bookmarks(a).size() == before + 1u);
        const cascade::core::Bookmark typed = rowNamed(a, "121.5000 MHz");
        CHECK(typed.id != 0u);
        CHECK(typed.scan);
        CHECK(typed.group == "Manual");
        CHECK(typed.mode == "NFM");
        CHECK(typed.bandwidthHz == 12500.0);
        // A LIST (ival[0] 1): added as an import adds - one of the two lines is the row above, again.
        r = A::apply(a, text(FOXAPP_OP_BOOKMARK_ADD_ROWS,
                             "121500000\tNFM\t12500\t1\tManual\t121.5000 MHz\n118100000\tAM\t10000\t0\tKABC\tKABC TWR", 1));
        CHECK(ok(r));
        CHECK(r.applied[0] == 1.0);
        CHECK(!rowNamed(a, "KABC TWR").scan);   // the tick the line gave it
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_ADD_ROWS, "no row here", 0)), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_ADD_ROWS, "", 1)), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BOOKMARK_ADD_ROWS, "121500000\tNFM\t12500\t1\tManual\t", 5)), FOXAPI_OUT_OF_RANGE));
        CHECK(A::bookmarks(a).size() == before + 2u);
    }

    covering(FOXAPP_OP_SCANNER_LIST);
    {
        const std::uint64_t one = A::addBookmark(a, "List one", 99.5e6, "WFM", 150000.0);
        const std::uint64_t two = A::addBookmark(a, "List two", 156.8e6, "NFM", 12500.0);
        for (const std::uint64_t id : {one, two}) { CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(id), 1)))); }
        CHECK(!A::scanTicked(a));
        // LIST MODE on or off stops nothing that is not running; a start scans the ticked rows.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 1))));
        CHECK(A::scanTicked(a));
        CHECK(!A::scanning(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 2))));
        CHECK(A::scanning(a));
        // ...and either switch stops a scan that runs.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 0))));
        CHECK(!A::scanTicked(a));
        CHECK(!A::scanning(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 2))));   // a start is list mode whatever it was
        CHECK(A::scanTicked(a));
        CHECK(A::scanning(a));
        // A range scan leaves list mode.
        FoxCommand c = ints(FOXAPI_OP_SCANNER_RUN, 1);
        c.num[0] = 144.0e6;
        c.num[1] = 146.0e6;
        c.num[2] = 12.5e3;
        CHECK(ok(A::apply(a, c)));
        CHECK(!A::scanTicked(a));
        A::exclusive(a, [&] { CHECK(ok(A::apply(a, ints(FOXAPI_OP_SCANNER_RUN, 0)))); });
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 7)), FOXAPI_OUT_OF_RANGE));
        // A start with nothing ticked.
        for (const cascade::core::Bookmark& b : A::bookmarks(a)) {
            if (b.scan) { CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(b.id), 0)))); }
        }
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 2)), FOXAPI_NO_CHANGE));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SCANNER_LIST, 0))));
    }

    covering(FOXAPP_OP_AIRBAND);
    {
        // Nothing ticked: LISTEN is refused with the note, and nothing else has anything to do.
        CHECK(refused(A::apply(a, text(FOXAPP_OP_AIRBAND, "No such preset", 1)), FOXAPI_NO_CHANGE));
        CHECK(A::airbandNote(a) == "Tick at least one AM or NFM frequency to listen to.");
        CHECK(!A::airbandListening(a));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND, 2)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND, 3)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND, 9)), FOXAPI_OUT_OF_RANGE));
        // SQUELCH and HOLD are the monitor's settings, kept and clamped, listening or not.
        FoxCommandResult r = A::apply(a, numInt(FOXAPP_OP_AIRBAND, -70.0, 4));
        CHECK(ok(r));
        CHECK(r.applied[1] == -70.0);
        CHECK(A::airbandSquelch(a) == -70.0f);
        r = A::apply(a, numInt(FOXAPP_OP_AIRBAND, -300.0, 4));
        CHECK(ok(r));
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        CHECK(A::airbandSquelch(a) == -110.0f);
        r = A::apply(a, numInt(FOXAPP_OP_AIRBAND, 90.0, 5));
        CHECK(ok(r));
        CHECK((r.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        CHECK(A::airbandHold(a) == 60.0);
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_AIRBAND, -55.0, 4))));
        CHECK(ok(A::apply(a, numInt(FOXAPP_OP_AIRBAND, 2.0, 5))));
        // TUNE ONE: a row that is not there is refused; one that is is tuned to.
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND, 6, 0)), FOXAPI_NOT_FOUND));
        const std::uint64_t air = A::addBookmark(a, "Airband one", 124.2e6, "AM", 10000.0);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(air), 1))));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_AIRBAND, 6, static_cast<std::int64_t>(air)))));
        CHECK(A::tuned(a) == 124.2e6);
        // LISTEN with the patch view showing waits for it to let go (the engine closes the view); with it closed, it starts at once.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 1))));
        r = A::apply(a, text(FOXAPP_OP_AIRBAND, "", 1));
        CHECK(ok(r));
        CHECK(r.applied[0] == 2.0);
        CHECK(A::airbandPending(a));
        CHECK(!A::patchPageOpen(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_AIRBAND, 0))));   // STOP cancels the wait
        CHECK(!A::airbandPending(a));
        CHECK(!A::airbandListening(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0))));
        // (the pass's own step would hand the radio back; the engine's hand-back is held in tests/test_engine_api_airband.cpp)
        r = A::apply(a, text(FOXAPP_OP_AIRBAND, "", 1));
        CHECK(ok(r));
        CHECK(r.applied[0] == 1.0 || r.applied[0] == 2.0);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_AIRBAND, 0))));
        CHECK(!A::airbandListening(a));
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 0))));
    }
    CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, pageWas ? 1 : 0))));
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

    covering(FOXAPP_OP_PLUGIN_RUN_MODE);
    {
        // HOW ONE PLUGIN RUNS (0.99.73, stub campaign B2): STOPPED, ALWAYS (pinned) and AUTO, by the module's file name; the engine holds
        // the state by plugin id. (The instances' side of it - what wakes, what sleeps, when - is tests/test_engine_api_03.cpp group N.)
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, 2))));
        CHECK(A::stopped(a, key));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, 1))));
        CHECK(!A::stopped(a, key));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, 0))));
        CHECK(!A::stopped(a, key));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, 3)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, -1)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, "nobody.dll", 1)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, std::string(), 1)), FOXAPI_BAD_ARGUMENT));
        // (left PINNED, as the START above left it)
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_RUN_MODE, key, 1))));
    }

    covering(FOXAPP_OP_PLUGIN_USE);
    {
        // The window's table of what it shows: one "<file>\t<bits>" line a plugin; an empty one clears it; a line that does not parse
        // is skipped, never an error.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_USE, key + "\t8\n"))));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_USE, "no tab here\n" + key + "\tnotanumber\n"))));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PLUGIN_USE, std::string()))));
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
        A::exclusive(a, [&] {  // (the pass's workers collect a finished fetch)
            CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_STORE_FETCH))));
            CHECK(A::storeBusy(a));  // the fetch is in flight
        });
        CHECK(A::waitStore(a));
    }

    covering(FOXAPI_OP_STORE_CANCEL);
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_STORE_CANCEL))));

    covering(FOXAPI_OP_STORE_INSTALL);
    {
        A::injectCatalogue(a);
        // (One block: a download the command started by mistake would fail at once on the pass's workers - setting the error and
        // ending the busy flag - and answer for it.)
        A::exclusive(a, [&] {
            A::clearInstallError(a);
            CHECK(refused(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "stage1-catalogue-only", 0)), FOXAPI_DENIED));
            CHECK(A::installError(a).find("legal notice") != std::string::npos);
            CHECK(!A::storeBusy(a));
        });
        CHECK(refused(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "no-such-id", 1)), FOXAPI_NOT_FOUND));
        CHECK(A::installError(a).find("no catalogue entry") != std::string::npos);
        A::exclusive(a, [&] {
            CHECK(ok(A::apply(a, text(FOXAPI_OP_STORE_INSTALL, "stage1-catalogue-only", 1))));
            CHECK(A::storeBusy(a));  // the download is in flight (and cannot land)
        });
        CHECK(A::waitStore(a));
    }

    covering(FOXAPI_OP_STORE_UPDATE_ALL);
    {
        A::injectCatalogue(a);
        A::clearInstallError(a);
        A::exclusive(a, [&] {  // (a pass would step the run: a download in flight that the next case's key then waits for)
            CHECK(ok(A::apply(a, ints(FOXAPI_OP_STORE_UPDATE_ALL, 1))));
            CHECK(A::addAllActive(a));
            A::stopAddAll(a);
        });
    }

    // FOXAPP_OP_STORE_ADD_ALL (OPEN 5): the store window's key - the engine
    // plans it; the command says which one module's own notice tick is on.
    // The catalogue's only module carries a notice: without its tick the run
    // is refused (nothing to add), with it the run starts.
    covering(FOXAPP_OP_STORE_ADD_ALL);
    {
        A::injectCatalogue(a);
        A::exclusive(a, [&] {  // (a run started by mistake would end, with an error, on the pass's workers: the same reason)
            A::clearInstallError(a);
            CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_ADD_ALL, "", 0))));
            CHECK(!A::addAllActive(a));
            CHECK(!A::installError(a).empty());
        });
        A::clearInstallError(a);
        A::exclusive(a, [&] {
            CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_ADD_ALL, "stage1-catalogue-only", 0))));
            CHECK(A::addAllActive(a));
            A::stopAddAll(a);
        });
    }

    covering(FOXAPP_OP_STORE_UPDATE);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_UPDATE, "stage1-catalogue-only")), FOXAPI_NOT_FOUND));

    // THE STORE'S SITES, PICTURES AND CLEAN-UP (0.99.49 - 0.99.74, stub campaign B3). The catalogue is pointed at a file that is not there and
    // the sites are local paths (no scheme: the seam), so every read fails at once and nothing here touches a network.
    covering(FOXAPP_OP_STORE_SITE);
    {
        const std::string savedUrl = A::catalogueUrl(a);
        const std::string site = (g_plugins.parent_path() / "b3-site-index.json").string();
        A::setCatalogueUrl(a, (g_plugins.parent_path() / "b3-no-such-catalogue.json").string());
        CHECK(A::waitStore(a));
        // ADD only with the consent the window collects: without it the address is not even looked at, and nothing is listed.
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_SITE, site, 0, 0)), FOXAPI_DENIED));
        CHECK(A::storeSites(a).empty());
        CHECK(A::siteError(a).find("not ticked") != std::string::npos);
        // An address that fails the ONE validator is refused with its reason, and nothing is listed.
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_SITE, "http://plugins.example.test/index.json", 0, 1)), FOXAPI_BAD_ARGUMENT));
        CHECK(A::storeSites(a).empty());
        CHECK(!A::siteError(a).empty());
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_SITE, site, 2, 1)), FOXAPI_OUT_OF_RANGE));
        // A good one is listed, and a read starts (it fails: there is no such file - the site is listed all the same, "failed").
        CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_SITE, "  " + site + "  ", 0, 1))));
        CHECK(A::storeSites(a) == std::vector<std::string>{site});
        CHECK(A::siteError(a).empty());
        CHECK(A::waitStore(a));
        // ...and once more is refused, whatever the consent.
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_SITE, site, 0, 1)), FOXAPI_BAD_ARGUMENT));
        CHECK(A::siteError(a).find("already in the list") != std::string::npos);
        // REMOVE: one that is not listed is NOT_FOUND; the listed one goes (its kept copy is deleted on a worker).
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_SITE, "https://nobody.example.test/index.json", 1)), FOXAPI_NOT_FOUND));
        CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_SITE, site, 1))));
        CHECK(A::storeSites(a).empty());
        CHECK(A::waitStore(a));
        A::setCatalogueUrl(a, savedUrl);
    }

    covering(FOXAPP_OP_STORE_PICTURES_WANT);
    {
        A::injectCatalogue(a);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_PICTURES_WANT, "no-such-key", 0)), FOXAPI_NOT_FOUND));
        // A key the catalogue holds (this entry lists no pictures: nothing is queued, and that is not a refusal).
        CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_PICTURES_WANT, "stage1-catalogue-only", 1))));
        // The bare id of a plugin site's entry names nothing: the key is "<id>@<site>".
        CHECK(refused(A::apply(a, text(FOXAPP_OP_STORE_PICTURES_WANT, "stage1-catalogue-only@elsewhere.example", 0)), FOXAPI_NOT_FOUND));
    }

    covering(FOXAPP_OP_STORE_CLEAN_OLD);
    {
        // CLEAN UP with no old copy found is nothing to do; REMOVE FILE on a name this engine's scan holds no record of removes nothing and
        // SAYS why (the last scan did not find it); any other mode is out of range.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_STORE_CLEAN_OLD, 0))));
        CHECK(A::waitStore(a));
        A::clearInstallError(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_STORE_CLEAN_OLD, "no-such-module.dll", 1))));
        CHECK(A::installError(a).find("last scan") != std::string::npos);
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_STORE_CLEAN_OLD, 2)), FOXAPI_OUT_OF_RANGE));
    }

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
        CHECK(!A::haveFixture(a));
        CHECK(!std::filesystem::exists(g_plugins / key));
    }

    covering(FOXAPP_OP_SET_CATALOGUE_URL);
    {
        // Committed on deactivate-after-edit (the window's pluginUrlBuf_) -
        // engine/stage3b-pre fields-to-commands round 2, replacing the direct
        // write.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_CATALOGUE_URL, "https://example.test/index.json"))));
        CHECK(A::catalogueUrl(a) == "https://example.test/index.json");
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SET_CATALOGUE_URL))));  // empty clears it too
        CHECK(A::catalogueUrl(a).empty());
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

    // The picture a patch decoder made goes with its node (Engine::syncPatchImages, the engine's pass: the window used
    // to be asked for it, and before that to prune its own cache): a gone node's picture goes, a live node's stays. (The
    // sync is run once first: it settles the graph epoch, which a document load moves and which clears every picture.)
    {
        A::syncPatchImages(a);
        const cascade::core::patch::NodeId keep = A::addPatchNode(a);
        const cascade::core::patch::NodeId gone = A::addPatchNode(a);
        A::putPatchImage(a, keep);
        A::putPatchImage(a, gone);
        A::removePatchNode(a, gone);
        A::syncPatchImages(a);
        CHECK(A::hasPatchImage(a, keep));
        CHECK(!A::hasPatchImage(a, gone));
        A::removePatchNode(a, keep);
        A::syncPatchImages(a);
        CHECK(!A::hasPatchImage(a, keep));
    }

    // FOXAPP_OP_PATCH_SET_GRAPH (OPEN 6, Design A): the whole graph, as the
    // Patch page's draft sends it - ids, the next id and a session-only
    // chosen centre kept, not remapped as a loaded document's are - and
    // refused WHOLE, the engine's graph untouched, when any of it cannot be
    // honoured.
    covering(FOXAPP_OP_PATCH_SET_GRAPH);
    {
        namespace pc = cascade::core::patch;
        pc::Graph g;
        const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 60.0f, 80.0f);
        const pc::NodeId gap = g.addNode(pc::NodeKind::Demod, "gone", pc::PortType::Iq, 0.0f, 0.0f);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "2 m", pc::PortType::Iq, 333.25f, -41.5f);
        (void)g.removeNode(gap);  // ids 1 and 3: a gap parse's own numbering would close
        if (pc::Node* n = g.mutableNode(radio)) {
            n->device = pc::kGeneratorKey;
            n->freqHz = 0.0;
            n->centreChosen = true;  // 0 Hz on the air, chosen - never saved in a document
            n->rateHz = 2.4e6;
        }
        if (pc::Node* n = g.mutableNode(chan)) {
            n->freqHz = 144800000.25;
            n->name = "2 m calling, " + std::string(240, '.');  // past FoxCommand::text
        }
        CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
        const std::string sent = pc::graphCommandText(g);
        CHECK(sent.size() > FOXAPI_TEXT_CHARS - 1);  // it travels as long text
        const std::uint64_t epoch = A::patchGraphEpoch(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, sent))));
        const pc::Graph& got = A::patchGraph(a);
        CHECK(pc::graphCommandText(got) == sent);
        CHECK(got.find(chan) != nullptr && got.find(chan)->freqHz == 144800000.25);
        CHECK(got.find(chan) != nullptr && got.find(chan)->x == 333.25f && got.find(chan)->y == -41.5f);
        CHECK(got.find(radio) != nullptr && got.find(radio)->centreChosen);
        CHECK(got.find(gap) == nullptr);
        CHECK(got.nextId() == 4u);  // the gap's id is not handed out again
        CHECK(got.wires().size() == 1u);
        CHECK(A::patchGraphEpoch(a) == epoch);  // an edit, not a new document

        // A new document counts, so a window mid-edit of the old one knows.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, sent, FOXAPP_PATCH_GRAPH_DOCUMENT))));
        CHECK(A::patchGraphEpoch(a) == epoch + 1u);

        // Refused, each of them, with a sentence - and the graph as it was.
        const char* const bad[] = {
            "",                                                         // nothing
            "not a patch\n",                                           // no header
            "foxsdr-patch 6\nnode 1 0 0 x y\n",                       // a node that cannot be read
            "foxsdr-patch 6\nnode 1 0 0 0 0 232 128 0 0 - - 0 1 -50 1 R\n"
            "node 1 1 0 0 0 232 104 0 0 - - 0 1 -50 1 C\n",            // the same id twice
            "foxsdr-patch 6\nnode 1 5 1 0 0 216 116 0 0 - - 0 1 -50 1 S\n"
            "node 2 0 0 0 0 232 128 0 0 - - 0 1 -50 1 R\nwire 2 0 1 0\n",  // Iq into an Audio port
            "foxsdr-patch 6\nnode 1 0 0 0 0 232 128 0 0 - - 0 1 -50 1 R\ncentre-chosen 9\n",
            "foxsdr-patch 6\nnode 1 0 0 0 0 232 128 0 0 - - 0 1 -50 1 R\nnext-id x\n",
        };
        for (const char* t : bad) {
            const FoxCommandResult r = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, t));
            if (!refused(r, FOXAPI_BAD_ARGUMENT)) { std::printf("FAIL: SET_GRAPH took [%s]\n", t); }
            CHECK(refused(r, FOXAPI_BAD_ARGUMENT));
            CHECK(std::strlen(r.message) > 0);
            CHECK(pc::graphCommandText(A::patchGraph(a)) == sent);
        }
        CHECK(A::patchGraphEpoch(a) == epoch + 1u);  // a refused document is no document

        // Values the document loader would quietly REPAIR are refused too
        // (the review of e29c1a7 found -200 dB stored as -50, 2e10 Hz as 0
        // and a width of 5000 as 4000): the engine runs the page's graph or
        // its own, never a third one.
        for (int k = 0; k < 3; ++k) {
            pc::Graph odd = g;
            if (pc::Node* n = odd.mutableNode(chan)) {
                if (k == 0) { n->squelchDb = -200.0f; }
                if (k == 1) { n->rateHz = 2.0e10; }
                if (k == 2) { n->w = 5000.0f; }
            }
            const FoxCommandResult r = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(odd)));
            if (!refused(r, FOXAPI_BAD_ARGUMENT)) { std::printf("FAIL: SET_GRAPH repaired value %d\n", k); }
            CHECK(refused(r, FOXAPI_BAD_ARGUMENT));
            CHECK(std::strlen(r.message) > 0);
            CHECK(pc::graphCommandText(A::patchGraph(a)) == sent);
        }

        // THE VERSION (API 0.3 8.12). Every accepted change to the graph is a version, an edit's answer names it, and an
        // edit that names the version it was made from is MERGED over the graph as it is now when that is not the current
        // one, with what the engine did meanwhile kept. (The wire-level cases, with sessions and the pass, are
        // tests/test_engine_api_03.cpp G13-2; these are the engine's own rule.)
        {
            const FoxCommandResult atOnce = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(g)));
            CHECK(ok(atOnce));
            const std::uint64_t v0 = static_cast<std::uint64_t>(atOnce.applied[0]);
            CHECK(v0 == A::patchGraphVersion(a));
            // An edit from the CURRENT version replaces; an edit from an older one merges.
            pc::Graph theirs = g;  // what a second sender read at v0 and changed
            if (pc::Node* n = theirs.mutableNode(chan)) { n->name = "theirs"; }
            pc::Graph mine = g;    // and what the first changed, from the same version
            if (pc::Node* n = mine.mutableNode(radio)) { n->x = 123.0f; }
            const FoxCommandResult first = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(mine), 0, static_cast<std::int64_t>(v0)));
            CHECK(ok(first));
            CHECK(first.applied[0] == static_cast<double>(v0 + 1u) && first.applied[1] == 0.0);
            CHECK((first.flags & FOXAPI_RESULT_CLAMPED) == 0u);
            const FoxCommandResult second = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(theirs), 0, static_cast<std::int64_t>(v0)));
            CHECK(ok(second));
            CHECK(second.applied[0] == static_cast<double>(v0 + 2u) && second.applied[1] == 1.0);
            CHECK((second.flags & FOXAPI_RESULT_CLAMPED) != 0u);
            CHECK(A::patchGraph(a).find(radio)->x == 123.0f);              // the first's edit survived
            CHECK(A::patchGraph(a).find(chan)->name == "theirs");          // and the second's landed
            // The engine's own change (a switch thrown, as START does) is a version of its own and is kept too: the sender
            // read version vSeen with the radio on and renamed a channel; before its command was applied the engine
            // switched the radio off.
            const std::uint64_t vSeen = A::patchGraphVersion(a);
            CHECK(A::patchGraph(a).find(radio)->on);
            pc::Graph rename = A::patchGraph(a);
            if (pc::Node* n = rename.mutableNode(chan)) { n->name = "renamed"; }
            A::changePatchGraph(a, [&](pc::Graph& graph) {
                if (pc::Node* n = graph.mutableNode(radio)) { n->on = false; }
            });
            const FoxCommandResult kept = A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(rename), 0, static_cast<std::int64_t>(vSeen)));
            CHECK(ok(kept));
            CHECK(kept.applied[1] == 1.0);
            CHECK(!A::patchGraph(a).find(radio)->on);                      // the engine's switch was not put back
            CHECK(A::patchGraph(a).find(chan)->name == "renamed");
            // A version the engine does not hold, one from the future, and a negative one.
            const std::string held = pc::graphCommandText(A::patchGraph(a));
            const std::uint64_t vNow = A::patchGraphVersion(a);
            CHECK(refused(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(mine), 0, static_cast<std::int64_t>(vNow + 40u))), FOXAPI_FAILED));
            CHECK(refused(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(mine), 0, -3)), FOXAPI_BAD_ARGUMENT));
            CHECK(pc::graphCommandText(A::patchGraph(a)) == held);
            CHECK(A::patchGraphVersion(a) == vNow);
            // A document ignores the version, and forgets the history: the next edit naming an old version is refused.
            const std::uint64_t e0 = A::patchGraphEpoch(a);
            CHECK(ok(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(mine), FOXAPP_PATCH_GRAPH_DOCUMENT, 7))));
            CHECK(A::patchGraphEpoch(a) == e0 + 1u);
            CHECK(refused(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(theirs), 0, static_cast<std::int64_t>(v0))), FOXAPI_FAILED));
        }

        // Leave the patch empty for whatever runs next.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(pc::Graph{})))));
        CHECK(A::patchGraph(a).nodes().empty());
    }

    // FOXAPP_OP_SET_PATCH_PAGE_OPEN (API 0.3 8.12): the standard window's patch view is showing. The engine starts
    // "showing" (the view is the window's default face); the flag only says whether the pass runs the patch step, and the
    // close rule - the patch stops, once - belongs to that step.
    covering(FOXAPP_OP_SET_PATCH_PAGE_OPEN);
    {
        CHECK(A::patchPageOpen(a));  // the default
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0))));
        CHECK(!A::patchPageOpen(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0))));  // said again: harmless
        CHECK(!A::patchPageOpen(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 1))));
        CHECK(A::patchPageOpen(a));
        // The close rule runs from the step, and only for a page that was open: showing, the step marks it; closed, the
        // patch stops and the mark goes; closed again, nothing more happens.
        A::pumpPatch(a);
        CHECK(A::patchWasOpen(a));
        A::exclusive(a, [&] {  // (... and a free-running pass's own step would act on it before the read)
            A::setPatchRunning(a, true);
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0))));
            CHECK(A::patchRunning(a));            // the command only says so ...
        });
        A::pumpPatch(a);
        CHECK(!A::patchRunning(a));           // ... the pass's step stops the patch
        CHECK(!A::patchWasOpen(a));
        A::setPatchRunning(a, true);
        A::pumpPatch(a);
        CHECK(A::patchRunning(a));            // the rule fired once: a page already closed stops nothing more
        A::setPatchRunning(a, false);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 1))));
    }

    // FOXAPP_OP_PATCH_LIST_RECORDINGS (API 0.3 8.12): the Radio part's device list asks for the I/Q recordings of the record
    // folder, and the engine reads them on its own pass (the window used to call the listing itself, on its thread). Nothing
    // lists them until asked; an empty folder is an empty list, not a refusal; a file is listed with the rate its header
    // says; a relisting finds what changed.
    covering(FOXAPP_OP_PATCH_LIST_RECORDINGS);
    {
        namespace fs = std::filesystem;
        const char* base = std::getenv("LOCALAPPDATA");
        const fs::path dir = fs::path(base != nullptr ? base : ".") / "apply_command_g13b_rec";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::string before = A::recordDir(a);
        A::setRecordDir(a, dir.string());
        CHECK(A::patchRecordingCount(a) == 0u);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_PATCH_LIST_RECORDINGS, 0))));
        CHECK(A::patchRecordingCount(a) == 0u);        // an empty folder: an empty list
        {
            std::ofstream f(dir / "tone_96k.wav", std::ios::binary | std::ios::trunc);
            const auto put16 = [&](std::uint16_t v) { f.put(static_cast<char>(v & 0xFF)); f.put(static_cast<char>(v >> 8)); };
            const auto put32 = [&](std::uint32_t v) { put16(static_cast<std::uint16_t>(v & 0xFFFF)); put16(static_cast<std::uint16_t>(v >> 16)); };
            const std::uint32_t frames = 9600;
            f.write("RIFF", 4);
            put32(36 + frames * 4);
            f.write("WAVE", 4);
            f.write("fmt ", 4);
            put32(16);
            put16(1);
            put16(2);
            put32(96000);
            put32(96000 * 4);
            put16(4);
            put16(16);
            f.write("data", 4);
            put32(frames * 4);
            for (std::uint32_t i = 0; i < frames; ++i) { put16(1000); put16(0); }
        }
        CHECK(A::patchRecordingCount(a) == 0u);        // nothing lists it by itself
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_PATCH_LIST_RECORDINGS, 0))));
        CHECK(A::patchRecordingCount(a) == 1u);
        CHECK(A::patchRecordingRate(a, 0) == 96000.0);
        fs::remove(dir / "tone_96k.wav", ec);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_PATCH_LIST_RECORDINGS, 0))));
        CHECK(A::patchRecordingCount(a) == 0u);        // ...and the relisting finds it gone
        A::setRecordDir(a, before);
        fs::remove_all(dir, ec);
    }

    // FOXAPP_OP_TRACK_INFO_LINE (the pump move, step P2): the track-info plugin's status line, polled by the window on its own
    // thread and SENT (the engine used to ask a host hook from its pass). It lands in the Decoder output backlog as "Aircraft
    // info", COUNTED (an uncounted line never reached the ring an interface reads), the tail stays bounded, and an empty line
    // is refused and changes nothing.
    covering(FOXAPP_OP_TRACK_INFO_LINE);
    {
        const std::uint64_t total0 = A::decoderLinesTotal(a);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_TRACK_INFO_LINE, "cannot reach the registry"))));
        CHECK(A::decoderLinesTotal(a) == total0 + 1u);
        CHECK(A::decoderLogBack(a).plugin == "Aircraft info");
        CHECK(A::decoderLogBack(a).text == "cannot reach the registry");
        CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACK_INFO_LINE, "")), FOXAPI_BAD_ARGUMENT));
        CHECK(A::decoderLinesTotal(a) == total0 + 1u);
        for (int i = 0; i < 700; ++i) { (void)A::apply(a, text(FOXAPP_OP_TRACK_INFO_LINE, "again")); }
        CHECK(A::decoderLogSize(a) <= 500u);
        CHECK(A::decoderLinesTotal(a) == total0 + 701u);
    }
}

void transmitterOps(AppWindow& a) {
    cascade::test::FakeTxIiod d;
    std::string err;
    CHECK(d.start(err));
    cascade::test::stockTxBoard(d);
    const std::string args = "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(d.port()));

    covering(FOXAPP_OP_SET_TRANSMIT_ARGS);
    {
        // The Transmit address box, committed as typed (round 3 fix): what
        // config.json keeps even when the board is never opened.
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_TRANSMIT_ARGS, "uri=ip:10.0.0.9"))));
        CHECK(A::txArgs(a) == "uri=ip:10.0.0.9");
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SET_TRANSMIT_ARGS))));  // a cleared box
        CHECK(A::txArgs(a).empty());
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_TRANSMIT_ARGS, "uri=ip:10.0.0.9"))));
    }

    covering(FOXAPI_OP_TX_OPEN);
    {
        // AN EMPTY ADDRESS OPENS NOTHING (round 3 fix): it used to keep the
        // previous transmitArgs_ and open that board - one the cleared box
        // no longer showed. Refused, and nothing changes.
        CHECK(refused(A::apply(a, cmd::make(FOXAPI_OP_TX_OPEN)), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPI_OP_TX_OPEN, "  ")), FOXAPI_BAD_ARGUMENT));
        CHECK(!A::haveTx(a));
        CHECK(A::txArgs(a) == "uri=ip:10.0.0.9");
    }
    CHECK(ok(A::apply(a, text(FOXAPI_OP_TX_OPEN, args))));
    CHECK(A::haveTx(a));
    CHECK(A::txArgs(a) == args);
    // An open board keeps the address it was opened with.
    CHECK(refused(A::apply(a, text(FOXAPP_OP_SET_TRANSMIT_ARGS, "uri=ip:10.0.0.1")), FOXAPI_FAILED));
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
    // THE KEY IS A SESSION'S, NOT A COMMAND'S (stage 5d). applyCommand REFUSES a TX_PTT: the key rules judge a PTT for a SESSION - its
    // consent, its hold, its place in the table's queues - and nothing that reaches applyCommand speaks for one. (The browser's PTT used to
    // arrive here, for a fake session of the web remote's; it is the web server's engine session's now and keys through the table:
    // tests/test_remote_control_pull.cpp, test_remote_key_release.cpp and test_transmit_dead_man.cpp hold every rule of that key.) The refusal
    // does not depend on consent: with the desk's consent given - the state in which the browser's PTT IS granted through its session -
    // and a transmitter open, the press and the release are both refused and nothing is keyed.
    tx_test::FrontEnd desk(A::engine(a), "the desk");
    auto consent = [&](int on) {
        desk.frame(true, false, false, true, on);
        A::pass(a);
    };
    {
        consent(0);
        A::setTransmitOpen(a, true);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_PTT, 1)), FOXAPI_DENIED));
        CHECK(A::remoteHoldMs(a) == 0);
        consent(1);
        A::setTransmitOpen(a, false);  // the page flag gates no key any more
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_PTT, 1)), FOXAPI_DENIED));
        CHECK(A::remoteHoldMs(a) == 0);
        CHECK(A::txOwner(a) == 0u);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TX_PTT, 0)), FOXAPI_DENIED));
        CHECK(A::remoteHoldMs(a) == 0);
    }

    // FOXAPP_OP_SOUND_CARDS_WANTED (OPEN 2): with the cards already listed it
    // takes no list (the listing itself, with a fake backend, is
    // test_soundcard_app_paths's testSoundCardsWantedIsACommand - this file
    // never lets PortAudio list this machine's inputs).
    covering(FOXAPP_OP_SOUND_CARDS_WANTED);
    A::exclusive(a, [&] {
        A::setSoundCardsListed(a, true);
        CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SOUND_CARDS_WANTED))));
        CHECK(!A::soundCardScanPending(a));
    });

    covering(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN);
    {
        // Opening and closing move transmitOpen_ exactly as the direct write
        // used to.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, 1))));
        CHECK(A::transmitOpen(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, 0))));
        CHECK(!A::transmitOpen(a));
        // Closing again (already closed) is a harmless no-op, not a refusal.
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, 0))));
        CHECK(!A::transmitOpen(a));
        // Since stage 4 the flag gates NO key: the window withdraws its
        // consent when the page closes (gui::txKeyCommands), and that is
        // what releases a remote key held (test_remote_key_release D).
    }
    consent(0);
    desk.close();

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

    covering(FOXAPP_OP_TELEMETRY_CONSENT);
    {
        // engine/stage3b-pre fields-to-commands round 2: was two direct field
        // writes plus a direct telemetryHeartbeat_.configure(...) call from
        // the Checkbox handler. ON mints an id; OFF forgets it - so a later
        // opt-in cannot be tied to the old one.
        CHECK(A::telemetryInstallId(a).empty());
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_TELEMETRY_CONSENT, 1))));
        CHECK(A::telemetryEnabled(a));
        CHECK(!A::telemetryInstallId(a).empty());
        const std::string firstId = A::telemetryInstallId(a);
        // Round 3 fix: ON WHILE ALREADY ON keeps the id (the checkbox's own
        // `on && !enabled`), rather than minting a new one.
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_TELEMETRY_CONSENT, 1)), FOXAPI_NO_CHANGE));
        CHECK(A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a) == firstId);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_TELEMETRY_CONSENT, 0))));
        CHECK(!A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a).empty());
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_TELEMETRY_CONSENT, 1))));
        CHECK(A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a) != firstId);  // a fresh id, not the old one
        const std::string secondId = A::telemetryInstallId(a);

        // THE API'S OP IS REFUSED FROM EVERY ORIGIN (round 3 fix, privacy):
        // applyCommand is where the web remote, CAT, a plugin and a future
        // API session all end, and none may switch reporting either way.
        covering(FOXAPI_OP_TELEMETRY_ENABLE);
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TELEMETRY_ENABLE, 0)), FOXAPI_DENIED));
        CHECK(A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a) == secondId);
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_TELEMETRY_CONSENT, 0))));
        CHECK(refused(A::apply(a, ints(FOXAPI_OP_TELEMETRY_ENABLE, 1)), FOXAPI_DENIED));
        CHECK(!A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a).empty());
        // The same through the queue a browser's or a key's command takes.
        A::submit(a, ints(FOXAPI_OP_TELEMETRY_ENABLE, 1));
        A::drain(a);
        CHECK(!A::telemetryEnabled(a));
        CHECK(A::telemetryInstallId(a).empty());
    }

    covering(FOXAPP_OP_NOTE_PANEL);
    {
        // Window port group 3: was engine_.telemetryNotePanel(name) from the
        // page's frame, every frame. Applied here as the op: the name is
        // listed once (a repeat is an answered no-op), an empty name is
        // BAD_ARGUMENT and lists nothing. (The cap of 16 and the REMOTE
        // refusal are test_engine_api_03's group U.)
        const std::vector<std::string> before = A::telemetryPanels(a);
        CHECK(before.size() < 16u);
        CHECK(ok(A::apply(a, text(FOXAPP_OP_NOTE_PANEL, "stage1-panel-note"))));
        CHECK(A::telemetryPanels(a).size() == before.size() + 1u);
        CHECK(A::telemetryPanels(a).back() == "stage1-panel-note");
        CHECK(ok(A::apply(a, text(FOXAPP_OP_NOTE_PANEL, "stage1-panel-note"))));
        CHECK(A::telemetryPanels(a).size() == before.size() + 1u);
        CHECK(refused(A::apply(a, text(FOXAPP_OP_NOTE_PANEL, "")), FOXAPI_BAD_ARGUMENT));
        CHECK(A::telemetryPanels(a).size() == before.size() + 1u);
    }

    covering(FOXAPI_OP_GPS);
    {
        CHECK(refused(A::apply(a, text(FOXAPI_OP_GPS, "", 1, 0, 9600.0)), FOXAPI_BAD_ARGUMENT));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_GPS, "STAGE1-NO-SUCH-PORT", 1, 0, 9600.0))));
        CHECK(ok(A::apply(a, text(FOXAPI_OP_GPS, "", 0))));
        CHECK(!A::gpsListening(a));
    }

    covering(FOXAPI_OP_SCAN_SERIAL_PORTS);
    {
        // API 0.3: the machine's ports, listed on the engine's thread; each
        // scan is counted (it moves listSeq), whatever it finds.
        const std::uint64_t before = A::serialPortsScans(a);
        CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_SCAN_SERIAL_PORTS))));
        CHECK(A::serialPortsScans(a) == before + 1u);
    }
}

// --- engine/stage3b-pre 2c: status lines/flags the window used to clear or
// consume in place, now commands -----------------------------------------------

void statusLineOps(AppWindow& a) {
    covering(FOXAPP_OP_SET_SOURCE_ERROR);
    {
        A::exclusive(a, [&] {  // (the pass writes the line too: set, command and read with none between)
            A::setSourceError(a, "stale");
            CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_SOURCE_ERROR, "a new message"))));
            CHECK(A::sourceError(a) == "a new message");
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_SET_SOURCE_ERROR))));  // empty text: clears
            CHECK(A::sourceError(a).empty());
        });
    }

    covering(FOXAPP_OP_SET_BOOKMARK_NOTE);
    {
        CHECK(ok(A::apply(a, text(FOXAPP_OP_SET_BOOKMARK_NOTE, "Exported 3 to somewhere.xml"))));
        CHECK(A::importNote(a) == "Exported 3 to somewhere.xml");
    }

    covering(FOXAPP_OP_CLEAR_STATUS);
    {
        // (Each line or flag below is one the pass writes or consumes too: each is set, cleared by the command and read as one block,
        // so the clearing seen is the command's.)
        A::exclusive(a, [&] {
            A::setGpsRefusal(a, "refused");
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_REFUSAL))));
            CHECK(A::gpsRefusal(a).empty());
        });

        A::exclusive(a, [&] {
            A::setSoundCardMissing(a, "missing");
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_SOUND_CARD_MISSING))));
            CHECK(A::soundCardMissing(a).empty());
        });

        A::exclusive(a, [&] {
            A::pushDecoderLogLine(a);
            CHECK(A::decoderLogSize(a) >= 1u);
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_DECODER_LOG))));
            CHECK(A::decoderLogSize(a) == 0u);
        });

        A::exclusive(a, [&] {
            A::setSoapyScanDeferredLogged(a, true);
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_SOAPY_SCAN_DEFERRED_LOGGED))));
            CHECK(!A::soapyScanDeferredLogged(a));
        });

        A::exclusive(a, [&] {
            A::setMutePopupQueued(a, true);
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_MUTE_POPUP_QUEUED))));
            CHECK(!A::mutePopupQueued(a));
        });

        // THE FINISHED READ'S LINE (window port group 14, part b; code 6): a failed read is Idle again, and the call is a no-op
        // while a read is running (the reader's own clearResult says so).
        CHECK(A::gpsFail(a));
        A::exclusive(a, [&] {  // (the pass polls the reader)
            CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_RESULT))));
            CHECK(A::gpsState(a) == 0);
        });
        CHECK(A::gpsListen(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_RESULT))));
        CHECK(A::gpsState(a) == 1);
        CHECK(A::gpsListening(a));
        A::gpsStop(a);
        // The refusal and the result are two selectors: clearing one leaves the other.
        A::setGpsRefusal(a, "refused");
        CHECK(A::gpsFail(a));
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_RESULT))));
        CHECK(A::gpsRefusal(a) == "refused");
        CHECK(ok(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_REFUSAL))));
        CHECK(A::gpsRefusal(a).empty());

        // An out-of-range selector is a refusal, not a silent no-op.
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, 999)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_CLEAR_STATUS, 7)), FOXAPI_OUT_OF_RANGE));
    }

    covering(FOXAPP_OP_MUTE_KEEP_RUNNING);
    {
        // "Keep it running" (engine/stage3b-pre fields-to-commands round 2):
        // both fields move together - the mute stays (muteKeptRunning_), and
        // the subject closes so the NEXT one is a fresh capture.
        // (One block: the subject is made up - no plugin here mutes - and the pass's own mute step closes such a popup, so a
        // free-running pass between these steps would answer for the command.)
        A::exclusive(a, [&] {
            A::setMutePopupSubject(a, "POCSAG", "pocsag");
            CHECK(A::mutePopupOpen(a));
            CHECK(A::mutePopupKeyCount(a) == 1u);
            CHECK(!A::muteKeptRunning(a));
            CHECK(ok(A::apply(a, cmd::make(FOXAPP_OP_MUTE_KEEP_RUNNING))));
            CHECK(A::muteKeptRunning(a));
            CHECK(!A::mutePopupOpen(a));
            CHECK(A::mutePopupKeyCount(a) == 0u);
        });
    }

    // engine/stage3b-pre Airspy round (OPEN 2/3): this suite's fake radio is
    // never recognised as an Airspy (cascade::gui::asAirspy's own check), so
    // only the REFUSAL half is provable here - the SUCCESS half, and the
    // published-state readback, is tests/test_airspy_app.cpp's real fake
    // Airspy device (that test gained its own coverage of these same
    // commands alongside this round).
    covering(FOXAPP_OP_AIRSPY_DECIMATION);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRSPY_DECIMATION, 2)), FOXAPI_FAILED));
    covering(FOXAPP_OP_AIRSPY_GAIN_MODE);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRSPY_GAIN_MODE, 0)), FOXAPI_FAILED));
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRSPY_GAIN_MODE, 99)), FOXAPI_OUT_OF_RANGE));
    covering(FOXAPP_OP_AIRSPY_AGC);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRSPY_AGC, 0, 1)), FOXAPI_FAILED));
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRSPY_AGC, 2, 1)), FOXAPI_OUT_OF_RANGE));
}

// --- the queue ------------------------------------------------------------------------------------

void queueDrainsInOrder(AppWindow& a) {
    std::printf("  [the local queue]\n");
    // A submitted command changes nothing until the drain; the drain applies
    // every one, in order, and empties the queue.
    // (One block up to the drain: a submit wakes a free-running pass, which drains at once - the queue's "until the drain" is
    // then that pass's, and the reads below would race it.)
    A::exclusive(a, [&] {
        CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_SQUELCH, -10.0))));
        A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -20.0));
        A::submit(a, num(FOXAPI_OP_SET_SQUELCH, -30.0));
        CHECK(A::queued(a) == 2u);
        CHECK(A::squelch(a) == -10.0f);
    });
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
        airbandOps(app);
        transmitterOps(app);
        otherOps(app);
        statusLineOps(app);
        queueDrainsInOrder(app);
    }
    everyOpHasACase();
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_apply_command");
}
