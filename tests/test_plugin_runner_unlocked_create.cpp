// test_plugin_runner_unlocked_create.cpp - NO PLUGIN'S create() RUNS WITH THE RUNNER'S LOCK HELD (freeze diagnosis P1/P6, host stage A).
//
// The field: dvb-receiver 0.4.0's create() shelled out and waited for cmd.exe with no time limit. The control thread sat in it with PluginRunner::mutex_
// held, and the window thread (pollImages) and the DSP thread (processIq) stopped behind that lock: six freeze reports. This pins the host half of the
// cure: a fake plugin whose create() sleeps ten seconds is started on one thread while two others do what the window and the DSP do, and
//   - no pollImages and no processIq call is held up by more than 20 ms beyond what the same call costs with no start in progress;
//   - the plugin reads "starting" in the row state meanwhile;
//   - when create() returns the plugin runs normally and is fed the next block.
// Then the interleavings the lock used to rule out: stop during the start, a rate change (rebuild) during the start, the modules unloaded (clear) during the
// start, two plugins starting at once, a second start of the same plugin, a retune that arrives while the create runs.
//
// No thread sanitizer exists on MSVC. The interleavings are covered by construction (the invariants in PluginRunner::buildAndPublishLocked) and by running
// each case below many times from several threads (the run script runs this file five times).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_api.hpp"
#include "core/plugin_host_data.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::HostImage;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using Clock = std::chrono::steady_clock;

constexpr double kIqRate = 2400000.0;
constexpr double kCentre = 100000000.0;

int sleepMs() {
    const char* e = std::getenv("FOXSDR_TEST_CREATE_MS");
    return e != nullptr && *e != '\0' ? std::max(50, std::atoi(e)) : 10000;
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

// A fake I/Q decoder of its own per N: counters, an optional create() that holds until released (or for sleepMs()), a retune record.
template <int N>
struct Fake {
    static inline std::atomic<int> created{0};
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<int> inCreate{0};
    static inline std::atomic<long long> frames{0};
    static inline std::atomic<int> retunes{0};
    static inline std::atomic<double> lastCentre{0.0};
    static inline std::atomic<double> createdCentre{0.0};
    static inline std::atomic<double> createdRate{0.0};
    static inline std::atomic<bool> slow{false};      // create() waits
    static inline std::atomic<bool> release{false};   // ...until this, or the time limit
    static inline int handle = 0;

    static void reset() {
        created = 0; destroyed = 0; inCreate = 0; frames = 0; retunes = 0; lastCentre = 0.0; createdCentre = 0.0; createdRate = 0.0;
        slow = false; release = false;
    }
    static void* create(double rateHz, double centreHz) {
        ++inCreate;
        if (slow.load()) {
            const auto until = Clock::now() + std::chrono::milliseconds(sleepMs());
            while (!release.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        createdRate = rateHz;
        createdCentre = centreHz;
        lastCentre = centreHz;
        ++created;
        --inCreate;
        return &handle;
    }
    static void process(void*, const float*, size_t frames_) { frames += static_cast<long long>(frames_); }
    static void retune(void*, double c) { ++retunes; lastCentre = c; }
    static int32_t poll(void*, char*, size_t) { return 0; }
    static void destroy(void*) { ++destroyed; }
    static CascadeIqDecoderApi api() {
        CascadeIqDecoderApi a{};
        a.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
        a.requiredRateHz = 0.0;
        a.create = &create;
        a.process = &process;
        a.retune = &retune;
        a.poll_text = &poll;
        a.destroy = &destroy;
        return a;
    }
};

// ...and an image decoder (pollImages has something to walk).
struct FakeImg {
    static inline int handle = 0;
    static inline std::atomic<int> created{0};
    static void* create(double, double) { ++created; return &handle; }
    static void process(void*, const float*, size_t) {}
    static int32_t pollImage(void*, CascadeImage*) { return 0; }
    static void release(void*, const CascadeImage*) {}
    static int32_t pollText(void*, char*, size_t) { return 0; }
    static void destroy(void*) {}
    static CascadeImageDecoderApi api() {
        CascadeImageDecoderApi a{};
        a.structSize = static_cast<uint32_t>(sizeof(CascadeImageDecoderApi));
        a.inputKind = CASCADE_INPUT_IQ;
        a.requiredRateHz = 0.0;
        a.create = &create;
        a.process = &process;
        a.poll_image = &pollImage;
        a.release_image = &release;
        a.poll_text = &pollText;
        a.destroy = &destroy;
        return a;
    }
};

LoadedPlugin iqPlugin(const char* name, const CascadeIqDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.version = "1.0.0";
    p.iqDecoder = api;
    return p;
}

LoadedPlugin imgPlugin(const char* name, const CascadeImageDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.version = "1.0.0";
    p.imageDecoder = api;
    return p;
}

bool hasRow(PluginRunner& r, const std::string& name, DecoderIdleReason why) {
    for (const DecoderStatus& s : r.status()) {
        if (s.plugin == name && s.reason == why) { return true; }
    }
    return false;
}

// What the window and the DSP do while a start is in progress: pollImages once a "frame", processIq once a "block". Records the worst time of each.
struct Drive {
    PluginRunner& r;
    std::atomic<bool> stop{false};
    std::atomic<double> worstPoll{0.0};
    std::atomic<double> worstProc{0.0};
    std::atomic<long> polls{0};
    std::atomic<long> blocks{0};
    std::thread win, dsp;
    explicit Drive(PluginRunner& runner) : r(runner) {}
    void begin() {
        win = std::thread([this] {
            std::vector<HostImage> imgs;
            while (!stop.load()) {
                const auto t = Clock::now();
                r.pollImages(imgs);
                const double ms = msSince(t);
                double w = worstPoll.load();
                while (ms > w && !worstPoll.compare_exchange_weak(w, ms)) {}
                ++polls;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        dsp = std::thread([this] {
            std::vector<float> block(4096 * 2, 0.0f);
            while (!stop.load()) {
                const auto t = Clock::now();
                r.processIq(block.data(), 4096);
                const double ms = msSince(t);
                double w = worstProc.load();
                while (ms > w && !worstProc.compare_exchange_weak(w, ms)) {}
                ++blocks;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    void end() {
        stop = true;
        if (win.joinable()) { win.join(); }
        if (dsp.joinable()) { dsp.join(); }
    }
    ~Drive() { end(); }
};

}  // namespace

int main() {
    std::printf("test_plugin_runner_unlocked_create (create sleeps %d ms)\n", sleepMs());
    const CascadeIqDecoderApi apiA = Fake<1>::api();   // the plugin whose create() is slow
    const CascadeIqDecoderApi apiB = Fake<2>::api();   // two other decoders that run all along
    const CascadeIqDecoderApi apiC = Fake<3>::api();
    const CascadeIqDecoderApi apiD = Fake<4>::api();   // a second slow one
    const CascadeImageDecoderApi apiImg = FakeImg::api();
    LoadedPlugin slowA = iqPlugin("SlowStart", &apiA);
    LoadedPlugin runB = iqPlugin("RunB", &apiB);
    LoadedPlugin runC = iqPlugin("RunC", &apiC);
    LoadedPlugin slowD = iqPlugin("SlowToo", &apiD);
    LoadedPlugin img = imgPlugin("Picture", &apiImg);

    // ------------------------------------------------------------------ THE TEST THAT DEFINES DONE
    {
        std::printf("a ten-second create() adds no more than 20 ms to any pollImages or processIq call\n");
        Fake<1>::reset(); Fake<2>::reset(); Fake<3>::reset();
        PluginRunner r;
        r.rebuild({runB, runC, img}, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 3u);

        // BASELINE: the same two calls with nothing starting.
        double baselinePoll = 0.0, baselineProc = 0.0;
        {
            Drive d(r);
            d.begin();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            d.end();
            baselinePoll = d.worstPoll.load();
            baselineProc = d.worstProc.load();
            std::printf("  baseline worst: pollImages %.2f ms, processIq %.2f ms\n", baselinePoll, baselineProc);
        }
        const long long fedB0 = Fake<2>::frames.load();

        Fake<1>::slow = true;
        std::atomic<bool> startReturned{false};
        std::atomic<bool> startResult{false};
        Drive d(r);
        d.begin();
        std::thread starter([&] { startResult = r.startPlugin(slowA); startReturned = true; });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));   // create() is running now
        CHECK(!startReturned.load());
        // THE ROW SAYS STARTING, with the lock free: status() answers at once.
        {
            const auto t = Clock::now();
            const bool starting = hasRow(r, "SlowStart", DecoderIdleReason::Starting);
            CHECK(starting);
            CHECK(msSince(t) < 50.0);
        }
        // Let it run ~1.5 s (or the whole create when it is short): the window and the DSP go on.
        const int observeMs = std::min(sleepMs() - 200, 1500);
        std::this_thread::sleep_for(std::chrono::milliseconds(observeMs));
        CHECK(!startReturned.load());
        const double wp = d.worstPoll.load();
        const double wc = d.worstProc.load();
        std::printf("  during the start: worst pollImages %.2f ms, worst processIq %.2f ms (%ld polls, %ld blocks)\n", wp, wc, d.polls.load(), d.blocks.load());
        CHECK(wp - baselinePoll <= 20.0);
        CHECK(wc - baselineProc <= 20.0);
        CHECK(d.polls.load() > 30 && d.blocks.load() > 30);   // (a Windows sleep(1 ms) is ~15 ms: about 100 calls a 1.5 s)
        CHECK(Fake<2>::frames.load() > fedB0);   // the two decoders that were running were fed every block meanwhile
        CHECK(Fake<3>::frames.load() > 0);
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Starting));

        // create() returns: it publishes and runs.
        Fake<1>::release = true;
        starter.join();
        CHECK(startResult.load());
        CHECK(Fake<1>::created.load() == 1);
        CHECK(!hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Running));
        const long long fedA0 = Fake<1>::frames.load();
        CHECK(waitFor([&] { return Fake<1>::frames.load() > fedA0; }, 2000));   // fed within a block or so
        CHECK(r.activeCount() == 4u);
        d.end();
        r.clear();
        CHECK(Fake<1>::destroyed.load() == 1);
        std::printf("  worst added delay: pollImages %.2f ms, processIq %.2f ms\n", wp - baselinePoll, wc - baselineProc);
    }

    // ------------------------------------------------------------------ STOP DURING THE START
    {
        std::printf("stop during the start: the instance is never published and is destroyed once\n");
        Fake<1>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        std::thread starter([&] { (void)r.startPlugin(slowA); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        r.stopPlugin(cascade::core::pluginKey(slowA));   // from this thread, while create() runs on the other
        CHECK(!hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        Fake<1>::release = true;
        starter.join();
        CHECK(Fake<1>::created.load() == 1);
        CHECK(Fake<1>::destroyed.load() == 1);          // built, never published, destroyed exactly once
        CHECK(!r.hasInstances(cascade::core::pluginKey(slowA)));
        CHECK(!hasRow(r, "SlowStart", DecoderIdleReason::Running));
        // ...and it can be started again after that.
        Fake<1>::slow = false;
        CHECK(r.startPlugin(slowA));
        CHECK(r.hasInstances(cascade::core::pluginKey(slowA)));
        r.clear();
    }

    // ------------------------------------------------------------------ UNLOAD (clear) DURING THE START
    {
        std::printf("unload during the start: clear() cancels it; the late instance is destroyed, not published or rebuilt\n");
        Fake<1>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        std::thread starter([&] { (void)r.startPlugin(slowA); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        r.clear();
        Fake<1>::release = true;
        starter.join();
        CHECK(Fake<1>::created.load() == 1);   // not built a second time
        CHECK(Fake<1>::destroyed.load() == 1);
        CHECK(!r.hasInstances(cascade::core::pluginKey(slowA)));
        CHECK(r.activeCount() == 0u);
    }

    // ------------------------------------------------------------------ RATE CHANGE (rebuild) DURING THE START
    {
        std::printf("rate change during the start: the stale instance is destroyed and the plugin is built again against the new rate\n");
        Fake<1>::reset(); Fake<2>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        std::thread starter([&] { (void)r.startPlugin(slowA); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        // The source changed: everything is rebuilt against 1.2 MS/s. The plugin whose create() is running is NOT created a second time by this call.
        r.rebuild({runB, slowA}, 48000.0, 1200000.0, kCentre + 1000.0);
        CHECK(Fake<1>::created.load() == 0);   // still inside its first create()
        Fake<1>::slow = false;                  // the second create (if any) is fast
        Fake<1>::release = true;
        starter.join();
        CHECK(Fake<1>::created.load() == 2);    // the first (stale) and the one against the new rate
        CHECK(Fake<1>::destroyed.load() == 1);  // the stale one
        CHECK(Fake<1>::createdRate.load() == 1200000.0);
        CHECK(r.hasInstances(cascade::core::pluginKey(slowA)));
        CHECK(r.hasInstances(cascade::core::pluginKey(runB)));
        CHECK(!hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        r.clear();
        CHECK(Fake<1>::destroyed.load() == 2);
    }

    // ------------------------------------------------------------------ A RETUNE THAT ARRIVES WHILE CREATE RUNS
    {
        std::printf("retune during the start: the new instance is told the centre before its first block\n");
        Fake<1>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        std::thread starter([&] { (void)r.startPlugin(slowA); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        r.retune(kCentre + 5000000.0);   // the lock is free: applied at once to what is published
        Fake<1>::release = true;
        starter.join();
        CHECK(Fake<1>::createdCentre.load() == kCentre);          // created against the old centre...
        CHECK(Fake<1>::lastCentre.load() == kCentre + 5000000.0); // ...and told the new one before it was published
        CHECK(Fake<1>::retunes.load() == 1);
        r.clear();
    }

    // ------------------------------------------------------------------ TWO PLUGINS AT ONCE
    {
        std::printf("two plugins starting at once: both run, neither waits for the other's create()\n");
        Fake<1>::reset(); Fake<4>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        Fake<4>::slow = true;
        std::thread t1([&] { (void)r.startPlugin(slowA); });
        std::thread t2([&] { (void)r.startPlugin(slowD); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1 && Fake<4>::inCreate.load() == 1; }, 3000));   // BOTH inside create() at the same moment
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        CHECK(hasRow(r, "SlowToo", DecoderIdleReason::Starting));
        Fake<4>::release = true;      // the second finishes first
        CHECK(waitFor([&] { return hasRow(r, "SlowToo", DecoderIdleReason::Running); }, 3000));
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        Fake<1>::release = true;
        t1.join();
        t2.join();
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Running));
        CHECK(r.activeCount() == 3u);
        // status rows and instances agree after the interleaved publishes: feeding works for all three.
        std::vector<float> block(4096 * 2, 0.0f);
        r.processIq(block.data(), 4096);
        CHECK(Fake<1>::frames.load() == 4096 && Fake<4>::frames.load() == 4096);
        r.clear();
    }

    // ------------------------------------------------------------------ A SECOND START OF THE SAME PLUGIN
    {
        std::printf("a second start of a plugin whose create() is running does nothing\n");
        Fake<1>::reset();
        PluginRunner r;
        r.rebuild({runB}, 48000.0, kIqRate, kCentre);
        Fake<1>::slow = true;
        std::thread starter([&] { (void)r.startPlugin(slowA); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        CHECK(!r.startPlugin(slowA));
        Fake<1>::release = true;
        starter.join();
        CHECK(Fake<1>::created.load() == 1);
        r.clear();
    }

    // ------------------------------------------------------------------ REBUILD BUILDS UNLOCKED TOO
    {
        std::printf("rebuild with a slow create() does not hold up pollImages or processIq either\n");
        Fake<1>::reset(); Fake<2>::reset();
        PluginRunner r;
        Fake<1>::slow = true;
        std::thread rb([&] { r.rebuild({runB, slowA, img}, 48000.0, kIqRate, kCentre); });
        CHECK(waitFor([&] { return Fake<1>::inCreate.load() == 1; }, 3000));
        Drive d(r);
        d.begin();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(sleepMs() - 200, 800)));
        const double wp = d.worstPoll.load(), wc = d.worstProc.load();
        std::printf("  during the rebuild: worst pollImages %.2f ms, worst processIq %.2f ms\n", wp, wc);
        CHECK(wp <= 20.0);
        CHECK(wc <= 20.0);
        CHECK(d.polls.load() > 50);
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Starting));
        Fake<1>::release = true;
        rb.join();
        d.end();
        CHECK(r.activeCount() == 3u);
        CHECK(hasRow(r, "SlowStart", DecoderIdleReason::Running));
        r.clear();
    }

    return testSummary("test_plugin_runner_unlocked_create");
}
