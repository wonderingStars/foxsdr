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
#include <vector>

#include "core/pipeline.hpp"
#include "net/api_server.hpp"
#include "net/control_hub.hpp"
#include "engine/s4b_mcp.hpp"
#include "engine/s4b_rotator.hpp"
#include "net/rtl_tcp_server.hpp"
#include "net/web_auth.hpp"
#include "net/tcp_audio.hpp"

namespace cascade::engine::wirec {
class Settings;
}

namespace cascade::engine::s4b {

struct Config {
    // audio over TCP
    bool tcpAudio = false;
    bool tcpAudioAll = false;
    std::string tcpAudioAllow;     // beyond this computer: the client addresses that may connect (net/peer_allow.hpp); without them the server does not start
    int tcpAudioPort = 7356;
    int tcpAudioRadio = 1;
    int tcpAudioPacket = 480;
    // the radio the network control and stream servers act on (1..8)
    int controlRadio = 1;
    // rtl_tcp: the radio's I/Q for programs that read an RTL-SDR over the network, and tuning from them
    bool rtl = false;
    bool rtlAll = false;
    std::string rtlAllow;          // the same for rtl_tcp, whose protocol has no place for a password
    int rtlPort = 1234;
    // the WebSocket / SSE API (net/api_server.hpp): other programs read what the receiver does and retune it; the web page's own account guards it
    bool api = false;
    bool apiAll = false;
    int apiPort = 8074;
    std::string apiBindForTest;   // TESTS ONLY (never a setting): the address the API binds, so "every address" can be proved without a firewall prompt
    // the MCP server (net/mcp_server.hpp): this computer only; an optional bearer token (a secret, kept in the protected store)
    bool mcp = false;
    int mcpPort = 8075;
    std::string mcpToken;
    // the antenna rotator (engine/s4b_rotator.hpp)
    RotatorConfig rotator;
    bool operator==(const Config& o) const {
        return rotator == o.rotator && api == o.api && apiAll == o.apiAll && apiPort == o.apiPort && apiBindForTest == o.apiBindForTest && mcp == o.mcp && mcpPort == o.mcpPort && mcpToken == o.mcpToken && tcpAudio == o.tcpAudio && tcpAudioAll == o.tcpAudioAll && tcpAudioAllow == o.tcpAudioAllow && rtlAllow == o.rtlAllow && tcpAudioPort == o.tcpAudioPort && tcpAudioRadio == o.tcpAudioRadio && tcpAudioPacket == o.tcpAudioPacket &&
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
    // What the radio is, for the rtl_tcp header: its rtl_tcp tuner code (5 R820T, 6 R828D ...) and its number of tuner gain steps. False = the engine does not know (the header says 0 and 0).
    std::function<bool(int radio, std::uint32_t& tunerType, std::uint32_t& gainCount)> tunerInfo;
    // Retunes a radio through the engine's own tuning command (every check it makes applies). "" = done, else the engine's sentence.
    std::function<std::string(int radio, double hz)> tune;
    // Sets a radio's demodulator mode through the engine's own mode command (name: one of the eight). "" = done, else the engine's sentence.
    std::function<std::string(int radio, const std::string& mode)> setMode;
    // The radio's state as the engine publishes it; false when the radio is not open.
    std::function<bool(int radio, cascade::net::HubStatus& out)> readStatus;
    // The saved bookmarks.
    std::function<std::vector<cascade::net::HubBookmark>()> readBookmarks;
    // The decoded lines newer than `seq` (which is moved up), never those of a private plugin: each is handed to `sink(unixSeconds, frequencyHz, decoder, text)`.
    std::function<void(std::uint64_t& seq, const std::function<void(double, double, const std::string&, const std::string&)>& sink)> readDecodes;
    // The web page's account (user name and the password's stored record: never the password). Empty record = no password is set.
    struct WebAccount { std::string user, record; };
    std::function<WebAccount()> webAccount;
    // The DNS names the web page's own Host rule accepts (this computer's name, name.local, the hosts of "Allowed web origins"): the interface answers a request that names one of them (M9-api-host-rule).
    std::function<std::vector<std::string>()> hostNames;
};

// The sentence that says who can reach a server: "this computer only" or the warning for every address.
std::string scopeOf(bool allAddresses);
// The same for the two servers that are widened only to a list of client addresses.
std::string scopeOfListed(bool allAddresses, const std::string& allow);

class ServerSet {
public:
    ServerSet() = default;
    ~ServerSet();
    ServerSet(const ServerSet&) = delete;
    ServerSet& operator=(const ServerSet&) = delete;

    void setHooks(Hooks h);
    // TESTS ONLY (never a setting): every Config applied from now on binds the program interface to this address ("" = as configured), so a test that ticks "every address" through the REAL engine
    // can be proved on 127.0.0.1 without a firewall prompt. Set before the first configure.
    void setApiBindForTest(const std::string& address);
    // Applies the settings; a server whose own settings changed is restarted, one that is off is stopped. Cheap when nothing changed.
    void configure(const Config& c);
    // Whether any server is switched on (the pass may skip the rest while this is false).
    bool active() const;
    // The rotator: the engine's op hands the person's request here (the worker does the turning).
    RotatorControl& rotator() { return rotator_; }
    // FOXAPP_OP_ROTATOR: text JSON; returns the FOXAPI status and, for a refusal, the sentence in `why`.
    std::int32_t rotatorOp(const std::string& text, std::string& why);
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
    void applyApi(const Config& c);
    void applyMcp(const Config& c);
    cascade::net::ApiAuth checkApiAuth(const std::string& authorization, const std::string& client);
    void controlPass();
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
    std::int64_t rtlInfoMs_ = 0;             // when the header's tuner numbers were last refreshed (the pass, at most twice a second, while no client is connected)
    std::atomic<std::uint64_t> rtlTuneWord_{0};   // a pending retune: bit 63 set, the Hz below (written by the server's IO thread, taken by the pass)
    std::atomic<std::uint64_t> rtlTunes_{0}, rtlTuneRefused_{0}, rtlIgnored_{0};

    // the control hub (the API and the MCP server read and ask through it), the two servers, and what the pass keeps for them
    RotatorControl rotator_;
    cascade::net::ControlHub hub_;
    std::unique_ptr<cascade::net::ApiServer> api_;
    std::string apiProblem_;
    std::unique_ptr<McpEndpoint> mcp_;
    std::string mcpProblem_;
    std::uint64_t decodeSeq_ = 0;
    bool feedLive_ = false;
    std::int64_t lastStatusMs_ = -1000000, lastBookmarksMs_ = -1000000;
    std::atomic<std::uint64_t> controlTunes_{0}, controlTuneRefused_{0}, controlModes_{0};
    struct Verified { std::string digest; std::int64_t until = 0; };
    std::mutex authM_;
    std::vector<Verified> verified_;
    std::string verifiedFor_;                // the account (user and stored record) the remembered logins were made under
    cascade::net::LoginThrottle throttle_;
    // The account hook is also read by the API's auth worker, which must never wait on m_ (stop() holds m_ while it joins that worker): its own lock.
    mutable std::mutex acctM_;
    std::function<Hooks::WebAccount()> webAccountFn_;
    std::atomic<bool> apiNeedsAccount_{false};   // the API listens beyond this computer: no account means nobody is let in
    std::string apiAccountPrint_;                // the account the running API was last seen with (pass thread)
    std::int64_t lastAccountCheckMs_ = -1000000;
    std::int64_t lastHostNamesMs_ = -1000000;
    std::string apiBindForTest_;
};

}  // namespace cascade::engine::s4b
