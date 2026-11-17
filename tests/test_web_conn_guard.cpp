// test_web_conn_guard.cpp - the plain web server's bound on a slow client (security review 2, R4; net/web_conn_guard.hpp).
//
//   G1  a request head dripped one byte at a time is cut off after the total time to read a request, however often a byte comes; with the bound off (requestRead = 0) the same drip is never cut off
//       (this is the break-it: the bound is what ends the connection, not something else)
//   G2  a later request of a kept-alive connection is bounded the same way
//   G3  an honest client is unaffected: a request in one packet is answered, a second one on the same connection too, and an answer that takes LONGER than the total time to send is
//       delivered whole (the bound is on reading a request, never on writing a response)
//   G4  the cap: with 3 allowed pending connections per address, the 4th and 5th silent connections are closed at once; sending a head on one frees its place
//   G5  the loopback address is not counted (the HTTPS relay's connections come from it)
//
// Every socket is on 127.0.0.1. Nothing outside this computer is contacted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "net/web_conn_guard.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

namespace {

#if defined(_WIN32)
using SockT = SOCKET;
#else
using SockT = int;
#endif

void sockInit() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}
void closeS(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}
SockT dial(int port) {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closeS(s);
        return INVALID_SOCKET;
    }
#if defined(_WIN32)
    DWORD tv = 50;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{0, 50000};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}
bool put(SockT s, const std::string& t) { return ::send(s, t.data(), static_cast<int>(t.size()), 0) == static_cast<int>(t.size()); }

// Reads what is there for up to `ms`; `closed` is set when the server closed the connection (or reset it).
std::string drain(SockT s, int ms, bool& closed) {
    closed = false;
    std::string out;
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    char b[4096];
    while (Clock::now() < end) {
        const int n = static_cast<int>(::recv(s, b, sizeof(b), 0));
        if (n > 0) { out.append(b, static_cast<std::size_t>(n)); continue; }
        if (n == 0) { closed = true; break; }
#if defined(_WIN32)
        const int e = ::WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) { continue; }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { continue; }
#endif
        closed = true;   // a reset
        break;
    }
    return out;
}

struct Rig {
    GuardedServer svr;
    std::thread th;
    int port = 0;
    explicit Rig(const ConnLimits& l) {
        svr.setLimits(l);
        svr.Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });
        // an answer that takes three seconds to send, in chunks
        svr.Get("/slow", [](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider("text/plain", [](std::size_t offset, httplib::DataSink& sink) {
                if (offset >= 6) { sink.done(); return true; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                sink.write("ab", 2);
                return true;
            });
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~Rig() {
        svr.stop();
        if (th.joinable()) { th.join(); }
    }
};

const char kHead[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n";

// Drips `kHead` and then one header byte per `gapMs`; returns the milliseconds until the server closed it, or -1 when it was still open after `maxMs`.
long long dripUntilClosed(SockT s, int gapMs, int maxMs) {
    const auto t0 = Clock::now();
    put(s, kHead);
    const std::string more = "X-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    for (std::size_t i = 0;; ++i) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (ms > maxMs) { return -1; }
        bool closed = false;
        (void)drain(s, gapMs, closed);
        if (closed) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); }
        if (i < more.size()) { (void)put(s, more.substr(i, 1)); }
    }
}

void testSlowHeadIsCutOff() {
    {
        ConnLimits l;
        l.requestRead = std::chrono::milliseconds(1500);
        Rig r(l);
        SockT s = dial(r.port);
        CHECK(s != INVALID_SOCKET);
        const long long ms = dripUntilClosed(s, 300, 6000);
        std::printf("G1: a head dripped a byte every 300 ms was cut off after %lld ms (total allowed 1500)\n", ms);
        CHECK(ms >= 1400 && ms <= 2500);   // at the total, not a keep-alive wait later
        closeS(s);
    }
    {   // the break-it, as a test: with the bound off the same drip is still being served after 4 s
        ConnLimits l;
        l.requestRead = std::chrono::milliseconds(0);
        Rig r(l);
        SockT s = dial(r.port);
        CHECK(s != INVALID_SOCKET);
        const long long ms = dripUntilClosed(s, 300, 4000);
        std::printf("G1 (bound off): the drip was still open after 4 s: %lld\n", ms);
        CHECK(ms == -1);
        closeS(s);
    }
}

void testLaterRequestOfAKeptAliveConnectionIsBounded() {
    ConnLimits l;
    l.requestRead = std::chrono::milliseconds(1500);
    Rig r(l);
    SockT s = dial(r.port);
    CHECK(s != INVALID_SOCKET);
    CHECK(put(s, std::string(kHead) + "\r\n"));
    bool closed = false;
    const std::string first = drain(s, 700, closed);
    CHECK(first.rfind("HTTP/1.1 200", 0) == 0 && !closed);
    const long long ms = dripUntilClosed(s, 300, 6000);
    std::printf("G2: the second request on a kept-alive connection, dripped, was cut off after %lld ms\n", ms);
    CHECK(ms >= 1400 && ms <= 2500);   // at the total, not a keep-alive wait later
    closeS(s);
}

void testHonestClientsAreUnaffected() {
    ConnLimits l;
    l.requestRead = std::chrono::milliseconds(1500);   // shorter than the slow answer below
    Rig r(l);
    SockT s = dial(r.port);
    CHECK(s != INVALID_SOCKET);
    bool closed = false;
    CHECK(put(s, std::string(kHead) + "\r\n"));
    CHECK(drain(s, 500, closed).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(put(s, std::string(kHead) + "\r\n"));
    CHECK(drain(s, 500, closed).rfind("HTTP/1.1 200", 0) == 0);   // and a second on the same connection
    CHECK(put(s, "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    const auto t0 = Clock::now();
    std::string body;
    for (int i = 0; i < 40 && body.find("\r\n0\r\n\r\n") == std::string::npos; ++i) { body += drain(s, 200, closed); if (closed) { break; } }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("G3: an answer that took %lld ms to send (total to read a request: 1500) arrived whole\n", static_cast<long long>(ms));
    CHECK(ms > 2500);
    CHECK(body.find("2\r\nab\r\n2\r\nab\r\n2\r\nab\r\n0\r\n\r\n") != std::string::npos);   // all three chunks and the end, whole
    closeS(s);
}

void testPendingCapPerAddress() {
    ConnLimits l;
    l.maxPendingPerAddress = 3;
    l.exemptLoopback = false;
    Rig r(l);
    std::vector<SockT> socks;
    for (int i = 0; i < 5; ++i) {
        socks.push_back(dial(r.port));
        CHECK(socks.back() != INVALID_SOCKET);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int closedNow = 0, openNow = 0;
    for (std::size_t i = 0; i < socks.size(); ++i) {
        bool closed = false;
        (void)drain(socks[i], 200, closed);
        if (closed) { ++closedNow; } else { ++openNow; }
        if ((i < 3) == closed) { std::printf("G4: connection %zu was %s\n", i + 1, closed ? "closed" : "open"); }
    }
    std::printf("G4: five silent connections with a cap of 3: %d open, %d closed at once, refusedPending=%llu\n", openNow, closedNow, static_cast<unsigned long long>(r.svr.refusedPending()));
    CHECK(openNow == 3 && closedNow == 2);
    CHECK(r.svr.refusedPending() == 2);
    // a head on the first connection frees its place: a new connection is taken and answered
    CHECK(put(socks[0], std::string(kHead) + "\r\n"));
    bool closed = false;
    CHECK(drain(socks[0], 500, closed).rfind("HTTP/1.1 200", 0) == 0);
    SockT again = dial(r.port);
    CHECK(again != INVALID_SOCKET);
    CHECK(put(again, std::string(kHead) + "\r\n"));
    CHECK(drain(again, 800, closed).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(r.svr.refusedPending() == 2);
    closeS(again);
    for (SockT s : socks) { closeS(s); }
}

void testLoopbackIsNotCounted() {
    ConnLimits l;
    l.maxPendingPerAddress = 3;   // exemptLoopback stays on
    Rig r(l);
    std::vector<SockT> socks;
    for (int i = 0; i < 12; ++i) {
        socks.push_back(dial(r.port));
        CHECK(socks.back() != INVALID_SOCKET);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(r.svr.refusedPending() == 0);
    for (SockT s : socks) { closeS(s); }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testSlowHeadIsCutOff();
    testLaterRequestOfAKeptAliveConnectionIsBounded();
    testHonestClientsAreUnaffected();
    testPendingCapPerAddress();
    testLoopbackIsNotCounted();
    return testSummary("test_web_conn_guard");
}
