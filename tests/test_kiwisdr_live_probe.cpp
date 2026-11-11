// test_kiwisdr_live_probe.cpp - the ONE-SHOT LIVE PROBE for the KiwiSDR client, and its
// self-test.
//
//   test_kiwisdr_live_probe                         self-test against the stand-in
//                                                   (what ctest runs; no network)
//   test_kiwisdr_live_probe --live HOST PORT KHZ OUTDIR
//                                                   ONE listen-only connection to a real
//                                                   receiver (owner-approved, once)
//
// HOW IT RECORDS "EVERY BYTE". The client class is used as it ships; it cannot be asked for
// its raw bytes, so the probe puts a transparent one-connection relay on 127.0.0.1 between
// the client and the receiver. The relay accepts exactly ONE client connection (the listener
// is closed after it), makes exactly ONE upstream TCP connection, copies bytes both ways, and
// records them. The only byte it changes is the Host header of the HTTP upgrade request
// (127.0.0.1:<relay port> becomes the real host:port, as the client would have sent it
// directly); the log says so. Files written to OUTDIR:
//
//   wire.log      time offset, direction, TCP chunk sizes, every WebSocket frame (text frames
//                 as text, binary frames as hex of the first 64 bytes plus length; MSG frames
//                 also as text), the probe's own notes, the client's state once a second
//   c2s.raw       every byte the client sent (after the Host rewrite)
//   s2c.raw       every byte the receiver sent (HTTP response included)
//   frames.bin    every WebSocket frame, unmasked: u8 dir(0 c2s,1 s2c), u8 opcode|0x80 if FIN,
//                 u32 LE length, u64 LE microseconds since start, payload. THE UNDECODED PAYLOADS.
//   audio.wav     what the client decoded (mono 16-bit at the client's ASSUMED rate)
//   summary.txt   the final counters
//
// SAFETY, ALL OF IT IN THIS FILE OR THE CLIENT: identity string "FoxSDR-test" and nothing else
// personal; no password is ever sent (an empty one); at most 45 s connected, then close(); a
// hard timer ends the process at 60 s whatever happens; on any failure it logs and exits, no
// retry, no second connection.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kiwisdr_fake_server.hpp"
#include "source/kiwisdr_client.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/stat.h>
#endif

using cascade::source::KiwiSdrClient;
using kiwifake::closeSock;
using kiwifake::Sock;
using kiwifake::shutSock;
using ms = std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kIdentity = "FoxSDR-test";

struct Config {
    std::string host;
    std::uint16_t port = 0;
    double freqHz = 0.0;
    std::string outDir;
    int durationS = 45;   // 45 s at most in live mode
    int hardStopS = 60;   // the hard timer
    int tuneLow = -5000;  // AM
    int tuneHigh = 5000;
};

struct Outcome {
    bool opened = false;
    KiwiSdrClient::State state = KiwiSdrClient::State::Idle;
    std::string detail;
    std::uint64_t frames = 0, samples = 0, gaps = 0, missed = 0, ignored = 0, keepAlives = 0;
    std::uint64_t upstreamConnections = 0;
    double seconds = 0.0;
    int wavSamples = 0;
};

// --- the recorder -------------------------------------------------------------------------

class Recorder {
public:
    bool open(const std::string& dir) {
        dir_ = dir;
        log_ = std::fopen((dir + "/wire.log").c_str(), "wb");
        c2s_ = std::fopen((dir + "/c2s.raw").c_str(), "wb");
        s2c_ = std::fopen((dir + "/s2c.raw").c_str(), "wb");
        frames_ = std::fopen((dir + "/frames.bin").c_str(), "wb");
        t0_ = Clock::now();
        return log_ && c2s_ && s2c_ && frames_;
    }
    ~Recorder() {
        for (FILE* f : {log_, c2s_, s2c_, frames_}) {
            if (f) { std::fclose(f); }
        }
    }
    double secs() const { return std::chrono::duration<double>(Clock::now() - t0_).count(); }

    void line(const std::string& text) {
        std::lock_guard<std::mutex> lk(m_);
        std::fprintf(log_, "T+%8.3f %s\n", secs(), text.c_str());
        std::fflush(log_);
    }
    void raw(bool c2s, const char* p, std::size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        FILE* f = c2s ? c2s_ : s2c_;
        std::fwrite(p, 1, n, f);
        std::fflush(f);
    }
    void frame(bool c2s, std::uint8_t opcode, bool fin, const std::string& payload) {
        std::lock_guard<std::mutex> lk(m_);
        std::uint8_t h[14];
        h[0] = c2s ? 0 : 1;
        h[1] = static_cast<std::uint8_t>(opcode | (fin ? 0x80 : 0));
        const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
        for (int i = 0; i < 4; ++i) { h[2 + i] = static_cast<std::uint8_t>(n >> (8 * i)); }
        const std::uint64_t us = static_cast<std::uint64_t>(secs() * 1e6);
        for (int i = 0; i < 8; ++i) { h[6 + i] = static_cast<std::uint8_t>(us >> (8 * i)); }
        std::fwrite(h, 1, sizeof h, frames_);
        std::fwrite(payload.data(), 1, payload.size(), frames_);
        std::fflush(frames_);
    }
    const std::string& dir() const { return dir_; }

private:
    std::mutex m_;
    std::string dir_;
    FILE *log_ = nullptr, *c2s_ = nullptr, *s2c_ = nullptr, *frames_ = nullptr;
    Clock::time_point t0_;
};

std::string esc(const std::string& s, std::size_t cap) {
    std::string o;
    for (std::size_t i = 0; i < s.size() && i < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\\') { o += "\\\\"; }
        else if (c == '\r') { o += "\\r"; }
        else if (c == '\n') { o += "\\n"; }
        else if (c >= 32 && c < 127) { o.push_back(static_cast<char>(c)); }
        else { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
    }
    if (s.size() > cap) { o += "...(+" + std::to_string(s.size() - cap) + " bytes)"; }
    return o;
}

std::string hexOf(const std::string& s, std::size_t cap) {
    std::string o;
    for (std::size_t i = 0; i < s.size() && i < cap; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02x", static_cast<unsigned char>(s[i]));
        o += b;
        if (i + 1 < s.size() && i + 1 < cap) { o.push_back(' '); }
    }
    return o;
}

// --- WebSocket frame tap (one per direction) ---------------------------------------------

class WsTap {
public:
    WsTap(Recorder& r, bool c2s) : r_(r), c2s_(c2s) {}
    // Bytes AFTER the HTTP head has been handled by the relay.
    void feed(const char* p, std::size_t n) {
        if (dead_) { return; }
        buf_.append(p, n);
        for (;;) {
            if (buf_.size() < 2) { return; }
            const auto b0 = static_cast<std::uint8_t>(buf_[0]);
            const auto b1 = static_cast<std::uint8_t>(buf_[1]);
            const bool masked = (b1 & 0x80) != 0;
            std::uint64_t len = b1 & 0x7F;
            std::size_t hdr = 2;
            if (len == 126) {
                if (buf_.size() < 4) { return; }
                len = (std::uint64_t(static_cast<std::uint8_t>(buf_[2])) << 8) | static_cast<std::uint8_t>(buf_[3]);
                hdr = 4;
            } else if (len == 127) {
                if (buf_.size() < 10) { return; }
                len = 0;
                for (int i = 0; i < 8; ++i) { len = (len << 8) | static_cast<std::uint8_t>(buf_[2 + i]); }
                hdr = 10;
            }
            if (len > (16u << 20)) {
                r_.line(dir() + " FRAME-TOO-LARGE declared=" + std::to_string(len) + " (tap stops parsing)");
                dead_ = true;
                return;
            }
            const std::size_t mk = masked ? 4 : 0;
            if (buf_.size() < hdr + mk + len) { return; }
            std::string payload = buf_.substr(hdr + mk, static_cast<std::size_t>(len));
            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>(payload[i] ^ buf_[hdr + (i % 4)]); }
            }
            const std::uint8_t op = b0 & 0x0F;
            const bool fin = (b0 & 0x80) != 0;
            buf_.erase(0, hdr + mk + static_cast<std::size_t>(len));
            report(op, fin, masked, payload);
            r_.frame(c2s_, op, fin, payload);
        }
    }

private:
    std::string dir() const { return c2s_ ? "C>S" : "S>C"; }
    void report(std::uint8_t op, bool fin, bool masked, const std::string& p) {
        static const char* names[] = {"CONT", "TEXT", "BIN", "?3", "?4", "?5", "?6", "?7",
                                      "CLOSE", "PING", "PONG"};
        const std::string nm = op < 11 ? names[op] : ("OP" + std::to_string(op));
        std::string l = dir() + " FRAME " + nm + " fin=" + (fin ? "1" : "0") + " masked=" + (masked ? "1" : "0") +
                        " len=" + std::to_string(p.size());
        if (op == 1) {
            l += " text=\"" + esc(p, 600) + "\"";
        } else if (op == 2 || op == 0) {
            const std::string tag = p.size() >= 3 ? p.substr(0, 3) : p;
            l += " tag=\"" + esc(tag, 8) + "\" hex64=" + hexOf(p, 64);
            if (tag == "MSG") { l += " text=\"" + esc(p.substr(3), 800) + "\""; }
        } else if (op == 8) {
            l += " payload=" + hexOf(p, 64);
            if (p.size() > 2) { l += " reason=\"" + esc(p.substr(2), 200) + "\""; }
        } else {
            l += " hex64=" + hexOf(p, 64);
        }
        r_.line(l);
    }

    Recorder& r_;
    bool c2s_;
    bool dead_ = false;
    std::string buf_;
};

// --- the one-connection relay --------------------------------------------------------------

class Relay {
public:
    Relay(Recorder& r, std::string host, std::uint16_t port) : r_(r), host_(std::move(host)), port_(port) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ok_ = listen_ != kiwifake::kBadSock && ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
              ::listen(listen_, 1) == 0;
        if (ok_) {
            socklen_t len = sizeof a;
            ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
            localPort_ = ntohs(a.sin_port);
            th_ = std::thread([this] { run(); });
        }
    }
    ~Relay() { stop(); }
    bool ok() const { return ok_; }
    std::uint16_t localPort() const { return localPort_; }
    std::uint64_t upstreamConnections() const { return upstream_.load(); }

    void stop() {
        stopping_ = true;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (listen_ != kiwifake::kBadSock) { shutSock(listen_); }
            if (client_ != kiwifake::kBadSock) { shutSock(client_); }
            if (up_ != kiwifake::kBadSock) { shutSock(up_); }
        }
        if (th_.joinable()) { th_.join(); }
        std::lock_guard<std::mutex> lk(m_);
        closeSock(listen_);
        listen_ = kiwifake::kBadSock;
    }

private:
    Sock connectUpstream() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || !res) {
            r_.line("PROBE upstream: name lookup failed for " + host_);
            return kiwifake::kBadSock;
        }
        Sock s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == kiwifake::kBadSock) { ::freeaddrinfo(res); return s; }
        char ip[64] = {0};
        ::getnameinfo(res->ai_addr, static_cast<socklen_t>(res->ai_addrlen), ip, sizeof ip, nullptr, 0, NI_NUMERICHOST);
        r_.line(std::string("PROBE upstream: connecting to ") + ip + ":" + std::to_string(port_));
        const int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
        ::freeaddrinfo(res);
        if (rc != 0) {
            r_.line("PROBE upstream: connect failed");
            closeSock(s);
            return kiwifake::kBadSock;
        }
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        r_.line("PROBE upstream: TCP connected");
        return s;
    }

    static bool sendAll(Sock s, const char* p, std::size_t n) {
        std::size_t off = 0;
        while (off < n) {
            const int k = ::send(s, p + off, static_cast<int>(n - off), 0);
            if (k <= 0) { return false; }
            off += static_cast<std::size_t>(k);
        }
        return true;
    }

    void run() {
        // Exactly one client connection, then no more.
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listen_, &rd);
        timeval tv{20, 0};
        if (::select(static_cast<int>(listen_) + 1, &rd, nullptr, nullptr, &tv) <= 0 || stopping_) { return; }
        Sock c = ::accept(listen_, nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lk(m_);
            closeSock(listen_);  // no second connection can ever be accepted
            listen_ = kiwifake::kBadSock;
            client_ = c;
        }
        if (c == kiwifake::kBadSock) { return; }
        int one = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);

        Sock u = connectUpstream();
        if (u == kiwifake::kBadSock) {
            shutSock(c);
            closeSock(c);
            std::lock_guard<std::mutex> lk(m_);
            client_ = kiwifake::kBadSock;
            return;
        }
        ++upstream_;
        {
            std::lock_guard<std::mutex> lk(m_);
            up_ = u;
        }
        std::thread down([this, u, c] { pump(u, c, false); });
        pump(c, u, true);
        shutSock(u);
        shutSock(c);
        down.join();
        std::lock_guard<std::mutex> lk(m_);
        closeSock(u);
        closeSock(c);
        up_ = client_ = kiwifake::kBadSock;
    }

    // Copies from -> to. c2s: the first bytes are the HTTP upgrade request; held until its head
    // is complete so the Host line can be restored.
    void pump(Sock from, Sock to, bool c2s) {
        WsTap tap(r_, c2s);
        const std::string d = c2s ? "C>S" : "S>C";
        std::string head;
        bool headDone = false;
        char buf[16384];
        for (;;) {
            const int n = ::recv(from, buf, sizeof buf, 0);
            if (n <= 0) {
                r_.line(d + " TCP end of stream (recv=" + std::to_string(n) + ")");
                shutSock(to);  // pass the end on
                return;
            }
            r_.line(d + " TCP chunk " + std::to_string(n) + " bytes");
            if (!headDone) {
                head.append(buf, static_cast<std::size_t>(n));
                const std::size_t e = head.find("\r\n\r\n");
                if (e == std::string::npos) {
                    if (head.size() > 16384) { return; }
                    continue;
                }
                const std::size_t hl = e + 4;
                std::string rest = head.substr(hl);
                std::string h = head.substr(0, hl);
                if (c2s) {
                    const std::string want = "Host: 127.0.0.1:" + std::to_string(localPort_) + "\r\n";
                    const std::size_t p = h.find(want);
                    if (p != std::string::npos) {
                        const std::string real = "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
                        h.replace(p, want.size(), real);
                        r_.line("PROBE relay restored the Host header: \"" + esc(want, 80) + "\" -> \"" + esc(real, 80) + "\"");
                    } else {
                        r_.line("PROBE relay did not find the expected Host header to restore");
                    }
                }
                r_.line(d + " HTTP head \"" + esc(h, 4000) + "\"");
                r_.raw(c2s, h.data(), h.size());
                if (!sendAll(to, h.data(), h.size())) { return; }
                headDone = true;
                if (!rest.empty()) {
                    r_.raw(c2s, rest.data(), rest.size());
                    if (!sendAll(to, rest.data(), rest.size())) { return; }
                    tap.feed(rest.data(), rest.size());
                }
                continue;
            }
            r_.raw(c2s, buf, static_cast<std::size_t>(n));
            if (!sendAll(to, buf, static_cast<std::size_t>(n))) { return; }
            tap.feed(buf, static_cast<std::size_t>(n));
        }
    }

    Recorder& r_;
    std::string host_;
    std::uint16_t port_;
    bool ok_ = false;
    std::uint16_t localPort_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> upstream_{0};
    std::mutex m_;
    Sock listen_ = kiwifake::kBadSock, client_ = kiwifake::kBadSock, up_ = kiwifake::kBadSock;
    std::thread th_;
};

// --- WAV ----------------------------------------------------------------------------------------

class WavWriter {
public:
    bool open(const std::string& path, int rate) {
        f_ = std::fopen(path.c_str(), "wb");
        rate_ = rate;
        if (!f_) { return false; }
        patch();
        return true;
    }
    ~WavWriter() {
        if (f_) { patch(); std::fclose(f_); }
    }
    void append(const std::int16_t* p, std::size_t n) {
        if (!f_ || n == 0) { return; }
        std::fseek(f_, 0, SEEK_END);
        std::fwrite(p, sizeof(std::int16_t), n, f_);  // little-endian host
        samples_ += n;
    }
    void patch() {
        if (!f_) { return; }
        auto le32 = [](std::uint32_t v, unsigned char* o) { for (int i = 0; i < 4; ++i) { o[i] = static_cast<unsigned char>(v >> (8 * i)); } };
        unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                               1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        const std::uint32_t bytes = static_cast<std::uint32_t>(samples_ * 2);
        le32(36 + bytes, h + 4);
        le32(static_cast<std::uint32_t>(rate_), h + 24);
        le32(static_cast<std::uint32_t>(rate_) * 2, h + 28);
        le32(bytes, h + 40);
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, f_);
        std::fflush(f_);
    }
    std::size_t samples() const { return samples_; }

private:
    FILE* f_ = nullptr;
    int rate_ = 12000;
    std::size_t samples_ = 0;
};

const char* stateName(KiwiSdrClient::State s) {
    using S = KiwiSdrClient::State;
    switch (s) {
        case S::Idle: return "Idle";
        case S::Connecting: return "Connecting";
        case S::Playing: return "Playing";
        case S::RefusedBusy: return "RefusedBusy";
        case S::RefusedPassword: return "RefusedPassword";
        case S::ServerDown: return "ServerDown";
        case S::Redirected: return "Redirected";
        case S::ConnectFailed: return "ConnectFailed";
        case S::Dropped: return "Dropped";
        case S::Malformed: return "Malformed";
        case S::Closed: return "Closed";
    }
    return "?";
}

// The hard timer: a thread that ends the whole process whatever else happens.
void startHardStop(Recorder* rec, int seconds, int exitCode) {
    std::thread([rec, seconds, exitCode] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        if (rec) { rec->line("PROBE HARD STOP after " + std::to_string(seconds) + " s: ending the process"); }
        std::fflush(stdout);
        std::_Exit(exitCode);
    }).detach();
}

Outcome runProbe(const Config& cfg, Recorder& rec) {
    Outcome out;
    rec.line("PROBE start: target " + cfg.host + ":" + std::to_string(cfg.port) + " freq_hz=" +
             std::to_string(static_cast<long long>(cfg.freqHz)) + " mode=am identity=" + kIdentity +
             " duration_s<=" + std::to_string(cfg.durationS) + " hard_stop_s=" + std::to_string(cfg.hardStopS));

    Relay relay(rec, cfg.host, cfg.port);
    if (!relay.ok()) {
        out.detail = "relay failed to start";
        rec.line("PROBE " + out.detail);
        return out;
    }

    WavWriter wav;
    KiwiSdrClient client;
    wav.open(rec.dir() + "/audio.wav", KiwiSdrClient::audioRateHz());
    rec.line("PROBE wav sample rate (the client's ASSUMED rate): " + std::to_string(KiwiSdrClient::audioRateHz()));

    KiwiSdrClient::Options o;
    o.host = "127.0.0.1";
    o.port = relay.localPort();
    o.identUser = kIdentity;
    o.password.clear();  // NEVER a password
    o.connectWait = ms(15000);
    o.stallWait = ms(15000);
    const auto t0 = Clock::now();
    out.opened = client.open(o);
    rec.line(std::string("PROBE client.open() returned ") + (out.opened ? "true" : "false") + " state=" + stateName(client.state()));
    if (!out.opened) {
        const auto st = client.status();
        out.state = st.state;
        out.detail = st.detail;
        rec.line("PROBE handshake failure, NOT retrying: " + st.detail);
        client.close();
        relay.stop();
        out.upstreamConnections = relay.upstreamConnections();
        return out;
    }
    const bool tuned = client.tune("am", cfg.tuneLow, cfg.tuneHigh, cfg.freqHz);
    rec.line(std::string("PROBE tune() returned ") + (tuned ? "true" : "false"));

    std::vector<std::int16_t> buf(8192);
    double nextNote = 1.0;
    for (;;) {
        const std::size_t n = client.read(buf.data(), buf.size(), ms(100));
        wav.append(buf.data(), n);
        const double t = std::chrono::duration<double>(Clock::now() - t0).count();
        if (t >= nextNote) {
            nextNote += 1.0;
            wav.patch();
            rec.line("PROBE state=" + std::string(stateName(client.state())) + " frames=" + std::to_string(client.audioFrames()) +
                     " samples=" + std::to_string(client.samplesDelivered()) + " gaps=" + std::to_string(client.sequenceGaps()) +
                     " smeter_raw=" + std::to_string(client.smeterRaw()) + " rssi_dbm=" + std::to_string(client.rssiDbm()));
        }
        const auto st = client.state();
        if (st != KiwiSdrClient::State::Connecting && st != KiwiSdrClient::State::Playing) {
            rec.line(std::string("PROBE terminal state reached: ") + stateName(st) + ": " + client.status().detail);
            break;
        }
        if (t >= cfg.durationS) {
            rec.line("PROBE time is up, closing");
            break;
        }
    }
    // Drain what is left.
    for (;;) {
        const std::size_t n = client.read(buf.data(), buf.size(), ms(0));
        if (n == 0) { break; }
        wav.append(buf.data(), n);
    }
    out.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto beforeClose = client.status();
    out.state = beforeClose.state;
    out.detail = beforeClose.detail;
    out.frames = client.audioFrames();
    out.samples = client.samplesDelivered();
    out.gaps = client.sequenceGaps();
    out.missed = client.missedFrames();
    out.ignored = client.ignoredFrames();
    out.keepAlives = client.keepAlivesSent();
    client.close();
    rec.line("PROBE client closed");
    wav.patch();
    out.wavSamples = static_cast<int>(wav.samples());
    std::this_thread::sleep_for(ms(300));  // let the relay log the end of the stream
    relay.stop();
    out.upstreamConnections = relay.upstreamConnections();

    char sum[1024];
    std::snprintf(sum, sizeof sum,
                  "state_at_end=%s\ndetail=%s\nseconds=%.2f\naudio_frames=%llu\nsamples_delivered=%llu\nsequence_gaps=%llu\n"
                  "missed_frames=%llu\nignored_frames=%llu\nkeepalives_sent=%llu\nwav_samples=%d\nupstream_connections=%llu\n"
                  "identity=%s\n",
                  stateName(out.state), out.detail.c_str(), out.seconds, (unsigned long long)out.frames,
                  (unsigned long long)out.samples, (unsigned long long)out.gaps, (unsigned long long)out.missed,
                  (unsigned long long)out.ignored, (unsigned long long)out.keepAlives, out.wavSamples,
                  (unsigned long long)out.upstreamConnections, kIdentity);
    if (FILE* f = std::fopen((rec.dir() + "/summary.txt").c_str(), "wb")) {
        std::fputs(sum, f);
        std::fclose(f);
    }
    rec.line(std::string("PROBE summary\n") + sum);
    return out;
}

std::string readFile(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char b[4096];
        std::size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) { s.append(b, n); }
        std::fclose(f);
    }
    return s;
}

// A stand-in receiver for the self-test: upgrade, say hello, stream PCM or send nothing.
kiwifake::FakeKiwi::Script standIn(bool endless) {
    return [endless](kiwifake::Conn& c, int) {
        if (!c.upgrade()) { return; }
        c.waitCommands(5, 4000);
        c.sendMsg("version_maj=1 version_min=0");
        std::uint32_t seq = 0;
        for (int i = 0; endless || i < 400; ++i) {
            std::string audio;
            for (int k = 0; k < 256; ++k) {
                const int v = static_cast<int>(8000.0 * std::sin(0.05 * (i * 256 + k)));
                audio.push_back(static_cast<char>((v >> 8) & 0xFF));
                audio.push_back(static_cast<char>(v & 0xFF));
            }
            if (!c.sendSnd(0, seq++, 1000, audio)) { break; }
            if (!c.sleep(10)) { break; }
        }
        while (c.sleep(10)) {}
    };
}

std::string tempDir(const char* name) {
#if defined(_WIN32)
    const char* t = std::getenv("TEMP");
    std::string d = std::string(t ? t : ".") + "\\kiwilive-" + name;
    std::system(("mkdir \"" + d + "\" >nul 2>nul").c_str());
#else
    std::string d = std::string("/tmp/kiwilive-") + name;
    ::mkdir(d.c_str(), 0700);
#endif
    return d;
}

void selfTest(const char* self) {
    // 1. A normal run against the stand-in: records, WAV, clean close, one connection.
    {
        const std::string dir = tempDir("selftest");
        Recorder rec;
        CHECK(rec.open(dir));
        kiwifake::FakeKiwi srv(standIn(false));
        CHECK(srv.ok());
        Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = srv.port();
        cfg.freqHz = 1000000.0;
        cfg.outDir = dir;
        cfg.durationS = 2;
        const Outcome o = runProbe(cfg, rec);
        CHECK(o.opened);
        CHECK(o.frames > 50);
        CHECK(o.state == KiwiSdrClient::State::Playing);
        CHECK(o.upstreamConnections == 1);
        CHECK(srv.accepted() == 1);
        CHECK(o.wavSamples > 10000);
        CHECK(o.seconds >= 1.9 && o.seconds < 5.0);
        const std::string wire = readFile(dir + "/wire.log");
        CHECK(wire.find("text=\"SET ident_user=FoxSDR-test\"") != std::string::npos);
        CHECK(wire.find("text=\"SET auth t=kiwi p=\"") != std::string::npos);
        CHECK(wire.find("text=\"SET mod=am low_cut=-5000 high_cut=5000 freq=1000.000\"") != std::string::npos);
        CHECK(wire.find("S>C FRAME BIN") != std::string::npos);
        CHECK(wire.find("hex64=53 4e 44") != std::string::npos);
        CHECK(wire.find("PROBE client closed") != std::string::npos);
        CHECK(wire.find("Host header") != std::string::npos);
        // Nothing but the five command kinds ever went out: no password, no location, no mail.
        std::size_t pos = 0;
        int texts = 0;
        while ((pos = wire.find("C>S FRAME TEXT", pos)) != std::string::npos) {
            const std::size_t e = wire.find('\n', pos);
            const std::string l = wire.substr(pos, e - pos);
            ++texts;
            const bool known = l.find("text=\"SET auth t=kiwi p=\"") != std::string::npos ||
                               l.find("text=\"SET ident_user=FoxSDR-test\"") != std::string::npos ||
                               l.find("text=\"SET AR OK") != std::string::npos ||
                               l.find("text=\"SET compression=") != std::string::npos ||
                               l.find("text=\"SET mod=am") != std::string::npos ||
                               l.find("text=\"SET keepalive\"") != std::string::npos;
            CHECK(known);
            pos = e;
        }
        CHECK(texts >= 5);
        CHECK(readFile(dir + "/s2c.raw").size() > 10000);
        CHECK(readFile(dir + "/c2s.raw").size() > 100);
        CHECK(readFile(dir + "/frames.bin").size() > 10000);
        const std::string wav = readFile(dir + "/audio.wav");
        CHECK(wav.size() > 44 && wav.compare(0, 4, "RIFF") == 0 && wav.compare(8, 4, "WAVE") == 0);
        CHECK(wav.size() == 44 + static_cast<std::size_t>(o.wavSamples) * 2);
        CHECK(readFile(dir + "/summary.txt").find("upstream_connections=1") != std::string::npos);
    }
    // 2. A handshake failure is logged and NOT retried (a port nothing listens on).
    {
        const std::string dir = tempDir("selftest-fail");
        Recorder rec;
        CHECK(rec.open(dir));
        Config cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 1;  // closed
        cfg.freqHz = 1000000.0;
        cfg.outDir = dir;
        cfg.durationS = 2;
        const Outcome o = runProbe(cfg, rec);
        CHECK(o.state != KiwiSdrClient::State::Playing);
        CHECK(o.frames == 0);
        CHECK(o.upstreamConnections == 0);
        CHECK(readFile(dir + "/wire.log").find("connect failed") != std::string::npos);
    }
    // 3. The hard stop: a child process runs the probe against a server that never stops and
    //    asks for 100 s; the hard timer (4 s here) must end it with exit code 3.
    {
        const auto t0 = Clock::now();
#if defined(_WIN32)
        const std::string cmd = std::string("\"\"") + self + "\" --hardstop-demo\"";
#else
        const std::string cmd = std::string("\"") + self + "\" --hardstop-demo";
#endif
        int rc = std::system(cmd.c_str());
#if !defined(_WIN32)
        rc = WEXITSTATUS(rc);
#endif
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("hard-stop demo: exit code %d after %.1f s\n", rc, s);
        CHECK(rc == 3);
        CHECK(s >= 3.5 && s < 15.0);
    }
}

int hardStopDemo() {
    const std::string dir = tempDir("hardstop");
    Recorder rec;
    if (!rec.open(dir)) { return 9; }
    startHardStop(&rec, 4, 3);
    kiwifake::FakeKiwi srv(standIn(true));
    Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port();
    cfg.freqHz = 1000000.0;
    cfg.outDir = dir;
    cfg.durationS = 100;
    runProbe(cfg, rec);
    return 0;  // not reached before the hard stop
}

int live(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s --live HOST PORT KHZ OUTDIR\n", argv[0]);
        return 2;
    }
    Config cfg;
    cfg.host = argv[2];
    const long port = std::strtol(argv[3], nullptr, 10);
    const double khz = std::strtod(argv[4], nullptr);
    cfg.outDir = argv[5];
    if (port < 1 || port > 65535 || khz < 10.0 || khz > 30000.0) {
        std::fprintf(stderr, "bad port or frequency\n");
        return 2;
    }
    cfg.port = static_cast<std::uint16_t>(port);
    cfg.freqHz = khz * 1000.0;
    cfg.durationS = 45;
    cfg.hardStopS = 60;
    static Recorder rec;  // static: the hard-stop thread may use it at any time
    if (!rec.open(cfg.outDir)) {
        std::fprintf(stderr, "cannot write to %s\n", cfg.outDir.c_str());
        return 2;
    }
    startHardStop(&rec, cfg.hardStopS, 3);
    const Outcome o = runProbe(cfg, rec);
    std::printf("opened=%d state=%s seconds=%.1f audio_frames=%llu samples=%llu upstream_connections=%llu detail=%s\n",
                o.opened ? 1 : 0, stateName(o.state), o.seconds, (unsigned long long)o.frames,
                (unsigned long long)o.samples, (unsigned long long)o.upstreamConnections, o.detail.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--live") == 0) { return live(argc, argv); }
    if (argc >= 2 && std::strcmp(argv[1], "--hardstop-demo") == 0) { return hardStopDemo(); }
    selfTest(argv[0]);
    return testSummary("test_kiwisdr_live_probe");
}
