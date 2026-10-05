// A DLL THAT IS NOT MAPPED UNTIL SOMETHING ASKS FOR IT, and that faults when
// called - for tests/test_crash_late_module.cpp and the "late" stage of
// tests/fixtures/soapy_fault_module.cpp.
//
// WHY IT EXISTS. The crash handler searches a table of the modules that were
// mapped when it was last refreshed (core/diag_report.cpp), and the field has
// shown twice that a fault can land in code mapped AFTER that: a vendor's
// libusb behind a SoapySDR module (the table built at start-up could not name
// it), and, on 2026-10-01, a Native Instruments ASIO driver that RtAudio maps
// in the middle of SoapyAudio's probe - after the enumeration child had
// already refreshed its table for the vendor modules, so its report listed the
// faulting frames as nine bare addresses. A DLL loaded with LoadLibrary at the
// moment of the call is that shape in miniature. It is never linked against,
// and never put in a directory SoapySDR's loader or the test's own search path
// would map it from - that would load it BEFORE the table is refreshed and
// test nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifdef _WIN32
#include <windows.h>

extern "C" __declspec(dllexport) __declspec(noinline) void lateFixtureFault() {
    volatile int* p = nullptr;
    *p = 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
#endif
