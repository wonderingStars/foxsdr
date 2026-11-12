// webhook.hpp - webhooks: on an alert or a decode event, an HTTP POST of JSON to a URL the user typed (row N21 of the gaps survey).
// Header-only and free of any HTTP library: the network is behind the small Transport interface below; net/webhook_http.hpp supplies
// the plain-HTTP transport on cpp-httplib. Not wired (see the record's WIRING HAND-OFF); OFF by default (an empty URL means disabled,
// no thread is started and the network is never touched).
//
// WHAT THE SENDER GUARANTEES
//  * enqueue() NEVER BLOCKS the caller on the network: it takes a mutex for a few instructions, appends to a bounded queue and
//    returns. The queue holds at most maxQueue (default 100) waiting events; when it is full the OLDEST waiting event is dropped
//    (counted) so the newest events are the ones that survive a long outage. One event is in flight at a time.
//  * One worker thread posts in order. A post is retried when it ends in a network error, a time-out, HTTP 408, 429 or any 5xx, up to
//    maxAttempts attempts (default 5), waiting backoffBaseMs * 2^(attempt-1) between attempts, never more than backoffMaxMs (default
//    1 s base, 60 s cap). Any other status (other 4xx, 3xx, 1xx) is a PERMANENT failure and is not retried. Redirects are NEVER followed
//    (a typed URL must not bounce a payload somewhere else). Success is a 2xx.
//  * Time-outs: connect (default 3 s) and read/write (default 5 s) are handed to the transport; a stalled receiver cannot hold the worker
//    longer than about their sum per attempt.
//  * stop() wakes a waiting back-off at once and joins; it can wait at most one transport attempt (the time-outs above).
//
// URLs: only http:// is accepted, and https:// ONLY if the transport says supportsHttps(). THE PROJECT ALREADY HAS TLS CLIENTS but none
// reusable from here: core/telemetry.cpp, core/crash_upload.cpp, core/feature_request.cpp and core/plugin_repo.cpp post over WinHTTP on
// Windows and over cpp-httplib built with CPPHTTPLIB_OPENSSL_SUPPORT elsewhere, each inside its own .cpp. This block adds no TLS; the
// wiring lane may implement Transport on top of that code (hand-off item). A URL with credentials ("user:pass@"), whitespace or control
// characters, a non-ASCII host, a port outside 1..65535, or more than 2048 bytes is refused, as is any other scheme (https without a
// TLS transport, ftp, file, javascript, data, ...).
//
// PAYLOAD: one JSON object per event (fields below), UTF-8, at most maxBodyBytes (default 16 KiB; the free-text fields are cut to fit,
// never the structure). Invalid UTF-8 in the text fields is replaced, so serialisation cannot throw.
//
// THREADS: enqueue() and the counters are callable from any thread. start()/stop() are control-thread calls.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace cascade::net::webhook {

struct ParsedUrl {
    std::string scheme;   // "http" or "https"
    std::string host;     // as typed (bracketed IPv6 without brackets)
    int port = 80;
    std::string path;     // starts with "/", includes the query
};

enum class UrlError { None, Empty, TooLong, BadScheme, HttpsUnsupported, NoHost, BadPort, HasUserinfo, BadChars, BadHost };

inline const char* urlErrorText(UrlError e) {
    switch (e) {
        case UrlError::None: return "ok";
        case UrlError::Empty: return "no URL";
        case UrlError::TooLong: return "URL too long";
        case UrlError::BadScheme: return "only http:// URLs are accepted";
        case UrlError::HttpsUnsupported: return "https:// is not supported by this build";
        case UrlError::NoHost: return "no host in the URL";
        case UrlError::BadPort: return "bad port";
        case UrlError::HasUserinfo: return "user names and passwords in the URL are not accepted";
        case UrlError::BadChars: return "the URL contains characters that are not allowed";
        case UrlError::BadHost: return "bad host name";
    }
    return "";
}

inline UrlError parseUrl(const std::string& url, bool allowHttps, ParsedUrl& out) {
    if (url.empty()) return UrlError::Empty;
    if (url.size() > 2048) return UrlError::TooLong;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '|' || c == '^')
            return UrlError::BadChars;
    std::string lower = url.substr(0, 8);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::size_t start;
    ParsedUrl p;
    if (lower.rfind("http://", 0) == 0) { p.scheme = "http"; p.port = 80; start = 7; }
    else if (lower.rfind("https://", 0) == 0) {
        if (!allowHttps) return UrlError::HttpsUnsupported;
        p.scheme = "https"; p.port = 443; start = 8;
    } else return UrlError::BadScheme;
    const std::size_t pathStart = url.find_first_of("/?#", start);
    const std::string authority = url.substr(start, pathStart == std::string::npos ? std::string::npos : pathStart - start);
    if (authority.find('@') != std::string::npos) return UrlError::HasUserinfo;
    if (authority.empty()) return UrlError::NoHost;
    std::string hostPart = authority, portPart;
    bool hasPort = false;
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return UrlError::BadHost;
        hostPart = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return UrlError::BadHost;
            portPart = authority.substr(close + 2);
            hasPort = true;
        }
        if (hostPart.empty()) return UrlError::NoHost;
        for (char c : hostPart)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return UrlError::BadHost;
    } else {
        const std::size_t colon = authority.find(':');
        if (colon != std::string::npos) { hostPart = authority.substr(0, colon); portPart = authority.substr(colon + 1); hasPort = true; }
        if (hostPart.empty()) return UrlError::NoHost;
        if (hostPart.size() > 253 || hostPart.front() == '.' || hostPart.front() == '-' || hostPart.back() == '-') return UrlError::BadHost;
        for (char c : hostPart)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.')) return UrlError::BadHost;
        if (hostPart.find("..") != std::string::npos) return UrlError::BadHost;
    }
    if (hasPort) {
        if (portPart.empty() || portPart.size() > 5) return UrlError::BadPort;
        int v = 0;
        for (char c : portPart) {
            if (c < '0' || c > '9') return UrlError::BadPort;
            v = v * 10 + (c - '0');
        }
        if (v < 1 || v > 65535) return UrlError::BadPort;
        p.port = v;
    }
    p.host = hostPart;
    p.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    if (p.path[0] != '/') p.path = "/" + p.path;     // "http://h?x=1" -> "/?x=1"
    const std::size_t hash = p.path.find('#');
    if (hash != std::string::npos) p.path.resize(hash);   // a fragment is never sent
    out = std::move(p);
    return UrlError::None;
}

// ---- the event --------------------------------------------------------------------------------------------------------------------------
struct WebhookEvent {
    std::string kind;        // "alert", "decode", "test", ...
    std::string title;
    std::string detail;
    double unixTime = 0;     // seconds
    double freqHz = 0;       // 0 = unknown
    std::string source;      // the decoder or rule name
};

namespace detail {

// Replaces invalid UTF-8 and control bytes (other than \n and \t) with '?', cut to maxBytes on a character boundary.
inline std::string cleanText(const std::string& in, std::size_t maxBytes) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size() && out.size() < maxBytes) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len != 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (ok && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;
        }
        if (ok && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
        }
        if (ok && len == 1 && ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)) ok = false;
        if (!ok) { out += '?'; ++i; continue; }
        if (out.size() + len > maxBytes) break;
        out.append(in, i, len);
        i += len;
    }
    return out;
}

}  // namespace detail

// The JSON payload. Free-text fields are cut so the whole body fits maxBodyBytes (the structure is never cut).
inline std::string eventToJson(const WebhookEvent& e, std::size_t maxBodyBytes = 16384) {
    auto build = [&](std::size_t detailMax) {
        nlohmann::json j;
        j["app"] = "FoxSDR";
        j["kind"] = detail::cleanText(e.kind, 64);
        j["title"] = detail::cleanText(e.title, 256);
        j["detail"] = detail::cleanText(e.detail, detailMax);
        j["time"] = std::isfinite(e.unixTime) ? e.unixTime : 0.0;
        j["freq_hz"] = std::isfinite(e.freqHz) ? e.freqHz : 0.0;
        j["source"] = detail::cleanText(e.source, 128);
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    };
    std::size_t detailMax = maxBodyBytes;
    std::string s = build(detailMax);
    while (s.size() > maxBodyBytes && detailMax > 0) {
        detailMax = detailMax > s.size() - maxBodyBytes ? detailMax - (s.size() - maxBodyBytes) - 1 : 0;   // escapes can double a byte: loop
        s = build(detailMax);
    }
    return s;
}

// ---- the transport ------------------------------------------------------------------------------------------------------------------------
struct PostResult {
    enum class Kind { Ok, HttpError, NetworkError, Timeout };
    Kind kind = Kind::NetworkError;
    int status = 0;   // HTTP status when one was received
    std::string error;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool supportsHttps() const { return false; }
    // One attempt. MUST NOT follow redirects, MUST honour both time-outs, MUST NOT throw.
    virtual PostResult post(const ParsedUrl& url, const std::string& body, const std::string& contentType, int connectTimeoutMs, int ioTimeoutMs) = 0;
};

struct Config {
    std::string url;                 // empty = disabled
    std::size_t maxQueue = 100;
    int maxAttempts = 5;
    int backoffBaseMs = 1000;
    int backoffMaxMs = 60000;
    int connectTimeoutMs = 3000;
    int ioTimeoutMs = 5000;
    std::size_t maxBodyBytes = 16384;
};

struct Stats {
    std::uint64_t enqueued = 0, delivered = 0, droppedOldest = 0, rejected = 0, retries = 0, gaveUp = 0, permanentFailures = 0;
    std::uint64_t attempts = 0, timeouts = 0, networkErrors = 0;
    std::size_t queued = 0;
    int lastStatus = 0;
};

enum class StartResult { Started, Disabled, BadUrl, HttpsUnsupported, AlreadyRunning };

inline bool retryable(const PostResult& r) {
    switch (r.kind) {
        case PostResult::Kind::Ok: return false;
        case PostResult::Kind::NetworkError:
        case PostResult::Kind::Timeout: return true;
        case PostResult::Kind::HttpError: return r.status == 408 || r.status == 429 || (r.status >= 500 && r.status <= 599);
    }
    return false;
}

class WebhookSender {
public:
    WebhookSender(Transport& t, Config c) : transport_(t), cfg_(sanitise(std::move(c))) {}
    ~WebhookSender() { stop(); }
    WebhookSender(const WebhookSender&) = delete;
    WebhookSender& operator=(const WebhookSender&) = delete;

    StartResult start(UrlError* why = nullptr) {
        std::lock_guard<std::mutex> lk(ctl_);
        if (running_) return StartResult::AlreadyRunning;
        if (cfg_.url.empty()) return StartResult::Disabled;
        const UrlError e = parseUrl(cfg_.url, transport_.supportsHttps(), url_);
        if (why) *why = e;
        if (e == UrlError::HttpsUnsupported) return StartResult::HttpsUnsupported;
        if (e != UrlError::None) return StartResult::BadUrl;
        {
            std::lock_guard<std::mutex> q(m_);
            stop_ = false;
            active_ = true;
        }
        running_ = true;
        worker_ = std::thread([this] { run(); });
        return StartResult::Started;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(ctl_);
        if (!running_) return;
        {
            std::lock_guard<std::mutex> q(m_);
            stop_ = true;
            active_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        running_ = false;
        std::lock_guard<std::mutex> q(m_);
        queue_.clear();
    }

    // Never blocks on the network. False when the sender is not running or the body is too large (counted as rejected).
    bool enqueueJson(std::string body) {
        std::lock_guard<std::mutex> q(m_);
        if (!active_) return false;
        if (body.size() > cfg_.maxBodyBytes) { ++stats_.rejected; return false; }
        if (queue_.size() >= cfg_.maxQueue) {
            queue_.pop_front();
            ++stats_.droppedOldest;
        }
        queue_.push_back(std::move(body));
        ++stats_.enqueued;
        cv_.notify_one();
        return true;
    }
    bool enqueue(const WebhookEvent& e) { return enqueueJson(eventToJson(e, cfg_.maxBodyBytes)); }

    Stats stats() const {
        std::lock_guard<std::mutex> q(m_);
        Stats s = stats_;
        s.queued = queue_.size();
        return s;
    }
    bool running() const { std::lock_guard<std::mutex> q(m_); return active_; }

private:
    static Config sanitise(Config c) {
        if (c.maxQueue < 1) c.maxQueue = 1;
        if (c.maxAttempts < 1) c.maxAttempts = 1;
        if (c.backoffBaseMs < 1) c.backoffBaseMs = 1;
        if (c.backoffMaxMs < c.backoffBaseMs) c.backoffMaxMs = c.backoffBaseMs;
        if (c.connectTimeoutMs < 100) c.connectTimeoutMs = 100;
        if (c.ioTimeoutMs < 100) c.ioTimeoutMs = 100;
        if (c.maxBodyBytes < 256) c.maxBodyBytes = 256;
        return c;
    }

    void run() {
        for (;;) {
            std::string body;
            {
                std::unique_lock<std::mutex> q(m_);
                cv_.wait(q, [&] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                body = std::move(queue_.front());
                queue_.pop_front();
            }
            deliver(body);
        }
    }

    void deliver(const std::string& body) {
        for (int attempt = 1; attempt <= cfg_.maxAttempts; ++attempt) {
            PostResult r = transport_.post(url_, body, "application/json", cfg_.connectTimeoutMs, cfg_.ioTimeoutMs);
            bool again = false;
            {
                std::lock_guard<std::mutex> q(m_);
                ++stats_.attempts;
                stats_.lastStatus = r.status;
                if (r.kind == PostResult::Kind::Ok) { ++stats_.delivered; return; }
                if (r.kind == PostResult::Kind::Timeout) ++stats_.timeouts;
                if (r.kind == PostResult::Kind::NetworkError) ++stats_.networkErrors;
                if (!retryable(r)) { ++stats_.permanentFailures; return; }
                if (attempt == cfg_.maxAttempts) { ++stats_.gaveUp; return; }
                ++stats_.retries;
                again = true;
            }
            if (!again) return;
            long long wait = cfg_.backoffBaseMs;
            for (int i = 1; i < attempt && wait < cfg_.backoffMaxMs; ++i) wait *= 2;
            wait = std::min<long long>(wait, cfg_.backoffMaxMs);
            std::unique_lock<std::mutex> q(m_);
            if (cv_.wait_for(q, std::chrono::milliseconds(wait), [&] { return stop_; })) return;
        }
    }

    Transport& transport_;
    Config cfg_;
    ParsedUrl url_;
    mutable std::mutex m_;
    std::mutex ctl_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool stop_ = false, active_ = false, running_ = false;
    Stats stats_;
};

}  // namespace cascade::net::webhook
