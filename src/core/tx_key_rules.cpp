// tx_key_rules.cpp - see tx_key_rules.hpp. The decisions and their sentences
// follow the reference engine (third_party/foxsdr_api/reference/mock_engine,
// applyKey / unkey / tickTransmitter / disarmRemote / markPrincipal) rule for
// rule; the review rounds each rule answers are named where it is enforced.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tx_key_rules.hpp"

#include <algorithm>
#include <utility>

namespace cascade::core {

namespace {

std::int64_t clampTimer(std::int64_t v, std::int64_t standard) {
    return std::clamp<std::int64_t>(v, 10, standard);
}

}  // namespace

TxKeyTimers TxKeyTimers::shortened() const {
    TxKeyTimers t;
    t.pttHoldMs = clampTimer(pttHoldMs, FOXAPI_PTT_HOLD_MS);
    t.latchTimeoutMs = clampTimer(latchTimeoutMs, FOXAPI_LATCH_TIMEOUT_MS);
    t.keepaliveMs = clampTimer(keepaliveMs, FOXAPI_KEEPALIVE_MS);
    t.remoteKeepaliveMs = clampTimer(remoteKeepaliveMs, FOXAPI_REMOTE_KEEPALIVE_MS);
    t.rearmMs = clampTimer(rearmMs, FOXAPI_LATCH_REARM_MS);
    return t;
}

TxKeyRules::TxKeyRules(TxKeyTimers timers) : t_(timers.shortened()) {}

std::int64_t TxKeyRules::holdRemainingMs(std::int64_t now) const {
    return holdUntilMs_ != 0 ? std::max<std::int64_t>(0, holdUntilMs_ - now) : 0;
}

std::int64_t TxKeyRules::latchRemainingMs(std::int64_t now) const {
    return latched_ ? std::max<std::int64_t>(0, latchUntilMs_ - now) : 0;
}

bool TxKeyRules::releaseFirst(const std::string& principal, std::uint64_t session) const {
    const auto m = marks_.find(principal);
    return m != marks_.end() && m->second.exempt.count(session) == 0;
}

std::vector<std::string> TxKeyRules::takeUnkeyEvents() {
    std::vector<std::string> out;
    out.swap(unkeyEvents_);
    return out;
}

// RULE 3: a principal is marked (or re-marked) by any end of its latch other
// than its owning session's LATCH 0, and a new end withdraws every exemption
// (round-4 review: the mark died with its session; round-5: a LATCH 0 from any
// session with nothing latched cleared it).
void TxKeyRules::mark(const std::string& principal, const std::string& why) {
    if (principal.empty()) { return; }
    Mark& m = marks_[principal];
    m.reason = why;
    m.exempt.clear();
}

// The engine's account of an end - never the owner's own release.
void TxKeyRules::unkey(const std::string& why, std::int64_t now) {
    if (latched_) {
        latchEnded_ = true;
        latchEndedMs_ = now;  // however it ended, the re-arm time starts (rule 4)
        latchEndReason_ = why;
        mark(ownerPrincipal_, why);
    }
    keyOwner_ = 0;
    ownerRemote_ = false;
    ownerPrincipal_.clear();
    holdUntilMs_ = 0;
    latched_ = false;
    latchUntilMs_ = 0;
    unkeyReason_ = why;
    ++changes_;
    unkeyEvents_.push_back(why);
}

void TxKeyRules::endKey(const std::string& why, std::int64_t now) {
    if (keyOwner_ != 0) { unkey(why, now); }
}

// RULE 5: the consent lapses, and a remote key goes with it.
void TxKeyRules::disarmRemote(const std::string& why, std::int64_t now) {
    if (remoteArmedBy_ == 0) { return; }
    remoteArmedBy_ = 0;
    ++changes_;
    if (keyOwner_ != 0 && ownerRemote_) { unkey(why, now); }
}

TxKeyAnswer TxKeyRules::key(const TxKeySender& s, bool latch, bool on, bool txOpen, std::int64_t now) {
    TxKeyAnswer a;
    if (!on) {
        // RULE 6: anyone with the grant may OPEN the key - never the unsafe
        // direction. A PTT 0 ends the hold; a LATCH 0 ends the latch.
        if (!latch && holdUntilMs_ != 0) { holdUntilMs_ = 0; }
        if (latch) {
            if (latched_) {
                latchEnded_ = true;
                latchEndedMs_ = now;  // the re-arm time starts now
                if (keyOwner_ == s.session) {
                    latchEndReason_ = "released by its owner (LATCH 0)";  // no mark
                } else {
                    // RULE 3: released from an interface that does NOT hold
                    // the latch - typically the operator pressing RELEASE in
                    // another window because the one holding it is stuck.
                    latchEndReason_ = ownerPrincipal_ == s.principal
                                          ? "released from another interface of the same operator (LATCH 0)"
                                          : "another interface released it (LATCH 0)";
                    mark(ownerPrincipal_, latchEndReason_);
                }
            }
            latched_ = false;
            latchUntilMs_ = 0;
            // A LATCH 0 exempts THE SESSION THAT SENT IT and no other, whatever
            // it found - and a session that has gone exempts nobody (its
            // release still releases, above).
            if (s.open) {
                const auto m = marks_.find(s.principal);
                if (m != marks_.end()) { m->second.exempt.insert(s.session); }
            }
        }
        if (holdUntilMs_ == 0 && !latched_ && keyOwner_ != 0) {
            keyOwner_ = 0;
            ownerRemote_ = false;
            ownerPrincipal_.clear();
            unkeyReason_.clear();  // the operator opened it themselves
        }
        ++changes_;
        return a;
    }
    if (!txOpen) {
        a.status = FOXAPI_NO_DEVICE;
        a.message = "there is no transmitter open";
        return a;
    }
    if (keyOwner_ != 0 && keyOwner_ != s.session) {
        a.status = FOXAPI_BUSY;
        a.message = "another interface holds the key";
        return a;
    }
    if (s.remote && remoteArmedBy_ == 0) {
        // Checked again here: consent may have been withdrawn since submit.
        a.status = FOXAPI_DENIED;
        a.message = "the local operator has not given consent to remote transmit";
        return a;
    }
    if (latch && latched_) {
        // RULE 2: a re-latch changes nothing; the ORIGINAL deadline stands.
        a.status = FOXAPI_NO_CHANGE;
        a.applied0 = static_cast<double>(std::max<std::int64_t>(0, latchUntilMs_ - now));
        a.message = "already latched; the latch keeps its original deadline";
        return a;
    }
    if (latch && latchEnded_ && now - latchEndedMs_ < t_.rearmMs) {
        // RULE 4 (round-2 review: LATCH 0 + LATCH 1 every frame kept a 400 ms
        // latch down for 1,494 ms).
        a.status = FOXAPI_DENIED;
        a.applied0 = static_cast<double>(t_.rearmMs - (now - latchEndedMs_));
        a.message = "no latch closes within " + std::to_string(t_.rearmMs) + " ms of the last one ending (" +
                    latchEndReason_ + ")";
        return a;
    }
    if (latch) {
        const auto m = marks_.find(s.principal);
        if (m != marks_.end() && m->second.exempt.count(s.session) == 0) {
            // RULE 3: this operator's last latch ended by something other than
            // its owner's own LATCH 0, and THIS interface has not released
            // since.
            a.status = FOXAPI_DENIED;
            a.message = "the latch ended (" + m->second.reason + "): this interface must send LATCH 0 first";
            return a;
        }
    }
    keyOwner_ = s.session;
    ownerRemote_ = s.remote;
    ownerPrincipal_ = s.principal;
    if (!latch) {
        holdUntilMs_ = now + t_.pttHoldMs;  // RULE 1: a hold, never a switch
    } else {
        latched_ = true;
        latchUntilMs_ = now + t_.latchTimeoutMs;
    }
    a.applied0 = 1.0;
    ++changes_;
    return a;
}

TxKeyAnswer TxKeyRules::arm(const TxKeySender& s, bool on, std::int64_t now) {
    TxKeyAnswer a;
    if (on) {
        // The consent belongs to THIS local session: it lapses when the
        // session disarms, closes or stops answering.
        remoteArmedBy_ = s.session;
        ++changes_;
    } else {
        disarmRemote("the local operator withdrew consent to remote transmit", now);
    }
    a.applied0 = remoteArmedBy_ != 0 ? 1.0 : 0.0;
    return a;
}

void TxKeyRules::sessionGone(std::uint64_t session, bool detached, std::int64_t now) {
    for (auto& m : marks_) { m.second.exempt.erase(session); }
    if (keyOwner_ == session) {
        unkey(detached ? "the login of the interface holding the key was revoked"
                       : "the interface holding the key closed its session",
              now);
    }
    if (remoteArmedBy_ == session) {
        disarmRemote(detached ? "the login of the local interface that gave consent was revoked"
                              : "the local interface that gave consent to remote transmit closed",
                     now);
    }
}

void TxKeyRules::tick(std::int64_t now, const Sessions& sessions) {
    if (remoteArmedBy_ != 0) {
        const Liveness armer = sessions.find(remoteArmedBy_);
        if (!armer.present) {
            disarmRemote("the local interface that gave consent to remote transmit closed", now);
        } else if (now - armer.lastBeatMs > t_.keepaliveMs) {
            disarmRemote("the local interface that gave consent to remote transmit stopped answering", now);
        }
    }
    if (keyOwner_ == 0) { return; }
    if (holdUntilMs_ != 0 && now >= holdUntilMs_) {
        holdUntilMs_ = 0;
        if (!latched_) {
            unkey("the PTT was not re-asserted within the hold", now);
            return;
        }
        ++changes_;
    }
    if (latched_ && now >= latchUntilMs_) {
        unkey("the latch timed out", now);  // marks the owner, as every engine-side end does
        return;
    }
    const Liveness owner = sessions.find(keyOwner_);
    if (!owner.present) {
        unkey("the interface holding the key closed its session", now);
        return;
    }
    // RULE 7: a remote session's keep-alive is the web remote's hold (2 s), a
    // local one's the frame loop's dead-man (1 s).
    const std::int64_t limit = owner.remote ? t_.remoteKeepaliveMs : t_.keepaliveMs;
    if (now - owner.lastBeatMs > limit) { unkey("the interface holding the key stopped answering", now); }
}

std::string TxKeyRules::stateText() const {
    std::string s = "owner=" + std::to_string(keyOwner_) + " latched=" + std::to_string(latched_ ? 1 : 0) +
                    " hold=" + std::to_string(holdUntilMs_ != 0 ? 1 : 0) + " armedBy=" +
                    std::to_string(remoteArmedBy_) + " ended=" + std::to_string(latchEnded_ ? 1 : 0) +
                    " endReason=[" + latchEndReason_ + "] unkey=[" + unkeyReason_ + "] marks={";
    for (const auto& kv : marks_) {
        s += kv.first + ":[" + kv.second.reason + "]:(";
        for (std::uint64_t id : kv.second.exempt) { s += std::to_string(id) + ","; }
        s += ") ";
    }
    return s + "}";
}

}  // namespace cascade::core
