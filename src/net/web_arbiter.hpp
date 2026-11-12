// web_arbiter.hpp - who may TUNE among the people connected to the web page (feat/webmodels).
//
// WHAT IS NOT HERE. Roles. The OWNER / LISTENER definition, sign-in, listener caps, bans, kicks and the owner's note are the `feat/web-secure`
// lane's net/web_users.hpp (WebRoom). This model builds NO second role system: it takes two facts a caller already knows about each viewer,
// `isAdmin` (the web_users OWNER) and `mayTune` (an owner, or a listener when the server switch listenersMayTune is on), and answers a
// different question that WebRoom does not: when SEVERAL viewers are allowed to tune, who is allowed to act RIGHT NOW.
//
// THE MODEL. Two modes.
//   OPEN: every viewer with mayTune may act at any time (today's behaviour: the last request wins at the engine). The arbiter still lists
//     the viewers.
//   EXCLUSIVE: exactly one viewer, the HOLDER, may act. Everyone else with mayTune may ask (requestControl): if nobody holds control they get
//     it at once, otherwise they are queued (first come first served, capped at maxQueue) and the admin sees the queue. The holder gives it up
//     (releaseControl), loses it by being idle (no mayAct(touch) for holderIdleMs) or by leaving or timing out, and then the NEXT in the queue
//     is promoted. An ADMIN may always act (the admin override: a decision, written down: the person who owns the receiver is never locked
//     out of it by a guest), may hand control to a named viewer (grant), take it away (revoke: the holder loses control and the NEXT in the queue takes over, as after a release; to leave nobody in
//     control the admin switches to OPEN and back, which clears the holder and the queue),
//     and switch the mode (switching to OPEN clears the holder and the queue).
//
// IDENTIFIERS. A viewer is named here only by an OPAQUE ID the arbiter makes up (12 hex digits from a seeded generator): never the cookie,
// the name or the address. viewers() returns those ids and facts, nothing else, so it can go to the admin's page without leaking who anyone
// is. This file prints nothing and writes nothing (source-scan test).
//
// TIME. Every method takes `nowMs` (a millisecond counter the caller owns), so tests need no sleeps. Expiry happens inside every call
// (before it is answered), and tick() does it on its own for a caller with nothing else to say.
//
// THREADS. One mutex, every public method takes it; the invariants "at most one holder", "the holder is a connected viewer who may tune",
// "the queue holds connected, may-tune, non-holder viewers once each" are rechecked by the tests under concurrent callers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::net {

enum class ControlMode { Open, Exclusive };

struct ArbiterConfig {
    std::size_t maxViewers = 32;
    std::size_t maxQueue = 8;
    std::int64_t viewerTimeoutMs = 30000;  // no heartbeat for this long: the viewer is gone
    std::int64_t holderIdleMs = 60000;     // the holder has not acted for this long: control moves on
    std::int64_t maxHoldMs = 0;            // 0 = no ceiling on one stretch of holding
    std::uint64_t idSeed = 0;              // 0 = seed from the operating system's random device
};

enum class ArbResult {
    Ok,
    Unknown,        // no such viewer (never joined, left or timed out)
    NotAdmin,
    Full,           // join: maxViewers reached
    ListenerOnly,   // the viewer may not tune at all
    Queued,         // requestControl: waiting; see queuePosition()
    AlreadyHolder,
    QueueFull,
    NotExclusive,   // grant / request in OPEN mode (nothing to arbitrate)
    NotHolder,      // release by someone who does not hold control
    Denied,         // mayAct: exclusive and not the holder
};

const char* arbResultText(ArbResult r);

struct ViewerInfo {
    std::string id;
    bool isAdmin = false;
    bool mayTune = false;
    bool holder = false;
    std::size_t queuePosition = 0;  // 1 = next in line; 0 = not queued
    std::int64_t connectedMs = 0;
    std::int64_t idleMs = 0;
};

struct ControlDecision {
    bool allowed = false;
    bool viaOverride = false;  // an admin acting without holding control in EXCLUSIVE mode
    ArbResult why = ArbResult::Ok;
};

class ControlArbiter {
public:
    explicit ControlArbiter(const ArbiterConfig& cfg = ArbiterConfig{});

    ArbResult join(bool isAdmin, bool mayTune, std::int64_t nowMs, std::string& idOut);
    ArbResult leave(const std::string& id, std::int64_t nowMs);
    ArbResult heartbeat(const std::string& id, std::int64_t nowMs);
    // The caller learnt a viewer's rights changed (a ban, listenersMayTune switched): same effects as the join flags.
    ArbResult setRights(const std::string& id, bool isAdmin, bool mayTune, std::int64_t nowMs);

    // May this viewer perform a tuning operation now? With `touch` true (the default) an allowed HOLDER's idle clock restarts; an
    // admin's override never restarts the holder's clock (the holder is not "active" because the admin is).
    ControlDecision mayAct(const std::string& id, std::int64_t nowMs, bool touch = true);

    ArbResult requestControl(const std::string& id, std::int64_t nowMs);
    ArbResult releaseControl(const std::string& id, std::int64_t nowMs);
    // Also withdraws a queued request (the viewer changed their mind).
    ArbResult cancelRequest(const std::string& id, std::int64_t nowMs);

    ArbResult setMode(const std::string& adminId, ControlMode mode, std::int64_t nowMs);
    ArbResult grant(const std::string& adminId, const std::string& targetId, std::int64_t nowMs);
    ArbResult revoke(const std::string& adminId, std::int64_t nowMs);

    ControlMode mode(std::int64_t nowMs);
    std::string holder(std::int64_t nowMs);          // "" = nobody
    std::vector<ViewerInfo> viewers(std::int64_t nowMs);
    std::vector<std::string> queue(std::int64_t nowMs);
    std::size_t viewerCount(std::int64_t nowMs);
    // Increases on every change of mode, holder, queue or viewer list: the page polls it instead of the list.
    std::uint64_t epoch() const;

    void tick(std::int64_t nowMs);

private:
    struct Viewer {
        std::string id;
        bool isAdmin = false;
        bool mayTune = false;
        std::int64_t joinedMs = 0;
        std::int64_t lastSeenMs = 0;
        std::int64_t lastActionMs = 0;
    };
    Viewer* find(const std::string& id);
    void expireLocked(std::int64_t nowMs);
    void dropLocked(const std::string& id);
    void promoteLocked(std::int64_t nowMs);
    void setHolderLocked(const std::string& id, std::int64_t nowMs);
    std::string newIdLocked();
    void removeFromQueueLocked(const std::string& id);

    ArbiterConfig cfg_;
    mutable std::mutex mu_;
    std::vector<Viewer> viewers_;
    std::deque<std::string> queue_;
    std::string holder_;
    std::int64_t holderSinceMs_ = 0;
    ControlMode mode_ = ControlMode::Open;
    std::uint64_t rng_ = 0;
    std::uint64_t epoch_ = 1;
};

}  // namespace cascade::net
