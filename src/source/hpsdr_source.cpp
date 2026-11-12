// hpsdr_source.cpp - see hpsdr_source.hpp and hpsdr_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    // A loopback target (a test stand-in, or a radio on this machine) is reached over loopback only: bind
    // there, so nothing listens on a wider address (a wider bind raises the firewall prompt). The broadcast
    // search needs the wildcard address.
    me.sin_addr.s_addr = ::htonl(host.rfind("127.", 0) == 0 ? INADDR_LOOPBACK : INADDR_ANY);
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

    // The built-in protocol-1 half (used when no decoder was injected): the decoder, and the sender of the
    // command-and-control frames. p1Mutex guards the decoder's tally against the control thread's reads;
    // sendMutex serialises the sender (setters and the periodic refresh) and the socket writes.
    std::mutex p1Mutex;
    hp::Protocol1Decoder p1;
    std::mutex sendMutex;
    hp::ControlSender sender;
    bool sending = false;  // between Start and Stop in built-in mode
    std::chrono::steady_clock::time_point nextRefresh{};
    std::atomic<std::uint64_t> controlPackets{0};
    static constexpr int kRefreshMs = 100;  // a choice: the document names no interval

    // One full cycle of the settings: every setting goes out once. Caller holds sendMutex.
    bool sendCycleLocked() {
        const std::size_t n = sender.packetsPerCycle();
        for (std::size_t i = 0; i < n; ++i) {
            const std::vector<std::uint8_t> pk = sender.nextPacket();
            if (!udp->sendAll(pk.data(), pk.size())) { return false; }
            controlPackets.fetch_add(1, std::memory_order_relaxed);
        }
        nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshMs);
        return true;
    }
    void refreshIfDue() {
        std::lock_guard<std::mutex> lk(sendMutex);
        if (sending && std::chrono::steady_clock::now() >= nextRefresh) { sendCycleLocked(); }
    }

    bool pump(NetLink& link) {
        std::size_t got = 0;
        refreshIfDue();
        if (!udp->recvSome(buf.data(), buf.size(), got)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            const char* e = udp->lastError();
            if (e != nullptr && std::strstr(e, "timed out") != nullptr) {
                if (!link.streaming.load(std::memory_order_relaxed)) {
                    silentTicks = 0;  // idle slices are not silence: only consecutive streaming ones count
                } else if (++silentTicks >= silentLimit) {
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
        out.clear();
        if (!dec) {  // the built-in protocol-1 decoder
            std::lock_guard<std::mutex> lk(p1Mutex);
            if (!p1.decodePacket(p.frames, out)) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
        } else if (!dec->decode(p.frames, out)) {
            bad.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
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

    // board=metis|hermes|griffin|angelia|orion|hermes_lite: the board the user expects. A radio that
    // answers as another board is refused (a wrong board id), not used.
    std::string wantBoard = argValue(args, "board");
    for (char& c : wantBoard) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    int wantBoardId = -1;
    if (!wantBoard.empty()) {
        for (int id = 0; id < 256 && wantBoardId < 0; ++id) {
            std::string n = hp::boardName(static_cast<std::uint8_t>(id));
            for (char& c : n) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            if (n != "unknown board" && n == wantBoard) { wantBoardId = id; }
        }
        if (wantBoardId < 0) {
            setError("board= must be metis, hermes, griffin, angelia, orion or hermes_lite");
            return false;
        }
    }

    std::vector<HpsdrDevice> found;
    if (!discoverHpsdr(host, port, std::chrono::milliseconds(timeoutMs_), found, err)) { setError(err); return false; }
    const HpsdrDevice* chosen = nullptr;
    std::size_t boardMismatch = 0;
    for (const HpsdrDevice& d : found) {
        if (!wantMac.empty() && d.reply.macText() != wantMac) { continue; }
        if (wantBoardId >= 0 && d.reply.boardId != wantBoardId) { ++boardMismatch; continue; }
        chosen = &d;
        break;
    }
    if (chosen == nullptr && boardMismatch > 0) {
        setError("the HPSDR radio that answered is not the " + wantBoard + " that was asked for (it said " +
                 hp::boardName(found.front().reply.boardId) + ")");
        return false;
    }
    if (chosen == nullptr) {
        setError(found.empty() ? "no HPSDR radio answered the discovery request"
                               : "no HPSDR radio with that MAC address answered (" + std::to_string(found.size()) + " other(s) did)");
        return false;
    }
    dev_ = *chosen;
    // One socket for Start, Stop and the data: the device sends to the address and port the Start came from.
    std::unique_ptr<UdpTransport> u = UdpTransport::bindLocal(dev_.ip.rfind("127.", 0) == 0 ? "127.0.0.1" : "0.0.0.0", 0,
                                                                       std::chrono::milliseconds(kPollMs), err);
    if (!u) { setError(err); return false; }
    if (!u->connectTo(dev_.ip, dev_.port)) { setError(u->lastError()); return false; }
    udp_ = std::shared_ptr<UdpTransport>(u.release());

    auto rx = std::make_shared<Rx>();
    rx->udp = udp_;
    rx->decoder = decoder_;
    rx->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    rx->p1.configure(1, 1, settings_.rateHz);
    {
        std::string why;
        rx->sender.set(settings_, why);
    }
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
    if (!decoder_) {  // built-in mode: the settings go to the radio right behind the Start
        std::lock_guard<std::mutex> sl(rx_->sendMutex);
        std::string why;
        if (!rx_->sender.set(settings_, why)) { setError(why); active_ = false; return false; }
        rx_->sender.restart();  // the PC's sequence number restarts at zero at each Start
        {
            std::lock_guard<std::mutex> pl(rx_->p1Mutex);
            rx_->p1.configure(1, 1, settings_.rateHz);
            rx_->p1.resetTally();
        }
        rx_->sending = true;
        if (!rx_->sendCycleLocked()) {
            rx_->sending = false;
            setError(std::string("could not send the control frames: ") + udp_->lastError());
            active_ = false;
            return false;
        }
    }
    return startKeeping();
}

void HpsdrSource::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopKeeping();
    if (rx_) {
        std::lock_guard<std::mutex> sl(rx_->sendMutex);
        rx_->sending = false;
    }
    if (active_ && udp_) {
        const std::vector<std::uint8_t> cmd = hp::buildStartStop(hp::kStopAll);
        udp_->sendAll(cmd.data(), cmd.size());
    }
    active_ = false;
}

double HpsdrSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return decoder_ ? decoder_->sampleRateHz() : static_cast<double>(settings_.rateHz);
}

// Applies new settings: stored always, sent at once when streaming in built-in mode. Caller holds mutex_.
bool HpsdrSource::applySettingsLocked(const hp::ControlSettings& next) {
    std::string why;
    if (!rx_) {  // not open: validate and keep for open()
        why = hp::validateSettings(next);
        if (!why.empty()) { setError(why); return false; }
        settings_ = next;
        return true;
    }
    std::lock_guard<std::mutex> sl(rx_->sendMutex);
    if (!rx_->sender.set(next, why)) { setError(why); return false; }
    settings_ = next;
    {
        std::lock_guard<std::mutex> pl(rx_->p1Mutex);
        rx_->p1.configure(1, 1, next.rateHz);
    }
    if (active_ && !decoder_ && rx_->sending && !rx_->sendCycleLocked()) {
        setError(std::string("could not send the control frames: ") + udp_->lastError());
        return false;
    }
    return true;
}

bool HpsdrSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (decoder_) {
        setError("setting the HPSDR sample rate is not available with an injected frame decoder");
        return false;
    }
    if (!std::isfinite(hz) || hz != std::floor(hz) || hz < 0.0 || hz > 1.0e9) {
        setError("the HPSDR sample rate must be 48000, 96000, 192000 or 384000 Hz");
        return false;
    }
    hp::ControlSettings next = settings_;
    next.rateHz = static_cast<int>(hz);
    return applySettingsLocked(next);
}

double HpsdrSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<double>(settings_.rxFreqHz[0]);
}

bool HpsdrSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (decoder_) {
        setError("tuning an HPSDR radio is not available with an injected frame decoder");
        return false;
    }
    if (!std::isfinite(hz) || hz < 0.0 || hz > 4294967295.0) {
        setError("the HPSDR frequency must be 0 to 4294967295 Hz (a 32-bit value in whole hertz)");
        return false;
    }
    hp::ControlSettings next = settings_;
    next.rxFreqHz[0] = static_cast<std::uint32_t>(std::llround(hz));
    return applySettingsLocked(next);
}

namespace {
// The document names the input attenuator for Hermes and Angelia only ("Hermes/Angelia Attenuator enable").
bool boardHasAttenuator(std::uint8_t id) { return id == 0x01 || id == 0x04; }
constexpr const char* kAttenuator = "Attenuator";
}  // namespace

std::vector<GainInfo> HpsdrSource::gains() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (decoder_ || !isOpen() || !boardHasAttenuator(dev_.reply.boardId)) { return {}; }
    return {GainInfo{kAttenuator, 0.0, 31.0, 1.0}};
}

bool HpsdrSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (name != kAttenuator || decoder_ || !isOpen() || !boardHasAttenuator(dev_.reply.boardId)) {
        setError("this HPSDR radio has no gain control called " + name);
        return false;
    }
    if (!std::isfinite(db) || db < 0.0 || db > 31.0) {
        setError("the HPSDR attenuator must be 0 to 31 dB");
        return false;
    }
    hp::ControlSettings next = settings_;
    next.attenuatorDb = static_cast<int>(std::lround(db));
    next.attenuatorEnable = next.attenuatorDb > 0;
    return applySettingsLocked(next);
}

double HpsdrSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return name == kAttenuator && settings_.attenuatorEnable ? static_cast<double>(settings_.attenuatorDb) : 0.0;
}

std::vector<double> HpsdrSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (decoder_) { return {decoder_->sampleRateHz()}; }
    std::vector<double> v;
    for (const int r : hp::supportedRates()) { v.push_back(static_cast<double>(r)); }
    return v;
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

hp::StreamTally HpsdrSource::frameTally() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return hp::StreamTally{}; }
    std::lock_guard<std::mutex> pl(rx_->p1Mutex);
    return rx_->p1.tally();
}

std::uint64_t HpsdrSource::controlPacketsSent() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->controlPackets.load() : 0;
}

std::uint64_t HpsdrSource::badPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->bad.load() : 0;
}

}  // namespace cascade::source
