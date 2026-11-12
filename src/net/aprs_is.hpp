// aprs_is.hpp - an APRS-IS client: login, filter, comment-line handling, reconnect with back-off. RECEIVE-ONLY BY DEFAULT.
//
// WHAT IT DOES. Connects (TCP) to an APRS-IS server the CALLER names (no server is built in, none is contacted by default), sends the
// login line, hands every packet line the server sends to a handler, and keeps the connection alive. With `gateway` OFF (the default)
// it writes NOTHING but the login line, ever: the login says `pass -1`, the documented receive-only value, whatever passcode the
// configuration holds, and forward() refuses. Only when the user has switched `gateway` ON AND supplied a callsign AND a passcode does
// the login carry the passcode and does forward() send heard packets, as an IGate, after the server has confirmed the login as verified.
//
// THE PASSCODE IS THE USER'S. This code does not compute, derive or suggest a passcode and contains no passcode generator; the APRS-IS
// documentation says the software author issues them on request and does not publish the algorithm. It is accepted as text the user
// typed (-1, or one to five digits) and is never written to a log, an error string or a stats field (redactedLogin() masks it).
//
// SOURCES (read 2026-11-11): "Connecting to APRS-IS" http://www.aprs-is.net/Connecting.aspx (login line, `pass -1` for receive-only,
// `vers`, `filter`, `#` lines are comments and double as server keep-alives, 512-byte line limit, TNC2 text ending CRLF, clients may
// generate only qAR and qAO, server sends an identification line first and the client logs in after it appears); "IGate Details"
// http://www.aprs-is.net/IGateDetails.aspx (the never-gate list and the path rule, below); "q Construct" http://www.aprs-is.net/q.aspx
// (qAR: a client placing RF packets on APRS-IS; qAO: the same for a receive-only IGate; callsign rules). The format of the server's
// login acknowledgement ("# logresp CALL verified, server X") is NOT given on those pages: this code looks for the words `logresp` and
// `verified` / `unverified` and treats anything else as not confirmed. UNVERIFIED against a real server (stand-ins only).
//
// THE GATE RULES (IGate Details), each one tested: a packet is NOT gated when
//   1 its AX.25 frame lacks control 0x03 / PID 0xF0 (input flag `controlPidOk`), 2 the TNC has PASSALL on (`tncPassAll`),
//   3 it is a third-party packet (data type '}') whose third-party header holds TCPIP or TCPXX, 4 it is a generic query (data type '?'),
//   5 its header holds TCPIP, TCPXX, NOGATE or RFONLY (the page marks the last two optional; both are applied),
// and: a third-party packet without TCPXX/TCPIP is gated with the RF header and the '}' removed (6); the path is never modified except
// to append `,qAO,IGATECALL` (7, 8): FoxSDR never transmits, so this is a receive-only IGate and the pages require qAO; qAR is NOT offered anywhere (the choice was removed 2026-11-11 on the orchestrator's correction); `q` constructs and the `I` construct never appear on
// RF, so an RF packet that carries one is malformed and is refused (9, 10). Added here, not from the page: a packet containing CR, LF
// or NUL, a nested third-party packet, an unparsable header, and a result over 510 bytes are refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_APRS_IS_HPP
#define CASCADE_NET_APRS_IS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "net/net_sock.hpp"
#include "net/spsc_ring.hpp"

namespace cascade::net {

inline constexpr std::uint16_t kAprsIsFilterPort = 14580;  // the user-defined-filter feed (Connecting page)
inline constexpr std::size_t kAprsMaxLineBytes = 512;      // including CR LF

struct AprsIsConfig {
    std::string host;                         // dotted quad or a host name (resolved on the worker thread); empty = not chosen: start() refuses
    std::uint16_t port = kAprsIsFilterPort;
    std::string callsign;                     // N0CALL or N0CALL-5; upper-cased
    std::string softwareName = "FoxSDR";      // one word
    std::string softwareVersion = "0";        // no spaces
    std::string filter;                       // e.g. "r/33/-96/25"; empty = none (the server then sends almost nothing)
    bool gateway = false;                     // OFF: nothing but the login is ever written
    std::string passcode;                     // the user's; used only when gateway is on; never generated here
    int connectTimeoutMs = 5000;
    int loginWaitMs = 5000;                   // wait this long for the server's identification line before logging in anyway
    int idleTimeoutMs = 120000;               // no byte from the server (not even a # line) for this long: reconnect
    int backoffInitialMs = 1000;
    int backoffMaxMs = 60000;
    int stableMs = 30000;                     // an online connection lasting this long resets the back-off
    std::size_t outQueueBytes = 64 * 1024;    // forward() ring
};

// ---- pure pieces (tested on their own) --------------------------------------------------------------------------------------------

// A callsign for the login: 3..9 characters in all, A-Z 0-9, at most one '-', then a one- or two-character SSID that is not "-0", the
// callsign part at least three characters, and not exactly eight characters of 0-9/A-F. `call` is upper-cased in place when valid.
bool aprsNormaliseCallsign(std::string& call);
// "-1", or one to five digits (value 0..32767). Says nothing about whether it matches the callsign: it cannot, and does not try.
bool aprsPasscodeWellFormed(const std::string& pass);
// The login line, CRLF-terminated. False and `error` when the configuration cannot log in (bad callsign, spaces in the software name or
// version, a CR/LF/NUL in the filter, a filter over 200 characters, the line over 512 bytes, gateway on without a well-formed passcode other than -1).
bool aprsBuildLogin(const AprsIsConfig& cfg, std::string& line, std::string& error);
// The same line with the passcode replaced by "***", for logs.
std::string aprsRedactedLogin(const std::string& loginLine);

enum class AprsLine { Empty, Comment, Packet };
AprsLine aprsClassify(const std::string& line);
struct AprsLogresp {
    bool isLogresp = false;
    bool verified = false;  // the word "verified" (and not "unverified") follows the word logresp
};
AprsLogresp aprsParseLogresp(const std::string& commentLine);

// The back-off after `attempt` consecutive failures (0 = the first): initial x 2^attempt, capped at max, overflow-safe.
int aprsNextBackoffMs(int attempt, int initialMs, int maxMs);

// ---- the gate ----------------------------------------------------------------------------------------------------------------------

struct GateInput {
    std::string tnc2;            // "SRC>DEST,PATH...:information", no CR LF
    bool controlPidOk = true;    // the AX.25 frame had control 0x03 and PID 0xF0
    bool tncPassAll = false;     // the receiving TNC had PASSALL on
};
enum class GateReason {
    Gated,
    NotUiFrame,
    PassAll,
    Malformed,
    ForbiddenPathElement,   // TCPIP, TCPXX, NOGATE or RFONLY in the header
    QConstructOnRf,         // a q construct or the I construct in an RF header
    GenericQuery,
    ThirdPartyInternet,     // third-party header holds TCPIP or TCPXX
    NestedThirdParty,
    TooLong,
    GatewayOff,             // forward(): gateway is not enabled
    NotOnline,              // forward(): not connected / login not yet answered
    NotVerified,            // forward(): the server did not confirm the login as verified
    QueueFull,              // forward(): the hand-over ring is full
};
const char* aprsGateReasonName(GateReason r);
struct GateDecision {
    GateReason reason = GateReason::Malformed;
    std::string line;  // when Gated: the line to send to APRS-IS, without CR LF
};
// Applies the rules above. `igateCall` must already be a normalised callsign.
GateDecision aprsGate(const GateInput& in, const std::string& igateCall);

// ---- the client --------------------------------------------------------------------------------------------------------------------

enum class AprsState { Stopped, Connecting, WaitingForServer, LoggedIn, Online, BackingOff };
const char* aprsStateName(AprsState s);

struct AprsStats {
    std::uint64_t connects = 0;        // TCP connections established
    std::uint64_t failures = 0;        // connection attempts or sessions that ended in failure
    std::uint64_t linesReceived = 0;
    std::uint64_t commentLines = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t idleReconnects = 0;
    std::uint64_t hostileDrops = 0;    // sessions ended because the server sent a line of thousands of bytes with no end
    std::uint64_t longLinesSkipped = 0;  // complete lines over 512 bytes, not handed to the handler
    std::uint64_t packetsForwarded = 0;
    std::uint64_t gateRefused = 0;
    std::uint64_t bytesWritten = 0;    // everything this client ever wrote (the login line and any forwarded packets)
};

class AprsIsClient {
public:
    using PacketHandler = std::function<void(const std::string&)>;

    explicit AprsIsClient(const AprsIsConfig& cfg);
    ~AprsIsClient();
    AprsIsClient(const AprsIsClient&) = delete;
    AprsIsClient& operator=(const AprsIsClient&) = delete;

    // Called on the client's thread for every non-comment line (CR LF removed). Set before start().
    void setPacketHandler(PacketHandler h) { handler_ = std::move(h); }

    // Validates the configuration (the login line must build) and starts the worker. False and `error` otherwise.
    bool start(std::string& error);
    void stop();

    // PRODUCER THREAD (one thread). Applies the gate rules and, when gating is on and the login is verified, queues the line. Returns the
    // reason (Gated = queued). Never blocks.
    GateReason forward(const GateInput& in);

    AprsState state() const { return state_.load(); }
    AprsStats stats() const;
    // The login line this client sends with the passcode masked.
    std::string redactedLogin() const { return aprsRedactedLogin(login_); }
    std::string lastError() const;

private:
    void run();
    bool session(sock::Handle s);
    void setError(const std::string& e);

    AprsIsConfig cfg_;
    std::string login_;
    PacketHandler handler_;
    MessageRing out_;
    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<AprsState> state_{AprsState::Stopped};
    std::atomic<bool> verified_{false};
    std::atomic<bool> unverified_{false};
    mutable std::mutex errMutex_;
    std::string lastError_;
    std::atomic<std::uint64_t> connects_{0}, failures_{0}, lines_{0}, comments_{0}, packets_{0}, idleReconnects_{0}, hostile_{0}, forwarded_{0},
        refused_{0}, written_{0}, longSkipped_{0};
};

}  // namespace cascade::net

#endif
