// report_send_guard.cpp - see report_send_guard.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/report_send_guard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>

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
#pragma comment(lib, "advapi32.lib")
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

#if defined(_WIN32)
// The path as Windows itself spells it (a mapped drive letter becomes the \\server\share it stands for, a junction its target), for the longest part of it that exists;
// what does not exist yet is appended as written. Two spellings of one folder (APPDATA as a mapped drive, the known folder as the share) then compare equal.
fs::path finalPathOf(const fs::path& in) {
    fs::path p = in;
    fs::path tail;
    for (int guard = 0; guard < 64 && !p.empty(); ++guard) {
        std::error_code ec;
        if (fs::exists(p, ec)) {
            HANDLE h = ::CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                std::vector<wchar_t> buf(2048);
                const DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), VOLUME_NAME_DOS);
                ::CloseHandle(h);
                if (n > 0 && n < buf.size()) {
                    std::wstring w(buf.data(), n);
                    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) { w = L"\\\\" + w.substr(8); }
                    else if (w.rfind(L"\\\\?\\", 0) == 0) { w = w.substr(4); }
                    return tail.empty() ? fs::path(w) : fs::path(w) / tail;
                }
            }
            break;
        }
        const fs::path parent = p.parent_path();
        if (parent == p) { break; }
        tail = p.filename() / tail;
        p = parent;
    }
    return in;
}
#endif

// A path as a comparable string: absolute where it can be, forward slashes, no trailing slash, lower case on Windows.
std::string norm(const fs::path& p) {
    std::error_code ec;
#if defined(_WIN32)
    fs::path a = finalPathOf(p);
    a = fs::weakly_canonical(a, ec);
    if (ec) { a = finalPathOf(p).lexically_normal(); }
#else
    fs::path a = fs::weakly_canonical(p, ec);
    if (ec) { a = p.lexically_normal(); }
#endif
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

// Every folder the operating system (and the user's own persistent settings) names as this user's profile, whatever this process's environment says. Anything that is
// not a usable absolute path (a failed call, an empty or odd answer) is left out; an empty list means "cannot tell", and cannot tell never silences anybody.
std::vector<std::string> osProfileRoots() {
    std::vector<std::string> out;
#if defined(_WIN32)
    // (only the roaming application-data folder: the whole profile folder would also hold a test's scratch under %TEMP%)
    for (const KNOWNFOLDERID* id : {&FOLDERID_RoamingAppData}) {
        PWSTR w = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &w)) && w != nullptr) { out.push_back(fs::path(w).string()); }
        if (w != nullptr) { ::CoTaskMemFree(w); }
    }
    // the user's own APPDATA kept in their persistent environment (HKCU\Environment): a profile deliberately moved to another drive
    wchar_t v[1024] = {};
    DWORD cb = sizeof(v) - sizeof(wchar_t);
    DWORD type = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"APPDATA", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, v, &cb) == ERROR_SUCCESS && v[0] != L'\0') {
        wchar_t ex[1024] = {};
        const DWORD n = ::ExpandEnvironmentStringsW(v, ex, 1024);
        if (n > 0 && n <= 1024) { out.push_back(fs::path(ex).string()); }
    }
#else
    struct passwd pw {};
    struct passwd* res = nullptr;
    char buf[4096];
    if (::getpwuid_r(::getuid(), &pw, buf, sizeof(buf), &res) == 0 && res != nullptr && pw.pw_dir != nullptr) { out.push_back(pw.pw_dir); }
#endif
    return out;
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

std::string outsideSendRefusalWithRoots(const std::string& url, const std::string& profileDir, const std::vector<std::string>& osRoots) {
    if (isLoopbackUrl(url)) { return std::string(); }
    if (envSet("FOXSDR_NO_OUTSIDE_REPORTS")) { return "env"; }
    if (syntheticRun()) { return "synthetic"; }
    if (envSet("FOXSDR_DIAG_DIR")) { return "diag-dir"; }
    if (legitimatelyElsewhere()) { return std::string(); }
    if (profileDir.empty()) { return "redirected"; }   // no folder for the queue at all: nothing leaves
    // THE LAST RULE ONLY EVER STOPS A TEST THAT FORGOT EVERY MARKER, NEVER A REAL USER. It refuses only when the operating system has named at least one usable profile folder and this
    // run's folder is under none of them. If the system call failed or answered nothing usable the guard cannot tell and the report is sent; a profile the system itself names (folder
    // redirection by policy, a share, a mapped drive, a persistent APPDATA on another drive) is under one of the names and is sent.
    bool anyUsable = false;
    for (const std::string& r : osRoots) {
        if (r.empty() || !fs::path(r).is_absolute()) { continue; }
        anyUsable = true;
        if (isUnder(fs::path(profileDir), fs::path(r))) { return std::string(); }
    }
    return anyUsable ? std::string("redirected") : std::string();
}

std::string outsideSendRefusal(const std::string& url, const std::string& profileDir) {
#if defined(_WIN32)
    return outsideSendRefusalWithRoots(url, profileDir, osProfileRoots());
#else
    // HOME is the real home (a drive or hermetic test points it elsewhere); XDG_CONFIG_HOME, if set, is the user's own choice and is not read.
    const char* home = std::getenv("HOME");
    return outsideSendRefusalWithRoots(url, home != nullptr ? std::string(home) : std::string(), osProfileRoots());
#endif
}

std::string outsideSendRefusal(const std::string& url) {
    return outsideSendRefusal(url, fs::path(ConfigStore::defaultPath()).parent_path().string());
}

}  // namespace cascade::core::radioreport
