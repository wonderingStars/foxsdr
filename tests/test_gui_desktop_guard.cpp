// test_gui_desktop_guard.cpp - THE GUARD THAT KEEPS gui-LABELLED TESTS OFF THE VISIBLE DESKTOP, both branches (engine fixes 4, item 3; tests/gui_desktop_guard.{hpp,cpp}).
//
//   G1  the decision: "Default" (any case) is refused, an empty name (unreadable) is refused, any other desktop name is allowed; the override "1" allows all three, any other value
//       ("0", "yes", "") does not; the refusal is ONE line that names the cure
//   G2  the program, both branches (FOXSDR_GUARD_DESKTOP_NAME stands in for the desktop, so no second desktop is needed): exit 1 and one line for "Default", exit 0 for "FoxSDRTestsEM"; the override makes "Default" pass; with nothing set the program answers for THIS test's real desktop (it must agree with `judge`)
// The wiring (every gui test requires the fixture) is asserted at configure time in tests/CMakeLists.txt and src/slint/CMakeLists.txt and was read back from ctest --show-only=json-v1
// (docs/multi-radio-records/engine-fixes-4.md).
//
// No window is opened. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gui_desktop_guard.hpp"
#include "test_check.hpp"

namespace {

using cascade::testguard::judge;

void setEnv(const char* k, const char* v) {   // v == nullptr: unset (both copies of the environment)
    _putenv_s(k, v != nullptr ? v : "");
    SetEnvironmentVariableA(k, v);
}

// Runs the guard program with no window; returns its exit code, -1 when it could not be started. `out` gets what it printed.
int runGuard(std::string& out) {
    out.clear();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + GUI_GUARD_EXE + "\"";
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char buf[512];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) { out.append(buf, got); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

int lines(const std::string& s) {
    int n = 0;
    for (char c : s) { if (c == '\n') { ++n; } }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("G1: the decision\n");
    CHECK(!judge("Default", nullptr).allowed);
    CHECK(!judge("default", nullptr).allowed);
    CHECK(!judge("DEFAULT", nullptr).allowed);
    CHECK(!judge("", nullptr).allowed);
    CHECK(judge("FoxSDRTests", nullptr).allowed);
    CHECK(judge("FoxSDRTestsEM", nullptr).allowed);
    CHECK(judge("Default2", nullptr).allowed);   // only the interactive desktop's own name is the visible one
    CHECK(judge("Default", "1").allowed && judge("", "1").allowed);
    for (const char* v : {"0", "yes", "", "true", "11"}) { CHECK(!judge("Default", v).allowed); }
    {
        const auto v = judge("Default", nullptr);
        CHECK(!v.message.empty() && v.message.find('\n') == std::string::npos);
        CHECK(v.message.find("run_on_hidden_desktop.ps1") != std::string::npos && v.message.find("FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1") != std::string::npos);
        CHECK(judge("", nullptr).message.find("REFUSED") == 0);
    }

    std::printf("G2: the program, both branches\n");
    {
        std::string out;
        setEnv("FOXSDR_ALLOW_VISIBLE_GUI_TESTS", nullptr);
        setEnv("FOXSDR_GUARD_DESKTOP_NAME", "Default");
        int code = runGuard(out);
        std::printf("  Default: exit %d: %s", code, out.c_str());
        CHECK(code == 1 && lines(out) == 1 && out.find("REFUSED") == 0);
        setEnv("FOXSDR_GUARD_DESKTOP_NAME", "FoxSDRTestsEM");
        code = runGuard(out);
        std::printf("  FoxSDRTestsEM: exit %d: %s", code, out.c_str());
        CHECK(code == 0 && lines(out) == 1 && out.find("FoxSDRTestsEM") != std::string::npos);
        setEnv("FOXSDR_GUARD_DESKTOP_NAME", "Default");
        setEnv("FOXSDR_ALLOW_VISIBLE_GUI_TESTS", "1");
        code = runGuard(out);
        CHECK(code == 0 && lines(out) == 1 && out.find("allowed") != std::string::npos);
        setEnv("FOXSDR_ALLOW_VISIBLE_GUI_TESTS", "0");
        CHECK(runGuard(out) == 1);
        // nothing forced: the program reads this process's own desktop, and must agree with judge() on the same name
        setEnv("FOXSDR_ALLOW_VISIBLE_GUI_TESTS", nullptr);
        setEnv("FOXSDR_GUARD_DESKTOP_NAME", nullptr);
        char name[256] = {};
        DWORD need = 0;
        HDESK d = GetThreadDesktop(GetCurrentThreadId());
        const bool read = d != nullptr && GetUserObjectInformationA(d, UOI_NAME, name, sizeof name, &need);
        CHECK(read);
        code = runGuard(out);
        std::printf("  this process's desktop '%s': exit %d\n", name, code);
        CHECK(code == (judge(read ? std::string(name) : std::string(), nullptr).allowed ? 0 : 1));
    }

    return testSummary("test_gui_desktop_guard");
}
