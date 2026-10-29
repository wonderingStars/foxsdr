// test_thread_beats.cpp - a wedged worker is reported by name, and nothing that is merely slow or parked ever is.
//
// docs/engine-stage4.md 6.27. core/thread_beats.hpp is the registry every long-lived worker beats into; the hang
// watchdog's poll reads it and files a hang report that NAMES the thread and the phase it was last in. This file holds
// that to its word, in four parts:
//
//   A. THE REGISTRY, against a clock the test owns (advanceClockForTest), with the PRODUCT'S constants: for every thread
//      in the inventory, a thread that beats is never found late in a simulated day; one that stops is found late at its
//      kind's budget and not a millisecond before, named, with its phase and its OS thread id; it is found once; it
//      recovers when it beats; a declared wait never reports (or reports at its own cap, for the one that has one); an
//      abandoned source thread is not reported; and each kind's budget is at least kMargin times the longest block the
//      kind can legitimately spend - which is what goes red when a budget is halved.
//   B. THE MONITOR, end to end, in real time with short budgets: a real HangWatchdog, a really wedged thread of EACH KIND,
//      and the report that comes out of it - the file exists within the budget plus a poll or two, `thread:` names the
//      thread, `thread-phase:` the phase, the wedged thread's stack is the first one and is labelled with the name, the
//      session goes on (the watchdog still runs, the other threads still beat, no second report is filed for the same
//      wedge), and when the thread comes back the watchdog logs the recovery and re-arms (a second wedge of the same
//      thread is a second report). A thread that beats, and one that is parked in a declared wait, produce nothing in
//      the same seconds.
//   C. THE LEDGER: a reported wedge is counted as `recovered.threadwedge` (read from Registry::wedgesFound() by the engine's pass, as the other recovered
//      counters are), once per wedge.
//   D. THE TX DEAD-MAN: a DSP thread wedged while the radio is keyed is reported with `transmitting: yes`, the monitor
//      does not touch the radio (the sink is still running, nothing was stopped), and the dead-man still does its own
//      job - the control side stops ticking, the key opens at kKeyAliveWait - with the registry running and a wedge
//      already filed. The monitor integrates with the dead-man and does not replace it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/hang_watchdog.hpp"
#include "core/health_events.hpp"
#include "core/sentinel.hpp"
#include "core/telemetry.hpp"
#include "core/thread_beats.hpp"
#include "core/transmitter.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace beats = cascade::core::beats;
using cascade::core::HangWatchdog;
using cascade::core::Transmitter;
using Clock = std::chrono::steady_clock;

namespace {

constexpr unsigned kShortBudgetMs = 1000;  // the seam's budget for the real-time parts

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-threadbeats-") + tag + "-" + std::to_string(pid));
}

std::size_t reportsIn(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("hang-", 0) == 0) { ++n; }
    }
    return n;
}

bool lineOf(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0;
}

// A thread that registers as `spec`, beats once in `phase`, and then does whatever `body` says, until released.
struct Worker {
    explicit Worker(const beats::ThreadSpec& s, const char* ph) : spec(s), phase(ph) {}
    ~Worker() { release(); }

    // Runs the thread: registers, beats `phase`, then calls `body(reg)` (which blocks without beating = a wedge).
    template <class Body>
    void start(Body body) {
        thread = std::thread([this, body]() {
            beats::Registration reg(spec);
            reg.beat(phase);
            tid.store(HangWatchdog::currentThreadIdForTest(), std::memory_order_release);
            ready.store(true, std::memory_order_release);
            body(reg);
        });
        while (!ready.load(std::memory_order_acquire)) { sleepMs(1); }
        started = Clock::now();
    }
    void release() {
        go.store(true, std::memory_order_release);
        if (thread.joinable()) { thread.join(); }
    }
    const beats::ThreadSpec& spec;
    const char* phase;
    std::thread thread;
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<unsigned long> tid{0};
    Clock::time_point started;
};

// ---------------------------------------------------------------------------
// A. THE REGISTRY, with the product's constants and a clock of the test's own
// ---------------------------------------------------------------------------

// The registry's clock stands still while this lives (see Registry::freezeClockForTest): part A is exact to the millisecond, and a
// ThreadSanitizer build spends milliseconds between two lines of a test.
struct FrozenClock {
    FrozenClock() { beats::Registry::instance().freezeClockForTest(true); }
    ~FrozenClock() { beats::Registry::instance().freezeClockForTest(false); }
    FrozenClock(const FrozenClock&) = delete;
    FrozenClock& operator=(const FrozenClock&) = delete;
};

beats::Finding* only(beats::PollResult& r) { return r.wedged.size() == 1 ? &r.wedged[0] : nullptr; }

void testInventoryShape() {
    // the kinds table is in enum order, one row per kind, every word distinct and a plain word
    std::set<std::string> words;
    for (std::size_t i = 0; i < beats::kKindCount; ++i) {
        CHECK(static_cast<std::size_t>(beats::kKinds[i].kind) == i);
        CHECK(beats::kKinds[i].budgetMs > 0);
        CHECK(beats::kKinds[i].legitBlockMs > 0);
        CHECK(std::string(beats::kKinds[i].legitWhat).size() > 20);
        words.insert(beats::kKinds[i].word);
        CHECK(beats::reportWord(beats::kKinds[i].word) == beats::kKinds[i].word);
    }
    CHECK(words.size() == beats::kKindCount);

    // THE MARGIN: a budget is at least kMargin times the longest block the kind legitimately spends. This is the check
    // that goes red when a budget is halved (mutant M2).
    for (std::size_t i = 0; i < beats::kKindCount; ++i) {
        const beats::KindSpec& k = beats::kKinds[i];
        if (k.budgetMs < beats::kMargin * k.legitBlockMs) {
            std::printf("FAIL kind %s: budget %u ms < %u x its legitimate block %u ms\n", k.word, k.budgetMs,
                        beats::kMargin, k.legitBlockMs);
        }
        CHECK(k.budgetMs >= beats::kMargin * k.legitBlockMs);
    }
    // ...and the two figures the brief names: the disk worker on a network share (21 s, B19), and the keeper's debounce.
    CHECK(beats::budgetMs(beats::Kind::Disk) >= 2 * 21000);
    CHECK(beats::budgetMs(beats::Kind::Keeper) >= 2 * 21000);
    CHECK(beats::budgetMs(beats::Kind::Feed) >= 2 * 60000);  // net::kProvisionBound, the password hash

    // the threads: distinct names, plain words, each of a known kind
    std::set<std::string> names;
    for (const beats::ThreadSpec* t : beats::kAllThreads) {
        CHECK(names.insert(t->name).second);
        CHECK(beats::reportWord(t->name) == t->name);
        CHECK(static_cast<std::size_t>(t->kind) < beats::kKindCount);
    }
    CHECK(names.size() == beats::kThreadCount);
    std::set<std::string> waits;
    for (const beats::WaitSpec* w : beats::kAllWaits) {
        CHECK(waits.insert(w->name).second);
        CHECK(beats::reportWord(w->name) == w->name);
        CHECK(std::string(w->argued).size() > 20);
    }

    // reportWord refuses what is not a plain word (it is written into a report)
    CHECK(beats::reportWord(nullptr) == "unknown");
    CHECK(beats::reportWord("") == "unknown");
    CHECK(beats::reportWord("C:\\Users\\someone") == "unknown");
    CHECK(beats::reportWord("Dsp") == "unknown");
    CHECK(beats::reportWord("a b") == "unknown");
    CHECK(beats::reportWord("plugin-process") == "plugin-process");
}

void testBeatingThreadIsNeverReported() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    for (const beats::ThreadSpec* spec : beats::kAllThreads) {
        beats::Registration r(*spec);
        r.beat("work");
        // a simulated day, a beat every 250 ms (the slowest cadence any thread here keeps is the keeper's 100 ms)
        for (int i = 0; i < 4 * 60 * 60 * 24; ++i) {
            reg.advanceClockForTest(250);
            r.beat("work");
            beats::PollResult p = reg.poll();
            if (!p.wedged.empty() || !p.recovered.empty()) {
                std::printf("FAIL %s was reported while beating (iteration %d)\n", spec->name, i);
                CHECK(false);
                break;
            }
        }
    }
    CHECK(reg.size() == 0);
}

void testWedgeIsFoundAtItsBudgetAndNamed() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    for (const beats::ThreadSpec* spec : beats::kAllThreads) {
        const unsigned budget = beats::budgetMs(spec->kind);
        beats::Registration r(*spec);
        r.beat("alpha");
        r.beat("beta");  // the LAST phase is the one reported

        reg.advanceClockForTest(budget - 1);
        {
            beats::PollResult p = reg.poll();
            if (!p.wedged.empty()) { std::printf("FAIL %s reported a millisecond early\n", spec->name); }
            CHECK(p.wedged.empty() && p.recovered.empty());
        }
        reg.advanceClockForTest(1);
        beats::PollResult p = reg.poll();
        beats::Finding* f = only(p);
        CHECK(f != nullptr);
        if (f != nullptr) {
            CHECK(f->name == spec->name);
            CHECK(f->phase == "beta");
            CHECK(f->wait.empty());
            CHECK(f->tid == HangWatchdog::currentThreadIdForTest());
            CHECK(f->budgetMs == budget);
            CHECK(f->stalledMs >= static_cast<std::int64_t>(budget));
        }
        // once: another look, and another hour, find nothing new
        CHECK(reg.poll().wedged.empty());
        reg.advanceClockForTest(3600 * 1000);
        {
            beats::PollResult again = reg.poll();
            CHECK(again.wedged.empty() && again.recovered.empty());
        }
        // it comes back: recovered, once, with how long it was away
        r.beat("gamma");
        beats::PollResult back = reg.poll();
        CHECK(back.wedged.empty());
        CHECK(back.recovered.size() == 1);
        if (back.recovered.size() == 1) {
            CHECK(back.recovered[0].name == spec->name);
            CHECK(back.recovered[0].stalledMs >= static_cast<std::int64_t>(budget));
        }
        CHECK(reg.poll().recovered.empty());
        // and it is armed again: a second wedge is a second finding
        reg.advanceClockForTest(budget);
        beats::PollResult second = reg.poll();
        CHECK(second.wedged.size() == 1);
    }
    CHECK(reg.size() == 0);
}

void testLegitimateBlocksNeverReport() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    // Each kind blocks for the longest legitimate time its row argues - with NOTHING beating in between - and is not
    // reported. This is the check "a budget halved -> the legitimate-block test RED" is made against, with the margin
    // above as its other face.
    for (std::size_t i = 0; i < beats::kKindCount; ++i) {
        const beats::KindSpec& k = beats::kKinds[i];
        const beats::ThreadSpec* spec = nullptr;
        for (const beats::ThreadSpec* t : beats::kAllThreads) {
            if (t->kind == k.kind) { spec = t; break; }
        }
        CHECK(spec != nullptr);  // every kind has at least one thread in the inventory
        if (spec == nullptr) { continue; }
        beats::Registration r(*spec);
        r.beat("block");
        reg.advanceClockForTest(k.legitBlockMs);
        beats::PollResult p = reg.poll();
        if (!p.wedged.empty()) { std::printf("FAIL kind %s reported after a legitimate %u ms block\n", k.word, k.legitBlockMs); }
        CHECK(p.wedged.empty());
        // the NETWORK-SHARE DISK WORKER, by name: a 21 s block, twice over, in a row (a second path behind the first)
        if (k.kind == beats::Kind::Disk) {
            r.beat("write");
            reg.advanceClockForTest(21000);
            CHECK(reg.poll().wedged.empty());
            r.beat("write");
            reg.advanceClockForTest(21000);
            CHECK(reg.poll().wedged.empty());
        }
    }
}

void testDeclaredWaits() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    // A wait with no cap never reports, however long: a server in accept() with nobody connecting is idle, not wedged.
    {
        beats::Registration r(beats::kThreadCatAccept);
        r.beat("accept-loop");
        r.wait(beats::kWaitAccept);
        reg.advanceClockForTest(48LL * 3600 * 1000);
        beats::PollResult p = reg.poll();
        CHECK(p.wedged.empty() && p.recovered.empty());
        // ...and leaving it re-arms the ordinary budget from NOW, not from before the wait
        r.endWait("handle");
        reg.advanceClockForTest(beats::budgetMs(beats::Kind::Server) - 1);
        CHECK(reg.poll().wedged.empty());
        reg.advanceClockForTest(1);
        beats::PollResult late = reg.poll();
        CHECK(late.wedged.size() == 1);
        if (late.wedged.size() == 1) {
            CHECK(late.wedged[0].name == "cat-accept");
            CHECK(late.wedged[0].phase == "handle");
        }
    }
    CHECK(reg.poll().wedged.empty());
    // A wait with a cap reports at the cap, and names the wait.
    {
        beats::Registration r(beats::kThreadDriverInstall);
        r.beat("start");
        r.wait(beats::kWaitInstaller);
        reg.advanceClockForTest(beats::kWaitInstaller.capMs - 1);
        CHECK(reg.poll().wedged.empty());
        reg.advanceClockForTest(1);
        beats::PollResult p = reg.poll();
        CHECK(p.wedged.size() == 1);
        if (p.wedged.size() == 1) {
            CHECK(p.wedged[0].name == "driver-install");
            CHECK(p.wedged[0].wait == "installer");
            CHECK(p.wedged[0].phase == "start");
            CHECK(p.wedged[0].budgetMs == beats::kWaitInstaller.capMs);
        }
    }
    // The scope form parks and un-parks.
    {
        beats::Registration r(beats::kThreadImageFeed);
        r.beat("idle");
        {
            beats::WaitScope w(r, beats::kWaitJob);
            reg.advanceClockForTest(10LL * 3600 * 1000);
            CHECK(reg.poll().wedged.empty());
        }
        reg.advanceClockForTest(beats::budgetMs(beats::Kind::Feed) - 1);
        CHECK(reg.poll().wedged.empty());
    }
    // A parked thread's snapshot line says so.
    {
        beats::Registration r(beats::kThreadServerHost);
        r.beat("hash");
        r.wait(beats::kWaitJob);
        const std::string snap = reg.snapshotText();
        CHECK(snap.find("server-host") != std::string::npos);
        CHECK(snap.find("wait=job") != std::string::npos);
        CHECK(snap.find("phase=hash") != std::string::npos);
    }
}

void testAbandonedThreadIsNotReported() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    auto live = std::make_shared<std::atomic<bool>>(true);
    beats::Registration r(beats::kThreadSource, live);
    r.beat("read");
    reg.advanceClockForTest(beats::budgetMs(beats::Kind::Source) / 2);
    live->store(false);  // the owner gave up on the thread and counted it (Recovered::SrcThread)
    reg.advanceClockForTest(beats::budgetMs(beats::Kind::Source));
    beats::PollResult p = reg.poll();
    CHECK(p.wedged.empty());
}

void testRearmAfterLostTime() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    beats::Registration r(beats::kThreadDsp);
    r.beat("block");
    reg.advanceClockForTest(60LL * 60 * 1000);  // the machine slept an hour
    reg.rearm();                                // what the watchdog does when it sees its own poll overshoot
    CHECK(reg.poll().wedged.empty());
}

void testRegistrationLifetime() {
    auto& reg = beats::Registry::instance();
    const FrozenClock frozen;  // the boundary tests below are exact: the clock runs only when the test advances it
    const std::size_t before = reg.size();
    {
        beats::Registration a(beats::kThreadDsp);
        beats::Registration b(beats::kThreadDsp);  // two of one name are fine (per-client threads)
        CHECK(reg.size() == before + 2);
        reg.advanceClockForTest(beats::budgetMs(beats::Kind::Dsp));
        beats::PollResult p = reg.poll();
        CHECK(p.wedged.size() == 2);
    }
    CHECK(reg.size() == before);
    // a registration that ended while reported leaves nothing behind
    CHECK(reg.poll().wedged.empty() && reg.poll().recovered.empty());
}

// Many threads registering, beating, parking and ending while one polls: no false report, nothing torn (TSan's leg).
void testConcurrentRegistryUse() {
    auto& reg = beats::Registry::instance();
    reg.setBudgetOverrideForTest(beats::Kind::Dsp, 4000);
    reg.setBudgetOverrideForTest(beats::Kind::Server, 4000);
    std::atomic<bool> stop{false};
    std::atomic<int> falseReports{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&stop, i]() {
            while (!stop.load(std::memory_order_relaxed)) {
                beats::Registration r(i % 2 == 0 ? beats::kThreadDsp : beats::kThreadWebIdle);
                for (int k = 0; k < 200 && !stop.load(std::memory_order_relaxed); ++k) {
                    r.beat(k % 2 == 0 ? "even" : "odd");
                    if (k % 50 == 0) {
                        r.wait(beats::kWaitJob);
                        r.endWait("woke");
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        });
    }
    std::thread poller([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            beats::PollResult p = reg.poll();
            falseReports += static_cast<int>(p.wedged.size());
            (void)reg.snapshotText();
            reg.rearm();
            sleepMs(2);
        }
    });
    sleepMs(1500);
    stop.store(true);
    for (std::thread& t : workers) { t.join(); }
    poller.join();
    reg.clearOverridesForTest();
    CHECK(falseReports.load() == 0);
    CHECK(reg.size() == 0);
}

// ---------------------------------------------------------------------------
// B. THE MONITOR, end to end: a real watchdog, a really wedged thread of each kind
// ---------------------------------------------------------------------------

// The first inventory thread of each kind, with the phase the wedged body last said.
struct KindCase {
    const beats::ThreadSpec* spec;
    const char* phase;
};

void testWedgedWorkerOfEachKind() {
    auto& reg = beats::Registry::instance();
    // one case per KIND (the kind decides the budget): the first inventory thread of that kind
    std::vector<KindCase> cases;
    for (std::size_t i = 0; i < beats::kKindCount; ++i) {
        for (const beats::ThreadSpec* t : beats::kAllThreads) {
            if (static_cast<std::size_t>(t->kind) == i) {
                cases.push_back({t, "mid-step"});
                break;
            }
        }
    }
    CHECK(cases.size() == beats::kKindCount);

    const fs::path dir = scratchDir("kinds");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    // The GUI threshold is far away: only a worker can be what is reported here.
    w.start(dir.string(), 600000);

    // A thread that beats, and one parked in a declared wait, for the whole run: they must produce nothing.
    std::atomic<bool> quiet{false};
    std::thread beater([&quiet]() {
        beats::Registration r(beats::kThreadWebIdle);
        while (!quiet.load(std::memory_order_relaxed)) {
            r.beat("tick");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::thread parker([&quiet]() {
        beats::Registration r(beats::kThreadCatAccept);
        r.beat("accept-loop");
        r.wait(beats::kWaitAccept);
        while (!quiet.load(std::memory_order_relaxed)) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    });
    // (their kinds share the Server budget below, so the override applies to them too: they must stay quiet at 1 s)

    unsigned expectedReports = 0;
    for (const KindCase& c : cases) {
        HangWatchdog::resetWedgeFileCountForTest();  // nine cases, and the cap is eight files
        reg.setBudgetOverrideForTest(c.spec->kind, kShortBudgetMs);
        // the wedge: registers, beats, then blocks without beating until released
        Worker wk(*c.spec, c.phase);
        wk.start([&wk](beats::Registration& r) {
            while (!wk.go.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            r.beat("released");
        });
        const auto t0 = wk.started;
        const std::size_t before = reportsIn(dir);

        // THE REPORT ARRIVES WITHIN THE BUDGET PLUS A POLL OR TWO AND THE CAPTURE, and not before the budget.
        const auto deadline = t0 + std::chrono::milliseconds(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 3000);
        while (Clock::now() < deadline && reportsIn(dir) == before) { sleepMs(10); }
        const double arrivedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        CHECK(reportsIn(dir) == before + 1);
        ++expectedReports;
        std::printf("wedge of kind %-8s (%s): report after %.0f ms (budget %u ms)\n", beats::kindSpec(c.spec->kind).word,
                    c.spec->name, arrivedMs, kShortBudgetMs);
        CHECK(arrivedMs >= static_cast<double>(kShortBudgetMs) - 30.0);
        CHECK(arrivedMs <= static_cast<double>(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 3000));

        // THE REPORT NAMES THE THREAD AND THE PHASE
        sleepMs(300);  // the capture writes the header first and the stacks after
        const std::string text = readFile(fs::path(w.lastReportPath()));
        CHECK(!text.empty());
        // (a developer's hook: THREAD_BEATS_SAMPLE_DIR=<dir> keeps a copy of each report, for the record in docs/engine-stage4.md)
        if (const char* keep = std::getenv("THREAD_BEATS_SAMPLE_DIR")) {
            std::ofstream(fs::path(keep) / (std::string("wedge-") + c.spec->name + ".txt"), std::ios::binary) << text;
        }
        CHECK(lineOf(text, "kind: hang"));
        CHECK(lineOf(text, std::string("thread: ") + c.spec->name));
        CHECK(lineOf(text, std::string("thread-phase: ") + c.phase));
        CHECK(lineOf(text, "thread-wait: none"));
        CHECK(lineOf(text, "threshold-ms: " + std::to_string(kShortBudgetMs)));
        CHECK(text.find("\nthread: ") < text.find("--- context ---"));
        CHECK(text.find("--- beats ---") != std::string::npos);
        CHECK(text.find(std::string("  ") + c.spec->name + " age-ms=") != std::string::npos);
        // the wedged thread's stack is in it, first, and is labelled with the name
        const std::string label = "--- thread " + std::to_string(wk.tid.load()) + " (" + c.spec->name + ", stalled) ---";
        CHECK(text.find(label) != std::string::npos);
        CHECK(text.find("--- thread ") == text.find(label));
        CHECK(text.find("(gui, stalled)") == std::string::npos);

        // THE SESSION GOES ON: the watchdog still runs, and the same wedge is not reported again
        CHECK(w.running());
        sleepMs(HangWatchdog::kPollMs * 3);
        CHECK(reportsIn(dir) == before + 1);
        CHECK(w.reportsWritten() == expectedReports);

        // RECOVERY: released, the thread beats and is re-armed (the watchdog logs it; a second wedge reports again below)
        wk.release();
        reg.clearOverridesForTest();
        sleepMs(HangWatchdog::kPollMs * 2 + 100);
    }

    // The beater and the parker, through all of that, never produced a report of their own: exactly one per case.
    quiet.store(true);
    beater.join();
    parker.join();
    CHECK(reportsIn(dir) == cases.size());
    CHECK(w.reportsWritten() == cases.size());
    w.stop();
    fs::remove_all(dir, ec);
}

// A thread that wedges, comes back and wedges again is reported twice: the first recovery re-armed it.
void testSecondWedgeOfTheSameThread() {
    auto& reg = beats::Registry::instance();
    const fs::path dir = scratchDir("again");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Disk, kShortBudgetMs);
    {
        beats::Registration r(beats::kThreadDiskJob);
        for (int round = 0; round < 2; ++round) {
            r.beat("write");
            const auto until = Clock::now() + std::chrono::milliseconds(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 2500);
            while (Clock::now() < until && w.reportsWritten() < static_cast<unsigned>(round + 1)) { sleepMs(10); }
            CHECK(w.reportsWritten() == static_cast<unsigned>(round + 1));
            r.beat("done");
            // One poll must see the beat (the recovery) and the silence must stay UNDER the budget, or the pause between the rounds
            // is itself a third wedge - which is what a pause of 2 polls + 100 ms did, under load, in the first full sweep.
            sleepMs(HangWatchdog::kPollMs + 250);
        }
    }
    w.stop();
    reg.clearOverridesForTest();
    CHECK(reportsIn(dir) == 2);
    fs::remove_all(dir, ec);
}

// Lost time and a debugger never accuse a worker (the two rules the frame loop's watchdog already keeps), and a worker's report is
// not the frame loop's: the sentinel's "the watchdog filed a freeze report after the last sign of life" match does not see it.
void testLostTimeAndDebuggerNeverReport() {
    auto& reg = beats::Registry::instance();
    const fs::path dir = scratchDir("rules");
    std::error_code ec;
    for (int mode = 0; mode < 4; ++mode) {
        // 0 control (reported)  1 lost time (never)  2 a debugger (never)  3 a debugger gone again (reported)
        HangWatchdog::resetWedgeFileCountForTest();
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        HangWatchdog w;
        w.setSuppressionForTest(mode == 2 ? HangWatchdog::SuppressionForTest::AlwaysSuppress
                                          : HangWatchdog::SuppressionForTest::NeverSuppress);
        if (mode == 1) { w.setBeatsOvershootMsForTest(0); }  // every poll "arrived late": the process lost time
        w.start(dir.string(), 600000);
        reg.setBudgetOverrideForTest(beats::Kind::Disk, kShortBudgetMs);
        {
            beats::Registration r(beats::kThreadDiskJob);
            r.beat("write");
            sleepMs(kShortBudgetMs + 5 * HangWatchdog::kPollMs);
            const bool reported = w.reportsWritten() == 1;
            const bool expected = (mode == 0 || mode == 3);
            if (reported != expected) { std::printf("FAIL mode %d: reported=%d expected=%d\n", mode, reported ? 1 : 0, expected ? 1 : 0); }
            CHECK(reported == expected);
            if (mode == 3) {
                // the freeze reports the SENTINEL looks for are the frame loop's `hang-<pid>-<digits>.txt`; a worker's is not one
                const auto since = std::chrono::system_clock::now() - std::chrono::hours(1);
#if defined(_WIN32)
                const unsigned long pid = ::GetCurrentProcessId();
#else
                const unsigned long pid = 0;
#endif
                CHECK(reportsIn(dir) == 1);
                CHECK(!cascade::core::freezeReportWrittenSince(dir.string(), pid, since));
                // the control: a frame-loop-style name IS seen
                std::ofstream((dir / ("hang-" + std::to_string(pid) + "-1.txt")).string()) << "kind: hang\n";
                CHECK(cascade::core::freezeReportWrittenSince(dir.string(), pid, since));
            }
            r.beat("back");
        }
        w.stop();
        reg.clearOverridesForTest();
    }
    fs::remove_all(dir, ec);
}

// Time the process lost is not a wedge, and it is not FORGIVEN EARLY either: when the watchdog sees its own poll arrive late
// (a machine asleep, a paused VM) it RE-ARMS every registration, so a worker that was silent through the lost time gets its
// whole budget again from there. With the lateness seam on, a thread silent for longer than its budget is not found; with the seam
// off again, it is found a budget after the LAST re-arm and not at the first ordinary poll - which is what removing the re-arm
// (leaving the stale stamps to be read) would do.
void testLostTimeRearmsRegistrations() {
    auto& reg = beats::Registry::instance();
    constexpr unsigned kBudget = 2000;
    const fs::path dir = scratchDir("rearm");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setBeatsOvershootMsForTest(0);  // every poll arrives "late": the process keeps losing time
    w.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Disk, kBudget);
    {
        beats::Registration r(beats::kThreadDiskJob);
        r.beat("write");
        sleepMs(kBudget + 400);               // silent for longer than the budget, all of it "lost time"
        CHECK(w.reportsWritten() == 0);
        w.setBeatsOvershootMsForTest(HangWatchdog::kBeatsOvershootMs);  // time is passing normally again
        // The last re-arm was at most one poll before this: a report is a whole budget after it - between 1.5 and 2 s from now,
        // and a registration left stale would be found at the very next poll, within half a second.
        sleepMs(1200);
        if (w.reportsWritten() != 0) { std::printf("FAIL the silent worker was found %u ms after time came back; it had been re-armed\n", 1200u); }
        CHECK(w.reportsWritten() == 0);
        // ...and it IS found, a budget after the re-arm: the lost time was not forgiven for ever
        const auto until = Clock::now() + std::chrono::milliseconds(kBudget + 4 * HangWatchdog::kPollMs + 2000);
        while (Clock::now() < until && w.reportsWritten() < 1) { sleepMs(10); }
        CHECK(w.reportsWritten() == 1);
        r.beat("back");
    }
    w.stop();
    reg.clearOverridesForTest();
    fs::remove_all(dir, ec);
}

// A stuck machine wedges every thread at once: the FILES are capped at kMaxWedgeReports, the count and the log are not.
void testWedgeFilesAreCapped() {
    namespace health = cascade::core::health;
    auto& reg = beats::Registry::instance();
    const fs::path dir = scratchDir("cap");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto ledger = health::globalLedger();
    ledger->reset();
    ledger->arm("", cascade::core::newInstallId(), false);
    health::RecoveryWatch watch;  // the engine's pass owns one; the first poll only takes the baseline
    {
        health::RecoveryReadings base;
        base.threadWedges = beats::Registry::wedgesFound();
        watch.poll(base);
    }
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Disk, kShortBudgetMs);
    constexpr unsigned kWedged = HangWatchdog::kMaxWedgeReports + 4;
    {
        std::vector<std::unique_ptr<beats::Registration>> regs;
        for (unsigned i = 0; i < kWedged; ++i) {
            regs.push_back(std::make_unique<beats::Registration>(beats::kThreadDiskJob));
            regs.back()->beat("write");
        }
        sleepMs(kShortBudgetMs + 6 * HangWatchdog::kPollMs + 3000);
        for (auto& r : regs) { r->beat("back"); }
    }
    {
        health::RecoveryReadings now;  // what Engine::pollRecovery reads each pass
        now.threadWedges = beats::Registry::wedgesFound();
        watch.poll(now);
    }
    CHECK(reportsIn(dir) == HangWatchdog::kMaxWedgeReports);
    const auto counts = ledger->counts();
    const auto it = counts.find("recovered.threadwedge");
    CHECK(it != counts.end() && it->second == kWedged);
    ledger->reset();
    w.stop();
    reg.clearOverridesForTest();
    fs::remove_all(dir, ec);
}

// A report directory that is empty writes no file ("off means off") and the watchdog keeps running.
void testNoReportDirNoFile() {
    auto& reg = beats::Registry::instance();
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start("", 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Disk, kShortBudgetMs);
    {
        beats::Registration r(beats::kThreadDiskJob);
        r.beat("write");
        sleepMs(kShortBudgetMs + 3 * HangWatchdog::kPollMs);
        CHECK(w.reportsWritten() == 0);
        CHECK(w.running());
        r.beat("back");
    }
    w.stop();
    reg.clearOverridesForTest();
}

// ---------------------------------------------------------------------------
// C. THE LEDGER
// ---------------------------------------------------------------------------

void testWedgeIsCountedInTheLedger() {
    namespace health = cascade::core::health;
    auto& reg = beats::Registry::instance();
    // the word exists, is legal, and is counted once an occurrence
    CHECK(health::validToken("recovered.threadwedge"));
    CHECK(!health::recoveredOncePerSession(health::Recovered::ThreadWedge));

    const fs::path dir = scratchDir("ledger");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Disk, kShortBudgetMs);

    // A scratch in-memory ledger armed on the process-wide one (the file path is empty: nothing is written to a profile).
    auto ledger = health::globalLedger();
    ledger->reset();
    ledger->arm("", cascade::core::newInstallId(), false);
    health::RecoveryWatch watch;
    {
        health::RecoveryReadings base;
        base.threadWedges = beats::Registry::wedgesFound();
        watch.poll(base);
    }
    {
        beats::Registration r(beats::kThreadDiskJob);
        r.beat("write");
        const auto until = Clock::now() + std::chrono::milliseconds(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 2500);
        while (Clock::now() < until && w.reportsWritten() < 1) { sleepMs(10); }
        CHECK(w.reportsWritten() == 1);
        r.beat("back");
    }
    {
        health::RecoveryReadings now;  // what Engine::pollRecovery reads each pass
        now.threadWedges = beats::Registry::wedgesFound();
        watch.poll(now);
    }
    std::uint32_t n = 0;
    const auto counts = ledger->counts();
    const auto it = counts.find("recovered.threadwedge");
    if (it != counts.end()) { n = it->second; }
    std::printf("ledger holds %zu token(s): %s\n", counts.size(), cascade::core::health::encode(counts).c_str());
    CHECK(n == 1);
    ledger->reset();
    w.stop();
    reg.clearOverridesForTest();
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// D. THE TX DEAD-MAN
// ---------------------------------------------------------------------------

class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override { running_ = true; ++starts; return true; }
    void stop() override { if (running_) { ++stops; } running_ = false; }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.0e5; hi = 6.0e7; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.0; hi = 0.0; return true; }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return ""; }
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};

private:
    std::atomic<bool> running_{false};
};

void testDspWedgeWhileKeyedLeavesTheDeadManAlone() {
    auto& reg = beats::Registry::instance();
    const fs::path dir = scratchDir("txdm");
    HangWatchdog::resetWedgeFileCountForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    Transmitter tx;
    auto sink = std::make_unique<RecordingSink>();
    RecordingSink* raw = sink.get();
    tx.setSink(std::move(sink));
    tx.setInput(cascade::core::TxInput::Tone);
    // The engine installs this probe; the test installs the same one.
    beats::Registry::setKeyedProbe(&tx, [&tx]() { return tx.transmitting(); });

    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Dsp, kShortBudgetMs);

    // Keyed: latched, with a control side that ticks (the way the pass does) on this thread.
    tx.setLatched(true);
    tx.frontEndAlive();
    tx.tick();
    CHECK(tx.transmitting());

    // The DSP thread wedges. The control side goes on ticking: it is another thread.
    Worker dsp(beats::kThreadDsp, "plugin-process");
    dsp.start([&dsp](beats::Registration&) {
        while (!dsp.go.load(std::memory_order_acquire)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    });
    const auto until = Clock::now() + std::chrono::milliseconds(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 3000);
    while (Clock::now() < until && w.reportsWritten() < 1) {
        tx.frontEndAlive();
        tx.tick();
        sleepMs(5);
    }
    CHECK(w.reportsWritten() == 1);
    sleepMs(300);
    const std::string text = readFile(fs::path(w.lastReportPath()));
    CHECK(lineOf(text, "thread: dsp"));
    CHECK(lineOf(text, "thread-phase: plugin-process"));
    // THE REPORT SAYS THE RADIO WAS KEYED, and the monitor did nothing to it
    CHECK(lineOf(text, "transmitting: yes"));
    CHECK(tx.transmitting());
    CHECK(raw->stops.load() == 0);

    // THE DEAD-MAN IS STILL THE DEAD-MAN: the control side stops ticking, and the key opens at kKeyAliveWait - with the
    // wedge already filed and the registry still running
    // The FRONT END keeps saying it is alive (the handle on the window's heartbeat is satisfied) and only the CONTROL side - the
    // pass that applies the key rules - stops: the handle under test is the one that watches tick().
    const auto quietFrom = Clock::now();
    while (Clock::now() < quietFrom + std::chrono::milliseconds(Transmitter::kKeyAliveWait.count() + 400)) {
        tx.frontEndAlive();
        sleepMs(5);
    }
    const double waited = std::chrono::duration<double, std::milli>(Clock::now() - quietFrom).count();
    CHECK(raw->stops.load() >= 1);
    CHECK(waited < static_cast<double>(Transmitter::kKeyAliveWait.count()) + 1500.0);
    tx.frontEndAlive();
    tx.tick();
    CHECK(!tx.transmitting());

    dsp.release();
    beats::Registry::clearKeyedProbe(&tx);
    reg.clearOverridesForTest();
    w.stop();

    // and a report written with the radio quiet says so
    HangWatchdog::resetWedgeFileCountForTest();
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    HangWatchdog w2;
    w2.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w2.start(dir.string(), 600000);
    reg.setBudgetOverrideForTest(beats::Kind::Dsp, kShortBudgetMs);
    static int quietOwner = 0;
    beats::Registry::setKeyedProbe(&quietOwner, []() { return false; });
    {
        beats::Registration r(beats::kThreadDsp);
        r.beat("block");
        const auto until2 = Clock::now() + std::chrono::milliseconds(kShortBudgetMs + 4 * HangWatchdog::kPollMs + 3000);
        while (Clock::now() < until2 && w2.reportsWritten() < 1) { sleepMs(10); }
        CHECK(w2.reportsWritten() == 1);
        sleepMs(300);
        CHECK(lineOf(readFile(fs::path(w2.lastReportPath())), "transmitting: no"));
        r.beat("back");
    }
    beats::Registry::clearKeyedProbe(&quietOwner);
    reg.clearOverridesForTest();
    w2.stop();
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    testInventoryShape();
    testBeatingThreadIsNeverReported();
    testWedgeIsFoundAtItsBudgetAndNamed();
    testLegitimateBlocksNeverReport();
    testDeclaredWaits();
    testAbandonedThreadIsNotReported();
    testRearmAfterLostTime();
    testRegistrationLifetime();
    testConcurrentRegistryUse();
    testWedgedWorkerOfEachKind();
    testSecondWedgeOfTheSameThread();
    testWedgeFilesAreCapped();
    testLostTimeAndDebuggerNeverReport();
    testLostTimeRearmsRegistrations();
    testNoReportDirNoFile();
    testWedgeIsCountedInTheLedger();
    testDspWedgeWhileKeyedLeavesTheDeadManAlone();
    return testSummary("test_thread_beats");
}
