// test_state_snapshot_golden.cpp - what every READER of the receiver's state
// is told, byte for byte, against a record taken from the code before engine
// stage 2 (docs/engine-stage2.md).
//
// WHAT IT PINS. Stage 2 moved every reader of the receiver's state onto one
// snapshot published once a frame: the web server's GET /api/status (and the
// /api/control key gate, which reads transmitAvailable from the same status),
// CAT's replies (executeCatLine), and the plugin host API's get_state /
// get_gain / get_sample_rates and the device checks in its requests. None of
// that may change what a reader is told. So the real application (fake
// radio, no config, scratch folders - the harness of test_apply_command) is
// driven through a fixed list of states, and for each one this renders:
//
//   - the /api/status body (net::statusJson, the route's own renderer),
//   - the reply to every CAT query and the control request of every CAT set,
//   - every plugin read, and the answer to a set of plugin requests,
//
// and compares the text with tests/golden/state_snapshot.golden, which was
// WRITTEN BY THE CODE BEFORE STAGE 2 (commit "Engine stage 2, capture", on
// top of bca426f) with FOXSDR_WRITE_GOLDEN=1. The only lines of this file
// that differ between that capture and stage 2 are the three in
// AppWindowTestAccess marked THE PATH UNDER TEST, and a report-only timing
// of the stage-2 publish taken after the last record.
//
// WHAT IS MASKED, and why. Two sets of figures are replaced by a fixed word
// before rendering, in the capture and now alike, because they are not a
// function of the receiver's state but of the machine and the moment: the
// audio sink's health (underruns, priming callbacks, ring level and
// capacity - the sink is this machine's real audio device), and, once the
// receiver has run, what it MEASURED (signal level, stereo pilot, auto-notch
// readout, RDS; the plugin's signalDb/sMeter and its squelch-open and stereo
// bits). The mapping of those fields is pinned instead by
// test_receiver_snapshot's field-by-field compose test.
//
// Doubles are written as their bit patterns, so "identical" means identical
// bits and not merely the same when printed.
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
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
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
#include "core/plugin_api.hpp"
#include "gui/app_window.hpp"
#include "imgui.h"
#include "net/cat_protocol.hpp"
#include "net/web_server.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;

std::filesystem::path g_scratch;

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
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_state_golden_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch / "plugins");
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string pluginDirHook() { return (g_scratch / "plugins").string(); }

// A radio that hands over zeros, so nothing it "receives" moves a figure.
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
        lna.minDb = 0.0;
        lna.maxDb = 40.0;
        lna.stepDb = 1.0;
        cascade::source::GainInfo vga = lna;
        vga.name = "VGA";
        vga.maxDb = 15.0;
        return {lna, vga};
    }
    bool setGainDb(const std::string& name, double db) override {
        (name == "LNA" ? lna_ : vga_) = std::round(db);
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

template <typename Fn>
bool waitFor(Fn fn, int ms = 20000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!fn()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

constexpr const char* kPluginKey = "golden_reader.dll";

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeNativeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &fakeSoapyScan;
        cascade::engine::Engine::testHooks_.pluginDir = &pluginDirHook;
    }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) { return a.applyCommand(c, {}); }
    static FoxCommandResult apply(AppWindow& a, const cmd::QueuedCommand& q) {
        return a.applyCommand(q.c, q.longText);
    }
    static bool waitOpen(AppWindow& a) {
        return waitFor([&a] {
            a.engine_.pollSourceAsync();
            return !a.engine_.deviceOpenPending_;
        });
    }
    static bool waitSoapy(AppWindow& a) {
        return waitFor([&a] {
            a.engine_.pollSourceAsync();
            return !a.engine_.soapyScanPending_;
        });
    }
    static bool running(AppWindow& a) { return a.engine_.pipeline_.running(); }
    // A RADIO'S RETUNE IS PACED (the retune coalescer): the command queues it
    // and the frame loop lands it with pollPendingRetune, every frame. The
    // drive has no frame loop, so it does the same until the centre is there.
    static bool settleRetune(AppWindow& a, double hz) {
        return waitFor([&a, hz] {
            a.engine_.pollPendingRetune();
            return a.engine_.pipeline_.activeSource().centerFrequencyHz() == hz;
        });
    }
    static cascade::core::PluginApiCore& api(AppWindow& a) { return a.engine_.pluginUi_.api(); }

    // State no command sets: the strings a reader passes through verbatim.
    static void fixRecordDir(AppWindow& a) { a.engine_.recordDir_ = "/golden/recordings"; }
    static void setStrings(AppWindow& a) {
        a.engine_.sourceError_ = "golden: the source said no";
        a.engine_.recordError_ = "golden: the disk is full";
        a.engine_.recordNotice_ = "golden: the take ended on purpose";
        a.engine_.catalogStatus_ = "golden: 3 plugins";
        a.engine_.catalogError_ = "golden: catalogue error";
        a.engine_.installReport_ = "golden: installed";
        a.engine_.installError_ = "golden: install failed";
        a.tunerStyle_ = cascade::gui::TunerStyle::Neon;
        cascade::core::DecodedLine d;
        d.plugin = "Golden decoder";
        d.text = "a decoded line";
        a.engine_.decoderLog_.push_back(d);
    }
    static void setTransmitPageOpen(AppWindow& a, bool on) { a.engine_.transmitOpen_ = on; }

    // ===== THE PATH UNDER TEST ================================================
    // Stage 2: the one publish, and exactly what the web server's and CAT's
    // providers call. (At the capture these three were the two per-frame
    // publishes - publishPluginApiState and publishWebSnapshot - and a copy
    // of webStatus_ under webMutex_ for both providers.) The plugin reads
    // below need no switch: they go through PluginApiCore either way.
    static void publish(AppWindow& a) { a.engine_.publishReceiverState(); }
    static cascade::net::RadioStatus webStatus(AppWindow& a) { return a.webStatusNow(); }
    static cascade::net::RadioStatus catStatus(AppWindow& a) { return a.catStatusNow(); }
    // Engine stage 2's cost, measured on the real window (report only).
    static double publishNs(AppWindow& a, int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) { a.engine_.publishReceiverState(); }
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    }
    // ==========================================================================
};

}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

std::vector<std::string> g_out;
bool g_measured = false;  // the receiver has run: its measurements are masked

std::string hex(double v) {
    std::uint64_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(u));
    return b;
}

std::string printable(const std::string& s) {
    std::string o;
    for (const unsigned char c : s) {
        if (c == '\n') {
            o += "\\n";
        } else if (c == '\r') {
            o += "\\r";
        } else if (c < 0x20 || c == 0x7f) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\x%02x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}

void mask(cascade::net::RadioStatus& s) {
    s.audioUnderruns = 0;
    s.audioPrimingCallbacks = 0;
    s.audioRingMs = 0.0;
    s.audioRingCapacityMs = 0.0;
    if (!g_measured) { return; }
    s.signalDb = -1.0f;
    s.stereoActive = false;
    s.pilotLocked = false;
    s.autoNotchEngaged = false;
    s.autoNotchFreqHz = 0.0;
    s.rdsSynced = false;
    s.rdsPiValid = false;
    s.rdsPi = 0;
    s.rdsPsValid = false;
    s.rdsPs.clear();
    s.rdsRadioText.clear();
    s.rdsPty = 0;
    s.rdsTp = false;
    s.rdsTa = false;
    s.rdsGroups = 0;
    s.rdsErrors = 0;
}

void recordWeb(AppWindow& a) {
    cascade::net::RadioStatus s = A::webStatus(a);
    mask(s);
    g_out.push_back("web " + cascade::net::statusJson(s, [](std::size_t) { return false; }));
}

void recordCat(AppWindow& a) {
    static const char* const kLines[] = {
        "f",           "m",          "v",           "s",           "t",
        "\\get_freq",  "\\get_mode", "\\get_vfo",   "\\chk_vfo",   "\\get_powerstat",
        "\\dump_state", "F 145012500", "F 433920000", "F 100000000", "M USB 2400",
        "M WFM 0",     "M CW 500",   "M FM -1",     "V VFOA",      "T 1",
        "S 0 VFOA",    "q",
    };
    for (const char* line : kLines) {
        // Asked afresh for every line, as the CAT server asks per request.
        const cascade::net::RadioStatus s = A::catStatus(a);
        const cascade::net::CatResult r = cascade::net::executeCatLine(line, s);
        std::string o = std::string("cat [") + printable(line) + "] reply=" + printable(r.reply) +
                        " quit=" + (r.quit ? "1" : "0") + " control=" + (r.hasControl ? "1" : "0");
        if (r.hasControl) {
            const cascade::net::ControlRequest& c = r.control;
            if (c.running) { o += std::string(" running=") + (*c.running ? "1" : "0"); }
            if (c.centerHz) { o += " centre=" + hex(*c.centerHz); }
            if (c.vfoOffsetHz) { o += " vfo=" + hex(*c.vfoOffsetHz); }
            if (c.mode) { o += " mode=" + std::to_string(static_cast<int>(*c.mode)); }
            if (c.bandwidthHz) { o += " bw=" + hex(*c.bandwidthHz); }
            if (c.transmitPtt) { o += std::string(" ptt=") + (*c.transmitPtt ? "1" : "0"); }
        }
        g_out.push_back(o);
    }
}

void recordPlugin(AppWindow& a) {
    cascade::core::PluginApiCore& api = A::api(a);
    cascade::core::PluginApiClient& c = api.client(kPluginKey, "Golden reader");
    api.setLiveSet({kPluginKey});
    api.setSettingsGranted(kPluginKey, true);
    api.setTuneGranted(kPluginKey, true);

    CascadeReceiverState st{};
    st.structSize = sizeof(st);
    const std::int32_t rc = api.getState(c, &st);
    std::uint32_t flags = st.flags;
    double sig = st.signalDb;
    double meter = st.sMeter;
    if (g_measured) {
        flags &= ~(CASCADE_STATE_SQUELCH_OPEN | CASCADE_STATE_STEREO);
        sig = -1.0;
        meter = -1.0;
    }
    char b[1024];
    std::snprintf(b, sizeof(b),
                  "plugin state rc=%d size=%u flags=%08x seq=%llu tune=%llu mode=%llu device=%llu "
                  "audio=%llu centre=%s vfo=%s tuned=%s rate=%s bw=%s sq=%s vol=%s sig=%s meter=%s "
                  "demod=%u gains=%u name=[%s]",
                  rc, st.structSize, flags, static_cast<unsigned long long>(st.seq),
                  static_cast<unsigned long long>(st.tuneSeq),
                  static_cast<unsigned long long>(st.modeSeq),
                  static_cast<unsigned long long>(st.deviceSeq),
                  static_cast<unsigned long long>(st.audioSeq), hex(st.centreHz).c_str(),
                  hex(st.vfoOffsetHz).c_str(), hex(st.tunedHz).c_str(), hex(st.sampleRateHz).c_str(),
                  hex(st.bandwidthHz).c_str(), hex(st.squelchDb).c_str(), hex(st.volume).c_str(),
                  hex(sig).c_str(), hex(meter).c_str(), st.demodMode, st.gainCount,
                  printable(std::string(st.deviceName, strnlen(st.deviceName, sizeof(st.deviceName))))
                      .c_str());
    g_out.push_back(b);

    for (std::uint32_t i = 0; i <= st.gainCount + 1u; ++i) {
        CascadeGainInfo gi{};
        gi.structSize = sizeof(gi);
        const std::int32_t grc = api.getGain(c, i, &gi);
        std::snprintf(b, sizeof(b),
                      "plugin gain %u rc=%d size=%u unit=%u name=[%s] min=%s max=%s step=%s cur=%s", i,
                      grc, gi.structSize, gi.unit,
                      printable(std::string(gi.name, strnlen(gi.name, sizeof(gi.name)))).c_str(),
                      hex(gi.minDb).c_str(), hex(gi.maxDb).c_str(), hex(gi.stepDb).c_str(),
                      hex(gi.currentDb).c_str());
        g_out.push_back(b);
    }

    double rates[40] = {};
    const std::int32_t n = api.getSampleRates(c, rates, 40);
    std::string r = "plugin rates n=" + std::to_string(n);
    for (std::int32_t i = 0; i < n && i < 40; ++i) { r += " " + hex(rates[i]); }
    g_out.push_back(r);
    std::snprintf(b, sizeof(b), "plugin rates-count-only n=%d", api.getSampleRates(c, nullptr, 0));
    g_out.push_back(b);

    // The requests whose answer depends on the snapshot (a radio open, AGC
    // supported, the gain stage and its range), and two that do not.
    using K = cascade::core::PluginControl::Kind;
    struct Ask {
        const char* what;
        K kind;
        double value;
        const char* gain;
        bool flag;
    };
    const Ask asks[] = {
        {"rate", K::SampleRate, 2.048e6, "", false},  {"gain-lna", K::Gain, 20.0, "LNA", false},
        {"gain-high", K::Gain, 999.0, "LNA", false},  {"gain-nope", K::Gain, 3.0, "NOPE", false},
        {"agc", K::DeviceAgc, 0.0, "", true},         {"volume", K::Volume, 0.25, "", false},
        {"frequency", K::Frequency, 145.5e6, "", false},
    };
    for (const Ask& q : asks) {
        cascade::core::PluginControl pc;
        pc.kind = q.kind;
        pc.value = q.value;
        pc.flag = q.flag;
        std::snprintf(pc.gainName, sizeof(pc.gainName), "%s", q.gain);
        std::snprintf(b, sizeof(b), "plugin ask %s rc=%d", q.what, api.requestControl(c, pc));
        g_out.push_back(b);
    }
    std::vector<cascade::core::PluginControl> drained;
    api.takeControls(drained);
}

void record(AppWindow& a, const char* name, bool publish = true) {
    std::printf("  state: %s\n", name);
    g_out.push_back(std::string("== ") + name);
    if (publish) { A::publish(a); }
    recordWeb(a);
    recordCat(a);
    recordPlugin(a);
}

FoxCommand num(std::uint32_t op, double n0, double n1 = 0.0) { return cmd::makeNum(op, n0, n1); }
FoxCommand ints(std::uint32_t op, std::int64_t v0, std::int64_t v1 = 0) {
    return cmd::makeInt(op, v0, v1);
}
cmd::QueuedCommand text(std::uint32_t op, const std::string& t, std::int64_t v0 = 0,
                        std::int64_t v1 = 0, double n0 = 0.0) {
    return cmd::makeText(op, t, v0, v1, n0);
}

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

void drive(AppWindow& a) {
    A::fixRecordDir(a);
    // Before anything was ever published: what a reader got then.
    record(a, "never published", false);
    record(a, "start");

    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_CENTRE, 145.0e6))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_VFO_OFFSET, 12500.0))));
    record(a, "tuned");

    for (std::int64_t m = FOXAPI_DEMOD_NFM; m <= FOXAPI_DEMOD_RAW; ++m) {
        CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MODE, m))));
        const std::string n = "mode " + std::to_string(m);
        record(a, n.c_str());
    }
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_NFM))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_BANDWIDTH, 9000.0))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_SQUELCH, -63.5))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_VOLUME, 0.37))));
    record(a, "channel");

    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MUTED, 1))));
    record(a, "muted");
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_MUTED, 0))));

    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEEMPHASIS, 1))));
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_STEREO, 0))));
    {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NR, 1, 1);
        c.num[0] = 0.7;
        CHECK(ok(A::apply(a, c)));
    }
    {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NOTCH, 1, 1);
        c.num[0] = 1234.0;
        c.num[1] = 17.0;
        CHECK(ok(A::apply(a, c)));
    }
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_AUTO_NOTCH, 1))));
    record(a, "audio dsp");

    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_DISPLAY_RANGE, -121.0, -11.0))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_POSITION, 53.8, -1.55))));
    A::setStrings(a);
    A::setTransmitPageOpen(a, true);  // no radio behind it: still not available
    record(a, "display, position, strings");

    CHECK(ok(A::apply(a, cmd::make(FOXAPI_OP_SCAN_DEVICES))));
    CHECK(A::waitSoapy(a));
    record(a, "scanned");

    CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "rtlsdr:serial=0001"))));
    CHECK(A::waitOpen(a));
    record(a, "radio open");

    CHECK(ok(A::apply(a, text(FOXAPI_OP_SET_GAIN, "LNA", 0, 0, 27.0))));
    CHECK(ok(A::apply(a, text(FOXAPI_OP_SET_GAIN, "VGA", 0, 0, 4.0))));
    CHECK(ok(A::apply(a, text(FOXAPI_OP_SET_ANTENNA, "B"))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_SAMPLE_RATE, 1.024e6))));
    record(a, "radio set");
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_SET_DEVICE_AGC, 1))));
    record(a, "radio agc");

    CHECK(ok(A::apply(a, text(FOXAPI_OP_BOOKMARK_ADD, "Golden one"))));
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_CENTRE, 433.9e6))));
    CHECK(A::settleRetune(a, 433.9e6));  // "Golden two" is added AT 433.9 MHz
    CHECK(ok(A::apply(a, text(FOXAPI_OP_BOOKMARK_ADD, "Golden two"))));
    {
        FoxCommand c = cmd::make(FOXAPP_OP_SCANNER_RANGE);
        c.ival[0] = 1 | 2 | 4;
        c.num[0] = 150.0e6;
        c.num[1] = 151.0e6;
        c.num[2] = 25.0e3;
        CHECK(ok(A::apply(a, c)));
    }
    record(a, "bookmarks, scanner range");

    CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 1))));
    CHECK(A::running(a));
    g_measured = true;
    record(a, "running");
    CHECK(ok(A::apply(a, num(FOXAPI_OP_SET_CENTRE, 145.0e6))));
    CHECK(A::settleRetune(a, 145.0e6));  // the retune lands while running
    record(a, "running, retuned");
    CHECK(ok(A::apply(a, ints(FOXAPI_OP_RUN, 0))));
    record(a, "stopped");

    CHECK(ok(A::apply(a, text(FOXAPI_OP_SELECT_SOURCE, "siggen"))));
    record(a, "back to the generator");
}

std::string goldenPath() { return GOLDEN_FILE; }

void compareOrWrite() {
    const char* w = std::getenv("FOXSDR_WRITE_GOLDEN");
    if (w != nullptr && std::string(w) == "1") {
        std::ofstream f(goldenPath(), std::ios::binary);
        for (const std::string& l : g_out) { f << l << "\n"; }
        std::printf("  WROTE %zu lines to %s\n", g_out.size(), goldenPath().c_str());
        CHECK(f.good());
        return;
    }
    std::ifstream f(goldenPath(), std::ios::binary);
    CHECK(f.good());
    std::vector<std::string> want;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }  // a CRLF checkout
        want.push_back(line);
    }
    std::printf("  golden: %zu lines, now: %zu lines\n", want.size(), g_out.size());
    CHECK(want.size() == g_out.size());
    int shown = 0;
    std::size_t differ = 0;
    const std::size_t n = std::min(want.size(), g_out.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (want[i] == g_out[i]) { continue; }
        ++differ;
        if (shown++ < 4) {
            std::printf("  line %zu differs\n    golden: %s\n    now:    %s\n", i + 1,
                        want[i].c_str(), g_out[i].c_str());
        }
    }
    std::printf("  %zu of %zu lines differ\n", differ, n);
    CHECK(differ == 0u);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_state_snapshot_golden\n");
    isolate();
    A::installHooks();
    ImGui::CreateContext();
    {
        AppWindow app;
        drive(app);
        // After every record, so it cannot move a counter the golden holds.
        std::printf("  window publish (the whole of publishReceiverState): %.0f ns/frame\n",
                    A::publishNs(app, 2000));
    }
    compareOrWrite();
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_state_snapshot_golden");
}
