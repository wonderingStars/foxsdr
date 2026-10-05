// A REAL SoapySDR vendor module for tests/test_soapy_enum_proc.cpp: it is put on
// SOAPY_SDR_PLUGIN_PATH and loaded by the REAL cascade.exe enumeration child,
// through SoapySDR's own module loader, exactly as a vendor's module is. It
// registers one driver, FIXTURE_DRIVER ("faultfixture" unless the build says
// otherwise), whose find function lists one device.
//
// WHAT IT STAGES is chosen by FOXSDR_TEST_SOAPY_FIXTURE, read once when the
// module is loaded - by the "faultfixture" build only (FIXTURE_STAGES):
//
//   exit  THE FIELD FAULT OF 2026-10-01 (crash reports 4138700E14D784C6,
//         3C2F1A0F27A8FD35 and 650B88A1735695DB, 0.99.57 on Windows 10 19045):
//         an access violation as the module is DETACHED from a process that
//         is exiting. There, SoapyAudio's probe had initialised a Native
//         Instruments ASIO driver, and the child that had already written its
//         answer died at exit - in DLL_PROCESS_DETACH, below
//         __scrt_common_main_seh, ucrtbase's exit and ntdll's loader
//         shutdown - so its answer was thrown away, it was re-probed, swept,
//         and three reports were filed for one scan.
//   find  an access violation inside the find function, on the probe thread
//         SoapySDR's walk runs it on - the shape of the libusb fault - so the
//         child's own crash report can be checked for what it names.
//   late  an access violation in a DLL the find function maps itself
//         (FOXSDR_TEST_SOAPY_LATE_DLL, tests/fixtures/late_fault_dll.cpp) -
//         the shape of the 2026-10-01 ASIO driver, which RtAudio maps in the
//         middle of SoapyAudio's probe, after the child has refreshed its
//         crash handler's module table for the modules it loaded up front.
//   anything else  a well-behaved module.
//
// WHAT IT RECORDS, in every build, when FOXSDR_TEST_SOAPY_FIXTURE_UNLOADS
// names a file: "<driver> closed" when its API session object is destroyed
// and "<driver> detached" when the module is detached. The session is
// SoapySDRPlay3's sdrplay_api singleton reduced to its shape - a
// function-local static made by the find function, whose destructor is
// where sdrplay_api_Close runs - so the "sdrplay" build (soapy_sdrplay_fixture,
// tests/CMakeLists.txt) shows whether the child really closed that API
// before it ended, and the "faultfixture" build shows that no other module
// was detached.
//
// Built by tests/CMakeLists.txt as a MODULE into a directory of its own, and
// never loaded by the test process itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef FIXTURE_DRIVER
#define FIXTURE_DRIVER "faultfixture"
#endif
#ifndef FIXTURE_LABEL
#define FIXTURE_LABEL "Fault fixture"
#endif
#ifndef FIXTURE_STAGES
#define FIXTURE_STAGES 1
#endif

namespace {

enum class Stage { None, Exit, Find, Late };

Stage stageFromEnvironment() {
#if FIXTURE_STAGES
    const char* v = std::getenv("FOXSDR_TEST_SOAPY_FIXTURE");
    if (v == nullptr) { return Stage::None; }
    if (std::strcmp(v, "exit") == 0) { return Stage::Exit; }
    if (std::strcmp(v, "find") == 0) { return Stage::Find; }
    if (std::strcmp(v, "late") == 0) { return Stage::Late; }
#endif
    return Stage::None;
}

// Read at load, on the healthy path: nothing is asked of the CRT at detach
// beyond the one append below.
const Stage g_stage = stageFromEnvironment();
const std::string g_unloadLog = [] {
    const char* v = std::getenv("FOXSDR_TEST_SOAPY_FIXTURE_UNLOADS");
    return std::string(v == nullptr ? "" : v);
}();

void record(const char* what) {
    if (g_unloadLog.empty()) { return; }
    if (std::FILE* f = std::fopen(g_unloadLog.c_str(), "a")) {
        std::fprintf(f, "%s %s\n", FIXTURE_DRIVER, what);
        std::fclose(f);
    }
}

// SoapySDRPlay3's sdrplay_api singleton, reduced: opened by the find
// function, closed only by its destructor - which runs when the module is
// unloaded (or the process exits through the CRT), never on TerminateProcess.
struct ApiSession {
    ~ApiSession() { record("closed"); }
};

ApiSession& apiSession() {
    static ApiSession instance;
    return instance;
}

void accessViolation() {
    volatile int* p = nullptr;
    *p = 1;
}

// THE FAULT IN A DLL MAPPED DURING THE PROBE (stage "late"): the shape of
// SoapyAudio's RtAudio loading an ASIO driver while its find function runs.
// The child refreshed its crash handler's module table once this module was
// loaded and before any find function ran, so a DLL mapped here is in no
// table - tests/fixtures/late_fault_dll.cpp, at the path the test names.
void callIntoLateDll() {
#ifdef _WIN32
    const char* path = std::getenv("FOXSDR_TEST_SOAPY_LATE_DLL");
    if (path == nullptr || *path == '\0') { return; }
    const HMODULE dll = ::LoadLibraryA(path);
    if (dll == nullptr) { return; }
    using Fault = void (*)();
    const Fault fault = reinterpret_cast<Fault>(
        reinterpret_cast<void*>(::GetProcAddress(dll, "lateFixtureFault")));
    if (fault != nullptr) { fault(); }
#endif
}

SoapySDR::KwargsList findFixture(const SoapySDR::Kwargs&) {
    if (g_stage == Stage::Find) { accessViolation(); }
    if (g_stage == Stage::Late) { callIntoLateDll(); }
    (void)apiSession();
    SoapySDR::Kwargs k;
    k["label"] = FIXTURE_LABEL;
    k["serial"] = "1";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeFixture(const SoapySDR::Kwargs&) {
    throw std::runtime_error("the fault fixture opens nothing");
}

const SoapySDR::Registry registerFixture(FIXTURE_DRIVER, &findFixture, &makeFixture,
                                         SOAPY_SDR_ABI_VERSION);

}  // namespace

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        record("detached");
        if (g_stage == Stage::Exit) { accessViolation(); }
    }
    return TRUE;
}
#else
__attribute__((destructor)) static void faultFixtureUnload() {
    record("detached");
    if (g_stage == Stage::Exit) { accessViolation(); }
}
#endif
