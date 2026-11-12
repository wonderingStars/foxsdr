// net/antenna_genius_tcp.hpp - the TCP transport for core/antenna_genius.hpp. OFF by default: nothing connects until the user has entered the device's
// address. The Antenna Genius is a LAN device, so a non-loopback address is its normal case: it is accepted only when the config says allowNonLoopback
// (a deliberate user setting; the tests never set it and connect only to 127.0.0.1 stand-ins). Port 9007 is the documented API port (the maker's wiki).
// Non-blocking, no thread: the client's poll() reads, the owner calls it from a worker. Nothing is printed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

#include "core/antenna_genius.hpp"
#include "loopback_sock.hpp"

namespace cascade::net {

struct GeniusTcpConfig {
    bool enabled = false;               // OFF by default
    std::string host;                   // dotted IPv4 of the device; empty until the user enters it
    int port = 9007;
    bool allowNonLoopback = false;      // the real device is on the LAN: a deliberate user setting
    int connectTimeoutMs = 1500;
};

class GeniusTcpTransport : public core::genius::Transport {
public:
    GeniusTcpTransport() = default;
    ~GeniusTcpTransport() override { close(); }
    GeniusTcpTransport(const GeniusTcpTransport&) = delete;
    GeniusTcpTransport& operator=(const GeniusTcpTransport&) = delete;

    bool open(const GeniusTcpConfig& cfg, std::string& error) {
        close();
        if (!cfg.enabled) { error = "the Antenna Genius connection is switched off"; return false; }
        std::uint32_t host = 0;
        if (!lo::parseIpv4(cfg.host, host)) { error = "the address must be a dotted IPv4 number"; return false; }
        if (!lo::isLoopbackHost(host) && !cfg.allowNonLoopback) { error = "a device address other than this machine needs the wider-address switch"; return false; }
        if (cfg.port < 1 || cfg.port > 65535) { error = "the port is out of range"; return false; }
        if (cfg.connectTimeoutMs < 1 || cfg.connectTimeoutMs > 10000) { error = "the connect timeout must be 1 to 10000 ms"; return false; }
        sock_ = lo::tcpConnect(host, cfg.port, cfg.connectTimeoutMs);
        if (sock_ == lo::kBad) { error = "could not connect"; return false; }
        return true;
    }
    void close() { lo::closeSock(sock_); sock_ = lo::kBad; }
    bool isOpen() const { return sock_ != lo::kBad; }

    int send(const char* data, std::size_t len) override {
        if (sock_ == lo::kBad) { return -1; }
        const int n = lo::tcpSend(sock_, data, len);
        return n == static_cast<int>(len) ? n : -1;   // a command that did not go out whole is a failure: the line protocol cannot resume mid-command
    }
    int recv(char* buf, std::size_t cap) override {
        if (sock_ == lo::kBad) { return -1; }
        return lo::tcpRecv(sock_, buf, cap);
    }

private:
    lo::Sock sock_ = lo::kBad;
};

}  // namespace cascade::net
