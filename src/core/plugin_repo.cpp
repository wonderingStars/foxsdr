// Implementation of core/plugin_repo.hpp. The header carries the security
// contract - the seven rules - and this file is arranged to match it:
// portable validation first (scheme, sha256 format, filename), then the CNG
// hash, then the WinHTTP transport, then the two public operations that
// compose them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo.hpp"

#include "core/file_read.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
// windows.h first, always: both of the headers below depend on its types.
#include <bcrypt.h>
#include <process.h>
#include <winhttp.h>
// Linked here rather than in CMakeLists.txt so adding this module needs no
// build-system change; MSVC threads the directive through the static library
// into the final link.
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <unistd.h>

#include <cstdlib>

// The TLS client for this platform. cpp-httplib is already vendored for the
// web server, and OpenSSL is already a dependency here for SHA-256 and
// PBKDF2, so the catalogue transport adds no new third-party code — it is the
// same two libraries the build already carries.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <openssl/evp.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core {

namespace {

// ---------------------------------------------------------------------------
// Small portable helpers
// ---------------------------------------------------------------------------

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII-only case folding on purpose: these compare protocol tokens and
// hexadecimal, where a locale-sensitive tolower() would be both slower and,
// in the Turkish locale, wrong about 'I'.
bool iequalsAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool startsWithAscii(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (lowerAscii(s[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = lowerAscii(c);
    }
    return s;
}

// A well-formed SHA-256: exactly 64 hexadecimal digits, no "0x", no spaces,
// no truncation. Anything else is refused rather than normalised - a hash
// this code had to repair is a hash nobody should trust.
bool isWellFormedSha256(const std::string& s) {
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        if (!isHexDigit(c)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Version-string helpers (see PluginRepo::compareVersions for the contract).
// Nothing here converts a segment to an integer: a catalogue is untrusted
// input, and "999999999999999999999999" must compare, not overflow.
// ---------------------------------------------------------------------------

std::vector<std::string> splitVersionSegments(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == '.') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool allDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

std::string stripLeadingZeros(const std::string& s) {
    std::size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') {
        ++i;
    }
    return s.substr(i);
}

// Numeric comparison of two digit strings of any length: longer wins once
// leading zeros are gone, and equal lengths compare byte-wise (which for
// digits IS numeric order).
int compareDigitSegments(const std::string& a, const std::string& b) {
    const std::string x = stripLeadingZeros(a);
    const std::string y = stripLeadingZeros(b);
    if (x.size() != y.size()) {
        return x.size() < y.size() ? -1 : 1;
    }
    if (x == y) {
        return 0;
    }
    return x < y ? -1 : 1;
}

std::int64_t nowUnix() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// This platform's loadable-module extension. ONE definition, used by both the
// manifest's view of "is this a plugin file" and sanitiseFileName, because the
// two must agree: a name one accepts and the other rejects either strands a
// file the manifest is tracking or admits one the scanner will never load.
// It matches PluginHost::hasPluginExtension by construction.
#if defined(_WIN32)
constexpr const char* kModuleExtension = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExtension = ".dylib";
#else
constexpr const char* kModuleExtension = ".so";
#endif

// Case-insensitively only where the platform's own loader is: NTFS treats
// "X.DLL" and "x.dll" as one file, POSIX does not. Matching PluginHost here is
// not cosmetic — a name this accepted but the scanner skipped would be
// downloaded, verified, written, and then never loaded, with nothing to say why.
inline bool hasModuleExtension(const std::string& name) {
    const std::size_t n = std::strlen(kModuleExtension);
    if (name.size() <= n) {
        return false;
    }
    const std::string tail = name.substr(name.size() - n);
#if defined(_WIN32)
    return iequalsAscii(tail, kModuleExtension);
#else
    return tail == kModuleExtension;
#endif
}

bool hasDllExtension(const std::string& name) { return hasModuleExtension(name); }

// ---------------------------------------------------------------------------
// Index parsing helpers. Every getter is STRICT about type: a known field
// carrying the wrong JSON type fails the whole parse rather than falling back
// to a default. This is the opposite of config.cpp's forgiving policy, and
// deliberately so - a hand-edited config that half-loads costs the user a
// preference, whereas an index this code half-understood costs them a DLL.
// ---------------------------------------------------------------------------

bool wantString(const json& j, const char* key, bool required, std::string& dst,
                const std::string& where, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) {
            error = where + ": missing required string field \"" + key + "\"";
            return false;
        }
        return true;
    }
    if (!it->is_string()) {
        error = where + ": field \"" + key + "\" is not a string";
        return false;
    }
    dst = it->get<std::string>();
    if (required && dst.empty()) {
        error = where + ": field \"" + key + "\" is empty";
        return false;
    }
    return true;
}

bool wantUint(const json& j, const char* key, bool required, std::uint64_t& dst,
              const std::string& where, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) {
            error = where + ": missing required integer field \"" + key + "\"";
            return false;
        }
        return true;
    }
    if (!it->is_number_unsigned()) {
        error = where + ": field \"" + key + "\" is not a non-negative integer";
        return false;
    }
    dst = it->get<std::uint64_t>();
    return true;
}

// ---------------------------------------------------------------------------
// The shop-window fields (0.99.72). Optional and cosmetic, so - unlike the
// getters above - a wrong type is a field left at its default plus a line in
// the parse report, never a refused catalogue.
// ---------------------------------------------------------------------------

void noteDrop(std::vector<std::string>* report, const std::string& where, const std::string& what) {
    if (report != nullptr) { report->push_back(where + ": " + what); }
}

// `key` as a string, if it is one. Absent or null is silence (the normal case).
bool softString(const json& j, const char* key, std::string& dst, const std::string& where,
                std::vector<std::string>* report) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) { return false; }
    if (!it->is_string()) {
        noteDrop(report, where, std::string("\"") + key + "\" is not a string; ignored");
        return false;
    }
    dst = it->get<std::string>();
    return true;
}

// YYYY-MM-DD, digits where digits go. Not a calendar check: the field is shown
// as it is, and the only thing worth refusing is a shape that is not a date.
bool looksLikeIsoDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) { continue; }
        if (s[i] < '0' || s[i] > '9') { return false; }
    }
    return true;
}

// A non-negative integer that fits an int (a picture's width or height); anything
// else, absent included, is 0 = "not stated".
int softSmallInt(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) { return 0; }
    const std::uint64_t v = it->get<std::uint64_t>();
    return v <= 100000ull ? static_cast<int>(v) : 0;
}

struct CapabilityName {
    const char* name;
    std::uint32_t bit;
};
// Every capability this host's ABI header defines, by the macro's own name -
// the spelling the catalogue generator publishes. A bit added to plugin_abi.h
// needs its name added here; the unknown-name path is what keeps a catalogue
// that is ahead of this build from being an error.
const CapabilityName kCapabilityNames[] = {
    {"CASCADE_CAP_DECODER", CASCADE_CAP_DECODER},
    {"CASCADE_CAP_IQ_DECODER", CASCADE_CAP_IQ_DECODER},
    {"CASCADE_CAP_IMAGE_DECODER", CASCADE_CAP_IMAGE_DECODER},
    {"CASCADE_CAP_TRACK_SOURCE", CASCADE_CAP_TRACK_SOURCE},
    {"CASCADE_CAP_PANEL", CASCADE_CAP_PANEL},
    {"CASCADE_CAP_HOST_CLIENT", CASCADE_CAP_HOST_CLIENT},
    {"CASCADE_CAP_PRESET", CASCADE_CAP_PRESET},
    {"CASCADE_CAP_BASEMAP", CASCADE_CAP_BASEMAP},
    {"CASCADE_CAP_TRACK_INFO", CASCADE_CAP_TRACK_INFO},
    {"CASCADE_CAP_INSTRUMENT", CASCADE_CAP_INSTRUMENT},
    {"CASCADE_CAP_AUDIO_OUT", CASCADE_CAP_AUDIO_OUT},
    {"CASCADE_CAP_AUDIO_PROCESSOR", CASCADE_CAP_AUDIO_PROCESSOR},
    {"CASCADE_CAP_SETTINGS_UI", CASCADE_CAP_SETTINGS_UI},
    {"CASCADE_CAP_RECEIVER_LOCATOR", CASCADE_CAP_RECEIVER_LOCATOR},
};

// Reads the shop-window fields of one catalogue entry into `e`. Never fails.
void readShopFields(const json& pj, PluginCatalogEntry& e, std::vector<std::string>* report) {
    const std::string where = "plugin \"" + e.id + "\"";

    softString(pj, "category", e.category, where, report);
    softString(pj, "whatsNew", e.whatsNew, where, report);

    std::string published;
    if (softString(pj, "published", published, where, report)) {
        if (looksLikeIsoDate(published)) {
            e.published = published;
        } else {
            noteDrop(report, where, "\"published\" is not a YYYY-MM-DD date; ignored");
        }
    }

    const auto expIt = pj.find("experimental");
    if (expIt != pj.end() && !expIt->is_null()) {
        if (expIt->is_boolean()) {
            e.experimental = expIt->get<bool>();
        } else {
            noteDrop(report, where, "\"experimental\" is not true or false; ignored");
        }
    }

    const auto capIt = pj.find("capabilities");
    if (capIt != pj.end() && !capIt->is_null()) {
        if (!capIt->is_array()) {
            noteDrop(report, where, "\"capabilities\" is not an array; ignored");
        } else {
            std::size_t ci = 0;
            for (const json& cj : *capIt) {
                const std::size_t at = ci++;
                if (!cj.is_string()) {
                    noteDrop(report, where,
                             "capability " + std::to_string(at) + " is not a string; ignored");
                    continue;
                }
                const std::string name = cj.get<std::string>();
                e.capabilityNames.push_back(name);
                e.capabilities |= PluginRepo::capabilityBitForName(name);
            }
        }
    }

    const auto shotsIt = pj.find("screenshots");
    if (shotsIt != pj.end() && !shotsIt->is_null()) {
        if (!shotsIt->is_array()) {
            noteDrop(report, where, "\"screenshots\" is not an array; ignored");
            return;
        }
        std::size_t si = 0;
        for (const json& sj : *shotsIt) {
            const std::string swhere = where + " screenshot " + std::to_string(si);
            ++si;
            if (e.screenshots.size() >= PluginRepo::kMaxScreenshotsPerPlugin) {
                noteDrop(report, swhere,
                         "dropped: a plugin may list at most " +
                             std::to_string(PluginRepo::kMaxScreenshotsPerPlugin) + " pictures");
                continue;
            }
            if (!sj.is_object()) {
                noteDrop(report, swhere, "dropped: not a JSON object");
                continue;
            }
            CatalogScreenshot s;
            std::string scratch;  // the strict getters' message is not used: the plugin survives
            if (!wantString(sj, "url", true, s.url, swhere, scratch)) {
                noteDrop(report, swhere, "dropped: no url");
                continue;
            }
            if (!wantString(sj, "sha256", true, s.sha256, swhere, scratch)) {
                noteDrop(report, swhere, "dropped: no sha256");
                continue;
            }
            if (!wantString(sj, "caption", true, s.caption, swhere, scratch)) {
                noteDrop(report, swhere, "dropped: no caption");
                continue;
            }
            // RULE 1 and RULE 2 again, for a picture: a plain-text URL never
            // reaches the transport, and there is no "unverified" picture.
            if (!PluginRepo::isHttpsUrl(s.url)) {
                noteDrop(report, swhere, "dropped: url is not https");
                continue;
            }
            if (!isWellFormedSha256(s.sha256)) {
                noteDrop(report, swhere, "dropped: sha256 is not 64 hexadecimal digits");
                continue;
            }
            s.sha256 = toLowerAscii(s.sha256);
            const auto sizeIt = sj.find("sizeBytes");
            if (sizeIt != sj.end() && sizeIt->is_number_unsigned()) {
                s.sizeBytes = sizeIt->get<std::uint64_t>();
            }
            s.width = softSmallInt(sj, "width");
            s.height = softSmallInt(sj, "height");
            e.screenshots.push_back(std::move(s));
        }
    }
}

// Per-phase timeouts for one httpsGet() call, shared by both platform
// transports below. The defaults reproduce exactly what this file used
// before these fields existed (10 s to resolve+connect, 20 s to send, 30 s
// to receive), so every existing caller (fetchText, fetchVerifiedFile,
// fetchIndex, install()'s download) is unaffected by default.
//
// fetchRegionalIndex() passes a much shorter set (see its call site) - the
// geo judge found that a black-holed foxsdr.com could delay the WHOLE plugin
// store list by the full public-catalogue-sized timeout (tens of seconds),
// even though the public catalogue had already been fetched successfully,
// because the regional fetch runs synchronously after it on the one
// catalogue worker (AppWindow::startCatalogFetch). The public list is never
// held back by more than this short a stall.
struct HttpTimeouts {
    int connectMs = 10000;
    int sendMs = 20000;
    int receiveMs = 30000;
};

// ---------------------------------------------------------------------------
// Windows-only: SHA-256 via CNG, and the HTTPS transport.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

std::string ntStatusText(const char* what, NTSTATUS st) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(st));
    return std::string(what) + " failed (NTSTATUS " + buf + ")";
}

// Streaming SHA-256. Streaming rather than "hash the finished file" because
// the download is hashed AS IT ARRIVES: the bytes are then never read back
// from a file that something else could have swapped underneath us between
// the write and the check.
class Sha256 {
public:
    Sha256() = default;
    ~Sha256() {
        if (hash_ != nullptr) {
            ::BCryptDestroyHash(hash_);
        }
        if (alg_ != nullptr) {
            ::BCryptCloseAlgorithmProvider(alg_, 0);
        }
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool init(std::string& error) {
        NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(st)) {
            error = ntStatusText("BCryptOpenAlgorithmProvider(SHA256)", st);
            return false;
        }
        DWORD lenBytes = 0;
        DWORD got = 0;
        st = ::BCryptGetProperty(alg_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&lenBytes),
                                 sizeof(lenBytes), &got, 0);
        if (!BCRYPT_SUCCESS(st) || lenBytes == 0) {
            error = ntStatusText("BCryptGetProperty(HASH_LENGTH)", st);
            return false;
        }
        digestLen_ = lenBytes;
        st = ::BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(st)) {
            error = ntStatusText("BCryptCreateHash", st);
            return false;
        }
        return true;
    }

    bool update(const void* data, std::size_t n, std::string& error) {
        // BCryptHashData takes a ULONG count; a >4 GiB single call cannot
        // happen here (the caps are far below that) but the loop keeps the
        // function honest if a caller ever hands it a huge buffer.
        const auto* p = static_cast<const unsigned char*>(data);
        while (n > 0) {
            const ULONG chunk =
                static_cast<ULONG>(n > 0x40000000u ? 0x40000000u : n);
            const NTSTATUS st =
                ::BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0);
            if (!BCRYPT_SUCCESS(st)) {
                error = ntStatusText("BCryptHashData", st);
                return false;
            }
            p += chunk;
            n -= chunk;
        }
        return true;
    }

    bool finishHex(std::string& hexOut, std::string& error) {
        std::vector<unsigned char> digest(digestLen_);
        const NTSTATUS st =
            ::BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (!BCRYPT_SUCCESS(st)) {
            error = ntStatusText("BCryptFinishHash", st);
            return false;
        }
        static const char* kHex = "0123456789abcdef";
        hexOut.clear();
        hexOut.reserve(digest.size() * 2);
        for (unsigned char b : digest) {
            hexOut.push_back(kHex[b >> 4]);
            hexOut.push_back(kHex[b & 0x0F]);
        }
        return true;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    DWORD digestLen_ = 32;
};

// --- WinHTTP -------------------------------------------------------------

struct HInternet {
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET x) : h(x) {}
    ~HInternet() { reset(); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    void reset(HINTERNET x = nullptr) {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
        h = x;
    }
    explicit operator bool() const { return h != nullptr; }
};

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* s, std::size_t len) {
    if (len == 0) {
        return std::string();
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

// WinHTTP's own error codes (12000-12999) do not resolve through
// FORMAT_MESSAGE_FROM_SYSTEM, so a plain system_category().message() turns
// "the certificate is invalid" into "unknown error" - exactly the message a
// support ticket cannot be answered from. Ask winhttp.dll for its own strings.
std::string osErrorText(DWORD e) {
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                  FORMAT_MESSAGE_ALLOCATE_BUFFER;
    HMODULE mod = nullptr;
    if (e >= 12000u && e <= 12999u) {
        mod = ::GetModuleHandleW(L"winhttp.dll");
        if (mod != nullptr) {
            flags |= FORMAT_MESSAGE_FROM_HMODULE;
        }
    }
    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(flags, mod, e, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg;
    if (n != 0 && buf != nullptr) {
        msg = narrow(buf, n);
    }
    if (buf != nullptr) {
        ::LocalFree(buf);
    }
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' ||
                            msg.back() == '.')) {
        msg.pop_back();
    }
    if (msg.empty()) {
        msg = "unknown error";
    }
    return msg + " (error " + std::to_string(e) + ")";
}

std::string winHttpError(const std::string& what) {
    return what + ": " + osErrorText(::GetLastError());
}

struct UrlParts {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring target;  // path + query, never empty
};

// Splits an https URL. Refuses anything that is not https here as well as in
// the portable pre-check, so the transport cannot be reached with a plain-text
// URL even if a future caller forgets the earlier gate.
bool crackHttpsUrl(const std::string& url, UrlParts& out, std::string& error) {
    const std::wstring w = widen(url);
    if (w.empty()) {
        error = "empty URL";
        return false;
    }
    wchar_t hostBuf[256];
    wchar_t pathBuf[2048];
    wchar_t extraBuf[2048];
    URL_COMPONENTS uc;
    std::memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    uc.lpszExtraInfo = extraBuf;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extraBuf));
    if (!::WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) {
        error = winHttpError("cannot parse URL \"" + url + "\"");
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "refusing a non-https URL: \"" + url + "\"";
        return false;
    }
    if (uc.dwHostNameLength == 0) {
        error = "URL has no host: \"" + url + "\"";
        return false;
    }
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.port = uc.nPort;
    out.target.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    out.target.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out.target.empty()) {
        out.target = L"/";
    }
    return true;
}

bool queryStatusCode(HINTERNET req, DWORD& status, std::string& error) {
    DWORD len = sizeof(status);
    if (!::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                               WINHTTP_NO_HEADER_INDEX)) {
        error = winHttpError("cannot read the HTTP status code");
        return false;
    }
    return true;
}

// Content-Length if the server sent one, else 0. Advisory: used for the
// progress bar and for an early "this is bigger than the cap" refusal, never
// as the stopping condition for the read loop.
std::uint64_t queryContentLength(HINTERNET req) {
    wchar_t buf[64];
    DWORD len = static_cast<DWORD>(sizeof(buf));
    if (!::WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                               buf, &len, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    std::uint64_t v = 0;
    for (const wchar_t* p = buf; *p != L'\0'; ++p) {
        if (*p < L'0' || *p > L'9') {
            return 0;
        }
        if (v > (0xFFFFFFFFFFFFFFFFull - static_cast<std::uint64_t>(*p - L'0')) / 10ull) {
            return 0;  // absurd value: treat as "not stated"
        }
        v = v * 10ull + static_cast<std::uint64_t>(*p - L'0');
    }
    return v;
}

bool queryLocation(HINTERNET req, std::string& location, std::string& error) {
    DWORD len = 0;
    ::WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                          &len, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        error = "the server sent a redirect with no Location header";
        return false;
    }
    std::wstring w(len / sizeof(wchar_t) + 1, L'\0');
    if (!::WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                               w.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
        error = winHttpError("cannot read the Location header");
        return false;
    }
    location = narrow(w.c_str(), std::wcslen(w.c_str()));
    return true;
}

// RULE 3. Resolves a redirect target against the URL it came from and refuses
// anything that leaves the host. A relative "/other/path" stays put by
// definition and is allowed; an absolute https URL must match host AND port
// exactly; an http:// target is a downgrade and is refused; anything else
// (protocol-relative, a scheme we do not know) is refused because there is no
// reading of it that is obviously safe.
bool resolveSameHostRedirect(const UrlParts& from, const std::string& location, UrlParts& to,
                             std::string& error) {
    if (location.empty()) {
        error = "the server sent an empty redirect target";
        return false;
    }
    if (location[0] == '/' && !(location.size() > 1 && location[1] == '/')) {
        to.host = from.host;
        to.port = from.port;
        to.target = widen(location);
        return true;
    }
    if (startsWithAscii(location, "http://")) {
        error = "refusing a redirect from https to http: \"" + location + "\"";
        return false;
    }
    if (!startsWithAscii(location, "https://")) {
        error = "refusing a redirect to an unsupported target: \"" + location + "\"";
        return false;
    }
    UrlParts next;
    if (!crackHttpsUrl(location, next, error)) {
        return false;
    }
    const std::string a = toLowerAscii(narrow(from.host.c_str(), from.host.size()));
    const std::string b = toLowerAscii(narrow(next.host.c_str(), next.host.size()));
    if (a != b || from.port != next.port) {
        error = "refusing a cross-host redirect: \"" + a + "\" -> \"" + b + "\"";
        return false;
    }
    to = next;
    return true;
}

// One HTTPS GET, streamed to `sink`.
//
// Certificate validation is left at WinHTTP's defaults - no
// WINHTTP_OPTION_SECURITY_FLAGS is ever set here, which is the only way to
// keep name and chain checking on - and revocation checking is EXPLICITLY
// enabled on top. Automatic redirects are switched off so rule 3 can be
// enforced by hand.
bool httpsGetNetwork(const std::string& url, std::uint64_t maxBytes,
                     const std::function<bool(const void*, std::size_t)>& sink,
                     std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error,
                     const HttpTimeouts& timeouts = HttpTimeouts{}) {
    UrlParts parts;
    if (!crackHttpsUrl(url, parts, error)) {
        return false;
    }

    HInternet session(::WinHttpOpen(L"cascade-plugin-repo/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = winHttpError("cannot initialise WinHTTP");
        return false;
    }
    // Modern TLS only. Best effort: an older SDK/OS that does not know a flag
    // simply keeps its own default, which is still validated.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    ::WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                       sizeof(protocols));
    // Bounded waits: a half-open connection must fail the install, not hang
    // the GUI thread that called it. Resolve shares the connect budget -
    // there is no separate caller-visible "resolve" phase worth naming.
    ::WinHttpSetTimeouts(session.h, timeouts.connectMs, timeouts.connectMs, timeouts.sendMs,
                        timeouts.receiveMs);

    for (int hop = 0;; ++hop) {
        if (hop > PluginRepo::kMaxRedirects) {
            error = "too many redirects";
            return false;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }

        HInternet conn(::WinHttpConnect(session.h, parts.host.c_str(), parts.port, 0));
        if (!conn) {
            error = winHttpError("cannot connect");
            return false;
        }
        HInternet req(::WinHttpOpenRequest(conn.h, L"GET", parts.target.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE));
        if (!req) {
            error = winHttpError("cannot create the request");
            return false;
        }

        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        ::WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        DWORD enable = WINHTTP_ENABLE_SSL_REVOCATION;
        ::WinHttpSetOption(req.h, WINHTTP_OPTION_ENABLE_FEATURE, &enable, sizeof(enable));

        if (!::WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                  0, 0, 0)) {
            error = winHttpError("the request could not be sent");
            return false;
        }
        if (!::WinHttpReceiveResponse(req.h, nullptr)) {
            error = winHttpError("no usable response");
            return false;
        }

        DWORD status = 0;
        if (!queryStatusCode(req.h, status, error)) {
            return false;
        }
        if (status >= 300 && status < 400) {
            std::string location;
            if (!queryLocation(req.h, location, error)) {
                return false;
            }
            UrlParts next;
            if (!resolveSameHostRedirect(parts, location, next, error)) {
                return false;
            }
            parts = next;
            continue;
        }
        if (status != 200) {
            error = "the server returned HTTP " + std::to_string(status);
            return false;
        }

        const std::uint64_t declared = queryContentLength(req.h);
        if (declared > maxBytes) {
            error = "refusing a " + std::to_string(declared) + "-byte download; the limit is " +
                    std::to_string(maxBytes) + " bytes";
            return false;
        }

        std::vector<unsigned char> buf(64 * 1024);
        std::uint64_t total = 0;
        for (;;) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                error = "cancelled";
                return false;
            }
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(req.h, &avail)) {
                error = winHttpError("the transfer failed");
                return false;
            }
            if (avail == 0) {
                break;
            }
            const DWORD want =
                avail < static_cast<DWORD>(buf.size()) ? avail : static_cast<DWORD>(buf.size());
            DWORD got = 0;
            if (!::WinHttpReadData(req.h, buf.data(), want, &got)) {
                error = winHttpError("the transfer failed");
                return false;
            }
            if (got == 0) {
                break;
            }
            // RULE 4: the cap is checked against what actually arrived, so a
            // server that lied in Content-Length (or sent none) is still
            // bounded.
            total += got;
            if (total > maxBytes) {
                error = "the download exceeded the " + std::to_string(maxBytes) +
                        "-byte limit and was aborted";
                return false;
            }
            if (!sink(buf.data(), static_cast<std::size_t>(got))) {
                error = "cannot write the downloaded data";
                return false;
            }
            if (progress != nullptr && declared > 0) {
                float f = static_cast<float>(static_cast<double>(total) /
                                             static_cast<double>(declared));
                if (f > 1.0f) {
                    f = 1.0f;
                }
                progress->store(f, std::memory_order_relaxed);
            }
        }
        return true;
    }
}

#else  // !_WIN32

// ---------------------------------------------------------------------------
// POSIX transport and digest.
//
// Same contract as the WinHTTP path above, and the same refusals: https only,
// a byte cap enforced before and during the read, cancellation honoured
// mid-transfer, and — the one that matters most — redirects followed ONLY
// within the same host and port. The catalogue's binaries are served from a
// host that redirects to a different one for release assets, and quietly
// following that is how a download ends up coming from somewhere the
// catalogue never named.
// ---------------------------------------------------------------------------

// Streaming SHA-256 over OpenSSL, mirroring the CNG class above so the
// download is hashed AS IT ARRIVES rather than read back from a file that
// something else could have swapped in between.
class Sha256 {
public:
    Sha256() = default;
    ~Sha256() {
        if (ctx_ != nullptr) {
            ::EVP_MD_CTX_free(ctx_);
        }
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool init(std::string& error) {
        ctx_ = ::EVP_MD_CTX_new();
        if (ctx_ == nullptr || ::EVP_DigestInit_ex(ctx_, ::EVP_sha256(), nullptr) != 1) {
            error = "cannot initialise SHA-256";
            return false;
        }
        return true;
    }
    bool update(const void* data, std::size_t n, std::string& error) {
        if (n == 0) {
            return true;
        }
        if (::EVP_DigestUpdate(ctx_, data, n) != 1) {
            error = "SHA-256 update failed";
            return false;
        }
        return true;
    }
    bool finishHex(std::string& hexOut, std::string& error) {
        unsigned char digest[32] = {0};
        unsigned int len = 0;
        if (::EVP_DigestFinal_ex(ctx_, digest, &len) != 1 || len != sizeof(digest)) {
            error = "SHA-256 finalisation failed";
            return false;
        }
        static const char* kHex = "0123456789abcdef";
        hexOut.clear();
        hexOut.reserve(sizeof(digest) * 2);
        for (unsigned char b : digest) {
            hexOut += kHex[(b >> 4) & 0x0F];
            hexOut += kHex[b & 0x0F];
        }
        return true;
    }

private:
    EVP_MD_CTX* ctx_ = nullptr;
};

struct UrlParts {
    std::string host;
    int port = 443;
    std::string target;  // path + query, never empty
};

bool crackHttpsUrl(const std::string& url, UrlParts& out, std::string& error) {
    out = UrlParts{};
    if (!startsWithAscii(url, "https://")) {
        error = "refusing a non-https URL: \"" + url + "\"";
        return false;
    }
    const std::string rest = url.substr(8);
    if (rest.empty()) {
        error = "empty URL";
        return false;
    }
    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.target = slash == std::string::npos ? "/" : rest.substr(slash);
    // Credentials in the authority are refused rather than ignored: a URL of
    // the form https://evil.example@real.example/ reads as the wrong host to
    // a human and would defeat the same-host redirect check below.
    if (authority.find('@') != std::string::npos) {
        error = "refusing a URL with embedded credentials";
        return false;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        const std::string portText = authority.substr(colon + 1);
        authority = authority.substr(0, colon);
        if (portText.empty() ||
            portText.find_first_not_of("0123456789") != std::string::npos) {
            error = "malformed port in URL: \"" + url + "\"";
            return false;
        }
        const long p = std::strtol(portText.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) {
            error = "port out of range in URL: \"" + url + "\"";
            return false;
        }
        out.port = static_cast<int>(p);
    }
    if (authority.empty()) {
        error = "URL has no host: \"" + url + "\"";
        return false;
    }
    out.host = authority;
    return true;
}

bool resolveSameHostRedirect(const UrlParts& from, const std::string& location, UrlParts& to,
                             std::string& error) {
    if (location.empty()) {
        error = "the server sent an empty redirect target";
        return false;
    }
    if (location[0] == '/' && !(location.size() > 1 && location[1] == '/')) {
        to.host = from.host;
        to.port = from.port;
        to.target = location;
        return true;
    }
    if (startsWithAscii(location, "http://")) {
        error = "refusing a redirect from https to http: \"" + location + "\"";
        return false;
    }
    if (!startsWithAscii(location, "https://")) {
        error = "refusing a redirect to an unsupported target: \"" + location + "\"";
        return false;
    }
    UrlParts next;
    if (!crackHttpsUrl(location, next, error)) {
        return false;
    }
    if (toLowerAscii(from.host) != toLowerAscii(next.host) || from.port != next.port) {
        error = "refusing a cross-host redirect: \"" + toLowerAscii(from.host) + "\" -> \"" +
                toLowerAscii(next.host) + "\"";
        return false;
    }
    to = next;
    return true;
}

bool httpsGetNetwork(const std::string& url, std::uint64_t maxBytes,
                     const std::function<bool(const void*, std::size_t)>& sink,
                     std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error,
                     const HttpTimeouts& timeouts = HttpTimeouts{}) {
    UrlParts parts;
    if (!crackHttpsUrl(url, parts, error)) {
        return false;
    }

    for (int hop = 0;; ++hop) {
        if (hop > PluginRepo::kMaxRedirects) {
            error = "too many redirects";
            return false;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }

        httplib::SSLClient cli(parts.host, parts.port);
        // Certificate verification is the default and is NOT disabled here.
        cli.enable_server_certificate_verification(true);
        // Redirects are handled by this loop, not by the client, because the
        // client would follow them anywhere.
        cli.set_follow_location(false);
        cli.set_connection_timeout(timeouts.connectMs / 1000, (timeouts.connectMs % 1000) * 1000);
        cli.set_read_timeout(timeouts.receiveMs / 1000, (timeouts.receiveMs % 1000) * 1000);
        cli.set_write_timeout(timeouts.sendMs / 1000, (timeouts.sendMs % 1000) * 1000);

        std::uint64_t received = 0;
        bool overLimit = false;
        bool sinkFailed = false;

        auto body = [&](const char* data, std::size_t n) {
            received += n;
            if (received > maxBytes) {
                overLimit = true;
                return false;
            }
            if (!sink(data, n)) {
                sinkFailed = true;
                return false;
            }
            return true;
        };
        auto onProgress = [&](std::uint64_t current, std::uint64_t total) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                return false;
            }
            if (progress != nullptr && total > 0) {
                progress->store(static_cast<float>(static_cast<double>(current) /
                                                   static_cast<double>(total)),
                                std::memory_order_relaxed);
            }
            return true;
        };

        // Headers are inspected before the body is accepted, so a redirect or
        // an oversized declared length costs no transfer.
        httplib::Result res = cli.Get(
            parts.target, httplib::Headers{{"User-Agent", "cascade-plugin-repo/1.0"}},
            [&](const httplib::Response& r) {
                if (r.status >= 300 && r.status < 400) {
                    return true;  // no body wanted; handled after the call
                }
                if (r.status != 200) {
                    return true;
                }
                const std::string len = r.get_header_value("Content-Length");
                if (!len.empty()) {
                    const std::uint64_t declared = std::strtoull(len.c_str(), nullptr, 10);
                    if (declared > maxBytes) {
                        overLimit = true;
                        return false;
                    }
                }
                return true;
            },
            body, onProgress);

        if (overLimit) {
            error = "refusing a download larger than " + std::to_string(maxBytes) + " bytes";
            return false;
        }
        if (sinkFailed) {
            error = "could not write the downloaded data";
            return false;
        }
        if (!res) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                error = "cancelled";
            } else {
                error = "request failed: " + httplib::to_string(res.error());
            }
            return false;
        }
        if (res->status >= 300 && res->status < 400) {
            UrlParts next;
            if (!resolveSameHostRedirect(parts, res->get_header_value("Location"), next,
                                         error)) {
                return false;
            }
            parts = next;
            continue;
        }
        if (res->status != 200) {
            error = "the server returned HTTP " + std::to_string(res->status);
            return false;
        }
        return true;
    }
}

#endif  // _WIN32

// THE ONE FUNCTION EVERY TRANSFER GOES THROUGH, and the seam a test can stand in
// for (PluginRepo::setTransportForTest, 0.99.64). The two httpsGetNetwork bodies
// above are the real thing, one per platform; this is a single indirection in
// front of them. A shipped build never sets the hook, so it costs one empty
// std::function test per transfer.
std::mutex& testTransportMutex() {
    static std::mutex m;
    return m;
}
PluginRepo::TestTransport& testTransportSlot() {
    static PluginRepo::TestTransport t;
    return t;
}

bool httpsGet(const std::string& url, std::uint64_t maxBytes,
              const std::function<bool(const void*, std::size_t)>& sink,
              std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error,
              const HttpTimeouts& timeouts = HttpTimeouts{}) {
    PluginRepo::TestTransport hook;
    {
        std::lock_guard<std::mutex> lk(testTransportMutex());
        hook = testTransportSlot();
    }
    if (hook) {
        // Cancelled is cancelled whoever serves the bytes: the same check the
        // real transports make before they connect.
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        return hook(url, maxBytes, sink, error);
    }
    return httpsGetNetwork(url, maxBytes, sink, progress, cancel, error, timeouts);
}

}  // namespace

void PluginRepo::setTransportForTest(TestTransport transport) {
    std::lock_guard<std::mutex> lk(testTransportMutex());
    testTransportSlot() = std::move(transport);
}

// ---------------------------------------------------------------------------
// PluginCatalogEntry
// ---------------------------------------------------------------------------

const PluginPlatform* PluginCatalogEntry::thisPlatform() const {
    const std::string os = PluginRepo::hostOs();
    const std::string arch = PluginRepo::hostArch();
    for (const PluginPlatform& p : platforms) {
        // Case-insensitive: "Windows"/"x64" and "windows"/"X64" describe the
        // same build, and a catalogue author should not lose a download to
        // capitalisation.
        if (iequalsAscii(p.os, os) && iequalsAscii(p.arch, arch)) {
            return &p;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Static, portable surface
// ---------------------------------------------------------------------------

std::string PluginRepo::defaultIndexUrl() {
    return "https://raw.githubusercontent.com/wonderingStars/foxsdr-plugins/master/"
           "index.json";
}

// ---------------------------------------------------------------------------
// Regional catalogue (see the REGIONAL CATALOGUE block in the header)
// ---------------------------------------------------------------------------

std::string PluginRepo::regionalIndexUrl() {
    // Same seam shape as every other FOXSDR_*_URL override in this codebase
    // (feature_request.cpp, telemetry.cpp, ...): GetEnvironmentVariableA on
    // Windows, getenv on POSIX, an empty value treated the same as unset.
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_REGIONAL_CATALOGUE_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* v = std::getenv("FOXSDR_REGIONAL_CATALOGUE_URL");
    if (v != nullptr && v[0] != '\0') { return std::string(v); }
#endif
    return "https://foxsdr.com/api/plugins/regional";
}

bool PluginRepo::regionalOverrideSet() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_REGIONAL_CATALOGUE_URL", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf);
#else
    const char* v = std::getenv("FOXSDR_REGIONAL_CATALOGUE_URL");
    return v != nullptr && v[0] != '\0';
#endif
}

std::string PluginRepo::regionalDownloadPrefix() {
    return "https://foxsdr.com/plugins/regional/";
}

bool PluginRepo::isRegionalDownloadUrl(const std::string& url, const std::string& prefix) {
    // Exact, byte-wise, case-sensitive prefix match - deliberately not run
    // through any URL parser, so this function is provable by reading it.
    // That is what refuses "https://foxsdr.com.evil.example/...", "https://
    // evilfoxsdr.com/...", an explicit port, userinfo, and a different case
    // of the same host: none of them share `prefix`'s exact bytes.
    if (prefix.empty() || url.size() <= prefix.size()) {
        return false;
    }
    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    // The remainder must be exactly "<region>/<file>": one slash, nothing
    // before the region, nothing after the file.
    const std::string rest = url.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == 0 || slash == std::string::npos) {
        return false;
    }
    const std::string region = rest.substr(0, slash);
    const std::string file = rest.substr(slash + 1);

    if (region.size() != 2 || region[0] < 'a' || region[0] > 'z' || region[1] < 'a' ||
        region[1] > 'z') {
        return false;
    }

    if (file.empty() || file.size() > kMaxFileNameChars) {
        return false;
    }
    const char first = file.front();
    const bool firstOk = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                        (first >= '0' && first <= '9');
    if (!firstOk) {
        return false;
    }
    // This character class alone is what refuses '?', '#', '%', '@', '\\'
    // and a second '/' (a subdirectory, or a traversal's separator) inside
    // `file` - the same shape of guard as sanitiseFileName(), but
    // deliberately NOT sanitiseFileName() itself: that function also
    // requires THIS host's module extension, and a regional entry publishes
    // both a .dll and a .so platform whichever host is asking (R1).
    for (char c : file) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    if (file.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool PluginRepo::regionalWanted(const std::string& catalogueUrl, bool overrideSet) {
    return overrideSet || catalogueUrl == defaultIndexUrl();
}

PluginRepo::RegionalMergeResult PluginRepo::mergeRegional(
    const std::vector<PluginCatalogEntry>& publicEntries,
    const std::vector<PluginCatalogEntry>& regionalEntries, const std::string& downloadPrefix) {
    RegionalMergeResult result;
    // Every public entry, unchanged and in order - never touched by this
    // function - THEN whatever regional entries survive, appended after.
    result.merged = publicEntries;

    for (const PluginCatalogEntry& e : regionalEntries) {
        bool inPublic = false;
        for (const PluginCatalogEntry& p : publicEntries) {
            if (p.id == e.id) {
                inPublic = true;
                break;
            }
        }
        if (inPublic) {
            result.dropped.push_back({e.id, "id also published in the public catalogue"});
            continue;
        }
        int idCount = 0;
        for (const PluginCatalogEntry& other : regionalEntries) {
            if (other.id == e.id) { ++idCount; }
        }
        if (idCount > 1) {
            result.dropped.push_back(
                {e.id, "id appears more than once in the regional list"});
            continue;
        }
        bool badUrl = false;
        for (const PluginPlatform& p : e.platforms) {
            if (!isRegionalDownloadUrl(p.url, downloadPrefix)) {
                badUrl = true;
                break;
            }
        }
        if (badUrl) {
            result.dropped.push_back(
                {e.id, "a platform download URL is not under the regional prefix"});
            continue;
        }
        PluginCatalogEntry copy = e;
        copy.regional = true;
        // NO PICTURES FROM THE REGIONAL LIST (0.99.72). A picture is fetched from
        // the address its entry names, and PRIVACY.md promises the store's only other
        // address is the public catalogue's own origin; a regional entry's pictures
        // would be an address the regional server picks. The binary's own URL is held
        // to the regional prefix above, and a picture has no such rule - so it has none.
        copy.screenshots.clear();
        result.merged.push_back(std::move(copy));
        ++result.added;
    }
    return result;
}

const char* PluginRepo::hostOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* PluginRepo::hostArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x64";
#else
    return "x86";
#endif
}

bool PluginRepo::isHttpsUrl(const std::string& url) {
    return startsWithAscii(url, "https://") && url.size() > 8;
}

bool PluginRepo::sanitiseFileName(const std::string& raw, std::string& out, std::string& error) {
    out.clear();
    error.clear();

    if (raw.empty()) {
        error = "the catalogue entry has an empty file name";
        return false;
    }
    if (raw.size() > kMaxFileNameChars) {
        error = "file name is longer than " + std::to_string(kMaxFileNameChars) + " characters";
        return false;
    }
    // The character class does most of the work: it excludes '/' and '\\'
    // (traversal), ':' (drive letters, NTFS alternate data streams), '%' and
    // '$' (environment and share syntax), quotes, wildcards, whitespace,
    // control characters and every byte >= 0x80.
    for (char c : raw) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            error = "file name contains a character outside [A-Za-z0-9._-]: \"" + raw + "\"";
            return false;
        }
    }
    // Belt and braces after the class check: no ".." can survive it anyway
    // (a traversal needs a separator), but a name whose stem is dots is still
    // nothing a legitimate catalogue would publish.
    if (raw.find("..") != std::string::npos) {
        error = "file name contains \"..\": \"" + raw + "\"";
        return false;
    }
    const char first = raw.front();
    const bool firstOk = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                         (first >= '0' && first <= '9');
    if (!firstOk) {
        // Rejects leading dots (hidden/dot-only names) and a leading '-',
        // which some tools would read as a switch.
        error = "file name must start with a letter or a digit: \"" + raw + "\"";
        return false;
    }
    // Must END in this platform's module extension: "evil.dll.exe" fails here,
    // which is the whole point. The extension is the host's own, not a list of
    // every platform's — a Windows installation has no business writing a .so
    // into its plugins directory, and PluginHost would not load it anyway.
    const std::size_t extLen = std::strlen(kModuleExtension);
    if (!hasModuleExtension(raw)) {
        error = std::string("file name does not end in \"") + kModuleExtension + "\": \"" +
                raw + "\"";
        return false;
    }
    // Windows reserved device names are reserved with ANY extension, so
    // "CON.dll" is not a file - creating it either fails oddly or talks to a
    // device. Refuse with a clear reason instead.
    const std::string stem = toLowerAscii(raw.substr(0, raw.size() - extLen));
    static const char* kReserved[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3",
                                      "com4", "com5", "com6", "com7", "com8", "com9", "lpt1",
                                      "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8",
                                      "lpt9"};
    for (const char* r : kReserved) {
        if (stem == r) {
            error = "file name is a reserved device name: \"" + raw + "\"";
            return false;
        }
    }

    out = raw;
    return true;
}

bool PluginRepo::parseIndex(const std::string& text, std::vector<PluginCatalogEntry>& out,
                            std::string& error, std::vector<std::string>* report) {
    out.clear();
    error.clear();

    // allow_exceptions=false: a hostile or truncated document is an expected
    // condition on this path, not an exceptional one.
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "plugin index: not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "plugin index: the root is not a JSON object";
        return false;
    }

    // Schema gate first. A different schema number may have renumbered or
    // reinterpreted fields, and a field this code misreads is a field that
    // can point a download somewhere unintended - so nothing else is read.
    {
        const auto it = j.find("schemaVersion");
        if (it == j.end()) {
            error = "plugin index: no schemaVersion";
            return false;
        }
        if (!it->is_number_integer() || it->get<int>() != 1) {
            error = "plugin index: unsupported schemaVersion (expected 1)";
            return false;
        }
    }

    const auto pluginsIt = j.find("plugins");
    if (pluginsIt == j.end()) {
        error = "plugin index: no \"plugins\" array";
        return false;
    }
    if (!pluginsIt->is_array()) {
        error = "plugin index: \"plugins\" is not an array";
        return false;
    }

    std::vector<PluginCatalogEntry> parsed;
    parsed.reserve(pluginsIt->size());

    std::size_t idx = 0;
    for (const json& pj : *pluginsIt) {
        const std::string where = "plugin index: entry " + std::to_string(idx);
        ++idx;
        if (!pj.is_object()) {
            error = where + " is not a JSON object";
            return false;
        }

        PluginCatalogEntry e;
        if (!wantString(pj, "id", true, e.id, where, error) ||
            !wantString(pj, "name", true, e.name, where, error) ||
            !wantString(pj, "version", true, e.version, where, error) ||
            !wantString(pj, "author", false, e.author, where, error) ||
            !wantString(pj, "licence", false, e.licence, where, error) ||
            !wantString(pj, "summary", false, e.summary, where, error) ||
            !wantString(pj, "description", false, e.description, where, error) ||
            !wantString(pj, "homepage", false, e.homepage, where, error) ||
            !wantString(pj, "legalNotice", false, e.legalNotice, where, error) ||
            // The retirement floor. Optional, and absent is the normal case:
            // a catalogue that says nothing retires nothing.
            !wantString(pj, "minSupportedVersion", false, e.minSupportedVersion, where, error)) {
            return false;
        }

        std::uint64_t abi = 0;
        if (!wantUint(pj, "abiVersion", false, abi, where, error)) {
            return false;
        }
        if (abi > 0xFFFFFFFFull) {
            error = where + ": abiVersion is out of range";
            return false;
        }
        e.abiVersion = static_cast<std::uint32_t>(abi);
        // RULE 5. An absent or different abiVersion is not a parse failure -
        // the entry must still be listed, so the UI can explain WHY it cannot
        // be installed rather than leaving the user to wonder where it went.
        e.compatible = (e.abiVersion == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));

        const auto platIt = pj.find("platforms");
        if (platIt != pj.end() && !platIt->is_null()) {
            if (!platIt->is_array()) {
                error = where + ": \"platforms\" is not an array";
                return false;
            }
            std::size_t pidx = 0;
            for (const json& plj : *platIt) {
                const std::string pwhere = where + " platform " + std::to_string(pidx);
                ++pidx;
                if (!plj.is_object()) {
                    error = pwhere + " is not a JSON object";
                    return false;
                }
                PluginPlatform p;
                if (!wantString(plj, "os", true, p.os, pwhere, error) ||
                    !wantString(plj, "arch", true, p.arch, pwhere, error) ||
                    !wantString(plj, "file", true, p.file, pwhere, error) ||
                    !wantString(plj, "url", true, p.url, pwhere, error) ||
                    !wantString(plj, "sha256", true, p.sha256, pwhere, error)) {
                    return false;
                }
                // RULE 1, at parse time: a plain-text URL never even reaches
                // the transport.
                if (!isHttpsUrl(p.url)) {
                    error = pwhere + ": url is not https: \"" + p.url + "\"";
                    return false;
                }
                // RULE 2: no hash, no entry. There is no "unverified" state.
                if (!isWellFormedSha256(p.sha256)) {
                    error = pwhere + ": sha256 must be 64 hexadecimal digits, got \"" +
                            p.sha256 + "\"";
                    return false;
                }
                p.sha256 = toLowerAscii(p.sha256);
                std::uint64_t size = 0;
                if (!wantUint(plj, "sizeBytes", false, size, pwhere, error)) {
                    return false;
                }
                p.sizeBytes = size;
                e.platforms.push_back(std::move(p));
            }
        }
        // A missing "platforms" key is not an error: an entry announced before
        // its first build exists is a legitimate state, and thisPlatform()
        // simply reports nullptr.

        // The shop-window fields last: they cannot fail the entry, and `e.id`
        // is settled by now for the report to name.
        readShopFields(pj, e, report);

        parsed.push_back(std::move(e));
    }

    out = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

bool PluginRepo::sha256Hex(const void* data, std::size_t n, std::string& hexOut,
                           std::string& error) {
    hexOut.clear();
    error.clear();
    Sha256 h;
    if (!h.init(error)) {
        return false;
    }
    // A null pointer with n == 0 is legal for the caller; give the digest a
    // valid address anyway rather than relying on it ignoring the pointer.
    static const unsigned char kEmpty = 0;
    if (!h.update(n == 0 ? &kEmpty : data, n, error)) {
        return false;
    }
    return h.finishHex(hexOut, error);
}

bool PluginRepo::sha256Matches(const std::string& expectedHex, const std::string& actualHex) {
    // Both sides must be real digests before they are allowed to agree.
    if (!isWellFormedSha256(expectedHex) || !isWellFormedSha256(actualHex)) {
        return false;
    }
    return iequalsAscii(expectedHex, actualHex);
}

bool PluginRepo::sha256File(const std::string& path, std::string& hexOut, std::string& error) {
    hexOut.clear();
    error.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open \"" + path + "\" for hashing";
        return false;
    }
    Sha256 h;
    if (!h.init(error)) {
        return false;
    }
    std::vector<char> buf(64 * 1024);
    for (;;) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0 && !h.update(buf.data(), static_cast<std::size_t>(got), error)) {
            return false;
        }
        if (!f) {
            break;  // eof (or a read error, caught below)
        }
    }
    if (f.bad()) {
        error = "read error while hashing \"" + path + "\"";
        return false;
    }
    return h.finishHex(hexOut, error);
}

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------

int PluginRepo::compareVersions(const std::string& a, const std::string& b) {
    const std::vector<std::string> sa = splitVersionSegments(a);
    const std::vector<std::string> sb = splitVersionSegments(b);
    const std::size_t n = sa.size() > sb.size() ? sa.size() : sb.size();
    for (std::size_t i = 0; i < n; ++i) {
        // A segment past the end - or an empty one from "1..2" or a trailing
        // dot - is zero, which is what makes "1.2" and "1.2.0" the same
        // version rather than two versions with an arbitrary order.
        std::string x = i < sa.size() ? sa[i] : std::string();
        std::string y = i < sb.size() ? sb[i] : std::string();
        if (x.empty()) {
            x = "0";
        }
        if (y.empty()) {
            y = "0";
        }
        const bool nx = allDigits(x);
        const bool ny = allDigits(y);
        if (nx && ny) {
            const int c = compareDigitSegments(x, y);
            if (c != 0) {
                return c;
            }
        } else if (nx != ny) {
            // A number outranks a non-number: "1.0.0" > "1.0.0-rc1", and an
            // unparseable version can never masquerade as newer.
            return nx ? 1 : -1;
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The local manifest
// ---------------------------------------------------------------------------

const char* PluginRepo::manifestFileName() { return "installed.json"; }

std::string PluginRepo::manifestPath(const std::string& pluginsDir) {
    return (fs::path(pluginsDir) / manifestFileName()).string();
}

bool PluginRepo::parseManifest(const std::string& text, std::vector<InstalledPlugin>& plugins,
                               std::vector<CachedPolicy>& policies,
                               std::vector<std::string>& notes, std::string& error) {
    plugins.clear();
    policies.clear();
    error.clear();

    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "plugin manifest: not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "plugin manifest: the root is not a JSON object";
        return false;
    }
    {
        // Absent schemaVersion is tolerated (assumed 1); a DIFFERENT one is
        // not, because a future schema may reinterpret the retirement floor,
        // and a floor this code misread would either brick a working plugin
        // or fail to retire a broken one.
        const auto it = j.find("schemaVersion");
        if (it != j.end() && (!it->is_number_integer() || it->get<int>() != 1)) {
            error = "plugin manifest: unsupported schemaVersion (expected 1)";
            return false;
        }
    }

    const auto pluginsIt = j.find("plugins");
    if (pluginsIt != j.end() && !pluginsIt->is_null()) {
        if (!pluginsIt->is_array()) {
            error = "plugin manifest: \"plugins\" is not an array";
            return false;
        }
        std::size_t idx = 0;
        for (const json& pj : *pluginsIt) {
            const std::string where = "manifest record " + std::to_string(idx);
            ++idx;
            if (!pj.is_object()) {
                notes.push_back(where + " is not an object; ignored");
                continue;
            }
            InstalledPlugin r;
            std::string ignoredError;
            if (!wantString(pj, "id", true, r.id, where, ignoredError) ||
                !wantString(pj, "version", true, r.version, where, ignoredError) ||
                !wantString(pj, "file", true, r.file, where, ignoredError)) {
                notes.push_back(where + " has no usable id/version/file; ignored");
                continue;
            }
            (void)wantString(pj, "name", false, r.name, where, ignoredError);
            // A manifest is a file on disk that a user, an installer or a bad
            // shutdown can have written. Its file name is put through exactly
            // the same guard as one that came off the wire, BEFORE any code
            // below joins it to a directory.
            std::string safeName;
            std::string nameError;
            if (!sanitiseFileName(r.file, safeName, nameError)) {
                notes.push_back(where + " (\"" + r.id + "\") names an unusable file: " +
                                nameError + "; ignored");
                continue;
            }
            std::string sha;
            (void)wantString(pj, "sha256", false, sha, where, ignoredError);
            if (!sha.empty() && !isWellFormedSha256(sha)) {
                notes.push_back(where + " (\"" + r.id + "\") has a malformed sha256; discarded");
                sha.clear();
            }
            r.sha256 = toLowerAscii(sha);

            const auto abiIt = pj.find("abiVersion");
            if (abiIt != pj.end() && abiIt->is_number_unsigned()) {
                const std::uint64_t v = abiIt->get<std::uint64_t>();
                // Out of range stays 0, i.e. "not recorded", i.e. fail open.
                r.abiVersion = v <= 0xFFFFFFFFull ? static_cast<std::uint32_t>(v) : 0u;
            }
            const auto atIt = pj.find("installedAt");
            if (atIt != pj.end() && atIt->is_number_integer()) {
                r.installedAtUnix = atIt->get<std::int64_t>();
            }

            bool duplicate = false;
            for (const InstalledPlugin& existing : plugins) {
                if (existing.id == r.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                notes.push_back(where + " repeats id \"" + r.id + "\"; the later record is ignored");
                continue;
            }
            plugins.push_back(std::move(r));
        }
    }

    const auto polIt = j.find("policies");
    if (polIt != j.end() && !polIt->is_null()) {
        if (!polIt->is_array()) {
            error = "plugin manifest: \"policies\" is not an array";
            plugins.clear();
            return false;
        }
        std::size_t idx = 0;
        for (const json& pj : *polIt) {
            const std::string where = "manifest policy " + std::to_string(idx);
            ++idx;
            if (!pj.is_object()) {
                notes.push_back(where + " is not an object; ignored");
                continue;
            }
            CachedPolicy p;
            std::string ignoredError;
            if (!wantString(pj, "id", true, p.id, where, ignoredError)) {
                notes.push_back(where + " has no id; ignored");
                continue;
            }
            (void)wantString(pj, "minSupportedVersion", false, p.minSupportedVersion, where,
                             ignoredError);
            (void)wantString(pj, "catalogueVersion", false, p.catalogueVersion, where,
                             ignoredError);
            const auto abiIt = pj.find("abiVersion");
            if (abiIt != pj.end() && abiIt->is_number_unsigned()) {
                const std::uint64_t v = abiIt->get<std::uint64_t>();
                p.abiVersion = v <= 0xFFFFFFFFull ? static_cast<std::uint32_t>(v) : 0u;
            }
            p.known = true;
            bool duplicate = false;
            for (const CachedPolicy& existing : policies) {
                if (existing.id == p.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                notes.push_back(where + " repeats id \"" + p.id + "\"; the later policy is ignored");
                continue;
            }
            policies.push_back(std::move(p));
        }
    }
    return true;
}

std::string PluginRepo::serialiseManifest(const std::vector<InstalledPlugin>& plugins,
                                          const std::vector<CachedPolicy>& policies) {
    json j;
    j["schemaVersion"] = 1;
    json arr = json::array();
    for (const InstalledPlugin& r : plugins) {
        json e;
        e["id"] = r.id;
        e["name"] = r.name;
        e["version"] = r.version;
        e["file"] = r.file;
        e["sha256"] = r.sha256;
        e["abiVersion"] = r.abiVersion;
        e["installedAt"] = r.installedAtUnix;
        arr.push_back(std::move(e));
    }
    j["plugins"] = std::move(arr);
    json pols = json::array();
    for (const CachedPolicy& p : policies) {
        if (!p.known) {
            continue;  // an unknown policy is the absence of a row, not a row
        }
        json e;
        e["id"] = p.id;
        e["minSupportedVersion"] = p.minSupportedVersion;
        e["catalogueVersion"] = p.catalogueVersion;
        e["abiVersion"] = p.abiVersion;
        pols.push_back(std::move(e));
    }
    j["policies"] = std::move(pols);
    // error_handler_t::replace, like every dump() in this tree: this text
    // includes user-entered or remote strings, and a byte that is not valid
    // UTF-8 must cost one replacement character, never a throw out of a save
    // path. tests/test_json_dump_policy.cpp holds every site to this.
    return j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) +
           "\n";
}

bool PluginRepo::saveManifest(const std::string& pluginsDir,
                              const std::vector<InstalledPlugin>& plugins,
                              const std::vector<CachedPolicy>& policies, std::string& error) {
    error.clear();
    std::error_code ec;
    fs::create_directories(fs::path(pluginsDir), ec);
    if (!fs::is_directory(fs::path(pluginsDir), ec)) {
        error = "plugin manifest: cannot create the plugins directory \"" + pluginsDir + "\"";
        return false;
    }

    const fs::path target(manifestPath(pluginsDir));
    const std::string text = serialiseManifest(plugins, policies);

    // Same atomic route as ConfigStore, for the same reason: a torn manifest
    // reads as corrupt, and a corrupt manifest silently forgets every cached
    // retirement floor.
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "plugin manifest: cannot create temp file \"" + tmp.string() + "\"";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code ignored;
            fs::remove(tmp, ignored);
            error = "plugin manifest: write to \"" + tmp.string() + "\" failed";
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "plugin manifest: atomic replace of \"" + target.string() +
                "\" failed: " + ec.message();
        return false;
    }
    return true;
}

bool PluginRepo::loadInventory(const std::string& pluginsDir, PluginInventory& out,
                               std::string& error) {
    out = PluginInventory{};
    error.clear();

    // ---- The manifest, if there is one ------------------------------------
    const fs::path mpath(manifestPath(pluginsDir));
    std::error_code ec;
    bool ok = true;
    if (fs::exists(mpath, ec)) {
        out.manifestPresent = true;
        // Through readTextFile (core/file_read.hpp, 0.99.65): a folder on the manifest's name, or a
        // read that fails, is "cannot be read" - the stream read this replaced threw on Linux.
        std::string text;
        if (readTextFile(mpath, text) != ReadResult::Ok) {
            error = "plugin manifest: cannot open \"" + mpath.string() + "\" for reading";
            out.notes.push_back(error + "; treating every installed plugin as unmanaged");
            ok = false;
        } else {
            if (!parseManifest(text, out.plugins, out.policies, out.notes, error)) {
                out.notes.push_back(error + "; treating every installed plugin as unmanaged");
                out.plugins.clear();
                out.policies.clear();
                ok = false;
            } else {
                out.manifestUsable = true;
            }
        }
    } else {
        // Not an error, and emphatically not a reason to block anything: a
        // first run, or plugins dropped in by hand, look exactly like this.
        out.notes.push_back("no plugin manifest yet; nothing is recorded as installed");
    }

    // ---- What is actually on disk -----------------------------------------
    std::vector<std::string> onDisk;
    if (fs::is_directory(fs::path(pluginsDir), ec)) {
        for (auto it = fs::directory_iterator(fs::path(pluginsDir), ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) {
                continue;
            }
            const std::string name = it->path().filename().string();
            if (hasDllExtension(name)) {
                onDisk.push_back(name);
            }
        }
    } else {
        out.notes.push_back("the plugins directory \"" + pluginsDir + "\" does not exist");
    }

    // ---- Reconcile: the manifest is a record, the disk is the truth --------
    std::vector<bool> claimed(onDisk.size(), false);
    for (InstalledPlugin& r : out.plugins) {
        std::size_t found = onDisk.size();
        for (std::size_t i = 0; i < onDisk.size(); ++i) {
            if (iequalsAscii(onDisk[i], r.file)) {  // NTFS is case-insensitive
                found = i;
                break;
            }
        }
        if (found == onDisk.size()) {
            r.missingFromDisk = true;
            out.notes.push_back("\"" + r.id + "\" is recorded as installed but its file \"" +
                                r.file + "\" is not there");
            continue;
        }
        claimed[found] = true;
        if (r.sha256.empty()) {
            continue;
        }
        // Re-hash rather than assume. A record that says "these bytes" and a
        // file that is different bytes is worth saying out loud - it is either
        // a manual overwrite or something worse.
        std::string actual;
        std::string hashError;
        const fs::path full = fs::path(pluginsDir) / r.file;
        if (!sha256File(full.string(), actual, hashError)) {
            out.notes.push_back("cannot verify \"" + r.file + "\": " + hashError);
        } else if (!sha256Matches(r.sha256, actual)) {
            r.digestMismatch = true;
            out.notes.push_back("\"" + r.file + "\" is not the file that was installed for \"" +
                                r.id + "\" (recorded " + r.sha256 + ", found " + actual + ")");
        }
    }
    for (std::size_t i = 0; i < onDisk.size(); ++i) {
        if (!claimed[i]) {
            out.unmanaged.push_back(onDisk[i]);
            out.notes.push_back("\"" + onDisk[i] +
                                "\" is installed but not recorded; it is left alone");
        }
    }
    return ok;
}

std::string PluginRepo::changedSinceInstallNote(const std::vector<InstalledPlugin>& records,
                                                const std::string& file) {
    if (file.empty()) { return {}; }
    for (const InstalledPlugin& r : records) {
        if (!r.digestMismatch || !iequalsAscii(r.file, file)) { continue; }
        return "\"" + r.file + "\" is not the file that was installed for \"" + r.id +
               "\": its bytes no longer match the sha256 recorded when it was installed. "
               "If you did not replace it yourself, remove it and fit it again from the "
               "plugin store.";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Cached catalogue policy
// ---------------------------------------------------------------------------

CachedPolicy PluginRepo::policyFor(const std::vector<CachedPolicy>& policies,
                                   const std::string& id) {
    for (const CachedPolicy& p : policies) {
        if (p.id == id) {
            return p;
        }
    }
    CachedPolicy none;
    none.id = id;
    none.known = false;  // explicit: this is THE fail-open state
    return none;
}

void PluginRepo::mergePolicies(std::vector<CachedPolicy>& cached,
                               const std::vector<PluginCatalogEntry>& catalogue) {
    for (const PluginCatalogEntry& e : catalogue) {
        if (e.id.empty()) {
            continue;
        }
        CachedPolicy fresh;
        fresh.id = e.id;
        fresh.known = true;
        fresh.minSupportedVersion = e.minSupportedVersion;
        fresh.catalogueVersion = e.version;
        fresh.abiVersion = e.abiVersion;
        bool replaced = false;
        for (CachedPolicy& p : cached) {
            if (p.id == e.id) {
                p = fresh;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            cached.push_back(std::move(fresh));
        }
    }
    // Ids the catalogue did not mention keep whatever they had. Deliberate:
    // an empty, truncated or substituted catalogue must not be able to lift a
    // retirement floor that was already published.
}

namespace {

// Field for field and in order: the merge replaces a policy in its own place and
// appends a new one, so two lists that mean the same thing are the same list.
bool sameCachedPolicies(const std::vector<CachedPolicy>& a, const std::vector<CachedPolicy>& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].known != b[i].known ||
            a[i].minSupportedVersion != b[i].minSupportedVersion ||
            a[i].catalogueVersion != b[i].catalogueVersion ||
            a[i].abiVersion != b[i].abiVersion) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool PluginRepo::cacheCataloguePolicies(const std::string& pluginsDir,
                                        const std::vector<PluginCatalogEntry>& catalogue,
                                        std::string& error) {
    PluginInventory inv;
    std::string loadError;
    // A corrupt manifest is not a reason to refuse to cache the new policy;
    // it is a reason to write a fresh one. What is lost is the record of
    // which plugins are installed (they degrade to unmanaged, i.e. fail open),
    // never a floor - the floors are being rewritten from the catalogue here.
    (void)loadInventory(pluginsDir, inv, loadError);
    const std::vector<CachedPolicy> before = inv.policies;
    mergePolicies(inv.policies, catalogue);
    // A CATALOGUE THAT TAUGHT US NOTHING WRITES NOTHING (0.99.63). The manifest
    // is in the plugins folder, and the plugin rescan decides whether the folder
    // has changed by its listing (core/plugin_dir_signature.hpp): a manifest
    // rewritten with the bytes it already had is a changed last-write time, so
    // every catalogue fetch looked like a change and a rescan that had nothing to
    // find tore every plugin down. Only a USABLE manifest is left alone: a
    // missing or corrupt one is still written, as it always was, and a floor
    // that moved - or a policy that is new - still is.
    if (inv.manifestUsable && sameCachedPolicies(before, inv.policies)) {
        error.clear();
        return true;
    }
    return saveManifest(pluginsDir, inv.plugins, inv.policies, error);
}

// ---------------------------------------------------------------------------
// Retirement
// ---------------------------------------------------------------------------

PluginBlockReason PluginRepo::pluginBlockReason(const InstalledPlugin& installed,
                                                const CachedPolicy& policy) {
    // ABI first. It is the harder break and the different remedy, so a plugin
    // that is BOTH stale and built for the wrong ABI must be reported as the
    // one an update cannot fix.
    //
    // 0 means "the manifest never recorded it" - unknown, not wrong. Blocking
    // on that would retire every plugin recorded by an older build of this
    // product, which is the fail-open rule's whole point.
    if (installed.abiVersion != 0 &&
        installed.abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
        return PluginBlockReason::AbiMismatch;
    }
    // No catalogue has ever described this plugin: a private build, an
    // enterprise plugin, or simply a user who has never opened the browser.
    // Nothing is known, so nothing is enforced.
    if (!policy.known || policy.minSupportedVersion.empty()) {
        return PluginBlockReason::None;
    }
    // A record with no version tells us nothing about which side of the floor
    // it is on. Same rule: no information, no block.
    if (installed.version.empty()) {
        return PluginBlockReason::None;
    }
    if (compareVersions(installed.version, policy.minSupportedVersion) < 0) {
        return PluginBlockReason::BelowMinimumVersion;
    }
    return PluginBlockReason::None;
}

std::string PluginRepo::pluginBlockMessage(const InstalledPlugin& installed,
                                           const CachedPolicy& policy) {
    const PluginBlockReason r = pluginBlockReason(installed, policy);
    // The label the user recognises: the display name if we recorded one, the
    // file name if not. Never the id, which is a machine key.
    const std::string label = !installed.name.empty()
                                  ? installed.name
                                  : (!installed.file.empty() ? installed.file : installed.id);
    const std::string versioned =
        installed.version.empty() ? label : (label + " " + installed.version);

    // USER COPY, SO EACH SENTENCE IS ONE TRANSLATABLE FORMAT STRING. These
    // were joined from fragments around the plugin's name, which no catalogue
    // can translate; with English in force tr() hands back the English and the
    // text is exactly what it was. The sentences are joined with a space, and
    // a whole sentence is the smallest thing ever handed to a translator.
    using cascade::i18n::tr;
    const auto fill = [](const char* format, const std::string& a, const std::string& b) {
        return formatText(format, a.c_str(), b.c_str());
    };
    switch (r) {
        case PluginBlockReason::None:
            return std::string();

        case PluginBlockReason::AbiMismatch:
            // No "ABI", no version numbers the user cannot act on, and NO
            // instruction to update: if the author has not published a build
            // for this release, updating cannot possibly help, and sending
            // someone round that loop is worse than telling them the truth.
            return fill(tr("%s was built for a different version of FoxSDR and has been "
                           "disabled. It needs a new build from the plugin's author before it "
                           "can be used again."),
                        versioned, std::string());

        case PluginBlockReason::BelowMinimumVersion: {
            std::string m = fill(tr("%s is out of date and has been disabled. Version %s or "
                                    "newer is required."),
                                 versioned, policy.minSupportedVersion);
            m += ' ';
            if (!policy.catalogueVersion.empty() &&
                compareVersions(policy.catalogueVersion, installed.version) > 0) {
                // The one-click case, and the only one where "Update it" is
                // true: the last catalogue we saw really did have a newer build.
                m += fill(tr("Version %s is available - update it to use it again."),
                          policy.catalogueVersion, std::string());
            } else {
                m += tr("No newer version was in the last plugin catalogue seen, so check for "
                        "updates or ask the plugin's author for a current build.");
            }
            return m;
        }
    }
    return std::string();
}

std::vector<BlockedPlugin> PluginRepo::blockedPlugins(
    const std::vector<InstalledPlugin>& installed, const std::vector<CachedPolicy>& policies) {
    std::vector<BlockedPlugin> out;
    for (const InstalledPlugin& p : installed) {
        const CachedPolicy policy = policyFor(policies, p.id);
        const PluginBlockReason r = pluginBlockReason(p, policy);
        if (r == PluginBlockReason::None) {
            continue;
        }
        BlockedPlugin b;
        b.installed = p;
        b.policy = policy;
        b.reason = r;
        b.message = pluginBlockMessage(p, policy);
        out.push_back(std::move(b));
    }
    return out;
}

std::size_t PluginRepo::blockedCount(const std::vector<InstalledPlugin>& installed,
                                     const std::vector<CachedPolicy>& policies) {
    std::size_t n = 0;
    for (const InstalledPlugin& p : installed) {
        if (pluginBlockReason(p, policyFor(policies, p.id)) != PluginBlockReason::None) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Update planning
// ---------------------------------------------------------------------------

std::vector<PluginUpdate> PluginRepo::planUpdates(const std::vector<PluginCatalogEntry>& catalogue,
                                                  const std::vector<InstalledPlugin>& installed) {
    std::vector<PluginUpdate> out;
    for (const PluginCatalogEntry& e : catalogue) {
        if (e.id.empty()) {
            continue;
        }
        const InstalledPlugin* have = nullptr;
        for (const InstalledPlugin& p : installed) {
            if (p.id == e.id) {
                have = &p;
                break;
            }
        }
        // Not installed: this is an update planner, not an installer. A
        // catalogue entry nobody has is a browser listing, nothing more.
        if (have == nullptr) {
            continue;
        }
        // The user deleted the file. Updating would put it back, which is not
        // what "update" means to anyone.
        if (have->missingFromDisk) {
            continue;
        }
        // Rule 5, again: an entry this host cannot load is not an upgrade.
        if (e.abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
            continue;
        }
        if (e.thisPlatform() == nullptr) {
            continue;
        }

        const int cmp = compareVersions(e.version, have->version);
        if (cmp < 0) {
            // NEVER a downgrade. Not for a floor, not for an ABI mismatch, not
            // for anything: a catalogue that has gone backwards is a mistake
            // or an attack, and replacing a working newer plugin with an older
            // one serves neither case.
            continue;
        }

        PluginUpdate u;
        if (cmp > 0) {
            u.reason = "version " + e.version + " replaces the installed " + have->version;
        } else if (have->abiVersion != 0 &&
                   have->abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
            // Same version number, but the installed build cannot run here and
            // the catalogue's build can. A rebuild at the same version is the
            // one case where "same version" is still worth downloading.
            u.reason = "the installed build does not work with this version of FoxSDR; the "
                       "catalogue has a rebuilt " +
                       e.version;
        } else {
            continue;  // same version, same ABI: already up to date
        }
        u.id = e.id;
        u.fromVersion = have->version;
        u.toVersion = e.version;
        u.entry = &e;
        out.push_back(std::move(u));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Capability names and installed state by id and version (0.99.72)
// ---------------------------------------------------------------------------

std::uint32_t PluginRepo::capabilityBitForName(const std::string& name) {
    for (const CapabilityName& c : kCapabilityNames) {
        if (name == c.name) { return c.bit; }
    }
    return 0;
}

InstallState PluginRepo::installStateFor(const PluginCatalogEntry& e,
                                         const std::vector<InstalledPlugin>& installed,
                                         const std::vector<ScannedModule>& scanned) {
    InstallState st;
    if (e.id.empty()) { return st; }

    bool found = false;
    std::string newest;
    const auto consider = [&](const std::string& version) {
        if (!found || compareVersions(version, newest) > 0) { newest = version; }
        found = true;
    };
    // The same-version rebuild planUpdates() also plans: an install record whose
    // build targeted another ABI than this host's.
    bool recordedForOtherAbi = false;
    const std::uint32_t hostAbi = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);

    // THE SCAN FIRST: what the module itself declares is what is running.
    for (const ScannedModule& m : scanned) {
        if (m.id == e.id) { consider(m.version); }
    }
    for (const InstalledPlugin& r : installed) {
        if (r.id != e.id) { continue; }
        // A record whose file the user deleted is not an installed plugin, and
        // updating it would put back what they removed (planUpdates says the same).
        if (r.missingFromDisk) { continue; }
        if (r.abiVersion != 0 && r.abiVersion != hostAbi) { recordedForOtherAbi = true; }
        // The scan wins where both know the module - the file is the join.
        bool scanKnowsIt = false;
        for (const ScannedModule& m : scanned) {
            if (m.id == e.id && iequalsAscii(m.file, r.file)) {
                scanKnowsIt = true;
                break;
            }
        }
        if (!scanKnowsIt) { consider(r.version); }
    }
    if (!found) { return st; }

    st.installedVersion = newest;
    const int cmp = compareVersions(e.version, newest);
    if (cmp > 0) {
        st.kind = InstallStateKind::UpdateAvailable;
    } else if (cmp < 0) {
        st.kind = InstallStateKind::NewerInstalled;
    } else if (recordedForOtherAbi && e.abiVersion == hostAbi) {
        st.kind = InstallStateKind::UpdateAvailable;
    } else {
        st.kind = InstallStateKind::Installed;
    }
    return st;
}

// ---------------------------------------------------------------------------
// Network operations
// ---------------------------------------------------------------------------


bool PluginRepo::fetchText(const std::string& url, std::uint64_t maxBytes, std::string& out,
                           std::string& error) {
    out.clear();
    error.clear();
    if (!isHttpsUrl(url)) {
        error = "refusing a non-https URL: \"" + url + "\"";
        return false;
    }
    const auto sink = [&out, maxBytes](const void* p, std::size_t n) {
        if (out.size() + n > maxBytes) { return false; }
        out.append(static_cast<const char*>(p), n);
        return true;
    };
    return httpsGet(url, maxBytes, sink, nullptr, nullptr, error);
}

bool PluginRepo::fetchVerifiedFile(const std::string& url, const std::string& expectedSha256,
                                   const std::string& destPath, std::uint64_t maxBytes,
                                   std::string& error, std::atomic<float>* progress,
                                   std::atomic<bool>* cancel, FetchFailure* failure) {
    error.clear();
    // WHICH KIND OF FAILURE, for the caller that counts them (see FetchFailure).
    // Set at every way out, so a caller never reads a stale value.
    const auto fail = [failure](FetchFailure why) {
        if (failure != nullptr) { *failure = why; }
        return false;
    };
    if (failure != nullptr) { *failure = FetchFailure::None; }
    if (!isHttpsUrl(url)) {
        error = "refusing a non-https download URL: \"" + url + "\"";
        return fail(FetchFailure::Refused);
    }
    if (!isWellFormedSha256(expectedSha256)) {
        error = "sha256 must be 64 hexadecimal digits";
        return fail(FetchFailure::Refused);
    }

    // The temp file sits beside the destination so the final move is a
    // same-volume rename; a cross-volume "rename" degrades to copy+delete and
    // reopens the partial-file window this ordering exists to close.
    const fs::path target(destPath);
    fs::path tmp = target;
    tmp += ".part";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::remove(tmp, ec);  // debris from a killed earlier run

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot create \"" + tmp.string() + "\"";
            return fail(FetchFailure::Place);
        }
        std::uint64_t written = 0;
        const auto sink = [&f, &written, maxBytes](const void* p, std::size_t n) {
            written += n;
            if (written > maxBytes) { return false; }
            f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
            return static_cast<bool>(f);
        };
        if (!httpsGet(url, maxBytes, sink, progress, cancel, error)) {
            // A write the disk refused ends the transfer too, from inside the
            // sink: that is the disk's failure, not the network's.
            const bool diskFull = !f;
            f.close();
            fs::remove(tmp, ec);
            if (error == "cancelled") { return fail(FetchFailure::Cancelled); }
            return fail(diskFull ? FetchFailure::Place : FetchFailure::Transfer);
        }
    }

    // VERIFY BEFORE NAMING. Until this passes, what is on disk is a ".part"
    // that nothing will run.
    std::string actual;
    if (!sha256File(tmp.string(), actual, error)) {
        fs::remove(tmp, ec);
        return fail(FetchFailure::Place);
    }
    if (!sha256Matches(expectedSha256, actual)) {
        fs::remove(tmp, ec);
        error = "sha256 mismatch: expected " + toLowerAscii(expectedSha256) + ", got " + actual;
        return fail(FetchFailure::Verify);
    }

    fs::remove(target, ec);
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "cannot move the verified file into place: " + ec.message();
        return fail(FetchFailure::Place);
    }
    return true;
}

bool PluginRepo::fetchIndex(const std::string& url, std::string& error) {
    entries_.clear();
    lastIndexText_.clear();
    lastParseReport_.clear();
    error.clear();
    progress_.store(0.0f, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);

    // COUNTED, ANONYMOUSLY (0.99.64, core/health_events.hpp): a catalogue that
    // could not be had - not reachable, not a catalogue, refused - but not one
    // the user (or a quit) cancelled. Nothing of the error text is kept.
    const auto failed = [&error]() {
        if (error != "cancelled") { health::notePluginCatalogueFailed(); }
        return false;
    };

    // RULE 1, before any socket exists.
    if (!isHttpsUrl(url)) {
        error = "refusing a non-https catalogue URL: \"" + url + "\"";
        return failed();
    }
    std::string body;
    body.reserve(64 * 1024);
    const auto sink = [&body](const void* p, std::size_t n) {
        body.append(static_cast<const char*>(p), n);
        return true;
    };
    if (!httpsGet(url, kMaxIndexBytes, sink, &progress_, &cancel_, error)) {
        return failed();
    }
    progress_.store(1.0f, std::memory_order_relaxed);
    // A parse failure leaves entries_ empty, which fetchIndex documents.
    if (!parseIndex(body, entries_, error, &lastParseReport_)) {
        lastParseReport_.clear();
        return failed();
    }
    // Kept only now that it parsed: the text saveCatalogueCache() writes is text
    // this client accepted, never text it refused.
    lastIndexText_ = std::move(body);
    return true;
}

bool PluginRepo::fetchRegionalIndex(const std::string& url, std::vector<PluginCatalogEntry>& out,
                                    std::string& error) {
    out.clear();
    error.clear();

    // RULE 1, before any socket exists - same as fetchIndex(), and the same
    // reason: the refusal must be provable without a network stack present.
    if (!isHttpsUrl(url)) {
        error = "refusing a non-https regional catalogue URL: \"" + url + "\"";
        return false;
    }
    std::string body;
    body.reserve(4 * 1024);
    const auto sink = [&body](const void* p, std::size_t n) {
        body.append(static_cast<const char*>(p), n);
        return true;
    };
    // Deliberately NOT progress_ (this is a side request beside the public
    // fetch's own progress, not a second phase of it) - cancel_ IS shared,
    // and NOT reset here, so a cancel() aimed at the public fetch this
    // immediately follows still stops this one. httpsGet checks it before
    // ever opening a connection, which is what makes R14's "no hang" true.
    //
    // SHORT TIMEOUTS, deliberately much tighter than the public catalogue's
    // (HttpTimeouts{} defaults): this call runs synchronously right after a
    // successful public fetch, on the one catalogue worker, so a black-holed
    // or slow foxsdr.com would otherwise hold back a public list the store
    // already has, for as long as the public fetch's own budget (tens of
    // seconds). 4 s to connect, 8 s total to receive the (256 KiB-capped,
    // normally few-KB) body - generous for a healthy connection, and short
    // enough that "the store took forever to open" stops being a symptom of
    // this feature. See the release judge's finding on this delay.
    if (!httpsGet(url, kMaxRegionalIndexBytes, sink, nullptr, &cancel_, error,
                  HttpTimeouts{/*connectMs=*/4000, /*sendMs=*/4000, /*receiveMs=*/8000})) {
        return false;
    }
    return parseIndex(body, out, error);
}

// ---------------------------------------------------------------------------
// The catalogue cache (0.99.72)
// ---------------------------------------------------------------------------

const char* PluginRepo::catalogueCacheFileName() { return "catalogue.json"; }
const char* PluginRepo::catalogueCacheTimeFileName() { return "catalogue.json.time"; }
const char* PluginRepo::screenshotCacheDirName() { return "store-cache"; }

namespace {

// <target>.part, whole, then one rename over <target>: the manifest's route, so a
// crash or a full disk leaves the old file or the new one and never half of
// either, and a failed write leaves no ".part" behind.
bool writeWholeFileAtomically(const fs::path& target, const std::string& text,
                              std::string& error) {
    fs::path part = target;
    part += ".part";
    std::error_code ec;
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot create \"" + part.string() + "\"";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(part, ec);
            error = "writing \"" + part.string() + "\" failed";
            return false;
        }
    }
    fs::rename(part, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(part, ignored);
        error = "cannot replace \"" + target.string() + "\": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool PluginRepo::saveCatalogueCache(const std::string& pluginsDir, const std::string& rawIndexText,
                                    std::int64_t readTimeUnix, std::string& error) {
    error.clear();
    if (pluginsDir.empty()) {
        error = "no plugins directory to keep the catalogue in";
        return false;
    }
    if (rawIndexText.empty()) {
        error = "there is no catalogue text to keep";
        return false;
    }
    if (rawIndexText.size() > kMaxIndexBytes) {
        error = "the catalogue is larger than the " + std::to_string(kMaxIndexBytes) +
                "-byte limit, so it is not kept";
        return false;
    }
    // THE CACHE HOLDS WHAT THIS CLIENT ACCEPTS. The caller hands over text that
    // parsed, and this checks it anyway: a careless caller must not be able to
    // make the next start offer what a fetch refused.
    {
        std::vector<PluginCatalogEntry> scratch;
        std::string parseError;
        if (!parseIndex(rawIndexText, scratch, parseError)) {
            error = "the catalogue is not kept because it is not one: " + parseError;
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(fs::path(pluginsDir), ec);
    if (!fs::is_directory(fs::path(pluginsDir), ec)) {
        error = "cannot create the plugins directory \"" + pluginsDir + "\"";
        return false;
    }
    if (!writeWholeFileAtomically(fs::path(pluginsDir) / catalogueCacheFileName(), rawIndexText,
                                  error)) {
        return false;
    }
    return writeWholeFileAtomically(fs::path(pluginsDir) / catalogueCacheTimeFileName(),
                                    std::to_string(readTimeUnix) + "\n", error);
}

bool PluginRepo::loadCachedIndex(const std::string& pluginsDir,
                                 std::vector<PluginCatalogEntry>& entries, std::int64_t& readTime,
                                 std::string& error) {
    entries.clear();
    readTime = 0;
    error.clear();
    const fs::path file = fs::path(pluginsDir) / catalogueCacheFileName();
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        error = "no catalogue has been kept yet";
        return false;
    }
    std::string text;
    if (readTextFile(file, text) != ReadResult::Ok) {
        error = "the kept catalogue cannot be read";
        return false;
    }
    if (text.size() > kMaxIndexBytes) {
        error = "the kept catalogue is larger than the " + std::to_string(kMaxIndexBytes) +
                "-byte limit";
        return false;
    }
    std::string parseError;
    if (!parseIndex(text, entries, parseError)) {
        entries.clear();
        error = "the kept catalogue is not usable and was ignored: " + parseError;
        return false;
    }
    // The time is a second file; its absence or damage costs the date, not the
    // list. 0 reads as "not recorded".
    std::string timeText;
    if (readTextFile(fs::path(pluginsDir) / catalogueCacheTimeFileName(), timeText) ==
        ReadResult::Ok) {
        while (!timeText.empty() && (timeText.back() == '\n' || timeText.back() == '\r' ||
                                     timeText.back() == ' ')) {
            timeText.pop_back();
        }
        if (!timeText.empty() && timeText.size() <= 18 && allDigits(timeText)) {
            readTime = std::stoll(timeText);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Screenshots (0.99.72)
// ---------------------------------------------------------------------------

std::string PluginRepo::screenshotCachePath(const std::string& pluginsDir,
                                            const std::string& sha256) {
    if (!isWellFormedSha256(sha256)) { return std::string(); }
    return (fs::path(pluginsDir) / screenshotCacheDirName() / (toLowerAscii(sha256) + ".png"))
        .string();
}

bool PluginRepo::fetchScreenshot(const std::string& pluginsDir, const CatalogScreenshot& s,
                                 std::string& localPath, std::string& error) {
    localPath.clear();
    error.clear();
    // A cancel that arrived between two operations must not kill this one (the
    // same rule as install()).
    cancel_.store(false, std::memory_order_relaxed);

    // Everything below runs BEFORE any socket or any file.
    if (!isHttpsUrl(s.url)) {
        error = "refusing a non-https picture URL: \"" + s.url + "\"";
        return false;
    }
    if (!isWellFormedSha256(s.sha256)) {
        error = "the picture's sha256 must be 64 hexadecimal digits";
        return false;
    }
    if (pluginsDir.empty()) {
        error = "no plugins directory to keep the picture in";
        return false;
    }
    const std::string expected = toLowerAscii(s.sha256);
    const fs::path target(screenshotCachePath(pluginsDir, expected));
    std::error_code ec;

    // THE CACHE FIRST, and a cached file is believed only after it is hashed: a
    // file that is not the picture its name says (damaged, or put there) is
    // deleted and fetched again, and a good one costs no request at all.
    if (fs::is_regular_file(target, ec)) {
        std::string actual;
        std::string hashError;
        if (sha256File(target.string(), actual, hashError) && sha256Matches(expected, actual)) {
            localPath = target.string();
            return true;
        }
        fs::remove(target, ec);
    }

    fs::create_directories(target.parent_path(), ec);
    if (!fs::is_directory(target.parent_path(), ec)) {
        error = "cannot create the picture cache \"" + target.parent_path().string() + "\"";
        return false;
    }
    fs::path part = target;
    part += ".part";
    fs::remove(part, ec);  // debris from a killed earlier run

    std::string actual;
    bool ok = false;
    {
        Sha256 hasher;
        if (!hasher.init(error)) { return false; }
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create the temporary file \"" + part.string() + "\"";
            return false;
        }
        std::uint64_t written = 0;
        bool overCap = false;
        std::string hashError;
        // RULE 4, for a picture: the cap is checked against what ARRIVES, and the
        // sink refuses the chunk that would cross it - so a transport that does not
        // count (a test's) and one that does end in the same place.
        const auto sink = [&](const void* buf, std::size_t n) {
            if (written + n > kMaxScreenshotBytes) {
                overCap = true;
                return false;
            }
            if (!hasher.update(buf, n, hashError)) { return false; }
            out.write(static_cast<const char*>(buf), static_cast<std::streamsize>(n));
            written += n;
            return static_cast<bool>(out);
        };
        ok = httpsGet(s.url, kMaxScreenshotBytes, sink, nullptr, &cancel_, error,
                      HttpTimeouts{/*connectMs=*/4000, /*sendMs=*/4000, /*receiveMs=*/15000});
        out.flush();
        const bool diskOk = static_cast<bool>(out);
        out.close();
        if (overCap) {
            ok = false;
            error = "the picture is larger than " + std::to_string(kMaxScreenshotBytes / 1024ull / 1024ull) +
                    " MiB and was refused";
        } else if (!ok && !hashError.empty()) {
            error = hashError;
        } else if (ok && !diskOk) {
            ok = false;
            error = "writing \"" + part.string() + "\" failed";
        }
        if (ok && !hasher.finishHex(actual, error)) { ok = false; }
    }
    if (!ok) {
        fs::remove(part, ec);
        return false;
    }
    if (!sha256Matches(expected, actual)) {
        fs::remove(part, ec);
        error = "the picture failed its integrity check and was discarded (expected " + expected +
                ", got " + actual + ")";
        return false;
    }
    fs::rename(part, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(part, ignored);
        error = "cannot move the verified picture into place at \"" + target.string() +
                "\": " + ec.message();
        return false;
    }
    localPath = target.string();
    return true;
}

std::vector<std::string> PluginRepo::pruneScreenshotCache(
    const std::string& pluginsDir, const std::vector<PluginCatalogEntry>& catalogue) {
    std::vector<std::string> removed;
    std::vector<std::string> wanted;  // "<sha>.png", lower case
    for (const PluginCatalogEntry& e : catalogue) {
        for (const CatalogScreenshot& s : e.screenshots) {
            if (isWellFormedSha256(s.sha256)) { wanted.push_back(toLowerAscii(s.sha256) + ".png"); }
        }
    }
    const fs::path dir = fs::path(pluginsDir) / screenshotCacheDirName();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { return removed; }

    std::vector<fs::path> doomed;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) { continue; }
        const std::string name = it->path().filename().string();
        const bool isPicture = name.size() == 64 + 4 && name.compare(64, 4, ".png") == 0 &&
                               isWellFormedSha256(name.substr(0, 64));
        const bool isDebris = name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0;
        if (!isPicture && !isDebris) { continue; }  // not ours: left alone
        if (isPicture &&
            std::find(wanted.begin(), wanted.end(), toLowerAscii(name)) != wanted.end()) {
            continue;
        }
        doomed.push_back(it->path());
    }
    for (const fs::path& p : doomed) {
        std::error_code rec;
        if (fs::remove(p, rec) && !rec) { removed.push_back(p.filename().string()); }
    }
    std::sort(removed.begin(), removed.end());
    return removed;
}

bool PluginRepo::install(const PluginCatalogEntry& e, const std::string& pluginsDir,
                         std::string& installedPath, std::string& error) {
    // THE ANONYMOUS COUNT OF A FAILED INSTALL (0.99.64, core/health_events.hpp):
    // one of network, hash, write, other - never the entry, the file or the
    // message. Every way in (the store's install, an update) comes through here.
    // A cancelled transfer is not a failed install.
    health::InstallClass failClass = health::InstallClass::Other;
    const bool ok = installImpl(e, pluginsDir, installedPath, error, failClass);
    if (!ok && error != "cancelled") { health::notePluginInstallFailed(failClass); }
    return ok;
}

bool PluginRepo::installImpl(const PluginCatalogEntry& e, const std::string& pluginsDir,
                             std::string& installedPath, std::string& error,
                             health::InstallClass& failClass) {
    installedPath.clear();
    error.clear();
    progress_.store(0.0f, std::memory_order_relaxed);
    // A cancel that arrived between two operations must not kill this one.
    cancel_.store(false, std::memory_order_relaxed);

    // ---- Everything below this line runs BEFORE any filesystem or network
    // activity, so a refused entry leaves no trace at all. ------------------

    // RULE 5: derived from abiVersion, not from the `compatible` flag, which
    // a hand-built struct could have set to anything.
    if (e.abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
        error = "\"" + e.name + "\" is built for plugin ABI " + std::to_string(e.abiVersion) +
                "; this build of cascade requires exactly " +
                std::to_string(CASCADE_PLUGIN_ABI_VERSION);
        return false;
    }
    const PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) {
        error = "\"" + e.name + "\" has no build for " + hostOs() + "/" + hostArch();
        return false;
    }
    // RULE 6.
    std::string safeName;
    if (!sanitiseFileName(p->file, safeName, error)) {
        error = "\"" + e.name + "\": " + error;
        return false;
    }
    // RULE 2, format half. Re-checked here because install() may be handed an
    // entry that did not come from parseIndex().
    if (!isWellFormedSha256(p->sha256)) {
        error = "\"" + e.name + "\": sha256 must be 64 hexadecimal digits";
        return false;
    }
    // RULE 1.
    if (!isHttpsUrl(p->url)) {
        error = "\"" + e.name + "\": refusing a non-https download URL: \"" + p->url + "\"";
        return false;
    }
    // REGIONAL CATALOGUE, second enforcement (see the header block). Still
    // before any socket: this function does not trust its caller to have
    // gone through mergeRegional(), which is the only place `regional` is
    // meant to be set true.
    if (e.regional && !isRegionalDownloadUrl(p->url, regionalDownloadPrefix())) {
        error = "\"" + e.name + "\" is a regional catalogue entry and may only download from " +
                regionalDownloadPrefix();
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(pluginsDir), ec);
    if (!fs::is_directory(fs::path(pluginsDir), ec)) {
        error = "cannot create the plugins directory \"" + pluginsDir + "\"";
        failClass = health::InstallClass::Write;
        return false;
    }

    const fs::path target = fs::path(pluginsDir) / safeName;
    // The temp file sits in the DESTINATION directory so the final move is a
    // same-volume rename (a cross-volume "rename" degrades to copy+delete,
    // reopening exactly the partial-file window this design closes). The
    // ".part" suffix also keeps PluginHost's ".dll" scan from ever seeing it.
#ifdef _WIN32
    const int downloadPid = _getpid();
#else
    const int downloadPid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(downloadPid) + ".part";
    std::error_code ignored;
    fs::remove(tmp, ignored);  // debris from a killed earlier run

    const std::string expected = toLowerAscii(p->sha256);
    std::string actual;
    bool ok = false;
    {
        Sha256 hasher;
        if (!hasher.init(error)) {
            return false;
        }
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create the temporary file \"" + tmp.string() + "\"";
            failClass = health::InstallClass::Write;
            return false;
        }
        std::string hashError;
        const auto sink = [&hasher, &out, &hashError](const void* buf, std::size_t n) {
            if (!hasher.update(buf, n, hashError)) {
                return false;
            }
            out.write(static_cast<const char*>(buf), static_cast<std::streamsize>(n));
            return static_cast<bool>(out);
        };
        // RULE 4: the cap is the module's, never anything sizeBytes claimed.
        ok = httpsGet(p->url, kMaxPluginBytes, sink, &progress_, &cancel_, error);
        out.flush();
        // A transfer that failed is the network's, unless the disk refused a
        // write on the way (which also ends the transfer, from inside the sink).
        if (!ok) { failClass = out ? health::InstallClass::Net : health::InstallClass::Write; }
        if (ok && !out) {
            ok = false;
            error = "writing \"" + tmp.string() + "\" failed";
            failClass = health::InstallClass::Write;
        }
        out.close();
        if (ok && !hasher.finishHex(actual, error)) {
            ok = false;
        }
    }
    if (!ok) {
        fs::remove(tmp, ignored);
        return false;
    }

    // RULE 2, the half that matters. Nothing has entered the plugins
    // directory under its real name yet; only an exact match lets it.
    if (!sha256Matches(expected, actual)) {
        fs::remove(tmp, ignored);
        error = "\"" + e.name + "\" failed its integrity check and was discarded (expected " +
                expected + ", got " + actual + ")";
        failClass = health::InstallClass::Hash;
        return false;
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Most likely the DLL is currently loaded by this very process.
        fs::remove(tmp, ignored);
        error = "cannot move the verified plugin into place at \"" + target.string() +
                "\": " + ec.message();
        failClass = health::InstallClass::Write;
        return false;
    }

    progress_.store(1.0f, std::memory_order_relaxed);
    installedPath = target.string();
    return true;
}

bool PluginRepo::recordInstall(const std::string& pluginsDir, const PluginCatalogEntry& e,
                               std::string& error) {
    error.clear();
    if (e.id.empty()) {
        error = "cannot record an install for a catalogue entry with no id";
        return false;
    }
    const PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) {
        error = "\"" + e.name + "\" has no build for " + hostOs() + "/" + hostArch();
        return false;
    }
    std::string safeName;
    if (!sanitiseFileName(p->file, safeName, error)) {
        error = "\"" + e.name + "\": " + error;
        return false;
    }

    PluginInventory inv;
    std::string loadError;
    (void)loadInventory(pluginsDir, inv, loadError);  // corrupt or absent: start fresh

    InstalledPlugin r;
    r.id = e.id;
    r.name = e.name;
    r.version = e.version;
    r.file = safeName;
    // The catalogue's digest, which install() has already proved equal to the
    // bytes on disk. Re-hashing here would compare the file against itself.
    r.sha256 = toLowerAscii(p->sha256);
    r.abiVersion = e.abiVersion;
    r.installedAtUnix = nowUnix();

    bool replaced = false;
    for (InstalledPlugin& existing : inv.plugins) {
        if (existing.id == r.id) {
            existing = r;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        inv.plugins.push_back(r);
    }
    // The policy that came with this entry is cached in the same write: the
    // floor must be remembered from the moment the catalogue was seen, not
    // from some later fetch that may never happen.
    const std::vector<PluginCatalogEntry> one{e};
    mergePolicies(inv.policies, one);
    return saveManifest(pluginsDir, inv.plugins, inv.policies, error);
}

bool PluginRepo::applyUpdate(const PluginUpdate& u, const std::string& pluginsDir,
                             std::string& installedPath, std::string& error) {
    installedPath.clear();
    error.clear();
    if (u.entry == nullptr) {
        error = "update plan for \"" + u.id + "\" has no catalogue entry";
        return false;
    }
    // The whole gauntlet, unchanged and unshortcut: ABI, platform, filename
    // sanitiser, sha256 format, https, then download-to-temp, hash-while
    // -streaming, verify, and only then rename over the installed file. If any
    // of that fails, the old plugin is still sitting there untouched.
    if (!install(*u.entry, pluginsDir, installedPath, error)) {
        return false;
    }
    // Only now, with a verified file in place, is the record changed.
    std::string recordError;
    if (!recordInstall(pluginsDir, *u.entry, recordError)) {
        error = "\"" + u.entry->name + "\" was updated to " + u.toVersion +
                ", but its record could not be written: " + recordError +
                ". Running the update again will repair it.";
        return false;
    }
    return true;
}

namespace {

// THE RECORD GOES WITH THE FILE (2026-09-23). A plugin removed from disk kept
// its manifest record, and for a DISABLED plugin that record is the row: the
// rescan found it, still below its floor, and drew the same "disabled" row the
// user had just pressed Remove on. So a removal also drops every record naming
// that file. Cached floors are catalogue knowledge, not install records, and
// are kept. No manifest, or one that cannot be read, is left exactly as it is:
// the file is already gone, and a manifest this code cannot parse is not one
// it should rewrite.
bool forgetFile(const std::string& pluginsDir, const std::string& fileName, std::string& error) {
    const fs::path mpath(PluginRepo::manifestPath(pluginsDir));
    std::error_code ec;
    if (!fs::exists(mpath, ec)) { return true; }
    std::string text;
    if (readTextFile(mpath, text) != ReadResult::Ok) { return true; }
    std::vector<InstalledPlugin> plugins;
    std::vector<CachedPolicy> policies;
    std::vector<std::string> notes;
    std::string perr;
    if (!PluginRepo::parseManifest(text, plugins, policies, notes, perr)) { return true; }
    const std::size_t before = plugins.size();
    plugins.erase(std::remove_if(plugins.begin(), plugins.end(),
                                 [&](const InstalledPlugin& p) {
                                     return iequalsAscii(p.file, fileName);
                                 }),
                  plugins.end());
    if (plugins.size() == before) { return true; }
    std::string werr;
    if (!PluginRepo::saveManifest(pluginsDir, plugins, policies, werr)) {
        error = "the file was deleted, but the plugin record could not be updated: " + werr;
        return false;
    }
    return true;
}

}  // namespace

bool PluginRepo::remove(const std::string& pluginsDir, const std::string& fileName,
                        std::string& error) {
    error.clear();
    std::string safeName;
    if (!sanitiseFileName(fileName, safeName, error)) {
        return false;
    }
    const fs::path target = fs::path(pluginsDir) / safeName;
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) {
        error = "\"" + target.string() + "\" is not an installed plugin file";
        return false;
    }
    if (!fs::remove(target, ec) || ec) {
        error = "cannot delete \"" + target.string() + "\": " +
                (ec ? ec.message() : std::string("the file is probably in use"));
        return false;
    }
    return forgetFile(pluginsDir, safeName, error);
}

bool PluginRepo::removeQuarantined(const std::string& pluginsDir, const std::string& fileName,
                                   const std::string& suffix, std::string& error) {
    error.clear();

    // The REAL name goes through the unmodified sanitiser first. Everything
    // that check refuses for an install - traversal, drive letters, device
    // names, a non-.dll ending - is refused here for exactly the same reasons.
    std::string safeName;
    if (!sanitiseFileName(fileName, safeName, error)) {
        return false;
    }

    // The suffix is appended only after that, and is constrained in its own
    // right: a caller that could pass "/../../x" or a second path component
    // here would have defeated the check above by the back door. It is a
    // fixed constant in the one caller that exists; validating it anyway costs
    // nothing and means a future caller cannot get it wrong quietly.
    if (suffix.empty() || suffix.front() != '.' || suffix.size() > 16) {
        error = "quarantine suffix must be a short extension beginning with '.'";
        return false;
    }
    for (char c : suffix) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.';
        if (!ok) {
            error = "quarantine suffix contains a character outside [A-Za-z0-9.]";
            return false;
        }
    }
    if (suffix.find("..") != std::string::npos) {
        error = "quarantine suffix contains \"..\"";
        return false;
    }

    const fs::path target = fs::path(pluginsDir) / (safeName + suffix);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) {
        error = "\"" + target.string() + "\" is not a disabled plugin file";
        return false;
    }
    if (!fs::remove(target, ec) || ec) {
        error = "cannot delete \"" + target.string() + "\": " +
                (ec ? ec.message() : std::string("the file is probably in use"));
        return false;
    }
    return forgetFile(pluginsDir, safeName, error);
}

}  // namespace cascade::core
