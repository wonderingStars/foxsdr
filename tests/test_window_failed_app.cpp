// A WINDOW THAT CANNOT BE CREATED, through the REAL cascade.exe and its real sentinel.
//
// Field signature 85FF854AA2C772F9 (two reports, Windows 10 19045, 0.99.73) read "ended before the
// first frame - ended by another process (exit code 1, as taskkill /F does); phase creating the
// window". Nothing ended the process: AppWindow::run returns 1 itself when glfwInit or
// glfwCreateWindow fails, and the sentinel took every exit code 1 for a kill. This runs the whole
// chain on a machine whose window CAN be made, by asking for the failure for real:
//
//   FOXSDR_TEST_WINDOW_FAIL=1 (a seam in AppWindow::run, honoured only with --frames) requests an
//   OpenGL version no driver has, so the real glfwCreateWindow returns NULL and the real code
//   below it runs: the `window:` log lines, the breadcrumb flag, `return 1`. No window is ever
//   shown, so this is safe on a desktop. CASCADE_SENTINEL_TEST=1 starts the real watcher, which
//   reads the real page and writes the real report.
//
// What is held:
//   - the application exits with 1 and its log says which GLFW call failed (the `window:` lines
//     reach the log on Windows too);
//   - the watcher writes ONE report, of the startup class, which says the application ended itself
//     and the window could not be created, and does not say another process ended it;
//   - its signature is the one decideSentinel gives the same facts (a group of its own, apart from
//     an exit code 1 with nothing recorded);
//   - the uploader would send it (a start-up report), unlike an ending from outside.
//
// Every run points FOXSDR_DIAG_DIR and CASCADE_CONFIG_TEST at a scratch tree and has no report
// endpoint, so the user's configuration and reports folder are never touched and nothing is
// uploaded.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/sentinel.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

#if defined(_WIN32)

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-winfail-") + tag + "-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// The value of one `key: ` header line of a report, or empty.
std::string headerLine(const std::string& text, const std::string& key) {
    const std::string needle = "\n" + key + ": ";
    const std::size_t at = ("\n" + text).find(needle);
    if (at == std::string::npos) { return std::string(); }
    const std::string whole = "\n" + text;
    const std::size_t from = at + needle.size();
    return whole.substr(from, whole.find('\n', from) - from);
}

std::vector<fs::path> sentinelReports(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        if (e.path().filename().string().find("-999999.txt") != std::string::npos) { out.push_back(e.path()); }
    }
    return out;
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));

    const fs::path dir = scratch("seam");
    {
        std::ofstream cfg(dir / "config.json", std::ios::binary | std::ios::trunc);
        cfg << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\" }\n";
    }
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    ::SetEnvironmentVariableA("CASCADE_SENTINEL_TEST", "1");
    ::SetEnvironmentVariableA("FOXSDR_TEST_WINDOW_FAIL", "1");
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string cmd = "\"" + exe + "\" --frames 3";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    const BOOL started =
        ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    ::SetEnvironmentVariableA("CASCADE_SENTINEL_TEST", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_TEST_WINDOW_FAIL", nullptr);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    CHECK(started != 0);
    if (!started) { return g_checksFailed == 0 ? 0 : 1; }
    ::CloseHandle(pi.hThread);

    // THE APPLICATION ENDS ITSELF with 1, quickly: nothing is drawn and nothing ends it.
    CHECK(::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD code = 0xDEAD0001ul;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    CHECK(code == 1);

    // ITS LOG says which call failed, and never that a window was made: the `window:` lines are
    // flushed on Windows as they are on Linux (0.99.78).
    const std::string log = readFile(dir / "logs" / "foxsdr.log");
    CHECK(contains(log, " window: GLFW initialised; platform chosen: win32"));
    CHECK(contains(log, " window: glfwCreateWindow failed; the GLFW error line before this"));
    CHECK(!contains(log, " window: window created"));
    // THE ERROR DIALOG IS NEVER PUT UP IN A BOUNDED RUN (a test must not wait on a click): the run
    // above finished without one, and the log says the branch was skipped - and not that it was taken.
    CHECK(contains(log, " window: error dialog suppressed for a flagged run"));
    CHECK(!contains(log, " window: error dialog shown"));
    // ...and it says so after the failure it is about, before the process ends.
    CHECK(log.find(" window: glfwCreateWindow failed") < log.find(" window: error dialog suppressed"));

    // THE WATCHER'S REPORT: written a moment after the application is gone.
    std::vector<fs::path> reports;
    for (int i = 0; i < 400 && reports.empty(); ++i) {
        reports = sentinelReports(dir);
        if (reports.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    }
    CHECK(reports.size() == 1);
    if (reports.size() == 1) {
        const std::string text = readFile(reports[0]);
        const std::string reason = headerLine(text, "reason");
        std::printf("window-creation failure, real application: %s\n", reason.c_str());
        CHECK(headerLine(text, "kind") == "crash");
        CHECK(reason.rfind(kSentinelReasonStartup, 0) == 0);
        CHECK(contains(reason, "the application ended itself: the window could not be created"));
        CHECK(contains(reason, "see the window: lines in the log"));
        CHECK(contains(reason, "; phase creating the window"));
        // WHAT THE REPORT MUST NOT SAY: that something else ended the process.
        CHECK(!contains(reason, "another process"));
        CHECK(!contains(reason, "taskkill"));
        CHECK(headerLine(text, "code") == "0x00000001");
        CHECK(sentinelReasonIsSentinel(reason) && !sentinelReasonIsLocalOnly(reason));

        // THE SIGNATURE is the one the decision gives the same facts: a group of its own, apart
        // from an exit code 1 that nothing explains.
        breadcrumb::Snapshot crumb;
        crumb.valid = true;
        crumb.phase = breadcrumb::Phase::CreatingWindow;
        crumb.flags = breadcrumb::kFlagWindowFailed;
        crumb.startedMs = 1;
        SentinelFacts f;
        f.exitKnown = true;
        f.exitCode = 1;
        f.crumb = crumb;
        f.nowMs = 100;
        const SentinelVerdict failed = decideSentinel(f);
        CHECK(sentinelSignature(failed, nullptr) == headerLine(text, "signature"));
        f.crumb.flags = 0;
        CHECK(sentinelSignature(decideSentinel(f), nullptr) != headerLine(text, "signature"));
    }
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
#else
    std::printf("test_window_failed_app: the Windows watcher is not under test on this platform\n");
#endif
    return g_checksFailed == 0 ? 0 : 1;
}
