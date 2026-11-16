// Tests for net/tcp_audio.hpp (screen lane S4b, G3-22: audio over TCP). Every test talks to the server over a REAL loopback socket and reads
// from a stand-in audio source; nothing here reaches an outside host or a real radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tcp_audio.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;

namespace {

// A stand-in radio: every call hands back `chunk` frames of a counting ramp whose value says which frame it is (frame n has the value (n % 1000) / 1000).
struct Source {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<bool> gone{false};
    std::atomic<int> calls{0};
    UdpAudioSender::Reader reader(std::size_t chunk = 480) {
        return [this, chunk](std::uint64_t& cursor, float* out, std::size_t cap) -> int {
            ++calls;
            if (gone.load()) { return -1; }
            const std::size_t n = std::min(chunk, cap);
            for (std::size_t i = 0; i < n; ++i) { out[i] = static_cast<float>((frames.fetch_add(1) % 1000) / 1000.0); }
            cursor += n;
            return static_cast<int>(n);
        };
    }
};

TcpAudioConfig cfg0() {
    TcpAudioConfig c;
    c.port = 0;   // the OS picks
    return c;
}

std::int16_t sampleAt(const std::vector<std::uint8_t>& b, std::size_t i) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[2 * i]) | (static_cast<std::uint16_t>(b[2 * i + 1]) << 8));
}

void testRefusals() {
    Source src;
    std::string err;
    {
        TcpAudioServer s;
        TcpAudioConfig c = cfg0();
        c.port = 70000;
        CHECK(!s.start(c, src.reader(), err) && !err.empty() && !s.running());
    }
    {
        TcpAudioServer s;
        TcpAudioConfig c = cfg0();
        c.packetFrames = 100;
        CHECK(!s.start(c, src.reader(), err) && err.find("480 or 960") != std::string::npos);
    }
    {
        TcpAudioServer s;
        CHECK(!s.start(cfg0(), UdpAudioSender::Reader{}, err) && !err.empty());
    }
    {
        // every address is a deliberate act: refused unless asked for, and the sentence says why
        TcpAudioServer s;
        TcpAudioConfig c = cfg0();
        c.bindAddress = "0.0.0.0";
        CHECK(!s.start(c, src.reader(), err) && err.find("every network interface") != std::string::npos && !s.running());
    }
    {
        // a name is not an address (no lookup may stall a start)
        TcpAudioServer s;
        TcpAudioConfig c = cfg0();
        c.bindAddress = "example.org";
        CHECK(!s.start(c, src.reader(), err) && !s.running());
    }
    {
        TcpAudioServer s;
        TcpAudioConfig c = cfg0();
        c.maxClients = 0;
        CHECK(!s.start(c, src.reader(), err) && !s.running());
    }
    {
        // a second start without a stop is refused, and the first keeps running
        TcpAudioServer s;
        CHECK(s.start(cfg0(), src.reader(), err) && s.running());
        CHECK(!s.start(cfg0(), src.reader(), err) && s.running());
        s.stop();
        CHECK(!s.running());
        s.stop();   // idempotent
    }
}

void testStreamIsTheUdpFormat() {
    Source src;
    std::string err;
    TcpAudioServer s;
    CHECK(s.start(cfg0(), src.reader(), err));
    CHECK(s.port() > 0);
    Client c;
    CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
    CHECK(c.waitBytes(4000, 3000));
    // whole 16-bit samples, a packet at a time; the ramp rises by 1/1000 of full scale per frame (32767/1000 = 32.767 per step), so consecutive samples
    // differ by 32 or 33 until the ramp wraps; and the first byte of the stream is the start of a sample (nothing is sent before the audio)
    CHECK(c.got.size() >= 4000);
    int smooth = 0, checked = 0;
    for (std::size_t i = 1; i + 1 < c.got.size() / 2 && i < 1500; ++i) {
        const int d = sampleAt(c.got, i) - sampleAt(c.got, i - 1);
        ++checked;
        if (d == 32 || d == 33 || d == 32 - 32767 || d == 33 - 32767 || (d < -32000)) { ++smooth; }
    }
    CHECK(checked > 1000 && smooth == checked);
    CHECK(s.stats().accepted == 1 && s.stats().clients == 1);
    s.stop();
    CHECK(c.waitEof(3000));   // the client sees the end of stream
}

void testOffByDefaultNothingBeforeStart() {
    // a server that was never started has no port, no stats, no thread
    TcpAudioServer s;
    CHECK(!s.running() && s.port() == 0 && s.stats().accepted == 0);
    Source src;
    CHECK(src.calls.load() == 0);
}

void testClientThatSendsGarbageIsIgnored() {
    Source src;
    std::string err;
    TcpAudioServer s;
    CHECK(s.start(cfg0(), src.reader(), err));
    Client bad, good;
    CHECK(bad.connect(static_cast<std::uint16_t>(s.port())) && good.connect(static_cast<std::uint16_t>(s.port())));
    std::vector<std::uint8_t> junk(256 * 1024);
    for (std::size_t i = 0; i < junk.size(); ++i) { junk[i] = static_cast<std::uint8_t>(i * 131u + 7u); }
    for (int k = 0; k < 8; ++k) { (void)bad.sendBytes(junk.data(), junk.size()); }
    CHECK(good.waitBytes(8000, 3000));
    CHECK(s.running());
    const FanoutStats st = s.stats();
    CHECK(st.bytesIgnored > 0);   // read and thrown away
    s.stop();
}

void testClientThatNeverReadsIsDropped() {
    Source src;
    std::string err;
    TcpAudioServer s;
    TcpAudioConfig c = cfg0();
    c.maxClients = 2;
    CHECK(s.start(c, src.reader(4800), err));
    Client idle, good;   // `idle` connects and never reads a byte
    CHECK(idle.connect(static_cast<std::uint16_t>(s.port())) && good.connect(static_cast<std::uint16_t>(s.port())));
    cascade::net::sock::setBuffers(idle.s, 4096, 0);   // (the stand-in client keeps its own receive buffer small too)
    const auto t0 = nettest::Clock::now();
    std::size_t total = 0;
    bool dropped = false;
    while (msSince(t0) < 20000) {
        total += good.pump(50);
        const FanoutStats st = s.stats();
        if (st.droppedSlow + st.droppedStalled >= 1) { dropped = true; break; }
    }
    CHECK(dropped);
    CHECK(total > 100000);   // the reading client kept being served while the idle one filled up and was dropped
    CHECK(s.running());
    s.stop();
}

void testThousandConnections() {
    Source src;
    std::string err;
    TcpAudioServer s;
    TcpAudioConfig c = cfg0();
    c.maxClients = 4;
    CHECK(s.start(c, src.reader(), err));
    int connected = 0;
    {
        std::vector<std::unique_ptr<Client>> held;
        for (int i = 0; i < 1000; ++i) {
            auto cl = std::make_unique<Client>();
            if (cl->connect(static_cast<std::uint16_t>(s.port()), 500)) { ++connected; }
            if (held.size() < 40) { held.push_back(std::move(cl)); }   // some stay open, the rest close at once
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const FanoutStats st = s.stats();
        CHECK(st.clients <= 4);                       // the cap held while they piled up
        CHECK(st.refusedFull > 0);                    // the rest were turned away at once
        CHECK(st.accepted + st.refusedFull >= 900);   // (the kernel's backlog may have turned a few away itself)
    }
    // the server still works for a well-behaved client afterwards
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Client ok;
    bool served = false;
    for (int attempt = 0; attempt < 20 && !served; ++attempt) {
        if (ok.connect(static_cast<std::uint16_t>(s.port())) && ok.waitBytes(2000, 1000)) { served = true; }
        else { ok.close(); ok.got.clear(); ok.eof = false; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    CHECK(served);
    CHECK(connected > 0);
    const auto t0 = nettest::Clock::now();
    s.stop();
    CHECK(msSince(t0) < 2000);   // a clean stop with clients attached
}

void testRadioGoneIsCountedNotFatal() {
    Source src;
    src.gone.store(true);
    std::string err;
    TcpAudioServer s;
    CHECK(s.start(cfg0(), src.reader(), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(s.noRadio() > 0 && s.running());
    src.gone.store(false);   // it comes back: the stream resumes
    Client c;
    CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
    CHECK(c.waitBytes(2000, 3000));
    s.stop();
}

}  // namespace

int main() {
    testRefusals();
    testStreamIsTheUdpFormat();
    testOffByDefaultNothingBeforeStart();
    testClientThatSendsGarbageIsIgnored();
    testClientThatNeverReadsIsDropped();
    testThousandConnections();
    testRadioGoneIsCountedNotFatal();
    return testSummary("test_tcp_audio");
}
