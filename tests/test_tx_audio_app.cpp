// test_tx_audio_app.cpp - a plugin transmits, or does not, through the REAL AppWindow, the real
// plugin runner and a REAL plugin module (tests/fixtures/tx_audio_plugin.cpp).
//
// CONVERTED (stub campaign B13, TX-PLUGIN; master's test unedited except as the reasons below say). It was master's own test of master's AppWindow: it reached the
// transmitter, the arming window, the grant and the plugin runner through `a.transmitter_`, `a.txArming_`, `a.armTransmitPlugins`, `a.applyPluginTransmitKey`,
// `a.afterTransmitTick` ... - members this branch's window does not have, because a plugin's key is the ENGINE's (src/engine/engine_tx_plugin.cpp): the grant, the arming
// window and the decision are decided on the engine's pass, so a window that stops cannot leave a plugin able to key. The house pattern (tests/test_programme_audio_app.cpp):
// th::engine(a) through AppWindowTestAccess, tests/pass_seam.hpp for every call that must run on the pass thread - so the test is registered again as _stepped, and
// the same assertions run against the pass on its own thread - and the PAGE is a stand-in that sends exactly the commands the real page sends (tx_front_end.hpp's
// FrontEnd for the key, gui::txPluginArmCommands for the window), because there is no ImGui frame here.
//   * frame() is the page's frame: the heartbeat, the key's commands (PTT), the arming window's commands (ARM, "still here", OFF), then ONE pass of the engine - whose key
//     drive is what master's frame() called by hand (applyPluginTransmitKey, Transmitter::tick, afterTransmitTick); tests/test_transmit_page.cpp section 10 pins that order;
//   * arm() / off() / grant() are the table's ops (FOXAPP_OP_TX_PLUGIN_ARM, FOXAPP_OP_PLUGIN_TX_GRANT), answered as master's were: the refusal is the result's applied[0];
//   * the arming window and the grant are read from the engine (txArming_, pluginTransmitAllowed_), the config as the window reads it (publishConfig, currentConfig).
// DELIBERATE DIFFERENCES, each with its reason:
//   * quiet, step 4: master armed and closed the page with no live frame between, so a module that was asking never saw an armed, live window. Here pressing ARM IS a live
//     frame (the lease starts live), and a module that asks at that instant may key in the pass that arms it; the page closing is the next frame. The step arms with the module
//     NOT asking and lets it ask only after the page has closed - the same claim (a window closed by the page going is not reopened by the page returning), made without the race;
//   * refusals: a length that is not 5, 15 or 60 is refused at SUBMIT (OUT_OF_RANGE: 0 is OFF, -1 is "the page is live"), where master's armTransmitPlugins returned BadMinutes;
//     the case is the same and the answer is mapped to it;
//   * closes, "the radio being CLOSED": the sentence is the engine's closeTransmitRadio, "the transmit radio was closed", as master's (the table's TX_CLOSE says "the
//     transmitter was closed" first; tests/test_engine_api_txplugin.cpp holds that path).
// The same claims, and the ones master could not make (the revoke within one pass, a REMOTE session never granted, the plugin dormant or unloaded while keyed, the window's
// lease), are in tests/test_engine_api_txplugin.cpp, against the engine with no window at all.
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
// THE FRAME. The engine's key drive runs, in this order, the hands' levels -> txPluginStep (master's applyPluginTransmitKey) -> Transmitter::tick ->
// txPluginAfterTick (master's afterTransmitTick), and tests/test_transmit_page.cpp reads fox_engine_api.cpp to pin that order. frame() below sends what the page sends and
// runs one pass, so a scenario is a sequence of real frames with the page live or not, the PTT held or not, and a few milliseconds of real time between them (the DSP
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
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_run.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/transmit_page.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"

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
    static cascade::engine::Engine& eng(AppWindow& a) { return th::engine(a); }
    static void rescan(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).rescanPlugins(); }); }
    static void start(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).startReceiver(); }); }
    static void stop(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).stopReceiver("test"); }); }
    static void pin(AppWindow& a, const std::string& file) {
        passseam::on(eng(a), [&] { eng(a).setPluginRun(file, cc::PluginRun::Always); });
    }
    static const std::vector<cc::LoadedPlugin>& plugins(AppWindow& a) { return eng(a).pluginHost_.plugins(); }
    static void setFlag(AppWindow& a, bool on) {
        eng(a).pluginApi_->settingsUiSet(kPluginName, "send", on ? "1" : "0");
    }
    static void openRadio(AppWindow& a, std::unique_ptr<cascade::source::IqSink> sink) {
        passseam::on(eng(a), [&] {
            eng(a).transmitter_.setSink(std::move(sink));
            eng(a).transmitter_.setMode(cascade::dsp::TxMode::NFM);
            eng(a).transmitter_.setInput(cascade::core::TxInput::Tone);
            eng(a).transmitter_.setDataFlat(true);
        });
    }
    static void closeRadio(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).closeTransmitRadio(); }); }

    // --- THE PAGE: a LOCAL session on the engine, and the commands the real page sends --------------------------------------------------
    struct Desk {
        explicit Desk(cascade::engine::Engine& e) : front(e, "the TRANSMIT page (test)") {}
        tx_test::FrontEnd front;                 // the heartbeat, the state, the key's commands (gui::txKeyCommands)
        cascade::gui::TxPluginArmMemory mem;     // between frames (gui::txPluginArmCommands)
    };
    static std::map<const AppWindow*, Desk*>& desks() {
        static std::map<const AppWindow*, Desk*> m;
        return m;
    }
    static void attach(AppWindow& a, Desk* d) { desks()[&a] = d; }
    static void detach(AppWindow& a) { desks().erase(&a); }
    static Desk& desk(AppWindow& a) { return *desks().at(&a); }
    static std::string fact(AppWindow& a, const char* key) {
        char buf[4096];
        const int32_t n = desk(a).front.api->get_setting(desk(a).front.s, (std::string("fact.") + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string();
    }
    // One command from the page's session, applied by the engine's passes, answered.
    static FoxCommandResult send(AppWindow& a, const FoxCommand& c, std::int32_t* submitStatus = nullptr) {
        Desk& d = desk(a);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = d.front.api->submit(d.front.s, &c, 1, &sr);
        const int32_t st = n < 0 ? n : sr.status;
        if (submitStatus != nullptr) { *submitStatus = st; }
        FoxCommandResult out{};
        out.structSize = sizeof(out);
        out.status = st;
        if (st != FOXAPI_OK) { return out; }
        for (int pass = 0; pass < 30; ++pass) {
            pumpOne(a);
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = d.front.api->poll_results(d.front.s, r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) { return r[i]; }
            }
        }
        out.status = FOXAPI_FAILED;
        return out;
    }
    // ONE PASS of the engine: its front part (the queues, the key drive, the plugins' phase, the publish) - what a frame's look is made from.
    static void pumpOne(AppWindow& a) {
        cascade::engine::Engine& e = eng(a);
        passseam::pass(e, [&] {
            e.pumpFrameBegin();
            e.pumpRemoteControls();
            e.pumpPlugins();
            (void)e.pumpPublish();
        });
    }
    static cc::TxArming::Refusal refusalOf(double applied0) {
        switch (static_cast<int>(applied0)) {
            case 1: return cc::TxArming::Refusal::NoRadio;
            case 2: return cc::TxArming::Refusal::NoGrant;
            case 3: return cc::TxArming::Refusal::BadMinutes;
            default: return cc::TxArming::Refusal::None;
        }
    }
    static cc::TxArming::Refusal arm(AppWindow& a, int minutes) {
        // Only 5, 15 and 60 are keys on the page; 0 is the OFF key and -1 is "the page is live", so neither is an ARM. A length the table refuses at submit is BadMinutes.
        if (!cc::TxArming::validMinutes(minutes) && (minutes == 0 || minutes == -1)) { return cc::TxArming::Refusal::BadMinutes; }
        std::int32_t st = FOXAPI_OK;
        const FoxCommandResult r = send(a, cc::cmd::makeInt(FOXAPP_OP_TX_PLUGIN_ARM, minutes, 0), &st);
        if (st == FOXAPI_OUT_OF_RANGE || r.status == FOXAPI_OUT_OF_RANGE) { return cc::TxArming::Refusal::BadMinutes; }
        return r.status == FOXAPI_OK ? cc::TxArming::Refusal::None : refusalOf(r.applied[0]);
    }
    static void off(AppWindow& a) { (void)send(a, cc::cmd::makeInt(FOXAPP_OP_TX_PLUGIN_ARM, 0, 0)); }
    static bool armed(AppWindow& a) { return passseam::read<bool>(eng(a), [&] { return eng(a).txArming_.armed(); }); }
    static std::string whyClosed(AppWindow& a) { return passseam::read<std::string>(eng(a), [&] { return eng(a).txArming_.lastReason(); }); }
    // A window that really was armed, but whose time is nearly up: `msLeft` milliseconds.
    static void armWithTimeLeft(AppWindow& a, int msLeft) {
        passseam::on(eng(a), [&] {
            eng(a).txArming_.arm(5, true, true, cc::TxArming::Clock::now() - std::chrono::minutes(5) + std::chrono::milliseconds(msLeft));
        });
    }
    static void grant(AppWindow& a, const std::string& file, bool on) {
        (void)send(a, cc::cmd::makeText(FOXAPP_OP_PLUGIN_TX_GRANT, file, on ? 1 : 0).c);
    }
    static void grantRunnerOnly(AppWindow& a, const std::string& file) {
        passseam::on(eng(a), [&] { eng(a).pluginRunner_.setTxGranted({file}); });
    }
    static bool durableGrant(AppWindow& a, const std::string& file) {
        return passseam::read<bool>(eng(a), [&] {
            for (const std::string& k : eng(a).pluginTransmitAllowed_) {
                if (k == file) { return true; }
            }
            return false;
        });
    }
    static bool refusalIsNone(AppWindow& a) {
        return passseam::read<bool>(eng(a), [&] { return eng(a).txArmRefusal_ == cc::TxArming::Refusal::None; });
    }

    // ONE FRAME of the page: the heartbeat, what the page sends (the key's commands, the arming window's), then one pass of the engine.
    static void frame(AppWindow& a, bool pageLive, bool pttHeld = false) {
        Desk& d = desk(a);
        d.front.beat();
        cascade::gui::TxKeyFrame kf;
        kf.pageLive = pageLive;
        kf.pttHeld = pttHeld;
        kf.latchedShown = (d.front.st.flags & FOXAPI_RX_TX_LATCHED) != 0u;
        kf.latchMine = kf.latchedShown && (d.front.st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
        cascade::gui::TxPluginArmFrame af;
        af.pageLive = pageLive;
        af.armedShown = fact(a, "tx.plugin.armed") == "1";
        af.nowS = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        FoxCommand batch[cascade::gui::kTxKeyMaxCommands + cascade::gui::kTxPluginArmMaxCommands];
        std::uint32_t n = cascade::gui::txKeyCommands(kf, d.front.mem, batch);
        n += cascade::gui::txPluginArmCommands(af, d.mem, batch + n);
        if (n > 0) { d.front.api->submit(d.front.s, batch, n, nullptr); }
        pumpOne(a);
        d.front.drain();
        FoxReceiverState now{};
        now.structSize = sizeof(now);
        if (d.front.api->read_state(d.front.s, &now) == FOXAPI_OK) { d.front.st = now; }
    }

    static bool pluginKeyed(AppWindow& a) { return eng(a).transmitter_.pluginKeyed(); }
    static bool transmitting(AppWindow& a) { return eng(a).transmitter_.transmitting(); }
    static cascade::core::Transmitter& tx(AppWindow& a) { return eng(a).transmitter_; }
    static cc::PluginRunner& runner(AppWindow& a) { return eng(a).pluginRunner_; }
    static std::uint32_t stateFlags(AppWindow& a, const std::string& file) {
        const cc::LoadedPlugin* lp = nullptr;
        for (const cc::LoadedPlugin& p : eng(a).pluginHost_.plugins()) {
            if (p.loaded && cc::pluginKey(p) == file) { lp = &p; }
        }
        if (lp == nullptr) { return 0xFFFFFFFFu; }
        cc::PluginApiClient& c = eng(a).pluginApi_->client(file, lp->name, lp->capabilities);
        CascadeReceiverState st{};
        st.structSize = static_cast<std::uint32_t>(sizeof(CascadeReceiverState));
        if (eng(a).pluginApi_->getState(c, &st) != CASCADE_API_OK) { return 0xFFFFFFFEu; }
        // ONLY THE THREE TRANSMIT FLAGS: the word also says whether the receiver is running.
        return st.flags & (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING);
    }
    static cc::AppConfig saved(AppWindow& a) {
        passseam::on(eng(a), [&] { eng(a).publishConfig(); });
        return a.currentConfig();
    }
    static void restore(AppWindow& a, const cc::AppConfig& cfg) { passseam::on(eng(a), [&] { eng(a).applyConfig(cfg); }); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

Counts countsOf(AppWindow& app) {
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
    std::unique_ptr<th::TestWindow> app = std::make_unique<th::TestWindow>();
    FakeTxRadio* radio = nullptr;
    std::string file = std::string(kFile) + kExt;
    Counts base;  // what the module had counted before this window's first call to it
    std::unique_ptr<Access::Desk> desk;  // the TRANSMIT page, as a LOCAL session on the engine

    explicit Rig(bool openRadio = true) {
        cc::DiagLog::instance().resetForTest();
        desk = std::make_unique<Access::Desk>(Access::eng(*app));
        Access::attach(*app, desk.get());
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
        Access::detach(*app);
        desk.reset();
        passseam::on(Access::eng(*app), [&] { Access::tx(*app).stop(); });
    }
    // The module is being asked, by the DSP thread: its active() has been called.
    bool polled() {
        const unsigned long long before = countsOf(*app).actives;
        return until(*app, true, [&] { return countsOf(*app).actives > before + 3; }, 8000);
    }
    unsigned long long pulls() { return countsOf(*app).pulls - base.pulls; }
};

// --- the scenarios ---------------------------------------------------------------

void testRefusals() {
    std::printf("  refusals: ARM says why, and only the three lengths exist\n");
    {
        // (One window at a time - converted, stub campaign B13: the fixture module's host pointer is the process's, the last one attached, so a second live window
        // is a worker thread of the first reading the second's host; master had the two alive together.)
        Rig noRadio(false);
        CHECK(noRadio.polled());
        // No radio: nothing to arm. (A grant is irrelevant: the order is radio, then grant.)
        CHECK(Access::arm(*noRadio.app, 5) == cc::TxArming::Refusal::NoRadio);
        CHECK(!Access::armed(*noRadio.app));
        Access::grant(*noRadio.app, noRadio.file, true);
        CHECK(Access::arm(*noRadio.app, 5) == cc::TxArming::Refusal::NoRadio);
        CHECK(!Access::armed(*noRadio.app));
    }

    Rig r;
    CHECK(r.polled());
    // A radio and no grant: nothing to let in.
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::NoGrant);
    CHECK(!Access::armed(*r.app));
    CHECK(!Access::refusalIsNone(*r.app));
    // Granted: any other length is refused; the three are accepted, and a good ARM clears the refusal.
    Access::grant(*r.app, r.file, true);
    CHECK(Access::refusalIsNone(*r.app));  // the grant answered it
    for (const int bad : {0, 1, 10, 30, 61, 120, -5, 65}) {
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
    //    the first such frame, and the module is not keyed. (Pressing ARM is itself the page being
    //    on screen, so the module is NOT asking while the window is armed here, and asks only after
    //    the page has closed - see the header.)
    Access::setFlag(*r.app, false);
    framesFor(*r.app, true, 300);
    CHECK(Access::arm(*r.app, 5) == cc::TxArming::Refusal::None);
    Access::frame(*r.app, false);
    Access::frame(*r.app, false);
    CHECK(!Access::armed(*r.app));
    CHECK(Access::whyClosed(*r.app) == "the TRANSMIT page was closed");
    Access::setFlag(*r.app, true);
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
    // ONE PASS is all the key may survive it (the radio's fault is the exception: the transmit
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
    auto first = std::make_unique<Rig>();
    Rig& r = *first;
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

    // A new window loading that file has the grant and a CLOSED window. (The first window is closed first - converted, stub campaign B13: the fixture module reads its
    // "send" setting through a process-global host pointer, the last instance attached, so a second live window's flag would reach the first window's module; in master
    // nothing ran the first window's frames, and here a free-running pass would.)
    first.reset();
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
