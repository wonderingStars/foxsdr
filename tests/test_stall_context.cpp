// test_stall_context.cpp - A FREEZE REPORT SAYS WHO STALLED AND WHAT IT WAITED FOR (freeze diagnosis section 7).
//
// The 17 field freeze reports could not name their cause. Each case below stages the stall with the product's own HangWatchdog and reads the report it wrote:
//   A  the control thread's watchdog: `watchdog: control`, the pass's phase, and the frame scope NOT copied from the window (it said where the WINDOW was)
//   B  a thread inside a plugin's create(): the plugin's public id, its version, the entry and `in-call` are named, though no call budget was exceeded
//   C  a thread waiting for a tagged lock whose holder is inside a plugin call: the lock, the holder's role, the site, how long, and the plugin (`lock-holder`)
//   D  the outcome: a stall that ends appends `recovered-after-ms` within 600 ms of its true length; one that does not end says `pending` and nothing follows
//   E  TaggedMutex itself: free, held, waited-for, and the waiting list gives the thread back
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/frame_timing.hpp"
#include "core/hang_watchdog.hpp"
#include "core/plugin_fault.hpp"
#include "core/stall_context.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using Clock = std::chrono::steady_clock;
namespace stall = cascade::core::stall;
namespace fault = cascade::core::fault;

namespace {

fs::path scratch(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("foxsdr-stallctx-") + tag + "-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<fs::path> reportsIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind("hang-", 0) == 0) { out.push_back(e.path()); }
    }
    return out;
}

bool hasLine(const std::string& text, const std::string& line) { return text.find("\n" + line + "\n") != std::string::npos; }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        sleepMs(5);
    }
    return p();
}

// A watchdog with a short threshold that reports a stall of THIS thread (the thread that beats is the one it judges).
struct Dog {
    HangWatchdog w;
    fs::path dir;
    explicit Dog(const char* tag, HangWatchdog::Role role, unsigned thresholdMs = 600) : dir(scratch(tag)) {
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.setRole(role);
        w.start(dir.string(), thresholdMs);
        for (int i = 0; i < 5; ++i) { w.heartbeat(); sleepMs(20); }
    }
    ~Dog() { w.stop(); std::error_code ec; fs::remove_all(dir, ec); }
};

// ---------------------------------------------------------------- A
void testControlWatchdog() {
    std::printf("A: the control thread's watchdog names itself, the pass phase, and does not copy the window's frame scope\n");
    stall::registerThreadRole(stall::Role::Control);
    Dog d("control", HangWatchdog::Role::Control);
    stall::setPassPhase(stall::PassPhase::Plugins);
    {
        // The WINDOW is in `rail` (one process-wide atomic the window's frame loop writes); this thread is the control thread and is stalled in its own pass.
        cascade::core::FrameScopeGuard inRail(cascade::core::FrameScope::Rail);
        sleepMs(1500);
    }
    d.w.heartbeat();
    stall::setPassPhase(stall::PassPhase::Idle);
    const std::vector<fs::path> files = reportsIn(d.dir);
    CHECK(files.size() == 1);
    if (files.size() != 1) { return; }
    const std::string text = readFile(files[0]);
    CHECK(hasLine(text, "watchdog: control"));
    CHECK(hasLine(text, "thread-role: control"));
    CHECK(hasLine(text, "pass-phase: plugins"));
    CHECK(hasLine(text, "frame-scope: other"));   // NOT `rail`
    CHECK(text.find("the engine's control thread did not complete a pass within the threshold") != std::string::npos);
    CHECK(hasLine(text, "blamed-plugin: none"));
    CHECK(hasLine(text, "waiting-lock: none"));
    std::printf("  control report: watchdog control, pass-phase plugins, frame-scope other\n");
}

// ---------------------------------------------------------------- B
void testInCallBlame() {
    std::printf("B: a thread stalled inside a plugin's create() names the plugin, its version and the entry\n");
    stall::registerThreadRole(stall::Role::Window);
    fault::resetForTest();
    fault::setIdentity("dvb-receiver", "0.4.0", fault::Origin::Official, nullptr, 0);
    fault::Slot* slot = fault::slotFor("dvb-receiver");
    Dog d("incall", HangWatchdog::Role::Window);
    fault::call(slot, fault::Entry::Create, [] { sleepMs(1500); });   // (the create budget is 30 s: no call report, only the watchdog's)
    d.w.heartbeat();
    const std::vector<fs::path> files = reportsIn(d.dir);
    CHECK(files.size() == 1);
    if (files.size() != 1) { return; }
    const std::string text = readFile(files[0]);
    CHECK(hasLine(text, "watchdog: window"));
    CHECK(hasLine(text, "blamed-plugin: dvb-receiver"));
    CHECK(hasLine(text, "blamed-plugin-version: 0.4.0"));
    CHECK(hasLine(text, "blamed-plugin-entry: create"));
    CHECK(hasLine(text, "blamed-plugin-how: in-call"));
    std::printf("  window report: dvb-receiver 0.4.0 create in-call\n");
}

// ---------------------------------------------------------------- C
void testLockHolderBlame() {
    std::printf("C: a thread waiting for a tagged lock whose holder is inside a plugin call names the lock, the holder, the site and the plugin\n");
    stall::registerThreadRole(stall::Role::Window);
    fault::resetForTest();
    fault::setIdentity("dvb-receiver", "0.4.0", fault::Origin::Official, nullptr, 0);
    fault::Slot* slot = fault::slotFor("dvb-receiver");
    stall::TaggedMutex runnerLock("plugin-runner");   // the product's name for PluginRunner::mutex_
    Dog d("lock", HangWatchdog::Role::Window);

    std::atomic<bool> held{false};
    std::thread holder([&] {
        stall::registerThreadRole(stall::Role::Control);
        fault::call(slot, fault::Entry::Create, [&] {
            const stall::SiteScope site("start");
            std::lock_guard<stall::TaggedMutex> lk(runnerLock);
            held = true;
            sleepMs(1600);
        });
    });
    CHECK(waitFor([&] { return held.load(); }, 2000));
    {
        // The stalled thread: the window, in pollImages-like wait for the lock (here a plain lock; the lock is the witness).
        const stall::SiteScope site("poll-images");
        std::lock_guard<stall::TaggedMutex> lk(runnerLock);
    }
    holder.join();
    d.w.heartbeat();
    const std::vector<fs::path> files = reportsIn(d.dir);
    CHECK(files.size() == 1);
    if (files.size() != 1) { return; }
    const std::string text = readFile(files[0]);
    CHECK(hasLine(text, "waiting-lock: plugin-runner"));
    CHECK(hasLine(text, "lock-holder-role: control"));
    CHECK(hasLine(text, "lock-site: start"));
    CHECK(text.find("\nlock-held-ms: ") != std::string::npos && !hasLine(text, "lock-held-ms: 0"));
    CHECK(hasLine(text, "blamed-plugin: dvb-receiver"));
    CHECK(hasLine(text, "blamed-plugin-version: 0.4.0"));
    CHECK(hasLine(text, "blamed-plugin-entry: create"));
    CHECK(hasLine(text, "blamed-plugin-how: lock-holder"));
    std::printf("  window report: waiting for plugin-runner, held by control at start, plugin dvb-receiver 0.4.0 lock-holder\n");
}

// ---------------------------------------------------------------- D
void testOutcome() {
    std::printf("D: a stall that ends appends its length; one that does not end is `pending` with nothing after it\n");
    stall::registerThreadRole(stall::Role::Window);
    {
        Dog d("recover", HangWatchdog::Role::Window);
        const auto began = Clock::now();
        sleepMs(1700);
        d.w.heartbeat();   // the stall ends
        const double trueMs = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        CHECK(waitFor([&] { const auto f = reportsIn(d.dir); return f.size() == 1 && readFile(f[0]).find("--- outcome ---") != std::string::npos; }, 3000));
        const std::vector<fs::path> files = reportsIn(d.dir);
        CHECK(files.size() == 1);
        if (files.size() == 1) {
            const std::string text = readFile(files[0]);
            CHECK(hasLine(text, "outcome: pending"));
            const std::size_t at = text.find("recovered-after-ms: ");
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                const double ms = std::atof(text.c_str() + at + 20);
                // measured from the last beat before the stall: the stall's true length is the sleep, plus what the beats before it had left
                std::printf("  recovered-after-ms %.0f for a stall measured at %.0f ms\n", ms, trueMs);
                CHECK(ms > trueMs - 600.0 && ms < trueMs + 600.0);
            }
            CHECK(text.rfind("--- outcome ---") > text.find("--- log"));   // appended at the end of the file
        }
    }
    {
        Dog d("pending", HangWatchdog::Role::Window);
        sleepMs(1400);   // reported, and never recovered while the watchdog runs
        const std::vector<fs::path> files = reportsIn(d.dir);
        CHECK(files.size() == 1);
        if (files.size() == 1) {
            const std::string text = readFile(files[0]);
            CHECK(hasLine(text, "outcome: pending"));
            CHECK(text.find("--- outcome ---") == std::string::npos);
        }
        d.w.heartbeat();
    }
}

// ---------------------------------------------------------------- E
void testTaggedMutex() {
    std::printf("E: TaggedMutex reads free, held and waited-for\n");
    stall::TaggedMutex m("diag-log-file");
    stall::LockReading r = m.read(stall::currentTid());
    CHECK(r.holderTid == 0 && !r.waiterFound);
    std::atomic<bool> got{false}, go{false};
    std::thread holder([&] {
        stall::registerThreadRole(stall::Role::Dsp);
        const stall::SiteScope site("process-iq");
        std::lock_guard<stall::TaggedMutex> lk(m);
        got = true;
        while (!go.load()) { sleepMs(2); }
    });
    CHECK(waitFor([&] { return got.load(); }, 2000));
    r = m.read(stall::currentTid());
    CHECK(r.holderTid != 0 && r.holderTid != stall::currentTid());
    CHECK(std::string(r.site) == "process-iq");
    CHECK(!m.try_lock());
    std::atomic<unsigned long> waiterTid{0};
    std::thread waiter([&] {
        waiterTid = stall::currentTid();
        std::lock_guard<stall::TaggedMutex> lk(m);
    });
    CHECK(waitFor([&] { return waiterTid.load() != 0 && m.read(waiterTid.load()).waiterFound; }, 2000));
    stall::LockReading seen;
    CHECK(stall::lockWaitedBy(waiterTid.load(), seen));
    CHECK(std::string(seen.name) == "diag-log-file");
    CHECK(stall::roleOfThread(seen.holderTid) == stall::Role::Dsp);
    go = true;
    holder.join();
    waiter.join();
    r = m.read(waiterTid.load());
    CHECK(!r.waiterFound && r.holderTid == 0);
    CHECK(m.try_lock());
    m.unlock();
}

}  // namespace

int main() {
    std::printf("test_stall_context\n");
    testTaggedMutex();
    testControlWatchdog();
    testInCallBlame();
    testLockHolderBlame();
    testOutcome();
    return testSummary("test_stall_context");
}
