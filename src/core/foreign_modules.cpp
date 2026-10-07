// See foreign_modules.hpp for what this is for, what "ours" means, what is disclosed
// and why the classifier compares strings. This file is the classifier, the words, the
// Windows half (the snapshot and the loader notification) and the watch that joins them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/foreign_modules.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <set>
#include <thread>

#include "core/diag_log.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <psapi.h>

#include <filesystem>
#pragma comment(lib, "psapi.lib")
#endif

namespace cascade::core {

namespace {

// ---------------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------------
bool asciiAlnum(unsigned long c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// What a name in a report may be made of. Letters and digits, and the punctuation real
// file names carry; nothing that is a separator, a quote (the log scrub turns quoted text
// into '<name>'), a comma (the list's own separator) or a control character. The '?' is
// what every other character is written as (fileNameOnly), so a name that had one reads
// back as the same name: Windows does not allow a '?' in a file name, so it never means
// anything else.
bool safeNameChar(unsigned long c) {
    if (asciiAlnum(c)) { return true; }
    switch (c) {
        case '.': case '_': case '-': case '+': case '(': case ')': case '~': case '@':
        case '&': case '!': case '=': case ';': case '{': case '}': case '[': case ']':
        case '?':
            return true;
        default:
            return false;
    }
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool lessNoCase(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char x = lowerAscii(a[i]);
        const char y = lowerAscii(b[i]);
        if (x != y) { return x < y; }
    }
    if (a.size() != b.size()) { return a.size() < b.size(); }
    return a < b;  // the same letters in another case: a fixed order, so the sort is stable
}

bool equalNoCaseNarrow(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) { return false; }
    }
    return true;
}

[[maybe_unused]] std::string lowerNarrow(std::string s) {
    for (char& c : s) { c = lowerAscii(c); }
    return s;
}

// The file system's own idea of "the same name": NTFS and FAT fold with the upper-case
// table CompareStringOrdinal uses. Elsewhere ASCII is all that is asked of it.
bool equalNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) { return false; }
    if (a.empty()) { return true; }
#if defined(_WIN32)
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                  static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    for (std::size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i];
        wchar_t y = b[i];
        if (x >= L'A' && x <= L'Z') { x = static_cast<wchar_t>(x - L'A' + L'a'); }
        if (y >= L'A' && y <= L'Z') { y = static_cast<wchar_t>(y - L'A' + L'a'); }
        if (x != y) { return false; }
    }
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Canonical paths
// ---------------------------------------------------------------------------
struct CanonPath {
    bool ok = false;                // false: not an absolute path
    std::wstring root;              // "C:", "\\server\share" or "\"
    std::vector<std::wstring> parts;
};

bool startsWithNoCase(const std::wstring& s, const wchar_t* prefix) {
    const std::size_t n = std::wcslen(prefix);
    return s.size() >= n && equalNoCase(s.substr(0, n), std::wstring(prefix));
}

void splitInto(const std::wstring& s, std::size_t from, std::vector<std::wstring>& out) {
    std::wstring cur;
    for (std::size_t i = from; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L'\\') {
            if (!cur.empty()) { out.push_back(cur); }
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
}

CanonPath canonicalise(std::wstring s) {
    CanonPath c;
    for (wchar_t& ch : s) {
        if (ch == L'/') { ch = L'\\'; }
    }
    // The prefixes a path wears: the extended-length form, its UNC spelling, the device
    // namespace and the NT object namespace. All of them name the same file as the plain
    // form, which is what the roots are written in.
    if (startsWithNoCase(s, L"\\\\?\\UNC\\")) {
        s = L"\\\\" + s.substr(8);
    } else if (s.compare(0, 4, L"\\\\?\\") == 0 || s.compare(0, 4, L"\\\\.\\") == 0 ||
               s.compare(0, 4, L"\\??\\") == 0) {
        s = s.substr(4);
    }

    std::size_t from = 0;
    if (s.size() >= 3 && s[1] == L':' && s[2] == L'\\' &&
        ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))) {
        c.root = s.substr(0, 2);
        from = 3;
    } else if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') {
        // \\server\share\...: the machine and the share are the root, because a folder
        // "beneath" it is a folder on that share.
        std::vector<std::wstring> all;
        splitInto(s, 2, all);
        if (all.size() < 2) { return c; }
        c.root = L"\\\\" + all[0] + L"\\" + all[1];
        all.erase(all.begin(), all.begin() + 2);
        c.parts = std::move(all);
        // `.` and `..` below, in the common tail.
        from = s.size() + 1;
    } else if (s.size() >= 1 && s[0] == L'\\') {
        c.root = L"\\";
        from = 1;
    } else {
        return c;  // relative, or a bare name: not something that can be shown to be ours
    }

    std::vector<std::wstring> raw = std::move(c.parts);
    c.parts.clear();
    if (from <= s.size()) { splitInto(s, from, raw); }
    for (const std::wstring& p : raw) {
        if (p == L".") { continue; }
        if (p == L"..") {
            if (!c.parts.empty()) { c.parts.pop_back(); }  // never above the root
            continue;
        }
        c.parts.push_back(p);
    }
    c.ok = true;
    return c;
}

bool beneath(const CanonPath& folder, const CanonPath& path) {
    if (!folder.ok || !path.ok) { return false; }
    // A root with nothing after it is a whole drive or share: calling everything on it "ours"
    // would turn the classifier off, so such a folder names nothing.
    if (folder.parts.empty()) { return false; }
    if (!equalNoCase(folder.root, path.root)) { return false; }
    if (path.parts.size() <= folder.parts.size()) { return false; }
    for (std::size_t i = 0; i < folder.parts.size(); ++i) {
        if (!equalNoCase(folder.parts[i], path.parts[i])) { return false; }
    }
    return true;
}

bool samePath(const CanonPath& a, const CanonPath& b) {
    if (!a.ok || !b.ok || !equalNoCase(a.root, b.root) || a.parts.size() != b.parts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
        if (!equalNoCase(a.parts[i], b.parts[i])) { return false; }
    }
    return true;
}

bool isDigits(const std::string& s, std::size_t from, std::size_t to) {
    if (from >= to) { return false; }
    for (std::size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9') { return false; }
    }
    return true;
}

// "+5 more", the tail of a list that did not fit.
bool parseMoreToken(const std::string& t, std::size_t& more) {
    static const char kTail[] = " more";
    const std::size_t tail = sizeof(kTail) - 1;
    if (t.size() < 2 + tail || t[0] != '+') { return false; }
    if (t.compare(t.size() - tail, tail, kTail) != 0) { return false; }
    if (!isDigits(t, 1, t.size() - tail) || t.size() - tail - 1 > 6) { return false; }
    more = static_cast<std::size_t>(std::strtoul(t.c_str() + 1, nullptr, 10));
    return true;
}

std::string trimSpaces(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && s[a] == ' ') { ++a; }
    while (b > a && s[b - 1] == ' ') { --b; }
    return s.substr(a, b - a);
}

// "A.dll, B.dll, +3 more" -> tokens. A name never contains a comma (safeNameChar).
std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            const std::string t = trimSpaces(cur);
            if (!t.empty()) { out.push_back(t); }
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    return out;
}

std::string moreText(std::size_t k) { return "+" + std::to_string(k) + " more"; }

// The text of a log line after its stamp and level ("12:34:56.789 info "), or empty when
// the line is not in the log's own shape.
std::string logMessage(const std::string& line) {
    if (line.size() < 14 || line[2] != ':' || line[5] != ':' || line[8] != '.' || line[12] != ' ') {
        return std::string();
    }
    const std::size_t sp = line.find(' ', 13);
    if (sp == std::string::npos) { return std::string(); }
    return line.substr(sp + 1);
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

constexpr const char* kStartPrefix = "modules: ";
constexpr const char* kArrivedPrefix = "module arrived: ";

}  // namespace

// ---------------------------------------------------------------------------
// The classifier
// ---------------------------------------------------------------------------
bool isForeignModule(const std::wstring& fullPath, const ForeignRoots& roots) {
    if (fullPath.empty()) { return false; }
    const CanonPath p = canonicalise(fullPath);
    if (!p.ok) { return true; }

    if (!roots.programFolder.empty() && beneath(canonicalise(roots.programFolder), p)) {
        return false;
    }
    if (!roots.windowsFolder.empty() && beneath(canonicalise(roots.windowsFolder), p)) {
        return false;
    }
    for (const std::wstring& v : roots.vendorFolders) {
        if (!v.empty() && beneath(canonicalise(v), p)) { return false; }
    }
    for (const std::wstring& f : roots.runtimeFiles) {
        if (!f.empty() && samePath(canonicalise(f), p)) { return false; }
    }
    return true;
}

std::string fileNameOnly(const std::wstring& path) {
    const std::size_t cut = path.find_last_of(L"\\/");
    const std::wstring leaf = (cut == std::wstring::npos) ? path : path.substr(cut + 1);
    std::string out;
    for (std::size_t i = 0; i < leaf.size(); ++i) {
        unsigned long c = static_cast<unsigned long>(leaf[i]);
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < leaf.size()) {
            const unsigned long d = static_cast<unsigned long>(leaf[i + 1]);
            if (d >= 0xDC00 && d <= 0xDFFF) {  // one code point in two units: one '?'
                ++i;
                c = 0x10000;
            }
        }
        out.push_back(c < 0x80 && (safeNameChar(c) || c == ' ') ? static_cast<char>(c) : '?');
        if (out.size() >= kForeignNameMaxBytes) { break; }
    }
    out = trimSpaces(out);
    if (out == "." || out == "..") { return std::string(); }  // a reference to a folder, not a file
    return out;
}

bool isPlainModuleName(const std::string& name) {
    if (name.empty() || name.size() > kForeignNameMaxBytes) { return false; }
    if (name.front() == ' ' || name.back() == ' ') { return false; }
    for (const char c : name) {
        const unsigned long u = static_cast<unsigned char>(c);
        if (!safeNameChar(u) && c != ' ') { return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------
std::vector<std::string> sortedUniqueNames(std::vector<std::string> names) {
    std::sort(names.begin(), names.end(), lessNoCase);
    names.erase(std::unique(names.begin(), names.end(),
                            [](const std::string& a, const std::string& b) {
                                return equalNoCaseNarrow(a, b);
                            }),
                names.end());
    return names;
}

std::string foreignStartLineText(const std::vector<std::string>& names) {
    if (names.empty()) { return "modules: none foreign"; }
    const std::string head =
        std::string(kStartPrefix) + std::to_string(names.size()) + " foreign - ";
    std::string list;
    std::size_t listed = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string add = (listed > 0 ? ", " : "") + names[i];
        const std::size_t remaining = names.size() - (i + 1);
        const std::string tail = remaining > 0 ? ", " + moreText(remaining) : std::string();
        // The first name is always listed (a name is at most 63 bytes, so it fits); every
        // later one only if the line, with the "+K more" it would then need, still fits.
        if (listed > 0 && head.size() + list.size() + add.size() + tail.size() > kForeignStartLineMaxChars) {
            break;
        }
        list += add;
        ++listed;
    }
    std::string out = head + list;
    if (listed < names.size()) { out += ", " + moreText(names.size() - listed); }
    return out;
}

std::string foreignArrivalLineText(const std::string& name, double sinceStartSec) {
    char secs[32] = {};
    std::snprintf(secs, sizeof(secs), "%.1f", sinceStartSec < 0.0 ? 0.0 : sinceStartSec);
    return std::string(kArrivedPrefix) + name + " (" + secs + " s)";
}

std::string foreignFieldText(const std::vector<std::string>& names, std::size_t unlisted) {
    if (names.empty() && unlisted == 0) { return "(none)"; }
    std::string list;
    std::size_t listed = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string add = (listed > 0 ? ", " : "") + names[i];
        const std::size_t remaining = names.size() - (i + 1) + unlisted;
        const std::string tail = remaining > 0 ? ", " + moreText(remaining) : std::string();
        if (listed > 0 && list.size() + add.size() + tail.size() > kForeignFieldMaxChars) { break; }
        list += add;
        ++listed;
    }
    const std::size_t left = names.size() - listed + unlisted;
    if (left > 0) { list += (listed > 0 ? ", " : "") + moreText(left); }
    return list;
}

std::string normaliseForeignField(const std::string& value) {
    const std::string v = trimSpaces(value);
    if (v == "(none)") { return v; }
    if (v.empty() || v[0] == '(') { return std::string(); }
    std::vector<std::string> names;
    std::size_t more = 0;
    for (const std::string& t : splitList(v)) {
        std::size_t k = 0;
        if (parseMoreToken(t, k)) {
            more += k;
        } else if (isPlainModuleName(t) && names.size() < kForeignMaxNames) {
            names.push_back(t);
        }
    }
    if (names.empty() && more == 0) { return std::string(); }
    return foreignFieldText(sortedUniqueNames(std::move(names)), more);
}

bool isForeignModulesLogLine(const std::string& line) {
    const std::string m = logMessage(line);
    return startsWith(m, kStartPrefix) || startsWith(m, kArrivedPrefix);
}

ForeignList foreignListFromLog(const std::vector<std::string>& lines) {
    ForeignList r;
    std::vector<std::string> names;
    for (const std::string& line : lines) {
        const std::string m = logMessage(line);
        if (m == "modules: none foreign") {
            r.known = true;
            names.clear();
            r.unlisted = 0;
        } else if (startsWith(m, kStartPrefix)) {
            // "modules: 3 foreign - A.dll, B.dll, +1 more"
            const std::size_t after = std::strlen(kStartPrefix);
            std::size_t digits = after;
            while (digits < m.size() && m[digits] >= '0' && m[digits] <= '9') { ++digits; }
            static const char kMid[] = " foreign - ";
            if (digits == after || m.compare(digits, sizeof(kMid) - 1, kMid) != 0) { continue; }
            r.known = true;
            names.clear();
            r.unlisted = 0;
            for (const std::string& t : splitList(m.substr(digits + sizeof(kMid) - 1))) {
                std::size_t k = 0;
                if (parseMoreToken(t, k)) {
                    r.unlisted += k;
                } else if (isPlainModuleName(t) && names.size() < kForeignMaxNames) {
                    names.push_back(t);
                }
            }
        } else if (startsWith(m, kArrivedPrefix)) {
            // "module arrived: A.dll (12.3 s)"
            const std::size_t from = std::strlen(kArrivedPrefix);
            const std::size_t end = m.rfind(" (");
            if (end == std::string::npos || end <= from) { continue; }
            const std::string name = m.substr(from, end - from);
            if (!isPlainModuleName(name) || names.size() >= kForeignMaxNames) { continue; }
            r.known = true;
            names.push_back(name);
        }
    }
    r.names = sortedUniqueNames(std::move(names));
    return r;
}

std::string foreignFieldFromLog(const std::vector<std::string>& lines) {
    const ForeignList l = foreignListFromLog(lines);
    if (!l.known) { return "(not recorded)"; }
    return foreignFieldText(l.names, l.unlisted);
}

std::vector<std::string> foreignNamesOf(const std::vector<std::wstring>& paths,
                                        const ForeignRoots& roots) {
    std::vector<std::string> names;
    for (const std::wstring& p : paths) {
        if (!isForeignModule(p, roots)) { continue; }
        const std::string n = fileNameOnly(p);
        if (!n.empty()) { names.push_back(n); }
    }
    return sortedUniqueNames(std::move(names));
}

// ---------------------------------------------------------------------------
// This process (Windows)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
namespace {

// An 8.3 short name ("C:\PROGRA~1\Nahimic\X.dll") cannot be told from a long one by a
// string, so a path that carries a tilde is expanded by the file system - for a local
// drive letter only: GetLongPathNameW on a network path can wait on the network.
std::wstring expandShortPath(const std::wstring& p) {
    if (p.find(L'~') == std::wstring::npos) { return p; }
    const bool local = p.size() >= 3 && p[1] == L':' && p[2] == L'\\';
    if (!local) { return p; }
    std::wstring buf(p.size() + 260, L'\0');
    DWORD n = ::GetLongPathNameW(p.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n >= buf.size()) {
        buf.assign(static_cast<std::size_t>(n) + 1, L'\0');
        n = ::GetLongPathNameW(p.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    }
    if (n == 0 || n >= buf.size()) { return p; }
    buf.resize(n);
    return buf;
}

std::vector<std::wstring>& ownFolderStore() {
    static std::vector<std::wstring> v;
    return v;
}
std::mutex& ownFolderMutex() {
    static std::mutex m;
    return m;
}

// The C and C++ runtime, by name. Each is asked of the loader by name and, if it is
// mapped, kept by the path the loader reports - which is wherever it really came from.
const wchar_t* const kRuntimeNames[] = {
    L"ucrtbase.dll",       L"ucrtbased.dll",          L"vcruntime140.dll",
    L"vcruntime140_1.dll", L"vcruntime140d.dll",      L"vcruntime140_1d.dll",
    L"msvcp140.dll",       L"msvcp140_1.dll",         L"msvcp140_2.dll",
    L"msvcp140_atomic_wait.dll", L"msvcp140_codecvt_ids.dll", L"msvcp140d.dll",
    L"concrt140.dll",      L"vcomp140.dll"};

std::wstring moduleFilePath(HMODULE h) {
    std::vector<wchar_t> buf(1024);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(h, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) { return std::wstring(); }
        if (n < buf.size()) { return std::wstring(buf.data(), n); }
        if (buf.size() >= 32768) { return std::wstring(); }
        buf.resize(buf.size() * 2);
    }
}

std::uint64_t processUptimeMs() {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) { return 0; }
    ::GetSystemTimeAsFileTime(&now);
    const auto u64 = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    const std::uint64_t a = u64(created);
    const std::uint64_t b = u64(now);
    return b > a ? (b - a) / 10000ull : 0ull;
}

}  // namespace

std::vector<std::wstring> loadedModulePaths() {
    HANDLE proc = ::GetCurrentProcess();
    std::vector<HMODULE> mods(512);
    DWORD needed = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (::EnumProcessModulesEx(proc, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                                   &needed, LIST_MODULES_ALL) == 0) {
            return {};
        }
        if (needed <= mods.size() * sizeof(HMODULE)) { break; }
        mods.resize(needed / sizeof(HMODULE) + 16);  // modules arrived while we asked: ask again
    }
    const std::size_t n = std::min<std::size_t>(needed / sizeof(HMODULE), mods.size());
    std::vector<std::wstring> out;
    out.reserve(n);
    std::vector<wchar_t> buf(4096);
    for (std::size_t i = 0; i < n; ++i) {
        const DWORD len = ::GetModuleFileNameExW(proc, mods[i], buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0 || len >= buf.size()) { continue; }
        out.push_back(expandShortPath(std::wstring(buf.data(), len)));
    }
    return out;
}

void registerOwnModuleFolder(const std::string& folder) {
    if (folder.empty()) { return; }
    std::wstring w;
    try {
        w = expandShortPath(std::filesystem::path(folder).wstring());
    } catch (...) {
        return;
    }
    if (w.empty()) { return; }
    std::lock_guard<std::mutex> lk(ownFolderMutex());
    std::vector<std::wstring>& v = ownFolderStore();
    const CanonPath c = canonicalise(w);
    for (const std::wstring& e : v) {
        if (samePath(canonicalise(e), c)) { return; }
    }
    v.push_back(w);
}

ForeignRoots currentForeignRoots() {
    ForeignRoots r;
    const std::wstring exe = moduleFilePath(nullptr);
    const std::size_t cut = exe.find_last_of(L'\\');
    if (cut != std::wstring::npos) { r.programFolder = expandShortPath(exe.substr(0, cut)); }

    std::wstring win(512, L'\0');
    UINT n = ::GetSystemWindowsDirectoryW(win.data(), static_cast<UINT>(win.size()));
    if (n >= win.size()) {
        win.assign(static_cast<std::size_t>(n) + 1, L'\0');
        n = ::GetSystemWindowsDirectoryW(win.data(), static_cast<UINT>(win.size()));
    }
    if (n > 0 && n < win.size()) {
        win.resize(n);
        r.windowsFolder = win;
    }

    {
        std::lock_guard<std::mutex> lk(ownFolderMutex());
        r.vendorFolders = ownFolderStore();
    }
    // SOAPY_SDR_PLUGIN_PATH is where SoapySDR looks for modules, so every folder it names
    // is a folder of vendor modules the application was asked to load.
    std::wstring env(1024, L'\0');
    DWORD en = ::GetEnvironmentVariableW(L"SOAPY_SDR_PLUGIN_PATH", env.data(), static_cast<DWORD>(env.size()));
    if (en >= env.size()) {
        env.assign(static_cast<std::size_t>(en) + 1, L'\0');
        en = ::GetEnvironmentVariableW(L"SOAPY_SDR_PLUGIN_PATH", env.data(), static_cast<DWORD>(env.size()));
    }
    if (en > 0 && en < env.size()) {
        env.resize(en);
        std::wstring cur;
        for (std::size_t i = 0; i <= env.size(); ++i) {
            if (i == env.size() || env[i] == L';') {
                if (!cur.empty()) { r.vendorFolders.push_back(expandShortPath(cur)); }
                cur.clear();
            } else {
                cur.push_back(env[i]);
            }
        }
    }

    for (const wchar_t* name : kRuntimeNames) {
        HMODULE h = ::GetModuleHandleW(name);
        if (h == nullptr) { continue; }
        const std::wstring p = moduleFilePath(h);
        if (!p.empty()) { r.runtimeFiles.push_back(expandShortPath(p)); }
    }
    return r;
}

std::vector<std::string> snapshotForeignModules() {
    return foreignNamesOf(loadedModulePaths(), currentForeignRoots());
}

#else  // !_WIN32

std::vector<std::wstring> loadedModulePaths() { return {}; }
void registerOwnModuleFolder(const std::string&) {}
ForeignRoots currentForeignRoots() { return {}; }
std::vector<std::string> snapshotForeignModules() { return {}; }

#endif

// ---------------------------------------------------------------------------
// The loader's notification (Windows)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
namespace {

// LdrRegisterDllNotification is in ntdll.dll and in no SDK header, and neither are the
// records its callback is handed. They are declared here from the documented layout
// (the LDR_DLL_NOTIFICATION_DATA union is two records of the same shape, "loaded" and
// "unloaded", and only the first is read). UNICODE_STRING is not taken from winternl.h
// so that this file does not depend on which of the two headers a translation unit saw.
struct NtUnicodeString {
    USHORT Length;  // bytes, not characters; not NUL-terminated
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct LdrLoadedData {
    ULONG Flags;
    const NtUnicodeString* FullDllName;
    const NtUnicodeString* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
constexpr ULONG kLdrReasonLoaded = 1;  // LDR_DLL_NOTIFICATION_REASON_LOADED (2 is UNLOADED)

using LdrNotifyFn = VOID(NTAPI*)(ULONG reason, const LdrLoadedData* data, PVOID context);
using LdrRegisterFn = LONG(NTAPI*)(ULONG flags, LdrNotifyFn fn, PVOID context, PVOID* cookie);
using LdrUnregisterFn = LONG(NTAPI*)(PVOID cookie);

// THE QUEUE THE CALLBACK FILLS. Preallocated, fixed-size slots: the callback runs inside
// the loader lock, so it may not allocate, may not log and may not call anything that
// loads a module. It takes one lock - an SRW lock, which is a few instructions and calls
// into nothing - and the only other holder is the window's thread for the length of a copy,
// during which it calls nothing either, so the two can never wait on each other through the
// loader. A full queue drops the new arrival and counts it.
constexpr std::size_t kSlots = 128;
constexpr std::size_t kPathChars = 300;  // the front of a longer path is what is kept: the roots are prefixes
constexpr std::size_t kBaseChars = 64;

struct ArrivalSlot {
    wchar_t path[kPathChars];
    wchar_t base[kBaseChars];
    std::uint64_t tickMs;
};

struct ArrivalQueue {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<bool> pending{false};  // written only with `lock` held, so a drain never loses a push
    std::size_t head = 0;
    std::size_t count = 0;
    unsigned dropped = 0;
    ArrivalSlot slots[kSlots] = {};

    void push(const wchar_t* path, std::size_t pathLen, const wchar_t* base, std::size_t baseLen,
              std::uint64_t tickMs) {
        ::AcquireSRWLockExclusive(&lock);
        if (count >= kSlots) {
            ++dropped;
        } else {
            ArrivalSlot& s = slots[(head + count) % kSlots];
            const std::size_t pn = std::min(pathLen, kPathChars - 1);
            const std::size_t bn = std::min(baseLen, kBaseChars - 1);
            if (pn > 0) { std::memcpy(s.path, path, pn * sizeof(wchar_t)); }
            s.path[pn] = L'\0';
            if (bn > 0) { std::memcpy(s.base, base, bn * sizeof(wchar_t)); }
            s.base[bn] = L'\0';
            s.tickMs = tickMs;
            ++count;
            pending.store(true, std::memory_order_release);
        }
        ::ReleaseSRWLockExclusive(&lock);
    }
};

// THE CALLBACK. Runs in whatever thread loaded the module, inside the loader lock.
// Copies two strings and a tick count and returns.
VOID NTAPI ldrCallback(ULONG reason, const LdrLoadedData* data, PVOID context) {
    if (reason != kLdrReasonLoaded || data == nullptr || context == nullptr) { return; }
    const NtUnicodeString* full = data->FullDllName;
    const NtUnicodeString* base = data->BaseDllName;
    const wchar_t* fp = (full != nullptr) ? full->Buffer : nullptr;
    const wchar_t* bp = (base != nullptr) ? base->Buffer : nullptr;
    const std::size_t fn = (fp != nullptr) ? full->Length / sizeof(wchar_t) : 0;
    const std::size_t bn = (bp != nullptr) ? base->Length / sizeof(wchar_t) : 0;
    static_cast<ArrivalQueue*>(context)->push(fp, fn, bp, bn, ::GetTickCount64());
}

struct Arrival {
    std::wstring path;
    std::wstring base;
    std::uint64_t tickMs = 0;
};

}  // namespace
#endif  // _WIN32

// ---------------------------------------------------------------------------
// The watch
// ---------------------------------------------------------------------------
struct ForeignModuleWatch::Impl {
    ForeignWatchOptions opts;

    mutable std::mutex m;  // guards everything below that is not atomic
    std::condition_variable cv;
    bool started = false;
    bool scanned = false;
    std::vector<std::string> names;  // sorted, unique, at most kForeignMaxNames
    std::set<std::string> lower;     // the same, folded
    std::size_t unlisted = 0;        // names beyond the cap, or the start line's "+K more"
    unsigned arrivalLines = 0;
    std::string value = "(not scanned yet)";

    std::atomic<bool> stop{false};
    std::atomic<bool> notified{false};

#if defined(_WIN32)
    ArrivalQueue queue;
    std::atomic<void*> cookie{nullptr};
    std::atomic<LdrUnregisterFn> unregisterFn{nullptr};

    void refreshValueLocked() { value = names.empty() && unlisted == 0 ? "(none)" : foreignFieldText(names, unlisted); }

    // Registers the notification. False when ntdll lacks the entry points or refuses.
    bool registerNotification() {
        HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
        if (nt == nullptr) { return false; }
        // Through void*: GetProcAddress returns a generic FARPROC, and a direct cast to a
        // function type of another signature is what GCC's -Wcast-function-type names.
        auto reg = reinterpret_cast<LdrRegisterFn>(
            reinterpret_cast<void*>(::GetProcAddress(nt, "LdrRegisterDllNotification")));
        auto unreg = reinterpret_cast<LdrUnregisterFn>(
            reinterpret_cast<void*>(::GetProcAddress(nt, "LdrUnregisterDllNotification")));
        if (reg == nullptr || unreg == nullptr) { return false; }
        PVOID c = nullptr;
        if (reg(0, &ldrCallback, &queue, &c) < 0 || c == nullptr) { return false; }
        unregisterFn.store(unreg);
        cookie.store(c);
        // The destructor may have run between the two lines above: whoever takes the cookie
        // out unregisters it, so it is unregistered exactly once.
        if (stop.load()) {
            shutdownNotification();
            return false;
        }
        notified.store(true);
        return true;
    }

    void shutdownNotification() {
        void* c = cookie.exchange(nullptr);
        if (c == nullptr) { return; }
        LdrUnregisterFn unreg = unregisterFn.load();
        if (unreg != nullptr) { unreg(c); }  // returns when no callback is running
        notified.store(false);
    }

    // The fallback feeds the same queue the callback does.
    void enqueue(const std::wstring& path) {
        const std::size_t cut = path.find_last_of(L'\\');
        const std::wstring base = (cut == std::wstring::npos) ? path : path.substr(cut + 1);
        queue.push(path.c_str(), path.size(), base.c_str(), base.size(), ::GetTickCount64());
    }

    void run() {
        bool registered = false;
        if (opts.notification) { registered = registerNotification(); }
        if (stop.load()) { return; }

        // REGISTERED FIRST, SCANNED SECOND: a module that loads between the two is in the
        // queue and in the scan, and the drain does not write it twice.
        const std::vector<std::wstring> baseline = loadedModulePaths();
        const std::vector<std::string> start = foreignNamesOf(baseline, currentForeignRoots());

        // The line first, then the state that lets poll() run: an arrival line can never
        // come before the line that says what was there at the start.
        DiagLog::instance().write("info", foreignStartLineText(start).c_str());
        {
            std::lock_guard<std::mutex> lk(m);
            for (const std::string& n : start) {
                if (names.size() >= kForeignMaxNames) {
                    ++unlisted;
                } else {
                    names.push_back(n);
                    lower.insert(lowerNarrow(n));
                }
            }
            refreshValueLocked();
            scanned = true;
        }
        if (registered) { return; }

        const std::string every = opts.rescanMs % 1000 == 0 ? std::to_string(opts.rescanMs / 1000) + " s"
                                                            : std::to_string(opts.rescanMs) + " ms";
        DiagLog::instance().write(
            "info", ("module watch: the loader notification is not available, looking again every " +
                     every)
                        .c_str());
        std::set<std::string> seen;
        const auto key = [](const std::wstring& p) {
            std::string k;
            for (const wchar_t c : p) { k.push_back(c < 0x80 ? lowerAscii(static_cast<char>(c)) : '?'); }
            return k;
        };
        for (const std::wstring& p : baseline) { seen.insert(key(p)); }
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m);
                if (cv.wait_for(lk, std::chrono::milliseconds(opts.rescanMs),
                                [this] { return stop.load(); })) {
                    return;
                }
            }
            for (const std::wstring& p : loadedModulePaths()) {
                if (seen.insert(key(p)).second) { enqueue(p); }
            }
        }
    }

    void drain() {
        if (!queue.pending.load(std::memory_order_acquire)) { return; }
        {
            std::lock_guard<std::mutex> lk(m);
            if (!scanned) { return; }  // the start line comes first
        }
        // NOTHING INSIDE THE LOCK ALLOCATES: the copy buffer is made first (an arrival is rare,
        // so the 94 KB is paid when one has come), and the lock is held for a memcpy of the
        // slots and nothing else. The thread that holds the loader lock and is waiting for
        // this one is a thread that loaded a module; it must never be made to wait for a heap.
        std::vector<ArrivalSlot> raw(kSlots);
        std::size_t n = 0;
        unsigned dropped = 0;
        ::AcquireSRWLockExclusive(&queue.lock);
        n = queue.count;
        for (std::size_t i = 0; i < n; ++i) { raw[i] = queue.slots[(queue.head + i) % kSlots]; }
        queue.head = (queue.head + n) % kSlots;
        queue.count = 0;
        dropped = queue.dropped;
        queue.dropped = 0;
        queue.pending.store(false, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&queue.lock);

        std::vector<Arrival> batch;
        batch.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            batch.push_back({std::wstring(raw[i].path), std::wstring(raw[i].base), raw[i].tickMs});
        }

        if (dropped > 0) {
            DiagLog::instance().write(
                "warn", ("module arrivals missed: " + std::to_string(dropped) +
                         " (more modules loaded at once than the queue holds)")
                            .c_str());
        }
        if (batch.empty()) { return; }

        const ForeignRoots roots = currentForeignRoots();
        const std::uint64_t nowTick = ::GetTickCount64();
        const std::uint64_t uptimeMs = processUptimeMs();
        for (const Arrival& a : batch) {
            if (a.path.empty()) { continue; }
            const std::wstring p = expandShortPath(a.path);
            if (!isForeignModule(p, roots)) { continue; }
            const std::string name = fileNameOnly(a.base.empty() ? p : a.base);
            if (name.empty()) { continue; }
            const std::string key = lowerNarrow(name);

            bool logIt = false;
            bool summary = false;
            {
                std::lock_guard<std::mutex> lk(m);
                if (lower.count(key) != 0) { continue; }  // known: a name is written once a session
                lower.insert(key);
                if (names.size() >= kForeignMaxNames) {
                    ++unlisted;
                } else {
                    names.insert(std::upper_bound(names.begin(), names.end(), name, lessNoCase), name);
                }
                refreshValueLocked();
                if (arrivalLines < kForeignMaxArrivalLines) {
                    logIt = true;
                } else if (arrivalLines == kForeignMaxArrivalLines) {
                    summary = true;
                }
                ++arrivalLines;
            }
            if (logIt) {
                const std::uint64_t age = nowTick >= a.tickMs ? nowTick - a.tickMs : 0;
                const double since = (uptimeMs >= age ? uptimeMs - age : 0) / 1000.0;
                DiagLog::instance().write("info", foreignArrivalLineText(name, since).c_str());
            } else if (summary) {
                DiagLog::instance().write(
                    "info", ("module arrivals: " + std::to_string(kForeignMaxArrivalLines) +
                             " logged, the rest are listed in the report's foreign-modules line only")
                                .c_str());
            }
        }
    }
#endif  // _WIN32
};

void ForeignModuleWatch::worker(std::shared_ptr<Impl> impl) {
#if defined(_WIN32)
    impl->run();
#else
    (void)impl;
#endif
}

ForeignModuleWatch::ForeignModuleWatch(ForeignWatchOptions options) : impl_(std::make_shared<Impl>()) {
    impl_->opts = options;
}

ForeignModuleWatch::~ForeignModuleWatch() {
    if (!impl_) { return; }
    impl_->stop.store(true);
    impl_->cv.notify_all();
#if defined(_WIN32)
    impl_->shutdownNotification();
#endif
}

void ForeignModuleWatch::start() {
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (impl_->started) { return; }
        impl_->started = true;
#if !defined(_WIN32)
        impl_->value = "(not applicable)";
        return;
#endif
    }
#if defined(_WIN32)
    try {
        std::thread(&ForeignModuleWatch::worker, impl_).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->value = "(not recorded)";
    }
#endif
}

void ForeignModuleWatch::poll() {
#if defined(_WIN32)
    impl_->drain();
#endif
}

std::string ForeignModuleWatch::contextValue() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->value;
}

bool ForeignModuleWatch::scanned() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->scanned;
}

std::vector<std::string> ForeignModuleWatch::names() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->names;
}

bool ForeignModuleWatch::usingNotification() const { return impl_->notified.load(); }

}  // namespace cascade::core
