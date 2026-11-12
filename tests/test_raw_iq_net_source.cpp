// test_raw_iq_net_source.cpp - RAW I/Q OVER TCP OR UDP AS A SOURCE (engine serial lane A, row R5): source/raw_iq_net_source.hpp.
//
//   A  the address grammar: every good form parses to what it says, every bad one is refused with a sentence (and nothing is guessed);
//   B  the FXIQ header parser and the three sample decoders, against values worked out by hand and against 20,000 random buffers (never past the end, never a false header);
//   C  a headerless stream through a scripted transport: u8, s16 and f32, cut at awkward places (a TCP read that ends in the middle of a sample), the numbers read back exactly;
//   D  a stream with the FXIQ header: the rate and centre come from it before the first sample, a stream that is not FXIQ is refused, a UDP gap is counted, a stray datagram is dropped and counted;
//   E  a stream that goes silent is declared lost with a sentence, a closed one too; closing joins the reader;
//   F  real sockets on 127.0.0.1: a UDP sender (FoxSDR's own transport) to a listening source, and a TCP server stand-in to a connecting one;
//   G  random bytes cut at random places through every format never lose or invent a sample.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "source/raw_iq_net_source.hpp"
#include "test_check.hpp"

using cascade::source::RawIqNetSource;
using cascade::source::iiod::Transport;
namespace rq = cascade::source::rawiq;

namespace {

// A scripted byte stream: each queued chunk is what one receive returns (a longer chunk is returned in pieces if the caller asks for less); an empty queue is a timeout, or a close.
class Script final : public Transport {
public:
    void push(std::vector<unsigned char> c) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(std::move(c));
    }
    void closeAfterQueue() { closing_ = true; }
    bool sendAll(const void*, std::size_t) override { return false; }
    bool recvAll(void*, std::size_t) override { return false; }
    bool recvSome(void* data, std::size_t max, std::size_t& got) override {
        got = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!q_.empty() && !shut_) {
                std::vector<unsigned char>& f = q_.front();
                const std::size_t n = std::min(max, f.size());
                std::memcpy(data, f.data(), n);
                if (n == f.size()) { q_.pop_front(); } else { f.erase(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(n)); }
                got = n;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        err_ = (closing_ || shut_) ? "the connection is closed" : "receive timed out";
        return false;
    }
    void shutdown() override { shut_ = true; }
    void close() override { shut_ = true; }
    const char* lastError() const override { return err_.c_str(); }

private:
    std::mutex m_;
    std::deque<std::vector<unsigned char>> q_;
    std::atomic<bool> shut_{false};
    bool closing_ = false;
    std::string err_;
};

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }
std::vector<unsigned char> fxiq(rq::Format f, std::uint32_t rate, double centre, std::uint32_t seq) {
    std::vector<unsigned char> h = {'F', 'X', 'I', 'Q', 1, 0, static_cast<unsigned char>(static_cast<int>(f)), 0};
    put32(h, rate);
    const std::uint64_t b = std::bit_cast<std::uint64_t>(centre);
    put32(h, static_cast<std::uint32_t>(b & 0xFFFFFFFFu));
    put32(h, static_cast<std::uint32_t>(b >> 32));
    put32(h, seq);
    put32(h, 0);
    return h;
}
std::vector<unsigned char> f32bytes(const std::vector<std::complex<float>>& s) {
    std::vector<unsigned char> b;
    for (const auto& v : s) {
        put32(b, std::bit_cast<std::uint32_t>(v.real()));
        put32(b, std::bit_cast<std::uint32_t>(v.imag()));
    }
    return b;
}

std::vector<std::complex<float>> drain(RawIqNetSource& s, std::size_t want, int ms) {
    std::vector<std::complex<float>> got;
    std::vector<std::complex<float>> buf(4096);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (got.size() < want && std::chrono::steady_clock::now() < end) {
        const std::size_t n = s.read(buf.data(), std::min<std::size_t>(buf.size(), want - got.size()));
        got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    return got;
}

std::vector<std::complex<float>> ramp(std::size_t n) {
    std::vector<std::complex<float>> v;
    for (std::size_t i = 0; i < n; ++i) { v.emplace_back(0.5f * std::sin(0.01f * static_cast<float>(i)), -0.25f * std::cos(0.02f * static_cast<float>(i))); }
    return v;
}

void testA() {
    std::printf("A: the address grammar\n");
    rq::Spec s;
    std::string e;
    CHECK(rq::parseSpec("192.168.1.20:5555?rate=2048000", s, e));
    CHECK(!s.udp && s.host == "192.168.1.20" && s.port == 5555 && s.rateHz == 2048000.0 && s.format == rq::Format::F32 && !s.header && s.timeoutMs == 3000);
    CHECK(rq::parseSpec("tcp://host.example:1?rate=1000&fmt=u8&freq=100.1e6&timeout=500", s, e));
    CHECK(s.host == "host.example" && s.port == 1 && s.format == rq::Format::U8 && s.centreHz == 100.1e6 && s.timeoutMs == 500);
    CHECK(rq::parseSpec("udp://127.0.0.1:65535?hdr=1", s, e));
    CHECK(s.udp && s.port == 65535 && s.header);
    CHECK(rq::parseSpec("udp://[::1]:9000?rate=48000&fmt=s16", s, e) && s.host == "::1" && s.format == rq::Format::S16);
    CHECK(rq::parseSpec("h:9?hdr=1&rate=48000&fmt=u8", s, e) && s.header);   // (with a header the keys are read and then ignored: the header decides)
    const char* bad[] = {
        "", "host", "host:", ":5555", "host:0?rate=48000", "host:65536?rate=48000", "host:x?rate=48000", "host:5?fmt=u8",        // no rate
        "host:5?rate=999", "host:5?rate=100000001", "host:5?rate=abc", "host:5?rate=nan", "host:5?rate=48000&fmt=f64", "host:5?rate=48000&hdr=2",
        "host:5?rate=48000&timeout=10", "host:5?rate=48000&bogus=1", "host:5?rate=48000&novalue", "host:5?rate=48000&=1", "host:5?rate=48000&freq=inf",
    };
    for (const char* b : bad) {
        std::string err;
        rq::Spec t;
        CHECK(!rq::parseSpec(b, t, err));
        CHECK(!err.empty());
    }
}

void testB() {
    std::printf("B: the header and the decoders\n");
    rq::Header h;
    std::vector<unsigned char> good = fxiq(rq::Format::S16, 2400000, 100.1e6, 7);
    CHECK(rq::parseHeader(good.data(), good.size(), h));
    CHECK(h.format == rq::Format::S16 && h.rateHz == 2400000u && h.centreHz == 100.1e6 && h.sequence == 7u);
    CHECK(!rq::parseHeader(good.data(), 27, h));
    CHECK(!rq::parseHeader(nullptr, 28, h));
    auto mutate = [&](std::size_t at, unsigned char v) {
        std::vector<unsigned char> m = good;
        m[at] = v;
        return !rq::parseHeader(m.data(), m.size(), h);
    };
    CHECK(mutate(0, 'f') && mutate(4, 2) && mutate(5, 1) && mutate(6, 0) && mutate(6, 4) && mutate(7, 1));
    CHECK(!rq::parseHeader(fxiq(rq::Format::U8, 999, 0.0, 0).data(), 28, h));
    CHECK(!rq::parseHeader(fxiq(rq::Format::U8, 100000001u, 0.0, 0).data(), 28, h));
    CHECK(!rq::parseHeader(fxiq(rq::Format::U8, 48000, std::nan(""), 0).data(), 28, h));
    CHECK(!rq::parseHeader(fxiq(rq::Format::U8, 48000, 1e14, 0).data(), 28, h));
    // decoders, by hand
    std::complex<float> o[2];
    const unsigned char u8[] = {0, 255, 128, 127};
    rq::decode(rq::Format::U8, u8, 2, o);
    CHECK(o[0].real() == -1.0f && o[0].imag() == 1.0f && std::fabs(o[1].real() - (0.5f / 127.5f)) < 1e-6f && std::fabs(o[1].imag() + 0.5f / 127.5f) < 1e-6f);
    const unsigned char s16[] = {0xFF, 0x7F, 0x01, 0x80, 0x00, 0x00, 0x00, 0x40};   // 32767, -32767 ; 0, 16384
    rq::decode(rq::Format::S16, s16, 2, o);
    CHECK(o[0].real() == 1.0f && o[0].imag() == -1.0f && o[1].real() == 0.0f && std::fabs(o[1].imag() - 16384.0f / 32767.0f) < 1e-7f);
    std::vector<unsigned char> f;
    put32(f, std::bit_cast<std::uint32_t>(0.25f));
    put32(f, std::bit_cast<std::uint32_t>(std::nanf("")));
    put32(f, std::bit_cast<std::uint32_t>(HUGE_VALF));
    put32(f, std::bit_cast<std::uint32_t>(-0.75f));
    rq::decode(rq::Format::F32, f.data(), 2, o);
    CHECK(o[0].real() == 0.25f && o[0].imag() == 0.0f && o[1].real() == 0.0f && o[1].imag() == -0.75f);   // non-finite reads as zero
    CHECK(rq::pairBytes(rq::Format::U8) == 2 && rq::pairBytes(rq::Format::S16) == 4 && rq::pairBytes(rq::Format::F32) == 8 && rq::pairBytes(rq::Format::None) == 0);
    // random buffers: no crash, no past-the-end read (the sanitiser-less check is that the answers are consistent), and a false header is as rare as it should be
    std::mt19937_64 rng(5);
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        std::vector<unsigned char> r(rng() % 60);
        for (auto& b : r) { b = static_cast<unsigned char>(rng()); }
        rq::Header x;
        if (rq::parseHeader(r.data(), r.size(), x)) { ++accepted; }
        std::vector<std::complex<float>> out(r.size() / 2 + 1);
        rq::decode(static_cast<rq::Format>(1 + i % 3), r.data(), r.size() / rq::pairBytes(static_cast<rq::Format>(1 + i % 3)), out.data());
    }
    CHECK(accepted == 0);   // 4 bytes of magic: 20,000 random buffers never make one
}

void testC() {
    std::printf("C: a headerless stream, cut at awkward places\n");
    const std::vector<std::complex<float>> sig = ramp(3000);
    const std::vector<unsigned char> bytes = f32bytes(sig);
    auto t = std::make_shared<Script>();
    // pieces of 1, 3, 7, 4096 and 5 bytes, repeating: every cut inside a sample happens
    std::size_t pos = 0;
    const std::size_t cuts[] = {1, 3, 7, 4096, 5, 2, 13};
    for (std::size_t k = 0; pos < bytes.size(); ++k) {
        const std::size_t n = std::min(cuts[k % 7], bytes.size() - pos);
        t->push(std::vector<unsigned char>(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(pos + n)));
        pos += n;
    }
    RawIqNetSource s;
    CHECK(s.openWithTransportForTest("10.0.0.1:5555?rate=2048000&freq=7.1e6", t));
    CHECK(s.isOpen() && s.sampleRateHz() == 2048000.0 && s.centerFrequencyHz() == 7.1e6 && !s.setSampleRateHz(48000.0) && s.setSampleRateHz(2048000.0));
    CHECK(std::string(s.driverKey()) == "rawiq");
    CHECK(s.start());
    const std::vector<std::complex<float>> got = drain(s, sig.size(), 8000);
    CHECK(got.size() == sig.size());
    CHECK(got == sig);   // every sample, in order, bit for bit
    // freq= said where the stream is: the same number is accepted, another refused with a sentence, and the centre stays
    CHECK(s.setCenterFrequencyHz(7.1e6));
    CHECK(!s.setCenterFrequencyHz(14.2e6) && s.centerFrequencyHz() == 7.1e6);
    CHECK(std::string(s.lastError()).find("no tuner") != std::string::npos);
    CHECK(!s.setCenterFrequencyHz(std::nan("")));
    {   // nothing said: the centre is a label the user may set
        RawIqNetSource lab;
        CHECK(lab.openWithTransportForTest("10.0.0.1:5555?rate=48000", std::make_shared<Script>()));
        CHECK(lab.centerFrequencyHz() == 0.0 && lab.setCenterFrequencyHz(14.2e6) && lab.centerFrequencyHz() == 14.2e6);
        lab.closeDevice();
    }
    s.closeDevice();
    CHECK(!s.isOpen());
    // u8 and s16
    for (const rq::Format fm : {rq::Format::U8, rq::Format::S16}) {
        auto t2 = std::make_shared<Script>();
        std::vector<unsigned char> b;
        std::vector<std::complex<float>> want;
        for (int i = 0; i < 500; ++i) {
            if (fm == rq::Format::U8) {
                b.push_back(static_cast<unsigned char>(i % 256));
                b.push_back(static_cast<unsigned char>((i * 7) % 256));
                want.emplace_back((static_cast<float>(i % 256) - 127.5f) / 127.5f, (static_cast<float>((i * 7) % 256) - 127.5f) / 127.5f);
            } else {
                const std::int16_t re = static_cast<std::int16_t>(i * 60 - 15000), im = static_cast<std::int16_t>(12000 - i * 40);
                b.push_back(static_cast<unsigned char>(re & 0xFF)); b.push_back(static_cast<unsigned char>((re >> 8) & 0xFF));
                b.push_back(static_cast<unsigned char>(im & 0xFF)); b.push_back(static_cast<unsigned char>((im >> 8) & 0xFF));
                want.emplace_back(static_cast<float>(re) / 32767.0f, static_cast<float>(im) / 32767.0f);
            }
        }
        for (std::size_t o = 0; o < b.size(); o += 37) { t2->push(std::vector<unsigned char>(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), o + 37)))); }
        RawIqNetSource s2;
        CHECK(s2.openWithTransportForTest(std::string("h:1?rate=250000&fmt=") + (fm == rq::Format::U8 ? "u8" : "s16"), t2));
        CHECK(s2.start());
        CHECK(drain(s2, want.size(), 8000) == want);
        s2.closeDevice();
    }
}

void testD() {
    std::printf("D: the FXIQ header\n");
    // TCP: the header once, then samples; some samples arrive in the same read as the header
    {
        const std::vector<std::complex<float>> sig = ramp(1000);
        std::vector<unsigned char> stream = fxiq(rq::Format::F32, 1920000, 433.92e6, 0);
        const std::vector<unsigned char> body = f32bytes(sig);
        stream.insert(stream.end(), body.begin(), body.end());
        auto t = std::make_shared<Script>();
        t->push(std::vector<unsigned char>(stream.begin(), stream.begin() + 40));   // 28 of header and 12 bytes of samples (one and a half)
        for (std::size_t o = 40; o < stream.size(); o += 500) { t->push(std::vector<unsigned char>(stream.begin() + static_cast<std::ptrdiff_t>(o), stream.begin() + static_cast<std::ptrdiff_t>(std::min(stream.size(), o + 500)))); }
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest("h:1?hdr=1", t));
        CHECK(s.sampleRateHz() == 1920000.0 && s.centerFrequencyHz() == 433.92e6);   // known before the first sample
        CHECK(s.start());
        CHECK(drain(s, sig.size(), 8000) == sig);
        s.closeDevice();
    }
    // not a FXIQ stream
    {
        auto t = std::make_shared<Script>();
        t->push(std::vector<unsigned char>(64, 0x55));
        RawIqNetSource s;
        CHECK(!s.openWithTransportForTest("h:1?hdr=1&timeout=600", t));
        CHECK(!s.isOpen());
        CHECK(std::string(s.lastError()).find("FXIQ") != std::string::npos);
    }
    // nothing arrives
    {
        auto t = std::make_shared<Script>();
        RawIqNetSource s;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.openWithTransportForTest("h:1?hdr=1&timeout=400", t));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        CHECK(std::string(s.lastError()).find("header") != std::string::npos);
    }
    // UDP: datagrams with a header each; a gap, a repeat and a stray
    {
        auto t = std::make_shared<Script>();
        auto dgram = [&](std::uint32_t seq, std::size_t pairs, float v) {
            std::vector<unsigned char> d = fxiq(rq::Format::F32, 960000, 0.0, seq);
            const std::vector<unsigned char> b = f32bytes(std::vector<std::complex<float>>(pairs, {v, -v}));
            d.insert(d.end(), b.begin(), b.end());
            return d;
        };
        t->push(dgram(10, 100, 0.1f));    // the first: its number is not a gap
        t->push(dgram(11, 100, 0.2f));
        t->push(dgram(14, 100, 0.3f));    // 12 and 13 missing: 2 counted
        t->push(dgram(14, 100, 0.9f));    // a repeat: dropped
        t->push(std::vector<unsigned char>(200, 0x11));   // not FXIQ: stray
        t->push(dgram(15, 100, 0.4f));
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest("udp://127.0.0.1:1?hdr=1", t));
        CHECK(s.start());
        const std::vector<std::complex<float>> got = drain(s, 400, 8000);
        CHECK(got.size() == 400);
        if (got.size() == 400) { CHECK(got[0].real() == 0.1f && got[100].real() == 0.2f && got[200].real() == 0.3f && got[300].real() == 0.4f); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("  UDP: %llu datagrams missing, %llu stray\n", static_cast<unsigned long long>(s.droppedBuffers()), static_cast<unsigned long long>(s.strayDatagrams()));
        CHECK(s.droppedBuffers() == 2u);
        CHECK(s.strayDatagrams() == 2u);   // the repeat and the stray
        s.closeDevice();
    }
}

void testE() {
    std::printf("E: silence and closing\n");
    {
        auto t = std::make_shared<Script>();
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest("h:1?rate=48000&timeout=600", t));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::complex<float>> b(256);
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
            (void)s.read(b.data(), b.size());
        }
        CHECK(s.faulted());
        std::printf("  silence: %s\n", s.faultedWhile().c_str());
        CHECK(s.faultedWhile().find("I/Q stream") != std::string::npos);
        s.closeDevice();
    }
    {
        auto t = std::make_shared<Script>();
        t->push(f32bytes(ramp(10)));
        t->closeAfterQueue();
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest("h:1?rate=48000", t));
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::complex<float>> b(256);
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) { (void)s.read(b.data(), b.size()); }
        CHECK(s.faulted());
        CHECK(s.faultedWhile().find("failed") != std::string::npos);
        s.closeDevice();
    }
    {
        // an idle source that is closed: joins promptly
        auto t = std::make_shared<Script>();
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest("h:1?rate=48000", t));
        const auto t0 = std::chrono::steady_clock::now();
        s.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        CHECK(!s.isOpen());
        CHECK(!s.start());   // start() before a successful open()
    }
}

void testF() {
    std::printf("F: real sockets on 127.0.0.1\n");
    // TCP: a stand-in server greets with the FXIQ header and some samples, then sends more
    {
        const std::vector<std::complex<float>> sig = ramp(4000);
        const std::vector<unsigned char> body = f32bytes(sig);
        std::string greeting;
        {
            const std::vector<unsigned char> h = fxiq(rq::Format::F32, 2400000, 144.8e6, 0);
            greeting.assign(h.begin(), h.end());
            greeting.append(body.begin(), body.begin() + 8000);
        }
        nettest::StandInServer srv;
        CHECK(srv.start());
        srv.setGreeting(greeting);
        RawIqNetSource s;
        CHECK(s.open("127.0.0.1:" + std::to_string(srv.port()) + "?hdr=1"));
        CHECK(s.sampleRateHz() == 2400000.0 && s.centerFrequencyHz() == 144.8e6);
        CHECK(s.start());
        srv.sendTo(0, std::string(body.begin() + 8000, body.end()));
        const std::vector<std::complex<float>> got = drain(s, sig.size(), 8000);
        CHECK(got == sig);
        s.closeDevice();
    }
    // UDP: FoxSDR's own datagram transport as the sender
    {
        std::uint16_t port = 0;
        std::string e;
        std::unique_ptr<cascade::source::UdpTransport> probe;
        for (std::uint16_t p = 47650; p < 47700 && !probe; ++p) {
            probe = cascade::source::UdpTransport::bindLocal("127.0.0.1", p, std::chrono::milliseconds(100), e);
            if (probe) { port = p; }
        }
        CHECK(probe != nullptr);
        probe.reset();   // the number is free: the source takes it
        RawIqNetSource s;
        // the header is waited for at open(): the sender starts a moment after the call begins
        std::atomic<bool> go{true};
        std::thread sender([&] {
            std::string se;
            std::unique_ptr<cascade::source::UdpTransport> tx = cascade::source::UdpTransport::bindLocal("127.0.0.1", 0, std::chrono::milliseconds(100), se);
            if (!tx || !tx->connectTo("127.0.0.1", port)) { return; }
            std::uint32_t seq = 0;
            while (go.load() && seq < 400) {
                std::vector<unsigned char> d = fxiq(rq::Format::S16, 250000, 0.0, seq);
                for (int i = 0; i < 256; ++i) { d.push_back(static_cast<unsigned char>(i)); d.push_back(0x10); d.push_back(static_cast<unsigned char>(255 - i)); d.push_back(0x20); }
                (void)tx->sendAll(d.data(), d.size());
                ++seq;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        CHECK(s.open("udp://127.0.0.1:" + std::to_string(port) + "?hdr=1&timeout=4000"));
        CHECK(s.sampleRateHz() == 250000.0);
        CHECK(s.start());
        const std::vector<std::complex<float>> got = drain(s, 256 * 20, 6000);
        std::printf("  UDP loopback: %zu samples, %llu datagrams missing, %llu stray\n", got.size(), static_cast<unsigned long long>(s.droppedBuffers()), static_cast<unsigned long long>(s.strayDatagrams()));
        CHECK(got.size() == 256u * 20u);
        go.store(false);
        sender.join();
        s.closeDevice();
    }
}

void testG() {
    std::printf("G: random bytes, random cuts\n");
    std::mt19937_64 rng(77);
    for (const rq::Format fm : {rq::Format::U8, rq::Format::S16, rq::Format::F32}) {
        const std::size_t pb = rq::pairBytes(fm);
        std::vector<unsigned char> bytes(pb * 4000 + (pb - 1));   // a tail that never completes a sample
        for (auto& b : bytes) { b = static_cast<unsigned char>(rng()); }
        auto t = std::make_shared<Script>();
        for (std::size_t o = 0; o < bytes.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng() % 70, bytes.size() - o);
            t->push(std::vector<unsigned char>(bytes.begin() + static_cast<std::ptrdiff_t>(o), bytes.begin() + static_cast<std::ptrdiff_t>(o + n)));
            o += n;
        }
        RawIqNetSource s;
        CHECK(s.openWithTransportForTest(std::string("h:1?rate=96000&fmt=") + (fm == rq::Format::U8 ? "u8" : (fm == rq::Format::S16 ? "s16" : "f32")), t));
        CHECK(s.start());
        const std::vector<std::complex<float>> got = drain(s, 4000, 8000);
        std::vector<std::complex<float>> want(4000);
        rq::decode(fm, bytes.data(), 4000, want.data());
        CHECK(got.size() == 4000);
        CHECK(got == want);
        const std::vector<std::complex<float>> more = drain(s, 1, 300);
        CHECK(more.empty());   // the odd tail invented nothing
        s.closeDevice();
    }
}

}  // namespace

int main() {
    std::printf("test_raw_iq_net_source\n");
    testA();
    testB();
    testC();
    testD();
    testE();
    testF();
    testG();
    return testSummary("test_raw_iq_net_source");
}
