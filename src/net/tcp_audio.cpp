// tcp_audio.cpp - see tcp_audio.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tcp_audio.hpp"

#include <chrono>
#include <vector>

namespace cascade::net {

TcpAudioServer::~TcpAudioServer() { stop(); }

bool TcpAudioServer::start(const TcpAudioConfig& cfg, UdpAudioSender::Reader reader, std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_relaxed)) {
        error = "audio over TCP is already running";
        return false;
    }
    if (cfg.port < 0 || cfg.port > 65535) {
        error = "the audio port must be between 1 and 65535";
        return false;
    }
    if (cfg.packetFrames != kUdpPacketFramesShort && cfg.packetFrames != kUdpPacketFramesLong) {
        error = "the packet size must be 480 or 960 samples";
        return false;
    }
    if (!reader) {
        error = "audio over TCP has no radio to read from";
        return false;
    }
    FanoutConfig fc;
    fc.bindAddress = cfg.bindAddress;
    fc.allowAllInterfaces = cfg.allowAllInterfaces;
    fc.port = static_cast<std::uint16_t>(cfg.port);
    fc.maxClients = cfg.maxClients;
    fc.clientQueueBytes = 64 * 1024;   // about 0.7 s of this audio: a client that falls further behind is dropped
    fc.ringBytes = 256 * 1024;
    fc.stallMs = 5000;
    fc.socketSendBuffer = 32 * 1024;   // small, so a client that stops reading fills it and is dropped within a second or two instead of the OS holding megabytes for it
    auto fan = std::make_unique<TcpFanout>(fc);
    if (!fan->start(error)) { return false; }
    fan_ = std::move(fan);
    reader_ = std::move(reader);
    packetFrames_ = cfg.packetFrames;
    noRadio_.store(0, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread([this] { run(); });
    return true;
}

void TcpAudioServer::stop() {
    if (!running_.load(std::memory_order_relaxed)) { return; }
    stop_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) { worker_.join(); }
    if (fan_) { fan_->stop(); }
    reader_ = nullptr;   // the reader holds the engine session it reads through: stopped means the session is closed
    running_.store(false, std::memory_order_relaxed);
    fan_.reset();
}

void TcpAudioServer::run() {
    std::uint64_t cursor = 0;
    std::vector<float> pend;
    std::vector<float> buf(4800);
    while (!stop_.load(std::memory_order_relaxed)) {
        const int got = reader_(cursor, buf.data(), buf.size());
        if (got < 0) {
            noRadio_.fetch_add(1, std::memory_order_relaxed);
            cursor = 0;       // the radio went and may come back: join live again
            pend.clear();
        } else if (got > 0) {
            pend.insert(pend.end(), buf.begin(), buf.begin() + got);
            if (pend.size() > 24000) { pend.erase(pend.begin(), pend.end() - 24000); }   // bounded: never more than half a second held
        }
        const std::size_t pf = static_cast<std::size_t>(packetFrames_);
        std::size_t used = 0;
        while (pend.size() - used >= pf) {
            const std::string payload = udpAudioPayload(pend.data() + used, pf);
            used += pf;
            (void)fan_->publish(payload.data(), payload.size());   // wait-free; nobody connected, or a full ring, only counts
        }
        if (used > 0) { pend.erase(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(used)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace cascade::net
