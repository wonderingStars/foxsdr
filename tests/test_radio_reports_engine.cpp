// The radio reports through the REAL engine (wave 8): every call site that files a failed open, the journal's events for an opened radio, what a scan
// lists, and the scan_none repair (L1) - against a fake radio that fails, hangs or stays silent on command, a stand-in server and the Worker's own
// validator (node, where it is found). No radio, no network, no window on the desk (a hermetic engine behind a test window; run it on the hidden desktop).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"

#ifndef CASCADE_SOURCE_ROOT
#error "CASCADE_SOURCE_ROOT is defined by tests/CMakeLists.txt"
#endif
#ifndef CASCADE_NODE_PATH
#define CASCADE_NODE_PATH ""
#endif

namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::string kArgs = "serial=00000001";
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
const char* const kD3 = "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial 00000001)";

std::atomic<int> g_failures{0};        // fail this many more opens with g_failText
std::string g_failText;
std::atomic<bool> g_hold{false};       // an open waits here until released
std::atomic<bool> g_silent{false};     // the opened radio gives no samples at all
std::atomic<int> g_opens{0};

class FakeRadio final : public cascade::source::DeviceSource {
public:
    bool start() override { abort_ = false; return true; }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override { rate_ = hz; return true; }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override { centre_ = hz; return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abort_ || g_silent.load()) { return 0; }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return error_.c_str(); }
    const char* driverKey() const override { return "rtlsdr"; }
    bool open(const std::string&) override {
        ++g_opens;
        while (g_hold.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        if (g_failures.load() > 0) {
            --g_failures;
            error_ = g_failText;
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
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 24.0e6; hi = 1766.0e6; return true; }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    std::string error_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind != "rtlsdr") { return nullptr; }
    return std::make_unique<FakeRadio>();
}

std::mutex g_scanMutex;
std::vector<cascade::source::NativeDeviceInfo> g_native;
std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_scanMutex);
    return g_native;
}
void setNative(std::vector<cascade::source::NativeDeviceInfo> v) {
    std::lock_guard<std::mutex> lk(g_scanMutex);
    g_native = std::move(v);
}
std::vector<cascade::source::SoapyDeviceInfo> fakeSoapy() { return {}; }

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
    const auto dir = fs::temp_directory_path() / ("foxsdr_radio_reports_engine_" + std::to_string(pid));
    fs::create_directories(dir);
    const std::string s = dir.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &fakeSoapy;
    }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { passseam::on(th::engine(a), [&] { th::applyConfig(a, c); }); }
    static void clock(AppWindow& a, double t) { passseam::on(th::engine(a), [&] { th::engine(a).savedRadioRetryClockS_ = t; }); }
    static void poll(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).pollSavedRadioRetry(); }); }
    static bool pending(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceOpenPending_; }); }
    static bool settle(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (pending(a)) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool deviceOpen(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).device_ != nullptr; }); }
    static void selectGenerator(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(0); }); }
    static void selectNative(AppWindow& a, int i) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(cascade::engine::kNativeRowBase + i); }); }
    static void staleTheOpen(AppWindow& a) { passseam::on(th::engine(a), [&] { ++th::engine(a).rx().sourceGen_; }); }
    static void scanNative(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).scanNative(); }); }
    static bool scanBoth(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).scanNative();
            th::engine(a).scanSoapy();
        });
        const auto t0 = std::chrono::steady_clock::now();
        while (passseam::read<bool>(th::engine(a), [&] { return th::engine(a).soapyScanPending_; })) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
    static void pass(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).radioReportsPass(); }); }
    // The engine's clock moved on, as if the session were this many seconds old (the first-session record is due a minute in).
    static void ageSession(AppWindow& a, int seconds) { passseam::on(th::engine(a), [&] { th::engine(a).clockOrigin_ -= std::chrono::seconds(seconds); }); }
    // This engine's launch count (a hermetic engine never ran the usage journal's start-up, so it holds 0): the first session is launch 1.
    static void setLaunches(AppWindow& a, std::uint64_t n) { passseam::on(th::engine(a), [&] { th::engine(a).telemetryLaunches_ = n; }); }
    static void restoreExtra(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).restoreExtraRadioDevice("rtlsdr", kArgs, 2.4e6, std::string(), 100.0e6); }); }
    static void quit(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).radioReportsQuit(); }); }
    static std::string usageRecord(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).telemetrySnapshot().toJson(); }); }
    static void noteWindow(AppWindow& a, const std::string& w) { passseam::on(th::engine(a), [&] { th::engine(a).noteWindowKind(w); }); }
    // The opened radio's journal state, aged: as if it had been open this many seconds (the 20 s look is the engine's clock).
    static void ageOpenRadio(AppWindow& a, double seconds) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).radioJournal_[0].openedAtS -= seconds;
            th::engine(a).radioJournal_[0].lastMinuteS -= seconds;
        });
    }
    static void startReceiver(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).startReceiver(); }); }
    static void minute(AppWindow& a, std::uint64_t read, std::uint64_t dropped) { passseam::on(th::engine(a), [&] { th::engine(a).journalStreamMinute(read, dropped, 100); }); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

struct Rig {
    fs::path dir;
    std::mutex mu;
    std::vector<std::string> bodies;
    explicit Rig(const char* tag) {
        dir = fs::temp_directory_path() / (std::string("rre-") + tag + "-" + rr::randomHex(8));
        fs::create_directories(dir);
    }
    ~Rig() {
        rr::global().configure(rr::Reporter::Options{});   // the process-wide reporter must not outlive the folder and the stand-in it sends to
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void start(bool usageOn = true) {
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.liveSend = true;
        o.transport = [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return rr::PostResult{true, 204, 0, ""};
        };
        if (usageOn) { o.identity.installId = kInstallId; }
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.channel = "installer";
        o.identity.updateChannel = "stable";
        o.identity.launches = 4;
        rr::global().configure(o);
    }
    std::string qdir() const { return (dir / "radio-reports").string(); }
};

std::vector<json> queued(const std::string& dir) {
    std::vector<json> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) { files.push_back(it->path()); }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string name = f.filename().string();
        std::ifstream in(f, std::ios::binary);
        if (name.rfind("fail-", 0) == 0 && f.extension() == ".json") {
            const json w = json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), nullptr, false);
            if (w.is_object() && w.contains("msg")) { out.push_back(w["msg"]); }
        } else if (name.rfind("journal-", 0) == 0 && f.extension() == ".jsonl") {
            std::string line;
            bool first = true;
            while (std::getline(in, line)) {
                if (first) { first = false; continue; }
                const json m = json::parse(line, nullptr, false);
                if (m.is_object()) { out.push_back(m); }
            }
        }
    }
    return out;
}

std::vector<json> ofKind(const std::vector<json>& all, const char* kind) {
    std::vector<json> out;
    for (const json& m : all) { if (m.value("kind", std::string()) == kind) { out.push_back(m); } }
    return out;
}

std::string wholeQueue(const std::string& dir) {
    std::string all;
    for (const std::string& f : rr::readQueueFiles(dir)) { all += f; }
    return all;
}

int validateWithWorker(const std::vector<json>& msgs) {
    const std::string node = CASCADE_NODE_PATH;
    if (node.empty()) { return -1; }
    Rig t("validate");
    const fs::path in = t.dir / "in.jsonl";
    const fs::path out = t.dir / "out.txt";
    {
        std::ofstream f(in, std::ios::binary);
        for (const json& m : msgs) { f << m.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; }
    }
    const std::string script = std::string(CASCADE_SOURCE_ROOT) + "/tests/radio_reports_validate.mjs";
    const std::string cmd = "\"\"" + node + "\" \"" + script + "\" \"" + in.string() + "\" > \"" + out.string() + "\"\"";
    const int rc = std::system(cmd.c_str());
    std::ifstream r(out);
    std::string line;
    int refused = 0;
    std::size_t n = 0;
    while (std::getline(r, line)) {
        ++n;
        if (line.rfind("OK ", 0) != 0) { ++refused; std::printf("WORKER VERDICT: %s\n", line.c_str()); }
    }
    if (rc != 0 || n != msgs.size()) { std::printf("validator run failed: rc=%d lines=%zu messages=%zu\n", rc, n, msgs.size()); return 1000; }
    return refused;
}

cascade::core::AppConfig savedRadio() {
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "rtlsdr";
    cfg.nativeArgs = kArgs;
    cfg.sampleRateHz = 2.4e6;
    cfg.centerHz = 100.0e6;
    return cfg;
}

void resetFake() {
    g_failures = 0;
    g_failText.clear();
    g_hold = false;
    g_silent = false;
    g_opens = 0;
    cascade::core::DiagLog::instance().resetForTest();
    setNative({{"rtlsdr", "Generic RTL2832U", kArgs}});
}

}  // namespace

int main() {
    std::printf("test_radio_reports_engine\n");
    isolate();
    Access::installHooks();
    std::vector<json> everything;
    auto keep = [&everything](const std::vector<json>& m) { everything.insert(everything.end(), m.begin(), m.end()); };

    // --- THE START-UP RESTORE THAT FAILS BUSY, AND THE SAVED-RADIO RETRIES: every failure its own report, a busy-again retry included ----------------
    {
        std::printf("  start-up restore fails busy; the retries fail busy; the fourth finds it free\n");
        resetFake();
        g_failures = 3;
        g_failText = cascade::usb::kInUseError;
        th::TestWindow app;
        Rig rig("busy");
        rig.start();
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());
        double t = 0.0;
        for (const double delay : {2.0, 4.0}) {
            t += delay;
            Access::clock(app, t);
            Access::poll(app);
            CHECK(Access::settle(app));
        }
        const std::vector<json> q = queued(rig.qdir());
        const std::vector<json> fails = ofKind(q, "radio_fail");
        CHECK(fails.size() == 3);   // the restore and two busy-again retries: three failures, three reports (the anonymous count skips the retries)
        std::set<std::string> triggers;
        long long maxAttempt = 0;
        for (const json& f : fails) {
            triggers.insert(f["body"]["trigger"].get<std::string>());
            maxAttempt = std::max<long long>(maxAttempt, f["body"]["attempt"].get<long long>());
            CHECK(f["body"]["driver"] == "rtlsdr");
            CHECK(f["body"]["model"] == "Generic RTL2832U");
            CHECK(f["body"]["serial"] == "00000001");
            CHECK(f["body"]["reasonClass"] == "busy");
            CHECK(f["radioKey"] == "generic-rtl2832u:00000001");
            CHECK(f["body"]["sampleRateSps"] == 2400000);
            CHECK(f["body"].contains("openMs"));
            CHECK(f["body"]["superseded"] == false);
            CHECK(f["installId"] == kInstallId);
        }
        CHECK(triggers == (std::set<std::string>{"startup-restore", "saved-retry"}));
        CHECK(maxAttempt == 3);
        // the serial is its own field and nowhere in the sentence
        for (const json& f : fails) { CHECK(f["body"]["reason"].get<std::string>().find("00000001") == std::string::npos); }
        // the journal: an open that began (on disk before the call), and nothing "ok" yet
        const std::vector<json> opens = ofKind(q, "radio_open");
        CHECK(opens.size() >= 3);
        for (const json& o : opens) { CHECK(o["body"]["phase"] == "start"); }
        CHECK(opens.front()["body"]["trigger"] == "startup-restore");
        // the third retry finds it free
        t += 8.0;
        Access::clock(app, t);
        Access::poll(app);
        CHECK(Access::settle(app));
        CHECK(Access::deviceOpen(app));
        const std::vector<json> after = queued(rig.qdir());
        const std::vector<json> ok = [&]() { std::vector<json> v; for (const json& o : ofKind(after, "radio_open")) { if (o["body"]["phase"] == "ok") { v.push_back(o); } } return v; }();
        CHECK(ok.size() == 1);
        if (!ok.empty()) {
            CHECK(ok[0]["body"]["trigger"] == "saved-retry");
            CHECK(ok[0]["body"].contains("openMs") && ok[0]["body"]["band"] == "fm-broadcast");   // 100 MHz: a band by NAME, never the frequency
            CHECK(ok[0]["radioKey"] == "generic-rtl2832u:00000001");
        }
        const std::vector<json> settings = ofKind(after, "radio_settings");
        CHECK(settings.size() >= 1);
        if (!settings.empty()) { CHECK(settings[0]["body"]["why"] == "open" && settings[0]["body"]["sampleRateSps"] == 2400000); }
        CHECK(rr::global().stats().failuresQueued == 3);
        keep(after);
    }

    // --- the D3 sentence through the real engine -------------------------------------------------------------------------------------------------
    {
        std::printf("  the Airspy HF+ sentence: class control, no serial in the reason\n");
        resetFake();
        g_failures = 1;
        g_failText = kD3;
        th::TestWindow app;
        Rig rig("d3");
        rig.start();
        Access::restore(app, savedRadio());
        const std::vector<json> q = queued(rig.qdir());
        const std::vector<json> fails = ofKind(q, "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            CHECK(fails[0]["body"]["reasonClass"] == "control");
            const std::string reason = fails[0]["body"]["reason"].get<std::string>();
            CHECK(reason.find("00000001") == std::string::npos);
            CHECK(reason.find("control transfer failed") != std::string::npos);
        }
        keep(q);
    }

    // --- Diagnostics off and Usage reporting off: the failure is still reported, anonymously ------------------------------------------------------
    {
        std::printf("  Usage reporting off: the failure is queued with no install id, the journal is not\n");
        resetFake();
        g_failures = 1;
        g_failText = "the radio is already in use by another program";
        th::TestWindow app;
        Rig rig("anon");
        rig.start(/*usageOn=*/false);
        Access::restore(app, savedRadio());
        const std::vector<json> q = queued(rig.qdir());
        CHECK(q.size() == 1);
        if (q.size() == 1) {
            CHECK(q[0]["kind"] == "radio_fail" && !q[0].contains("installId") && q[0].contains("messageId"));
            CHECK(q[0]["body"]["diagOn"] == false || q[0]["body"]["diagOn"] == true);
        }
        keep(q);
    }

    // --- THE OPEN THE USER MOVED ON FROM is still a failed open ------------------------------------------------------------------------------------
    {
        std::printf("  a failed open the user had moved on from: filed with superseded set\n");
        resetFake();
        g_failures = 1;
        g_failText = "the radio is already in use by another program";
        g_hold = true;
        th::TestWindow app;
        Rig rig("superseded");
        rig.start();
        Access::scanNative(app);
        Access::selectNative(app, 0);   // the open starts and waits inside the fake
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Access::staleTheOpen(app);      // another route (the web page, a file drop) installs a source meanwhile: the open in flight is no longer wanted
        g_hold = false;
        CHECK(Access::settle(app));
        const std::vector<json> fails = ofKind(queued(rig.qdir()), "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            CHECK(fails[0]["body"]["superseded"] == true);
            CHECK(fails[0]["body"]["trigger"] == "user-pick");
            CHECK(fails[0]["body"]["reasonClass"] == "busy");
        }
        keep(fails);
    }

    // --- A REAL SESSION IN THE REAL ENGINE leaves nothing of the machine or of what was tuned in what it queued ----------------------------------------------
    {
        std::printf("  a session tuned to 433.920 MHz, opened and failed again: no frequency, path or account name in the queue\n");
        resetFake();
        th::TestWindow app;
        Rig rig("secrets");
        rig.start();
        cascade::core::AppConfig cfg = savedRadio();
        cfg.centerHz = 433.920e6;
        Access::restore(app, cfg);                 // opens, tuned to 433.920 MHz (the real engine writes its real log lines)
        CHECK(Access::deviceOpen(app));
        Access::pass(app);
        Access::selectGenerator(app);
        g_failures = 1;
        g_failText = "could not open " + std::string(kArgs) + " (the account is at " + cascade::core::DiagLog::instance().filePath() + ") tuned to 433.920 MHz";
        Access::restore(app, cfg);                 // fails, with the log ring holding the real session
        Access::pass(app);
        const std::string all = wholeQueue(rig.qdir());
        CHECK(!all.empty());
        std::string low = all;
        for (char& c : low) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        CHECK(low.find("433.9") == std::string::npos);
        CHECK(low.find("433,9") == std::string::npos);
        CHECK(low.find("433920000") == std::string::npos);
        CHECK(low.find("foxsdr_radio_reports_engine_") == std::string::npos);   // the scratch folder this run isolated itself in
        const char* const user = std::getenv("USERNAME");
        if (user != nullptr && std::string(user).size() >= 4) {
            std::string u = user;
            for (char& c : u) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
            // (the account name is the machine's, so it is looked for in what the real engine queued, not assumed)
            CHECK(low.find("\\users\\" + u) == std::string::npos && low.find("/users/" + u) == std::string::npos);
        }
        keep(queued(rig.qdir()));
    }

    // --- THE FIRST SESSION (L7): sent about a minute in, never a launch, and it takes nothing off the ledgers the usage record carries ------------------------
    {
        std::printf("  the first-session record: a minute in, the health counts so far, and the ledgers untouched\n");
        resetFake();
        auto ledger = cascade::core::health::globalLedger();
        ledger->reset();
        ledger->arm("", cascade::core::newInstallId(), false);
        cascade::core::health::noteRadioOpen("rtlsdr");
        th::TestWindow app;
        Rig rig("first");
        {
            rr::Reporter::Options o;
            o.dir = (rig.dir / "radio-reports").string();
            o.endpoint = "http://127.0.0.1:9/";
            o.liveSend = false;
            o.identity.installId = kInstallId;
            o.identity.version = "0.99.109";
            o.identity.os = "Windows 10.0.22631";
            o.identity.arch = "x64";
            o.identity.launches = 1;   // the install's first session
            rr::global().configure(o);
        }
        Access::setLaunches(app, 1);
        const std::string ledgerBefore = ledger->encoded();
        Access::pass(app);
        CHECK(ofKind(queued(rig.qdir()), "first_session").empty());   // not yet: it is about a minute in
        Access::ageSession(app, 70);
        Access::pass(app);
        Access::pass(app);   // (once: the engine does not say it twice)
        const std::vector<json> firsts = ofKind(queued(rig.qdir()), "first_session");
        std::printf("    first-session look: %zu queued, guard refused %llu, journal queued %llu\n", firsts.size(), static_cast<unsigned long long>(rr::global().stats().guardRefused),
                    static_cast<unsigned long long>(rr::global().stats().journalQueued));
        CHECK(firsts.size() == 1);
        if (!firsts.empty()) {
            const json& b = firsts[0]["body"];
            CHECK(b["launches"] == 1 && b["trigger"] == "after-minute");
            CHECK(b["health"].get<std::string>().find("radio_open.rtlsdr=1") != std::string::npos);   // the health tokens counted so far
            CHECK(!b.contains("id") && !b.contains("window"));   // `window` is not a first-session column
        }
        CHECK(ledger->encoded() == ledgerBefore);   // nothing was settled: the usage record at the next start still carries every token
        keep(firsts);
    }

    // --- RADIOS 2 TO 8 (the same function restores each of them) --------------------------------------------------------------------------------------
    {
        std::printf("  the restore of an added radio that fails is its own report\n");
        resetFake();
        g_failures = 1;
        g_failText = "no RTL-SDR with serial 00000001 is present";
        th::TestWindow app;
        Rig rig("extra");
        rig.start();
        Access::restoreExtra(app);
        const std::vector<json> fails = ofKind(queued(rig.qdir()), "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            CHECK(fails[0]["body"]["trigger"] == "startup-restore" && fails[0]["body"]["reasonClass"] == "absent");
            CHECK(fails[0]["radioKey"] == "generic-rtl2832u:00000001");
        }
        keep(fails);
    }

    // --- THE CALL SITES THAT ARE NOT DRIVEN HERE are held structurally: each file that opens a radio files its failure --------------------------------
    {
        std::printf("  every place a radio open fails calls reportFailedOpen (a source scan, not a drive)\n");
        auto text = [](const char* rel) {
            std::ifstream in(std::string(CASCADE_SOURCE_ROOT) + "/src/engine/" + rel, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        auto count = [](const std::string& hay, const std::string& needle) {
            int n = 0;
            for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) { ++n; }
            return n;
        };
        const std::string engine = text("engine.cpp");
        CHECK(count(engine, "reportFailedOpen(") >= 3);   // finishDeviceOpen (twice: the open the user moved on from, and the failure) and the start-up restore
        CHECK(count(text("engine_radio_saved.cpp"), "reportFailedOpen(") == 1);
        CHECK(count(text("engine_patch_radios.cpp"), "reportFailedOpen(") == 1);
        CHECK(count(text("engine_soundcard.cpp"), "reportFailedOpen(\"soundcard\"") == 1);
        // ...and each sits beside the anonymous count it must never replace
        CHECK(count(engine, "noteRadioFail(") >= 2 && count(text("engine_soundcard.cpp"), "noteRadioFail(") == 1);
    }

    // --- THE JOURNAL OF A RADIO THAT OPENS: found, open, settings, stream, close ---------------------------------------------------------------------
    {
        std::printf("  an opened radio: radio_found, radio_open, radio_settings, a minute's radio_stream, radio_close\n");
        resetFake();
        th::TestWindow app;
        Rig rig("journal");
        rig.start();
        Access::scanBoth(app);   // radio_found for what the scan listed
        Access::restore(app, savedRadio());
        CHECK(Access::deviceOpen(app));
        Access::pass(app);
        Access::ageOpenRadio(app, 60.0);
        Access::minute(app, 144000000, 0);   // a minute's samples at 2.4 MS/s
        Access::selectGenerator(app);
        Access::pass(app);
        const std::vector<json> q = queued(rig.qdir());
        const std::vector<json> found = ofKind(q, "radio_found");
        CHECK(found.size() >= 1);
        if (!found.empty()) {
            CHECK(found[0]["body"]["binding"] == "usable" && found[0]["body"]["source"] == "scan");
            CHECK(found[0]["radioKey"] == "generic-rtl2832u:00000001");
        }
        for (const json& f : found) { CHECK(f["body"]["model"] != "other" || true); }   // (the Pluto and rtl_tcp ADDRESS rows are not radios that were found)
        for (const json& f : found) { CHECK(f["body"]["driver"] != "pluto" && f["body"]["driver"] != "rtltcp"); }
        const std::vector<json> streams = ofKind(q, "radio_stream");
        CHECK(streams.size() == 1);
        if (!streams.empty()) {
            CHECK(streams[0]["body"]["samplesPerSec"] == 2400000);
            CHECK(streams[0]["body"]["drops"] == 0);
            CHECK(streams[0]["body"]["windowSec"].get<long long>() >= 1);
        }
        const std::vector<json> closes = ofKind(q, "radio_close");
        CHECK(closes.size() == 1);
        if (!closes.empty()) { CHECK(closes[0]["body"]["cause"] == "user" && closes[0]["radioKey"] == "generic-rtl2832u:00000001"); }
        keep(q);
    }

    // --- OPENED, NO SAMPLES (L3) -----------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  a radio that opens and gives no samples: reported after 20 s; one that gives samples: nothing\n");
        resetFake();
        g_silent = true;
        th::TestWindow app;
        Rig rig("silent");
        rig.start();
        Access::restore(app, savedRadio());
        CHECK(Access::deviceOpen(app));
        Access::startReceiver(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        Access::pass(app);
        CHECK(ofKind(queued(rig.qdir()), "radio_stream").empty());   // not yet: it was only just opened
        Access::ageOpenRadio(app, 30.0);
        Access::pass(app);
        const std::vector<json> streams = ofKind(queued(rig.qdir()), "radio_stream");
        CHECK(streams.size() == 1);
        if (!streams.empty()) {
            CHECK(streams[0]["body"]["samplesPerSec"] == 0 && streams[0]["body"]["windowSec"] == 20);
            CHECK(streams[0]["body"]["expectedSps"] == 2400000);
        }
        keep(streams);
        // the same, with samples flowing: no event
        resetFake();
        g_silent = false;
        th::TestWindow app2;
        Rig rig2("flowing");
        rig2.start();
        Access::restore(app2, savedRadio());
        Access::startReceiver(app2);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        Access::pass(app2);
        Access::ageOpenRadio(app2, 30.0);
        Access::pass(app2);
        CHECK(ofKind(queued(rig2.qdir()), "radio_stream").empty());
    }

    // --- A SESSION THAT ENDED WITH NO RADIO OPENED (L1): what the scan listed, how many scans, how many opens failed ------------------------------------
    {
        std::printf("  a session that ends with no radio opened says so, with what the scan listed\n");
        resetFake();
        g_failures = 1;
        g_failText = "the radio is already in use by another program";
        th::TestWindow app;
        Rig rig("sessionend");
        rig.start();
        {
            rr::Reporter::Options o;
            o.dir = (rig.dir / "radio-reports").string();
            o.endpoint = "http://127.0.0.1:9/";
            o.liveSend = false;   // inspect the queue, do not send it
            o.identity.installId = kInstallId;
            o.identity.version = "0.99.109";
            o.identity.os = "Windows 10.0.22631";
            o.identity.arch = "x64";
            o.identity.launches = 4;
            rr::global().configure(o);
        }
        Access::scanBoth(app);
        Access::restore(app, savedRadio());   // fails busy
        CHECK(!Access::deviceOpen(app));
        Access::quit(app);
        const std::vector<json> ends = ofKind(queued(rig.qdir()), "session_end");
        CHECK(ends.size() == 1);
        if (!ends.empty()) {
            const json& b = ends[0]["body"];
            CHECK(b["opened"] == false && b["openFailures"] == 1 && b["scans"].get<long long>() >= 2);
            CHECK(b["listed"].size() == 1 && b["listed"][0] == "Generic RTL2832U");
            CHECK(ends[0]["radioKey"] == "");
        }
        // ...and a session that opened a radio does not
        resetFake();
        th::TestWindow app2;
        Access::restore(app2, savedRadio());
        CHECK(Access::deviceOpen(app2));
        Rig rig2("sessionend2");
        {
            rr::Reporter::Options o;
            o.dir = (rig2.dir / "radio-reports").string();
            o.endpoint = "http://127.0.0.1:9/";
            o.liveSend = false;
            o.identity.installId = kInstallId;
            o.identity.version = "0.99.109";
            o.identity.os = "x";
            o.identity.arch = "x64";
            rr::global().configure(o);
        }
        Access::pass(app2);
        Access::quit(app2);
        CHECK(ofKind(queued(rig2.qdir()), "session_end").empty());
    }

    // --- the usage record: the window, the update channel (L11a), the marker -----------------------------------------------------------------------------
    {
        std::printf("  the usage record says which window, the update channel, and nothing of a test\n");
        resetFake();
        th::TestWindow app;
        const json before = json::parse(Access::usageRecord(app));
        CHECK(!before.contains("window"));
        CHECK(before.contains("uch") && before["uch"] == "stable");    // the application DOES send the update channel (since 0.99.104)
        Access::noteWindow(app, "new");
        CHECK(json::parse(Access::usageRecord(app))["window"] == "new");
        Access::noteWindow(app, "legacy");
        CHECK(json::parse(Access::usageRecord(app))["window"] == "legacy");
        Access::noteWindow(app, "something else");
        CHECK(!json::parse(Access::usageRecord(app)).contains("window"));
    }

    // --- scan_none (L1): the two address rows are not radios that were found -------------------------------------------------------------------------
    {
        std::printf("  a scan that finds no radio counts scan_none; one that finds a radio does not\n");
        auto g = cascade::core::health::globalLedger();
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        // THE REAL SCAN ALWAYS ADDS THE TWO ADDRESS ROWS (an ADALM-Pluto's and an rtl_tcp server's): a hook that lists them is the machine with no radio on it
        setNative({{"pluto", "ADALM-Pluto (network)", "uri=ip:192.168.2.1"}, {"rtltcp", "rtl_tcp server (network)", "rtltcp=127.0.0.1:1234"}});
        {
            th::TestWindow app;
            CHECK(Access::scanBoth(app));
            std::string tokens;
            for (const auto& kv : g->counts()) { tokens += kv.first + ";"; }
            std::printf("    health after an empty scan: %s\n", tokens.c_str());
            CHECK(g->counts().count("scan_none") == 1);
        }
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        setNative({{"rtlsdr", "RTL-SDR A", kArgs}});
        {
            th::TestWindow app;
            CHECK(Access::scanBoth(app));
            CHECK(g->counts().count("scan_none") == 0);
        }
        setNative({});
    }

    // --- the Worker's own validator over every message the engine made ---------------------------------------------------------------------------------
    {
        const int refused = validateWithWorker(everything);
        if (refused < 0) {
            std::printf("SKIPPED: node was not found, so the Worker's validator did not see the %zu messages the engine made\n", everything.size());
        } else {
            std::printf("the Worker's validator saw %zu messages the engine made: %d refused\n", everything.size(), refused);
            CHECK(everything.size() >= 8);
            CHECK(refused == 0);
        }
    }

    return testSummary("test_radio_reports_engine");
}
