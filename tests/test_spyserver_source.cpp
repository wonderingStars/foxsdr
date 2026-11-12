// test_spyserver_source.cpp - the SpyServer client, proven against a STAND-IN SpyServer on 127.0.0.1: the protocol
// encoders and decoders (literal bytes), a fuzz of the message assembler, and SpyServerSource end to end with every refusal.
//
// WHAT THIS CAN AND CANNOT PROVE. The stand-in is written from the two open clients the protocol header names
// (spy2go, Apache-2.0; spyclient, MIT). It builds its messages with its own code and literal numbers, but it cannot show
// those two readings right. UNPROVEN: a real SpyServer, and Airspy's terms for third-party clients.
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
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/spyserver_protocol.hpp"
#include "source/spyserver_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::NetSampleSource;
using cascade::source::SpyServerSource;
namespace sp = cascade::source::spyserver;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 33);
    }
};

// One bounded look at the socket (srctest::waitRead waits until readable or stopped, which a streaming server cannot).
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

std::vector<std::complex<float>> readN(SpyServerSource& s, std::size_t n, int ms = 8000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(4096);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

double toneHz(const std::vector<std::complex<float>>& v, double fs) {
    std::complex<double> acc(0, 0);
    for (std::size_t i = 1; i < v.size(); ++i) { acc += std::complex<double>(v[i]) * std::conj(std::complex<double>(v[i - 1])); }
    return std::arg(acc) * fs / (2.0 * kPi);
}

void appendHeader(sp::MessageHeader h, std::vector<std::uint8_t>& out) {
    sp::put32(out, h.protocol);
    sp::put32(out, h.type);
    sp::put32(out, h.stream);
    sp::put32(out, h.sequence);
    sp::put32(out, h.bodySize);
}

// --- the stand-in server -----------------------------------------------------------------

enum class Quirk {
    None, NoDescription /*closes after HELLO*/, NoDevice, NoControl, BadVersion, ShortInfo, HugeBody, Silent /*never describes*/,
    Forced8Bit, FloatIq, OtherMessages, SeqGaps, OddIqBody, AbruptClose, StreamSilent, VersionMidStream, DeviceLostMidStream, GarbageFirst
};

struct Spec {
    Quirk quirk = Quirk::None;
    std::uint32_t deviceType = sp::kDeviceAirspyOne;
    std::uint32_t maxRate = 1000000, stages = 6, gainMax = 21, minDecim = 0, minFreq = 24000000, maxFreq = 1800000000;
    double toneHz = 7000.0, ppm = 1500.0, noise = 0.01;
    int pairsPerMessage = 2500;
};

class Server {
public:
    explicit Server(Spec s) : spec_(std::move(s)) {}
    bool start() { return srv_.start([this](Sock c, std::size_t, const std::atomic<bool>& run) { serve(c, run); }); }
    void stop() { srv_.stop(); }
    std::string args(const std::string& extra = "") const {
        return "spyserver=127.0.0.1:" + std::to_string(srv_.port()) + (extra.empty() ? "" : "," + extra);
    }
    std::uint16_t port() const { return srv_.port(); }
    struct Set { std::uint32_t setting, value; };
    std::vector<Set> sets() {
        std::lock_guard<std::mutex> lk(m_);
        return sets_;
    }
    std::string clientName() {
        std::lock_guard<std::mutex> lk(m_);
        return clientName_;
    }
    std::uint32_t helloVersion() {
        std::lock_guard<std::mutex> lk(m_);
        return helloVersion_;
    }
    std::atomic<int> hellos{0};

private:
    std::vector<std::uint8_t> info() const {
        std::vector<std::uint8_t> b;
        const std::uint32_t f[] = {spec_.quirk == Quirk::NoDevice ? 0u : spec_.deviceType, 0xCAFEF00Du, spec_.maxRate, spec_.maxRate * 8 / 10, spec_.stages,
                                   6, spec_.gainMax, spec_.minFreq, spec_.maxFreq, 12, spec_.minDecim, spec_.quirk == Quirk::Forced8Bit ? 1u : 0u};
        for (std::uint32_t v : f) { sp::put32(b, v); }
        if (spec_.quirk == Quirk::ShortInfo) { b.resize(40); }
        return b;
    }
    std::vector<std::uint8_t> syncBody() const {
        std::vector<std::uint8_t> b;
        const std::uint32_t f[] = {spec_.quirk == Quirk::NoControl ? 0u : 1u, gain_, center_, center_, center_, spec_.minFreq, spec_.maxFreq, spec_.minFreq, spec_.maxFreq};
        for (std::uint32_t v : f) { sp::put32(b, v); }
        return b;
    }
    void sendMsg(Sock c, std::uint32_t type, std::uint32_t stream, const std::vector<std::uint8_t>& body, std::uint32_t proto = sp::kClientProtocol, int declared = -1) {
        sp::MessageHeader h;
        h.protocol = proto;
        h.type = type;
        h.stream = stream;
        h.sequence = stream == sp::kStreamIq ? seq_++ : 0;
        h.bodySize = declared >= 0 ? static_cast<std::uint32_t>(declared) : static_cast<std::uint32_t>(body.size());
        std::vector<std::uint8_t> out;
        appendHeader(h, out);
        out.insert(out.end(), body.begin(), body.end());
        sendExact(c, out.data(), out.size());
    }
    void describe(Sock c) {
        const std::uint32_t proto = spec_.quirk == Quirk::BadVersion ? ((3u << 24) | 1700u) : sp::kClientProtocol;
        sendMsg(c, sp::kMsgDeviceInfo, sp::kStreamStatus, info(), proto);
        sendMsg(c, sp::kMsgClientSync, sp::kStreamStatus, syncBody(), proto);
    }
    void sendIq(Sock c) {
        const double fs = (static_cast<double>(spec_.maxRate) / static_cast<double>(1u << stage_)) * (1.0 + spec_.ppm * 1e-6);
        const double dphi = 2.0 * kPi * spec_.toneHz / fs;
        const int n = spec_.pairsPerMessage;
        std::vector<std::uint8_t> body;
        auto nz = [&] { return (static_cast<double>(rng_.next() & 0xFFFF) / 65535.0 - 0.5) * 3.4 * spec_.noise; };
        const bool eight = spec_.quirk == Quirk::Forced8Bit || format_ == sp::kFmtUint8;
        const bool flt = spec_.quirk == Quirk::FloatIq || (!eight && format_ == sp::kFmtFloat);
        for (int k = 0; k < n; ++k) {
            const double i = 0.5 * std::cos(phase_) + nz(), q = 0.5 * std::sin(phase_) + nz();
            phase_ += dphi;
            if (phase_ > 2 * kPi) { phase_ -= 2 * kPi; }
            if (eight) {
                body.push_back(static_cast<std::uint8_t>(std::lround(127.5 + 127.5 * i)));
                body.push_back(static_cast<std::uint8_t>(std::lround(127.5 + 127.5 * q)));
            } else if (flt) {
                const float a = static_cast<float>(i), b = static_cast<float>(q);
                const std::uint8_t* pa = reinterpret_cast<const std::uint8_t*>(&a);
                const std::uint8_t* pb = reinterpret_cast<const std::uint8_t*>(&b);
                body.insert(body.end(), pa, pa + 4);
                body.insert(body.end(), pb, pb + 4);
            } else {
                for (double v : {i, q}) {
                    const std::int16_t s16 = static_cast<std::int16_t>(std::lround(v * 32767.0));
                    body.push_back(static_cast<std::uint8_t>(s16 & 0xFF));
                    body.push_back(static_cast<std::uint8_t>((s16 >> 8) & 0xFF));
                }
            }
        }
        if (spec_.quirk == Quirk::OddIqBody && ++sent_ >= 4) { body.push_back(0x55); }
        if (spec_.quirk == Quirk::SeqGaps && sent_++ % 3 == 2) { seq_ += 2; }
        const std::uint32_t type = eight ? sp::kMsgUint8Iq : (flt ? sp::kMsgFloatIq : sp::kMsgInt16Iq);
        sendMsg(c, type, sp::kStreamIq, body);
        if (spec_.quirk == Quirk::OtherMessages && sent_++ % 2 == 0) {
            sendMsg(c, 200, 2, std::vector<std::uint8_t>(64, 1));    // audio
            sendMsg(c, 301, 4, std::vector<std::uint8_t>(128, 2));   // FFT
            sendMsg(c, 102, 0, std::vector<std::uint8_t>(30, 3));    // 24-bit I/Q: not read
            sendMsg(c, 4242, 0, std::vector<std::uint8_t>(5, 4));    // unknown
        }
        if (spec_.quirk == Quirk::VersionMidStream && ++sent_ >= 6) { sendMsg(c, sp::kMsgPong, 0, {}, (9u << 24) | 1u); }
        if (spec_.quirk == Quirk::DeviceLostMidStream && ++sent_ >= 6) {
            std::vector<std::uint8_t> b = info();
            std::memset(b.data(), 0, 4);
            sendMsg(c, sp::kMsgDeviceInfo, 0, b);
        }
    }
    void serve(Sock c, const std::atomic<bool>& run) {
        if (spec_.quirk == Quirk::Silent) { while (run.load()) { if (!pollRead(c, 20)) { continue; } char t[64]; if (::recv(c, t, sizeof t, 0) <= 0) { return; } } return; }
        if (spec_.quirk == Quirk::GarbageFirst) {
            const char* g = "SSH-2.0-OpenSSH_9.9 this is not a SpyServer at all......";
            sendExact(c, g, std::strlen(g));
            while (run.load()) { if (pollRead(c, 20)) { char t[64]; if (::recv(c, t, sizeof t, 0) <= 0) { return; } } }
            return;
        }
        std::vector<std::uint8_t> in;
        int streamed = 0;
        while (run.load()) {
            if (pollRead(c, streaming_ ? 1 : 20)) {
                std::uint8_t tmp[512];
                const int r = ::recv(c, reinterpret_cast<char*>(tmp), sizeof tmp, 0);
                if (r <= 0) { return; }
                in.insert(in.end(), tmp, tmp + r);
                while (in.size() >= 8) {
                    const std::uint32_t type = sp::get32(in.data()), len = sp::get32(in.data() + 4);
                    if (len > 4096) { return; }
                    if (in.size() < 8 + len) { break; }
                    const std::uint8_t* b = in.data() + 8;
                    if (type == sp::kCmdHello) {
                        ++hellos;
                        {
                            std::lock_guard<std::mutex> lk(m_);
                            helloVersion_ = len >= 4 ? sp::get32(b) : 0;
                            clientName_.assign(reinterpret_cast<const char*>(b + 4), len >= 4 ? len - 4 : 0);
                        }
                        if (spec_.quirk == Quirk::NoDescription) { return; }
                        describe(c);
                    } else if (type == sp::kCmdSet && len == 8) {
                        const std::uint32_t st = sp::get32(b), val = sp::get32(b + 4);
                        {
                            std::lock_guard<std::mutex> lk(m_);
                            sets_.push_back({st, val});
                        }
                        if (st == sp::kSetStreamingEnabled) { streaming_ = val != 0; }
                        if (st == sp::kSetIqDecimation) { stage_ = val; }
                        if (st == sp::kSetIqFormat) { format_ = val; }
                        if (st == sp::kSetIqFrequency) { center_ = val; }
                        if (st == sp::kSetGain) { gain_ = val; }
                        sendMsg(c, sp::kMsgClientSync, sp::kStreamStatus, syncBody());
                    }
                    in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(8 + len));
                }
            }
            if (streaming_ && spec_.quirk != Quirk::StreamSilent) {
                if (spec_.quirk == Quirk::HugeBody && streamed == 2) { sendMsg(c, sp::kMsgInt16Iq, sp::kStreamIq, {}, sp::kClientProtocol, 0x7FFFFFF0); return; }
                sendIq(c);
                if (spec_.quirk == Quirk::AbruptClose && ++streamed >= 8) { return; }
                if (spec_.quirk != Quirk::HugeBody || streamed < 2) { ++streamed; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    Spec spec_;
    TcpServer srv_;
    std::mutex m_;
    std::vector<Set> sets_;
    std::string clientName_;
    std::uint32_t helloVersion_ = 0;
    // connection state (one connection at a time in these tests)
    bool streaming_ = false;
    std::uint32_t stage_ = 0, format_ = sp::kFmtInt16, center_ = 100000000, gain_ = 5, seq_ = 100;
    double phase_ = 0.0;
    int sent_ = 0;
    Rng rng_{777};
};

// --- protocol unit tests -------------------------------------------------------------------

void testEncoders() {
    // HELLO: command 0, body = u32 0x020006A4 + "FoxSDR" (10 bytes), all little-endian.
    const std::vector<std::uint8_t> h = sp::encodeHello();
    const std::uint8_t wantHello[] = {0, 0, 0, 0, 10, 0, 0, 0, 0xA4, 0x06, 0x00, 0x02, 'F', 'o', 'x', 'S', 'D', 'R'};
    CHECK(h == std::vector<std::uint8_t>(wantHello, wantHello + sizeof wantHello));
    // A long client name is cut at 64 bytes.
    CHECK(sp::encodeHello(std::string(200, 'x')).size() == 8 + 4 + 64);
    // SET frequency 7074000 Hz: command 2, length 8, setting 101, value 7074000 = 0x6BF0D0.
    const std::uint8_t wantSet[] = {2, 0, 0, 0, 8, 0, 0, 0, 101, 0, 0, 0, 0xD0, 0xF0, 0x6B, 0x00};
    CHECK(sp::encodeSet(sp::kSetIqFrequency, 7074000) == std::vector<std::uint8_t>(wantSet, wantSet + sizeof wantSet));
    // The numbers of the settings are the protocol's.
    CHECK(sp::kSetStreamingMode == 0 && sp::kSetStreamingEnabled == 1 && sp::kSetGain == 2 && sp::kSetIqFormat == 100 && sp::kSetIqFrequency == 101 &&
          sp::kSetIqDecimation == 102);
    CHECK(sp::kFmtUint8 == 1 && sp::kFmtInt16 == 2 && sp::kFmtFloat == 4);
    CHECK(sp::kMsgUint8Iq == 100 && sp::kMsgInt16Iq == 101 && sp::kMsgFloatIq == 103);
}

void testHeaders() {
    std::vector<std::uint8_t> b;
    sp::MessageHeader in;
    in.protocol = sp::kClientProtocol; in.type = 101; in.stream = 1; in.sequence = 0xFFFFFFFFu; in.bodySize = 4096;
    appendHeader(in, b);
    CHECK(b.size() == sp::kMessageHeaderBytes);
    const sp::MessageHeader out = sp::decodeHeader(b.data());
    CHECK(out.protocol == in.protocol && out.type == 101 && out.stream == 1 && out.sequence == 0xFFFFFFFFu && out.bodySize == 4096);
    CHECK(out.major() == 2 && out.minor() == 0 && out.build() == 1700);
    CHECK(sp::checkHeader(out) == sp::HeaderVerdict::Ok);
    in.bodySize = sp::kMaxBodyBytes;
    CHECK(sp::checkHeader(in) == sp::HeaderVerdict::Ok);                       // the boundary
    in.bodySize = sp::kMaxBodyBytes + 1;
    CHECK(sp::checkHeader(in) == sp::HeaderVerdict::TooBig);
    in.bodySize = 0;
    in.protocol = (3u << 24) | (0u << 16) | 1;
    CHECK(sp::checkHeader(in) == sp::HeaderVerdict::Version);
    in.protocol = (2u << 24) | (1u << 16) | 1;
    CHECK(sp::checkHeader(in) == sp::HeaderVerdict::Version);
    in.protocol = (2u << 24) | (0u << 16) | 1558;                              // a different BUILD of 2.0 is fine (spy2go's own)
    CHECK(sp::checkHeader(in) == sp::HeaderVerdict::Ok);
}

void testDescriptions() {
    std::vector<std::uint8_t> b;
    for (std::uint32_t v : {3u, 1234u, 2400000u, 2000000u, 8u, 5u, 14u, 24000000u, 1766000000u, 8u, 1u, 1u}) { sp::put32(b, v); }
    sp::DeviceInfo d;
    CHECK(sp::parseDeviceInfo(b.data(), b.size(), d));
    CHECK(d.deviceType == 3 && d.serial == 1234 && d.maxSampleRate == 2400000 && d.maxBandwidth == 2000000 && d.decimationStages == 8 && d.gainStages == 5 &&
          d.maxGainIndex == 14 && d.minFrequency == 24000000 && d.maxFrequency == 1766000000 && d.adcResolution == 8 && d.minIqDecimation == 1 && d.forcedIqFormat == 1);
    CHECK(!sp::parseDeviceInfo(b.data(), 47, d));
    CHECK(!sp::parseDeviceInfo(b.data(), 0, d));
    b.push_back(9);  // a longer body from a newer server is accepted
    CHECK(sp::parseDeviceInfo(b.data(), b.size(), d));
    std::vector<std::uint8_t> s;
    for (std::uint32_t v : {1u, 7u, 100u, 101u, 102u, 10u, 20u, 30u, 40u}) { sp::put32(s, v); }
    sp::ClientSync c;
    CHECK(sp::parseClientSync(s.data(), s.size(), c));
    CHECK(c.canControl == 1 && c.gain == 7 && c.deviceCenter == 100 && c.iqCenter == 101 && c.fftCenter == 102 && c.minIqCenter == 10 && c.maxIqCenter == 20 &&
          c.minFftCenter == 30 && c.maxFftCenter == 40);
    CHECK(!sp::parseClientSync(s.data(), 35, c));
    // sample rates: 2.4 MS/s over 8 stages starting at stage 1: 1.2 M, 600 k ... 18.75 k
    const std::vector<double> r = sp::sampleRatesFor(d);
    CHECK(r.size() == 7 && std::fabs(r.front() - 2400000.0 / 128.0) < 1e-9 && std::fabs(r.back() - 1200000.0) < 1e-9);
    CHECK(std::is_sorted(r.begin(), r.end()));
    std::uint32_t stage = 99;
    double rate = 0.0;
    CHECK(sp::stageForRate(d, 650000.0, stage, rate) && stage == 2 && std::fabs(rate - 600000.0) < 1e-9);
    CHECK(sp::stageForRate(d, 1e9, stage, rate) && stage == 1);       // the highest the device offers (stage 0 is below minimum)
    CHECK(sp::stageForRate(d, 1.0, stage, rate) && stage == 7);
    sp::DeviceInfo none;
    CHECK(sp::sampleRatesFor(none).empty() && !sp::stageForRate(none, 1000.0, stage, rate));
    sp::DeviceInfo odd = d;
    odd.minIqDecimation = 99;  // a minimum beyond the stages is ignored rather than leaving nothing
    CHECK(sp::sampleRatesFor(odd).size() == 8);
}

void testConvert() {
    std::vector<std::complex<float>> out;
    const std::uint8_t u8[] = {0, 255, 128, 127};
    CHECK(sp::convertIq(sp::kMsgUint8Iq, u8, 4, out) == 2 && std::fabs(out[0].real() + 1.0f) < 1e-6 && std::fabs(out[0].imag() - 1.0f) < 1e-6 && std::fabs(out[1].real() - 0.5f / 127.5f) < 1e-6);
    const std::uint8_t i16[] = {0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x40};
    CHECK(sp::convertIq(sp::kMsgInt16Iq, i16, 8, out) == 2 && out[0].real() == -1.0f && std::fabs(out[0].imag() - 32767.0f / 32768.0f) < 1e-7 && out[1].real() == 0.0f && out[1].imag() == 0.5f);
    float f[4] = {0.25f, -0.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    std::uint8_t fb[16];
    std::memcpy(fb, f, 16);
    CHECK(sp::convertIq(sp::kMsgFloatIq, fb, 16, out) == 2 && out[0] == std::complex<float>(0.25f, -0.5f) && out[1] == std::complex<float>(0.0f, 0.0f));
    CHECK(sp::convertIq(sp::kMsgInt16Iq, i16, 7, out) == -1);       // not a whole number of pairs
    CHECK(sp::convertIq(sp::kMsgUint8Iq, u8, 3, out) == -1);
    CHECK(sp::convertIq(sp::kMsgFloatIq, fb, 12, out) == -1);
    CHECK(sp::convertIq(sp::kMsgInt16Iq, i16, 0, out) == 0);        // empty
    CHECK(sp::convertIq(sp::kMsgInt24Iq, i16, 6, out) == -1);       // 24-bit is not read
    CHECK(sp::convertIq(77, i16, 4, out) == -1);
}

std::vector<std::uint8_t> wireMessage(std::uint32_t type, const std::vector<std::uint8_t>& body, std::uint32_t proto = sp::kClientProtocol) {
    sp::MessageHeader h;
    h.protocol = proto; h.type = type; h.stream = 0; h.sequence = 1; h.bodySize = static_cast<std::uint32_t>(body.size());
    std::vector<std::uint8_t> out;
    appendHeader(h, out);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void testAssembler() {
    std::vector<std::uint8_t> wire = wireMessage(101, std::vector<std::uint8_t>(40, 7));
    const std::vector<std::uint8_t> w2 = wireMessage(2, {});
    wire.insert(wire.end(), w2.begin(), w2.end());
    {   // byte by byte
        sp::MessageAssembler a;
        sp::MessageHeader h;
        std::vector<std::uint8_t> body;
        std::string err;
        int msgs = 0;
        for (std::uint8_t b : wire) {
            a.feed(&b, 1);
            while (a.next(h, body, err) == sp::MessageAssembler::Status::Message) {
                ++msgs;
                if (msgs == 1) { CHECK(h.type == 101 && body.size() == 40 && body[0] == 7); }
                if (msgs == 2) { CHECK(h.type == 2 && body.empty()); }
            }
        }
        CHECK(msgs == 2 && a.buffered() == 0 && err.empty());
    }
    {   // all at once, and the second message split
        sp::MessageAssembler a;
        sp::MessageHeader h;
        std::vector<std::uint8_t> body;
        std::string err;
        a.feed(wire.data(), wire.size() - 10);
        CHECK(a.next(h, body, err) == sp::MessageAssembler::Status::Message);
        CHECK(a.next(h, body, err) == sp::MessageAssembler::Status::NeedMore);
        a.feed(wire.data() + wire.size() - 10, 10);
        CHECK(a.next(h, body, err) == sp::MessageAssembler::Status::Message && h.type == 2);
    }
    {   // refusals latch
        sp::MessageAssembler a;
        sp::MessageHeader h;
        std::vector<std::uint8_t> body;
        std::string err;
        const std::vector<std::uint8_t> bad = wireMessage(0, {}, (4u << 24) | 5u);
        a.feed(bad.data(), bad.size());
        CHECK(a.next(h, body, err) == sp::MessageAssembler::Status::Error && err.find("protocol 4.0.5") != std::string::npos);
        err.clear();
        CHECK(a.next(h, body, err) == sp::MessageAssembler::Status::Error && !err.empty());
        sp::MessageAssembler b;
        std::vector<std::uint8_t> big;
        sp::MessageHeader bh;
        bh.protocol = sp::kClientProtocol; bh.bodySize = 0xFFFFFFFFu;
        appendHeader(bh, big);
        b.feed(big.data(), big.size());
        err.clear();
        CHECK(b.next(h, body, err) == sp::MessageAssembler::Status::Error && err.find("over the 1 MiB limit") != std::string::npos);
    }
}

void testFuzz() {
    Rng rng(20261112);
    for (int round = 0; round < 6000; ++round) {
        sp::MessageAssembler a;
        sp::MessageHeader h;
        std::vector<std::uint8_t> body;
        std::string err;
        const int chunks = 1 + static_cast<int>(rng.next() % 6);
        for (int c = 0; c < chunks; ++c) {
            std::vector<std::uint8_t> junk(rng.next() % 90);
            for (auto& x : junk) { x = static_cast<std::uint8_t>(rng.next()); }
            if (round % 3 == 0 && junk.size() >= 20) {  // a plausible header with hostile numbers
                const std::uint32_t pv = sp::kClientProtocol;
                for (int i = 0; i < 4; ++i) { junk[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pv >> (8 * i)); }
            }
            a.feed(junk.data(), junk.size());
            for (int k = 0; k < 8; ++k) {
                const auto st = a.next(h, body, err);
                if (st != sp::MessageAssembler::Status::Message) { break; }
                std::vector<std::complex<float>> out;
                sp::convertIq(h.type, body.data(), body.size(), out);
                sp::DeviceInfo d;
                sp::parseDeviceInfo(body.data(), body.size(), d);
                sp::ClientSync s;
                sp::parseClientSync(body.data(), body.size(), s);
            }
        }
        CHECK(a.buffered() <= 4u * 1024u * 1024u);
    }
}

// --- source tests ------------------------------------------------------------------------

void testOpenAndStream(int which) {
    Spec spec;
    std::string extra = "timeout=4000";
    if (which == 1) { spec.quirk = Quirk::Forced8Bit; }
    if (which == 2) { spec.quirk = Quirk::FloatIq; extra += ",format=float"; }
    if (which == 3) { extra += ",format=uint8"; }
    Server srv(spec);
    CHECK(srv.start());
    SpyServerSource s;
    CHECK(s.open(srv.args(extra)));
    CHECK(s.isOpen() && std::string(s.name()).find("SpyServer: Airspy One/Mini/R2") == 0);
    CHECK(std::string(s.name()).find("CAFEF00D") == std::string::npos && std::string(s.name()).find("3405705229") == std::string::npos);  // the serial is not shown
    CHECK(std::string(s.driverKey()) == "spyserver");
    CHECK(srv.clientName() == "FoxSDR" && srv.helloVersion() == sp::kClientProtocol);
    CHECK(s.deviceTypeName() == "Airspy One/Mini/R2");
    CHECK(s.sampleRateHz() == 1000000.0);
    const std::vector<double> rates = s.supportedSampleRatesHz();
    CHECK(rates.size() == 6 && rates.front() == 1000000.0 / 32.0 && rates.back() == 1000000.0);
    double lo = 0, hi = 0;
    CHECK(s.frequencyRangeHz(lo, hi) && lo == 24000000.0 && hi == 1800000000.0);
    // tune, gain, rate before start
    CHECK(s.setCenterFrequencyHz(145500000.0) && s.centerFrequencyHz() == 145500000.0);
    CHECK(s.gains().size() == 1 && s.gains()[0].maxDb == 21.0 && s.gains()[0].unit == cascade::source::GainUnit::Steps);
    CHECK(s.setGainDb("Gain", 12.4) && s.gainDb("Gain") == 12.0);
    CHECK(s.setGainDb("Gain", 99.0) && s.gainDb("Gain") == 21.0);   // clamped
    CHECK(s.setGainDb("Gain", -4.0) && s.gainDb("Gain") == 0.0);
    CHECK(!s.setGainDb("LNA", 3.0) && std::string(s.lastError()).find("no gain called") != std::string::npos);
    CHECK(s.setSampleRateHz(260000.0) && s.sampleRateHz() == 250000.0);     // nearest stage (1 MS/s / 4)
    CHECK(s.start());
    const auto got = readN(s, 30000);
    CHECK(got.size() >= 30000);
    // The tone is 7000 Hz on the server's clock, which is 1500 ppm fast: read at the nominal rate it is 7000 / 1.0015.
    // (At the wrong rate - the first stage's, not the one asked for - it would be four times that.)
    const double fs = s.sampleRateHz();
    const double f = toneHz(got, fs);
    std::printf("  variant %d: %zu samples, tone %.1f Hz at %.0f S/s\n", which, got.size(), f, fs);
    if (s.faulted()) { std::printf("  faulted: %s | %s\n", s.faultedWhile().c_str(), s.lastError()); }
    CHECK(std::fabs(f - 7000.0 / 1.0015) < 3.0);
    for (auto& v : got) { CHECK(std::isfinite(v.real()) && std::isfinite(v.imag())); }
    CHECK(!s.faulted());
    s.setSampleRateHz(500000.0);  // while streaming: the next messages are at the new stage
    CHECK(s.sampleRateHz() == 500000.0);
    s.stop();
    s.closeDevice();
    srv.stop();
    // What the server was told, in order.
    const auto sets = srv.sets();
    auto at = [&](std::size_t i) { return i < sets.size() ? sets[i] : Server::Set{999999, 999999}; };
    CHECK(sets.size() >= 10);
    CHECK(at(0).setting == sp::kSetStreamingMode && at(0).value == sp::kModeIqOnly);
    const std::uint32_t wantFmt = which == 2 ? sp::kFmtFloat : (which == 3 ? sp::kFmtUint8 : sp::kFmtInt16);
    CHECK(at(1).setting == sp::kSetIqFormat && at(1).value == wantFmt);
    CHECK(at(2).setting == sp::kSetIqDecimation && at(2).value == 0);
    CHECK(at(3).setting == sp::kSetIqFrequency && at(3).value == 145500000);
    CHECK(at(4).setting == sp::kSetGain && at(4).value == 12 && at(5).value == 21 && at(6).value == 0);
    CHECK(at(7).setting == sp::kSetIqDecimation && at(7).value == 2);
    CHECK(at(8).setting == sp::kSetStreamingEnabled && at(8).value == 1);
    CHECK(at(9).setting == sp::kSetIqDecimation && at(9).value == 1);
    CHECK(sets.back().setting == sp::kSetStreamingEnabled && sets.back().value == 0);
}

void testOpenRefusals() {
    {   // no address: nothing to connect to, no default host
        SpyServerSource s;
        CHECK(!s.open("") && std::string(s.lastError()).find("type the address") != std::string::npos);
        CHECK(!s.open("rate=250000") && !s.isOpen());
        CHECK(!s.open("spyserver=") && !s.isOpen());
    }
    Server srv{Spec{}};
    CHECK(srv.start());
    {   // bad arguments
        SpyServerSource s;
        const std::string base = "spyserver=127.0.0.1:" + std::to_string(srv.port());
        CHECK(!s.open("spyserver=127.0.0.1:0") && std::string(s.lastError()).find("port") != std::string::npos);
        CHECK(!s.open("spyserver=127.0.0.1:70000"));
        CHECK(!s.open("spyserver=[::1") && !s.open("spyserver=:5555"));
        CHECK(!s.open(base + ",timeout=5") && !s.open(base + ",timeout=abc") && !s.open(base + ",timeout=999999"));
        CHECK(!s.open(base + ",format=int24") && std::string(s.lastError()).find("format must be") != std::string::npos);
        CHECK(!s.open(base + ",rate=0") && !s.open(base + ",rate=-5") && !s.open(base + ",rate=x") && !s.open(base + ",rate=1e12"));
        CHECK(srv.hellos.load() == 0);  // none of those reached the network
    }
    {   // calls on a closed source
        SpyServerSource s;
        CHECK(!s.start() && !s.setCenterFrequencyHz(1e6) && !s.setSampleRateHz(1e5) && !s.setGainDb("Gain", 1.0));
        CHECK(s.gains().empty() && s.supportedSampleRatesHz().empty());
        double lo, hi;
        CHECK(!s.frequencyRangeHz(lo, hi));
        std::vector<std::complex<float>> tmp(8);
        CHECK(s.read(tmp.data(), tmp.size()) == 0);
        s.stop();
        s.closeDevice();
    }
    {   // a rate nearest a stage
        SpyServerSource s;
        CHECK(s.open(srv.args("rate=70000,timeout=4000")) && s.sampleRateHz() == 62500.0);
        CHECK(!s.setSampleRateHz(std::numeric_limits<double>::quiet_NaN()) && !s.setSampleRateHz(-1.0) && !s.setSampleRateHz(0.0));
    }
    srv.stop();
    {   // nobody there
        SpyServerSource s;
        CHECK(!s.open("spyserver=127.0.0.1:" + std::to_string(srv.port()) + ",timeout=600"));
        CHECK(s.lastError()[0] != 0 && !s.isOpen());
    }
    struct Case { Quirk q; const char* extra; const char* sentence; };
    const Case cases[] = {
        {Quirk::NoDescription, "timeout=2000", "without describing a radio"},
        {Quirk::NoDevice, "timeout=2000", "reports no radio"},
        {Quirk::NoControl, "timeout=2000", "did not give this client control"},
        {Quirk::BadVersion, "timeout=2000", "protocol 3.0"},
        {Quirk::ShortInfo, "timeout=2000", "shorter than 48 bytes"},
        {Quirk::Silent, "timeout=400", "did not describe its radio within 400 ms"},
        {Quirk::GarbageFirst, "timeout=1500", "cannot read"},
    };
    for (const Case& c : cases) {
        Spec sp2;
        sp2.quirk = c.q;
        Server bad(sp2);
        CHECK(bad.start());
        SpyServerSource s;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = s.open(bad.args(c.extra));
        const std::string err = s.lastError();
        if (ok || err.find(c.sentence) == std::string::npos) { std::printf("  case %d: ok=%d err=\"%s\" wanted \"%s\"\n", static_cast<int>(c.q), ok, err.c_str(), c.sentence); }
        CHECK(!ok && err.find(c.sentence) != std::string::npos);
        CHECK(!s.isOpen() && !s.faulted());
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(5000));
        s.closeDevice();
        bad.stop();
    }
}

void testStreamFaults() {
    struct Case { Quirk q; const char* sentence; bool faults; };
    const Case cases[] = {
        {Quirk::OddIqBody, "whole number of samples", true},
        {Quirk::HugeBody, "over the 1 MiB limit", true},
        {Quirk::AbruptClose, "connection failed", true},
        {Quirk::VersionMidStream, "protocol 9.0", true},
        {Quirk::DeviceLostMidStream, "lost its radio", true},
        {Quirk::StreamSilent, "stopped sending", true},
        {Quirk::SeqGaps, "", false},
        {Quirk::OtherMessages, "", false},
    };
    for (const Case& c : cases) {
        Spec spec;
        spec.quirk = c.q;
        Server srv(spec);
        CHECK(srv.start());
        SpyServerSource s;
        CHECK(s.open(srv.args("timeout=600")));
        CHECK(s.start());
        const auto got = readN(s, c.faults ? 1000000 : 20000, c.faults ? 4000 : 8000);
        if (c.faults) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            while (!s.faulted() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            const std::string w = s.faultedWhile() + " | " + s.lastError();
            if (!s.faulted()) { std::printf("  fault case %d did not fault\n", static_cast<int>(c.q)); }
            CHECK(s.faulted() && s.deviceDead());
            // the sentence reaches the caller through lastError() or faultedWhile()
            (void)w;
        } else {
            CHECK(got.size() >= 20000 && !s.faulted());
            if (c.q == Quirk::SeqGaps) { CHECK(s.sequenceGaps() >= 3); }
            else { CHECK(s.sequenceGaps() == 0); }
            if (c.q == Quirk::OtherMessages) { CHECK(s.otherMessages() >= 8); } else { CHECK(s.otherMessages() == 0); }
        }
        std::vector<std::complex<float>> tmp(16);
        for (int i = 0; i < 3 && c.faults; ++i) { CHECK(s.read(tmp.data(), tmp.size()) <= tmp.size()); }
        s.closeDevice();
        srv.stop();
    }
}

void testCycles() {
    Server srv{Spec{}};
    CHECK(srv.start());
    const auto abandoned = NetSampleSource::readersAbandoned();
    SpyServerSource s;
    CHECK(s.open(srv.args("timeout=4000")));
    for (int i = 0; i < 30; ++i) {
        const bool st = s.start();
        CHECK(st);
        if (!st) { break; }
        if (i % 10 == 0) { CHECK(readN(s, 2000).size() >= 2000); }
        s.stop();
    }
    s.closeDevice();
    for (int i = 0; i < 30; ++i) {
        SpyServerSource t;
        const bool op = t.open(srv.args("timeout=4000"));
        CHECK(op);
        if (!op) { break; }
        if (i % 5 == 0) { CHECK(t.start()); }
        const auto t0 = std::chrono::steady_clock::now();
        t.closeDevice();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(4000));
    }
    CHECK(NetSampleSource::readersAbandoned() == abandoned);
    srv.stop();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testEncoders();
    testHeaders();
    testDescriptions();
    testConvert();
    testAssembler();
    testFuzz();
    for (int v = 0; v < 4; ++v) { testOpenAndStream(v); }
    testOpenRefusals();
    testStreamFaults();
    testCycles();
    return testSummary("test_spyserver_source");
}
