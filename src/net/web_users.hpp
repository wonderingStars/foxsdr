// Who is on the web page (feat/web-secure): the OWNER and the LISTENERS, their sessions and limits, bans, the owner's note, the optional
// chat and the in-memory audit trail.
//
// WHY THIS IS A MODULE OF ITS OWN. Like net/web_auth.hpp it is pure computation over strings and an injected clock - no sockets, no HTTP,
// no files - so every rule below can be driven deterministically from a test (concurrent joins at the cap, a ban that ends a live session,
// an idle limit, the chat's rate limit) without a network, and the server only asks it questions.
//
// THE DEFINITION (decided by the owner of the product, 2026-11-11):
//   * the OWNER signs in with the account the page always had and has full control of the receiver;
//   * a LISTENER may only LOOK and LISTEN: the spectrum, the status (with the host's paths and device identifiers taken out), the audio, the
//     decoded pictures and tiles. Every tuning and settings operation is refused (net/web_server.cpp holds that gate, with one server switch,
//     listenersMayTune, that lets a listener tune; nothing else is ever opened to a listener);
//   * a listener signs in with a NAME of its choosing and the one shared LISTENER password the owner set. The name is a handle, not an
//     identity: it exists so the owner can see who is on, kick, and ban by name. A name equal to the owner's account name is refused, so a
//     listener can never pass for the owner in the chat or the list.
//
// WHAT NEVER LEAVES MEMORY. Sessions, the ban lists, the chat, the note and the audit trail live in this object and nowhere else: this file
// opens no file and prints nothing (tests/test_web_users.cpp scans the source for both). The host copies standard error into its log and its
// uploaded reports and writes every setting to the config file, and a listener's name, its address and what it typed are people's data, not
// the product's. So the ban lists end with the server (a restart clears them: writing addresses to disk was the other choice, and is the
// owner's to make), a chat line is held only in a ring of kChatRing and never gets past it, and an audit entry carries the kind of event and
// the listener's chosen name - never an address and never text a person wrote.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "net/web_auth.hpp"

namespace cascade::net {

enum class Role { None, Owner, Listener };

// The limits. The cap's default is small on purpose: every listener holds an audio worker and a live-update worker of the server.
inline constexpr std::size_t kDefaultMaxListeners = 4;
inline constexpr std::size_t kMaxListenersCeiling = 32;     // = kMaxSessions: a full room still leaves the owner a session
inline constexpr std::int64_t kDefaultListenerIdleSeconds = 15 * 60;
inline constexpr std::int64_t kDefaultListenerSessionSeconds = 8 * 60 * 60;
inline constexpr std::size_t kMaxListenerNameLength = 24;
inline constexpr std::size_t kMaxChatBytes = 200;            // after sanitising, before escaping
inline constexpr std::size_t kMaxNoteBytes = 200;
inline constexpr std::size_t kChatRing = 50;                 // the most recent lines kept; older ones are gone, not archived
inline constexpr std::size_t kAuditRing = 200;
inline constexpr std::size_t kMaxBansPerList = 256;
inline constexpr std::size_t kChatBurst = 3;                 // lines ...
inline constexpr std::int64_t kChatWindowSeconds = 10;       // ... per this many seconds, per person

struct Identity {
    Role role = Role::None;
    std::string name;      // a listener's handle; empty for the owner
    std::string address;   // the TRUE client address the sign-in came from (a listener's; empty for the owner)
};

struct RoomConfig {
    bool listenersEnabled = false;
    std::size_t maxListeners = kDefaultMaxListeners;      // 1 .. kMaxListenersCeiling (clamped here, refused by the bind policy)
    std::int64_t idleSeconds = kDefaultListenerIdleSeconds;       // 0 = no limit
    std::int64_t sessionSeconds = kDefaultListenerSessionSeconds; // 0 = no limit
    bool listenersMayTune = false;
    bool chatEnabled = false;
};

struct ListenerInfo {
    std::string name;
    std::string address;
    std::int64_t signedInSeconds = 0;
    std::int64_t idleSeconds = 0;
};

enum class JoinResult {
    Ok,
    Disabled,        // the room has no listeners switched on
    BadName,         // empty, too long, or a character outside the allowed set
    ReservedName,    // the owner's account name
    BannedAddress,
    BannedName,
    NameTaken,       // a live listener already has it
    Full,            // the cap
    Error,           // no CSPRNG
};

enum class ChatResult { Ok, Disabled, Empty, TooLong, BadText, RateLimited };

struct ChatLine {
    std::uint64_t seq = 0;
    std::int64_t t = 0;
    std::string name;       // the speaker's handle; the owner's line says "owner"
    bool owner = false;
    std::string html;       // ESCAPED text: a client may put it in the page as it is
};

struct OwnerNote {
    std::uint64_t seq = 0;  // 0 = no note has ever been set; the number changes with every set or clear
    std::string html;       // escaped; empty = cleared
};

enum class AuditKind { Join, Leave, Expired, Kick, Ban, Unban, RefusedJoin, RefusedOp, Note, Chat };
const char* auditWord(AuditKind k);

struct AuditEvent {
    std::uint64_t seq = 0;
    std::int64_t t = 0;
    AuditKind kind = AuditKind::Join;
    std::string name;       // the listener's chosen handle, or empty. NEVER an address, NEVER what anyone typed.
};

struct ModResult {
    bool ok = false;           // false: bad argument or the list is full
    std::size_t ended = 0;     // live listener sessions this ended
};

// --- Pure helpers (no state) ---------------------------------------------------------------------------------------------------------

// A listener name: 1..kMaxListenerNameLength characters of ASCII letters, digits, '_', '-', '.', and single inner spaces; no leading or
// trailing space. Returns false (out untouched) for anything else - a handle with control characters, markup or look-alike letters is not
// one anybody needs, and refusing is simpler than normalising.
bool sanitizeListenerName(const std::string& raw, std::string& out);

// Free text a person typed (a chat line, the owner's note): valid UTF-8 only; control characters, line breaks and the bidirectional-override
// and zero-width characters are turned into spaces or dropped, runs of spaces collapse, the ends are trimmed. Never throws.
ChatResult sanitizeText(const std::string& raw, std::size_t maxBytes, std::string& out);

// & < > " ' and the backtick, as character references. Applied to everything a person typed before it leaves this module.
std::string escapeHtml(const std::string& text);

// Lower-cases ASCII (names and addresses are compared this way).
std::string foldCase(const std::string& s);

// --- The room --------------------------------------------------------------------------------------------------------------------------

class WebRoom {
public:
    // Replaces the configuration and EMPTIES the room (sessions, bans, chat, note, audit): called when the server starts.
    void configure(const RoomConfig& cfg);
    RoomConfig config() const;
    // Drops everything but keeps the configuration (the server stopping).
    void clear();

    // --- Sessions ---------------------------------------------------------------------------------------------------------------------
    bool issueOwner(std::int64_t now, std::string& tokenOut, std::string& error);
    JoinResult joinListener(std::int64_t now, const std::string& rawName, const std::string& address, const std::string& ownerName,
                            std::string& tokenOut, std::string& error);

    // Who holds `token`. `touch` says the request is activity (it resets the idle clock); a stream re-checking its session passes false, so
    // an abandoned tab with its audio still playing is still found idle. A listener past its idle or session limit is ended here.
    bool identify(const std::string& token, std::int64_t now, bool touch, Identity& out);
    bool revoke(const std::string& token, std::int64_t now);
    void revokeAll();

    // Ends listeners past a limit; returns how many it ended. Called on the server's idle tick.
    std::size_t tick(std::int64_t now);

    std::size_t ownerSessions() const;
    std::size_t listenerCount(std::int64_t now);                  // live listeners (limits applied first)
    std::vector<ListenerInfo> listeners(std::int64_t now);

    // --- Moderation -------------------------------------------------------------------------------------------------------------------
    ModResult kick(const std::string& name, std::int64_t now);                // ends the sessions of that name
    ModResult banName(const std::string& name, std::int64_t now);             // refuses it from now on AND ends its sessions
    ModResult banAddress(const std::string& address, std::int64_t now);       // the same, by the address the sign-in came from
    bool unbanName(const std::string& name, std::int64_t now);
    bool unbanAddress(const std::string& address, std::int64_t now);
    bool isBannedAddress(const std::string& address) const;
    bool isBannedName(const std::string& name) const;
    std::vector<std::string> bannedNames() const;
    std::vector<std::string> bannedAddresses() const;

    // --- Note and chat ----------------------------------------------------------------------------------------------------------------
    // The owner's broadcast note: shown to every listener until cleared (empty text clears). Returns the same codes as chat.
    ChatResult setNote(const std::string& text, std::int64_t now);
    OwnerNote note() const;

    // A chat line. Off unless the room's chatEnabled is set. Rate limited per person (kChatBurst lines per kChatWindowSeconds).
    ChatResult postChat(const Identity& who, const std::string& text, std::int64_t now);
    std::vector<ChatLine> chatSince(std::uint64_t afterSeq) const;
    std::uint64_t chatSeq() const;

    // --- Audit ------------------------------------------------------------------------------------------------------------------------
    void audit(AuditKind kind, const std::string& name, std::int64_t now);   // for the server's refused operations
    std::vector<AuditEvent> auditSince(std::uint64_t afterSeq) const;

private:
    struct Listener {
        std::vector<std::uint8_t> tokenHash;
        std::string name;        // as chosen (display)
        std::string key;         // folded
        std::string address;
        std::int64_t joinedAt = 0;
        std::int64_t lastSeen = 0;
    };

    // Callers hold mutex_.
    void auditLocked(AuditKind kind, const std::string& name, std::int64_t now);
    std::size_t purgeLocked(std::int64_t now);
    std::size_t endWhereLocked(bool byName, const std::string& key, std::int64_t now, AuditKind why);
    void eraseListenerLocked(std::size_t index);

    mutable std::mutex mutex_;
    RoomConfig cfg_;
    SessionStore owner_;                    // owner tokens: the same store, TTL and cap as before listeners existed
    std::vector<Listener> listeners_;
    std::vector<std::string> bannedNames_;      // folded
    std::vector<std::string> bannedAddresses_;  // folded
    std::deque<ChatLine> chat_;
    std::uint64_t chatSeq_ = 0;
    std::map<std::string, std::deque<std::int64_t>> chatTimes_;
    OwnerNote note_;
    std::deque<AuditEvent> audit_;
    std::uint64_t auditSeq_ = 0;
};

// What a listener may ask of the receiver when the server switch lets them tune: the request is allowed iff, once the tuning fields are
// taken away, NOTHING is left (ControlRequest::empty() is the single list of every field, so a field added later is refused here until
// someone decides otherwise). A request with nothing in it is refused too. (Defined in web_users.cpp; the request type is only forward-declared here.)
struct ControlRequest;
bool listenerMayTune(const ControlRequest& req);

}  // namespace cascade::net
