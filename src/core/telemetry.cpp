// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/telemetry.hpp"

#include "core/diag_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#elif defined(CASCADE_ANDROID)
// ANDROID-TODO(uploads-via-java). No OpenSSL on the NDK, so newInstallId()
// below draws from getrandom() directly (a raw kernel syscall, not a crypto
// library - see net/web_auth.cpp's randomBytes() for the same move) and
// postJson() further down is a no-op stub: no httplib, no
// CPPHTTPLIB_OPENSSL_SUPPORT (every TU including httplib.h must agree with
// net/web_server.cpp, which does not define it here).
//
// syscall(SYS_getrandom, ...), not the getrandom() libc wrapper: that
// wrapper is API 28+, below this build's android-26 floor (see
// net/web_auth.cpp's randomBytes() for the same fix against the same
// build failure).
#include <cstdlib>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <cstdlib>

#include <sys/utsname.h>

#include <openssl/rand.h>

// The TLS client for this platform - the same header, set up the same way,
// as plugin_repo.cpp's catalogue client and crash_upload.cpp's uploader:
// cpp-httplib is already vendored for the web server and OpenSSL is already
// linked here (see CMakeLists.txt) for RAND_bytes, so this transport adds no
// new third-party dependency.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

namespace cascade::core {

namespace {

// Keys worth keeping from a SoapySDR argument string. `driver` says which
// backend, `product`/`type` say which model. Everything else - serial, label
// (which embeds the serial), addr, uri - either identifies the individual
// unit or the network it is on.
bool interestingKey(const std::string& k) {
    return k == "driver" || k == "product" || k == "type";
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) { --b; }
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

}  // namespace

void SecondAccrual::reset(double now) {
    mark_ = now;
    started_ = true;
}

std::uint64_t SecondAccrual::advance(double now) {
    if (!started_ || now <= mark_) {
        // Never started, or the clock did not move forward. Re-mark and bank
        // nothing rather than let a backwards step produce a huge count.
        mark_ = now;
        started_ = true;
        return 0;
    }
    const double whole = std::floor(now - mark_);
    mark_ += whole;  // the sub-second remainder is CARRIED, not discarded
    return static_cast<std::uint64_t>(whole);
}

std::string sanitiseDevice(const std::string& soapyArgs) {
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= soapyArgs.size()) {
        const std::size_t comma = soapyArgs.find(',', at);
        const std::string field =
            soapyArgs.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos) {
            const std::string k = lower(trim(field.substr(0, eq)));
            const std::string v = trim(field.substr(eq + 1));
            // Bounded, and only ever from the allow list.
            if (interestingKey(k) && !v.empty() && v.size() <= 32) {
                const std::string lv = lower(v);
                if (std::find(parts.begin(), parts.end(), lv) == parts.end()) {
                    parts.push_back(lv);
                }
            }
        }
        if (comma == std::string::npos) { break; }
        at = comma + 1;
    }
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) { out += ' '; }
        out += p;
    }
    return out;
}

std::string newInstallId() {
    unsigned char bytes[16] = {0};
    // The system CSPRNG, not rand(): an id that could be predicted from the
    // launch time would let reports be correlated by someone who guessed it.
#if defined(_WIN32)
    if (::BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::string();
    }
#elif defined(CASCADE_ANDROID)
    if (::syscall(SYS_getrandom, bytes, sizeof(bytes), 0) !=
        static_cast<ssize_t>(sizeof(bytes))) {
        return std::string();
    }
#else
    if (::RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
        return std::string();
    }
#endif
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : bytes) {
        out += kHex[(b >> 4) & 0x0F];
        out += kHex[b & 0x0F];
    }
    return out;
}

bool validInstallId(const std::string& id) {
    if (id.size() != 32) { return false; }
    for (char c : id) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) { return false; }
    }
    return true;
}

std::string osDescription() {
#if defined(_WIN32)
    // The build number is the useful part (it distinguishes Windows 10 from
    // 11), and it is shared by millions of machines. RtlGetVersion rather
    // than GetVersionEx because the latter lies without a manifest.
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn != nullptr && fn(&vi) == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu",
                          static_cast<unsigned long>(vi.dwMajorVersion),
                          static_cast<unsigned long>(vi.dwMinorVersion),
                          static_cast<unsigned long>(vi.dwBuildNumber));
            return std::string(buf);
        }
    }
    return "Windows";
#else
    // Kernel name and release only ("Linux 6.8.0"). The distribution name is
    // deliberately not read from /etc/os-release: it is more identifying than
    // it is useful, and the point of this field is telling one broad platform
    // from another.
    struct utsname u {};
    if (::uname(&u) == 0) {
        return std::string(u.sysname) + " " + u.release;
    }
    return "unknown";
#endif
}

std::string archDescription() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string TelemetryReport::toJson() const {
    nlohmann::json j;
    j["id"] = installId;
    j["v"] = appVersion;
    j["os"] = os;
    j["arch"] = arch;
    j["launches"] = launches;
    j["crashes"] = crashes;
    j["sessionSec"] = session.seconds;
    j["sdr"] = session.sdrModel;
    nlohmann::json modes = nlohmann::json::object();
    for (const auto& kv : session.modeSeconds) {
        modes[kv.first] = kv.second;
    }
    j["modes"] = std::move(modes);
    j["panels"] = session.panels;
    j["plugins"] = session.plugins;
    // replace, not throw: a plugin name is third-party text and must not be
    // able to make the report unserialisable (see web_server.cpp for the same
    // hazard taking down the whole browser UI).
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

namespace {

// The project's own domain rather than the workers.dev name behind it: this
// string is compiled into every shipped binary, so it has to be one that can
// be repointed later without orphaning copies already installed.
//
// Reporting is still OFF until the user turns it on - this only decides where
// a report would go, not whether one is sent.
constexpr char kDefaultEndpoint[] = "https://telemetry.foxsdr.com";

}  // namespace

std::string telemetryEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        return std::string(buf, n);
    }
#else
    // The same seam, read the POSIX way. getenv() returns nullptr when unset;
    // an empty value is treated as unset too, matching the Windows probe.
    const char* env = std::getenv("FOXSDR_TELEMETRY_URL");
    if (env != nullptr && env[0] != '\0') {
        return std::string(env);
    }
#endif
    return std::string(kDefaultEndpoint);
}

#if defined(_WIN32)
namespace {

// One HTTPS POST of a small JSON body. Certificate validation is left at
// WinHTTP's defaults - no security flags are ever relaxed here - and the
// timeouts are short because this runs on a thread the destructor joins.
void postJson(const std::string& url, const std::string& json) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    const std::wstring wurl(url.begin(), url.end());
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { return; }
    // https only: a usage report is not secret, but sending it in clear would
    // put an install id on the wire for any network in between to collect.
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { return; }

    HINTERNET ses = ::WinHttpOpen(L"FoxSDR-usage/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) { return; }
    ::WinHttpSetTimeouts(ses, 4000, 4000, 6000, 6000);
    HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
    if (con != nullptr) {
        HINTERNET req = ::WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req != nullptr) {
            const wchar_t* kType = L"Content-Type: application/json\r\n";
            ::WinHttpSendRequest(req, kType, static_cast<DWORD>(-1),
                                 const_cast<char*>(json.data()),
                                 static_cast<DWORD>(json.size()),
                                 static_cast<DWORD>(json.size()), 0);
            // The response is not read and not acted on. There is nothing the
            // server could say that this client should obey - no config, no
            // commands, no identifiers - and not reading it is the simplest
            // way to guarantee that stays true.
            ::WinHttpReceiveResponse(req, nullptr);
            ::WinHttpCloseHandle(req);
        }
        ::WinHttpCloseHandle(con);
    }
    ::WinHttpCloseHandle(ses);
}

}  // namespace
#elif defined(CASCADE_ANDROID)
namespace {

// ANDROID-TODO(uploads-via-java): see the comment on the CASCADE_ANDROID
// #include block near the top of this file. Usage reports are silently
// dropped here, same as a network that black-holes them (see the comment
// above HeartbeatSender::beat's call site) - the schedule still advances, so
// this never becomes a backlog of threads.
void postJson(const std::string& url, const std::string& json) {
    (void)json;
    diagWarnf("telemetry: HTTPS is not available on this platform "
              "(ANDROID-TODO(uploads-via-java)); dropping report to %s",
              url.c_str());
}

}  // namespace
#else
namespace {

// One HTTPS POST of a small JSON body, mirroring the WinHTTP version above:
// certificate verification is left at its default (ON - not relaxed here,
// see enable_server_certificate_verification below), https only - unlike the
// crash uploader there is no loopback exception, matching the WinHTTP
// branch's own refusal of any non-https scheme - and the timeouts are short
// because this runs on a thread the destructor joins.
void postJson(const std::string& url, const std::string& json) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) { return; }
    const std::string scheme = url.substr(0, schemeEnd);
    // https only: a usage report is not secret, but sending it in clear
    // would put an install id on the wire for any network in between to
    // collect. FOXSDR_TELEMETRY_URL pointed at a plain http black hole (see
    // installer/msix/README.md) is silenced by this check alone - nothing is
    // ever connected to.
    if (scheme != "https") { return; }

    const std::string rest = url.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    const std::string target = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (authority.empty()) { return; }

    httplib::Client cli(std::string("https://") + authority);
    if (!cli.is_valid()) { return; }
    cli.enable_server_certificate_verification(true);
    cli.set_follow_location(false);
    cli.set_connection_timeout(4, 0);
    cli.set_read_timeout(6, 0);
    cli.set_write_timeout(6, 0);
    // The response is not read and not acted on. There is nothing the server
    // could say that this client should obey - no config, no commands, no
    // identifiers - and not reading it is the simplest way to guarantee that
    // stays true.
    cli.Post(target, json, "application/json");
}

}  // namespace
#endif

TelemetryReporter::~TelemetryReporter() {
    if (thread_.joinable()) { thread_.join(); }
}

bool TelemetryReporter::busy() const { return thread_.joinable(); }

void TelemetryReporter::send(const std::string& url, const std::string& json) {
    if (url.empty() || json.empty() || thread_.joinable()) { return; }
#if defined(_WIN32)
    thread_ = std::thread([url, json]() {
        try {
            postJson(url, json);
        } catch (...) {
            // Silent by design: see the header.
        }
    });
#else
    thread_ = std::thread([url, json]() {
        try {
            postJson(url, json);
        } catch (...) {
            // Silent by design: see the header.
        }
    });
#endif
}

// ---------------------------------------------------------------------------
// Heartbeats
// ---------------------------------------------------------------------------

HeartbeatSender::~HeartbeatSender() {
    if (thread_.joinable()) { thread_.join(); }
}

void HeartbeatSender::configure(const std::string& url, const std::string& installId,
                                const std::string& appVersion, std::uint64_t intervalSec) {
    url_ = url;
    id_ = installId;
    v_ = appVersion;
    interval_ = intervalSec == 0 ? 1 : intervalSec;
    // validInstallId, not just non-empty: an opt-out clears the id, and this
    // refusing anything but a real id is what makes "off means off" hold for
    // beats without a separate flag to keep in sync.
    configured_ = !url_.empty() && validInstallId(id_);
    firstSent_ = false;
    nextAt_ = 0.0;
}

std::string HeartbeatSender::beatJson(const std::string& id, const std::string& v) {
    nlohmann::json j;
    j["id"] = id;
    j["v"] = v;
    j["beat"] = 1;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool HeartbeatSender::due(double now) const {
    if (!configured_) { return false; }
    if (!firstSent_) { return true; }
    return now >= nextAt_;
}

void HeartbeatSender::poll(double now) {
    if (!due(now)) {
        // Reap a finished beat thread between beats, so the next one can
        // start without the join landing on the beat that is due.
        if (thread_.joinable() && done_.load()) { thread_.join(); }
        return;
    }
    // The schedule advances whether or not this beat can be sent: a network
    // that black-holes produces MISSING beats, never a backlog of threads.
    firstSent_ = true;
    nextAt_ = now + static_cast<double>(interval_);
    if (thread_.joinable()) {
        if (!done_.load()) { return; }  // previous beat still in flight: skip
        thread_.join();
    }
#if defined(_WIN32)
    done_.store(false);
    const std::string url = url_;
    const std::string json = beatJson(id_, v_);
    std::atomic<bool>* done = &done_;
    thread_ = std::thread([url, json, done]() {
        try {
            postJson(url, json);
        } catch (...) {
            // Silent by design, same as the reporter.
        }
        done->store(true);
    });
#else
    done_.store(false);
    const std::string url = url_;
    const std::string json = beatJson(id_, v_);
    std::atomic<bool>* done = &done_;
    thread_ = std::thread([url, json, done]() {
        try {
            postJson(url, json);
        } catch (...) {
            // Silent by design, same as the reporter.
        }
        done->store(true);
    });
#endif
}

}  // namespace cascade::core
