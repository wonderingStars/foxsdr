// test_snapshot_app.cpp - the one receiver snapshot (engine stage 2,
// docs/engine-stage2.md), driven through the REAL window.
//
//   [1] THE COST OF THE WINDOW'S PUBLISH with three decoder plugins loaded and
//       fed (tests/fixture_stage1_plugin.cpp built under three names), and
//       the decoder figures it publishes against the window's own counters
//       (review L4: one runner lock per publish).
//   [2] WALKING ONE through the window (review M1): each source condition,
//       changed alone through its control, changes exactly its own flag.
//   [3] THE WEB ROWS AND THEIR IDS ARE ONE BLOCK (review L3): while a newer
//       block is held back, a row tunes the bookmark the served block shows
//       on it; once it lands, the new row.
//
// HERMETIC, like test_apply_command: no config, scratch folders, fake radio,
// fake scans, the fixture plugins in a plugin directory of this test's own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
#include "iiod_fake_server.hpp"
#include "imgui.h"
#include "net/web_control.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;

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
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_snapshot_app_" + std::to_string(pid));
    g_plugins = g_scratch / "plugins";
    std::filesystem::create_directories(g_plugins);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // Three decoder modules: the stage-1 fixture built under three names.
    for (const char* f : {FIXTURE_SNAP_A, FIXTURE_SNAP_B, FIXTURE_SNAP_C}) {
        const std::filesystem::path fixture(f);
        std::error_code ec;
        std::filesystem::copy_file(fixture, g_plugins / fixture.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { std::printf("could not copy a fixture plugin: %s\n", ec.message().c_str()); }
    }
}

std::string pluginDirHook() { return g_plugins.string(); }

// A radio that hands over zeros and supports device AGC.
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
    std::vector<cascade::source::GainInfo> gains() const override {
        cascade::source::GainInfo lna;
        lna.name = "LNA";
        lna.maxDb = 40.0;
        return {lna};
    }
    bool setGainDb(const std::string&, double db) override {
        lna_ = db;
        return true;
    }
    double gainDb(const std::string&) const override { return lna_; }
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override {
        agc_ = on;
        return true;
    }
    bool autoGain() const override { return agc_; }
    std::vector<std::string> antennas() const override { return {"A"}; }
    bool setAntenna(const std::string&) override { return true; }
    std::string antenna() const override { return "A"; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.048e6}; }
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
    double rate_ = 2.048e6;
    double centre_ = 100.0e6;
    double lna_ = 20.0;
    bool agc_ = false;
    bool open_ = false;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    return std::make_unique<FakeRadio>(kind);
}
std::vector<cascade::source::NativeDeviceInfo> fakeNativeScan() {
    return {{"rtlsdr", "Fake RTL", "serial=0001"}};
}
std::vector<cascade::source::SoapyDeviceInfo> fakeSoapyScan() { return {}; }

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

struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.pluginDir = &pluginDirHook;
        AppWindow::testHooks_.makeDevice = &makeFake;
        AppWindow::testHooks_.nativeScan = &fakeNativeScan;
        AppWindow::testHooks_.soapyScan = &fakeSoapyScan;
    }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) { return a.applyCommand(c, {}); }
    static FoxCommandResult apply(AppWindow& a, const cmd::QueuedCommand& q) {
        return a.applyCommand(q.c, q.longText);
    }
    static bool waitOpen(AppWindow& a) {
        return waitFor([&a] {
            a.pollSourceAsync();
            return !a.deviceOpenPending_;
        });
    }
    static bool sourceBusy(AppWindow& a) {
        a.pollSourceAsync();
        return a.soapyScanPending_ || a.deviceOpenPending_;
    }
    static void setRecordDir(AppWindow& a, const std::string& d) { a.recordDir_ = d; }
    static void setTransmitPageOpen(AppWindow& a, bool on) { a.transmitOpen_ = on; }
    static bool haveTx(AppWindow& a) { return a.transmitter_.haveSink(); }
    static std::uint64_t addBookmark(AppWindow& a, const std::string& name, double hz, const char* mode) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = mode;
        b.bandwidthHz = 10.0e3;
        const int at = a.freqMgr_.add(b);
        return a.freqMgr_.list()[static_cast<std::size_t>(at)].id;
    }
    static int modeIndex(AppWindow& a) { return a.modeIndex_; }
    static cascade::net::RadioStatus webStatus(AppWindow& a) { return a.webStatusNow(); }
    static void webRequest(AppWindow& a, const cascade::net::ControlRequest& r) { a.applyControlRequest(r); }
    static cascade::core::ReceiverSnapshot& snapshot(AppWindow& a) { return *a.receiverSnapshot_; }
    static void publish(AppWindow& a) { a.publishReceiverState(); }
    static std::size_t loadedDecoders(AppWindow& a) { return a.loadedDecoderCount(); }
    static std::size_t fedDecoders(AppWindow& a) { return a.fedDecoderCount(); }
    static std::size_t runnerActive(AppWindow& a) { return a.pluginRunner_.activeCount(); }
    static bool running(AppWindow& a) { return a.pipeline_.running(); }
    static cascade::core::PublishedState state(AppWindow& a) {
        cascade::core::PublishedState s;
        const bool ok = a.receiverSnapshot_->read(s);
        CHECK(ok);
        return s;
    }
    static double publishNs(AppWindow& a, int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) { a.publishReceiverState(); }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    }
};

}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

// --- [1] the cost, and the decoder figures -----------------------------------------------

void decodersAndCost(AppWindow& a) {
    std::printf("[1] three fed decoders: the publish's cost and its decoder figures\n");
    std::printf("      decoders loaded: %zu\n", A::loadedDecoders(a));
    CHECK(A::loadedDecoders(a) == 3u);
    const double idleNs = A::publishNs(a, 3000);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RUN, 1))));
    CHECK(A::running(a));
    CHECK(waitFor([&a] { return A::fedDecoders(a) == 3u; }));
    std::printf("      fed: %zu, runner active: %zu\n", A::fedDecoders(a), A::runnerActive(a));
    // Warm, then measure (median of five runs of 3000 publishes).
    (void)A::publishNs(a, 500);
    std::vector<double> runs;
    for (int r = 0; r < 5; ++r) { runs.push_back(A::publishNs(a, 3000)); }
    std::sort(runs.begin(), runs.end());
    std::printf("      publishReceiverState, receiver stopped: %.0f ns/frame\n", idleNs);
    std::printf("      publishReceiverState, running, 3 fed: median %.0f ns/frame (runs %.0f..%.0f)\n",
                runs[2], runs[0], runs[4]);
    // The figures the publish carries are the window's own.
    A::publish(a);
    const cascade::core::PublishedState s = A::state(a);
    CHECK(s.rx.decodersRunning == A::fedDecoders(a));
    CHECK(s.rx.decodersFitted == A::loadedDecoders(a));
    CHECK(((s.rx.flags & FOXAPI_RX_DECODER_ACTIVE) != 0u) == (A::running(a) && A::runnerActive(a) > 0u));
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RUN, 0))));
    A::publish(a);
    const cascade::core::PublishedState t = A::state(a);
    CHECK(t.rx.decodersRunning == 0u && (t.rx.flags & FOXAPI_RX_DECODER_ACTIVE) == 0u);
    CHECK(t.rx.decodersFitted == 3u);
}

// --- [2] walking one through the window: each source condition -> its own flag -----------
//
// core::receiverFlags is walked exhaustively in test_receiver_snapshot [7c];
// this proves the WINDOW hands each condition its own source. Each step
// changes one condition through the control a user or a remote uses, and the
// published flags must change in exactly that condition's flag. The flags
// that follow the signal (SQUELCH_OPEN, STEREO_ACTIVE) or the audio device
// (SINK_OPEN) are left out of the comparison; the per-session TX_KEY_MINE
// and TX_LATCH_RELEASE_FIRST are never set. Not reachable here without real
// hardware or a fault: FAULTED, TX_KEYED, TX_LATCHED, WEB_LISTENING - those
// sources are one-to-one lines in fillPublishedState, and their mapping is
// walked in [7c].

constexpr std::uint32_t kMeasured = FOXAPI_RX_SQUELCH_OPEN | FOXAPI_RX_STEREO_ACTIVE | FOXAPI_RX_SINK_OPEN;

std::uint32_t flagsNow(AppWindow& a) {
    A::publish(a);
    return A::state(a).rx.flags & ~kMeasured;
}

void step(AppWindow& a, std::uint32_t& before, const char* what, std::uint32_t want) {
    const std::uint32_t now = flagsNow(a);
    const std::uint32_t changed = now ^ before;
    const bool okay = changed == want;
    std::printf("      %-40s changed %08x%s\n", what, changed, okay ? "" : "   EXPECTED DIFFERENT");
    if (!okay) { std::printf("        wanted %08x\n", want); }
    CHECK(okay);
    before = now;
}

cascade::core::cmd::QueuedCommand textCmd(std::uint32_t op, const std::string& t) {
    return cmd::makeText(op, t);
}

void walkingOneWindow(AppWindow& a) {
    std::printf("[2] the window: each source condition alone -> exactly its own flag\n");
    A::setRecordDir(a, (g_scratch / "rec").string());
    std::uint32_t f = flagsNow(a);
    std::printf("      start: %08x\n", f);

    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1))));
    step(a, f, "mute", FOXAPI_RX_MUTED);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0))));
    step(a, f, "unmute", FOXAPI_RX_MUTED);
    const bool stereoOn = (f & FOXAPI_RX_STEREO_ENABLED) != 0u;
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_STEREO, stereoOn ? 0 : 1))));
    step(a, f, "stereo switch", FOXAPI_RX_STEREO_ENABLED);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_STEREO, stereoOn ? 1 : 0))));
    step(a, f, "stereo switch back", FOXAPI_RX_STEREO_ENABLED);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_NR, 1, 1))));
    step(a, f, "noise reduction on", FOXAPI_RX_NR);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_NR, 0, 0))));
    step(a, f, "noise reduction off", FOXAPI_RX_NR);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_NOTCH, 1, 1))));
    step(a, f, "notch on", FOXAPI_RX_NOTCH);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_NOTCH, 0, 0))));
    step(a, f, "notch off", FOXAPI_RX_NOTCH);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 1))));
    step(a, f, "auto notch on", FOXAPI_RX_AUTO_NOTCH);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 0))));
    step(a, f, "auto notch off", FOXAPI_RX_AUTO_NOTCH);

    // A radio: two conditions at once, the radio and its AGC capability. (The
    // scan lists the fake radio; it moves no flag.)
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_SCAN_DEVICES))));
    CHECK(waitFor([&a] { return !A::sourceBusy(a); }));
    step(a, f, "scan for radios", 0u);
    CHECK(ok(A::apply(a, textCmd(FOXAPI_OP_SELECT_SOURCE, "rtlsdr:serial=0001"))));
    CHECK(A::waitOpen(a));
    step(a, f, "a radio opens (+ its AGC capability)", FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_AGC_SUPPORTED);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, 1))));
    step(a, f, "device AGC on", FOXAPI_RX_DEVICE_AGC);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, 0))));
    step(a, f, "device AGC off", FOXAPI_RX_DEVICE_AGC);

    // Running feeds the three decoders: the DEC lamp comes with it.
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RUN, 1))));
    CHECK(waitFor([&a] { return A::runnerActive(a) > 0u; }));
    step(a, f, "run (+ the decoders it feeds)", FOXAPI_RX_RUNNING | FOXAPI_RX_DECODER_ACTIVE);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 1))));
    step(a, f, "record I/Q", FOXAPI_RX_RECORDING_IQ);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 0))));
    step(a, f, "stop I/Q", FOXAPI_RX_RECORDING_IQ);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1))));
    step(a, f, "record audio", FOXAPI_RX_RECORDING_AUDIO);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 0))));
    step(a, f, "stop audio", FOXAPI_RX_RECORDING_AUDIO);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_DECODER_STOP_ALL, 0))));
    CHECK(waitFor([&a] { return A::runnerActive(a) == 0u; }));
    step(a, f, "stop every decoder", FOXAPI_RX_DECODER_ACTIVE);
    // Running with nothing fed: the published count is the window's own, 0.
    {
        const cascade::core::PublishedState s = A::state(a);
        std::printf("      running, no decoder fed: published %u, window %zu, fitted %u\n",
                    s.rx.decodersRunning, A::fedDecoders(a), s.rx.decodersFitted);
        CHECK(s.rx.decodersRunning == A::fedDecoders(a));
        CHECK(s.rx.decodersRunning == 0u);
        CHECK(s.rx.decodersFitted == A::loadedDecoders(a));
    }
    {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 1);
        c.num[0] = 144.0e6;
        c.num[1] = 146.0e6;
        c.num[2] = 12.5e3;
        CHECK(ok(A::apply(a, c)));
    }
    step(a, f, "scanner on", FOXAPI_RX_SCANNER_ACTIVE);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 0))));
    step(a, f, "scanner off", FOXAPI_RX_SCANNER_ACTIVE);
    CHECK(ok(A::apply(a, cmd::makeInt(FOXAPI_OP_RUN, 0))));
    step(a, f, "stop", FOXAPI_RX_RUNNING);

    // The transmitter (a fake IIO daemon on loopback) and the page.
    cascade::test::FakeTxIiod d;
    std::string err;
    CHECK(d.start(err));
    cascade::test::stockTxBoard(d);
    const std::string args = "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(d.port()));
    CHECK(ok(A::apply(a, textCmd(FOXAPI_OP_TX_OPEN, args))));
    CHECK(A::haveTx(a));
    step(a, f, "a transmitter opens", FOXAPI_RX_TX_AVAILABLE);
    A::setTransmitPageOpen(a, true);
    step(a, f, "the Transmit page opens (consent)", FOXAPI_RX_TX_REMOTE_ARMED);
    A::setTransmitPageOpen(a, false);
    step(a, f, "the Transmit page closes", FOXAPI_RX_TX_REMOTE_ARMED);
    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_TX_CLOSE))));
    step(a, f, "the transmitter closes", FOXAPI_RX_TX_AVAILABLE);
}

// --- [3] the browser's rows and the ids they are resolved by: one block ------------------

void webRowsFollowTheirBlock(AppWindow& a) {
    std::printf("[3] a web bookmark row resolves through the block the page was served from\n");
    CHECK(ok(A::apply(a, textCmd(FOXAPI_OP_SELECT_SOURCE, "siggen"))));
    CHECK(ok(A::apply(a, cmd::makeNum(FOXAPI_OP_SET_CENTRE, 100.0e6))));
    CHECK(ok(A::apply(a, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 0.0))));
    A::addBookmark(a, "Row A", 100.3e6, "AM");
    A::addBookmark(a, "Row B", 100.5e6, "NFM");
    A::publish(a);
    const cascade::net::RadioStatus s1 = A::webStatus(a);
    CHECK(s1.bookmarks.size() == 2u && s1.bookmarks[0].name == "Row A");

    // A reader is inside both locks as the next publish arrives: its block is
    // held back. The desktop has added D, which sorts in above A.
    cascade::core::ReceiverSnapshot& snap = A::snapshot(a);
    {
        std::unique_lock<std::mutex> held = snap.holdSwapLockForTest();
        std::unique_lock<std::mutex> heldHandoff = snap.holdHandoffLockForTest();
        A::addBookmark(a, "Row D", 100.1e6, "USB");
        A::publish(a);
    }
    CHECK(snap.installPending());
    const cascade::net::RadioStatus s2 = A::webStatus(a);
    std::printf("      /api/status row 0 while the new block waits: %s\n",
                s2.bookmarks.empty() ? "(none)" : s2.bookmarks[0].name.c_str());
    CHECK(!s2.bookmarks.empty() && s2.bookmarks[0].name == "Row A");
    // The page tunes the row it shows: 0 is A (AM).
    cascade::net::ControlRequest tune;
    tune.bookmarkTune = 0;
    A::webRequest(a, tune);
    std::printf("      web tune of row 0 -> mode index %d (AM is 2, USB 4)\n", A::modeIndex(a));
    CHECK(A::modeIndex(a) == 2);

    // The writer's next pass lands the block, nothing else changing; row 0
    // is D now, for the page and for the map alike.
    CHECK(snap.retryInstall());
    const cascade::net::RadioStatus s3 = A::webStatus(a);
    CHECK(!s3.bookmarks.empty() && s3.bookmarks[0].name == "Row D");
    A::webRequest(a, tune);
    CHECK(A::modeIndex(a) == 4);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_snapshot_app\n");
    isolate();
    A::installHooks();
    ImGui::CreateContext();
    {
        AppWindow app;
        decodersAndCost(app);
        walkingOneWindow(app);
        webRowsFollowTheirBlock(app);
    }
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_snapshot_app");
}
