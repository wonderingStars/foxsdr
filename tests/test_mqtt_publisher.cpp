// Tests for the MQTT 3.1.1 publisher (core/mqtt_publisher.hpp). EVERYTHING TALKS TO STAND-IN BROKERS INSIDE THIS TEST on 127.0.0.1: no real broker, no
// outside host. The codec is checked against bytes worked out from the OASIS text; the publisher against a scripted broker that can refuse,
// stay silent, ack late, never ack, close early, send hostile lengths and send garbage.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/mqtt_publisher.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace mq = cascade::core::mqtt;
namespace pld = cascade::core::powerlog::detail;
using Clock = std::chrono::steady_clock;

namespace {

std::string bytes(std::initializer_list<int> v) {
    std::string s;
    for (int b : v) { s.push_back(static_cast<char>(b)); }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// A scripted stand-in broker.
// ---------------------------------------------------------------------------------------------------------------------------------------
struct Packet {
    int type = 0, flags = 0, conn = 0;
    std::string topic, payload, clientId, user, password;
    int id = 0, keepAlive = 0, connectFlags = 0;
    Clock::time_point at;
};

struct Script {
    int connackCode = 0;
    bool sendConnack = true;
    std::string connackRaw;          // if not empty: sent INSTEAD of a CONNACK
    std::string afterConnack;        // raw bytes sent right after a good CONNACK
    bool ackQos1 = true;
    int ignoreFirstAcks = 0;         // do not ack the first N QoS 1 publishes of the whole run
    bool answerPing = true;
    int closeConnAfterPublishes = -1;  // on connection #1 only: close (no ack) after this many publishes
    bool closeImmediately = false;     // accept and close
    int closeAfterConnackConn = -1;    // close right after CONNACK on that connection index
};

class FakeBroker {
public:
    ~FakeBroker() { stop(); }
    bool start(int port = 0) {
        pld::ensureNet();
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (pld::badSock(listen_)) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<unsigned short>(port));
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { return false; }
#if defined(_WIN32)
        int len = sizeof a;
#else
        socklen_t len = sizeof a;
#endif
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        if (::listen(listen_, 4) != 0) { return false; }
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) { th_.join(); }
        if (!pld::badSock(listen_)) { pld::closeSock(listen_); listen_ = static_cast<pld::sock_t>(-1); }
    }
    int port() const { return port_; }
    Script& script() { return script_; }
    std::vector<Packet> packets() { std::lock_guard<std::mutex> lk(mu_); return pk_; }
    int connections() { return conns_.load(); }
    std::vector<Clock::time_point> connectTimes() { std::lock_guard<std::mutex> lk(mu_); return times_; }
    int countType(int type) {
        int n = 0;
        for (const Packet& p : packets()) { if (p.type == type) { ++n; } }
        return n;
    }

private:
    void loop() {
        while (run_) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(listen_, &r);
            timeval tv{0, 50000};
#if defined(_WIN32)
            const int n = ::select(0, &r, nullptr, nullptr, &tv);
#else
            const int n = ::select(listen_ + 1, &r, nullptr, nullptr, &tv);
#endif
            if (n <= 0) { continue; }
            pld::sock_t c = ::accept(listen_, nullptr, nullptr);
            if (pld::badSock(c)) { continue; }
            const int idx = conns_.fetch_add(1);
            { std::lock_guard<std::mutex> lk(mu_); times_.push_back(Clock::now()); }
            serve(c, idx);
            pld::closeSock(c);
        }
    }
    static bool readable(pld::sock_t c, int ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(c, &r);
        timeval tv{0, ms * 1000};
#if defined(_WIN32)
        return ::select(0, &r, nullptr, nullptr, &tv) > 0;
#else
        return ::select(c + 1, &r, nullptr, nullptr, &tv) > 0;
#endif
    }
    void send(pld::sock_t c, const std::string& s) { ::send(c, s.data(), static_cast<int>(s.size()), 0); }

    void serve(pld::sock_t c, int idx) {
        if (script_.closeImmediately) { return; }
        std::string in;
        int publishes = 0;
        bool gotConnect = false;
        while (run_) {
            if (!readable(c, 50)) { continue; }
            char buf[4096];
            const int n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) { return; }
            in.append(buf, static_cast<std::size_t>(n));
            for (;;) {
                if (in.size() < 2) { break; }
                std::size_t rl = 0, used = 0;
                const auto st = mq::decodeRemainingLength(reinterpret_cast<const unsigned char*>(in.data()) + 1, in.size() - 1, &rl, &used);
                if (st == mq::RlStatus::NeedMore) { break; }
                if (st == mq::RlStatus::Malformed) { return; }
                const std::size_t total = 1 + used + rl;
                if (in.size() < total) { break; }
                Packet p;
                p.type = static_cast<unsigned char>(in[0]) >> 4;
                p.flags = static_cast<unsigned char>(in[0]) & 15;
                p.conn = idx;
                p.at = Clock::now();
                const unsigned char* b = reinterpret_cast<const unsigned char*>(in.data()) + 1 + used;
                if (p.type == 1) {
                    std::size_t o = 0;
                    auto str = [&]() { const std::size_t l = (b[o] << 8) | b[o + 1]; std::string s(reinterpret_cast<const char*>(b) + o + 2, l); o += 2 + l; return s; };
                    str();                  // "MQTT"
                    o += 1;                 // level
                    p.connectFlags = b[o++];
                    p.keepAlive = (b[o] << 8) | b[o + 1];
                    o += 2;
                    p.clientId = str();
                    if ((p.connectFlags & 0x80) != 0) { p.user = str(); }
                    if ((p.connectFlags & 0x40) != 0) { p.password = str(); }
                    gotConnect = true;
                } else if (p.type == 3) {
                    const std::size_t tl = (b[0] << 8) | b[1];
                    p.topic.assign(reinterpret_cast<const char*>(b) + 2, tl);
                    std::size_t o = 2 + tl;
                    if (((p.flags >> 1) & 3) > 0) { p.id = (b[o] << 8) | b[o + 1]; o += 2; }
                    p.payload.assign(reinterpret_cast<const char*>(b) + o, rl - o);
                } else if (p.type == 4) {
                    p.id = (b[0] << 8) | b[1];
                }
                { std::lock_guard<std::mutex> lk(mu_); pk_.push_back(p); }
                in.erase(0, total);
                if (p.type == 1) {
                    if (!script_.connackRaw.empty()) { send(c, script_.connackRaw); }
                    else if (script_.sendConnack) {
                        send(c, bytes({0x20, 0x02, 0x00, script_.connackCode}));
                        if (!script_.afterConnack.empty()) { send(c, script_.afterConnack); }
                    }
                    if (script_.closeAfterConnackConn == idx) { return; }
                    if (script_.connackCode != 0 && script_.connackRaw.empty()) { return; }
                } else if (p.type == 3) {
                    ++publishes;
                    if (script_.closeConnAfterPublishes >= 0 && idx == 0 && publishes >= script_.closeConnAfterPublishes) { return; }
                    if (((p.flags >> 1) & 3) == 1 && script_.ackQos1) {
                        const int seen = acksSeen_.fetch_add(1);
                        if (seen >= script_.ignoreFirstAcks) { send(c, mq::pubAck(static_cast<std::uint16_t>(p.id))); }
                    }
                } else if (p.type == 12) {
                    if (script_.answerPing) { send(c, bytes({0xD0, 0x00})); }
                } else if (p.type == 14) {
                    return;
                }
                (void)gotConnect;
            }
        }
    }

    pld::sock_t listen_ = static_cast<pld::sock_t>(-1);
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread th_;
    std::mutex mu_;
    std::vector<Packet> pk_;
    std::vector<Clock::time_point> times_;
    std::atomic<int> conns_{0}, acksSeen_{0};
    Script script_;
};

bool waitFor(const std::function<bool()>& f, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

mq::Options baseOpt(int port) {
    mq::Options o;
    o.enabled = true;
    o.host = "127.0.0.1";
    o.port = port;
    o.clientId = "foxsdr-test";
    o.pollMs = 5;
    o.connectTimeoutMs = 400;
    o.ackTimeoutMs = 300;
    o.backoffMinMs = 40;
    o.backoffMaxMs = 400;
    o.flushMs = 500;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
void testCodec() {
    // Remaining Length: the table of the standard (2.2.3, Table 2.4)
    struct Rl { std::size_t v; std::string enc; };
    const Rl rows[] = {{0, bytes({0x00})}, {127, bytes({0x7F})}, {128, bytes({0x80, 0x01})}, {16383, bytes({0xFF, 0x7F})},
                       {16384, bytes({0x80, 0x80, 0x01})}, {2097151, bytes({0xFF, 0xFF, 0x7F})}, {2097152, bytes({0x80, 0x80, 0x80, 0x01})},
                       {268435455, bytes({0xFF, 0xFF, 0xFF, 0x7F})}};
    for (const Rl& r : rows) {
        std::string o;
        CHECK(mq::encodeRemainingLength(r.v, &o) && o == r.enc);
        std::size_t v = 0, used = 0;
        CHECK(mq::decodeRemainingLength(reinterpret_cast<const unsigned char*>(r.enc.data()), r.enc.size(), &v, &used) == mq::RlStatus::Ok && v == r.v &&
              used == r.enc.size());
    }
    std::string o;
    CHECK(!mq::encodeRemainingLength(268435456, &o) && o.empty());
    std::size_t v = 0, used = 0;
    const unsigned char five[5] = {0x80, 0x80, 0x80, 0x80, 0x01};
    CHECK(mq::decodeRemainingLength(five, 5, &v, &used) == mq::RlStatus::Malformed);
    CHECK(mq::decodeRemainingLength(five, 3, &v, &used) == mq::RlStatus::NeedMore);
    CHECK(mq::decodeRemainingLength(five, 0, &v, &used) == mq::RlStatus::NeedMore);

    // CONNECT, worked out by hand from 3.1: 10 | RL 13 | 00 04 "MQTT" | 04 | flags 02 | keep alive 003C | 0001 "a"
    mq::ConnectOptions c;
    c.clientId = "a";
    c.keepAliveSec = 60;
    std::string pkt, err;
    CHECK(mq::buildConnect(c, &pkt, &err));
    CHECK(pkt == bytes({0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C, 0x00, 0x01, 'a'}));
    c.username = "u";
    c.password = "pw";
    c.cleanSession = false;
    CHECK(mq::buildConnect(c, &pkt, &err));
    CHECK(pkt == bytes({0x10, 0x14, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xC0, 0x00, 0x3C, 0x00, 0x01, 'a', 0x00, 0x01, 'u', 0x00, 0x02, 'p', 'w'}));
    c.password.clear();  // a user name alone: no password flag
    CHECK(mq::buildConnect(c, &pkt, &err) && (static_cast<unsigned char>(pkt[9]) == 0x80));
    c = mq::ConnectOptions();
    c.password = "x";  // a password without a user name is not sent (3.1.2-22)
    c.clientId = "id";
    CHECK(mq::buildConnect(c, &pkt, &err) && static_cast<unsigned char>(pkt[9]) == 0x02 && pkt.find('x') == std::string::npos);
    c.clientId.clear();  // empty id needs CleanSession 1 (3.1.3-7)
    CHECK(mq::buildConnect(c, &pkt, &err));
    c.cleanSession = false;
    CHECK(!mq::buildConnect(c, &pkt, &err));
    c.cleanSession = true;
    c.clientId = std::string("a\0b", 3);
    CHECK(!mq::buildConnect(c, &pkt, &err));
    c.clientId = "\xC0\xAF";  // overlong
    CHECK(!mq::buildConnect(c, &pkt, &err));
    c.clientId = std::string(70000, 'x');
    CHECK(!mq::buildConnect(c, &pkt, &err));

    // PUBLISH: QoS 0 "a/b" "hi"; QoS 1 retained id 1; with DUP; the flags and lengths of 3.3.1
    CHECK(mq::buildPublish("a/b", "hi", 0, false, false, 0, &pkt, &err) && pkt == bytes({0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'}));
    CHECK(mq::buildPublish("a/b", "hi", 1, true, false, 1, &pkt, &err) && pkt == bytes({0x33, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x01, 'h', 'i'}));
    CHECK(mq::buildPublish("a/b", "hi", 1, false, true, 0x0102, &pkt, &err) && pkt == bytes({0x3A, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x01, 0x02, 'h', 'i'}));
    CHECK(mq::buildPublish("a/b", "hi", 0, false, true, 0, &pkt, &err) && static_cast<unsigned char>(pkt[0]) == 0x30);  // DUP never set for QoS 0 (3.3.1-2)
    CHECK(mq::buildPublish("a/b", "", 0, false, false, 0, &pkt, &err) && pkt == bytes({0x30, 0x05, 0x00, 0x03, 'a', '/', 'b'}));  // empty payload is legal
    CHECK(!mq::buildPublish("a/b", "x", 2, false, false, 1, &pkt, &err));      // QoS 2 refused
    CHECK(!mq::buildPublish("a/b", "x", 1, false, false, 0, &pkt, &err));      // identifier 0 (2.3.1-1)
    CHECK(!mq::buildPublish("", "x", 0, false, false, 0, &pkt, &err));         // empty topic
    CHECK(!mq::buildPublish("a/#", "x", 0, false, false, 0, &pkt, &err));      // wildcard (3.3.2-2)
    CHECK(!mq::buildPublish("a/+/b", "x", 0, false, false, 0, &pkt, &err));
    CHECK(!mq::buildPublish(std::string("a\0b", 3), "x", 0, false, false, 0, &pkt, &err));
    CHECK(!mq::buildPublish(std::string(65536, 'a'), "x", 0, false, false, 0, &pkt, &err));  // 1.5.3 limit
    CHECK(mq::buildPublish(std::string(65535, 'a'), "x", 0, false, false, 0, &pkt, &err));
    CHECK(mq::buildPublish("caf\xC3\xA9/\xE2\x82\xAC", "x", 0, false, false, 0, &pkt, &err));  // valid UTF-8 topic
    CHECK(!mq::buildPublish("bad\xFF", "x", 0, false, false, 0, &pkt, &err));
    CHECK(!mq::buildPublish("\xED\xA0\x80", "x", 0, false, false, 0, &pkt, &err));  // a surrogate
    CHECK(mq::pubAck(0x0102) == bytes({0x40, 0x02, 0x01, 0x02}) && mq::pingReq() == bytes({0xC0, 0}) && mq::disconnectPacket() == bytes({0xE0, 0}));
    // a 200-byte payload needs a two-byte Remaining Length
    CHECK(mq::buildPublish("t", std::string(200, 'p'), 0, false, false, 0, &pkt, &err) && static_cast<unsigned char>(pkt[1]) == (0x80 | ((203) & 0x7F)) &&
          static_cast<unsigned char>(pkt[2]) == 1);
}

void testReader() {
    mq::BrokerReader r;
    mq::Incoming in;
    std::size_t used = 0;
    // good packets, split anywhere
    const std::string good = bytes({0x20, 0x02, 0x01, 0x00, 0x40, 0x02, 0x12, 0x34, 0xD0, 0x00});
    for (std::size_t split = 1; split < good.size(); ++split) {
        r.reset();
        std::vector<mq::Incoming> got;
        for (const std::string& part : {good.substr(0, split), good.substr(split)}) {
            std::size_t off = 0;
            while (off < part.size()) {
                const auto st = r.feed(reinterpret_cast<const unsigned char*>(part.data()) + off, part.size() - off, &used, &in);
                off += used;
                CHECK(st != mq::BrokerReader::Status::Error);
                if (st == mq::BrokerReader::Status::Packet) { got.push_back(in); }
            }
        }
        CHECK(got.size() == 3 && got[0].type == mq::Incoming::Type::ConnAck && got[0].sessionPresent && got[0].returnCode == 0);
        CHECK(got.size() == 3 && got[1].type == mq::Incoming::Type::PubAck && got[1].packetId == 0x1234 && got[2].type == mq::Incoming::Type::PingResp);
    }
    // hostile: each is an Error, raised no later than the byte that shows it, with nothing buffered
    const std::vector<std::string> bad = {
        bytes({0x20, 0x03, 0, 0, 0}),                              // CONNACK with length 3
        bytes({0x20, 0x01, 0}),                                    // length 1
        bytes({0x20, 0x00}),                                       // length 0
        bytes({0x40, 0x03, 0, 1, 0}),                              // PUBACK length 3
        bytes({0xD0, 0x01, 0}),                                    // PINGRESP with a body
        bytes({0x30, 0xFF, 0xFF, 0xFF, 0x7F}),                     // a PUBLISH of 268,435,455 bytes
        bytes({0x20, 0xFF, 0xFF, 0xFF, 0x7F}),                     // a CONNACK claiming that length
        bytes({0x20, 0x80, 0x80, 0x80, 0x80, 0x01}),               // five-byte Remaining Length
        bytes({0x21, 0x02, 0, 0}),                                 // CONNACK with a flag bit set (2.2.2-2)
        bytes({0x90, 0x03, 0, 1, 0}),                              // SUBACK, never asked for
        bytes({0x00, 0x00}), bytes({0xF0, 0x00}), bytes({0xFF}),   // reserved types
        bytes({0x20, 0x02, 0x02, 0x00}),                           // CONNACK acknowledge-flags reserved bit set
    };
    for (const std::string& b : bad) {
        r.reset();
        const auto st = r.feed(reinterpret_cast<const unsigned char*>(b.data()), b.size(), &used, &in);
        CHECK(st == mq::BrokerReader::Status::Error);
        CHECK(used <= 5);
        // and it stays dead: more bytes are not mistaken for a packet
        used = 0;
        const unsigned char more[4] = {0x20, 0x02, 0x00, 0x00};
        CHECK(r.feed(more, 4, &used, &in) == mq::BrokerReader::Status::Error && used == 4);
    }
    // the whole reader is a handful of bytes: a hostile length cannot make it allocate
    CHECK(sizeof(mq::BrokerReader) <= 16);
    // random garbage never crashes and never claims a packet it cannot have
    std::mt19937 g(3);
    for (int i = 0; i < 2000; ++i) {
        r.reset();
        std::string junk(g() % 12, 0);
        for (char& ch : junk) { ch = static_cast<char>(g()); }
        r.feed(reinterpret_cast<const unsigned char*>(junk.data()), junk.size(), &used, &in);
        CHECK(used <= junk.size());
    }
}

void testDisabledAndInvalid() {
    FakeBroker b;
    CHECK(b.start());
    mq::Options o = baseOpt(b.port());
    o.enabled = false;
    mq::Publisher p(o);
    CHECK(!p.start());
    CHECK(p.publish("a/b", "x") == mq::PublishResult::Disabled);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(b.connections() == 0 && p.stats().offered == 0);
    // enabled but unusable options do not start
    mq::Options e = baseOpt(b.port());
    e.host = "";
    CHECK(!mq::Publisher(e).start());
    e = baseOpt(b.port());
    e.port = 0;
    CHECK(!mq::Publisher(e).start());
    e = baseOpt(b.port());
    e.host = "bad host\r\n";
    CHECK(!mq::Publisher(e).start());
    // publish() refusals (never reach the wire)
    mq::Options on = baseOpt(b.port());
    on.slotBytes = 128;
    mq::Publisher q(on);  // not started: publish still validates and queues
    CHECK(q.publish("a/#", "x") == mq::PublishResult::Invalid);
    CHECK(q.publish("", "x") == mq::PublishResult::Invalid);
    CHECK(q.publish("a/b", "x", 2) == mq::PublishResult::Invalid);
    CHECK(q.publish("a/b", std::string(200, 'x')) == mq::PublishResult::TooLarge);
    CHECK(q.publish("a/b", std::string(100, 'x')) == mq::PublishResult::Accepted);
    CHECK(q.stats().rejected == 4 && q.stats().offered == 1);
}

void testHappyPath() {
    FakeBroker b;
    CHECK(b.start());
    mq::Options o = baseOpt(b.port());
    o.username = "user";
    o.password = "secret";
    o.keepAliveSec = 20;
    mq::Publisher p(o);
    CHECK(p.start());
    CHECK(p.publish("foxsdr/ft8", "one", 0, false) == mq::PublishResult::Accepted);
    CHECK(p.publish("foxsdr/ft8", "two", 1, true) == mq::PublishResult::Accepted);
    CHECK(p.publish("foxsdr/wspr", "three", 1, false) == mq::PublishResult::Accepted);
    CHECK(waitFor([&] { return p.stats().acked == 2; }, 3000));
    CHECK(p.connected());
    const std::vector<Packet> pk = b.packets();
    CHECK(pk.size() >= 4 && pk[0].type == 1);
    if (pk.size() >= 4) {
        CHECK(pk[0].clientId == "foxsdr-test" && pk[0].user == "user" && pk[0].password == "secret" && pk[0].keepAlive == 20 && pk[0].connectFlags == 0xC2);
        CHECK(pk[1].type == 3 && pk[1].topic == "foxsdr/ft8" && pk[1].payload == "one" && pk[1].flags == 0 && pk[1].id == 0);
        CHECK(pk[2].type == 3 && pk[2].payload == "two" && pk[2].flags == 0x03 && pk[2].id != 0);       // QoS 1, retain
        CHECK(pk[3].type == 3 && pk[3].topic == "foxsdr/wspr" && pk[3].flags == 0x02 && pk[3].id != pk[2].id);
    }
    p.stop();
    const mq::Stats s = p.stats();
    CHECK(s.sentQos0 == 1 && s.sentQos1 == 2 && s.acked == 2 && s.resent == 0 && s.connects == 1 && s.protocolErrors == 0);
    // clean disconnect: the last packet the broker saw is DISCONNECT, E0 00
    const std::vector<Packet> all = b.packets();
    CHECK(!all.empty() && all.back().type == 14);
    CHECK(!p.connected());
}

void testRefusedAndBackoff() {
    for (int code : {4, 5, 2}) {
        FakeBroker b;
        b.script().connackCode = code;
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        CHECK(waitFor([&] { return b.connections() >= 4; }, 4000));
        const std::string e = p.lastError();
        std::printf("CONNACK %d refused: %s; attempts %d\n", code, e.c_str(), b.connections());
        CHECK(e.find(mq::connackText(code)) != std::string::npos);
        CHECK(p.stats().connectFailures >= 3 && !p.connected());
        // the back-off grows: gaps between attempts at least min, then roughly doubling, capped at max (40, 80, 160, 320, 400 ms here)
        const auto times = b.connectTimes();
        std::vector<double> gaps;
        for (std::size_t i = 1; i < times.size(); ++i) { gaps.push_back(std::chrono::duration<double, std::milli>(times[i] - times[i - 1]).count()); }
        CHECK(gaps.size() >= 3);
        if (gaps.size() >= 3) { CHECK(gaps[0] >= 35 && gaps[1] >= 75 && gaps[2] >= 155 && gaps[2] > gaps[0]); }
        for (double g : gaps) { CHECK(g < 1200); }
        p.stop();
    }
}

void testHostileBroker() {
    struct Case { const char* name; std::function<void(Script&)> set; };
    const Case cases[] = {
        {"CONNACK of length 3", [](Script& s) { s.connackRaw = bytes({0x20, 0x03, 0, 0, 0}); }},
        {"CONNACK of length 1", [](Script& s) { s.connackRaw = bytes({0x20, 0x01, 0}); }},
        {"huge PUBLISH length instead of CONNACK", [](Script& s) { s.connackRaw = bytes({0x30, 0xFF, 0xFF, 0xFF, 0x7F}); }},
        {"five-byte length", [](Script& s) { s.connackRaw = bytes({0x20, 0x80, 0x80, 0x80, 0x80, 0x01}); }},
        {"garbage", [](Script& s) { s.connackRaw = "GET / HTTP/1.1\r\n\r\n"; }},
        {"huge length after a good CONNACK", [](Script& s) { s.afterConnack = bytes({0x30, 0xFF, 0xFF, 0xFF, 0x7F, 'x', 'x'}); }},
        {"a SUBACK after CONNACK", [](Script& s) { s.afterConnack = bytes({0x90, 0x03, 0, 1, 0}); }},
        {"second CONNACK", [](Script& s) { s.afterConnack = bytes({0x20, 0x02, 0, 0}); }},
    };
    for (const Case& c : cases) {
        FakeBroker b;
        c.set(b.script());
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/x", "m", 1);
        CHECK(waitFor([&] { return p.stats().protocolErrors >= 2; }, 4000));
        std::printf("hostile broker (%s): protocol errors %llu, reconnect attempts %d, still running\n", c.name,
                    static_cast<unsigned long long>(p.stats().protocolErrors), b.connections());
        CHECK(b.connections() >= 2);   // it came back, it did not die or hang
        p.stop();                      // and stops promptly
    }
    // closed early, half a packet, silence, accept-and-close
    {
        FakeBroker b;
        b.script().closeImmediately = true;
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        CHECK(waitFor([&] { return p.stats().connectFailures >= 3; }, 4000));
        CHECK(p.lastError().find("closed") != std::string::npos);
        p.stop();
    }
    {
        FakeBroker b;
        b.script().connackRaw = bytes({0x20});  // one byte, then nothing
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.connectTimeoutMs = 150;
        mq::Publisher p(o);
        CHECK(p.start());
        CHECK(waitFor([&] { return p.stats().connectFailures >= 2; }, 4000));
        CHECK(p.lastError() == "no CONNACK");
        p.stop();
    }
    {
        FakeBroker b;
        b.script().sendConnack = false;  // silence
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.connectTimeoutMs = 150;
        mq::Publisher p(o);
        const auto t0 = Clock::now();
        CHECK(p.start());
        CHECK(waitFor([&] { return p.stats().connectFailures >= 1; }, 3000));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        CHECK(ms < 1000);  // the 150 ms CONNACK timeout, not a hang
        const auto t1 = Clock::now();
        p.stop();
        const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
        CHECK(stopMs < 1000);
    }
    // a hostile broker does not stop a good one from working afterwards
    {
        FakeBroker b;
        b.script().connackRaw = bytes({0x20, 0x03, 0, 0, 0});
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/ok", "payload", 1);
        CHECK(waitFor([&] { return p.stats().protocolErrors >= 1; }, 3000));
        b.script().connackRaw.clear();
        CHECK(waitFor([&] { return p.stats().acked == 1; }, 4000));
        p.stop();
    }
}

void testQos1Resend() {
    // no ack for the first publish: re-sent with DUP and the same identifier, then acked
    {
        FakeBroker b;
        b.script().ignoreFirstAcks = 1;
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/r", "m", 1);
        CHECK(waitFor([&] { return p.stats().acked == 1; }, 4000));
        std::vector<Packet> pubs;
        for (const Packet& k : b.packets()) { if (k.type == 3) { pubs.push_back(k); } }
        CHECK(pubs.size() == 2);
        if (pubs.size() == 2) {
            CHECK((pubs[0].flags & 8) == 0 && (pubs[1].flags & 8) != 0);  // DUP only on the re-delivery (3.3.1-1)
            CHECK(pubs[0].id == pubs[1].id && pubs[0].id != 0 && pubs[0].payload == pubs[1].payload);
        }
        CHECK(p.stats().resent == 1);
        p.stop();
    }
    // never acked: three tries, then the connection is dropped and the message goes again after the reconnect
    {
        FakeBroker b;
        b.script().ackQos1 = false;
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/n", "m", 1);
        CHECK(waitFor([&] { return b.connections() >= 2; }, 6000));
        CHECK(p.stats().resent >= 2);
        p.stop();
    }
    // the broker drops the connection after the publish, unacknowledged: after the reconnect it is re-sent with DUP and the same identifier
    {
        FakeBroker b;
        b.script().closeConnAfterPublishes = 1;
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/c", "keep me", 1);
        CHECK(waitFor([&] { return p.stats().acked == 1; }, 5000));
        std::vector<Packet> pubs;
        for (const Packet& k : b.packets()) { if (k.type == 3) { pubs.push_back(k); } }
        CHECK(pubs.size() == 2);
        if (pubs.size() == 2) {
            CHECK(pubs[0].conn == 0 && pubs[1].conn == 1 && (pubs[1].flags & 8) != 0 && pubs[0].id == pubs[1].id);
        }
        CHECK(p.stats().reconnects == 1 && p.stats().connects == 2);
        p.stop();
    }
    // an acknowledgement for an identifier nobody holds is ignored
    {
        FakeBroker b;
        b.script().afterConnack = mq::pubAck(4242);
        CHECK(b.start());
        mq::Publisher p(baseOpt(b.port()));
        CHECK(p.start());
        p.publish("t/i", "m", 1);
        CHECK(waitFor([&] { return p.stats().acked == 1; }, 3000));
        CHECK(p.stats().protocolErrors == 0 && p.stats().connects == 1);
        p.stop();
    }
    // more QoS 1 messages than the inflight window: all delivered, identifiers distinct
    {
        FakeBroker b;
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.maxInflight = 3;
        mq::Publisher p(o);
        CHECK(p.start());
        for (int i = 0; i < 40; ++i) { p.publish("t/w", std::to_string(i), 1); }
        CHECK(waitFor([&] { return p.stats().acked == 40; }, 5000));
        std::set<int> ids;
        for (const Packet& k : b.packets()) { if (k.type == 3) { ids.insert(k.id); } }
        CHECK(ids.size() >= 3);
        p.stop();
    }
}

void testKeepAlive() {
    {
        FakeBroker b;
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.keepAliveSec = 1;
        mq::Publisher p(o);
        CHECK(p.start());
        CHECK(waitFor([&] { return b.countType(12) >= 2; }, 4000));  // idle: PINGREQ about every half keep-alive
        CHECK(p.stats().pings >= 2 && p.connected() && p.stats().connects == 1);
        p.stop();
    }
    {
        FakeBroker b;
        b.script().answerPing = false;
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.keepAliveSec = 1;
        mq::Publisher p(o);
        CHECK(p.start());
        CHECK(waitFor([&] { return p.stats().reconnects >= 1; }, 6000));  // no PINGRESP within the keep-alive: the connection is dropped and re-made
        CHECK(p.stats().connects >= 2);
        p.stop();
    }
    {
        FakeBroker b;  // keep-alive 0: no PINGREQ at all
        CHECK(b.start());
        mq::Options o = baseOpt(b.port());
        o.keepAliveSec = 0;
        mq::Publisher p(o);
        CHECK(p.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(b.countType(12) == 0 && p.connected());
        p.stop();
    }
}

void testDropOldestWhileAway() {
    // a port with nothing on it
    FakeBroker probe;
    CHECK(probe.start());
    const int port = probe.port();
    probe.stop();
    mq::Options o = baseOpt(port);
    o.queueSlots = 64;
    mq::Publisher p(o);
    CHECK(p.start());
    for (int i = 0; i < 1000; ++i) { p.publish("t/q", std::to_string(i), 0); }
    CHECK(waitFor([&] { return p.stats().connectFailures >= 1; }, 2000));
    CHECK(p.stats().droppedOldest == 936 && p.stats().droppedNewest == 0 && p.stats().offered == 1000);
    // the broker appears: exactly the newest 64 arrive, in order
    FakeBroker b;
    CHECK(b.start(port));
    CHECK(waitFor([&] { return p.stats().sentQos0 == 64; }, 5000));
    p.stop();
    std::vector<int> got;
    for (const Packet& k : b.packets()) { if (k.type == 3) { got.push_back(std::atoi(k.payload.c_str())); } }
    CHECK(got.size() == 64);
    for (std::size_t i = 0; i < got.size(); ++i) { CHECK(got[i] == 936 + static_cast<int>(i)); }
    std::printf("broker away: 1000 offered into 64 slots, %llu dropped (oldest first), the newest 64 delivered in order\n",
                static_cast<unsigned long long>(p.stats().droppedOldest));
}

void testNoAllocation() {
    FakeBroker probe;
    CHECK(probe.start());
    const int port = probe.port();
    probe.stop();
    mq::Options o = baseOpt(port);
    o.queueSlots = 32;
    mq::Publisher p(o);  // not started: the ring fills and wraps
    std::string payload(300, 'z');
    p.publish("t/a", payload);
    AllocProbe probeNews;
    for (int i = 0; i < 5000; ++i) { p.publish("foxsdr/decode/ft8", payload, i % 2, (i % 3) == 0); }
    CHECK(p.publish("t/#", "x") == mq::PublishResult::Invalid);
    CHECK(p.publish("t/a", std::string_view(payload.data(), 10)) == mq::PublishResult::Accepted);
    const long news = probeNews.news();
    CHECK(news == 0);
    CHECK(p.stats().droppedOldest > 4900);
}

void testStress() {
    FakeBroker b;
    CHECK(b.start());
    mq::Options o = baseOpt(b.port());
    o.queueSlots = 128;
    mq::Publisher p(o);
    CHECK(p.start());
    CHECK(waitFor([&] { return p.connected(); }, 2000));
    const int N = 30000;
    std::thread prod([&] {
        for (int i = 0; i < N; ++i) {
            const std::string s = std::to_string(i);
            p.publish("t/s", s + ":" + s + ":" + s, 0);
            if ((i % 256) == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
        }
    });
    prod.join();
    p.stop();  // flushes
    const mq::Stats s = p.stats();
    std::vector<Packet> pubs;
    for (const Packet& k : b.packets()) { if (k.type == 3) { pubs.push_back(k); } }
    long long prev = -1;
    bool order = true, intact = true;
    for (const Packet& k : pubs) {
        const std::size_t c1 = k.payload.find(':');
        const std::size_t c2 = k.payload.find(':', c1 + 1);
        const long long a = std::atoll(k.payload.substr(0, c1).c_str());
        const long long bb = std::atoll(k.payload.substr(c1 + 1, c2 - c1 - 1).c_str());
        const long long cc = std::atoll(k.payload.substr(c2 + 1).c_str());
        if (a != bb || bb != cc) { intact = false; }  // a torn slot would mix two messages
        if (a <= prev) { order = false; }
        prev = a;
    }
    std::printf("stress: offered %llu, delivered %zu, dropped oldest %llu, dropped newest %llu\n", static_cast<unsigned long long>(s.offered), pubs.size(),
                static_cast<unsigned long long>(s.droppedOldest), static_cast<unsigned long long>(s.droppedNewest));
    CHECK(order && intact);
    CHECK(s.offered == static_cast<std::uint64_t>(N));
    CHECK(pubs.size() + s.droppedOldest + s.droppedNewest == s.offered);  // nothing lost silently, nothing delivered twice
    CHECK(pubs.size() > 1000);
}

}  // namespace

int main() {
    testCodec();
    testReader();
    testDisabledAndInvalid();
    testHappyPath();
    testRefusedAndBackoff();
    testHostileBroker();
    testQos1Resend();
    testKeepAlive();
    testDropOldestWhileAway();
    testNoAllocation();
    testStress();
    return testSummary("test_mqtt_publisher");
}
