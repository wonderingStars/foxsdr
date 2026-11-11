// Tests for the control arbiter (net/web_arbiter.hpp). Time is a counter the test owns: no sleeps.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net/web_arbiter.hpp"
#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#define CASCADE_SOURCE_DIR "."
#endif

using namespace cascade::net;

namespace {

ArbiterConfig cfg() {
    ArbiterConfig c;
    c.idSeed = 42;
    return c;
}

std::string join(ControlArbiter& a, bool admin, bool tune, std::int64_t now) {
    std::string id;
    CHECK(a.join(admin, tune, now, id) == ArbResult::Ok);
    return id;
}

void testOpenModeAndIds() {
    ControlArbiter a(cfg());
    const std::string owner = join(a, true, true, 0);
    const std::string guest = join(a, false, true, 0);
    const std::string listener = join(a, false, false, 0);
    CHECK(owner.size() == 12 && guest.size() == 12 && owner != guest && guest != listener);
    CHECK(a.mode(0) == ControlMode::Open);
    // OPEN: every viewer that may tune may act; a listener may not
    CHECK(a.mayAct(owner, 1).allowed);
    CHECK(a.mayAct(guest, 1).allowed && !a.mayAct(guest, 1).viaOverride);
    const Decision d = a.mayAct(listener, 1);
    CHECK(!d.allowed && d.why == ArbResult::ListenerOnly);
    const Decision u = a.mayAct("nobody0000x", 1);
    CHECK(!u.allowed && u.why == ArbResult::Unknown);
    // nothing to request in OPEN mode
    CHECK(a.requestControl(guest, 2) == ArbResult::NotExclusive);
    CHECK(a.holder(2).empty());
    // an admin always may tune, whatever the flag said at join
    const std::string adminNoTune = join(a, true, false, 3);
    CHECK(a.mayAct(adminNoTune, 3).allowed);
    CHECK(a.viewerCount(3) == 4);
}

void testIdsAreOpaque() {
    ControlArbiter a(cfg());
    std::set<std::string> ids;
    for (int i = 0; i < 32; ++i) { ids.insert(join(a, false, true, i)); }
    CHECK(ids.size() == 32);
    for (const std::string& id : ids) {
        CHECK(id.size() == 12);
        CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
    // the listing carries ids and flags only; a viewer listing has no other string in it
    for (const ViewerInfo& v : a.viewers(40)) { CHECK(ids.count(v.id) == 1); }
    // two arbiters with different seeds give different ids; the same seed repeats (a test aid, not a promise to clients)
    ArbiterConfig c1 = cfg(), c2 = cfg();
    c2.idSeed = 43;
    ControlArbiter x(c1), y(c2), z(c1);
    std::string ix, iy, iz;
    x.join(false, true, 0, ix);
    y.join(false, true, 0, iy);
    z.join(false, true, 0, iz);
    CHECK(ix != iy && ix == iz);
    // an unseeded arbiter works and does not repeat across instances (random device)
    ControlArbiter r1, r2;
    std::string a1, a2;
    r1.join(false, true, 0, a1);
    r2.join(false, true, 0, a2);
    CHECK(a1 != a2);
}

void testExclusiveRequestGrantQueueRelease() {
    ControlArbiter a(cfg());
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    const std::string g3 = join(a, false, true, 0);
    CHECK(a.setMode(owner, ControlMode::Exclusive, 10) == ArbResult::Ok);
    // nobody holds it: exclusive means nobody but the admin (override) may act
    CHECK(!a.mayAct(g1, 11).allowed);
    CHECK(a.mayAct(owner, 11).allowed && a.mayAct(owner, 11).viaOverride);
    CHECK(a.requestControl(g1, 12) == ArbResult::Ok);                 // free: granted at once
    CHECK(a.holder(12) == g1);
    CHECK(a.mayAct(g1, 13).allowed && !a.mayAct(g1, 13).viaOverride);
    CHECK(!a.mayAct(g2, 13).allowed && a.mayAct(g2, 13).why == ArbResult::Denied);
    CHECK(a.requestControl(g1, 14) == ArbResult::AlreadyHolder);
    CHECK(a.requestControl(g2, 15) == ArbResult::Queued);
    CHECK(a.requestControl(g3, 16) == ArbResult::Queued);
    CHECK(a.requestControl(g2, 17) == ArbResult::Queued);              // asking twice does not queue twice
    CHECK((a.queue(17) == std::vector<std::string>{g2, g3}));
    for (const ViewerInfo& v : a.viewers(17)) {
        if (v.id == g2) { CHECK(v.queuePosition == 1); }
        if (v.id == g3) { CHECK(v.queuePosition == 2); }
        if (v.id == g1) { CHECK(v.holder && v.queuePosition == 0); }
    }
    // a non-holder cannot release
    CHECK(a.releaseControl(g2, 18) == ArbResult::NotHolder);
    CHECK(a.holder(18) == g1);
    // the holder releases: first in line is promoted
    CHECK(a.releaseControl(g1, 19) == ArbResult::Ok);
    CHECK(a.holder(19) == g2);
    CHECK((a.queue(19) == std::vector<std::string>{g3}));
    CHECK(!a.mayAct(g1, 20).allowed);
    // a queued viewer can withdraw
    CHECK(a.cancelRequest(g3, 21) == ArbResult::Ok);
    CHECK(a.queue(21).empty());
    CHECK(a.cancelRequest(g3, 22) == ArbResult::NotHolder);
    // releasing with an empty queue leaves nobody holding
    CHECK(a.releaseControl(g2, 23) == ArbResult::Ok);
    CHECK(a.holder(23).empty());
    CHECK(a.requestControl(g3, 24) == ArbResult::Ok);
}

void testAdminOverrideHandOverRevoke() {
    ControlArbiter a(cfg());
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    const std::string lis = join(a, false, false, 0);
    // none of these work in OPEN mode
    CHECK(a.grant(owner, g1, 1) == ArbResult::NotExclusive);
    CHECK(a.revoke(owner, 1) == ArbResult::NotExclusive);
    // only an admin may switch modes
    CHECK(a.setMode(g1, ControlMode::Exclusive, 2) == ArbResult::NotAdmin);
    CHECK(a.mode(2) == ControlMode::Open);
    CHECK(a.setMode("zzzzzzzzzzzz", ControlMode::Exclusive, 2) == ArbResult::Unknown);
    CHECK(a.setMode(owner, ControlMode::Exclusive, 3) == ArbResult::Ok);
    // hand-over: the admin gives control to a named viewer
    CHECK(a.grant(owner, g1, 4) == ArbResult::Ok);
    CHECK(a.holder(4) == g1);
    CHECK(a.grant(owner, g1, 5) == ArbResult::AlreadyHolder);
    CHECK(a.grant(owner, lis, 5) == ArbResult::ListenerOnly);         // a listener can never be handed control
    CHECK(a.grant(owner, "zzzzzzzzzzzz", 5) == ArbResult::Unknown);
    CHECK(a.grant(g2, g2, 5) == ArbResult::NotAdmin);                  // a guest cannot grant itself
    CHECK(a.holder(5) == g1);
    // handing to g2 takes it from g1, and removes g2 from the queue if it was waiting
    CHECK(a.requestControl(g2, 6) == ArbResult::Queued);
    CHECK(a.grant(owner, g2, 7) == ArbResult::Ok);
    CHECK(a.holder(7) == g2 && a.queue(7).empty());
    CHECK(!a.mayAct(g1, 8).allowed);
    // the admin acts regardless, and its action does not refresh the holder's idle clock
    const Decision d = a.mayAct(owner, 8);
    CHECK(d.allowed && d.viaOverride);
    // revoke: the holder loses control and the next in line takes over
    CHECK(a.requestControl(g1, 9) == ArbResult::Queued);
    CHECK(a.revoke(owner, 10) == ArbResult::Ok);
    CHECK(a.holder(10) == g1 && a.queue(10).empty());
    CHECK(a.revoke(g1, 11) == ArbResult::NotAdmin);
    CHECK(a.revoke(owner, 11) == ArbResult::Ok);                       // nobody is waiting now: nobody holds afterwards
    CHECK(a.holder(11).empty());
    CHECK(a.revoke(owner, 12) == ArbResult::NotHolder);                // nothing to revoke
    CHECK(!a.mayAct(g2, 12).allowed);
    // back to OPEN: holder and queue cleared, everyone who may tune acts again
    CHECK(a.setMode(owner, ControlMode::Open, 12) == ArbResult::Ok);
    CHECK(a.holder(12).empty() && a.queue(12).empty());
    CHECK(a.mayAct(g1, 13).allowed && a.mayAct(g2, 13).allowed);
}

void testIdleAndTimeouts() {
    ArbiterConfig c = cfg();
    c.holderIdleMs = 1000;
    c.viewerTimeoutMs = 5000;
    ControlArbiter a(c);
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    a.setMode(owner, ControlMode::Exclusive, 0);
    CHECK(a.requestControl(g1, 0) == ArbResult::Ok);
    CHECK(a.requestControl(g2, 0) == ArbResult::Queued);
    // acting refreshes the holder's idle clock
    CHECK(a.mayAct(g1, 900).allowed);
    CHECK(a.holder(1800) == g1);                                       // 900 ms since the last action: still held
    CHECK(a.mayAct(g1, 1800, /*touch=*/false).allowed);                // a look that does not count as activity
    CHECK(a.holder(1899) == g1);
    // 1000 ms since the last real action (900): at 1900 control moves on to g2
    CHECK(a.holder(1900) == g2);
    CHECK(a.queue(1900).empty());
    CHECK(!a.mayAct(g1, 1901).allowed);
    // g1 is still connected (heartbeats), but a viewer that stops heartbeating is dropped from everything
    a.heartbeat(g1, 2000);
    a.heartbeat(owner, 2000);
    a.heartbeat(g2, 4999);                                             // keeps g2 (the holder) alive, but it never acts
    CHECK(a.viewerCount(4999) == 3);
    // g2 idle too long as a holder: nobody holds
    CHECK(a.holder(4999).empty());
    // g1 and owner last seen at 2000: timeout 5000 -> gone at 7000
    CHECK(a.viewerCount(6999) == 3);
    CHECK(a.viewerCount(7000) == 1);
    CHECK(a.heartbeat(g1, 7000) == ArbResult::Unknown);                // g1 is gone; heartbeats do not revive it
    CHECK(a.requestControl(g1, 7001) == ArbResult::Unknown);
}

void testLeaveAndTimeoutPromote() {
    ArbiterConfig c = cfg();
    c.viewerTimeoutMs = 3000;
    ControlArbiter a(c);
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    const std::string g3 = join(a, false, true, 0);
    a.setMode(owner, ControlMode::Exclusive, 0);
    a.requestControl(g1, 0);
    a.requestControl(g2, 0);
    a.requestControl(g3, 0);
    // the holder leaves: g2 is promoted
    CHECK(a.leave(g1, 100) == ArbResult::Ok);
    CHECK(a.holder(100) == g2);
    CHECK(a.leave(g1, 101) == ArbResult::Unknown);
    // a queued viewer leaves: it is gone from the queue
    CHECK(a.leave(g3, 200) == ArbResult::Ok);
    CHECK(a.queue(200).empty());
    // a viewer in the queue that times out is skipped on promotion
    ControlArbiter b(c);
    const std::string o2 = join(b, true, true, 0);
    const std::string h = join(b, false, true, 0);
    const std::string q1 = join(b, false, true, 0);
    const std::string q2 = join(b, false, true, 0);
    b.setMode(o2, ControlMode::Exclusive, 0);
    b.requestControl(h, 0);
    b.requestControl(q1, 0);
    b.requestControl(q2, 0);
    b.heartbeat(h, 2500);
    b.heartbeat(q2, 2500);
    b.heartbeat(o2, 2500);
    // q1 is last seen at 0: gone at 3000. The holder releases at 3000: q2 must get it, not q1
    CHECK(b.releaseControl(h, 3000) == ArbResult::Ok);
    CHECK(b.holder(3000) == q2);
    // holder timing out entirely (not just idle) promotes the next
    ControlArbiter e(c);
    const std::string o3 = join(e, true, true, 0);
    const std::string h3 = join(e, false, true, 0);
    const std::string n3 = join(e, false, true, 0);
    e.setMode(o3, ControlMode::Exclusive, 0);
    e.requestControl(h3, 0);
    e.requestControl(n3, 0);
    e.heartbeat(n3, 2900);
    e.heartbeat(o3, 2900);
    CHECK(e.holder(3000) == n3);
}

void testMaxHold() {
    ArbiterConfig c = cfg();
    c.maxHoldMs = 10000;
    c.holderIdleMs = 100000;
    c.viewerTimeoutMs = 1000000;
    ControlArbiter a(c);
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    a.setMode(owner, ControlMode::Exclusive, 0);
    a.requestControl(g1, 0);
    a.requestControl(g2, 0);
    for (std::int64_t t = 1000; t < 10000; t += 1000) { CHECK(a.mayAct(g1, t).allowed); }   // busy, never idle
    CHECK(a.holder(9999) == g1);
    CHECK(a.holder(10000) == g2);                                      // the ceiling moves control on
}

void testCapsAndRights() {
    ArbiterConfig c = cfg();
    c.maxViewers = 3;
    c.maxQueue = 1;
    ControlArbiter a(c);
    const std::string owner = join(a, true, true, 0);
    const std::string g1 = join(a, false, true, 0);
    const std::string g2 = join(a, false, true, 0);
    std::string extra;
    CHECK(a.join(false, true, 0, extra) == ArbResult::Full);
    CHECK(extra.empty());
    a.setMode(owner, ControlMode::Exclusive, 0);
    CHECK(a.requestControl(g1, 0) == ArbResult::Ok);
    CHECK(a.requestControl(g2, 0) == ArbResult::Queued);
    CHECK(a.requestControl(owner, 0) == ArbResult::QueueFull);         // even an admin queues like anyone else
    // the server switched listenersMayTune off: g1 loses its right and its control; g2 is promoted
    CHECK(a.setRights(g1, false, false, 1) == ArbResult::Ok);
    CHECK(a.holder(1) == g2);
    CHECK(!a.mayAct(g1, 2).allowed && a.mayAct(g1, 2).why == ArbResult::ListenerOnly);
    CHECK(a.requestControl(g1, 3) == ArbResult::ListenerOnly);
    // a queued viewer that loses the right is dropped from the queue
    CHECK(a.requestControl(owner, 4) == ArbResult::Queued);
    CHECK(a.setRights(owner, false, false, 5) == ArbResult::Ok);      // demoted admin that may not tune
    CHECK(a.queue(5).empty());
    CHECK(a.setMode(owner, ControlMode::Open, 6) == ArbResult::NotAdmin);   // and may no longer switch modes
    CHECK(a.setRights("zzzzzzzzzzzz", true, true, 7) == ArbResult::Unknown);
}

void testEpoch() {
    ControlArbiter a(cfg());
    const std::uint64_t e0 = a.epoch();
    const std::string owner = join(a, true, true, 0);
    const std::string g = join(a, false, true, 0);
    CHECK(a.epoch() > e0);
    a.setMode(owner, ControlMode::Exclusive, 1);
    const std::uint64_t e1 = a.epoch();
    a.requestControl(g, 2);
    CHECK(a.epoch() > e1);
    const std::uint64_t e2 = a.epoch();
    a.mayAct(g, 3);                                                    // asking never changes the epoch
    a.holder(3);
    a.viewers(3);
    CHECK(a.epoch() == e2);
    a.releaseControl(g, 4);
    CHECK(a.epoch() > e2);
}

// Concurrent callers with a shared clock: whatever the interleaving, the snapshot invariants hold and no call throws.
void testConcurrentInvariants() {
    ArbiterConfig c = cfg();
    c.holderIdleMs = 300;
    c.viewerTimeoutMs = 2000;
    c.maxViewers = 12;
    c.maxQueue = 4;
    ControlArbiter a(c);
    std::atomic<std::int64_t> clock{0};
    std::string admin;
    a.join(true, true, 0, admin);
    a.setMode(admin, ControlMode::Exclusive, 0);
    std::atomic<int> violations{0};
    std::atomic<bool> stop{false};

    auto checkSnapshot = [&] {
        const std::int64_t now = clock.load();
        const auto vs = a.viewers(now);
        int holders = 0;
        std::set<std::size_t> positions;
        std::set<std::string> ids;
        for (const ViewerInfo& v : vs) {
            if (v.holder) {
                ++holders;
                if (!v.mayTune) { ++violations; }                      // the holder must be allowed to tune
                if (v.queuePosition != 0) { ++violations; }            // the holder is not also queued
            }
            if (v.queuePosition != 0) {
                if (!v.mayTune) { ++violations; }
                if (!positions.insert(v.queuePosition).second) { ++violations; }   // positions are distinct
            }
            if (!ids.insert(v.id).second) { ++violations; }            // ids are unique
        }
        if (holders > 1) { ++violations; }
        for (std::size_t p = 1; p <= positions.size(); ++p) {
            if (positions.count(p) == 0) { ++violations; }              // positions are 1..n with no gap
        }
        if (vs.size() > c.maxViewers) { ++violations; }
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            std::mt19937_64 rng(1000 + t);
            std::string id;
            for (int i = 0; i < 3000; ++i) {
                const std::int64_t now = clock.fetch_add(7) + 7;
                if (id.empty()) {
                    a.join(false, (rng() % 5) != 0, now, id);
                    continue;
                }
                switch (rng() % 9) {
                    case 0: a.requestControl(id, now); break;
                    case 1: a.releaseControl(id, now); break;
                    case 2: a.mayAct(id, now); break;
                    case 3: a.heartbeat(id, now); break;
                    case 4: if (a.leave(id, now) == ArbResult::Ok) { id.clear(); } break;
                    case 5: a.cancelRequest(id, now); break;
                    case 6: a.grant(admin, id, now); break;
                    case 7: a.setRights(id, false, (rng() % 3) != 0, now); break;
                    default: a.tick(now); break;
                }
                if ((i % 17) == 0) { checkSnapshot(); }
            }
        });
    }
    std::thread watcher([&] {
        while (!stop.load()) { checkSnapshot(); std::this_thread::yield(); }
    });
    for (auto& th : ts) { th.join(); }
    stop = true;
    watcher.join();
    checkSnapshot();
    CHECK(violations.load() == 0);
}

void testSourceNeverPrintsOrWrites() {
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/net/web_arbiter.cpp", std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty());
    for (const char* banned : {"printf", "std::cout", "std::cerr", "fopen", "ofstream", "fstream", "fwrite", "OutputDebugString", "stderr", "stdout",
                               "settings_set", "getenv"}) {
        CHECK(src.find(banned) == std::string::npos);
    }
}

}  // namespace

int main() {
    testOpenModeAndIds();
    testIdsAreOpaque();
    testExclusiveRequestGrantQueueRelease();
    testAdminOverrideHandOverRevoke();
    testIdleAndTimeouts();
    testLeaveAndTimeoutPromote();
    testMaxHold();
    testCapsAndRights();
    testEpoch();
    testConcurrentInvariants();
    testSourceNeverPrintsOrWrites();
    return testSummary("test_web_arbiter");
}
