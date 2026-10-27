// test_engine_api_txplugin.cpp - A PLUGIN MAY KEY THE TRANSMITTER, in the engine (stub campaign B13, TX-PLUGIN; master 0.99.74's CASCADE_CAP_TX_AUDIO), held against the
// real engine through the C table and the engine's own passes, with NO WINDOW ANYWHERE (the page is a stand-in that sends exactly the commands the real page sends:
// gui::txPluginArmCommands). SAFETY-CRITICAL, and every rule below is pinned by a case that was shown RED against a mutant of the code that holds it (docs/engine-stage4.md
// 6.22, B13). THE RADIO IS A LEDGER (FakeTxRadio, below): nothing in this file keys a real transmitter, and none of it could - a sink that records what it was given is the
// only one the engine is ever handed here.
//
// THE WORLD: the real module tests/fixtures/tx_audio_plugin.cpp loaded the way a downloaded one is (a decoder that declares CASCADE_CAP_TX_AUDIO and asks for the
// transmitter whenever its "send" setting is on, counting every call the host makes to it, so "it was never pulled from" is read off the module and not off what the engine
// says it did), the engine's passes driven by hand (Engine::controlPass) with the test thread standing in for the DSP thread (PluginRunner::pollTxAudio, once a pass), and
// the Transmitter's own TX thread real.
//
//   A  THE SURFACE (LOCAL ONLY): the three ops are known, are on neither internal profile, and are DENIED to a REMOTE session, to the web server's session and to a LOCAL
//      session without the TRANSMIT grant - and a REMOTE sender cannot reach the engine's grant or window even if the table's three submit-time locks were gone (the fourth
//      lock, where it is applied); FOXAPI_OP_PLUGIN_GRANT (which IS on the web profile) still has no transmit kind;
//   B  THE REFUSALS: ARM with no radio, with no grant, and only the three lengths; each says why and publishes it; what answers a refusal clears it;
//   C  QUIET UNLESS EVERYTHING HOLDS: asked, polled, never pulled from and never keyed: not granted, granted and not armed, armed and not asked, a radio gone;
//   D  KEYS: granted + armed + live + asked = keyed, pulled from, told (get_state), logged; it lets go; it asks again in the same window;
//   E  REVOKE: a revoked grant, or a closed window, unkeys WITHIN ONE PASS; the engine's own grant check stands without the runner's; a revoked grant that was the last one
//      closes the window, and re-granting alone does not re-key;
//   F  EVERY CLOSING REASON unkeys, and the module still asking does NOT key again until a person arms a new window: the page closed, OFF, the grant withdrawn, the radio
//      closed, the radio changed, a radio fault, the cap, the window's own time, a configuration loaded, the receiver stopped, the window that armed it gone, silent, or no
//      longer saying the page is on screen, the engine stopping;
//   G  THE HARDENING: a keyed plugin going DORMANT, STOPPED, UNLOADED or REBUILT unkeys at once and closes the window, and woken again it does not re-key;
//   H  THE HAND WINS: a PTT held while the plugin asks is the hand's transmission, not the plugin's, and the plugin is keyed again when the hand lets go;
//   I  THE DEAD-MAN: a control side that stops ticking lets go of a plugin's key on the TX thread, and the plugin is not re-keyed;
//   J  THE CONFIG: the grant is saved and restored; the window is neither, and a configuration loaded mid-session closes it;
//   K  THE FACTS: what the page and the plate are drawn from, LOCAL only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "gui/transmit_page.hpp"
#include "source/tx_sink.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;
using cascade::engine::Engine;

namespace {

// --- the radio: a ledger -----------------------------------------------------------------------------------------------------------

class FakeTxRadio final : public cascade::source::IqSink {
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
    const char* name() const override { return "ledger transmit radio"; }
    const char* lastError() const override { return faulted_ ? "the ledger radio faulted" : ""; }
    void fault() { faulted_ = true; }
    ~FakeTxRadio() override;

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<unsigned long long> samples{0};

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> faulted_{false};
};

FakeTxRadio* g_lastRadio = nullptr;   // the radio the engine was last handed (the engine owns it; the test only reads the ledger)
FakeTxRadio::~FakeTxRadio() {
    if (g_lastRadio == this) { g_lastRadio = nullptr; }
}
std::unique_ptr<cascade::source::IqSink> makeLedger() {
    auto r = std::make_unique<FakeTxRadio>();
    g_lastRadio = r.get();
    return r;
}

}  // namespace

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
struct AppWindowTestAccess {
    static void setPluginDir(std::string (*dir)()) { Engine::testHooks_.pluginDir = dir; }
    static void installWindow(Engine& e, cc::PluginUi& ui) { e.setPluginUiApplier([&ui](const cc::PluginDirective& d) { ui.apply(d); }); }
    static void removeWindow(Engine& e) { e.setPluginUiApplier({}); }
    static void useLedger(Engine& e) { e.txSinkForTest_ = &makeLedger; }
    static cc::PluginRunner& runner(Engine& e) { return e.pluginRunner_; }
    static cc::PluginApiCore& core(Engine& e) { return *e.pluginApi_; }
    static cc::Transmitter& tx(Engine& e) { return e.transmitter_; }
    static const std::vector<cc::LoadedPlugin>& plugins(Engine& e) { return e.pluginHost_.plugins(); }
    static bool armed(Engine& e) { return e.txArming_.armed(); }
    static int armedMinutes(Engine& e) { return e.txArming_.minutes(); }
    static std::string whyClosed(Engine& e) { return e.txArming_.lastReason(); }
    static std::vector<std::string>& granted(Engine& e) { return e.pluginTransmitAllowed_; }
    static void setRemote(Engine& e, bool on) { e.commandFromRemote_ = on; }
    static FoxCommandResult applyDirect(Engine& e, const FoxCommand& c) { return e.applyCommand(c, std::string()); }
    static FoxCommandResult armAs(Engine& e, std::uint64_t session, bool remote, const FoxCommand& c) { return e.txPluginArmOp(session, remote, true, c); }
    static void armWithTimeLeft(Engine& e, int msLeft) {
        e.txArming_.arm(5, true, true, cc::TxArming::Clock::now() - std::chrono::minutes(5) + std::chrono::milliseconds(msLeft));
    }
    static cc::AppConfig readConfig(Engine& e) {
        cc::AppConfig c;
        e.fillConfig(c);
        return c;
    }
    static void restoreConfig(Engine& e, const cc::AppConfig& c) { e.applyConfig(c); }
    static void rescan(Engine& e) { e.rescanPlugins(); }
    static void refreshRunner(Engine& e) { e.refreshPluginRunner(); }
    static void engineStopping(Engine& e) { e.api_->engineStopping(); }
    // stopControlThread's flag (set before it lets go of the key), set by hand: a pass in flight cannot key again.
    static void setStopping(Engine& e, bool on) { e.txPluginStopping_.store(on); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kPageGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE;

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif
const std::string kFile = std::string("tx-audio-fixture-1.0.0") + kExt;   // the module file name, as the grant names it
const char* const kName = "TX Audio Fixture";

fs::path g_scratch;
fs::path g_plugins;
std::string g_fixture;
std::string pluginDirText() { return g_plugins.string(); }

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string();
}

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

void dumpTxLog(const char* where) {
    std::printf("    [%s] the log's tx lines:\n", where);
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find("tx:") != std::string::npos) { std::printf("      %s\n", line.c_str()); }
    }
}

FoxCommand op(std::uint32_t code, long long i0 = 0, long long i1 = 0, const char* text = nullptr) {
    FoxCommand c = testing::cmd(code, 0.0, i0, text);
    c.ival[1] = i1;
    return c;
}

struct Counts {
    unsigned long long actives = 0;
    unsigned long long pulls = 0;
    unsigned long long frames = 0;
};

Counts countsOf(Engine& e) {
    Counts c;
    for (const cc::LoadedPlugin& p : Access::plugins(e)) {
        if (!p.loaded || p.name != kName || p.nativeHandle == nullptr) { continue; }
#if defined(_WIN32)
        FARPROC f = ::GetProcAddress(static_cast<HMODULE>(p.nativeHandle), "tx_fixture_counts");
#else
        void* f = ::dlsym(p.nativeHandle, "tx_fixture_counts");
#endif
        if (f == nullptr) { continue; }
        reinterpret_cast<void (*)(unsigned long long*, unsigned long long*, unsigned long long*)>(f)(&c.actives, &c.pulls, &c.frames);
    }
    return c;
}

// A world: an engine with the fixture fitted, a ledger radio offered to TX_OPEN, and a LOCAL session standing in for the TRANSMIT page.
struct Rig {
    Engine eng;
    std::unique_ptr<cc::PluginUi> ui;   // a "window" (only for the rigs that need dormancy: a window-less host treats AUTO as pinned)
    std::unique_ptr<testing::Session> page;
    std::unique_ptr<testing::Session> watch;   // another LOCAL session that only reads facts (the page's own may be closed)
    cascade::gui::TxPluginArmMemory mem;
    Counts base;
    bool beating = true;

    explicit Rig(bool windowed = false, bool openRadio = true) {
        cc::DiagLog::instance().resetForTest();
        g_lastRadio = nullptr;
        Access::setPluginDir(&pluginDirText);
        if (windowed) {
            ui = std::make_unique<cc::PluginUi>(eng.pluginApiShared());
            eng.configureApi(kHooks);
            Access::installWindow(eng, *ui);
        } else {
            eng.configureApi(kHooks);
        }
        eng.initialise();
        eng.controlPass();
        Access::useLedger(eng);
        page = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kPageGrants);
        CHECK(page->ok());
        watch = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        CHECK(watch->ok());
        passes(3);
        base = countsOf(eng);
        if (openRadio) { CHECK_EQ(send(op(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).status(), FOXAPI_OK); }
    }
    ~Rig() {
        if (ui) { Access::removeWindow(eng); }
        Access::setPluginDir(nullptr);
    }

    // The test thread is the DSP thread's stand-in: one audio block's poll of the module per pass.
    void passes(int n) {
        for (int i = 0; i < n; ++i) {
            Access::runner(eng).pollTxAudio();
            eng.controlPass();
        }
    }
    void drain() {
        FoxCommandResult r[16];
        for (;;) {
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(page->raw(), r, 16);
            if (got < 16) { break; }
        }
    }
    // One command from the page's session, and passes until it lands.
    testing::Outcome send(const FoxCommand& c, testing::Session* as = nullptr) {
        testing::Session& s = as != nullptr ? *as : *page;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 20 && !o.landed; ++pass) {
            if (&s == page.get() && beating) { s.beat(); }
            passes(1);
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s.raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    // Submits and runs exactly ONE pass: what "within one pass" is measured against.
    void submitThenOnePass(const FoxCommand& c) {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        CHECK_EQ(api()->submit(page->raw(), &c, 1, &sr), 1);
        CHECK_EQ(sr.status, FOXAPI_OK);
        if (beating) { page->beat(); }
        passes(1);
    }
    double nowS() const { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::string rf(const std::string& key) { return fact(*watch, key); }
    // One frame of the page: the heartbeat, the page's arming commands (gui::txPluginArmCommands, the function the real page sends by), the DSP stand-in, one pass.
    void frame(bool pageLive, int pressed = -2) {
        if (page->raw() != nullptr) {
            if (beating) { page->beat(); }
            cascade::gui::TxPluginArmFrame f;
            f.pageLive = pageLive;
            f.pressed = pressed;
            f.armedShown = rf("tx.plugin.armed") == "1";
            f.nowS = nowS();
            FoxCommand c[cascade::gui::kTxPluginArmMaxCommands];
            const std::uint32_t n = cascade::gui::txPluginArmCommands(f, mem, c);
            if (n > 0) { (void)api()->submit(page->raw(), c, n, nullptr); }
        }
        passes(1);
        if (page->raw() != nullptr) { drain(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool until(const std::function<bool()>& pred, int boundMs, bool pageLive = true) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);
        while (std::chrono::steady_clock::now() < deadline) {
            frame(pageLive);
            if (pred()) { return true; }
        }
        frame(pageLive);
        return pred();
    }
    void framesFor(int ms, bool pageLive = true) { (void)until([] { return false; }, ms, pageLive); }

    // --- what the module and the engine say --------------------------------------------------------------------------------------------
    void setFlag(bool on) { Access::core(eng).settingsUiSet(kName, "send", on ? "1" : "0"); }
    bool keyed() { return Access::tx(eng).pluginKeyed(); }
    bool requested() { return Access::tx(eng).pluginKeyRequested(); }
    bool onAir() { return Access::tx(eng).transmitting(); }
    bool armed() { return Access::armed(eng); }
    unsigned long long pulls() { return countsOf(eng).pulls - base.pulls; }
    FakeTxRadio* radio() { return g_lastRadio; }
    // The module is being asked by the DSP stand-in: its active() has been called a few times.
    bool polled() {
        const unsigned long long before = countsOf(eng).actives;
        return until([&] { return countsOf(eng).actives > before + 3; }, 8000);
    }
    bool grant(bool on = true) { return send(op(FOXAPP_OP_PLUGIN_TX_GRANT, on ? 1 : 0, 0, kFile.c_str())).status() == FOXAPI_OK; }
    // ARM n minutes, as a key press on the page.
    void press(int minutes) { frame(true, minutes); }
    // The module granted, a window armed for 5 minutes and the module asking: keyed. The state most scenarios start from.
    bool key() {
        if (!polled()) { return false; }
        if (!grant()) { return false; }
        press(5);
        if (!armed()) { return false; }
        setFlag(true);
        if (!until([&] { return keyed(); }, 5000)) { return false; }
        return until([&] { return pulls() > 0ull; }, 5000);
    }
    std::uint32_t stateFlags() {
        cc::PluginApiClient* cl = nullptr;
        for (const cc::LoadedPlugin& p : Access::plugins(eng)) {
            if (p.loaded && cc::pluginKey(p) == kFile) { cl = &Access::core(eng).client(kFile, p.name, p.capabilities); }
        }
        if (cl == nullptr) { return 0xFFFFFFFFu; }
        CascadeReceiverState st{};
        st.structSize = static_cast<std::uint32_t>(sizeof(CascadeReceiverState));
        if (Access::core(eng).getState(*cl, &st) != CASCADE_API_OK) { return 0xFFFFFFFEu; }
        return st.flags & (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING);
    }
};

// ---- A: the surface --------------------------------------------------------------------------------------------------------------------

void testSurface() {
    std::printf("  A: the three ops are LOCAL only, and nothing else grants TRANSMIT\n");
    for (const std::uint32_t o : {FOXAPP_OP_TX_PLUGIN_ARM, FOXAPP_OP_TX_DATA_FLAT, FOXAPP_OP_PLUGIN_TX_GRANT}) {
        CHECK(cc::cmd::isKnownOp(o));
        CHECK(cc::cmd::isAppOp(o));
        for (const cascade::engine::InternalKind k : {cascade::engine::InternalKind::Web, cascade::engine::InternalKind::Cat}) {
            for (const std::uint32_t p : cascade::engine::internalProfileOps(k)) { CHECK(p != o); }
        }
    }
    // The web profile is the closure of what a browser's request can say: 38 ops, as docs/api-0.3-additions.md 8.16.3 counts them. This change does not grow it.
    CHECK_EQ(static_cast<int>(cascade::engine::internalProfileOps(cascade::engine::InternalKind::Web).size()), 38);   // (29 + the 3 store ops + the 6 sliders' app ops)
    Rig r(false, false);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT, "s3cret");
    CHECK(remote.ok());
    testing::Session noTx(api(), r.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE);
    CHECK(noTx.ok());
    const FoxCommand grantCmd = op(FOXAPP_OP_PLUGIN_TX_GRANT, 1, 0, kFile.c_str());
    const FoxCommand armCmd = op(FOXAPP_OP_TX_PLUGIN_ARM, 5, 0);
    const FoxCommand flatCmd = op(FOXAPP_OP_TX_DATA_FLAT, 0);
    r.passes(3);
    for (const FoxCommand* c : {&grantCmd, &armCmd, &flatCmd}) {
        // A REMOTE session holding TRANSMIT (a web remote's PTT and nothing else) is DENIED all three at submit...
        const testing::Outcome rem = r.send(*c, &remote);
        CHECK_EQ(rem.status(), FOXAPI_DENIED);
        // ...and so is a LOCAL session without the grant.
        const testing::Outcome local = r.send(*c, &noTx);
        CHECK_EQ(local.status(), FOXAPI_DENIED);
    }
    // THE WEB SERVER'S OWN SESSION: it holds TRANSMIT (the browser's PTT) and a profile that has none of the three.
    {
        std::int32_t st = 0;
        FoxSession* web = r.eng.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
        CHECK(web != nullptr);
        if (web != nullptr) {
            for (const FoxCommand* c : {&grantCmd, &armCmd, &flatCmd}) {
                FoxSubmitResult sr{};
                sr.structSize = sizeof(sr);
                const int32_t n = api()->submit(web, c, 1, &sr);
                const int32_t submitted = n < 0 ? n : sr.status;
                bool answered = false;
                FoxCommandResult res{};
                for (int pass = 0; pass < 20 && !answered && submitted == FOXAPI_OK; ++pass) {
                    r.passes(1);
                    FoxCommandResult rr[8];
                    for (auto& x : rr) { x.structSize = sizeof(x); }
                    const int32_t got = api()->poll_results(web, rr, 8);
                    for (int32_t i = 0; i < got; ++i) {
                        if (rr[i].ticket == sr.ticket) {
                            answered = true;
                            res = rr[i];
                        }
                    }
                }
                // (the profile refuses it at submit or answers DENIED with the refusal flag - either way it was not applied)
                CHECK(submitted == FOXAPI_DENIED || (answered && res.status == FOXAPI_DENIED && (res.flags & FOXAPI_RESULT_REFUSED) != 0u));
            }
            api()->close_session(web);
        }
    }
    r.passes(3);
    // NOTHING CHANGED: no grant, no window.
    CHECK(Access::granted(r.eng).empty());
    CHECK(!r.armed());
    CHECK_EQ(r.rf("tx.plugin.armed"), std::string("0"));
    // THE FOURTH LOCK, where the ops are applied: with the table's submit-time locks out of the way, a REMOTE flag still stops the grant, the flat tick and the window.
    Access::setRemote(r.eng, true);
    CHECK_EQ(Access::applyDirect(r.eng, grantCmd).status, FOXAPI_DENIED);
    CHECK_EQ(Access::applyDirect(r.eng, flatCmd).status, FOXAPI_DENIED);
    Access::setRemote(r.eng, false);
    CHECK(Access::granted(r.eng).empty());
    CHECK_EQ(Access::armAs(r.eng, 1, /*remote=*/true, armCmd).status, FOXAPI_DENIED);
    CHECK(!r.armed());
    // The window is a session's: applyCommand, which has no sender, refuses it (as it refuses a PTT).
    CHECK_EQ(Access::applyDirect(r.eng, armCmd).status, FOXAPI_DENIED);
    CHECK(!r.armed());
    // FOXAPI_OP_PLUGIN_GRANT is on the web profile, so nothing transmit-related rides on it: kind 3 is still "no such grant", and the grant list stays empty.
    CHECK_EQ(r.send(op(FOXAPI_OP_PLUGIN_GRANT, 3, 1, kFile.c_str())).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPI_OP_PLUGIN_GRANT, 4, 1, kFile.c_str())).status(), FOXAPI_OUT_OF_RANGE);
    CHECK(Access::granted(r.eng).empty());

    // THE RANGES, judged at submit.
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 7)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 61)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, -5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 4294967296LL + 5)).status(), FOXAPI_OUT_OF_RANGE);     // (a 64-bit value that wraps to 5 is not 5)
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, -4294967296LL + 5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 0, 2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_DATA_FLAT, 2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_TX_GRANT, 2, 0, kFile.c_str())).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_TX_GRANT, 1, 0, nullptr)).status(), FOXAPI_BAD_ARGUMENT);
    // A grant is for a loaded module that declares transmit audio; a revoke is taken for any name.
    CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_TX_GRANT, 1, 0, "no-such-module.dll")).status(), FOXAPI_NOT_FOUND);
    CHECK(Access::granted(r.eng).empty());
    CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_TX_GRANT, 0, 0, "no-such-module.dll")).status(), FOXAPI_OK);
}

// ---- B: the refusals ------------------------------------------------------------------------------------------------------------------

void testRefusals() {
    std::printf("  B: ARM says why it is refused, only the three lengths exist, and what answers a refusal clears it\n");
    testing::Outcome o;
    {
        // (One engine at a time: the fixture module's host pointer is the process's, the last one attached.)
        Rig noRadio(false, false);
        CHECK(noRadio.polled());
        // No radio: nothing to arm. (A grant is irrelevant: the order is radio, then grant.)
        o = noRadio.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 5));
        CHECK_EQ(o.status(), FOXAPI_NO_DEVICE);
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
        CHECK(!noRadio.armed());
        CHECK_EQ(noRadio.rf("tx.plugin.refusal"), std::string("1"));
        CHECK(noRadio.grant());
        CHECK_EQ(noRadio.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 5)).status(), FOXAPI_NO_DEVICE);
        CHECK(!noRadio.armed());
        // Opening a radio answers "no transmitter is open".
        CHECK_EQ(noRadio.send(op(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).status(), FOXAPI_OK);
        noRadio.passes(2);
        CHECK_EQ(noRadio.rf("tx.plugin.refusal"), std::string("0"));
    }

    Rig r;
    CHECK(r.polled());
    // A radio and no grant: nothing to let in.
    o = r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 5));
    CHECK_EQ(o.status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK(!r.armed());
    CHECK_EQ(r.rf("tx.plugin.refusal"), std::string("2"));
    // Granted: the grant answered it; the three lengths are accepted, and OFF closes each.
    CHECK(r.grant());
    r.passes(2);
    CHECK_EQ(r.rf("tx.plugin.refusal"), std::string("0"));
    for (const int ok : {5, 15, 60}) {
        o = r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, ok));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK(r.armed());
        CHECK_EQ(Access::armedMinutes(r.eng), ok);
        r.passes(2);
        CHECK_EQ(r.rf("tx.plugin.armed"), std::string("1"));
        CHECK_EQ(r.rf("tx.plugin.minutes"), std::to_string(ok));
        const int left = std::atoi(r.rf("tx.plugin.secondsLeft").c_str());
        CHECK(left > ok * 60 - 5 && left <= ok * 60);
        CHECK_EQ(r.send(op(FOXAPP_OP_TX_PLUGIN_ARM, 0, 0)).status(), FOXAPI_OK);
        CHECK(!r.armed());
    }
    CHECK_EQ(logged("tx: armed 5 min"), 1u);
    CHECK_EQ(logged("tx: armed 15 min"), 1u);
    CHECK_EQ(logged("tx: armed 60 min"), 1u);
    CHECK_EQ(logged("tx: disarmed (you pressed OFF)"), 3u);
    CHECK_EQ(r.radio()->starts.load(), 0);
}

// ---- C: quiet unless everything holds --------------------------------------------------------------------------------------------------

void testQuiet() {
    std::printf("  C: the module asks, and nothing is keyed or pulled until every condition holds\n");
    Rig r;
    CHECK(r.polled());
    r.setFlag(true);   // the module now wants the transmitter, all the time

    // 1. NOT GRANTED, NOT ARMED: asked, polled, never pulled from, never keyed.
    r.framesFor(600);
    CHECK(r.pulls() == 0ull);
    CHECK_EQ(r.radio()->starts.load(), 0);
    CHECK(!r.keyed());
    CHECK_EQ(r.stateFlags(), 0u);
    CHECK((std::atoi(r.rf("plugin." + kFile + ".tx").c_str()) & FOXAPP_TXBIT_ASKED) != 0);   // it has asked: the plate offers the grant

    // 2. THE GRANT ALONE: still not keyed.
    CHECK(r.grant());
    r.framesFor(600);
    CHECK(r.pulls() == 0ull);
    CHECK_EQ(r.radio()->starts.load(), 0);
    CHECK(!r.keyed());
    CHECK_EQ(r.stateFlags(), static_cast<std::uint32_t>(CASCADE_STATE_TX_GRANTED));

    // 3. ARMED BUT THE PAGE IS NOT ON SCREEN: the window closes on the first such frame (the module is not asking while it is armed here: pressing ARM is itself the page
    //    being on screen, and a module that asks at that instant may key in the pass that arms it - the page closing is the next frame). Then it asks, and the page comes back:
    //    the window stays CLOSED, and nothing is keyed.
    r.setFlag(false);
    r.framesFor(300);
    r.press(5);
    CHECK(r.armed());
    r.frame(false);
    r.frame(false);
    CHECK(!r.armed());
    CHECK_EQ(Access::whyClosed(r.eng), std::string("the TRANSMIT page was closed"));
    r.setFlag(true);
    r.framesFor(600);   // the page is back, the module asking...
    CHECK(!r.armed());   // ...and the window stays CLOSED
    CHECK(r.pulls() == 0ull);
    CHECK_EQ(r.radio()->starts.load(), 0);
    CHECK(!r.keyed());

    // 4. GRANTED, ARMED, PAGE LIVE, BUT THE MODULE DOES NOT WANT THE KEY: quiet.
    r.setFlag(false);
    r.framesFor(300);
    r.press(5);
    r.framesFor(600);
    CHECK(r.armed());
    CHECK(r.pulls() == 0ull);
    CHECK_EQ(r.radio()->starts.load(), 0);
    CHECK(!r.keyed());
    CHECK_EQ(r.stateFlags(), static_cast<std::uint32_t>(CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));

    // 5. THE RADIO GOES AWAY with the window open: the window closes and nothing is keyed. (No pass runs between the module being told to ask and the radio going.)
    r.setFlag(true);
    Access::tx(r.eng).setSink(nullptr);   // the sink removed under the engine's feet
    r.framesFor(500);
    CHECK(!r.armed());
    CHECK(!r.keyed());
    CHECK(r.pulls() == 0ull);
    if (logged("tx: plugin") != 0u) { dumpTxLog("quiet 5"); }
    CHECK_EQ(logged("tx: plugin"), 0u);   // no key line was ever written, in any of the five
}

// ---- D: keys --------------------------------------------------------------------------------------------------------------------------

void testKeys() {
    std::printf("  D: granted + armed + live + the module asking = keyed; it is pulled from and told\n");
    Rig r;
    CHECK(r.polled());
    CHECK(r.grant());
    r.press(5);
    CHECK(r.armed());
    r.framesFor(300);
    CHECK(!r.keyed());   // armed, and the module does not want the key yet
    CHECK(r.pulls() == 0ull);

    r.setFlag(true);
    CHECK(r.until([&] { return r.keyed(); }, 5000));
    CHECK_EQ(r.radio()->starts.load(), 1);
    CHECK(Access::tx(r.eng).activeInput() == cc::TxInput::Plugin);
    // The runner is told it is keyed on the drive that keyed, and pulls the module's audio into the ring.
    CHECK(r.until([&] { return r.pulls() > 0ull; }, 5000));
    CHECK(r.until([&] { return Access::tx(r.eng).pluginFramesSent() > 4800u; }, 5000));
    const Counts c = countsOf(r.eng);
    std::printf("    keyed: the module was asked %llu times and pulled %llu times for %llu frames; %llu frames went out\n", c.actives - r.base.actives,
                c.pulls - r.base.pulls, c.frames - r.base.frames, static_cast<unsigned long long>(Access::tx(r.eng).pluginFramesSent()));
    CHECK(r.radio()->samples.load() > 30000ull);   // frames x 10: the ledger radio runs at 480 kS/s
    // It is told, through get_state, all three things.
    CHECK_EQ(r.stateFlags(), static_cast<std::uint32_t>(CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING));
    CHECK_EQ(logged("tx: armed 5 min"), 1u);
    CHECK_EQ(logged("tx: plugin TX Audio Fixture keyed"), 1u);
    CHECK(Access::tx(r.eng).effectiveMode() == cascade::dsp::TxMode::NFM);
    CHECK(!Access::tx(r.eng).preemphasis());
    CHECK_EQ(r.rf("tx.plugin.keyed"), std::string(kName));
    CHECK((std::atoi(r.rf("plugin." + kFile + ".tx").c_str()) & FOXAPP_TXBIT_ON_AIR) != 0);

    // DATA (flat) is the engine's: unticked, a plugin's audio goes out in the page's mode with the pre-emphasis.
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_DATA_FLAT, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.rf("tx.plugin.dataFlat"), std::string("0"));
    CHECK_EQ(r.send(op(FOXAPP_OP_TX_DATA_FLAT, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.rf("tx.plugin.dataFlat"), std::string("1"));

    // The module lets go: after its tail the key opens, the radio is stopped, and it is told.
    r.setFlag(false);
    CHECK(r.until([&] { return !r.keyed(); }, 5000));
    CHECK(r.until([&] { return !r.onAir(); }, 5000));
    CHECK(r.radio()->stops.load() >= 1);
    CHECK(r.armed());   // letting go is not a fault: the window stays open
    CHECK_EQ(r.stateFlags(), static_cast<std::uint32_t>(CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));
    CHECK_EQ(logged("tx: plugin TX Audio Fixture unkeyed (the plugin let go"), 1u);
    const unsigned long long pullsAfter = r.pulls();
    r.framesFor(400);
    CHECK(r.pulls() == pullsAfter);   // and it is not asked for audio once it is unkeyed

    // It asks again within the same window: keyed again, no second ARM needed.
    r.setFlag(true);
    CHECK(r.until([&] { return r.keyed(); }, 5000));
    CHECK_EQ(r.radio()->starts.load(), 2);
    r.setFlag(false);
    CHECK(r.until([&] { return !r.onAir(); }, 5000));
}

// ---- E: revoke ------------------------------------------------------------------------------------------------------------------------

void testRevoke() {
    std::printf("  E: a revoked grant, or a closed window, unkeys WITHIN ONE PASS - and re-granting alone does not re-key\n");
    {
        Rig r;
        CHECK(r.key());
        // THE GRANT REVOKED: the op lands at the head of ONE pass and the key is up at the end of that same pass - the transmitter has let go of the radio.
        r.submitThenOnePass(op(FOXAPP_OP_PLUGIN_TX_GRANT, 0, 0, kFile.c_str()));
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.onAir());
        CHECK(!r.armed());   // it was the last grant: the window closes with it
        CHECK_EQ(Access::whyClosed(r.eng), std::string("no plugin holds the TRANSMIT grant any more"));
        CHECK(Access::granted(r.eng).empty());
        // The module still asks; the grant comes back; NOTHING re-keys until a person arms a new window.
        const int starts = r.radio()->starts.load();
        CHECK(r.grant());
        r.framesFor(700);
        CHECK(!r.keyed());
        CHECK_EQ(r.radio()->starts.load(), starts);
        r.press(5);
        CHECK(r.until([&] { return r.keyed(); }, 5000));   // a new decision keys it
        CHECK_EQ(r.radio()->starts.load(), starts + 1);
    }
    {
        // THE WINDOW CANNOT OUTLIVE THE LAST GRANT, keyed or not: armed, the module not asking, the only grant revoked - the window closes in that pass, and the module asking
        // afterwards with the grant given back is NOT keyed (a new window is a new decision).
        Rig r;
        CHECK(r.polled());
        CHECK(r.grant());
        r.press(5);
        CHECK(r.armed());
        r.submitThenOnePass(op(FOXAPP_OP_PLUGIN_TX_GRANT, 0, 0, kFile.c_str()));
        CHECK(!r.armed());
        CHECK_EQ(Access::whyClosed(r.eng), std::string("no plugin holds the TRANSMIT grant any more"));
        r.setFlag(true);
        CHECK(r.grant());
        r.framesFor(700);
        CHECK(!r.keyed());
        CHECK_EQ(r.radio()->starts.load(), 0);
        CHECK(r.pulls() == 0ull);
    }
    {
        Rig r;
        CHECK(r.key());
        // OFF: the same.
        r.submitThenOnePass(op(FOXAPP_OP_TX_PLUGIN_ARM, 0, 0));
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.onAir());
        CHECK(!r.armed());
    }
    {
        Rig r;
        CHECK(r.key());
        // THE ENGINE'S OWN GRANT CHECK, with the runner's out of the way: the runner still holds the grant, the engine's durable list does not. The runner's copy would let the
        // plugin on, so this is the check that stands alone ("granted HERE as well as in the runner").
        Access::granted(r.eng).clear();
        r.passes(1);
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.onAir());
    }
    {
        Rig r;
        CHECK(r.key());
        // THE ENGINE'S OWN WINDOW CHECK: the module's key is withdrawn the pass the window closes even when nothing else changed (the grant, the radio and the page are fine).
        r.submitThenOnePass(op(FOXAPP_OP_TX_PLUGIN_ARM, 0, 1));
        CHECK(!r.keyed());
        CHECK(!r.onAir());
        CHECK_EQ(Access::whyClosed(r.eng), std::string("the TRANSMIT page was closed"));
    }
}

// ---- E2: the control thread stopping -------------------------------------------------------------------------------------------------------

void testStopping() {
    std::printf("  E: the control thread stopping: from the moment the flag is set the drive does not key a plugin, whatever else holds\n");
    Rig r;
    CHECK(r.polled());
    CHECK(r.grant());
    r.press(5);
    CHECK(r.armed());
    Access::setStopping(r.eng, true);   // (stopControlThread sets it BEFORE it lets go of the key; a pass still running must not ask for it again)
    r.setFlag(true);
    r.framesFor(700);
    CHECK(r.armed());   // every other condition holds...
    CHECK(!r.requested());
    CHECK(!r.keyed());   // ...and nothing is keyed
    CHECK_EQ(r.radio()->starts.load(), 0);
    CHECK(r.pulls() == 0ull);
    Access::setStopping(r.eng, false);   // the flag was the only thing holding it
    CHECK(r.until([&] { return r.keyed(); }, 5000));
}

// ---- F: every closing reason -----------------------------------------------------------------------------------------------------------

// Keys the module, runs `cut`, and checks: the key is down inside `passBound` more passes, the window closed for `reasonFragment`, and the module still asking does NOT key
// again (the page live, the radio there, the module willing, a window not armed).
void closes(const char* what, const std::function<void(Rig&)>& cut, const char* reasonFragment, bool windowCloses = true, bool afterCutPageLive = true,
            bool sameRadio = true) {
    std::printf("  F: %s\n", what);
    Rig r;
    CHECK(r.key());
    const int startsBefore = r.radio()->starts.load();
    cut(r);
    CHECK(r.until([&] { return !r.keyed(); }, 3000, afterCutPageLive));
    CHECK(r.until([&] { return !r.onAir(); }, 3000, afterCutPageLive));
    if (windowCloses) {
        CHECK(!r.armed());
        const std::string why = Access::whyClosed(r.eng);
        if (why.find(reasonFragment) == std::string::npos) { std::printf("    the window closed because: \"%s\" (wanted \"%s\")\n", why.c_str(), reasonFragment); }
        CHECK(why.find(reasonFragment) != std::string::npos);
    }
    const unsigned long long pulls = r.pulls();
    r.framesFor(700, true);
    CHECK(!r.keyed());
    CHECK(!r.onAir());
    if (r.radio() != nullptr && windowCloses && sameRadio) { CHECK_EQ(r.radio()->starts.load(), startsBefore); }
    if (!sameRadio && r.radio() != nullptr) { CHECK_EQ(r.radio()->starts.load(), 0); }   // (a radio opened in its place has never been keyed)
    CHECK(r.pulls() == pulls);
    CHECK(logged("tx: plugin TX Audio Fixture unkeyed") >= 1u);
    CHECK((r.stateFlags() & CASCADE_STATE_TRANSMITTING) == 0u);
}

void testCloses() {
    closes("the PAGE closing unkeys and closes the window", [](Rig& r) { r.frame(false); r.frame(false); }, "the TRANSMIT page was closed");
    closes("OFF unkeys and closes the window", [](Rig& r) { r.frame(true, 0); }, "you pressed OFF");
    closes("the grant being WITHDRAWN unkeys and closes the window", [](Rig& r) { CHECK(r.grant(false)); }, "no plugin holds the TRANSMIT grant");
    closes("the radio being CLOSED unkeys and closes the window", [](Rig& r) { CHECK_EQ(r.send(op(FOXAPI_OP_TX_CLOSE)).status(), FOXAPI_OK); }, "the transmitter was closed", true, true,
           false);
    closes("the radio being CHANGED unkeys and closes the window", [](Rig& r) { CHECK_EQ(r.send(op(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).status(), FOXAPI_OK); },
           "the transmitter was changed", true, true, false);
    closes("a radio FAULT unkeys, closes the window, and is not retried", [](Rig& r) { r.radio()->fault(); }, "");
    closes("the window's own TIME unkeys it", [](Rig& r) { Access::armWithTimeLeft(r.eng, 500); }, "the arming window ran out");
    closes("a configuration being loaded closes the window", [](Rig& r) { Access::restoreConfig(r.eng, Access::readConfig(r.eng)); }, "a configuration was loaded");
    closes("the receiver being STOPPED unkeys and closes the window", [](Rig& r) { CHECK_EQ(r.send(op(FOXAPI_OP_RUN, 0)).status(), FOXAPI_OK); }, "the receiver was stopped");
    closes("the engine STOPPING unkeys and closes the window", [](Rig& r) { Access::engineStopping(r.eng); }, "the engine is shutting down");
    // THE LEASE: the window belongs to the session that opened it.
    closes("the window that armed it CLOSING closes the window", [](Rig& r) { r.page->close(); r.passes(1); }, "the window that armed this was closed");
    closes("the window that armed it going SILENT (no heartbeat) closes the window", [](Rig& r) { r.beating = false; }, "the window stopped responding");
    closes("the page ceasing to say it is on screen (a lost OFF, a hung frame) closes the window", [](Rig& r) {
        // The session beats, the page says nothing: no "still here" and no OFF, as if the commands were lost.
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < end && r.armed()) {
            r.page->beat();
            r.passes(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }, "the TRANSMIT page was closed");
}

void testCap() {
    std::printf("  F: a continuous key (the latch's own number) ends it, and the window with it\n");
    Rig r;
    CHECK(r.polled());
    CHECK(r.grant());
    Access::tx(r.eng).setLatchTimeoutForTest(std::chrono::milliseconds(500));
    r.press(5);
    r.setFlag(true);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.until([&] { return r.keyed(); }, 5000));
    // The module holds active() high for ever. The key is cut at the cap, not before.
    CHECK(r.until([&] { return !r.keyed(); }, 5000));
    const double heldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    keyed for about %.0f ms against a 500 ms cap\n", heldMs);
    CHECK(heldMs > 400.0);
    CHECK(heldMs < 3000.0);
    CHECK(!r.armed());   // the window closed with it
    CHECK(Access::whyClosed(r.eng).find("held the key") != std::string::npos);
    CHECK_EQ(logged("tx: plugin TX Audio Fixture unkeyed (a plugin held the key"), 1u);
    // Still asking; still the page live; still the radio: NOT keyed again.
    const int starts = r.radio()->starts.load();
    r.framesFor(800);
    CHECK(!r.keyed());
    CHECK_EQ(r.radio()->starts.load(), starts);
    // The operator arms again: a fresh decision, and it keys.
    r.press(5);
    CHECK(r.until([&] { return r.keyed(); }, 5000));
    CHECK_EQ(r.radio()->starts.load(), starts + 1);
}

// ---- G: the hardening ---------------------------------------------------------------------------------------------------------------------

void testHardening() {
    std::printf("  G: a keyed plugin that goes DORMANT, is STOPPED, UNLOADED or REBUILT unkeys at once and closes the window\n");
    // The reasons are said by the rules above; what is held here is that they happen IN THE PASS the plugin goes, not a block (or a drain) later.
    {
        // DORMANT: with a window, an AUTO plugin runs while something uses it (here: the Decoder output window is open and it is a text decoder) and goes down
        // FOXSDR_DORMANT_AFTER_MS (400 in this process) after the last use.
        Rig r(/*windowed=*/true);
        const std::string use = kFile + "\t" + std::to_string(FOXAPP_PLUGIN_USE_DECODER_OUTPUT) + "\n";
        const FoxCommand useCmd = op(FOXAPP_OP_PLUGIN_USE, 0, 0, use.c_str());
        CHECK_EQ(r.send(useCmd).status(), FOXAPI_OK);
        CHECK(r.polled());
        CHECK(r.grant());
        r.press(5);
        r.setFlag(true);
        CHECK(r.until([&] { return r.keyed(); }, 5000));
        CHECK(r.until([&] { return r.pulls() > 0ull; }, 5000));
        // The use goes. Nothing happens until the interval is out...
        CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_USE)).status(), FOXAPI_OK);   // (an empty table: nothing uses it)
        const auto cleared = std::chrono::steady_clock::now();
        r.framesFor(150);
        CHECK(r.keyed());   // (a plugin that is merely not being looked at is not put down early)
        // ...and the PASS that puts it to sleep takes the key and the window with it.
        bool sawSleep = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
        while (std::chrono::steady_clock::now() < deadline) {
            r.frame(true);
            if (!Access::runner(r.eng).hasInstances(kFile)) {
                sawSleep = true;
                break;
            }
        }
        CHECK(sawSleep);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cleared).count();
        std::printf("    dormant %.0f ms after its last use (interval 400); in that pass: key requested %d, keyed %d, window armed %d\n", ms, r.requested() ? 1 : 0,
                    r.keyed() ? 1 : 0, r.armed() ? 1 : 0);
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.armed());
        r.frame(true);   // (the sleep is in the plugin phase, after the pass's key drive: the radio itself is let go by the next drive)
        CHECK(!r.onAir());
        CHECK(Access::whyClosed(r.eng).find("went dormant") != std::string::npos);
        // Woken again, still asking: NOT re-keyed.
        const int starts = r.radio()->starts.load();
        CHECK_EQ(r.send(useCmd).status(), FOXAPI_OK);
        CHECK(r.until([&] { return Access::runner(r.eng).hasInstances(kFile); }, 3000));
        r.framesFor(800);
        CHECK(!r.keyed());
        CHECK_EQ(r.radio()->starts.load(), starts);
    }
    {
        // STOPPED: the run state STOPPED takes the plugin's instances down.
        Rig r;
        CHECK(r.key());
        r.submitThenOnePass(op(FOXAPP_OP_PLUGIN_RUN_MODE, 2, 0, kFile.c_str()));
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.onAir());
        CHECK(!r.armed());
        CHECK(Access::whyClosed(r.eng).find("was stopped") != std::string::npos);
        // Un-stopped (ALWAYS) and still asking: not re-keyed.
        CHECK_EQ(r.send(op(FOXAPP_OP_PLUGIN_RUN_MODE, 1, 0, kFile.c_str())).status(), FOXAPI_OK);
        const int starts = r.radio()->starts.load();
        r.framesFor(800);
        CHECK(!r.keyed());
        CHECK_EQ(r.radio()->starts.load(), starts);
    }
    {
        // UNLOADED: a rescan unmaps the module and loads it again.
        Rig r;
        CHECK(r.key());
        Access::rescan(r.eng);
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.armed());
        CHECK(Access::whyClosed(r.eng).find("unloaded") != std::string::npos);
        r.frame(true);
        CHECK(!r.onAir());
        r.framesFor(800);
        CHECK(!r.keyed());
    }
    {
        // REBUILT: the runner's instances are rebuilt (a retune, a source change, a patch).
        Rig r;
        CHECK(r.key());
        Access::refreshRunner(r.eng);
        CHECK(!r.requested());
        CHECK(!r.keyed());
        CHECK(!r.armed());
        CHECK(Access::whyClosed(r.eng).find("rebuilt") != std::string::npos);
        r.framesFor(800);
        CHECK(!r.keyed());
    }
    {
        // A rebuild that finds the window OPEN BUT NOTHING KEYED leaves the window alone: the operator's authorisation is not spent by a retune.
        Rig r;
        CHECK(r.polled());
        CHECK(r.grant());
        r.press(5);
        CHECK(r.armed());
        Access::refreshRunner(r.eng);
        CHECK(r.armed());
        r.setFlag(true);
        CHECK(r.until([&] { return r.keyed(); }, 5000));
    }
}

// ---- H: the hand wins -----------------------------------------------------------------------------------------------------------------

void testHand() {
    std::printf("  H: a PTT held while the plugin asks is the hand's transmission, not the plugin's\n");
    Rig r;
    CHECK(r.key());
    // The operator takes the PTT: a LOCAL session's TX_PTT 1, held (re-asserted every frame, as the page does).
    const auto hold = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            const FoxCommand ptt = op(FOXAPI_OP_TX_PTT, 1);
            (void)api()->submit(r.page->raw(), &ptt, 1, nullptr);
            r.frame(true);
        }
    };
    hold(400);
    CHECK(r.onAir());
    CHECK(!r.keyed());   // keyed, but not for the plugin
    CHECK(Access::tx(r.eng).activeInput() == cc::TxInput::Tone);   // the page's input
    CHECK(Access::tx(r.eng).preemphasis());
    // The runner is told it is no longer keyed for the plugin: it is not pulled from.
    const unsigned long long pulls = r.pulls();
    hold(400);
    CHECK(r.pulls() == pulls);
    // The PTT released: the window is still open and the plugin still asks: keyed for it again.
    const FoxCommand off = op(FOXAPI_OP_TX_PTT, 0);
    (void)api()->submit(r.page->raw(), &off, 1, nullptr);
    CHECK(r.until([&] { return r.keyed(); }, 5000));
    r.setFlag(false);
    CHECK(r.until([&] { return !r.onAir(); }, 5000));
}

// ---- I: the dead-man ------------------------------------------------------------------------------------------------------------------

void testDeadMan() {
    std::printf("  I: a control side that stops ticking lets go of a plugin's key on the TX thread, and the plugin is not re-keyed\n");
    Rig r;
    CHECK(r.key());
    CHECK(r.onAir());
    // NO PASSES AT ALL for longer than Transmitter::kKeyAliveWait: nothing here ticks the transmitter, and the TX thread - which neither the passes nor this test stop - lets go.
    const auto t0 = std::chrono::steady_clock::now();
    while (r.radio()->stops.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(5000)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    the TX thread let go %.0f ms after the last pass (the dead-man's number is %lld ms)\n", ms,
                static_cast<long long>(cc::Transmitter::kKeyAliveWait.count()));
    CHECK(r.radio()->stops.load() >= 1);
    CHECK(ms > 800.0);
    CHECK(ms < 3000.0);
    // The next pass sees it: the window is closed, nothing keys again.
    const int starts = r.radio()->starts.load();
    r.framesFor(900);
    CHECK(!r.keyed());
    CHECK(!r.armed());
    CHECK_EQ(r.radio()->starts.load(), starts);
}

// ---- J: the config --------------------------------------------------------------------------------------------------------------------

void testConfig() {
    std::printf("  J: the grant is saved and restored; the arming window is neither\n");
    cc::AppConfig saved;
    {
        Rig r;
        CHECK(r.polled());
        CHECK(r.grant());
        r.press(15);
        CHECK(r.armed());
        saved = Access::readConfig(r.eng);
        CHECK(saved.pluginTransmitAllowed.size() == 1u && saved.pluginTransmitAllowed[0] == kFile);
        // The file on disk: the grant is in it, and nothing that names the window.
        const std::string path = (g_scratch / "armed_config.json").string();
        std::string err;
        CHECK(cc::ConfigStore::save(path, saved, err));
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
        CHECK(text.find(kFile) != std::string::npos);
        std::string lower = text;
        for (char& ch : lower) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        CHECK(lower.find("arm") == std::string::npos);   // not an "armed", an "arming", an "arm..."
        CHECK(lower.find("transmitptt") == std::string::npos);
        CHECK(lower.find("transmitlatch") == std::string::npos);
        // A configuration loaded mid-session closes the window (and keeps the grant it carries).
        Access::restoreConfig(r.eng, saved);
        CHECK(!r.armed());
        CHECK_EQ(Access::whyClosed(r.eng), std::string("a configuration was loaded"));
        CHECK(Access::granted(r.eng).size() == 1u);
    }
    {
        // A new engine loading that file has the grant and a CLOSED window, and the module asking is not keyed.
        Rig again;
        CHECK(Access::granted(again.eng).empty());
        Access::restoreConfig(again.eng, saved);
        CHECK(Access::granted(again.eng).size() == 1u && Access::granted(again.eng)[0] == kFile);
        CHECK(!again.armed());
        CHECK(again.polled());
        again.setFlag(true);
        again.framesFor(700);
        CHECK(!again.keyed());   // a restored grant is not a restored authorisation
        CHECK_EQ(again.radio()->starts.load(), 0);
        CHECK(again.pulls() == 0ull);
        CHECK_EQ(again.stateFlags(), static_cast<std::uint32_t>(CASCADE_STATE_TX_GRANTED));
    }
}

// ---- K: the facts --------------------------------------------------------------------------------------------------------------------

void testFacts() {
    std::printf("  K: the facts the page and the plate are drawn from, and who may read them\n");
    Rig r;
    CHECK_EQ(r.rf("tx.plugin.armed"), std::string("0"));
    CHECK_EQ(r.rf("tx.plugin.minutes"), std::string(""));
    CHECK_EQ(r.rf("tx.plugin.keyed"), std::string(""));
    CHECK_EQ(r.rf("tx.plugin.dataFlat"), std::string("1"));
    CHECK(r.polled());
    // A module that declares transmit audio has bits; they say declared, and (after it asks) asked.
    const auto bits = [&] { return static_cast<std::uint32_t>(std::atoi(r.rf("plugin." + kFile + ".tx").c_str())); };
    CHECK((bits() & FOXAPP_TXBIT_DECLARED) != 0u);
    CHECK((bits() & (FOXAPP_TXBIT_GRANTED | FOXAPP_TXBIT_ON_AIR)) == 0u);
    CHECK(r.key());
    CHECK((bits() & (FOXAPP_TXBIT_GRANTED | FOXAPP_TXBIT_ASKED | FOXAPP_TXBIT_HOLDER | FOXAPP_TXBIT_ON_AIR)) ==
          (FOXAPP_TXBIT_GRANTED | FOXAPP_TXBIT_ASKED | FOXAPP_TXBIT_HOLDER | FOXAPP_TXBIT_ON_AIR));
    CHECK_EQ(r.rf("tx.plugin.keyed"), std::string(kName));
    CHECK_EQ(r.rf("tx.plugin.name"), std::string(kName));
    // A REMOTE session reads none of them (they are LOCAL only).
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(remote.ok());
    r.passes(2);
    CHECK_EQ(fact(remote, "tx.plugin.armed"), std::string(""));
    CHECK_EQ(fact(remote, "tx.plugin.keyed"), std::string(""));
    CHECK_EQ(fact(remote, "plugin." + kFile + ".tx"), std::string(""));
    // A window that closes says why on the fact, once, and the OFF the operator pressed says nothing.
    CHECK(r.grant(false));
    r.passes(2);
    CHECK_EQ(r.rf("tx.plugin.reason"), std::string("no plugin holds the TRANSMIT grant any more"));
    CHECK_EQ(r.rf("tx.plugin.armed"), std::string("0"));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_txplugin\n");
    if (argc < 2) {
        std::printf("usage: test_engine_api_txplugin <tx_audio_fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    engine_api_test::hermetic("txplugin");
    // The plugins' hysteresis, shortened for G (the engine reads it at its first lifecycle step).
    engine_api_test::setEnv("FOXSDR_DORMANT_AFTER_MS", "400");
    g_scratch = fs::path(std::getenv("LOCALAPPDATA") != nullptr ? std::getenv("LOCALAPPDATA") : ".") / "txplugin_test";
    g_plugins = g_scratch / "plugins";
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_plugins, ec);
    fs::copy_file(g_fixture, g_plugins / kFile, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    testSurface();
    testRefusals();
    testQuiet();
    testKeys();
    testRevoke();
    testStopping();
    testCloses();
    testCap();
    testHardening();
    testHand();
    testDeadMan();
    testConfig();
    testFacts();

    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_txplugin");
}
