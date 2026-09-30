// Tests for core/store_update.{hpp,cpp}: the pure parts of the Microsoft Store
// update check a packaged copy makes (0.99.53), and the test seam.
//
// WHAT CAN AND CANNOT BE TESTED HERE, stated plainly. The two blocking calls
// talk to the Microsoft Store through Windows, and a ctest run must not: it
// would make the suite depend on the Store being up and on what it said about
// a package this machine does not have. So the real calls are exercised ONLY
// through the seam (FOXSDR_FAKE_STORE_UPDATE), which returns before any WinRT
// is touched, and the parts that decide what the user is told - the mapping
// from Windows' final state, the error text - are pure and tested directly.
// The real API against a Store-identity package is a manual smoke, recorded
// in installer/msix/README.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/store_update.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <chrono>
#include <thread>

#include "core/diag_log.hpp"
#include "core/updater.hpp"
#include "test_check.hpp"

using cascade::core::StoreInstallResult;
using cascade::core::StoreUpdateFake;

namespace {

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value == nullptr) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value, 1);
    }
#endif
}

// THE SEAM'S VOCABULARY: four words and nothing else.
void testParseFake() {
    CHECK(cascade::core::parseStoreUpdateFake(nullptr) == StoreUpdateFake::Off);
    CHECK(cascade::core::parseStoreUpdateFake("") == StoreUpdateFake::Off);
    CHECK(cascade::core::parseStoreUpdateFake("none") == StoreUpdateFake::None);
    CHECK(cascade::core::parseStoreUpdateFake("available") == StoreUpdateFake::Available);
    CHECK(cascade::core::parseStoreUpdateFake("mandatory") == StoreUpdateFake::Mandatory);
    CHECK(cascade::core::parseStoreUpdateFake("error") == StoreUpdateFake::Error);
    // Anything else is OFF - and OFF means the REAL call. The test that sets a
    // misspelt value gets the real Store, which is loud (an error from a
    // machine with no Store package), not a silently different canned answer.
    CHECK(cascade::core::parseStoreUpdateFake("Available") == StoreUpdateFake::Off);
    CHECK(cascade::core::parseStoreUpdateFake("yes") == StoreUpdateFake::Off);
    CHECK(cascade::core::parseStoreUpdateFake(" none") == StoreUpdateFake::Off);
}

// WINDOWS' FINAL STATE, mapped to what the banner says. The numbers are the
// published StorePackageUpdateState values; store_update.cpp static_asserts
// them against the SDK header, so this table and the header cannot disagree.
void testInstallOutcomeFromState() {
    const auto installed = cascade::core::storeInstallOutcomeFromState(3);  // Completed
    CHECK(installed.result == StoreInstallResult::Installed);
    CHECK(installed.reasonKey == nullptr);

    // The user said no to either of Windows' dialogs. Not a failure.
    const auto cancelled = cascade::core::storeInstallOutcomeFromState(4);  // Canceled
    CHECK(cancelled.result == StoreInstallResult::Cancelled);
    CHECK(cancelled.reasonKey == nullptr);

    // Every error state is Failed WITH a reason - a failure with no words is
    // the thing the banner must never show.
    for (const int s : {5, 6, 7, 8}) {
        const auto o = cascade::core::storeInstallOutcomeFromState(s);
        CHECK(o.result == StoreInstallResult::Failed);
        CHECK(o.reasonKey != nullptr && std::strlen(o.reasonKey) > 0);
    }
    // Low battery and Wi-Fi say what the user can do about it.
    CHECK(std::strstr(cascade::core::storeInstallOutcomeFromState(6).reasonKey, "battery") !=
          nullptr);
    CHECK(std::strstr(cascade::core::storeInstallOutcomeFromState(7).reasonKey, "Wi-Fi") !=
          nullptr);
    CHECK(std::strstr(cascade::core::storeInstallOutcomeFromState(8).reasonKey, "Wi-Fi") !=
          nullptr);

    // A state that is not final (Pending 0, Downloading 1, Deploying 2) or a
    // number the SDK did not have: the request ended without finishing. That
    // is a failure, and the raw number goes with it for whoever reads the log.
    for (const int s : {0, 1, 2, 9, -1, 1000}) {
        const auto o = cascade::core::storeInstallOutcomeFromState(s);
        CHECK(o.result == StoreInstallResult::Failed);
        CHECK(o.reasonKey != nullptr);
        CHECK(o.detail == "state " + std::to_string(s));
    }
}

// THE ERROR TEXT: the code always, 8 hex digits, then Windows' own words.
void testErrorText() {
    CHECK(cascade::core::storeErrorText(0x803F6107u, "The app is not known") ==
          "0x803F6107: The app is not known");
    // Windows' messages end in CR LF; the banner line must not.
    CHECK(cascade::core::storeErrorText(0x80070578u, "Invalid window handle.\r\n") ==
          "0x80070578: Invalid window handle.");
    CHECK(cascade::core::storeErrorText(0x1u, "") == "0x00000001");
    CHECK(cascade::core::storeErrorText(0x1u, " \r\n") == "0x00000001");
}

// THE SEAM END TO END, through the same two functions the application calls.
// No WinRT is reached on any of these paths: every one is a canned answer.
void testSeamedCheck() {
    setEnv("FOXSDR_FAKE_STORE_INSTALL", nullptr);

    setEnv("FOXSDR_FAKE_STORE_UPDATE", "none");
    CHECK(cascade::core::storeUpdateFake() == StoreUpdateFake::None);
    auto c = cascade::core::checkStoreForUpdates(nullptr);
    CHECK(c.ok);
    CHECK(c.updates == 0);
    CHECK(!c.mandatory);
    CHECK(c.error.empty());
    auto i = cascade::core::requestStoreUpdateInstall(nullptr);
    CHECK(i.result == StoreInstallResult::NothingToInstall);

    setEnv("FOXSDR_FAKE_STORE_UPDATE", "available");
    c = cascade::core::checkStoreForUpdates(nullptr);
    CHECK(c.ok);
    CHECK(c.updates == 1);
    CHECK(!c.mandatory);
    i = cascade::core::requestStoreUpdateInstall(nullptr);
    CHECK(i.result == StoreInstallResult::Installed);

    setEnv("FOXSDR_FAKE_STORE_INSTALL", "cancelled");
    i = cascade::core::requestStoreUpdateInstall(nullptr);
    CHECK(i.result == StoreInstallResult::Cancelled);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "failed");
    i = cascade::core::requestStoreUpdateInstall(nullptr);
    CHECK(i.result == StoreInstallResult::Failed);
    CHECK(i.reasonKey != nullptr);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", nullptr);

    setEnv("FOXSDR_FAKE_STORE_UPDATE", "mandatory");
    c = cascade::core::checkStoreForUpdates(nullptr);
    CHECK(c.ok);
    CHECK(c.updates == 1);
    CHECK(c.mandatory);

    // "error": not ok, and the error carries a code a support conversation can
    // search for - never an empty string, never an exception.
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "error");
    c = cascade::core::checkStoreForUpdates(nullptr);
    CHECK(!c.ok);
    CHECK(c.updates == 0);
    CHECK(c.error.rfind("0x803F6107", 0) == 0);
    std::printf("  fake error reads: %s\n", c.error.c_str());
    i = cascade::core::requestStoreUpdateInstall(nullptr);
    CHECK(i.result == StoreInstallResult::Failed);
    CHECK(i.detail.rfind("0x803F6107", 0) == 0);

    setEnv("FOXSDR_FAKE_STORE_UPDATE", nullptr);
    CHECK(cascade::core::storeUpdateFake() == StoreUpdateFake::Off);

    // EVERY CANNED ANSWER SAYS SO in the log, so a log from a run with the
    // seam set can never be mistaken for one where the Store was asked.
    int fakeCheckLines = 0;
    int fakeInstallLines = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find("store update: FOXSDR_FAKE_STORE_UPDATE=") == std::string::npos) { continue; }
        if (line.find("the Store was not asked") != std::string::npos) { ++fakeCheckLines; }
        if (line.find("nothing was requested") != std::string::npos) { ++fakeInstallLines; }
    }
    std::printf("  fake lines in the log: %d check, %d install\n", fakeCheckLines,
                fakeInstallLines);
    CHECK(fakeCheckLines == 4);    // none, available, mandatory, error
    CHECK(fakeInstallLines == 5);  // none, available x3, error
}

// THE WORKER TYPE the application runs both calls on: AbandonableTask, the
// generalised UpdateCheckTask (tests/test_updater.cpp proves the abandoning).
// Here: it carries a StoreUpdateCheck by value and hands it over once.
void testTaskCarriesTheOutcome() {
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "mandatory");
    cascade::core::AbandonableTask<cascade::core::StoreUpdateCheck> task;
    task.start([] { return cascade::core::checkStoreForUpdates(nullptr); });
    cascade::core::StoreUpdateCheck out;
    CHECK(task.reap(std::chrono::milliseconds(5000)));  // finished, not abandoned
    CHECK(!task.running());
    CHECK(!task.poll(out));                              // reaped: nothing left to hand over

    task.start([] { return cascade::core::checkStoreForUpdates(nullptr); });
    bool got = false;
    for (int n = 0; n < 500 && !got; ++n) {
        got = task.poll(out);
        if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got);
    CHECK(out.ok && out.updates == 1 && out.mandatory);
    CHECK(!task.poll(out));  // once
    setEnv("FOXSDR_FAKE_STORE_UPDATE", nullptr);
}

}  // namespace

int main() {
    testParseFake();
    testInstallOutcomeFromState();
    testErrorText();
    testSeamedCheck();
    testTaskCarriesTheOutcome();
    return testSummary("test_store_update");
}
