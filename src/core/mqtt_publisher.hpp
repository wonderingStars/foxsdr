// A PUBLISH-ONLY MQTT 3.1.1 client for decoded messages: CONNECT (client id, optional user name and password, keep-alive), PUBLISH at QoS 0 and
// QoS 1 (PUBACK handling, re-send with DUP), the retain flag, PINGREQ, clean DISCONNECT, reconnect with back-off.
//
// SOURCE: OASIS MQTT Version 3.1.1, OASIS Standard, 29 October 2014, https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html (read
// 2026-11-11; section 4.4's text on re-sending was beyond what was read and is implemented from the packet-identifier and DUP rules in 2.3.1 and
// 3.3.1). No MQTT software was read.
//
// PLAIN TCP ONLY. There is NO TLS here (no TLS library is linked for this block): the user name and PASSWORD CROSS THE NETWORK IN CLEAR TEXT. Use
// it on a trusted LAN or through a TLS-terminating proxy or stunnel on the local machine; the wiring's settings text must say so.
//
// WHAT IT DOES NOT DO: subscribe, QoS 2, will messages, MQTT 5, a persistent session (CleanSession is 1 by default; with 0 only the client side
// remembers). A broker that sends anything but CONNACK, PUBACK or PINGRESP (a PUBLISH, say, which a publish-only client never asked for) is
// treated as a protocol error: the connection is closed and re-opened. A reply is never buffered: the reader holds at most the two bytes of a
// CONNACK or PUBACK body, so a hostile length cannot make it allocate.
//
// THREADS. publish() is called from ONE producing thread (never the DSP thread's loop of a hard real-time kind: it is wait-free and allocation-free,
// but it does take the microseconds to copy a message); everything on the network happens on the publisher's own worker thread. Messages go
// through a bounded ring of fixed-size slots; when the broker is away (or slow) the ring fills and the OLDEST message is overwritten and counted
// (droppedOldest). In the rare instant the worker is copying the oldest slot out, the NEW message is dropped instead and counted (droppedNewest).
// Off by default: with enabled == false start() does nothing and publish() returns Disabled.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/decode_json.hpp"
#include "core/power_log.hpp"  // the loopback-capable socket helpers (detail::dial and friends); no Influx code is used

namespace cascade::core::mqtt {

constexpr std::size_t kMaxRemainingLength = 268435455;  // 4.2.2.3 / Table 2.4: four bytes of 7 bits

// ---- codec (pure) ---------------------------------------------------------------------------------------------------------------------

inline bool validUtf8String(std::string_view s) {
    if (s.size() > 65535) { return false; }  // 1.5.3: at most 65,535 bytes
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '\0') { return false; }  // 1.5.3: U+0000 must not appear
        const std::size_t n = decodejson::detail::utf8Len(s, i);
        if (n == 0) { return false; }
        i += n;
    }
    return true;
}
// 3.3.2.1 / 4.7: a topic NAME in a PUBLISH is non-empty, has no wildcard characters, and is valid UTF-8.
inline bool validTopicName(std::string_view t) {
    return !t.empty() && t.find_first_of("#+") == std::string_view::npos && validUtf8String(t);
}

inline bool encodeRemainingLength(std::size_t n, std::string* out) {
    if (n > kMaxRemainingLength) { return false; }
    do {
        unsigned char d = static_cast<unsigned char>(n % 128);
        n /= 128;
        if (n > 0) { d |= 0x80; }
        out->push_back(static_cast<char>(d));
    } while (n > 0);
    return true;
}

enum class RlStatus { Ok, NeedMore, Malformed };
inline RlStatus decodeRemainingLength(const unsigned char* p, std::size_t n, std::size_t* value, std::size_t* used) {
    std::size_t mult = 1, v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i >= n) { return RlStatus::NeedMore; }
        v += static_cast<std::size_t>(p[i] & 127) * mult;
        if ((p[i] & 128) == 0) {
            *value = v;
            *used = i + 1;
            return RlStatus::Ok;
        }
        mult *= 128;
    }
    return RlStatus::Malformed;  // a fifth continuation byte (2.2.3)
}

inline void putU16(std::string& o, std::size_t v) {
    o.push_back(static_cast<char>((v >> 8) & 0xFF));
    o.push_back(static_cast<char>(v & 0xFF));
}
inline void putString(std::string& o, std::string_view s) {
    putU16(o, s.size());
    o.append(s.data(), s.size());
}

struct ConnectOptions {
    std::string clientId;
    std::string username;       // empty: none
    std::string password;       // sent only together with a user name (3.1.2-22)
    std::uint16_t keepAliveSec = 30;
    bool cleanSession = true;
};

// CONNECT (3.1). False with *err set for a client id that is empty with CleanSession 0, or a string that is not valid.
inline bool buildConnect(const ConnectOptions& c, std::string* out, std::string* err) {
    if (!validUtf8String(c.clientId) || (c.clientId.empty() && !c.cleanSession)) { *err = "bad client id"; return false; }
    if (!validUtf8String(c.username) || c.password.size() > 65535) { *err = "bad user name or password"; return false; }
    std::string body;
    putString(body, "MQTT");
    body.push_back(4);  // protocol level 4 = 3.1.1
    unsigned flags = c.cleanSession ? 0x02u : 0u;
    if (!c.username.empty()) {
        flags |= 0x80u;
        if (!c.password.empty()) { flags |= 0x40u; }
    }
    body.push_back(static_cast<char>(flags));
    putU16(body, c.keepAliveSec);
    putString(body, c.clientId);
    if (!c.username.empty()) {
        putString(body, c.username);
        if (!c.password.empty()) { putString(body, c.password); }
    }
    out->assign(1, static_cast<char>(0x10));
    if (!encodeRemainingLength(body.size(), out)) { *err = "too long"; return false; }
    out->append(body);
    return true;
}

// PUBLISH (3.3) at QoS 0 or 1. packetId must be 1..65535 for QoS 1 and is not written for QoS 0.
inline bool buildPublish(std::string_view topic, std::string_view payload, int qos, bool retain, bool dup, std::uint16_t packetId, std::string* out,
                         std::string* err) {
    if (qos != 0 && qos != 1) { *err = "only QoS 0 and 1"; return false; }
    if (!validTopicName(topic)) { *err = "bad topic"; return false; }
    if (qos == 1 && packetId == 0) { *err = "packet id 0"; return false; }
    const std::size_t rl = 2 + topic.size() + (qos > 0 ? 2 : 0) + payload.size();
    if (rl > kMaxRemainingLength) { *err = "payload too large"; return false; }
    unsigned b0 = 0x30u | (retain ? 1u : 0u) | (static_cast<unsigned>(qos) << 1) | ((dup && qos > 0) ? 0x08u : 0u);  // DUP must be 0 for QoS 0 (3.3.1-2)
    out->assign(1, static_cast<char>(b0));
    encodeRemainingLength(rl, out);
    putString(*out, topic);
    if (qos > 0) { putU16(*out, packetId); }
    out->append(payload.data(), payload.size());
    return true;
}
inline std::string pingReq() { return std::string("\xC0\x00", 2); }
inline std::string disconnectPacket() { return std::string("\xE0\x00", 2); }
inline std::string pubAck(std::uint16_t id) { std::string s("\x40\x02", 2); putU16(s, id); return s; }

// What a publish-only client may receive.
struct Incoming {
    enum class Type { ConnAck, PubAck, PingResp } type = Type::ConnAck;
    bool sessionPresent = false;
    std::uint8_t returnCode = 0;   // CONNACK
    std::uint16_t packetId = 0;    // PUBACK
};

// A bounded incremental reader: it accepts only CONNACK (RL 2, flags 0), PUBACK (RL 2, flags 0) and PINGRESP (RL 0, flags 0). Any other packet
// type, flag, length or a malformed length is an Error at the first byte that shows it; nothing is ever buffered beyond two body bytes.
class BrokerReader {
public:
    enum class Status { NeedMore, Packet, Error };
    // Consumes bytes from data until a packet completes, an error, or the data runs out; *consumed says how many were used.
    Status feed(const unsigned char* data, std::size_t n, std::size_t* consumed, Incoming* out) {
        std::size_t i = 0;
        while (i < n) {
            const unsigned char b = data[i++];
            switch (state_) {
                case State::Header:
                    switch (b) {
                        case 0x20: expect_ = 2; type_ = Incoming::Type::ConnAck; break;
                        case 0x40: expect_ = 2; type_ = Incoming::Type::PubAck; break;
                        case 0xD0: expect_ = 0; type_ = Incoming::Type::PingResp; break;
                        default: *consumed = i; return fail();
                    }
                    state_ = State::Length;
                    break;
                case State::Length:
                    if (b != expect_) { *consumed = i; return fail(); }  // includes every continuation byte (>= 0x80) and every wrong length
                    got_ = 0;
                    if (expect_ == 0) { *consumed = i; return done(out); }
                    state_ = State::Body;
                    break;
                case State::Body:
                    body_[got_++] = b;
                    if (got_ == expect_) { *consumed = i; return done(out); }
                    break;
                case State::Dead:
                    *consumed = n;
                    return Status::Error;
            }
        }
        *consumed = i;
        return Status::NeedMore;
    }
    void reset() { state_ = State::Header; got_ = 0; }

private:
    enum class State { Header, Length, Body, Dead };
    Status fail() { state_ = State::Dead; return Status::Error; }
    Status done(Incoming* out) {
        out->type = type_;
        if (type_ == Incoming::Type::ConnAck) {
            if ((body_[0] & 0xFE) != 0) { return fail(); }  // reserved bits of the acknowledge flags
            out->sessionPresent = (body_[0] & 1) != 0;
            out->returnCode = body_[1];
        } else if (type_ == Incoming::Type::PubAck) {
            out->packetId = static_cast<std::uint16_t>((body_[0] << 8) | body_[1]);
        }
        state_ = State::Header;
        return Status::Packet;
    }
    State state_ = State::Header;
    Incoming::Type type_ = Incoming::Type::ConnAck;
    unsigned char expect_ = 0;
    unsigned char body_[2] = {0, 0};
    unsigned char got_ = 0;
};

inline const char* connackText(int code) {
    switch (code) {
        case 0: return "accepted";
        case 1: return "unacceptable protocol version";
        case 2: return "client identifier rejected";
        case 3: return "server unavailable";
        case 4: return "bad user name or password";
        case 5: return "not authorized";
        default: return "unknown return code";
    }
}

// ---- the publisher ------------------------------------------------------------------------------------------------------------------------

struct Options {
    bool enabled = false;
    std::string host;
    int port = 1883;
    std::string clientId;               // empty: "foxsdr-" + 8 hex digits from the clock
    std::string username, password;     // password only with a user name
    int keepAliveSec = 30;              // 0 = no keep-alive
    bool cleanSession = true;
    std::size_t queueSlots = 256;       // messages held while the broker is away
    std::size_t slotBytes = 2048;       // topic + payload per message
    std::size_t maxInflight = 8;        // unacknowledged QoS 1 messages
    int connectTimeoutMs = 3000;
    int ackTimeoutMs = 4000;            // QoS 1 re-send after this
    int backoffMinMs = 500, backoffMaxMs = 60000;
    int pollMs = 20;
    int flushMs = 500;                  // stop() tries this long to deliver what is queued before DISCONNECT
};

struct Stats {
    std::uint64_t offered = 0, rejected = 0, droppedOldest = 0, droppedNewest = 0;
    std::uint64_t sentQos0 = 0, sentQos1 = 0, acked = 0, resent = 0;
    std::uint64_t connects = 0, connectFailures = 0, reconnects = 0, pings = 0, protocolErrors = 0, bytesOut = 0;
};

enum class PublishResult { Accepted, Disabled, Invalid, TooLarge };

class Publisher {
public:
    explicit Publisher(const Options& o) : opt_(o) {
        opt_.queueSlots = std::clamp<std::size_t>(opt_.queueSlots, 2, 65536);
        opt_.slotBytes = std::clamp<std::size_t>(opt_.slotBytes, 64, 1u << 20);
        opt_.maxInflight = std::clamp<std::size_t>(opt_.maxInflight, 1, 1024);
        opt_.keepAliveSec = std::clamp(opt_.keepAliveSec, 0, 65535);
        slots_ = std::vector<Slot>(opt_.queueSlots);
        buf_.assign(opt_.queueSlots * opt_.slotBytes, 0);
        for (auto& s : slots_) { s.state.store(kEmpty); }
        if (opt_.clientId.empty()) {
            char b[24];
            std::snprintf(b, sizeof b, "foxsdr-%08x", static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu));
            opt_.clientId = b;
        }
    }
    ~Publisher() { stop(); }
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // Starts the worker. False when disabled, already running, or the options cannot make a CONNECT.
    bool start() {
        if (!opt_.enabled || worker_.joinable()) { return false; }
        if (opt_.host.empty() || opt_.port < 1 || opt_.port > 65535) { return false; }
        ConnectOptions c;
        c.clientId = opt_.clientId;
        c.username = opt_.username;
        c.password = opt_.password;
        c.keepAliveSec = static_cast<std::uint16_t>(opt_.keepAliveSec);
        c.cleanSession = opt_.cleanSession;
        std::string err;
        if (!buildConnect(c, &connectPacket_, &err)) { setError(err); return false; }
        if (opt_.host.find_first_of("\r\n ") != std::string::npos) { setError("bad host"); return false; }
        stopping_ = false;
        worker_ = std::thread([this] { run(); });
        return true;
    }
    // Clean shutdown: tries to deliver what is queued (up to flushMs), sends DISCONNECT, closes, joins.
    void stop() {
        stopping_ = true;
        if (worker_.joinable()) { worker_.join(); }
    }

    // PRODUCER THREAD (one). Wait-free and allocation-free. QoS 0 or 1; the topic must be a valid topic NAME; topic + payload must fit a slot.
    PublishResult publish(std::string_view topic, std::string_view payload, int qos = 0, bool retain = false) {
        if (!opt_.enabled) { return PublishResult::Disabled; }
        if ((qos != 0 && qos != 1) || !validTopicName(topic)) { rejected_.fetch_add(1, std::memory_order_relaxed); return PublishResult::Invalid; }
        if (topic.size() + payload.size() > opt_.slotBytes) { rejected_.fetch_add(1, std::memory_order_relaxed); return PublishResult::TooLarge; }
        offered_.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        Slot& s = slots_[static_cast<std::size_t>(h % slots_.size())];
        int st = s.state.load(std::memory_order_acquire);
        for (;;) {
            if (st == kReading) {  // the worker is copying the oldest message out right now: this one is dropped instead
                droppedNewest_.fetch_add(1, std::memory_order_relaxed);
                return PublishResult::Accepted;
            }
            if (s.state.compare_exchange_weak(st, kWriting, std::memory_order_acq_rel)) { break; }
        }
        if (st == kFull) { droppedOldest_.fetch_add(1, std::memory_order_relaxed); }
        char* p = buf_.data() + static_cast<std::size_t>(h % slots_.size()) * opt_.slotBytes;
        std::memcpy(p, topic.data(), topic.size());
        std::memcpy(p + topic.size(), payload.data(), payload.size());
        s.topicLen = static_cast<std::uint32_t>(topic.size());
        s.payloadLen = static_cast<std::uint32_t>(payload.size());
        s.qos = static_cast<std::uint8_t>(qos);
        s.retain = retain;
        s.seq = h;
        s.state.store(kFull, std::memory_order_release);
        head_.store(h + 1, std::memory_order_release);
        return PublishResult::Accepted;
    }

    Stats stats() const {
        Stats s;
        s.offered = offered_.load(); s.rejected = rejected_.load(); s.droppedOldest = droppedOldest_.load(); s.droppedNewest = droppedNewest_.load();
        s.sentQos0 = sentQos0_.load(); s.sentQos1 = sentQos1_.load(); s.acked = acked_.load(); s.resent = resent_.load();
        s.connects = connects_.load(); s.connectFailures = connectFailures_.load(); s.reconnects = reconnects_.load(); s.pings = pings_.load();
        s.protocolErrors = protocolErrors_.load(); s.bytesOut = bytesOut_.load();
        return s;
    }
    bool connected() const { return connected_.load(); }
    std::string lastError() const { std::lock_guard<std::mutex> lk(errMu_); return err_; }
    const Options& options() const { return opt_; }

private:
    using detail_sock = powerlog::detail::sock_t;
    static constexpr int kEmpty = 0, kFull = 1, kReading = 2, kWriting = 3;
    struct Slot {
        std::atomic<int> state{kEmpty};
        std::uint64_t seq = 0;
        std::uint32_t topicLen = 0, payloadLen = 0;
        std::uint8_t qos = 0;
        bool retain = false;
    };
    struct Message { std::string topic, payload; int qos = 0; bool retain = false; };
    struct Inflight { std::uint16_t id; std::string packet; std::chrono::steady_clock::time_point sentAt; int attempts; };
    using Clock = std::chrono::steady_clock;

    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(errMu_); err_ = e; }

    // WORKER: the oldest message still in the ring, or false.
    bool pop(Message* m) {
        for (;;) {
            const std::uint64_t head = head_.load(std::memory_order_acquire);
            if (tail_ >= head) { return false; }
            Slot& s = slots_[static_cast<std::size_t>(tail_ % slots_.size())];
            int expected = kFull;
            if (!s.state.compare_exchange_strong(expected, kReading, std::memory_order_acq_rel)) {
                if (expected == kWriting) { return false; }  // the producer is mid-write on it: try again on the next pass
                ++tail_;                                      // (kEmpty cannot happen for a slot behind head; skip defensively)
                continue;
            }
            if (s.seq != tail_) {                              // overwritten by a newer message: the one we wanted was dropped (counted by the producer)
                s.state.store(kFull, std::memory_order_release);
                ++tail_;
                continue;
            }
            const char* p = buf_.data() + static_cast<std::size_t>(tail_ % slots_.size()) * opt_.slotBytes;
            m->topic.assign(p, s.topicLen);
            m->payload.assign(p + s.topicLen, s.payloadLen);
            m->qos = s.qos;
            m->retain = s.retain;
            s.state.store(kEmpty, std::memory_order_release);
            ++tail_;
            return true;
        }
    }
    bool ringEmpty() const { return tail_ >= head_.load(std::memory_order_acquire); }

    bool sendAll(detail_sock sock, const std::string& p) {
        std::size_t off = 0;
        while (off < p.size()) {
            const int n = ::send(sock, p.data() + off, static_cast<int>(p.size() - off), 0);
            if (n <= 0) { return false; }
            off += static_cast<std::size_t>(n);
        }
        bytesOut_.fetch_add(p.size(), std::memory_order_relaxed);
        lastSend_ = Clock::now();
        return true;
    }
    // Waits up to ms for data; 1 readable, 0 timeout, -1 error.
    static int waitReadable(detail_sock sock, int ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(sock, &r);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
#if defined(_WIN32)
        const int n = ::select(0, &r, nullptr, nullptr, &tv);
#else
        const int n = ::select(sock + 1, &r, nullptr, nullptr, &tv);
#endif
        return n < 0 ? -1 : (n == 0 ? 0 : 1);
    }
    // Reads once into buf; >0 bytes, 0 closed, -1 error.
    static int recvSome(detail_sock sock, unsigned char* buf, int cap) { return ::recv(sock, reinterpret_cast<char*>(buf), cap, 0); }

    void sleepSlices(int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (!stopping_.load() && Clock::now() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20, std::max(1, ms)))); }
    }

    // One connection from dial to loss. Returns when the connection ends or stopping_.
    void session(detail_sock sock) {
        BrokerReader reader;
        Incoming in;
        // CONNACK
        if (!sendAll(sock, connectPacket_)) { connectFailures_.fetch_add(1); setError("could not send CONNECT"); return; }
        const auto deadline = Clock::now() + std::chrono::milliseconds(opt_.connectTimeoutMs);
        bool acked = false;
        unsigned char rb[256];
        while (!acked) {
            const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left <= 0 || stopping_.load()) { connectFailures_.fetch_add(1); setError("no CONNACK"); return; }
            const int w = waitReadable(sock, std::min(left, 50));
            if (w < 0) { connectFailures_.fetch_add(1); setError("socket error"); return; }
            if (w == 0) { continue; }
            const int n = recvSome(sock, rb, sizeof rb);
            if (n <= 0) { connectFailures_.fetch_add(1); setError("closed before CONNACK"); return; }
            std::size_t used = 0;
            const auto st = reader.feed(rb, static_cast<std::size_t>(n), &used, &in);
            if (st == BrokerReader::Status::Error || (st == BrokerReader::Status::Packet && in.type != Incoming::Type::ConnAck)) {
                protocolErrors_.fetch_add(1); connectFailures_.fetch_add(1); setError("bad reply to CONNECT"); return;
            }
            if (st == BrokerReader::Status::Packet) {
                if (in.returnCode != 0) {
                    connectFailures_.fetch_add(1);
                    setError(std::string("CONNECT refused: ") + connackText(in.returnCode));
                    return;
                }
                acked = true;
                // bytes after the CONNACK in the same read are fed on below
                if (used < static_cast<std::size_t>(n)) { pendingBytes_.assign(reinterpret_cast<char*>(rb) + used, static_cast<std::size_t>(n) - used); }
            }
        }
        connects_.fetch_add(1);
        connected_.store(true);
        backoffMs_ = opt_.backoffMinMs;
        setError("");
        pingOutstanding_ = false;
        lastSend_ = Clock::now();
        // re-send what was unacknowledged on the last connection, with DUP set and the same packet identifiers (2.3.1, 3.3.1-1)
        for (Inflight& f : inflight_) {
            f.packet[0] = static_cast<char>(static_cast<unsigned char>(f.packet[0]) | 0x08);
            if (!sendAll(sock, f.packet)) { connected_.store(false); return; }
            f.sentAt = Clock::now();
            resent_.fetch_add(1);
        }
        std::chrono::steady_clock::time_point flushEnd{};
        bool flushing = false;
        for (;;) {
            if (stopping_.load() && !flushing) {
                flushing = true;
                flushEnd = Clock::now() + std::chrono::milliseconds(opt_.flushMs);
            }
            if (flushing && ((ringEmpty() && inflight_.empty()) || Clock::now() >= flushEnd)) {
                sendAll(sock, disconnectPacket());
                break;
            }
            // send what is queued, up to the inflight window
            Message m;
            int budget = 64;
            while (budget-- > 0 && inflight_.size() < opt_.maxInflight && pop(&m)) {
                std::string pkt, err;
                if (m.qos == 0) {
                    if (!buildPublish(m.topic, m.payload, 0, m.retain, false, 0, &pkt, &err)) { continue; }
                    if (!sendAll(sock, pkt)) { connected_.store(false); return; }
                    sentQos0_.fetch_add(1);
                } else {
                    const std::uint16_t id = nextId();
                    if (!buildPublish(m.topic, m.payload, 1, m.retain, false, id, &pkt, &err)) { continue; }
                    if (!sendAll(sock, pkt)) {
                        inflight_.push_back({id, pkt, Clock::now(), 1});  // keep it: it is re-sent after the reconnect
                        connected_.store(false);
                        return;
                    }
                    inflight_.push_back({id, std::move(pkt), Clock::now(), 1});
                    sentQos1_.fetch_add(1);
                }
            }
            // read
            const int w = waitReadable(sock, opt_.pollMs);
            if (w < 0) { connected_.store(false); setError("socket error"); return; }
            if (w > 0 || !pendingBytes_.empty()) {
                int n;
                if (!pendingBytes_.empty()) {
                    n = static_cast<int>(std::min(pendingBytes_.size(), sizeof rb));
                    std::memcpy(rb, pendingBytes_.data(), static_cast<std::size_t>(n));
                    pendingBytes_.erase(0, static_cast<std::size_t>(n));
                } else {
                    n = recvSome(sock, rb, sizeof rb);
                }
                if (n <= 0) { connected_.store(false); setError("connection closed by the broker"); return; }
                std::size_t off = 0;
                while (off < static_cast<std::size_t>(n)) {
                    std::size_t used = 0;
                    const auto st = reader.feed(rb + off, static_cast<std::size_t>(n) - off, &used, &in);
                    off += used;
                    if (st == BrokerReader::Status::Error) {
                        protocolErrors_.fetch_add(1);
                        connected_.store(false);
                        setError("protocol error from the broker");
                        return;
                    }
                    if (st == BrokerReader::Status::Packet) {
                        if (in.type == Incoming::Type::PubAck) {
                            for (std::size_t i = 0; i < inflight_.size(); ++i) {
                                if (inflight_[i].id == in.packetId) {
                                    inflight_.erase(inflight_.begin() + static_cast<std::ptrdiff_t>(i));
                                    acked_.fetch_add(1);
                                    break;
                                }
                            }  // an acknowledgement for an identifier we do not hold is ignored
                        } else if (in.type == Incoming::Type::PingResp) {
                            pingOutstanding_ = false;
                        } else {  // a second CONNACK
                            protocolErrors_.fetch_add(1);
                            connected_.store(false);
                            setError("unexpected CONNACK");
                            return;
                        }
                    }
                }
            }
            // re-send overdue QoS 1
            const auto now = Clock::now();
            for (Inflight& f : inflight_) {
                if (now - f.sentAt >= std::chrono::milliseconds(opt_.ackTimeoutMs)) {
                    if (f.attempts >= 3) { connected_.store(false); setError("no PUBACK after 3 tries"); return; }
                    f.packet[0] = static_cast<char>(static_cast<unsigned char>(f.packet[0]) | 0x08);
                    if (!sendAll(sock, f.packet)) { connected_.store(false); return; }
                    f.sentAt = now;
                    ++f.attempts;
                    resent_.fetch_add(1);
                }
            }
            // keep-alive
            if (opt_.keepAliveSec > 0) {
                const auto idle = now - lastSend_;
                if (!pingOutstanding_ && idle >= std::chrono::milliseconds(opt_.keepAliveSec * 500)) {
                    if (!sendAll(sock, pingReq())) { connected_.store(false); return; }
                    pingOutstanding_ = true;
                    pingSent_ = now;
                    pings_.fetch_add(1);
                } else if (pingOutstanding_ && now - pingSent_ >= std::chrono::milliseconds(opt_.keepAliveSec * 1000)) {
                    connected_.store(false);
                    setError("no PINGRESP");
                    return;
                }
            }
        }
    }

    std::uint16_t nextId() {
        for (int guard = 0; guard < 70000; ++guard) {
            idCounter_ = static_cast<std::uint16_t>(idCounter_ == 65535 ? 1 : idCounter_ + 1);
            bool used = false;
            for (const Inflight& f : inflight_) { if (f.id == idCounter_) { used = true; break; } }
            if (!used) { return idCounter_; }
        }
        return 1;
    }

    void run() {
        backoffMs_ = opt_.backoffMinMs;
        bool everConnected = false;
        while (!stopping_.load()) {
            powerlog::InfluxTarget t;  // only used for its host / port / timeout by the shared dial helper
            t.enabled = true;
            t.host = opt_.host;
            t.port = opt_.port;
            t.timeoutMs = opt_.connectTimeoutMs;
            std::string err;
            powerlog::detail::sock_t sock = powerlog::detail::dial(t, false, &err);
            if (powerlog::detail::badSock(sock)) {
                connectFailures_.fetch_add(1);
                setError(err);
            } else {
                if (everConnected) { reconnects_.fetch_add(1); }
                reader_reset();
                session(sock);
                connected_.store(false);
                if (connects_.load() > 0) { everConnected = true; }
                powerlog::detail::closeSock(sock);
                if (stopping_.load()) { break; }
            }
            // back off before the next attempt: min, 2 min, 4 min ... up to max
            sleepSlices(backoffMs_);
            backoffMs_ = std::min(opt_.backoffMaxMs, backoffMs_ * 2);
        }
        connected_.store(false);
    }
    void reader_reset() { pendingBytes_.clear(); }

    Options opt_;
    std::string connectPacket_;
    std::vector<Slot> slots_;
    std::vector<char> buf_;
    std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_ = 0;  // worker's
    std::thread worker_;
    std::atomic<bool> stopping_{false}, connected_{false};
    std::vector<Inflight> inflight_;  // worker's
    std::string pendingBytes_;        // worker's
    std::uint16_t idCounter_ = 0;
    int backoffMs_ = 500;
    Clock::time_point lastSend_{}, pingSent_{};
    bool pingOutstanding_ = false;
    mutable std::mutex errMu_;
    std::string err_;
    std::atomic<std::uint64_t> offered_{0}, rejected_{0}, droppedOldest_{0}, droppedNewest_{0}, sentQos0_{0}, sentQos1_{0}, acked_{0}, resent_{0},
        connects_{0}, connectFailures_{0}, reconnects_{0}, pings_{0}, protocolErrors_{0}, bytesOut_{0};
};

}  // namespace cascade::core::mqtt
