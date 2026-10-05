// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/telemetry.hpp"

#include "core/package_identity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#else
#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

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
    j["stalls"] = stalls;
    // Through the vocabulary a second time: whatever this member was filled
    // with, only legal tokens leave the machine.
    j["health"] = health::sanitise(health);
    j["ch"] = channel;
    j["first"] = firstRun;
    j["fv"] = firstVersion;
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
//
// Returns whether the server answered 2xx. Only the STATUS is read.
bool postJson(const std::string& url, const std::string& json) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    const std::wstring wurl(url.begin(), url.end());
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { return false; }
    // https only: a usage report is not secret, but sending it in clear would
    // put an install id on the wire for any network in between to collect.
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { return false; }

    bool accepted = false;
    HINTERNET ses = ::WinHttpOpen(L"FoxSDR-usage/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) { return false; }
    ::WinHttpSetTimeouts(ses, 4000, 4000, 6000, 6000);
    HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
    if (con != nullptr) {
        HINTERNET req = ::WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req != nullptr) {
            const wchar_t* kType = L"Content-Type: application/json\r\n";
            const BOOL sent = ::WinHttpSendRequest(req, kType, static_cast<DWORD>(-1),
                                                   const_cast<char*>(json.data()),
                                                   static_cast<DWORD>(json.size()),
                                                   static_cast<DWORD>(json.size()), 0);
            // The response BODY is not read and nothing in it is acted on.
            // There is nothing the server could say that this client should
            // obey - no config, no commands, no identifiers - and not reading
            // it is the simplest way to guarantee that stays true. The STATUS
            // is read, because a counter that only forgets itself once its
            // record was taken has to know whether it was (see StallLedger).
            if (sent && ::WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0;
                DWORD size = sizeof(status);
                if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                          WINHTTP_NO_HEADER_INDEX)) {
                    accepted = httpStatusAccepted(static_cast<int>(status));
                }
            }
            ::WinHttpCloseHandle(req);
        }
        ::WinHttpCloseHandle(con);
    }
    ::WinHttpCloseHandle(ses);
    return accepted;
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
//
// Returns whether the server answered 2xx. Only the STATUS is read.
bool postJson(const std::string& url, const std::string& json) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) { return false; }
    const std::string scheme = url.substr(0, schemeEnd);
    // https only: a usage report is not secret, but sending it in clear
    // would put an install id on the wire for any network in between to
    // collect. FOXSDR_TELEMETRY_URL pointed at a plain http black hole (see
    // installer/msix/README.md) is silenced by this check alone - nothing is
    // ever connected to.
    if (scheme != "https") { return false; }

    const std::string rest = url.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    const std::string target = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (authority.empty()) { return false; }

    httplib::Client cli(std::string("https://") + authority);
    if (!cli.is_valid()) { return false; }
    cli.enable_server_certificate_verification(true);
    cli.set_follow_location(false);
    cli.set_connection_timeout(4, 0);
    cli.set_read_timeout(6, 0);
    cli.set_write_timeout(6, 0);
    // The response BODY is not read and nothing in it is acted on. There is
    // nothing the server could say that this client should obey - no config,
    // no commands, no identifiers - and not reading it is the simplest way to
    // guarantee that stays true. The STATUS is read, because a counter that
    // only forgets itself once its record was taken has to know whether it
    // was (see StallLedger).
    const httplib::Result res = cli.Post(target, json, "application/json");
    if (!res) { return false; }
    return httpStatusAccepted(res->status);
}

}  // namespace
#endif

TelemetryReporter::~TelemetryReporter() {
    if (thread_.joinable()) { thread_.join(); }
}

bool TelemetryReporter::busy() const { return thread_.joinable(); }

bool httpStatusAccepted(int status) { return status >= 200 && status < 300; }

void TelemetryReporter::send(const std::string& url, const std::string& json, SendDone done) {
    // ONE body for every platform: the Windows (WinHTTP) and POSIX (httplib)
    // transports each define postJson above, and nothing about the thread or
    // the completion differs between them.
    sendVia([](const std::string& u, const std::string& j) { return postJson(u, j); }, url, json,
            std::move(done));
}

void TelemetryReporter::sendVia(Transport transport, const std::string& url,
                                const std::string& json, SendDone done) {
    if (url.empty() || json.empty() || !transport || thread_.joinable()) { return; }
    thread_ = std::thread([transport = std::move(transport), url, json,
                           done = std::move(done)]() {
        bool accepted = false;
        try {
            accepted = transport(url, json);
        } catch (...) {
            // Silent by design: see the header. A throw is a refusal.
        }
        if (done) {
            try {
                done(accepted);
            } catch (...) {
            }
        }
    });
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

namespace {
constexpr char kSentMarkerPrefix[] = "telemetry-sent-";
}  // namespace

std::string installChannel() {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return runningInPackage() ? "store" : "installer";
#else
    const char* appimage = std::getenv("APPIMAGE");
    return (appimage != nullptr && appimage[0] != '\0') ? "appimage" : "tarball";
#endif
}

std::string utcDateToday() {
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    if (::gmtime_s(&tmv, &now) != 0) { return std::string(); }
#else
    if (::gmtime_r(&now, &tmv) == nullptr) { return std::string(); }
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday);
    return std::string(buf);
}

bool validFirstRunDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) { continue; }
        if (s[i] < '0' || s[i] > '9') { return false; }
    }
    const int month = (s[5] - '0') * 10 + (s[6] - '0');
    const int day = (s[8] - '0') * 10 + (s[9] - '0');
    return s.compare(0, 4, "2026") >= 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool validFirstVersion(const std::string& s) {
    // A version string and nothing else: digits, letters, dots, dashes and
    // plus, at most 48 characters - the Worker's own width for a version.
    if (s.empty() || s.size() > 48) { return false; }
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+';
        if (!ok) { return false; }
    }
    return true;
}

std::string reportSendMarkerName(const std::string& json) {
    std::uint64_t h = 14695981039346656037ull;  // FNV-1a 64
    for (unsigned char c : json) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    return std::string(kSentMarkerPrefix) + hex;
}

bool claimReportSend(const std::string& dir, const std::string& json) {
    if (dir.empty() || json.empty()) { return true; }
    // UTF-8 in, whatever the platform wants out: a std::string path on Windows
    // would be read in the ANSI code page and miss a non-ASCII user folder.
    const std::filesystem::path folder(std::u8string(dir.begin(), dir.end()));
    const std::string name = reportSendMarkerName(json);
    const std::filesystem::path marker = folder / name;
#if defined(_WIN32)
    const HANDLE h = ::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return !(err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS);
    }
    ::CloseHandle(h);
#else
    const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { return errno != EEXIST; }
    ::close(fd);
#endif
    // Won. Older reports' markers are no longer needed: their reports can
    // never be pending again, because the config now holds this one or newer.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string other = it->path().filename().string();
        if (other != name && other.rfind(kSentMarkerPrefix, 0) == 0) {
            std::error_code rm;
            std::filesystem::remove(it->path(), rm);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Display stalls
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path utf8Path(const std::string& s) {
    // UTF-8 in, whatever the platform wants out: a std::string path on Windows
    // would be read in the ANSI code page and miss a non-ASCII user folder.
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace

std::string StallLedger::fileText(const std::string& installId, std::uint64_t count) {
    return installId + " " + std::to_string(count) + "\n";
}

bool StallLedger::parseFileText(const std::string& text, const std::string& installId,
                                std::uint64_t& count) {
    // "<32 hex> <digits>" and an optional line end, and nothing else: a file
    // that is not exactly that - hand-edited, truncated by a kill mid-write, or
    // left by an earlier identity - counts as no number rather than a guess.
    std::string s = text;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) { s.pop_back(); }
    if (s.size() < 34 || s[32] != ' ') { return false; }
    if (s.compare(0, 32, installId) != 0 || !validInstallId(installId)) { return false; }
    const std::string digits = s.substr(33);
    if (digits.empty() || digits.size() > 9) { return false; }
    std::uint64_t n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') { return false; }
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    count = n > kMaxCount ? kMaxCount : n;
    return true;
}

std::string StallLedger::pathIn(const std::string& configDir) {
    if (configDir.empty()) { return std::string(); }
    return configDir + "/" + kFileName;
}

void StallLedger::removeFile(const std::string& path) {
    if (path.empty()) { return; }
    std::error_code ec;
    std::filesystem::remove(utf8Path(path), ec);
}

void StallLedger::arm(const std::string& path, const std::string& installId, bool loadExisting) {
    if (!validInstallId(installId)) {
        disarm();
        return;
    }
    std::uint64_t n = 0;
    if (loadExisting && !path.empty()) {
        std::ifstream in(utf8Path(path), std::ios::binary);
        if (in) {
            // Bounded: a ledger is thirty-odd bytes, so anything bigger is not one.
            char buf[128];
            in.read(buf, sizeof buf);
            std::uint64_t parsed = 0;
            if (parseFileText(std::string(buf, static_cast<std::size_t>(in.gcount())), installId,
                              parsed)) {
                n = parsed;
            }
        }
    }
    auto t = std::make_shared<Target>();
    t->path = path;
    t->installId = installId;
    {
        std::lock_guard<std::mutex> lk(targetMutex_);
        target_ = std::move(t);
    }
    count_.store(n, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
}

void StallLedger::disarm() {
    std::shared_ptr<const Target> old;
    {
        std::lock_guard<std::mutex> lk(targetMutex_);
        old = std::move(target_);
        target_.reset();
    }
    armed_.store(false, std::memory_order_release);
    count_.store(0, std::memory_order_relaxed);
    if (old && !old->path.empty()) {
        // Off the caller's thread: this is the Settings switch, on the GUI
        // thread, and a disk that is slow or held by an antivirus must not be
        // able to stall the frame. The thread captures a copy of the path and
        // nothing else, so it is harmless if it outlives everything.
        std::thread([p = old->path]() { removeFile(p); }).detach();
    }
}

std::uint64_t StallLedger::count() const {
    return armed_.load(std::memory_order_acquire) ? count_.load(std::memory_order_relaxed) : 0;
}

void StallLedger::note() {
    if (!armed_.load(std::memory_order_acquire)) { return; }
    std::uint64_t c = count_.load(std::memory_order_relaxed);
    while (c < kMaxCount &&
           !count_.compare_exchange_weak(c, c + 1, std::memory_order_relaxed)) {
    }
    persist();
}

void StallLedger::settle(std::uint64_t carried) {
    if (carried == 0 || !armed_.load(std::memory_order_acquire)) { return; }
    std::uint64_t c = count_.load(std::memory_order_relaxed);
    while (!count_.compare_exchange_weak(c, c > carried ? c - carried : 0,
                                         std::memory_order_relaxed)) {
    }
    persist();
}

void StallLedger::persist() {
    std::shared_ptr<const Target> t;
    {
        std::lock_guard<std::mutex> lk(targetMutex_);
        t = target_;
    }
    if (!t || t->path.empty()) { return; }
    // The count is read AFTER the I/O lock is held, so whichever of the two
    // writing threads goes last writes the newest value and an older one can
    // never overwrite it.
    std::lock_guard<std::mutex> io(ioMutex_);
    if (!armed_.load(std::memory_order_acquire)) { return; }  // switched off meanwhile
    const std::uint64_t n = count_.load(std::memory_order_relaxed);
    const std::filesystem::path file = utf8Path(t->path);
    if (n == 0) {
        // Nothing to remember is no file at all: most installs never have one.
        std::error_code ec;
        std::filesystem::remove(file, ec);
        return;
    }
    // Temp file then rename, so a process ended mid-write leaves either the old
    // number or the new one and never half of one.
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { return; }
        const std::string text = fileText(t->installId, n);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); }
}

TelemetryReporter::SendDone settleOnAccept(std::shared_ptr<StallLedger> ledger,
                                           std::uint64_t carried) {
    return [ledger = std::move(ledger), carried](bool accepted) {
        if (accepted && ledger) { ledger->settle(carried); }
    };
}

bool prepareStartupRecord(const std::string& configDir, const std::string& pendingJson,
                          const StallLedger& ledger, std::string& outJson,
                          std::uint64_t& outCarried) {
    outJson.clear();
    outCarried = 0;
    if (pendingJson.empty()) { return false; }
    // The claim is made on the report AS STORED, never on what is sent: the
    // stall count can change between two launches (it is subtracted once a
    // record is accepted), and a marker named after the changed content would
    // let a launch that died before its first save send the same session a
    // second time.
    if (!configDir.empty() && !claimReportSend(configDir, pendingJson)) { return false; }
    outCarried = ledger.count();
    outJson = withStalls(pendingJson, outCarried);
    return true;
}

TelemetryReporter::SendDone settleOnAccept(std::shared_ptr<StallLedger> stalls,
                                           std::uint64_t carriedStalls,
                                           std::shared_ptr<health::HealthLedger> healthLedger,
                                           health::Counts carriedHealth) {
    return [stalls = std::move(stalls), carriedStalls, healthLedger = std::move(healthLedger),
            carriedHealth = std::move(carriedHealth)](bool accepted) {
        if (!accepted) { return; }
        if (stalls) { stalls->settle(carriedStalls); }
        if (healthLedger) { healthLedger->settle(carriedHealth); }
    };
}

bool prepareStartupRecord(const std::string& configDir, const std::string& pendingJson,
                          const StallLedger& stalls, const health::HealthLedger& healthLedger,
                          std::string& outJson, std::uint64_t& outCarriedStalls,
                          health::Counts& outCarriedHealth) {
    outCarriedHealth.clear();
    if (!prepareStartupRecord(configDir, pendingJson, stalls, outJson, outCarriedStalls)) {
        return false;
    }
    // EARLIER SESSIONS ONLY: this run's own counts describe the session still
    // in progress, whose record is sent at the next start-up.
    outCarriedHealth = healthLedger.priorCounts();
    outJson = health::withHealth(outJson, health::encode(outCarriedHealth));
    return true;
}

std::string withStalls(const std::string& recordJson, std::uint64_t stalls) {
    nlohmann::json j = nlohmann::json::parse(recordJson, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return recordJson; }
    j["stalls"] = stalls;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace cascade::core
