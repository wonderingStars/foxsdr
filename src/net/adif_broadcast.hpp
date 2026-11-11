// net/adif_broadcast.hpp - broadcasts logged-QSO records as ADIF (ADI) text: one record per UDP datagram, and/or to the clients of a TCP listener.
//
// OFF BY DEFAULT: a default-constructed config is disabled and start() refuses it. LOOPBACK ONLY BY DEFAULT: the destination and the listener
// address must be a 127.x.x.x number unless the config says allowNonLoopback (a deliberate user setting); names are never resolved. The class only
// SENDS: nothing read from a UDP socket, and TCP clients' bytes are read only to notice they have gone away and are thrown away.
//
// NO THREAD: publish() and pump() are non-blocking and return at once; the caller runs them on a worker thread (never the pass or the DSP thread).
// A TCP client that cannot take the whole record without blocking is dropped (a half-sent record would corrupt its stream) and counted.
//
// WIRE FORMAT: the ADIF specification (https://adif.org/314/ADIF_314.htm, opened 2026-11-11) defines the data format and says ADIF "can be conveyed
// in files, over networks, or as web service parameters and results"; it does NOT define a UDP or TCP framing. The framing here is this lane's own:
// UDP - exactly one record (text ending <EOR>) per datagram, at most 1400 bytes; TCP - the records back to back, each ending <EOR> and a CR LF, no
// header. Receivers that parse ADI by its <EOR> markers read both.
//
// PRIVACY: the records are the user's own log data. This code prints nothing, writes no setting and keeps no copy: a record is sent and forgotten.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "loopback_sock.hpp"

namespace cascade::net {

inline constexpr std::size_t kAdifMaxRecord = 1400;   // one datagram on any normal path, and a bound on what a caller can hand in
inline constexpr std::size_t kAdifMaxClients = 8;

struct AdifBroadcastConfig {
    bool enabled = false;                  // OFF by default
    std::string host = "127.0.0.1";        // dotted IPv4 only
    int udpPort = 0;                       // 0 = no UDP output; otherwise the destination port
    int tcpPort = -1;                      // -1 = no TCP listener; 0 = a free port (see tcpPort()); otherwise that port
    bool allowNonLoopback = false;         // a deliberate user setting; tests never set it
};

class AdifBroadcaster {
public:
    AdifBroadcaster() = default;
    ~AdifBroadcaster() { stop(); }
    AdifBroadcaster(const AdifBroadcaster&) = delete;
    AdifBroadcaster& operator=(const AdifBroadcaster&) = delete;

    // Opens the sockets the config asks for. False with a sentence in `error` (nothing is left open): disabled, a name instead of a number, a
    // non-loopback address without allowNonLoopback, a bad port, no output selected, a port that cannot be opened.
    bool start(const AdifBroadcastConfig& cfg, std::string& error) {
        stop();
        if (!cfg.enabled) { error = "the ADIF broadcast is switched off"; return false; }
        std::uint32_t host = 0;
        if (!lo::parseIpv4(cfg.host, host)) { error = "the address must be a dotted IPv4 number"; return false; }
        if (!lo::isLoopbackHost(host) && !cfg.allowNonLoopback) { error = "only the local machine (127.x.x.x) is allowed unless a wider address is switched on"; return false; }
        if (cfg.udpPort < 0 || cfg.udpPort > 65535 || cfg.tcpPort < -1 || cfg.tcpPort > 65535) { error = "a port is out of range"; return false; }
        if (cfg.udpPort == 0 && cfg.tcpPort < 0) { error = "neither a UDP port nor a TCP listener was asked for"; return false; }
        if (cfg.udpPort > 0) {
            udp_ = lo::udpOpen(lo::kLoopbackHost, -1);
            if (udp_ == lo::kBad) { error = "could not open a UDP socket"; stop(); return false; }
            udpHost_ = host;
            udpPort_ = cfg.udpPort;
        }
        if (cfg.tcpPort >= 0) {
            // the listener is on the same host number; a non-loopback host here is the deliberate wider setting
            listener_ = lo::tcpListen(host, cfg.tcpPort);
            if (listener_ == lo::kBad) { error = "could not open the TCP listener"; stop(); return false; }
            tcpPort_ = lo::boundPortOf(listener_);
        }
        running_ = true;
        return true;
    }

    void stop() {
        for (lo::Sock c : clients_) { lo::closeSock(c); }
        clients_.clear();
        lo::closeSock(udp_);
        lo::closeSock(listener_);
        udp_ = listener_ = lo::kBad;
        udpPort_ = 0;
        tcpPort_ = -1;
        running_ = false;
    }

    bool running() const { return running_; }
    int tcpPort() const { return tcpPort_; }
    std::size_t clientCount() const { return clients_.size(); }

    // Accepts waiting TCP clients and forgets the ones that have gone. Cheap; publish() calls it too.
    void pump() {
        if (!running_) { return; }
        if (listener_ != lo::kBad) {
            for (int i = 0; i < 16; ++i) {
                const lo::Sock c = lo::tcpAccept(listener_);
                if (c == lo::kBad) { break; }
                if (clients_.size() >= kAdifMaxClients) { lo::closeSock(c); ++refusedClients_; continue; }
                clients_.push_back(c);
            }
        }
        char scratch[64];
        for (std::size_t i = 0; i < clients_.size();) {
            if (lo::tcpRecv(clients_[i], scratch, sizeof(scratch)) < 0) {   // closed or failed; bytes a client sends are discarded
                lo::closeSock(clients_[i]);
                clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
                ++droppedClients_;
            } else {
                ++i;
            }
        }
    }

    // Sends one ADI record. True when at least one output took it. A record that is empty, larger than kAdifMaxRecord or does not end in <EOR>
    // (optionally followed by white space) is refused and counted; the broadcast is not running: false.
    bool publish(const std::string& record) {
        if (!running_) { return false; }
        if (!wellFormed(record)) { ++refused_; return false; }
        pump();
        bool any = false;
        if (udp_ != lo::kBad) {
            const int n = lo::udpSendTo(udp_, udpHost_, udpPort_, record.data(), record.size());
            if (n == static_cast<int>(record.size())) { ++udpSent_; any = true; } else { ++udpFailed_; }
        }
        if (listener_ != lo::kBad) {
            std::string framed = record;
            while (!framed.empty() && (framed.back() == '\r' || framed.back() == '\n' || framed.back() == ' ')) { framed.pop_back(); }
            framed += "\r\n";
            for (std::size_t i = 0; i < clients_.size();) {
                const int n = lo::tcpSend(clients_[i], framed.data(), framed.size());
                if (n == static_cast<int>(framed.size())) { ++tcpSent_; any = true; ++i; continue; }
                // would block, partial or gone: the stream cannot be trusted any more
                lo::closeSock(clients_[i]);
                clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
                ++droppedClients_;
            }
        }
        return any;
    }

    std::uint64_t udpSent() const { return udpSent_; }
    std::uint64_t udpFailed() const { return udpFailed_; }
    std::uint64_t tcpSent() const { return tcpSent_; }
    std::uint64_t refused() const { return refused_; }
    std::uint64_t droppedClients() const { return droppedClients_; }
    std::uint64_t refusedClients() const { return refusedClients_; }

    // The record check, public for the tests: non-empty, within the size bound, ends with <EOR> (any case) plus optional white space, and carries no
    // NUL byte.
    static bool wellFormed(const std::string& r) {
        if (r.empty() || r.size() > kAdifMaxRecord) { return false; }
        std::size_t e = r.size();
        while (e > 0 && (r[e - 1] == '\r' || r[e - 1] == '\n' || r[e - 1] == ' ')) { --e; }
        if (e < 5) { return false; }
        const char* t = r.data() + e - 5;
        const bool eor = t[0] == '<' && (t[1] == 'E' || t[1] == 'e') && (t[2] == 'O' || t[2] == 'o') && (t[3] == 'R' || t[3] == 'r') && t[4] == '>';
        if (!eor) { return false; }
        return r.find('\0') == std::string::npos;
    }

private:
    bool running_ = false;
    lo::Sock udp_ = lo::kBad;
    lo::Sock listener_ = lo::kBad;
    std::uint32_t udpHost_ = 0;
    int udpPort_ = 0;
    int tcpPort_ = -1;
    std::vector<lo::Sock> clients_;
    std::uint64_t udpSent_ = 0, udpFailed_ = 0, tcpSent_ = 0, refused_ = 0, droppedClients_ = 0, refusedClients_ = 0;
};

}  // namespace cascade::net
