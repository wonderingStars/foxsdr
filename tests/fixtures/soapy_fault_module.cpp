// A REAL SoapySDR vendor module for tests/test_soapy_enum_proc.cpp: it is put on
// SOAPY_SDR_PLUGIN_PATH and loaded by the REAL cascade.exe enumeration child,
// through SoapySDR's own module loader, exactly as a vendor's module is. It
// registers one driver, "faultfixture", whose find function lists one device.
//
// WHAT IT STAGES is chosen by FOXSDR_TEST_SOAPY_FIXTURE, read once when the
// module is loaded:
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
//   anything else  a well-behaved module.
//
// Built by tests/CMakeLists.txt as a MODULE into a directory of its own, and
// never loaded by the test process itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

enum class Stage { None, Exit, Find };

Stage stageFromEnvironment() {
    const char* v = std::getenv("FOXSDR_TEST_SOAPY_FIXTURE");
    if (v == nullptr) { return Stage::None; }
    if (std::strcmp(v, "exit") == 0) { return Stage::Exit; }
    if (std::strcmp(v, "find") == 0) { return Stage::Find; }
    return Stage::None;
}

// Read at load, on the healthy path: nothing is asked of the CRT at detach.
const Stage g_stage = stageFromEnvironment();

void accessViolation() {
    volatile int* p = nullptr;
    *p = 1;
}

SoapySDR::KwargsList findFixture(const SoapySDR::Kwargs&) {
    if (g_stage == Stage::Find) { accessViolation(); }
    SoapySDR::Kwargs k;
    k["label"] = "Fault fixture";
    k["serial"] = "1";
    return SoapySDR::KwargsList{k};
}

SoapySDR::Device* makeFixture(const SoapySDR::Kwargs&) {
    throw std::runtime_error("the fault fixture opens nothing");
}

const SoapySDR::Registry registerFixture("faultfixture", &findFixture, &makeFixture,
                                         SOAPY_SDR_ABI_VERSION);

}  // namespace

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_stage == Stage::Exit) { accessViolation(); }
    return TRUE;
}
#else
__attribute__((destructor)) static void faultFixtureUnload() {
    if (g_stage == Stage::Exit) { accessViolation(); }
}
#endif
