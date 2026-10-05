// Fuzz target: the web remote's HTTP front door, over a real loopback socket.
//
// The web server speaks plain HTTP to whatever connects: a browser on the same
// machine, or - when the user opens it to the LAN - anything on the network.
// Everything in front of parseControlRequest is cpp-httplib's request parser
// and this program's route handlers (the status and spectrum JSON, the cookie
// and login handling, the control endpoint, the tile and image routes). This
// target starts the REAL server once, on a loopback port, with no password set,
// and sends each input to it as the raw bytes of a connection - so the HTTP
// parser sees the input exactly as a hostile client would write it, including
// pipelined, truncated and oversized requests.
//
// The server runs in the fuzzer's own process, so AddressSanitizer covers the
// handler threads as well as the caller.
//
// Properties: the server answers a plain status request afterwards (checked
// every sixty-fourth input) - a request that wedges or kills the listener is a
// denial of service for the web remote.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Same TU-level definitions every translation unit that includes httplib.h on a
// non-Windows host must agree on (see test_web_server.cpp).
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif

#ifdef _WIN32
// winsock2.h before windows.h, as everywhere in this tree.
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using fuzz_socket_t = SOCKET;
constexpr fuzz_socket_t kFuzzInvalidSocket = INVALID_SOCKET;
inline void fuzzClose(fuzz_socket_t s) { ::closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using fuzz_socket_t = int;
constexpr fuzz_socket_t kFuzzInvalidSocket = -1;
inline void fuzzClose(fuzz_socket_t s) { ::close(s); }
#endif

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "fuzz_common.hpp"
#include "net/web_server.hpp"

using namespace cascade::net;

namespace {

RadioStatus sampleStatus() {
    RadioStatus s;
    s.running = true;
    s.centerHz = 100000000.0;
    s.sampleRateHz = 2000000.0;
    s.vfoOffsetHz = 300000.0;
    s.bandwidthHz = 150000.0;
    s.mode = "WFM";
    s.sourceName = "SigGen";
    return s;
}

struct Fixture {
    WebServer server;
    int port = -1;

    Fixture() {
#ifdef _WIN32
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        server.setStatusProvider([]() { return sampleStatus(); });
        server.setSpectrumProvider([](SpectrumSnapshot&) { return false; });
        WebServerConfig cfg;
        cfg.enabled = true;
        cfg.bindAddress = "127.0.0.1";
        cfg.username = "admin";
        std::string error;
        // A range of ports from the process id, so concurrent fuzzers and a
        // replay do not fight for one.
        const int base = 20000 + (FUZZ_GETPID() % 20000);
        for (int p = base; p < base + 100; ++p) {
            cfg.port = p;
            if (server.start(cfg, error)) {
                port = p;
                break;
            }
            if (!server.decision().allowed()) { break; }
        }
    }
    ~Fixture() { server.stop(); }
};

Fixture& fixture() {
    static Fixture f;
    return f;
}

// Why the last connect() failed, for the message when the server stops answering.
int g_lastConnectError = 0;

// Sends `bytes` as one connection, half-closes, and reads the answer to its end
// (or to a short timeout, or a size cap). Returns what was read.
std::string exchange(int port, const std::uint8_t* bytes, std::size_t n) {
    std::string answer;
    const fuzz_socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kFuzzInvalidSocket) { return answer; }
#ifdef _WIN32
    DWORD ms = 500;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 500 * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    // A different loopback source address for each run of connections (all of
    // 127.0.0.0/8 is this machine). A connection the server closes first leaves
    // its address pair in TIME_WAIT for minutes, and at thousands of inputs a
    // second one the ~16,000 ephemeral ports of one source address run out
    // within seconds on Windows: connect() then fails and the fuzzer sees a
    // "dead server" that is only an exhausted client. Each source address has
    // its own set of ports.
    static unsigned source = 0;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(0x7F000000u | (2u + (source++ % 200u)));
    (void)::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        g_lastConnectError = ::WSAGetLastError();
#else
        g_lastConnectError = errno;
#endif
        fuzzClose(s);
        return answer;
    }
    g_lastConnectError = 0;
    // MSG_NOSIGNAL: the server closes early on a request it refuses (an oversized
    // header, say) while the rest is still being written, and on Linux a send to
    // a closed peer is a SIGPIPE that would end the fuzzer, not a failed send.
#ifdef MSG_NOSIGNAL
    const int sendFlags = MSG_NOSIGNAL;
#else
    const int sendFlags = 0;
#endif
    std::size_t sent = 0;
    while (sent < n) {
        const int r = static_cast<int>(::send(s, reinterpret_cast<const char*>(bytes) + sent,
                                              static_cast<int>(n - sent), sendFlags));
        if (r <= 0) { break; }
        sent += static_cast<std::size_t>(r);
    }
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
    char buf[4096];
    while (answer.size() < (1u << 20)) {
        const int r = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (r <= 0) { break; }
        answer.append(buf, static_cast<std::size_t>(r));
    }
    fuzzClose(s);
    return answer;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Fixture& f = fixture();
    FUZZ_REQUIRE(f.port > 0);
    (void)exchange(f.port, data, size);

    // Every sixty-fourth input, ask the server something plain. A few tries,
    // because an input that opened a streaming route (the audio feed) leaves a
    // worker busy until its peer is noticed to be gone: slow is not wedged. Only
    // a server that gives no 200 for two seconds has been taken out.
    static unsigned count = 0;
    if ((++count & 63u) == 0) {
        static const char kPing[] =
            "GET /api/status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string reply;
        for (int attempt = 0; attempt < 8; ++attempt) {
            reply = exchange(f.port, reinterpret_cast<const std::uint8_t*>(kPing), sizeof(kPing) - 1);
            if (reply.rfind("HTTP/1.1 200", 0) == 0) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (reply.rfind("HTTP/1.1 200", 0) != 0) {
            std::fprintf(stderr,
                         "web server did not answer a plain status request; last reply: [%.200s] "
                         "connect error %d, server running %d\n",
                         reply.c_str(), g_lastConnectError, f.server.running() ? 1 : 0);
        }
        FUZZ_REQUIRE(reply.rfind("HTTP/1.1 200", 0) == 0);
    }
    return 0;
}
