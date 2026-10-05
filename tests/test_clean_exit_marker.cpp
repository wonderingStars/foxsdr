// The clean-exit marker, held against the PLUGIN stage of the teardown.
//
// WHY THIS FILE EXISTS. telemetryCleanExit is the one fact that makes the
// next start say "the last run did not end normally" - it is the crash
// counter, the trigger for offering a report, and the `last-run-unclean` line
// of the diagnostics bundle. test_diag_hang.cpp holds it to two things: false
// on disk while the process lives, and true after a clean exit - and, by
// reading app_window.cpp, that it is written after pipeline_.stop().
//
// What none of that sees is the stretch AFTER pipeline_.stop(). AppWindow::
// run() wrote the marker, waited for it to land, and only then ran
// patchStopAll() and detachAndUnloadPlugins() - the stage in which every
// plugin's destroy() runs. A destroy() is third-party code: a worker join, a
// vendor close, a flush. One that never returns leaves the window on screen
// and not responding, the user closes it from the taskbar (End task is
// TerminateProcess), and the file on disk already says "clean". Field report,
// FoxSDR 0.99.59, Radar Sweep running: "I will close it via the task bar and
// when I restart it does not recognize that it crashed", with the next
// session's bundle reading last-run-unclean: no, crashes: 0 over 79 launches.
//
// HOW IT IS STAGED. CASCADE_DIAG_UNLOAD_STALL_MS (bounded runs only, beside
// CASCADE_DIAG_SHUTDOWN_STALL_MS) wedges the teardown at the end of the plugin
// unload and prints a line when it does. The test waits for that line - so the
// process is PROVABLY inside the stage, past every save the teardown makes -
// reads the config the way the next start would, and then kills the process
// the way End task does. The positive control lets the same stall finish and
// requires the marker to arrive, so a fix that simply stopped writing it fails
// too.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-cleanexit-") + tag + "-" +
                   std::to_string(::GetCurrentProcessId()));
}

struct Child {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE readEnd = nullptr;  // the child's stdout
    std::string seen;          // everything read from it so far
    bool started = false;
};

// Starts `cascade.exe --frames 3` hermetically, with its config in `cfg` and
// the unload stall set to `stallMs`, and its stdout on a pipe this test reads.
Child startApp(const fs::path& cfg, int stallMs) {
    Child c;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&c.readEnd, &writeEnd, &sa, 0)) { return c; }
    ::SetHandleInformation(c.readEnd, HANDLE_FLAG_INHERIT, 0);

    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfg.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_DIAG_UNLOAD_STALL_MS",
                              stallMs > 0 ? std::to_string(stallMs).c_str() : nullptr);

    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    std::string cmd = "\"" + exe + "\" --frames 3";
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
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    ::SetEnvironmentVariableA("CASCADE_DIAG_UNLOAD_STALL_MS", nullptr);
    ::CloseHandle(writeEnd);  // the child holds its own copy
    if (ok) {
        c.process = pi.hProcess;
        c.thread = pi.hThread;
        c.started = true;
    }
    return c;
}

// Reads the child's stdout until `needle` has appeared or the process ends or
// `timeoutMs` passes. Returns whether the needle was seen.
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

void closeChild(Child& c) {
    if (c.thread != nullptr) { ::CloseHandle(c.thread); }
    if (c.process != nullptr) { ::CloseHandle(c.process); }
    if (c.readEnd != nullptr) { ::CloseHandle(c.readEnd); }
    c = Child{};
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    // --- A process killed INSIDE the plugin unload is an UNCLEAN exit -------
    {
        const fs::path dir = scratchDir("killed");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path cfg = dir / "config.json";

        // 60 s: far longer than this block needs, so the stall is still going
        // when the process is killed.
        Child c = startApp(cfg, 60000);
        CHECK(c.started);
        if (c.started) {
            const bool inside = waitForOutput(c, "--diag-unload-stall wedging the plugin unload", 60000);
            std::printf("child reached the plugin-unload stall: %s\n", inside ? "yes" : "NO");
            CHECK(inside);

            // WHAT THE NEXT START WOULD READ, with the process still wedged in
            // the stage where a plugin's destroy() runs. The teardown has by
            // now made every save it makes, so this is the file as End task
            // would leave it.
            const std::string whileWedged = readFile(cfg);
            std::printf("config while wedged: %zu bytes\n", whileWedged.size());
            CHECK(!whileWedged.empty());
            CHECK(whileWedged.find("\"telemetryCleanExit\": false") != std::string::npos);
            CHECK(whileWedged.find("\"telemetryCleanExit\": true") == std::string::npos);

            // End task is TerminateProcess.
            ::TerminateProcess(c.process, 1);
            ::WaitForSingleObject(c.process, 10000);
            const std::string afterKill = readFile(cfg);
            CHECK(afterKill.find("\"telemetryCleanExit\": false") != std::string::npos);
            CHECK(afterKill.find("\"telemetryCleanExit\": true") == std::string::npos);
        }
        closeChild(c);
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- ...and the same run left to finish still writes the marker ----------
    {
        const fs::path dir = scratchDir("finished");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path cfg = dir / "config.json";

        Child c = startApp(cfg, 1500);
        CHECK(c.started);
        if (c.started) {
            CHECK(waitForOutput(c, "--diag-unload-stall wedging the plugin unload", 60000));
            CHECK(::WaitForSingleObject(c.process, 60000) == WAIT_OBJECT_0);
            DWORD exitCode = 1;
            ::GetExitCodeProcess(c.process, &exitCode);
            CHECK(exitCode == 0);
            const std::string after = readFile(cfg);
            CHECK(after.find("\"telemetryCleanExit\": true") != std::string::npos);
        }
        closeChild(c);
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    SKIP_LINUX("the clean-exit marker run uses CreateProcess and TerminateProcess");
#endif
    return testSummary("test_clean_exit_marker");
}
