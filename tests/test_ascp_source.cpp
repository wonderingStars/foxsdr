// test_ascp_source.cpp - the RFspace / Afedri "ASCP" (SDR-IP) network-radio client, proven against a
// STAND-IN radio on loopback (TCP control + UDP stream), and the wire format against the literal bytes
// printed in Afedri's own document.
//
// WHAT THIS CAN AND CANNOT PROVE. The stand-in is written from the same reading as the client
// (src/source/ascp_protocol.hpp lists the sources): it has its own message writer with literal numbers,
// but it cannot show the reading right. The literal-byte tests (7050000 Hz on channel 2, RF gain 0x64,
// the main-clock request and its 80 MHz answer, the UDP header 04 84) ARE checked against bytes printed
// in the manufacturer's document, so those are independent. UNPROVEN: a real NetSDR / SDR-IP / Cloud-IQ /
// Afedri radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/ascp_protocol.hpp"
#include "source/ascp_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::AscpModel;
using cascade::source::AscpSource;
using cascade::source::NetSampleSource;
namespace as = cascade::source::ascp;

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- the stand-in radio ------------------------------------------------------------------

enum class Quirk {
    None,
    NakOptional,        // answers 02 00 to the optional setup items
    NoUdp,              // accepts everything, never sends a stream packet
    CloseAfterStart,    // drops the TCP connection once the receiver is on
    JunkControl,        // answers the name request with a header whose length is 1
    SilentOnName,       // never answers anything
    DropEvery7,         // leaves out every seventh UDP packet
    RepeatEvery9,       // sends every ninth UDP packet twice
    BadUdp,             // interleaves undersized / mis-headed datagrams
    TcpGarbageDuring,   // random bytes on the control connection while streaming
    NakGain,            // answers 02 00 to the RF gain item
    OverloadStatus      // sends unsolicited A/D-overload status while streaming
};

struct RadioSpec {
    std::string name = "NetSDR";
    std::string serial = "0123ABCD";
    std::uint64_t clockHz = 80000000;
    std::uint64_t minHz = 100, maxHz = 34000000;
    Quirk quirk = Quirk::None;
    double toneHz = 12345.0;       // the tone the stream carries, in the radio's own time
    double ppm = 800.0;            // the radio's sample clock is off by this much (a 12.3 kHz tone moves ~10 Hz)
    double noise = 0.02;           // noise standard deviation as a fraction of full scale
    double amp = 0.4;
    int initialRate = 250000;
};

// One bounded poll (srctest::waitRead blocks until readable or the flag drops).
bool pollRead(Sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_usec = ms * 1000;
#if defined(_WIN32)
    return ::select(0, &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(s + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

int lastSockError() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

Bytes ctl(as::MsgType t, std::uint16_t item, const Bytes& params) {
    return as::encodeMessage(t, item, params);
}

class Radio {
public:
    explicit Radio(RadioSpec s) : spec_(std::move(s)) {}
    ~Radio() { stop(); }

    bool start() {
        // The client sends its keepalive datagram to (127.0.0.1, TCP port): the radio's UDP socket and its
        // TCP listener must share one number. Take a free UDP port first, then try to listen on it.
        startNet();  // before the first socket of the process
        bool listening = false;
        static std::atomic<std::uint32_t> salt{static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
        Rng pick(salt.fetch_add(0x9E3779B9u) | 1u);
        for (int attempt = 0; attempt < 200 && !listening; ++attempt) {
            // a random number in the dynamic range: the OS's own choice walks sequentially into the same
            // excluded range (Windows reserves hundreds of ports in blocks of ~100) and fails 100 times in a row
            const std::uint16_t want = static_cast<std::uint16_t>(49152 + pick.next() % 16384);
            if (bindUdpNextToTcp) {
                udp_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
                a.sin_port = ::htons(want);
                if (udp_ == kBadSock || ::bind(udp_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                    lastBindError = lastSockError();
                    closeSock(udp_);
                    udp_ = kBadSock;
                    continue;
                }
            }
            tcp_ = std::make_unique<TcpServer>();
            if (tcp_->start([this](Sock c, std::size_t, const std::atomic<bool>& run) { serve(c, run); }, want)) {
                listening = true;
                break;
            }
            lastBindError = lastSockError();
            tcp_.reset();
            closeSock(udp_);
            udp_ = kBadSock;
        }
        if (!listening) {
            std::printf("stand-in radio could not pair a TCP and a UDP port (last error %d)\n", lastBindError);
            return false;
        }
        run_.store(true);
        if (udp_ != kBadSock) { udpThread_ = std::thread([this] { udpLoop(); }); }
        return true;
    }
    void stop() {
        run_.store(false);
        if (tcp_) { tcp_->stop(); }
        if (udpThread_.joinable()) { udpThread_.join(); }
        if (udp_ != kBadSock) { closeSock(udp_); udp_ = kBadSock; }
    }
    std::uint16_t port() const { return tcp_ ? tcp_->port() : 0; }
    std::string args(const std::string& extra = "") const {
        // udpport=0 unless asked otherwise: on one host the radio's UDP socket (bound to the TCP port number,
        // where the keepalive goes) would otherwise be the more specific match for the client's own datagrams
        const bool own = extra.find("udpport=") != std::string::npos;
        return "rfspace=127.0.0.1:" + std::to_string(port()) + (own ? "" : ",udpport=0") + (extra.empty() ? "" : "," + extra);
    }
    bool receiverOn() const { return on_.load(); }
    int lastBindError = 0;
    bool bindUdpNextToTcp = true;  // false: no radio-side UDP socket (the default-port test holds the number itself)

    RadioSpec spec_;
    std::atomic<int> rate{250000};
    std::atomic<std::uint64_t> freq{0};
    std::atomic<int> gain{0};
    std::atomic<int> onCount{0}, offCount{0}, keepalives{0}, statusRequests{0};
    std::atomic<int> bits24{-1};  // the mode byte last seen with receiver-on: 1 = 24-bit contiguous
    std::atomic<int> mtuSmall{-1};
    std::atomic<int> sent{0};
    std::atomic<int> unknownMessages{0};
    std::mutex logMutex;
    std::vector<std::uint16_t> itemsSeen;

private:
    void reply(Sock c, as::MsgType t, std::uint16_t item, const Bytes& p) {
        const Bytes m = ctl(t, item, p);
        sendExact(c, m.data(), m.size());
    }
    void nak(Sock c) {
        const Bytes m = {0x02, 0x00};
        sendExact(c, m.data(), m.size());
    }
    static Bytes freqBytes(std::uint64_t hz) {
        Bytes b;
        as::putFrequency(b, hz);
        return b;
    }
    int snap(int wanted) const {
        if (spec_.name == "AFEDRI SDR-Net" || spec_.name == "AFE822x SDR-Net") {
            return static_cast<int>(as::afedriRealRate(spec_.clockHz, static_cast<std::uint64_t>(wanted)));
        }
        return wanted;  // a fixed-table radio is sent exact table values
    }

    void handle(Sock c, const as::Message& m) {
        {
            std::lock_guard<std::mutex> lk(logMutex);
            itemsSeen.push_back(m.item);
        }
        if (spec_.quirk == Quirk::SilentOnName) { return; }
        const bool isSet = m.type == as::MsgType::Set;
        const bool isReq = m.type == as::MsgType::Request;
        const bool isRange = m.type == as::MsgType::RequestRange;
        if (m.type == as::MsgType::Data0 || m.nak) { return; }
        switch (m.item) {
            case as::ci::InterfaceName: {
                if (spec_.quirk == Quirk::JunkControl) { const Bytes junk = {0x01, 0x00, 0xFF}; sendExact(c, junk.data(), junk.size()); return; }
                Bytes b(spec_.name.begin(), spec_.name.end());
                b.push_back(0);
                reply(c, as::MsgType::Set, m.item, b);
                return;
            }
            case as::ci::SerialNumber: {
                Bytes b(spec_.serial.begin(), spec_.serial.end());
                b.push_back(0);
                reply(c, as::MsgType::Set, m.item, b);
                return;
            }
            case as::ci::RxInSampleRate: {
                Bytes b{0};
                as::putLe(b, spec_.clockHz, 4);
                reply(c, as::MsgType::Set, m.item, b);
                return;
            }
            case as::ci::StatusCode: {
                ++statusRequests;
                reply(c, as::MsgType::Set, m.item, Bytes{0x0B});
                return;
            }
            case as::ci::RxFrequency: {
                if (isRange) {
                    Bytes b{0, 1};
                    as::putFrequency(b, spec_.minHz);
                    as::putFrequency(b, spec_.maxHz);
                    as::putFrequency(b, 0);
                    reply(c, as::MsgType::RequestRange, m.item, b);
                    return;
                }
                if (isSet && m.body.size() >= 6) { freq.store(as::getFrequency(m.body.data() + 1)); }
                Bytes b{0};
                as::putFrequency(b, freq.load());
                reply(c, as::MsgType::Set, m.item, b);
                return;
            }
            case as::ci::RxOutSampleRate: {
                if (isSet && m.body.size() >= 5) { rate.store(snap(static_cast<int>(as::getLe(m.body.data() + 1, 4)))); }
                Bytes b{0};
                as::putLe(b, static_cast<std::uint32_t>(rate.load()), 4);
                reply(c, as::MsgType::Set, m.item, b);
                return;
            }
            case as::ci::RxRfGain: {
                if (spec_.quirk == Quirk::NakGain) { nak(c); return; }
                if (isSet && m.body.size() >= 2) { gain.store(static_cast<std::int8_t>(m.body[1])); }
                reply(c, as::MsgType::Set, m.item, Bytes{0, static_cast<std::uint8_t>(static_cast<std::int8_t>(gain.load()))});
                return;
            }
            case as::ci::RxOutputParams: {
                if (spec_.quirk == Quirk::NakOptional) { nak(c); return; }
                if (isSet && !m.body.empty()) { mtuSmall.store(m.body[0]); }
                reply(c, as::MsgType::Set, m.item, Bytes{m.body.empty() ? std::uint8_t{0} : m.body[0]});
                return;
            }
            case as::ci::RxState: {
                if (isSet && m.body.size() >= 3) {
                    if (m.body[1] == as::kRxOn) {
                        bits24.store(m.body[2] == as::kModeContiguous24 ? 1 : 0);
                        on_.store(true);
                        ++onCount;
                    } else {
                        on_.store(false);
                        ++offCount;
                    }
                }
                reply(c, as::MsgType::Set, m.item, Bytes{m.body.size() > 0 ? m.body[0] : std::uint8_t{0}, m.body.size() > 1 ? m.body[1] : std::uint8_t{0},
                                                      m.body.size() > 2 ? m.body[2] : std::uint8_t{0}, 0});
                if (spec_.quirk == Quirk::CloseAfterStart && on_.load()) { closeNow_.store(true); }
                return;
            }
            case as::ci::RxChanSetup:
            case as::ci::RxRfFilter:
            case as::ci::RxAdModes:
            case as::ci::RxSyncInMode:
            case as::ci::RxPulseOutMode: {
                if (spec_.quirk == Quirk::NakOptional) { nak(c); return; }
                reply(c, as::MsgType::Set, m.item, m.body);
                return;
            }
            default:
                ++unknownMessages;
                (void)isReq;
                nak(c);
                return;
        }
    }

    void serve(Sock c, const std::atomic<bool>& run) {
        as::StreamAssembler asmb;
        std::uint8_t buf[2048];
        Rng rng(7);
        int sinceGarbage = 0;
        while (run.load() && !closeNow_.load()) {
            if (spec_.quirk == Quirk::TcpGarbageDuring && on_.load() && ++sinceGarbage > 3) {
                sinceGarbage = 0;
                Bytes g(32);
                for (auto& x : g) { x = static_cast<std::uint8_t>(rng.next()); }
                g[1] |= 0x20;  // a plausible header type
                sendExact(c, g.data(), g.size());
            }
            if (spec_.quirk == Quirk::OverloadStatus && on_.load() && ++sinceGarbage > 2) {
                sinceGarbage = 0;
                const Bytes st = ctl(as::MsgType::Request, as::ci::StatusCode, Bytes{0x20});  // radio->host type 1: unsolicited
                sendExact(c, st.data(), st.size());
            }
            if (!pollRead(c, 20)) { continue; }
            const int n = ::recv(c, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) { break; }
            std::vector<as::Message> msgs;
            asmb.feed(buf, static_cast<std::size_t>(n), msgs);
            for (const as::Message& m : msgs) { handle(c, m); }
        }
        if (closeNow_.load()) { ::shutdown(c, 2); }
    }

    void udpLoop() {
        sockaddr_in client{};
        bool haveClient = false;
        double phase = 0.0;
        std::uint16_t seq = 0;
        int count = 0;
        Rng rng(99);
        Bytes dg(2048);
        while (run_.load()) {
            // keepalive datagrams from the client (and nothing else) arrive here
            if (pollRead(udp_, 1)) {
                sockaddr_in from{};
                SockLen fl = sizeof(from);
                const int n = ::recvfrom(udp_, reinterpret_cast<char*>(dg.data()), static_cast<int>(dg.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (n == 1 && dg[0] == as::kUdpKeepaliveByte) {
                    client = from;
                    haveClient = true;
                    ++keepalives;
                }
            }
            if (!on_.load()) { seq = 0; count = 0; continue; }
            if (!haveClient || spec_.quirk == Quirk::NoUdp) { continue; }
            const bool b24 = bits24.load() == 1;
            const bool small = mtuSmall.load() == 1;
            const std::size_t len = b24 ? (small ? as::kUdpSmall24 : as::kUdpLarge24) : (small ? as::kUdpSmall16 : as::kUdpLarge16);
            const std::size_t step = b24 ? 6 : 4;
            const double fs = static_cast<double>(rate.load()) * (1.0 + spec_.ppm * 1e-6);
            const double dphi = 2.0 * kPi * spec_.toneHz / fs;
            // several packets per tick: a Windows sleep is ~15 ms, and a small-packet stream needs more than one
            const int burst = std::max(1, static_cast<int>(1600 / ((len - 4) / step)));
            for (int bi = 0; bi < burst; ++bi) {
            Bytes p(len);
            const std::uint16_t h = static_cast<std::uint16_t>((4u << 13) | len);
            p[0] = static_cast<std::uint8_t>(h);
            p[1] = static_cast<std::uint8_t>(h >> 8);
            p[2] = static_cast<std::uint8_t>(seq);
            p[3] = static_cast<std::uint8_t>(seq >> 8);
            for (std::size_t i = 4; i + step <= len; i += step) {
                auto nz = [&]() { return (static_cast<double>(rng.next() & 0xFFFF) / 65535.0 - 0.5) * 2.0 * spec_.noise * 1.7; };
                const double re = spec_.amp * std::cos(phase) + nz();
                const double im = spec_.amp * std::sin(phase) + nz();
                phase += dphi;
                if (phase > 2.0 * kPi) { phase -= 2.0 * kPi; }
                if (b24) {
                    const std::int32_t a = static_cast<std::int32_t>(std::lround(re * 8388607.0)), q = static_cast<std::int32_t>(std::lround(im * 8388607.0));
                    for (int k = 0; k < 3; ++k) { p[i + k] = static_cast<std::uint8_t>(a >> (8 * k)); p[i + 3 + k] = static_cast<std::uint8_t>(q >> (8 * k)); }
                } else {
                    const std::int16_t a = static_cast<std::int16_t>(std::lround(re * 32767.0)), q = static_cast<std::int16_t>(std::lround(im * 32767.0));
                    p[i] = static_cast<std::uint8_t>(a); p[i + 1] = static_cast<std::uint8_t>(a >> 8);
                    p[i + 2] = static_cast<std::uint8_t>(q); p[i + 3] = static_cast<std::uint8_t>(q >> 8);
                }
            }
            ++count;
            auto sendDg = [&](const Bytes& b) {
                ::sendto(udp_, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0, reinterpret_cast<const sockaddr*>(&client), sizeof(client));
            };
            const bool dropped = spec_.quirk == Quirk::DropEvery7 && count % 7 == 0;
            if (spec_.quirk == Quirk::BadUdp && count % 5 == 0) {
                Bytes shortp(p.begin(), p.begin() + 100);        sendDg(shortp);
                Bytes wrongLen = p; wrongLen[0] ^= 0x10;         sendDg(wrongLen);
                Bytes notData = p; notData[1] = 0x20 | (p[1] & 0x1F); sendDg(notData);  // type 1, not a data item
            }
            if (!dropped) { sendDg(p); }
            if (spec_.quirk == Quirk::RepeatEvery9 && count % 9 == 0) { sendDg(p); }
            ++sent;
            seq = static_cast<std::uint16_t>(seq == 0xFFFF ? 1 : seq + 1);
            }
            // about 500 packets a second at most: the receiver keeps up on a loaded machine
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::unique_ptr<TcpServer> tcp_;
    Sock udp_ = kBadSock;
    std::thread udpThread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> on_{false};
    std::atomic<bool> closeNow_{false};
};

std::vector<std::complex<float>> readN(AscpSource& s, std::size_t n, int ms = 5000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(1024);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

// The tone's frequency from the mean phase step between neighbours (robust to noise, to a few lost
// samples it is not: callers use clean runs).
double toneHz(const std::vector<std::complex<float>>& v, double fs) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 1; i < v.size(); ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
    return std::arg(acc) * fs / (2.0 * kPi);
}

double meanMag(const std::vector<std::complex<float>>& v) {
    double m = 0;
    for (auto& x : v) { m += std::abs(x); }
    return v.empty() ? 0 : m / static_cast<double>(v.size());
}

// --- tests -----------------------------------------------------------------------------

void testDocumentBytes() {
    // Afedri "AFE822x SDR-Net protocol extensions", section IV: set the NCO frequency 7050000 Hz on
    // channel 2 (0x006B9310) : 0A 00 | 20 00 | 02 | 10 93 6B 00 00
    {
        Bytes p{0x02};
        as::putFrequency(p, 7050000);
        const Bytes m = as::encodeMessage(as::MsgType::Set, as::ci::RxFrequency, p);
        const Bytes want = {0x0A, 0x00, 0x20, 0x00, 0x02, 0x10, 0x93, 0x6B, 0x00, 0x00};
        CHECK(m == want);
        as::StreamAssembler a;
        std::vector<as::Message> out;
        a.feed(want.data(), want.size(), out);
        CHECK(out.size() == 1 && out[0].type == as::MsgType::Set && out[0].item == 0x20 && out[0].body.size() == 6);
        CHECK(as::getFrequency(out[0].body.data() + 1) == 7050000);
    }
    // RF gain +14 dB value 0x64 on channel 2: 06 00 | 38 00 | 02 | 64
    {
        const Bytes m = as::encodeMessage(as::MsgType::Set, as::ci::RxRfGain, Bytes{0x02, 0x64});
        CHECK((m == Bytes{0x06, 0x00, 0x38, 0x00, 0x02, 0x64}));
    }
    // "Request Main Clock frequency": 04 20 B0 00 ; the answer 09 00 B0 00 00 00 B4 C4 04 is 80000000
    {
        const Bytes q = as::encodeMessage(as::MsgType::Request, as::ci::RxInSampleRate);
        CHECK((q == Bytes{0x04, 0x20, 0xB0, 0x00}));
        const Bytes a = {0x09, 0x00, 0xB0, 0x00, 0x00, 0x00, 0xB4, 0xC4, 0x04};
        as::StreamAssembler asmb;
        std::vector<as::Message> out;
        asmb.feed(a.data(), a.size(), out);
        CHECK(out.size() == 1 && out[0].item == 0xB0 && out[0].body.size() == 5);
        CHECK(as::getLe(out[0].body.data() + 1, 4) == 80000000ull);
    }
    // The UDP data item header of the 1024-byte packet: 04 84 = type 4, length 1028.
    {
        Bytes p(1028, 0);
        p[0] = 0x04; p[1] = 0x84; p[2] = 0x34; p[3] = 0x12;
        p[4] = 0xFF; p[5] = 0x7F; p[6] = 0x00; p[7] = 0x80;  // I = 32767, Q = -32768
        as::UdpPacket info;
        std::vector<std::complex<float>> out;
        CHECK(as::parseUdpPacket(p.data(), p.size(), info, out));
        CHECK(info.sequence == 0x1234 && info.bits == 16 && info.pairs == 256 && out.size() == 256);
        CHECK_NEAR(out[0].real(), 32767.0 / 32768.0, 1e-6);
        CHECK_NEAR(out[0].imag(), -1.0, 1e-6);
    }
    // Data ack: 03 60 <channel>
    CHECK((as::encodeDataAck(5) == Bytes{0x03, 0x60, 0x05}));
    // Afedri's rate function: 76.8 MHz, 250 kS/s requested -> decimation 77 -> 249350 (the document
    // computes 249350.6); 80 MHz at 2 MS/s is decimation 10 exactly; zero arguments give 0.
    CHECK(as::afedriRealRate(76800000, 250000) == 249350);
    CHECK(as::afedriRealRate(80000000, 2000000) == 2000000);
    CHECK(as::afedriRealRate(80000000, 62500) == 62500);
    CHECK(as::afedriRealRate(0, 1000) == 0 && as::afedriRealRate(1000, 0) == 0);
    CHECK(as::afedriRealRate(80000000, 100000000) > 0);  // a rate above the clock cannot divide by zero
}

void testAssembler() {
    // byte-at-a-time
    {
        Bytes stream;
        for (std::uint16_t it : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{0x20}}) {
            const Bytes m = as::encodeMessage(as::MsgType::Set, it, Bytes{1, 2, 3});
            stream.insert(stream.end(), m.begin(), m.end());
        }
        stream.push_back(0x02);  // a NAK
        stream.push_back(0x00);
        as::StreamAssembler a;
        std::vector<as::Message> out;
        for (std::uint8_t b : stream) { a.feed(&b, 1, out); }
        CHECK(a.error().empty() && out.size() == 4);
        CHECK(out[0].item == 1 && out[1].item == 2 && out[2].item == 0x20 && out[3].nak);
        CHECK(out[2].body == (Bytes{1, 2, 3}));
    }
    // the impossible lengths
    for (const Bytes& bad : {Bytes{0x01, 0x00}, Bytes{0x00, 0x00}, Bytes{0x03, 0x00, 0x00}}) {
        as::StreamAssembler a;
        std::vector<as::Message> out;
        a.feed(bad.data(), bad.size(), out);
        CHECK(!a.error().empty() && out.empty());
        // after an error nothing more is believed
        const Bytes ok = as::encodeMessage(as::MsgType::Set, 1);
        a.feed(ok.data(), ok.size(), out);
        CHECK(out.empty());
    }
    // length 0 on a data message means 8194 bytes
    {
        Bytes m(8194, 0x55);
        m[0] = 0x00; m[1] = 0x80;  // type 4, length field 0
        as::StreamAssembler a;
        std::vector<as::Message> out;
        a.feed(m.data(), m.size(), out);
        CHECK(a.error().empty() && out.size() == 1 && out[0].body.size() == 8192);
    }
    // an ack
    {
        const Bytes m = as::encodeDataAck(2);
        as::StreamAssembler a;
        std::vector<as::Message> out;
        a.feed(m.data(), m.size(), out);
        CHECK(out.size() == 1 && out[0].type == as::MsgType::DataAck && out[0].body == (Bytes{2}));
    }
    // the largest control message fits; one more byte does not
    CHECK(!as::encodeMessage(as::MsgType::Set, 1, Bytes(0x1FFF - 4, 0)).empty());
    CHECK(as::encodeMessage(as::MsgType::Set, 1, Bytes(0x1FFF - 3, 0)).empty());
}

void testRanges() {
    Bytes b{0, 2};
    for (std::uint64_t v : {100ull, 34000000ull, 0ull, 144000000ull, 146000000ull, 0ull}) { as::putFrequency(b, v); }
    std::vector<as::FreqRange> rs;
    CHECK(as::parseFrequencyRanges(b, rs) && rs.size() == 2);
    CHECK(rs[0].minHz == 100 && rs[0].maxHz == 34000000 && rs[1].minHz == 144000000 && rs[1].maxHz == 146000000);
    Bytes cut(b.begin(), b.end() - 1);
    CHECK(!as::parseFrequencyRanges(cut, rs));
    CHECK(!as::parseFrequencyRanges(Bytes{0, 0}, rs) && !as::parseFrequencyRanges(Bytes{0}, rs));
    Bytes inverted{0, 1};
    for (std::uint64_t v : {500ull, 100ull, 0ull}) { as::putFrequency(inverted, v); }
    CHECK(!as::parseFrequencyRanges(inverted, rs));
}

void testUdpParsing() {
    // 24-bit: I = 0x7FFFFF, Q = 0x800000 (-8388608), then I = 1, Q = -1
    Bytes p(1444, 0);
    p[0] = 0xA4; p[1] = 0x85;  // 0x85A4 = type 4, length 1444
    p[2] = 0xFF; p[3] = 0xFF;
    const std::uint8_t px[12] = {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    std::memcpy(&p[4], px, 12);
    as::UdpPacket info;
    std::vector<std::complex<float>> out;
    CHECK(as::parseUdpPacket(p.data(), p.size(), info, out));
    CHECK(info.bits == 24 && info.pairs == 240 && out.size() == 240 && info.sequence == 0xFFFF);
    CHECK_NEAR(out[0].real(), 8388607.0 / 8388608.0, 1e-7);
    CHECK_NEAR(out[0].imag(), -1.0, 1e-7);
    CHECK_NEAR(out[1].real(), 1.0 / 8388608.0, 1e-9);
    CHECK_NEAR(out[1].imag(), -1.0 / 8388608.0, 1e-9);
    // the four sizes and nothing else
    int accepted = 0;
    for (std::size_t n = 0; n < 2000; ++n) {
        Bytes q(n, 0);
        if (n >= 2) {
            const std::uint16_t h = static_cast<std::uint16_t>((4u << 13) | n);
            q[0] = static_cast<std::uint8_t>(h);
            q[1] = static_cast<std::uint8_t>(h >> 8);
        }
        std::vector<std::complex<float>> o;
        as::UdpPacket i2;
        if (n >= 4 && as::parseUdpPacket(q.data(), q.size(), i2, o)) {
            ++accepted;
            CHECK(n == 1028 || n == 516 || n == 1444 || n == 388);
        }
    }
    CHECK(accepted == 4);
    // a header whose length disagrees, and a non-data type, are refused and append nothing
    Bytes q(1028, 0);
    q[0] = 0x05; q[1] = 0x84;
    std::vector<std::complex<float>> o;
    CHECK(!as::parseUdpPacket(q.data(), q.size(), info, o) && o.empty());
    q[0] = 0x04; q[1] = 0x24;  // type 1
    CHECK(!as::parseUdpPacket(q.data(), q.size(), info, o) && o.empty());
}

void testSequence() {
    as::UdpSequence s;
    CHECK(s.note(0) == 0 && s.note(1) == 0 && s.note(2) == 0);
    CHECK(s.note(5) == 2 && s.lost() == 2);          // 3 and 4 missing
    CHECK(s.note(5) == 0 && s.late() == 1);          // a repeat
    CHECK(s.note(3) == 0 && s.late() == 2);          // a late one
    CHECK(s.note(6) == 0);
    // wrap: ... 65534, 65535, then 1 (0 is skipped)
    as::UdpSequence w;
    w.note(65533);
    CHECK(w.note(65534) == 0 && w.note(65535) == 0 && w.note(1) == 0 && w.note(2) == 0);
    CHECK(w.lost() == 0 && w.late() == 0);
    CHECK(w.note(4) == 1 && w.lost() == 1);
    // a stream that begins mid-count (the first packet was lost) is not "lost"
    as::UdpSequence m;
    CHECK(m.note(900) == 0 && m.lost() == 0);
    // a restart at 0 resets
    CHECK(m.note(0) == 0 && m.note(1) == 0 && m.lost() == 0);
    m.reset();
    CHECK(m.note(77) == 0 && m.lost() == 0 && m.late() == 0);
}

void testModels() {
    CHECK(cascade::source::ascpModelFromName("NetSDR") == AscpModel::NetSdr);
    CHECK(cascade::source::ascpModelFromName("SDR-IP") == AscpModel::SdrIp);
    CHECK(cascade::source::ascpModelFromName("CloudIQ") == AscpModel::CloudIq);
    CHECK(cascade::source::ascpModelFromName("CloudSDR") == AscpModel::CloudIq);
    CHECK(cascade::source::ascpModelFromName("AFEDRI SDR-Net") == AscpModel::Afedri);
    CHECK(cascade::source::ascpModelFromName("AFE822x SDR-Net") == AscpModel::Afedri);
    CHECK(cascade::source::ascpModelFromName("") == AscpModel::Unknown);
    CHECK(cascade::source::ascpModelFromName("netsdr") == AscpModel::Unknown);  // exact names only
    CHECK(cascade::source::ascpModelFromName("SDR-14") == AscpModel::Unknown);  // USB-only, not this protocol's TCP
    const auto n = cascade::source::ascpRatesFor(AscpModel::NetSdr, 0);
    CHECK(n.size() == 5 && n[0] == 62500 && n[2] == 250000 && n[4] == 2000000);
    const auto ip = cascade::source::ascpRatesFor(AscpModel::SdrIp, 0);
    CHECK(ip.size() == 5 && ip[3] == 615384);
    const auto cl = cascade::source::ascpRatesFor(AscpModel::CloudIq, 0);
    CHECK(cl.size() == 5 && cl[0] == 48000 && cl[2] == 245760);
    const auto af = cascade::source::ascpRatesFor(AscpModel::Afedri, 80000000);
    CHECK(!af.empty() && af.front() == 62500 && af.back() == 2000000);
    CHECK(cascade::source::ascpRatesFor(AscpModel::Unknown, 80000000).empty());
}

void testOpenAndStream(const char* model, int rate, bool want24, bool wantSmall, bool afedri) {
    const auto abandonedAtStart = NetSampleSource::readersAbandoned();
    RadioSpec spec;
    spec.name = model;
    Radio r(spec);
    CHECK(r.start());
    AscpSource s;
    CHECK(std::string(s.driverKey()) == "rfspace");
    CHECK(s.open(r.args("rate=" + std::to_string(rate) + ",timeout=3000")));
    CHECK(s.isOpen() && s.interfaceName() == model && s.serialNumber() == "0123ABCD");
    CHECK(s.localUdpPort() != 0 && s.localUdpPort() != r.port());
    CHECK(std::string(s.name()).find("RFspace-protocol radio") == 0);
    double lo = 0, hi = 0;
    CHECK(s.frequencyRangeHz(lo, hi) && lo == 100 && hi == 34000000);
    CHECK(!s.setCenterFrequencyHz(40.0e6) && std::string(s.lastError()).find("outside") != std::string::npos);
    CHECK(s.setCenterFrequencyHz(7050000.0) && s.centerFrequencyHz() == 7050000.0 && r.freq.load() == 7050000);
    CHECK(!s.setCenterFrequencyHz(-1.0) && !s.setCenterFrequencyHz(std::nan("")));
    const auto gs = s.gains();
    CHECK(gs.size() == 1 && gs[0].name == "RF gain" && gs[0].minDb == -30 && gs[0].maxDb == 0 && gs[0].stepDb == 10);
    CHECK(s.setGainDb("RF gain", -20.0) && r.gain.load() == -20 && s.gainDb("RF gain") == -20.0);
    CHECK(!s.setGainDb("RF gain", -15.0) && !s.setGainDb("RF gain", 10.0) && !s.setGainDb("LNA", 0.0));
    CHECK(r.gain.load() == -20);
    CHECK(s.setSampleRateHz(rate));
    if (afedri) {
        CHECK(s.setSampleRateHz(123457.0));  // any rate in 8 kS/s .. 2 MS/s
        CHECK(!s.setSampleRateHz(7999.0) && !s.setSampleRateHz(2000001.0));
        CHECK(s.setSampleRateHz(rate));
    } else {
        CHECK(!s.setSampleRateHz(123457.0) && !s.setSampleRateHz(0.0));  // only the listed rates
    }
    CHECK(s.start() && s.running());
    CHECK(r.bits24.load() == (want24 ? 1 : 0) && r.mtuSmall.load() == (wantSmall ? 1 : 0));
    CHECK(!s.setSampleRateHz(rate));  // not while running
    const double fs = s.sampleRateHz();
    CHECK(fs >= rate * 0.99 && fs <= rate * 1.01);
    const auto got = readN(s, 20000);
    CHECK(got.size() >= 20000);
    // the tone, with the radio's clock 40 ppm off, is found where that clock puts it
    const double expect = spec.toneHz / (1.0 + spec.ppm * 1e-6);
    // The phase-step estimator's own noise grows with the rate; the 800 ppm shift (10 Hz) is only
    // resolvable up to 300 kS/s, where the tolerance is 2.5 Hz + 2e-6 * fs.
    CHECK_NEAR(toneHz(got, fs), expect, 2.5 + fs * 2e-5);
    if (fs <= 300000.0) {
        CHECK_NEAR(toneHz(got, fs), expect, 4.0);
        CHECK(std::fabs(toneHz(got, fs) - spec.toneHz) > 4.0);  // the clock error is really in the data
    }
    CHECK_NEAR(meanMag(got), spec.amp, 0.04);
    CHECK(s.packetsLost() == 0 && s.badPackets() == 0);
    CHECK(r.keepalives.load() >= 1);
    s.stop();
    CHECK(!s.running() && r.offCount.load() >= 1 && !r.receiverOn());
    s.closeDevice();
    CHECK(!s.isOpen());
    CHECK(NetSampleSource::readersAbandoned() == abandonedAtStart);
}

void testOpenRefusals() {
    // arguments
    {
        AscpSource s;
        CHECK(!s.open("rfspace=127.0.0.1:0") && !s.open("rfspace=127.0.0.1:70000") && !s.open("rfspace=[::1"));
        CHECK(!s.open("rfspace=127.0.0.1:1,timeout=5") && !s.open("rfspace=127.0.0.1:1,timeout=999999"));
        CHECK(!s.open("rfspace=127.0.0.1:1,udpport=70000") && !s.open("rfspace=127.0.0.1:1,udpport=abc"));
        CHECK(!s.open("rfspace=127.0.0.1:1,rate=5") && !s.open("rfspace=127.0.0.1:1,rate=abc"));
        CHECK(!s.isOpen());
    }
    // nothing listens
    {
        AscpSource s;
        CHECK(!s.open("rfspace=127.0.0.1:" + std::to_string(deadPort()) + ",timeout=300"));
        CHECK(!s.isOpen() && std::string(s.lastError()).find("could not reach the radio") != std::string::npos);
        CHECK(!s.start() && !s.setCenterFrequencyHz(7.0e6) && !s.setGainDb("RF gain", 0.0));
        CHECK(s.gains().empty() && s.supportedSampleRatesHz().empty());
    }
    // a host that does not exist
    {
        AscpSource s;
        CHECK(!s.open("rfspace=no-such-host.invalid,timeout=300") && !s.isOpen());
    }
    // a server that answers with a name this build does not know
    {
        RadioSpec sp;
        sp.name = "Totally Different Radio\x01\x02";
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(!s.open(r.args("timeout=3000")) && !s.isOpen());
        const std::string e = s.lastError();
        CHECK(e.find("Totally Different Radio") != std::string::npos && e.find("does not know") != std::string::npos);
        CHECK(e.find('\x01') == std::string::npos);  // network text is made printable
    }
    // an empty name, and SDR-14 / SDR-IQ (USB only)
    for (const char* nm : {"", "SDR-14", "SDR-IQ"}) {
        RadioSpec sp;
        sp.name = nm;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(!s.open(r.args("timeout=400")) && !s.isOpen());
    }
    // a server that never answers
    {
        RadioSpec sp;
        sp.quirk = Quirk::SilentOnName;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open(r.args("timeout=300")) && !s.isOpen());
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        CHECK(std::string(s.lastError()).find("did not answer") != std::string::npos);
    }
    // a server that answers the name with an impossible message length
    {
        RadioSpec sp;
        sp.quirk = Quirk::JunkControl;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(!s.open(r.args("timeout=3000")) && !s.isOpen());
    }
    // the same source object opens again after a failure
    {
        RadioSpec sp;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(!s.open("rfspace=127.0.0.1:" + std::to_string(deadPort()) + ",timeout=300"));
        CHECK(s.open(r.args("timeout=3000")) && s.isOpen());
        CHECK(s.open(r.args("timeout=3000")) && s.isOpen());  // open() on an open source reopens
        s.closeDevice();
        s.closeDevice();  // idempotent
    }
    // the local UDP port is taken: say so
    {
        RadioSpec sp;
        Radio r(sp);
        CHECK(r.start());
        // a wildcard UDP socket holds the port the client is told to use
        Sock hog = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_ANY);
        CHECK(::bind(hog, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        SockLen len = sizeof(a);
        ::getsockname(hog, reinterpret_cast<sockaddr*>(&a), &len);
        const std::uint16_t taken = ::ntohs(a.sin_port);
        AscpSource s;
        CHECK(!s.open(r.args("timeout=500,udpport=" + std::to_string(taken))) && !s.isOpen());
        CHECK(std::string(s.lastError()).find("UDP port") != std::string::npos);
        closeSock(hog);
    }
}

void testDefaultUdpPort() {
    // Without udpport the client binds its UDP socket to the TCP port number, as CuteSDR does. The TCP
    // port is ephemeral; if that number is busy for UDP on this machine the open must say so, so try
    // fresh radios until one number is free (and then require it to be used).
    bool proven = false;
    for (int attempt = 0; attempt < 40 && !proven; ++attempt) {
        RadioSpec sp;
        Radio r(sp);
        r.bindUdpNextToTcp = false;
        CHECK(r.start());
        AscpSource s;
        if (s.open("rfspace=127.0.0.1:" + std::to_string(r.port()) + ",timeout=3000")) {
            CHECK(s.localUdpPort() == r.port());
            s.closeDevice();
            CHECK(s.localUdpPort() == 0);
            proven = true;
        } else {
            CHECK(std::string(s.lastError()).find("UDP port") != std::string::npos);
        }
    }
    CHECK(proven);
}

void testOptionalItemsMayBeNaked() {
    RadioSpec sp;
    sp.quirk = Quirk::NakOptional;
    Radio r(sp);
    CHECK(r.start());
    AscpSource s;
    CHECK(s.open(r.args("rate=250000,timeout=3000")));
    CHECK(s.start());  // the optional setup items were refused; the rate and receiver-on were not
    CHECK(readN(s, 4000).size() >= 4000);
    s.stop();
    s.closeDevice();
}

void testGainNak() {
    RadioSpec sp;
    sp.quirk = Quirk::NakGain;
    Radio r(sp);
    CHECK(r.start());
    AscpSource s;
    CHECK(s.open(r.args("timeout=3000")));
    CHECK(!s.setGainDb("RF gain", -10.0));
    CHECK(std::string(s.lastError()).find("does not implement") != std::string::npos);
    CHECK(s.gainDb("RF gain") == 0.0);  // nothing was recorded as set
    s.closeDevice();
}

void waitFault(AscpSource& s, int seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
}

void testStreamFaults() {
    {  // the radio never sends a packet: the source says so within its timeout and does not hang
        RadioSpec sp;
        sp.quirk = Quirk::NoUdp;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(s.open(r.args("timeout=1500")));
        CHECK(s.start());
        waitFault(s, 6);
        CHECK(s.faulted() && s.faultedWhile().find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
    {  // the radio drops the control connection after the receiver is on
        RadioSpec sp;
        sp.quirk = Quirk::CloseAfterStart;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(s.open(r.args("timeout=1500")));
        s.start();
        waitFault(s, 6);
        CHECK(s.faulted());
        s.closeDevice();
        CHECK(!s.isOpen());
    }
    // lost and repeated packets are counted; the stream goes on
    for (Quirk q : {Quirk::DropEvery7, Quirk::RepeatEvery9, Quirk::BadUdp}) {
        RadioSpec sp;
        sp.quirk = q;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        CHECK(readN(s, 20000).size() >= 20000);
        if (q == Quirk::DropEvery7) { CHECK(s.packetsLost() >= 5); }
        if (q == Quirk::RepeatEvery9) { CHECK(s.packetsLate() >= 3); }
        if (q == Quirk::BadUdp) { CHECK(s.badPackets() >= 6); }
        CHECK(!s.faulted());
        s.closeDevice();
    }
    {  // random bytes on the control connection while streaming: a fault or consumed, never a crash
        RadioSpec sp;
        sp.quirk = Quirk::TcpGarbageDuring;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(s.open(r.args("timeout=3000")));
        s.start();
        waitFault(s, 3);
        s.closeDevice();
        CHECK(!s.isOpen());
    }
    {  // unsolicited A/D-overload status is counted
        RadioSpec sp;
        sp.quirk = Quirk::OverloadStatus;
        Radio r(sp);
        CHECK(r.start());
        AscpSource s;
        CHECK(s.open(r.args("timeout=3000")));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        while (s.overloadReports() < 3 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.overloadReports() >= 3 && !s.faulted());
        s.closeDevice();
    }
}

void testCycles() {
    RadioSpec sp;
    Radio r(sp);
    CHECK(r.start());
    AscpSource s;
    const auto abandoned = NetSampleSource::readersAbandoned();
    CHECK(s.open(r.args("rate=250000,timeout=3000")));
    for (int i = 0; i < 50; ++i) {
        const bool st = s.start();
        CHECK(st);
        if (!st) { break; }
        if (i % 10 == 0) { CHECK(readN(s, 512).size() >= 512); }
        s.stop();
    }
    CHECK(r.onCount.load() == 50 && r.offCount.load() >= 50);
    s.closeDevice();
    for (int i = 0; i < 50; ++i) {
        AscpSource t;
        const bool op = t.open(r.args("timeout=3000"));
        CHECK(op);
        if (!op) { break; }
        if (i % 5 == 0) { CHECK(t.start()); }
        const auto t0 = std::chrono::steady_clock::now();
        t.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000));
    }
    CHECK(NetSampleSource::readersAbandoned() == abandoned);
}

void testRestartChangesRate() {
    RadioSpec sp;
    Radio r(sp);
    CHECK(r.start());
    AscpSource s;
    CHECK(s.open(r.args("rate=250000,timeout=3000")));
    CHECK(s.start() && r.bits24.load() == 1);
    CHECK(readN(s, 2000).size() >= 2000);
    s.stop();
    CHECK(s.setSampleRateHz(2000000.0));
    CHECK(s.start());
    CHECK(r.bits24.load() == 0 && r.mtuSmall.load() == 0);  // 16-bit above 1.5 MS/s, large packets
    CHECK(readN(s, 8000).size() >= 8000);
    CHECK(s.sampleRateHz() == 2000000.0);
    s.stop();
    CHECK(s.setSampleRateHz(62500.0));
    CHECK(s.start() && r.mtuSmall.load() == 1 && r.bits24.load() == 1);  // small packets at 100 kS/s and below
    CHECK(readN(s, 2000).size() >= 2000);
    s.closeDevice();
}

void testReadWhenIdle() {
    RadioSpec sp;
    Radio r(sp);
    CHECK(r.start());
    AscpSource s;
    std::vector<std::complex<float>> tmp(64);
    CHECK(s.read(tmp.data(), tmp.size()) == 0);  // not open
    CHECK(s.open(r.args("timeout=3000")));
    CHECK(s.read(tmp.data(), tmp.size()) == 0);  // open, not started: nothing kept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(s.read(tmp.data(), tmp.size()) == 0);
    s.closeDevice();
}

void testFuzz() {
    Rng rng(0xC0FFEE);
    // bytes into the assembler: never a crash, never more messages than bytes
    for (int i = 0; i < 5000; ++i) {
        as::StreamAssembler a;
        std::vector<as::Message> out;
        Bytes b(1 + rng.next() % 600);
        for (auto& x : b) { x = static_cast<std::uint8_t>(rng.next()); }
        if (i % 3 == 0) { b[1] &= 0x1F; }  // keep the type small so some control messages form
        std::size_t off = 0;
        while (off < b.size()) {
            const std::size_t n = std::min<std::size_t>(1 + rng.next() % 40, b.size() - off);
            a.feed(b.data() + off, n, out);
            off += n;
        }
        CHECK(out.size() <= b.size());
    }
    // datagrams of every size with random content: a parse never overruns, samples stay in range
    for (int i = 0; i < 5000; ++i) {
        const std::size_t n = (i % 4 == 0) ? (i % 8 == 0 ? 1028 : 1444) : rng.next() % 1600;
        Bytes b(n);
        for (auto& x : b) { x = static_cast<std::uint8_t>(rng.next()); }
        if (n >= 2 && (i % 4 == 0)) {
            const std::uint16_t h = static_cast<std::uint16_t>((4u << 13) | n);
            b[0] = static_cast<std::uint8_t>(h);
            b[1] = static_cast<std::uint8_t>(h >> 8);
        }
        as::UdpPacket info;
        std::vector<std::complex<float>> o;
        const bool ok = as::parseUdpPacket(b.data(), b.size(), info, o);
        CHECK(!ok || (o.size() == info.pairs));
        for (auto& v : o) { CHECK(std::fabs(v.real()) <= 1.0f && std::fabs(v.imag()) <= 1.0f); }
        as::UdpSequence q;
        q.note(static_cast<std::uint16_t>(rng.next()));
        q.note(static_cast<std::uint16_t>(rng.next()));
    }
    // random range bodies
    for (int i = 0; i < 2000; ++i) {
        Bytes b(rng.next() % 120);
        for (auto& x : b) { x = static_cast<std::uint8_t>(rng.next()); }
        std::vector<as::FreqRange> rs;
        const bool ok = as::parseFrequencyRanges(b, rs);
        CHECK(!ok || (!rs.empty() && rs.size() <= 8));
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testDocumentBytes();
    testAssembler();
    testRanges();
    testUdpParsing();
    testSequence();
    testModels();
    testOpenAndStream("NetSDR", 250000, true, false, false);
    testOpenAndStream("NetSDR", 62500, true, true, false);
    testOpenAndStream("SDR-IP", 125000, true, false, false);
    testOpenAndStream("CloudIQ", 245760, true, false, false);
    testOpenAndStream("AFEDRI SDR-Net", 500000, true, false, true);
    testOpenAndStream("AFE822x SDR-Net", 1000000, true, false, true);
    testOpenRefusals();
    testDefaultUdpPort();
    testOptionalItemsMayBeNaked();
    testGainNak();
    testStreamFaults();
    testCycles();
    testRestartChangesRate();
    testReadWhenIdle();
    testFuzz();
    return testSummary("test_ascp_source");
}
