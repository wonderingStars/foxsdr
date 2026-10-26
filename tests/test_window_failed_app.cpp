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
// THE SOFTWARE RENDERER (0.99.90, gui/software_gl.hpp). A refused window now hands the run to a
// second process on Mesa's llvmpipe, which this build stages in softgl\ beside cascade.exe. With
// =1 the second process is refused too, so the chain above still ends in exit code 1 - now the
// second process's, passed on by the first - and still in ONE report: the first process switches
// its watcher off before it hands over, and the second has its own. The other failure paths that
// never show a window are held here as well:
//   - FOXSDR_GL=hardware: no second process, and the log says why;
//   - FOXSDR_GL=software with every window refused: the software renderer from the start, no
//     second process, and "already tried";
//   - no softgl\ beside the executable (a copy of cascade.exe without it): no second process;
//   - FOXSDR_TEST_WINDOW_FAIL=imgui: the Dear ImGui OpenGL 3 backend refusing, which until 0.99.90
//     ended the process with one line on stderr - now the log line, the dialog branch and the
//     watcher's "the window could not be created", exactly like a refused window.
// The paths that DO show a window (the hand-over that succeeds) are in test_software_gl_app.cpp.
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
#include <utility>
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

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) { ++n; }
    return n;
}

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

// The variables every run sets; each run below adds its own and all are cleared afterwards.
const char* const kRunVars[] = {"FOXSDR_DIAG_DIR",  "CASCADE_CONFIG_TEST", "CASCADE_SENTINEL_TEST",
                                "FOXSDR_TEST_WINDOW_FAIL", "FOXSDR_GL",  "FOXSDR_CRASH_URL"};

struct Run {
    bool started = false;
    DWORD code = 0xDEAD0001ul;
    std::string log;
};

// One bounded `--frames 3` run of `exe` with a scratch diagnostics folder and configuration, the
// variables in `env` set for it (and every one of kRunVars cleared before and after), standard
// handles on NUL and no console. Waits up to a minute.
Run runApp(const std::string& exe, const fs::path& dir, const std::vector<std::pair<const char*, const char*>>& env) {
    Run r;
    {
        std::ofstream cfg(dir / "config.json", std::ios::binary | std::ios::trunc);
        cfg << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\" }\n";
    }
    for (const char* v : kRunVars) { ::SetEnvironmentVariableA(v, nullptr); }
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    for (const auto& [name, value] : env) { ::SetEnvironmentVariableA(name, value); }

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
    for (const char* v : kRunVars) { ::SetEnvironmentVariableA(v, nullptr); }
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    r.started = started != 0;
    if (!r.started) { return r; }
    ::CloseHandle(pi.hThread);
    if (::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) { ::GetExitCodeProcess(pi.hProcess, &r.code); }
    ::CloseHandle(pi.hProcess);
    r.log = readFile(dir / "logs" / "foxsdr.log");
    return r;
}

// THE WATCHER'S REPORT: written a moment after the application is gone. Waits up to ten seconds
// for the first, then a little longer for any second one that should not be there.
std::vector<fs::path> waitForReports(const fs::path& dir) {
    std::vector<fs::path> reports;
    for (int i = 0; i < 400 && reports.empty(); ++i) {
        reports = sentinelReports(dir);
        if (reports.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return sentinelReports(dir);
}

// The report says the window could not be created, and carries the signature decideSentinel gives
// the same facts.
void checkWindowFailedReport(const fs::path& report) {
    const std::string text = readFile(report);
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

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));
    // The build stages the software renderer beside the application (CMakeLists.txt, CASCADE_SOFTGL);
    // the hand-over below is only reachable with it.
    const fs::path softgl = fs::path(CASCADE_APP_BINDIR) / "softgl";
    CHECK(fs::exists(softgl / "opengl32.dll") && fs::exists(softgl / "libgallium_wgl.dll"));
    std::vector<fs::path> keep;

    // =======================================================================
    // 1. Every window refused: the hand-over, the second refusal, exit 1, ONE report
    // =======================================================================
    {
        const fs::path dir = scratch("seam");
        const Run r = runApp(exe, dir, {{"CASCADE_SENTINEL_TEST", "1"}, {"FOXSDR_TEST_WINDOW_FAIL", "1"}});
        CHECK(r.started);
        // THE APPLICATION ENDS ITSELF with 1, quickly: nothing is drawn and nothing ends it.
        CHECK(r.code == 1);

        // ITS LOG says which call failed, and never that a window was made: the `window:` lines are
        // flushed on Windows as they are on Linux (0.99.78).
        const std::string& log = r.log;
        CHECK(contains(log, " window: GLFW initialised; platform chosen: win32"));
        CHECK(contains(log, " window: glfwCreateWindow failed; the GLFW error line before this"));
        CHECK(!contains(log, " window: window created"));
        // THE ERROR DIALOG IS NEVER PUT UP IN A BOUNDED RUN (a test must not wait on a click): the run
        // above finished without one, and the log says the branch was skipped - and not that it was
        // taken.
        CHECK(contains(log, " window: error dialog suppressed for a flagged run"));
        CHECK(!contains(log, " window: error dialog shown"));
        // ...and it says so after the failure it is about, before the process ends.
        CHECK(log.find(" window: glfwCreateWindow failed") < log.find(" window: error dialog suppressed"));

        // THE HAND-OVER (0.99.90), in order: the refusal is the driver's, a second process is
        // started, it loads the software renderer from the start, is refused as well (=1), does
        // not start a third, says so, and the first passes its exit code on.
        const std::size_t refused = log.find(" window: glfwCreateWindow failed");
        const std::size_t retry = log.find("; starting again with the software renderer in softgl");
        const std::size_t handing = log.find(" window: handing over to a second FoxSDR process");
        const std::size_t again = log.find(" window: started again by process ");
        const std::size_t loaded = log.find(" window: software renderer loaded from softgl (GALLIUM_DRIVER=llvmpipe)");
        const std::size_t tried = log.find(" window: no software fallback: the software renderer was already tried");
        const std::size_t ended = log.find(" window: the second FoxSDR process ended with exit code 1");
        CHECK(retry != std::string::npos && handing != std::string::npos && again != std::string::npos);
        CHECK(loaded != std::string::npos && tried != std::string::npos && ended != std::string::npos);
        CHECK(refused < retry && retry < handing && handing < again && again < loaded && loaded < tried &&
              tried < ended);
        CHECK(count(log, " window: handing over") == 1);
        CHECK(count(log, " window: started again by process ") == 1);
        // the second refusal is the one the person is told about, once
        CHECK(count(log, " window: error dialog suppressed for a flagged run") == 1);
        CHECK(log.find(" window: error dialog suppressed") > again);

        // ONE REPORT, the second process's: the first one's watcher was switched off as it handed over.
        const std::vector<fs::path> reports = waitForReports(dir);
        CHECK(reports.size() == 1);
        if (reports.size() == 1) { checkWindowFailedReport(reports[0]); }
        keep.push_back(dir);
    }

    // =======================================================================
    // 2. FOXSDR_GL=hardware: no second process, and the line that says why
    // =======================================================================
    {
        const fs::path dir = scratch("hardware");
        const Run r = runApp(exe, dir, {{"FOXSDR_GL", "hardware"}, {"FOXSDR_TEST_WINDOW_FAIL", "hardware"}});
        CHECK(r.started && r.code == 1);
        CHECK(contains(r.log, " window: FOXSDR_GL=hardware; the graphics driver only, with no software fallback"));
        CHECK(contains(r.log, " window: no software fallback: FOXSDR_GL=hardware"));
        CHECK(!contains(r.log, "handing over") && !contains(r.log, "started again by process"));
        CHECK(!contains(r.log, "software renderer loaded"));
        CHECK(contains(r.log, " window: error dialog suppressed for a flagged run"));
        keep.push_back(dir);
    }

    // =======================================================================
    // 3. FOXSDR_GL=software and every window refused: one process, "already tried"
    // =======================================================================
    {
        const fs::path dir = scratch("software");
        const Run r = runApp(exe, dir, {{"FOXSDR_GL", "software"}, {"FOXSDR_TEST_WINDOW_FAIL", "1"}});
        CHECK(r.started && r.code == 1);
        CHECK(contains(r.log, " window: FOXSDR_GL=software; the software renderer is used from the start"));
        CHECK(contains(r.log, " window: software renderer loaded from softgl"));
        CHECK(contains(r.log, " window: no software fallback: the software renderer was already tried"));
        CHECK(!contains(r.log, "handing over") && !contains(r.log, "started again by process"));
        // the renderer is loaded BEFORE GLFW is asked for anything
        CHECK(r.log.find("software renderer loaded") < r.log.find(" window: GLFW initialised"));
        keep.push_back(dir);
    }

    // =======================================================================
    // 4. No software renderer beside the application: no second process
    // =======================================================================
    {
        const fs::path dir = scratch("missing");
        const fs::path app = dir / "app";
        std::error_code ec;
        fs::create_directories(app, ec);
        fs::copy_file(exe, app / "cascade.exe", fs::copy_options::overwrite_existing, ec);
        const fs::path soapy = fs::path(CASCADE_APP_BINDIR) / "SoapySDR.dll";
        if (fs::exists(soapy)) { fs::copy_file(soapy, app / "SoapySDR.dll", fs::copy_options::overwrite_existing, ec); }
        CHECK(!fs::exists(app / "softgl"));
        const Run r = runApp((app / "cascade.exe").string(), dir, {{"FOXSDR_TEST_WINDOW_FAIL", "hardware"}});
        CHECK(r.started && r.code == 1);
        CHECK(contains(r.log, " window: no software fallback: softgl\\opengl32.dll and libgallium_wgl.dll are not beside"));
        CHECK(!contains(r.log, "handing over") && !contains(r.log, "started again by process"));
        CHECK(contains(r.log, " window: error dialog suppressed for a flagged run"));
        keep.push_back(dir);
    }

    // =======================================================================
    // 5. The Dear ImGui OpenGL 3 backend refusing: no longer a silent exit
    // =======================================================================
    {
        const fs::path dir = scratch("imgui");
        const Run r = runApp(exe, dir, {{"CASCADE_SENTINEL_TEST", "1"}, {"FOXSDR_TEST_WINDOW_FAIL", "imgui"}});
        CHECK(r.started && r.code == 1);
        CHECK(contains(r.log, " window: window created"));
        CHECK(contains(r.log, " window: the Dear ImGui OpenGL 3 backend could not start (its shaders need OpenGL 3.0)"));
        CHECK(contains(r.log, " window: error dialog suppressed for a flagged run"));
        CHECK(r.log.find("Dear ImGui OpenGL 3 backend could not start") < r.log.find("error dialog suppressed"));
        // never shown, never handed over
        CHECK(!contains(r.log, " window: window shown"));
        CHECK(!contains(r.log, "handing over"));
        const std::vector<fs::path> reports = waitForReports(dir);
        CHECK(reports.size() == 1);
        if (reports.size() == 1) { checkWindowFailedReport(reports[0]); }
        keep.push_back(dir);
    }

    std::error_code ec;
    if (g_checksFailed == 0) {
        for (const fs::path& d : keep) { fs::remove_all(d, ec); }
    }
#else
    std::printf("test_window_failed_app: the Windows watcher is not under test on this platform\n");
#endif
    return g_checksFailed == 0 ? 0 : 1;
}
