// Tests for net/iq_out.hpp: sample conversion against hand-computed values, the header bytes, the TCP and UDP streams against an
// independently computed expectation, counted drops, config refusals, stop while sending, and a MEASUREMENT of the CPU cost at 2.4 MS/s.
// Loopback sockets only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/iq_out.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;
namespace sock = cascade::net::sock;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes convert(IqFormat f, const std::vector<float>& iq) {
    Bytes out(iq.size() / 2 * iqBytesPerSample(f), 0xEE);
    iqConvert(f, iq.data(), iq.size() / 2, out.data());
    return out;
}

std::int16_t s16At(const Bytes& b, std::size_t i) { return static_cast<std::int16_t>(b[2 * i] | (b[2 * i + 1] << 8)); }

void testConversions() {
    // 8-bit unsigned: u = round(x * 127.5 + 127.5)
    const std::vector<float> v = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 1e30f, -1e30f, 0.0078431373f /* 1/127.5 */};
    const Bytes u = convert(IqFormat::U8, v);
    CHECK(u.size() == 10);
    const std::uint8_t want[10] = {128, 255, 0, 191, 64, 255, 0, 255, 0, 129};
    for (std::size_t i = 0; i < 10; ++i) { CHECK(u[i] == want[i]); }
    // non-finite reads as zero in every format
    const std::vector<float> nf = {std::nanf(""), INFINITY, -INFINITY, -std::nanf("")};
    const Bytes un = convert(IqFormat::U8, nf);
    for (std::uint8_t b : un) { CHECK(b == 128); }
    const Bytes sn = convert(IqFormat::S16, nf);
    for (std::size_t i = 0; i < 4; ++i) { CHECK(s16At(sn, i) == 0); }
    const Bytes fn = convert(IqFormat::F32, nf);
    for (std::uint8_t b : fn) { CHECK(b == 0); }
    // 16-bit signed little-endian: s = round(x * 32767), clamped to +-32767
    const std::vector<float> w = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 3.0f, -3.0f, 1.0f / 32767.0f};
    const Bytes s = convert(IqFormat::S16, w);
    CHECK(s.size() == 16);
    const int ws[8] = {0, 32767, -32767, 16384, -16384, 32767, -32767, 1};
    for (std::size_t i = 0; i < 8; ++i) { CHECK(s16At(s, i) == ws[i]); }
    CHECK(s[2] == 0xFF && s[3] == 0x7F);  // 32767 = 0x7FFF, low byte first
    // 32-bit float: the IEEE bytes, little-endian
    const std::vector<float> fl = {1.0f, -2.5f, 0.0f, 3.4e38f};
    const Bytes f = convert(IqFormat::F32, fl);
    CHECK(f.size() == 16);
    CHECK((f == Bytes{0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x20, 0xC0, 0, 0, 0, 0, 0xCA, 0xBF, 0x7F, 0x7F}) ||
          (std::memcmp(f.data() + 12, &fl[3], 4) == 0));
    CHECK(f[0] == 0x00 && f[1] == 0x00 && f[2] == 0x80 && f[3] == 0x3F);  // 1.0f = 0x3F800000
    CHECK(f[4] == 0x00 && f[5] == 0x00 && f[6] == 0x20 && f[7] == 0xC0);  // -2.5f = 0xC0200000
    // the interleave order is I then Q, and every byte of the output is written (no stale bytes)
    CHECK(convert(IqFormat::U8, {1.0f, -1.0f}) == (Bytes{255, 0}));
    CHECK(convert(IqFormat::S16, {1.0f, -1.0f}) == (Bytes{0xFF, 0x7F, 0x01, 0x80}));
    CHECK(iqBytesPerSample(IqFormat::U8) == 2 && iqBytesPerSample(IqFormat::S16) == 4 && iqBytesPerSample(IqFormat::F32) == 8);
    CHECK(iqBytesPerSample(static_cast<IqFormat>(9)) == 0);
    // empty input writes nothing
    std::uint8_t guard[4] = {1, 2, 3, 4};
    iqConvert(IqFormat::S16, nullptr, 0, guard);
    CHECK(guard[0] == 1 && guard[3] == 4);
    // a full-scale sweep: monotone and within one step of x for the 8-bit format, 1/65534 for 16
    float worst8 = 0, worst16 = 0;
    for (int i = -1000; i <= 1000; ++i) {
        const float x = static_cast<float>(i) / 1000.0f;
        const Bytes a = convert(IqFormat::U8, {x, 0.0f});
        const Bytes b = convert(IqFormat::S16, {x, 0.0f});
        worst8 = std::max(worst8, std::fabs((static_cast<float>(a[0]) - 127.5f) / 127.5f - x));
        worst16 = std::max(worst16, std::fabs(static_cast<float>(s16At(b, 0)) / 32767.0f - x));
    }
    std::printf("quantisation error over -1..1: 8-bit %.5f (half a step = %.5f), 16-bit %.7f\n", worst8, 0.5 / 127.5, worst16);
    CHECK(worst8 <= 0.5f / 127.5f + 1e-5f);
    CHECK(worst16 <= 0.5f / 32767.0f + 1e-6f);
}

void testHeader() {
    IqHeader h;
    h.format = IqFormat::S16;
    h.sampleRateHz = 2400000;
    h.centreHz = 100.1e6;
    h.sequence = 0x01020304;
    std::uint8_t b[kIqHeaderBytes];
    std::memset(b, 0xEE, sizeof(b));
    iqWriteHeader(h, b);
    CHECK(std::memcmp(b, "FXIQ", 4) == 0);
    CHECK(b[4] == 1 && b[5] == 0);                          // version
    CHECK(b[6] == 2 && b[7] == 0);                          // format s16
    CHECK(b[8] == 0x00 && b[9] == 0x9F && b[10] == 0x24 && b[11] == 0x00);  // 2400000 = 0x00249F00, low byte first
    // 100.1e6 as an IEEE double = 0x4197E4A5D3333333? computed independently below
    double c = 100.1e6;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &c, 8);
    for (int i = 0; i < 8; ++i) { CHECK(b[12 + i] == static_cast<std::uint8_t>(bits >> (8 * i))); }
    CHECK(b[20] == 4 && b[21] == 3 && b[22] == 2 && b[23] == 1);  // sequence, little-endian
    CHECK(b[24] == 0 && b[25] == 0 && b[26] == 0 && b[27] == 0);  // flags
    IqHeader r;
    CHECK(iqReadHeader(b, sizeof(b), r));
    CHECK(r.format == IqFormat::S16 && r.sampleRateHz == 2400000 && r.centreHz == 100.1e6 && r.sequence == 0x01020304);
    // hostile headers
    CHECK(!iqReadHeader(b, 27, r));
    CHECK(!iqReadHeader(b, 0, r));
    std::uint8_t bad[kIqHeaderBytes];
    std::memcpy(bad, b, sizeof(b));
    bad[0] = 'X';
    CHECK(!iqReadHeader(bad, sizeof(bad), r));
    std::memcpy(bad, b, sizeof(b));
    bad[4] = 2;
    CHECK(!iqReadHeader(bad, sizeof(bad), r));
    for (int f : {0, 4, 255}) {
        std::memcpy(bad, b, sizeof(b));
        bad[6] = static_cast<std::uint8_t>(f);
        CHECK(!iqReadHeader(bad, sizeof(bad), r));
    }
    std::memcpy(bad, b, sizeof(b));
    bad[24] = 1;
    CHECK(!iqReadHeader(bad, sizeof(bad), r));
    for (IqFormat f : {IqFormat::U8, IqFormat::S16, IqFormat::F32}) {
        h.format = f;
        iqWriteHeader(h, b);
        CHECK(iqReadHeader(b, sizeof(b), r) && r.format == f);
    }
}

std::vector<float> noise(std::size_t nComplex, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.1f, 1.1f);  // beyond full scale on purpose: the clamp is part of the stream
    std::vector<float> v(nComplex * 2);
    for (float& x : v) { x = d(rng); }
    return v;
}

void testConfigRefusals() {
    std::string err;
    {
        IqOutConfig c;
        c.tcp.port = 0;
        c.header = true;  // no sample rate
        CHECK(!IqOut(c).start(err) && err.find("sample rate") != std::string::npos);
        c.sampleRateHz = 48000;
        c.centreHz = std::nan("");
        CHECK(!IqOut(c).start(err));
        c.centreHz = 1e6;
        IqOut ok(c);
        CHECK(ok.start(err));
    }
    {
        IqOutConfig c;
        c.tcp.port = 0;
        c.format = static_cast<IqFormat>(7);
        CHECK(!IqOut(c).start(err) && err.find("format") != std::string::npos);
    }
    {
        IqOutConfig c;
        c.tcp.port = 0;
        c.chunkSamples = 1 << 20;  // far over the per-client queue
        CHECK(!IqOut(c).start(err) && err.find("chunk") != std::string::npos);
    }
    {
        IqOutConfig c;
        c.udp = true;
        c.udpCfg.port = 4000;
        c.format = IqFormat::F32;
        c.chunkSamples = 400;  // 3200 bytes > 1400
        CHECK(!IqOut(c).start(err) && err.find("datagram") != std::string::npos);
        c.chunkSamples = 0;  // automatic: as many as fit
        IqOut a(c);
        CHECK(a.chunkSamples() == 175);  // 1400 / 8
        c.header = true;
        c.sampleRateHz = 1000;
        IqOut b(c);
        CHECK(b.chunkSamples() == (1400 - kIqHeaderBytes) / 8);  // 171
        c.format = IqFormat::U8;
        IqOut u(c);
        CHECK(u.chunkSamples() == (1400 - kIqHeaderBytes) / 2);  // 686
        // the all-interfaces refusal of the UDP sink and the TCP fan-out still applies through this wrapper
        IqOutConfig t;
        t.tcp.bindAddress = "0.0.0.0";
        CHECK(!IqOut(t).start(err) && err.find("every network interface") != std::string::npos);
        IqOutConfig d;
        d.udp = true;
        d.udpCfg.port = 4000;
        d.udpCfg.destAddress = "255.255.255.255";
        CHECK(!IqOut(d).start(err));
    }
    {
        IqOut n(IqOutConfig{});
        CHECK(n.submit(nullptr, 5) == 0);
        float x[2] = {0, 0};
        CHECK(n.submit(x, 0) == 0);
        CHECK(n.submit(x, 1) == 0);  // not running: refused, counted as dropped
        CHECK(n.stats().samplesDropped == 1);
    }
}

// A TCP stream, headerless and with the header, in each format: the bytes a client gets are exactly the independent conversion.
void testTcpStreams() {
    for (const bool withHeader : {false, true}) {
        for (const IqFormat f : {IqFormat::U8, IqFormat::S16, IqFormat::F32}) {
            IqOutConfig c;
            c.format = f;
            c.header = withHeader;
            c.sampleRateHz = 2400000;
            c.centreHz = 1090e6;
            c.tcp.port = 0;
            c.chunkSamples = 1000;
            IqOut out(c);
            std::string err;
            CHECK(out.start(err));
            Client k;
            CHECK(k.connect(out.tcpPort()));
            for (int spins = 0; out.stats().tcp.clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            const std::vector<float> iq = noise(4321, 7 + static_cast<unsigned>(f));  // not a multiple of the chunk: 4 full chunks and a part
            CHECK(out.submit(iq.data(), 4321) == 4321);
            const Bytes want = convert(f, iq);
            const std::size_t hdr = withHeader ? kIqHeaderBytes : 0;
            CHECK(k.waitBytes(hdr + want.size(), 3000));
            CHECK(k.got.size() == hdr + want.size());
            if (withHeader && k.got.size() >= kIqHeaderBytes) {
                IqHeader h;
                CHECK(iqReadHeader(k.got.data(), k.got.size(), h));
                CHECK(h.format == f && h.sampleRateHz == 2400000 && h.centreHz == 1090e6 && h.sequence == 0);
            }
            CHECK(Bytes(k.got.begin() + static_cast<std::ptrdiff_t>(hdr), k.got.end()) == want);
            const IqStats s = out.stats();
            CHECK(s.samplesIn == 4321 && s.samplesQueued == 4321 && s.samplesDropped == 0);
            // a second client connecting later gets the header (when on) and then only new samples
            Client late;
            CHECK(late.connect(out.tcpPort()));
            for (int spins = 0; out.stats().tcp.clients < 2 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            late.pump(100);
            CHECK(late.got.size() == hdr);
        }
    }
}

// UDP: every datagram is header (when on) + whole samples; sequence numbers are consecutive; the payload is the independent conversion.
void testUdpStreams() {
    for (const bool withHeader : {false, true}) {
        for (const IqFormat f : {IqFormat::U8, IqFormat::S16, IqFormat::F32}) {
            sock::Handle rx = sock::kInvalid;
            std::string err;
            std::uint32_t ip = 0;
            sock::parseIpv4("127.0.0.1", ip);
            rx = sock::bindUdp(ip, 0, err);
            CHECK(rx != sock::kInvalid);
            IqOutConfig c;
            c.udp = true;
            c.format = f;
            c.header = withHeader;
            c.sampleRateHz = 250000;
            c.centreHz = 162.025e6;
            c.udpCfg.port = sock::boundPort(rx);
            c.udpCfg.socketSendBuffer = 1 << 20;
            IqOut out(c);
            CHECK(out.start(err));
            const std::size_t chunk = out.chunkSamples();
            const std::size_t total = chunk * 5 + 17;
            const std::vector<float> iq = noise(total, 100 + static_cast<unsigned>(f));
            // receive in a thread while the producer submits (loopback UDP drops when the receiver's buffer is full)
            std::vector<Bytes> got;
            std::atomic<bool> done{false};
            std::thread rcv([&] {
                std::vector<std::uint8_t> buf(70000);
                while (!done.load()) {
                    sock::PollItem it;
                    it.s = rx;
                    it.want = sock::kReadable;
                    if (sock::pollMany(&it, 1, 20) <= 0) { continue; }
                    const long r = sock::recvSome(rx, buf.data(), buf.size());
                    if (r > 0) { got.emplace_back(buf.begin(), buf.begin() + r); }
                }
            });
            CHECK(out.submit(iq.data(), total) == total);
            for (int spins = 0; got.size() < 6 && spins < 300; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            done.store(true);
            rcv.join();
            sock::closeHandle(rx);
            CHECK(got.size() == 6);
            const Bytes want = convert(f, iq);
            const std::size_t hdr = withHeader ? kIqHeaderBytes : 0;
            const std::size_t bps = iqBytesPerSample(f);
            Bytes joined;
            for (std::size_t i = 0; i < got.size(); ++i) {
                CHECK(got[i].size() <= 1400);
                CHECK(got[i].size() >= hdr);
                if (withHeader) {
                    IqHeader h;
                    CHECK(iqReadHeader(got[i].data(), got[i].size(), h));
                    CHECK(h.sequence == i && h.format == f && h.sampleRateHz == 250000 && h.centreHz == 162.025e6);
                }
                CHECK((got[i].size() - hdr) % bps == 0);  // whole samples only
                joined.insert(joined.end(), got[i].begin() + static_cast<std::ptrdiff_t>(hdr), got[i].end());
            }
            CHECK(joined == want);
            CHECK(out.stats().samplesIn == total && out.stats().samplesDropped == 0);
        }
    }
}

// Slower network than radio: drops are counted, the accounting identity holds, the producer is not slowed.
void testDropsAreCounted() {
    // TCP: a client that never reads, a small ring and queue; flat-out producer
    IqOutConfig c;
    c.tcp.port = 0;
    c.tcp.ringBytes = 64 * 1024;
    c.tcp.clientQueueBytes = 32 * 1024;
    c.tcp.socketSendBuffer = 4096;
    c.chunkSamples = 1000;
    IqOut out(c);
    std::string err;
    CHECK(out.start(err));
    Client lazy;
    CHECK(lazy.connect(out.tcpPort()));
    sock::setBuffers(lazy.s, 4096, 0);
    for (int spins = 0; out.stats().tcp.clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const std::vector<float> iq = noise(100000, 5);
    std::uint64_t submitted = 0;
    double worstMs = 0;
    for (int i = 0; i < 300; ++i) {
        const auto t0 = nettest::Clock::now();
        out.submit(iq.data(), 100000);
        worstMs = std::max(worstMs, msSince(t0));
        submitted += 100000;
    }
    const IqStats s = out.stats();
    CHECK(s.samplesIn == submitted);
    CHECK(s.samplesIn == s.samplesQueued + s.samplesDropped);  // every sample is either queued or counted as dropped
    CHECK(s.samplesDropped > 0 && s.chunksDropped > 0);
    CHECK(s.samplesDropped == s.chunksDropped * 1000);
    std::printf("tcp drops: %llu of %llu samples dropped (%.1f %%), slowest submit() of 100000 samples %.2f ms\n",
                static_cast<unsigned long long>(s.samplesDropped), static_cast<unsigned long long>(s.samplesIn),
                100.0 * static_cast<double>(s.samplesDropped) / static_cast<double>(s.samplesIn), worstMs);
    // the lazy client is eventually dropped by the fan-out, and that is visible too
    for (int spins = 0; out.stats().tcp.droppedSlow + out.stats().tcp.droppedStalled + out.stats().tcp.closedByPeer == 0 && spins < 300; ++spins) {
        out.submit(iq.data(), 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(out.stats().tcp.droppedSlow + out.stats().tcp.droppedStalled >= 1);

    // UDP: ring far smaller than the burst
    sock::Handle rx;
    std::uint32_t ip = 0;
    sock::parseIpv4("127.0.0.1", ip);
    rx = sock::bindUdp(ip, 0, err);
    CHECK(rx != sock::kInvalid);
    IqOutConfig u;
    u.udp = true;
    u.header = true;
    u.sampleRateHz = 1000;
    u.udpCfg.port = sock::boundPort(rx);
    u.udpCfg.ringBytes = 16 * 1024;
    IqOut uo(u);
    CHECK(uo.start(err));
    for (int i = 0; i < 100; ++i) { uo.submit(iq.data(), 100000); }
    const IqStats us = uo.stats();
    CHECK(us.samplesIn == 10000000);
    CHECK(us.samplesIn == us.samplesQueued + us.samplesDropped);
    CHECK(us.samplesDropped > 0);
    CHECK(us.udp.ringDropped == us.chunksDropped);
    std::printf("udp drops: %llu of %llu samples dropped into an 16 KiB ring\n", static_cast<unsigned long long>(us.samplesDropped),
                static_cast<unsigned long long>(us.samplesIn));
    uo.stop();
    sock::closeHandle(rx);
}

void testStopWhileSending() {
    double worst = 0;
    for (int round = 0; round < 4; ++round) {
        IqOutConfig c;
        c.tcp.port = 0;
        IqOut out(c);
        std::string err;
        CHECK(out.start(err));
        Client k;
        CHECK(k.connect(out.tcpPort()));
        std::atomic<bool> stopReading{false};
        std::thread reader([&] {
            while (!stopReading.load() && !k.eof) { k.got.clear(); k.pump(10); }
        });
        std::atomic<bool> stop{false};
        const std::vector<float> iq = noise(50000, 3);
        std::thread prod([&] {
            while (!stop.load()) { out.submit(iq.data(), 50000); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto t0 = nettest::Clock::now();
        out.stop();
        worst = std::max(worst, msSince(t0));
        stop.store(true);
        prod.join();
        stopReading.store(true);
        reader.join();
        CHECK(out.stats().samplesIn > 0);
    }
    std::printf("iq stop() while the producer submits flat out: worst of 4 = %.1f ms\n", worst);
    CHECK(worst < 1000.0);
}

// ---- the measurement ---------------------------------------------------------------------------------------------------------------

// Producer-thread cost of conversion alone, per format, at 2.4 MS/s: seconds of CPU per second of signal.
void measureConversion() {
    const std::size_t block = 4096;
    const std::vector<float> iq = noise(block, 1);
    std::vector<std::uint8_t> out(block * 8);
    for (IqFormat f : {IqFormat::U8, IqFormat::S16, IqFormat::F32}) {
        const std::size_t blocks = 2400000 / block * 4;  // four seconds of signal at 2.4 MS/s
        const auto t0 = nettest::Clock::now();
        std::uint8_t sink = 0;
        for (std::size_t i = 0; i < blocks; ++i) {
            iqConvert(f, iq.data(), block, out.data());
            sink = static_cast<std::uint8_t>(sink + out[i % out.size()]);
        }
        const double ms = msSince(t0);
        const double signalSeconds = static_cast<double>(blocks * block) / 2.4e6;
        std::printf("MEASURE conversion only, %s: %.1f ms for %.2f s of signal at 2.4 MS/s = %.2f %% of one core (sink %u)\n",
                    f == IqFormat::U8 ? "u8 " : (f == IqFormat::S16 ? "s16" : "f32"), ms, signalSeconds, 100.0 * ms / 1000.0 / signalSeconds, sink);
        CHECK(ms / 1000.0 / signalSeconds < 0.25);  // generous pin: well under a quarter of a core
    }
}

// The whole path in real time: a producer paced at 2.4 MS/s in 20 ms blocks, TCP with one client reading in this process, then UDP with a
// receiver in this process. CPU is the PROCESS's (producer + IO thread + the in-process reader), over 4 s of wall time.
void measureRealTime() {
    const std::size_t block = 48000;  // 20 ms at 2.4 MS/s
    const std::vector<float> iq = noise(block, 2);
    for (const bool udp : {false, true}) {
        for (IqFormat f : {IqFormat::U8, IqFormat::S16, IqFormat::F32}) {
            IqOutConfig c;
            c.format = f;
            c.udp = udp;
            c.tcp.port = 0;
            sock::Handle rx = sock::kInvalid;
            std::string err;
            std::uint32_t ip = 0;
            sock::parseIpv4("127.0.0.1", ip);
            if (udp) {
                rx = sock::bindUdp(ip, 0, err);
                c.udpCfg.port = sock::boundPort(rx);
                c.udpCfg.socketSendBuffer = 4 << 20;
                sock::setBuffers(rx, 8 << 20, 0);
            }
            IqOut out(c);
            CHECK(out.start(err));
            Client k;
            std::atomic<bool> stopRead{false};
            std::atomic<std::uint64_t> received{0};
            std::thread reader;
            if (!udp) {
                CHECK(k.connect(out.tcpPort()));
                reader = std::thread([&] {
                    std::uint8_t buf[1 << 16];
                    while (!stopRead.load()) {
                        sock::PollItem it;
                        it.s = k.s;
                        it.want = sock::kReadable;
                        if (sock::pollMany(&it, 1, 10) <= 0) { continue; }
                        const long r = sock::recvSome(k.s, buf, sizeof(buf));
                        if (r > 0) { received += static_cast<std::uint64_t>(r); } else if (r == 0) { break; }
                    }
                });
            } else {
                reader = std::thread([&] {
                    std::vector<std::uint8_t> buf(70000);
                    while (!stopRead.load()) {
                        sock::PollItem it;
                        it.s = rx;
                        it.want = sock::kReadable;
                        if (sock::pollMany(&it, 1, 10) <= 0) { continue; }
                        const long r = sock::recvSome(rx, buf.data(), buf.size());
                        if (r > 0) { received += static_cast<std::uint64_t>(r); }
                    }
                });
            }
            const int blocks = 200;  // 4 s
            const double cpu0 = nettest::processCpuSeconds();
            const auto t0 = nettest::Clock::now();
            double submitMs = 0;
            for (int i = 0; i < blocks; ++i) {
                const auto a = nettest::Clock::now();
                out.submit(iq.data(), block);
                submitMs += msSince(a);
                // pace to real time
                while (msSince(t0) < (i + 1) * 20.0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
            const double wall = msSince(t0) / 1000.0;
            const double cpu = nettest::processCpuSeconds() - cpu0;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const IqStats s = out.stats();
            stopRead.store(true);
            reader.join();
            out.stop();
            if (udp) { sock::closeHandle(rx); }
            std::printf("MEASURE real time %s %s: process CPU %.3f s in %.2f s wall = %.1f %% of one core; submit() calls %.1f ms total = %.2f %% of one core; dropped %llu of %llu samples; %.2f MB received\n",
                        udp ? "udp" : "tcp", f == IqFormat::U8 ? "u8 " : (f == IqFormat::S16 ? "s16" : "f32"), cpu, wall, 100.0 * cpu / wall, submitMs,
                        100.0 * submitMs / 1000.0 / wall, static_cast<unsigned long long>(s.samplesDropped), static_cast<unsigned long long>(s.samplesIn),
                        static_cast<double>(received.load()) / 1e6);
            CHECK(cpu / wall < 0.60);  // generous pin
            CHECK(s.samplesDropped * 100 < s.samplesIn * 5 || udp);  // TCP to a reading client at 2.4 MS/s: under 5 % dropped
        }
    }
}

}  // namespace

int main() {
    testConversions();
    testHeader();
    testConfigRefusals();
    testTcpStreams();
    testUdpStreams();
    testDropsAreCounted();
    testStopWhileSending();
    measureConversion();
    measureRealTime();
    return testSummary("test_iq_out");
}
