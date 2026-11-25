// NOTHING FROM A SWITCHED-OFF PERIOD IS EVER SENT - the engine's collectors (0.99.115, brief 79; the rule R of the owner's thread).
//
// R: nothing that happened while a data switch was OFF is ever sent, even after that switch is turned ON again. While OFF nothing of its family is collected, counted or written for
// later sending; turning it ON starts from empty. Every case is "ON, activity, OFF, activity, ON, activity", read through the REAL consent command (FOXAPP_OP_TELEMETRY_CONSENT) or the
// REAL interface half (the new window's Diagnostics tick), on a real hermetic engine; what a later report can hold is only the last ON period's.
//
//   U4  the session's usage accumulators (panels, mode seconds, radios at most, session seconds)
//   U5  launch, crash and exit counters, the first-run date and version, what is journalled to config.json
//   U6  the stall ledger (and its file)           U7  the health ledger (and its file)
//   U3  the radio report's engine side: scans, what the scan listed, failed opens, the slots (a radio opened in the OFF period), the session flags, the length of the session
//   U11 the log lines a radio failure report carries (the ring since Usage was switched on)
//   D8  Diagnostics: the new window's tick reaches the health ledger, and both edges empty what that family counted, withhold the unsent reports and move the ring floor
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/event_trail.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/telemetry.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::gui {
struct AppWindowTestAccess {
    using E = cascade::engine::Engine;
    static void startup(AppWindow& a, const cascade::core::AppConfig& c, const std::string& dir) { passseam::on(th::engine(a), [&] { th::engine(a).telemetryStartup(c, dir); }); }
    static void consent(AppWindow& a, int on) { passseam::on(th::engine(a), [&] { (void)th::engine(a).applyCommand(cascade::core::cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, on)); }); }
    static cascade::core::TelemetryReport snapshot(AppWindow& a) { return passseam::read<cascade::core::TelemetryReport>(th::engine(a), [&] { return th::engine(a).telemetrySnapshot(); }); }
    static cascade::core::AppConfig journal(AppWindow& a) {
        return passseam::read<cascade::core::AppConfig>(th::engine(a), [&] {
            cascade::core::AppConfig c;
            th::engine(a).telemetryJournal(c);
            return c;
        });
    }
    static void notePanel(AppWindow& a, const char* n) { passseam::on(th::engine(a), [&] { th::engine(a).telemetryNotePanel(n); }); }
    static void accrueFrom(AppWindow& a, double secondsAgo) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).telemetryModeAccrual_.reset(th::engine(a).host_->wallTimeS() - secondsAgo);
            th::engine(a).telemetryAccrueMode();
        });
    }
    static void setModeSeconds(AppWindow& a, const char* m, std::uint64_t s) { passseam::on(th::engine(a), [&] { th::engine(a).telemetryModeSeconds_[m] = s; }); }
    static void setRadiosMax(AppWindow& a, std::uint32_t n) { passseam::on(th::engine(a), [&] { th::engine(a).telemetryRadiosMax_ = n; }); }
    static std::uint32_t radiosMax(AppWindow& a) { return passseam::read<std::uint32_t>(th::engine(a), [&] { return th::engine(a).telemetryRadiosMax_; }); }
    static std::uint64_t launches(AppWindow& a) { return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).telemetryLaunches_; }); }
    static std::uint64_t crashes(AppWindow& a) { return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).telemetryCrashes_; }); }
    static std::string firstRun(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).telemetryFirstRun_; }); }
    static bool enabled(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).telemetryEnabled_; }); }
    static std::uint64_t scans(AppWindow& a) { return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).radioScans_; }); }
    static std::size_t scanModels(AppWindow& a) { return passseam::read<std::size_t>(th::engine(a), [&] { return th::engine(a).radioScanModels_.size(); }); }
    static std::uint64_t openFailures(AppWindow& a) { return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).radioOpenFailures_; }); }
    static void setOpenFailures(AppWindow& a, std::uint64_t n) { passseam::on(th::engine(a), [&] { th::engine(a).radioOpenFailures_ = n; }); }
    static void scanLanded(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).nativeDevices_.clear();
            th::engine(a).nativeDevices_.push_back(cascade::source::NativeDeviceInfo{"rtlsdr", "Generic RTL2832U", "serial=00000001"});
            th::engine(a).radioReportsScanLanded(true, false);
        });
    }
    static void openSlot(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            auto& j = th::engine(a).radioJournal_[0];
            j = cascade::engine::Engine::RadioJournalSlot{};
            j.open = true;
            j.kind = "rtlsdr";
            j.args = "serial=00000001";
            j.rid = rr::identify("rtlsdr", "serial=00000001", "Generic RTL2832U");
            j.openedAtS = th::engine(a).clockS();
            j.lastMinuteS = j.openedAtS;
        });
    }
    static bool slotOpen(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).radioJournal_[0].open; }); }
    static void setFlags(AppWindow& a, bool v) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).radioOpenedThisSession_ = v;
            th::engine(a).radioFirstSessionSent_ = v;
            th::engine(a).radioSessionEndSent_ = v;
            th::engine(a).radioEnvSent_ = v;
        });
    }
    static int flags(AppWindow& a) {
        return passseam::read<int>(th::engine(a), [&] {
            return (th::engine(a).radioOpenedThisSession_ ? 1 : 0) | (th::engine(a).radioFirstSessionSent_ ? 2 : 0) | (th::engine(a).radioSessionEndSent_ ? 4 : 0) | (th::engine(a).radioEnvSent_ ? 8 : 0);
        });
    }
    static void ageSession(AppWindow& a, int seconds) { passseam::on(th::engine(a), [&] { th::engine(a).clockOrigin_ -= std::chrono::seconds(seconds); }); }
    static void quit(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).radioReportsQuit(); }); }
    static void failOpen(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).reportFailedOpen("rtlsdr", "serial=00000001", "the radio is already in use by another program", rr::Trigger::StartupRestore, false, 2.4e6, 12);
        });
    }
    static void interfaceHalf(AppWindow& a, bool diagnostics) {
        cascade::core::AppConfig c;
        c.diagnosticsEnabled = diagnostics;
        const std::string doc = cascade::core::serializeInterfaceHalf(c);
        passseam::on(th::engine(a), [&] {
            std::string err;
            CHECK(th::engine(a).applyInterfaceConfig(doc, err));
        });
    }
    static std::size_t panelCount(AppWindow& a) { return passseam::read<std::size_t>(th::engine(a), [&] { return th::engine(a).telemetryPanels_.size(); }); }
    static std::size_t modeSecondsCount(AppWindow& a) { return passseam::read<std::size_t>(th::engine(a), [&] { return th::engine(a).telemetryModeSeconds_.size(); }); }
    static bool exitCausePending(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).exitCausePending_; }); }
    static std::string interfaceDoc(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).interfaceConfigDocument(); }); }
    static void setControlDiagDir(AppWindow& a, const std::string& d) { passseam::on(th::engine(a), [&] { th::engine(a).controlDiagDir_ = d; }); }
    static bool healthAllowed(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).healthLedger_->allowed(); }); }
    static std::string healthEncoded(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).healthLedger_->encoded(); }); }
    static bool healthArmed(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).healthLedger_->armed(); }); }
    static std::uint64_t stallCount(AppWindow& a) { return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).stallLedger_->count(); }); }
    static bool stallArmed(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).stallLedger_->armed(); }); }
    static void stallNote(AppWindow& a) { th::engine(a).stallLedger_->note(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

const char* const kId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_root;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_root = fs::temp_directory_path() / ("foxsdr_off_period_engine_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root);
    const std::string s = g_root.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_DIAG_DIR", (g_root / "diag").string());
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

struct Dir {
    fs::path p;
    explicit Dir(const char* tag) {
        p = g_root / (std::string(tag) + "-" + rr::randomHex(6));
        fs::create_directories(p);
    }
    ~Dir() {
        rr::global().configure(rr::Reporter::Options{});   // the process-wide reporter must not outlive the folder
        std::error_code ec;
        fs::remove_all(p, ec);
    }
    std::string str() const { return p.string(); }
    std::string queue() const { return (p / "radio-reports").string(); }
};

cascade::core::AppConfig startCfg(bool usageOn, bool diagOn) {
    cascade::core::AppConfig c;
    c.telemetryEnabled = usageOn;
    c.telemetryInstallId = usageOn ? kId : "";
    c.diagnosticsEnabled = diagOn;
    c.telemetryLaunches = 6;
    c.telemetryCrashes = 2;
    c.telemetryExitsKilled = 1;
    c.telemetryExitsUnknown = 1;
    c.telemetryFirstRun = "2026-01-02";
    c.telemetryFirstVersion = "0.99.80";
    c.telemetryCleanExit = true;
    return c;
}

void resetProcessState() {
    cascade::core::health::globalLedger()->reset();
    cascade::core::globalStallLedger()->disarm();
    cascade::core::DiagLog::instance().resetForTest();
    cascade::core::trail::resetForTest();
    cascade::core::issue::resetIssueReportsForTest();
}

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

bool waitGone(const fs::path& p, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (!fs::exists(p)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !fs::exists(p);
}

std::string statusOf(const fs::path& report) {
    std::ifstream f(cascade::core::uploadSidecarPath(report.string()), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::size_t at = text.find("status: ");
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find('\n', at);
    return text.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
}

bool waitStatus(const fs::path& report, const char* want, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (statusOf(report) == want) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return statusOf(report) == want;
}

}  // namespace

int main() {
    std::printf("test_off_period_engine\n");
    setEnv("CASCADE_CONFIG_TEST", "1");
    setEnv("FOXSDR_INSTANCE_SCOPE", "opx" + std::to_string(static_cast<unsigned long>(
#if defined(_WIN32)
                                                               GetCurrentProcessId()
#else
                                                               getpid()
#endif
                                                                   )));
    isolate();

    // --- U4: the session's usage accumulators --------------------------------------------------------------------------------------------------------
    {
        std::printf("  U4  panels, mode seconds, radios at most and the session's seconds: only the last ON period\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("u4");
        Access::startup(app, startCfg(true, true), d.str());
        Access::notePanel(app, "map");
        Access::setModeSeconds(app, "WFM", 9);
        Access::setRadiosMax(app, 3);
        Access::ageSession(app, 0);
        {
            const cascade::core::TelemetryReport r = Access::snapshot(app);
            CHECK(r.session.panels == std::vector<std::string>({"map"}));
            CHECK(r.session.modeSeconds.count("WFM") == 1 && r.session.radiosMax == 3);
        }
        Access::consent(app, 0);                                  // OFF
        {
            const cascade::core::AppConfig c = Access::journal(app);
            CHECK(c.telemetryPending.empty());                    // nothing is written for later sending
        }
        Access::notePanel(app, "while-off");                      // activity in the OFF period
        Access::accrueFrom(app, 5.0);
        CHECK(Access::panelCount(app) == 0);                      // the entry points returned at once: nothing was kept (the ON edge's clearing is the second line of defence, not the first)
        CHECK(Access::modeSecondsCount(app) == 0);
        Access::setModeSeconds(app, "USB", 4);                    // (what an accrual that was not refused would have banked)
        Access::consent(app, 1);                                  // ON: starts from empty
        {
            const cascade::core::TelemetryReport r = Access::snapshot(app);
            CHECK(r.session.panels.empty());
            CHECK(r.session.modeSeconds.empty());
            CHECK(r.session.radiosMax <= 1);
            CHECK(r.session.seconds <= 3);                        // counted from the ON, not from the start of the session
        }
        Access::notePanel(app, "after-on");
        CHECK(Access::snapshot(app).session.panels == std::vector<std::string>({"after-on"}));
        // the entry points return at once while OFF
        Access::consent(app, 0);
        Access::notePanel(app, "off-again");
        Access::accrueFrom(app, 7.0);
        CHECK(Access::panelCount(app) == 0 && Access::modeSecondsCount(app) == 0);
        Access::consent(app, 1);
        CHECK(Access::snapshot(app).session.panels.empty());
        CHECK(Access::snapshot(app).session.modeSeconds.empty());
    }

    // --- U5: launch / crash / exit counters, first-run date and version, what is written to config.json ---------------------------------------------
    {
        std::printf("  U5  launches, crashes, exits, first run: not counted while OFF, not written, a new life at the ON\n");
        resetProcessState();
        {   // a start with Usage reporting already OFF (the previous run did not exit cleanly)
            th::TestWindow app;
            Dir d("u5a");
            cascade::core::AppConfig c = startCfg(false, true);
            c.telemetryCleanExit = false;
            Access::startup(app, c, d.str());
            CHECK(Access::launches(app) == 0);
            CHECK(Access::crashes(app) == 0);
            CHECK(Access::firstRun(app).empty());
            const cascade::core::AppConfig out = Access::journal(app);
            CHECK(out.telemetryLaunches == 0 && out.telemetryCrashes == 0);
            CHECK(out.telemetryExitsDied == 0 && out.telemetryExitsKilled == 0 && out.telemetryExitsEnded == 0 && out.telemetryExitsUnknown == 0);
            CHECK(out.telemetryFirstRun.empty() && out.telemetryFirstVersion.empty());
            CHECK(out.telemetryPending.empty());
            Access::consent(app, 1);                              // turned ON in this session: a new id, a new life
            CHECK(Access::launches(app) == 1);
            CHECK(Access::crashes(app) == 0);
            CHECK(!Access::firstRun(app).empty() && Access::firstRun(app) != "2026-01-02");
            const cascade::core::TelemetryReport r = Access::snapshot(app);
            CHECK(r.launches == 1 && r.crashes == 0 && r.exits.died == 0 && r.exits.killed == 0 && r.exits.unknown == 0);
        }
        resetProcessState();
        {   // ON, then OFF mid-session: the counters of the earlier ON period go too
            th::TestWindow app;
            Dir d("u5b");
            Access::startup(app, startCfg(true, true), d.str());
            CHECK(Access::launches(app) == 7);                    // unchanged for an ON start: 0.99.114 counts the same
            CHECK(Access::crashes(app) == 2);
            Access::consent(app, 0);
            CHECK(Access::launches(app) == 0 && Access::crashes(app) == 0 && Access::firstRun(app).empty());
            const cascade::core::AppConfig out = Access::journal(app);
            CHECK(out.telemetryLaunches == 0 && out.telemetryCrashes == 0 && out.telemetryExitsKilled == 0 && out.telemetryExitsUnknown == 0);
            CHECK(out.telemetryFirstRun.empty() && out.telemetryFirstVersion.empty() && out.telemetryPending.empty());
            Access::consent(app, 1);
            const cascade::core::TelemetryReport r = Access::snapshot(app);
            CHECK(r.launches == 1 && r.crashes == 0 && r.exits.killed == 0 && r.exits.unknown == 0);
            CHECK(r.firstRun != "2026-01-02");
        }
    }

    // --- U6: the stall ledger -------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  U6  the stall ledger: ON, a stall, OFF, a stall, ON, a stall -> one, and no file from the OFF period\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("u6");
        Access::startup(app, startCfg(true, true), d.str());
        const fs::path file = d.p / "telemetry-stalls";
        Access::stallNote(app);
        CHECK(Access::stallCount(app) == 1);
        Access::consent(app, 0);
        CHECK(!Access::stallArmed(app));
        CHECK(waitGone(file));                                    // removed with the id
        Access::stallNote(app);                                   // a stall in the OFF period: counted nowhere
        CHECK(Access::stallCount(app) == 0);
        CHECK(!fs::exists(file));
        CHECK(Access::snapshot(app).stalls == 0);
        Access::consent(app, 1);
        CHECK(Access::stallArmed(app));
        CHECK(Access::stallCount(app) == 0);                      // starts from empty, not from the file of an earlier period
        Access::stallNote(app);
        CHECK(Access::stallCount(app) == 1);
        CHECK(Access::snapshot(app).stalls == 1);
    }

    // --- U7: the health ledger ------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  U7  the health ledger: radio failures counted while OFF are never in a later usage record\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("u7");
        Access::startup(app, startCfg(true, true), d.str());
        const fs::path file = d.p / "telemetry-health";
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");
        CHECK(!Access::healthEncoded(app).empty());
        Access::consent(app, 0);
        CHECK(!Access::healthArmed(app));
        CHECK(waitGone(file));
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");   // a failure in the OFF period
        CHECK(Access::healthEncoded(app).empty());
        CHECK(Access::snapshot(app).health.empty());
        CHECK(!fs::exists(file));
        Access::consent(app, 1);
        CHECK(Access::healthArmed(app));
        CHECK(Access::healthEncoded(app).empty() && Access::snapshot(app).health.empty());
        CHECK(Access::journal(app).telemetryPending.find("radio_fail") == std::string::npos);
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");   // an ON period
        CHECK(!Access::healthEncoded(app).empty());
        CHECK(Access::journal(app).telemetryPending.find("radio_fail") != std::string::npos);
    }

    // --- U3: the radio report's engine side (scans, listing, failed opens, slots, flags, session length) ---------------------------------------------
    {
        std::printf("  U3  scans, what a scan listed, failed opens, an open slot, the session flags and its length\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("u3");
        Access::startup(app, startCfg(true, true), d.str());
        Access::scanLanded(app);
        CHECK(Access::scans(app) == 1 && Access::scanModels(app) == 1);
        Access::setOpenFailures(app, 2);
        Access::openSlot(app);
        Access::setFlags(app, true);
        Access::consent(app, 0);                                  // OFF
        CHECK(Access::scans(app) == 0 && Access::scanModels(app) == 0 && Access::openFailures(app) == 0);
        CHECK(!Access::slotOpen(app));
        CHECK(Access::flags(app) == 0);
        Access::scanLanded(app);                                  // a scan in the OFF period: not counted, nothing listed
        Access::failOpen(app);                                    // a failed open in the OFF period
        CHECK(Access::scans(app) == 0 && Access::scanModels(app) == 0 && Access::openFailures(app) == 0);
        Access::openSlot(app);                                    // a radio opened in the OFF period
        Access::ageSession(app, 600);
        Access::consent(app, 1);                                  // ON: starts from empty
        CHECK(Access::scans(app) == 0 && Access::scanModels(app) == 0 && Access::openFailures(app) == 0);
        CHECK(!Access::slotOpen(app));
        CHECK(Access::flags(app) == 0);
        Access::quit(app);                                        // quit with no radio opened since the ON
        const std::vector<json> q = queued(d.queue());
        const std::vector<json> ends = ofKind(q, "session_end");
        CHECK(ends.size() == 1);
        if (!ends.empty()) {
            CHECK(ends[0]["body"]["scans"] == 0);
            CHECK(ends[0]["body"]["openFailures"] == 0);
            CHECK(ends[0]["body"]["listed"].empty());
            CHECK(ends[0]["body"]["sessionSec"].get<long long>() < 120);   // counted from the ON, not from the session's start (600 s ago)
        }
        CHECK(ofKind(q, "radio_close").empty());                  // (a hand-set slot with no receiver behind it: the engine itself is cleared. A radio that is REALLY open at the ON edge is found and adopted by the
                                                                  // pass and does get a close line, and no stream window for the minute it was found in: test_radio_reports_engine, brief 83)
        CHECK(ofKind(q, "radio_fail").empty());
    }

    // --- U11: the log lines a failure report carries ------------------------------------------------------------------------------------------------
    {
        std::printf("  U11 a radio failure report carries only log lines written since Usage reporting was switched on\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("u11");
        Access::startup(app, startCfg(true, true), d.str());
        cascade::core::diagLogf("rtlsdr: line from the first ON period FIRSTPERIODMARK");
        Access::consent(app, 0);
        cascade::core::diagLogf("rtlsdr: line from the OFF period OFFPERIODMARK");
        Access::consent(app, 1);
        cascade::core::diagLogf("rtlsdr: line from the last ON period ONPERIODMARK");
        Access::failOpen(app);
        const std::vector<json> fails = ofKind(queued(d.queue()), "radio_fail");
        CHECK(fails.size() == 1);
        if (!fails.empty()) {
            const std::string body = fails[0]["body"].dump();
            CHECK(body.find("OFFPERIODMARK") == std::string::npos);
            CHECK(body.find("FIRSTPERIODMARK") == std::string::npos);
            CHECK(body.find("ONPERIODMARK") != std::string::npos);
        }
        // the person's own copy of the log is whole
        bool whole = false;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { if (l.find("OFFPERIODMARK") != std::string::npos) { whole = true; } }
        CHECK(whole);
    }

    // --- D8: Diagnostics, through the new window's interface half ------------------------------------------------------------------------------------
    {
        std::printf("  D8  the Diagnostics tick (interface half): the health ledger follows it; both edges empty the family\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("d8");
        Access::startup(app, startCfg(true, true), d.str());
        CHECK(Access::healthAllowed(app));
        // reports on disk, written in an ON period and not yet sent
        const fs::path crashDir = fs::path(cascade::core::diagCrashDir());
        std::error_code ec;
        fs::create_directories(crashDir, ec);
        Access::setControlDiagDir(app, crashDir.string());       // the folder the host named for the reports (a bounded run names none, and then nothing is written)
        const fs::path rep = crashDir / "crash-20261124-101010-77-1.txt";
        { std::ofstream f(rep, std::ios::binary | std::ios::trunc); f << "FoxSDR report body\nkind: crash\n"; }
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");
        Access::stallNote(app);
        cascade::core::trail::noteMode(2);
        cascade::core::diagLogf("window: line from the first ON period D8FIRSTMARK");
        CHECK(!Access::healthEncoded(app).empty() && Access::stallCount(app) == 1);

        Access::interfaceHalf(app, false);                        // Diagnostics OFF
        CHECK(!Access::healthAllowed(app));                       // (the new window used to leave this on)
        CHECK(Access::healthEncoded(app).empty());                // what that family counted is gone
        CHECK(Access::stallCount(app) == 0);
        CHECK(cascade::core::trail::formatNow().empty());
        CHECK(waitStatus(rep, "local-only"));                     // the unsent report is the person's own copy now, never sent
        CHECK(fs::exists(rep));
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");   // OFF period
        CHECK(Access::healthEncoded(app).empty());
        cascade::core::diagLogf("window: line from the OFF period D8OFFMARK");
        cascade::core::issue::fileIssueReport(cascade::core::health::Recovered::Viewport);                   // met while OFF: nothing written, nothing kept

        Access::interfaceHalf(app, true);                         // Diagnostics ON
        CHECK(Access::healthAllowed(app));
        CHECK(Access::healthEncoded(app).empty());
        CHECK(Access::stallCount(app) == 0);
        cascade::core::diagLogf("window: line from the last ON period D8ONMARK");
        bool offLine = false, onLine = false, firstLine = false;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshotFor(cascade::core::DiagLog::ReportFamily::Diagnostics)) {
            if (l.find("D8OFFMARK") != std::string::npos) { offLine = true; }
            if (l.find("D8FIRSTMARK") != std::string::npos) { firstLine = true; }
            if (l.find("D8ONMARK") != std::string::npos) { onLine = true; }
        }
        CHECK(!offLine && !firstLine && onLine);
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");   // ON period: counted again
        CHECK(!Access::healthEncoded(app).empty());
        CHECK(statusOf(rep) == "local-only");                     // still withheld after the ON
        fs::remove(rep, ec);
        fs::remove(cascade::core::uploadSidecarPath(rep.string()), ec);
    }

    // --- D4, D5 (review 4): a start with Usage OFF ---------------------------------------------------------------------------------------------------------
    {
        std::printf("  D4  a start with Usage OFF still reads the previous exit's evidence for the person's own log (the pending read is not cleared)\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("d4");
        cascade::core::AppConfig c = startCfg(false, true);
        c.telemetryCleanExit = false;                             // the last run ended unclean
        c.telemetrySessionStarted = 1700000000;                   // ... and recorded when it began: there is evidence to read
        Access::startup(app, c, d.str());
        CHECK(!Access::enabled(app));
        CHECK(Access::exitCausePending(app));                     // (0.99.115 round 3 cleared it: the log line was never written)
        CHECK(Access::crashes(app) == 0);                         // but no counter moved: nothing is counted for a report that will not be sent
    }
    {
        std::printf("  D5  a start with Usage OFF empties the beta tester's running report and its retry queue held in the interface's half\n");
        resetProcessState();
        th::TestWindow app;
        Dir d("d5");
        cascade::core::AppConfig c = startCfg(false, true);
        c.testerUsageCurrent = "{\"sessionSeconds\":120}";
        c.testerUsagePending = {"{\"queued\":1}"};
        {
            std::string err;
            // the half as a window is served it: what the engine holds is what the application loaded
            passseam::on(th::engine(app), [&] { th::engine(app).applyConfig(c); });
        }
        Access::startup(app, c, d.str());
        cascade::core::AppConfig served;
        std::string err;
        CHECK(cascade::core::parseInterfaceHalf(Access::interfaceDoc(app), served, err));
        CHECK(served.testerUsageCurrent.empty());
        CHECK(served.testerUsagePending.empty());
    }
    resetProcessState();
    std::error_code ec;
    fs::remove_all(g_root, ec);
    return testSummary("test_off_period_engine");
}
