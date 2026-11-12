// test_soapy_remote_source.cpp - the SoapyRemote client, proven against a STAND-IN server on a
// real loopback socket.
//
// WHAT THIS CAN AND CANNOT PROVE. The stand-in below is written from the same description the
// client was (src/source/soapy_remote_protocol.hpp: SoapyRemote's Boost-licensed sources, read
// 2026-11-11), independently of the client's codec: every number here is a literal, never the
// client's named constant, and the server has its own packer and parser. A test that only
// agreed with the client would prove nothing, so the literals are the check. What this suite
// does NOT prove: that a real SoapySDRServer accepts these bytes and sends these replies. That
// is UNPROVEN: no real server was reachable and no outside service may be contacted from this
// lane.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "source/soapy_remote_protocol.hpp"
#include "source/soapy_remote_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::SoapyRemoteSource;
using cascade::source::NetSampleSource;

namespace {

// --- the stand-in's own packer / parser (literals only) ---------------------------------

struct Out {
    Bytes b;
    Out& type(std::uint8_t t) { b.push_back(t); return *this; }
    Out& i32(int v) { type(2); put32(b, static_cast<std::uint32_t>(v)); return *this; }
    Out& i64(long long v) {
        type(3);
        put32(b, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
        put32(b, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)));
        return *this;
    }
    Out& f64(double v) {
        type(4);
        int e = 0;
        const double x = std::frexp(v, &e);
        const long long m = static_cast<long long>(std::ldexp(x, 53));
        i32(e);
        i64(m);
        return *this;
    }
    Out& str(const std::string& s) { type(6); i32(static_cast<int>(s.size())); b.insert(b.end(), s.begin(), s.end()); return *this; }
    Out& boolean(bool v) { type(1); b.push_back(v ? 1 : 0); return *this; }
    Out& strList(const std::vector<std::string>& v) { type(9); i32(static_cast<int>(v.size())); for (auto& s : v) { str(s); } return *this; }
    Out& f64List(const std::vector<double>& v) { type(10); i32(static_cast<int>(v.size())); for (double d : v) { f64(d); } return *this; }
    Out& range(double lo, double hi, double step) { type(7); f64(lo); f64(hi); f64(step); return *this; }
    Out& rangeList(const std::vector<std::array<double, 3>>& v) {
        type(8);
        i32(static_cast<int>(v.size()));
        for (auto& r : v) { range(r[0], r[1], r[2]); }
        return *this;
    }
    Bytes frame() const {
        Bytes m;
        put32(m, 0x53525043u);
        put32(m, 0x400u);
        put32(m, static_cast<std::uint32_t>(12 + b.size() + 4));
        m.insert(m.end(), b.begin(), b.end());
        put32(m, 0x43505253u);
        return m;
    }
    static Bytes voidReply() { Out o; o.type(14); return o.frame(); }
    static Bytes exceptionReply(const std::string& msg) { Out o; o.type(13); o.str(msg); return o.frame(); }
};

struct In {
    const Bytes& p;
    std::size_t o = 0;
    explicit In(const Bytes& x) : p(x) {}
    void need(std::size_t n) const { if (o + n > p.size()) { throw std::runtime_error("stand-in: read past the request"); } }
    int call() { need(1); o += 1; return i32(); }
    int i32raw() { need(4); const int v = static_cast<int>(get32(p.data() + o)); o += 4; return v; }
    int i32() { need(1); o += 1; return i32raw(); }
    char chr() { need(2); o += 1; return static_cast<char>(p[o++]); }
    bool boolean() { need(2); o += 1; return p[o++] != 0; }
    std::string str() {
        need(1);
        o += 1;  // the STRING type byte
        const int n = i32();
        need(static_cast<std::size_t>(n));
        std::string s(reinterpret_cast<const char*>(p.data() + o), static_cast<std::size_t>(n));
        o += static_cast<std::size_t>(n);
        return s;
    }
    double f64() {
        need(1);
        o += 1;  // the FLOAT64 type byte
        const int e = i32();
        need(9);
        o += 1;
        std::uint64_t m = (static_cast<std::uint64_t>(get32(p.data() + o)) << 32) | get32(p.data() + o + 4);
        o += 8;
        return std::ldexp(static_cast<double>(static_cast<long long>(m)), e - 53);
    }
    std::map<std::string, std::string> kwargs() {
        need(1);
        o += 1;
        const int n = i32();
        std::map<std::string, std::string> m;
        for (int i = 0; i < n; ++i) { std::string k = str(); m[k] = str(); }
        return m;
    }
    std::vector<int> sizeList() {
        need(1);
        o += 1;
        const int n = i32();
        std::vector<int> v;
        for (int i = 0; i < n; ++i) { v.push_back(i32()); }
        return v;
    }
};

// --- the stand-in server ----------------------------------------------------------------

enum class Mode {
    Normal, Junk, Silent, CloseNow, BadTrailer, HugeLength, TinyLength, ExceptionOnMake,
    TruncatedReply, CS12Format, OddCallThenNormal
};
enum class StreamFault { None, SplitHeader, DieAfter, LyingBytes, ElemsBeyond, StatusFatal, StatusOverflow, Stall };

class FakeSoapy {
public:
    ~FakeSoapy() { stop(); }
    bool start() {
        return srv_.start([this](Sock s, std::size_t idx, const std::atomic<bool>& run) { serve(s, idx, run); });
    }
    void stop() {
        streamRun_.store(false);
        if (streamThread_.joinable()) { streamThread_.join(); }
        srv_.stop();
        closeSock(dataSock_);
        closeSock(statusSock_);
        dataSock_ = statusSock_ = kBadSock;
    }
    std::uint16_t port() const { return srv_.port(); }
    std::string args(const std::string& extra = "") const {
        return "soapyremote=127.0.0.1:" + std::to_string(srv_.port()) + (extra.empty() ? "" : "," + extra);
    }

    std::atomic<Mode> mode{Mode::Normal};
    std::atomic<StreamFault> fault{StreamFault::None};
    std::atomic<int> faultAfter{20};
    std::string nativeFormat = "CS16";
    double fullScale = 32768.0;

    std::vector<int> calls() { std::lock_guard<std::mutex> l(m_); return calls_; }
    int countCall(int c) { int n = 0; for (int x : calls()) { n += (x == c); } return n; }
    std::map<std::string, std::string> makeArgs() { std::lock_guard<std::mutex> l(m_); return makeArgs_; }
    std::map<std::string, std::string> setupArgs() { std::lock_guard<std::mutex> l(m_); return setupArgs_; }
    double lastFreq() { std::lock_guard<std::mutex> l(m_); return lastFreq_; }
    double lastRate() { std::lock_guard<std::mutex> l(m_); return lastRate_; }
    double lastGain() { std::lock_guard<std::mutex> l(m_); return lastGain_; }
    std::string lastGainName() { std::lock_guard<std::mutex> l(m_); return lastGainName_; }
    std::string lastDirection() { std::lock_guard<std::mutex> l(m_); return lastDir_; }
    int acks() const { return acks_.load(); }
    int firstAckBytes() const { return firstAckBytes_.load(); }
    int firstAckElems() const { return firstAckElems_.load(); }
    int datagramsSent() const { return sent_.load(); }

private:
    void rec(int c) { std::lock_guard<std::mutex> l(m_); calls_.push_back(c); }

    bool readMsg(Sock s, const std::atomic<bool>& run, Bytes& payload) {
        std::uint8_t h[12];
        if (!recvExact(s, run, h, 12)) { return false; }
        if (get32(h) != 0x53525043u) { return false; }
        const std::uint32_t len = get32(h + 8);
        if (len < 17 || len > (1u << 20)) { return false; }
        Bytes rest(len - 12);
        if (!recvExact(s, run, rest.data(), rest.size())) { return false; }
        if (get32(rest.data() + rest.size() - 4) != 0x43505253u) { return false; }
        rest.resize(rest.size() - 4);
        payload = std::move(rest);
        return true;
    }
    bool send(Sock s, const Bytes& b) { return sendExact(s, b.data(), b.size()); }

    void serve(Sock s, std::size_t idx, const std::atomic<bool>& run) {
        try { serveImpl(s, idx, run); } catch (const std::exception& e) { std::fprintf(stderr, "stand-in server: %s (after call %d)", e.what(), calls_.empty() ? -1 : calls_.back()); std::fputc(10, stderr); }
    }

    void serveImpl(Sock s, std::size_t idx, const std::atomic<bool>& run) {
        const Mode m = mode.load();
        if (m == Mode::CloseNow) { return; }
        if (m == Mode::Silent) { while (run.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); } return; }
        if (m == Mode::Junk) {
            const std::string junk = "HTTP/1.1 400 Bad Request\r\n\r\n";
            sendExact(s, junk.data(), junk.size());
            // wait for the client's request then hang up
            Bytes p;
            readMsg(s, run, p);
            return;
        }
        (void)idx;
        for (;;) {
            Bytes p;
            if (!readMsg(s, run, p)) { return; }
            In in(p);
            const int call = in.call();
            rec(call);
            Out o;
            switch (call) {
                case 1: {  // MAKE
                    auto k = in.kwargs();
                    { std::lock_guard<std::mutex> l(m_); makeArgs_ = k; }
                    if (mode.load() == Mode::ExceptionOnMake) { send(s, Out::exceptionReply("no such device")); continue; }
                    if (mode.load() == Mode::BadTrailer) {
                        Bytes f = Out::voidReply();
                        f.back() ^= 0xFF;
                        send(s, f);
                        continue;  // and carry on answering: only the trailer check can refuse this
                    }
                    if (mode.load() == Mode::HugeLength) {
                        Bytes f = Out::voidReply();
                        f[8] = 0xFF; f[9] = 0xFF; f[10] = 0xFF; f[11] = 0xFF;
                        send(s, f);
                        return;
                    }
                    if (mode.load() == Mode::TinyLength) {
                        Bytes f = Out::voidReply();
                        f[8] = 0; f[9] = 0; f[10] = 0; f[11] = 16;
                        send(s, f);
                        return;
                    }
                    if (mode.load() == Mode::TruncatedReply) {
                        Bytes f = Out::voidReply();
                        f.resize(f.size() - 3);
                        send(s, f);
                        return;
                    }
                    send(s, Out::voidReply());
                    break;
                }
                case 305: {  // native stream format
                    in.chr(); in.i32();
                    const std::string fmt = mode.load() == Mode::CS12Format ? "CS12" : nativeFormat;
                    o.str(fmt).f64(fullScale);
                    send(s, o.frame());
                    break;
                }
                case 902: o.f64List({250000.0, 1024000.0, 2048000.0}); send(s, o.frame()); break;
                case 805: o.rangeList({{24.0e6, 1766.0e6, 0.0}}); send(s, o.frame()); break;
                case 500: o.strList({"RX", "Aux"}); send(s, o.frame()); break;
                case 700: o.strList({"LNA"}); send(s, o.frame()); break;
                case 708: o.range(0.0, 40.0, 1.0); send(s, o.frame()); break;
                case 709: o.boolean(true); send(s, o.frame()); break;
                case 503: o.boolean(true); send(s, o.frame()); break;
                case 800: {
                    { std::lock_guard<std::mutex> l(m_); lastDir_ = std::to_string(static_cast<int>(in.chr())); }
                    in.i32();
                    const double f = in.f64();
                    { std::lock_guard<std::mutex> l(m_); lastFreq_ = f; }
                    send(s, Out::voidReply());
                    break;
                }
                case 900: { in.chr(); in.i32(); const double r = in.f64(); { std::lock_guard<std::mutex> l(m_); lastRate_ = r; } send(s, Out::voidReply()); break; }
                case 704: {
                    in.chr(); in.i32();
                    const std::string n = in.str();
                    const double g = in.f64();
                    { std::lock_guard<std::mutex> l(m_); lastGain_ = g; lastGainName_ = n; }
                    send(s, Out::voidReply());
                    break;
                }
                case 701: case 501: case 504: send(s, Out::voidReply()); break;
                case 300: {  // SETUP_STREAM
                    in.chr(); in.str(); in.sizeList();
                    auto k = in.kwargs();
                    const std::string clientPort = in.str();
                    const std::string statusPort = in.str();
                    { std::lock_guard<std::mutex> l(m_); setupArgs_ = k; }
                    if (!setupStream(s, run, k, clientPort)) { return; }
                    break;
                }
                case 302: {  // ACTIVATE
                    in.i32();
                    startStream();
                    o.i32(0);
                    send(s, o.frame());
                    break;
                }
                case 303: stopStream(); o.i32(0); send(s, o.frame()); break;
                case 301: stopStream(); send(s, Out::voidReply()); break;
                case 2: send(s, Out::voidReply()); break;
                case 3: send(s, Out::voidReply()); return;
                default: send(s, Out::exceptionReply("unknown call")); break;
            }
        }
    }

    bool setupStream(Sock ctl, const std::atomic<bool>& run, const std::map<std::string, std::string>& k,
                     const std::string& clientPort) {
        const bool udp = k.at("remote:prot") == "udp";
        stopStream();
        udpClientPort_ = 0;
        closeSock(dataSock_);
        closeSock(statusSock_);
        dataSock_ = statusSock_ = kBadSock;
        if (udp) {
            udpSock_ = std::make_unique<UdpSock>();
            if (!udpSock_->open()) { return false; }
            udpClientPort_ = static_cast<std::uint16_t>(std::stoi(clientPort));
            Out o;
            o.i32(7).str(std::to_string(udpSock_->port()));
            send(ctl, o.frame());
            return true;
        }
        udpSock_.reset();
        // TCP: a listener of its own; first reply is its port, then wait for two connections.
        Sock l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        SockLen len = sizeof(a);
        ::getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
        ::listen(l, 4);
        Out first;
        first.str(std::to_string(ntohs(a.sin_port)));
        send(ctl, first.frame());
        Sock d = kBadSock, st = kBadSock;
        if (waitRead(l, run, 50) || waitRead(l, run, 1000)) { d = ::accept(l, nullptr, nullptr); }
        if (waitRead(l, run, 1000)) { st = ::accept(l, nullptr, nullptr); }
        closeSock(l);
        if (d == kBadSock || st == kBadSock) { closeSock(d); closeSock(st); return false; }
        dataSock_ = d;
        statusSock_ = st;
        Out second;
        second.i32(7).str(std::to_string(ntohs(a.sin_port)));
        send(ctl, second.frame());
        return true;
    }

    void startStream() {
        stopStream();
        streamRun_.store(true);
        streamThread_ = std::thread([this] { streamLoop(); });
    }
    void stopStream() {
        streamRun_.store(false);
        if (streamThread_.joinable()) { streamThread_.join(); }
    }

    void streamLoop() {
        const bool udp = udpSock_ != nullptr && dataSock_ == kBadSock;
        std::uint32_t seq = 0;
        std::int64_t n = 0;
        int sent = 0;
        std::thread acker;
        std::atomic<bool> ackRun{true};
        if (udp) {
            acker = std::thread([this, &ackRun] {
                Bytes d;
                std::uint16_t from = 0;
                while (ackRun.load()) {
                    if (!udpSock_->recvFrom(ackRun, d, from)) { continue; }
                    if (d.size() == 24) {
                        if (acks_.fetch_add(1) == 0) {
                            firstAckBytes_.store(static_cast<int>(get32(d.data())));
                            firstAckElems_.store(static_cast<int>(get32(d.data() + 8)));
                        }
                    }
                }
            });
        } else {
            acker = std::thread([this, &ackRun] {
                Bytes d(24);
                while (ackRun.load()) {
                    if (!waitRead(dataSock_, ackRun, 20)) { continue; }
                    std::uint8_t h[24];
                    if (!recvExact(dataSock_, ackRun, h, 24)) { return; }
                    if (acks_.fetch_add(1) == 0) {
                        firstAckBytes_.store(static_cast<int>(get32(h)));
                        firstAckElems_.store(static_cast<int>(get32(h + 8)));
                    }
                }
            });
        }
        while (streamRun_.load()) {
            const StreamFault f = fault.load();
            if (f == StreamFault::Stall && sent >= faultAfter.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const int elems = 200;
            Bytes d;
            std::uint32_t bytes = 24 + elems * 4;
            std::int32_t elemsField = elems;
            if (f == StreamFault::LyingBytes && sent == faultAfter.load()) { bytes = 24 + elems * 4 + 5000; }
            if (f == StreamFault::ElemsBeyond && sent == faultAfter.load()) { elemsField = elems + 50; }
            if (f == StreamFault::StatusFatal && sent == faultAfter.load()) { elemsField = -2; bytes = 24; }
            if (f == StreamFault::StatusOverflow && sent % 5 == 4) { elemsField = -4; bytes = 24; }
            put32(d, bytes);
            put32(d, seq++);
            put32(d, static_cast<std::uint32_t>(elemsField));
            put32(d, 0);
            put32(d, 0);
            put32(d, 0);
            if (bytes > 24) {
                const int actual = std::min(elems, static_cast<int>((bytes - 24) / 4));
                for (int i = 0; i < elems; ++i, ++n) {
                    const std::int16_t v = static_cast<std::int16_t>(n % 20000);
                    const std::int16_t q = static_cast<std::int16_t>(-v);
                    d.push_back(static_cast<std::uint8_t>(v & 0xFF));
                    d.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                    d.push_back(static_cast<std::uint8_t>(q & 0xFF));
                    d.push_back(static_cast<std::uint8_t>((q >> 8) & 0xFF));
                }
                (void)actual;
            }
            // lying length: the header claims more than is sent (UDP: more than the datagram)
            if (udp) {
                udpSock_->sendTo(udpClientPort_, d.data(), d.size());
            } else if (f == StreamFault::SplitHeader && sent == 3) {
                // a header cut in two with a pause longer than the client's receive slice
                if (!sendExact(dataSock_, d.data(), 10)) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(450));
                if (!sendExact(dataSock_, d.data() + 10, d.size() - 10)) { break; }
            } else if (!sendExact(dataSock_, d.data(), d.size())) {
                break;
            }
            ++sent;
            sent_.store(sent);
            if (f == StreamFault::DieAfter && sent >= faultAfter.load()) {
                if (!udp) { closeSock(dataSock_); dataSock_ = kBadSock; }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ackRun.store(false);
        acker.join();
    }

    TcpServer srv_;
    std::mutex m_;
    std::vector<int> calls_;
    std::map<std::string, std::string> makeArgs_, setupArgs_;
    double lastFreq_ = 0, lastRate_ = 0, lastGain_ = -1;
    std::string lastGainName_, lastDir_;
    std::atomic<int> acks_{0}, firstAckBytes_{-1}, firstAckElems_{-1}, sent_{0};
    std::unique_ptr<UdpSock> udpSock_;
    std::uint16_t udpClientPort_ = 0;
    Sock dataSock_ = kBadSock, statusSock_ = kBadSock;
    std::atomic<bool> streamRun_{false};
    std::thread streamThread_;
};

// Reads until n samples have arrived or the time is up.
std::vector<std::complex<float>> readN(SoapyRemoteSource& s, std::size_t n, int ms = 4000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(512);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

bool rampOk(const std::vector<std::complex<float>>& v, std::size_t n) {
    // The stand-in sends I = k % 20000 (CS16), Q = -I, scaled by 1/32768. Delivery starts at
    // an arbitrary point of the ramp (samples before start() are discarded), so check that
    // every sample is on the ramp and that consecutive samples step by one.
    if (v.size() < n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        const float ev = v[i].real() * 32768.0f;
        const float eq = v[i].imag() * 32768.0f;
        if (std::fabs(ev + eq) > 0.01f) { return false; }
        if (i > 0) {
            const float prev = v[i - 1].real() * 32768.0f;
            const float step = ev - prev;
            if (!(std::fabs(step - 1.0f) < 0.01f || std::fabs(step + 19999.0f) < 0.01f)) { return false; }
        }
    }
    return true;
}

// --- tests -----------------------------------------------------------------------------

void testHappyPath(const char* prot) {
    FakeSoapy fake;
    CHECK(fake.start());
    SoapyRemoteSource src;
    const std::string extra = std::string("prot=") + prot + ",device=driver:rtlsdr;serial:ABC";
    CHECK(src.open(fake.args(extra)));
    CHECK(src.isOpen());
    CHECK(std::string(src.driverKey()) == "soapyremote");
    CHECK(src.remoteFormat() == "CS16");
    CHECK_NEAR(src.remoteFullScale(), 32768.0, 0.001);
    CHECK(src.streamProtocol() == prot);
    // MAKE carried the no-recursion key and the device args, nothing else.
    const auto make = fake.makeArgs();
    CHECK(make.count("soapy_remote_no_deeper") == 1);
    CHECK(make.count("driver") == 1 && make.at("driver") == "rtlsdr");
    CHECK(make.count("serial") == 1 && make.at("serial") == "ABC");
    // The stream setup carried our choices.
    const auto sa = fake.setupArgs();
    CHECK(sa.at("remote:prot") == std::string(prot));
    CHECK(sa.at("remote:mtu") == "1500");
    // What the server said about itself.
    const std::vector<double> rates = src.supportedSampleRatesHz();
    CHECK(rates.size() == 3 && rates[0] == 250000.0 && rates[2] == 2048000.0);
    double lo = 0, hi = 0;
    CHECK(src.frequencyRangeHz(lo, hi) && lo == 24.0e6 && hi == 1766.0e6);
    CHECK(src.antennas().size() == 2 && src.antenna() == "RX");
    CHECK(src.gains().size() == 1 && src.gains()[0].name == "LNA" && src.gains()[0].maxDb == 40.0);
    CHECK(src.autoGainSupported() && src.hasFrequencyCorrection());
    // Controls reach the server in the units given.
    CHECK(src.setCenterFrequencyHz(100.5e6));
    CHECK_NEAR(fake.lastFreq(), 100.5e6, 0.001);
    CHECK(fake.lastDirection() == "1");  // RX
    CHECK(src.setSampleRateHz(1000000.0));  // snapped to the nearest offered rate
    CHECK_NEAR(fake.lastRate(), 1024000.0, 0.001);
    CHECK_NEAR(src.sampleRateHz(), 1024000.0, 0.001);
    CHECK(src.setGainDb("LNA", 55.0));  // clamped to the range
    CHECK_NEAR(fake.lastGain(), 40.0, 0.001);
    CHECK(fake.lastGainName() == "LNA");
    CHECK(!src.setGainDb("Nope", 1.0));
    CHECK(src.setAutoGain(true) && src.autoGain());
    CHECK(src.setAntenna("Aux") && src.antenna() == "Aux");
    CHECK(!src.setAntenna("Nope"));
    CHECK(src.setFrequencyCorrectionPpm(1.5));
    CHECK(!src.setSampleRateHz(-1.0));
    CHECK(!src.setCenterFrequencyHz(std::nan("")));
    // Stopped: nothing is delivered, though the stream (not yet activated) is set up.
    std::vector<std::complex<float>> tmp(16);
    CHECK(src.read(tmp.data(), tmp.size()) == 0);
    CHECK(src.start());
    CHECK(src.running());
    const auto got = readN(src, 3000);
    CHECK(got.size() >= 3000);
    CHECK(rampOk(got, 3000));
    CHECK(fake.acks() >= 1);
    CHECK(fake.firstAckBytes() == 24);
    CHECK(fake.firstAckElems() == 699);  // window 1 MiB / mtu 1500
    src.stop();
    CHECK(!src.running());
    CHECK(fake.countCall(303) == 1);
    src.closeDevice();
    CHECK(!src.isOpen());
    CHECK(fake.countCall(301) == 1 && fake.countCall(2) == 1 && fake.countCall(3) == 1);
}

void testOpenRefusals() {
    {  // connection refused
        SoapyRemoteSource s;
        CHECK(!s.open("soapyremote=127.0.0.1:" + std::to_string(deadPort())));
        CHECK(std::string(s.lastError()).size() > 0);
        CHECK(!s.isOpen());
    }
    {  // bad addresses
        SoapyRemoteSource s;
        CHECK(!s.open("soapyremote=127.0.0.1:0"));
        CHECK(!s.open("soapyremote=127.0.0.1:99999"));
        CHECK(!s.open("soapyremote=[::1"));
        CHECK(!s.open("soapyremote=127.0.0.1:1,prot=sctp"));
        CHECK(!s.open("soapyremote=127.0.0.1:1,timeout=5"));
    }
    struct Case { Mode m; const char* what; };
    const Case cases[] = {
        {Mode::Junk, "wrong greeting"},          {Mode::CloseNow, "closed at once"},
        {Mode::BadTrailer, "bad trailer"},       {Mode::HugeLength, "oversized length field"},
        {Mode::TinyLength, "impossible length"}, {Mode::TruncatedReply, "truncated frame"},
        {Mode::ExceptionOnMake, "exception reply"}, {Mode::CS12Format, "unsupported format"},
    };
    for (const Case& c : cases) {
        FakeSoapy fake;
        CHECK(fake.start());
        fake.mode.store(c.m);
        SoapyRemoteSource s;
        const bool ok = s.open(fake.args("timeout=800"));
        if (ok) { std::printf("UNEXPECTED open success: %s\n", c.what); }
        CHECK(!ok);
        CHECK(!s.isOpen());
        CHECK(std::string(s.lastError()).size() > 0);
        s.closeDevice();
    }
    {  // silent server: bounded by the timeout
        FakeSoapy fake;
        CHECK(fake.start());
        fake.mode.store(Mode::Silent);
        SoapyRemoteSource s;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open(fake.args("timeout=300")));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 3000);
        CHECK(std::string(s.lastError()).find("timed out") != std::string::npos);
    }
    {  // the error text says what the server said
        FakeSoapy fake;
        CHECK(fake.start());
        fake.mode.store(Mode::ExceptionOnMake);
        SoapyRemoteSource s;
        CHECK(!s.open(fake.args()));
        CHECK(std::string(s.lastError()).find("no such device") != std::string::npos);
    }
}

void testStreamFaults() {
    struct Case { StreamFault f; bool fatal; const char* what; };
    const Case cases[] = {
        {StreamFault::DieAfter, true, "server closes mid-stream"},
        {StreamFault::LyingBytes, true, "length beyond the data"},
        {StreamFault::ElemsBeyond, true, "element count beyond the length"},
        {StreamFault::StatusFatal, true, "fatal stream status"},
        {StreamFault::StatusOverflow, false, "overflow status is not fatal"},
    };
    for (const char* prot : {"tcp", "udp"}) {
        for (const Case& c : cases) {
            if (std::string(prot) == "udp" && c.f == StreamFault::DieAfter) { continue; }  // a datagram stream has no close
            if (std::string(prot) == "tcp" && c.f == StreamFault::LyingBytes) { continue; }  // over TCP the length IS the framing
            FakeSoapy fake;
            CHECK(fake.start());
            fake.fault.store(c.f);
            fake.faultAfter.store(20);
            SoapyRemoteSource s;
            CHECK(s.open(fake.args(std::string("prot=") + prot)));
            CHECK(s.start());
            std::vector<std::complex<float>> tmp(512);
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            while (std::chrono::steady_clock::now() < end && !s.faulted()) {
                s.read(tmp.data(), tmp.size());
                if (!c.fatal && fake.datagramsSent() > 60) { break; }
            }
            if (c.fatal != s.faulted()) { std::printf("stream fault case wrong: %s over %s\n", c.what, prot); }
            CHECK(c.fatal == s.faulted());
            if (c.fatal) {
                CHECK(s.deviceDead());
                CHECK(std::string(s.lastError()).size() > 0);
                CHECK(!s.faultedWhile().empty());
            }
            s.closeDevice();
        }
    }
    {  // a stalled stream (server goes silent while activated) is a fault, bounded
        FakeSoapy fake;
        CHECK(fake.start());
        fake.fault.store(StreamFault::Stall);
        fake.faultAfter.store(10);
        SoapyRemoteSource s;
        CHECK(s.open(fake.args("timeout=300")));
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(512);
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) { s.read(tmp.data(), tmp.size()); }
        CHECK(s.faulted());
        CHECK(std::string(s.lastError()).find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testSplitHeader() {
    // TCP reassembly: a header that arrives in two pieces 450 ms apart (the client waits in
    // 200 ms slices) must lose nothing and break nothing: the ramp stays continuous.
    FakeSoapy fake;
    CHECK(fake.start());
    fake.fault.store(StreamFault::SplitHeader);
    SoapyRemoteSource s;
    CHECK(s.open(fake.args("prot=tcp")));
    CHECK(s.start());
    const auto got = readN(s, 2000);
    CHECK(got.size() >= 2000);
    CHECK(rampOk(got, 2000));
    CHECK(!s.faulted());
    s.closeDevice();
}

void testStartStopAndShutdown() {
    FakeSoapy fake;
    CHECK(fake.start());
    const unsigned long long before = NetSampleSource::readersAbandoned();
    SoapyRemoteSource s;
    CHECK(s.open(fake.args()));
    for (int i = 0; i < 50; ++i) {
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(64);
        s.read(tmp.data(), tmp.size());
        s.stop();
    }
    CHECK(fake.countCall(302) == 50 && fake.countCall(303) == 50);
    // After a stop nothing queued before it is delivered after the next start.
    CHECK(s.start());
    const auto got = readN(s, 500);
    CHECK(got.size() >= 500 && rampOk(got, 500));
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto closeMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(closeMs < 2500);
    // 50 whole open/close cycles, no abandoned reader, none hanging.
    for (int i = 0; i < 50; ++i) {
        SoapyRemoteSource t;
        CHECK(t.open(fake.args(i % 2 ? "prot=udp" : "prot=tcp")));
        if (i % 5 == 0) { CHECK(t.start()); }
        t.closeDevice();
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    // Destruction without closeDevice(), running.
    {
        SoapyRemoteSource u;
        CHECK(u.open(fake.args()));
        CHECK(u.start());
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    // calls on a closed source refuse, they do not crash
    SoapyRemoteSource c;
    CHECK(!c.start());
    CHECK(!c.setCenterFrequencyHz(1e6));
    CHECK(!c.setSampleRateHz(1e6));
    c.closeDevice();
    c.stop();
}

void testShutdownWhileServerSilent() {
    // The server stops sending while the source is streaming; close must still return inside
    // the reader-join bound.
    FakeSoapy fake;
    CHECK(fake.start());
    fake.fault.store(StreamFault::Stall);
    fake.faultAfter.store(5);
    SoapyRemoteSource s;
    CHECK(s.open(fake.args("timeout=5000")));  // a long call bound must not slow the close
    CHECK(s.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const unsigned long long abandonedBefore = NetSampleSource::readersAbandoned();
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "close with a silent streaming server: %lld ms, abandoned %llu\n", static_cast<long long>(ms),
                NetSampleSource::readersAbandoned() - abandonedBefore);
    CHECK(NetSampleSource::readersAbandoned() == abandonedBefore);
    CHECK(ms < 1500);
}

void testAddressParsing() {
    std::string h, e;
    std::uint16_t p = 0;
    CHECK(cascade::source::parseSoapyRemoteAddress("pi.local", h, p, e) && h == "pi.local" && p == 55132);
    CHECK(cascade::source::parseSoapyRemoteAddress("pi.local:1234", h, p, e) && p == 1234);
    CHECK(cascade::source::parseSoapyRemoteAddress("[fe80::1]:77", h, p, e) && h == "fe80::1" && p == 77);
    CHECK(cascade::source::parseSoapyRemoteAddress("fe80::1", h, p, e) && h == "fe80::1" && p == 55132);
    CHECK(!cascade::source::parseSoapyRemoteAddress("", h, p, e));
    CHECK(!cascade::source::parseSoapyRemoteAddress("a:b", h, p, e));
    CHECK(!cascade::source::parseSoapyRemoteAddress("a:70000", h, p, e));
}

// --- the codec, byte for byte ------------------------------------------------------------

void testCodecBytes() {
    namespace sr = cascade::source::soapyremote;
    // CHAR 1 after a CALL 305: 0F 00000131 | 00 01 ; framed with SRPC / version / length / CPRS.
    sr::Packer p;
    p.call(305).chr(1).i32(0);
    const std::vector<std::uint8_t> m = p.finish();
    const Bytes expect = {
        0x53, 0x52, 0x50, 0x43,  0x00, 0x00, 0x04, 0x00,  0x00, 0x00, 0x00, 0x1D,  // header, length 29
        0x0F, 0x02, 0x00, 0x00, 0x01, 0x31,                                          // CALL 305 (typed INT32)
        0x00, 0x01,                                                                  // CHAR 1
        0x02, 0x00, 0x00, 0x00, 0x00,                                                // INT32 0
        0x43, 0x50, 0x52, 0x53};
    CHECK(m == expect);
    // 100.0 as the frexp pair: 0.78125 * 2^7: exponent 7, mantissa 0.78125 * 2^53.
    sr::Packer q;
    q.f64(100.0);
    const auto f = q.finish();
    const Bytes want = {0x04,  0x02, 0x00, 0x00, 0x00, 0x07,  0x03, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(Bytes(f.begin() + 12, f.end() - 4) == want);
    sr::Unpacker u(Bytes(f.begin() + 12, f.end() - 4), 0x400);
    CHECK(u.f64() == 100.0 && u.done());
    for (const double v : {0.0, -1.5, 1.0e9, 24.0e6, 2048000.0, 1e-9, -3.14159}) {
        sr::Packer r;
        r.f64(v);
        const auto g = r.finish();
        sr::Unpacker w(Bytes(g.begin() + 12, g.end() - 4), 0x400);
        CHECK(w.f64() == v);
    }
    // datagram header
    sr::DatagramHeader h;
    h.bytes = 824; h.sequence = 9; h.elems = 200; h.flags = 3; h.time = 0x0102030405060708LL;
    std::uint8_t raw[24];
    sr::encodeDatagramHeader(h, raw);
    const std::uint8_t exp[24] = {0, 0, 3, 0x38, 0, 0, 0, 9, 0, 0, 0, 200, 0, 0, 0, 3, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(std::memcmp(raw, exp, 24) == 0);
    const sr::DatagramHeader back = sr::decodeDatagramHeader(raw);
    CHECK(back.bytes == 824 && back.elems == 200 && back.time == 0x0102030405060708LL);
    CHECK(sr::checkDatagram(back, 824, 4) == sr::DatagramVerdict::Ok);
    CHECK(sr::checkDatagram(back, 800, 4) == sr::DatagramVerdict::LengthLiesBeyondData);
    CHECK(sr::checkDatagram(back, 10, 4) == sr::DatagramVerdict::TooShort);
    CHECK(sr::checkDatagram(back, 824, 8) == sr::DatagramVerdict::ElemsBeyondLength);
    CHECK(sr::checkDatagram(back, 824, 0) == sr::DatagramVerdict::BadElemSize);
}

// --- 10,000 pseudo-random inputs into every parser, fixed seed ---------------------------

void testFuzz() {
    namespace sr = cascade::source::soapyremote;
    Rng rng(0xC0FFEEu);
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 10000; ++i) {
        Bytes in;
        const int len = static_cast<int>(rng.next() % 200);
        const int style = static_cast<int>(rng.next() % 4);
        if (style > 0) {  // a plausible header, random body
            put32(in, 0x53525043u);
            put32(in, 0x400u);
            put32(in, style == 1 ? static_cast<std::uint32_t>(12 + len + 4) : rng.next());
        }
        for (int k = 0; k < len; ++k) { in.push_back(static_cast<std::uint8_t>(rng.next())); }
        if (style == 1) { put32(in, rng.next() % 2 ? 0x43505253u : rng.next()); }
        ScriptTransport t(in);
        Bytes payload;
        std::uint32_t version = 0;
        std::string err;
        if (sr::recvMessage(t, payload, version, err)) {
            ++accepted;
            // every typed reader on whatever came out: throws or returns, never overruns
            const auto tryAll = [&](auto fn) {
                try { sr::Unpacker u(payload, version); fn(u); } catch (const std::exception&) {}
            };
            tryAll([](sr::Unpacker& u) { u.str(); });
            tryAll([](sr::Unpacker& u) { u.stringList(); });
            tryAll([](sr::Unpacker& u) { u.f64List(); });
            tryAll([](sr::Unpacker& u) { u.rangeList(); });
            tryAll([](sr::Unpacker& u) { u.kwargs(); });
            tryAll([](sr::Unpacker& u) { u.f64(); u.i32(); u.boolean(); });
        } else {
            ++rejected;
            CHECK(!err.empty());
        }
        // datagram headers from random bytes
        std::uint8_t h[24];
        for (auto& b : h) { b = static_cast<std::uint8_t>(rng.next()); }
        const sr::DatagramHeader d = sr::decodeDatagramHeader(h);
        sr::checkDatagram(d, rng.next() % 2000, rng.next() % 9);
    }
    CHECK(rejected > 5000);
    CHECK(accepted > 10);
    // payloads of valid messages with random, lying counts
    for (int i = 0; i < 1000; ++i) {
        Bytes pl = {9};  // STRING_LIST
        pl.push_back(2);
        put32(pl, rng.next());
        for (int k = 0; k < 20; ++k) { pl.push_back(static_cast<std::uint8_t>(rng.next())); }
        try { sr::Unpacker u(pl, 0x400); u.stringList(); } catch (const std::exception&) {}
    }
}

// --- the same refusals without sockets: scripted control transports ---------------------

void testScripted() {
    namespace sr = cascade::source::soapyremote;
    {  // a server that answers MAKE correctly then truncates the next reply
        Bytes in;
        const Bytes v = Out::voidReply();
        in.insert(in.end(), v.begin(), v.end());
        Bytes trunc = Out().type(6).frame();
        trunc.resize(trunc.size() - 2);
        in.insert(in.end(), trunc.begin(), trunc.end());
        auto t = std::make_shared<ScriptTransport>(in);
        SoapyRemoteSource s;
        CHECK(!s.openWithTransportForTest("soapyremote=127.0.0.1:1", t));
        CHECK(!s.isOpen());
    }
    {  // oversize length is refused before anything is read for it
        Bytes in;
        put32(in, 0x53525043u); put32(in, 0x400u); put32(in, 0x7FFFFFFFu);
        ScriptTransport t(in);
        Bytes pl; std::uint32_t v = 0; std::string e;
        CHECK(!sr::recvMessage(t, pl, v, e));
        CHECK(e.find("longer than the limit") != std::string::npos);
    }
}

}  // namespace

#define STEP(call) do { std::fputs(#call, stderr); std::fputc(10, stderr); std::fflush(stderr); call; } while (0)

int main() {
    STEP(testCodecBytes());
    STEP(testAddressParsing());
    STEP(testHappyPath("tcp"));
    STEP(testHappyPath("udp"));
    STEP(testOpenRefusals());
    STEP(testStreamFaults());
    STEP(testSplitHeader());
    STEP(testStartStopAndShutdown());
    STEP(testShutdownWhileServerSilent());
    STEP(testScripted());
    STEP(testFuzz());
    return testSummary("test_soapy_remote_source");
}
