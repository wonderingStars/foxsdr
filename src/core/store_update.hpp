// store_update.hpp - "does the Microsoft Store have a newer FoxSDR for this
// copy, and will Windows install it now?"
//
// WHY THIS EXISTS (0.99.53, owner-approved 2026-09-30). A copy installed from
// the Microsoft Store never ran the foxsdr.com update check, on purpose: the
// Store is that copy's update channel (see package_identity.hpp). But the
// Store's own background updating was measured to be slow - several Store
// installs were still relaunching on 0.99.26 more than a day after the next
// package went live - and a Store user was never told. So a packaged copy now
// ASKS THE STORE, through Windows' own API, and offers the update in the app.
//
// WHAT IT TALKS TO. Only the Microsoft Store service, through
// Windows.Services.Store.StoreContext. Never foxsdr.com, and nothing of ours is
// sent: the Store identifies the app by its package identity, which Windows
// supplies. Nothing installs unless the user presses the key AND confirms the
// dialog Windows itself shows.
//
// WHAT IS PURE AND WHAT IS NOT. The mapping from the API's final state to a
// plain outcome, the error text and the test seam's parse are pure functions
// (and are where the unit tests live); the two blocking calls are the only
// WinRT in the program, and WinRT stays inside store_update.cpp - this header
// is included on Linux, where both calls answer "not available".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_STORE_UPDATE_HPP
#define CASCADE_CORE_STORE_UPDATE_HPP

#include <cstdint>
#include <string>

namespace cascade::core {

// What one "does the Store have an update" question came back with. BY VALUE:
// the worker that produced it writes nothing anybody else owns, so it can be
// abandoned at quit (core::AbandonableTask).
struct StoreUpdateCheck {
    bool ok = false;         // the Store answered (an answer of "none" is ok)
    int updates = 0;         // packages with an update: the app, plus any optional ones
    bool mandatory = false;  // any of them marked "mandatory" in Partner Center
    std::string error;       // when !ok: "0x803F6107: <Windows' own message>"
};

// What the install request came to.
enum class StoreInstallResult {
    Installed,        // Windows reports the update installed
    Cancelled,        // the user declined one of Windows' two dialogs
    NothingToInstall, // the Store no longer lists an update for this copy
    Failed,           // anything else; `reasonKey` and `detail` say what
};

struct StoreInstallOutcome {
    StoreInstallResult result = StoreInstallResult::Failed;
    // An ENGLISH sentence marked with FOX_TR_NOOP, for the GUI to pass through
    // tr(); null when the result needs no reason.
    const char* reasonKey = nullptr;
    // Untranslated detail appended after the reason: an HRESULT and Windows'
    // own (already localised) message, or the raw state number. May be empty.
    std::string detail;
};

// THE PURE MAPPING from StorePackageUpdateResult.OverallState to an outcome.
// The argument is the enum's published integer value
// (Windows.Services.Store.StorePackageUpdateState: Pending 0, Downloading 1,
// Deploying 2, Completed 3, Canceled 4, OtherError 5, ErrorLowBattery 6,
// ErrorWiFiRecommended 7, ErrorWiFiRequired 8), so this compiles and is
// tested everywhere; store_update.cpp static_asserts those numbers against
// the SDK header on Windows. Any value not in that list is Failed, with the
// number in `detail`.
StoreInstallOutcome storeInstallOutcomeFromState(int overallState);

// "0x803F6107: <message>". The code is always there and always 8 hex digits,
// because it is what a support conversation searches for; the message is
// Windows' own and may be empty.
std::string storeErrorText(std::uint32_t hresult, const std::string& message);

// --- THE TEST SEAM -----------------------------------------------------------
//
// FOXSDR_FAKE_STORE_UPDATE=none|available|mandatory|error makes BOTH calls
// below return a canned answer WITHOUT touching WinRT or the network, and log
// a line that says so ("store update: FOXSDR_FAKE_STORE_UPDATE=..."), so a
// ctest `--frames` run can drive the whole GUI path on a machine with no
// package and no Store. FOXSDR_FAKE_STORE_INSTALL=installed|cancelled|failed
// chooses the fake install's outcome (installed when unset).
//
// SAFE AS AN ENVIRONMENT HOOK for the same reason FOXSDR_FAKE_PACKAGE is: the
// most it can do is make a copy believe the Store has, or has not, an update.
// It cannot install, download or send anything - the fake install request is
// a canned answer, not a request.
enum class StoreUpdateFake { Off, None, Available, Mandatory, Error };

// Pure: "none" / "available" / "mandatory" / "error" and nothing else; null,
// empty or anything unrecognised is Off.
StoreUpdateFake parseStoreUpdateFake(const char* value);

// The live seam, read from the environment on each call (cheap, and a test in
// one process can change it).
StoreUpdateFake storeUpdateFake();

// --- THE TWO BLOCKING CALLS --------------------------------------------------
//
// Both are meant for a worker thread: each initialises the Windows Runtime on
// the calling thread (multi-threaded apartment) for the duration of the call
// and uninitialises it afterwards, so the GUI thread's COM state is never
// touched. Neither throws; every failure is in the outcome.
//
// `ownerWindow` is the main window's HWND, as a pointer so <windows.h> stays
// out of this header. It is handed to IInitializeWithWindow::Initialize before
// the Store is asked anything - see store_update.cpp for Microsoft's
// documentation of why a desktop app must.
StoreUpdateCheck checkStoreForUpdates(void* ownerWindow);

// Re-asks the Store for its updates INSIDE the call (so no WinRT object ever
// crosses a thread or outlives it) and passes them to
// RequestDownloadAndInstallStorePackageUpdatesAsync. Windows shows its own
// consent dialogs; a successful install may close FoxSDR before this returns.
StoreInstallOutcome requestStoreUpdateInstall(void* ownerWindow);

// The one log line a packaged copy writes when it asks the Store.
const char* storeUpdateAskLine();

}  // namespace cascade::core

#endif  // CASCADE_CORE_STORE_UPDATE_HPP
