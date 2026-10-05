// The sentinel, wired through the REAL cascade.exe (0.99.64).
//
// test_sentinel_proc.cpp proves the watcher against stand-in applications. What
// only the real program can show is the WIRING: that main() starts the sentinel
// for a run that asks for one, that the real AppWindow's phases and heartbeat
// reach the page the watcher reads, that the real window's WM_ENDSESSION reaches
// it, that the Diagnostics switch the real application drives ends it, that the
// real file the watcher is a running copy of can be replaced the moment the
// application is gone, and that a clean scripted run leaves no report.
//
// THESE ARE BOUNDED RUNS WITH A TEST SEAM. A bounded `--frames` run never starts a
// sentinel unless CASCADE_SENTINEL_TEST=1 is set, which main() honours only with
// --frames and only when the run may write diagnostics (FOXSDR_DIAG_DIR), like every
// seam in that file. Every run here points FOXSDR_DIAG_DIR and CASCADE_CONFIG_TEST at
// a scratch tree (the user's configuration and reports folder are never touched) and
// has no report endpoint, so nothing is uploaded: FOXSDR_DIAG_DIR on its own is the
// interlock crashUploadEndpoint() documents.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_upload.hpp"
#include "core/sentinel.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

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
    const fs::path dir = base / (std::string("cascade-sentapp-") + tag + "-" +
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

// Starts `exe` with `args` against a scratch tree, with the sentinel seam on. The
// environment is set for the child and put back at once.
App startApp(const fs::path& dir, const std::string& exe, const std::string& args) {
    App a;
    a.dir = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    ::SetEnvironmentVariableA("CASCADE_SENTINEL_TEST", "1");
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// "sentinel: watching this session (process N, started in X ms)".
struct SentinelLine {
    unsigned long pid = 0;
    double startMs = -1.0;
};
SentinelLine sentinelLine(const std::string& log) {
    SentinelLine s;
    const std::string key = "sentinel: watching this session (process ";
    const std::size_t at = log.find(key);
    if (at == std::string::npos) { return s; }
    s.pid = std::strtoul(log.c_str() + at + key.size(), nullptr, 10);
    const std::size_t in = log.find("started in ", at);
    if (in != std::string::npos) { s.startMs = std::strtod(log.c_str() + in + 11, nullptr); }
    return s;
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

HANDLE openWatch(unsigned long pid) {
    return ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
}

bool gone(HANDLE h, unsigned timeoutMs) {
    return h == nullptr || ::WaitForSingleObject(h, timeoutMs) == WAIT_OBJECT_0;
}

std::vector<fs::path> sentinelReports(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        const std::string n = e.path().filename().string();
        if (n.find("-999999.txt") != std::string::npos) { out.push_back(e.path()); }
    }
    return out;
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

// The first visible top-level window the process owns whose class is GLFW's.
struct WindowSearch {
    DWORD pid;
    HWND found;
};
BOOL CALLBACK findWindow(HWND hwnd, LPARAM lp) {
    auto* s = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid || !::IsWindowVisible(hwnd)) { return TRUE; }
    wchar_t cls[64] = {};
    ::GetClassNameW(hwnd, cls, 63);
    if (std::wcsncmp(cls, L"GLFW", 4) != 0) { return TRUE; }
    s->found = hwnd;
    return FALSE;
}

// The copies of cascade.exe whose parent is `pid`: the sentinel, if there is one. (A
// console program started with no console has a conhost.exe child of the system's own,
// which is not what is asked about.)
std::vector<DWORD> childrenOf(DWORD pid) {
    std::vector<DWORD> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return out; }
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (::Process32FirstW(snap, &e)) {
        do {
            if (e.th32ParentProcessID == pid && ::_wcsicmp(e.szExeFile, L"cascade.exe") == 0) {
                out.push_back(e.th32ProcessID);
            }
        } while (::Process32NextW(snap, &e));
    }
    ::CloseHandle(snap);
    return out;
}

void writeConfig(const fs::path& p, bool diagnostics) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "{\n  \"schemaVersion\": 1,\n  \"diagnosticsEnabled\": " << (diagnostics ? "true" : "false")
        << ",\n  \"diagnosticsMinidump\": false\n}\n";
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));

    // =======================================================================
    // A CLEAN SCRIPTED RUN: the sentinel is started, sees the real exit, writes nothing
    // =======================================================================
    {
        const fs::path dir = scratch("clean");
        App app = startApp(dir, exe, "--frames 3");
        CHECK(app.started);
        CHECK(waitForLog(app, "sentinel: watching this session (process ", 60000));
        const SentinelLine line = sentinelLine(logOf(app));
        CHECK(line.pid != 0 && line.pid != app.pid);
        HANDLE watch = openWatch(line.pid);
        CHECK(watch != nullptr);
        std::printf("real application: the sentinel started in %.1f ms (the application's own measurement)\n",
                    line.startMs);
        CHECK(line.startMs >= 0.0 && line.startMs < 250.0);

        CHECK(waitExit(app, 60000) == 0);
        const auto ended = std::chrono::steady_clock::now();
        CHECK(gone(watch, 1000));  // it SAW the real exit and went
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ended).count();
        std::printf("real application: the sentinel was gone %.0f ms after the application ended\n", ms);
        CHECK(ms < 1000.0);
        DWORD sentinelExit = 0xDEAD;
        if (watch != nullptr) { ::GetExitCodeProcess(watch, &sentinelExit); }
        CHECK(sentinelExit == 0);
        CHECK(allReports(dir) == 0);
        const std::string log = logOf(app);
        CHECK(countOf(log, "sentinel: watching this session") == 1);
        CHECK(countOf(log, "the watcher ended while the application was still running") == 0);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE REAL cascade.exe CAN BE REPLACED the moment the application is gone
    // =======================================================================
    {
        const fs::path dir = scratch("replace");
        const fs::path copy = dir / "cascade-copy.exe";
        std::error_code ec;
        fs::copy_file(exe, copy, fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        // The runtime beside it, so the copy starts as the original does.
        const fs::path soapy = fs::path(CASCADE_APP_BINDIR) / "SoapySDR.dll";
        if (fs::exists(soapy)) { fs::copy_file(soapy, dir / "SoapySDR.dll", fs::copy_options::overwrite_existing, ec); }
        App app = startApp(dir / "run", copy.string(), "--frames 3");
        CHECK(app.started);
        CHECK(waitForLog(app, "sentinel: watching this session (process ", 60000));
        // While the application and its watcher run, the file is in use.
        CHECK(!::DeleteFileW(copy.wstring().c_str()));
        CHECK(waitExit(app, 60000) == 0);
        const auto ended = std::chrono::steady_clock::now();
        bool deleted = false;
        double afterMs = 0.0;
        for (;;) {
            deleted = ::DeleteFileW(copy.wstring().c_str()) != 0;
            afterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ended).count();
            if (deleted || afterMs > 3000.0) { break; }
            sleepMs(5);
        }
        std::printf("real application: cascade.exe could be deleted %.0f ms after the application ended\n", afterMs);
        CHECK(deleted);
        CHECK(afterMs < 1000.0);
        closeApp(app);
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // ENDED FROM OUTSIDE WHILE THE REAL WINDOW IS DRAWING: the real phase and heartbeat
    // =======================================================================
    {
        const fs::path dir = scratch("outside");
        App app = startApp(dir, exe, "--frames 1000000");
        CHECK(app.started);
        CHECK(waitForLog(app, "frame loop starting (bounded)", 60000));
        const SentinelLine line = sentinelLine(logOf(app));
        HANDLE watch = openWatch(line.pid);
        CHECK(line.pid != 0 && watch != nullptr);
        sleepMs(2500);  // frames, real ones: the loop is past its first frame
        ::TerminateProcess(app.process, 1);
        CHECK(waitExit(app, 30000) == 1);
        CHECK(gone(watch, 3000));
        sleepMs(200);
        const std::vector<fs::path> r = sentinelReports(dir);
        CHECK(r.size() == 1);
        CHECK(allReports(dir) == 1);
        if (r.size() == 1) {
            ParsedReport p;
            CHECK(parseReportText(readFile(r[0]), p));
            std::printf("real application, ended from outside: %s\n", p.reason.c_str());
            CHECK(p.reason.rfind(kSentinelReasonOutside, 0) == 0);
            CHECK(contains(p.reason, "; phase running; silent "));
            // THE HEARTBEAT WAS LIVE: the real frame loop was drawing, so the silence
            // is a frame's worth, not seconds.
            const std::size_t at = p.reason.find("silent ");
            CHECK(at != std::string::npos && std::strtoul(p.reason.c_str() + at + 7, nullptr, 10) < 2);
            CHECK(p.code == "0x00000001");
            CHECK(p.uptimeSec >= 2 && p.uptimeSec < 60);
            CHECK(contains(readFile(r[0]), "frame loop starting (bounded)"));  // the real log's tail
            CHECK(sweepStatus(dir, r[0]) == "local-only");
        }
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE REAL WINDOW IS TOLD THE SESSION IS ENDING (WM_ENDSESSION), then ended
    // =======================================================================
    {
        const fs::path dir = scratch("session");
        App app = startApp(dir, exe, "--frames 1000000");
        CHECK(app.started);
        CHECK(waitForLog(app, "frame loop starting (bounded)", 60000));
        const SentinelLine line = sentinelLine(logOf(app));
        HANDLE watch = openWatch(line.pid);
        sleepMs(1500);
        WindowSearch search{app.pid, nullptr};
        for (int i = 0; i < 100 && search.found == nullptr; ++i) {
            ::EnumWindows(&findWindow, reinterpret_cast<LPARAM>(&search));
            if (search.found == nullptr) { sleepMs(50); }
        }
        CHECK(search.found != nullptr);
        if (search.found != nullptr) {
            DWORD_PTR result = 0;
            // ENDSESSION_LOGOFF (0x80000000) as lParam: a log off. SendMessage, so it
            // has been through the window procedure when it returns.
            const LRESULT ok = ::SendMessageTimeoutW(search.found, WM_ENDSESSION, TRUE,
                                                     static_cast<LPARAM>(0x80000000u),
                                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
            CHECK(ok != 0);
            // The message did not end the application: it is still drawing.
            sleepMs(300);
            CHECK(::WaitForSingleObject(app.process, 0) == WAIT_TIMEOUT);
            ::TerminateProcess(app.process, 1);
        } else {
            ::TerminateProcess(app.process, 1);
        }
        CHECK(waitExit(app, 30000) == 1);
        CHECK(gone(watch, 3000));
        sleepMs(200);
        const std::vector<fs::path> r = sentinelReports(dir);
        CHECK(r.size() == 1);
        if (r.size() == 1) {
            ParsedReport p;
            CHECK(parseReportText(readFile(r[0]), p));
            std::printf("real application, session ending: %s\n", p.reason.c_str());
            CHECK(p.reason.rfind(kSentinelReasonSession, 0) == 0);
            CHECK(sentinelReasonIsLocalOnly(p.reason));
            CHECK(sweepStatus(dir, r[0]) == "local-only");
        }
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // THE REAL DIAGNOSTICS SWITCH, driven through AppWindow::applyDiagnosticsEnabled
    // =======================================================================
    {
        const fs::path dir = scratch("toggle");
        App app = startApp(dir, exe, "--frames 1000000 --diag-toggle off");
        CHECK(app.started);
        CHECK(waitForLog(app, "sentinel: watching this session (process ", 60000));
        const SentinelLine line = sentinelLine(logOf(app));
        HANDLE watch = openWatch(line.pid);
        CHECK(line.pid != 0 && watch != nullptr);
        // At frame 30 the application switches Diagnostics off, and the sentinel is
        // ended - while the application is still running.
        CHECK(gone(watch, 30000));
        CHECK(::WaitForSingleObject(app.process, 0) == WAIT_TIMEOUT);
        sleepMs(300);
        ::TerminateProcess(app.process, 0xC0000409ul);
        waitExit(app, 30000);
        sleepMs(700);
        CHECK(allReports(dir) == 0);
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // DIAGNOSTICS OFF IN THE USER'S OWN CONFIGURATION: no sentinel runs at all
    // =======================================================================
    {
        const fs::path dir = scratch("stored-off");
        writeConfig(dir / "config.json", false);
        App app = startApp(dir, exe, "--frames 1000000");
        CHECK(app.started);
        sleepMs(3500);  // the window is up and drawing
        CHECK(::WaitForSingleObject(app.process, 0) == WAIT_TIMEOUT);
        // Not a process of its own, and nothing on disk: off means off.
        CHECK(childrenOf(app.pid).empty());
        ::TerminateProcess(app.process, 1);
        waitExit(app, 30000);
        sleepMs(500);
        CHECK(!fs::exists(dir / "crashes"));
        CHECK(!fs::exists(dir / "logs"));
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // ...AND SWITCHED ON PART-WAY THROUGH (the Settings checkbox, driven by
    // --diag-toggle on): the sentinel starts then, and sees a session that is running
    // =======================================================================
    {
        const fs::path dir = scratch("toggle-on");
        writeConfig(dir / "config.json", false);
        App app = startApp(dir, exe, "--frames 1000000 --diag-toggle on");
        CHECK(app.started);
        CHECK(waitForLog(app, "sentinel: watching this session (process ", 60000));
        const SentinelLine line = sentinelLine(logOf(app));
        HANDLE watch = openWatch(line.pid);
        CHECK(line.pid != 0 && watch != nullptr);
        CHECK(childrenOf(app.pid).size() == 1);
        sleepMs(800);
        ::TerminateProcess(app.process, 1);
        CHECK(waitExit(app, 30000) == 1);
        CHECK(gone(watch, 3000));
        sleepMs(200);
        const std::vector<fs::path> r = sentinelReports(dir);
        CHECK(r.size() == 1);
        if (r.size() == 1) {
            ParsedReport p;
            CHECK(parseReportText(readFile(r[0]), p));
            std::printf("real application, Diagnostics switched on mid-session: %s\n", p.reason.c_str());
            CHECK(contains(p.reason, "; phase running;"));  // the page was kept from the start
        }
        if (watch != nullptr) { ::CloseHandle(watch); }
        closeApp(app);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    SKIP_LINUX("the real-binary sentinel runs use CreateProcess, TerminateProcess and SendMessage");
#endif
    return testSummary("test_sentinel_app");
}
