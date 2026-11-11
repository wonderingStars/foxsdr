// net/xplane_listener.hpp - the UDP listener that feeds core/xplane_udp.hpp: OFF by default, bound to 127.0.0.1 unless a wider address is
// switched on deliberately. Receive only: it never sends a datagram (so it never asks X-Plane for anything; the user points X-Plane's data
// output at this port). No thread: poll() drains what is waiting and returns, called by the owner on a worker thread.
//
// PRIVACY: positions of simulated aircraft are not personal data of a person on the air, but nothing here prints or stores anything; the tracks live
// in the feed object in memory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

#include "core/xplane_udp.hpp"
#include "loopback_sock.hpp"

namespace cascade::net {

struct XplaneListenerConfig {
    bool enabled = false;               // OFF by default
    std::string host = "127.0.0.1";     // dotted IPv4 only
    int port = 0;                       // 0 = a free port (read it with port()); otherwise the port X-Plane's data output is pointed at
    bool allowNonLoopback = false;      // a deliberate user setting; tests never set it
};

class XplaneListener {
public:
    XplaneListener() = default;
    ~XplaneListener() { stop(); }
    XplaneListener(const XplaneListener&) = delete;
    XplaneListener& operator=(const XplaneListener&) = delete;

    bool start(const XplaneListenerConfig& cfg, std::string& error) {
        stop();
        if (!cfg.enabled) { error = "the X-Plane listener is switched off"; return false; }
        std::uint32_t host = 0;
        if (!lo::parseIpv4(cfg.host, host)) { error = "the address must be a dotted IPv4 number"; return false; }
        if (!lo::isLoopbackHost(host) && !cfg.allowNonLoopback) { error = "only the local machine (127.x.x.x) is allowed unless a wider address is switched on"; return false; }
        if (cfg.port < 0 || cfg.port > 65535) { error = "the port is out of range"; return false; }
        sock_ = lo::udpOpen(host, cfg.port);
        if (sock_ == lo::kBad) { error = "could not open the UDP port"; return false; }
        port_ = lo::boundPortOf(sock_);
        return true;
    }
    void stop() { lo::closeSock(sock_); sock_ = lo::kBad; port_ = 0; }
    bool running() const { return sock_ != lo::kBad; }
    int port() const { return port_; }

    // Reads at most `maxDatagrams` waiting datagrams into the feed; returns how many were read. A datagram longer than the buffer is counted in
    // oversized() and not fed (a longer one cannot be a valid X-Plane packet).
    int poll(core::xplane::XplaneFeed& feed, std::int64_t nowUnix, int maxDatagrams = 64) {
        if (sock_ == lo::kBad) { return 0; }
        int n = 0;
        std::uint8_t buf[core::xplane::kMaxDatagram + 1];
        while (n < maxDatagrams) {
            const int got = lo::udpRecv(sock_, buf, sizeof(buf));
            if (got == -1) { break; }
            ++n;
            if (got == -2) { ++oversized_; continue; }
            if (static_cast<std::size_t>(got) > core::xplane::kMaxDatagram) { ++oversized_; continue; }
            feed.feed(buf, static_cast<std::size_t>(got), nowUnix);
        }
        return n;
    }
    std::uint64_t oversized() const { return oversized_; }

private:
    lo::Sock sock_ = lo::kBad;
    int port_ = 0;
    std::uint64_t oversized_ = 0;
};

}  // namespace cascade::net
