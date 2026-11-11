// net/discord_presence.hpp - Discord "rich presence" over Discord's LOCAL pipe (row N25): a line on the user's Discord profile saying they are
// listening to the radio. OFF BY DEFAULT: the setting is the application's client ID, and EMPTY means off (the owner registers a Discord
// application later and puts its ID in the settings; nothing here contains one).
//
// NO NETWORK CONNECTION IS MADE BY THIS PROGRAM. The transport is a named pipe on this machine (\\.\pipe\discord-ipc-0 to -9) to the Discord
// desktop client, which talks to Discord's servers itself, exactly as it does for any game.
//
// THE PROTOCOL is from Discord's developer documentation, "RPC" (https://docs.discord.com/developers/topics/rpc, opened 2026-11-11; the page
// carries no documentation licence or copyright notice, so only the wire facts are used, in this project's own words and code) and its Activity
// object (https://docs.discord.com/developers/events/gateway-events). The facts used:
//   * the pipe is \\?\pipe\discord-ipc-{n}; a frame is a 4-byte opcode and a 4-byte payload length (the page's example bytes are little endian)
//     then the JSON payload; opcodes 0 handshake, 1 frame, 2 close, 3 ping, 4 pong;
//   * the handshake payload is {"v":1,"client_id":"..."}; Discord answers with a frame whose "evt" is "READY";
//   * a SET_ACTIVITY command is {"cmd":"SET_ACTIVITY","args":{"pid":N,"activity":{...}},"nonce":"..."}; the activity "type" may be 0 Playing,
//     2 Listening, 3 Watching or 5 Competing (this uses 2); "details" and "state" are text lines; a user may update 5 times per 20 seconds (so
//     this sends at most one update per 5 seconds and coalesces anything faster);
//   * a command before the handshake is answered with error code 4006.
//   NOT STATED by the page, so chosen here and said so: the length limit of details and state (this caps each at 128 UTF-8 bytes, cutting only
//   on a character boundary), the byte order (little endian, from the example), and how an activity is CLEARED (this omits "activity" from
//   args, as Discord's own game libraries do from memory; UNVERIFIED against a live Discord client).
//
// WHAT IS SHOWN. By default only that the user is listening: details "Listening to the radio", nothing else - no frequency, mode, station,
// place, call sign or file name. With showFrequencyMode ticked, the state line adds the tuned frequency and the demodulation mode ("7.050 MHz
// USB"). Nothing is written to disk or logged by this class.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_DISCORD_PRESENCE_HPP
#define CASCADE_NET_DISCORD_PRESENCE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cascade::net {

namespace discord {

enum class Op : std::uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };
inline constexpr std::size_t kMaxFrameBytes = 64u * 1024u;   // a frame longer than this is not Discord: the connection is dropped
inline constexpr std::size_t kMaxTextBytes = 128;

// 8-byte header (opcode, length: little endian u32 each) and the payload.
std::vector<std::uint8_t> encodeFrame(Op op, const std::string& payload);

// Reassembles frames from pieces. A violation (an unknown opcode, a length over the cap) is permanent.
class FrameParser {
public:
    struct Frame {
        Op op;
        std::string payload;
    };
    bool feed(const std::uint8_t* data, std::size_t n, std::vector<Frame>& out);
    const std::string& error() const { return error_; }

private:
    std::vector<std::uint8_t> buf_;
    std::string error_;
};

// A snowflake: 17 to 20 decimal digits. The only thing a client ID may be.
bool validClientId(const std::string& id);
std::string handshakePayload(const std::string& clientId);
// `activityJson` empty clears the activity.
std::string setActivityPayload(std::uint32_t pid, const std::string& nonce, const std::string& activityJson);
std::string activityJson(const std::string& details, const std::string& state);
std::string jsonEscape(const std::string& utf8);
// At most `maxBytes` bytes of `s`, cut on a character boundary (and never inside a multi-byte sequence).
std::string capUtf8(const std::string& s, std::size_t maxBytes = kMaxTextBytes);

enum class Reply : std::uint8_t { Other, Ready, Error };
struct ReplyInfo {
    Reply kind = Reply::Other;
    int code = 0;   // Error: the error code, 0 when absent
};
ReplyInfo parseReply(const std::string& payload);

}  // namespace discord

struct PresenceInput {
    bool listening = false;     // a receiver is running and the audio is on
    double frequencyHz = 0.0;
    std::string mode;           // the demodulation mode's name ("USB", "NFM" ...), never anything personal
};
struct PresenceText {
    bool show = false;
    std::string details;
    std::string state;
};
// What the profile line says for `in`. Pure.
PresenceText composePresence(const PresenceInput& in, bool showFrequencyMode);
std::string formatFrequency(double hz);   // "7.050 MHz", "810 kHz", "162.550 MHz"

struct DiscordOptions {
    std::string clientId;               // EMPTY = off: no thread, no pipe, nothing
    bool showFrequencyMode = false;     // default: only that the user is listening
    std::string pipeBase = R"(\\.\pipe\discord-ipc-)";   // the pipe is pipeBase + "0".."maxPipes-1" (tests point it at a stand-in server)
    int maxPipes = 10;
    int minIntervalMs = 5000;           // between SET_ACTIVITY frames (Discord allows 5 updates per 20 s)
    int retryMs = 15000;                // first wait before looking for Discord again; doubles up to maxRetryMs
    int maxRetryMs = 60000;
    int tickMs = 50;
    std::uint32_t pid = 0;              // 0: this process
};

// The state words: "off" (no client ID), "bad-id", "searching" (Discord not found yet), "connecting", "connected", "refused" (Discord answered the
// handshake with an error), "lost" (the connection broke; retrying).
class DiscordPresence {
public:
    explicit DiscordPresence(DiscordOptions options);
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // Safe from any thread, never blocks. The newest input wins; faster than minIntervalMs is coalesced; an unchanged line is not sent again.
    void update(const PresenceInput& in);
    // Remove the line now (and when this object ends).
    void clear();

    std::string state() const;
    std::size_t framesSent() const;     // SET_ACTIVITY frames
    std::size_t connects() const;
    std::size_t errorsSeen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_DISCORD_PRESENCE_HPP
