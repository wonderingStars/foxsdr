// hpsdr2_source.cpp - see hpsdr2_source.hpp and hpsdr2_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr2_source.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
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

namespace h2 = hpsdr2;
namespace hp = hpsdr;

namespace {

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

constexpr int kSliceMs = 200;
constexpr int kRefreshMs = 100;  // the document: "at least every second (every 100 mS is recommended)"

bool resolveV4(const std::string& host, std::uint16_t port, sockaddr_in& out) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) { return true; }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) { return false; }
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

bool isLoopbackText(const std::string& s) { return s.rfind("127.", 0) == 0; }

long parsePort(const std::string& s, bool& bad) {
    if (s.empty()) { return 0; }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || v < 1 || v > 65535) { bad = true; return 0; }
    return v;
}

}  // namespace

// One unconnected UDP socket: everything the radio says arrives here, whichever of its ports it came from.
struct Hpsdr2Source::Net {
    SockT s = kBad;
    ~Net() {
        if (s != kBad) { closeS(s); }
    }
    bool bind(bool loopback, std::string& err) {
        if (!netUp()) { err = "the network could not be started"; return false; }
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBad) { err = "could not open a UDP socket"; return false; }
        sockaddr_in me{};
        me.sin_family = AF_INET;
        // A loopback radio is reached over loopback only; the wildcard address is for real networks (and
        // the broadcast search, which needs it).
        me.sin_addr.s_addr = ::htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
        if (::bind(s, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
            err = "could not bind a UDP socket";
            return false;
        }
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
        // At 1.536 MS/s the radio sends about 6500 packets of 1444 bytes a second; the default receive buffer
        // (64 KiB) holds about 40 of them, so a reader that is descheduled for 6 ms would lose packets. Ask
        // for 4 MiB (best effort: a refusal only means the default stays).
        const int big = 4 << 20;
        ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
        return true;
    }
    bool sendTo(const sockaddr_in& to, const std::vector<std::uint8_t>& d) {
        return ::sendto(s, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
                        reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == static_cast<int>(d.size());
    }
    // 1 = a datagram, 0 = nothing within waitMs, -1 = error.
    int recvFrom(std::uint8_t* buf, std::size_t max, std::size_t& got, sockaddr_in& from, int waitMs) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{};
        tv.tv_sec = waitMs / 1000;
        tv.tv_usec = (waitMs % 1000) * 1000;
#if defined(_WIN32)
        const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
        const int r = ::select(s + 1, &rf, nullptr, nullptr, &tv);
#endif
        if (r < 0) { return -1; }
        if (r == 0) { return 0; }
        SockLen fl = sizeof(from);
        const int n = ::recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(max), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 0) { return -1; }
        got = static_cast<std::size_t>(n);
        return 1;
    }
};

std::string Hpsdr2Device::name() const {
    return std::string(h2::boardInfo(reply.boardType).name) + " (" + reply.macText() + ", firmware " +
           std::to_string(static_cast<int>(reply.firmwareVersion)) + ")";
}

namespace {

// Collects replies on `net` for `window`. `single`: stop at the first valid reply.
void collectReplies(const std::function<int(std::uint8_t*, std::size_t, std::size_t&, sockaddr_in&, int)>& recv,
                    std::chrono::milliseconds window, bool single, std::vector<Hpsdr2Device>& found) {
    std::set<std::string> seen;
    const auto end = std::chrono::steady_clock::now() + window;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0) { break; }
        std::uint8_t buf[2048];
        std::size_t got = 0;
        sockaddr_in from{};
        const int r = recv(buf, sizeof(buf), got, from, static_cast<int>(left.count()));
        if (r <= 0) { break; }
        h2::DiscoveryReply rep;
        if (!h2::parseDiscoveryReply(buf, got, rep)) { continue; }
        if (!seen.insert(rep.macText()).second) { continue; }
        Hpsdr2Device d;
        char ip[64] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        d.ip = ip;
        d.port = ntohs(from.sin_port);
        d.reply = rep;
        found.push_back(d);
        if (single) { break; }
    }
}

}  // namespace

bool discoverHpsdr2(const std::string& host, std::uint16_t port, std::chrono::milliseconds window,
                    std::vector<Hpsdr2Device>& found, std::string& error) {
    found.clear();
    sockaddr_in to{};
    if (!resolveV4(host, port, to)) { error = "could not find the host " + host; return false; }
    Hpsdr2Source::Net net;
    if (!net.bind(isLoopbackText(host), error)) { return false; }
    if (!net.sendTo(to, h2::buildDiscovery())) { error = "could not send the discovery request"; return false; }
    collectReplies([&net](std::uint8_t* b, std::size_t m, std::size_t& g, sockaddr_in& f, int w) { return net.recvFrom(b, m, g, f, w); },
                   window, host != "255.255.255.255", found);
    return true;
}

// ---------------------------------------------------------------------------------------
// the receive half, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct Hpsdr2Source::Rx {
    std::shared_ptr<Net> net;
    sockaddr_in dev{};
    std::uint32_t devIp = 0;  // network order
    std::uint16_t mainPort = h2::kDiscoveryPort;
    h2::PortPlan ports;
    bool phaseWord = true;
    double clockHz = 122880000.0;
    int adcs = 1;

    // control state; sendMutex guards everything below up to "receive"
    std::mutex sendMutex;
    std::uint32_t seqGeneral = 0, seqDdc = 0, seqHp = 0;
    int rateHz = 48000;
    std::uint32_t freqWord = 0;
    int attenDb = 0;
    bool sending = false;
    std::chrono::steady_clock::time_point nextRefresh{};
    std::atomic<std::uint64_t> controlPackets{0};

    bool sendPacket(std::uint16_t port, const std::vector<std::uint8_t>& p) {
        sockaddr_in to = dev;
        to.sin_port = ::htons(port);
        if (!net->sendTo(to, p)) { return false; }
        controlPackets.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool sendGeneralLocked() { return sendPacket(mainPort, h2::buildGeneral(seqGeneral++, ports, phaseWord)); }
    bool sendDdcLocked() { return sendPacket(ports.effectiveDdcSpecific(), h2::buildDdcSpecific(seqDdc++, adcs, rateHz)); }
    bool sendHpLocked(bool run) {
        nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshMs);
        return sendPacket(ports.effectiveHighPriorityToHardware(), h2::buildHighPriority(seqHp++, run, freqWord, attenDb));
    }
    void refreshIfDue() {
        std::lock_guard<std::mutex> lk(sendMutex);
        if (sending && std::chrono::steady_clock::now() >= nextRefresh) { sendHpLocked(true); }
    }

    // receive
    std::mutex trackMutex;
    hp::SequenceTracker tracker;
    bool armed = true;
    std::chrono::steady_clock::time_point armDeadline{};
    std::atomic<std::uint64_t> bad{0}, statusPackets{0}, adcOverloadPackets{0}, fifoOverflowPackets{0};
    std::atomic<long long> lastDataMs{0};  // steady-clock milliseconds; start() and the reader both write it
    int silentMs = 2000;
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(4096);
    std::vector<std::complex<float>> out;

    bool pump(NetLink& link) {
        refreshIfDue();
        std::size_t got = 0;
        sockaddr_in from{};
        const int r = net->recvFrom(buf.data(), buf.size(), got, from, kSliceMs);
        const auto now = std::chrono::steady_clock::now();
        if (r < 0) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            link.fault("the HPSDR network connection failed", "receive error");
            return false;
        }
        // Silence means no DDC data: status packets keep arriving from a radio that has stopped streaming.
        if (!link.streaming.load(std::memory_order_relaxed)) {
            lastDataMs.store(nowMs());  // idle time is not silence
        } else if (nowMs() - lastDataMs.load() > silentMs) {
            link.fault("the HPSDR radio stopped sending", "no data arrived");
            return false;
        }
        if (r == 0) { return true; }
        if (from.sin_addr.s_addr != devIp) { return true; }  // someone else's datagram
        const std::uint16_t port = ntohs(from.sin_port);
        if (port == ports.effectiveDdc0()) {
            h2::DdcPacket p;
            if (h2::parseDdcPacket(buf.data(), got, p) != h2::DdcVerdict::Ok) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
            {
                std::lock_guard<std::mutex> lk(trackMutex);
                if (!armed) {
                    if (p.sequence == 0 || now >= armDeadline) { armed = true; } else { return true; }  // stale, from before the Stop
                }
                tracker.note(0, p.sequence);
            }
            lastDataMs.store(nowMs());
            out.clear();
            h2::decodeDdcSamples(p, out);
            link.pushSamples(out.data(), out.size());
        } else if (port == ports.effectiveHighPriorityFromHardware()) {
            h2::Status st;
            if (!h2::parseStatus(buf.data(), got, st)) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
            statusPackets.fetch_add(1, std::memory_order_relaxed);
            if ((st.adcOverload & 0x01) != 0) { adcOverloadPackets.fetch_add(1, std::memory_order_relaxed); }
            if (st.ddcFifoOverflow) { fifoOverflowPackets.fetch_add(1, std::memory_order_relaxed); }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

Hpsdr2Source::~Hpsdr2Source() { closeDevice(); }

bool Hpsdr2Source::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(mutex_);
    std::string host = "255.255.255.255";
    std::uint16_t port = h2::kDiscoveryPort;
    const std::string addr = argValue(args, "hpsdr2");
    std::string err;
    if (!addr.empty() && !parseHostPort(addr, h2::kDiscoveryPort, host, port, err)) { setError(err); return false; }
    timeoutMs_ = 2000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    std::string wantMac = argValue(args, "mac");
    for (char& c : wantMac) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const bool takeover = argValue(args, "takeover") == "1";
    h2::PortPlan plan;
    {
        bool badPort = false;
        plan.ddcSpecific = static_cast<std::uint16_t>(parsePort(argValue(args, "ddcport"), badPort));
        plan.highPriorityToHardware = static_cast<std::uint16_t>(parsePort(argValue(args, "hpport"), badPort));
        plan.highPriorityFromHardware = static_cast<std::uint16_t>(parsePort(argValue(args, "statusport"), badPort));
        plan.ddc0 = static_cast<std::uint16_t>(parsePort(argValue(args, "ddc0port"), badPort));
        if (badPort) { setError("a port argument must be 1 to 65535"); return false; }
        // The reader tells packets apart by the radio's source port, so the two it listens for must differ.
        if (plan.effectiveDdc0() == plan.effectiveHighPriorityFromHardware() || plan.effectiveDdc0() == h2::kDiscoveryPort ||
            plan.effectiveHighPriorityFromHardware() == h2::kDiscoveryPort) {
            setError("the DDC0 and status source ports must differ from each other and from 1024");
            return false;
        }
    }

    sockaddr_in target{};
    if (!resolveV4(host, port, target)) { setError("could not find the host " + host); return false; }
    auto net = std::make_shared<Net>();
    if (!net->bind(isLoopbackText(host), err)) { setError(err); return false; }
    if (!net->sendTo(target, h2::buildDiscovery())) { setError("could not send the discovery request"); return false; }
    std::vector<Hpsdr2Device> found;
    collectReplies([&net](std::uint8_t* b, std::size_t m, std::size_t& g, sockaddr_in& f, int w) { return net->recvFrom(b, m, g, f, w); },
                   std::chrono::milliseconds(timeoutMs_), host != "255.255.255.255" && wantMac.empty(), found);
    const Hpsdr2Device* chosen = nullptr;
    for (const Hpsdr2Device& d : found) {
        if (wantMac.empty() || d.reply.macText() == wantMac) { chosen = &d; break; }
    }
    if (chosen == nullptr) {
        setError(found.empty() ? "no HPSDR protocol-2 radio answered the discovery request"
                               : "no HPSDR radio with that MAC address answered (" + std::to_string(found.size()) + " other(s) did)");
        return false;
    }
    const h2::BoardInfo bi = h2::boardInfo(chosen->reply.boardType);
    if (!bi.documented) {
        setError("board type " + std::to_string(static_cast<int>(chosen->reply.boardType)) + " (" + bi.name +
                 ") has no table in the openHPSDR Ethernet protocol document's hardware appendix (boards 1 to 4), so it is not supported");
        return false;
    }
    if (chosen->reply.busy && !takeover) {
        setError("the radio says it is already connected to another computer (add takeover=1 to take it over)");
        return false;
    }
    if (chosen->reply.formatModes != 0 && !((chosen->reply.formatModes & 0x01) != 0 && (chosen->reply.formatModes & 0x04) != 0)) {
        setError("the radio does not offer big-endian 3-byte I&Q, the only format this build reads");
        return false;
    }
    dev_ = *chosen;
    ports_ = plan;

    auto rx = std::make_shared<Rx>();
    rx->net = net;
    resolveV4(dev_.ip, dev_.port, rx->dev);
    rx->devIp = rx->dev.sin_addr.s_addr;
    rx->mainPort = dev_.port;
    rx->ports = plan;
    rx->phaseWord = chosen->reply.phaseWord;
    rx->clockHz = bi.dspClockHz;
    rx->adcs = bi.adcs;
    rx->rateHz = rateHz_;
    rx->attenDb = attenDb_;
    rx->silentMs = timeoutMs_;
    rx_ = rx;
    if (!beginReader([rx](NetLink& l) { return rx->pump(l); }, []() {}, rx)) {
        rx_.reset();
        return false;
    }
    markOpen(true);
    setName("HPSDR 2: " + dev_.name());
    return true;
}

void Hpsdr2Source::closeDevice() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_ && !isOpen()) { endReader(); return; }
    const std::string keep = lastError();
    if (rx_ && active_) {
        std::lock_guard<std::mutex> sl(rx_->sendMutex);
        rx_->sending = false;
        rx_->sendHpLocked(false);  // best effort: run bit cleared
    }
    active_ = false;
    markOpen(false);
    endReader();
    rx_.reset();
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool Hpsdr2Source::start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("start() called with no HPSDR radio open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    bool ok = false;
    const std::uint32_t word = h2::frequencyWord(freqHz_, rx_->phaseWord, rx_->clockHz, ok);
    if (!ok) { setError("the frequency cannot be encoded for this radio"); return false; }
    {
        std::lock_guard<std::mutex> tl(rx_->trackMutex);
        rx_->tracker.reset();  // the radio resets its sequence numbers at a Stop
        rx_->armed = false;
        rx_->armDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    }
    rx_->bad.store(0);
    rx_->statusPackets.store(0);
    rx_->adcOverloadPackets.store(0);
    rx_->fifoOverflowPackets.store(0);
    {
        std::lock_guard<std::mutex> sl(rx_->sendMutex);
        rx_->rateHz = rateHz_;
        rx_->freqWord = word;
        rx_->attenDb = attenDb_;
        rx_->lastDataMs.store(Rx::nowMs());
        // the document's order: General, DDC Specific, then High Priority with the run bit
        if (!rx_->sendGeneralLocked() || !rx_->sendDdcLocked() || !rx_->sendHpLocked(true)) {
            setError("could not send the start-up packets");
            return false;
        }
        rx_->sending = true;
    }
    active_ = true;
    return startKeeping();
}

void Hpsdr2Source::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopKeeping();
    if (rx_ && active_) {
        std::lock_guard<std::mutex> sl(rx_->sendMutex);
        rx_->sending = false;
        rx_->sendHpLocked(false);
    }
    active_ = false;
}

double Hpsdr2Source::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<double>(rateHz_);
}

bool Hpsdr2Source::applyLocked() {
    if (!rx_ || !active_) { return true; }
    std::lock_guard<std::mutex> sl(rx_->sendMutex);
    bool ok = false;
    rx_->freqWord = h2::frequencyWord(freqHz_, rx_->phaseWord, rx_->clockHz, ok);
    rx_->rateHz = rateHz_;
    rx_->attenDb = attenDb_;
    // a rate change needs the DDC Specific packet (all DDC settings in one packet); every change then
    // re-sends the High Priority packet, which carries the frequency word and the attenuator
    if (!rx_->sendDdcLocked() || !rx_->sendHpLocked(true)) {
        setError("could not send the new settings to the radio");
        return false;
    }
    return true;
}

bool Hpsdr2Source::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::uint16_t ksps = 0;
    if (!std::isfinite(hz) || hz != std::floor(hz) || hz < 0.0 || hz > 1.0e9 || !h2::rateToKsps(static_cast<int>(hz), ksps)) {
        setError("the HPSDR sample rate must be 48000, 96000, 192000, 384000, 768000 or 1536000 Hz");
        return false;
    }
    rateHz_ = static_cast<int>(hz);
    return applyLocked();
}

double Hpsdr2Source::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return freqHz_;
}

bool Hpsdr2Source::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    bool ok = false;
    if (rx_) {
        h2::frequencyWord(hz, rx_->phaseWord, rx_->clockHz, ok);
    } else {
        ok = std::isfinite(hz) && hz >= 0.0 && hz <= 4294967295.0;
    }
    if (!ok) {
        setError(rx_ && rx_->phaseWord ? "the HPSDR frequency must be 0 up to (not including) the 122.88 MHz DSP clock"
                                       : "the HPSDR frequency must be 0 to 4294967295 Hz");
        return false;
    }
    freqHz_ = std::round(hz);
    return applyLocked();
}

namespace {
constexpr const char* kAttenuator = "Attenuator";
}

std::vector<GainInfo> Hpsdr2Source::gains() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { return {}; }
    return {GainInfo{kAttenuator, 0.0, 31.0, 1.0}};
}

bool Hpsdr2Source::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (name != kAttenuator || !isOpen()) { setError("this HPSDR radio has no gain control called " + name); return false; }
    if (!std::isfinite(db) || db < 0.0 || db > 31.0) { setError("the HPSDR attenuator must be 0 to 31 dB"); return false; }
    attenDb_ = static_cast<int>(std::lround(db));
    return applyLocked();
}

double Hpsdr2Source::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return name == kAttenuator ? static_cast<double>(attenDb_) : 0.0;
}

std::vector<double> Hpsdr2Source::supportedSampleRatesHz() const {
    std::vector<double> v;
    for (const int r : h2::supportedRates()) { v.push_back(static_cast<double>(r)); }
    return v;
}

Hpsdr2Device Hpsdr2Source::device() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return dev_;
}

std::uint64_t Hpsdr2Source::packetsLost() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.lost();
}

std::uint64_t Hpsdr2Source::packetsLate() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.late();
}

std::uint64_t Hpsdr2Source::badPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->bad.load() : 0;
}

std::uint64_t Hpsdr2Source::statusPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->statusPackets.load() : 0;
}

std::uint64_t Hpsdr2Source::adcOverloadPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->adcOverloadPackets.load() : 0;
}

std::uint64_t Hpsdr2Source::fifoOverflowPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->fifoOverflowPackets.load() : 0;
}

std::uint64_t Hpsdr2Source::controlPacketsSent() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->controlPackets.load() : 0;
}

}  // namespace cascade::source
