// hpsdr_source.cpp - see hpsdr_source.hpp and hpsdr_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr_source.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::source {

namespace hp = hpsdr;

namespace {

constexpr int kPollMs = 200;

#if defined(_WIN32)
using SockT = SOCKET;
const SockT kBad = INVALID_SOCKET;
void closeS(SockT s) { ::closesocket(s); }
bool netUp() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
using SockLen = int;
#else
using SockT = int;
const SockT kBad = -1;
void closeS(SockT s) { ::close(s); }
bool netUp() { return true; }
using SockLen = socklen_t;
#endif

}  // namespace

std::string HpsdrDevice::name() const {
    return std::string(hp::boardName(reply.boardId)) + " (" + reply.macText() + ", version " +
           std::to_string(static_cast<int>(reply.codeVersion)) + ")";
}

bool discoverHpsdr(const std::string& host, std::uint16_t port, std::chrono::milliseconds window,
                   std::vector<HpsdrDevice>& found, std::string& error) {
    found.clear();
    if (!netUp()) { error = "the network could not be started"; return false; }
    const SockT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) { error = "could not open a UDP socket"; return false; }
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = ::htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
        closeS(s);
        error = "could not bind a UDP socket";
        return false;
    }
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            closeS(s);
            error = "could not find the host " + host;
            return false;
        }
        to.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        ::freeaddrinfo(res);
    }
    const std::vector<std::uint8_t> req = hp::buildDiscoveryRequest();
    if (::sendto(s, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to)) != static_cast<int>(req.size())) {
        closeS(s);
        error = "could not send the discovery request";
        return false;
    }
    std::set<std::string> seen;
    const auto end = std::chrono::steady_clock::now() + window;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0) { break; }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{};
        tv.tv_sec = static_cast<long>(left.count() / 1000);
        tv.tv_usec = static_cast<long>((left.count() % 1000) * 1000);
#if defined(_WIN32)
        const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
        const int r = ::select(s + 1, &rf, nullptr, nullptr, &tv);
#endif
        if (r <= 0) { break; }
        std::uint8_t buf[2048];
        sockaddr_in from{};
        SockLen fl = sizeof(from);
        const int n = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) { continue; }
        hp::DiscoveryReply rep;
        if (!hp::parseDiscoveryReply(buf, static_cast<std::size_t>(n), rep)) { continue; }
        if (!seen.insert(rep.macText()).second) { continue; }
        HpsdrDevice d;
        char ip[64] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        d.ip = ip;
        d.port = ntohs(from.sin_port);  // the guide: the device answers from its port (1024 on real hardware)
        d.reply = rep;
        found.push_back(d);
        if (host != "255.255.255.255") { break; }  // one device was asked, one answered: no need to wait out the window
    }
    closeS(s);
    return true;
}

// ---------------------------------------------------------------------------------------
// the receive half, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct HpsdrSource::Rx {
    std::shared_ptr<UdpTransport> udp;
    std::mutex decMutex;
    std::shared_ptr<HpsdrFrameDecoder> decoder;
    hp::SequenceTracker tracker;
    std::mutex trackMutex;
    // After a Start, packets still queued from before the last Stop are discarded until the new stream
    // shows itself (sequence 0, which the device restarts at, per the guide) or a short grace passes
    // (the first packet may itself be lost).
    bool armed = true;
    std::chrono::steady_clock::time_point armDeadline{};
    std::atomic<std::uint64_t> bad{0};
    int silentTicks = 0;
    int silentLimit = 10;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(4096);
    std::vector<std::complex<float>> out;

    bool pump(NetLink& link) {
        std::size_t got = 0;
        if (!udp->recvSome(buf.data(), buf.size(), got)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            const char* e = udp->lastError();
            if (e != nullptr && std::strstr(e, "timed out") != nullptr) {
                if (link.streaming.load(std::memory_order_relaxed) && ++silentTicks >= silentLimit) {
                    link.fault("the HPSDR radio stopped sending", "no data arrived");
                    return false;
                }
                return true;
            }
            link.fault("the HPSDR network connection failed", e != nullptr ? e : "");
            return false;
        }
        silentTicks = 0;
        hp::DataPacket p;
        if (!hp::parseDataPacket(buf.data(), got, p)) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
        if (p.endpoint != hp::kEndpointIq) { return true; }  // the bandscope is not used
        {
            std::lock_guard<std::mutex> lk(trackMutex);
            if (!armed) {
                if (p.sequence == 0 || std::chrono::steady_clock::now() >= armDeadline) {
                    armed = true;
                } else {
                    return true;  // a stale packet from before the Stop
                }
            }
            tracker.note(p.endpoint, p.sequence);
        }
        std::shared_ptr<HpsdrFrameDecoder> dec;
        {
            std::lock_guard<std::mutex> lk(decMutex);
            dec = decoder;
        }
        if (!dec) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
        out.clear();
        if (!dec->decode(p.frames, out)) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
        if (!out.empty()) { link.pushSamples(out.data(), out.size()); }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

HpsdrSource::~HpsdrSource() { closeDevice(); }

void HpsdrSource::setFrameDecoder(std::shared_ptr<HpsdrFrameDecoder> d) {
    std::lock_guard<std::mutex> lk(mutex_);
    decoder_ = d;
    if (rx_) {
        std::lock_guard<std::mutex> dl(rx_->decMutex);
        rx_->decoder = d;
    }
}

bool HpsdrSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(mutex_);
    std::string host = "255.255.255.255";
    std::uint16_t port = hp::kDevicePort;
    const std::string addr = argValue(args, "hpsdr");
    std::string err;
    if (!addr.empty() && !parseHostPort(addr, hp::kDevicePort, host, port, err)) { setError(err); return false; }
    timeoutMs_ = 2000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    std::string wantMac = argValue(args, "mac");
    for (char& c : wantMac) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    std::vector<HpsdrDevice> found;
    if (!discoverHpsdr(host, port, std::chrono::milliseconds(timeoutMs_), found, err)) { setError(err); return false; }
    const HpsdrDevice* chosen = nullptr;
    for (const HpsdrDevice& d : found) {
        if (wantMac.empty() || d.reply.macText() == wantMac) { chosen = &d; break; }
    }
    if (chosen == nullptr) {
        setError(found.empty() ? "no HPSDR radio answered the discovery request"
                               : "no HPSDR radio with that MAC address answered (" + std::to_string(found.size()) + " other(s) did)");
        return false;
    }
    dev_ = *chosen;
    // One socket for Start, Stop and the data: the device sends to the address and port the Start came from.
    std::unique_ptr<UdpTransport> u = UdpTransport::bindLocal("0.0.0.0", 0, std::chrono::milliseconds(kPollMs), err);
    if (!u) { setError(err); return false; }
    if (!u->connectTo(dev_.ip, dev_.port)) { setError(u->lastError()); return false; }
    udp_ = std::shared_ptr<UdpTransport>(u.release());

    auto rx = std::make_shared<Rx>();
    rx->udp = udp_;
    rx->decoder = decoder_;
    rx->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    rx_ = rx;
    if (!beginReader([rx](NetLink& l) { return rx->pump(l); }, [rx]() { rx->udp->shutdown(); }, rx)) {
        rx_.reset();
        udp_.reset();
        return false;
    }
    markOpen(true);
    setName("HPSDR: " + dev_.name());
    return true;
}

void HpsdrSource::closeDevice() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_ && !isOpen()) { endReader(); return; }
    const std::string keep = lastError();
    if (udp_ && active_) {
        const std::vector<std::uint8_t> stopCmd = hp::buildStartStop(hp::kStopAll);
        udp_->sendAll(stopCmd.data(), stopCmd.size());  // best effort
    }
    active_ = false;
    markOpen(false);
    endReader();
    if (udp_) { udp_->close(); }
    udp_.reset();
    rx_.reset();
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool HpsdrSource::start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("start() called with no HPSDR radio open"); return false; }
    if (faulted()) { return false; }
    if (!decoder_) {
        setError("HPSDR receive is not available: the layout of the 512-byte frames is not known to this build");
        return false;
    }
    if (active_) { return true; }
    {
        std::lock_guard<std::mutex> tl(rx_->trackMutex);
        rx_->tracker.reset();  // the device resets its sequence numbers at Start
        rx_->armed = false;
        rx_->armDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    }
    const std::vector<std::uint8_t> cmd = hp::buildStartStop(hp::kStartIq);
    if (!udp_->sendAll(cmd.data(), cmd.size())) {
        setError(std::string("could not send the start command: ") + udp_->lastError());
        return false;
    }
    active_ = true;
    return startKeeping();
}

void HpsdrSource::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopKeeping();
    if (active_ && udp_) {
        const std::vector<std::uint8_t> cmd = hp::buildStartStop(hp::kStopAll);
        udp_->sendAll(cmd.data(), cmd.size());
    }
    active_ = false;
}

double HpsdrSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return decoder_ ? decoder_->sampleRateHz() : 0.0;
}

bool HpsdrSource::setSampleRateHz(double) {
    setError("setting the HPSDR sample rate is not available: it is set inside the 512-byte frames, whose layout is not known to this build");
    return false;
}

bool HpsdrSource::setCenterFrequencyHz(double) {
    setError("tuning an HPSDR radio is not available: the frequency is set inside the 512-byte frames, whose layout is not known to this build");
    return false;
}

std::vector<double> HpsdrSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return decoder_ ? std::vector<double>{decoder_->sampleRateHz()} : std::vector<double>{};
}

HpsdrDevice HpsdrSource::device() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return dev_;
}

std::uint64_t HpsdrSource::packetsLost() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.lost();
}

std::uint64_t HpsdrSource::packetsLate() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.late();
}

std::uint64_t HpsdrSource::badPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->bad.load() : 0;
}

}  // namespace cascade::source
