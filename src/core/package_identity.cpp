// package_identity.cpp - see package_identity.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/package_identity.hpp"

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "core/i18n.hpp"  // FOX_TR_NOOP: the stand-down sentence is drawn through tr()

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace cascade::core {

namespace {

#if defined(_WIN32)
// From shared/winerror.h. Written out rather than included, because appmodel.h
// is not otherwise needed here and the value is a published, frozen constant.
constexpr long kAppmodelErrorNoPackage = 15700L;

using GetCurrentPackageFullNameFn = LONG(WINAPI*)(UINT32*, PWSTR);

// One narrowing, in one place. The package full name is ASCII by construction
// (the identity Name and Publisher hash are both restricted character sets),
// so this cannot lose anything in practice; it is written as an explicit
// per-unit conversion rather than a std::string(wide.begin(), wide.end())
// so a non-ASCII byte would become '?' rather than a mangled unit.
std::string narrow(const std::wstring& w) {
    if (w.empty()) { return {}; }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    if (n <= 0) { return {}; }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}

// The real query. Kept separate from the caching and from the seams so the
// only thing in it is the Win32 call convention.
PackageIdentity queryWindows() {
    const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) { return {}; }
    const auto fn = reinterpret_cast<GetCurrentPackageFullNameFn>(
        reinterpret_cast<void*>(::GetProcAddress(k32, "GetCurrentPackageFullName")));
    if (fn == nullptr) {
        // Pre-Windows-8 or a stripped kernel32: no app model, so no package.
        return {};
    }

    // PACKAGE_FULL_NAME_MAX_LENGTH is 127 characters; ask for the length
    // anyway and grow, so a future longer name is read rather than truncated.
    UINT32 len = 0;
    LONG rc = fn(&len, nullptr);
    if (rc == kAppmodelErrorNoPackage) { return identityFromApiResult(rc, {}); }
    if (rc != ERROR_INSUFFICIENT_BUFFER && rc != ERROR_SUCCESS) {
        return identityFromApiResult(rc, {});
    }
    if (len == 0) { return identityFromApiResult(kAppmodelErrorNoPackage, {}); }

    std::vector<wchar_t> buf(len);
    rc = fn(&len, buf.data());
    if (rc != ERROR_SUCCESS) { return identityFromApiResult(rc, {}); }
    // len includes the terminating null on success.
    std::wstring wide(buf.data());
    return identityFromApiResult(ERROR_SUCCESS, narrow(wide));
}
#else
PackageIdentity queryWindows() { return {}; }
#endif

// The environment seam, read once with the rest of the answer. "none" is the
// explicit unpackaged forcing; any other non-empty value is a fake full name.
std::optional<PackageIdentity> fakeFromEnvironment() {
    const char* v = std::getenv("FOXSDR_FAKE_PACKAGE");
    if (v == nullptr || *v == '\0') { return std::nullopt; }
    const std::string value(v);
    if (value == "none") { return PackageIdentity{false, {}}; }
    return PackageIdentity{true, value};
}

std::mutex& stateMutex() {
    static std::mutex m;
    return m;
}

// The in-process seam. Not folded into the cache below, because a test that
// sets it must be able to set it again with a different value, and a cache
// that had already answered would ignore the second.
std::optional<PackageIdentity>& testOverride() {
    static std::optional<PackageIdentity> v;
    return v;
}

}  // namespace

PackageIdentity identityFromApiResult(long apiResult, const std::string& fullName) {
    PackageIdentity id;
#if defined(_WIN32)
    constexpr long kSuccess = 0L;  // ERROR_SUCCESS
#else
    constexpr long kSuccess = 0L;
#endif
    if (apiResult == kSuccess) {
        id.packaged = true;
        id.fullName = fullName;
        return id;
    }
    // Every other code, APPMODEL_ERROR_NO_PACKAGE included: not packaged, and
    // no name to report. See the header for why an unknown code lands here.
    return id;
}

const PackageIdentity& packageIdentity() {
    // Deliberately NOT a function-local static computed once and for all: the
    // test seam has to be able to change the answer. The cache below is
    // therefore invalidated whenever an override is set or cleared.
    static PackageIdentity cached;
    static bool haveCached = false;

    std::lock_guard<std::mutex> lock(stateMutex());
    if (testOverride().has_value()) { return *testOverride(); }
    if (!haveCached) {
        if (const std::optional<PackageIdentity> fake = fakeFromEnvironment(); fake.has_value()) {
            cached = *fake;
        } else {
            cached = queryWindows();
        }
        haveCached = true;
    }
    return cached;
}

bool runningInPackage() { return packageIdentity().packaged; }

void setPackageIdentityForTest(const PackageIdentity& id) {
    std::lock_guard<std::mutex> lock(stateMutex());
    testOverride() = id;
}

void clearPackageIdentityForTest() {
    std::lock_guard<std::mutex> lock(stateMutex());
    testOverride().reset();
}

UpdateCheckDisposition updateCheckDisposition(bool packaged, bool userEnabled) {
    if (!userEnabled) { return UpdateCheckDisposition::OffByChoice; }
    if (packaged) { return UpdateCheckDisposition::StorePackage; }
    return UpdateCheckDisposition::Run;
}

const char* updateCheckStandDownLine() {
    return "update check: running from a Store package - the Store delivers updates";
}

const char* updateCheckStandDownSentence() {
    return FOX_TR_NOOP("this copy came from the Microsoft Store, which delivers its updates - so FoxSDR does "
           "not check foxsdr.com and nothing is downloaded here");
}

}  // namespace cascade::core
