// HTTPS for the web page (feat/web-secure): a TLS terminator built on Windows' own Schannel (SSPI), with no TLS library of ours.
//
// WHY A TERMINATOR IN FRONT OF THE PLAIN SERVER AND NOT TLS INSIDE IT. The vendored cpp-httplib can only do TLS through OpenSSL, which this
// product does not ship (third_party/THIRD_PARTY.md). What Windows has is Schannel, and httplib cannot be pointed at it. So the plain server
// stays exactly the server that has been tested for months, bound to 127.0.0.1 on a private port that nothing off the machine can reach, and
// this class owns the PUBLIC port: it accepts, runs the TLS handshake with AcceptSecurityContext, and relays the decrypted bytes to the plain
// server over a loopback connection (EncryptMessage / DecryptMessage). While HTTPS is on, plain HTTP is never offered to the network.
//
// THE TRUE CLIENT ADDRESS. The plain server sees every relayed connection come from 127.0.0.1, which would make the sign-in limiter count the
// whole network as one client and the ban list ban the machine itself. So nothing is passed in a header (a header is the CLIENT's to write): the
// terminator records, for each loopback connection it opens, the connection's own source port against the real peer address it accepted
// from, and the server asks peerOf(req.remote_port). The source port of a loopback connection is chosen by the operating system for the
// terminator's socket; a remote client has no way to reach it, and a request that carries "X-Forwarded-For" or anything like it is read as
// what it is - a header nobody trusts. A connection the table does not know (a program on THIS machine talking to the private port directly)
// reads as 127.0.0.1, which is what it is.
//
// WHAT IT REFUSES AND DROPS. TLS 1.0 and 1.1 (and SSL) are switched off in the credential; 1.2 is the minimum and 1.3 is used where Windows
// offers it. A client that does not finish the handshake within the handshake limit, or says nothing for the idle limit, or stops reading
// for the write limit, is dropped; the number of connections is capped, in total and per address. A renegotiation request is a close.
//
// THE CERTIFICATE. Either one the user picks by thumbprint (found in the Personal store of the current user or the machine; it must have a
// private key), or a self-signed one made here with CNG: ECDSA P-256, a subject alternative name for each name and address the page is
// reachable by, server-authentication usage, and a private key that is NOT EXPORTABLE. Its SHA-256 fingerprint (of the DER) is published so a
// person can compare it with what the browser shows before clicking through the self-signed warning.
//
// PLATFORM. Windows only. On any other platform available() is false and every call fails with a plain sentence ("UNPROVEN on Linux"):
// nothing here has been built or run there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cascade::net {

struct TlsOptions {
    std::string thumbprint;           // 40 hex digits (SHA-1, as Windows shows it); empty = make a self-signed certificate
    std::string certFile;             // self-signed: where the public certificate is kept (empty = memory only, new at every start)
    std::string keyName = "FoxSDR-Web-TLS";   // self-signed with certFile: the key's name in the user's key store
    std::vector<std::string> names;   // self-signed: extra DNS names and IP literals for the subject alternative name
    bool includeMachineNames = true;  // self-signed: this computer's name, "localhost", 127.0.0.1, ::1 and every LAN IPv4 address
};

struct TlsTimeouts {
    int handshakeMs = 10000;   // the whole handshake
    int idleMs = 30000;        // no byte either way
    int writeMs = 10000;       // the client will not take what is sent
};

struct TlsStats {
    std::uint64_t accepted = 0;
    std::uint64_t handshakesOk = 0;
    std::uint64_t handshakesFailed = 0;     // a bad protocol version, a client that hung up, a garbage hello
    std::uint64_t tls13 = 0;                // handshakes that ended on TLS 1.3
    std::uint64_t tls12 = 0;                // ... on TLS 1.2
    std::uint64_t droppedHandshakeTimeout = 0;
    std::uint64_t droppedIdle = 0;
    std::uint64_t droppedWriteStall = 0;
    std::uint64_t refusedBusy = 0;          // over the connection cap
};

class TlsTerminator {
public:
    TlsTerminator();
    ~TlsTerminator();
    TlsTerminator(const TlsTerminator&) = delete;
    TlsTerminator& operator=(const TlsTerminator&) = delete;

    // True where this is built (Windows).
    static bool available();

    // Makes or finds the certificate and acquires the Schannel credential. Nothing listens yet. False with `error` set (a sentence).
    bool prepare(const TlsOptions& opt, std::string& error);

    // Binds `bindAddress:port` (port 0 = any), starts accepting, and relays each connection to 127.0.0.1:backendPort. prepare() first.
    bool start(const std::string& bindAddress, int port, int backendPort, std::string& error);
    void stop();                       // returns once every thread has ended; prompt (a fraction of a second)
    bool running() const;
    int boundPort() const;

    // Timeouts and caps: before start().
    void setTimeouts(const TlsTimeouts& t);
    void setMaxConnections(std::size_t total, std::size_t perAddress);

    // The certificate, once prepared.
    std::string fingerprintSha256() const;        // 64 lower-case hex digits of the DER
    std::string thumbprintSha1() const;           // 40 lower-case hex digits, as Windows shows it
    std::vector<std::string> certificateNames() const;   // the subject alternative names (self-signed ones; empty for a picked one)
    std::int64_t certificateNotAfter() const;     // seconds since the epoch
    bool selfSigned() const;
    bool keyExportable() const;                   // always false for what this makes; read back from the key for a picked certificate
    std::vector<std::uint8_t> certificateDer() const;

    // The real client address of the relayed connection whose loopback source port is `backendSourcePort`; empty when unknown.
    std::string peerOf(int backendSourcePort) const;
    std::size_t connections() const;
    TlsStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::net
