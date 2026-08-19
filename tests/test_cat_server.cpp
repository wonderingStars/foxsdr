// Tests for net/cat_server.hpp — the CAT server driven over a REAL socket.
//
// cat_protocol is already tested as a pure function, so nothing here re-checks
// what a command means. What this file exists to prove is everything the pure
// tests structurally cannot: that a socket is actually listening, that
// requests split across TCP segments are reassembled, that several commands
// arriving in one segment are all answered, that set requests reach the queue
// the GUI thread drains, and that the connection cap and the line cap hold.
//
// A mock cannot show any of that, and an I/O feature verified only against a
// mock is a hypothesis rather than a result.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_server.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include "test_check.hpp"

using cascade::net::CatServer;
using cascade::net::ControlRequest;
using cascade::net::RadioStatus;

namespace {

RadioStatus g_status;

RadioStatus provider() { return g_status; }

void closeClient(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) ::closesocket(s);
#else
    if (s != INVALID_SOCKET) ::close(s);
#endif
}

SockT connectTo(int port) {
#if defined(_WIN32)
    WSADATA d{};
    ::WSAStartup(MAKEWORD(2, 2), &d);
#endif
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    // A receive timeout, or every "nothing should arrive" assertion below
    // blocks for ever instead of failing: recv() on a healthy but silent
    // socket does not return, so the deadline in readLines would never be
    // reached. This is what makes those cases testable at all.
#if defined(_WIN32)
    DWORD tv = 250;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                 sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeClient(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool sendRaw(SockT s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(
            ::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads until `lines` newlines have arrived or the deadline passes. Counting
// lines rather than bytes is what makes this robust to segmentation: the point
// of several tests here is that the reply may not arrive in one piece.
std::string readLines(SockT s, int lines, int timeoutMs = 3000) {
    std::string out;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int have = 0;
        for (char c : out) {
            if (c == '\n') ++have;
        }
        if (have >= lines) break;
        char buf[256];
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;  // peer closed
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return out;
}

int startOnFreePort(CatServer& server, std::string& error) {
    for (int port = 18533; port < 18633; ++port) {
        if (server.start(static_cast<std::uint16_t>(port), /*bindAll=*/false, error)) {
            return port;
        }
    }
    return -1;
}

void resetStatus() {
    g_status = RadioStatus{};
    g_status.running = true;
    g_status.centerHz = 145'000'000.0;
    g_status.sampleRateHz = 2'400'000.0;
    g_status.vfoOffsetHz = 0.0;
    g_status.bandwidthHz = 12'500.0;
    g_status.mode = "NFM";
}

// --- the tests -------------------------------------------------------------

void testRefusesToStartWithoutAProvider() {
    CatServer server;
    std::string error = "stale";
    // No status provider: every command would have to invent a frequency, so
    // starting at all is refused rather than serving fiction.
    CHECK(!server.start(18999, false, error));
    CHECK(!error.empty());
    CHECK(!server.running());
}

void testRoundTripOverARealSocket() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    CHECK(port > 0);
    if (port < 0) {
        std::printf("SKIP: no free port for the CAT server (%s)\n", error.c_str());
        return;
    }
    CHECK(server.running());

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) {
        server.stop();
        return;
    }

    // A get returns its value and nothing else.
    CHECK(sendRaw(c, "f\n"));
    CHECK(readLines(c, 1) == "145000000\n");

    // Two lines for mode.
    CHECK(sendRaw(c, "m\n"));
    CHECK(readLines(c, 2) == "FM\n12500\n");

    // A set answers RPRT 0 and queues a control.
    CHECK(sendRaw(c, "F 145500000\n"));
    CHECK(readLines(c, 1) == "RPRT 0\n");

    const std::vector<ControlRequest> queued = server.takePendingControls();
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u) {
        CHECK(queued[0].vfoOffsetHz.has_value());
        // Taking them again must yield nothing: a control applied twice would
        // fight whatever the user did in between.
        CHECK(server.takePendingControls().empty());
    }

    CHECK(server.commandCount() >= 3u);

    closeClient(c);
    server.stop();
    CHECK(!server.running());
}

void testCommandSplitAcrossSegmentsIsReassembled() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // TCP is a byte stream, not a message stream: a client is entitled to send
    // one command in three writes, and a server that assumed one read equals
    // one command would answer RPRT -4 to each fragment.
    CHECK(sendRaw(c, "F 14"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sendRaw(c, "6000"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sendRaw(c, "000\n"));

    CHECK(readLines(c, 1) == "RPRT 0\n");
    const std::vector<ControlRequest> queued = server.takePendingControls();
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u) {
        CHECK(queued[0].vfoOffsetHz.has_value());
        if (queued[0].vfoOffsetHz.has_value()) {
            // 146.0 MHz from a 145.0 MHz centre: 1 MHz, inside the usable band.
            CHECK(*queued[0].vfoOffsetHz > 999'000.0);
            CHECK(*queued[0].vfoOffsetHz < 1'001'000.0);
        }
    }

    closeClient(c);
    server.stop();
}

void testSeveralCommandsInOneSegment() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // The mirror case: three commands in one write must produce three replies
    // in order, not just the first. Clients pipeline exactly like this.
    CHECK(sendRaw(c, "f\nt\nv\n"));
    const std::string reply = readLines(c, 3);
    CHECK(reply == "145000000\n0\nVFOA\n");

    closeClient(c);
    server.stop();
}

void testQuitClosesTheConnection() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    CHECK(sendRaw(c, "q\n"));
    // The server hangs up rather than replying, so a read returns nothing.
    char buf[64];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    int n = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
        if (n <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(n == 0);  // orderly close, not data

    closeClient(c);
    server.stop();
}

void testOverlongLineIsRefusedRatherThanBuffered() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // A client that never sends a newline must not be able to grow the
    // server's buffer without bound — that is a memory leak driven from the
    // network. The server answers once and hangs up.
    const std::string flood(cascade::net::kMaxCatLineBytes + 256, 'x');
    sendRaw(c, flood);
    const std::string reply = readLines(c, 1);
    CHECK(reply == "RPRT -1\n");

    closeClient(c);
    server.stop();
}

void testIdleConnectionSurvives() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    CHECK(sendRaw(c, "f\n"));
    CHECK(readLines(c, 1) == "145000000\n");

    // THE CASE EVERY OTHER TEST HERE MISSES. All of them send their next
    // command immediately, so they collectively prove the server works for a
    // BUSY client and say nothing about an idle one — and a CAT client is idle
    // almost all of the time, polling a frequency every second or two.
    //
    // The listener carries a short receive timeout so accept() can notice a
    // stop request, and on Linux an accepted socket INHERITS it. A timeout
    // surfaces as recv() returning -1, which reads identically to a closed
    // connection, so the server hung up on healthy clients the moment they
    // paused. This waits several times that timeout and then asks again.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    CHECK(sendRaw(c, "f\n"));
    const std::string afterIdle = readLines(c, 1);
    CHECK(afterIdle == "145000000\n");
    // And the server must still count it as one connected client, not have
    // silently reaped it.
    CHECK(server.clientCount() == 1);

    closeClient(c);
    server.stop();
}

void testConnectionCap() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    std::vector<SockT> held;
    for (int i = 0; i < cascade::net::kMaxCatClients; ++i) {
        SockT s = connectTo(port);
        CHECK(s != INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            // Force the server to actually service the connection so its count
            // is up to date before the next one is attempted.
            sendRaw(s, "f\n");
            readLines(s, 1);
            held.push_back(s);
        }
    }
    CHECK(server.clientCount() == cascade::net::kMaxCatClients);

    // One more is accepted by the OS and then dropped by the server, so the
    // client sees an immediate close rather than a hang.
    SockT extra = connectTo(port);
    if (extra != INVALID_SOCKET) {
        sendRaw(extra, "f\n");
        const std::string reply = readLines(extra, 1, 1500);
        CHECK(reply.empty());
        closeClient(extra);
    }

    for (SockT s : held) closeClient(s);
    server.stop();
}

void testStopIsIdempotentAndSafeWithNoClients() {
    resetStatus();
    CatServer server;
    server.setStatusProvider(provider);
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }
    CHECK(server.running());
    server.stop();
    CHECK(!server.running());
    server.stop();  // a second stop must not deadlock or crash
    CHECK(!server.running());
}

}  // namespace

int main() {
    testRefusesToStartWithoutAProvider();
    testRoundTripOverARealSocket();
    testCommandSplitAcrossSegmentsIsReassembled();
    testSeveralCommandsInOneSegment();
    testQuitClosesTheConnection();
    testOverlongLineIsRefusedRatherThanBuffered();
    testIdleConnectionSurvives();
    testConnectionCap();
    testStopIsIdempotentAndSafeWithNoClients();
    return testSummary("test_cat_server");
}
