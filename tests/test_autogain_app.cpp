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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "net/web_control.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace cc = cascade::core;

namespace {

struct Record {
    std::string kind;
    std::string args;
    std::vector<bool> agcCalls;  // every setAutoGain(on), in order
    // Only for a radio with stages (Registry::stages): every setGainDb(name, db) it was asked
    // for, in order, and what each stage HOLDS now - the radio's own truth, which the window's
    // mirror (deviceGainsDb_) is only a copy of.
    std::vector<std::pair<std::string, double>> gainCalls;
    std::map<std::string, double> gainNow;
    // Every setGainDb and setAutoGain the radio received, in ONE list and in order ("gain LNA
    // 33.0", "agc 1"): the two calls above are in lists of their own, which cannot say which came
    // first - and the manual gain is put back BEFORE the remembered auto gain (0.99.75).
    std::vector<std::string> events;
};

std::string gainEvent(const std::string& name, double db) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "gain %s %.1f", name.c_str(), db);
    return buf;
}

struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Record>> made;
    std::vector<cascade::source::NativeDeviceInfo> native;
    // A radio that has auto gain but refuses to switch it ON (it accepts OFF, which
    // is what the open's manual-gain probe asks).
    std::atomic<bool> refuseAgcOn{false};
    // GAIN STAGES (0.99.74, the readback after automatic gain goes off). Off, a radio lists no
    // stages at all, which is what every earlier test here relies on. On, it has an LNA (0..40)
    // and a PGA (0..76) that move in 1 dB steps and hold what they are given.
    std::atomic<bool> stages{false};
    // The live finding on the B200 (SoapyUHD): switching automatic gain OFF leaves the radio at
    // the BOTTOM of every stage, whatever it held before the switch went on.
    std::atomic<bool> dropOnAgcOff{false};
    // While automatic gain is ON the radio reports its own choice (kAgcOwnDb on every stage),
    // not the manual value it will return to.
    std::atomic<bool> agcReportsOwn{false};
    // The PGA refuses to be moved (a step the driver turns down).
    std::atomic<bool> refusePga{false};
    // A driver whose autoGain() getter is not kept in step with the switch (it says "off" whatever
    // it was asked): the box has always shown what the radio ACCEPTED, not what the getter says.
    std::atomic<bool> autoGainGetterStale{false};
    // TWENTY stages, S00..S19 (0..40 dB, 1 dB steps), for the cap on how many stages of one radio the
    // manual gain memory keeps (0.99.75). Takes the place of the LNA/PGA pair.
    std::atomic<bool> manyStages{false};
};
Registry g_reg;

const double kLnaMax = 40.0;
const double kPgaMax = 76.0;
const double kAgcOwnDb = 17.0;

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
            if (g_reg.stages.load()) {
                rec_->gainNow["LNA"] = lna_;
                rec_->gainNow["PGA"] = pga_;
            }
        }
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override {
        if (g_reg.manyStages.load()) {
            std::vector<cascade::source::GainInfo> v;
            for (int i = 0; i < 20; ++i) { v.push_back({manyName(i), 0.0, kLnaMax, 1.0}); }
            return v;
        }
        if (!g_reg.stages.load()) { return {}; }
        return {{"LNA", 0.0, kLnaMax, 1.0}, {"PGA", 0.0, kPgaMax, 1.0}};
    }
    bool setGainDb(const std::string& name, double db) override {
        if (g_reg.manyStages.load()) {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->gainCalls.emplace_back(name, db);
            rec_->events.push_back(gainEvent(name, db));
            many_[name] = std::round(std::min(std::max(db, 0.0), kLnaMax));
            rec_->gainNow[name] = many_[name];
            return true;
        }
        if (!g_reg.stages.load()) { return false; }
        std::lock_guard<std::mutex> lk(g_reg.m);
        rec_->gainCalls.emplace_back(name, db);
        rec_->events.push_back(gainEvent(name, db));
        if (name == "LNA") {
            lna_ = std::round(std::min(std::max(db, 0.0), kLnaMax));
            rec_->gainNow[name] = lna_;
            return true;
        }
        if (name == "PGA" && !g_reg.refusePga.load()) {
            pga_ = std::round(std::min(std::max(db, 0.0), kPgaMax));
            rec_->gainNow[name] = pga_;
            return true;
        }
        err_ = "the radio turned down the gain step for " + name;
        return false;
    }
    double gainDb(const std::string& name) const override {
        if (g_reg.manyStages.load()) {
            const auto it = many_.find(name);
            return it == many_.end() ? 0.0 : it->second;
        }
        if (!g_reg.stages.load()) { return 0.0; }
        if (agc_ && g_reg.agcReportsOwn.load()) { return kAgcOwnDb; }
        return name == "LNA" ? lna_ : (name == "PGA" ? pga_ : 0.0);
    }
    bool autoGainSupported() const override { return kindHasAutoGain(kind_); }
    bool setAutoGain(bool on) override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->agcCalls.push_back(on);
            rec_->events.push_back(on ? "agc 1" : "agc 0");
        }
        if (!kindHasAutoGain(kind_)) { return false; }
        if (on && g_reg.refuseAgcOn.load()) {
            err_ = "the radio refused automatic gain";
            return false;
        }
        const bool was = agc_;
        agc_ = on;
        if (was && !on && g_reg.stages.load() && g_reg.dropOnAgcOff.load()) {
            // The B200's behaviour: out of automatic gain it is at the bottom of every stage.
            std::lock_guard<std::mutex> lk(g_reg.m);
            lna_ = 0.0;
            pga_ = 0.0;
            rec_->gainNow["LNA"] = lna_;
            rec_->gainNow["PGA"] = pga_;
        }
        return true;
    }
    bool autoGain() const override { return agc_ && !g_reg.autoGainGetterStale.load(); }
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
    static std::string manyName(int i) {
        return std::string("S") + (i < 10 ? "0" : "") + std::to_string(i);
    }
    std::map<std::string, double> many_;  // the stages of the manyStages radio
    std::string kind_;
    std::shared_ptr<Record> rec_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    bool agc_ = false;
    double lna_ = 10.0;  // what the radio holds on each stage (used only with Registry::stages)
    double pga_ = 12.0;
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

// The line written when automatic gain goes OFF and the manual gains are put back on the radio.
const char* const kRestoredLine = "source: manual gain restored after auto gain off";

int restoredLines() {
    int n = 0;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(kRestoredLine) != std::string::npos) { ++n; }
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
    g_reg.stages = false;
    g_reg.dropOnAgcOff = false;
    g_reg.agcReportsOwn = false;
    g_reg.refusePga = false;
    g_reg.autoGainGetterStale = false;
    g_reg.manyStages = false;
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
    // EVERY READ AND WRITE OF THE ENGINE GOES THROUGH THE SEAM (tests/pass_seam.hpp), as tests/test_converter_app_paths.cpp does.
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }
    static bool waitOpen(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceOpenPending_; })) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool selectNative(AppWindow& a, const std::string& args) {
        int row = -1;
        passseam::on(th::engine(a), [&] {
            th::engine(a).scanNative();
            for (std::size_t i = 0; i < th::engine(a).nativeDevices_.size(); ++i) {
                if (th::engine(a).nativeDevices_[i].args == args) {
                    row = AppWindow::kNativeRowBase + static_cast<int>(i);
                    break;
                }
            }
            if (row >= 0) { th::engine(a).selectSource(row); }
        });
        return row >= 0 && waitOpen(a);
    }
    static bool selectSoapy(AppWindow& a, const std::string& args) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).soapyDevices_.clear();
            th::engine(a).soapyDevices_.push_back({"B200", args});
            th::engine(a).selectSource(th::engine(a).soapyRowBase());
        });
        return waitOpen(a);
    }
    static void reopen(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).reopenAfterDriverFault(); }); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { passseam::on(th::engine(a), [&] { th::applyConfig(a, cfg); }); }
    static cascade::core::AppConfig saved(AppWindow& a) {
        passseam::on(th::engine(a), [&] { th::engine(a).publishConfig(); });
        return a.currentConfig();
    }

    // The engine's state: the box, whether it is offered, and the memory (COPIES, made on the pass).
    static bool agc(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceAgc_; }); }
    static bool agcSupported(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceAgcSupported_; });
    }
    static std::map<std::string, bool> memory(AppWindow& a) {
        return passseam::read<std::map<std::string, bool>>(th::engine(a), [&] { return th::engine(a).autoGainMemory_; });
    }
    static void remember(AppWindow& a, const std::string& key, bool on) {
        passseam::on(th::engine(a), [&] { th::engine(a).autoGainMemory_[key] = on; });
    }
    // The box's own path (the SET_DEVICE_AGC op's handler).
    static bool toggle(AppWindow& a, bool want) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).changeAutoGain(want); });
    }

    // The gain mirrors (what the Source section's sliders and the top bar's GAIN dial show), and the user's hand on a slider: the radio is
    // asked and the mirror takes the READBACK, as the slider's own code does.
    static std::vector<std::string> gainNames(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(th::engine(a), [&] { return th::engine(a).deviceGainNames_; });
    }
    static std::vector<float> gainMirror(AppWindow& a) {
        return passseam::read<std::vector<float>>(th::engine(a), [&] { return th::engine(a).deviceGainsDb_; });
    }
    // THE HAND ON A SLIDER THAT REMEMBERS NOTHING: the radio is asked and the mirror takes the readback (no manual memory is written).
    static bool setManual(AppWindow& a, std::size_t i, double db) {
        return passseam::read<bool>(th::engine(a), [&] {
            cascade::engine::Engine& e = th::engine(a);
            if (e.device_ == nullptr || i >= e.deviceGainNames_.size()) { return false; }
            if (!e.device_->setGainDb(e.deviceGainNames_[i], db)) { return false; }
            e.deviceGainsDb_[i] = static_cast<float>(e.device_->gainDb(e.deviceGainNames_[i]));
            return true;
        });
    }
    static void clearError(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).sourceError_.clear(); }); }
    static std::string error(AppWindow& a) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).sourceError_; });
    }

    // THE MANUAL GAIN MEMORY (0.99.75). `userGain` is the path the Source section's sliders and the top bar's GAIN dial both take to the
    // radio - in the branch a FOXAPI_OP_SET_GAIN command, which the engine answers with the readback and remembers; `controlGain` is a
    // browser's gain control, the same command by name.
    using GainList = std::vector<std::pair<std::string, double>>;
    static std::map<std::string, GainList> manualMemory(AppWindow& a) {
        return passseam::read<std::map<std::string, GainList>>(th::engine(a), [&] { return th::engine(a).manualGainMemory_; });
    }
    static void rememberManual(AppWindow& a, const std::string& key, GainList g) {
        passseam::on(th::engine(a), [&] { th::engine(a).manualGainMemory_[key] = std::move(g); });
    }
    static bool userGain(AppWindow& a, std::size_t stage, double db) {
        return passseam::read<bool>(th::engine(a), [&] {
            cascade::engine::Engine& e = th::engine(a);
            if (e.device_ == nullptr || stage >= e.deviceGainNames_.size()) { return false; }
            const cascade::core::cmd::QueuedCommand q =
                cascade::core::cmd::makeText(FOXAPI_OP_SET_GAIN, e.deviceGainNames_[stage], 0, 0, db);
            const FoxCommandResult r = e.applyCommand(q.c, q.longText);
            return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
        });
    }
    static void controlGain(AppWindow& a, const std::string& name, double db) {
        passseam::on(th::engine(a), [&] {
            const cascade::core::cmd::QueuedCommand q = cascade::core::cmd::makeText(FOXAPI_OP_SET_GAIN, name, 0, 0, db);
            (void)th::engine(a).applyCommand(q.c, q.longText);
        });
    }
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
    th::TestWindow app;
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
    th::TestWindow app;
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
    th::TestWindow app;
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
    th::TestWindow app;
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
    th::TestWindow app;
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
    th::TestWindow app;
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
    th::TestWindow app;
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
        th::TestWindow app;
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
        th::TestWindow app;
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
        th::TestWindow app;
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
        th::TestWindow app;
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
        th::TestWindow app;
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

// --- THE GAINS AFTER AUTOMATIC GAIN GOES OFF (0.99.74) ---------------------------------------------
//
// Found live on the B200 by the verifier of the top bar's GAIN dial: switching automatic gain OFF
// left the radio about 35 dB below the 76 dB the window's mirror (and so the dial) still showed,
// because the radio drops to the bottom of every stage when it leaves automatic gain and the window
// never put its manual gains back nor read them again. Only an Airspy was read back (the callers
// refresh its mirrors). The fake radio here does what the B200 does: out of automatic gain it holds
// the BOTTOM of every stage.

// The radio holds (lna, pga) AND the window's mirror says the same - the dial and the sliders
// are showing what the radio really has.
bool holds(cascade::gui::AppWindow& app, std::size_t r, double lna, double pga) {
    const Record rec = made(r);
    const std::vector<float> m = Access::gainMirror(app);
    const auto at = [&rec](const char* n) {
        const auto it = rec.gainNow.find(n);
        return it == rec.gainNow.end() ? -1.0 : it->second;
    };
    const bool ok = at("LNA") == lna && at("PGA") == pga && m.size() == 2u &&
                    m[0] == static_cast<float>(lna) && m[1] == static_cast<float>(pga);
    if (!ok) {
        std::printf("    wanted LNA %.0f PGA %.0f; the radio holds LNA %.0f PGA %.0f, the mirror says "
                    "%.0f %.0f\n",
                    lna, pga, at("LNA"), at("PGA"), m.size() > 0 ? static_cast<double>(m[0]) : -1.0,
                    m.size() > 1 ? static_cast<double>(m[1]) : -1.0);
    }
    return ok;
}

// A radio with the two stages, opened the B200's way (a Soapy device: a mid-dial 30 dB goes into
// every stage at open) or as a native RTL-SDR (left as its driver chose).
std::size_t openStaged(cascade::gui::AppWindow& app, bool soapy) {
    const std::size_t idx = madeCount();
    CHECK(soapy ? Access::selectSoapy(app, kSoapyArgs) : Access::selectNative(app, kRtlA));
    CHECK(madeCount() == idx + 1);
    const std::vector<std::string> names = Access::gainNames(app);
    CHECK(names.size() == 2u);
    CHECK(Access::agcSupported(app));
    return idx;
}

void testGainsComeBackAfterAgcOff(bool soapy) {
    std::printf("  AUTO off on a radio that drops to the bottom of every stage (%s): the manual "
                "gains go back on the radio and the mirror reads them\n",
                soapy ? "Soapy, the B200's way" : "native");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, soapy);
    CHECK(!Access::agc(app));
    // The hand on the sliders.
    CHECK(Access::setManual(app, 0, 33.0));
    CHECK(Access::setManual(app, 1, 21.0));
    CHECK(holds(app, r, 33.0, 21.0));

    // AUTO OFF while it is already off is no change of mode: nothing is set again, nothing logged.
    {
        const std::size_t calls = made(r).gainCalls.size();
        CHECK(Access::toggle(app, false));
        CHECK(made(r).gainCalls.size() == calls);
        CHECK(restoredLines() == 0);
    }

    // AUTO ON: this radio leaves its gains alone, and the mirror is read back (the same numbers).
    CHECK(Access::toggle(app, true));
    CHECK(Access::agc(app));
    CHECK(holds(app, r, 33.0, 21.0));
    CHECK(restoredLines() == 0);

    // AUTO OFF: the radio falls to its bottoms; the window puts the manual values back, one call
    // per stage, and the mirror is what the radio then holds.
    const std::size_t before = made(r).gainCalls.size();
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
    CHECK(holds(app, r, 33.0, 21.0));
    const Record after = made(r);
    CHECK(after.gainCalls.size() == before + 2u);
    if (after.gainCalls.size() == before + 2u) {
        CHECK(after.gainCalls[before].first == "LNA" && after.gainCalls[before].second == 33.0);
        CHECK(after.gainCalls[before + 1].first == "PGA" && after.gainCalls[before + 1].second == 21.0);
    }
    CHECK(restoredLines() == 1);

    // And again: the second round trip comes back the same, with one more line.
    CHECK(Access::toggle(app, true));
    CHECK(Access::toggle(app, false));
    CHECK(holds(app, r, 33.0, 21.0));
    CHECK(restoredLines() == 2);
}

void testGainsComeBackAfterRememberedAgcOff() {
    std::printf("  AUTO off on a radio opened already on auto gain (remembered): the gains it was "
                "opened with come back\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    th::TestWindow app;
    Access::remember(app, kKeyA, true);
    const std::size_t r = openStaged(app, false);
    CHECK(Access::agc(app));
    // Nothing was switched by the user, so there is no manual value but the one the open read.
    CHECK(holds(app, r, 10.0, 12.0));
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
    CHECK(holds(app, r, 10.0, 12.0));
    CHECK(restoredLines() == 1);
}

void testAgcOnShowsWhatTheRadioReports() {
    std::printf("  AUTO on reads the radio back (the dial shows what it reports); AUTO off still "
                "returns the MANUAL values, not what auto gain chose\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    g_reg.agcReportsOwn = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    CHECK(Access::setManual(app, 0, 33.0));
    CHECK(Access::setManual(app, 1, 21.0));
    CHECK(Access::toggle(app, true));
    {
        // The radio's own stages did not move; it REPORTS its automatic choice, and the mirror
        // (the dial) shows that.
        const std::vector<float> m = Access::gainMirror(app);
        CHECK(m.size() == 2u && m[0] == static_cast<float>(kAgcOwnDb) &&
              m[1] == static_cast<float>(kAgcOwnDb));
        const Record rec = made(r);
        CHECK(rec.gainNow.at("LNA") == 33.0 && rec.gainNow.at("PGA") == 21.0);
    }
    // ON again while it is on is no change of mode: the numbers kept are still the user's, not the
    // 17 dB the dial now shows.
    CHECK(Access::toggle(app, true));
    // Out of automatic gain it is the user's own numbers that come back.
    CHECK(Access::toggle(app, false));
    CHECK(holds(app, r, 33.0, 21.0));
}

void testBoxKeepsWhatTheRadioAccepted() {
    std::printf("  the box shows what the radio ACCEPTED, even from a driver whose autoGain() getter "
                "is not kept in step\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.autoGainGetterStale = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    (void)r;
    CHECK(Access::toggle(app, true));
    CHECK(Access::agc(app));
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
}

void testGainStepRefusedIsDroppedSilently() {
    std::printf("  a stage the radio turns down is dropped without a word; the others come back and "
                "the mirror matches the radio\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    CHECK(Access::setManual(app, 0, 33.0));
    CHECK(Access::setManual(app, 1, 21.0));
    CHECK(Access::toggle(app, true));
    g_reg.refusePga = true;
    Access::clearError(app);
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
    // LNA is back; the PGA stays at the bottom it fell to, and the mirror says so.
    CHECK(holds(app, r, 33.0, 0.0));
    // Silently: no error line for a step of a gesture that is not the user's.
    CHECK(Access::error(app).empty());
    CHECK(restoredLines() == 1);
}

void testAgcRefusedTouchesNoGain() {
    std::printf("  a refused AUTO on, and a radio with no stages: no gain call, no line\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    {
        th::TestWindow app;
        const std::size_t r = openStaged(app, true);
        CHECK(Access::setManual(app, 0, 33.0));
        CHECK(Access::setManual(app, 1, 21.0));
        g_reg.refuseAgcOn = true;
        const std::size_t before = made(r).gainCalls.size();
        CHECK(!Access::toggle(app, true));
        CHECK(!Access::agc(app));
        CHECK(made(r).gainCalls.size() == before);
        CHECK(holds(app, r, 33.0, 21.0));
        CHECK(restoredLines() == 0);
    }
    // A radio that lists no stages: the switch works and nothing is restored or logged.
    resetRegistry();
    {
        th::TestWindow app;
        const std::size_t r = openNative(app, kRtlA);
        CHECK(Access::gainNames(app).empty());
        CHECK(Access::toggle(app, true));
        CHECK(Access::toggle(app, false));
        CHECK(!Access::agc(app));
        CHECK(made(r).gainCalls.empty());
        CHECK(restoredLines() == 0);
    }
}

// --- THE MANUAL GAIN IS REMEMBERED PER RADIO (0.99.75) -----------------------------------------------
//
// The owner runs his B200 at 76 dB and every open pushed a mid-dial 30 dB into each stage of a Soapy
// radio (kSoapyGainDefaultDb), with nothing remembering the number his hand had chosen. What is proved
// here, through the real AppWindow on the same fake radios (an LNA 0..40 and a PGA 0..76 that quantise
// to whole decibels and hold what they are given):
//   * only a gain the USER chose is written - not the open's own 30 dB push, not the restore, not a
//     refused step, not anything while auto gain is on - and what is written is the radio's READBACK;
//   * every path a user's gain takes writes it: applyUserGain (the sliders and the GAIN dial) and a
//     browser's gain control (the scope face's knob is the one path with no test: it is drawn by ImGui);
//   * it is put back at an ordinary open: after the push, clamped into the stage's range, only for
//     stages the radio still lists, a refused step skipped, the mirror reading what the radio took, one
//     log line, and BEFORE the remembered auto gain - while a recovery reopen restores nothing;
//   * it survives a restart through the saved config file, and is capped per radio and in radios.

using ManualList = Access::GainList;

const char* const kManualRestoredPrefix = "source: manual gain restored - ";

std::vector<std::string> manualRestoredLines() {
    std::vector<std::string> out;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(kManualRestoredPrefix) != std::string::npos) { out.push_back(line); }
    }
    return out;
}

bool manualMemoryIs(cascade::gui::AppWindow& app, const std::string& key, const ManualList& want) {
    const auto m = Access::manualMemory(app);
    const auto it = m.find(key);
    if (it == m.end()) {
        std::printf("    no manual gain entry for %s\n", key.c_str());
        return false;
    }
    if (it->second != want) {
        std::printf("    the entry for %s holds:", key.c_str());
        for (const auto& p : it->second) { std::printf(" %s %.2f", p.first.c_str(), p.second); }
        std::printf("\n");
        return false;
    }
    return true;
}

// Where an event is in the radio's one list of calls, or npos.
std::size_t eventIndex(const Record& r, const std::string& what, bool last) {
    std::size_t found = std::string::npos;
    for (std::size_t i = 0; i < r.events.size(); ++i) {
        if (r.events[i] == what) {
            found = i;
            if (!last) { return i; }
        }
    }
    return found;
}

void testManualNothingRemembered() {
    std::printf("  manual gain: nothing remembered, so the open's own push is all there is and nothing is written\n");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    // THE B200'S WAY: a mid-dial 30 dB into every stage, and nothing else.
    CHECK(holds(app, r, 30.0, 30.0));
    CHECK(made(r).gainCalls == (ManualList{{"LNA", 30.0}, {"PGA", 30.0}}));
    // ...and that push is not the user's: nothing is remembered because of it.
    CHECK(Access::manualMemory(app).empty());
    CHECK(manualRestoredLines().empty());
    CHECK(Access::saved(app).manualGainByRadio.empty());
}

void testManualUserGainIsRemembered() {
    std::printf("  manual gain: a stage the user sets is remembered as the radio's readback, per stage, and a "
                "refusal or auto gain writes nothing\n");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    CHECK(Access::manualMemory(app).empty());

    // THE HAND ON THE PGA: 76 dB, the owner's number. Only that stage is written - the LNA the open
    // pushed to 30 is not the user's choice and is not in it.
    CHECK(Access::userGain(app, 1, 76.0));
    CHECK(holds(app, r, 30.0, 76.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"PGA", 76.0}}));

    // THE READBACK, not the request: the radio rounds 33.4 to 33. Stages are kept sorted by name.
    CHECK(Access::userGain(app, 0, 33.4));
    CHECK(holds(app, r, 33.0, 76.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"LNA", 33.0}, {"PGA", 76.0}}));

    // THE SAVED CONFIG CARRIES IT, and the save debounce would notice the change.
    {
        const cascade::core::AppConfig cfg = Access::saved(app);
        CHECK(cfg.manualGainByRadio == Access::manualMemory(app));
        CHECK(cfg.manualGainByRadio.count(kKeySoapy) == 1);
    }

    // A REFUSAL writes nothing: the radio turns the PGA step down, so the memory keeps the 76.
    g_reg.refusePga = true;
    CHECK(!Access::userGain(app, 1, 10.0));
    g_reg.refusePga = false;
    CHECK(manualMemoryIs(app, kKeySoapy, {{"LNA", 33.0}, {"PGA", 76.0}}));

    // AUTO GAIN ON: the radio takes the step but reports its own choice while it gains itself, so
    // nothing is written (the user's numbers are the ones already there).
    CHECK(Access::toggle(app, true));
    CHECK(Access::userGain(app, 1, 50.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"LNA", 33.0}, {"PGA", 76.0}}));
    // ...and a stage the radio does not list, or no stage at all, is nothing to write.
    CHECK(!Access::userGain(app, 9, 5.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"LNA", 33.0}, {"PGA", 76.0}}));
}

void testManualBrowserGainIsRemembered() {
    std::printf("  manual gain: a gain a browser sets is remembered like one the hand sets\n");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    const std::size_t r = openStaged(app, true);
    Access::controlGain(app, "PGA", 60.0);
    CHECK(holds(app, r, 30.0, 60.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"PGA", 60.0}}));
    // A stage the radio has no such name for: refused, and nothing written.
    Access::controlGain(app, "GHOST", 5.0);
    CHECK(manualMemoryIs(app, kKeySoapy, {{"PGA", 60.0}}));
}

void testManualRestoredAtOpen(bool soapy) {
    std::printf("  manual gain: put back at the open (%s), after the push, the mirror reading what the radio "
                "took, one line\n",
                soapy ? "Soapy, the B200's way" : "native");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    const std::string key = soapy ? kKeySoapy : kKeyA;
    Access::rememberManual(app, key, {{"LNA", 33.0}, {"PGA", 76.0}});
    const std::size_t r = openStaged(app, soapy);
    // THE RADIO HOLDS THEM AND THE MIRROR (the dial) SHOWS THEM.
    CHECK(holds(app, r, 33.0, 76.0));
    // After the push on a Soapy radio (30, 30 first), and alone on a native one (its driver chose
    // its own state at open and is left alone).
    const ManualList wantCalls = soapy ? ManualList{{"LNA", 30.0}, {"PGA", 30.0}, {"LNA", 33.0}, {"PGA", 76.0}}
                                       : ManualList{{"LNA", 33.0}, {"PGA", 76.0}};
    CHECK(made(r).gainCalls == wantCalls);
    // One line, naming the stages and what they hold.
    const std::vector<std::string> lines = manualRestoredLines();
    CHECK(lines.size() == 1u);
    CHECK(lines.size() == 1u &&
          lines[0].find("source: manual gain restored - LNA 33.0, PGA 76.0") != std::string::npos);
    // The open does not rewrite the memory.
    CHECK(manualMemoryIs(app, key, {{"LNA", 33.0}, {"PGA", 76.0}}));
    CHECK(Access::manualMemory(app).size() == 1u);
    // ...and nothing was remembered for the other family of radio.
    CHECK(Access::memory(app).empty());
}

void testManualClampedAndFiltered() {
    std::printf("  manual gain: clamped into the stage's range, and only for stages the radio still reports\n");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    // GHOST is a stage this radio does not list; -5 is below the LNA's 0 and 200 above the PGA's 76.
    Access::rememberManual(app, kKeySoapy, {{"GHOST", 12.0}, {"LNA", -5.0}, {"PGA", 200.0}});
    const std::size_t r = openStaged(app, true);
    CHECK(holds(app, r, 0.0, 76.0));
    // The radio was asked for the CLAMPED numbers, and never for the ghost.
    const Record rec = made(r);
    CHECK(rec.gainCalls ==
          (ManualList{{"LNA", 30.0}, {"PGA", 30.0}, {"LNA", 0.0}, {"PGA", 76.0}}));
    for (const auto& c : rec.gainCalls) { CHECK(c.first != "GHOST"); }
    const std::vector<std::string> lines = manualRestoredLines();
    CHECK(lines.size() == 1u);
    CHECK(lines.size() == 1u &&
          lines[0].find("source: manual gain restored - LNA 0.0, PGA 76.0") != std::string::npos);
    // The stage the radio does not list is kept for a radio that does (nothing is erased at open).
    CHECK(manualMemoryIs(app, kKeySoapy, {{"GHOST", 12.0}, {"LNA", -5.0}, {"PGA", 200.0}}));
}

void testManualRefusedStageSkipped() {
    std::printf("  manual gain: a stage the radio turns down is skipped without a word, the others come back\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.refusePga = true;
    th::TestWindow app;
    Access::rememberManual(app, kKeyA, {{"LNA", 20.0}, {"PGA", 70.0}});
    const std::size_t r = openStaged(app, false);
    // The LNA is back; the PGA stayed where its driver left it (12) and the mirror says so.
    CHECK(holds(app, r, 20.0, 12.0));
    CHECK(Access::error(app).empty());
    const std::vector<std::string> lines = manualRestoredLines();
    CHECK(lines.size() == 1u);
    CHECK(lines.size() == 1u &&
          lines[0].find("source: manual gain restored - LNA 20.0") != std::string::npos);
    CHECK(lines.size() == 1u && lines[0].find("PGA") == std::string::npos);
}

void testManualNotOnARecoveryReopen() {
    std::printf("  manual gain: a reopen after a driver fault puts back the gains the user had a moment ago, "
                "not the memory\n");
    resetRegistry();
    g_reg.stages = true;
    th::TestWindow app;
    Access::rememberManual(app, kKeySoapy, {{"PGA", 70.0}});
    const std::size_t first = openStaged(app, true);
    CHECK(holds(app, first, 30.0, 70.0));
    CHECK(manualRestoredLines().size() == 1u);
    // The radio is moved by something that is not the user's hand on a control (the test hook
    // writes the radio and the mirror and not the memory): the memory still says 70.
    CHECK(Access::setManual(app, 1, 25.0));
    CHECK(manualMemoryIs(app, kKeySoapy, {{"PGA", 70.0}}));
    const std::size_t again = madeCount();
    Access::reopen(app);
    CHECK(Access::waitOpen(app));
    CHECK(madeCount() == again + 1);
    // The new radio has the 25 the user had, not the remembered 70; and no second line.
    CHECK(holds(app, again, 30.0, 25.0));
    CHECK(manualRestoredLines().size() == 1u);
    CHECK(manualMemoryIs(app, kKeySoapy, {{"PGA", 70.0}}));
}

void testManualBeforeTheRememberedAutoGain() {
    std::printf("  manual gain: put back BEFORE the remembered auto gain, so the numbers are under it for the "
                "switch to return to\n");
    resetRegistry();
    g_reg.stages = true;
    g_reg.dropOnAgcOff = true;
    th::TestWindow app;
    Access::rememberManual(app, kKeyA, {{"LNA", 33.0}, {"PGA", 76.0}});
    Access::remember(app, kKeyA, true);
    const std::size_t r = openStaged(app, false);
    CHECK(Access::agc(app));
    // IN THE RADIO'S OWN ORDER OF CALLS: the manual probe, both stages, then the switch ON.
    const Record rec = made(r);
    const std::size_t agcOn = eventIndex(rec, "agc 1", false);
    const std::size_t lastGain = eventIndex(rec, "gain PGA 76.0", true);
    CHECK(agcOn != std::string::npos && lastGain != std::string::npos);
    CHECK(lastGain < agcOn);
    // Switched off, the numbers the user had come back (the B200 falls to the bottom of every stage).
    CHECK(Access::toggle(app, false));
    CHECK(!Access::agc(app));
    CHECK(holds(app, r, 33.0, 76.0));
}

void testManualRestart() {
    std::printf("  manual gain: a restart through the saved file brings the radio back at the gain it was left\n");
    resetRegistry();
    g_reg.stages = true;
    const std::string path = (g_scratch / "restart-manual.json").string();
    std::string err;
    {
        th::TestWindow app;
        openStaged(app, false);
        CHECK(Access::userGain(app, 0, 33.0));
        CHECK(Access::userGain(app, 1, 70.0));
        const cascade::core::AppConfig cfg = Access::saved(app);
        CHECK(cfg.sourceKind == "rtlsdr" && cfg.nativeArgs == kRtlA);
        CHECK(cfg.manualGainByRadio.count(kKeyA) == 1);
        CHECK(cascade::core::ConfigStore::save(path, cfg, err));
    }
    {
        cascade::core::AppConfig cfg;
        CHECK(cascade::core::ConfigStore::load(path, cfg, err));
        CHECK(cfg.manualGainByRadio.count(kKeyA) == 1 &&
              cfg.manualGainByRadio.at(kKeyA) == (ManualList{{"LNA", 33.0}, {"PGA", 70.0}}));
        th::TestWindow app;
        const std::size_t idx = madeCount();
        Access::restore(app, cfg);
        CHECK(madeCount() == idx + 1);
        CHECK(made(idx).args == kRtlA);
        CHECK(holds(app, idx, 33.0, 70.0));
        CHECK(manualMemoryIs(app, kKeyA, {{"LNA", 33.0}, {"PGA", 70.0}}));
    }
}

void testManualCaps() {
    std::printf("  manual gain: at most 64 radios and 16 stages of one radio are remembered; an entry already "
                "there is always updated\n");
    resetRegistry();
    g_reg.stages = true;
    {
        th::TestWindow app;
        // 63 other radios and the one about to be opened: the memory is full at 64.
        for (int i = 0; i < 63; ++i) {
            Access::rememberManual(app, cc::biasTeeRadioKey("rtlsdr", "serial=F" + std::to_string(1000 + i)),
                                   {{"LNA", 5.0}});
        }
        Access::rememberManual(app, kKeyA, {{"LNA", 5.0}});
        CHECK(Access::manualMemory(app).size() == 64u);
        // A radio already in the memory is updated at the cap...
        openStaged(app, false);
        CHECK(Access::userGain(app, 0, 12.0));
        CHECK(manualMemoryIs(app, kKeyA, {{"LNA", 12.0}}));
        CHECK(Access::manualMemory(app).size() == 64u);
        // ...and a radio that is not in it is not added.
        openNative(app, kRtlB);
        CHECK(Access::userGain(app, 0, 7.0));
        CHECK(Access::manualMemory(app).size() == 64u);
        CHECK(Access::manualMemory(app).count(kKeyB) == 0u);
    }
    resetRegistry();
    g_reg.manyStages = true;
    {
        th::TestWindow app;
        const std::size_t r = openNative(app, kRtlA);
        (void)r;
        CHECK(Access::gainNames(app).size() == 20u);
        for (std::size_t i = 0; i < 20u; ++i) { CHECK(Access::userGain(app, i, static_cast<double>(i + 1u))); }
        // S00..S15 are in; S16..S19 are not.
        const auto memory = Access::manualMemory(app);   // a COPY, made on the pass: the iterator below must not outlive a temporary
        const auto it = memory.find(kKeyA);
        CHECK(it != memory.end());
        if (it != memory.end()) {
            CHECK(it->second.size() == 16u);
            CHECK(!it->second.empty() && it->second.front() == std::make_pair(std::string("S00"), 1.0));
            CHECK(!it->second.empty() && it->second.back() == std::make_pair(std::string("S15"), 16.0));
        }
        // A stage already remembered is still updated past the cap.
        CHECK(Access::userGain(app, 0, 40.0));
        CHECK(manualMemoryIs(app, kKeyA, [] {
            ManualList want;
            for (int i = 0; i < 16; ++i) {
                want.emplace_back(std::string("S") + (i < 10 ? "0" : "") + std::to_string(i),
                                  i == 0 ? 40.0 : static_cast<double>(i + 1));
            }
            return want;
        }()));
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
    testGainsComeBackAfterAgcOff(true);
    testGainsComeBackAfterAgcOff(false);
    testGainsComeBackAfterRememberedAgcOff();
    testAgcOnShowsWhatTheRadioReports();
    testBoxKeepsWhatTheRadioAccepted();
    testGainStepRefusedIsDroppedSilently();
    testAgcRefusedTouchesNoGain();

    testManualNothingRemembered();
    testManualUserGainIsRemembered();
    testManualBrowserGainIsRemembered();
    testManualRestoredAtOpen(true);
    testManualRestoredAtOpen(false);
    testManualClampedAndFiltered();
    testManualRefusedStageSkipped();
    testManualNotOnARecoveryReopen();
    testManualBeforeTheRememberedAutoGain();
    testManualRestart();
    testManualCaps();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_autogain_app");
}
