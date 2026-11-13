// hardware_guard.hpp - THE RULE "NO TEST OPENS THE PERSON'S OWN RADIO UNLESS THEY ASK FOR IT" (engine fixes 5, item 1).
//
// A test that opens or walks the real USB bus, or loads a vendor driver to find radios, touches hardware that belongs to the person at the keyboard: on 2026-11-13 test_rtlsdr_live
// opened the owner's RTL-SDR during a merge gate and went red on what the dongle returned. Such a test now refuses to run unless the person asks for it by name:
//
//     FOXSDR_ALLOW_REAL_HARDWARE_TESTS=1
//
// Only the exact value "1" allows it ("0", "yes", "true" and the empty string do not). Without it the test prints ONE line that begins "SKIP real hardware" and returns 0; CTest
// reports it as SKIPPED (tests/CMakeLists.txt sets SKIP_REGULAR_EXPRESSION "SKIP real hardware" on every test that uses this header), never as passed and never as failed.
//
// The decision is a pure function so both branches are tested without a radio (tests/test_hardware_guard.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace cascade::testguard {

inline constexpr const char* kRealHardwareVariable = "FOXSDR_ALLOW_REAL_HARDWARE_TESTS";

// `value`: the content of the variable, nullptr when it is not set.
inline bool realHardwareAllowed(const char* value) {
    return value != nullptr && std::string(value) == "1";
}

inline bool realHardwareAllowed() {
    const char* v = std::getenv(kRealHardwareVariable);   // (the CRT copy; tests that change it use _putenv_s)
    return realHardwareAllowed(v);
}

// The one line a skipped test prints. Begins with the marker CTest looks for.
inline std::string realHardwareSkipLine(const char* testName, const char* what) {
    return std::string("SKIP real hardware: ") + testName + " " + what + " - set " + kRealHardwareVariable +
           "=1 to run it on purpose (nothing was opened)";
}

// For a test whose whole point is the hardware: call first in main(); when it returns true, `return 0` at once.
inline bool skipUnlessRealHardwareAllowed(const char* testName, const char* what) {
    if (realHardwareAllowed()) { return false; }
    std::printf("%s\n", realHardwareSkipLine(testName, what).c_str());
    return true;
}

}  // namespace cascade::testguard
