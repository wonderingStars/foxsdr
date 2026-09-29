// test_tx_key_rules.cpp - THE KEY (API 0.2), rule by rule, on core::TxKeyRules
// alone: no engine, no thread, no radio, and a clock the test turns - so each
// rule is pinned by what the rules DECIDE, with no scheduler in the way
// (docs/engine-stage4.md, decision 1). The same rules are held again through
// the engine's table, with real threads and the real transmitter, by
// tests/test_engine_api_transmit.cpp.
//
//   R1  TX_PTT 1 is a HOLD: it keys for pttHoldMs and no more; re-asserting
//       extends it; silence opens it ("not re-asserted"); PTT 0 opens it now,
//       with no reason given (the operator's own act).
//   R2  a latch opens itself latchTimeoutMs after it CLOSED; LATCH 1 while
//       latched is NO_CHANGE and does not move the deadline.
//   R3  ONE LATCH, NOT A CHAIN: every end but the owner's own LATCH 0 marks
//       the principal; LATCH 1 is refused to every session of it until THAT
//       session sends LATCH 0; a LATCH 0 exempts only its sender; a new end
//       withdraws every exemption; a gone session exempts nobody; the owner's
//       own LATCH 0 marks nothing.
//   R4  after any latch ends, none closes again for rearmMs.
//   R5  a remote PTT needs consent from a LOCAL session, which lapses when it
//       disarms, closes or stops answering - opening a remote key with it.
//   R6  one key holder; anyone may open the key.
//   R7  a key holder whose heartbeat is older than its keep-alive (1 s local,
//       2 s remote) loses the key; a closed or detached one too.
//   R8  a stop, a fault, a transmitter change or close opens the key (endKey),
//       and a latch ended that way marks its principal.
//   T   the test options may only SHORTEN the timers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "core/tx_key_rules.hpp"
#include "test_check.hpp"

using cascade::core::TxKeyAnswer;
using cascade::core::TxKeyRules;
using cascade::core::TxKeySender;
using cascade::core::TxKeyTimers;

namespace {

// The sessions the rules ask about in tick(): which are open, local or
// remote, and when each last called heartbeat().
class World final : public TxKeyRules::Sessions {
public:
    TxKeyRules::Liveness find(std::uint64_t id) const override {
        const auto it = s_.find(id);
        if (it == s_.end()) { return {}; }
        return it->second;
    }
    void open(std::uint64_t id, bool remote, std::int64_t now) { s_[id] = {true, remote, now}; }
    void beat(std::uint64_t id, std::int64_t now) { s_[id].lastBeatMs = now; }
    void close(std::uint64_t id) { s_.erase(id); }

private:
    std::map<std::uint64_t, TxKeyRules::Liveness> s_;
};

TxKeySender local(std::uint64_t id) { return {id, false, "local", true}; }
TxKeySender remote(std::uint64_t id) { return {id, true, "token:t", true}; }

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

bool sawEvent(TxKeyRules& r, const char* part) {
    bool seen = false;
    for (const std::string& e : r.takeUnkeyEvents()) { seen = seen || has(e, part); }
    return seen;
}

TxKeyTimers fast() {
    TxKeyTimers t;
    t.pttHoldMs = 200;
    t.latchTimeoutMs = 400;
    t.keepaliveMs = 150;
    t.remoteKeepaliveMs = 300;
    return t;
}

}  // namespace

int main() {
    std::printf("test_tx_key_rules\n");

    // --- R1: the PTT is a hold ---------------------------------------------------
    {
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        CHECK(r.key(local(1), false, true, true, 0).status == FOXAPI_OK);
        CHECK(r.keyed() && r.keyOwner() == 1u && r.holdRemainingMs(0) == 200);
        // Beats alone do not keep it: at the hold's end it opens.
        for (std::int64_t t = 10; t < 200; t += 10) {
            w.beat(1, t);
            r.tick(t, w);
        }
        CHECK(r.keyed());
        w.beat(1, 200);
        r.tick(200, w);
        CHECK(!r.keyed());
        CHECK(has(r.unkeyReason(), "not re-asserted"));
        CHECK(sawEvent(r, "not re-asserted"));
        // Re-asserted inside the hold, it stays keyed as long as it goes on.
        bool stayed = true;
        CHECK(r.key(local(1), false, true, true, 300).status == FOXAPI_OK);
        for (std::int64_t t = 300; t < 1300; t += 60) {
            w.beat(1, t);
            r.key(local(1), false, true, true, t);
            r.tick(t + 1, w);
            stayed = stayed && r.keyed();
        }
        CHECK(stayed);
        // PTT 0 opens it at once, and it is the operator's doing: no reason.
        CHECK(r.key(local(1), false, false, true, 1300).status == FOXAPI_OK);
        CHECK(!r.keyed());
        CHECK(r.unkeyReason().empty());
        std::printf("R1 hold: keyed 200 ms by one PTT, held 1000 ms by re-asserting, released by PTT 0\n");
    }

    // --- R2: the latch's own deadline, and no extension ------------------------------
    {
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        CHECK(r.key(local(1), true, true, true, 0).status == FOXAPI_OK);
        CHECK(r.latched() && r.latchRemainingMs(0) == 400);
        const TxKeyAnswer again = r.key(local(1), true, true, true, 300);
        CHECK(again.status == FOXAPI_NO_CHANGE);
        CHECK(again.applied0 == 100.0);        // the ORIGINAL deadline, 100 ms left
        CHECK(r.latchRemainingMs(300) == 100);
        w.beat(1, 399);
        r.tick(399, w);
        CHECK(r.keyed());
        w.beat(1, 400);
        r.tick(400, w);
        CHECK(!r.keyed() && !r.latched());
        CHECK(has(r.unkeyReason(), "latch timed out"));
        std::printf("R2 latch: re-latch at 300 ms NO_CHANGE with 100 ms left; opened itself at 400 ms\n");
    }

    // --- R3: one latch, not a chain ------------------------------------------------------
    {
        // A timeout marks the principal: the owner's LATCH 1 is refused (past
        // the re-arm time) until it sends LATCH 0; then it latches again.
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        w.open(2, false, 0);
        CHECK(r.key(local(1), true, true, true, 0).status == FOXAPI_OK);
        w.beat(1, 400);
        r.tick(400, w);  // timed out
        CHECK(r.marked("local"));
        CHECK(r.releaseFirst("local", 1) && r.releaseFirst("local", 2));
        const TxKeyAnswer stuck = r.key(local(1), true, true, true, 2000);
        CHECK(stuck.status == FOXAPI_DENIED);
        CHECK(has(stuck.message, "timed out") && has(stuck.message, "LATCH 0"));
        // Another session of the same operator is refused as well...
        CHECK(r.key(local(2), true, true, true, 2000).status == FOXAPI_DENIED);
        // ...and a session opened LATER (a reconnect) too.
        w.open(3, false, 2000);
        CHECK(r.key(local(3), true, true, true, 2000).status == FOXAPI_DENIED);
        // Session 2's LATCH 0 exempts session 2 only.
        CHECK(r.key(local(2), true, false, true, 2000).status == FOXAPI_OK);
        CHECK(!r.releaseFirst("local", 2) && r.releaseFirst("local", 1));
        CHECK(r.key(local(1), true, true, true, 2001).status == FOXAPI_DENIED);
        CHECK(r.key(local(2), true, true, true, 2001).status == FOXAPI_OK);
        // Session 2 now holds a latch; session 1 (stuck on LATCH 1) presses
        // RELEASE-less nothing. Session 3's LATCH 0 ends session 2's latch:
        // a marking end - it re-marks, withdraws 2's exemption, exempts 3.
        w.beat(2, 2100);
        CHECK(r.key(local(3), true, false, true, 2100).status == FOXAPI_OK);
        CHECK(!r.keyed());
        CHECK(has(r.stateText(), "released from another interface"));
        CHECK(r.releaseFirst("local", 2));
        CHECK(!r.releaseFirst("local", 3));
        // The owner's OWN LATCH 0 is not a marking end: 3 latches (after the
        // re-arm time) and releases itself; nobody is re-marked.
        CHECK(r.key(local(3), true, true, true, 3200).status == FOXAPI_OK);
        CHECK(r.key(local(3), true, false, true, 3300).status == FOXAPI_OK);
        CHECK(!r.releaseFirst("local", 3));   // still exempt: its own release marked nothing
        CHECK(r.releaseFirst("local", 1));    // 1 never released
        // A GONE session's LATCH 0 still releases, and exempts nobody.
        CHECK(r.key(local(3), true, true, true, 4400).status == FOXAPI_OK);
        TxKeySender goneTwo = local(2);
        goneTwo.open = false;
        CHECK(r.key(goneTwo, true, false, true, 4500).status == FOXAPI_OK);
        CHECK(!r.keyed());
        CHECK(r.releaseFirst("local", 2));
        // LATCH 0 is idempotent: once or six times, the same state.
        r.key(local(1), true, false, true, 4600);
        const std::string once = r.stateText();
        for (int i = 0; i < 5; ++i) { r.key(local(1), true, false, true, 4700); }
        CHECK(r.stateText() == once);
        CHECK(!r.releaseFirst("local", 1));
        std::printf("R3 marks: timeout marked the operator; each session released for itself; another "
                    "window's LATCH 0 re-marked; the owner's own did not; a gone session exempted nobody\n");
    }

    // --- R4: the re-arm time --------------------------------------------------------------
    {
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        CHECK(r.key(local(1), true, true, true, 0).status == FOXAPI_OK);
        CHECK(r.key(local(1), true, false, true, 100).status == FOXAPI_OK);   // own release: no mark
        const TxKeyAnswer early = r.key(local(1), true, true, true, 100);
        CHECK(early.status == FOXAPI_DENIED);
        CHECK(early.applied0 == static_cast<double>(FOXAPI_LATCH_REARM_MS));
        CHECK(has(early.message, "released by its owner"));
        CHECK(r.key(local(1), true, true, true, 100 + FOXAPI_LATCH_REARM_MS - 1).status == FOXAPI_DENIED);
        CHECK(r.key(local(1), true, true, true, 100 + FOXAPI_LATCH_REARM_MS).status == FOXAPI_OK);
        // A toggle every "frame" cannot hold it: LATCH 0 + LATCH 1 every 16 ms
        // for 3 s is keyed only while the re-arm allows.
        int keyedFrames = 0, frames = 0;
        for (std::int64_t t = 1200; t < 4200; t += 16) {
            r.key(local(1), true, false, true, t);
            r.key(local(1), true, true, true, t);
            ++frames;
            keyedFrames += r.keyed() ? 1 : 0;
        }
        std::printf("R4 re-arm: refused %d ms after its own release; toggling every 16 ms for 3 s keyed %d of %d "
                    "frames\n",
                    FOXAPI_LATCH_REARM_MS - 1, keyedFrames, frames);
        CHECK(keyedFrames * 10 < frames);   // one frame in each re-arm period at most
    }

    // --- R5: remote PTT needs local consent --------------------------------------------------
    {
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        w.open(9, true, 0);
        const TxKeyAnswer refused = r.key(remote(9), false, true, true, 0);
        CHECK(refused.status == FOXAPI_DENIED);
        CHECK(has(refused.message, "consent"));
        CHECK(r.arm(local(1), true, 0).applied0 == 1.0);
        CHECK(r.remoteArmedBy() == 1u);
        CHECK(r.key(remote(9), false, true, true, 10).status == FOXAPI_OK);
        CHECK(r.keyed() && r.ownerRemote());
        // Withdrawn: the remote key opens at once.
        r.arm(local(1), false, 20);
        CHECK(!r.keyed());
        CHECK(has(r.unkeyReason(), "withdrew consent"));
        // The consenting session closing...
        r.arm(local(1), true, 30);
        CHECK(r.key(remote(9), false, true, true, 30).status == FOXAPI_OK);
        r.sessionGone(1, false, 40);
        w.close(1);
        CHECK(!r.keyed() && r.remoteArmedBy() == 0u);
        CHECK(has(r.unkeyReason(), "closed"));
        // ...or going quiet (its keep-alive, 150 ms here).
        w.open(2, false, 50);
        r.arm(local(2), true, 50);
        CHECK(r.key(remote(9), false, true, true, 50).status == FOXAPI_OK);
        w.beat(9, 150);
        r.tick(150, w);
        CHECK(r.keyed());
        w.beat(9, 201);
        r.tick(201, w);   // session 2's last beat 151 ms ago
        CHECK(!r.keyed() && r.remoteArmedBy() == 0u);
        CHECK(has(r.unkeyReason(), "stopped answering"));
        std::printf("R5 consent: refused without it; opened on disarm, on the consenting session's close and "
                    "when it went quiet\n");
    }

    // --- R6: one key holder, anyone may open it ---------------------------------------------------
    {
        TxKeyRules r(fast());
        CHECK(r.key(local(1), false, true, true, 0).status == FOXAPI_OK);
        const TxKeyAnswer busy = r.key(local(2), false, true, true, 1);
        CHECK(busy.status == FOXAPI_BUSY);
        CHECK(r.keyOwner() == 1u);
        CHECK(r.key(local(2), false, false, true, 2).status == FOXAPI_OK);
        CHECK(!r.keyed());
        // No transmitter: a key-down is NO_DEVICE and changes nothing.
        CHECK(r.key(local(1), false, true, false, 3).status == FOXAPI_NO_DEVICE);
        CHECK(!r.keyed());
        std::printf("R6 one holder: a second session BUSY; its PTT 0 opened the first's key\n");
    }

    // --- R7: keep-alive, and closing --------------------------------------------------------------------
    {
        TxKeyRules r(fast());
        World w;
        w.open(1, false, 0);
        CHECK(r.key(local(1), true, true, true, 0).status == FOXAPI_OK);   // a latch, so no hold interferes
        w.beat(1, 100);
        r.tick(250, w);   // 150 ms since the beat: at the limit, still keyed
        CHECK(r.keyed());
        r.tick(251, w);
        CHECK(!r.keyed());
        CHECK(has(r.unkeyReason(), "stopped answering"));
        CHECK(r.marked("local"));   // a lost keep-alive is a marking end
        // Remote: 300 ms here (2 s standard).
        TxKeyRules q(fast());
        World v;
        v.open(1, false, 0);
        v.open(9, true, 0);
        q.arm(local(1), true, 0);
        CHECK(q.key(remote(9), false, true, true, 0).status == FOXAPI_OK);
        for (std::int64_t t = 0; t <= 400; t += 20) {
            v.beat(1, t);
            if (t % 180 == 0) { q.key(remote(9), false, true, true, t); }   // re-asserted, never beating
            q.tick(t, v);
        }
        CHECK(!q.keyed());
        CHECK(has(q.unkeyReason(), "stopped answering"));
        // Closing the holder's session opens it at the start of the next pass.
        TxKeyRules c(fast());
        CHECK(c.key(local(5), false, true, true, 0).status == FOXAPI_OK);
        c.sessionGone(5, false, 1);
        CHECK(!c.keyed());
        CHECK(has(c.unkeyReason(), "closed its session"));
        TxKeyRules d(fast());
        CHECK(d.key(local(5), false, true, true, 0).status == FOXAPI_OK);
        d.sessionGone(5, true, 1);
        CHECK(has(d.unkeyReason(), "revoked"));
        std::printf("R7 keep-alive: local key lost 151 ms after the last beat; a remote one re-asserting without "
                    "beating lost it; closing and detaching opened it\n");
    }

    // --- R8: the engine's own ends -----------------------------------------------------------------------
    {
        for (const char* why : {"the receiver was stopped", "a fault stopped the engine",
                                "the transmitter was changed", "the transmitter was closed"}) {
            TxKeyRules r(fast());
            CHECK(r.key(local(1), true, true, true, 0).status == FOXAPI_OK);
            r.endKey(why, 10);
            CHECK(!r.keyed() && !r.latched());
            CHECK(r.unkeyReason() == why);
            CHECK(r.marked("local"));
            CHECK(sawEvent(r, why));
            const TxKeyAnswer a = r.key(local(1), true, true, true, 2000);
            CHECK(a.status == FOXAPI_DENIED && has(a.message, why));
        }
        TxKeyRules idle(fast());
        idle.endKey("the receiver was stopped", 0);   // nobody keyed: nothing happens
        CHECK(idle.unkeyReason().empty());
        CHECK(!idle.marked("local"));
        std::printf("R8 engine ends: stop, fault, transmitter change and close each opened a latch and marked it\n");
    }

    // --- T: the timers can only be shortened -----------------------------------------------------------------
    {
        TxKeyTimers longer;
        longer.pttHoldMs = 999999;
        longer.latchTimeoutMs = 99999999;
        longer.keepaliveMs = 999999;
        longer.remoteKeepaliveMs = 999999;
        longer.rearmMs = 1;
        const TxKeyTimers t = longer.shortened();
        CHECK(t.pttHoldMs == FOXAPI_PTT_HOLD_MS);
        CHECK(t.latchTimeoutMs == FOXAPI_LATCH_TIMEOUT_MS);
        CHECK(t.keepaliveMs == FOXAPI_KEEPALIVE_MS);
        CHECK(t.remoteKeepaliveMs == FOXAPI_REMOTE_KEEPALIVE_MS);
        CHECK(t.rearmMs == 10);
        TxKeyRules r(longer);
        CHECK(r.timers().pttHoldMs == FOXAPI_PTT_HOLD_MS);
    }

    return testSummary("test_tx_key_rules");
}
