// Implementation of core/plugin_repo.hpp. The header carries the security
// contract - the seven rules - and this file is arranged to match it:
// portable validation first (scheme, sha256 format, filename), then the CNG
// hash, then the WinHTTP transport, then the two public operations that
// compose them.
//
// SPDX-License-Identifier: MIT
#include "core/plugin_repo.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
bool httpsGet(const std::string& url, std::uint64_t maxBytes,
              const std::function<bool(const void*, std::size_t)>& sink,
              std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error) {
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
    // the GUI thread that called it.
    ::WinHttpSetTimeouts(session.h, 10000, 10000, 20000, 30000);

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

#endif  // _WIN32

}  // namespace

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
    return "https://raw.githubusercontent.com/wonderingStars/sdr-minus-plus-plugins/master/"
           "index.json";
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
    // Must END in .dll: "evil.dll.exe" fails here, which is the whole point.
    if (raw.size() <= 4 || !iequalsAscii(raw.substr(raw.size() - 4), ".dll")) {
        error = "file name does not end in \".dll\": \"" + raw + "\"";
        return false;
    }
    // Windows reserved device names are reserved with ANY extension, so
    // "CON.dll" is not a file - creating it either fails oddly or talks to a
    // device. Refuse with a clear reason instead.
    const std::string stem = toLowerAscii(raw.substr(0, raw.size() - 4));
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
                            std::string& error) {
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
            !wantString(pj, "legalNotice", false, e.legalNotice, where, error)) {
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
#if defined(_WIN32)
    Sha256 h;
    if (!h.init(error)) {
        return false;
    }
    // A null pointer with n == 0 is legal for the caller; give CNG a valid
    // address anyway rather than relying on it ignoring the pointer.
    static const unsigned char kEmpty = 0;
    if (!h.update(n == 0 ? &kEmpty : data, n, error)) {
        return false;
    }
    return h.finishHex(hexOut, error);
#else
    (void)data;
    (void)n;
    error = std::string("sha256: ") + "not supported on this platform";
    return false;
#endif
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
#if defined(_WIN32)
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
#else
    (void)path;
    error = std::string("sha256: ") + "not supported on this platform";
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Network operations
// ---------------------------------------------------------------------------

bool PluginRepo::fetchIndex(const std::string& url, std::string& error) {
    entries_.clear();
    error.clear();
    progress_.store(0.0f, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);

    // RULE 1, before any socket exists.
    if (!isHttpsUrl(url)) {
        error = "refusing a non-https catalogue URL: \"" + url + "\"";
        return false;
    }
#if defined(_WIN32)
    std::string body;
    body.reserve(64 * 1024);
    const auto sink = [&body](const void* p, std::size_t n) {
        body.append(static_cast<const char*>(p), n);
        return true;
    };
    if (!httpsGet(url, kMaxIndexBytes, sink, &progress_, &cancel_, error)) {
        return false;
    }
    progress_.store(1.0f, std::memory_order_relaxed);
    // A parse failure leaves entries_ empty, which fetchIndex documents.
    return parseIndex(body, entries_, error);
#else
    error = std::string("plugin catalogue: ") + "not supported on this platform";
    return false;
#endif
}

bool PluginRepo::install(const PluginCatalogEntry& e, const std::string& pluginsDir,
                         std::string& installedPath, std::string& error) {
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

#if defined(_WIN32)
    std::error_code ec;
    fs::create_directories(fs::path(pluginsDir), ec);
    if (!fs::is_directory(fs::path(pluginsDir), ec)) {
        error = "cannot create the plugins directory \"" + pluginsDir + "\"";
        return false;
    }

    const fs::path target = fs::path(pluginsDir) / safeName;
    // The temp file sits in the DESTINATION directory so the final move is a
    // same-volume rename (a cross-volume "rename" degrades to copy+delete,
    // reopening exactly the partial-file window this design closes). The
    // ".part" suffix also keeps PluginHost's ".dll" scan from ever seeing it.
    fs::path tmp = target;
    tmp += "." + std::to_string(_getpid()) + ".part";
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
        if (ok && !out) {
            ok = false;
            error = "writing \"" + tmp.string() + "\" failed";
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
        return false;
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Most likely the DLL is currently loaded by this very process.
        fs::remove(tmp, ignored);
        error = "cannot move the verified plugin into place at \"" + target.string() +
                "\": " + ec.message();
        return false;
    }

    progress_.store(1.0f, std::memory_order_relaxed);
    installedPath = target.string();
    return true;
#else
    (void)pluginsDir;
    error = std::string("plugin install: ") + "not supported on this platform";
    return false;
#endif
}

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
    return true;
}

}  // namespace cascade::core
