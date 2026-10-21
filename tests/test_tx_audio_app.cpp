// test_tx_audio_app.cpp - a plugin transmits, or does not, through the REAL AppWindow, the real
// plugin runner and a REAL plugin module (tests/fixtures/tx_audio_plugin.cpp).
//
// WHAT THIS ADDS TO tests/test_tx_audio.cpp. That file proves each rule on its own with static
// tables. This one proves the rules are WIRED: that the application's own frame loop (the
// decision, the transmitter's tick, the runner being told it is keyed, the window closing for
// the reasons it should) turns a module that is always willing to transmit into a transmitter
// that stays quiet unless every condition holds - and that what the module itself saw is
// what the application says it did. The module counts every call the host makes to it
// (actives, pulls, frames), so "it was never pulled from" is read off the module and not off
// the application's account of itself.
//
// THE FRAME. drawUi runs, in this order, txPageKey -> setLatched/setPttHeld ->
// applyPluginTransmitKey -> Transmitter::tick -> afterTransmitTick, and
// tests/test_transmit_page.cpp reads app_window.cpp to pin that order. frame() below makes the
// same calls in the same order, so a scenario is a sequence of real frames with the page live
// or not, the PTT held or not, and a few milliseconds of real time between them (the DSP
// thread is the pipeline's own, fed by the signal generator, and it is what polls the module).
//
// THE RADIO is a ledger (FakeTxRadio): nothing here transmits.
//
// SCENARIOS, each on a fresh window:
//   refusals    ARM with no radio, with no grant; every choice of minutes but the three
//   quiet       the module asks (flag on): without the grant, granted but not armed, armed but
//               with the radio gone - nothing is keyed, nothing is pulled
//   keys        granted + armed + the flag: keyed, the module is pulled from, it is told
//               (get_state), the log says so; the flag off: unkeyed after the tail
//   closes      each reason the window must close and the key must open: the page closed, the
//               radio closed, OFF, the grant withdrawn, a radio fault, the cap, the window's own
//               time - and after each, the module still asking does NOT key again
//   hand        a PTT held while the plugin asks: the plugin is not the source
//   config      the grant is saved and loaded; the window is neither
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_run.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;

namespace {

fs::path g_scratch;
fs::path g_plugins;
std::string g_fixture;

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

const char* const kFile = "tx-audio-fixture-1.0.0";  // the module file name, less its extension
const char* const kPluginName = "TX Audio Fixture";

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_tx_audio_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    cc::PackageIdentity id;
    id.packaged = true;
    id.fullName = "tx-audio-app-test";
    cc::setPackageIdentityForTest(id);
    g_plugins = fs::path(cc::PluginHost::userPluginDir());
    fs::create_directories(g_plugins, ec);
    fs::copy_file(g_fixture, g_plugins / (std::string(kFile) + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// --- the radio ----------------------------------------------------------------

class FakeTxRadio : public cascade::source::IqSink {
public:
    bool start() override {
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override {
        if (running_) { ++stops; }
        running_ = false;
    }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return -89.75; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override {
        if (!running_ || faulted_) { return 0; }
        samples += n;
        return n;
    }
    bool faulted() const override { return faulted_; }
    const char* name() const override { return "fake transmit radio"; }
    const char* lastError() const override { return faulted_ ? "the fake radio faulted" : ""; }
    void fault() { faulted_ = true; }

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<unsigned long long> samples{0};

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> faulted_{false};
};

// --- what the module saw ---------------------------------------------------------

struct Counts {
    unsigned long long actives = 0;
    unsigned long long pulls = 0;
    unsigned long long frames = 0;
};

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void rescan(AppWindow& a) { a.rescanPlugins(); }
    static void start(AppWindow& a) { a.startReceiver(); }
    static void stop(AppWindow& a) { a.stopReceiver("test"); }
    static void takeDown(AppWindow& a) { a.detachAndUnloadPlugins(); }
    static void pin(AppWindow& a, const std::string& file) {
        a.setPluginRun(file, cc::PluginRun::Always);
    }
    static const std::vector<cc::LoadedPlugin>& plugins(const AppWindow& a) {
        return a.pluginHost_.plugins();
    }
    static void setFlag(AppWindow& a, bool on) {
        a.pluginUi_.api().settingsUiSet(kPluginName, "send", on ? "1" : "0");
    }
    static void openRadio(AppWindow& a, std::unique_ptr<cascade::source::IqSink> sink) {
        a.transmitter_.setSink(std::move(sink));
        a.transmitter_.setMode(cascade::dsp::TxMode::NFM);
        a.transmitter_.setInput(cascade::core::TxInput::Tone);
        a.transmitter_.setDataFlat(a.transmitDataFlat_);
    }
    static void closeRadio(AppWindow& a) { a.closeTransmitRadio(); }
    static cc::TxArming::Refusal arm(AppWindow& a, int minutes) { return a.armTransmitPlugins(minutes); }
    static void off(AppWindow& a) { a.disarmTransmitPlugins("you pressed OFF"); }
    static bool armed(const AppWindow& a) { return a.txArming_.armed(); }
    static std::string whyClosed(const AppWindow& a) { return a.txArming_.lastReason(); }
    // A window that really was armed, but whose time is nearly up: `msLeft` milliseconds.
    static void armWithTimeLeft(AppWindow& a, int msLeft) {
        a.txArming_.arm(5, true, true,
                        cc::TxArming::Clock::now() - std::chrono::minutes(5) +
                            std::chrono::milliseconds(msLeft));
    }
    static void grant(AppWindow& a, const std::string& file, bool on) {
        a.setPluginTransmitAllowed(file, on);
    }
    static void grantRunnerOnly(AppWindow& a, const std::string& file) {
        a.pluginRunner_.setTxGranted({file});
    }
    static bool durableGrant(const AppWindow& a, const std::string& file) {
        for (const std::string& k : a.pluginTransmitAllowed_) {
            if (k == file) { return true; }
        }
        return false;
    }
    static bool refusalIsNone(const AppWindow& a) {
        return a.txArmRefusal_ == cc::TxArming::Refusal::None;
    }

    // ONE FRAME, in drawUi's order.
    static void frame(AppWindow& a, bool pageLive, bool pttHeld = false) {
        a.transmitPageLive_ = pageLive;
        const TxPageKey key = txPageKey(a.transmitPageLive_, a.transmitter_.latched(), false, pttHeld);
        a.transmitter_.setLatched(key.latched);
        a.transmitter_.setPttHeld(key.pttHeld);
        a.applyPluginTransmitKey(key);
        a.transmitter_.tick();
        a.afterTransmitTick();
    }

    static bool pluginKeyed(const AppWindow& a) { return a.transmitter_.pluginKeyed(); }
    static bool transmitting(const AppWindow& a) { return a.transmitter_.transmitting(); }
    static cascade::core::Transmitter& tx(AppWindow& a) { return a.transmitter_; }
    static cc::PluginRunner& runner(AppWindow& a) { return a.pluginRunner_; }
    static bool pipelineRunning(const AppWindow& a) { return a.pipeline_.running(); }
    static std::uint32_t stateFlags(AppWindow& a, const std::string& file) {
        a.publishPluginApiState();
        const cc::LoadedPlugin* lp = nullptr;
        for (const cc::LoadedPlugin& p : a.pluginHost_.plugins()) {
            if (p.loaded && cc::pluginKey(p) == file) { lp = &p; }
        }
        if (lp == nullptr) { return 0xFFFFFFFFu; }
        cc::PluginApiClient& c = a.pluginUi_.api().client(file, lp->name, lp->capabilities);
        CascadeReceiverState st{};
        st.structSize = static_cast<std::uint32_t>(sizeof(CascadeReceiverState));
        if (a.pluginUi_.api().getState(c, &st) != CASCADE_API_OK) { return 0xFFFFFFFEu; }
        // ONLY THE THREE TRANSMIT FLAGS: the word also says whether the receiver is running.
        return st.flags & (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED |
                           CASCADE_STATE_TRANSMITTING);
    }
    static cc::AppConfig saved(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cc::AppConfig& cfg) { a.applyConfig(cfg); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

Counts countsOf(const AppWindow& app) {
    Counts c;
    for (const cc::LoadedPlugin& p : Access::plugins(app)) {
        if (!p.loaded || p.name != kPluginName || p.nativeHandle == nullptr) { continue; }
#if defined(_WIN32)
        FARPROC f = ::GetProcAddress(static_cast<HMODULE>(p.nativeHandle), "tx_fixture_counts");
#else
        void* f = ::dlsym(p.nativeHandle, "tx_fixture_counts");
#endif
        if (f == nullptr) { continue; }
        reinterpret_cast<void (*)(unsigned long long*, unsigned long long*, unsigned long long*)>(
            f)(&c.actives, &c.pulls, &c.frames);
    }
    return c;
}

// Real time, a frame at a time, until `pred` or the bound.
bool until(AppWindow& app, bool pageLive, const std::function<bool()>& pred, int boundMs,
           bool ptt = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);
    while (std::chrono::steady_clock::now() < deadline) {
        Access::frame(app, pageLive, ptt);
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    Access::frame(app, pageLive, ptt);
    return pred();
}

void framesFor(AppWindow& app, bool pageLive, int ms, bool ptt = false) {
    until(app, pageLive, [] { return false; }, ms, ptt);
}

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// Every transmit line the log holds, for the failure message.
void dumpTxLog(const char* where) {
    std::printf("    [%s] the log's tx lines:\n", where);
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find("tx:") != std::string::npos) { std::printf("      %s\n", line.c_str()); }
    }
}

// A window with the fixture loaded, pinned to run, and the receiver running, so the module is
// being polled by the pipeline's DSP thread. The module's flag starts OFF.
struct Rig {
    std::unique_ptr<AppWindow> app = std::make_unique<AppWindow>();
    FakeTxRadio* radio = nullptr;
    std::string file = std::string(kFile) + kExt;
    Counts base;  // what the module had counted before this window's first call to it

    explicit Rig(bool openRadio = true) {
        cc::DiagLog::instance().resetForTest();
        Access::rescan(*app);
        bool loaded = false;
        for (const cc::LoadedPlugin& p : Access::plugins(*app)) {
            if (p.loaded && p.name == kPluginName) {
                loaded = true;
                CHECK(p.txAudio != nullptr);
                CHECK((p.capabilities & CASCADE_CAP_TX_AUDIO) != 0u);
            }
        }
        CHECK(loaded);
        base = countsOf(*app);
        Access::pin(*app, file);
        Access::setFlag(*app, false);
        Access::start(*app);
        if (openRadio) {
            auto r = std::make_unique<FakeTxRadio>();
            radio = r.get();
            Access::openRadio(*app, std::move(r));
        }
    }
    ~Rig() {
        // The radio is destroyed with the window; stop the receiver first so the DSP thread is
        // not inside the plugin when the module is unmapped.
        Access::stop(*app);
        Access::tx(*app).stop();
    }
    // The module is being asked, by the DSP thread: its active() has been called.
    bool polled() {
        const unsigned long long before = countsOf(*app).actives;
        return until(*app, true, [&] { return countsOf(*app).actives > before + 3; }, 8000);
    }
    unsigned long long pulls() const { return countsOf(*app).pulls - base.pulls; }
};

// --- the scenarios ---------------------------------------------------------------

void testRefusals() {
    std::printf("  refusals: ARM says why, and only the three lengths exist\n");
    Rig noRadio(false);
    CHECK(noRadio.polled());
    // No radio: nothing to arm. (A grant is irrelevant: the order is radio, then grant.)
    CHECK(Access::arm(*noRadio.app, 5) == cc::TxArming::Refusal::NoRadio);
    CHECK(!Access::armed(*noRadio.app));
    Access::grant(*noRadio.app, noRadio.file, true);
    CHECK(Access::arm(*noRadio.app, 5) == cc::TxArming::Refusal::NoRadio);
    CHECK(!Access::armed(*noRadio.app));

    Rig r;
    CHECK(r.polled());
    // A radio and no grant: nothing to let in.
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::NoGrant);
    CHECK(!Access::armed(*r.app));
    CHECK(!Access::refusalIsNone(*r.app));
    // Granted: any other length is refused; the three are accepted, and a good ARM clears the refusal.
    Access::grant(*r.app, r.file, true);
    CHECK(Access::refusalIsNone(*r.app));  // the grant answered it
    for (const int bad : {0, 1, 10, 30, 61, 120, -5}) {
        CHECK(Access::arm(*r.app, bad) == cc::TxArming::Refusal::BadMinutes);
        CHECK(!Access::armed(*r.app));
    }
    for (const int ok : {5, 15, 60}) {
        CHECK(Access::arm(*r.app, ok) == cc::TxArming::Refusal::None);
        CHECK(Access::armed(*r.app));
        Access::off(*r.app);
        CHECK(!Access::armed(*r.app));
    }
    CHECK(logged("tx: armed 5 min") == 1u);
    CHECK(logged("tx: armed 15 min") == 1u);
    CHECK(logged("tx: armed 60 min") == 1u);
    CHECK(logged("tx: disarmed (you pressed OFF)") == 3u);
    CHECK(r.radio->starts.load() == 0);
}

void testQuietUnlessEverythingHolds() {
    std::printf("  quiet: the module asks, and nothing is keyed or pulled until every condition holds\n");
    Rig r;
    CHECK(r.polled());
    Access::setFlag(*r.app, true);  // the module now wants the transmitter, all the time

    // 1. NOT GRANTED, NOT ARMED: asked, polled, never pulled from, never keyed. The module has
    //    ASKED (the grant would be offered on its plate), and that is all it has done.
    framesFor(*r.app, true, 700);
    CHECK(Access::runner(*r.app).txState(r.file).asked);
    CHECK(Access::runner(*r.app).txAskers().size() == 1u);
    CHECK(r.pulls() == 0ull);
    CHECK(r.radio->starts.load() == 0);
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(Access::stateFlags(*r.app, r.file) == 0u);  // no grant, no window, not keyed

    // 2. THE GRANT ALONE (the plate's key, which saves it): still not keyed.
    Access::grant(*r.app, r.file, true);
    CHECK(Access::durableGrant(*r.app, r.file));
    framesFor(*r.app, true, 700);
    CHECK(r.pulls() == 0ull);
    CHECK(r.radio->starts.load() == 0);
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(Access::stateFlags(*r.app, r.file) == CASCADE_STATE_TX_GRANTED);

    // 3. ARMED BUT THE RUNNER DOES NOT HOLD THE GRANT (a grant on the application's side only):
    //    the runner never holds the transmitter, so the module is never pulled from.
    Access::grantRunnerOnly(*r.app, "");  // an entry that matches no module: nobody is granted
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    framesFor(*r.app, true, 700);
    CHECK(r.pulls() == 0ull);
    if (r.radio->starts.load() != 0) { dumpTxLog("step 3"); }
    CHECK(r.radio->starts.load() == 0);
    CHECK(!Access::pluginKeyed(*r.app));
    Access::off(*r.app);
    Access::grant(*r.app, r.file, true);  // restores the runner's copy

    // 4. GRANTED AND ARMED BUT THE PAGE IS NOT LIVE (closed or rolled up): the window closes on
    //    the first such frame, and the module is not keyed.
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    Access::frame(*r.app, false);
    CHECK(!Access::armed(*r.app));
    CHECK(Access::whyClosed(*r.app) == "the TRANSMIT page was closed");
    framesFor(*r.app, true, 700);  // the page is back, the module still asking...
    CHECK(!Access::armed(*r.app));                 // ...and the window stays CLOSED
    CHECK(r.pulls() == 0ull);
    CHECK(r.radio->starts.load() == 0);
    CHECK(!Access::pluginKeyed(*r.app));

    // 5. GRANTED, ARMED, PAGE LIVE, BUT THE MODULE DOES NOT WANT THE KEY: quiet.
    Access::setFlag(*r.app, false);
    framesFor(*r.app, true, 300);  // the module's worker sees the flag drop (not armed: no key)
    const unsigned long long pullsBefore = r.pulls();
    const int startsBefore = r.radio->starts.load();
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    framesFor(*r.app, true, 700);
    CHECK(Access::armed(*r.app));
    CHECK(r.pulls() == pullsBefore);
    CHECK(r.radio->starts.load() == startsBefore);
    CHECK(startsBefore == 0);
    CHECK(pullsBefore == 0ull);
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(Access::stateFlags(*r.app, r.file) == (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));

    // 6. THE RADIO GOES AWAY with the window open: the window closes and nothing is keyed.
    Access::setFlag(*r.app, true);
    Access::tx(*r.app).setSink(nullptr);  // the sink removed under the application's feet
    framesFor(*r.app, true, 500);
    CHECK(!Access::armed(*r.app));
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(r.pulls() == 0ull);
    if (logged("tx: plugin") != 0u) { dumpTxLog("step 6"); }
    CHECK(logged("tx: plugin") == 0u);  // no key line was ever written
}

void testKeysAndUnkeys() {
    std::printf("  keys: granted + armed + live + the module asking = keyed; it is pulled from and told\n");
    Rig r;
    CHECK(r.polled());
    Access::grant(*r.app, r.file, true);
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    framesFor(*r.app, true, 300);
    CHECK(!Access::pluginKeyed(*r.app));       // armed, and the module does not want the key yet
    CHECK(r.pulls() == 0ull);

    Access::setFlag(*r.app, true);
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    CHECK(r.radio->starts.load() == 1);
    CHECK(Access::tx(*r.app).activeInput() == cascade::core::TxInput::Plugin);
    // The DSP thread is told it is keyed one frame later, and pulls the module's audio into the ring.
    CHECK(until(*r.app, true, [&] { return r.pulls() > 0ull; }, 5000));
    CHECK(until(*r.app, true, [&] { return Access::tx(*r.app).pluginFramesSent() > 4800u; }, 5000));
    const Counts c = countsOf(*r.app);
    std::printf("    keyed: the module was asked %llu times and pulled %llu times for %llu frames; %llu frames went out\n",
                c.actives - r.base.actives, c.pulls - r.base.pulls, c.frames - r.base.frames,
                static_cast<unsigned long long>(Access::tx(*r.app).pluginFramesSent()));
    CHECK(r.radio->samples.load() > 30000ull);  // frames x 10: the fake radio runs at 480 kS/s
    // It is told, through get_state, all three things.
    CHECK(Access::stateFlags(*r.app, r.file) ==
          (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING));
    CHECK(logged("tx: armed 5 min") == 1u);
    CHECK(logged("tx: plugin TX Audio Fixture keyed") == 1u);
    CHECK(Access::tx(*r.app).effectiveMode() == cascade::dsp::TxMode::NFM);
    CHECK(!Access::tx(*r.app).preemphasis());

    // The module lets go: after its tail the key opens, the radio is stopped, and it is told.
    Access::setFlag(*r.app, false);
    CHECK(until(*r.app, true, [&] { return !Access::pluginKeyed(*r.app); }, 5000));
    CHECK(until(*r.app, true, [&] { return !Access::transmitting(*r.app); }, 5000));
    CHECK(r.radio->stops.load() >= 1);
    CHECK(Access::armed(*r.app));  // letting go is not a fault: the window stays open
    CHECK(Access::stateFlags(*r.app, r.file) == (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));
    CHECK(logged("tx: plugin TX Audio Fixture unkeyed (the plugin let go") == 1u);
    const unsigned long long pullsAfter = r.pulls();
    framesFor(*r.app, true, 400);
    CHECK(r.pulls() == pullsAfter);  // and it is not asked for audio once it is unkeyed

    // It asks again within the same window: keyed again, no second ARM needed.
    Access::setFlag(*r.app, true);
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    CHECK(r.radio->starts.load() == 2);
    Access::setFlag(*r.app, false);
    CHECK(until(*r.app, true, [&] { return !Access::transmitting(*r.app); }, 5000));
}

// Keys the module, then runs `cut` (which must close the key), and checks the module still
// asking does not key again.
void closesScenario(const char* what, const std::function<void(Rig&)>& cut, bool windowShouldClose,
                    bool pageLiveAfter, const char* reasonFragment) {
    std::printf("  closes: %s\n", what);
    Rig r;
    CHECK(r.polled());
    Access::grant(*r.app, r.file, true);
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    Access::setFlag(*r.app, true);
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    CHECK(until(*r.app, true, [&] { return r.pulls() > 0ull; }, 5000));
    const int startsBefore = r.radio != nullptr ? r.radio->starts.load() : 0;

    cut(r);
    // ONE FRAME is all the key may survive it (the radio's fault is the exception: the transmit
    // thread has to notice it first).
    CHECK(until(*r.app, pageLiveAfter, [&] { return !Access::pluginKeyed(*r.app); }, 3000));
    CHECK(until(*r.app, pageLiveAfter, [&] { return !Access::transmitting(*r.app); }, 3000));
    if (windowShouldClose) {
        CHECK(!Access::armed(*r.app));
        const std::string why = Access::whyClosed(*r.app);
        if (why.find(reasonFragment) == std::string::npos) {
            std::printf("    the window closed because: \"%s\" (wanted \"%s\")\n", why.c_str(),
                        reasonFragment);
        }
        CHECK(why.find(reasonFragment) != std::string::npos);
    }
    // The module goes on asking. Nothing keys again - with the page live, the radio there and
    // the module willing - until a person arms a window.
    const unsigned long long pulls = r.pulls();
    framesFor(*r.app, true, 700);
    CHECK(!Access::pluginKeyed(*r.app));
    if (r.radio != nullptr && windowShouldClose) {
        CHECK(r.radio->starts.load() == startsBefore);
    }
    CHECK(r.pulls() == pulls);
    CHECK(logged("tx: plugin TX Audio Fixture unkeyed") >= 1u);
    CHECK(Access::stateFlags(*r.app, r.file) != 0xFFFFFFFFu);
    CHECK((Access::stateFlags(*r.app, r.file) & CASCADE_STATE_TRANSMITTING) == 0u);
}

void testCloses() {
    closesScenario("the PAGE closing unkeys on that frame and closes the window",
                   [](Rig& r) { Access::frame(*r.app, false); }, true, true,
                   "the TRANSMIT page was closed");
    closesScenario("OFF unkeys and closes the window",
                   [](Rig& r) { Access::off(*r.app); }, true, true, "you pressed OFF");
    closesScenario("the grant being WITHDRAWN unkeys and closes the window",
                   [](Rig& r) { Access::grant(*r.app, r.file, false); }, true, true,
                   "no plugin holds the TRANSMIT grant");
    closesScenario("the radio being CLOSED unkeys and closes the window",
                   [](Rig& r) {
                       Access::closeRadio(*r.app);
                       r.radio = nullptr;  // destroyed with the sink
                   },
                   true, true, "the transmit radio was closed");
    closesScenario("a radio FAULT unkeys, closes the window, and is not retried",
                   [](Rig& r) { r.radio->fault(); }, true, true, "");
    closesScenario("the window's own TIME unkeys it",
                   [](Rig& r) { Access::armWithTimeLeft(*r.app, 500); }, true, true,
                   "the arming window ran out");
    closesScenario("an application restart (a config load) closes the window",
                   [](Rig& r) { Access::restore(*r.app, Access::saved(*r.app)); }, true, true,
                   "a configuration was loaded");
}

void testCap() {
    std::printf("  cap: a minute of continuous key (the latch's own number) ends it, and the window with it\n");
    Rig r;
    CHECK(r.polled());
    Access::grant(*r.app, r.file, true);
    Access::tx(*r.app).setLatchTimeoutForTest(std::chrono::milliseconds(500));
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    Access::setFlag(*r.app, true);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    // The module holds active() high for ever. The key is cut at the cap, not before.
    CHECK(until(*r.app, true, [&] { return !Access::pluginKeyed(*r.app); }, 5000));
    const double heldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    keyed for about %.0f ms against a 500 ms cap\n", heldMs);
    CHECK(heldMs > 400.0);
    CHECK(heldMs < 3000.0);
    CHECK(!Access::armed(*r.app));                     // the window closed with it
    CHECK(Access::whyClosed(*r.app).find("held the key") != std::string::npos);
    CHECK(logged("tx: plugin TX Audio Fixture unkeyed (a plugin held the key") == 1u);
    // Still asking; still page live; still the radio: NOT keyed again.
    const int starts = r.radio->starts.load();
    framesFor(*r.app, true, 800);
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(r.radio->starts.load() == starts);
    // The operator arms again: a fresh decision, and it keys.
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    CHECK(r.radio->starts.load() == starts + 1);
}

void testHandPrecedence() {
    std::printf("  hand: a PTT held while the plugin asks is the hand's transmission, not the plugin's\n");
    Rig r;
    CHECK(r.polled());
    Access::grant(*r.app, r.file, true);
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    Access::setFlag(*r.app, true);
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    CHECK(until(*r.app, true, [&] { return r.pulls() > 0ull; }, 5000));

    // The operator takes the PTT.
    framesFor(*r.app, true, 300, true);
    CHECK(Access::transmitting(*r.app));
    CHECK(!Access::pluginKeyed(*r.app));
    CHECK(Access::tx(*r.app).activeInput() == cascade::core::TxInput::Tone);  // the page's input
    CHECK(Access::tx(*r.app).preemphasis());
    // The runner is told it is no longer keyed for the plugin: it is not pulled from.
    const unsigned long long pulls = r.pulls();
    framesFor(*r.app, true, 400, true);
    CHECK(r.pulls() == pulls);
    // The PTT released: the plugin is still wanted, the window still open: keyed for it again.
    CHECK(until(*r.app, true, [&] { return Access::pluginKeyed(*r.app); }, 5000));
    Access::setFlag(*r.app, false);
    CHECK(until(*r.app, true, [&] { return !Access::transmitting(*r.app); }, 5000));
}

void testConfig() {
    std::printf("  config: the grant is saved and loaded; the arming window is neither\n");
    Rig r;
    CHECK(r.polled());
    Access::grant(*r.app, r.file, true);
    CHECK(Access::arm(*r.app, 15) == cc::TxArming::Refusal::None);
    CHECK(Access::armed(*r.app));

    const cc::AppConfig cfg = Access::saved(*r.app);
    CHECK(cfg.pluginTransmitAllowed.size() == 1u && cfg.pluginTransmitAllowed[0] == r.file);

    // The file on disk: the grant is in it, and nothing that names the window.
    const std::string path = (g_scratch / "armed_config.json").string();
    std::string err;
    CHECK(cc::ConfigStore::save(path, cfg, err));
    std::string text;
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            char buf[4096];
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
            std::fclose(f);
        }
    }
    CHECK(text.find("pluginTransmitAllowed") != std::string::npos);
    CHECK(text.find(r.file) != std::string::npos);
    std::string lower = text;
    for (char& ch : lower) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
    CHECK(lower.find("arm") == std::string::npos);        // not an "armed", an "arming", an "arm..."
    CHECK(lower.find("transmitptt") == std::string::npos);
    CHECK(lower.find("transmitlatch") == std::string::npos);

    // A new window loading that file has the grant and a CLOSED window.
    {
        Rig again;
        cc::AppConfig loaded;
        CHECK(cc::ConfigStore::load(path, loaded, err));
        Access::restore(*again.app, loaded);
        CHECK(Access::durableGrant(*again.app, again.file));
        CHECK(!Access::armed(*again.app));
        again.polled();
        Access::setFlag(*again.app, true);
        framesFor(*again.app, true, 700);
        CHECK(!Access::pluginKeyed(*again.app));   // a restored grant is not a restored authorisation
        CHECK(again.radio->starts.load() == 0);
        CHECK(again.pulls() == 0ull);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_tx_audio_app <tx_audio_fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    isolate();

    testRefusals();
    testQuietUnlessEverythingHolds();
    testKeysAndUnkeys();
    testCloses();
    testCap();
    testHandPrecedence();
    testConfig();

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_tx_audio_app");
}
