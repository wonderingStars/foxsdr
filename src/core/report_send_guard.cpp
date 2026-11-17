// report_send_guard.cpp - see report_send_guard.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/report_send_guard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "core/config.hpp"
#include "core/package_identity.hpp"
#include "core/sys_portable.hpp"
#include "core/telemetry.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <KnownFolders.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace cascade::core::radioreport {

namespace fs = std::filesystem;

namespace {

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' && std::string(v) != "0";
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// A path as a comparable string: absolute where it can be, forward slashes, no trailing slash, lower case on Windows.
std::string norm(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(p, ec);
    if (ec) { a = p.lexically_normal(); }
    std::string s = a.generic_string();
    while (s.size() > 1 && s.back() == '/') { s.pop_back(); }
#if defined(_WIN32)
    s = lower(s);
#endif
    return s;
}

bool isUnder(const fs::path& dir, const fs::path& base) {
    const std::string d = norm(dir);
    const std::string b = norm(base);
    if (b.empty()) { return false; }
    return d == b || (d.size() > b.size() && d.compare(0, b.size(), b) == 0 && d[b.size()] == '/');
}

// The folder the operating system names as this user's profile root, whatever the environment says. Empty when it cannot be found.
fs::path realProfileRoot() {
#if defined(_WIN32)
    PWSTR w = nullptr;
    fs::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &w)) && w != nullptr) { out = fs::path(w); }
    if (w != nullptr) { ::CoTaskMemFree(w); }
    return out;
#else
    struct passwd pw {};
    struct passwd* res = nullptr;
    char buf[4096];
    if (::getpwuid_r(::getuid(), &pw, buf, sizeof(buf), &res) == 0 && res != nullptr && pw.pw_dir != nullptr) { return fs::path(pw.pw_dir); }
    return fs::path();
#endif
}

// A real user's profile that is legitimately not the system's: a portable copy (marker beside the program) or the Store package.
bool legitimatelyElsewhere() {
    if (runningInPackage()) { return true; }
    const sys::PortablePaths p = sys::resolvePortable(sys::currentExeDir());
    return p.portable;
}

}  // namespace

bool isLoopbackUrl(const std::string& url) {
    const std::size_t s = url.find("://");
    if (s == std::string::npos) { return false; }
    std::string rest = url.substr(s + 3);
    const std::size_t slash = rest.find_first_of("/?#");
    if (slash != std::string::npos) { rest.resize(slash); }
    const std::size_t at = rest.rfind('@');
    if (at != std::string::npos) { rest.erase(0, at + 1); }   // user@host: the host is after it
    std::string host;
    if (!rest.empty() && rest[0] == '[') {
        const std::size_t close = rest.find(']');
        if (close == std::string::npos) { return false; }
        host = rest.substr(1, close - 1);
    } else {
        const std::size_t colon = rest.find(':');
        host = colon == std::string::npos ? rest : rest.substr(0, colon);
    }
    host = lower(host);
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

std::string outsideSendRefusal(const std::string& url, const std::string& profileDir) {
    if (isLoopbackUrl(url)) { return std::string(); }
    if (envSet("FOXSDR_NO_OUTSIDE_REPORTS")) { return "env"; }
    if (syntheticRun()) { return "synthetic"; }
    if (envSet("FOXSDR_DIAG_DIR")) { return "diag-dir"; }
    if (legitimatelyElsewhere()) { return std::string(); }
    const fs::path real = realProfileRoot();
    if (real.empty() || profileDir.empty()) { return "redirected"; }   // cannot prove it is the user's own: nothing leaves
#if defined(_WIN32)
    return isUnder(fs::path(profileDir), real) ? std::string() : std::string("redirected");
#else
    // HOME is the real home (a drive or hermetic test points it elsewhere); XDG_CONFIG_HOME, if set, is the user's own choice and is not read.
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') { return "redirected"; }
    return norm(fs::path(home)) == norm(real) ? std::string() : std::string("redirected");
#endif
}

std::string outsideSendRefusal(const std::string& url) {
    return outsideSendRefusal(url, fs::path(ConfigStore::defaultPath()).parent_path().string());
}

}  // namespace cascade::core::radioreport
