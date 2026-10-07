// HOW THE PREVIOUS SESSION ENDED, through the REAL cascade.exe (0.99.69).
//
// test_exit_cause.cpp proves the rule and the counters in-process. What only the real
// program can show is the WIRING: that start-up counts the unclean exit as `unknown`
// in the same breath as `crashes`, that the evidence is read off the thread that draws
// the window and moves it to its class, that the class and the session start reach
// config.json, that the payload the session journals carries the four counters, and
// that one line says it in the diagnostic log.
//
// TWO KINDS OF RUN.
//
//   - STAGED: a config that says the previous session did not finish, a start time for
//     it, and REAL reports in the reports folder - written by the real finishSentinelWatch,
//     so the sentence the rule matches on is the sentence the writer writes - then the
//     real application started on them. One row per class, and the cases with nothing to
//     read (Diagnostics off, an older build's config).
//
//   - KILLED: the real application started, ended the way a person ends it
//     (TerminateProcess, with the code a task kill leaves and with a crash code), its
//     REAL sentinel left to write the REAL report, and the real application started again
//     on what it left. The counters accumulate across several of these.
//
// Every run points FOXSDR_DIAG_DIR and CASCADE_CONFIG_TEST at a scratch tree (the user's
// configuration and reports folder are never touched) and sends its usage record nowhere
// (FOXSDR_TELEMETRY_URL names a port nothing listens on), exactly as the other
// real-binary tests do.
//
// One limit is built into the rule and so into this test: a report and the start of the
// session are both stamped in whole seconds, so a death and a restart inside the SAME
// second cannot be told apart, and the kills below leave a second between them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/exit_cause.hpp"
#include "core/sentinel.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;
using breadcrumb::Phase;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) { ++n; }
    return n;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

#if defined(_WIN32)

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-exitapp-") + tag + "-" +
                                 std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

struct App {
    HANDLE process = nullptr;
    DWORD pid = 0;
    fs::path dir;
    bool started = false;
};

// Starts the application with `args` against the scratch tree `dir`. `sentinel` puts
// the sentinel seam on, so that a run ended by TerminateProcess has its ending written.
App startApp(const fs::path& dir, const std::string& args, bool sentinel) {
    App a;
    a.dir = dir;
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    ::SetEnvironmentVariableA("CASCADE_SENTINEL_TEST", sentinel ? "1" : nullptr);
    ::SetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    ::SetEnvironmentVariableA("CASCADE_SENTINEL_TEST", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", nullptr);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    if (ok) {
        ::CloseHandle(pi.hThread);
        a.process = pi.hProcess;
        a.pid = pi.dwProcessId;
        a.started = true;
    }
    return a;
}

std::string logOf(const App& a) { return readFile(a.dir / "logs" / "foxsdr.log"); }

bool waitForLog(const App& a, const std::string& needle, unsigned timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (contains(logOf(a), needle)) { return true; }
        if (::WaitForSingleObject(a.process, 0) == WAIT_OBJECT_0) { return contains(logOf(a), needle); }
        sleepMs(25);
    }
    return contains(logOf(a), needle);
}

DWORD waitExit(App& a, unsigned timeoutMs) {
    if (::WaitForSingleObject(a.process, timeoutMs) != WAIT_OBJECT_0) { return 0xDEAD0001ul; }
    DWORD code = 0;
    ::GetExitCodeProcess(a.process, &code);
    return code;
}

void closeApp(App& a) {
    if (a.process != nullptr) {
        if (::WaitForSingleObject(a.process, 0) != WAIT_OBJECT_0) { ::TerminateProcess(a.process, 99); }
        ::CloseHandle(a.process);
    }
    a = App{};
}

std::size_t reportCount(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0 &&
            (name.rfind("crash-", 0) == 0 || name.rfind("hang-", 0) == 0)) {
            ++n;
        }
    }
    return n;
}

bool waitForReports(const fs::path& dir, std::size_t n, unsigned timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (reportCount(dir) >= n) { return true; }
        sleepMs(25);
    }
    return reportCount(dir) >= n;
}

AppConfig loadConfig(const fs::path& dir) {
    AppConfig c;
    std::string err;
    CHECK(ConfigStore::load((dir / "config.json").string(), c, err));
    return c;
}

// What the counters say, and the one invariant that must hold of them at rest.
struct Counts {
    std::uint64_t crashes, died, killed, ended, unknown;
    std::uint64_t sum() const { return died + killed + ended + unknown; }
};
Counts countsOf(const AppConfig& c) {
    return {c.telemetryCrashes, c.telemetryExitsDied, c.telemetryExitsKilled, c.telemetryExitsEnded,
            c.telemetryExitsUnknown};
}

// A config that says the previous session did not finish: `prevStart` is when it began,
// the counters are what it had counted. Diagnostics on or off as asked.
void writeStagedConfig(const fs::path& dir, std::uint64_t prevStart, const Counts& before,
                       bool diagnostics) {
    AppConfig cfg;
    cfg.telemetryEnabled = true;
    cfg.telemetryInstallId = newInstallId();
    cfg.telemetryLaunches = 4;
    cfg.telemetryCleanExit = false;
    cfg.telemetryCrashes = before.crashes;
    cfg.telemetryExitsDied = before.died;
    cfg.telemetryExitsKilled = before.killed;
    cfg.telemetryExitsEnded = before.ended;
    cfg.telemetryExitsUnknown = before.unknown;
    cfg.telemetrySessionStarted = prevStart;
    cfg.diagnosticsEnabled = diagnostics;
    std::string err;
    CHECK(ConfigStore::writeFile((dir / "config.json").string(), ConfigStore::serialize(cfg), err));
}

// One REAL sentinel report, written by the real last act of the watcher.
void writeSentinelReport(const fs::path& dir, unsigned long pid, unsigned long code,
                         const breadcrumb::Snapshot& crumb) {
    const fs::path logs = dir / "staged-logs";
    std::error_code ec;
    fs::create_directories(logs, ec);
    {
        std::ofstream out(logs / "foxsdr.log", std::ios::binary | std::ios::trunc);
        out << "12:00:00.000 info FoxSDR 0.99.69 (abc123def456) starting\n12:00:01.000 info the last line\n";
    }
    SentinelEnd e;
    e.appPid = pid;
    e.exitKnown = true;
    e.exitCode = code;
    e.crumb = crumb;
    e.crashDir = (dir / "crashes").string();
    e.logDir = logs.string();
    e.uptimeSec = 77;
    fs::create_directories(dir / "crashes", ec);
    const SentinelOutcome o = finishSentinelWatch(e);
    CHECK(!o.reportPath.empty());
}

breadcrumb::Snapshot crumbOf(Phase p, std::uint64_t silentMs, std::uint32_t flags = 0,
                             std::uint64_t frames = 600) {
    breadcrumb::Snapshot s;
    s.valid = true;
    const std::uint64_t now = breadcrumb::nowMs();
    s.startedMs = now - 60000;
    s.phase = p;
    s.frames = frames;
    s.beatMs = frames > 0 ? now - silentMs : 0;
    s.phaseMs = now - 50000;
    s.flags = flags;
    return s;
}

std::uint64_t nowEpoch() { return static_cast<std::uint64_t>(std::time(nullptr)); }

// Runs the real application on a staged tree to the end, and waits for the one log line.
bool runStaged(const fs::path& dir, bool expectLine, std::string* logOut) {
    App app = startApp(dir, "--frames 600", /*sentinel=*/false);
    CHECK(app.started);
    bool saw = false;
    if (expectLine) { saw = waitForLog(app, "previous session ended: ", 60000); }
    CHECK(waitExit(app, 60000) == 0);
    if (logOut != nullptr) { *logOut = logOf(app); }
    closeApp(app);
    return saw || !expectLine;
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));

    // =======================================================================
    // STAGED: one real report per class, the real application started on it
    // =======================================================================
    // The breadcrumb is built when the row RUNS, not here: it is stamped with the clock, and
    // a row built when this table was would be seconds stale by the time it is used and be
    // read as a window that had stopped drawing.
    struct Crumb {
        Phase phase;
        std::uint64_t silentMs;
        std::uint32_t flags;
        std::uint64_t frames;
    };
    struct Row {
        const char* name;
        unsigned long code;
        Crumb crumb;
        const char* logWord;   // what the log line says
        ExitClass expect;
    };
    const Row rows[] = {
        {"ended from outside (taskkill)", 1ul, {Phase::Running, 10, 0, 600},
         "previous session ended: killed (ended from outside while the window was drawing: ended by another process",
         ExitClass::Killed},
        {"a closed console window", 0xC000013Aul, {Phase::Running, 10, 0, 600},
         "previous session ended: killed (ended from outside while the window was drawing: Ctrl+C or a console close",
         ExitClass::Killed},
        {"a crash exit code nothing reported", 0xC0000409ul, {Phase::Running, 10, 0, 600},
         "previous session ended: died (crash exit code: fast-fail", ExitClass::Died},
        {"a window that had stopped drawing", 1ul, {Phase::Running, 8000, 0, 600},
         "previous session ended: died (the window had stopped drawing when it ended", ExitClass::Died},
        {"ended before the first frame by taskkill", 1ul, {Phase::BuildingApp, 10, 0, 0},
         "previous session ended: killed (ended before the first frame", ExitClass::Killed},
        {"the operating system closing the session", 1ul,
         {Phase::Running, 10, breadcrumb::kFlagSessionEnding, 600},
         "previous session ended: ended (the operating system was closing the session", ExitClass::Ended},
    };
    unsigned long fakePid = 8100;
    for (const Row& row : rows) {
        const fs::path dir = scratch("staged");
        // An older unclean exit already counted: the counters start non-zero, so the
        // test is about what is ADDED and not about a fresh file.
        const Counts before{3, 1, 1, 0, 0};
        writeStagedConfig(dir, nowEpoch() - 120, before, /*diagnostics=*/true);
        writeSentinelReport(dir, ++fakePid, row.code,
                            crumbOf(row.crumb.phase, row.crumb.silentMs, row.crumb.flags, row.crumb.frames));

        std::string log;
        CHECK(runStaged(dir, /*expectLine=*/true, &log));
        const AppConfig after = loadConfig(dir);
        const Counts c = countsOf(after);
        std::printf("staged, %s: crashes %llu -> %llu, died/killed/ended/unknown %llu/%llu/%llu/%llu\n", row.name,
                    static_cast<unsigned long long>(before.crashes), static_cast<unsigned long long>(c.crashes),
                    static_cast<unsigned long long>(c.died), static_cast<unsigned long long>(c.killed),
                    static_cast<unsigned long long>(c.ended), static_cast<unsigned long long>(c.unknown));
        CHECK(c.crashes == before.crashes + 1);
        CHECK(c.sum() == before.sum() + 1);   // THE INVARIANT: the four moved with `crashes`
        CHECK(c.died == before.died + (row.expect == ExitClass::Died ? 1u : 0u));
        CHECK(c.killed == before.killed + (row.expect == ExitClass::Killed ? 1u : 0u));
        CHECK(c.ended == before.ended + (row.expect == ExitClass::Ended ? 1u : 0u));
        CHECK(c.unknown == before.unknown);
        CHECK(contains(log, row.logWord));
        CHECK(countOf(log, "previous session ended: ") == 1);   // ONE line
        // This session's own start is journalled for the next start-up to use.
        CHECK(after.telemetrySessionStarted >= nowEpoch() - 120 && after.telemetrySessionStarted <= nowEpoch());
        // The journalled usage record carries the split, and it is the config's.
        const nlohmann::json rec = nlohmann::json::parse(after.telemetryPending, nullptr, false);
        CHECK(!rec.is_discarded() && rec.is_object());
        if (rec.is_object()) {
            CHECK(rec.value("crashes", 0ull) == c.crashes);
            CHECK(rec.value("exits_died", 99ull) == c.died);
            CHECK(rec.value("exits_killed", 99ull) == c.killed);
            CHECK(rec.value("exits_ended", 99ull) == c.ended);
            CHECK(rec.value("exits_unknown", 99ull) == c.unknown);
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // STAGED, WITH NOTHING TO READ: each lands in `unknown`, with its reason
    // =======================================================================
    {
        // Diagnostics OFF in the stored configuration: nothing is read, whatever
        // the folder holds (a report is there, and is not looked at).
        const fs::path dir = scratch("off");
        const Counts before{0, 0, 0, 0, 0};
        writeStagedConfig(dir, nowEpoch() - 120, before, /*diagnostics=*/false);
        writeSentinelReport(dir, ++fakePid, 1ul, crumbOf(Phase::Running, 10));
        App app = startApp(dir, "--frames 600", false);
        CHECK(app.started);
        CHECK(waitExit(app, 60000) == 0);
        closeApp(app);
        const Counts c = countsOf(loadConfig(dir));
        CHECK(c.crashes == 1 && c.unknown == 1 && c.sum() == 1);
        CHECK(c.died == 0 && c.killed == 0 && c.ended == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // An older build's file: no start time recorded, so the reports cannot be told
        // from older ones - and the log says why.
        const fs::path dir = scratch("older");
        writeStagedConfig(dir, /*prevStart=*/0, Counts{5, 0, 0, 0, 0}, true);
        writeSentinelReport(dir, ++fakePid, 1ul, crumbOf(Phase::Running, 10));
        std::string log;
        CHECK(runStaged(dir, true, &log));
        const Counts c = countsOf(loadConfig(dir));
        CHECK(c.crashes == 6 && c.unknown == 1 && c.sum() == 1 && c.killed == 0);
        CHECK(contains(log, "previous session ended: unknown (the previous session's start was not recorded"));
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // Diagnostics on, nothing in the folder: no evidence at all (a power cut).
        const fs::path dir = scratch("none");
        writeStagedConfig(dir, nowEpoch() - 120, Counts{0, 0, 0, 0, 0}, true);
        std::string log;
        CHECK(runStaged(dir, true, &log));
        const Counts c = countsOf(loadConfig(dir));
        CHECK(c.crashes == 1 && c.unknown == 1 && c.sum() == 1);
        CHECK(contains(log, "previous session ended: unknown ("));
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // A report from BEFORE the previous session began is an older ending's.
        const fs::path dir = scratch("stale");
        writeSentinelReport(dir, ++fakePid, 1ul, crumbOf(Phase::Running, 10));
        sleepMs(1100);
        writeStagedConfig(dir, nowEpoch(), Counts{0, 0, 0, 0, 0}, true);  // began AFTER that report
        sleepMs(1100);
        std::string log;
        CHECK(runStaged(dir, true, &log));
        const Counts c = countsOf(loadConfig(dir));
        CHECK(c.crashes == 1 && c.unknown == 1 && c.killed == 0 && c.sum() == 1);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // A CLEAN previous session adds nothing: no unclean exit, no line, no movement.
        const fs::path dir = scratch("clean");
        AppConfig cfg;
        cfg.telemetryEnabled = true;
        cfg.telemetryInstallId = newInstallId();
        cfg.telemetryCleanExit = true;
        cfg.telemetryCrashes = 2;
        cfg.telemetryExitsDied = 1;
        cfg.telemetryExitsUnknown = 1;
        cfg.telemetrySessionStarted = nowEpoch() - 100;
        std::string err;
        CHECK(ConfigStore::writeFile((dir / "config.json").string(), ConfigStore::serialize(cfg), err));
        writeSentinelReport(dir, ++fakePid, 1ul, crumbOf(Phase::Running, 10));   // an old ending, already counted
        std::string log;
        CHECK(runStaged(dir, /*expectLine=*/false, &log));
        const Counts c = countsOf(loadConfig(dir));
        CHECK(c.crashes == 2 && c.died == 1 && c.unknown == 1 && c.killed == 0 && c.sum() == 2);
        CHECK(countOf(log, "previous session ended: ") == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // THE CLASS IS WRITTEN WITHOUT A CLEAN EXIT. The evidence is read a moment after
        // start-up and moves the count from `unknown` to its class; that move has to reach
        // the file by the ordinary debounced save, because a session that is itself ended a
        // few seconds later - this one, by TerminateProcess - never makes the saves of a
        // clean exit. (If the move were not part of what the debounce compares, the file
        // would still say `unknown` and the class would be lost with the process.)
        const fs::path dir = scratch("persisted");
        const Counts before{1, 0, 0, 0, 1};
        writeStagedConfig(dir, nowEpoch() - 120, before, /*diagnostics=*/true);
        writeSentinelReport(dir, ++fakePid, 1ul, crumbOf(Phase::Running, 10));
        App app = startApp(dir, "--frames 1000000", /*sentinel=*/false);
        CHECK(app.started);
        CHECK(waitForLog(app, "previous session ended: killed (", 60000));
        sleepMs(4000);  // the debounce is about two seconds; the process is still running
        CHECK(::WaitForSingleObject(app.process, 0) == WAIT_TIMEOUT);
        ::TerminateProcess(app.process, 1);
        CHECK(waitExit(app, 30000) == 1);
        closeApp(app);
        const AppConfig after = loadConfig(dir);
        const Counts c = countsOf(after);
        CHECK(!after.telemetryCleanExit);   // it really was ended without a clean exit
        CHECK(c.crashes == 2 && c.sum() == before.sum() + 1);
        CHECK(c.killed == 1 && c.unknown == 1 && c.died == 0 && c.ended == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // KILLED: the real application ended by TerminateProcess, its REAL sentinel
    // writing the REAL report, the real application started again - twice killed,
    // then once more to read what the second left
    // =======================================================================
    {
        const fs::path dir = scratch("killed");
        const unsigned long killCodes[] = {1ul, 0xC0000005ul};
        std::size_t reportsSoFar = 0;
        Counts last{0, 0, 0, 0, 0};
        for (int i = 0; i < 2; ++i) {
            // Each start sees the previous one's ending. (From the second, the line for the
            // one before it is already in the log, so the count of lines is the check.)
            App app = startApp(dir, "--frames 1000000", /*sentinel=*/true);
            CHECK(app.started);
            CHECK(waitForLog(app, "frame loop starting (bounded)", 60000));
            if (i > 0) { CHECK(waitForLog(app, "previous session ended: ", 60000)); }
            sleepMs(2000);  // frames, real ones: the loop is past its first frames
            ::TerminateProcess(app.process, killCodes[i]);
            CHECK(waitExit(app, 30000) == killCodes[i]);
            ++reportsSoFar;
            CHECK(waitForReports(dir, reportsSoFar, 10000));  // the sentinel's own report
            closeApp(app);
            sleepMs(1100);  // a report and a start are stamped in whole seconds
        }
        // THE LAST START: a bounded run that ends cleanly, and reads what the second one left.
        App app = startApp(dir, "--frames 600", /*sentinel=*/true);
        CHECK(app.started);
        CHECK(waitForLog(app, "previous session ended: ", 60000));
        CHECK(waitExit(app, 60000) == 0);
        const std::string log = logOf(app);
        closeApp(app);

        const AppConfig after = loadConfig(dir);
        last = countsOf(after);
        std::printf("real kills: crashes %llu, died/killed/ended/unknown %llu/%llu/%llu/%llu\n",
                    static_cast<unsigned long long>(last.crashes), static_cast<unsigned long long>(last.died),
                    static_cast<unsigned long long>(last.killed), static_cast<unsigned long long>(last.ended),
                    static_cast<unsigned long long>(last.unknown));
        // Run 1 was killed with code 1 (killed), run 2 with a crash code (died): the first
        // restart counted run 1 (and could not yet read run 2), the last counted run 2.
        CHECK(last.crashes == 2);
        CHECK(last.sum() == last.crashes);              // THE INVARIANT, after real starts
        CHECK(last.killed == 1 && last.died == 1 && last.ended == 0 && last.unknown == 0);
        // The lines the two restarts wrote, one each, in order.
        CHECK(countOf(log, "previous session ended: ") == 2);
        const std::size_t killedAt = log.find("previous session ended: killed (");
        const std::size_t diedAt = log.find("previous session ended: died (crash exit code: access violation");
        CHECK(killedAt != std::string::npos && diedAt != std::string::npos && killedAt < diedAt);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    SKIP_LINUX("the real-binary exit-cause runs use CreateProcess and TerminateProcess");
#endif
    return testSummary("test_exit_cause_app");
}
