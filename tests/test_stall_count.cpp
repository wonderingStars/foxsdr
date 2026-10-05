// Display stalls: counted, carried to the usage record, and reset only when the
// server has taken the record.
//
// WHAT THIS HOLDS. A freeze the hang watchdog classifies as `kind: stall` (the
// display driver was waiting, not FoxSDR) is kept on the user's machine and
// never uploaded, so how OFTEN it happens was invisible. The usage record now
// carries one number for it (core/telemetry.hpp, StallLedger). This file holds
// that number to the properties it exists for:
//
//   - a freeze classified `stall` is counted and a `hang` is not, using the
//     REAL classifier on the same frame lists tests/test_display_stall.cpp uses;
//   - the count is on disk before anything on the GUI thread could save it, so
//     a session ended from the taskbar mid-freeze is still counted at the next
//     start - proved against the real watchdog thread and the real capture;
//   - the count is in the next record, and is subtracted only when the record
//     was ACCEPTED: a failed send keeps it, a stall that happened while the
//     send was in flight is not lost to the subtraction;
//   - with usage reporting off nothing is counted, kept or sent;
//   - the real application arms, journals and clears it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/hang_watchdog.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::core::StallLedger;
using cascade::core::TelemetryReporter;

namespace {

fs::path scratch(const char* tag) {
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::temp_directory_path();
    const fs::path d = base / (std::string("cascade-stalls-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// The real classifier, exactly as tests/test_display_stall.cpp calls it.
bool isStall(const std::vector<const char*>& frames) {
    return HangWatchdog::isDisplayPresentationStall(frames.data(), static_cast<int>(frames.size()));
}

// Waits for the detached removal disarm() starts, bounded.
bool waitGone(const fs::path& p) {
    for (int i = 0; i < 300; ++i) {
        if (!fs::exists(p)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !fs::exists(p);
}

// ---------------------------------------------------------------------------
// A freeze classified `stall` is counted; a `hang` is not.
// ---------------------------------------------------------------------------
void testOnlyADisplayStallIsCounted() {
    const fs::path dir = scratch("kinds");
    const std::string id = cascade::core::newInstallId();
    auto ledger = std::make_shared<StallLedger>();
    ledger->arm(StallLedger::pathIn(u8(dir)), id, /*loadExisting=*/false);
    CHECK(ledger->armed());
    CHECK(ledger->count() == 0);

    HangWatchdog wd;
    wd.setDisplayStallSink([ledger] { ledger->note(); });

    // THE REPORT THIS WHOLE FEATURE EXISTS FOR: a wait, AMD's display driver
    // under it, then opengl32 and the application.
    wd.recordFreezeKind(isStall({"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}));
    CHECK(ledger->count() == 1);
    CHECK(wd.displayStallsRecorded() == 1u);

    // A HANG DOES NOT COUNT. The 0.96.2 SDRplay report (SwapBuffers on top, a
    // dead vendor service below), an ordinary deadlock, and a thread BUSY in a
    // display driver rather than waiting on one - each is a fault in this
    // program and is reported, not counted away.
    wd.recordFreezeKind(isStall({"ntdll.dll", "sdrplay_api.dll", "cascade.exe", "cascade.exe"}));
    wd.recordFreezeKind(isStall({"ntdll.dll", "cascade.exe", "cascade.exe"}));
    wd.recordFreezeKind(isStall({"atio6axx.dll", "opengl32.dll", "cascade.exe"}));
    CHECK(ledger->count() == 1);
    CHECK(wd.displayStallsRecorded() == 1u);

    // The other vendors' stacks and the Linux window-system ones count too.
    wd.recordFreezeKind(isStall({"ntdll.dll", "nvoglv64.dll", "opengl32.dll", "cascade.exe"}));
    wd.recordFreezeKind(isStall({"libc.so.6", "libc.so.6", "libwayland-client.so.0",
                                 "libEGL_mesa.so.0", "cascade"}));
    wd.recordFreezeKind(isStall({"libc.so.6", "libusb-1.0.so.0", "cascade"}));  // a hang
    CHECK(ledger->count() == 3);
    CHECK(wd.displayStallsRecorded() == 3u);

    // On disk already - nothing saved the config, nothing ran on a GUI thread.
    CHECK(readFile(StallLedger::pathIn(u8(dir))) == StallLedger::fileText(id, 3));
}

// ---------------------------------------------------------------------------
// The count survives a session ended mid-freeze, and it is the WATCHDOG'S
// thread that wrote it.
// ---------------------------------------------------------------------------
void testACountSurvivesAKilledSession() {
    const fs::path dir = scratch("killed");
    const std::string id = cascade::core::newInstallId();
    const std::string file = StallLedger::pathIn(u8(dir));

    {
        auto ledger = std::make_shared<StallLedger>();
        ledger->arm(file, id, false);
        // Two stalls, noted from a thread that is not this one - the watchdog's.
        std::thread watchdogThread([ledger] {
            ledger->note();
            ledger->note();
        });
        watchdogThread.join();
        CHECK(ledger->count() == 2);
        // No save, no clean shutdown: the process is ended from the taskbar.
        // The ledger object simply stops existing, as the process would.
    }

    // THE NEXT START reads it back.
    StallLedger next;
    next.arm(file, id, /*loadExisting=*/true);
    CHECK(next.count() == 2);

    // A copy left behind by an EARLIER IDENTITY is not attributed to this one:
    // turning reporting off and on again mints a new id, and the count must not
    // follow it across.
    StallLedger otherIdentity;
    otherIdentity.arm(file, cascade::core::newInstallId(), true);
    CHECK(otherIdentity.count() == 0);

    // Whatever else is in the file is no number at all, never a guess.
    const std::vector<std::string> junkFiles = {
        "", "garbage", "3", "not-an-id 3\n", std::string("\xff\xfe\0 5", 5),
        id + " -4\n", id + " 4x\n", id + " \n", id + "  4\n", id + " 99999999999999999999\n",
        id + "\n", id.substr(0, 31) + " 4\n"};
    for (const std::string& junk : junkFiles) {
        writeFile(file, junk);
        StallLedger s;
        s.arm(file, id, true);
        CHECK(s.count() == 0);
    }
    // A hand-edited huge number is clamped to the cap.
    writeFile(file, id + " 99999999\n");
    StallLedger big;
    big.arm(file, id, true);
    CHECK(big.count() == StallLedger::kMaxCount);
    // ...and a file that is simply missing is zero.
    fs::remove(file);
    StallLedger none;
    none.arm(file, id, true);
    CHECK(none.count() == 0);
}

// The real watchdog, the real capture, a real thread: a freeze classified as the
// display's leaves `kind: stall` on disk AND the number in the ledger's file, and
// a real hang leaves the number alone. The frames cannot be staged (no monitor to
// switch off), so the classification is forced; everything after it is the
// product's code, on the watchdog's own thread.
void testTheRealCaptureCountsAStallAndNotAHang() {
    const fs::path reports = scratch("capture-reports");
    const fs::path home = scratch("capture-home");
    const std::string id = cascade::core::newInstallId();
    const std::string file = StallLedger::pathIn(u8(home));

    auto ledger = std::make_shared<StallLedger>();
    ledger->arm(file, id, false);

    {
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.setDisplayStallSink([ledger] { ledger->note(); });
        w.setForceDisplayStallForTest(true);
        w.start(u8(reports), 600);
        // Not beaten: the GUI thread is "frozen". Threshold plus polls.
        for (int i = 0; i < 100 && w.reportsWritten() == 0u; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(w.reportsWritten() == 1u);
        CHECK(w.displayStallsRecorded() == 1u);
        const std::string report = readFile(fs::path(w.lastReportPath()));
        CHECK(report.find("kind: stall") != std::string::npos);
        w.stop();
    }
    CHECK(ledger->count() == 1);
    // On disk by the time the report exists: a taskbar kill now loses nothing.
    CHECK(readFile(file) == StallLedger::fileText(id, 1));

    // A REAL HANG on the same ledger: not forced, the frames are this test's
    // own, which are not a display driver's. Reported in full, counted not at all.
    const fs::path reports2 = scratch("capture-reports-hang");
    {
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.setDisplayStallSink([ledger] { ledger->note(); });
        w.start(u8(reports2), 600);
        for (int i = 0; i < 100 && w.reportsWritten() == 0u; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(w.reportsWritten() == 1u);
        const std::string report = readFile(fs::path(w.lastReportPath()));
        CHECK(report.find("kind: hang") != std::string::npos);
        CHECK(w.displayStallsRecorded() == 0u);
        w.stop();
    }
    CHECK(ledger->count() == 1);   // unchanged by the hang
    CHECK(readFile(file) == StallLedger::fileText(id, 1));
}

// ---------------------------------------------------------------------------
// The count is in the next record, and resets only on acceptance.
// ---------------------------------------------------------------------------
void testTheCountRidesTheNextRecord() {
    cascade::core::TelemetryReport r;
    r.installId = cascade::core::newInstallId();
    r.appVersion = "0.99.61";
    r.launches = 7;
    r.session.seconds = 120;
    r.session.sdrModel = "uhd b200";
    const std::string stored = r.toJson();   // journalled with stalls 0
    CHECK(nlohmann::json::parse(stored)["stalls"] == 0);

    const std::string sent = cascade::core::withStalls(stored, 4);
    const nlohmann::json a = nlohmann::json::parse(stored);
    const nlohmann::json b = nlohmann::json::parse(sent);
    CHECK(b["stalls"] == 4);
    // Nothing else about the record moved: same keys, same values.
    CHECK(a.size() == b.size());
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (it.key() == "stalls") { continue; }
        CHECK(b.contains(it.key()));
        CHECK(b[it.key()] == it.value());
    }
    // A stored record from a build that had no such field gains it...
    nlohmann::json old = a;
    old.erase("stalls");
    CHECK(nlohmann::json::parse(cascade::core::withStalls(old.dump(), 2))["stalls"] == 2);
    // ...and a stored record that is not a JSON object is left alone.
    CHECK(cascade::core::withStalls("not json", 3) == "not json");
    CHECK(cascade::core::withStalls("[1,2]", 3) == "[1,2]");
    CHECK(cascade::core::withStalls("", 3).empty());
}

// What START-UP hands the sender: the stored record with the ledger's count of
// that moment in it - and still sent only ONCE, however the count moves.
void testStartUpSendsTheLedgerCountOnceOnly() {
    const fs::path dir = scratch("startup");
    const std::string id = cascade::core::newInstallId();
    auto ledger = std::make_shared<StallLedger>();
    ledger->arm(StallLedger::pathIn(u8(dir)), id, false);
    ledger->note();
    ledger->note();

    cascade::core::TelemetryReport r;
    r.installId = id;
    r.appVersion = "0.99.61";
    r.session.seconds = 600;
    r.stalls = 0;   // as journalled by a save made BEFORE the stalls
    const std::string stored = r.toJson();

    std::string out;
    std::uint64_t carried = 99;
    CHECK(cascade::core::prepareStartupRecord(u8(dir), stored, *ledger, out, carried));
    // The record that goes out carries the ledger's number, not the journalled one.
    CHECK(carried == 2);
    CHECK(nlohmann::json::parse(out)["stalls"] == 2);
    CHECK(nlohmann::json::parse(out)["sessionSec"] == 600);

    // The same stored record again (a launch that died before its first save, or
    // a second copy): CLAIMED already, nothing is sent - even though the ledger
    // has meanwhile changed, which would have changed the sent content and, if
    // the claim were made on that, let the session be sent twice.
    ledger->note();
    out = "untouched";
    CHECK(!cascade::core::prepareStartupRecord(u8(dir), stored, *ledger, out, carried));
    CHECK(out.empty());
    CHECK(carried == 0);

    // No pending record: nothing to send, and the ledger is left alone.
    CHECK(!cascade::core::prepareStartupRecord(u8(dir), "", *ledger, out, carried));
    CHECK(ledger->count() == 3);

    // A different stored record (the next session's) is sent, with the figure
    // as it now stands.
    r.session.seconds = 700;
    CHECK(cascade::core::prepareStartupRecord(u8(dir), r.toJson(), *ledger, out, carried));
    CHECK(carried == 3);
    CHECK(nlohmann::json::parse(out)["stalls"] == 3);

    // No directory to claim in fails open - the old behaviour, a possible
    // duplicate being better than never reporting again.
    CHECK(cascade::core::prepareStartupRecord("", stored, *ledger, out, carried));
    CHECK(nlohmann::json::parse(out)["stalls"] == 3);

    // A disarmed ledger (reporting off) carries zero.
    StallLedger off;
    CHECK(cascade::core::prepareStartupRecord("", stored, off, out, carried));
    CHECK(carried == 0);
    CHECK(nlohmann::json::parse(out)["stalls"] == 0);
}

void testResetsOnlyWhenTheRecordIsAccepted() {
    const fs::path dir = scratch("settle");
    const std::string id = cascade::core::newInstallId();
    const std::string file = StallLedger::pathIn(u8(dir));
    auto ledger = std::make_shared<StallLedger>();
    ledger->arm(file, id, false);
    ledger->note();
    ledger->note();
    ledger->note();
    CHECK(ledger->count() == 3);

    const std::string record = "{\"id\":\"x\"}";
    const std::uint64_t carried = ledger->count();

    // A SEND THAT FAILS keeps the count - in memory and on disk.
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) { return false; }, "https://x/", record,
                  cascade::core::settleOnAccept(ledger, carried));
    }
    CHECK(ledger->count() == 3);
    CHECK(readFile(file) == StallLedger::fileText(id, 3));

    // A transport that throws is a refusal, not a crash and not an acceptance.
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) -> bool { throw 1; }, "https://x/", record,
                  cascade::core::settleOnAccept(ledger, carried));
    }
    CHECK(ledger->count() == 3);

    // NOTHING SENT AT ALL (no endpoint, no body) is not an acceptance either.
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) { return true; }, "", record,
                  cascade::core::settleOnAccept(ledger, carried));
        r.sendVia([](const std::string&, const std::string&) { return true; }, "https://x/", "",
                  cascade::core::settleOnAccept(ledger, carried));
    }
    CHECK(ledger->count() == 3);

    // THE REAL TRANSPORT, refusing: a plain-http endpoint is turned away before
    // any socket is opened (the documented black hole), and that is reported as
    // "not accepted" - so the count stays.
    {
        TelemetryReporter r;
        r.send("http://127.0.0.1:9/", record, cascade::core::settleOnAccept(ledger, carried));
    }
    CHECK(ledger->count() == 3);
    CHECK(!cascade::core::httpStatusAccepted(0));      // no answer
    CHECK(!cascade::core::httpStatusAccepted(199));
    CHECK(cascade::core::httpStatusAccepted(200));
    CHECK(cascade::core::httpStatusAccepted(204));
    CHECK(cascade::core::httpStatusAccepted(299));
    CHECK(!cascade::core::httpStatusAccepted(301));
    CHECK(!cascade::core::httpStatusAccepted(400));    // refused: the Worker rejected it
    CHECK(!cascade::core::httpStatusAccepted(413));
    CHECK(!cascade::core::httpStatusAccepted(429));
    CHECK(!cascade::core::httpStatusAccepted(500));

    // A SEND THAT IS ACCEPTED resets it - by exactly what the record carried.
    // A stall that happens while the send is in flight is NOT lost to the
    // subtraction: it is still owed to the next record.
    {
        TelemetryReporter r;
        r.sendVia(
            [&](const std::string&, const std::string&) {
                ledger->note();   // the watchdog's thread, mid-send
                return true;
            },
            "https://x/", record, cascade::core::settleOnAccept(ledger, carried));
    }
    CHECK(ledger->count() == 1);
    CHECK(readFile(file) == StallLedger::fileText(id, 1));

    // And once everything is settled there is no file at all: most installs
    // never have one.
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) { return true; }, "https://x/", record,
                  cascade::core::settleOnAccept(ledger, 1));
    }
    CHECK(ledger->count() == 0);
    CHECK(!fs::exists(file));
    // Settling more than is held bottoms out at zero.
    ledger->note();
    ledger->settle(50);
    CHECK(ledger->count() == 0);
}

// ---------------------------------------------------------------------------
// Off means off.
// ---------------------------------------------------------------------------
void testOffMeansNothingCountedKeptOrSent() {
    const fs::path dir = scratch("off");
    const std::string id = cascade::core::newInstallId();
    const std::string file = StallLedger::pathIn(u8(dir));

    // Never armed - what an opted-out run has - counts nothing, keeps nothing.
    auto ledger = std::make_shared<StallLedger>();
    CHECK(!ledger->armed());
    HangWatchdog wd;
    wd.setDisplayStallSink([ledger] { ledger->note(); });
    wd.recordFreezeKind(true);
    wd.recordFreezeKind(true);
    CHECK(ledger->count() == 0);
    CHECK(!fs::exists(file));
    // ...so what a record would carry is zero, and settling one is a no-op.
    CHECK(cascade::core::withStalls("{\"id\":\"x\"}", ledger->count()) == "{\"id\":\"x\",\"stalls\":0}");
    ledger->settle(5);
    CHECK(ledger->count() == 0);

    // Arming needs a real identity: an empty or malformed id disarms.
    ledger->arm(file, "", false);
    CHECK(!ledger->armed());
    ledger->arm(file, "someone@example.com", false);
    CHECK(!ledger->armed());
    ledger->note();
    CHECK(ledger->count() == 0);
    CHECK(!fs::exists(file));

    // Armed, counting, on disk - then switched off: forgotten and removed.
    ledger->arm(file, id, false);
    ledger->note();
    ledger->note();
    CHECK(ledger->count() == 2);
    CHECK(fs::exists(file));
    ledger->disarm();
    CHECK(!ledger->armed());
    CHECK(ledger->count() == 0);
    CHECK(waitGone(file));
    ledger->note();               // a stall after the switch is not counted
    CHECK(ledger->count() == 0);
    // A send that completes after the switch cannot bring a number back.
    ledger->settle(2);
    CHECK(ledger->count() == 0);
    CHECK(!fs::exists(file));

    // Switching back ON starts from nothing and reads no file, even one an
    // earlier identity left.
    writeFile(file, StallLedger::fileText(id, 9));
    ledger->arm(file, cascade::core::newInstallId(), /*loadExisting=*/false);
    CHECK(ledger->count() == 0);
    fs::remove(file);

    // In memory only, with no directory: counts, keeps no file.
    StallLedger mem;
    mem.arm("", id, false);
    mem.note();
    CHECK(mem.count() == 1);
}

// ---------------------------------------------------------------------------
// The REAL application arms it, journals it, and clears it.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define STALL_SETENV(k, v) ::SetEnvironmentVariableA((k), (v))
#define STALL_POPEN _popen
#define STALL_PCLOSE _pclose
#define STALL_EXE "/cascade.exe"
#else
#define STALL_SETENV(k, v) \
    ((v) != nullptr ? ::setenv((k), (v), 1) : ::unsetenv((k)))
#define STALL_POPEN popen
#define STALL_PCLOSE pclose
#define STALL_EXE "/cascade"
#endif

// A bounded run of the real binary against a private config, with the usage
// endpoint pointed at a plain-http black hole (refused before any socket is
// opened), so nothing can leave the machine. `args` are extra arguments after
// --frames; `diagDir` (when set) redirects the whole diagnostics tree there and
// turns capture on, as tests/test_diag_hang.cpp does; `forceDisplay` classifies
// the stall --diag-stall stages as the display's.
struct AppRun {
    std::string args;
    fs::path diagDir;
    bool forceDisplay = false;
    int frames = 30;
};

void setAppEnv(const fs::path& config, const AppRun& run) {
    STALL_SETENV("CASCADE_CONFIG_TEST", config.string().c_str());
    STALL_SETENV("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    STALL_SETENV("FOXSDR_DIAG_DIR", run.diagDir.empty() ? nullptr : run.diagDir.string().c_str());
    STALL_SETENV("CASCADE_DIAG_FORCE_DISPLAY_STALL", run.forceDisplay ? "1" : nullptr);
}

void clearAppEnv() {
    STALL_SETENV("CASCADE_CONFIG_TEST", nullptr);
    STALL_SETENV("FOXSDR_TELEMETRY_URL", nullptr);
    STALL_SETENV("FOXSDR_DIAG_DIR", nullptr);
    STALL_SETENV("CASCADE_DIAG_FORCE_DISPLAY_STALL", nullptr);
}

void runApp(const fs::path& config, const AppRun& run = AppRun()) {
    setAppEnv(config, run);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + STALL_EXE;
    const std::string tail = " --frames " + std::to_string(run.frames) +
                             (run.args.empty() ? std::string() : " " + run.args);
#if defined(_WIN32)
    const std::string cmd = "\"\"" + exe + "\"" + tail + " 2>&1\"";
#else
    const std::string cmd = "\"" + exe + "\"" + tail + " 2>&1";
#endif
    FILE* p = STALL_POPEN(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof buf, p) != nullptr) {}
    if (p != nullptr) { STALL_PCLOSE(p); }
    clearAppEnv();
}

// A usage-reporting-on config, with nothing pending.
void writeOnConfig(const fs::path& config, const std::string& id) {
    cascade::core::AppConfig cfg;
    cfg.telemetryEnabled = true;
    cfg.telemetryInstallId = id;
    cfg.telemetryLaunches = 4;
    cfg.telemetryCleanExit = true;
    std::string err;
    CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                cascade::core::ConfigStore::serialize(cfg), err));
}

// The `stalls` the config's journalled session record carries, or -1.
int pendingStalls(const fs::path& config) {
    cascade::core::AppConfig back;
    std::string err;
    if (!cascade::core::ConfigStore::load(config.string(), back, err)) { return -1; }
    const nlohmann::json p = nlohmann::json::parse(back.telemetryPending, nullptr, false);
    if (p.is_discarded() || !p.is_object() || !p.contains("stalls")) { return -1; }
    return p["stalls"].get<int>();
}

// The kind line of the (single) freeze report a run left in `diagDir`.
std::string freezeKind(const fs::path& diagDir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(diagDir / "crashes", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("hang-", 0) != 0 || e.path().extension() != ".txt") { continue; }
        const std::string text = readFile(e.path());
        if (text.rfind("kind: stall", 0) == 0) { return "stall"; }
        if (text.rfind("kind: hang", 0) == 0) { return "hang"; }
    }
    return "";
}

void testTheRealApplicationJournalsAndClears() {
    const std::string id = cascade::core::newInstallId();

    // ON, with a ledger from a session that was ended mid-freeze, and a pending
    // record waiting to be sent. The send is refused (black hole), so the count
    // must still be there afterwards - the reset is for an ACCEPTED record only.
    {
        const fs::path dir = scratch("app-on");
        const fs::path config = dir / "config.json";
        cascade::core::AppConfig cfg;
        cfg.telemetryEnabled = true;
        cfg.telemetryInstallId = id;
        cfg.telemetryLaunches = 4;
        cfg.telemetryCleanExit = true;
        cfg.telemetryPending = "{\"id\":\"" + id + "\",\"v\":\"0.0.0\",\"sessionSec\":9}";
        std::string err;
        CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                    cascade::core::ConfigStore::serialize(cfg), err));
        writeFile(StallLedger::pathIn(u8(dir)), StallLedger::fileText(id, 2));

        runApp(config);

        cascade::core::AppConfig back;
        CHECK(cascade::core::ConfigStore::load(config.string(), back, err));
        const nlohmann::json pending = nlohmann::json::parse(back.telemetryPending, nullptr, false);
        CHECK(!pending.is_discarded());
        // The session this run journalled carries the figure the ledger held.
        CHECK(pending.contains("stalls"));
        CHECK(pending.value("stalls", 99) == 2);
        // The previous record was claimed and sent through the real transport,
        // which turned it away: nothing was accepted, so nothing was reset.
        CHECK(readFile(StallLedger::pathIn(u8(dir))) == StallLedger::fileText(id, 2));
    }

    // A ledger left by ANOTHER identity is not attributed to this one.
    {
        const fs::path dir = scratch("app-other");
        const fs::path config = dir / "config.json";
        cascade::core::AppConfig cfg;
        cfg.telemetryEnabled = true;
        cfg.telemetryInstallId = id;
        cfg.telemetryCleanExit = true;
        std::string err;
        CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                    cascade::core::ConfigStore::serialize(cfg), err));
        writeFile(StallLedger::pathIn(u8(dir)),
                  StallLedger::fileText(cascade::core::newInstallId(), 7));
        runApp(config);
        cascade::core::AppConfig back;
        CHECK(cascade::core::ConfigStore::load(config.string(), back, err));
        const nlohmann::json pending = nlohmann::json::parse(back.telemetryPending, nullptr, false);
        CHECK(!pending.is_discarded());
        CHECK(pending.value("stalls", 99) == 0);
    }

    // OFF: nothing is journalled, and a ledger an earlier opted-in run left
    // behind is removed at start-up.
    {
        const fs::path dir = scratch("app-off");
        const fs::path config = dir / "config.json";
        cascade::core::AppConfig cfg;
        cfg.telemetryEnabled = false;
        cfg.telemetryCleanExit = true;
        std::string err;
        CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                    cascade::core::ConfigStore::serialize(cfg), err));
        writeFile(StallLedger::pathIn(u8(dir)), StallLedger::fileText(id, 5));
        runApp(config);
        cascade::core::AppConfig back;
        CHECK(cascade::core::ConfigStore::load(config.string(), back, err));
        CHECK(back.telemetryPending.empty());
        CHECK(!fs::exists(StallLedger::pathIn(u8(dir))));
        CHECK(readFile(config).find("\"stalls\"") == std::string::npos);
    }
}

// A STALLED SESSION IN THE REAL BINARY, end to end: --diag-stall wedges the frame
// loop past the shipped 5 s threshold; with the classification forced to the
// display's, the watchdog's own thread must count it, the ledger file must hold
// it, and the session's journalled record must carry it. The same wedge left a
// hang must not be counted, and with usage reporting off nothing may be.
void testARealStalledSessionIsCounted() {
    const std::string id = cascade::core::newInstallId();

    {   // a display stall, reporting ON
        const fs::path dir = scratch("app-stall-on");
        const fs::path config = dir / "config.json";
        writeOnConfig(config, id);
        AppRun r;
        r.args = "--diag-stall 7000";
        r.diagDir = dir / "diag";
        r.forceDisplay = true;
        r.frames = 120;
        runApp(config, r);
        CHECK(freezeKind(r.diagDir) == "stall");
        CHECK(readFile(StallLedger::pathIn(u8(dir))) == StallLedger::fileText(id, 1));
        CHECK(pendingStalls(config) == 1);
    }
    {   // the very same wedge, left a HANG: reported, not counted
        const fs::path dir = scratch("app-stall-hang");
        const fs::path config = dir / "config.json";
        writeOnConfig(config, id);
        AppRun r;
        r.args = "--diag-stall 7000";
        r.diagDir = dir / "diag";
        r.frames = 120;
        runApp(config, r);
        CHECK(freezeKind(r.diagDir) == "hang");
        CHECK(!fs::exists(StallLedger::pathIn(u8(dir))));
        CHECK(pendingStalls(config) == 0);
    }
    {   // a display stall with usage reporting OFF: nothing counted, kept or journalled
        const fs::path dir = scratch("app-stall-off");
        const fs::path config = dir / "config.json";
        cascade::core::AppConfig cfg;
        cfg.telemetryEnabled = false;
        cfg.telemetryCleanExit = true;
        std::string err;
        CHECK(cascade::core::ConfigStore::writeFile(config.string(),
                                                    cascade::core::ConfigStore::serialize(cfg), err));
        AppRun r;
        r.args = "--diag-stall 7000";
        r.diagDir = dir / "diag";
        r.forceDisplay = true;
        r.frames = 120;
        runApp(config, r);
        CHECK(freezeKind(r.diagDir) == "stall");   // the report is Diagnostics' business...
        CHECK(!fs::exists(StallLedger::pathIn(u8(dir))));   // ...the count is usage reporting's
        CHECK(pendingStalls(config) == -1);
    }
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// THE PATH A STALL IS MOST LIKELY ON: the user ends a frozen window from the
// taskbar. End task is TerminateProcess - nothing runs, the GUI thread never
// saves again - and the stall is still counted at the next start.
// ---------------------------------------------------------------------------
struct Child {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE readEnd = nullptr;
    std::string seen;
    bool started = false;
};

Child startFrozenApp(const fs::path& config, const fs::path& diagDir) {
    Child c;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&c.readEnd, &writeEnd, &sa, 0)) { return c; }
    ::SetHandleInformation(c.readEnd, HANDLE_FLAG_INHERIT, 0);

    AppRun r;
    r.diagDir = diagDir;
    r.forceDisplay = true;
    setAppEnv(config, r);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    // 60 s: far longer than this test needs, so the process is still frozen
    // when it is ended.
    std::string cmd = "\"" + exe + "\" --frames 600 --diag-stall 60000";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    clearAppEnv();
    ::CloseHandle(writeEnd);
    if (ok) {
        c.process = pi.hProcess;
        c.thread = pi.hThread;
        c.started = true;
    }
    return c;
}

bool waitForOutput(Child& c, const std::string& needle, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (::PeekNamedPipe(c.readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (::ReadFile(c.readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
                c.seen.append(buf, got);
            }
        } else if (::WaitForSingleObject(c.process, 0) == WAIT_OBJECT_0) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (c.seen.find(needle) != std::string::npos) { return true; }
    }
    return c.seen.find(needle) != std::string::npos;
}

void testAStallSurvivesEndTask() {
    const std::string id = cascade::core::newInstallId();
    const fs::path dir = scratch("app-endtask");
    const fs::path config = dir / "config.json";
    const std::string ledger = StallLedger::pathIn(u8(dir));
    writeOnConfig(config, id);

    Child c = startFrozenApp(config, dir / "diag");
    CHECK(c.started);
    if (c.started) {
        CHECK(waitForOutput(c, "--diag-stall wedging the frame loop", 60000));
        // The watchdog's report lands about five seconds into the freeze; wait
        // for the ledger rather than for a time.
        bool counted = false;
        for (int i = 0; i < 400 && !counted; ++i) {
            counted = readFile(ledger) == StallLedger::fileText(id, 1);
            if (!counted) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
        CHECK(counted);
        // STILL FROZEN: the process has not come back from its wedge.
        CHECK(::WaitForSingleObject(c.process, 0) == WAIT_TIMEOUT);
        // And the config on disk cannot know: the GUI thread is the only thing
        // that saves it, and it has not run since the freeze began.
        CHECK(pendingStalls(config) == 0);

        // End task.
        ::TerminateProcess(c.process, 1);
        ::WaitForSingleObject(c.process, 10000);
    }
    if (c.thread != nullptr) { ::CloseHandle(c.thread); }
    if (c.process != nullptr) { ::CloseHandle(c.process); }
    if (c.readEnd != nullptr) { ::CloseHandle(c.readEnd); }

    // Nothing was saved by the dead process, and the number is still there.
    CHECK(pendingStalls(config) == 0);
    CHECK(readFile(ledger) == StallLedger::fileText(id, 1));

    // THE NEXT START finds it, and journals it into the record it will send for
    // that session. (What is SENT is the ledger's count at that moment, through
    // a transport that is refused here, so the number is not reset.)
    runApp(config);
    CHECK(pendingStalls(config) == 1);
    CHECK(readFile(ledger) == StallLedger::fileText(id, 1));
}
#endif  // _WIN32

}  // namespace

int main() {
    testOnlyADisplayStallIsCounted();
    testACountSurvivesAKilledSession();
    testTheRealCaptureCountsAStallAndNotAHang();
    testTheCountRidesTheNextRecord();
    testStartUpSendsTheLedgerCountOnceOnly();
    testResetsOnlyWhenTheRecordIsAccepted();
    testOffMeansNothingCountedKeptOrSent();
    testTheRealApplicationJournalsAndClears();
    testARealStalledSessionIsCounted();
#if defined(_WIN32)
    testAStallSurvivesEndTask();
#else
    SKIP_LINUX("the End-task run uses CreateProcess and TerminateProcess");
#endif
    return testSummary("test_stall_count");
}
