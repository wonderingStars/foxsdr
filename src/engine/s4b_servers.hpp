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

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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
    bool operator==(const Config& o) const {
        return tcpAudio == o.tcpAudio && tcpAudioAll == o.tcpAudioAll && tcpAudioPort == o.tcpAudioPort && tcpAudioRadio == o.tcpAudioRadio && tcpAudioPacket == o.tcpAudioPacket;
    }
};

// The settings (the `srv.*` keys) as one value.
Config configFrom(const cascade::engine::wirec::Settings& s);

// What the engine gives the servers (set once). Each is optional: a server whose hook is missing says so in its problem sentence.
struct Hooks {
    std::function<cascade::net::UdpAudioSender::Reader(int radio)> audioReader;   // the engine's audio of a radio (a session closed when the reader is destroyed)
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
    void stop();

    // The facts: `fact(name, value)` for each (names without the "fact." prefix).
    void facts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    // A short text that changes whenever a fact would (the pass bumps its version on a change).
    std::string signature() const;

private:
    void applyTcpAudio(const Config& c);

    mutable std::mutex m_;
    Hooks hooks_;
    Config cfg_;
    bool configured_ = false;
    std::unique_ptr<cascade::net::TcpAudioServer> tcpAudio_;
    std::string tcpAudioProblem_;
};

}  // namespace cascade::engine::s4b
