// test_frame_timing.cpp - the slow-frame record (core/frame_timing.hpp, 0.99.64):
// what a frame's wall time is divided into, which frames are recorded and how,
// what must never be counted, the log's words and its rate limit, the one-atomic
// answer to "which scope is the window's thread in", and what all of it costs.
//
// THE CLASS, NOT THE WINDOW. Everything here drives a FrameTimer with a clock the
// test owns, so a frame of 249 ms and one of 250 ms are exactly that and nothing
// waits. What the WINDOW does with it - the scopes' places in the frame loop, the
// hidden window, the display change, the modal loop, a script's sleep - is in
// tests/test_slow_frames_app.cpp, through the real application.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/frame_timing.hpp"
#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

constexpr std::int64_t kMs = 1'000'000LL;

// The clocks of the timer under test. `g_asleep` is time the awake clock did not
// see: a suspend.
std::int64_t g_now = 1'000'000'000'000LL;
std::int64_t g_asleep = 0;
std::int64_t fakeSteady() { return g_now; }
std::int64_t fakeAwake() { return g_now - g_asleep; }

struct Line {
    bool warn;
    std::string text;
};
std::vector<Line> g_lines;
void sink(bool warn, const char* line) { g_lines.push_back({warn, line}); }

void advanceMs(std::int64_t ms) { g_now += ms * kMs; }

// A fresh timer on the fake clocks, writing to the vector.
struct Rig {
    FrameTimer t{&fakeSteady, &fakeAwake};
    Rig() {
        g_lines.clear();
        g_asleep = 0;
        g_now += 100'000LL * kMs;  // every rig starts a clean 100 s later
        t.setSinkForTest(&sink);
    }
    // One frame: `body` runs between begin and end, and the held frame is
    // settled at the top of the next, as the loop does.
    template <class F>
    void frame(long index, F body, bool discardPrevious = false, bool exclude = false) {
        t.beginFrame(index);
        t.settlePrevious(discardPrevious);
        if (exclude) { t.excludeFrame(); }
        body();
        t.endFrame();
    }
    // A frame that spends `ms` in `s`, plus `otherMs` in no scope.
    void slowIn(long index, FrameScope s, std::int64_t ms, std::int64_t otherMs = 0) {
        frame(index, [&] {
            { FrameScopeGuard g(t, s); advanceMs(ms); }
            advanceMs(otherMs);
        });
    }
};

int countLinesWith(const char* what) {
    int n = 0;
    for (const Line& l : g_lines) {
        if (l.text.find(what) != std::string::npos) { ++n; }
    }
    return n;
}

bool onlyFixedWords(const std::string& s) {
    // "frame: 412 ms, 388 ms of it in rail" and the two suffixes, nothing else.
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == ':' ||
                        c == ',' || c == ';' || c == '-' || c == '/' || c == '(' || c == ')';
        if (!ok) { return false; }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("test_frame_timing\n");

    // --- the names, the tiers, and the numbers copied from the watchdog -------
    {
        std::set<std::string> seen;
        for (int i = 0; i < kFrameScopeCount; ++i) {
            const std::string n = frameScopeName(static_cast<FrameScope>(i));
            CHECK(!n.empty());
            CHECK(seen.insert(n).second);  // unique
            for (char c : n) { CHECK((c >= 'a' && c <= 'z') || c == '-'); }
            FrameScope back = FrameScope::Other;
            CHECK(frameScopeFromName(n.c_str(), back));
            CHECK(back == static_cast<FrameScope>(i));
        }
        FrameScope junk = FrameScope::Rail;
        CHECK(!frameScopeFromName("no-such-scope", junk));
        CHECK(!frameScopeFromName(nullptr, junk));
        CHECK(junk == FrameScope::Rail);  // untouched on failure
        CHECK(std::strcmp(frameScopeName(FrameScope::PluginsReload), "plugins-reload") == 0);
        CHECK(std::strcmp(frameScopeName(FrameScope::Other), "other") == 0);
        // "on the order of 10-15" counted scopes, plus start-up, user-wait and the
        // time in none.
        volatile int nScopes = kFrameScopeCount;
        CHECK(nScopes >= 10 && nScopes <= 20);

        // The two real clocks agree about how long a short wait was - the awake
        // clock is the steady one with the suspends taken out, so on a machine
        // that does not sleep during this test they cannot differ.
        const std::int64_t s0 = frameSteadyNanos();
        const std::int64_t a0 = frameAwakeNanos();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const std::int64_t s1 = frameSteadyNanos();
        const std::int64_t a1 = frameAwakeNanos();
        CHECK((s1 - s0) >= 50 * kMs);
        CHECK(std::llabs((s1 - s0) - (a1 - a0)) < 30 * kMs);

        // The tiers: below the first is not slow, each boundary belongs to the
        // tier it starts.
        CHECK(slowFrameTier(0) == -1);
        CHECK(slowFrameTier(249 * kMs) == -1);
        CHECK(slowFrameTier(250 * kMs) == 0);
        CHECK(slowFrameTier(999 * kMs) == 0);
        CHECK(slowFrameTier(1000 * kMs) == 1);
        CHECK(slowFrameTier(4999 * kMs) == 1);
        CHECK(slowFrameTier(5000 * kMs) == 2);
        CHECK(slowFrameTier(600'000 * kMs) == 2);

        // COPIED, NOT INCLUDED, and held equal: the top tier is the watchdog's
        // own threshold, and start-up is its start-up window.
        volatile std::int64_t topTierNs = kSlowFrameTierNs[2];
        volatile long startupFrames = FrameTimer::kStartupFrames;
        CHECK(topTierNs == static_cast<std::int64_t>(HangWatchdog::kDefaultThresholdMs) * kMs);
        CHECK(startupFrames == static_cast<long>(HangWatchdog::kStartupFrames));
    }

    // --- a fast frame leaves nothing: no count, no line -----------------------
    {
        Rig r;
        for (long i = 0; i < 50; ++i) { r.slowIn(i + 100, FrameScope::Rail, 8); }
        r.t.finish();
        CHECK(!r.t.counts().any());
        CHECK(g_lines.empty());
        CHECK(r.t.excludedSlowFrames() == 0);
        CHECK(r.t.framesTimed() == 50);
        // 249 ms is the last fast one.
        r.slowIn(200, FrameScope::Rail, 249);
        r.t.finish();
        CHECK(!r.t.counts().any());
    }

    // --- the scopes always add up to the frame --------------------------------
    {
        Rig r;
        r.t.beginFrame(100);
        const std::int64_t start = g_now;
        advanceMs(7);  // events, charged to the guard the loop holds
        {
            FrameScopeGuard loop(r.t, FrameScope::Events);
            advanceMs(5);
            loop.to(FrameScope::FrameStart);
            advanceMs(11);
            loop.to(FrameScope::Other);
            advanceMs(13);
            {
                FrameScopeGuard g(r.t, FrameScope::Recorder);
                advanceMs(17);
            }
            advanceMs(19);
        }
        advanceMs(3);
        const std::int64_t wall = g_now - start;
        // endFrame closes the last interval; the sum is read after it.
        r.t.endFrame();
        std::int64_t sum = 0;
        for (int s = 0; s < kFrameScopeCount; ++s) {
            sum += r.t.scopeNanos(static_cast<FrameScope>(s));
        }
        CHECK(sum == wall);
        CHECK(r.t.scopeNanos(FrameScope::Events) == 5 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::FrameStart) == 11 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::Recorder) == 17 * kMs);
        // Everything inside no scope is `other`: 7 before the guard, 13 + 19
        // between, 3 after.
        CHECK(r.t.scopeNanos(FrameScope::Other) == (7 + 13 + 19 + 3) * kMs);
    }

    // --- a nested scope's time is its own, and leaves the outer one's out ------
    {
        Rig r;
        r.t.beginFrame(100);
        {
            FrameScopeGuard outer(r.t, FrameScope::Rail);
            advanceMs(100);
            {
                FrameScopeGuard inner(r.t, FrameScope::Recorder);
                advanceMs(300);
                {
                    FrameScopeGuard innermost(r.t, FrameScope::PluginsReload);
                    advanceMs(40);
                }
                advanceMs(2);
            }
            advanceMs(50);
        }
        r.t.endFrame();
        CHECK(r.t.scopeNanos(FrameScope::Rail) == 150 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::Recorder) == 302 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::PluginsReload) == 40 * kMs);
        // The current scope is back to where it was, and nothing is left open.
        CHECK(r.t.currentScope() == FrameScope::Other);
        r.t.finish();
        // The frame was 492 ms, and the scope that took the most of it was the
        // recorder - not the rail the recorder was started from.
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Recorder)][0] == 1);
        CHECK(r.t.counts().total() == 1);
    }

    // --- to() moves a guard without nesting, and the caller's scope comes back --
    {
        Rig r;
        r.t.beginFrame(100);
        {
            FrameScopeGuard caller(r.t, FrameScope::Polls);
            advanceMs(10);
            {
                FrameScopeGuard walker(r.t, FrameScope::PreDraw);
                advanceMs(20);
                walker.to(FrameScope::Toolbar);
                advanceMs(30);
                walker.to(FrameScope::Rail);
                CHECK(r.t.currentScope() == FrameScope::Rail);
                advanceMs(40);
            }
            // Back in the CALLER's scope, not in `other` and not in the first
            // scope the walker was opened with.
            CHECK(r.t.currentScope() == FrameScope::Polls);
            advanceMs(50);
        }
        r.t.endFrame();
        CHECK(r.t.scopeNanos(FrameScope::Polls) == 60 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::PreDraw) == 20 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::Toolbar) == 30 * kMs);
        CHECK(r.t.scopeNanos(FrameScope::Rail) == 40 * kMs);
    }

    // --- a slow frame is counted once, in the highest tier it reached, against
    //     the scope that took the most of it ----------------------------------
    {
        Rig r;
        struct Case { FrameScope s; std::int64_t ms; int tier; };
        const Case cases[] = {
            {FrameScope::Rail, 250, 0},      {FrameScope::Rail, 999, 0},
            {FrameScope::Polls, 1000, 1},    {FrameScope::Polls, 4999, 1},
            {FrameScope::Present, 5000, 2},  {FrameScope::Present, 60'000, 2},
        };
        long idx = 100;
        for (const Case& c : cases) { r.slowIn(idx++, c.s, c.ms); }
        r.t.finish();
        const SlowFrameCounts n = r.t.counts();
        CHECK(n.count[static_cast<int>(FrameScope::Rail)][0] == 2);
        CHECK(n.count[static_cast<int>(FrameScope::Polls)][1] == 2);
        CHECK(n.count[static_cast<int>(FrameScope::Present)][2] == 2);
        // ONCE each: six frames, six counts - a 60 s frame is not also a 1 s one.
        CHECK(n.total() == 6);
        for (int s = 0; s < kFrameScopeCount; ++s) {
            for (int t = 0; t < kSlowFrameTiers; ++t) {
                const bool expected =
                    (s == static_cast<int>(FrameScope::Rail) && t == 0) ||
                    (s == static_cast<int>(FrameScope::Polls) && t == 1) ||
                    (s == static_cast<int>(FrameScope::Present) && t == 2);
                CHECK((n.count[s][t] != 0) == expected);
            }
        }
        // slowFrameCounts() is the same table.
        CHECK(slowFramesText(n) == "rail 2/0/0, polls 0/2/0, present 0/0/2");
    }

    // --- the slowest scope wins; the first of equals; `other` can win ----------
    {
        Rig r;
        r.frame(100, [&] {
            { FrameScopeGuard g(r.t, FrameScope::Status); advanceMs(300); }
            { FrameScopeGuard g(r.t, FrameScope::Rail); advanceMs(300); }
        });
        r.frame(101, [&] { advanceMs(400); });  // no scope at all
        r.t.finish();
        const SlowFrameCounts n = r.t.counts();
        // Equal: the earlier in the enum - rail - and not the one that ran last.
        CHECK(n.count[static_cast<int>(FrameScope::Rail)][0] == 1);
        CHECK(n.count[static_cast<int>(FrameScope::Status)][0] == 0);
        // Time in no scope is its own scope, and it is what the table says.
        CHECK(n.count[static_cast<int>(FrameScope::Other)][0] == 1);
    }

    // --- the log line: its words, its numbers, and nothing else ----------------
    {
        Rig r;
        r.slowIn(100, FrameScope::Rail, 388, 24);
        r.slowIn(101, FrameScope::Recorder, 1500);
        r.t.finish();
        CHECK(g_lines.size() == 3);  // rail, recorder, and the summary
        if (g_lines.size() >= 3) {
            CHECK(g_lines[0].text == "frame: 412 ms, 388 ms of it in rail");
            CHECK(!g_lines[0].warn);  // a stutter is information
            CHECK(g_lines[1].text == "frame: 1500 ms, 1500 ms of it in recorder");
            CHECK(g_lines[1].warn);   // a freeze is a warning
            CHECK(g_lines[2].text.rfind("frame: slow frames this session - ", 0) == 0);
            CHECK(g_lines[2].text.find("rail 1/0/0, recorder 0/1/0") != std::string::npos);
        }
        for (const Line& l : g_lines) { CHECK(onlyFixedWords(l.text)); }
        // Short enough for the log's line, which truncates.
        for (const Line& l : g_lines) { CHECK(l.text.size() < DiagLog::kLineBytes); }
    }

    // --- one line per scope per 30 s; the ones not written are counted in the
    //     next one that is --------------------------------------------------------
    {
        Rig r;
        r.slowIn(100, FrameScope::Rail, 300);
        advanceMs(10'000);
        r.slowIn(101, FrameScope::Rail, 300);   // 10 s later: not logged
        r.slowIn(102, FrameScope::Polls, 300);  // another scope has its own allowance
        advanceMs(10'000);
        r.slowIn(103, FrameScope::Rail, 300);   // 20 s after the first: not logged
        r.t.finish();                           // settles 103; writes the summary
        CHECK(countLinesWith("of it in rail") == 1);
        CHECK(countLinesWith("of it in polls") == 1);
        // ...but every one of them is in the table.
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Rail)][0] == 3);
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Polls)][0] == 1);

        // 31 s after the first line the allowance is back, and the line says how
        // many it did not write.
        advanceMs(31'000);
        r.slowIn(104, FrameScope::Rail, 300);
        r.t.finish();
        CHECK(countLinesWith("of it in rail") == 2);
        bool saidTwo = false;
        for (const Line& l : g_lines) {
            if (l.text.find("of it in rail; 2 more slow frames in it not logged") !=
                std::string::npos) {
                saidTwo = true;
            }
        }
        CHECK(saidTwo);
        for (const Line& l : g_lines) { CHECK(onlyFixedWords(l.text)); }
    }

    // --- start-up is not discarded: it is its own scope, with its part named ---
    {
        Rig r;
        // Frame 29 is the last of start-up, 30 the first that is not.
        r.frame(29, [&] {
            { FrameScopeGuard g(r.t, FrameScope::FrameStart); advanceMs(700); }
            { FrameScopeGuard g(r.t, FrameScope::Rail); advanceMs(300); }
        });
        r.slowIn(30, FrameScope::Rail, 400);
        r.t.finish();
        const SlowFrameCounts n = r.t.counts();
        CHECK(n.count[static_cast<int>(FrameScope::Startup)][0] == 0);
        CHECK(n.count[static_cast<int>(FrameScope::Startup)][1] == 1);  // 1000 ms
        CHECK(n.count[static_cast<int>(FrameScope::FrameStart)][1] == 0);
        CHECK(n.count[static_cast<int>(FrameScope::Rail)][0] == 1);     // frame 30
        CHECK(n.total() == 2);
        CHECK(countLinesWith("frame: 1000 ms, 1000 ms of it in startup; largest part: frame-start "
                             "700 ms") == 1);
    }

    // --- NOT COUNTED: a frame the window was not being shown for ---------------
    {
        Rig r;
        r.frame(100, [&] { FrameScopeGuard g(r.t, FrameScope::Present); advanceMs(900); },
                false, /*exclude=*/true);
        r.frame(101, [&] {});
        r.t.finish();
        CHECK(!r.t.counts().any());
        CHECK(g_lines.empty());
        CHECK(r.t.excludedSlowFrames() == 1);  // it WAS slow - the test is not vacuous
        // Without the exclusion the identical frame is counted.
        r.frame(102, [&] { FrameScopeGuard g(r.t, FrameScope::Present); advanceMs(900); });
        r.t.finish();
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Present)][0] == 1);
    }

    // --- NOT COUNTED: the swap that stalled before the display change was told ---
    {
        Rig r;
        r.slowIn(100, FrameScope::Present, 900);
        // The next frame's first look says the display changed: the held frame goes.
        r.frame(101, [&] {}, /*discardPrevious=*/true);
        r.t.finish();
        CHECK(!r.t.counts().any());
        CHECK(g_lines.empty());
        CHECK(r.t.excludedSlowFrames() == 1);
        // And when nothing changed, the same frame is counted - a frame later.
        r.slowIn(102, FrameScope::Present, 900);
        CHECK(!r.t.counts().any());  // still held: the next frame has not looked yet
        r.frame(103, [&] {}, false);
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Present)][0] == 1);
        CHECK(countLinesWith("of it in present") == 1);
    }

    // --- NOT COUNTED: a person's time (user-wait), and a modal loop ------------
    {
        Rig r;
        // A shell call held for 8 s inside the rail: the frame's counted time is
        // what is left, and 8 s of waiting is never a candidate however large.
        r.frame(100, [&] {
            FrameScopeGuard rail(r.t, FrameScope::Rail);
            advanceMs(40);
            r.t.userWaitBegin();
            CHECK(r.t.currentScope() == FrameScope::UserWait);
            advanceMs(8000);
            r.t.userWaitEnd();
            CHECK(r.t.currentScope() == FrameScope::Rail);  // back where it was
            advanceMs(20);
        });
        r.t.finish();
        CHECK(!r.t.counts().any());
        CHECK(g_lines.empty());
        CHECK(r.t.scopeNanos(FrameScope::UserWait) == 8000 * kMs);
        CHECK(r.t.excludedSlowFrames() == 0);  // not slow at all, once the wait is out

        // The same wait around a real hitch: the hitch is counted at ITS size.
        r.frame(101, [&] {
            FrameScopeGuard rail(r.t, FrameScope::Rail);
            r.t.userWaitBegin();
            advanceMs(8000);
            r.t.userWaitEnd();
            advanceMs(400);
        });
        r.t.finish();
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Rail)][0] == 1);  // 400 ms, not 8 s
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::UserWait)][2] == 0);
        CHECK(countLinesWith("frame: 400 ms, 400 ms of it in rail") == 1);

        // Nested brackets: an inner pair does not end an outer one.
        r.frame(102, [&] {
            r.t.userWaitBegin();
            r.t.userWaitBegin();
            advanceMs(3000);
            r.t.userWaitEnd();
            CHECK(r.t.currentScope() == FrameScope::UserWait);
            advanceMs(3000);
            r.t.userWaitEnd();
            CHECK(r.t.currentScope() == FrameScope::Other);
        });
        CHECK(r.t.scopeNanos(FrameScope::UserWait) == 6000 * kMs);
        r.t.finish();
        CHECK(r.t.counts().total() == 1);  // still only the hitch

        // An unbalanced end changes nothing.
        r.t.userWaitEnd();
        CHECK(r.t.currentScope() == FrameScope::Other);

        // A modal window loop inside the message pump is the same thing.
        Rig m;
        m.frame(100, [&] {
            FrameScopeGuard pump(m.t, FrameScope::Events);
            advanceMs(2500);
            m.t.moveToUserWait(FrameScope::Events);
        });
        m.t.finish();
        CHECK(!m.t.counts().any());
        CHECK(m.t.scopeNanos(FrameScope::Events) == 0);
        CHECK(m.t.scopeNanos(FrameScope::UserWait) == 2500 * kMs);
        // ...and without the move the pump is a frame like any other.
        m.frame(101, [&] { FrameScopeGuard pump(m.t, FrameScope::Events); advanceMs(2500); });
        m.t.finish();
        CHECK(m.t.counts().count[static_cast<int>(FrameScope::Events)][1] == 1);
    }

    // --- COUNTED: a plugin reload under its watchdog pause is the freeze -------
    {
        Rig r;
        // The watchdog's pause excuses the WATCHDOG; the frame is another matter.
        r.frame(100, [&] {
            FrameScopeGuard polls(r.t, FrameScope::Polls);
            advanceMs(2);
            {
                FrameScopeGuard reload(r.t, FrameScope::PluginsReload);
                advanceMs(3600);
            }
        });
        r.t.finish();
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::PluginsReload)][1] == 1);
        CHECK(countLinesWith("frame: 3602 ms, 3600 ms of it in plugins-reload") == 1);
        CHECK(slowFramesText(r.t.counts()) == "plugins-reload 0/1/0");
    }

    // --- NOT COUNTED: the machine asleep (the clocks disagree) -----------------
    {
        Rig r;
        r.frame(100, [&] {
            FrameScopeGuard g(r.t, FrameScope::Present);
            advanceMs(600);
            g_asleep += 3'600'000 * kMs;  // an hour of suspend, seen only by the steady clock
            g_now += 3'600'000 * kMs;
        });
        r.t.finish();
        CHECK(!r.t.counts().any());
        CHECK(r.t.excludedSlowFrames() == 1);
        // A frame whose clocks differ by less than a second is a slow frame, not
        // a suspend: the tolerance is real.
        r.frame(101, [&] {
            FrameScopeGuard g(r.t, FrameScope::Present);
            advanceMs(300);
            g_now += 500 * kMs;  // half a second the awake clock did not see
            g_asleep += 500 * kMs;
        });
        r.t.finish();
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Present)][0] == 1);
    }

    // --- the loop is over: the held frame is counted, the summary is said once --
    {
        Rig r;
        r.slowIn(100, FrameScope::Toolbar, 500);
        CHECK(!r.t.counts().any());
        r.t.finish();
        CHECK(r.t.counts().count[static_cast<int>(FrameScope::Toolbar)][0] == 1);
        CHECK(countLinesWith("frame: slow frames this session - toolbar 1/0/0") == 1);
        // Nothing slow: nothing said.
        Rig q;
        q.slowIn(100, FrameScope::Toolbar, 20);
        q.t.finish();
        CHECK(g_lines.empty());
    }

    // --- the line a bounded run prints is whole however long the table is -------
    // Found by the real-window test: every scope can have an entry, and a table of
    // them is longer than any fixed buffer the figures were once formatted into.
    {
        Rig r;
        long idx = 100;
        for (int s = 0; s < kFrameScopeCount; ++s) {
            const FrameScope fs = static_cast<FrameScope>(s);
            if (fs == FrameScope::UserWait || fs == FrameScope::Startup) { continue; }
            r.slowIn(idx++, fs, 300);
        }
        r.t.finish();
        const std::string line = r.t.summaryText();
        for (int s = 0; s < kFrameScopeCount; ++s) {
            const FrameScope fs = static_cast<FrameScope>(s);
            if (fs == FrameScope::UserWait || fs == FrameScope::Startup) { continue; }
            CHECK(line.find(std::string(frameScopeName(fs)) + " 1/0/0") != std::string::npos);
        }
        CHECK(line.size() > 256);
        const std::string tail = "; slow frames not counted: 0";
        CHECK(line.size() >= tail.size() &&
              line.compare(line.size() - tail.size(), tail.size(), tail) == 0);
        CHECK(line.rfind("17 frames, mean ", 0) == 0);
    }

    // --- the bundle's value, and its place in the bundle -----------------------
    {
        SlowFrameCounts none;
        CHECK(slowFramesText(none) == "none");
        SlowFrameCounts some;
        some.count[static_cast<int>(FrameScope::PluginsReload)][1] = 1;
        some.count[static_cast<int>(FrameScope::Recorder)][0] = 2;
        // In the order of the enum, not of insertion; zeros omitted.
        CHECK(slowFramesText(some) == "recorder 2/0/0, plugins-reload 0/1/0");

        const std::string base = "FoxSDR diagnostics bundle\nversion: 1\nsdrplay-service: x\n\n--- log ---\nline\n";
        const std::string out = withSlowFramesField(base, "recorder 2/0/0");
        CHECK(out ==
              "FoxSDR diagnostics bundle\nversion: 1\nsdrplay-service: x\nslow-frames: recorder "
              "2/0/0\n\n--- log ---\nline\n");
        CHECK(withSlowFramesField(base, "").find("\nslow-frames: none\n") != std::string::npos);
        CHECK(frameBundleFieldNames().size() == 1 && frameBundleFieldNames()[0] == "slow-frames");
    }

    // --- the test hooks' parsers ---------------------------------------------
    {
        const std::vector<FrameStall> s =
            parseFrameStalls("rail=300@45, polls=200@50,bogus=1@2,rail=0@3,rail=9000@3,present=50@x,"
                             "status=5y@4,other=400@90,toolbar=100");
        // " polls" with its space is not a scope name; bogus is not a scope; the
        // delays 0 and 9000 are out of range; "@x" and "5y" are not numbers; the
        // last has no "@". Two items survive.
        std::set<std::string> kept;
        for (const FrameStall& f : s) { kept.insert(frameScopeName(f.scope)); }
        CHECK(s.size() == 2);
        CHECK(kept.count("rail") == 1);
        CHECK(kept.count("other") == 1);
        CHECK(kept.count("bogus") == 0 && kept.count("polls") == 0 && kept.count("present") == 0 &&
              kept.count("status") == 0 && kept.count("toolbar") == 0);
        bool railOk = false;
        for (const FrameStall& f : s) {
            if (f.scope == FrameScope::Rail && f.ms == 300 && f.frame == 45) { railOk = true; }
        }
        CHECK(railOk);
        CHECK(parseFrameStalls(nullptr).empty());
        CHECK(parseFrameStalls("").empty());

        const FrameSituations a = parseFrameSituations("hidden@40-60,display@100,modal@80");
        CHECK(!a.hiddenAt(39) && a.hiddenAt(40) && a.hiddenAt(60) && !a.hiddenAt(61));
        CHECK(a.displayChangeAt(100) && !a.displayChangeAt(99));
        CHECK(a.modalLoopAt(80) && !a.modalLoopAt(81));
        const FrameSituations b = parseFrameSituations(nullptr);
        CHECK(!b.hiddenAt(0) && !b.displayChangeAt(0) && !b.modalLoopAt(0));
        const FrameSituations c = parseFrameSituations("hidden@5,display@x");
        CHECK(!c.hiddenAt(5));
    }

    // --- the stall hook holds the thread INSIDE the named scope -----------------
    {
        FrameTimer t;  // the real clock
        g_lines.clear();
        t.setSinkForTest(&sink);
        t.setStalls(parseFrameStalls("rail=300@7,other=300@7"));
        t.beginFrame(6);  // not the frame: no stall
        { FrameScopeGuard g(t, FrameScope::Rail); }
        t.endFrame();
        CHECK(t.longestFrameNs() < 100 * kMs);
        t.beginFrame(7);
        { FrameScopeGuard g(t, FrameScope::Toolbar); }  // not the scope: no stall
        const std::int64_t before = t.scopeNanos(FrameScope::Rail);
        CHECK(before == 0);
        { FrameScopeGuard g(t, FrameScope::Rail); }
        CHECK(t.scopeNanos(FrameScope::Rail) >= 250 * kMs);
        CHECK(t.scopeNanos(FrameScope::Toolbar) < 100 * kMs);
        t.endFrame();
        // "other" is held where the frame is in no scope, so it is `other` that
        // took the second 300 ms.
        CHECK(t.scopeNanos(FrameScope::Other) >= 250 * kMs);
        // Once only.
        t.beginFrame(7);
        { FrameScopeGuard g(t, FrameScope::Rail); }
        t.endFrame();
        CHECK(t.longestFrameNs() >= 550 * kMs);
        t.finish();
        CHECK(t.counts().total() == 1);
    }

    // --- ANOTHER THREAD CAN ASK WHERE THE FRAME IS ------------------------------
    // The window's thread is parked inside a scope; another thread reads the
    // process-wide atomic and must see exactly that scope, then the nested one,
    // then none. No lock anywhere: the reader never waits for the parked thread.
    {
        FrameTimer& t = frameTimer();
        t.resetForTest();
        CHECK(currentFrameScope() == FrameScope::Other);

        std::atomic<int> step{0};  // the parked thread's position, for the reader
        std::atomic<int> seen[4];
        for (auto& s : seen) { s.store(-1); }
        std::atomic<bool> readerDone[4];
        for (auto& d : readerDone) { d.store(false); }

        std::thread reader([&] {
            for (int i = 1; i <= 3; ++i) {
                // Wait for the parked thread to say it is parked at position i.
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (step.load() < i && std::chrono::steady_clock::now() < until) {
                    std::this_thread::yield();
                }
                seen[i].store(static_cast<int>(currentFrameScope()));
                readerDone[i].store(true);
            }
        });

        auto park = [&](int position) {
            step.store(position);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!readerDone[position].load() && std::chrono::steady_clock::now() < until) {
                std::this_thread::yield();
            }
        };

        t.beginFrame(100);
        {
            FrameScopeGuard outer(FrameScope::Rail);
            park(1);
            {
                FrameScopeGuard inner(FrameScope::PluginsReload);
                park(2);
            }
            outer.to(FrameScope::Status);
            park(3);
        }
        t.endFrame();
        reader.join();

        CHECK(seen[1].load() == static_cast<int>(FrameScope::Rail));
        CHECK(seen[2].load() == static_cast<int>(FrameScope::PluginsReload));
        CHECK(seen[3].load() == static_cast<int>(FrameScope::Status));
        CHECK(currentFrameScope() == FrameScope::Other);
        t.resetForTest();
    }

    // --- the process's table, as a snapshot -------------------------------------
    {
        FrameTimer& t = frameTimer();
        t.resetForTest();
        g_lines.clear();
        g_asleep = 0;
        t.setClocksForTest(&fakeSteady, &fakeAwake);
        t.setSinkForTest(&sink);
        CHECK(!slowFrameCounts().any());
        g_now += 100'000LL * kMs;
        t.beginFrame(100);
        { FrameScopeGuard g(FrameScope::Recorder); advanceMs(300); }
        t.endFrame();
        t.finish();
        const SlowFrameCounts snap = slowFrameCounts();
        CHECK(snap.count[static_cast<int>(FrameScope::Recorder)][0] == 1);
        CHECK(snap.total() == 1);
        t.resetForTest();
        CHECK(!slowFrameCounts().any());
    }

    // --- WHAT IT COSTS, measured ------------------------------------------------
    // The real clock and the real global timer, in the shape of a frame: a frame
    // begins, a dozen guards open and close, the frame ends. The claim is "far
    // below a millisecond a frame" (a frame is 8 to 16 ms); the bound asserted is
    // generous so a loaded machine cannot fail it, and the measured figures are
    // printed so a change in the cost is visible in the log.
    {
        FrameTimer& t = frameTimer();
        t.resetForTest();
        constexpr int kFrames = 20000;
        constexpr int kGuardsPerFrame = 24;  // what the frame loop opens today, rounded up
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f) {
            t.beginFrame(1000 + f);
            for (int g = 0; g < kGuardsPerFrame; ++g) {
                FrameScopeGuard guard(static_cast<FrameScope>(g % 15));
            }
            t.endFrame();
        }
        const double totalNs =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        const double perFrameNs = totalNs / kFrames;
        const double perGuardNs = perFrameNs / kGuardsPerFrame;
        std::printf("    instrumentation: %.0f ns per guard, %.2f us per frame of %d guards\n",
                    perGuardNs, perFrameNs / 1000.0, kGuardsPerFrame);
        CHECK(perFrameNs < 100'000.0);  // 0.1 ms: ten times what it should be
        // A fast frame records nothing, however many guards it had.
        CHECK(!slowFrameCounts().any());
        t.resetForTest();
    }

    return testSummary("test_frame_timing");
}
