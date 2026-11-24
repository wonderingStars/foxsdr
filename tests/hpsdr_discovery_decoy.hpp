// hpsdr_discovery_decoy.hpp - a stand-in for a host that is NOT the radio the user typed (R3-11): it listens
// where the typed address points (127.0.0.1:<port>), and answers every datagram from ANOTHER address
// (127.0.0.2) with the bytes it is given, sent to the asker's own port. A discovery that believes any
// sender takes it for the radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "src_test_net.hpp"

namespace srctest {

class DiscoveryDecoy {
public:
    explicit DiscoveryDecoy(std::vector<std::uint8_t> reply) : reply_(std::move(reply)) {}
    ~DiscoveryDecoy() { stop(); }
    bool start() {
        startNet();
        listen_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        other_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listen_ == kBadSock || other_ == kBadSock) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { return false; }
        SockLen len = sizeof(a);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len) != 0) { return false; }
        port_ = ::ntohs(a.sin_port);
        sockaddr_in o{};
        o.sin_family = AF_INET;
        o.sin_addr.s_addr = ::htonl(0x7F000002u);  // 127.0.0.2
        o.sin_port = 0;
        if (::bind(other_, reinterpret_cast<sockaddr*>(&o), sizeof(o)) != 0) { return false; }
        run_.store(true);
        th_ = std::thread([this] {
            while (run_.load()) {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(listen_, &rf);
                timeval tv{};
                tv.tv_usec = 20000;
#if defined(_WIN32)
                const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
                const int r = ::select(listen_ + 1, &rf, nullptr, nullptr, &tv);
#endif
                if (r <= 0) { continue; }
                char buf[256];
                sockaddr_in from{};
                SockLen fl = sizeof(from);
                const int n = ::recvfrom(listen_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (n <= 0) { continue; }
                requests_.fetch_add(1);
                ::sendto(other_, reinterpret_cast<const char*>(reply_.data()), static_cast<int>(reply_.size()), 0,
                         reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            }
        });
        return true;
    }
    void stop() {
        run_.store(false);
        if (th_.joinable()) { th_.join(); }
        closeSock(listen_);
        closeSock(other_);
        listen_ = other_ = kBadSock;
    }
    std::uint16_t port() const { return port_; }
    int requests() const { return requests_.load(); }

private:
    std::vector<std::uint8_t> reply_;
    Sock listen_ = kBadSock, other_ = kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> requests_{0};
    std::thread th_;
};

}  // namespace srctest
