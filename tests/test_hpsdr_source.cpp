// test_hpsdr_source.cpp - the openHPSDR protocol-1 (Metis) client: discovery, start/stop and the receive
// envelope, proven against a STAND-IN device on loopback UDP written from "Metis - How it works" V1.33
// (cited in src/source/hpsdr_protocol.hpp), with literal byte strings.
//
// THE FRAME LAYOUT IS NOT IN THAT DOCUMENT, so the decoder installed here is a TEST INVENTION: each
// 512-byte frame is 128 pairs of little-endian int16 I, Q. It proves the plumbing (envelope, sequence
// accounting, gating, shutdown) and says NOTHING about the real HPSDR frame format. UNPROVEN: every
// real device.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/hpsdr_protocol.hpp"
#include "source/hpsdr_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::HpsdrSource;
using cascade::source::HpsdrDevice;
using cascade::source::NetSampleSource;
namespace hp = cascade::source::hpsdr;

namespace {

// The test-invented frame layout (NOT the real one): 128 pairs of int16 LE per 512-byte frame.
class TestDecoder : public cascade::source::HpsdrFrameDecoder {
public:
    bool decode(const std::uint8_t* f, std::vector<std::complex<float>>& out) override {
        if (reject.load()) { return false; }
        for (int i = 0; i < 256; ++i) {
            const std::int16_t a = static_cast<std::int16_t>(f[4 * i] | (f[4 * i + 1] << 8));
            const std::int16_t b = static_cast<std::int16_t>(f[4 * i + 2] | (f[4 * i + 3] << 8));
            out.push_back({a / 32768.0f, b / 32768.0f});
        }
        return true;
    }
    double sampleRateHz() const override { return 192000.0; }
    std::atomic<bool> reject{false};
};

enum class Mode { Normal, NoReply, Junk, ShortReply, BadStatus, DoubleReply };
enum class Flow { None, DropOne, RepeatOne, BadPackets, Bandscope, Stall, Foreign };

class FakeDevice {
public:
    explicit FakeDevice(std::uint8_t macLast = 0x66, std::uint8_t board = 0x01) : macLast_(macLast), board_(board) {}
    ~FakeDevice() { stop(); }
    bool start() {
        if (!sock_.open()) { return false; }
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_.store(false);
        if (th_.joinable()) { th_.join(); }
        stopStream();
    }
    std::uint16_t port() const { return sock_.port(); }
    std::string args(const std::string& extra = "") const {
        return "hpsdr=127.0.0.1:" + std::to_string(sock_.port()) + (extra.empty() ? "" : "," + extra);
    }
    std::atomic<Mode> mode{Mode::Normal};
    std::atomic<Flow> flow{Flow::None};
    std::atomic<int> flowAt{30};
    std::atomic<int> discoveries{0}, starts{0}, stops{0}, badCommands{0};
    std::atomic<int> lastStartCommand{-1};
    std::atomic<int> firstSeqAfterStart{-1};
    int sent() const { return sent_.load(); }

private:
    Bytes reply(std::uint8_t status) const {
        Bytes r(60, 0);
        r[0] = 0xEF; r[1] = 0xFE; r[2] = status;
        const std::uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, macLast_};
        for (int i = 0; i < 6; ++i) { r[3 + i] = mac[i]; }
        r[9] = 32;      // code version
        r[10] = board_; // Board_ID
        return r;
    }
    void loop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!sock_.recvFrom(run_, d, from)) { continue; }
            if (d.size() == 63 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x02 &&
                std::all_of(d.begin() + 3, d.end(), [](std::uint8_t b) { return b == 0; })) {
                ++discoveries;
                switch (mode.load()) {
                    case Mode::NoReply: break;
                    case Mode::Junk: { Bytes j = reply(2); j[0] = 0x00; sock_.sendTo(from, j.data(), j.size()); break; }
                    case Mode::ShortReply: { Bytes j = reply(2); j.resize(8); sock_.sendTo(from, j.data(), j.size()); break; }
                    case Mode::BadStatus: { Bytes j = reply(7); sock_.sendTo(from, j.data(), j.size()); break; }
                    case Mode::DoubleReply: { const Bytes j = reply(2); sock_.sendTo(from, j.data(), j.size()); sock_.sendTo(from, j.data(), j.size()); break; }
                    default: { const Bytes j = reply(streaming_.load() ? 3 : 2); sock_.sendTo(from, j.data(), j.size()); break; }
                }
            } else if (d.size() == 64 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x04 &&
                       std::all_of(d.begin() + 4, d.end(), [](std::uint8_t b) { return b == 0; })) {
                lastStartCommand = d[3];
                if (d[3] & 1) {
                    ++starts;
                    startStream(from);
                } else {
                    ++stops;
                    stopStream();
                }
            } else {
                ++badCommands;
            }
        }
    }
    void startStream(std::uint16_t to) {
        stopStream();
        streamRun_.store(true);
        streaming_.store(true);
        stream_ = std::thread([this, to] {
            std::uint32_t seq = 0;
            std::int64_t n = 0;
            int count = 0;
            while (streamRun_.load()) {
                const Flow f = flow.load();
                if (f == Flow::Stall && count >= flowAt.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
                Bytes frames(1024);
                for (int i = 0; i < 256; ++i, ++n) {
                    const std::int16_t v = static_cast<std::int16_t>(n % 20000);
                    const std::int16_t q = static_cast<std::int16_t>(-v);
                    frames[4 * i] = static_cast<std::uint8_t>(v & 0xFF);
                    frames[4 * i + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                    frames[4 * i + 2] = static_cast<std::uint8_t>(q & 0xFF);
                    frames[4 * i + 3] = static_cast<std::uint8_t>((q >> 8) & 0xFF);
                }
                // literal envelope: EF FE 01 06 <seq big-endian> <1024 bytes>
                Bytes p = {0xEF, 0xFE, 0x01, 0x06, static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
                           static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
                p.insert(p.end(), frames.begin(), frames.end());
                if (count == 0) { firstSeqAfterStart = static_cast<int>(seq); }
                const bool atFlow = count == flowAt.load();
                if (f == Flow::DropOne && atFlow) { ++seq; ++count; continue; }               // a packet lost on the wire
                if (f == Flow::RepeatOne && atFlow) { sock_.sendTo(to, p.data(), p.size()); }  // sent twice
                if (f == Flow::BadPackets && count % 7 == 3) {
                    Bytes b1(p.begin(), p.end() - 1);      sock_.sendTo(to, b1.data(), b1.size());   // 1031 bytes
                    Bytes b2 = p; b2[0] = 0x00;            sock_.sendTo(to, b2.data(), b2.size());   // wrong magic
                    Bytes b3 = p; b3[3] = 0x09;            sock_.sendTo(to, b3.data(), b3.size());   // unknown end point
                    Bytes b4 = p; b4.push_back(0);         sock_.sendTo(to, b4.data(), b4.size());   // 1033 bytes
                }
                if (f == Flow::Bandscope && count % 5 == 0) { Bytes b = p; b[3] = 0x04; sock_.sendTo(to, b.data(), b.size()); }
                if (f == Flow::Foreign && count % 5 == 0) { const Bytes r = reply(3); sock_.sendTo(to, r.data(), r.size()); }
                sock_.sendTo(to, p.data(), p.size());
                ++seq;
                ++count;
                sent_.store(count);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    void stopStream() {
        streamRun_.store(false);
        std::lock_guard<std::mutex> l(m_);
        if (stream_.joinable()) { stream_.join(); }
        streaming_.store(false);
    }

    std::uint8_t macLast_, board_;
    UdpSock sock_;
    std::atomic<bool> run_{false}, streamRun_{false}, streaming_{false};
    std::thread th_, stream_;
    std::mutex m_;
    std::atomic<int> sent_{0};
};

std::vector<std::complex<float>> readN(HpsdrSource& s, std::size_t n, int ms = 4000) {
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

// Samples come in runs of 256 on the test ramp; across a lost packet the ramp jumps by 256 more.
int rampBreaks(const std::vector<std::complex<float>>& v) {
    int breaks = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const float a = v[i].real() * 32768.0f, b = v[i - 1].real() * 32768.0f;
        const float step = a - b;
        if (!(std::fabs(step - 1.0f) < 0.01f || std::fabs(step + 19999.0f) < 0.01f)) { ++breaks; }
    }
    return breaks;
}

// --- tests -----------------------------------------------------------------------------------

void testCodecBytes() {
    // literal strings from the guide
    const auto d = hp::buildDiscoveryRequest();
    CHECK(d.size() == 63 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x02);
    CHECK(std::all_of(d.begin() + 3, d.end(), [](std::uint8_t b) { return b == 0; }));
    const auto s = hp::buildStartStop(0x01);
    CHECK(s.size() == 64 && s[0] == 0xEF && s[1] == 0xFE && s[2] == 0x04 && s[3] == 0x01);
    CHECK(std::all_of(s.begin() + 4, s.end(), [](std::uint8_t b) { return b == 0; }));
    CHECK(hp::buildStartStop(0x03)[3] == 3 && hp::buildStartStop(hp::kStopAll)[3] == 0);
    CHECK(hp::kStartIq == 1 && hp::kStartBandscope == 2 && hp::kEndpointIq == 6 && hp::kEndpointBandscope == 4 &&
          hp::kEndpointToDevice == 2 && hp::kDataPacketBytes == 1032 && hp::kDevicePort == 1024);
    // data packet envelope: EF FE 01 06 00 00 01 02 + 1024 bytes
    std::vector<std::uint8_t> frames(1024);
    for (std::size_t i = 0; i < frames.size(); ++i) { frames[i] = static_cast<std::uint8_t>(i); }
    const auto p = hp::buildDataPacket(6, 0x0102, frames.data());
    CHECK(p.size() == 1032 && p[0] == 0xEF && p[1] == 0xFE && p[2] == 1 && p[3] == 6 && p[4] == 0 && p[5] == 0 && p[6] == 1 && p[7] == 2);
    hp::DataPacket dp;
    CHECK(hp::parseDataPacket(p.data(), p.size(), dp) && dp.endpoint == 6 && dp.sequence == 0x0102 && dp.frames == p.data() + 8);
    CHECK(!hp::parseDataPacket(p.data(), 1031, dp) && !hp::parseDataPacket(p.data(), 1033, dp) && !hp::parseDataPacket(nullptr, 0, dp));
    auto bad = p;
    bad[0] = 0;
    CHECK(!hp::parseDataPacket(bad.data(), bad.size(), dp));
    bad = p; bad[2] = 2;
    CHECK(!hp::parseDataPacket(bad.data(), bad.size(), dp));
    bad = p; bad[3] = 5;
    CHECK(!hp::parseDataPacket(bad.data(), bad.size(), dp));
    // discovery reply: every field
    std::vector<std::uint8_t> r(60, 0);
    r[0] = 0xEF; r[1] = 0xFE; r[2] = 0x03;
    for (int i = 0; i < 6; ++i) { r[3 + i] = static_cast<std::uint8_t>(0xA0 + i); }
    r[9] = 31; r[10] = 6;
    hp::DiscoveryReply rep;
    CHECK(hp::parseDiscoveryReply(r.data(), r.size(), rep) && rep.sending && rep.codeVersion == 31 && rep.boardId == 6);
    CHECK(rep.macText() == "a0:a1:a2:a3:a4:a5");
    r[2] = 0x02;
    CHECK(hp::parseDiscoveryReply(r.data(), r.size(), rep) && !rep.sending);
    CHECK(hp::parseDiscoveryReply(r.data(), 11, rep));      // the 11 bytes the fields need
    CHECK(!hp::parseDiscoveryReply(r.data(), 10, rep));
    r[2] = 0x04;
    CHECK(!hp::parseDiscoveryReply(r.data(), 60, rep));      // not a discovery status
    r[2] = 0x02; r[1] = 0x00;
    CHECK(!hp::parseDiscoveryReply(r.data(), 60, rep));
    CHECK(std::string(hp::boardName(0)) == "Metis" && std::string(hp::boardName(1)) == "Hermes" &&
          std::string(hp::boardName(2)) == "Griffin" && std::string(hp::boardName(4)) == "Angelia" &&
          std::string(hp::boardName(5)) == "Orion" && std::string(hp::boardName(6)) == "Hermes_Lite" &&
          std::string(hp::boardName(3)) == "unknown board");
    // sequence accounting (the baseline is the first packet; a gap is lost, a step back is late)
    hp::SequenceTracker t;
    CHECK(t.note(6, 100) == 0 && t.note(6, 101) == 0);
    CHECK(t.note(6, 104) == 2 && t.lost() == 2);
    CHECK(t.note(6, 103) == 0 && t.late() == 1);
    CHECK(t.note(6, 105) == 0 && t.lost() == 2);
    CHECK(t.note(4, 7) == 0 && t.note(4, 8) == 0);  // end points are independent
    t.reset();
    CHECK(t.lost() == 0 && t.late() == 0 && t.note(6, 5) == 0);
    hp::SequenceTracker w;
    w.note(6, 0xFFFFFFFEu);
    CHECK(w.note(6, 0xFFFFFFFFu) == 0 && w.note(6, 0) == 0 && w.lost() == 0);  // wraps
}

void testDiscovery() {
    {
        FakeDevice dev;
        CHECK(dev.start());
        std::vector<HpsdrDevice> found;
        std::string err;
        CHECK(cascade::source::discoverHpsdr("127.0.0.1", dev.port(), std::chrono::milliseconds(400), found, err));
        CHECK(found.size() == 1 && found[0].ip == "127.0.0.1" && found[0].port == dev.port());
        CHECK(found[0].reply.boardId == 1 && found[0].reply.codeVersion == 32 && !found[0].reply.sending);
        CHECK(found[0].name().find("Hermes") == 0 && found[0].name().find("00:1c:c0:a2:13:66") != std::string::npos);
        dev.mode = Mode::DoubleReply;
        CHECK(cascade::source::discoverHpsdr("127.0.0.1", dev.port(), std::chrono::milliseconds(400), found, err) && found.size() == 1);
        for (const Mode m : {Mode::NoReply, Mode::Junk, Mode::ShortReply, Mode::BadStatus}) {
            dev.mode = m;
            CHECK(cascade::source::discoverHpsdr("127.0.0.1", dev.port(), std::chrono::milliseconds(300), found, err));
            CHECK(found.empty());  // refused replies are not devices
        }
    }
    std::vector<HpsdrDevice> found;
    std::string err;
    CHECK(!cascade::source::discoverHpsdr("no-such-host.invalid", 1024, std::chrono::milliseconds(100), found, err) && !err.empty());
}

void testOpenAndStream() {
    FakeDevice dev;
    CHECK(dev.start());
    HpsdrSource s;
    CHECK(std::string(s.driverKey()) == "hpsdr");
    CHECK(s.open(dev.args("timeout=600")));
    CHECK(s.isOpen() && s.device().reply.boardId == 1);
    CHECK(std::string(s.name()).find("HPSDR: Hermes") == 0);
    // no decoder: start refuses, and says why; the setters refuse likewise
    CHECK(!s.start() && std::string(s.lastError()).find("not known") != std::string::npos);
    CHECK(dev.starts.load() == 0);
    CHECK(!s.setCenterFrequencyHz(7.0e6) && !s.setSampleRateHz(192000.0));
    CHECK(std::string(s.lastError()).find("not available") != std::string::npos);
    CHECK(s.gains().empty() && s.antennas().empty() && !s.autoGainSupported());
    auto dec = std::make_shared<TestDecoder>();
    s.setFrameDecoder(dec);
    CHECK(s.sampleRateHz() == 192000.0 && s.supportedSampleRatesHz().size() == 1);
    std::vector<std::complex<float>> tmp(16);
    CHECK(s.read(tmp.data(), tmp.size()) == 0);
    CHECK(s.start() && s.running());
    const auto got = readN(s, 5000);
    CHECK(got.size() >= 5000);
    CHECK(rampBreaks(got) == 0);
    CHECK(dev.lastStartCommand.load() == 1 && dev.starts.load() == 1);
    CHECK(dev.firstSeqAfterStart.load() == 0);
    CHECK(s.packetsLost() == 0 && s.badPackets() == 0);
    s.stop();
    CHECK(!s.running());
    for (int i = 0; i < 100 && dev.stops.load() < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(dev.stops.load() == 1 && dev.lastStartCommand.load() == 0);
    // PC-to-device traffic is only ever discovery, start and stop
    CHECK(dev.badCommands.load() == 0);
    s.closeDevice();
    CHECK(!s.isOpen());
}

void testOpenRefusals() {
    {
        HpsdrSource s;
        CHECK(!s.open("hpsdr=127.0.0.1:0") && !s.open("hpsdr=127.0.0.1:70000") && !s.open("hpsdr=[::1") && !s.open("hpsdr=127.0.0.1:1,timeout=2"));
    }
    FakeDevice dev;
    CHECK(dev.start());
    for (const Mode m : {Mode::NoReply, Mode::Junk, Mode::ShortReply, Mode::BadStatus}) {
        dev.mode = m;
        HpsdrSource s;
        CHECK(!s.open(dev.args("timeout=300")) && !s.isOpen());
        CHECK(std::string(s.lastError()).find("no HPSDR radio answered") != std::string::npos);
    }
    dev.mode = Mode::Normal;
    HpsdrSource s;
    CHECK(!s.open(dev.args("timeout=300,mac=de:ad:be:ef:00:01")));
    CHECK(std::string(s.lastError()).find("MAC") != std::string::npos);
    CHECK(s.open(dev.args("timeout=300,mac=00:1C:C0:A2:13:66")));  // case-insensitive
    s.closeDevice();
}

void testFlows() {
    struct Case { Flow f; const char* what; };
    const Case cases[] = {{Flow::DropOne, "a lost packet"}, {Flow::RepeatOne, "a repeated packet"}, {Flow::BadPackets, "malformed packets"},
                          {Flow::Bandscope, "bandscope packets"}, {Flow::Foreign, "a discovery reply mid-stream"}};
    for (const Case& c : cases) {
        FakeDevice dev;
        CHECK(dev.start());
        dev.flow = c.f;
        dev.flowAt = 25;
        HpsdrSource s;
        CHECK(s.open(dev.args("timeout=600")));
        s.setFrameDecoder(std::make_shared<TestDecoder>());
        CHECK(s.start());
        const auto got = readN(s, 40 * 256, 6000);
        CHECK(got.size() >= 40 * 256);
        CHECK(!s.faulted());  // none of these is fatal
        switch (c.f) {
            case Flow::DropOne:
                CHECK(s.packetsLost() == 1);
                CHECK(rampBreaks(got) == 1);  // the ramp jumps once, at the lost packet
                break;
            case Flow::RepeatOne:
                CHECK(s.packetsLate() == 1 && s.packetsLost() == 0);
                break;
            case Flow::BadPackets: CHECK(s.badPackets() >= 4 && rampBreaks(got) == 0); break;
            case Flow::Bandscope: CHECK(rampBreaks(got) == 0 && s.badPackets() == 0); break;
            case Flow::Foreign: CHECK(s.badPackets() >= 1 && rampBreaks(got) == 0); break;
            default: break;
        }
        s.closeDevice();
    }
    {  // a decoder that rejects the frames: counted, not fatal
        FakeDevice dev;
        CHECK(dev.start());
        HpsdrSource s;
        CHECK(s.open(dev.args()));
        auto dec = std::make_shared<TestDecoder>();
        dec->reject = true;
        s.setFrameDecoder(dec);
        CHECK(s.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(s.badPackets() >= 5 && !s.faulted());
        s.closeDevice();
    }
    {  // a radio that goes silent while streaming is a fault, bounded
        FakeDevice dev;
        CHECK(dev.start());
        dev.flow = Flow::Stall;
        dev.flowAt = 10;
        HpsdrSource s;
        CHECK(s.open(dev.args("timeout=400")));
        s.setFrameDecoder(std::make_shared<TestDecoder>());
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(512);
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) { s.read(tmp.data(), tmp.size()); }
        CHECK(s.faulted() && std::string(s.lastError()).find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testCyclesAndShutdown() {
    FakeDevice dev;
    CHECK(dev.start());
    const unsigned long long before = NetSampleSource::readersAbandoned();
    HpsdrSource s;
    CHECK(s.open(dev.args()));
    s.setFrameDecoder(std::make_shared<TestDecoder>());
    for (int i = 0; i < 50; ++i) {
        const bool started = s.start();
        if (!started) { std::fprintf(stderr, "cycle %d start failed: %s | faulted=%d", i, s.lastError(), s.faulted() ? 1 : 0); std::fputc(10, stderr); }
        CHECK(started);
        std::vector<std::complex<float>> tmp(64);
        s.read(tmp.data(), tmp.size());
        s.stop();
    }
    CHECK(s.start());
    const auto got = readN(s, 1000);
    CHECK(got.size() >= 1000 && rampBreaks(got) == 0);  // nothing from before a stop is delivered after the restart
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "hpsdr close while streaming: %lld ms", static_cast<long long>(ms));
    std::fputc(10, stderr);
    CHECK(ms < 1500);
    for (int i = 0; i < 50; ++i) {
        HpsdrSource t;
        const bool opened = t.open(dev.args("timeout=500"));
        if (!opened) { std::fprintf(stderr, "cycle %d open failed: %s", i, t.lastError()); std::fputc(10, stderr); }
        CHECK(opened);
        if (i % 5 == 0) { t.setFrameDecoder(std::make_shared<TestDecoder>()); CHECK(t.start()); }
        t.closeDevice();
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    {
        HpsdrSource u;  // destroyed while streaming
        CHECK(u.open(dev.args()));
        u.setFrameDecoder(std::make_shared<TestDecoder>());
        CHECK(u.start());
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    HpsdrSource c;
    CHECK(!c.start());
    c.closeDevice();
    c.stop();
}

void testFuzz() {
    Rng rng(0xFACADEu);
    int parsedData = 0, parsedDisc = 0;
    for (int i = 0; i < 10000; ++i) {
        Bytes in(rng.next() % 1100);
        for (auto& b : in) { b = static_cast<std::uint8_t>(rng.next()); }
        const int style = static_cast<int>(rng.next() % 3);
        if (style > 0 && in.size() > 12) { in[0] = 0xEF; in[1] = 0xFE; in[2] = style == 1 ? 0x01 : static_cast<std::uint8_t>(2 + rng.next() % 2); in[3] = static_cast<std::uint8_t>(rng.next() % 8); }
        if (style == 1 && rng.next() % 2) { in.resize(1032, 0xEF); in[0] = 0xEF; in[1] = 0xFE; in[2] = 0x01; in[3] = 6; }
        hp::DataPacket p;
        if (hp::parseDataPacket(in.data(), in.size(), p)) { ++parsedData; CHECK(p.frames == in.data() + 8 && in.size() == 1032); }
        hp::DiscoveryReply r;
        if (hp::parseDiscoveryReply(in.data(), in.size(), r)) { ++parsedDisc; }
        hp::SequenceTracker t;
        t.note(static_cast<std::uint8_t>(in.empty() ? 0 : in[0]), rng.next());
        t.note(6, rng.next());
    }
    CHECK(parsedData > 100 && parsedDisc > 100);
}

}  // namespace

#define STEP(call) do { std::fputs(#call, stderr); std::fputc(10, stderr); std::fflush(stderr); call; } while (0)

int main() {
    STEP(testCodecBytes());
    STEP(testDiscovery());
    STEP(testOpenAndStream());
    STEP(testOpenRefusals());
    STEP(testFlows());
    STEP(testCyclesAndShutdown());
    STEP(testFuzz());
    return testSummary("test_hpsdr_source");
}
