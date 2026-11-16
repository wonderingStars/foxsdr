// s4b_servers.hpp - screen lane S4b's engine half: the servers other programs connect to (docs/multi-radio-records/screen-s4b.md).
//
// WHAT THIS IS. A small set of network servers the person can turn on from SYSTEM > Outputs, each a stand-alone model that already has its own tests, wired to the engine's
// pass and to the wirec settings (engine/wire_c.cpp: the keys `srv.*`): audio over TCP (net/tcp_audio.hpp). More servers join this class lane row by lane row.
//
// EVERY SERVER IS OFF until the person turns it on (the keys' defaults are "0"). No socket and no thread exists while all are off. Each listens on THIS COMPUTER ONLY (127.0.0.1) unless the
// person also turns on that server's own "listen on every address" key; scope() says in a sentence which of the two it is, and the window says it plainly. A server that cannot start says why in
// a sentence (a port in use, a refused address) in the fact; the sentence never holds a password or a token.
//
// THREADS. configure() and the pass hooks run on the engine's pass thread; each server has its own IO threads (the fan-out's). The pass never waits on a client.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/pipeline.hpp"
#include "net/rtl_tcp_server.hpp"
#include "net/tcp_audio.hpp"

namespace cascade::engine::wirec {
class Settings;
}

namespace cascade::engine::s4b {

struct Config {
    // audio over TCP
    bool tcpAudio = false;
    bool tcpAudioAll = false;
    int tcpAudioPort = 7356;
    int tcpAudioRadio = 1;
    int tcpAudioPacket = 480;
    // the radio the network control and stream servers act on (1..8)
    int controlRadio = 1;
    // rtl_tcp: the radio's I/Q for programs that read an RTL-SDR over the network, and tuning from them
    bool rtl = false;
    bool rtlAll = false;
    int rtlPort = 1234;
    bool operator==(const Config& o) const {
        return tcpAudio == o.tcpAudio && tcpAudioAll == o.tcpAudioAll && tcpAudioPort == o.tcpAudioPort && tcpAudioRadio == o.tcpAudioRadio && tcpAudioPacket == o.tcpAudioPacket &&
               controlRadio == o.controlRadio && rtl == o.rtl && rtlAll == o.rtlAll && rtlPort == o.rtlPort;
    }
};

// The settings (the `srv.*` keys) as one value.
Config configFrom(const cascade::engine::wirec::Settings& s);

// What the engine gives the servers (set once). Each is optional: a server whose hook is missing says so in its problem sentence.
struct Hooks {
    std::function<cascade::net::UdpAudioSender::Reader(int radio)> audioReader;   // the engine's audio of a radio (a session closed when the reader is destroyed)
    // Gives the radio's pipeline the tap on its raw band (null = takes it away). "" = done, else the sentence why not (the radio is not open, the radio sends network I/Q already).
    std::function<std::string(int radio, cascade::core::Pipeline::IqSink* sink)> attachIq;
    // Names the radio's present receiver (0 = the radio is not open): a radio that was closed and opened again has a new one and the tap is given to it again.
    std::function<std::uintptr_t(int radio)> radioIdentity;
    // Retunes a radio through the engine's own tuning command (every check it makes applies). "" = done, else the engine's sentence.
    std::function<std::string(int radio, double hz)> tune;
};

// The sentence that says who can reach a server: "this computer only" or the warning for every address.
std::string scopeOf(bool allAddresses);

class ServerSet {
public:
    ServerSet() = default;
    ~ServerSet();
    ServerSet(const ServerSet&) = delete;
    ServerSet& operator=(const ServerSet&) = delete;

    void setHooks(Hooks h);
    // Applies the settings; a server whose own settings changed is restarted, one that is off is stopped. Cheap when nothing changed.
    void configure(const Config& c);
    // Whether any server is switched on (the pass may skip the rest while this is false).
    bool active() const;
    // Once a control pass, on the pass thread: keeps the stream taps on the right receivers and applies what a client asked for.
    void pass();
    // True while the rtl_tcp server holds this radio's (1-based) raw-band tap: network I/Q out must not take it.
    bool holdsIq(int radio) const;
    void stop();

    // The facts: `fact(name, value)` for each (names without the "fact." prefix).
    void facts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    // A short text that changes whenever a fact would (the pass bumps its version on a change).
    std::string signature() const;

private:
    void applyTcpAudio(const Config& c);
    void applyRtl(const Config& c);
    void releaseRtl();    // the tap off the radio, then the server stopped (m_ held)
    struct RtlTap final : cascade::core::Pipeline::IqSink {
        cascade::net::RtlTcpServer* target = nullptr;
        void submit(const float* iq, std::size_t n) noexcept override { if (target != nullptr) { (void)target->submit(iq, n); } }
    };

    mutable std::mutex m_;
    Hooks hooks_;
    Config cfg_;
    bool configured_ = false;
    std::unique_ptr<cascade::net::TcpAudioServer> tcpAudio_;
    std::string tcpAudioProblem_;
    std::unique_ptr<cascade::net::RtlTcpServer> rtl_;
    RtlTap rtlTap_;
    int rtlRadio_ = 0;                       // the radio the tap is on (1-based), 0 = none
    std::uintptr_t rtlIdentity_ = 0;         // the receiver it was given to
    std::string rtlProblem_;
    std::atomic<std::uint64_t> rtlTuneWord_{0};   // a pending retune: bit 63 set, the Hz below (written by the server's IO thread, taken by the pass)
    std::atomic<std::uint64_t> rtlTunes_{0}, rtlTuneRefused_{0}, rtlIgnored_{0};
};

}  // namespace cascade::engine::s4b
