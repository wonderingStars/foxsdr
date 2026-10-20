// test_plugin_directive.cpp - THE DIRECTIVE LINK between the engine's pass and the interface's frame (the pump move, step P3;
// core/plugin_directive.hpp). No engine, no plugin: the protocol alone, with threads.
//
// WHAT THE PROTOCOL PROMISES, and what each case holds:
//
//   A  A DIRECTIVE POSTED IS APPLIED BY THE FRONT END'S THREAD, once, outside the lock, and only then acknowledged: epochs grow by
//      one per post, an empty link applies nothing, the applier runs on the caller of applyNewest and never on the poster.
//   B  NEWEST WINS. Directives posted while one waits REPLACE it: of five posts the front end applies the fifth and only the
//      fifth, skipped() counts the four it never saw, and an epoch that was skipped reads as acknowledged once a newer one is.
//   C  THE WAIT IS BOUNDED AND CAN GIVE UP. With no front end waitApplied returns false after the bound (not before, not much
//      after); a give-up predicate ends it at once; a front end that applies mid-wait releases it long before the bound.
//   D  THE ACKNOWLEDGEMENT FOLLOWS THE APPLY. A waiter that sees an epoch acknowledged also sees everything the applier did for
//      it (a flag the applier sets last) - over many rounds across two threads. (Break-it: store the acknowledgement before the
//      applier runs and D fails.)
//   E  A POST DURING AN APPLY IS NOT LOST. A directive posted while the applier runs stays pending, the acknowledged epoch is the
//      one applied, and the next call applies the new one.
//   F  A STORM. A poster and a front end run flat out: epochs the applier sees only grow, the last post is eventually applied,
//      and applied + skipped is exactly the number of posts.
//   G  polls() MOVES WITH EVERY CALL (applied or not): it is how the engine tells a front end that is framing from one that is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_directive.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

using cascade::core::PluginDirective;
using cascade::core::PluginDirectiveLink;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

PluginDirective marked(const std::string& stop) {
    PluginDirective d;
    d.stopped.push_back(stop);
    return d;
}

void testBasics() {
    std::printf("  A: post, apply on the caller's thread, acknowledge\n");
    PluginDirectiveLink link;
    CHECK(!link.hasPending());
    CHECK_EQ(link.postedEpoch(), 0u);
    CHECK_EQ(link.appliedEpoch(), 0u);
    int calls = 0;
    CHECK(!link.applyNewest([&](const PluginDirective&) { ++calls; }));  // nothing waiting: nothing applied
    CHECK_EQ(calls, 0);

    const std::uint64_t e1 = link.post(marked("one"));
    const std::uint64_t e2 = link.post(marked("two"));
    CHECK_EQ(e1, 1u);
    CHECK_EQ(e2, 2u);
    CHECK(link.hasPending());
    CHECK_EQ(link.appliedEpoch(), 0u);  // posting acknowledges nothing

    const std::thread::id poster = std::this_thread::get_id();
    std::thread::id appliedOn{};
    std::string seen;
    std::uint64_t seenEpoch = 0;
    bool applied = false;
    // (CHECK keeps plain counters: it is called from this thread only.)
    std::thread gui([&] {
        applied = link.applyNewest([&](const PluginDirective& d) {
            appliedOn = std::this_thread::get_id();
            seen = d.stopped.empty() ? std::string() : d.stopped[0];
            seenEpoch = d.epoch;
        });
    });
    gui.join();
    CHECK(applied);
    CHECK(appliedOn != poster);   // the applier ran on the thread that called applyNewest
    CHECK(seen == "two");         // the newest
    CHECK_EQ(seenEpoch, 2u);
    CHECK_EQ(link.appliedEpoch(), 2u);
    CHECK(!link.hasPending());
    CHECK_EQ(link.appliedCount(), 1u);
    // ...and an epoch that was never applied itself (1) is acknowledged by a newer one that was.
    CHECK(link.waitApplied(1, 1ms));
    CHECK(link.waitApplied(2, 1ms));
    CHECK(!link.waitApplied(3, 1ms));
}

void testNewestWins() {
    std::printf("  B: newest wins - five posts, one apply, four skipped\n");
    PluginDirectiveLink link;
    for (int i = 1; i <= 5; ++i) { (void)link.post(marked(std::to_string(i))); }
    CHECK_EQ(link.skipped(), 4u);
    std::vector<std::uint64_t> applied;
    CHECK(link.applyNewest([&](const PluginDirective& d) { applied.push_back(d.epoch); }));
    CHECK(!link.applyNewest([&](const PluginDirective& d) { applied.push_back(d.epoch); }));
    CHECK_EQ(applied.size(), 1u);
    CHECK_EQ(applied[0], 5u);
    CHECK_EQ(link.appliedEpoch(), 5u);
    CHECK_EQ(link.appliedCount(), 1u);
    CHECK_EQ(link.skipped(), 4u);
    for (std::uint64_t e = 1; e <= 5; ++e) { CHECK(link.waitApplied(e, 1ms)); }
}

void testBoundedWait() {
    std::printf("  C: the wait is bounded, gives up on a predicate, and ends when the front end applies\n");
    PluginDirectiveLink link;
    const std::uint64_t e = link.post(marked("x"));

    // No front end: the bound is the answer - not early, not much late.
    {
        const auto t0 = Clock::now();
        CHECK(!link.waitApplied(e, 120ms));
        const double ms = msSince(t0);
        std::printf("      no front end: gave up after %.1f ms (bound 120)\n", ms);
        CHECK(ms >= 110.0);
        CHECK(ms < 600.0);
    }
    // A predicate that says stop ends it at once (within a polling slice), long before the bound.
    {
        std::atomic<bool> stop{false};
        std::thread flip([&] {
            std::this_thread::sleep_for(30ms);
            stop.store(true);
        });
        const auto t0 = Clock::now();
        CHECK(!link.waitApplied(e, 5000ms, [&] { return stop.load(); }));
        const double ms = msSince(t0);
        flip.join();
        std::printf("      give-up predicate: ended after %.1f ms (bound 5000)\n", ms);
        CHECK(ms >= 25.0);
        CHECK(ms < 1000.0);
    }
    // A front end that applies mid-wait releases the waiter promptly.
    {
        std::thread gui([&] {
            std::this_thread::sleep_for(40ms);
            (void)link.applyNewest([](const PluginDirective&) {});
        });
        const auto t0 = Clock::now();
        CHECK(link.waitApplied(e, 5000ms));
        const double ms = msSince(t0);
        gui.join();
        std::printf("      front end applied mid-wait: released after %.1f ms\n", ms);
        CHECK(ms >= 30.0);
        CHECK(ms < 1000.0);
    }
}

void testAckFollowsApply() {
    std::printf("  D: whoever sees an epoch acknowledged sees what the applier did for it\n");
    PluginDirectiveLink link;
    std::atomic<std::uint64_t> done{0};  // the applier's last act, per epoch
    std::atomic<bool> stop{false};
    std::thread gui([&] {
        while (!stop.load()) {
            (void)link.applyNewest([&](const PluginDirective& d) {
                std::this_thread::yield();      // widen the window between "applier running" and "acknowledged"
                done.store(d.epoch);            // the applier's work for this epoch
            });
        }
    });
    int torn = 0;
    for (int i = 0; i < 3000; ++i) {
        const std::uint64_t e = link.post(marked("r"));
        if (link.waitApplied(e, 2000ms)) {
            // Acknowledged: the applier has finished the work of this epoch or a newer one.
            if (done.load() < e) { ++torn; }
        }
    }
    stop.store(true);
    gui.join();
    CHECK_EQ(torn, 0);
}

void testPostDuringApply() {
    std::printf("  E: a directive posted while the applier runs is kept for the next call\n");
    PluginDirectiveLink link;
    (void)link.post(marked("first"));
    std::uint64_t secondEpoch = 0;
    std::uint64_t acknowledgedInside = 99;
    CHECK(link.applyNewest([&](const PluginDirective& d) {
        CHECK_EQ(d.epoch, 1u);
        std::thread other([&] { secondEpoch = link.post(marked("second")); });
        other.join();
        acknowledgedInside = link.appliedEpoch();
    }));
    CHECK_EQ(secondEpoch, 2u);
    CHECK_EQ(acknowledgedInside, 0u);       // not acknowledged until the applier returns
    CHECK_EQ(link.appliedEpoch(), 1u);       // ...and then only the one it applied
    CHECK(link.hasPending());                // the second is waiting
    std::string seen;
    CHECK(link.applyNewest([&](const PluginDirective& d) { seen = d.stopped[0]; }));
    CHECK(seen == "second");
    CHECK_EQ(link.appliedEpoch(), 2u);
    CHECK_EQ(link.skipped(), 0u);
}

void testStorm() {
    std::printf("  F: a poster and a front end flat out - epochs only grow, the last is applied, applied + skipped = posts\n");
    PluginDirectiveLink link;
    constexpr int kPosts = 20000;
    std::atomic<bool> posting{true};
    std::uint64_t last = 0;
    int backwards = 0;
    std::thread gui([&] {
        while (posting.load() || link.hasPending()) {
            (void)link.applyNewest([&](const PluginDirective& d) {
                if (d.epoch <= last) { ++backwards; }
                last = d.epoch;
            });
        }
    });
    for (int i = 0; i < kPosts; ++i) { (void)link.post(marked("s")); }
    posting.store(false);
    gui.join();
    CHECK_EQ(backwards, 0);
    CHECK_EQ(last, static_cast<std::uint64_t>(kPosts));
    CHECK_EQ(link.appliedEpoch(), static_cast<std::uint64_t>(kPosts));
    CHECK_EQ(link.appliedCount() + link.skipped(), static_cast<std::uint64_t>(kPosts));
    std::printf("      %llu applied, %llu skipped of %d\n", static_cast<unsigned long long>(link.appliedCount()),
                static_cast<unsigned long long>(link.skipped()), kPosts);
}

void testPolls() {
    std::printf("  G: polls() moves with every call, applied or not\n");
    PluginDirectiveLink link;
    const std::uint64_t p0 = link.polls();
    for (int i = 0; i < 7; ++i) { (void)link.applyNewest([](const PluginDirective&) {}); }
    CHECK_EQ(link.polls(), p0 + 7u);
    (void)link.post(marked("a"));
    (void)link.applyNewest([](const PluginDirective&) {});
    CHECK_EQ(link.polls(), p0 + 8u);
}

}  // namespace

int main() {
    std::printf("test_plugin_directive\n");
    testBasics();
    testNewestWins();
    testBoundedWait();
    testAckFollowsApply();
    testPostDuringApply();
    testStorm();
    testPolls();
    return testSummary("test_plugin_directive");
}
