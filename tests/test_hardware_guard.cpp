// test_hardware_guard.cpp - THE GUARD THAT KEEPS TESTS OFF THE PERSON'S OWN RADIO, both branches (engine fixes 5, item 1; tests/hardware_guard.hpp).
//
//   H1  the decision: only the exact value "1" allows real hardware; unset, "", "0", "yes", "true", "11", " 1" do not; the skip line begins with the marker CTest reads
//   H2  the variable is read from the process environment (set "1" -> allowed, unset -> not), restored afterwards
//   H3  test_rtlsdr_live, run as a child WITHOUT the variable (and with "0", "yes", ""), opens nothing: one line beginning "SKIP real hardware", exit 0, and it never printed
//       the line that follows an enumeration of the bus ("live check:")
//   H4  test_usb_winusb, same: it passes, says "hardware half SKIPPED (not allowed)" and never "opened"
//   H5  test_soapy_source, same: "real-bus scans SKIPPED (not allowed)", no "soapy devices:" line (that line follows a real scan)
//   H6  a source scan: every test file that calls a real-bus entry point on a code line (not a comment) includes hardware_guard.hpp, so a NEW such test cannot be added unguarded
//
// The branch with the variable set to "1" is proved on the decision (H1, H2) only: running the guarded tests with it set would open the real radio, which is the thing this guard
// exists to prevent. No radio is opened here.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hardware_guard.hpp"
#include "test_check.hpp"

namespace {

using cascade::testguard::kRealHardwareVariable;
using cascade::testguard::realHardwareAllowed;

void setVar(const char* value) {   // nullptr: unset (both copies of the environment)
    _putenv_s(kRealHardwareVariable, value != nullptr ? value : "");
    SetEnvironmentVariableA(kRealHardwareVariable, value);
}

// Runs `exe` with no window; returns its exit code (-1: could not start); `out` gets stdout and stderr.
int runChild(const std::string& exe, std::string& out) {
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
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\"";
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) { out.append(buf, got); }
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 99); }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void testDecision() {
    CHECK(realHardwareAllowed("1"));
    CHECK(!realHardwareAllowed(nullptr));
    for (const char* v : {"", "0", "yes", "true", "11", " 1", "1 ", "on", "ALL"}) { CHECK(!realHardwareAllowed(v)); }
    const std::string line = cascade::testguard::realHardwareSkipLine("test_x", "opens the dongle");
    CHECK(line.rfind("SKIP real hardware", 0) == 0);
    CHECK(has(line, kRealHardwareVariable));
    CHECK(line.find('\n') == std::string::npos);
}

void testEnvironment() {
    setVar(nullptr);
    CHECK(!realHardwareAllowed());
    setVar("1");
    CHECK(realHardwareAllowed());
    setVar("0");
    CHECK(!realHardwareAllowed());
    setVar(nullptr);
    CHECK(!realHardwareAllowed());
}

void testLiveTestIsSkipped() {
    for (const char* v : {static_cast<const char*>(nullptr), "0", "yes", ""}) {
        setVar(v);
        std::string out;
        const int code = runChild(RTLSDR_LIVE_EXE, out);
        CHECK(code == 0);
        CHECK(out.rfind("SKIP real hardware", 0) == 0);
        CHECK(has(out, "test_rtlsdr_live"));
        CHECK(!has(out, "live check:"));       // printed right after the bus is enumerated
        CHECK(!has(out, "RTL-SDR(s) bound"));
    }
    setVar(nullptr);
}

void testWinusbHalfIsSkipped() {
    setVar(nullptr);
    std::string out;
    const int code = runChild(USB_WINUSB_EXE, out);
    CHECK(code == 0);
    CHECK(has(out, "hardware half SKIPPED (not allowed)"));
    CHECK(!has(out, "opened "));
    CHECK(!has(out, "a second open while held"));
    CHECK(!has(out, "the dongle would not open"));
}

void testSoapyRealScansAreSkipped() {
    setVar(nullptr);
    std::string out;
    const int code = runChild(SOAPY_SOURCE_EXE, out);
    CHECK(code == 0);
    CHECK(has(out, "real-bus scans SKIPPED (not allowed)"));
    CHECK(!has(out, "soapy devices:"));
    CHECK(!has(out, "soapy rescan:"));
}

bool codeLineHas(const std::string& text, const std::vector<std::string>& needles, std::string& which) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line.compare(i, 2, "//") == 0) { continue; }   // blank or a comment line
        for (const std::string& n : needles) {
            if (line.find(n) != std::string::npos) { which = n; return true; }
        }
    }
    return false;
}

void testSourceScan() {
    // The entry points that reach the real bus or open a real device. A file that calls one on a code line must include the guard.
    const std::vector<std::string> needles = {"enumerateRtlSdr()", "openWinUsb(found", "SoapySource::enumerate()", "enumerateInProcess()", "enumerateIsolated()"};
    int scanned = 0;
    int guarded = 0;
    for (const auto& e : std::filesystem::directory_iterator(TESTS_SOURCE_DIR)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("test_", 0) != 0 || e.path().extension() != ".cpp" || name == "test_hardware_guard.cpp") { continue; }
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        ++scanned;
        std::string which;
        if (!codeLineHas(text, needles, which)) { continue; }
        ++guarded;
        const bool ok = has(text, "#include \"hardware_guard.hpp\"");
        if (!ok) { std::printf("UNGUARDED real-bus call %s in %s\n", which.c_str(), name.c_str()); }
        CHECK(ok);
    }
    std::printf("source scan: %d test files read, %d call a real-bus entry point (all must include hardware_guard.hpp)\n", scanned, guarded);
    CHECK(scanned > 200);
    CHECK(guarded >= 3);   // test_rtlsdr_live, test_usb_winusb, test_soapy_source: a scan that finds none is broken
}

}  // namespace

int main() {
    testDecision();
    testEnvironment();
    testLiveTestIsSkipped();
    testWinusbHalfIsSkipped();
    testSoapyRealScansAreSkipped();
    testSourceScan();
    return testSummary("test_hardware_guard");
}
