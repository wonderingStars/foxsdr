// See udp_audio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/udp_audio.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/thread_beats.hpp"

namespace cascade::net {
namespace {

#if defined(_WIN32)
using Sock = std::intptr_t;
constexpr Sock kInvalid = static_cast<Sock>(INVALID_SOCKET);
SOCKET raw(Sock s) { return static_cast<SOCKET>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::closesocket(raw(s)); } }
bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void setNonBlocking(Sock s) { u_long on = 1; ::ioctlsocket(raw(s), FIONBIO, &on); }
#else
using Sock = std::intptr_t;
constexpr Sock kInvalid = -1;
int raw(Sock s) { return static_cast<int>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::close(raw(s)); } }
bool ensureWinsock() { return true; }
void setNonBlocking(Sock s) { const int fl = ::fcntl(raw(s), F_GETFL, 0); ::fcntl(raw(s), F_SETFL, fl | O_NONBLOCK); }
#endif

}  // namespace

std::string udpAudioPayload(const float* samples, std::size_t n) {
    std::string p;
    p.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        float v = samples[i];
        if (!(v == v)) { v = 0.0f; }
        const double c = std::max(-1.0, std::min(1.0, static_cast<double>(v)));
        const auto s = static_cast<std::int16_t>(std::lround(c * 32767.0));
        p.push_back(static_cast<char>(s & 0xFF));
        p.push_back(static_cast<char>((s >> 8) & 0xFF));
    }
    return p;
}

UdpAudioSender::~UdpAudioSender() { stop(); }

bool UdpAudioSender::start(const std::string& host, int port, int packetFrames, Reader reader, std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_relaxed)) {
        error = "audio over the network is already running";
        return false;
    }
    if (port < 1 || port > 65535) {
        error = "the audio port must be between 1 and 65535";
        return false;
    }
    if (packetFrames != kUdpPacketFramesShort && packetFrames != kUdpPacketFramesLong) {
        error = "the packet size must be 480 or 960 samples";
        return false;
    }
    if (host.empty() || host.size() > 253) {
        error = "the audio destination host is empty or too long";
        return false;
    }
    if (!reader) {
        error = "audio over the network has no radio to read from";
        return false;
    }
    if (!ensureWinsock()) {
        error = "could not initialise the network stack";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr) {
        error = "cannot find the audio destination '" + host + "' (not an IPv4 address or a name this machine can look up)";
        return false;
    }
    std::memcpy(addr_, res->ai_addr, std::min<std::size_t>(res->ai_addrlen, sizeof(addr_)));
    addrLen_ = static_cast<int>(res->ai_addrlen);
    ::freeaddrinfo(res);
    const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s == kInvalid) {
        error = "could not create a UDP socket";
        return false;
    }
    setNonBlocking(s);
    sock_ = s;
    reader_ = std::move(reader);
    packetFrames_ = packetFrames;
    stop_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread([this] { run(); });
    return true;
}

void UdpAudioSender::stop() {
    if (!running_.load(std::memory_order_relaxed)) { return; }
    stop_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) { worker_.join(); }
    closeSock(sock_);
    sock_ = kInvalid;
    reader_ = nullptr;   // the reader holds the engine session it reads through: stopped means the session is closed
    running_.store(false, std::memory_order_relaxed);
}

void UdpAudioSender::run() {
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadUdpAudio);
    std::uint64_t cursor = 0;
    std::vector<float> pend;
    std::vector<float> buf(4800);
    while (!stop_.load(std::memory_order_relaxed)) {
        beat.beat("read");
        const int got = reader_(cursor, buf.data(), buf.size());
        if (got < 0) {
            noRadio_.fetch_add(1, std::memory_order_relaxed);
            cursor = 0;       // the radio went and may come back: join live again
            pend.clear();
        } else if (got > 0) {
            pend.insert(pend.end(), buf.begin(), buf.begin() + got);
            // Bounded: a worker that was held up never sends more than half a second at once.
            if (pend.size() > 24000) { pend.erase(pend.begin(), pend.end() - 24000); }
        }
        const std::size_t pf = static_cast<std::size_t>(packetFrames_);
        std::size_t used = 0;
        beat.beat("send");
        while (pend.size() - used >= pf) {
            const std::string payload = udpAudioPayload(pend.data() + used, pf);
            used += pf;
#if defined(_WIN32)
            const int r = ::sendto(raw(sock_), payload.data(), static_cast<int>(payload.size()), 0, reinterpret_cast<const sockaddr*>(addr_), addrLen_);
#else
            const int r = static_cast<int>(::sendto(raw(sock_), payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(addr_), static_cast<socklen_t>(addrLen_)));
#endif
            if (r < 0) {
                errors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                packets_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(static_cast<std::uint64_t>(r), std::memory_order_relaxed);
            }
        }
        if (used > 0) { pend.erase(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(used)); }
        beat.beat("idle");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace cascade::net
