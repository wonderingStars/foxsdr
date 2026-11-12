// Tests for net/tcp_fanout.hpp (and the ring and socket shim under it) - the shared TCP fan-out server of the network output parts.
// Every test talks to the server over a REAL loopback socket; nothing here reaches an outside host.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tcp_fanout.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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

// A 64-byte record: 4-byte little-endian sequence, then a pattern derived from it.
std::vector<std::uint8_t> record(std::uint32_t seq, std::size_t len = 64) {
    std::vector<std::uint8_t> r(len);
    for (int i = 0; i < 4; ++i) { r[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(seq >> (8 * i)); }
    for (std::size_t i = 4; i < len; ++i) { r[i] = static_cast<std::uint8_t>(seq * 31u + i); }
    return r;
}

// Checks that `got` is exactly records [first, first+count) of length `len`, in order. Returns the number of whole records found in sequence.
std::uint32_t inSequence(const std::vector<std::uint8_t>& got, std::uint32_t first, std::size_t len = 64) {
    std::uint32_t n = 0;
    for (std::size_t off = 0; off + len <= got.size(); off += len, ++n) {
        const std::vector<std::uint8_t> want = record(first + n, len);
        if (std::memcmp(got.data() + off, want.data(), len) != 0) { return n; }
    }
    return n;
}

FanoutConfig smallCfg() {
    FanoutConfig c;
    c.port = 0;
    c.maxClients = 8;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
void testRingBasics() {
    MessageRing r(128);
    CHECK(r.capacity() == 128);
    std::uint8_t out[256];
    CHECK(r.pop(out, sizeof(out)) == 0);  // empty
    CHECK(!r.push(out, 0));               // empty record refused
    CHECK(r.oversize() == 1);
    std::vector<std::uint8_t> big(r.maxMessage() + 1, 1);
    CHECK(!r.push(big.data(), big.size()));  // oversize refused
    CHECK(r.oversize() == 2);
    // wrap: many records of odd lengths through a small ring, order and bytes kept
    std::uint32_t pushed = 0, popped = 0;
    for (int round = 0; round < 500; ++round) {
        const std::size_t len = 5 + static_cast<std::size_t>(round % 37);
        std::vector<std::uint8_t> v(len, static_cast<std::uint8_t>(round));
        if (r.push(v.data(), v.size())) { ++pushed; }
        if (round % 3 == 2) {
            while (true) {
                const std::size_t n = r.pop(out, sizeof(out));
                if (n == 0) { break; }
                ++popped;
                CHECK(n >= 5 && n < 42);
                CHECK(std::all_of(out, out + n, [&](std::uint8_t b) { return b == out[0]; }));
            }
        }
    }
    while (r.pop(out, sizeof(out)) != 0) { ++popped; }
    CHECK(pushed == popped);
    CHECK(r.dropped() + pushed == 500);  // every push is either in or counted as dropped
    CHECK(r.dropped() > 0);              // the small ring really did overflow at times
    CHECK(r.empty());
}

void testRingTwoThreads() {
    // One producer, one consumer, 300k records through a ring far smaller than the total; each record is 16 bytes of its own sequence.
    MessageRing r(64 * 1024);
    constexpr std::uint32_t kTotal = 300000;
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> bad{0};
    std::atomic<std::uint32_t> got{0};
    std::thread consumer([&] {
        std::uint32_t last = 0;
        bool any = false;
        std::uint8_t buf[64];
        for (;;) {
            const std::size_t n = r.pop(buf, sizeof(buf));
            if (n == 0) {
                if (done.load() && r.empty()) { break; }
                std::this_thread::yield();
                continue;
            }
            std::uint32_t seq = 0, check = 0;
            std::memcpy(&seq, buf, 4);
            std::memcpy(&check, buf + 12, 4);
            if (n != 16 || check != ~seq || (any && seq <= last)) { ++bad; }
            last = seq;
            any = true;
            ++got;
        }
    });
    for (std::uint32_t i = 0; i < kTotal; ++i) {
        std::uint8_t rec[16] = {};
        const std::uint32_t inv = ~i;
        std::memcpy(rec, &i, 4);
        std::memcpy(rec + 12, &inv, 4);
        r.push(rec, sizeof(rec));
    }
    done.store(true);
    consumer.join();
    CHECK(bad.load() == 0);
    CHECK(got.load() + r.dropped() == kTotal);
    std::printf("ring two threads: %u delivered, %llu dropped of %u\n", got.load(), static_cast<unsigned long long>(r.dropped()), kTotal);
}

// ---------------------------------------------------------------------------------------------------------------------
void testConfigRefusals() {
    {
        FanoutConfig c = smallCfg();
        c.bindAddress = "0.0.0.0";
        TcpFanout f(c);
        std::string err;
        CHECK(!f.start(err));
        CHECK(err.find("every network interface") != std::string::npos);
        CHECK(!f.running());
    }
    {
        FanoutConfig c = smallCfg();
        c.bindAddress = "0.0.0.0";
        c.allowAllInterfaces = true;  // asked for explicitly
        TcpFanout f(c);
        std::string err;
        CHECK(f.start(err));
        CHECK(f.port() != 0);
        f.stop();
    }
    const char* bad[] = {"localhost", "", "1.2.3", "1.2.3.4.5", "256.0.0.1", "01.2.3.4", "::1", "127.0.0.1 ", "1..2.3"};
    for (const char* a : bad) {
        FanoutConfig c = smallCfg();
        c.bindAddress = a;
        TcpFanout f(c);
        std::string err;
        CHECK(!f.start(err));
        CHECK(err.find("not an IPv4 address") != std::string::npos);
    }
    {
        FanoutConfig c = smallCfg();
        c.maxClients = 0;
        std::string err;
        CHECK(!TcpFanout(c).start(err));
        c.maxClients = kFanoutMaxClientsCap + 1;
        CHECK(!TcpFanout(c).start(err));
        c = smallCfg();
        c.clientQueueBytes = 100;
        CHECK(!TcpFanout(c).start(err));
        c = smallCfg();
        c.greeting.assign(c.clientQueueBytes, 1);
        CHECK(!TcpFanout(c).start(err));
        c = smallCfg();
        c.pollMs = 0;
        CHECK(!TcpFanout(c).start(err));
    }
    {
        // a port already in use is refused with words, and the first server is unharmed
        FanoutConfig c = smallCfg();
        TcpFanout a(c);
        std::string err;
        CHECK(a.start(err));
        FanoutConfig c2 = smallCfg();
        c2.port = a.port();
        TcpFanout b(c2);
        CHECK(!b.start(err));
        CHECK(err.find("could not listen") != std::string::npos);
        Client cl;
        CHECK(cl.connect(a.port()));
        // a second start on a running object is refused too
        CHECK(!a.start(err));
        CHECK(err == "already running");
    }
    {
        FanoutConfig c = smallCfg();
        TcpFanout f(c);
        CHECK(!f.publish("x", 1));  // not running: nothing accepted
        CHECK(f.stats().published == 0);
    }
}

void testHappyPathAndGreeting() {
    FanoutConfig c = smallCfg();
    c.greeting = {'H', 'I', '\n'};
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    CHECK(f.port() != 0);
    Client a;
    CHECK(a.connect(f.port()));
    CHECK(a.waitBytes(3, 2000));
    CHECK(a.got.size() == 3 && a.got[0] == 'H' && a.got[2] == '\n');  // greeting first, exactly
    for (std::uint32_t i = 0; i < 100; ++i) {
        const auto r = record(i);
        CHECK(f.publish(r.data(), r.size()));
    }
    CHECK(a.waitBytes(3 + 6400, 3000));
    std::vector<std::uint8_t> body(a.got.begin() + 3, a.got.end());
    CHECK(body.size() == 6400);
    CHECK(inSequence(body, 0) == 100);  // every record, in order, byte-exact
    const FanoutStats s = f.stats();
    CHECK(s.accepted == 1 && s.published == 100 && s.ringDropped == 0 && s.clients == 1);
    CHECK(s.bytesSent == 6403);
    // a late client gets the greeting and only what is published after it
    Client b;
    CHECK(b.connect(f.port()));
    CHECK(b.waitBytes(3, 2000));
    CHECK(b.got.size() == 3);
    const auto r = record(100);
    CHECK(f.publish(r.data(), r.size()));
    CHECK(b.waitBytes(3 + 64, 2000));
    CHECK(a.waitBytes(3 + 6400 + 64, 2000));
    f.stop();
    CHECK(!f.running());
    CHECK(f.port() != 0);
}

void testEmptyAndOversizePublish() {
    FanoutConfig c = smallCfg();
    c.clientQueueBytes = 4096;
    c.ringBytes = 1 << 20;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    CHECK(f.maxMessage() == 4096);
    std::vector<std::uint8_t> big(4097, 7), ok(4096, 7);
    CHECK(!f.publish(big.data(), big.size()));   // longer than any client could queue
    CHECK(!f.publish(ok.data(), 0));             // empty
    CHECK(f.publish(ok.data(), ok.size()));      // exactly the bound
    CHECK(f.stats().oversize == 2);
    CHECK(f.stats().published == 1);
}

void testManyClients() {
    FanoutConfig c = smallCfg();
    c.maxClients = 40;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    constexpr int kClients = 32;
    constexpr std::uint32_t kRecords = 2000;
    std::vector<std::unique_ptr<Client>> cl;
    for (int i = 0; i < kClients; ++i) {
        cl.push_back(std::make_unique<Client>());
        CHECK(cl.back()->connect(f.port()));
    }
    const auto t0 = nettest::Clock::now();
    for (int spins = 0; f.stats().clients < kClients && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(f.stats().clients == kClients);
    for (std::uint32_t i = 0; i < kRecords; ++i) {
        const auto r = record(i);
        while (!f.publish(r.data(), r.size())) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    int complete = 0;
    for (auto& k : cl) {
        if (k->waitBytes(static_cast<std::size_t>(kRecords) * 64, 8000) && inSequence(k->got, 0) == kRecords) { ++complete; }
    }
    CHECK(complete == kClients);  // every one of 32 clients got every record, in order
    std::printf("32 clients x %u records (%zu KiB each): %.0f ms\n", kRecords, static_cast<std::size_t>(kRecords) * 64 / 1024, msSince(t0));
    CHECK(f.stats().droppedSlow == 0 && f.stats().ringDropped == 0);
}

void testClientCap() {
    FanoutConfig c = smallCfg();
    c.maxClients = 3;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    std::vector<std::unique_ptr<Client>> cl;
    for (int i = 0; i < 3; ++i) {
        cl.push_back(std::make_unique<Client>());
        CHECK(cl.back()->connect(f.port()));
    }
    for (int spins = 0; f.stats().clients < 3 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(f.stats().clients == 3);
    Client extra1, extra2;
    CHECK(extra1.connect(f.port()));  // the OS accepts the connection ...
    CHECK(extra2.connect(f.port()));
    CHECK(extra1.waitEof(2000));  // ... and the server closes it at once
    CHECK(extra2.waitEof(2000));
    CHECK(f.stats().refusedFull == 2);
    CHECK(f.stats().clients == 3);
    const auto r = record(0);
    CHECK(f.publish(r.data(), r.size()));
    for (auto& k : cl) { CHECK(k->waitBytes(64, 2000)); }
    CHECK(extra1.got.empty());
    // a slot frees when a client leaves
    cl[0]->close();
    for (int spins = 0; f.stats().clients > 2 && spins < 300; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(f.stats().clients == 2);
    Client again;
    CHECK(again.connect(f.port()));
    for (int spins = 0; f.stats().clients < 3 && spins < 300; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(f.stats().clients == 3);
}

// A client that never reads must be dropped; the producer and the other client must be unaffected.
void testSlowClientDroppedNotProducer() {
    FanoutConfig c = smallCfg();
    c.clientQueueBytes = 64 * 1024;
    c.ringBytes = 8 << 20;
    c.socketSendBuffer = 4096;
    c.stallMs = 0;  // isolate the queue-overflow rule
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    Client lazy, good;
    CHECK(lazy.connect(f.port()));
    sock::setBuffers(lazy.s, 4096, 0);
    CHECK(good.connect(f.port()));
    for (int spins = 0; f.stats().clients < 2 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    std::atomic<bool> stopReading{false};
    std::thread reader([&] {
        while (!stopReading.load()) { good.pump(5); }
    });
    double worst = 0;
    std::uint32_t seq = 0;
    std::size_t sent = 0;
    const auto t0 = nettest::Clock::now();
    while (f.stats().droppedSlow == 0 && sent < 400u * 1024 * 1024 && msSince(t0) < 20000) {
        const auto r = record(seq++, 1024);
        const auto a = nettest::Clock::now();
        const bool ok = f.publish(r.data(), r.size());
        worst = std::max(worst, msSince(a));
        if (ok) { sent += r.size(); } else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if ((seq & 63) == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
    }
    CHECK(f.stats().droppedSlow == 1);  // exactly the lazy one
    CHECK(f.stats().clients == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopReading.store(true);
    reader.join();
    // the good client still gets data after the lazy one was dropped
    const std::size_t before = good.got.size();
    const auto r = record(seq++, 1024);
    CHECK(f.publish(r.data(), r.size()));
    CHECK(good.waitBytes(before + 1024, 2000));
    CHECK(inSequence(good.got, 0, 1024) >= 1);  // contiguous from the first record: the good client lost nothing
    // the lazy client's connection was closed by the server: it reads what the OS held, then EOF
    CHECK(lazy.waitEof(3000));
    std::printf("slow client dropped after %zu KiB published; producer's slowest publish() call %.3f ms\n", sent / 1024, worst);
    CHECK(worst < 100.0);  // the producer never waited on the lazy client (bound is generous for a busy machine)
}

// A client holding pending bytes and not reading is dropped by the stall rule even though its queue never overflows.
void testStalledClientDropped() {
    FanoutConfig c = smallCfg();
    c.clientQueueBytes = 4 << 20;
    c.ringBytes = 8 << 20;
    c.socketSendBuffer = 4096;
    c.stallMs = 300;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    Client lazy;
    CHECK(lazy.connect(f.port()));
    sock::setBuffers(lazy.s, 4096, 0);
    for (int spins = 0; f.stats().clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    // Publish until the server holds bytes the client has not taken (the kernel's buffers are full and bytesSent has stopped growing
    // while more was published), far below the queue bound, then stop.
    std::size_t sent = 0;
    std::uint32_t seq = 0;
    std::uint64_t lastSent = ~0ull;
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < 20000 && sent < 200u * 1024 * 1024) {
        const auto r = record(seq++, 1024);
        if (f.publish(r.data(), r.size())) { sent += r.size(); } else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if ((seq & 255) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const std::uint64_t now = f.stats().bytesSent;
            if (now == lastSent && sent > now + 64 * 1024) { break; }
            lastSent = now;
        }
    }
    CHECK(sent > f.stats().bytesSent + 64 * 1024);  // bytes are pending
    CHECK(f.stats().droppedSlow == 0);              // ... and the queue (4 MiB) has not overflowed
    const auto t1 = nettest::Clock::now();
    while (f.stats().droppedStalled == 0 && msSince(t1) < 5000) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(f.stats().droppedStalled == 1);
    CHECK(f.stats().droppedSlow == 0);
    CHECK(f.stats().clients == 0);
    std::printf("stalled client dropped %.0f ms after the last publish (stallMs 300)\n", msSince(t1));
}

// A client that connects and drops (and one that connects and half-closes, and one that resets) at speed, from several threads, while the
// producer is publishing: the server stays up, counts add up, and a fresh client is served correctly afterwards.
void testConnectDropStorm() {
    FanoutConfig c = smallCfg();
    c.maxClients = 16;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    std::atomic<bool> stopPub{false};
    std::atomic<std::uint32_t> pubSeq{0};
    std::thread producer([&] {
        while (!stopPub.load()) {
            const auto r = record(pubSeq.fetch_add(1));
            f.publish(r.data(), r.size());
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::atomic<int> connected{0}, failed{0};
    std::vector<std::thread> storm;
    for (int t = 0; t < 4; ++t) {
        storm.emplace_back([&, t] {
            for (int i = 0; i < 75; ++i) {
                Client k;
                if (k.connect(f.port(), 3000)) {
                    ++connected;
                    if ((i + t) % 3 == 0) { k.pump(2); }
                    if ((i + t) % 5 == 0) { k.sendBytes("junk", 4); }
                } else {
                    ++failed;
                }
            }
        });
    }
    for (auto& th : storm) { th.join(); }
    stopPub.store(true);
    producer.join();
    CHECK(connected.load() + failed.load() == 300);
    CHECK(connected.load() >= 250);
    for (int spins = 0; f.stats().clients > 0 && spins < 400; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const FanoutStats s = f.stats();
    CHECK(s.clients == 0);
    CHECK(s.accepted + s.refusedFull >= static_cast<std::uint64_t>(connected.load()));
    CHECK(s.accepted == s.closedByPeer + s.droppedSlow + s.droppedStalled);  // every accepted client is accounted for
    Client fresh;
    CHECK(fresh.connect(f.port()));
    for (int spins = 0; f.stats().clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const auto r = record(777);
    CHECK(f.publish(r.data(), r.size()));
    CHECK(fresh.waitBytes(64, 2000));
    CHECK(inSequence(fresh.got, 777) == 1);
    std::printf("storm: %d connects, %d failed, accepted %llu refused %llu closedByPeer %llu\n", connected.load(), failed.load(),
                static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.refusedFull),
                static_cast<unsigned long long>(s.closedByPeer));
}

// Hostile peer: floods the server with bytes; the server reads and throws them away, counts them, and serves the others.
void testHostileFlood() {
    FanoutConfig c = smallCfg();
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    Client bad, good;
    CHECK(bad.connect(f.port()));
    CHECK(good.connect(f.port()));
    for (int spins = 0; f.stats().clients < 2 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    std::vector<std::uint8_t> junk(65536, 0xAA);
    std::size_t sentJunk = 0;
    const auto t0 = nettest::Clock::now();
    std::uint32_t seq = 0;
    while (sentJunk < 8u * 1024 * 1024 && msSince(t0) < 10000) {
        const long r = bad.sendBytes(junk.data(), junk.size());
        if (r > 0) { sentJunk += static_cast<std::size_t>(r); } else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        const auto rec = record(seq++);
        f.publish(rec.data(), rec.size());
        good.pump(0);
    }
    CHECK(sentJunk >= 8u * 1024 * 1024);
    CHECK(good.waitBytes(static_cast<std::size_t>(seq) * 64, 5000));
    CHECK(inSequence(good.got, 0) == seq);  // the well-behaved client lost nothing to the flood
    for (int spins = 0; f.stats().bytesIgnored < sentJunk && spins < 300; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(f.stats().bytesIgnored >= sentJunk - 1);
    CHECK(f.stats().clients == 2);  // junk is not a reason to disconnect, only to ignore
    // bytes sent by a client are never echoed back
    bad.pump(100);
    bool echoed = false;
    for (std::size_t i = 0; i + 1 < bad.got.size(); ++i) {
        if (bad.got[i] == 0xAA && bad.got[i + 1] == 0xAA) { echoed = true; }
    }
    CHECK(!echoed);
}

// Stop while a producer is still publishing and clients are attached and reading: returns quickly, every client sees the close, the
// producer's calls after the stop are refused without blocking, and the same object starts again.
void testStopWhileSending() {
    double worstStop = 0;
    for (int round = 0; round < 6; ++round) {
        FanoutConfig c = smallCfg();
        c.maxClients = 12;
        TcpFanout f(c);
        std::string err;
        CHECK(f.start(err));
        // eight clients, each READING in its own thread (so none is dropped as slow before the stop)
        std::vector<std::unique_ptr<Client>> cl;
        for (int i = 0; i < 8; ++i) {
            cl.push_back(std::make_unique<Client>());
            CHECK(cl.back()->connect(f.port()));
        }
        for (int spins = 0; f.stats().clients < 8 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        std::atomic<int> sawEof{0};
        std::vector<std::thread> readers;
        for (auto& k : cl) {
            readers.emplace_back([&sawEof, &k] {
                if (k->waitEof(8000)) { ++sawEof; }
            });
        }
        std::atomic<bool> stopPub{false};
        std::atomic<long> refusedAfter{0};
        std::thread producer([&] {
            std::uint32_t s = 0;
            while (!stopPub.load()) {
                const auto r = record(s++, 512);
                if (!f.publish(r.data(), r.size()) && !f.running()) { ++refusedAfter; }
                if ((s & 15) == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60 + 10 * round));
        CHECK(f.stats().droppedSlow == 0 && f.stats().clients == 8);  // all eight are attached and keeping up when the stop comes
        const auto t0 = nettest::Clock::now();
        f.stop();
        const double took = msSince(t0);
        worstStop = std::max(worstStop, took);
        CHECK(took < 1000.0);
        CHECK(!f.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopPub.store(true);
        producer.join();
        for (auto& th : readers) { th.join(); }
        CHECK(refusedAfter.load() > 0);  // after the stop the producer is told "no" and is never blocked
        CHECK(sawEof.load() == 8);       // every client saw the server close it
        CHECK(f.stats().clients == 0);
        // restart on the same object
        CHECK(f.start(err));
        Client again;
        CHECK(again.connect(f.port()));
        for (int spins = 0; f.stats().clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        const auto r = record(1);
        CHECK(f.publish(r.data(), r.size()));
        CHECK(again.waitBytes(64, 2000));
        f.stop();
        f.stop();  // idempotent
    }
    std::printf("stop() with 8 clients attached and the producer publishing: worst of 6 = %.1f ms\n", worstStop);
}

void testDestructorStops() {
    for (int i = 0; i < 20; ++i) {
        FanoutConfig c = smallCfg();
        TcpFanout f(c);
        std::string err;
        CHECK(f.start(err));
        Client k;
        CHECK(k.connect(f.port()));
        const auto r = record(1);
        f.publish(r.data(), r.size());
        // destroyed here with a client attached
    }
}

// Wait-free producer: the cost of publish() is measured, and it does not depend on the consumer being slow.
void testPublishCost() {
    FanoutConfig c = smallCfg();
    c.ringBytes = 1 << 20;
    TcpFanout f(c);
    std::string err;
    CHECK(f.start(err));
    Client k;
    CHECK(k.connect(f.port()));  // never read
    std::vector<std::uint8_t> r(1024, 5);
    constexpr int kN = 200000;
    const auto t0 = nettest::Clock::now();
    int ok = 0;
    for (int i = 0; i < kN; ++i) { ok += f.publish(r.data(), r.size()) ? 1 : 0; }
    const double us = msSince(t0) * 1000.0 / kN;
    std::printf("publish() of 1 KiB with the ring filling and a client that never reads: %.3f us per call (%d of %d accepted)\n", us, ok, kN);
    CHECK(us < 50.0);
    CHECK(ok < kN);                         // the ring did fill, and the caller was told, not blocked
    CHECK(f.stats().ringDropped + static_cast<std::uint64_t>(ok) == kN);
}

}  // namespace

int main() {
    testRingBasics();
    testRingTwoThreads();
    testConfigRefusals();
    testHappyPathAndGreeting();
    testEmptyAndOversizePublish();
    testManyClients();
    testClientCap();
    testSlowClientDroppedNotProducer();
    testStalledClientDropped();
    testConnectDropStorm();
    testHostileFlood();
    testStopWhileSending();
    testDestructorStops();
    testPublishCost();
    return testSummary("test_tcp_fanout");
}
