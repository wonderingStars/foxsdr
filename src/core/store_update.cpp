// store_update.cpp - see store_update.hpp.
//
// WHAT MICROSOFT DOCUMENTS, AND WHAT THIS FILE DOES ABOUT IT (checked
// 2026-09-30 on learn.microsoft.com):
//
//  1. THE OWNER WINDOW IS REQUIRED. "In-app purchases and trials", section
//     "Using the StoreContext class with the Desktop Bridge": a Win32 desktop
//     application "must configure the StoreContext object to specify which
//     application window is the owner window for modal dialogs", and one that
//     does not "will return inaccurate data or errors". For C++ the steps are:
//     include shobjidl.h, cast the StoreContext to IInitializeWithWindow and
//     call Initialize with the owner's HWND. "Display WinRT UI objects that
//     depend on CoreWindow" lists Windows.Services.Store.StoreContext among the
//     classes that implement IInitializeWithWindow and shows the C++/WinRT form,
//     `obj.as<::IInitializeWithWindow>()->Initialize(hWnd)`. So bindOwner()
//     below runs before EITHER call - the reference pages for
//     GetAppAndOptionalStorePackageUpdatesAsync and
//     RequestDownloadAndInstallStorePackageUpdatesAsync both list
//     0x80070578 (ERROR_INVALID_WINDOW_HANDLE) as "did not configure the
//     StoreContext object to specify which application window is the owner".
//
//  2. THE THREAD. The RequestDownloadAndInstall... reference page also says
//     "This method must be called on the UI thread", and gives the same
//     0x80070578 for a call that was not. That sentence is written for UWP,
//     where the UI thread is the one owning the CoreWindow a dialog attaches
//     to; a desktop app has no CoreWindow, which is exactly what the
//     IInitializeWithWindow step replaces. Microsoft Q&A 1063594 is a Win32
//     desktop game that got "This function must be called from a UI thread"
//     while provably ON its UI thread, and was fixed by the owner-window step,
//     not by moving threads. The calls here are therefore made on a WORKER, in
//     a multi-threaded apartment the worker initialises for itself, with the
//     main window as owner - which keeps the GUI thread's COM state untouched
//     and its frame loop running while Windows' dialogs are up. A 0x80070578
//     from the real Store would mean this reading is wrong; it is reported
//     verbatim in the outcome ("could not install: 0x80070578 ..."), never
//     swallowed.
//
//  3. ASKING OFTEN IS HARMLESS. GetAppAndOptionalStorePackageUpdatesAsync
//     "limits how often it checks for new updates": no more than one check
//     every 30 minutes and ten in 24 hours, beyond which it "returns the last
//     known status instead of performing a new check". The install request
//     re-asks inside its own call and gets that cached answer.
//
//  4. WHAT THE USER SEES. RequestDownloadAndInstallStorePackageUpdatesAsync
//     "displays a UI dialog that requests permission"; after the download a
//     second dialog asks to install and "warns the user that the app might
//     need to restart". Declining either gives OverallState Canceled. And
//     "Under normal circumstances, app will terminate here" (MSIX: "Update
//     Store-published apps from your code") - which is why the banner says
//     FoxSDR may close.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/store_update.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/i18n.hpp"  // FOX_TR_NOOP: the reasons are drawn through tr()

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// unknwn.h BEFORE any C++/WinRT header: it is what lets winrt::Windows::...
// objects be queried for a classic COM interface (IInitializeWithWindow).
#include <unknwn.h>
#include <shobjidl_core.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Services.Store.h>
#endif

namespace cascade::core {

namespace {

// The published values of Windows.Services.Store.StorePackageUpdateState.
// Held to the SDK's own enum below, so a header that ever disagreed would stop
// the build rather than mis-report an install.
constexpr int kStatePending = 0;
constexpr int kStateDownloading = 1;
constexpr int kStateDeploying = 2;
constexpr int kStateCompleted = 3;
constexpr int kStateCanceled = 4;
constexpr int kStateOtherError = 5;
constexpr int kStateErrorLowBattery = 6;
constexpr int kStateErrorWiFiRecommended = 7;
constexpr int kStateErrorWiFiRequired = 8;

#if defined(_WIN32)
using winrt::Windows::Services::Store::StorePackageUpdateState;
static_assert(static_cast<int>(StorePackageUpdateState::Pending) == kStatePending);
static_assert(static_cast<int>(StorePackageUpdateState::Downloading) == kStateDownloading);
static_assert(static_cast<int>(StorePackageUpdateState::Deploying) == kStateDeploying);
static_assert(static_cast<int>(StorePackageUpdateState::Completed) == kStateCompleted);
static_assert(static_cast<int>(StorePackageUpdateState::Canceled) == kStateCanceled);
static_assert(static_cast<int>(StorePackageUpdateState::OtherError) == kStateOtherError);
static_assert(static_cast<int>(StorePackageUpdateState::ErrorLowBattery) ==
              kStateErrorLowBattery);
static_assert(static_cast<int>(StorePackageUpdateState::ErrorWiFiRecommended) ==
              kStateErrorWiFiRecommended);
static_assert(static_cast<int>(StorePackageUpdateState::ErrorWiFiRequired) ==
              kStateErrorWiFiRequired);
#endif

// The code the fake "error" answers with: the one Microsoft documents for "the
// Store doesn't have any knowledge about the app" (In-app purchases and
// trials, "Test your in-app purchase or trial implementation") - the answer a
// package registered outside the Store gets, so the fake looks like the most
// likely real failure.
constexpr std::uint32_t kFakeErrorCode = 0x803F6107u;

const char* fakeName(StoreUpdateFake f) {
    switch (f) {
    case StoreUpdateFake::None: return "none";
    case StoreUpdateFake::Available: return "available";
    case StoreUpdateFake::Mandatory: return "mandatory";
    case StoreUpdateFake::Error: return "error";
    case StoreUpdateFake::Off: break;
    }
    return "off";
}

StoreUpdateCheck fakeCheck(StoreUpdateFake f) {
    StoreUpdateCheck out;
    switch (f) {
    case StoreUpdateFake::None: out.ok = true; break;
    case StoreUpdateFake::Available:
        out.ok = true;
        out.updates = 1;
        break;
    case StoreUpdateFake::Mandatory:
        out.ok = true;
        out.updates = 1;
        out.mandatory = true;
        break;
    case StoreUpdateFake::Error:
    case StoreUpdateFake::Off:
        out.error = storeErrorText(kFakeErrorCode, "fake: the Store does not know this app");
        break;
    }
    return out;
}

StoreInstallOutcome fakeInstall(StoreUpdateFake f) {
    if (f == StoreUpdateFake::None) {
        return {StoreInstallResult::NothingToInstall, nullptr, {}};
    }
    if (f != StoreUpdateFake::Available && f != StoreUpdateFake::Mandatory) {
        StoreInstallOutcome o;
        o.reasonKey = FOX_TR_NOOP("the Microsoft Store could not be asked");
        o.detail = storeErrorText(kFakeErrorCode, "fake: the Store does not know this app");
        return o;
    }
    const char* how = std::getenv("FOXSDR_FAKE_STORE_INSTALL");
    const std::string h = (how != nullptr) ? how : "";
    if (h == "cancelled") { return storeInstallOutcomeFromState(kStateCanceled); }
    if (h == "failed") { return storeInstallOutcomeFromState(kStateOtherError); }
    return storeInstallOutcomeFromState(kStateCompleted);
}

const char* resultName(StoreInstallResult r) {
    switch (r) {
    case StoreInstallResult::Installed: return "installed";
    case StoreInstallResult::Cancelled: return "cancelled by the user";
    case StoreInstallResult::NothingToInstall: return "nothing to install";
    case StoreInstallResult::Failed: break;
    }
    return "failed";
}

void logInstall(const StoreInstallOutcome& o) {
    std::string line = resultName(o.result);
    if (o.reasonKey != nullptr) { line += std::string(" - ") + o.reasonKey; }
    if (!o.detail.empty()) { line += " (" + o.detail + ")"; }
    diagLogf("store update: install request ended: %s", line.c_str());
}

#if defined(_WIN32)
// The worker's own apartment, for exactly the length of one call. S_FALSE
// (this pool thread had already joined the MTA) is success and needs its own
// CoUninitialize too. RPC_E_CHANGED_MODE means somebody left the thread in a
// single-threaded apartment; blocking on a WinRT operation there could wait
// on a message pump nobody runs, so the call refuses instead.
class MtaScope {
public:
    MtaScope() : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~MtaScope() {
        if (SUCCEEDED(hr_)) { ::CoUninitialize(); }
    }
    MtaScope(const MtaScope&) = delete;
    MtaScope& operator=(const MtaScope&) = delete;
    bool ok() const { return SUCCEEDED(hr_); }
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

using winrt::Windows::Services::Store::StoreContext;

void bindOwner(const StoreContext& ctx, void* ownerWindow) {
    if (ownerWindow == nullptr) { return; }
    const auto init = ctx.as<::IInitializeWithWindow>();
    winrt::check_hresult(init->Initialize(static_cast<HWND>(ownerWindow)));
}

std::string errorOf(const winrt::hresult_error& e) {
    return storeErrorText(static_cast<std::uint32_t>(e.code().value),
                          winrt::to_string(e.message()));
}

// The WinRT work itself, kept in its own function so every WinRT object is
// released before MtaScope's CoUninitialize runs.
StoreUpdateCheck checkInApartment(void* ownerWindow) {
    StoreUpdateCheck out;
    try {
        const StoreContext ctx = StoreContext::GetDefault();
        bindOwner(ctx, ownerWindow);
        const auto updates = ctx.GetAppAndOptionalStorePackageUpdatesAsync().get();
        out.ok = true;
        out.updates = static_cast<int>(updates.Size());
        for (const auto& u : updates) {
            if (u.Mandatory()) { out.mandatory = true; }
        }
    } catch (const winrt::hresult_error& e) {
        out.error = errorOf(e);
    } catch (const std::exception& e) {
        out.error = std::string("exception: ") + e.what();
    } catch (...) {
        out.error = "unknown exception";
    }
    return out;
}

StoreInstallOutcome installInApartment(void* ownerWindow) {
    StoreInstallOutcome out;
    try {
        const StoreContext ctx = StoreContext::GetDefault();
        bindOwner(ctx, ownerWindow);
        const auto updates = ctx.GetAppAndOptionalStorePackageUpdatesAsync().get();
        if (updates.Size() == 0) {
            out.result = StoreInstallResult::NothingToInstall;
            return out;
        }
        diagLogf("store update: asking Windows to download and install %u package update(s)",
                 static_cast<unsigned>(updates.Size()));
        const auto result = ctx.RequestDownloadAndInstallStorePackageUpdatesAsync(updates).get();
        out = storeInstallOutcomeFromState(static_cast<int>(result.OverallState()));
    } catch (const winrt::hresult_error& e) {
        out = StoreInstallOutcome{};
        out.reasonKey = FOX_TR_NOOP("Windows refused the request");
        out.detail = errorOf(e);
    } catch (const std::exception& e) {
        out = StoreInstallOutcome{};
        out.reasonKey = FOX_TR_NOOP("Windows refused the request");
        out.detail = std::string("exception: ") + e.what();
    } catch (...) {
        out = StoreInstallOutcome{};
        out.reasonKey = FOX_TR_NOOP("Windows refused the request");
        out.detail = "unknown exception";
    }
    return out;
}
#endif

}  // namespace

StoreInstallOutcome storeInstallOutcomeFromState(int overallState) {
    StoreInstallOutcome o;
    switch (overallState) {
    case kStateCompleted: o.result = StoreInstallResult::Installed; return o;
    case kStateCanceled: o.result = StoreInstallResult::Cancelled; return o;
    case kStateOtherError:
        o.reasonKey = FOX_TR_NOOP("the Microsoft Store reported an error");
        return o;
    case kStateErrorLowBattery:
        o.reasonKey = FOX_TR_NOOP("the battery is too low - plug in and try again");
        return o;
    case kStateErrorWiFiRecommended:
    case kStateErrorWiFiRequired:
        o.reasonKey = FOX_TR_NOOP("the Microsoft Store wants a Wi-Fi connection for this download");
        return o;
    case kStatePending:
    case kStateDownloading:
    case kStateDeploying:
    default: break;
    }
    // A state that is not final, or a number the SDK did not have: the request
    // ended without saying it had finished, and that is a failure to report.
    o.reasonKey = FOX_TR_NOOP("the Microsoft Store stopped before the update was installed");
    o.detail = "state " + std::to_string(overallState);
    return o;
}

std::string storeErrorText(std::uint32_t hresult, const std::string& message) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08X", static_cast<unsigned>(hresult));
    std::string out(code);
    // Windows' messages end in "\r\n"; a banner line should not.
    std::string m = message;
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) {
        m.pop_back();
    }
    if (!m.empty()) { out += ": " + m; }
    return out;
}

StoreUpdateFake parseStoreUpdateFake(const char* value) {
    if (value == nullptr) { return StoreUpdateFake::Off; }
    const std::string v(value);
    if (v == "none") { return StoreUpdateFake::None; }
    if (v == "available") { return StoreUpdateFake::Available; }
    if (v == "mandatory") { return StoreUpdateFake::Mandatory; }
    if (v == "error") { return StoreUpdateFake::Error; }
    return StoreUpdateFake::Off;
}

StoreUpdateFake storeUpdateFake() {
    return parseStoreUpdateFake(std::getenv("FOXSDR_FAKE_STORE_UPDATE"));
}

const char* storeUpdateAskLine() {
    return "update check: running from a Store package - asking the Microsoft Store, not foxsdr.com";
}

StoreUpdateCheck checkStoreForUpdates(void* ownerWindow) {
    StoreUpdateCheck out;
    if (const StoreUpdateFake f = storeUpdateFake(); f != StoreUpdateFake::Off) {
        diagLogf("store update: FOXSDR_FAKE_STORE_UPDATE=%s - canned answer, the Store was not "
                 "asked",
                 fakeName(f));
        out = fakeCheck(f);
    } else {
#if defined(_WIN32)
        const MtaScope apartment;
        if (!apartment.ok()) {
            out.error = storeErrorText(static_cast<std::uint32_t>(apartment.hr()),
                                       "the worker thread could not join the Windows Runtime");
        } else {
            out = checkInApartment(ownerWindow);
        }
#else
        (void)ownerWindow;
        out.error = "the Microsoft Store exists only on Windows";
#endif
    }
    if (out.ok) {
        diagLogf("store update: the Store lists %d update(s)%s", out.updates,
                 out.mandatory ? ", at least one mandatory" : "");
    } else {
        diagWarnf("store update: the check did not complete: %s", out.error.c_str());
        // Counted (0.99.64, core/health_events.hpp): an update check that did not
        // complete, and nothing of what it said.
        health::noteUpdateCheckFailed();
    }
    return out;
}

StoreInstallOutcome requestStoreUpdateInstall(void* ownerWindow) {
    StoreInstallOutcome out;
    if (const StoreUpdateFake f = storeUpdateFake(); f != StoreUpdateFake::Off) {
        diagLogf("store update: FOXSDR_FAKE_STORE_UPDATE=%s - canned install outcome, nothing was "
                 "requested",
                 fakeName(f));
        out = fakeInstall(f);
    } else {
#if defined(_WIN32)
        const MtaScope apartment;
        if (!apartment.ok()) {
            out.reasonKey = FOX_TR_NOOP("Windows refused the request");
            out.detail = storeErrorText(static_cast<std::uint32_t>(apartment.hr()),
                                        "the worker thread could not join the Windows Runtime");
        } else {
            out = installInApartment(ownerWindow);
        }
#else
        (void)ownerWindow;
        out.reasonKey = FOX_TR_NOOP("the Microsoft Store could not be asked");
        out.detail = "the Microsoft Store exists only on Windows";
#endif
    }
    logInstall(out);
    // The Store's install request that did not happen (not one the user
    // declined): counted as "the update could not be started" (0.99.64).
    if (out.result == StoreInstallResult::Failed) { health::noteUpdateRunFailed(); }
    return out;
}

}  // namespace cascade::core
