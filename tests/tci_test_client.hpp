// tci_test_client.hpp - the sockets and the raw WebSocket client the TCI tests share (tests/test_tci_server.cpp, tests/test_tci_engine.cpp):
// loopback connections, masked client frames, a reader of the server's unmasked frames. Include it ONCE per test, after the socket headers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace {

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void closeSock(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}

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

SockT connectTo(const char* ip, int port) {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return INVALID_SOCKET; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closeSock(s);
        return INVALID_SOCKET;
    }
    return s;
}

void setRecvTimeout(SockT s, int ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAll(SockT s, const std::string& d) {
    std::size_t off = 0;
    while (off < d.size()) {
        const int n = static_cast<int>(::send(s, d.data() + off, static_cast<int>(d.size() - off), 0));
        if (n <= 0) { return false; }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// A free loopback port: bound, read back, closed.
int freePort() {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
    closeSock(s);
    return port;
}

// ---- a raw WebSocket client (masked frames out, unmasked frames in) -------------------------------------------------------------------

std::string maskedFrame(ws::Opcode op, const std::string& payload, bool fin = true, bool mask = true, unsigned rsv = 0) {
    const unsigned char k[4] = {0x12, 0x34, 0x56, 0x78};
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | (rsv << 4) | static_cast<unsigned>(op)));
    const unsigned m = mask ? 0x80u : 0u;
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(m | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        f.push_back(static_cast<char>(m | 127));
        for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF)); }
    }
    if (mask) {
        f.append(reinterpret_cast<const char*>(k), 4);
        for (std::size_t i = 0; i < payload.size(); ++i) { f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ k[i & 3])); }
    } else {
        f += payload;
    }
    return f;
}

struct ServerMsg {
    ws::Opcode op;
    std::string payload;
};

struct RawClient {
    SockT s = INVALID_SOCKET;
    std::string buf;
    std::string httpHead;   // the response head of the handshake

    ~RawClient() { closeSock(s); }
    bool open(int port, const char* ip = "127.0.0.1") {
        s = connectTo(ip, port);
        if (s != INVALID_SOCKET) { setRecvTimeout(s, 200); }
        return s != INVALID_SOCKET;
    }
    bool handshake() {
        const std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(s, req)) { return false; }
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end) {
            const std::size_t at = buf.find("\r\n\r\n");
            if (at != std::string::npos) {
                httpHead = buf.substr(0, at + 4);
                buf.erase(0, at + 4);
                return httpHead.rfind("HTTP/1.1 101", 0) == 0 && httpHead.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
            }
            fill();
        }
        return false;
    }
    // Reads once; false when the peer has closed (or failed).
    bool fill() {
        char b[8192];
        const int n = static_cast<int>(::recv(s, b, sizeof(b), 0));
        if (n > 0) { buf.append(b, static_cast<std::size_t>(n)); return true; }
        if (n == 0) { return false; }
#if defined(_WIN32)
        const int e = ::WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }
    // One server frame, within `ms`; false on timeout or close.
    bool next(ServerMsg& m, int ms = 2000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (buf.size() >= 2) {
                const unsigned char b0 = static_cast<unsigned char>(buf[0]);
                std::uint64_t len = static_cast<unsigned char>(buf[1]) & 0x7Fu;
                std::size_t hdr = 2;
                if (len == 126 && buf.size() >= 4) { len = (static_cast<std::uint64_t>(static_cast<unsigned char>(buf[2])) << 8) | static_cast<unsigned char>(buf[3]); hdr = 4; }
                else if (len == 127 && buf.size() >= 10) { len = 0; for (int i = 0; i < 8; ++i) { len = (len << 8) | static_cast<unsigned char>(buf[2 + i]); } hdr = 10; }
                else if (len >= 126) { hdr = 0; }
                if (hdr != 0 && buf.size() >= hdr + len) {
                    m.op = static_cast<ws::Opcode>(b0 & 0x0F);
                    m.payload = buf.substr(hdr, static_cast<std::size_t>(len));
                    buf.erase(0, hdr + static_cast<std::size_t>(len));
                    return true;
                }
            }
            if (Clock::now() > end) { return false; }
            if (!fill()) { return false; }
        }
    }
    // Text messages until one equals `want` (or timeout); the others are collected in `seen`.
    bool waitText(const std::string& want, std::vector<std::string>* seen = nullptr, int ms = 3000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        ServerMsg m;
        while (Clock::now() < end) {
            if (!next(m, 200)) { continue; }
            if (m.op == ws::Opcode::Text) {
                if (seen != nullptr) { seen->push_back(m.payload); }
                if (m.payload == want) { return true; }
            }
        }
        return false;
    }
    bool sendText(const std::string& t) { return sendAll(s, maskedFrame(ws::Opcode::Text, t)); }
    // True when the server closes the connection within `ms` (a close frame or a plain end).
    bool closedWithin(int ms, std::uint16_t* code = nullptr) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        ServerMsg m;
        for (;;) {
            if (Clock::now() > end) { return false; }
            if (next(m, 100)) {
                if (m.op == ws::Opcode::Close && code != nullptr && m.payload.size() >= 2) {
                    *code = static_cast<std::uint16_t>((static_cast<unsigned char>(m.payload[0]) << 8) | static_cast<unsigned char>(m.payload[1]));
                }
                continue;
            }
            char b[16];
            const int n = static_cast<int>(::recv(s, b, sizeof(b), 0));
            if (n == 0) { return true; }
            if (n < 0) {
#if defined(_WIN32)
                const int e = ::WSAGetLastError();
                if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK) { return true; }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { return true; }
#endif
            }
        }
    }
};

}  // namespace
