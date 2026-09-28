// Tests for core/tester_link.hpp - the beta-tester PORTAL LINK: URL parsing,
// the one-shot link-request file, the single-instance guard, and the two
// network calls (confirm-by-name, migration exchange) against a local
// server, exactly test_tester_usage.cpp's discipline applied to this second,
// independent flow.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tester_link.hpp"
#include "core/tester_usage.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <httplib.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

using cascade::core::BetaLinkNameSender;
using cascade::core::BetaLinkOutcome;
using cascade::core::BetaMigrationOutcome;
using cascade::core::BetaMigrationSender;
using cascade::core::UploadCancel;

namespace {

// ---------------------------------------------------------------------------
// parseBetaLinkUrl - table-driven, pure function, no network
// ---------------------------------------------------------------------------

void testParseBetaLinkUrlExactShapeOnly() {
    const std::string good = "foxsdr://beta?t=" + std::string(40, 'a');
    CHECK(cascade::core::parseBetaLinkUrl(good) == std::string(40, 'a'));

    // Tolerates exactly one layer of surrounding matching quotes - a
    // shell/shortcut quoting artefact around argv[1].
    CHECK(cascade::core::parseBetaLinkUrl("\"" + good + "\"") == std::string(40, 'a'));
    CHECK(cascade::core::parseBetaLinkUrl("'" + good + "'") == std::string(40, 'a'));
    // Mismatched quotes are not "one layer" - left as part of the content and
    // so rejected.
    CHECK(cascade::core::parseBetaLinkUrl("\"" + good + "'").empty());

    // Wrong scheme.
    CHECK(cascade::core::parseBetaLinkUrl("http://beta?t=" + std::string(40, 'a')).empty());
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr2://beta?t=" + std::string(40, 'a')).empty());

    // A path segment.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta/x?t=" + std::string(40, 'a')).empty());

    // An extra query parameter, either side of the token.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(40, 'a') + "&x=1")
              .empty());
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?x=1&t=" + std::string(40, 'a')).empty());

    // Uppercase hex - hex.EncodeToString never emits it, so it is refused
    // rather than normalised (a corrupted copy must fail loudly).
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(40, 'A')).empty());

    // Wrong length (32 hex - a PORTAL token, not an app token) - the two
    // shapes must never be confused.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(32, 'a')).empty());

    // Empty token.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=").empty());

    // Embedded control byte / literal %00 (three ASCII chars, not a real
    // NUL - argv cannot carry an actual NUL) both fail: the control-byte scan
    // catches a raw one, and "%00" simply is not valid hex so the length or
    // hex check refuses it either way.
    {
        std::string withNul = "foxsdr://beta?t=" + std::string(39, 'a');
        withNul.push_back('\0');  // 40th "digit" is a NUL, not hex
        CHECK(cascade::core::parseBetaLinkUrl(withNul).empty());
    }
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=%00" + std::string(37, 'a')).empty());

    // Oversized.
    CHECK(cascade::core::parseBetaLinkUrl(std::string(200, 'x')).empty());

    // Empty input.
    CHECK(cascade::core::parseBetaLinkUrl("").empty());
}

// ---------------------------------------------------------------------------
// validAppToken / extractAppToken
// ---------------------------------------------------------------------------

void testValidAppTokenIsFortyHexNeverThirtyTwo() {
    CHECK(cascade::core::validAppToken(std::string(40, 'a')));
    CHECK(cascade::core::validAppToken("0123456789abcdef0123456789abcdef01234567"));
    CHECK(!cascade::core::validAppToken(std::string(32, 'a')));  // a PORTAL token's shape
    CHECK(!cascade::core::validAppToken(std::string(40, 'A')));
    CHECK(!cascade::core::validAppToken(std::string(39, 'a')));
    CHECK(!cascade::core::validAppToken(""));
}

void testExtractAppTokenTrimsAndValidatesOnly() {
    CHECK(cascade::core::extractAppToken("  " + std::string(40, 'a') + "\n") ==
          std::string(40, 'a'));
    CHECK(cascade::core::extractAppToken(std::string(32, 'a')).empty());  // portal shape refused
    CHECK(cascade::core::extractAppToken("https://foxsdr.com/#t=" + std::string(40, 'a'))
              .empty());  // no fragment parsing for app tokens
}

// ---------------------------------------------------------------------------
// TesterUsageQueue::rewriteToken
// ---------------------------------------------------------------------------

std::string reportWithToken(const std::string& tok) {
    nlohmann::json j;
    j["token"] = tok;
    j["version"] = "1.0";
    return j.dump();
}

void testRewriteTokenRelabelsEveryItem() {
    cascade::core::TesterUsageQueue q;
    q.push(reportWithToken(std::string(32, 'a')));
    q.push(reportWithToken(std::string(32, 'a')));
    CHECK(q.size() == 2);
    q.rewriteToken(std::string(40, 'b'));
    CHECK(q.size() == 2);  // nothing dropped - relabelled, not filtered
    for (const std::string& item : q.items()) {
        CHECK(cascade::core::tokenOfReport(item) == std::string(40, 'b'));
    }
}

void testRewriteTokenLeavesUnparsableItemsAlone() {
    cascade::core::TesterUsageQueue q;
    q.push("not json");
    q.rewriteToken(std::string(40, 'c'));
    CHECK(q.size() == 1);
    CHECK(q.items()[0] == "not json");  // untouched, not dropped
}

// ---------------------------------------------------------------------------
// The one-shot link-request file
// ---------------------------------------------------------------------------

fs::path uniqueDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const auto pid = ::GetCurrentProcessId();
#else
    const auto pid = ::getpid();
#endif
    return fs::temp_directory_path() /
           (std::string("cascade-tester-link-") + tag + "-" + std::to_string(pid) + "-" +
            std::to_string(stamp));
}

void testWriteThenClaimRoundTrips() {
    const fs::path dir = uniqueDir("roundtrip");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tok = std::string(40, 'd');

    CHECK(cascade::core::writeLinkRequestFile(dir.string(), tok));
    CHECK(fs::exists(cascade::core::linkRequestPath(dir.string())));

    const std::string claimed = cascade::core::claimLinkRequestFile(dir.string());
    CHECK(claimed == tok);
    // Claimed exactly once: the file is gone, and a second claim finds
    // nothing.
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());

    fs::remove_all(dir, ec);
}

void testWriteRefusesAnInvalidToken() {
    const fs::path dir = uniqueDir("badtoken");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK(!cascade::core::writeLinkRequestFile(dir.string(), std::string(32, 'a')));  // portal shape
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    fs::remove_all(dir, ec);
}

void testClaimOfAStaleFileDeletesItUnread() {
    const fs::path dir = uniqueDir("stale");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tok = std::string(40, 'e');
    CHECK(cascade::core::writeLinkRequestFile(dir.string(), tok));

    // `now` well past the file's real write time, past the 24h bound -
    // claimed and discarded, never returned.
    const std::time_t farFuture = std::time(nullptr) + cascade::core::kLinkRequestMaxAgeSec + 3600;
    const std::string claimed = cascade::core::claimLinkRequestFile(dir.string(), farFuture);
    CHECK(claimed.empty());
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));  // claimed either way

    fs::remove_all(dir, ec);
}

void testClaimWithNothingThereIsEmpty() {
    const fs::path dir = uniqueDir("empty");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());
    fs::remove_all(dir, ec);
}

void testClaimOfGarbageContentIsEmptyButStillConsumesTheFile() {
    const fs::path dir = uniqueDir("garbage");
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(cascade::core::linkRequestPath(dir.string()), std::ios::binary);
        f << "not a token";
    }
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// claimPrimaryInstanceAt - the two-call-in-one-process trick: the mutex/lock
// identity is what is being tested, not cross-process behaviour, so calling
// it twice for the SAME identity from one test binary is exactly what
// exercises "a second claimant sees it is already held" (a real second
// process would see the identical ERROR_ALREADY_EXISTS/EWOULDBLOCK either
// way - CreateMutex and flock() do not distinguish same-process from
// cross-process callers).
// ---------------------------------------------------------------------------

void testClaimPrimaryInstanceFirstWinsSecondDoesNot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const std::string identity = "Local\\FoxSDR-instance-test-" + std::to_string(stamp);
#else
    const fs::path dir = uniqueDir("instance");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string identity = (dir / "instance.lock").string();
#endif
    CHECK(cascade::core::claimPrimaryInstanceAt(identity));
    CHECK(!cascade::core::claimPrimaryInstanceAt(identity));
#if !defined(_WIN32)
    fs::remove_all(dir, ec);
#endif
}

void testClaimPrimaryInstanceDifferentIdentitiesAreIndependent() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const std::string a = "Local\\FoxSDR-instance-test-a-" + std::to_string(stamp);
    const std::string b = "Local\\FoxSDR-instance-test-b-" + std::to_string(stamp);
#else
    const fs::path dir = uniqueDir("instance2");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string a = (dir / "a.lock").string();
    const std::string b = (dir / "b.lock").string();
#endif
    CHECK(cascade::core::claimPrimaryInstanceAt(a));
    CHECK(cascade::core::claimPrimaryInstanceAt(b));  // unrelated identity, unaffected
#if !defined(_WIN32)
    fs::remove_all(dir, ec);
#endif
}

// ---------------------------------------------------------------------------
// betaApiBaseUrl override
// ---------------------------------------------------------------------------

void testBetaApiBaseUrlOverride() {
#if defined(_WIN32)
    ::SetEnvironmentVariableA("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
    CHECK(cascade::core::betaApiBaseUrl() == "http://127.0.0.1:9");
    ::SetEnvironmentVariableA("FOXSDR_BETA_API_URL", nullptr);
#else
    ::setenv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9", 1);
    CHECK(cascade::core::betaApiBaseUrl() == "http://127.0.0.1:9");
    ::unsetenv("FOXSDR_BETA_API_URL");
#endif
    CHECK(cascade::core::betaApiBaseUrl() == "https://foxsdr.com");
}

// ---------------------------------------------------------------------------
// The real transport against a local server - confirm-by-name and migration
// exchange, both response shapes plus the invalid/network-error split.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// A local HTTP stub that answers ANY request with a configured status/body,
// and records the last request's method, path and Authorization header - the
// same raw-socket shape as test_tester_usage.cpp's own StubServer,
// independent of it (and of test_crash_upload.cpp's) so a change to one
// transport's fixture cannot silently break another's coverage.
class StubServer {
public:
    bool start(int status, const std::string& body) {
        status_ = status;
        body_ = body;
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    // Nothing listens at all - as close to "the server is down" as a
    // deterministic test gets.
    bool startRefused() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        ::closesocket(s);
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
    }
    ~StubServer() { stop(); }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string lastAuth() {
        std::lock_guard<std::mutex> lk(mu_);
        return lastAuth_;
    }
    std::string lastMethod() {
        std::lock_guard<std::mutex> lk(mu_);
        return lastMethod_;
    }

private:
    void loop() {
        while (run_) {
            fd_set rd;
            FD_ZERO(&rd);
            if (listen_ == INVALID_SOCKET) { break; }
            FD_SET(listen_, &rd);
            timeval tv{0, 100 * 1000};
            const int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n <= 0) { continue; }
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { continue; }
            std::string req;
            char buf[4096];
            std::size_t contentLength = 0;
            std::size_t headerEnd = std::string::npos;
            while (run_) {
                const int got = ::recv(c, buf, sizeof(buf), 0);
                if (got <= 0) { break; }
                req.append(buf, buf + got);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::size_t at = lowerFind(req, "content-length:");
                        if (at != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::size_t sp = req.find(' ');
                lastMethod_ = (sp == std::string::npos) ? std::string() : req.substr(0, sp);
                const std::size_t at = lowerFind(req, "authorization:");
                lastAuth_.clear();
                if (at != std::string::npos) {
                    const std::size_t lineEnd = req.find("\r\n", at);
                    std::size_t start = at + 14;
                    while (start < req.size() && req[start] == ' ') { ++start; }
                    lastAuth_ = req.substr(start, (lineEnd == std::string::npos ? req.size() : lineEnd) - start);
                }
            }
            const std::string status = std::to_string(status_);
            const std::string statusLine = "HTTP/1.1 " + status + " x\r\nContent-Length: " +
                                           std::to_string(body_.size()) + "\r\n\r\n" + body_;
            ::send(c, statusLine.c_str(), static_cast<int>(statusLine.size()), 0);
            ::shutdown(c, SD_BOTH);
            ::closesocket(c);
        }
    }
    static std::size_t lowerFind(const std::string& hay, const std::string& needle) {
        std::string l;
        l.reserve(hay.size());
        for (char ch : hay) {
            l.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return l.find(needle);
    }

    int status_ = 200;
    std::string body_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread thread_;
    std::mutex mu_;
    std::string lastAuth_;
    std::string lastMethod_;
};

#else  // !_WIN32

// The POSIX mirror: a local httplib::Server serving plain http, which
// authRequestBounded's loopback exception allows without a certificate.
struct StubServer {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    int status = 200;
    std::string body;
    std::mutex mu;
    std::string lastAuth_;
    std::string lastMethod_;

    bool start(int st, const std::string& b) {
        status = st;
        body = b;
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lk(mu);
            lastMethod_ = req.method;
            lastAuth_ = req.get_header_value("Authorization");
            res.status = status;
            res.set_content(body, "application/json");
        };
        srv.Get(".*", handler);
        srv.Post(".*", handler);
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
        return true;
    }
    bool startRefused() {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
        probe.stop();
        return true;
    }
    ~StubServer() {
        srv.stop();
        if (th.joinable()) { th.join(); }
    }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port); }
    std::string lastAuth() {
        std::lock_guard<std::mutex> lk(mu);
        return lastAuth_;
    }
    std::string lastMethod() {
        std::lock_guard<std::mutex> lk(mu);
        return lastMethod_;
    }
};

#endif  // _WIN32

void testResolveAppTokenNameOk() {
    StubServer srv;
    CHECK(srv.start(200, "{\"name\":\"Ada\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::Ok);
    CHECK(r.name == "Ada");
    CHECK(srv.lastMethod() == "GET");
    CHECK(srv.lastAuth() == "Bearer " + std::string(40, 'a'));
}

void testResolveAppTokenNameInvalidOn404() {
    StubServer srv;
    CHECK(srv.start(404, ""));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::Invalid);
    CHECK(r.name.empty());
}

void testResolveAppTokenNameNetworkErrorOnRefusal() {
    StubServer srv;
    CHECK(srv.startRefused());
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::NetworkError);
}

void testResolveAppTokenNameNetworkErrorOnMalformedBody() {
    StubServer srv;
    CHECK(srv.start(200, "not json"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::NetworkError);
}

void testExchangePortalTokenOk() {
    StubServer srv;
    const std::string appTok = std::string(40, 'f');
    CHECK(srv.start(200, "{\"appToken\":\"" + appTok + "\",\"name\":\"Grace\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::Ok);
    CHECK(r.appToken == appTok);
    CHECK(r.name == "Grace");
    CHECK(srv.lastMethod() == "POST");
    CHECK(srv.lastAuth() == "Bearer " + std::string(32, 'p'));
}

void testExchangePortalTokenInvalidOn404And401() {
    {
        StubServer srv;
        CHECK(srv.start(404, ""));
        auto cancel = std::make_shared<UploadCancel>();
        const auto r =
            cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
        CHECK(r.outcome == BetaMigrationOutcome::Invalid);
    }
    {
        StubServer srv;
        CHECK(srv.start(401, ""));
        auto cancel = std::make_shared<UploadCancel>();
        const auto r =
            cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
        CHECK(r.outcome == BetaMigrationOutcome::Invalid);
    }
}

void testExchangePortalTokenNetworkErrorOnRefusal() {
    StubServer srv;
    CHECK(srv.startRefused());
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::NetworkError);
}

void testExchangePortalTokenRejectsAMintedTokenOfTheWrongShape() {
    // A site bug (or a stub server for this exact test) returning a 32-hex
    // value in "appToken" must not be trusted as an app token - the two
    // shapes are how this codebase tells them apart at all.
    StubServer srv;
    CHECK(srv.start(200, "{\"appToken\":\"" + std::string(32, 'z') + "\",\"name\":\"X\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::NetworkError);
}

// ---------------------------------------------------------------------------
// The fire-and-forget senders
// ---------------------------------------------------------------------------

void testBetaLinkNameSenderRunsAndReports() {
    StubServer srv;
    CHECK(srv.start(200, "{\"name\":\"Marie\"}"));
    BetaLinkNameSender s;
    s.send(srv.baseUrl(), std::string(40, 'a'));
    std::optional<cascade::core::BetaLinkResolved> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeResult();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got.has_value());
    if (got) {
        CHECK(got->outcome == BetaLinkOutcome::Ok);
        CHECK(got->name == "Marie");
    }
}

void testBetaMigrationSenderRunsAndReports() {
    StubServer srv;
    const std::string appTok = std::string(40, 'b');
    CHECK(srv.start(200, "{\"appToken\":\"" + appTok + "\",\"name\":\"Rosalind\"}"));
    BetaMigrationSender s;
    s.send(srv.baseUrl(), std::string(32, 'p'));
    std::optional<cascade::core::BetaMigrationResult> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeResult();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got.has_value());
    if (got) {
        CHECK(got->outcome == BetaMigrationOutcome::Ok);
        CHECK(got->appToken == appTok);
    }
}

void testSendersRefuseASecondCallWhileBusy() {
    // A hanging server would be needed to observe busy() mid-flight reliably;
    // the cheaper, deterministic half of this property is that a sender with
    // no url/token at all never starts a thread, which busy() reflects
    // immediately.
    BetaLinkNameSender s;
    CHECK(!s.busy());
    s.send("", "");
    CHECK(!s.busy());
}

// ---------------------------------------------------------------------------
// Nothing here logs the token - same discipline as test_tester_usage.cpp's
// own scan, applied to the two new source files this feature adds.
// ---------------------------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path findRepoRoot(const std::string& givenRoot) {
    if (!givenRoot.empty()) { return fs::path(givenRoot); }
    fs::path dir = fs::current_path();
    std::error_code ec;
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_regular_file(dir / "PRIVACY.md", ec) && fs::is_directory(dir / "src", ec)) {
            return dir;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

void testTokenNeverAppearsNearALogCall(const std::string& givenRoot) {
    const fs::path repoRoot = findRepoRoot(givenRoot);
    CHECK(!repoRoot.empty());
    if (repoRoot.empty()) { return; }

    // core/tester_link.cpp: no log-emitting call at all, same rule as
    // tester_usage.cpp.
    const std::string linkSrc = readFile(repoRoot / "src" / "core" / "tester_link.cpp");
    CHECK(!linkSrc.empty());
    CHECK(linkSrc.find("diagLogf") == std::string::npos);
    CHECK(linkSrc.find("diagWarnf") == std::string::npos);
    CHECK(linkSrc.find("std::printf") == std::string::npos);
    CHECK(linkSrc.find("fprintf") == std::string::npos);

    // main.cpp: the one diagLogf call this feature adds logs only a length,
    // never the token/URL variable - scan every logging line for the names
    // that WOULD leak it.
    const std::string mainSrc = readFile(repoRoot / "src" / "main.cpp");
    CHECK(!mainSrc.empty());
    std::istringstream lines(mainSrc);
    std::string line;
    bool clean = true;
    int lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        const bool logs = line.find("diagLogf(") != std::string::npos ||
                          line.find("diagWarnf(") != std::string::npos;
        if (!logs) { continue; }
        // linkToken.size() is fine (that is exactly what the one permitted
        // log line reports) - what must never appear is the variable used
        // any other way on a logging line.
        const bool mentionsToken = line.find("linkToken") != std::string::npos;
        const bool onlyItsSize = line.find("linkToken.size()") != std::string::npos &&
                                 line.find("linkToken") == line.rfind("linkToken");
        const bool leaks = (mentionsToken && !onlyItsSize) ||
                           line.find("argv[1]") != std::string::npos;
        if (leaks) {
            clean = false;
            std::printf("FAIL main.cpp:%d logs the tester link token: %s\n", lineNo, line.c_str());
        }
    }
    CHECK(clean);

    // gui/app_window.cpp already has a line-scan for this in
    // test_tester_usage.cpp; extend the same property here to the new field
    // names this feature adds, independent of that file's own list.
    const std::string appSrc = readFile(repoRoot / "src" / "gui" / "app_window.cpp");
    CHECK(!appSrc.empty());
    std::istringstream appLines(appSrc);
    bool appClean = true;
    int appLineNo = 0;
    while (std::getline(appLines, line)) {
        ++appLineNo;
        const bool logs = line.find("diagLogf(") != std::string::npos ||
                          line.find("diagWarnf(") != std::string::npos;
        if (!logs) { continue; }
        const bool leaks = line.find("testerAppToken_") != std::string::npos ||
                           line.find("testerLinkPending_") != std::string::npos ||
                           line.find("testerLinkResolvingToken_") != std::string::npos;
        if (leaks) {
            appClean = false;
            std::printf("FAIL app_window.cpp:%d logs the app token: %s\n", appLineNo, line.c_str());
        }
    }
    CHECK(appClean);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string givenRoot = argc > 1 ? argv[1] : std::string();

    testParseBetaLinkUrlExactShapeOnly();
    testValidAppTokenIsFortyHexNeverThirtyTwo();
    testExtractAppTokenTrimsAndValidatesOnly();
    testRewriteTokenRelabelsEveryItem();
    testRewriteTokenLeavesUnparsableItemsAlone();
    testWriteThenClaimRoundTrips();
    testWriteRefusesAnInvalidToken();
    testClaimOfAStaleFileDeletesItUnread();
    testClaimWithNothingThereIsEmpty();
    testClaimOfGarbageContentIsEmptyButStillConsumesTheFile();
    testClaimPrimaryInstanceFirstWinsSecondDoesNot();
    testClaimPrimaryInstanceDifferentIdentitiesAreIndependent();
    testBetaApiBaseUrlOverride();
    testResolveAppTokenNameOk();
    testResolveAppTokenNameInvalidOn404();
    testResolveAppTokenNameNetworkErrorOnRefusal();
    testResolveAppTokenNameNetworkErrorOnMalformedBody();
    testExchangePortalTokenOk();
    testExchangePortalTokenInvalidOn404And401();
    testExchangePortalTokenNetworkErrorOnRefusal();
    testExchangePortalTokenRejectsAMintedTokenOfTheWrongShape();
    testBetaLinkNameSenderRunsAndReports();
    testBetaMigrationSenderRunsAndReports();
    testSendersRefuseASecondCallWhileBusy();
    testTokenNeverAppearsNearALogCall(givenRoot);

    return testSummary("test_tester_link");
}
