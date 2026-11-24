// test_soapy_enum_cap.cpp - the Soapy search child's captured output is bounded, and the child's whole
// job (not just the child) is ended once the child is gone (R3-10).
//
// This executable is its own helper: enumerateIsolated() is pointed at it with EnumOptions::helperPath, the
// parent spawns it with the real helper's argument (--enumerate-json), and the mode comes in an environment
// variable. The command line, pipe, inheritance and wait are the production ones; only the answer is synthetic.
//   flood   : writes 96 MiB to stdout and exits. The parent used to keep all of it (about 200 MiB at peak).
//   linger  : starts a grandchild that inherits the pipe and sleeps, then exits at once. The parent used to wait
//             on the pipe for as long as the grandchild lived.
// Windows only (the job object); elsewhere the test does nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "source/soapy_enum_proc.hpp"
#include "test_check.hpp"

#ifdef _WIN32
namespace {

constexpr const char* kModeVar = "CASCADE_TEST_ENUMCAP_MODE";

std::string selfPath() {
    char buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return std::string(buf, n);
}

int helperFlood() {
    const std::string chunk(1 << 20, 'a');
    for (int i = 0; i < 96; ++i) { std::fwrite(chunk.data(), 1, chunk.size(), stdout); }
    std::fflush(stdout);
    return 0;
}

int helperLinger() {
    // the grandchild: same executable, inherits this process's standard handles (the parent's pipe among them)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    std::string cmd = "\"" + selfPath() + "\" --sleep-grandchild";
    PROCESS_INFORMATION pi{};
    if (::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"devices\":[]}\n");
    std::fflush(stdout);
    return 0;
}

SIZE_T peakWorkingSet() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakWorkingSetSize;
}

cascade::source::EnumOptions optionsFor(unsigned long timeoutMs) {
    cascade::source::EnumOptions o;
    o.helperPath = selfPath();
    o.attempts = 1;
    o.perDriverSweep = false;
    o.allowInProcessFallback = false;
    o.timeoutMs = timeoutMs;
    return o;
}

void testOutputIsCapped() {
    ::SetEnvironmentVariableA(kModeVar, "flood");
    const SIZE_T before = peakWorkingSet();
    const auto r = cascade::source::enumerateIsolated(optionsFor(60000));
    const SIZE_T after = peakWorkingSet();
    ::SetEnvironmentVariableA(kModeVar, nullptr);
    const double grewMiB = static_cast<double>(after > before ? after - before : 0) / (1024.0 * 1024.0);
    std::printf("96 MiB of child output: peak working set grew %.1f MiB, outcome %s\n", grewMiB,
                cascade::source::enumOutcomeName(r.outcome));
    CHECK(grewMiB < 40.0);       // a bounded capture; the old code held the lot (>= 96 MiB, about 200 at peak)
    CHECK(r.devices.empty());    // and a cut-off answer is not a list of devices
}

void testGrandchildDoesNotHoldTheScan() {
    ::SetEnvironmentVariableA(kModeVar, "linger");
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = cascade::source::enumerateIsolated(optionsFor(60000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ::SetEnvironmentVariableA(kModeVar, nullptr);
    std::printf("child exited with a grandchild holding the pipe: scan took %lld ms, outcome %s\n",
                static_cast<long long>(ms), cascade::source::enumOutcomeName(r.outcome));
    CHECK(ms < 15000);  // the grandchild sleeps 40 s; the old code waited for it
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--sleep-grandchild") == 0) {
        ::Sleep(40000);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--enumerate-json") == 0) {
        const char* mode = std::getenv(kModeVar);
        if (mode != nullptr && std::strcmp(mode, "flood") == 0) { return helperFlood(); }
        if (mode != nullptr && std::strcmp(mode, "linger") == 0) { return helperLinger(); }
        return 0;
    }
    ::SetEnvironmentVariableA("LOCALAPPDATA", nullptr);  // (nothing here writes a profile: no crash directory is armed)
    testOutputIsCapped();
    testGrandchildDoesNotHoldTheScan();
    return testSummary("test_soapy_enum_cap");
}
#else
int main() { return testSummary("test_soapy_enum_cap"); }
#endif
