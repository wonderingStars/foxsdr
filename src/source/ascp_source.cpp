// ascp_source.cpp - see ascp_source.hpp and ascp_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/ascp_source.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>

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
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::source {

namespace as = ascp;

namespace {

constexpr int kPollMs = 200;
constexpr int kKeepaliveMs = 2000;

#if defined(_WIN32)
using SockT = SOCKET;
const SockT kBad = INVALID_SOCKET;
void closeS(SockT s) { ::closesocket(s); }
void shutS(SockT s) { ::shutdown(s, SD_BOTH); }
bool netUp() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
using SockLen = int;
void setBlocking(SockT s, bool b) {
    u_long m = b ? 0 : 1;
    ::ioctlsocket(s, FIONBIO, &m);
}
#else
using SockT = int;
const SockT kBad = -1;
void closeS(SockT s) { ::close(s); }
void shutS(SockT s) { ::shutdown(s, SHUT_RDWR); }
bool netUp() { return true; }
using SockLen = socklen_t;
void setBlocking(SockT s, bool b) {
    const int f = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, b ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
}
#endif

// A printable, bounded copy of text that came from the network, for a sentence.
std::string tame(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (r.size() >= 40) { r += "..."; break; }
        r += (c >= 32 && c < 127) ? c : '?';
    }
    return r;
}

std::string cstrFrom(const std::vector<std::uint8_t>& body) {
    std::string s;
    for (std::uint8_t b : body) {
        if (b == 0) { break; }
        s += static_cast<char>(b);
    }
    return s;
}

}  // namespace

AscpModel ascpModelFromName(const std::string& n) {
    if (n == "NetSDR") { return AscpModel::NetSdr; }
    if (n == "SDR-IP") { return AscpModel::SdrIp; }
    if (n == "CloudSDR" || n == "CloudIQ") { return AscpModel::CloudIq; }
    if (n == "AFEDRI SDR-Net" || n == "AFE822x SDR-Net") { return AscpModel::Afedri; }
    return AscpModel::Unknown;
}

const char* ascpModelName(AscpModel m) {
    switch (m) {
        case AscpModel::NetSdr: return "NetSDR";
        case AscpModel::SdrIp: return "SDR-IP";
        case AscpModel::CloudIq: return "Cloud-IQ";
        case AscpModel::Afedri: return "Afedri SDR-Net";
        default: return "unknown";
    }
}

std::vector<double> ascpRatesFor(AscpModel m, std::uint64_t mainClockHz) {
    std::vector<double> r;
    auto fromDiv = [&](double clk, std::initializer_list<double> divs) {
        for (double d : divs) { r.push_back(std::floor(clk / d)); }
    };
    switch (m) {
        case AscpModel::NetSdr: fromDiv(80.0e6, {1280.0, 640.0, 320.0, 128.0, 40.0}); break;
        case AscpModel::SdrIp: fromDiv(80.0e6, {1280.0, 640.0, 320.0, 130.0, 40.0}); break;
        case AscpModel::CloudIq: fromDiv(122.88e6, {2560.0, 984.0, 500.0, 200.0, 68.0}); break;
        case AscpModel::Afedri:
            for (std::uint64_t want : {62500ull, 125000ull, 250000ull, 500000ull, 1000000ull, 2000000ull}) {
                const double real = static_cast<double>(as::afedriRealRate(mainClockHz, want));
                if (real > 0.0 && (r.empty() || r.back() != real)) { r.push_back(real); }
            }
            break;
        default: break;
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// the connection, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct AscpSource::Conn {
    SockT tcp = kBad, udp = kBad;
    sockaddr_in radioUdp{};  // where the one-byte keepalive goes (the radio's address, the TCP port)
    std::uint32_t radioIp = 0;  // network order: only datagrams from here are believed
    std::uint16_t localUdp = 0;

    std::mutex sendMutex;
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::uint32_t, std::vector<std::uint8_t>> replies;
    std::map<std::uint32_t, std::uint64_t> replySeq;
    std::uint64_t nakSeq = 0;
    std::atomic<std::uint64_t> overload{0};
    as::StreamAssembler asmb;

    std::mutex seqMutex;
    as::UdpSequence seq;
    std::atomic<std::uint64_t> bad{0};

    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(65536);
    std::vector<std::complex<float>> out;
    int silentTicks = 0, silentLimit = 15;
    std::chrono::steady_clock::time_point nextKeepalive{};

    ~Conn() {
        if (tcp != kBad) { closeS(tcp); }
        if (udp != kBad) { closeS(udp); }
    }

    bool sendBytes(const std::vector<std::uint8_t>& m) {
        if (m.empty()) { return false; }
        std::lock_guard<std::mutex> lk(sendMutex);
        std::size_t sent = 0;
        while (sent < m.size()) {
            const int r = ::send(tcp, reinterpret_cast<const char*>(m.data() + sent), static_cast<int>(m.size() - sent), 0);
            if (r <= 0) { return false; }
            sent += static_cast<std::size_t>(r);
        }
        return true;
    }

    void sendUdpKeepalive() {
        const char b = static_cast<char>(as::kUdpKeepaliveByte);
        ::sendto(udp, &b, 1, 0, reinterpret_cast<const sockaddr*>(&radioUdp), sizeof(radioUdp));
    }

    void dispatch(const as::Message& m) {
        std::lock_guard<std::mutex> lk(mu);
        if (m.nak) {
            ++nakSeq;
        } else if (m.type == as::MsgType::Set || m.type == as::MsgType::RequestRange) {
            // the radio's responses (type 0) and range responses (type 2), keyed by type and item
            const std::uint32_t key = (static_cast<std::uint32_t>(m.type) << 16) | m.item;
            replies[key] = m.body;
            ++replySeq[key];
        } else if (m.type == as::MsgType::Request) {  // radio->host type 1 = unsolicited
            if (m.item == as::ci::StatusCode && !m.body.empty() && m.body[0] == 0x20) { ++overload; }
        }
        cv.notify_all();
    }

    bool pump(NetLink& link) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(tcp, &rf);
        FD_SET(udp, &rf);
        timeval tv{};
        tv.tv_usec = kPollMs * 1000;
#if defined(_WIN32)
        const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
        const int r = ::select(std::max(tcp, udp) + 1, &rf, nullptr, nullptr, &tv);
#endif
        const bool streaming = link.streaming.load(std::memory_order_relaxed);
        if (!link.run.load(std::memory_order_relaxed)) { return false; }
        if (r < 0) {
            link.fault("the radio connection failed", "select failed");
            return false;
        }
        bool gotUdp = false;
        if (r > 0 && FD_ISSET(tcp, &rf)) {
            const int n = ::recv(tcp, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
            if (n <= 0) {
                if (!link.run.load(std::memory_order_relaxed)) { return false; }
                link.fault("the radio closed the connection", n == 0 ? "end of stream" : "receive error");
                return false;
            }
            std::vector<as::Message> msgs;
            asmb.feed(buf.data(), static_cast<std::size_t>(n), msgs);
            for (const as::Message& m : msgs) { dispatch(m); }
            if (!asmb.error().empty()) {
                link.fault("the radio sent a malformed control message", asmb.error());
                return false;
            }
        }
        if (r > 0 && FD_ISSET(udp, &rf)) {
            // drain what is queued, bounded so the TCP side is looked at again
            for (int i = 0; i < 64; ++i) {
                sockaddr_in from{};
                SockLen fl = sizeof(from);
                const int n = ::recvfrom(udp, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from), &fl);
                if (n <= 0) { break; }
                if (from.sin_addr.s_addr != radioIp) { bad.fetch_add(1, std::memory_order_relaxed); continue; }
                as::UdpPacket info;
                out.clear();
                if (!as::parseUdpPacket(buf.data(), static_cast<std::size_t>(n), info, out)) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                gotUdp = true;
                {
                    std::lock_guard<std::mutex> lk(seqMutex);
                    seq.note(info.sequence);
                }
                if (streaming && !out.empty()) { link.pushSamples(out.data(), out.size()); }
                fd_set one;
                FD_ZERO(&one);
                FD_SET(udp, &one);
                timeval z{};
#if defined(_WIN32)
                if (::select(0, &one, nullptr, nullptr, &z) <= 0) { break; }
#else
                if (::select(udp + 1, &one, nullptr, nullptr, &z) <= 0) { break; }
#endif
            }
        }
        if (!streaming) {
            silentTicks = 0;  // idle slices are not silence
        } else if (gotUdp) {
            silentTicks = 0;
        } else if (r == 0 && ++silentTicks >= silentLimit) {
            link.fault("the radio stopped sending", "no I/Q data arrived");
            return false;
        }
        if (streaming) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextKeepalive) {
                nextKeepalive = now + std::chrono::milliseconds(kKeepaliveMs);
                sendBytes(as::encodeMessage(as::MsgType::Request, as::ci::StatusCode));
                sendUdpKeepalive();
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

AscpSource::~AscpSource() { closeDevice(); }

bool AscpSource::failOpen(const std::string& why) {
    const std::string keep = why;
    teardown();
    setError(keep);
    return false;
}

void AscpSource::teardown() {
    active_ = false;
    markOpen(false);
    endReader();
    std::lock_guard<std::mutex> cl(connMutex_);
    conn_.reset();
}

bool AscpSource::transact(as::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                          std::vector<std::uint8_t>& reply, bool& nak) {
    nak = false;
    reply.clear();
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c == nullptr) { setError("no radio connection is open"); return false; }
    const std::uint32_t want = (static_cast<std::uint32_t>(type == as::MsgType::RequestRange ? 2 : 0) << 16) | item;
    std::uint64_t beforeSeq = 0, beforeNak = 0;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        beforeSeq = c->replySeq[want];
        beforeNak = c->nakSeq;
    }
    if (!c->sendBytes(as::encodeMessage(type, item, params))) {
        setError("could not send to the radio (the connection is closed)");
        return false;
    }
    std::unique_lock<std::mutex> lk(c->mu);
    const bool ok = c->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs_), [&] {
        return c->replySeq[want] != beforeSeq || c->nakSeq != beforeNak || faulted();
    });
    if (c->replySeq[want] != beforeSeq) {
        reply = c->replies[want];
        return true;
    }
    if (c->nakSeq != beforeNak) {
        nak = true;
        return true;
    }
    if (faulted()) { setError(faultedWhile().empty() ? "the radio connection failed" : faultedWhile()); return false; }
    (void)ok;
    setError("the radio did not answer (receive timed out)");
    return false;
}

bool AscpSource::transactMandatory(as::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                                   std::vector<std::uint8_t>& reply, const char* what) {
    bool nak = false;
    if (!transact(type, item, params, reply, nak)) {
        setError(std::string(what) + ": " + lastError());
        return false;
    }
    if (nak) {
        setError(std::string("the radio does not implement ") + what);
        return false;
    }
    return true;
}

bool AscpSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::string host = "127.0.0.1", err;
    std::uint16_t port = as::kDefaultPort;
    const std::string addr = argValue(args, "rfspace");
    if (!addr.empty() && !parseHostPort(addr, as::kDefaultPort, host, port, err)) { setError(err); return false; }
    timeoutMs_ = 3000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    long udpPort = port;
    const std::string up = argValue(args, "udpport");
    if (!up.empty()) {
        char* e = nullptr;
        udpPort = std::strtol(up.c_str(), &e, 10);
        if (e == up.c_str() || *e != '\0' || udpPort < 0 || udpPort > 65535) { setError("the UDP port must be 0 to 65535"); return false; }
    }
    wantedRateHz_ = 0.0;
    const std::string rt = argValue(args, "rate");
    if (!rt.empty()) {
        char* e = nullptr;
        const double v = std::strtod(rt.c_str(), &e);
        if (e == rt.c_str() || *e != '\0' || !(v >= 1000.0) || v > 1e7) { setError("the rate must be 1000 to 10000000 samples per second"); return false; }
        wantedRateHz_ = v;
    }
    if (!netUp()) { setError("the network could not be started"); return false; }

    // --- resolve (IPv4 only: the UDP side and the keepalive are IPv4) and connect ---------------
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        setError("could not find the host " + host + " on the network (only IPv4 addresses are supported; is the address right?)");
        return false;
    }
    sockaddr_in peer = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    ::freeaddrinfo(res);
    peer.sin_port = ::htons(port);

    auto conn = std::make_shared<Conn>();
    conn->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    conn->radioIp = peer.sin_addr.s_addr;
    conn->radioUdp = peer;
    conn->tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (conn->tcp == kBad) { setError("could not open a TCP socket"); return false; }
    setBlocking(conn->tcp, false);
    const int cr = ::connect(conn->tcp, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    (void)cr;
    {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(conn->tcp, &wf);
        FD_SET(conn->tcp, &ef);
        timeval tv{};
        tv.tv_sec = 3;
#if defined(_WIN32)
        const int sr = ::select(0, nullptr, &wf, &ef, &tv);
#else
        const int sr = ::select(conn->tcp + 1, nullptr, &wf, &ef, &tv);
#endif
        int soerr = 0;
        SockLen sl = sizeof(soerr);
        if (sr > 0) { ::getsockopt(conn->tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl); }
        if (sr <= 0 || soerr != 0 || FD_ISSET(conn->tcp, &ef)) {
            setError("could not reach the radio at " + host + ":" + std::to_string(port) +
                     (sr <= 0 ? " (no answer in 3 s)" : " (connection refused)") +
                     " - is the address right, and is the radio's control port open?");
            return false;
        }
    }
    setBlocking(conn->tcp, true);
    int nodelay = 1;
    ::setsockopt(conn->tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    conn->udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (conn->udp == kBad) { setError("could not open a UDP socket"); return false; }
    {
        int big = 4 * 1024 * 1024;
        ::setsockopt(conn->udp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = ::htonl(INADDR_ANY);
        me.sin_port = ::htons(static_cast<std::uint16_t>(udpPort));
        if (::bind(conn->udp, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
            setError("could not use UDP port " + std::to_string(udpPort) +
                     " here (another program, or another radio, has it; try udpport=0)");
            return false;
        }
        SockLen ml = sizeof(me);
        if (::getsockname(conn->udp, reinterpret_cast<sockaddr*>(&me), &ml) == 0) { conn->localUdp = ::ntohs(me.sin_port); }
    }
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_ = conn;
    }
    if (!beginReader([conn](NetLink& l) { return conn->pump(l); }, [conn]() { shutS(conn->tcp); }, conn)) {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_.reset();
        return false;
    }

    // --- identify ----------------------------------------------------------------------------
    std::vector<std::uint8_t> rep;
    bool nak = false;
    if (!transact(as::MsgType::Request, as::ci::InterfaceName, {}, rep, nak)) {
        return failOpen("this does not look like an RFspace or Afedri network radio: it did not answer the name request (" + std::string(lastError()) + ")");
    }
    if (nak || rep.empty()) { return failOpen("this does not look like an RFspace or Afedri network radio: it refused the name request"); }
    name_ = cstrFrom(rep);
    model_ = ascpModelFromName(name_);
    if (model_ == AscpModel::Unknown) {
        return failOpen("the device at " + host + ":" + std::to_string(port) + " calls itself \"" + tame(name_) +
                        "\", which this build does not know (it knows NetSDR, SDR-IP, Cloud-IQ and Afedri SDR-Net)");
    }
    serial_.clear();
    if (transact(as::MsgType::Request, as::ci::SerialNumber, {}, rep, nak) && !nak) { serial_ = cstrFrom(rep); }
    setError("");
    mainClockHz_ = 80000000;
    if (model_ == AscpModel::Afedri) {
        // body: channel (not used), then the clock in Hz as 4 bytes (the Afedri document's example answer)
        if (transact(as::MsgType::Request, as::ci::RxInSampleRate, {}, rep, nak) && !nak && rep.size() >= 5) {
            const std::uint64_t clk = as::getLe(rep.data() + 1, 4);
            if (clk >= 10000000ull && clk <= 400000000ull) { mainClockHz_ = clk; }
        }
        setError("");
    }
    haveRange_ = false;
    {
        std::vector<std::uint8_t> p{as::kChannel1};
        std::vector<as::FreqRange> rs;
        if (transact(as::MsgType::RequestRange, as::ci::RxFrequency, p, rep, nak) && !nak && as::parseFrequencyRanges(rep, rs)) {
            range_ = rs.front();
            for (const as::FreqRange& r : rs) {
                range_.minHz = std::min(range_.minHz, r.minHz);
                range_.maxHz = std::max(range_.maxHz, r.maxHz);
            }
            haveRange_ = true;
        }
        setError("");
    }
    freqHz_ = 0;
    if (transact(as::MsgType::Request, as::ci::RxFrequency, {as::kChannel1}, rep, nak) && !nak && rep.size() >= 6) {
        freqHz_ = as::getFrequency(rep.data() + 1);
    }
    setError("");
    rateHz_ = 0.0;
    if (transact(as::MsgType::Request, as::ci::RxOutSampleRate, {as::kChannel1}, rep, nak) && !nak && rep.size() >= 5) {
        rateHz_ = static_cast<double>(as::getLe(rep.data() + 1, 4));
    }
    setError("");
    const std::vector<double> rates = ascpRatesFor(model_, mainClockHz_);
    if (wantedRateHz_ <= 0.0) { wantedRateHz_ = rates.size() > 2 ? rates[2] : (rates.empty() ? 250000.0 : rates.front()); }
    rfGainDb_ = 0;
    markOpen(true);
    setName(std::string("RFspace-protocol radio: ") + ascpModelName(model_) + " (" + host + ")");
    return true;
}

void AscpSource::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() && !conn_) { endReader(); return; }
    const std::string keep = lastError();
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c && active_) {
        c->sendBytes(as::encodeMessage(as::MsgType::Set, as::ci::RxState, {as::kRxStateComplex, as::kRxIdle, 0, 0}));  // best effort
    }
    c.reset();
    teardown();
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool AscpSource::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("start() called with no radio open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (!c) { setError("start() called with no radio open"); return false; }
    std::vector<std::uint8_t> rep;
    bool nak = false;
    // The optional items: the radio may answer "not implemented"; that is not an error here.
    const bool cloud = model_ == AscpModel::CloudIq;
    transact(as::MsgType::Set, as::ci::RxChanSetup, {as::kChanSetupSingle1}, rep, nak);
    transact(as::MsgType::Set, as::ci::RxRfFilter, {as::kChannel1, cloud ? as::kRfFilterBypass : as::kRfFilterAuto}, rep, nak);
    transact(as::MsgType::Set, as::ci::RxAdModes, {as::kChannel1, static_cast<std::uint8_t>(as::kAdDither | as::kAdPga)}, rep, nak);
    transact(as::MsgType::Set, as::ci::RxSyncInMode, {0, as::kSyncInOff}, rep, nak);
    transact(as::MsgType::Set, as::ci::RxPulseOutMode, {0, as::kPulseOutOff}, rep, nak);
    setError("");
    // the rate
    std::vector<std::uint8_t> rp{as::kChannel1};
    as::putLe(rp, static_cast<std::uint32_t>(wantedRateHz_), 4);
    if (!transactMandatory(as::MsgType::Set, as::ci::RxOutSampleRate, rp, rep, "setting the sample rate")) { return false; }
    if (rep.size() < 5) { setError("the radio's answer to the sample-rate request was too short"); return false; }
    const double real = static_cast<double>(as::getLe(rep.data() + 1, 4));
    if (!(real >= 1000.0)) { setError("the radio reported a sample rate it cannot be using"); return false; }
    rateHz_ = real;
    transact(as::MsgType::Set, as::ci::RxOutputParams, {rateHz_ > 100000.0 ? as::kOutputLargeMtu : as::kOutputSmallMtu}, rep, nak);
    setError("");
    {
        std::lock_guard<std::mutex> sl(c->seqMutex);
        c->seq.reset();
    }
    c->nextKeepalive = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKeepaliveMs);
    if (!startKeeping()) { return false; }
    c->sendUdpKeepalive();
    const std::uint8_t mode = rateHz_ < 1500000.0 ? as::kModeContiguous24 : as::kModeContiguous16;
    if (!transactMandatory(as::MsgType::Set, as::ci::RxState, {as::kRxStateComplex, as::kRxOn, mode, 0}, rep, "starting the receiver")) {
        stopKeeping();
        return false;
    }
    active_ = true;
    return true;
}

void AscpSource::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    stopKeeping();
    if (active_) {
        std::shared_ptr<Conn> c;
        {
            std::lock_guard<std::mutex> cl(connMutex_);
            c = conn_;
        }
        if (c) {
            std::vector<std::uint8_t> rep;
            bool nak = false;
            transact(as::MsgType::Set, as::ci::RxState, {as::kRxStateComplex, as::kRxIdle, 0, 0}, rep, nak);
        }
    }
    active_ = false;
}

double AscpSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return rateHz_ > 0.0 ? rateHz_ : wantedRateHz_;
}

std::vector<double> AscpSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return ascpRatesFor(model_, mainClockHz_);
}

bool AscpSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no radio open"); return false; }
    if (active_) { setError("stop the radio before changing its sample rate"); return false; }
    const std::vector<double> rates = ascpRatesFor(model_, mainClockHz_);
    bool ok = false;
    if (model_ == AscpModel::Afedri) {
        ok = hz >= 8000.0 && hz <= 2000000.0;  // the Afedri document: up to 2000 kHz in single-channel mode
        if (ok) { wantedRateHz_ = hz; }
    } else {
        for (double r : rates) {
            if (std::fabs(r - hz) <= 1.0) { ok = true; wantedRateHz_ = r; }
        }
    }
    if (!ok) {
        setError(model_ == AscpModel::Afedri ? "an Afedri radio takes 8 kS/s to 2 MS/s"
                                              : std::string("this ") + ascpModelName(model_) + " offers only its listed sample rates");
        return false;
    }
    rateHz_ = 0.0;
    return true;
}

double AscpSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return static_cast<double>(freqHz_);
}

bool AscpSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no radio open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz) || hz > 1.0e12) { setError("setCenterFrequencyHz() requires a frequency"); return false; }
    const std::uint64_t f = static_cast<std::uint64_t>(std::llround(hz));
    if (haveRange_ && (f < range_.minHz || f > range_.maxHz)) {
        setError("the radio tunes " + std::to_string(range_.minHz) + " to " + std::to_string(range_.maxHz) + " Hz; " + std::to_string(f) + " Hz is outside that");
        return false;
    }
    std::vector<std::uint8_t> p{as::kChannel1}, rep;
    as::putFrequency(p, f);
    if (!transactMandatory(as::MsgType::Set, as::ci::RxFrequency, p, rep, "tuning")) { return false; }
    freqHz_ = rep.size() >= 6 ? as::getFrequency(rep.data() + 1) : f;
    return true;
}

bool AscpSource::frequencyRangeHz(double& lo, double& hi) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!haveRange_) { return false; }
    lo = static_cast<double>(range_.minHz);
    hi = static_cast<double>(range_.maxHz);
    return true;
}

std::vector<GainInfo> AscpSource::gains() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::vector<GainInfo> v;
    if (isOpen()) {
        GainInfo g;
        g.name = "RF gain";
        g.minDb = -30.0;
        g.maxDb = 0.0;
        g.stepDb = 10.0;
        v.push_back(g);
    }
    return v;
}

bool AscpSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (name != "RF gain") { setError("this radio has one gain, \"RF gain\""); return false; }
    if (!isOpen()) { setError("setGainDb() called with no radio open"); return false; }
    const long v = std::lround(db);
    if (std::fabs(db - static_cast<double>(v)) > 0.01 || (v != 0 && v != -10 && v != -20 && v != -30)) {
        setError("the RF gain takes 0, -10, -20 or -30 dB");
        return false;
    }
    std::vector<std::uint8_t> rep;
    if (!transactMandatory(as::MsgType::Set, as::ci::RxRfGain, {as::kChannel1, static_cast<std::uint8_t>(static_cast<std::int8_t>(v))}, rep, "setting the RF gain")) { return false; }
    rfGainDb_ = static_cast<int>(v);
    return true;
}

double AscpSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return name == "RF gain" ? rfGainDb_ : 0.0;
}

AscpModel AscpSource::model() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return model_;
}
std::string AscpSource::interfaceName() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return name_;
}
std::string AscpSource::serialNumber() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return serial_;
}
std::uint16_t AscpSource::localUdpPort() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->localUdp : 0;
}
std::uint64_t AscpSource::packetsLost() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (!c) { return 0; }
    std::lock_guard<std::mutex> lk(c->seqMutex);
    return c->seq.lost();
}
std::uint64_t AscpSource::packetsLate() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (!c) { return 0; }
    std::lock_guard<std::mutex> lk(c->seqMutex);
    return c->seq.late();
}
std::uint64_t AscpSource::badPackets() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->bad.load() : 0;
}
std::uint64_t AscpSource::overloadReports() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->overload.load() : 0;
}

}  // namespace cascade::source
