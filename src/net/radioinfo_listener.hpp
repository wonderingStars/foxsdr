// net/radioinfo_listener.hpp - the UDP listener for core/radioinfo_udp.hpp: OFF by default, 127.0.0.1 unless a wider address is switched on, receive only,
// no thread (poll() drains what is waiting). The default port N1MM Logger+ recommends is 12060 (the page opened); nothing here picks it: the port is a
// setting of the user, 0 means any free port.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

#include "core/radioinfo_udp.hpp"
#include "loopback_sock.hpp"

namespace cascade::net {

struct RadioInfoListenerConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    int port = 0;
    bool allowNonLoopback = false;
};

class RadioInfoListener {
public:
    RadioInfoListener() = default;
    ~RadioInfoListener() { stop(); }
    RadioInfoListener(const RadioInfoListener&) = delete;
    RadioInfoListener& operator=(const RadioInfoListener&) = delete;

    bool start(const RadioInfoListenerConfig& cfg, std::string& error) {
        stop();
        if (!cfg.enabled) { error = "the radio-info listener is switched off"; return false; }
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

    // Reads at most maxDatagrams waiting datagrams into the table; returns how many were read.
    int poll(core::rig::RadioInfoTable& table, std::int64_t nowUnix, int maxDatagrams = 64) {
        if (sock_ == lo::kBad) { return 0; }
        int n = 0;
        char buf[core::rig::kRadioInfoMaxDatagram + 1];
        while (n < maxDatagrams) {
            const int got = lo::udpRecv(sock_, buf, sizeof(buf));
            if (got == -1) { break; }
            ++n;
            if (got == -2 || got > static_cast<int>(core::rig::kRadioInfoMaxDatagram)) { ++refused_; continue; }
            core::rig::RadioInfo r;
            std::string err;
            if (core::rig::parseRadioInfo(buf, static_cast<std::size_t>(got), r, err)) { table.update(r, nowUnix); ++accepted_; } else { ++refused_; }
        }
        return n;
    }
    std::uint64_t accepted() const { return accepted_; }
    std::uint64_t refused() const { return refused_; }

private:
    lo::Sock sock_ = lo::kBad;
    int port_ = 0;
    std::uint64_t accepted_ = 0, refused_ = 0;
};

}  // namespace cascade::net
