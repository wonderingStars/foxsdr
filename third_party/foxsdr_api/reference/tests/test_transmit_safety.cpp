// test_transmit_safety.cpp - the key-safety rules docs/API.md puts on the
// ENGINE, so that no interface can forget them: the PTT is a hold that must be
// re-asserted, the latch times out and is refused to remote sessions, and a
// session that stops answering or closes loses the key.
//
// The engine is started with SHORTENED timers (the test options may only ever
// shorten them - one check below proves a longer value is clamped down).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "mock_engine/mock_engine.hpp"

using testing::cmd;
using testing::kAllGrants;

namespace {

const FoxEngineApi* api() { return foxsdr::mock::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr const char* kFast = "pttHoldMs=200;latchTimeoutMs=400;keepaliveMs=150;token=t";

bool keyed(testing::Session& s) { return (s.state().flags & FOXAPI_RX_TX_KEYED) != 0u; }

void openTx(testing::Session& s) {
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy")).result.status, FOXAPI_OK);
}

// Keeps a session's heartbeat alive for `ms`, as an interface's frame loop does.
void beatFor(testing::Session& s, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        s.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool sawUnkey(testing::Session& s, const char* reasonPart) {
    for (const FoxEvent& e : s.events()) {
        if (e.kind == FOXAPI_EVENT_TX_UNKEYED && std::strstr(e.text, reasonPart) != nullptr) {
            return true;
        }
    }
    return false;
}

void testNoTransmitterNoKey() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    // Judged at SUBMIT against the published state (and answered as a
    // refused result), so a key request can never wait in the queue for a
    // radio to appear underneath it.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_NO_DEVICE);
    // Without the TRANSMIT grant nothing about the transmitter moves.
    testing::Session rx(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants & ~FOXAPI_GRANT_TRANSMIT);
    CHECK_EQ(rx.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy")).status(), FOXAPI_DENIED);
    CHECK(!keyed(s));
}

void testPttIsAHold() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    api()->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).result.status, FOXAPI_OK);
    const FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_KEYED) != 0u);
    CHECK((st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u);
    CHECK(st.txHoldRemainingMs > 0 && st.txHoldRemainingMs <= 200);
    // Heartbeats alone do not keep a PTT down: it must be RE-ASSERTED.
    beatFor(s, 450);
    CHECK(!keyed(s));
    CHECK(sawUnkey(s, "not re-asserted"));
    CHECK(std::strstr(s.state().txUnkeyReason, "not re-asserted") != nullptr);

    // Re-asserting inside the hold keeps it keyed for as long as it goes on.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).result.status, FOXAPI_OK);
    bool stayed = true;
    for (int i = 0; i < 12; ++i) {  // ~720 ms, three and a half holds
        s.beat();
        s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        stayed = stayed && keyed(s);
    }
    CHECK(stayed);
    // Releasing opens it at once, and it is the operator's doing: no reason.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 0)).result.status, FOXAPI_OK);
    CHECK(!keyed(s));
    CHECK_EQ(std::string(s.state().txUnkeyReason), std::string(""));
}

void testLatchRules() {
    testing::Engine e(api(), kFast);
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    CHECK(remote.ok());
    openTx(local);
    // The far end of a network can never latch.
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    // ...but once the local operator consents it can use the hold, like the
    // web remote's PTT.
    local.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(remote));
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 0)).status(), FOXAPI_OK);

    // A local latch holds with no re-assertion, then opens itself.
    api()->subscribe(local.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    local.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    beatFor(local, 250);
    CHECK(keyed(local));
    CHECK((local.state().flags & FOXAPI_RX_TX_LATCHED) != 0u);
    beatFor(local, 350);  // past the 400 ms latch timeout
    CHECK(!keyed(local));
    CHECK(sawUnkey(local, "latch timed out"));
}

// THE LATCH CANNOT BE HELD FOR EVER BY RE-LATCHING. The review kept a
// 400 ms latch down for 2581 ms by re-sending LATCH 1 every 300 ms: each one
// re-stamped the deadline. The app's Transmitter::setLatched(true) is a
// no-op while latched, so its 60 s deadline stands. Here a re-latch while
// latched is answered FOXAPI_NO_CHANGE and the ORIGINAL deadline holds; and
// once the latch has timed out, LATCH 1 from the same interface is DENIED
// until it has released the latch (LATCH 0), so an interface that re-sends
// LATCH 1 every frame gets one latch, not one after another.
void testRelatchDoesNotExtend() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    api()->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    openTx(s);
    s.beat();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    int relatches = 0;
    int noChange = 0;
    int denied = 0;
    int other = 0;
    long long unkeyedAt = -1;
    bool rekeyed = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1300)) {
        s.beat();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const bool k = keyed(s);
        if (!k && unkeyedAt < 0) {
            unkeyedAt = ms;
        }
        rekeyed = rekeyed || (k && unkeyedAt >= 0);
        if (ms / 80 > relatches) {  // LATCH 1 again every 80 ms, before and after the timeout
            ++relatches;
            const int32_t st = s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
            noChange += st == FOXAPI_NO_CHANGE ? 1 : 0;
            denied += st == FOXAPI_DENIED ? 1 : 0;
            other += (st != FOXAPI_NO_CHANGE && st != FOXAPI_DENIED) ? 1 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("re-latch every 80 ms against a 400 ms latch for 1300 ms: %d re-latches "
                "(%d NO_CHANGE, %d DENIED, %d other), unkeyed at %lld ms, re-keyed: %s\n",
                relatches, noChange, denied, other, unkeyedAt, rekeyed ? "YES" : "no");
    CHECK(unkeyedAt >= 0);
    CHECK(unkeyedAt < 600);  // 400 ms and the control loop's slack, not 1300
    CHECK(!rekeyed);         // no re-latch after the timeout took hold
    CHECK(noChange >= 3);    // the ones while it was latched changed nothing
    CHECK(denied >= 5);      // the ones after it timed out were refused
    CHECK_EQ(other, 0);
    CHECK(sawUnkey(s, "latch timed out"));
    // Releasing the timed-out latch and latching again (once the re-arm time
    // has passed) is a NEW latch with a new deadline: the operator's
    // deliberate act, not a re-assertion.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    beatFor(s, FOXAPI_LATCH_REARM_MS + 100);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(s));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
}

// A frame's batch fired without waiting, its results drained and ignored:
// what a level-triggered interface does every frame.
void fire(testing::Session& s, std::initializer_list<FoxCommand> cs) {
    std::vector<FoxCommand> v(cs);
    api()->submit(s.raw(), v.data(), static_cast<uint32_t>(v.size()), nullptr);
    FoxCommandResult rr[64];
    for (auto& x : rr) x.structSize = sizeof(x);
    while (api()->poll_results(s.raw(), rr, 64) == 64) {
    }
}

// EVERY LATCH END MARKS ITS OWNER. Round-3 review: only a TIMEOUT marked the
// owning session, so a level-triggered interface (LATCH 1 every frame) whose
// latch was opened by a stop, a fault, a transmitter change or a lost
// keep-alive latched AGAIN the moment the re-arm time ran out - 1001 ms after
// the operator pressed STOP, with the receiver stopped; and a 300 ms stall
// every 2.5 s gave a chain of latches keyed 68% of the time. Now a latch that
// ends by ANY means other than its owner's own LATCH 0 marks the owner, and
// the owner's LATCH 1 is refused (DENIED, naming why it ended) until that
// owner sends LATCH 0. Each case below runs its own engine, in parallel, with
// the interface re-sending LATCH 1 every frame for 2.5 s after the end - well
// past the re-arm time - and the key must never close again.
enum EndHow {
    kEndTimeout, kEndStop, kEndRemoteStop, kEndFault, kEndTxChange, kEndTxCloseOpen,
    kEndKeepAlive, kEndOtherRelease, kEndCount
};

struct EndCase {
    EndHow how;
    const char* name;
    const char* reason;  // must appear in the refusal's message
};

const EndCase kEndCases[kEndCount] = {
    {kEndTimeout, "the latch timeout", "timed out"},
    {kEndStop, "STOP (RUN 0)", "stopped"},
    {kEndRemoteStop, "RUN 0 from a REMOTE session", "stopped"},
    {kEndFault, "a fault", "fault"},
    {kEndTxChange, "a transmitter change (TX_OPEN)", "transmitter was changed"},
    {kEndTxCloseOpen, "TX_CLOSE then TX_OPEN", "transmitter was closed"},
    {kEndKeepAlive, "a lost keep-alive (300 ms stall)", "stopped answering"},
    // Round 4: every LOCAL session is the same operator (the principal). A
    // LATCH 0 from one that does NOT own the active latch - the operator
    // pressing RELEASE in another window because this one is stuck - MARKS
    // the operator: the stuck window must not latch again after the re-arm
    // time. When in doubt the transmitter stays unkeyed.
    {kEndOtherRelease, "another LOCAL session's LATCH 0 (released from another window)", "released from another interface"},
};

struct EndOutcome {
    bool keyedBeforeEnd = false;  // control: the probe did key the transmitter
    long long endedAt = -1;       // when the key was first seen open
    int reclosed = 0;             // times it closed again after that
    long long firstRecloseAt = -1;
    int32_t refusal = 0;          // a LATCH 1 at the end of the window
    uint32_t refusalFlags = 0;
    std::string refusalMessage;
    int32_t release = 0;          // the owner's own LATCH 0
    int32_t afterRelease = 0;     // LATCH 1 once released and re-armed
    bool keyedAfterRelease = false;
};

EndOutcome levelTriggeredAcross(EndHow how) {
    const std::string opts = std::string("pttHoldMs=200;keepaliveMs=150;token=t;latchTimeoutMs=") +
                             (how == kEndTimeout ? "400" : "10000");
    testing::Engine e(api(), opts.c_str());
    testing::Session ui(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session op(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                            FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "t");
    op.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    EndOutcome o;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    bool acted = false;
    bool was = false;
    while (ms() < 4000 && (o.endedAt < 0 || ms() < o.endedAt + 2500)) {
        if (!acted && ms() >= 300 && how != kEndTimeout) {
            acted = true;
            switch (how) {
            case kEndStop: op.run(cmd(FOXAPI_OP_RUN, 0, 0)); break;
            case kEndRemoteStop: remote.beat(); remote.run(cmd(FOXAPI_OP_RUN, 0, 0)); break;
            case kEndFault: op.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.fault=usb gone")); break;
            case kEndTxChange: op.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy")); break;
            case kEndTxCloseOpen:
                op.run(cmd(FOXAPI_OP_TX_CLOSE));
                op.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
                break;
            case kEndKeepAlive:
                std::this_thread::sleep_for(std::chrono::milliseconds(300));  // hung: no beat, no submit
                break;
            case kEndOtherRelease: op.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)); break;
            default: break;
            }
        }
        ui.beat();
        op.beat();
        remote.beat();
        fire(ui, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
        const bool k = keyed(op);
        if (k && o.endedAt < 0) {
            o.keyedBeforeEnd = true;
        }
        if (!k && was && o.endedAt < 0) {
            o.endedAt = ms();
        }
        if (k && !was && o.endedAt >= 0) {
            ++o.reclosed;
            if (o.firstRecloseAt < 0) {
                o.firstRecloseAt = ms();
            }
        }
        was = k;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ui.beat();
    const auto refused = ui.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    o.refusal = refused.status();
    o.refusalFlags = refused.result.flags;
    o.refusalMessage = refused.result.message;
    // Only the owner's own LATCH 0 clears the mark; then (after the re-arm
    // time) a new latch is the operator's deliberate act and is allowed.
    o.release = ui.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status();
    const auto rearm = std::chrono::steady_clock::now() + std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 100);
    while (std::chrono::steady_clock::now() < rearm) {
        ui.beat();
        op.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (how == kEndStop || how == kEndRemoteStop || how == kEndFault) {
        op.run(cmd(FOXAPI_OP_RUN, 0, 1));
    }
    ui.beat();
    o.afterRelease = ui.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    o.keyedAfterRelease = keyed(ui);
    ui.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    return o;
}

void testEveryLatchEndMarksItsOwner() {
    std::vector<EndOutcome> out(kEndCount);
    std::vector<std::thread> threads;
    for (int i = 0; i < kEndCount; ++i) {
        threads.emplace_back([&out, i] { out[i] = levelTriggeredAcross(kEndCases[i].how); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int i = 0; i < kEndCount; ++i) {
        const EndOutcome& o = out[i];
        std::printf("latch ended by %s at %lld ms: closed again %d time(s) (first at %lld ms); "
                    "LATCH 1 then -> %d \"%s\"; own LATCH 0 -> %d; LATCH 1 after -> %d keyed=%d\n",
                    kEndCases[i].name, o.endedAt, o.reclosed, o.firstRecloseAt, o.refusal,
                    o.refusalMessage.c_str(), o.release, o.afterRelease, o.keyedAfterRelease ? 1 : 0);
        CHECK(o.keyedBeforeEnd);   // the probe did key it
        CHECK(o.endedAt >= 0);     // and the end did open it
        CHECK_EQ(o.reclosed, 0);   // THE RULE: never again until LATCH 0
        CHECK_EQ(o.refusal, FOXAPI_DENIED);
        CHECK((o.refusalFlags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(o.refusalMessage.find(kEndCases[i].reason) != std::string::npos);
        CHECK(o.refusalMessage.find("LATCH 0") != std::string::npos);
        CHECK_EQ(o.release, FOXAPI_OK);
        CHECK_EQ(o.afterRelease, FOXAPI_OK);
        CHECK(o.keyedAfterRelease);
    }
}

// The review's chain probe, as it ran: 12 s of LATCH 1 every frame with a
// 300 ms stall every 2.5 s against a 3 s latch timeout. One latch, not a chain.
void testStallsDoNotChainLatches() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=3000;keepaliveMs=150;token=t");
    testing::Session ui(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    openTx(ui);
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    int closes = 0, frames = 0, keyedFrames = 0;
    bool was = false;
    long long nextStall = 2500;
    while (ms() < 12000) {
        if (ms() >= nextStall) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            nextStall += 2500;
            continue;
        }
        ui.beat();
        obs.beat();
        fire(ui, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
        const bool k = keyed(obs);
        closes += (k && !was) ? 1 : 0;
        was = k;
        ++frames;
        keyedFrames += k ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("12 s of LATCH 1 every frame, 300 ms stall every 2.5 s: closed %d time(s), keyed %d of %d "
                "frames\n", closes, keyedFrames, frames);
    CHECK_EQ(closes, 1);
    CHECK(keyedFrames * 100 < frames * 35);  // the first 2.5 s at most (the review's chain: 68%)
}

// Every result a session has waiting, read until none arrives for `quietMs`.
std::vector<FoxCommandResult> drainAll(testing::Session& s, int quietMs = 300) {
    std::vector<FoxCommandResult> all;
    auto last = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - last < std::chrono::milliseconds(quietMs)) {
        FoxCommandResult r[64];
        for (auto& x : r) x.structSize = sizeof(x);
        const int32_t n = api()->poll_results(s.raw(), r, 64);
        if (n > 0) {
            all.insert(all.end(), r, r + n);
            last = std::chrono::steady_clock::now();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return all;
}

// Holds the control thread for `ms` (mock.stall) and returns once it is
// inside the stall, so what is submitted next waits in the queue.
void stall(testing::Session& admin, int ms) {
    const std::string t = "mock.stall=" + std::to_string(ms);
    fire(admin, {cmd(FOXAPI_OP_SETTING_SET, 0, 0, t.c_str())});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

FoxSubmitResult submitOne(testing::Session& s, const FoxCommand& c) {
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    api()->submit(s.raw(), &c, 1, &r);
    return r;
}

// COMMANDS THAT CAN ONLY MAKE THE TRANSMITTER SAFER ARE NEVER BUSY, AND NEVER
// PILE UP. Round-3 review: with 256 results unread, LATCH 0 and RUN 0 were
// answered BUSY and the key stayed closed; round 3 gave them 16 reserve
// places. Round-4 review: the reserve was shared by all five, so a frame
// loop sending PTT 0 every frame (key up) used it in 16 frames and then the
// operator's LATCH 0 and RUN 0 were BUSY again, latch held. Now each safer
// KIND has at most one command outstanding per session: a newer one of the
// same kind is MERGED with it (answered FOXAPI_NO_CHANGE and the ticket whose
// one result answers both), so they are never BUSY and a session holds at
// most FOXAPI_MAX_PENDING + FOXAPI_SAFETY_RESERVE (one per safer kind)
// unread results, however long it goes on.
void testSaferCommandsAreNeverBusy() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=150;token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(obs));
    std::vector<FoxCommand> fill(FOXAPI_MAX_PENDING, cmd(FOXAPI_OP_SET_VOLUME, 0.3));
    CHECK_EQ(api()->submit(s.raw(), fill.data(), static_cast<uint32_t>(fill.size()), nullptr),
             static_cast<int32_t>(FOXAPI_MAX_PENDING));
    CHECK(check::waitFor([&] { s.beat(); return obs.state().volume == 0.3; }, 2000));
    // THE REVIEW'S FRAME LOOP: stopped reading, PTT 0 every frame.
    int busy = 0, taken = 0, merged = 0;
    uint64_t pttTicket = 0;
    bool oneTicket = true;
    for (int frame = 0; frame < 40; ++frame) {
        s.beat();
        const FoxSubmitResult r = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        busy += r.status == FOXAPI_BUSY ? 1 : 0;
        taken += r.status == FOXAPI_OK ? 1 : 0;
        merged += r.status == FOXAPI_NO_CHANGE ? 1 : 0;
        if (r.status == FOXAPI_OK && pttTicket == 0) {
            pttTicket = r.ticket;
        }
        oneTicket = oneTicket && r.ticket == pttTicket && r.ticket != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("256 unread + PTT 0 every frame for 40 frames: %d new ticket(s), %d merged, %d BUSY\n", taken,
                merged, busy);
    CHECK_EQ(busy, 0);
    CHECK_EQ(taken, 1);
    CHECK_EQ(merged, 39);
    CHECK(oneTicket);
    // Now the operator presses RELEASE and STOP (and closes the transmitter
    // and withdraws consent): all taken, and the key opens. The ordinary
    // command beside them is still BUSY.
    FoxCommand rel[5] = {cmd(FOXAPI_OP_SET_VOLUME, 0.4), cmd(FOXAPI_OP_TX_LATCH, 0, 0), cmd(FOXAPI_OP_RUN, 0, 0),
                         cmd(FOXAPI_OP_TX_CLOSE), cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0)};
    FoxSubmitResult sr[5];
    for (auto& x : sr) x.structSize = sizeof(x);
    CHECK_EQ(api()->submit(s.raw(), rel, 5, sr), 4);
    CHECK_EQ(sr[0].status, FOXAPI_BUSY);
    CHECK_EQ(sr[1].status, FOXAPI_OK);
    CHECK_EQ(sr[2].status, FOXAPI_OK);
    CHECK_EQ(sr[3].status, FOXAPI_OK);
    CHECK_EQ(sr[4].status, FOXAPI_OK);
    CHECK(check::waitFor([&] { s.beat(); return !keyed(obs); }, 1000));
    CHECK((obs.state().flags & FOXAPI_RX_RUNNING) == 0u);
    CHECK((obs.state().flags & FOXAPI_RX_TX_AVAILABLE) == 0u);
    // 500 more, every safer kind 100 times: all merged, none BUSY, nothing new.
    std::vector<FoxCommand> burst;
    for (int i = 0; i < 100; ++i) {
        burst.push_back(cmd(FOXAPI_OP_TX_PTT, 0, 0));
        burst.push_back(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        burst.push_back(cmd(FOXAPI_OP_RUN, 0, 0));
        burst.push_back(cmd(FOXAPI_OP_TX_CLOSE));
        burst.push_back(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0));
    }
    std::vector<FoxSubmitResult> br(burst.size());
    for (auto& x : br) x.structSize = sizeof(x);
    CHECK_EQ(api()->submit(s.raw(), burst.data(), static_cast<uint32_t>(burst.size()), br.data()), 0);
    int burstMerged = 0;
    for (const auto& x : br) {
        burstMerged += x.status == FOXAPI_NO_CHANGE ? 1 : 0;
    }
    CHECK_EQ(burstMerged, 500);
    // BOUNDED: everything waiting is the 256 and ONE per safer kind, each
    // ticket exactly once - and the PTT 0 ticket is among them.
    const std::vector<FoxCommandResult> all = drainAll(s);
    std::vector<uint64_t> tickets;
    bool sawPtt = false;
    for (const auto& r : all) {
        tickets.push_back(r.ticket);
        sawPtt = sawPtt || (r.ticket == pttTicket && r.op == FOXAPI_OP_TX_PTT);
    }
    std::sort(tickets.begin(), tickets.end());
    const bool unique = std::adjacent_find(tickets.begin(), tickets.end()) == tickets.end();
    std::printf("waiting results after 540 safer commands behind 256 unread: %zu\n", all.size());
    CHECK_EQ(all.size(), static_cast<std::size_t>(FOXAPI_MAX_PENDING + FOXAPI_SAFETY_RESERVE));
    CHECK(unique);
    CHECK(sawPtt);
}

// THE QUEUE IS BOUNDED TOO, even while the control thread is held and
// nothing is applied: 10,000 safer commands from one session leave at most
// one waiting per kind.
void testSaferCommandsDoNotPileUpInTheQueue() {
    testing::Engine e(api(), "token=t");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    stall(admin, 600);
    int busy = 0;
    const uint32_t ops[5] = {FOXAPI_OP_TX_PTT, FOXAPI_OP_TX_LATCH, FOXAPI_OP_RUN, FOXAPI_OP_TX_CLOSE,
                             FOXAPI_OP_TX_REMOTE_ARM};
    for (int batch = 0; batch < 10; ++batch) {
        std::vector<FoxCommand> v;
        for (int i = 0; i < 1000; ++i) {
            v.push_back(cmd(ops[i % 5], 0, 0));
        }
        std::vector<FoxSubmitResult> r(v.size());
        for (auto& x : r) x.structSize = sizeof(x);
        api()->submit(s.raw(), v.data(), static_cast<uint32_t>(v.size()), r.data());
        for (const auto& x : r) {
            busy += x.status == FOXAPI_BUSY ? 1 : 0;
        }
    }
    char depth[32] = {};
    CHECK(api()->get_setting(s.raw(), "mock.queueDepth", depth, sizeof(depth)) > 0);
    std::printf("10,000 safer commands during a stall: %d BUSY, %s waiting in the queue\n", busy, depth);
    CHECK_EQ(busy, 0);
    CHECK(std::atoi(depth) <= 5);
    const std::vector<FoxCommandResult> all = drainAll(s, 800);
    CHECK_EQ(all.size(), static_cast<std::size_t>(5));
}

// MERGING NEVER REORDERS AGAINST WHAT COULD UNDO IT. A merged safer command
// is in force from the FIRST time it was sent: while the control thread was
// held, the local operator withdrew consent (TX_REMOTE_ARM 0), a remote PTT
// arrived, and the frame loop sent TX_REMOTE_ARM 0 again. The two withdrawals
// are one command, but the remote PTT queued between them is judged with the
// consent already withdrawn - it is refused and nothing keys. And a session's
// own order is kept exactly: TX_CLOSE, PTT 1, TX_CLOSE never keys.
void testMergingKeepsSafetyInOrder() {
    {
        testing::Engine e(api(), "token=t;pttHoldMs=2000;keepaliveMs=1000;remoteKeepaliveMs=2000");
        testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
        openTx(local);
        local.beat();
        remote.beat();
        CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
        api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
        stall(admin, 400);
        local.beat();
        remote.beat();
        const FoxSubmitResult w1 = submitOne(local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0));
        const FoxSubmitResult ptt = submitOne(remote, cmd(FOXAPI_OP_TX_PTT, 0, 1));
        const FoxSubmitResult w2 = submitOne(local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0));
        CHECK_EQ(w1.status, FOXAPI_OK);
        CHECK_EQ(ptt.status, FOXAPI_OK);
        CHECK_EQ(w2.status, FOXAPI_NO_CHANGE);  // merged...
        CHECK_EQ(w2.ticket, w1.ticket);         // ...into the first one's ticket
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        local.beat();
        int32_t pttStatus = 999;
        for (const auto& r : drainAll(remote)) {
            if (r.ticket == ptt.ticket) pttStatus = r.status;
        }
        int w1Results = 0;
        for (const auto& r : drainAll(local)) {
            w1Results += r.ticket == w1.ticket ? 1 : 0;
        }
        std::printf("consent withdrawn twice around a queued remote PTT: PTT -> %d\n", pttStatus);
        CHECK_EQ(pttStatus, FOXAPI_DENIED);
        CHECK_EQ(w1Results, 1);  // one result for the two merged submits
        CHECK(!sawUnkey(watcher, ""));  // nothing was ever keyed
        CHECK((watcher.state().flags & FOXAPI_RX_TX_KEYED) == 0u);
    }
    {
        testing::Engine e(api(), "token=t;pttHoldMs=2000;keepaliveMs=1000");
        testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        openTx(s);
        api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
        stall(admin, 400);
        s.beat();
        FoxCommand q[3] = {cmd(FOXAPI_OP_TX_CLOSE), cmd(FOXAPI_OP_TX_PTT, 0, 1), cmd(FOXAPI_OP_TX_CLOSE)};
        FoxSubmitResult r[3];
        for (auto& x : r) x.structSize = sizeof(x);
        CHECK_EQ(api()->submit(s.raw(), q, 3, r), 2);
        CHECK_EQ(r[2].status, FOXAPI_NO_CHANGE);
        CHECK_EQ(r[2].ticket, r[0].ticket);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        s.beat();
        int32_t pttStatus = 999;
        for (const auto& x : drainAll(s)) {
            if (x.ticket == r[1].ticket) pttStatus = x.status;
        }
        std::printf("TX_CLOSE, PTT 1, TX_CLOSE queued together: PTT -> %d\n", pttStatus);
        CHECK_EQ(pttStatus, FOXAPI_NO_DEVICE);
        CHECK(!sawUnkey(watcher, ""));
        CHECK((watcher.state().flags & FOXAPI_RX_TX_AVAILABLE) == 0u);
    }
    {   // ...and what the session itself sent in between stands: it withdrew
        // consent, GAVE IT AGAIN, then withdrew it. The remote PTT queued
        // after the re-arm is accepted (exactly as sent), and the second
        // withdrawal opens it - merging did not override the session's own
        // later wish.
        testing::Engine e(api(), "token=t;pttHoldMs=2000;keepaliveMs=1000;remoteKeepaliveMs=2000");
        testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
        openTx(local);
        local.beat();
        remote.beat();
        CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
        api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
        stall(admin, 400);
        local.beat();
        remote.beat();
        submitOne(local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0));
        submitOne(local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1));
        const FoxSubmitResult ptt = submitOne(remote, cmd(FOXAPI_OP_TX_PTT, 0, 1));
        submitOne(local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        local.beat();
        int32_t pttStatus = 999;
        for (const auto& r : drainAll(remote)) {
            if (r.ticket == ptt.ticket) pttStatus = r.status;
        }
        std::printf("withdraw, re-arm, remote PTT, withdraw: PTT -> %d\n", pttStatus);
        CHECK_EQ(pttStatus, FOXAPI_OK);
        CHECK(sawUnkey(watcher, "withdrew consent"));  // the second withdrawal opened it
        CHECK((watcher.state().flags & FOXAPI_RX_TX_KEYED) == 0u);
    }
    {   // A merged ticket is answered ONCE, even when its result is read
        // while a merged application is still queued.
        testing::Engine e(api(), "token=t");
        testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        const FoxSubmitResult first = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));  // applied; its result waits unread
        stall(admin, 400);
        const FoxSubmitResult again = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        CHECK_EQ(first.status, FOXAPI_OK);
        CHECK_EQ(again.status, FOXAPI_NO_CHANGE);
        CHECK_EQ(again.ticket, first.ticket);
        int early = 0;
        for (const auto& r : drainAll(s, 50)) {
            early += r.ticket == first.ticket ? 1 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the merged copy is applied now
        int late = 0;
        for (const auto& r : drainAll(s)) {
            late += r.ticket == first.ticket ? 1 : 0;
        }
        std::printf("a merged ticket read while its copy was queued: answered %d then %d time(s)\n", early, late);
        CHECK_EQ(early, 1);
        CHECK_EQ(late, 0);  // not answered twice
        const FoxSubmitResult next = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        CHECK_EQ(next.status, FOXAPI_OK);  // the kind is free again: a new ticket
        CHECK(next.ticket > first.ticket);
    }
    {   // NEVER MERGED INTO A TICKET WHOSE RESULT HAS BEEN READ (round-5
        // mutant, merge_into_read). The result of `first` is read while a
        // merged copy of it is still queued; a THIRD send of the kind, still
        // inside the stall, must get a NEW ticket (and that ticket its own
        // result), never NO_CHANGE naming the ticket already answered - its
        // answer would never arrive.
        testing::Engine e(api(), "token=t");
        testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        const FoxSubmitResult first = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        stall(admin, 500);
        const FoxSubmitResult again = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));
        int early = 0;
        for (const auto& r : drainAll(s, 50)) {
            early += r.ticket == first.ticket ? 1 : 0;
        }
        const FoxSubmitResult third = submitOne(s, cmd(FOXAPI_OP_TX_PTT, 0, 0));  // still held
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        int lateFirst = 0;
        int thirdResults = 0;
        for (const auto& r : drainAll(s)) {
            lateFirst += r.ticket == first.ticket ? 1 : 0;
            thirdResults += r.ticket == third.ticket ? 1 : 0;
        }
        std::printf("a third send after the merged ticket was read: %d t%llu (first t%llu); first answered %d then %d, "
                    "third answered %d\n", third.status, static_cast<unsigned long long>(third.ticket),
                    static_cast<unsigned long long>(first.ticket), early, lateFirst, thirdResults);
        CHECK_EQ(again.status, FOXAPI_NO_CHANGE);
        CHECK_EQ(early, 1);
        CHECK_EQ(third.status, FOXAPI_OK);
        CHECK(third.ticket > first.ticket);
        CHECK_EQ(lateFirst, 0);
        CHECK_EQ(thirdResults, 1);
    }
    {   // EVERY SEND IN ITS OWN PLACE, EXACTLY AS IF NOT MERGED (round 6).
        // Window S closes the transmitter, window C opens it again, window D
        // presses PTT, and S's frame loop sends TX_CLOSE again (merged into
        // the first ticket). Round 5 applied the merged close "early" before
        // C's open AND before D's PTT, so the PTT was refused - stricter than
        // the same four commands one pass each, where the PTT keys and S's
        // second close opens it. That broke "merged or not, the same state"
        // (round-6 review), so the merged run must now do exactly what the
        // unmerged one does: the PTT keys, and S's second close - which has
        // its own place because commands that are not safer came between -
        // opens it again, with its reason.
        struct Seen {
            int32_t ptt = 999;
            bool unkeyedByClose = false;
            uint32_t flags = 0;
            int32_t close2 = 999;
        };
        auto runIt = [](bool merged) {
            testing::Engine e(api(), "token=t;pttHoldMs=2000;keepaliveMs=1000");
            testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            testing::Session c(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            testing::Session d(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
            s.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
            api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
            d.beat();
            Seen o;
            if (merged) {
                stall(admin, 400);
                submitOne(s, cmd(FOXAPI_OP_TX_CLOSE));
                submitOne(c, cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
                const FoxSubmitResult ptt = submitOne(d, cmd(FOXAPI_OP_TX_PTT, 0, 1));
                o.close2 = submitOne(s, cmd(FOXAPI_OP_TX_CLOSE)).status;
                for (const auto& r : drainAll(d, 600)) {
                    if (r.ticket == ptt.ticket) o.ptt = r.status;
                }
            } else {
                s.run(cmd(FOXAPI_OP_TX_CLOSE));
                c.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
                o.ptt = d.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status();
                o.close2 = s.run(cmd(FOXAPI_OP_TX_CLOSE)).status();
            }
            o.unkeyedByClose = sawUnkey(watcher, "transmitter was closed");
            o.flags = watcher.state().flags & (FOXAPI_RX_TX_AVAILABLE | FOXAPI_RX_TX_KEYED);
            return o;
        };
        const Seen m = runIt(true);
        const Seen u = runIt(false);
        std::printf("TX_CLOSE (S), TX_OPEN (C), PTT (D), TX_CLOSE (S): merged PTT -> %d, closed it %d; one a pass "
                    "PTT -> %d, closed it %d\n", m.ptt, m.unkeyedByClose ? 1 : 0, u.ptt, u.unkeyedByClose ? 1 : 0);
        CHECK_EQ(m.close2, FOXAPI_NO_CHANGE);  // it did merge
        CHECK_EQ(m.ptt, u.ptt);
        CHECK_EQ(m.unkeyedByClose, u.unkeyedByClose);
        CHECK_EQ(m.flags, u.flags);
        CHECK_EQ(u.ptt, FOXAPI_OK);
        CHECK(u.unkeyedByClose);
        CHECK_EQ(m.flags, 0u);  // closed, not keyed
    }
}

// A MERGED SEND KEEPS ITS FIRST PLACE (round-6 review, L6/l6b/l6c). A holds
// the latch. In ONE held pass window B sends a safer command X, A releases
// its own latch (LATCH 0), and B's frame loop sends X again (merged into B's
// ticket). Round 5 applied the merged X only in the place of its LAST send -
// after A's own release - so A's latch ended as its owner's release,
// unmarked, and a third window C that never released latched after the
// re-arm time; with X sent once, B ended the latch and the operator was
// marked. X is LATCH 0, RUN 0 and TX_CLOSE in turn; sent once or twice, C
// must be refused.
struct PlaceSeen {
    bool latchedBefore = false;
    int32_t second = 999;
    int32_t cLatch = 999;
    std::string cMessage;
};

PlaceSeen aroundAnOwnRelease(uint32_t op, bool twice) {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=60000;keepaliveMs=1000;token=t");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session c(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    PlaceSeen o;
    admin.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    a.beat();
    a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    o.latchedBefore = keyed(a);
    stall(admin, 300);
    submitOne(b, cmd(op, 0, 0));
    submitOne(a, cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    if (twice) {
        o.second = submitOne(b, cmd(op, 0, 0)).status;
    }
    drainAll(a, 500);
    drainAll(b, 50);
    if (op == FOXAPI_OP_RUN) admin.run(cmd(FOXAPI_OP_RUN, 0, 1));
    if (op == FOXAPI_OP_TX_CLOSE) admin.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    beatFor(c, FOXAPI_LATCH_REARM_MS + 100);
    const auto r = c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    o.cLatch = r.status();
    o.cMessage = r.result.message;
    return o;
}

void testMergedSendsKeepTheirPlace() {
    const uint32_t ops[3] = {FOXAPI_OP_TX_LATCH, FOXAPI_OP_RUN, FOXAPI_OP_TX_CLOSE};
    const char* names[3] = {"LATCH 0", "RUN 0", "TX_CLOSE"};
    std::vector<PlaceSeen> out(6);
    std::vector<std::thread> threads;
    for (int k = 0; k < 3; ++k) {
        threads.emplace_back([&out, &ops, k] { out[2 * k] = aroundAnOwnRelease(ops[k], false); });
        threads.emplace_back([&out, &ops, k] { out[2 * k + 1] = aroundAnOwnRelease(ops[k], true); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int k = 0; k < 3; ++k) {
        const PlaceSeen& once = out[2 * k];
        const PlaceSeen& twice = out[2 * k + 1];
        std::printf("B %s, A's own LATCH 0, [B %s again, merged]: C's LATCH 1 once -> %d, twice -> %d "
                    "(second send %d; \"%s\")\n", names[k], names[k], once.cLatch, twice.cLatch, twice.second,
                    twice.cMessage.c_str());
        CHECK(once.latchedBefore && twice.latchedBefore);
        CHECK_EQ(twice.second, FOXAPI_NO_CHANGE);  // the second send did merge
        CHECK_EQ(once.cLatch, FOXAPI_DENIED);      // B ended A's latch: marked
        CHECK_EQ(twice.cLatch, once.cLatch);       // ...merged or not
    }
}

// Presses the app's LATCH key (transmit_page.hpp: a TOGGLE of the shown
// state) the way the header's key protocol says an interface must: shown
// released, the press sends LATCH 0 and LATCH 1 in ONE batch; shown latched,
// it sends LATCH 0. Returns the last command's result.
int32_t pressLatchKey(testing::Session& s) {
    const bool shownLatched = (s.state().flags & FOXAPI_RX_TX_LATCHED) != 0u;
    FoxCommand q[2] = {cmd(FOXAPI_OP_TX_LATCH, 0, 0), cmd(FOXAPI_OP_TX_LATCH, 0, 1)};
    FoxSubmitResult sr[2];
    for (auto& x : sr) x.structSize = sizeof(x);
    const uint32_t n = shownLatched ? 1u : 2u;
    api()->submit(s.raw(), q, n, sr);
    const uint64_t last = sr[n - 1].ticket;
    int32_t status = sr[n - 1].status == FOXAPI_OK ? 999 : sr[n - 1].status;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (status == 999 && std::chrono::steady_clock::now() < end) {
        FoxCommandResult r[16];
        for (auto& x : r) x.structSize = sizeof(x);
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == last) status = r[i].status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return status;
}

bool releaseFirst(testing::Session& s) { return (s.state().flags & FOXAPI_RX_TX_LATCH_RELEASE_FIRST) != 0u; }

// THE LATCH KEY PROTOCOL (round-6 review, L1): the app's own latch key is a
// toggle, and ported as "a press sends LATCH <not what is shown>" it asked
// for LATCH 1 after every timeout and was refused for ever - after a restart
// too - with nothing in the state to say why. Now (1) the state has
// FOXAPI_RX_TX_LATCH_RELEASE_FIRST, per session: this session must send
// LATCH 0 before its LATCH 1 is taken; and (2) the header's key protocol has
// a press that latches send LATCH 0 and LATCH 1 in ONE batch. A window stuck
// sending LATCH 1 every frame (its own thread, never stopped) stays refused
// throughout; the toggle key that follows the protocol recovers after a
// timeout, after its own release (which marks nobody: round-6 mutant
// owner_release_remarks), and in a restarted interface.
void testLatchKeyProtocol() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=600;keepaliveMs=1000;token=t");
    testing::Session t(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session stuck(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    openTx(t);
    std::atomic<bool> stop{false};
    std::atomic<int> stuckOk{0};
    std::atomic<bool> stuckFlag{false};
    std::thread level([&] {  // the stuck window: LATCH 1 every frame
        while (!stop.load()) {
            stuck.beat();
            FoxCommand c = cmd(FOXAPI_OP_TX_LATCH, 0, 1);
            api()->submit(stuck.raw(), &c, 1, nullptr);
            FoxCommandResult rr[64];
            for (auto& x : rr) x.structSize = sizeof(x);
            int32_t n = 0;
            while ((n = api()->poll_results(stuck.raw(), rr, 64)) > 0) {
                for (int32_t i = 0; i < n; ++i) stuckOk += rr[i].status == FOXAPI_OK ? 1 : 0;
            }
            stuckFlag = releaseFirst(stuck);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    CHECK(check::waitFor([&] { t.beat(); return keyed(obs); }, 2000));  // the stuck window's one latch
    CHECK(!releaseFirst(t));  // nothing has ended yet
    beatFor(t, 600 + FOXAPI_LATCH_REARM_MS + 200);  // it times out: the operator is marked
    CHECK(!keyed(obs));
    const bool tFlagAfterTimeout = releaseFirst(t);
    const bool stuckFlagAfterTimeout = stuckFlag.load();
    // The probe's port of the toggle, which sends only LATCH 1: refused.
    t.beat();
    const int32_t bare = t.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    // A second window W releases now (nothing latched): W is exempt.
    testing::Session w(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    w.beat();
    w.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    const bool wFlagAfterRelease = releaseFirst(w);
    // The protocol's press: taken.
    t.beat();
    const int32_t press1 = pressLatchKey(t);
    const FoxReceiverState held = t.state();
    const bool tFlagWhileHeld = releaseFirst(t);
    beatFor(t, 300);
    const bool stillMine = (t.state().flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
    t.beat();
    const int32_t press2 = pressLatchKey(t);  // shown latched: the release
    const bool openAfter = !keyed(obs);
    beatFor(t, FOXAPI_LATCH_REARM_MS + 300);
    // T's own release marked nobody: T is not flagged, and W - which
    // released before T latched and has not touched the key since - keeps
    // its exemption: its bare LATCH 1 is taken. (Round-6 mutant
    // owner_release_remarks re-marked here; the owner re-exempts itself on
    // the same LATCH 0, so only ANOTHER exempt window can see it.)
    const bool tFlagAfterOwn = releaseFirst(t);
    const bool wFlagAfterOwn = releaseFirst(w);
    w.beat();
    const int32_t bareAfterOwn = w.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    w.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    beatFor(t, FOXAPI_LATCH_REARM_MS + 100);
    // A restarted interface: a new session starts marked, and one press works.
    testing::Session t2(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    t2.beat();
    const bool t2Flag = releaseFirst(t2);
    const int32_t press3 = pressLatchKey(t2);
    const bool t2Mine = (t2.state().flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
    pressLatchKey(t2);
    beatFor(t2, 300);
    stop.store(true);
    level.join();
    std::printf("latch key protocol beside a stuck window: flags after the timeout T %d stuck %d; bare LATCH 1 -> %d; "
                "W released, flag %d; press -> %d (latched %d, flag %d, still mine after 300 ms %d); press -> %d "
                "(open %d); after T's own release: T flag %d, W flag %d, W's bare LATCH 1 -> %d; restarted T2 flag "
                "%d, press -> %d mine %d; stuck window latches %d, flag at the end %d\n",
                tFlagAfterTimeout ? 1 : 0, stuckFlagAfterTimeout ? 1 : 0, bare, wFlagAfterRelease ? 1 : 0, press1,
                (held.flags & FOXAPI_RX_TX_LATCHED) ? 1 : 0, tFlagWhileHeld ? 1 : 0, stillMine ? 1 : 0, press2,
                openAfter ? 1 : 0, tFlagAfterOwn ? 1 : 0, wFlagAfterOwn ? 1 : 0, bareAfterOwn, t2Flag ? 1 : 0,
                press3, t2Mine ? 1 : 0, stuckOk.load(), stuckFlag.load() ? 1 : 0);
    CHECK(tFlagAfterTimeout);
    CHECK(stuckFlagAfterTimeout);
    CHECK_EQ(bare, FOXAPI_DENIED);
    CHECK_EQ(press1, FOXAPI_OK);
    CHECK((held.flags & FOXAPI_RX_TX_LATCHED) != 0u);
    CHECK((held.flags & FOXAPI_RX_TX_KEY_MINE) != 0u);
    CHECK(!tFlagWhileHeld);
    CHECK(stillMine);
    CHECK_EQ(press2, FOXAPI_OK);
    CHECK(openAfter);
    CHECK(!wFlagAfterRelease);
    CHECK(!tFlagAfterOwn);
    CHECK(!wFlagAfterOwn);
    CHECK_EQ(bareAfterOwn, FOXAPI_OK);
    CHECK(t2Flag);
    CHECK_EQ(press3, FOXAPI_OK);
    CHECK(t2Mine);
    CHECK_EQ(stuckOk.load(), 1);  // the stuck window's first latch, and never another
    CHECK(stuckFlag.load());
}

// A SAFER COMMAND REFUSED AT SUBMIT IS AN ORDINARY COMMAND - so it counts
// toward FOXAPI_MAX_PENDING and is answered BUSY past it, as docs/API.md 7.2
// says (round-6 mutant refused_safer_never_busy survived: nothing tested it).
// The valid one of the kind beside it is still never BUSY.
void testRefusedSaferCommandCanBeBusy() {
    testing::Engine e(api(), "token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    std::vector<FoxCommand> fill(FOXAPI_MAX_PENDING, cmd(FOXAPI_OP_SET_VOLUME, 0.3));
    CHECK_EQ(api()->submit(s.raw(), fill.data(), static_cast<uint32_t>(fill.size()), nullptr),
             static_cast<int32_t>(FOXAPI_MAX_PENDING));
    FoxCommand bad = cmd(FOXAPI_OP_TX_LATCH, 0, 0);
    bad.num[2] = std::nan("");
    const FoxSubmitResult badR = submitOne(s, bad);
    const FoxSubmitResult goodR = submitOne(s, cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    std::printf("256 unread, then a malformed LATCH 0 -> %d (ticket %llu), a valid one -> %d\n", badR.status,
                static_cast<unsigned long long>(badR.ticket), goodR.status);
    CHECK_EQ(badR.status, FOXAPI_BUSY);
    CHECK_EQ(badR.ticket, 0ull);
    CHECK_EQ(goodR.status, FOXAPI_OK);
}

// THE mock.stallAfterPass SEAM HOLDS THE CONTROL THREAD (round-6 mutant
// seam_noop survived: testDetachedKeyHolderIsReleasedFirst leans on it, and
// nothing proved it did anything). A command sent right after it waits out
// the stall; the same command without it is answered at once.
void testStallAfterPassHolds() {
    testing::Engine e(api(), "token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    auto timeOne = [&] {
        const auto t0 = std::chrono::steady_clock::now();
        s.run(cmd(FOXAPI_OP_SET_VOLUME, 0.2));
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    const long long plain = timeOne();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.stallAfterPass=400")).status(), FOXAPI_OK);
    const long long held = timeOne();
    std::printf("a command answered in %lld ms, and %lld ms right after mock.stallAfterPass=400\n", plain, held);
    CHECK(plain < 150);
    CHECK(held >= 250);
}

// A RESULT NEVER ARRIVES BEFORE THE STATE IT DESCRIBES. Round-4 break-it
// run: a LATCH 1 answered OK was twice read back UNKEYED, because the result
// was delivered while the pass was still applying and the state was
// published only at its end. Here the pass is made long on purpose (a stall
// queued right behind the command): the result must not be readable until
// the snapshot shows it.
void testResultsFollowTheirState() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=1000;token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(s);
    s.beat();
    FoxCommand q[3] = {cmd(FOXAPI_OP_TX_LATCH, 0, 1), cmd(FOXAPI_OP_SET_VOLUME, 0.7),
                       cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.stall=300")};
    FoxSubmitResult r[3];
    for (auto& x : r) x.structSize = sizeof(x);
    CHECK_EQ(api()->submit(s.raw(), q, 3, r), 3);
    int32_t latchStatus = 999;
    bool keyedWhenAnswered = false;
    double volumeWhenAnswered = -1;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (latchStatus == 999 && std::chrono::steady_clock::now() < end) {
        FoxCommandResult got[8];
        for (auto& x : got) x.structSize = sizeof(x);
        const int32_t n = api()->poll_results(s.raw(), got, 8);
        for (int32_t i = 0; i < n; ++i) {
            if (got[i].ticket == r[0].ticket) {
                latchStatus = got[i].status;
                const FoxReceiverState st = s.state();  // at once, as an interface would
                keyedWhenAnswered = (st.flags & FOXAPI_RX_TX_KEYED) != 0u;
                volumeWhenAnswered = st.volume;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("LATCH 1 answered %d; the snapshot then: keyed %d, volume %.1f\n", latchStatus,
                keyedWhenAnswered ? 1 : 0, volumeWhenAnswered);
    CHECK_EQ(latchStatus, FOXAPI_OK);
    CHECK(keyedWhenAnswered);
    CHECK_EQ(volumeWhenAnswered, 0.7);
    s.beat();
    s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
}

// LATCH 0 IS IDEMPOTENT, SO MERGING AND RE-APPLYING IT IS HARMLESS. Round-5
// review (r3): with LATCH 0 meaning "release and mark" while a latch was
// active but "clear the operator's mark" while none was, merging changed the
// outcome. Queued in one stalled pass: B LATCH 0, A LATCH 1, C RUN 1,
// B LATCH 0. In that order A latches and B's second LATCH 0 releases it from
// another window (marked). Merged, B's LATCH 0 was applied early before A's
// LATCH 1 and again before C's RUN 1 (releasing A: marked), then once more in
// its own place - with nothing latched, which CLEARED the mark, and A latched
// again after the re-arm time. Now a LATCH 0 only ever exempts the session
// that sent it, so applying it once or five times, early or in its place,
// leaves the same state: A refused, B free to press.
struct LatchState {
    bool keyedAfterPass = true;
    int32_t aResult = 999;       // A's LATCH 1 in the pass
    int32_t bSecond = 999;       // B's second submit: NO_CHANGE when merged
    int32_t aAfter = 999;        // A's LATCH 1 past the re-arm time
    std::string aAfterMessage;
    int32_t bAfter = 999;        // B's LATCH 1 past the re-arm time
    bool bKeyed = false;
};

LatchState releaseAroundALatch(bool merged, int extraReleases) {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=2000;token=t");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session c(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    admin.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    LatchState o;
    a.beat();
    if (merged) {
        stall(admin, 300);
        submitOne(b, cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        const FoxSubmitResult a1 = submitOne(a, cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        submitOne(c, cmd(FOXAPI_OP_RUN, 0, 1));
        o.bSecond = submitOne(b, cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status;
        for (int i = 0; i < extraReleases; ++i) {
            submitOne(b, cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        }
        for (const auto& r : drainAll(a, 500)) {
            if (r.ticket == a1.ticket) o.aResult = r.status;
        }
    } else {
        b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        o.aResult = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
        c.run(cmd(FOXAPI_OP_RUN, 0, 1));
        o.bSecond = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status();
        for (int i = 0; i < extraReleases; ++i) {
            b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        }
    }
    o.keyedAfterPass = keyed(admin);
    beatFor(a, FOXAPI_LATCH_REARM_MS + 100);
    a.beat();
    const auto again = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    o.aAfter = again.status();
    o.aAfterMessage = again.result.message;
    b.beat();
    o.bAfter = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    o.bKeyed = keyed(b);
    b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
    return o;
}

void testLatchReleaseIsIdempotent() {
    struct Case {
        bool merged;
        int extra;
        const char* name;
    };
    const Case cases[] = {{true, 0, "merged in one pass (the review's r3)"},
                          {false, 0, "one pass each"},
                          {true, 4, "merged, B's LATCH 0 sent six times"},
                          {false, 4, "one pass each, B's LATCH 0 sent six times"}};
    std::vector<LatchState> out(std::size(cases));
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        threads.emplace_back([&out, &cases, i] { out[i] = releaseAroundALatch(cases[i].merged, cases[i].extra); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const LatchState& o = out[i];
        std::printf("B LATCH 0, A LATCH 1, C RUN 1, B LATCH 0 - %s: A's latch -> %d, B's second -> %d, keyed after "
                    "%d; past the re-arm time A's LATCH 1 -> %d \"%s\", B's -> %d keyed %d\n",
                    cases[i].name, o.aResult, o.bSecond, o.keyedAfterPass ? 1 : 0, o.aAfter,
                    o.aAfterMessage.c_str(), o.bAfter, o.bKeyed ? 1 : 0);
        CHECK_EQ(o.aResult, FOXAPI_OK);                                  // A did latch in between
        CHECK_EQ(o.bSecond, cases[i].merged ? FOXAPI_NO_CHANGE : FOXAPI_OK);  // the merge happened
        CHECK(!o.keyedAfterPass);                                        // B's release opened it
        CHECK_EQ(o.aAfter, FOXAPI_DENIED);                               // A never released
        CHECK(o.aAfterMessage.find("released from another interface") != std::string::npos);
        CHECK_EQ(o.bAfter, FOXAPI_OK);                                   // B did
        CHECK(o.bKeyed);
    }
}

// ALL FIVE SAFER COMMANDS ARE IDEMPOTENT (foxsdr_api.h, FOXAPI_SAFETY_RESERVE):
// what merging and the early re-application rely on. Local window L holds a
// latch with consent given; window O sends one safer command once, three
// times in separate passes, or three times merged into one pass. The state
// afterwards - running, transmitter, key, latch, consent, why the key
// opened - and who may latch afterwards (O, then L, past the re-arm time)
// must be the same all three ways.
struct AfterSafer {
    uint32_t flags = 0;
    std::string unkeyReason;
    int32_t oLatch = 999;
    int32_t lLatch = 999;
};

AfterSafer afterSafer(uint32_t op, int times, bool merged) {
    testing::Engine e(api(), "pttHoldMs=2000;latchTimeoutMs=10000;keepaliveMs=5000;token=t");
    testing::Session l(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session o(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    l.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    l.beat();
    l.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1));
    l.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    if (merged) {
        stall(admin, 200);
        for (int i = 0; i < times; ++i) {
            submitOne(o, cmd(op, 0, 0));
        }
        drainAll(o, 400);
    } else {
        for (int i = 0; i < times; ++i) {
            o.run(cmd(op, 0, 0));
        }
    }
    AfterSafer a;
    const FoxReceiverState st = l.state();
    a.flags = st.flags & (FOXAPI_RX_RUNNING | FOXAPI_RX_TX_AVAILABLE | FOXAPI_RX_TX_KEYED | FOXAPI_RX_TX_LATCHED |
                          FOXAPI_RX_TX_REMOTE_ARMED);
    a.unkeyReason = st.txUnkeyReason;
    beatFor(l, FOXAPI_LATCH_REARM_MS + 100);
    o.beat();
    a.oLatch = o.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    if (a.oLatch == FOXAPI_OK) {
        o.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
        beatFor(l, FOXAPI_LATCH_REARM_MS + 100);  // O's own release starts the re-arm time again
    }
    l.beat();
    a.lLatch = l.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    return a;
}

void testSaferCommandsAreIdempotent() {
    const uint32_t ops[5] = {FOXAPI_OP_TX_PTT, FOXAPI_OP_TX_LATCH, FOXAPI_OP_RUN, FOXAPI_OP_TX_CLOSE,
                             FOXAPI_OP_TX_REMOTE_ARM};
    const char* names[5] = {"TX_PTT 0", "TX_LATCH 0", "RUN 0", "TX_CLOSE", "TX_REMOTE_ARM 0"};
    std::vector<AfterSafer> out(15);
    std::vector<std::thread> threads;
    for (int k = 0; k < 5; ++k) {
        threads.emplace_back([&out, &ops, k] { out[3 * k] = afterSafer(ops[k], 1, false); });
        threads.emplace_back([&out, &ops, k] { out[3 * k + 1] = afterSafer(ops[k], 3, false); });
        threads.emplace_back([&out, &ops, k] { out[3 * k + 2] = afterSafer(ops[k], 3, true); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int k = 0; k < 5; ++k) {
        const AfterSafer& once = out[3 * k];
        const AfterSafer& thrice = out[3 * k + 1];
        const AfterSafer& merged = out[3 * k + 2];
        std::printf("%s from another window, once / three times / three merged: flags %x/%x/%x, O's LATCH 1 %d/%d/%d, "
                    "L's %d/%d/%d, why \"%s\"\n", names[k], once.flags, thrice.flags, merged.flags, once.oLatch,
                    thrice.oLatch, merged.oLatch, once.lLatch, thrice.lLatch, merged.lLatch, once.unkeyReason.c_str());
        CHECK_EQ(thrice.flags, once.flags);
        CHECK_EQ(merged.flags, once.flags);
        CHECK_EQ(thrice.unkeyReason, once.unkeyReason);
        CHECK_EQ(merged.unkeyReason, once.unkeyReason);
        CHECK_EQ(thrice.oLatch, once.oLatch);
        CHECK_EQ(merged.oLatch, once.oLatch);
        CHECK_EQ(thrice.lLatch, once.lLatch);
        CHECK_EQ(merged.lLatch, once.lLatch);
    }
}

// A SAFER COMMAND REFUSED AT SUBMIT IS NEVER MERGED. Round-5 review (r2): a
// malformed LATCH 0 (NaN) or RUN 0 (text with no NUL) sent after a valid one
// of the kind REPLACED it in the queue, so the valid release or stop was
// never applied and the latch stayed closed. A command refused at submit
// changes nothing, so it is not a safer command at all: it takes its own
// ticket (as an ordinary command, within FOXAPI_MAX_PENDING) and its own
// refusal, and the valid one is applied - in either order.
struct MalformedSeen {
    int32_t taken = 0;
    int32_t sr0 = 999, sr1 = 999;
    uint64_t t0 = 0, t1 = 0;
    int32_t goodResult = 999, badResult = 999;
    uint32_t badFlags = 0;
    bool keyedAfter = true;
    bool runningAfter = true;
};

MalformedSeen malformedBeside(uint32_t op, bool badFirst) {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=1000;token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    s.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    s.beat();
    s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    FoxCommand good = cmd(op, 0, 0);
    FoxCommand bad = cmd(op, 0, 0);
    if (op == FOXAPI_OP_TX_LATCH) {
        bad.num[1] = std::nan("");
    } else {
        std::memset(bad.text, 'x', sizeof(bad.text));  // no NUL
    }
    FoxCommand batch[2] = {badFirst ? bad : good, badFirst ? good : bad};
    FoxSubmitResult sr[2];
    for (auto& x : sr) x.structSize = sizeof(x);
    MalformedSeen o;
    o.taken = api()->submit(s.raw(), batch, 2, sr);
    o.sr0 = sr[0].status;
    o.sr1 = sr[1].status;
    o.t0 = sr[0].ticket;
    o.t1 = sr[1].ticket;
    const uint64_t goodTicket = badFirst ? sr[1].ticket : sr[0].ticket;
    const uint64_t badTicket = badFirst ? sr[0].ticket : sr[1].ticket;
    for (const auto& r : drainAll(s)) {
        if (r.ticket == goodTicket) o.goodResult = r.status;
        if (r.ticket == badTicket) {
            o.badResult = r.status;
            o.badFlags = r.flags;
        }
    }
    const FoxReceiverState st = obs.state();
    o.keyedAfter = (st.flags & FOXAPI_RX_TX_KEYED) != 0u;
    o.runningAfter = (st.flags & FOXAPI_RX_RUNNING) != 0u;
    return o;
}

void testRefusedSaferCommandsAreNotMerged() {
    for (const uint32_t op : {static_cast<uint32_t>(FOXAPI_OP_TX_LATCH), static_cast<uint32_t>(FOXAPI_OP_RUN)}) {
        for (const bool badFirst : {false, true}) {
            const MalformedSeen o = malformedBeside(op, badFirst);
            std::printf("%s 0 %s: submit -> %d (%d t%llu, %d t%llu); valid one -> %d, malformed one -> %d; "
                        "keyed after %d, running after %d\n",
                        op == FOXAPI_OP_RUN ? "RUN" : "LATCH", badFirst ? "malformed then valid" : "valid then malformed",
                        o.taken, o.sr0, static_cast<unsigned long long>(o.t0), o.sr1,
                        static_cast<unsigned long long>(o.t1), o.goodResult, o.badResult, o.keyedAfter ? 1 : 0,
                        o.runningAfter ? 1 : 0);
            CHECK_EQ(o.taken, 2);  // two NEW tickets: nothing merged
            CHECK_EQ(o.sr0, FOXAPI_OK);
            CHECK_EQ(o.sr1, FOXAPI_OK);
            CHECK(o.t0 != 0 && o.t1 != 0 && o.t0 != o.t1);
            CHECK_EQ(o.goodResult, FOXAPI_OK);
            CHECK_EQ(o.badResult, FOXAPI_BAD_ARGUMENT);
            CHECK((o.badFlags & FOXAPI_RESULT_REFUSED) != 0u);
            CHECK(!o.keyedAfter);  // the valid release or stop was applied
            if (op == FOXAPI_OP_RUN) {
                CHECK(!o.runningAfter);
            }
        }
    }
}

// RESULTS ARRIVE IN TICKET ORDER, MERGED ONES INCLUDED (API.md rule 2).
// Round-5 review (r5/order): RUN 0 (t1), SET_VOLUME (t2), RUN 0 (merged
// into t1) in one submit: the merged copy replaced t1's in the queue, BEHIND
// t2, and the results came out 2 then 1.
void testMergedResultsKeepTicketOrder() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=2000;token=t");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    for (const bool held : {false, true}) {
        if (held) {
            stall(admin, 300);  // the same, queued behind a busy control thread
        }
        FoxCommand q[5] = {cmd(FOXAPI_OP_RUN, 0, 0), cmd(FOXAPI_OP_SET_VOLUME, 0.3), cmd(FOXAPI_OP_RUN, 0, 0),
                           cmd(FOXAPI_OP_SET_SQUELCH, -80.0), cmd(FOXAPI_OP_RUN, 0, 0)};
        FoxSubmitResult sr[5];
        for (auto& x : sr) x.structSize = sizeof(x);
        CHECK_EQ(api()->submit(s.raw(), q, 5, sr), 3);
        CHECK_EQ(sr[2].status, FOXAPI_NO_CHANGE);
        CHECK_EQ(sr[2].ticket, sr[0].ticket);
        CHECK_EQ(sr[4].ticket, sr[0].ticket);
        std::vector<uint64_t> order;
        for (const auto& r : drainAll(s, held ? 600 : 300)) {
            order.push_back(r.ticket);
        }
        std::printf("RUN 0, VOLUME, RUN 0 (merged), SQUELCH, RUN 0 (merged)%s: results for tickets", held ? ", held" : "");
        for (uint64_t t : order) std::printf(" %llu", static_cast<unsigned long long>(t));
        std::printf(" (tickets %llu %llu %llu)\n", static_cast<unsigned long long>(sr[0].ticket),
                    static_cast<unsigned long long>(sr[1].ticket), static_cast<unsigned long long>(sr[3].ticket));
        CHECK(order == std::vector<uint64_t>({sr[0].ticket, sr[1].ticket, sr[3].ticket}));
        s.run(cmd(FOXAPI_OP_RUN, 0, 1));
    }
}

// THE TICKET BOOKS HOLD UNDER MERGING (the round-5 review's stress probe):
// one session sends random batches of safer and ordinary commands - PTT,
// LATCH, RUN, consent, volume - for 3000 frames, reading some results now
// and then, while another session holds the control thread for random
// stalls. Every NEW ticket gets exactly one result, no result names an
// unknown ticket, NEW tickets only increase, results arrive in strictly
// increasing ticket order, and NO_CHANGE never names a ticket whose result
// has already been read (a merge into a delivered ticket would be a second
// answer nobody gets).
void testTicketBooksUnderMerging() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=5000");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(admin);
    std::mt19937 rng(12345);
    std::map<uint64_t, int> results;
    std::set<uint64_t> issued, unread;
    uint64_t maxTicket = 0, lastResult = 0;
    int badNoChange = 0, unknown = 0, nonIncreasing = 0, merged = 0, outOfOrder = 0;
    std::atomic<bool> stopStalls{false};
    std::thread staller([&] {
        std::mt19937 r2(7);
        while (!stopStalls.load()) {
            const std::string t = "mock.stall=" + std::to_string(r2() % 40);
            fire(admin, {cmd(FOXAPI_OP_SETTING_SET, 0, 0, t.c_str())});
            admin.beat();
            std::this_thread::sleep_for(std::chrono::milliseconds(r2() % 30));
        }
    });
    auto readSome = [&](uint32_t cap) {
        FoxCommandResult rr[64];
        for (auto& x : rr) x.structSize = sizeof(x);
        const int32_t n = api()->poll_results(a.raw(), rr, cap);
        for (int32_t i = 0; i < n; ++i) {
            unknown += issued.count(rr[i].ticket) == 0 ? 1 : 0;
            outOfOrder += rr[i].ticket <= lastResult ? 1 : 0;
            lastResult = rr[i].ticket;
            ++results[rr[i].ticket];
            unread.erase(rr[i].ticket);
        }
    };
    for (int iter = 0; iter < 3000; ++iter) {
        a.beat();
        const int nb = 1 + static_cast<int>(rng() % 6);
        std::vector<FoxCommand> v;
        for (int i = 0; i < nb; ++i) {
            switch (rng() % 9) {
            case 0: v.push_back(cmd(FOXAPI_OP_TX_PTT, 0, 0)); break;
            case 1: v.push_back(cmd(FOXAPI_OP_TX_LATCH, 0, 0)); break;
            case 2: v.push_back(cmd(FOXAPI_OP_RUN, 0, 0)); break;
            case 3: v.push_back(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0)); break;
            case 4: v.push_back(cmd(FOXAPI_OP_TX_PTT, 0, 1)); break;
            case 5: v.push_back(cmd(FOXAPI_OP_RUN, 0, 1)); break;
            case 6: v.push_back(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)); break;
            case 7: v.push_back(cmd(FOXAPI_OP_SET_VOLUME, static_cast<double>(rng() % 100) / 100.0)); break;
            default: v.push_back(cmd(FOXAPI_OP_TX_LATCH, 0, 1)); break;
            }
        }
        std::vector<FoxSubmitResult> sr(v.size());
        for (auto& x : sr) x.structSize = sizeof(x);
        api()->submit(a.raw(), v.data(), static_cast<uint32_t>(v.size()), sr.data());
        for (const auto& r : sr) {
            if (r.status == FOXAPI_OK) {
                nonIncreasing += r.ticket <= maxTicket ? 1 : 0;
                maxTicket = std::max(maxTicket, r.ticket);
                issued.insert(r.ticket);
                unread.insert(r.ticket);
            } else if (r.status == FOXAPI_NO_CHANGE) {
                ++merged;
                badNoChange += unread.count(r.ticket) == 0 ? 1 : 0;
            }
        }
        if (rng() % 3 == 0) readSome(1 + static_cast<uint32_t>(rng() % 64));
        if (rng() % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stopStalls.store(true);
    staller.join();
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500)) {
        a.beat();
        readSome(64);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int missing = 0, dup = 0;
    for (uint64_t t : issued) {
        const int n = results.count(t) != 0 ? results[t] : 0;
        missing += n == 0 ? 1 : 0;
        dup += n > 1 ? 1 : 0;
    }
    std::printf("ticket books: %zu new tickets, %d merged; missing %d, duplicated %d, unknown %d, out of order %d, "
                "NO_CHANGE naming a read ticket %d, non-increasing new tickets %d\n",
                issued.size(), merged, missing, dup, unknown, outOfOrder, badNoChange, nonIncreasing);
    CHECK(issued.size() > 1000);
    CHECK(merged > 1000);  // the probe did merge, a lot
    CHECK_EQ(missing, 0);
    CHECK_EQ(dup, 0);
    CHECK_EQ(unknown, 0);
    CHECK_EQ(outOfOrder, 0);
    CHECK_EQ(badNoChange, 0);
    CHECK_EQ(nonIncreasing, 0);
}

// A GONE WINDOW'S RELEASE STILL RELEASES, AND FREES NOBODY. (1) Window B
// presses RELEASE and closes before the control thread gets to it: the
// release is a safer command, so it is still applied - A's latch opens and
// the operator is marked (round-5 mutant: a gone session's LATCH 0 skipped,
// and the latch stayed closed). (2) Round-5 review (r6): with the operator
// already marked, a window that queued LATCH 0 and closed CLEARED the mark
// for everyone; now a closed session's LATCH 0 exempts nobody - not even
// itself, it is gone - so "closing sessions never clears a mark" is exact.
void testGoneWindowsReleaseStillReleases() {
    {
        testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=2000;token=t");
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        openTx(admin);
        a.beat();
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        stall(admin, 300);
        {
            testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            CHECK_EQ(submitOne(b, cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status, FOXAPI_OK);
            b.close();  // RELEASE, then the window closes
        }
        const bool opened = check::waitFor([&] { a.beat(); return !keyed(obs); }, 800);
        beatFor(a, FOXAPI_LATCH_REARM_MS + 100);
        const auto again = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        std::printf("RELEASE queued in window B, which then closed: latch opened %d; A's LATCH 1 after -> %d \"%s\"\n",
                    opened ? 1 : 0, again.status(), again.result.message);
        CHECK(opened);
        CHECK_EQ(again.status(), FOXAPI_DENIED);
        CHECK(std::strstr(again.result.message, "released from another interface") != nullptr);
    }
    {
        testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=2000;token=t");
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        openTx(admin);
        a.beat();
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(admin.run(cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);  // stop: the operator is marked
        CHECK_EQ(admin.run(cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        beatFor(a, FOXAPI_LATCH_REARM_MS + 100);
        const int32_t marked = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
        stall(admin, 300);
        {
            testing::Session w(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            submitOne(w, cmd(FOXAPI_OP_TX_LATCH, 0, 0));
            w.close();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        a.beat();
        const auto after = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        std::printf("operator marked, LATCH 1 -> %d; a window queued LATCH 0 and closed: LATCH 1 -> %d \"%s\"\n",
                    marked, after.status(), after.result.message);
        CHECK_EQ(marked, FOXAPI_DENIED);
        CHECK_EQ(after.status(), FOXAPI_DENIED);
        CHECK(!keyed(a));
        testing::Session fresh(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        fresh.beat();
        CHECK_EQ(fresh.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    }
}

// A DETACHED KEY HOLDER IS RELEASED FIRST (API.md 7.2: "any key or consent
// it held is released before anything it had queued would have been
// applied"). A remote session holds the PTT; while the control thread is
// held, the local operator presses PTT and the remote's login is revoked.
// The detach is dealt with FIRST in the pass, so the local PTT finds the key
// free and keys, and the key's release names the revoked login. (Round-5
// mutant: a detached owner left to the end-of-pass check - the local PTT was
// answered BUSY, another interface holding the key.)
void testDetachedKeyHolderIsReleasedFirst() {
    testing::Engine e(api(), "user=op;password=pw;pttHoldMs=2000;keepaliveMs=2000;remoteKeepaliveMs=2000");
    char tok[80] = {};
    CHECK_EQ(api()->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    openTx(local);
    local.beat();
    remote.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(watcher));
    api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    // Held AFTER a pass has made its keep-alive and token checks (a plain
    // mock.stall is inside a pass, whose own end-of-pass check would find
    // the detached owner and hide the order under test).
    fire(admin, {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.stallAfterPass=300")});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    local.beat();
    const FoxSubmitResult ptt = submitOne(local, cmd(FOXAPI_OP_TX_PTT, 0, 1));
    const bool heldAtRevoke = keyed(watcher);
    CHECK_EQ(api()->logout(e.raw(), tok), FOXAPI_OK);
    CHECK(heldAtRevoke);  // the remote still held the key when its login went
    int32_t pttStatus = 999;
    for (const auto& r : drainAll(local, 500)) {
        if (r.ticket == ptt.ticket) pttStatus = r.status;
    }
    const FoxReceiverState st = local.state();
    std::printf("remote key holder detached while a local PTT waited behind it: local PTT -> %d, key mine %d\n",
                pttStatus, (st.flags & FOXAPI_RX_TX_KEY_MINE) ? 1 : 0);
    CHECK_EQ(pttStatus, FOXAPI_OK);
    CHECK((st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u);
    CHECK(sawUnkey(watcher, "revoked"));
    local.run(cmd(FOXAPI_OP_TX_PTT, 0, 0));
}

// A CLOSED OR DETACHED SESSION'S QUEUED COMMANDS: ONLY THE SAFER ONES APPLY.
// Round-4 review: a closed session's queued TX_REMOTE_ARM 1 was applied after
// the close had been dealt with (only its key requests were refused), so a
// remote PTT queued behind it in the same pass keyed the transmitter on
// consent from a window that had already gone. Now a session that has closed
// or been detached has only TX_LATCH 0, TX_PTT 0, RUN 0, TX_CLOSE and
// TX_REMOTE_ARM 0 applied; everything else it left queued is refused
// (DETACHED).
void testGoneSessionsApplyOnlySaferCommands() {
    {   // the review's scenario
        testing::Engine e(api(), "token=t;pttHoldMs=2000;keepaliveMs=1000;remoteKeepaliveMs=2000");
        auto local = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
        openTx(*local);
        local->beat();
        remote.beat();
        CHECK_EQ(local->run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
        api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
        const uint64_t txSeqBefore = watcher.state().txSeq;
        stall(*local, 400);
        local->beat();
        remote.beat();
        submitOne(*local, cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1));  // a level-triggered page re-asserts consent
        const FoxSubmitResult ptt = submitOne(remote, cmd(FOXAPI_OP_TX_PTT, 0, 1));
        CHECK_EQ(ptt.status, FOXAPI_OK);
        local->close();  // the window closes: its consent must lapse
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        int32_t pttStatus = 999;
        double applied = -1;
        for (const auto& r : drainAll(remote)) {
            if (r.ticket == ptt.ticket) {
                pttStatus = r.status;
                applied = r.applied[0];
            }
        }
        const FoxReceiverState st = watcher.state();
        std::printf("closed window's queued re-arm + remote PTT: PTT -> %d applied %.0f; txSeq %llu -> %llu\n",
                    pttStatus, applied, static_cast<unsigned long long>(txSeqBefore),
                    static_cast<unsigned long long>(st.txSeq));
        CHECK_EQ(pttStatus, FOXAPI_DENIED);
        CHECK_EQ(applied, 0.0);
        CHECK(!sawUnkey(watcher, ""));  // nothing was keyed, so nothing had to open
        CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
        CHECK((st.flags & FOXAPI_RX_TX_REMOTE_ARMED) == 0u);
        CHECK_EQ(st.txSeq, txSeqBefore + 1);  // the consent lapsing, and nothing else
    }
    {   // a closed session's retune, setting and key request are refused; its stop is not
        testing::Engine e(api(), kFast);
        auto local = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        openTx(*local);
        api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
        const FoxReceiverState before = watcher.state();
        stall(admin, 400);
        local->beat();
        FoxCommand q[4] = {cmd(FOXAPI_OP_SET_CENTRE, 100.0e6), cmd(FOXAPI_OP_TX_LATCH, 0, 1),
                           cmd(FOXAPI_OP_SET_VOLUME, 0.9), cmd(FOXAPI_OP_RUN, 0, 0)};
        CHECK_EQ(api()->submit(local->raw(), q, 4, nullptr), 4);
        local->close();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const FoxReceiverState st = watcher.state();
        std::printf("closed session's queued retune/latch/volume/stop: centre %.0f (was %.0f), volume %.2f, "
                    "running %d, keyed %d\n", st.centreHz, before.centreHz, st.volume,
                    (st.flags & FOXAPI_RX_RUNNING) ? 1 : 0, (st.flags & FOXAPI_RX_TX_KEYED) ? 1 : 0);
        CHECK_EQ(st.centreHz, before.centreHz);
        CHECK_EQ(st.volume, before.volume);
        CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
        CHECK(!sawUnkey(watcher, ""));  // the latch never closed
        CHECK((st.flags & FOXAPI_RX_RUNNING) == 0u);  // the stop still stopped
    }
    {   // a DETACHED session's stop applies too; its retune does not
        testing::Engine e(api(), "user=op;password=pw");
        char tok[80] = {};
        CHECK_EQ(api()->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE, tok);
        const double centre = admin.state().centreHz;
        stall(admin, 400);
        FoxCommand q[2] = {cmd(FOXAPI_OP_SET_CENTRE, 100.0e6), cmd(FOXAPI_OP_RUN, 0, 0)};
        CHECK_EQ(api()->submit(remote.raw(), q, 2, nullptr), 2);
        CHECK_EQ(api()->logout(e.raw(), tok), FOXAPI_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const FoxReceiverState st = admin.state();
        CHECK_EQ(st.centreHz, centre);
        CHECK((st.flags & FOXAPI_RX_RUNNING) == 0u);
    }
}

// A DETACHED SESSION'S QUEUED COMMANDS ARE NOT APPLIED. Round-3 review: the
// control thread applied a pass's commands before it dealt with the sessions
// detached or closed in that pass, and never looked again, so commands a
// session queued just before its login was revoked still landed - a remote
// PTT keyed the transmitter until the same pass opened it again. The test
// holds the control thread (mock.stall) while the remote queues a PTT and a
// retune and its token is revoked behind them.
void testDetachedSessionsQueuedCommandsAreRefused() {
    testing::Engine e(api(), "user=op;password=pw;pttHoldMs=2000;keepaliveMs=1000;remoteKeepaliveMs=2000");
    char tok[80] = {};
    CHECK_EQ(api()->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    CHECK(remote.ok());
    openTx(local);
    local.beat();
    remote.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    const double centreBefore = watcher.state().centreHz;
    const uint64_t txSeqBefore = watcher.state().txSeq;
    fire(local, {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.stall=400")});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the control thread is inside the stall
    local.beat();
    remote.beat();
    FoxCommand q[2] = {cmd(FOXAPI_OP_TX_PTT, 0, 1), cmd(FOXAPI_OP_SET_CENTRE, 100.0e6)};
    CHECK_EQ(api()->submit(remote.raw(), q, 2, nullptr), 2);  // taken: the token was still good
    CHECK_EQ(api()->logout(e.raw(), tok), FOXAPI_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // stall over, next pass done
    local.beat();
    const FoxReceiverState st = watcher.state();
    std::printf("after a revoke behind queued commands: centre %.0f (was %.0f), txSeq %llu (was %llu)\n",
                st.centreHz, centreBefore, static_cast<unsigned long long>(st.txSeq),
                static_cast<unsigned long long>(txSeqBefore));
    CHECK_EQ(st.centreHz, centreBefore);  // the retune never landed
    CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
    CHECK(!sawUnkey(watcher, ""));        // nothing was keyed, so nothing had to open
    CHECK_EQ(st.txSeq, txSeqBefore);      // the transmitter did not move at all
    CHECK_EQ(api()->heartbeat(remote.raw()), FOXAPI_DETACHED);
}

// TWO SESSIONS CANNOT FREE EACH OTHER. Round-2 review: the mark was ONE
// engine-wide slot, so when a second session's latch timed out it overwrote
// the first session's mark, and the first could latch again without ever
// sending LATCH 0. Two level-triggered interfaces re-sending LATCH 1 every
// frame kept the transmitter keyed 185 of 193 frames over 3 s. Both forms of
// the review's probe are held here: the frame-by-frame one, and the narrow
// one (A times out; B tries; A tries again with no LATCH 0).
//
// Round 4 made the mark the OPERATOR's (the principal: every LOCAL session
// is "local"), not the session's, because a session can be closed and
// reopened (round-4 review: a reconnecting window escaped its own mark). So
// two local level-triggered interfaces now get ONE latch between them, not
// one each: when A's latch times out the operator is marked, and B's
// LATCH 1 is refused as A's is. Counted in LATCHES over 4 s (round 3: keyed
// time alone no longer caught a one-slot engine).
void testTwoSessionsCannotFreeEachOther() {
    {
        testing::Engine e(api(), kFast);
        testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        openTx(a);
        int frames = 0;
        int keyedFrames = 0;
        int closesA = 0;
        int closesB = 0;
        bool was = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(4000)) {
            a.beat();
            b.beat();
            fire(a, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
            fire(b, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
            ++frames;
            const FoxReceiverState sa = a.state();
            const bool k = (sa.flags & FOXAPI_RX_TX_KEYED) != 0u;
            if (k && !was) {
                if ((sa.flags & FOXAPI_RX_TX_KEY_MINE) != 0u) {
                    ++closesA;
                } else {
                    ++closesB;
                }
            }
            was = k;
            keyedFrames += k ? 1 : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::printf("two sessions re-latching every frame for 4 s: latch closed by A %d, by B %d; "
                    "keyed %d of %d frames\n", closesA, closesB, keyedFrames, frames);
        // A's one latch (400 ms), then nothing, ever: B is the same operator.
        CHECK(frames > 100);
        CHECK_EQ(closesA, 1);
        CHECK_EQ(closesB, 0);
        CHECK(keyedFrames * 100 < frames * 15);
    }
    {
        testing::Engine e(api(), kFast);
        testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        openTx(a);
        a.beat();
        b.beat();
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        auto beatBoth = [&](int ms) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < end) {
                a.beat();
                b.beat();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        beatBoth(600);  // A's latch times out
        CHECK(!keyed(a));
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
        beatBoth(FOXAPI_LATCH_REARM_MS + 100);
        // B is the same operator: marked by A's timeout, well past the re-arm.
        const auto fromB = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        CHECK_EQ(fromB.status(), FOXAPI_DENIED);
        CHECK(std::strstr(fromB.result.message, "timed out") != nullptr);
        CHECK(!keyed(a));
        // THE REVIEW'S CASE: A never sent LATCH 0, and nothing B did (or did
        // not do) frees it.
        const auto again = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        CHECK_EQ(again.status(), FOXAPI_DENIED);
        CHECK((again.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(!keyed(a));
        // A LATCH 0 frees ONLY the session that sent it (round-5 decision):
        // B's release lets B press, and still not A - A never released.
        CHECK_EQ(b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
        const auto afterB = a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
        CHECK_EQ(afterB.status(), FOXAPI_DENIED);
        CHECK(std::strstr(afterB.result.message, "LATCH 0") != nullptr);
        CHECK(!keyed(a));
        CHECK_EQ(b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        CHECK(keyed(b));
        CHECK((b.state().flags & FOXAPI_RX_TX_KEY_MINE) != 0u);
        CHECK_EQ(b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);  // B's own: no mark
        beatBoth(FOXAPI_LATCH_REARM_MS + 100);
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);  // A still not free
        // A's own release frees A.
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
        CHECK(keyed(a));
        CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    }
}

// THE MARK BELONGS TO THE OPERATOR AND SURVIVES EVERY SESSION. Round-4
// review: the mark was cleared when its session CLOSED, and one WebSocket is
// one session (docs/TRANSPORTS.md 2.1): a crash-isolated window that stalls
// is closed with its session, reconnects by reading the token file again, and
// its frame loop's LATCH 1 found no mark on the new session - a chain of
// latches, one per reconnect (5 latches, keyed 65% of 12 s). Now the mark is
// kept per PRINCIPAL - "local" for every LOCAL session, the token for a
// remote one - and closing sessions never clears it. And the operator is
// never locked out: a fresh session that releases first may press again.
void testMarkBelongsToTheOperator() {
    testing::Engine e(api(), kFast);
    auto a = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    auto b = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(*a);
    a->beat();
    CHECK_EQ(a->run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600 + FOXAPI_LATCH_REARM_MS + 100)) {
        a->beat();
        b->beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!keyed(*a));  // timed out: the operator is marked
    // Every session closes. That clears nothing.
    a.reset();
    b.reset();
    testing::Session c(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    c.beat();
    const auto fresh = c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    CHECK_EQ(fresh.status(), FOXAPI_DENIED);
    CHECK(std::strstr(fresh.result.message, "LATCH 0") != nullptr);
    CHECK(!keyed(c));
    // Another principal cannot release it for the local operator (a remote
    // session may not send LATCH at all).
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_DENIED);
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    // NOT LOCKED OUT: the fresh session releases, then presses, and latches.
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    c.beat();
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(c));
    // The round-4 probe's lock-out case: after a STOP from a second local
    // session the operator is marked, so that session's own LATCH 1 is
    // refused too - until it releases (LATCH 0), which frees THAT session
    // and no other (round-5 decision): c, whose latch the stop ended, must
    // release for itself.
    testing::Session d(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    c.beat();
    d.beat();
    CHECK_EQ(d.run(cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    CHECK(!keyed(c));
    CHECK_EQ(d.run(cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    const auto t1 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 100)) {
        c.beat();
        d.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(d.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    CHECK(!keyed(c));
    CHECK_EQ(d.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    c.beat();
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);  // d's release freed d only
    CHECK(!keyed(c));
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    c.beat();
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(c));
    // d releases c's ACTIVE latch: released from another window, so the
    // operator is marked - past the re-arm time c is still refused - and the
    // message says how it ended.
    d.beat();
    CHECK_EQ(d.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    CHECK(!keyed(c));
    const auto t2 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t2 < std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 100)) {
        c.beat();
        d.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto sameOperator = c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    CHECK_EQ(sameOperator.status(), FOXAPI_DENIED);
    CHECK(std::strstr(sameOperator.result.message, "released from another interface") != nullptr);
    CHECK(std::strstr(sameOperator.result.message, "LATCH 0") != nullptr);
    // ...and release-then-press still works, in the session that releases:
    // c's own LATCH 0 (nothing active) exempts c.
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    c.beat();
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(c));
    CHECK_EQ(c.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    // The mark still stands for everyone else: a window opened NOW (a
    // reconnect) starts refused, and must release for itself.
    testing::Session late(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    beatFor(late, FOXAPI_LATCH_REARM_MS + 100);
    CHECK_EQ(late.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(late.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(late.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(late.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status(), FOXAPI_OK);
}

// RELEASE IN ANOTHER WINDOW KEEPS A STUCK WINDOW'S KEY OPEN - HOWEVER THE
// OPERATOR PRESSES IT, AND WHILE THEY LATCH DELIBERATELY IN THE OTHER WINDOW.
// Window A is stuck re-sending LATCH 1 every frame, on its own thread, from
// the first frame to the last: it is never stopped. The operator presses
// RELEASE in window B:
//   - once, while A holds the latch (32db548: A re-latched 1 s later);
//   - twice, 300 ms apart (round-5 review, r1b: the second press found
//     nothing latched, cleared the operator's mark, and A re-keyed 1 s after
//     the first press);
//   - once, AFTER A's latch has timed out (round-5 review, r1a: nothing was
//     latched, so the press cleared the mark and A re-keyed 30 ms later);
//   - every frame from 300 ms, a level-triggered window with its latch key
//     up (round-5 review, r1c).
// A LATCH 0 now EXEMPTS ONLY THE SESSION THAT SENT IT from its principal's
// mark, so none of these frees A: A keys once, at the start, and never again
// - watched for 5 s after the last press. Then, with A still sending
// LATCH 1 every frame, the operator latches deliberately in B (LATCH 0, then
// LATCH 1): B keys and holds the key, B's own LATCH 0 opens it, and A still
// never keys (1.5 s past the re-arm time). A's LATCH 1 results: exactly one
// OK in the whole run.
enum ReleaseHow { kReleaseOnce, kReleaseTwice, kReleaseAfterTimeout, kReleaseEveryFrame, kReleaseCount };

struct ReleaseSeen {
    bool keyedBefore = false;       // control: A did latch
    int closes = 0;                 // the key closing, up to the deliberate latch: A's one latch
    long long firstRekey = -1;      // ms after the first press that the key closed again
    int32_t press1 = 999;
    int32_t press2 = 999;
    int32_t deliberateRelease = 999;
    int32_t deliberateLatch = 999;
    bool bHeld = false;             // B's key, keyed, for 300 ms with A still sending LATCH 1
    int32_t ownRelease = 999;
    bool openAfterOwn = false;
    int closesAfterOwn = 0;         // must stay 0: A is still not free
    int aOk = 0;                    // A's LATCH 1 results that keyed
    int aDenied = 0;
    std::string aLastDenied;
};

ReleaseSeen releaseElsewhere(ReleaseHow how) {
    const std::string opts = std::string("pttHoldMs=200;keepaliveMs=150;token=t;latchTimeoutMs=") +
                             (how == kReleaseAfterTimeout ? "1500" : "20000");
    testing::Engine e(api(), opts.c_str());
    testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    b.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    ReleaseSeen s;
    std::atomic<bool> stop{false};
    std::thread stuck([&] {  // window A: LATCH 1 every frame, never stopped
        while (!stop.load()) {
            a.beat();
            FoxCommand c = cmd(FOXAPI_OP_TX_LATCH, 0, 1);
            api()->submit(a.raw(), &c, 1, nullptr);
            FoxCommandResult rr[64];
            for (auto& x : rr) x.structSize = sizeof(x);
            int32_t n = 0;
            while ((n = api()->poll_results(a.raw(), rr, 64)) > 0) {
                for (int32_t i = 0; i < n; ++i) {
                    s.aOk += rr[i].status == FOXAPI_OK ? 1 : 0;
                    if (rr[i].status == FOXAPI_DENIED) {
                        ++s.aDenied;
                        s.aLastDenied = rr[i].message;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    bool was = false;
    int* counter = &s.closes;
    long long firstPress = -1;
    long long lastPress = -1;
    long long offAt = -1;
    bool level = false;
    auto look = [&] {
        b.beat();
        if (level) {
            fire(b, {cmd(FOXAPI_OP_TX_LATCH, 0, 0)});
        }
        const bool k = keyed(b);
        if (k && !was) {
            ++*counter;
            if (firstPress >= 0 && s.firstRekey < 0 && counter == &s.closes) {
                s.firstRekey = ms() - firstPress;
            }
        }
        if (k) {
            s.keyedBefore = true;
        }
        if (!k && was && offAt < 0) {
            offAt = ms();
        }
        was = k;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto press = [&] {
        b.beat();
        const int32_t st = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status();
        lastPress = ms();
        if (firstPress < 0) {
            firstPress = lastPress;
        }
        return st;
    };
    if (how == kReleaseAfterTimeout) {
        while (ms() < 4000 && (offAt < 0 || ms() < offAt + 1200)) look();
        s.press1 = press();
    } else {
        while (ms() < 300) look();
        s.press1 = press();
        if (how == kReleaseTwice) {
            while (ms() < lastPress + 300) look();
            s.press2 = press();
        }
        level = how == kReleaseEveryFrame;
    }
    while (ms() < lastPress + 5000) look();
    level = false;
    // The operator latches deliberately in B, while A goes on.
    b.beat();
    s.deliberateRelease = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status();
    b.beat();
    s.deliberateLatch = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status();
    was = keyed(b);
    s.bHeld = true;
    const long long held = ms();
    while (ms() < held + 300) {
        b.beat();
        const FoxReceiverState st = b.state();
        s.bHeld = s.bHeld && (st.flags & FOXAPI_RX_TX_KEYED) != 0u && (st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    b.beat();
    s.ownRelease = b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).status();
    s.openAfterOwn = !keyed(b);
    was = !s.openAfterOwn;
    counter = &s.closesAfterOwn;
    const long long after = ms();
    while (ms() < after + FOXAPI_LATCH_REARM_MS + 1500) look();
    stop.store(true);
    stuck.join();
    return s;
}

void testReleaseInAnotherWindowKeepsTheKeyOpen() {
    static const char* const kNames[kReleaseCount] = {
        "RELEASE once in window B while A holds the latch",
        "RELEASE twice in window B, 300 ms apart",
        "RELEASE once in window B after A's latch timed out",
        "LATCH 0 every frame from window B (latch key up)"};
    std::vector<ReleaseSeen> out(kReleaseCount);
    std::vector<std::thread> threads;
    for (int i = 0; i < kReleaseCount; ++i) {
        threads.emplace_back([&out, i] { out[i] = releaseElsewhere(static_cast<ReleaseHow>(i)); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int i = 0; i < kReleaseCount; ++i) {
        const ReleaseSeen& s = out[i];
        std::printf("stuck window A, %s: key closed %d time(s) (again %lld ms after the first press, -1 = never "
                    "in 5 s); B's deliberate LATCH 0 -> %d, LATCH 1 -> %d, held %d, own LATCH 0 -> %d, open %d; "
                    "closed %d time(s) after; A's LATCH 1 results: %d OK, %d DENIED (\"%s\")\n",
                    kNames[i], s.closes, s.firstRekey, s.deliberateRelease, s.deliberateLatch, s.bHeld ? 1 : 0,
                    s.ownRelease, s.openAfterOwn ? 1 : 0, s.closesAfterOwn, s.aOk, s.aDenied,
                    s.aLastDenied.c_str());
        CHECK(s.keyedBefore);
        CHECK_EQ(s.closes, 1);          // A's one latch, and never again after any press
        CHECK_EQ(s.firstRekey, -1LL);
        CHECK_EQ(s.press1, FOXAPI_OK);
        if (i == kReleaseTwice) {
            CHECK_EQ(s.press2, FOXAPI_OK);
        }
        // Taken - new, or merged into the window's own outstanding LATCH 0
        // when it had been sending one every frame.
        CHECK(s.deliberateRelease == FOXAPI_OK || (i == kReleaseEveryFrame && s.deliberateRelease == FOXAPI_NO_CHANGE));
        CHECK_EQ(s.deliberateLatch, FOXAPI_OK);  // B's own release exempted B: its press keys
        CHECK(s.bHeld);
        CHECK_EQ(s.ownRelease, FOXAPI_OK);
        CHECK(s.openAfterOwn);
        CHECK_EQ(s.closesAfterOwn, 0);  // B's release did not exempt A
        CHECK_EQ(s.aOk, 1);
        CHECK(s.aDenied > 0);
        CHECK(s.aLastDenied.find("LATCH 0") != std::string::npos);
    }
}

// The round-4 probe's two reconnect forms, each in its own engine, in
// parallel. Nothing here may call CHECK (the harness counts are not atomic):
// each returns what it saw and the caller checks.
struct ChainSeen {
    int closes = 0;
    int frames = 0;
    int keyedFrames = 0;
    int sessions = 1;
};

// A crash-isolated window re-sending LATCH 1 every frame that stalls for
// 500 ms every 2.5 s; the transport closes it (and its session) for not
// reading, and it reconnects with a NEW session each time. 12 s, 3 s latch.
ChainSeen reconnectingChain() {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=3000;keepaliveMs=150;remoteKeepaliveMs=300;token=t");
    auto ui = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session obs(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    ui->run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    ChainSeen seen;
    bool was = false;
    long long nextStall = 2500;
    auto look = [&] {
        const bool k = keyed(obs);
        seen.closes += (k && !was) ? 1 : 0;
        was = k;
        ++seen.frames;
        seen.keyedFrames += k ? 1 : 0;
    };
    while (ms() < 12000) {
        if (ms() >= nextStall) {
            const long long stallEnd = ms() + 500;
            while (ms() < stallEnd) {  // stalled: no beat, no read, no submit
                obs.beat();
                look();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ui->close();  // closed by the transport, with its session
            ui = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            ++seen.sessions;  // ...and reconnected: a new session
            nextStall += 2500;
            continue;
        }
        ui->beat();
        obs.beat();
        fire(*ui, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
        look();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return seen;
}

// The operator presses STOP (and starts again) while a level-triggered
// window is latched; the window then reconnects with a new session and goes
// on sending LATCH 1 every frame. Returns when (ms after the stop) the key
// closed again, or -1 when it never did; `keyedBefore` is the control.
long long reopenAfterStop(bool& keyedBefore) {
    testing::Engine e(api(), "pttHoldMs=200;latchTimeoutMs=10000;keepaliveMs=150;token=t");
    testing::Session op(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    auto ui = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    op.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"));
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    long long stopAt = -1;
    long long rekeyAt = -1;
    bool reopened = false;
    bool was = false;
    keyedBefore = false;
    while (ms() < 3800) {
        if (stopAt < 0 && ms() > 300) {
            op.run(cmd(FOXAPI_OP_RUN, 0, 0));
            op.run(cmd(FOXAPI_OP_RUN, 0, 1));
            stopAt = ms();
        }
        if (stopAt >= 0 && !reopened && ms() > stopAt + 200) {
            reopened = true;
            ui = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        }
        ui->beat();
        op.beat();
        fire(*ui, {cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
        const bool k = keyed(op);
        keyedBefore = keyedBefore || (stopAt < 0 && k);
        if (stopAt >= 0 && k && !was && rekeyAt < 0) {
            rekeyAt = ms() - stopAt;
        }
        was = k;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return rekeyAt;
}

void testReconnectingDoesNotEscapeTheMark() {
    ChainSeen chain;
    long long rekey = -2;
    bool keyedBefore = false;
    std::thread t1([&] { chain = reconnectingChain(); });
    std::thread t2([&] { rekey = reopenAfterStop(keyedBefore); });
    t1.join();
    t2.join();
    std::printf("reconnecting window, 12 s of LATCH 1 every frame, 500 ms stall + new session every 2.5 s: "
                "%d session(s), latch closed %d time(s), keyed %d of %d frames\n",
                chain.sessions, chain.closes, chain.keyedFrames, chain.frames);
    std::printf("window reconnected after STOP: keyed before %d; closed again %lld ms after the stop (-1 = never)\n",
                keyedBefore ? 1 : 0, rekey);
    CHECK(chain.sessions >= 5);  // the probe did reconnect, again and again
    CHECK_EQ(chain.closes, 1);   // ONE latch across every reconnect (review: 5)
    CHECK(chain.keyedFrames * 100 < chain.frames * 35);  // the first 2.5 s at most (review: 65%)
    CHECK(keyedBefore);
    CHECK_EQ(rekey, -1LL);
}

// TOGGLING CANNOT HOLD THE LATCH. Round-2 review: LATCH 0 + LATCH 1 in one
// batch every frame re-closed the latch within one control pass, so the key
// never opened (1,494 ms continuously keyed against a 400 ms timeout). After
// ANY latch ends - released, timed out, or opened by a stop, a fault, a
// transmitter change or a lost keep-alive - no latch closes again, from any
// session, for FOXAPI_LATCH_REARM_MS: the key is OFF for at least that long.
void testToggleCannotHoldTheLatch() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session other(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(s);
    long long longest = 0;
    long long runStart = -1;
    int keyedFrames = 0;
    int frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    while (ms() < 1500) {
        s.beat();
        other.beat();
        fire(s, {cmd(FOXAPI_OP_TX_LATCH, 0, 0), cmd(FOXAPI_OP_TX_LATCH, 0, 1)});
        const bool k = keyed(s);
        ++frames;
        keyedFrames += k ? 1 : 0;
        if (k && runStart < 0) runStart = ms();
        if (!k && runStart >= 0) {
            longest = std::max(longest, ms() - runStart);
            runStart = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (runStart >= 0) longest = std::max(longest, ms() - runStart);
    std::printf("LATCH 0 + LATCH 1 every frame for 1.5 s: keyed %d of %d frames, longest run %lld ms\n",
                keyedFrames, frames, longest);
    CHECK(longest < 100);                    // never held: each close is opened by the next toggle
    CHECK(keyedFrames * 100 < frames * 10);  // and re-closing waits out the re-arm time
    // The re-arm time is engine-wide: another session cannot close it either.
    fire(s, {cmd(FOXAPI_OP_TX_LATCH, 0, 0)});
    s.beat();
    const auto rearmed = other.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    CHECK_EQ(rearmed.status(), FOXAPI_DENIED);
    // The refusal says how long the re-arm time is (from the constant, not a
    // hard-coded "second") and why the last latch ended.
    const std::string msg = rearmed.result.message;
    std::printf("re-arm refusal: \"%s\"\n", msg.c_str());
    CHECK(msg.find(std::to_string(FOXAPI_LATCH_REARM_MS) + " ms") != std::string::npos);
    CHECK(msg.find("released by its owner") != std::string::npos);
    CHECK(rearmed.result.applied[0] > 0.0 && rearmed.result.applied[0] <= FOXAPI_LATCH_REARM_MS);
    // ...and it ends.
    const auto t1 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(FOXAPI_LATCH_REARM_MS + 100)) {
        s.beat();
        other.beat();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_EQ(other.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(other));
}

// THE KEY NEVER SURVIVES A STOP (foxsdr_api.h, "THE KEY"). RUN 0 opens it,
// with a reason, whether it was latched or held.
void testStopUnkeys() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    api()->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(s));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    const FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_RUNNING) == 0u);
    CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
    CHECK((st.flags & FOXAPI_RX_TX_LATCHED) == 0u);
    CHECK(std::strstr(st.txUnkeyReason, "stopped") != nullptr);
    CHECK(sawUnkey(s, "stopped"));
    // Another session inside the re-arm time is told the real cause: the
    // stop, not "the latch was released".
    testing::Session other(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    other.beat();
    const auto early = other.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1));
    CHECK_EQ(early.status(), FOXAPI_DENIED);
    CHECK(std::strstr(early.result.message, "stopped") != nullptr);
    CHECK(std::strstr(early.result.message, (std::to_string(FOXAPI_LATCH_REARM_MS) + " ms").c_str()) !=
          nullptr);
    // A held PTT is opened by a stop just the same.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(s));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    CHECK(!keyed(s));
}

// A REMOTE SESSION HAS THE WEB REMOTE'S POWER AND NO MORE. The app's web
// remote can send only transmitPtt (web_control.cpp), and only while the
// local operator has the Transmit page open (app_window.cpp releases it
// otherwise). The review's lone remote session opened the transmitter, set
// 5 GHz and 0 dB attenuation and keyed it. Now: every transmitter op but PTT
// is refused to a remote session whatever its grants, and PTT needs the
// LOCAL operator's consent (TX_REMOTE_ARM from a local session), which
// lapses when that session disarms, closes or stops answering.
void testRemoteIsPttOnlyWithLocalConsent() {
    testing::Engine e(api(), kFast);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    CHECK(remote.ok());
    remote.beat();
    // The review's sequence, from a lone remote session.
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy")).status(), FOXAPI_DENIED);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_SET_FREQUENCY, 5.0e9)).status(), FOXAPI_DENIED);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_SET_POWER, 0.0)).status(), FOXAPI_DENIED);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_SET_MODE, 0, FOXAPI_TX_MODE_AM)).status(), FOXAPI_DENIED);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status() == FOXAPI_OK, false);
    CHECK(!keyed(remote));
    // A remote session cannot give itself consent either.
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_DENIED);

    // The local operator opens the transmitter but has NOT consented: the
    // remote PTT is still refused.
    auto local = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(*local);
    local->beat();
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_DENIED);
    CHECK(!keyed(remote));
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_CLOSE)).status(), FOXAPI_DENIED);
    CHECK((remote.state().flags & FOXAPI_RX_TX_AVAILABLE) != 0u);

    // Consent: the remote PTT works, as a hold.
    api()->subscribe(remote.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    CHECK_EQ(local->run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    CHECK((remote.state().flags & FOXAPI_RX_TX_REMOTE_ARMED) != 0u);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(remote));
    // ...but still nothing else.
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_SET_POWER, 0.0)).status(), FOXAPI_DENIED);
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).status(), FOXAPI_DENIED);

    // Withdrawing consent opens a remote key at once.
    local->beat();
    remote.beat();
    CHECK_EQ(local->run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 0)).status(), FOXAPI_OK);
    CHECK(!keyed(remote));
    CHECK(sawUnkey(remote, "consent"));
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_DENIED);

    // The consent lives with the local session that gave it: when that
    // session stops answering, consent lapses and the remote key opens.
    local->beat();
    CHECK_EQ(local->run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(remote));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(check::waitFor([&] {
        remote.beat();  // the remote keeps answering and keeps asserting...
        remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1), 50);
        return !keyed(remote);  // ...but the local operator has gone quiet
    }, 1000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("remote key opened %lld ms after the consenting local session went quiet\n",
                static_cast<long long>(ms));
    CHECK(ms < 400);  // local keep-alive 150 ms + slack
    CHECK((remote.state().flags & FOXAPI_RX_TX_REMOTE_ARMED) == 0u);

    // And when it closes.
    local->beat();
    CHECK_EQ(local->run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(remote));
    local.reset();
    CHECK(check::waitFor([&] { return !keyed(remote); }, 300));
    CHECK((remote.state().flags & FOXAPI_RX_TX_REMOTE_ARMED) == 0u);
}

// A REMOTE SESSION'S KEEP-ALIVE IS THE WEB REMOTE'S HOLD, NOT THE LOCAL
// FRAME'S. The app's remote PTT hold is 2 s (Transmitter::kRemotePttHoldMs),
// against a local dead-man of 1 s, because a network link jitters; here a
// remote key survives a silence the LOCAL keep-alive would not, and still
// opens when the REMOTE keep-alive runs out. (PTT re-assertions are not
// heartbeats: liveness is the interface's own beat, never the transport's.)
void testRemoteKeepAlive() {
    testing::Engine e(api(), "pttHoldMs=2000;latchTimeoutMs=400;keepaliveMs=150;"
                             "remoteKeepaliveMs=400;token=t");
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    openTx(local);
    local.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    const auto t0 = std::chrono::steady_clock::now();
    long long unkeyedAt = -1;
    bool keyedAt250 = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1200)) {
        local.beat();  // the local operator is present throughout
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const bool k = keyed(local);
        if (ms >= 240 && ms <= 260) {
            keyedAt250 = keyedAt250 || k;
        }
        if (!k) {
            unkeyedAt = ms;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("remote key with no remote heartbeat opened at %lld ms (remote keep-alive 400)\n",
                unkeyedAt);
    CHECK(keyedAt250);                            // past the LOCAL keep-alive, still keyed
    CHECK(unkeyedAt >= 380 && unkeyedAt < 650);   // the REMOTE keep-alive opened it
}

void testKeepAliveLoss() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK(keyed(watcher));
    // The interface's frame loop wedges: no more heartbeats. The latch alone
    // would hold for 400 ms; the keep-alive must win at 150.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(check::waitFor([&] { return !keyed(watcher); }, 1000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("keep-alive loss unkeyed after %lld ms\n", static_cast<long long>(ms));
    CHECK(ms < 390);
    CHECK(sawUnkey(watcher, "stopped answering"));
}

void testSessionCloseReleases() {
    testing::Engine e(api(), kFast);
    testing::Session watcher(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    api()->subscribe(watcher.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_TX_UNKEYED));
    auto s = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(*s);
    s->beat();
    CHECK_EQ(s->run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK(keyed(watcher));
    const auto t0 = std::chrono::steady_clock::now();
    s.reset();  // close_session
    CHECK(check::waitFor([&] { return !keyed(watcher); }, 1000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("session close unkeyed after %lld ms\n", static_cast<long long>(ms));
    CHECK(ms < 100);  // woken at once, not left to the keep-alive
    CHECK(sawUnkey(watcher, "closed its session"));
}

void testOneKeyholder() {
    testing::Engine e(api(), kFast);
    testing::Session a(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session b(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(a);
    a.beat();
    b.beat();
    CHECK_EQ(a.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK((b.state().flags & FOXAPI_RX_TX_KEY_MINE) == 0u);
    // B cannot take over a key A holds...
    CHECK_EQ(b.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).result.status, FOXAPI_BUSY);
    // ...but anyone with the grant may OPEN it: that is never the unsafe way.
    CHECK_EQ(b.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).result.status, FOXAPI_OK);
    CHECK(!keyed(a));
}

void testFaultAndCloseUnkey() {
    testing::Engine e(api(), kFast);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_CLOSE)).result.status, FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
    CHECK((st.flags & FOXAPI_RX_TX_AVAILABLE) == 0u);

    openTx(s);
    s.beat();
    // Closing the transmitter ended the latch, so the re-arm time applies
    // to a new one as it does after any other end...
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_DENIED);
    beatFor(s, FOXAPI_LATCH_REARM_MS + 100);
    // ...and, since it was not s's own LATCH 0 that ended it, s is marked:
    // past the re-arm time it still cannot latch until it releases. (Round 3
    // changed this expectation: before, only a timeout marked the owner.)
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_DENIED);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0)).result.status, FOXAPI_OK);
    beatFor(s, FOXAPI_LATCH_REARM_MS + 100);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.fault=radio unplugged")).result.status,
             FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_KEYED) == 0u);
    CHECK((st.flags & FOXAPI_RX_FAULTED) != 0u);
    CHECK((st.flags & FOXAPI_RX_RUNNING) == 0u);
    CHECK_EQ(std::string(st.faultMessage), std::string("radio unplugged"));
    // Starting again clears the fault.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    CHECK((s.state().flags & FOXAPI_RX_FAULTED) == 0u);
}

void testTimersCannotBeLengthened() {
    testing::Engine e(api(), "pttHoldMs=999999;latchTimeoutMs=99999999;keepaliveMs=999999");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    openTx(s);
    s.beat();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).result.status, FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK(st.txHoldRemainingMs > 0 && st.txHoldRemainingMs <= FOXAPI_PTT_HOLD_MS);
    s.run(cmd(FOXAPI_OP_TX_PTT, 0, 0));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK(st.txLatchRemainingMs > 0 && st.txLatchRemainingMs <= FOXAPI_LATCH_TIMEOUT_MS);
    s.run(cmd(FOXAPI_OP_TX_LATCH, 0, 0));
}

// The remote keep-alive cannot be lengthened either: a remote key asserted
// over and over with no heartbeat opens within FOXAPI_REMOTE_KEEPALIVE_MS
// even when the option asks for far longer.
// A REMOTE KEY DIES WITH ITS LOGIN TOKEN, even while its interface keeps
// beating and asserting: an expired token detaches the session, and a
// detached session holds nothing.
void testKeyDiesWithTheToken() {
    testing::Engine e(api(), "user=op;password=pw;tokenTtlMs=400;pttHoldMs=2000");
    char tok[80] = {};
    CHECK_EQ(api()->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    openTx(local);
    local.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    CHECK(keyed(local));
    const auto t0 = std::chrono::steady_clock::now();
    const bool opened = check::waitFor([&] {
        local.beat();
        remote.beat();
        fire(remote, {cmd(FOXAPI_OP_TX_PTT, 0, 1)});
        return !keyed(local);
    }, 1500);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("remote key opened %lld ms after keying, with a 400 ms token\n",
                static_cast<long long>(ms));
    CHECK(opened);
    CHECK(ms < 700);
    CHECK_EQ(api()->heartbeat(remote.raw()), FOXAPI_DETACHED);
}

void testRemoteKeepAliveCannotBeLengthened() {
    testing::Engine e(api(), "remoteKeepaliveMs=999999;token=t");
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    openTx(local);
    local.beat();
    CHECK_EQ(local.run(cmd(FOXAPI_OP_TX_REMOTE_ARM, 0, 1)).status(), FOXAPI_OK);
    remote.beat();
    CHECK_EQ(remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1)).status(), FOXAPI_OK);
    const auto t0 = std::chrono::steady_clock::now();
    const bool opened = check::waitFor([&] {
        local.beat();
        remote.run(cmd(FOXAPI_OP_TX_PTT, 0, 1), 50);  // asserting, but never beating
        return !keyed(local);
    }, FOXAPI_REMOTE_KEEPALIVE_MS + 800);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("remote keep-alive asked for 999999 ms opened the key after %lld ms\n",
                static_cast<long long>(ms));
    CHECK(opened);
    CHECK(ms >= FOXAPI_REMOTE_KEEPALIVE_MS - 50);
}

}  // namespace

int main() {
    testNoTransmitterNoKey();
    testPttIsAHold();
    testLatchRules();
    testRelatchDoesNotExtend();
    testTwoSessionsCannotFreeEachOther();
    testMarkBelongsToTheOperator();
    testReleaseInAnotherWindowKeepsTheKeyOpen();
    testReconnectingDoesNotEscapeTheMark();
    testEveryLatchEndMarksItsOwner();
    testStallsDoNotChainLatches();
    testSaferCommandsAreNeverBusy();
    testSaferCommandsDoNotPileUpInTheQueue();
    testMergingKeepsSafetyInOrder();
    testDetachedSessionsQueuedCommandsAreRefused();
    testGoneSessionsApplyOnlySaferCommands();
    testResultsFollowTheirState();
    testLatchReleaseIsIdempotent();
    testSaferCommandsAreIdempotent();
    testRefusedSaferCommandsAreNotMerged();
    testMergedResultsKeepTicketOrder();
    testTicketBooksUnderMerging();
    testGoneWindowsReleaseStillReleases();
    testDetachedKeyHolderIsReleasedFirst();
    testMergedSendsKeepTheirPlace();
    testLatchKeyProtocol();
    testRefusedSaferCommandCanBeBusy();
    testStallAfterPassHolds();
    testToggleCannotHoldTheLatch();
    testStopUnkeys();
    testRemoteIsPttOnlyWithLocalConsent();
    testRemoteKeepAlive();
    testKeepAliveLoss();
    testSessionCloseReleases();
    testOneKeyholder();
    testFaultAndCloseUnkey();
    testTimersCannotBeLengthened();
    testRemoteKeepAliveCannotBeLengthened();
    testKeyDiesWithTheToken();
    return check::finish("test_transmit_safety");
}
