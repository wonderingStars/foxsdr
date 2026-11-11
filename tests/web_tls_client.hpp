// web_tls_client.hpp - a TLS CLIENT written against Windows' own Schannel, for the tests of the web page's HTTPS (net/web_tls.hpp).
//
// It is written in the test, not borrowed from the code under test, so a defect in the terminator's Schannel use is not mirrored by a client
// that makes the same mistake. It validates nothing about the certificate chain (the certificate is self-signed): the tests read the peer's
// certificate back and compare its fingerprint with what the server published.
//
// Windows only; the including test is a no-op elsewhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <security.h>
#include <sspi.h>
#include <subauth.h>
#include <schannel.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "net/web_auth.hpp"

namespace web_test {

struct HttpReply {
    int status = 0;
    std::string head;
    std::string body;
    bool ok = false;
    std::string header(const std::string& name) const {
        std::string low = head;
        std::string key = "\r\n" + name + ":";
        for (char& c : low) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        for (char& c : key) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        const std::size_t p = low.find(key);
        if (p == std::string::npos) { return std::string(); }
        std::size_t b = p + key.size();
        while (b < head.size() && head[b] == ' ') { ++b; }
        const std::size_t e = head.find("\r\n", b);
        return head.substr(b, (e == std::string::npos ? head.size() : e) - b);
    }
};

class SchannelClient {
public:
    SchannelClient() = default;
    SchannelClient(const SchannelClient&) = delete;
    SchannelClient& operator=(const SchannelClient&) = delete;
    ~SchannelClient() { close(); }

    static bool wsa() {
        static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        return ok;
    }

    // TCP only (no TLS yet).
    bool connectTcp(const std::string& host, int port, int timeoutMs = 5000) {
        if (!wsa()) { return false; }
        close();
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr) { return false; }
        s_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s_ == INVALID_SOCKET) { freeaddrinfo(res); return false; }
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);
        const int c = ::connect(s_, res->ai_addr, static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        if (c != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closeSocket(); return false; }
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s_, &w);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, nullptr, &tv) <= 0) { closeSocket(); return false; }
        int err = 0;
        int el = sizeof(err);
        getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
        if (err != 0) { closeSocket(); return false; }
        return true;
    }

    // TLS handshake. `disabled` is a TLS_PARAMETERS::grbitDisabledProtocols mask (0 = whatever the machine offers).
    bool handshake(const std::string& serverName, DWORD disabled, std::string& error, int timeoutMs = 10000) {
        SCH_CREDENTIALS sc{};
        TLS_PARAMETERS tp{};
        tp.grbitDisabledProtocols = disabled;
        sc.dwVersion = SCH_CREDENTIALS_VERSION;
        sc.cTlsParameters = disabled != 0 ? 1 : 0;
        sc.pTlsParameters = disabled != 0 ? &tp : nullptr;
        sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        TimeStamp ts;
        SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr,
                                                       nullptr, &cred_, &ts);
        if (ss != SEC_E_OK) { error = "AcquireCredentialsHandle " + hex(ss); return false; }
        haveCred_ = true;
        target_ = serverName;
        std::wstring target(serverName.begin(), serverName.end());
        const ULONG flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
        std::vector<std::uint8_t> in;
        bool first = true;
        bool needMore = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (needMore) {
                if (!recvSome(in, deadline)) { error = "connection ended during the handshake"; return false; }
                needMore = false;
            }
            SecBuffer inb[2];
            inb[0] = {static_cast<ULONG>(in.size()), SECBUFFER_TOKEN, in.data()};
            inb[1] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc inD{SECBUFFER_VERSION, 2, inb};
            SecBuffer outb{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc outD{SECBUFFER_VERSION, 1, &outb};
            ULONG attr = 0;
            CtxtHandle fresh{};
            ss = InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_, const_cast<SEC_WCHAR*>(target.c_str()), flags, 0, 0, first ? nullptr : &inD,
                                            0, &fresh, &outD, &attr, &ts);
            if (first && (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)) { ctx_ = fresh; haveCtx_ = true; }
            first = false;
            if (outb.pvBuffer != nullptr) {
                if (outb.cbBuffer > 0) { sendRaw(static_cast<const std::uint8_t*>(outb.pvBuffer), outb.cbBuffer); }
                FreeContextBuffer(outb.pvBuffer);
            }
            if (ss == SEC_E_INCOMPLETE_MESSAGE) { needMore = true; continue; }
            if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK) {
                std::vector<std::uint8_t> rest;
                if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer > 0) { rest.assign(in.end() - inb[1].cbBuffer, in.end()); }
                if (ss == SEC_E_OK) {
                    cipher_ = std::move(rest);
                    QueryContextAttributes(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
                    secure_ = true;
                    return true;
                }
                in = std::move(rest);
                needMore = in.empty();
                continue;
            }
            error = "InitializeSecurityContext " + hex(ss);
            return false;
        }
    }

    bool secure() const { return secure_; }

    // The negotiated protocol as Schannel's SP_PROT_* word.
    DWORD protocol() {
        SecPkgContext_ConnectionInfo ci{};
        if (!haveCtx_ || QueryContextAttributes(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &ci) != SEC_E_OK) { return 0; }
        return ci.dwProtocol;
    }

    // The server certificate's SHA-256 (lower-case hex), as the client received it.
    std::string peerFingerprint() {
        PCCERT_CONTEXT cert = nullptr;
        if (!haveCtx_ || QueryContextAttributes(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || cert == nullptr) { return std::string(); }
        std::vector<std::uint8_t> d;
        std::string ignored;
        std::string out;
        if (cascade::net::sha256(cert->pbCertEncoded, cert->cbCertEncoded, d, ignored)) {
            static const char* hx = "0123456789abcdef";
            for (std::uint8_t b : d) { out.push_back(hx[b >> 4]); out.push_back(hx[b & 15]); }
        }
        CertFreeCertificateContext(cert);
        return out;
    }

    bool send(const std::string& text) { return send(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }
    bool send(const std::uint8_t* p, std::size_t n) {
        if (!secure_) { return sendRaw(p, n); }
        std::vector<std::uint8_t> buf;
        while (n > 0) {
            const std::size_t chunk = std::min<std::size_t>(n, sizes_.cbMaximumMessage);
            buf.assign(sizes_.cbHeader + chunk + sizes_.cbTrailer, 0);
            std::memcpy(buf.data() + sizes_.cbHeader, p, chunk);
            SecBuffer b[4];
            b[0] = {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()};
            b[1] = {static_cast<ULONG>(chunk), SECBUFFER_DATA, buf.data() + sizes_.cbHeader};
            b[2] = {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes_.cbHeader + chunk};
            b[3] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) { return false; }
            if (!sendRaw(buf.data(), static_cast<std::size_t>(b[0].cbBuffer) + b[1].cbBuffer + b[2].cbBuffer)) { return false; }
            p += chunk;
            n -= chunk;
        }
        return true;
    }

    // Appends up to whatever arrives within `timeoutMs` to `out`: returns the bytes added, 0 on a timeout, -1 when the peer closed or failed.
    int receive(std::string& out, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (secure_) {
                const int got = drain(out);
                if (got != 0) { return got; }
                if (closedByPeer_) { return -1; }
            } else if (!cipher_.empty()) {
                const std::size_t n = cipher_.size();
                out.append(reinterpret_cast<const char*>(cipher_.data()), n);
                cipher_.clear();
                return static_cast<int>(n);
            }
            std::vector<std::uint8_t> more;
            const int r = recvOnce(more, deadline);
            if (r < 0) { return -1; }
            if (r == 0) { return 0; }
            if (secure_) {
                cipher_.insert(cipher_.end(), more.begin(), more.end());
            } else {
                out.append(reinterpret_cast<const char*>(more.data()), more.size());
                return static_cast<int>(more.size());
            }
        }
    }

    // One HTTP exchange on this connection. `extra` is header lines each ending "\r\n".
    HttpReply request(const std::string& method, const std::string& path, const std::string& extra = std::string(), const std::string& body = std::string(),
                      int timeoutMs = 20000) {
        HttpReply r;
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: test\r\n" + extra;
        if (!body.empty() || method == "POST") { req += "Content-Length: " + std::to_string(body.size()) + "\r\n"; }
        req += "\r\n";
        if (!send(req) || (!body.empty() && !send(body))) { return r; }
        std::string raw;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::size_t headEnd = std::string::npos;
        while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline || receive(raw, 500) < 0) { return r; }
        }
        r.head = raw.substr(0, headEnd + 2);
        std::string rest = raw.substr(headEnd + 4);
        if (raw.size() > 12) { r.status = std::atoi(raw.c_str() + 9); }
        const std::string cl = r.header("Content-Length");
        if (!cl.empty()) {
            const std::size_t want = static_cast<std::size_t>(std::stoull(cl));
            while (rest.size() < want) {
                if (std::chrono::steady_clock::now() > deadline || receive(rest, 500) < 0) { break; }
            }
            r.body = rest.substr(0, want);
            r.ok = rest.size() >= want;
        } else if (r.header("Transfer-Encoding").find("chunked") != std::string::npos) {
            std::string out;
            std::size_t pos = 0;
            for (;;) {
                std::size_t eol;
                while ((eol = rest.find("\r\n", pos)) == std::string::npos) {
                    if (std::chrono::steady_clock::now() > deadline || receive(rest, 500) < 0) { return r; }
                }
                const std::size_t sz = std::stoull(rest.substr(pos, eol - pos), nullptr, 16);
                pos = eol + 2;
                if (sz == 0) { r.ok = true; break; }
                while (rest.size() < pos + sz + 2) {
                    if (std::chrono::steady_clock::now() > deadline || receive(rest, 500) < 0) { return r; }
                }
                out.append(rest, pos, sz);
                pos += sz + 2;
            }
            r.body = std::move(out);
        } else {
            r.body = rest;
            r.ok = true;
        }
        return r;
    }

    // Reads the de-chunked payload of a streaming reply until `bytes` have arrived (or the time runs out). Call after sending the request
    // and reading the head with request-like logic: here it does both, returning the payload gathered.
    std::string stream(const std::string& path, std::size_t bytes, const std::string& extra, int timeoutMs, std::string* headOut = nullptr) {
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: test\r\n" + extra + "\r\n";
        if (!send(req)) { return std::string(); }
        std::string raw;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::size_t headEnd;
        while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline || receive(raw, 500) < 0) { return std::string(); }
        }
        if (headOut != nullptr) { *headOut = raw.substr(0, headEnd + 2); }
        std::string rest = raw.substr(headEnd + 4);
        std::string out;
        std::size_t pos = 0;
        while (out.size() < bytes) {
            std::size_t eol;
            while ((eol = rest.find("\r\n", pos)) == std::string::npos) {
                if (std::chrono::steady_clock::now() > deadline || receive(rest, 500) < 0) { return out; }
            }
            const std::size_t sz = std::stoull(rest.substr(pos, eol - pos), nullptr, 16);
            pos = eol + 2;
            if (sz == 0) { return out; }
            while (rest.size() < pos + sz + 2) {
                if (std::chrono::steady_clock::now() > deadline || receive(rest, 500) < 0) { return out; }
            }
            out.append(rest, pos, sz);
            pos += sz + 2;
        }
        return out;
    }

    // Raw TCP access for tests that send bytes of their own.
    bool sendRawBytes(const std::string& b) { return sendRaw(reinterpret_cast<const std::uint8_t*>(b.data()), b.size()); }

    // Waits up to `timeoutMs` for the peer to close (or reset) the connection; true if it did, false if the connection is still open.
    bool peerClosedWithin(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            std::vector<std::uint8_t> more;
            const int r = recvOnce(more, deadline);
            if (r < 0) { return true; }
            if (r == 0) { return false; }
        }
    }

    // Raw bytes received without TLS processing (for the hand-made hello tests): everything that arrives within `timeoutMs`.
    std::string rawReceive(int timeoutMs) {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            std::vector<std::uint8_t> more;
            const int r = recvOnce(more, deadline);
            if (r <= 0) { break; }
            out.append(reinterpret_cast<const char*>(more.data()), more.size());
        }
        return out;
    }

    void close() {
        secure_ = false;
        if (haveCtx_) { DeleteSecurityContext(&ctx_); haveCtx_ = false; }
        if (haveCred_) { FreeCredentialsHandle(&cred_); haveCred_ = false; }
        closeSocket();
        cipher_.clear();
        closedByPeer_ = false;
    }

private:
    static std::string hex(long v) {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(v));
        return b;
    }

    void closeSocket() {
        if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; }
    }

    bool sendRaw(const std::uint8_t* p, std::size_t n) {
        while (n > 0) {
            const int r = ::send(s_, reinterpret_cast<const char*>(p), static_cast<int>(std::min<std::size_t>(n, 1 << 20)), 0);
            if (r > 0) { p += r; n -= static_cast<std::size_t>(r); continue; }
            if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set w;
                FD_ZERO(&w);
                FD_SET(s_, &w);
                timeval tv{5, 0};
                if (select(0, nullptr, &w, nullptr, &tv) <= 0) { return false; }
                continue;
            }
            return false;
        }
        return true;
    }

    // 1: bytes appended; 0: timed out; -1: closed or failed.
    int recvOnce(std::vector<std::uint8_t>& out, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { return 0; }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s_, &r);
            const int ms = static_cast<int>(std::min<long long>(left, 200));
            timeval tv{ms / 1000, (ms % 1000) * 1000};
            const int sel = select(0, &r, nullptr, nullptr, &tv);
            if (sel < 0) { return -1; }
            if (sel == 0) { continue; }
            std::uint8_t buf[16384];
            const int n = ::recv(s_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n == 0) { return -1; }
            if (n < 0) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { continue; }
                return -1;
            }
            out.insert(out.end(), buf, buf + n);
            return 1;
        }
    }

    // Feeds post-handshake handshake bytes to Schannel; what is left after them (application data) goes back into cipher_.
    bool continueHandshake(std::vector<std::uint8_t> in) {
        const ULONG flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
        const std::wstring target(target_.begin(), target_.end());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool needMore = in.empty();
        for (;;) {
            if (needMore) {
                if (!recvSome(in, deadline)) { return false; }
                needMore = false;
            }
            SecBuffer inb[2];
            inb[0] = {static_cast<ULONG>(in.size()), SECBUFFER_TOKEN, in.data()};
            inb[1] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc inD{SECBUFFER_VERSION, 2, inb};
            SecBuffer outb{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc outD{SECBUFFER_VERSION, 1, &outb};
            ULONG attr = 0;
            TimeStamp ts;
            const SECURITY_STATUS ss = InitializeSecurityContextW(&cred_, &ctx_, const_cast<SEC_WCHAR*>(target.c_str()), flags, 0, 0, &inD, 0, &ctx_,
                                                                  &outD, &attr, &ts);
            if (outb.pvBuffer != nullptr) {
                if (outb.cbBuffer > 0) { sendRaw(static_cast<const std::uint8_t*>(outb.pvBuffer), outb.cbBuffer); }
                FreeContextBuffer(outb.pvBuffer);
            }
            if (ss == SEC_E_INCOMPLETE_MESSAGE) { needMore = true; continue; }
            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
                std::vector<std::uint8_t> rest;
                if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer > 0) { rest.assign(in.end() - inb[1].cbBuffer, in.end()); }
                if (ss == SEC_E_OK) {
                    cipher_ = std::move(rest);
                    return true;
                }
                in = std::move(rest);
                needMore = in.empty();
                continue;
            }
            return false;
        }
    }

    bool recvSome(std::vector<std::uint8_t>& out, std::chrono::steady_clock::time_point deadline) { return recvOnce(out, deadline) > 0; }

    // Decrypts what is buffered; returns plaintext bytes added (0 when it needs more).
    int drain(std::string& out) {
        int added = 0;
        while (!cipher_.empty()) {
            SecBuffer b[4];
            b[0] = {static_cast<ULONG>(cipher_.size()), SECBUFFER_DATA, cipher_.data()};
            for (int i = 1; i < 4; ++i) { b[i] = {0, SECBUFFER_EMPTY, nullptr}; }
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS ss = DecryptMessage(&ctx_, &d, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) { break; }
            if (ss == SEC_I_CONTEXT_EXPIRED) { closedByPeer_ = true; cipher_.clear(); break; }
            if (ss == SEC_I_RENEGOTIATE) {
                // TLS 1.3 post-handshake messages (the server's session tickets) arrive like this: the data in EXTRA is for the handshake.
                std::vector<std::uint8_t> tok;
                for (int i = 1; i < 4; ++i) {
                    if (b[i].BufferType == SECBUFFER_EXTRA) {
                        const std::uint8_t* q = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                        tok.assign(q, q + b[i].cbBuffer);
                    }
                }
                cipher_.clear();
                if (!continueHandshake(tok)) { closedByPeer_ = true; break; }
                continue;
            }
            if (ss != SEC_E_OK) { closedByPeer_ = true; cipher_.clear(); break; }
            std::vector<std::uint8_t> rest;
            for (int i = 1; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_DATA) {
                    out.append(static_cast<const char*>(b[i].pvBuffer), b[i].cbBuffer);
                    added += static_cast<int>(b[i].cbBuffer);
                } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                    const std::uint8_t* q = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                    rest.assign(q, q + b[i].cbBuffer);
                }
            }
            cipher_ = std::move(rest);
        }
        return added;
    }

    SOCKET s_ = INVALID_SOCKET;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false;
    bool haveCtx_ = false;
    bool secure_ = false;
    bool closedByPeer_ = false;
    std::string target_;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<std::uint8_t> cipher_;
};

}  // namespace web_test
#endif  // _WIN32
