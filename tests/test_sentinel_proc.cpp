// The sentinel, against REAL PROCESSES that really die (0.99.64).
//
// This test program plays three parts, chosen by its command line:
//
//   (none)       the test: starts stand-in applications, kills them or lets them die,
//                and reads what the sentinel wrote;
//   --app ...    a STAND-IN FOR THE APPLICATION: the real breadcrumb, the real
//                sentinel start (core/sentinel.hpp), the real log and - for one
//                scenario - the real in-process crash handler, driven through the
//                phases AppWindow really has, so that none of this needs a window;
//   --sentinel   THE SENTINEL: runSentinelMain, the same code cascade.exe runs for
//                `cascade --sentinel`, started by the stand-in exactly as the
//                application starts it (this executable is its own helper, the
//                pattern test_soapy_enum_proc.cpp uses).
//
// What the real cascade.exe does is in test_sentinel_app.cpp.
//
// EVERY DEATH HERE IS A REAL ONE, and none can put a dialog on the desktop: the
// stand-in sets SEM_NOGPFAULTERRORBOX before anything else, and its unhandled
// exception filter ends the process quietly with the exception's code (the same
// way the enumeration helper does) - so an access violation and a stack overflow
// are real faults whose only trace is the exit code. A fast-fail and a heap
// corruption cannot reach any filter by definition; the first is a real
// __fastfail, the second (as the enumeration tests have always done it) is
// TerminateProcess with the code the heap manager would have used.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/breadcrumb.hpp"
#include "core/crash_handler.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
#include "core/sentinel.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>

#include <werapi.h>
#pragma comment(lib, "wer.lib")
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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

// The N of "...; silent N s" at the end of a sentinel reason, or -1.
long silentSecondsOf(const std::string& reason) {
    const std::size_t at = reason.rfind("; silent ");
    return at == std::string::npos ? -1 : std::strtol(reason.c_str() + at + 9, nullptr, 10);
}

// ===========================================================================
// THE STAND-IN FOR THE APPLICATION
// ===========================================================================
#if defined(_WIN32)
LONG WINAPI quietDeath(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep != nullptr && ep->ExceptionRecord != nullptr)
                           ? ep->ExceptionRecord->ExceptionCode
                           : 0xE0000001ul;
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH;
}
#pragma warning(push)
#pragma warning(disable : 4717)  // recursive on all control paths: that is the point
int recurse(volatile int depth) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(depth);
    return recurse(depth + 1) + pad[0];
}
#pragma warning(pop)
#endif

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void say(const char* fmt, unsigned long v) {
    std::printf(fmt, v);
    std::fflush(stdout);
}

[[noreturn]] void hold() {
    for (;;) { sleepMs(1000); }
}

// A frame loop of `beats` heartbeats, ten milliseconds apart, polling the watcher
// as the real loop does every few seconds (here every frame, so the test is fast).
void drawFrames(int beats) {
    for (int i = 0; i < beats; ++i) {
        breadcrumb::beat();
        sentinelPoll();
        sleepMs(10);
    }
}

void toRunning(int beats) {
    breadcrumb::setPhase(Phase::CreatingWindow);
    breadcrumb::setPhase(Phase::AwaitingFirstFrame);
    drawFrames(beats);
}

std::string argValue(int argc, char** argv, const char* name, const char* fallback = "") {
    const std::string key = std::string("--") + name + "=";
    for (int i = 2; i < argc; ++i) {
        if (std::strncmp(argv[i], key.c_str(), key.size()) == 0) { return argv[i] + key.size(); }
    }
    return fallback;
}

int standIn(int argc, char** argv) {
#if defined(_WIN32)
    // FIRST: no Windows Error Reporting dialog, whatever happens next. The error mode
    // is what every other crash test sets (and is all an exception that reaches the
    // filter below needs); a real __fastfail skips every filter and goes straight to
    // Windows Error Reporting, which with only that mode still starts WerFault.exe
    // quietly - measured on this machine: two silent instances, no window - so the
    // process also asks Windows, in the SDK's own words, that "fault reporting UI
    // should not be shown" (WER_FAULT_REPORTING_NO_UI), on any machine.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    ::SetUnhandledExceptionFilter(&quietDeath);
#endif
    const fs::path dir = argValue(argc, argv, "dir");
    const std::string scenario = argValue(argc, argv, "scenario");
    const std::string sentinelExe = argValue(argc, argv, "sentinel-exe");
    const bool diag = argValue(argc, argv, "diag", "on") == "on";
    const int runMs = std::atoi(argValue(argc, argv, "run-ms", "0").c_str());
    const fs::path crashes = dir / "crashes";
    const fs::path logs = dir / "logs";
    std::error_code ec;
    fs::create_directories(crashes, ec);
    fs::create_directories(logs, ec);

    DiagLog::instance().configure(logs.string(), diag);
    diagLogf("FoxSDR 0.99.64 (standin) starting");

    if (scenario == "handled") {
        // The REAL in-process crash handler, armed as main() arms it.
        CrashHandlerConfig cfg;
        cfg.crashDir = crashes.string();
        cfg.enabled = true;
        cfg.exitAfterReport = true;  // die at once and quietly, with the fault's own code
        installCrashHandlers(cfg);
    }

    SentinelOptions so;
    so.crashDir = crashes.string();
    so.logDir = logs.string();
    so.exePath = sentinelExe;  // empty: this executable
    sentinelConfigure(so);
    // The sentinel follows the Diagnostics switch exactly as AppWindow drives it.
    sentinelSetEnabled(diag);
    say("SENTINEL %lu\n", sentinelProcessId());
    breadcrumb::setPhase(Phase::BuildingApp);

    if (scenario == "clean") {
        toRunning(6);
        breadcrumb::setPhase(Phase::ShutdownBegun);
        breadcrumb::setPhase(Phase::ShutdownStoppingReceiver);
        breadcrumb::setPhase(Phase::ShutdownUnloadingPlugins);
        breadcrumb::setPhase(Phase::ShutdownWritingMarker);
        breadcrumb::setPhase(Phase::ShutdownClosingWindow);
        breadcrumb::setPhase(Phase::Finished);
        return 0;
    }
    if (scenario == "run") {  // drawing, until the test ends it from outside
        toRunning(40);
        diagLogf("stand-in: about to be ended");
        say("READY %lu\n", 0);
        for (;;) { drawFrames(1); }
    }
    if (scenario == "frozen") {  // drew for a while, then the frame loop stopped
        toRunning(40);              // past the 30 start-up frames: the 5 s threshold applies
        diagLogf("stand-in: about to freeze");
        say("READY %lu\n", 0);
        hold();
    }
    if (scenario == "startup") {  // never got as far as a first frame
        breadcrumb::setPhase(Phase::CreatingWindow);
        diagLogf("stand-in: window being created");
        say("READY %lu\n", 0);
        hold();
    }
    if (scenario == "session") {  // Windows told the window the session is ending
        toRunning(40);
        breadcrumb::noteSessionEnding();
        say("READY %lu\n", 0);
        for (;;) { drawFrames(1); }
    }
    if (scenario == "radio") {  // dies while a radio is being opened
        toRunning(40);
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, true);
        say("READY %lu\n", 0);
        for (;;) { drawFrames(1); }
    }
    if (scenario == "sentinel-killed" || scenario == "fail-start") {
        toRunning(20);
        say("READY %lu\n", 0);
        // The application carries on. It draws for runMs and then ends normally.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(runMs);
        while (std::chrono::steady_clock::now() < until) { drawFrames(1); }
        breadcrumb::setPhase(Phase::ShutdownClosingWindow);
        return 0;
    }
    if (scenario == "toggle") {  // Diagnostics switched off, then on, mid-session
        toRunning(40);
        say("READY %lu\n", 0);
        sleepMs(300);
        sentinelSetEnabled(false);
        say("OFF %lu\n", sentinelProcessId());
        sleepMs(600);
        if (argValue(argc, argv, "then") == "on") {
            sentinelSetEnabled(true);
            say("SENTINEL2 %lu\n", sentinelProcessId());
        }
        for (;;) { drawFrames(1); }
    }
    if (scenario == "late-on") {  // started with Diagnostics off, switched on mid-session
        toRunning(40);
        say("READY %lu\n", 0);
        sleepMs(300);
        sentinelSetEnabled(true);
        say("SENTINEL2 %lu\n", sentinelProcessId());
        for (;;) { drawFrames(1); }
    }

#if defined(_WIN32)
    // The deaths. Each runs after a real stretch of Running, with a line in the log.
    auto dieAfterRunning = [&](auto&& die) {
        toRunning(40);
        diagLogf("stand-in: about to die (%s)", scenario.c_str());
        say("READY %lu\n", 0);
        sleepMs(50);
        die();
    };
    if (scenario == "av" || scenario == "handled") {
        dieAfterRunning([] {
            volatile int* p = nullptr;
            *p = 1;
        });
    }
    if (scenario == "failfast") {
        dieAfterRunning([] { __fastfail(7); });  // FAST_FAIL_FATAL_APP_EXIT: what abort() ends in
    }
    if (scenario == "heap") {
        // Windows raises this from inside the heap manager and no user-mode filter
        // sees it; the exit code is all anyone gets (test_soapy_enum_proc.cpp's way).
        dieAfterRunning([] { ::TerminateProcess(::GetCurrentProcess(), 0xC0000374ul); });
    }
    if (scenario == "overflow") {
        dieAfterRunning([] { (void)recurse(0); });
    }
    if (scenario == "startup-av") {
        breadcrumb::setPhase(Phase::CreatingWindow);
        say("READY %lu\n", 0);
        sleepMs(50);
        volatile int* p = nullptr;
        *p = 1;
    }
#endif
    std::fprintf(stderr, "stand-in: unknown scenario '%s'\n", scenario.c_str());
    return 9;
}

#if defined(_WIN32)
// ===========================================================================
// THE TEST HARNESS (Windows)
// ===========================================================================
struct Proc {
    HANDLE process = nullptr;
    HANDLE readEnd = nullptr;
    DWORD pid = 0;
    std::string seen;
    bool started = false;
};

// The ANSI code page, which is what every narrow string in this test (a path from
// std::filesystem, getenv) is in.
std::wstring wide(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) { ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n); }
    return out;
}

std::string selfPath() {
    char buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, n);
}

// Starts `exe` with `args`, its stdout on a pipe this test reads.
Proc spawn(const std::string& exe, const std::string& args) {
    Proc p;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&p.readEnd, &writeEnd, &sa, 0)) { return p; }
    ::SetHandleInformation(p.readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmd = L"\"" + wide(exe) + L"\" " + wide(args);
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    ::CloseHandle(writeEnd);  // the child has its own copy; this makes end-of-file arrive
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    if (ok) {
        ::CloseHandle(pi.hThread);
        p.process = pi.hProcess;
        p.pid = pi.dwProcessId;
        p.started = true;
    }
    return p;
}

Proc standInProc(const fs::path& dir, const std::string& scenario, const std::string& extra = "",
                 const std::string& exe = std::string()) {
    return spawn(exe.empty() ? selfPath() : exe,
                 "--app \"--dir=" + dir.string() + "\" --scenario=" + scenario + " " + extra);
}

void pump(Proc& p) {
    DWORD avail = 0;
    while (p.readEnd != nullptr && ::PeekNamedPipe(p.readEnd, nullptr, 0, nullptr, &avail, nullptr) &&
           avail > 0) {
        char buf[4096];
        DWORD got = 0;
        if (!::ReadFile(p.readEnd, buf, sizeof(buf), &got, nullptr) || got == 0) { break; }
        p.seen.append(buf, got);
    }
}

bool waitFor(Proc& p, const std::string& needle, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(p);
        if (contains(p.seen, needle)) { return true; }
        if (::WaitForSingleObject(p.process, 0) == WAIT_OBJECT_0) {
            pump(p);
            return contains(p.seen, needle);
        }
        sleepMs(10);
    }
    return contains(p.seen, needle);
}

// "KEY <number>" as the stand-in prints it, or 0.
unsigned long numberAfter(const Proc& p, const std::string& key) {
    const std::size_t at = p.seen.rfind(key + " ");
    if (at == std::string::npos) { return 0; }
    return std::strtoul(p.seen.c_str() + at + key.size() + 1, nullptr, 10);
}

DWORD waitExit(Proc& p, unsigned timeoutMs) {
    const DWORD w = ::WaitForSingleObject(p.process, timeoutMs);
    if (w != WAIT_OBJECT_0) { return 0xDEAD0001ul; }
    DWORD code = 0;
    ::GetExitCodeProcess(p.process, &code);
    pump(p);
    return code;
}

void kill(Proc& p, UINT code) {
    if (p.process != nullptr) { ::TerminateProcess(p.process, code); }
}

void closeProc(Proc& p) {
    if (p.process != nullptr) {
        // A test that failed half way must not leave its stand-in running.
        if (::WaitForSingleObject(p.process, 0) != WAIT_OBJECT_0) { ::TerminateProcess(p.process, 99); }
        ::CloseHandle(p.process);
    }
    if (p.readEnd != nullptr) { ::CloseHandle(p.readEnd); }
    p = Proc{};
}

// A handle to a process we did not start, for waiting on it (the sentinel).
HANDLE openWatch(unsigned long pid) {
    return ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
}

bool gone(HANDLE h, unsigned timeoutMs) {
    return h == nullptr || ::WaitForSingleObject(h, timeoutMs) == WAIT_OBJECT_0;
}

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-sentproc-") + tag + "-" +
                                 std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// The reports in a folder named for `pid`: every crash-*-<pid>-*.txt.
std::vector<fs::path> reportsFor(const fs::path& dir, unsigned long pid) {
    std::vector<fs::path> out;
    std::error_code ec;
    const std::string mid = "-" + std::to_string(pid) + "-";
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("crash-", 0) == 0 && n.find(mid) != std::string::npos &&
            n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0) {
            out.push_back(e.path());
        }
    }
    return out;
}

std::vector<fs::path> waitForReport(const fs::path& dir, unsigned long pid, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        std::vector<fs::path> r = reportsFor(dir, pid);
        if (!r.empty() || std::chrono::steady_clock::now() >= deadline) { return r; }
        sleepMs(20);
    }
}

std::size_t allReports(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) { ++n; }
    }
    return n;
}

// What the uploader would do with each report: a sweep against a port nothing
// listens on, so an ATTEMPT shows as "failed" and a report kept local as "local-only".
std::string sweepStatus(const fs::path& dir, const fs::path& report) {
    SweepParams params;
    params.crashDir = (dir / "crashes").string();
    params.url = "http://127.0.0.1:1/api/crash";
    params.enabled = true;
    params.nowEpoch = static_cast<std::uint64_t>(std::time(nullptr));
    sweepCrashDir(params, std::make_shared<UploadCancel>());
    const std::string text = readFile(report.string() + ".upload");
    const std::size_t at = text.find("status: ");
    return at == std::string::npos ? std::string("(none)")
                                   : text.substr(at + 8, text.find('\n', at) - at - 8);
}

// One death, end to end: run the scenario, let it die (or kill it), find the one report.
struct Death {
    DWORD appExit = 0;
    unsigned long appPid = 0;
    unsigned long sentinelPid = 0;
    bool sentinelGone = false;
    std::vector<fs::path> reports;
    std::string text;
    ParsedReport parsed;
    bool parsedOk = false;
};

// `killWith` 0: the scenario dies by itself; otherwise the test ends it with that code.
Death liveAndDie(const fs::path& dir, const std::string& scenario, UINT killWith,
                 unsigned settleMs = 0, const std::string& extra = std::string()) {
    Death d;
    Proc app = standInProc(dir, scenario, extra);
    CHECK(app.started);
    if (!app.started) { return d; }
    d.appPid = app.pid;
    CHECK(waitFor(app, "READY", 20000));
    d.sentinelPid = numberAfter(app, "SENTINEL");
    HANDLE watch = d.sentinelPid != 0 ? openWatch(d.sentinelPid) : nullptr;
    if (settleMs > 0) { sleepMs(settleMs); }
    if (killWith != 0) { kill(app, killWith); }
    d.appExit = waitExit(app, 20000);
    d.sentinelGone = gone(watch, 3000);
    d.reports = waitForReport(dir, d.appPid, 3000);
    if (watch != nullptr) { ::CloseHandle(watch); }
    if (d.reports.size() == 1) {
        d.text = readFile(d.reports[0]);
        d.parsedOk = parseReportText(d.text, d.parsed);
    }
    closeProc(app);
    return d;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--sentinel") == 0) { return runSentinelMain(argc, argv); }
    if (argc >= 2 && std::strcmp(argv[1], "--app") == 0) { return standIn(argc, argv); }

#if defined(_WIN32)
    // =======================================================================
    // A CLEAN EXIT: nothing written, the sentinel gone within a second, and
    // the executable replaceable at once
    // =======================================================================
    {
        const fs::path dir = scratch("clean");
        Proc app = standInProc(dir, "clean");
        CHECK(app.started);
        CHECK(waitFor(app, "SENTINEL", 20000));
        const unsigned long sentinelPid = numberAfter(app, "SENTINEL");
        CHECK(sentinelPid != 0 && sentinelPid != app.pid);
        HANDLE watch = openWatch(sentinelPid);
        CHECK(watch != nullptr);
        CHECK(waitExit(app, 20000) == 0);
        const auto appEnded = std::chrono::steady_clock::now();
        // THE SENTINEL SAW THE EXIT AND WENT: within a second, and with exit code 0.
        CHECK(gone(watch, 1000));
        const double goneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - appEnded).count();
        std::printf("clean exit: the sentinel was gone %.0f ms after the application ended\n", goneMs);
        CHECK(goneMs < 1000.0);
        DWORD sentinelExit = 0xDEAD;
        if (watch != nullptr) { ::GetExitCodeProcess(watch, &sentinelExit); }
        CHECK(sentinelExit == 0);
        CHECK(allReports(dir) == 0);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // THE EXECUTABLE CAN BE REPLACED PROMPTLY: the sentinel IS the file, so a copy
    // run as both the application and its sentinel must be deletable (the installer's
    // first act) within a second of the application ending.
    {
        const fs::path dir = scratch("replace");
        const fs::path copy = dir / "copy-of-the-app.exe";
        std::error_code ec;
        fs::copy_file(selfPath(), copy, fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        Proc app = standInProc(dir, "clean", "", copy.string());
        CHECK(app.started);
        CHECK(waitFor(app, "SENTINEL", 20000));
        const unsigned long sentinelPid = numberAfter(app, "SENTINEL");
        CHECK(sentinelPid != 0);
        // While it runs the file really is in use: this is what makes the test mean something.
        CHECK(!::DeleteFileW(copy.wstring().c_str()));
        CHECK(waitExit(app, 20000) == 0);
        const auto appEnded = std::chrono::steady_clock::now();
        bool deleted = false;
        double afterMs = 0;
        while (!deleted) {
            deleted = ::DeleteFileW(copy.wstring().c_str()) != 0;
            afterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - appEnded).count();
            if (deleted || afterMs > 3000.0) { break; }
            sleepMs(5);
        }
        std::printf("replace: the executable could be deleted %.0f ms after the application ended\n", afterMs);
        CHECK(deleted);
        CHECK(afterMs < 1000.0);
        closeProc(app);
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // KILLED BY ANOTHER PROCESS WHILE HEALTHY: a report kept here, never sent
    // =======================================================================
    {
        // A folder with a SPACE in its name, as a profile folder often has: it must reach
        // the watcher intact, on its command line.
        const fs::path dir = scratch("out side");
        const Death d = liveAndDie(dir, "run", 1);
        CHECK(d.appExit == 1);
        CHECK(d.sentinelGone);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.reports[0].filename().string().find("-" + std::to_string(d.appPid) + "-999999.txt") != std::string::npos);
            CHECK(d.parsedOk);
            std::printf("outside: %s\n", d.parsed.reason.c_str());
            CHECK(d.parsed.reason.rfind(kSentinelReasonOutside, 0) == 0);
            CHECK(contains(d.parsed.reason, "exit code 1, as taskkill /F does"));
            CHECK(contains(d.parsed.reason, "; phase running; silent "));
            CHECK(d.parsed.code == "0x00000001");
            CHECK(sentinelReasonIsLocalOnly(d.parsed.reason));
            CHECK(d.parsed.uptimeSec < 60);
            // THE LOG TAIL is the stand-in's own, from the file it flushed to, same lines.
            CHECK(contains(d.text, "stand-in: about to be ended"));
            CHECK(contains(d.text, "FoxSDR 0.99.64 (standin) starting"));
            CHECK(contains(d.text, "sentinel: watching this session (process " + std::to_string(d.sentinelPid) + ","));
            // The uploader keeps it on this machine: the sweep marks it local-only, no attempt.
            CHECK(sweepStatus(dir, d.reports[0]) == "local-only");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // A FOLDER WITH A LETTER OUTSIDE ASCII IN ITS NAME (a profile folder often has one),
    // in the ANSI code page the folders arrive in. The watcher is handed the folder as
    // text on its command line, so this is where a wrong conversion would show: it would
    // be handed a folder that does not exist and write nothing. Run only where the code
    // page is Windows-1252 and the letter (e acute, byte 0xE9) therefore means what the
    // test says it does.
    if (::GetACP() == 1252) {
        const std::string tag = "caf\xE9";
        const fs::path dir = scratch(tag.c_str());
        const Death d = liveAndDie(dir, "run", 1);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            CHECK(d.parsed.reason.rfind(kSentinelReasonOutside, 0) == 0);
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    } else {
        std::printf("non-ASCII folder: skipped, the active code page is %u, not 1252\n", ::GetACP());
    }

    // =======================================================================
    // THE DEATHS THE IN-PROCESS HANDLER CANNOT SEE: one upload-eligible report each
    // =======================================================================
    struct Row { const char* scenario; DWORD code; const char* words; };
    const Row deaths[] = {
        {"failfast", 0xC0000409ul, "fast-fail (abort or failed integrity check)"},
        {"av", 0xC0000005ul, "access violation"},
        {"heap", 0xC0000374ul, "heap corruption"},
        {"overflow", 0xC00000FDul, "stack overflow"},
    };
    for (const Row& r : deaths) {
        const fs::path dir = scratch(r.scenario);
        const Death d = liveAndDie(dir, r.scenario, 0);
        std::printf("%s: application exit 0x%08lX\n", r.scenario, static_cast<unsigned long>(d.appExit));
        CHECK(d.appExit == r.code);
        CHECK(d.sentinelGone);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            std::printf("%s: %s\n", r.scenario, d.parsed.reason.c_str());
            CHECK(d.parsed.reason.rfind(kSentinelReasonCrash, 0) == 0);
            CHECK(contains(d.parsed.reason, std::string(" - ") + r.words + "; phase running; silent "));
            char code[16] = {};
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(r.code));
            CHECK(d.parsed.code == code);
            CHECK(!sentinelReasonIsLocalOnly(d.parsed.reason));
            CHECK(contains(d.text, std::string("stand-in: about to die (") + r.scenario + ")"));
            // UPLOAD-ELIGIBLE: the sweep ATTEMPTS it (and, with nothing listening, fails).
            CHECK(sweepStatus(dir, d.reports[0]) == "failed");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // A crash the in-process handler DID report: no second report.
    {
        const fs::path dir = scratch("handled");
        Proc app = standInProc(dir, "handled");
        CHECK(app.started);
        CHECK(waitFor(app, "READY", 20000));
        const unsigned long sentinelPid = numberAfter(app, "SENTINEL");
        HANDLE watch = openWatch(sentinelPid);
        const DWORD exitCode = waitExit(app, 20000);
        CHECK(exitCode == 0xC0000005ul);
        CHECK(gone(watch, 3000));  // the sentinel has finished deciding
        const std::vector<fs::path> mine = reportsFor(dir, app.pid);
        std::printf("handled: %zu report(s) for the application\n", mine.size());
        CHECK(mine.size() == 1);
        if (mine.size() == 1) {
            // ...and it is the handler's, with a stack, not the sentinel's.
            const std::string text = readFile(mine[0]);
            CHECK(mine[0].filename().string().find("-999999") == std::string::npos);
            CHECK(contains(text, "reason: access violation") && contains(text, "--- stack"));
        }
        CHECK(allReports(dir) == 1);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // FROZEN, THEN KILLED: the silent time, and the control that shows it is the silence
    // =======================================================================
    {
        const fs::path dir = scratch("frozen");
        // 5.6 s of silence against the watchdog's own 5 s: frozen, and sent.
        const Death d = liveAndDie(dir, "frozen", 1, 5600);
        CHECK(d.appExit == 1);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            std::printf("frozen: %s\n", d.parsed.reason.c_str());
            CHECK(d.parsed.reason.rfind(kSentinelReasonFrozen, 0) == 0);
            // 5.6 s asked for; a loaded machine can add a little, so a window, not a number.
            CHECK(contains(d.parsed.reason, "; phase running; silent "));
            const long silent = silentSecondsOf(d.parsed.reason);
            CHECK(silent >= 5 && silent <= 9);
            CHECK(!sentinelReasonIsLocalOnly(d.parsed.reason));
            CHECK(sweepStatus(dir, d.reports[0]) == "failed");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // THE CONTROL: the same application, ended after 1.5 s of silence, is not frozen.
        const fs::path dir = scratch("frozen-control");
        const Death d = liveAndDie(dir, "frozen", 1, 1500);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            CHECK(d.parsed.reason.rfind(kSentinelReasonOutside, 0) == 0);
            const long silent = silentSecondsOf(d.parsed.reason);
            CHECK(silent >= 1 && silent <= 3);
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // A DEATH BEFORE THE FIRST FRAME: the start-up class
    // =======================================================================
    {
        const fs::path dir = scratch("startup");
        const Death d = liveAndDie(dir, "startup", 1, 200);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            std::printf("startup: %s\n", d.parsed.reason.c_str());
            CHECK(d.parsed.reason.rfind(kSentinelReasonStartup, 0) == 0);
            CHECK(contains(d.parsed.reason, "; phase creating the window;"));
            CHECK(!sentinelReasonIsLocalOnly(d.parsed.reason));
            CHECK(sweepStatus(dir, d.reports[0]) == "failed");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // ...and a real fault there is the start-up class with its code, not a crash class.
        const fs::path dir = scratch("startup-av");
        const Death d = liveAndDie(dir, "startup-av", 0);
        CHECK(d.appExit == 0xC0000005ul);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            CHECK(d.parsed.reason.rfind(kSentinelReasonStartup, 0) == 0);
            CHECK(contains(d.parsed.reason, "access violation; phase creating the window"));
            CHECK(d.parsed.code == "0xC0000005");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE SESSION CLOSING, and what else was going on
    // =======================================================================
    {
        const fs::path dir = scratch("session");
        const Death d = liveAndDie(dir, "session", 1, 100);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            std::printf("session: %s\n", d.parsed.reason.c_str());
            CHECK(d.parsed.reason.rfind(kSentinelReasonSession, 0) == 0);
            CHECK(sentinelReasonIsLocalOnly(d.parsed.reason));
            CHECK(sweepStatus(dir, d.reports[0]) == "local-only");
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        const fs::path dir = scratch("radio");
        const Death d = liveAndDie(dir, "radio", 0xC0000409ul, 100);
        CHECK(d.reports.size() == 1);
        if (d.reports.size() == 1) {
            CHECK(d.parsedOk);
            CHECK(contains(d.parsed.reason, "; phase opening a radio;"));
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE SENTINEL KILLED FIRST, and a sentinel that cannot start: the
    // application is unaffected and says so ONCE
    // =======================================================================
    {
        const fs::path dir = scratch("sentinel-killed");
        Proc app = standInProc(dir, "sentinel-killed", "--run-ms=2500");
        CHECK(app.started);
        CHECK(waitFor(app, "READY", 20000));
        const unsigned long sentinelPid = numberAfter(app, "SENTINEL");
        CHECK(sentinelPid != 0);
        HANDLE watch = openWatch(sentinelPid);
        // The sentinel is ended from outside, as a user or a security product might.
        HANDLE killer = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(sentinelPid));
        CHECK(killer != nullptr);
        if (killer != nullptr) {
            CHECK(::TerminateProcess(killer, 5) != 0);
            ::CloseHandle(killer);
        }
        CHECK(gone(watch, 3000));
        // The application is unaffected: it draws on and ends cleanly, by itself.
        CHECK(waitExit(app, 20000) == 0);
        const std::string log = readFile(dir / "logs" / "foxsdr.log");
        CHECK(countOf(log, "sentinel: the watcher ended while the application was still running") == 1);
        CHECK(countOf(log, "sentinel: not started") == 0);
        CHECK(allReports(dir) == 0);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        const fs::path dir = scratch("fail-start");
        Proc app = standInProc(dir, "fail-start", "--run-ms=1000 --sentinel-exe=" + (dir / "no-such-program.exe").string());
        CHECK(app.started);
        CHECK(waitFor(app, "READY", 20000));
        CHECK(numberAfter(app, "SENTINEL") == 0);  // none started
        CHECK(waitExit(app, 20000) == 0);          // and the application did not care
        const std::string log = readFile(dir / "logs" / "foxsdr.log");
        CHECK(countOf(log, "sentinel: not started") == 1);
        CHECK(countOf(log, "an ending this session that the application cannot report itself will not be written up") == 1);
        CHECK(allReports(dir) == 0);
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // TWO APPLICATION INSTANCES: two sentinels, each reporting only its own
    // =======================================================================
    {
        const fs::path dir = scratch("two");
        Proc a = standInProc(dir, "run");   // will be ended from outside
        Proc b = standInProc(dir, "av");    // will die of an access violation by itself
        CHECK(a.started && b.started);
        CHECK(waitFor(a, "READY", 20000));
        CHECK(waitFor(b, "READY", 20000));
        const unsigned long sa = numberAfter(a, "SENTINEL");
        const unsigned long sb = numberAfter(b, "SENTINEL");
        CHECK(sa != 0 && sb != 0 && sa != sb);
        HANDLE wa = openWatch(sa);
        HANDLE wb = openWatch(sb);
        CHECK(waitExit(b, 20000) == 0xC0000005ul);
        CHECK(gone(wb, 3000));
        // B's sentinel has finished; A's is still waiting, and has written nothing.
        CHECK(::WaitForSingleObject(wa, 0) == WAIT_TIMEOUT);
        CHECK(reportsFor(dir, a.pid).empty());
        const std::vector<fs::path> rb = reportsFor(dir, b.pid);
        CHECK(rb.size() == 1);
        kill(a, 1);
        CHECK(waitExit(a, 20000) == 1);
        CHECK(gone(wa, 3000));
        const std::vector<fs::path> ra = waitForReport(dir, a.pid, 3000);
        CHECK(ra.size() == 1);
        CHECK(allReports(dir) == 2);
        if (ra.size() == 1 && rb.size() == 1) {
            ParsedReport pa, pb;
            CHECK(parseReportText(readFile(ra[0]), pa) && parseReportText(readFile(rb[0]), pb));
            CHECK(pa.reason.rfind(kSentinelReasonOutside, 0) == 0);  // A: ended from outside
            CHECK(pb.reason.rfind(kSentinelReasonCrash, 0) == 0);    // B: a crash code
            CHECK(contains(pb.reason, "access violation"));
            CHECK(pa.signature != pb.signature);
        }
        if (wa != nullptr) { ::CloseHandle(wa); }
        if (wb != nullptr) { ::CloseHandle(wb); }
        closeProc(a);
        closeProc(b);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE DIAGNOSTICS SWITCH: off means no sentinel and no report
    // =======================================================================
    {
        // Started with Diagnostics OFF: no process at all, and nothing is written.
        const fs::path dir = scratch("diag-off");
        const Death d = liveAndDie(dir, "run", 0xC0000409ul, 100, "--diag=off");
        CHECK(d.sentinelPid == 0);
        CHECK(d.reports.empty());
        CHECK(allReports(dir) == 0);
        CHECK(!fs::exists(dir / "logs" / "foxsdr.log"));  // and no log either, as ever
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // Switched OFF mid-session: the sentinel is ended, and a later death is not reported.
        const fs::path dir = scratch("toggle-off");
        Proc app = standInProc(dir, "toggle");
        CHECK(app.started);
        CHECK(waitFor(app, "READY", 20000));
        const unsigned long sentinelPid = numberAfter(app, "SENTINEL");
        HANDLE watch = openWatch(sentinelPid);
        CHECK(sentinelPid != 0 && watch != nullptr);
        CHECK(waitFor(app, "OFF", 20000));
        CHECK(gone(watch, 3000));  // the watcher was ended by the switch
        sleepMs(300);
        kill(app, 0xC0000409ul);
        waitExit(app, 20000);
        sleepMs(500);
        CHECK(allReports(dir) == 0);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // ...and switched back ON: a new sentinel, which reports the death - and its
        // phase is RUNNING, because the page has been kept from the start.
        const fs::path dir = scratch("toggle-on");
        Proc app = standInProc(dir, "toggle", "--then=on");
        CHECK(app.started);
        CHECK(waitFor(app, "SENTINEL2", 20000));
        const unsigned long first = numberAfter(app, "SENTINEL");
        const unsigned long second = numberAfter(app, "SENTINEL2");
        CHECK(first != 0 && second != 0 && first != second);
        sleepMs(300);
        kill(app, 1);
        waitExit(app, 20000);
        const std::vector<fs::path> r = waitForReport(dir, app.pid, 3000);
        CHECK(r.size() == 1);
        if (r.size() == 1) {
            ParsedReport p;
            CHECK(parseReportText(readFile(r[0]), p));
            CHECK(contains(p.reason, "; phase running;"));
            CHECK(p.reason.rfind(kSentinelReasonOutside, 0) == 0);
        }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    {
        // Started with Diagnostics off and switched on part-way: the sentinel starts then.
        const fs::path dir = scratch("late-on");
        Proc app = standInProc(dir, "late-on", "--diag=off");
        CHECK(app.started);
        CHECK(waitFor(app, "SENTINEL2", 20000));
        CHECK(numberAfter(app, "SENTINEL") == 0 && numberAfter(app, "SENTINEL2") != 0);
        sleepMs(300);
        kill(app, 1);
        waitExit(app, 20000);
        const std::vector<fs::path> r = waitForReport(dir, app.pid, 3000);
        CHECK(r.size() == 1);
        if (r.size() == 1) {
            ParsedReport p;
            CHECK(parseReportText(readFile(r[0]), p));
            CHECK(contains(p.reason, "; phase running;"));
        }
        closeProc(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // WHAT STARTING IT COSTS THE APPLICATION: CreateProcess, and nothing it waits for
    // =======================================================================
    {
        // This test process plays the application: it names itself as the sentinel's
        // executable (main() above runs runSentinelMain for `--sentinel`).
        const fs::path dir = scratch("cost");
        SentinelOptions so;
        so.crashDir = (dir / "crashes").string();
        so.logDir = (dir / "logs").string();
        std::error_code ec;
        fs::create_directories(so.crashDir, ec);
        fs::create_directories(so.logDir, ec);
        sentinelConfigure(so);
        std::vector<double> ms;
        for (int i = 0; i < 25; ++i) {
            const bool up = sentinelSetEnabled(true);
            CHECK(up);
            ms.push_back(sentinelLastStartMs());
            sentinelSetEnabled(false);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("start-up cost: sentinelSetEnabled(true) took min %.1f, median %.1f, max %.1f ms over %zu starts\n",
                    ms.front(), ms[ms.size() / 2], ms.back(), ms.size());
        CHECK(ms[ms.size() / 2] < 100.0);
        CHECK(allReports(dir) == 0);  // each was ended by the switch: nothing written
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    // =======================================================================
    // LINUX. Written on a machine that cannot run it - see docs/DIAGNOSTICS.md.
    // =======================================================================
    {
        char tmpl[] = "/tmp/cascade-sentproc-XXXXXX";
        const char* made = ::mkdtemp(tmpl);
        CHECK(made != nullptr);
        const fs::path dir = made != nullptr ? fs::path(made) : fs::path("/tmp/cascade-sentproc-fallback");
        const std::string self = fs::read_symlink("/proc/self/exe").string();

        auto start = [&](const char* scenario, int& outFd) {
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0) { return static_cast<pid_t>(-1); }
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::dup2(fds[1], 1);
                ::close(fds[0]);
                ::close(fds[1]);
                const std::string dirArg = "--dir=" + dir.string();
                const std::string scArg = std::string("--scenario=") + scenario;
                ::execl(self.c_str(), self.c_str(), "--app", dirArg.c_str(), scArg.c_str(),
                        static_cast<char*>(nullptr));
                ::_exit(127);
            }
            ::close(fds[1]);
            outFd = fds[0];
            return pid;
        };
        auto readUntil = [](int fd, const char* needle, int timeoutMs) {
            std::string seen;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < until && !contains(seen, needle)) {
                char buf[256];
                const ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n <= 0) { break; }
                seen.append(buf, static_cast<std::size_t>(n));
            }
            return seen;
        };
        auto reportsOf = [&](pid_t pid) {
            std::vector<fs::path> out;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
                const std::string n = e.path().filename().string();
                if (n.find("-" + std::to_string(pid) + "-999999.txt") != std::string::npos) { out.push_back(e.path()); }
            }
            return out;
        };
        auto waitReports = [&](pid_t pid) {
            for (int i = 0; i < 300; ++i) {
                const auto r = reportsOf(pid);
                if (!r.empty()) { return r; }
                sleepMs(10);
            }
            return reportsOf(pid);
        };

        // Clean: no report.
        {
            int fd = -1;
            const pid_t app = start("clean", fd);
            CHECK(app > 0);
            int status = 0;
            ::waitpid(app, &status, 0);
            sleepMs(500);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            CHECK(reportsOf(app).empty());
            ::close(fd);
        }
        // SIGKILLed while drawing: a local-only report that says the cause is not available.
        {
            int fd = -1;
            const pid_t app = start("run", fd);
            CHECK(app > 0);
            CHECK(contains(readUntil(fd, "READY", 20000), "READY"));
            ::kill(app, SIGKILL);
            int status = 0;
            ::waitpid(app, &status, 0);
            const auto r = waitReports(app);
            CHECK(r.size() == 1);
            if (r.size() == 1) {
                ParsedReport p;
                CHECK(parseReportText(readFile(r[0]), p));
                CHECK(p.reason.rfind(kSentinelReasonOutside, 0) == 0);
                CHECK(contains(p.reason, "exit status not available on this platform"));
                CHECK(p.code == "unknown");
            }
            ::close(fd);
        }
        // SIGKILLed before the first frame: sent.
        {
            int fd = -1;
            const pid_t app = start("startup", fd);
            CHECK(app > 0);
            CHECK(contains(readUntil(fd, "READY", 20000), "READY"));
            ::kill(app, SIGKILL);
            int status = 0;
            ::waitpid(app, &status, 0);
            const auto r = waitReports(app);
            CHECK(r.size() == 1);
            if (r.size() == 1) {
                ParsedReport p;
                CHECK(parseReportText(readFile(r[0]), p));
                CHECK(p.reason.rfind(kSentinelReasonStartup, 0) == 0);
            }
            ::close(fd);
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#endif
    return testSummary("test_sentinel_proc");
}
