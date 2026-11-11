// udp_sink.cpp - see udp_sink.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/udp_sink.hpp"

#include <algorithm>

namespace cascade::net {

UdpSink::UdpSink(const UdpConfig& cfg) : cfg_(cfg), ring_(std::max<std::size_t>(cfg.ringBytes, 4096)) {}

UdpSink::~UdpSink() { stop(); }

bool UdpSink::start(std::string& error) {
    if (running_.load()) {
        error = "already running";
        return false;
    }
    if (cfg_.port == 0) {
        error = "destination port must be 1..65535";
        return false;
    }
    if (cfg_.maxDatagram < 16 || cfg_.maxDatagram > kUdpMaxDatagram) {
        error = "datagram size must be between 16 and " + std::to_string(kUdpMaxDatagram) + " bytes";
        return false;
    }
    if (!sock::parseIpv4(cfg_.destAddress, ip_)) {
        error = "'" + cfg_.destAddress + "' is not an IPv4 address (dotted quad, for example 127.0.0.1)";
        return false;
    }
    if (ip_ == 0) {
        error = "0.0.0.0 is not a destination";
        return false;
    }
    const bool broadcast = ip_ == 0xFFFFFFFFu;
    const bool multicast = (ip_ >> 28) == 0xE;
    if ((broadcast || multicast) && !cfg_.allowBroadcast) {
        error = "broadcast and multicast destinations are refused unless they are asked for explicitly";
        return false;
    }
    std::vector<std::uint8_t> sink(ring_.maxMessage());
    while (ring_.pop(sink.data(), sink.size()) != 0) {}
    sock_ = sock::openUdp(cfg_.allowBroadcast, error);
    if (sock_ == sock::kInvalid) { return false; }
    if (cfg_.socketSendBuffer > 0) { sock::setBuffers(sock_, 0, cfg_.socketSendBuffer); }
    stopFlag_.store(false);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void UdpSink::stop() {
    if (!thread_.joinable()) { return; }
    stopFlag_.store(true);
    thread_.join();
    running_.store(false, std::memory_order_release);
}

bool UdpSink::send(const void* data, std::size_t n) noexcept {
    if (!running_.load(std::memory_order_acquire)) { return false; }
    if (n == 0 || n > cfg_.maxDatagram) {
        oversize_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return ring_.push(data, n);
}

UdpStats UdpSink::stats() const {
    UdpStats s;
    s.sent = sent_.load();
    s.bytesSent = bytesSent_.load();
    s.ringDropped = ring_.dropped();
    s.socketDropped = socketDropped_.load();
    s.sendErrors = sendErrors_.load();
    s.oversize = oversize_.load() + ring_.oversize();
    return s;
}

void UdpSink::run() {
    std::vector<std::uint8_t> msg(ring_.maxMessage());
    while (!stopFlag_.load(std::memory_order_acquire)) {
        bool any = false;
        for (;;) {
            const std::size_t n = ring_.pop(msg.data(), msg.size());
            if (n == 0) { break; }
            any = true;
            const long r = sock::sendTo(sock_, msg.data(), n, ip_, cfg_.port);
            if (r == static_cast<long>(n)) {
                sent_.fetch_add(1, std::memory_order_relaxed);
                bytesSent_.fetch_add(n, std::memory_order_relaxed);
            } else if (r < 0 && sock::wouldBlock()) {
                socketDropped_.fetch_add(1, std::memory_order_relaxed);
            } else {
                sendErrors_.fetch_add(1, std::memory_order_relaxed);
            }
            if (stopFlag_.load(std::memory_order_acquire)) { break; }
        }
        if (!any) { sock::pollMany(nullptr, 0, 1); }  // idle: a one-millisecond nap
    }
    sock::closeHandle(sock_);
    sock_ = sock::kInvalid;
}

}  // namespace cascade::net
