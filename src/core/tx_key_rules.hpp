// tx_key_rules.hpp - WHO MAY KEY THE TRANSMITTER, AND FOR HOW LONG: the engine
// API's rules for the key (API 0.2, "THE KEY" in third_party/foxsdr_api/
// foxsdr_api.h and reference/docs/API.md section 6.10), adopted by the owner's
// decision of 2026-09-29 (docs/engine-stage4.md section 3, decision 1).
//
// WHAT THIS IS. The decisions, and nothing else: which session holds the key,
// whether the PTT's hold has run out, whether the latch has timed out, which
// operator (PRINCIPAL) is marked and which of its sessions have released since,
// whether the local operator has consented to a remote PTT, and whether the
// key holder's session is still answering. No thread, no radio, no clock of its
// own (every call is given `now`): core::Transmitter does the transmitting and
// keeps its own, independent dead-man's handles (core/transmitter.hpp), and
// engine/fox_engine_api.cpp calls these rules on the engine's control pass and
// drives the Transmitter from what they decide. So every rule here can be
// tested by itself, with a clock the test turns (tests/test_tx_key_rules.cpp),
// and again through the table (tests/test_engine_api_transmit.cpp, the port of
// the reference mock engine's test_transmit_safety).
//
// THE RULES (API 0.2; the sentences are the reference engine's, word for word,
// because interfaces and tests match on them):
//   1. TX_PTT 1 is a HOLD, not a switch: it keys, or extends, for pttHoldMs
//      (FOXAPI_PTT_HOLD_MS, 2 s) and no more. The interface re-asserts while
//      the operator holds the key; silence opens it. TX_PTT 0 releases now.
//   2. TX_LATCH is for LOCAL sessions only (the table refuses a remote one at
//      submit). A latch opens itself latchTimeoutMs (60 s) after it CLOSED; a
//      LATCH 1 while latched changes nothing (NO_CHANGE) and the original
//      deadline stands.
//   3. ONE LATCH, NOT A CHAIN. A latch belongs to a PRINCIPAL ("local" for
//      every local session - the operator at this machine). Any end other
//      than the OWNING session's LATCH 0 marks the principal; while marked, a
//      LATCH 1 from any of its sessions is refused (DENIED, naming how the
//      latch ended) until THAT session itself sends LATCH 0. A LATCH 0
//      exempts only its sender; every new marking end withdraws every
//      exemption; the mark is never cleared; a gone session exempts nobody.
//   4. After ANY latch ends, no latch closes again for rearmMs
//      (FOXAPI_LATCH_REARM_MS, 1 s).
//   5. A REMOTE session's PTT needs the local operator's consent: TX_REMOTE_ARM
//      1 from a LOCAL session. The consent belongs to that session and lapses
//      when it disarms, closes or stops answering, opening a remote key at
//      once.
//   6. One key holder at a time; anyone with the grant may OPEN the key.
//   7. A session whose heartbeat is older than its keep-alive (keepaliveMs
//      local, 1 s; remoteKeepaliveMs remote, 2 s) loses the key, and so does a
//      closed or detached session. Only heartbeat() counts.
//   8. The key never survives a stop, a fault, a transmitter change or close
//      (the engine calls endKey), and is never restored from configuration
//      (nothing here is persisted).
//
// THREADING. One thread: the one that runs the engine's control pass (the
// engine's control thread; the window's frame while it still pumps the engine
// itself). Nothing here locks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TX_KEY_RULES_HPP
#define CASCADE_CORE_TX_KEY_RULES_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "foxsdr_api.h"

namespace cascade::core {

// The timers. The standard values are the API's; an engine created with test
// options may only SHORTEN them (TxKeyTimers::shortened).
struct TxKeyTimers {
    std::int64_t pttHoldMs = FOXAPI_PTT_HOLD_MS;
    std::int64_t latchTimeoutMs = FOXAPI_LATCH_TIMEOUT_MS;
    std::int64_t keepaliveMs = FOXAPI_KEEPALIVE_MS;
    std::int64_t remoteKeepaliveMs = FOXAPI_REMOTE_KEEPALIVE_MS;
    std::int64_t rearmMs = FOXAPI_LATCH_REARM_MS;
    // Each field clamped into [10 ms, its standard value]: nothing may
    // lengthen a safety timer.
    TxKeyTimers shortened() const;
};

// Who sent a key command, as the rules need to know it.
struct TxKeySender {
    std::uint64_t session = 0;
    bool remote = false;
    std::string principal;  // "local" for every LOCAL session
    bool open = true;       // false: closed or detached before this was applied
};

// A command's answer: FOXAPI_OK (applied), or why nothing changed.
struct TxKeyAnswer {
    std::int32_t status = FOXAPI_OK;
    double applied0 = 0.0;
    std::string message;
};

class TxKeyRules {
public:
    explicit TxKeyRules(TxKeyTimers timers = TxKeyTimers{});

    const TxKeyTimers& timers() const { return t_; }

    // --- the commands ----------------------------------------------------------
    // TX_PTT (latch false) or TX_LATCH (latch true). `txOpen`: a transmitter
    // is open now (a key-down with none is NO_DEVICE).
    TxKeyAnswer key(const TxKeySender& s, bool latch, bool on, bool txOpen, std::int64_t now);
    // TX_REMOTE_ARM. Only a LOCAL session's is ever applied (the table refuses
    // a remote one at submit); its consent is its own.
    TxKeyAnswer arm(const TxKeySender& s, bool on, std::int64_t now);

    // --- the engine's own ends -------------------------------------------------
    // A stop, a fault, a transmitter change or close, or the Transmitter
    // letting go by itself: opens the key if anyone holds it (and marks a
    // latch's principal). A no-op with no key held.
    void endKey(const std::string& why, std::int64_t now);
    // A session closed or detached (before the pass applies anything): its
    // key and its consent go, its own exemptions go.
    void sessionGone(std::uint64_t session, bool detached, std::int64_t now);

    // --- once a pass -----------------------------------------------------------
    struct Liveness {
        bool present = false;        // open (not closed, not detached)
        bool remote = false;
        std::int64_t lastBeatMs = 0;
    };
    class Sessions {
    public:
        virtual Liveness find(std::uint64_t session) const = 0;

    protected:
        ~Sessions() = default;
    };
    // The hold, the latch's deadline, the key holder's keep-alive and the
    // consenting session's.
    void tick(std::int64_t now, const Sessions& sessions);

    // --- what they decided -----------------------------------------------------
    std::uint64_t keyOwner() const { return keyOwner_; }
    bool ownerRemote() const { return ownerRemote_; }
    bool keyed() const { return keyOwner_ != 0; }
    bool latched() const { return latched_; }
    bool holding() const { return holdUntilMs_ != 0; }
    std::int64_t holdRemainingMs(std::int64_t now) const;
    std::int64_t latchRemainingMs(std::int64_t now) const;
    std::uint64_t remoteArmedBy() const { return remoteArmedBy_; }
    // Why the key last opened ON ITS OWN ("" when the operator opened it).
    const std::string& unkeyReason() const { return unkeyReason_; }
    // FOXAPI_RX_TX_LATCH_RELEASE_FIRST for this session.
    bool releaseFirst(const std::string& principal, std::uint64_t session) const;
    bool marked(const std::string& principal) const { return marks_.count(principal) != 0; }
    // Bumped by every change of the above (the snapshot's txSeq follows it).
    std::uint64_t changes() const { return changes_; }
    // The FOXAPI_EVENT_TX_UNKEYED sentences since the last call, oldest first.
    std::vector<std::string> takeUnkeyEvents();
    // The key, the latch, the marks and their exemptions as text, times left
    // out (two runs that must end in the same state produce the same text).
    std::string stateText() const;

private:
    void unkey(const std::string& why, std::int64_t now);
    void disarmRemote(const std::string& why, std::int64_t now);
    void mark(const std::string& principal, const std::string& why);

    TxKeyTimers t_;
    std::uint64_t keyOwner_ = 0;
    bool ownerRemote_ = false;
    std::string ownerPrincipal_;
    std::uint64_t remoteArmedBy_ = 0;
    std::int64_t holdUntilMs_ = 0;
    bool latched_ = false;
    std::int64_t latchUntilMs_ = 0;
    struct Mark {
        std::string reason;
        std::set<std::uint64_t> exempt;
    };
    std::map<std::string, Mark> marks_;
    bool latchEnded_ = false;  // a latch has ended, for any reason
    std::int64_t latchEndedMs_ = 0;
    std::string latchEndReason_;
    std::string unkeyReason_;
    std::uint64_t changes_ = 1;
    std::vector<std::string> unkeyEvents_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TX_KEY_RULES_HPP
