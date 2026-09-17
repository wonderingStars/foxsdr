// Tests for core/feature_request.hpp - the client half of the built-in
// "REQUEST A FEATURE" button.
//
// EVERY SEND IN THIS FILE GOES TO A LOCAL STUB ON 127.0.0.1, never to
// foxsdr.com - see the first block below, which proves featureRequestEndpoint()
// resolves under the loopback address BEFORE anything here calls send(), the
// same discipline tests/test_crash_upload.cpp holds itself to for the crash
// uploader's identical transport.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/feature_request.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <condition_variable>

// Must match every other TU in the program that includes httplib.h on
// non-Windows (plugin_repo.cpp, crash_upload.cpp, telemetry.cpp,
// web_server.cpp, test_crash_upload.cpp) - or a client/server layout mismatch
// reproduces the SEGV documented in web_server.cpp's own comment on this.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

// BOUNDS-SAFE INDEXING - see test_crash_upload.cpp's identical helper and the
// Lessons-learned entry it exists to satisfy: a CHECK that records and
// continues must never be followed by an unguarded v[0].
template <typename T>
T at(const std::vector<T>& v, std::size_t i) {
    return (i < v.size()) ? v[i] : T();
}

nlohmann::json parseOrEmpty(const std::string& s) {
    nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return nlohmann::json::object(); }
    return j;
}

FeatureRequestPayload samplePayload(const std::string& text = "Please add a squelch tail hang timer",
                                    const std::string& contact = "") {
    FeatureRequestPayload p;
    p.text = text;
    p.contact = contact;
    p.version = "0.99.0-test";
    p.platform = "windows";
    p.arch = "x64";
    return p;
}

// Runs poll() every few milliseconds until state() leaves Sending or the
// deadline passes - never a bare sleep guessing how long a real socket needs.
FeatureRequestState waitForTerminal(FeatureRequestSender& sender, std::uint64_t nowEpoch,
                                    int timeoutMs = 8000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        sender.poll(nowEpoch);
        const FeatureRequestState st = sender.state();
        if (st != FeatureRequestState::Sending) { return st; }
        if (std::chrono::steady_clock::now() >= deadline) { return st; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---------------------------------------------------------------------------
// PRIVACY.md, parsed - same pattern as test_crash_upload.cpp's
// documentedUploadFields(), simplified for a flat table with no context row.
// ---------------------------------------------------------------------------
std::string readPrivacyDoc() {
    std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / "PRIVACY.md", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> backtickedIn(const std::string& cell) {
    std::vector<std::string> out;
    std::size_t at2 = 0;
    while (true) {
        const std::size_t a = cell.find('`', at2);
        if (a == std::string::npos) { break; }
        const std::size_t b = cell.find('`', a + 1);
        if (b == std::string::npos) { break; }
        const std::string tok = cell.substr(a + 1, b - a - 1);
        bool ident = !tok.empty();
        for (char c : tok) {
            if (!std::isalnum(static_cast<unsigned char>(c))) { ident = false; }
        }
        if (ident) { out.push_back(tok); }
        at2 = b + 1;
    }
    return out;
}

std::set<std::string> documentedFeatureRequestFields() {
    std::set<std::string> fields;
    const std::string doc = readPrivacyDoc();
    const std::size_t start = doc.find("## What is sent when you request a feature");
    if (start == std::string::npos) { return fields; }
    const std::size_t end = doc.find("\n## ", start + 4);
    const std::string section = doc.substr(start, (end == std::string::npos) ? end : end - start);

    std::size_t at3 = 0;
    while (at3 < section.size()) {
        const std::size_t nl = section.find('\n', at3);
        const std::string line =
            section.substr(at3, (nl == std::string::npos) ? std::string::npos : nl - at3);
        at3 = (nl == std::string::npos) ? section.size() : nl + 1;
        if (line.rfind("| `", 0) != 0) { continue; }
        std::size_t p = 1;
        const std::size_t bar = line.find('|', p);
        if (bar == std::string::npos) { continue; }
        for (const std::string& n : backtickedIn(line.substr(p, bar - p))) { fields.insert(n); }
    }
    return fields;
}

// ---------------------------------------------------------------------------
// A local HTTP stub - same discipline as test_crash_upload.cpp's StubServer:
// read headers to the blank line, read exactly Content-Length body bytes,
// reply, then close cleanly - a fake that closes without draining sends an
// RST and flakes.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

class StubServer {
public:
    enum class Mode { Accept200, BadRequest400, RateLimit429, Hang, Refuse };

    bool start(Mode mode) {
        mode_ = mode;
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
        if (mode_ == Mode::Refuse) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return true;
        }
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
        for (SOCKET s : held_) { ::closesocket(s); }
        held_.clear();
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/api/feature-request";
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
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
            connections_.fetch_add(1);
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
                        const std::size_t at4 = lowerFind(req, "content-length:");
                        if (at4 != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at4 + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            if (headerEnd != std::string::npos) {
                std::lock_guard<std::mutex> lk(mu_);
                requests_.push_back(req.substr(0, headerEnd));
                bodies_.push_back(req.substr(headerEnd + 4));
            }
            if (mode_ == Mode::Hang) {
                held_.push_back(c);
                continue;
            }
            std::string resp;
            if (mode_ == Mode::RateLimit429) {
                const std::string body = "{\"ok\":false,\"error\":\"slow down, please\"}";
                resp = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n"
                       "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            } else if (mode_ == Mode::BadRequest400) {
                const std::string body =
                    "{\"ok\":false,\"error\":\"text must be between 10 and 2000 characters\"}";
                resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                       "Content-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            } else {
                const std::string body = "{\"ok\":true}";
                resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            }
            ::send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
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

    Mode mode_ = Mode::Accept200;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<std::string> requests_;
    std::vector<SOCKET> held_;
};

#else  // !_WIN32

class StubServer {
public:
    enum class Mode { Accept200, BadRequest400, RateLimit429, Hang, Refuse };

    bool start(Mode mode) {
        mode_ = mode;
        if (mode_ == Mode::Refuse) {
            // See test_crash_upload.cpp's identical comment: bind then
            // release a port so it is refused in microseconds rather than
            // left listening-but-unaccepted, which would hang on the READ
            // timeout instead.
            httplib::Server probe;
            port_ = probe.bind_to_any_port("127.0.0.1");
            probe.stop();
            return port_ > 0;
        }
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { return false; }
        server_.Post("/api/feature-request",
                     [this](const httplib::Request& req, httplib::Response& res) {
                         {
                             std::lock_guard<std::mutex> lk(mu_);
                             bodies_.push_back(req.body);
                             requests_.push_back(req.get_header_value("Content-Type"));
                         }
                         connections_.fetch_add(1);
                         if (mode_ == Mode::Hang) {
                             std::unique_lock<std::mutex> lk(hangMu_);
                             hangCv_.wait_for(lk, std::chrono::seconds(30),
                                              [this] { return hangStop_.load(); });
                             return;
                         }
                         if (mode_ == Mode::RateLimit429) {
                             res.status = 429;
                             res.set_header("Retry-After", "120");
                             res.set_content("{\"ok\":false,\"error\":\"slow down, please\"}",
                                            "application/json");
                         } else if (mode_ == Mode::BadRequest400) {
                             res.status = 400;
                             res.set_content(
                                 "{\"ok\":false,\"error\":\"text must be between 10 and "
                                 "2000 characters\"}",
                                 "application/json");
                         } else {
                             res.status = 200;
                             res.set_content("{\"ok\":true}", "application/json");
                         }
                     });
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }

    void stop() {
        hangStop_.store(true);
        hangCv_.notify_all();
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/api/feature-request";
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }
    // On POSIX this holds the captured Content-Type, not the raw header
    // block WinHTTP's variant does - both call sites below only ever check
    // for "application/json" as a substring, which is true either way.
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

private:
    Mode mode_ = Mode::Accept200;
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::atomic<int> connections_{0};
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<std::string> requests_;
    std::mutex hangMu_;
    std::condition_variable hangCv_;
    std::atomic<bool> hangStop_{false};
};

#endif  // _WIN32

}  // namespace

int main() {
    // --- THE PAYLOAD, and the field inventory in every direction ------------
    {
        const FeatureRequestPayload p = samplePayload("  hello, this has leading space  ", "  ");
        const std::string js = featureRequestJson(p);
        const nlohmann::json j = parseOrEmpty(js);
        CHECK(j.value("schema", 0) == 1);
        // TRIMMED, and the contact - all whitespace - trims to empty.
        CHECK(j.value("text", std::string()) == "hello, this has leading space");
        CHECK(j.value("contact", std::string()) == "");
        CHECK(j.value("version", std::string()) == "0.99.0-test");
        CHECK(j.value("platform", std::string()) == "windows");
        CHECK(j.value("arch", std::string()) == "x64");

        std::set<std::string> actual;
        for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
        const std::set<std::string> declared(featureRequestFieldNames().begin(),
                                             featureRequestFieldNames().end());
        CHECK(!declared.empty());
        CHECK(actual == declared);

        // ...and PRIVACY.md names exactly the same six fields.
        const std::set<std::string> documented = documentedFeatureRequestFields();
        CHECK(!documented.empty());
        CHECK(documented == declared);
    }

    // --- UTF-8 CHARACTER COUNTING, not bytes --------------------------------
    {
        CHECK(featureRequestCharCount("") == 0);
        CHECK(featureRequestCharCount("hello") == 5);
        // "café" - the e-acute is a two-byte UTF-8 sequence (0xC3 0xA9), so
        // this is 4 CHARACTERS across 5 BYTES. Counting bytes would read 5.
        CHECK(featureRequestCharCount("caf\xC3\xA9") == 4);
        // Three CJK characters, each three UTF-8 bytes: 3 characters, 9 bytes.
        CHECK(featureRequestCharCount("\xE4\xBD\xA0\xE5\xA5\xBD\xE5\x95\x8A") == 3);
        // A single four-byte emoji (U+1F4E1, a satellite antenna): 1
        // character, 4 bytes - the boundary furthest from "just count bytes".
        CHECK(featureRequestCharCount("\xF0\x9F\x93\xA1") == 1);
        // A lone continuation byte with no lead byte in front of it: malformed
        // UTF-8 undercounts rather than crashing or overcounting.
        CHECK(featureRequestCharCount("\x80\x80") == 0);
    }

    // --- VALIDATION BOUNDARIES, text: 9 / 10 / 2000 / 2001 -------------------
    {
        CHECK(!validateFeatureRequestText(std::string(9, 'a')).empty());
        CHECK(validateFeatureRequestText(std::string(10, 'a')).empty());
        CHECK(validateFeatureRequestText(std::string(kFeatureRequestMaxChars, 'a')).empty());
        CHECK(!validateFeatureRequestText(std::string(kFeatureRequestMaxChars + 1, 'a')).empty());
        // Whitespace-only: trims to zero characters, so it fails exactly like
        // an empty string - never "10 characters of nothing".
        CHECK(!validateFeatureRequestText(std::string(20, ' ')).empty());
        CHECK(!validateFeatureRequestText("\n\n\n\n\n\n\n\n\n\n\n\n").empty());
        // 9 ASCII characters plus one 2-byte UTF-8 character is 10
        // characters across 11 bytes - the boundary a byte-counting bug would
        // get wrong in the OPPOSITE direction from the café case above.
        CHECK(validateFeatureRequestText(std::string(9, 'a') + "\xC3\xA9").empty());
        // 1500 code points, each a two-byte UTF-8 character (3000 bytes):
        // comfortably inside 10..2000, and well past what an 8192-byte body
        // cap would have refused for a 4-byte-per-character message of the
        // same length - which is exactly why the server's cap moved to
        // 16384 (2026-09-17): 2000 four-byte characters plus a contact line
        // is over 8 KiB on its own. Length here is still judged purely in
        // CHARACTERS, never bytes, regardless of what the body cap is.
        {
            std::string s1500;
            s1500.reserve(1500 * 2);
            for (int i = 0; i < 1500; ++i) { s1500 += "\xC3\xA9"; }
            CHECK(featureRequestCharCount(s1500) == 1500);
            CHECK(validateFeatureRequestText(s1500).empty());
        }
        // Nine code points, each a three-byte UTF-8 character (27 bytes):
        // under the 10-character minimum even though the byte count alone
        // would look plausible to a counter that measured bytes instead.
        {
            std::string s9;
            s9.reserve(9 * 3);
            for (int i = 0; i < 9; ++i) { s9 += "\xE4\xBD\xA0"; }
            CHECK(featureRequestCharCount(s9) == 9);
            CHECK(!validateFeatureRequestText(s9).empty());
        }
    }

    // --- CONTROL CHARACTERS ARE STRIPPED BEFORE COUNTING, matching the
    //     server (feature-request-contract.md, updated 2026-09-17) -----------
    {
        // Ten control characters and nothing else strip to ZERO printable
        // characters - the server would never see ten characters to accept,
        // so this client must not either. (Before the 2026-09-17 contract
        // clarification this client counted control bytes as characters,
        // which would have let a person's SEND key light up for a message
        // the server was always going to refuse as too short.)
        CHECK(featureRequestTextCharCount(std::string(10, '\x07')) == 0);
        CHECK(!validateFeatureRequestText(std::string(10, '\x07')).empty());
        // Ten PRINTABLE characters plus five control characters still count
        // as exactly ten - the control bytes vanish rather than being
        // refused outright or double-counted.
        CHECK(featureRequestTextCharCount(std::string(10, 'a') + std::string(5, '\x07')) == 10);
        CHECK(validateFeatureRequestText(std::string(10, 'a') + std::string(5, '\x07')).empty());
        // Nine printable characters plus an EMBEDDED newline is ten - '\n' is
        // kept in TEXT (the contract's own exception), so this clears the
        // minimum. Embedded in the MIDDLE deliberately, not trailing: a
        // trailing '\n' is whitespace and trimFeatureWhitespace() would trim
        // it away before this rule ever got a chance to keep it, which would
        // make the test pass for the wrong reason (or not test the "kept"
        // rule at all).
        CHECK(featureRequestTextCharCount("aaaa\naaaaa") == 10);
        CHECK(validateFeatureRequestText("aaaa\naaaaa").empty());
        // Nine printable characters plus an embedded TAB, same story.
        CHECK(featureRequestTextCharCount("aaaa\taaaaa") == 10);
        // CONTACT KEEPS NEITHER: an embedded newline or tab in the contact
        // line is stripped just like any other control character, because
        // contact is meant to be one line - so 60 letters, an embedded
        // newline and tab, and 60 more letters is still exactly 120, not
        // 122.
        CHECK(featureRequestContactCharCount(std::string(60, 'a') + "\n\t" + std::string(60, 'a')) ==
             120);
        CHECK(validateFeatureRequestContact(std::string(60, 'a') + "\n\t" + std::string(60, 'a'))
                  .empty());
        // 121 printable characters plus control bytes is still over the
        // bound - the control bytes never rescue an over-length contact by
        // being miscounted as the characters that put it over.
        CHECK(!validateFeatureRequestContact(std::string(121, 'a') + "\x07").empty());
    }

    // --- VALIDATION BOUNDARIES, contact: 120 / 121 ---------------------------
    {
        CHECK(validateFeatureRequestContact(std::string(kFeatureRequestMaxContactChars, 'a'))
                  .empty());
        CHECK(!validateFeatureRequestContact(
                   std::string(kFeatureRequestMaxContactChars + 1, 'a'))
                   .empty());
        // Empty and whitespace-only are both fine - contact is OPTIONAL.
        CHECK(validateFeatureRequestContact("").empty());
        CHECK(validateFeatureRequestContact("   ").empty());
    }

    // --- FOXSDR_FEATURE_URL is honoured, and the default is the real host ---
    //
    // The default is asserted but NEVER connected to - this test only reads
    // the string featureRequestEndpoint() would use, the same way
    // test_crash_upload.cpp checks crashUploadEndpoint()'s default without a
    // socket.
    {
#if defined(_WIN32)
        ::SetEnvironmentVariableA("FOXSDR_FEATURE_URL", nullptr);
#else
        ::unsetenv("FOXSDR_FEATURE_URL");
#endif
        CHECK(featureRequestEndpoint() == "https://foxsdr.com/api/feature-request");
#if defined(_WIN32)
        ::SetEnvironmentVariableA("FOXSDR_FEATURE_URL", "http://127.0.0.1:9");
#else
        ::setenv("FOXSDR_FEATURE_URL", "http://127.0.0.1:9", 1);
#endif
        CHECK(featureRequestEndpoint() == "http://127.0.0.1:9");
        // THE PROOF NOTHING IN THIS FILE CAN REACH foxsdr.com: every send()
        // below points at a StubServer whose url() is under 127.0.0.1, and
        // this line pins that the override mechanism the whole file relies
        // on for that actually works, on THIS platform, before any of it is
        // trusted.
        CHECK(featureRequestEndpoint().rfind("http://127.0.0.1:", 0) == 0);
#if defined(_WIN32)
        ::SetEnvironmentVariableA("FOXSDR_FEATURE_URL", nullptr);
#else
        ::unsetenv("FOXSDR_FEATURE_URL");
#endif
    }

    // --- platform()/arch() answer the contract's own vocabulary -------------
    {
        const std::string plat = featureRequestPlatform();
        CHECK(plat == "windows" || plat == "linux" || plat == "android");
        const std::string arch = featureRequestArch();
        CHECK(arch == "x64" || arch == "arm64");
    }

    // --- A real POST to a real socket, and a real 200 accepted --------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Accept200));
        FeatureRequestSender sender;
        const std::uint64_t t0 = 1000;
        const FeatureRequestPayload p = samplePayload();
        CHECK(sender.send(srv.url(), p, t0));
        // A second send while the first is in flight is refused outright -
        // the belt-and-braces check inside send() itself, before any GUI
        // disabling is involved.
        CHECK(!sender.send(srv.url(), p, t0));
        const FeatureRequestState st = waitForTerminal(sender, t0);
        CHECK(st == FeatureRequestState::Sent);
        CHECK(sender.lastStatus() == 200);
        CHECK(srv.connections() == 1);
        // THE BODY THE SERVER RECEIVED IS BYTE-FOR-BYTE THE BUILDER'S JSON.
        CHECK(at(srv.bodies(), 0) == featureRequestJson(p));
        // Content-Type: application/json, on whichever platform's request
        // capture this is.
        const std::string hdr = at(srv.requests(), 0);
        CHECK(hdr.find("application/json") != std::string::npos);

        // --- cooldown blocks a second send ---------------------------------
        CHECK(!sender.send(srv.url(), p, t0 + 1));
        CHECK(sender.blockedUntil() == t0 + kFeatureRequestCooldownSeconds);
        // Still only the one connection: the blocked call never touched the
        // socket at all.
        CHECK(srv.connections() == 1);
        // ...until the cooldown has fully elapsed.
        sender.poll(t0 + kFeatureRequestCooldownSeconds - 1);
        CHECK(sender.state() == FeatureRequestState::Sent);
        sender.poll(t0 + kFeatureRequestCooldownSeconds);
        CHECK(sender.state() == FeatureRequestState::Idle);
        CHECK(sender.send(srv.url(), p, t0 + kFeatureRequestCooldownSeconds));
        CHECK(waitForTerminal(sender, t0 + kFeatureRequestCooldownSeconds) ==
             FeatureRequestState::Sent);
        CHECK(srv.connections() == 2);
        srv.stop();
    }

    // --- 400 with an error sentence -> Failed, sentence surfaced ------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::BadRequest400));
        FeatureRequestSender sender;
        const std::uint64_t t0 = 2000;
        CHECK(sender.send(srv.url(), samplePayload(), t0));
        const FeatureRequestState st = waitForTerminal(sender, t0);
        CHECK(st == FeatureRequestState::Failed);
        CHECK(sender.lastStatus() == 400);
        CHECK(sender.failureMessage() == "text must be between 10 and 2000 characters");
        // The same 30 s lockout as a plain success - a generic failure is
        // not exempt from the client-side cooldown.
        CHECK(sender.blockedUntil() == t0 + kFeatureRequestCooldownSeconds);
        srv.stop();
    }

    // --- 429 with Retry-After: 120 -> CoolingDown, ~120 s --------------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::RateLimit429));
        FeatureRequestSender sender;
        const std::uint64_t t0 = 3000;
        CHECK(sender.send(srv.url(), samplePayload(), t0));
        const FeatureRequestState st = waitForTerminal(sender, t0);
        CHECK(st == FeatureRequestState::CoolingDown);
        CHECK(sender.lastStatus() == 429);
        CHECK(sender.failureMessage() == "slow down, please");
        CHECK(sender.blockedUntil() == t0 + 120);
        // A second send refuses for the WHOLE 120 s, not the ordinary 30.
        CHECK(!sender.send(srv.url(), samplePayload(), t0 + 30));
        sender.poll(t0 + 119);
        CHECK(sender.state() == FeatureRequestState::CoolingDown);
        sender.poll(t0 + 120);
        CHECK(sender.state() == FeatureRequestState::Idle);
        srv.stop();
    }

    // --- a server that accepts and never answers -> Failed within the bound,
    //     and cancel() returns promptly ------------------------------------
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Hang));
        FeatureRequestSender sender;
        const std::uint64_t t0 = 4000;
        const auto t0Wall = std::chrono::steady_clock::now();
        CHECK(sender.send(srv.url(), samplePayload(), t0));
        // No cancel() here: this measures the TRANSPORT's own bound (the
        // same connect/send/receive timeouts postCrashReport() uses), not
        // the cancel path - that is the next block.
        const FeatureRequestState st = waitForTerminal(sender, t0, 12000);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0Wall)
                              .count();
        std::printf("hang-mode send resolved after %.0f ms\n", ms);
        CHECK(st == FeatureRequestState::Failed);
        CHECK(sender.lastStatus() == 0);
        // Comfortably under the contract's ~10 s bound - postBounded()'s own
        // WinHTTP/httplib timeouts (3 s connect, 5 s receive) are what
        // actually enforce it; this just proves the sender surfaces that as
        // Failed rather than staying in Sending forever.
        CHECK(ms < 10000.0);
        srv.stop();
    }

    // --- cancel() (the destructor path) returns promptly, not at the bound --
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Hang));
        auto sender = std::make_unique<FeatureRequestSender>();
        const std::uint64_t t0 = 5000;
        CHECK(sender->send(srv.url(), samplePayload(), t0));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0Wall = std::chrono::steady_clock::now();
        // Destroying the sender is the shutdown path: cancel() then join(),
        // exactly like CrashUploader::stop(). Allowing scheduler-tick slack
        // rather than an exact millisecond equality, per the house rule.
        sender.reset();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0Wall)
                              .count();
        std::printf("cancel (destructor) returned after %.0f ms\n", ms);
        CHECK(ms < 2000.0);
        srv.stop();
    }

    // --- a refused connection is Failed too, and the sentence is generic ----
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Refuse));
        FeatureRequestSender sender;
        const std::uint64_t t0 = 6000;
        CHECK(sender.send(srv.url(), samplePayload(), t0));
        const FeatureRequestState st = waitForTerminal(sender, t0);
        CHECK(st == FeatureRequestState::Failed);
        CHECK(sender.lastStatus() == 0);
        CHECK(!sender.failureMessage().empty());
        srv.stop();
    }

    // --- when the page empties its text box, and how big its buffers are -----
    {
        using cascade::core::featureRequestClearsTextNow;
        using S = cascade::core::FeatureRequestState;
        // The one frame a success is first seen.
        CHECK(featureRequestClearsTextNow(S::Sending, S::Sent));
        // NOT WHILE THE THANK-YOU IS STILL SHOWING. Sent is held for the whole
        // cooldown, and a rule of "clear whenever it is Sent" wiped every
        // keystroke of a second request for thirty seconds.
        // RED WHEN the rule looks only at the current state.
        CHECK(!featureRequestClearsTextNow(S::Sent, S::Sent));
        CHECK(!featureRequestClearsTextNow(S::Idle, S::Sent));
        // A failure, a rate limit and a send still in flight keep the words.
        CHECK(!featureRequestClearsTextNow(S::Sending, S::Failed));
        CHECK(!featureRequestClearsTextNow(S::Sending, S::CoolingDown));
        CHECK(!featureRequestClearsTextNow(S::Sending, S::Sending));
        CHECK(!featureRequestClearsTextNow(S::Idle, S::Idle));

        // THE BUFFERS HOLD WHAT THE LIMITS ALLOW: 2000 four-byte characters
        // and a terminator, with room for the one character too many that
        // validation then refuses in words.
        // RED WHEN a buffer is sized from the character count alone.
        const std::string longest(cascade::core::kFeatureRequestMaxChars * 4u, 'x');
        CHECK(longest.size() + 4u + 1u <= cascade::core::kFeatureRequestTextBufferBytes);
        CHECK(cascade::core::kFeatureRequestMaxContactChars * 4u + 4u + 1u <=
              cascade::core::kFeatureRequestContactBufferBytes);
    }

    // --- THE TWO HALVES AGAINST EACH OTHER, only when asked ------------------
    //
    // Everything above proves this client against a fake it wrote itself, and
    // the server's tests prove it against requests they wrote themselves; two
    // halves each green against their own idea of the contract is how a
    // byte-counting server and a character-counting client both passed on the
    // same day (2026-09-17). With FOXSDR_FEATURE_E2E_URL pointing at a REAL
    // foxsdr-site binary on loopback, this sends what the application sends
    // and reads what the server really answers. It refuses any host but
    // 127.0.0.1, so it cannot be aimed at the production site by mistake.
    {
        const char* e2e = std::getenv("FOXSDR_FEATURE_E2E_URL");
        if (e2e == nullptr || e2e[0] == '\0') {
            std::printf("SKIPPED: end-to-end against a real site binary "
                        "(set FOXSDR_FEATURE_E2E_URL=http://127.0.0.1:<port>/api/feature-request)\n");
        } else {
            const std::string url = e2e;
            CHECK(url.rfind("http://127.0.0.1:", 0) == 0);
            if (url.rfind("http://127.0.0.1:", 0) == 0) {
                // 1500 two-byte letters and a contact in the same script: the
                // message a byte-counting server refused.
                std::string cyr;
                for (int i = 0; i < 1500; ++i) { cyr += "\xD0\xB6"; }
                int accepted = 0;
                int limited = 0;
                std::string lastSentence;
                // Six sends from one address: the contract's five an hour,
                // then the sixth refused with the server's own sentence. A
                // fresh sender each time, because one sender's 30 s cooldown
                // is tested above and is not what is being measured here.
                for (int i = 0; i < 6; ++i) {
                    FeatureRequestSender sender;
                    const std::uint64_t t0 = 5000;
                    CHECK(sender.send(url, samplePayload(cyr, "\xD0\xB6\xD0\xB6"), t0));
                    const FeatureRequestState st = waitForTerminal(sender, t0);
                    if (st == FeatureRequestState::Sent && sender.lastStatus() == 200) {
                        ++accepted;
                    } else if (sender.lastStatus() == 429) {
                        ++limited;
                        lastSentence = sender.failureMessage();
                        CHECK(sender.blockedUntil() > t0 + kFeatureRequestCooldownSeconds);
                    } else {
                        std::printf("e2e send %d: state %d, HTTP %d, %s\n", i,
                                    static_cast<int>(st), sender.lastStatus(),
                                    sender.failureMessage().c_str());
                    }
                }
                std::printf("e2e: accepted %d, rate-limited %d, sentence: %s\n", accepted,
                            limited, lastSentence.c_str());
                CHECK(accepted == 5);
                CHECK(limited == 1);
                CHECK(!lastSentence.empty());
            }
        }
    }

    return testSummary("test_feature_request");
}
