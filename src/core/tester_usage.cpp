// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tester_usage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

std::string rfc3339Utc(std::time_t t) {
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

bool validTesterToken(const std::string& token) {
    if (token.size() != 32) { return false; }
    for (char c : token) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) { return false; }
    }
    return true;
}

std::string extractTesterToken(const std::string& input) {
    const std::string t = trim(input);
    if (t.empty()) { return std::string(); }

    // THE BARE TOKEN. Checked first and checked exactly - a 32-character
    // string that happens to look like a URL fragment (it cannot: the
    // alphabet is 0-9a-f only) would otherwise fall through to the parser
    // below for no reason.
    if (validTesterToken(t)) { return t; }

    // THE PRIVATE PORTAL LINK: https://foxsdr.com/#t=<token> (web/portal.js's
    // manageLink, "/#t=" + token). The token lives in the fragment, so this
    // is a plain string search rather than a URL parse - there is no server
    // this could leak to at this point, the string never left the clipboard.
    //
    // "#c=" (confirmLink, a single-use confirmation secret) is deliberately
    // NOT accepted here: it opens nothing this field is for, and beta.go
    // keeps the two in separate indexes precisely so one can never be
    // presented in place of the other.
    const std::size_t hash = t.find('#');
    if (hash == std::string::npos) { return std::string(); }
    std::string frag = t.substr(hash + 1);
    // The fragment may carry more than one key ("t=...&x=...", though the
    // site never emits that); split on '&' and look for "t=" among the
    // parts, the same way URLSearchParams would.
    std::size_t at = 0;
    while (at <= frag.size()) {
        const std::size_t amp = frag.find('&', at);
        const std::string part =
            frag.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.size() > 2 && part[0] == 't' && part[1] == '=') {
            const std::string candidate = trim(part.substr(2));
            if (validTesterToken(candidate)) { return candidate; }
        }
        if (amp == std::string::npos) { break; }
        at = amp + 1;
    }
    return std::string();
}

std::string testerUsagePlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#else
    return "linux";
#endif
}

namespace {
constexpr char kDefaultTesterUsageEndpoint[] = "https://foxsdr.com/api/tester-usage";
}  // namespace

std::string testerUsageEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* env = std::getenv("FOXSDR_TESTER_USAGE_URL");
    if (env != nullptr && env[0] != '\0') { return std::string(env); }
#endif
    return std::string(kDefaultTesterUsageEndpoint);
}

std::string TesterUsageReport::toJson() const {
    nlohmann::json j;
    j["token"] = token;
    j["version"] = version;
    j["platform"] = platform;
    j["arch"] = arch;
    nlohmann::json s;
    s["start"] = session.start;
    s["minutes"] = session.minutes;
    j["session"] = std::move(s);
    j["features"] = session.features;
    nlohmann::json plugins = nlohmann::json::array();
    for (const TesterUsagePlugin& p : session.plugins) {
        nlohmann::json e;
        e["id"] = p.id;
        e["version"] = p.version;
        e["minutes"] = p.minutes;
        plugins.push_back(std::move(e));
    }
    j["plugins"] = std::move(plugins);
    j["radios"] = session.radios;
    // replace, not throw: a plugin id/version or a hand-typed field is
    // third-party or user text, and a bad byte in one must cost one
    // replacement character rather than an exception out of a save/send path
    // (tests/test_json_dump_policy.cpp holds every dump() in this tree to
    // this rule).
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

#if defined(_WIN32)
namespace {

TesterUsageOutcome postJsonStatus(const std::string& url, const std::string& json,
                                   int connectTimeoutMs, int rwTimeoutMs) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    const std::wstring wurl(url.begin(), url.end());
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { return TesterUsageOutcome::Retry; }
    // https only, for the reason the header states: a tester's token is a
    // credential, not an anonymous counter.
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { return TesterUsageOutcome::Retry; }

    HINTERNET ses = ::WinHttpOpen(L"FoxSDR-tester-usage/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) { return TesterUsageOutcome::Retry; }
    ::WinHttpSetTimeouts(ses, connectTimeoutMs, connectTimeoutMs, rwTimeoutMs, rwTimeoutMs);
    HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
    TesterUsageOutcome outcome = TesterUsageOutcome::Retry;
    if (con != nullptr) {
        HINTERNET req = ::WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req != nullptr) {
            const wchar_t* kType = L"Content-Type: application/json\r\n";
            const BOOL sent =
                ::WinHttpSendRequest(req, kType, static_cast<DWORD>(-1),
                                     const_cast<char*>(json.data()),
                                     static_cast<DWORD>(json.size()),
                                     static_cast<DWORD>(json.size()), 0);
            if (sent && ::WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0;
                DWORD size = sizeof(status);
                if (::WinHttpQueryHeaders(
                        req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
                    if (status >= 200 && status < 300) {
                        outcome = TesterUsageOutcome::Sent;
                    } else if (status == 401) {
                        outcome = TesterUsageOutcome::Invalid;
                    } else if (status == 400 || status == 413) {
                        outcome = TesterUsageOutcome::Rejected;
                    } else {
                        // 429 and anything else this contract has no name
                        // for: keep it, never treat an unrecognised code as
                        // permission to discard a report the site may yet
                        // accept.
                        outcome = TesterUsageOutcome::Retry;
                    }
                }
            }
            ::WinHttpCloseHandle(req);
        }
        ::WinHttpCloseHandle(con);
    }
    ::WinHttpCloseHandle(ses);
    return outcome;
}

}  // namespace
#else
namespace {

TesterUsageOutcome postJsonStatus(const std::string& url, const std::string& json,
                                   int connectTimeoutMs, int rwTimeoutMs) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) { return TesterUsageOutcome::Retry; }
    const std::string scheme = url.substr(0, schemeEnd);
    if (scheme != "https") { return TesterUsageOutcome::Retry; }

    const std::string rest = url.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    const std::string target = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (authority.empty()) { return TesterUsageOutcome::Retry; }

    httplib::Client cli(std::string("https://") + authority);
    if (!cli.is_valid()) { return TesterUsageOutcome::Retry; }
    cli.enable_server_certificate_verification(true);
    cli.set_follow_location(false);
    const int connSec = connectTimeoutMs / 1000;
    const int connUsec = (connectTimeoutMs % 1000) * 1000;
    const int rwSec = rwTimeoutMs / 1000;
    const int rwUsec = (rwTimeoutMs % 1000) * 1000;
    cli.set_connection_timeout(connSec, connUsec);
    cli.set_read_timeout(rwSec, rwUsec);
    cli.set_write_timeout(rwSec, rwUsec);

    const httplib::Result res = cli.Post(target, json, "application/json");
    if (!res) { return TesterUsageOutcome::Retry; }
    const int status = res->status;
    if (status >= 200 && status < 300) { return TesterUsageOutcome::Sent; }
    if (status == 401) { return TesterUsageOutcome::Invalid; }
    if (status == 400 || status == 413) { return TesterUsageOutcome::Rejected; }
    return TesterUsageOutcome::Retry;
}

}  // namespace
#endif

TesterUsageOutcome postTesterUsage(const std::string& url, const std::string& json,
                                    int connectTimeoutMs, int rwTimeoutMs) {
    if (url.empty() || json.empty()) { return TesterUsageOutcome::Retry; }
    try {
        return postJsonStatus(url, json, connectTimeoutMs, rwTimeoutMs);
    } catch (...) {
        // Silent by design, same rule as telemetry's transport: a usage
        // report that interrupted anything to complain would be worse than
        // one that quietly retries next launch.
        return TesterUsageOutcome::Retry;
    }
}

TesterUsageSender::~TesterUsageSender() {
    if (thread_.joinable()) { thread_.join(); }
}

bool TesterUsageSender::busy() const { return thread_.joinable() && !done_.load(); }
bool TesterUsageSender::finished() const { return thread_.joinable() && done_.load(); }

void TesterUsageSender::reap() {
    if (thread_.joinable() && done_.load()) { thread_.join(); }
}

void TesterUsageSender::send(const std::string& url, const std::string& json,
                              int connectTimeoutMs, int rwTimeoutMs,
                              std::function<void(TesterUsageOutcome)> onDone) {
    if (url.empty() || json.empty()) { return; }
    if (thread_.joinable() && !done_.load()) { return; }  // already sending
    reap();
    done_.store(false);
    std::atomic<bool>* done = &done_;
    thread_ = std::thread([url, json, connectTimeoutMs, rwTimeoutMs, onDone, done]() {
        TesterUsageOutcome outcome = TesterUsageOutcome::Retry;
        try {
            outcome = postTesterUsage(url, json, connectTimeoutMs, rwTimeoutMs);
        } catch (...) {
            // Silent by design; outcome stays Retry.
        }
        if (onDone) { onDone(outcome); }
        done->store(true);
    });
}

// ---------------------------------------------------------------------------
// The bounded queue
// ---------------------------------------------------------------------------

void TesterUsageQueue::push(const std::string& reportJson) {
    if (reportJson.empty()) { return; }
    items_.push_back(reportJson);
    while (items_.size() > kMax) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::removeFront() {
    if (!items_.empty()) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::setItems(std::vector<std::string> items) {
    if (items.size() > kMax) {
        items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() - kMax));
    }
    items_ = std::move(items);
}

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

void TesterUsageRecorder::noteFeature(const std::string& id) {
    if (!armed() || id.empty()) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    if (features_.size() >= kMaxFeatures) { return; }
    if (std::find(features_.begin(), features_.end(), id) == features_.end()) {
        features_.push_back(id);
    }
}

void TesterUsageRecorder::noteRadio(const std::string& kind) {
    if (!armed() || kind.empty()) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    if (radios_.size() >= kMaxRadios) { return; }
    if (std::find(radios_.begin(), radios_.end(), kind) == radios_.end()) {
        radios_.push_back(kind);
    }
}

void TesterUsageRecorder::accruePlugin(const std::string& id, const std::string& version,
                                        double seconds) {
    if (!armed() || id.empty() || !(seconds > 0.0)) { return; }
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(id);
    if (it == plugins_.end()) {
        if (plugins_.size() >= kMaxPlugins) { return; }
        it = plugins_.emplace(id, Accrual{}).first;
    }
    it->second.version = version;  // the most recently seen version wins
    it->second.seconds += seconds;
}

std::vector<std::string> TesterUsageRecorder::features() const {
    std::lock_guard<std::mutex> lk(mu_);
    return features_;
}

std::vector<std::string> TesterUsageRecorder::radios() const {
    std::lock_guard<std::mutex> lk(mu_);
    return radios_;
}

std::vector<TesterUsagePlugin> TesterUsageRecorder::plugins() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TesterUsagePlugin> out;
    out.reserve(plugins_.size());
    for (const auto& [id, acc] : plugins_) {
        TesterUsagePlugin p;
        p.id = id;
        p.version = acc.version;
        // Rounded to nearest, not truncated: a plugin fed for 89 seconds
        // reports 1 minute, not 0 - see the header's note on accruePlugin.
        p.minutes = static_cast<std::uint64_t>(std::llround(acc.seconds / 60.0));
        out.push_back(std::move(p));
    }
    return out;
}

void TesterUsageRecorder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    features_.clear();
    radios_.clear();
    plugins_.clear();
}

// ---------------------------------------------------------------------------
// Catalogue id lookup
// ---------------------------------------------------------------------------

std::string catalogueIdForPlugin(const std::string& fileName, const std::string& displayName,
                                  const std::vector<InstalledPlugin>& installed) {
    const std::string bare = fs::path(fileName).filename().string();
    for (const InstalledPlugin& ip : installed) {
        if (!bare.empty() && ip.file == bare) { return ip.id; }
    }
    // FALLBACK: a slug of the display name, for a plugin with no manifest
    // record (a side-loaded .dll/.so, or a manifest that failed to parse).
    // Best-effort rather than nothing - see the header.
    std::string slug;
    slug.reserve(displayName.size());
    bool lastDash = false;
    for (unsigned char c : displayName) {
        if (std::isalnum(c) != 0) {
            slug += static_cast<char>(std::tolower(c));
            lastDash = false;
        } else if (!lastDash && !slug.empty()) {
            slug += '-';
            lastDash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') { slug.pop_back(); }
    return slug;
}

}  // namespace cascade::core
