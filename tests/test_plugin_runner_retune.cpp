// PluginRunner::retune() must not queue behind the DSP thread - and must still arrive.
//
// WHAT THIS IS FOR. AppWindow::applyRetuneNow, the one place the source centre moves, calls
// Pipeline::resetRds and then PluginRunner::retune. Pipeline::resetRds no longer waits for the
// DSP thread (tests/test_pipeline_setters.cpp); had retune() kept its blocking lock the freeze
// of field report "hang ntdll.dll @ cascade::core::Pipeline::resetRds" (0.99.81, a toolbar
// retune with ~40 decoders loaded) would simply have moved one call down. The runner's mutex
// is held by the DSP thread across EVERY I/Q decoder's process() (processIq), and std::mutex
// is not fair.
//
// So retune() tries the lock; if a block is inside, the centre is left for the DSP thread to
// apply at the top of its next processIq(). This file pins the three things that design must
// keep: the call returns at once, the centre is NOT lost, and the order of tunes is.
//
//   1. a decoder wedged inside process() (a harder wedge than any real plugin: it does not
//      return until released) and retune() returns within a bar;
//   2. the pending centre reaches the plugin at the next block, BEFORE that block's samples;
//   3. LATEST WINS: three retunes while wedged are one callback with the last value; a retune
//      that did get the lock cancels an older pending one, so a stale centre is never applied
//      after a newer one;
//   4. an instance created after a pending tune (startPlugin) is created against it, and a
//      rebuild() - which is handed a centre of its own - discards it;
//   5. the same through a real running Pipeline whose decoder spends 150 ms in every block.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using cascade::core::LoadedPlugin;
using cascade::core::Pipeline;
using cascade::core::PluginRunner;

constexpr double kIqRateHz = 1000000.0;
constexpr double kAudioRateHz = 48000.0;

// "Did not wait for the lock", ms. A pending store is nanoseconds; the wedge outlasts this by
// 15x, so a retune that waits for the runner cannot pass by luck.
constexpr double kMaxRetuneMs = 100.0;
constexpr int kWedgeSafetyMs = 1500;  // the wedge lets go on its own, so a failing build ends

// --- the fake decoders -----------------------------------------------------------------------

struct Fake {
    std::mutex m;
    std::vector<std::string> log;  // "create@<centre>", "retune@<centre>", "process"
    std::atomic<bool> wedge{false};
    std::atomic<bool> release{false};
    std::atomic<bool> inProcess{false};
    std::atomic<int> sleepMs{0};
    std::atomic<double> lastRetune{-1.0};
    std::atomic<double> createdAt{-1.0};
    void note(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        log.push_back(s);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(m);
        return log;
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m);
        log.clear();
        wedge = false;
        release = false;
        inProcess = false;
        sleepMs = 0;
        lastRetune = -1.0;
        createdAt = -1.0;
    }
};

Fake g_a;  // the plugin that is fed (and wedged)
Fake g_b;  // a second plugin, started later

std::string centreStr(double hz) {
    char b[64];
    std::snprintf(b, sizeof b, "%.0f", hz);
    return b;
}

void* createA(double, double centre) {
    g_a.createdAt = centre;
    g_a.note("create@" + centreStr(centre));
    return &g_a;
}
void* createB(double, double centre) {
    g_b.createdAt = centre;
    g_b.note("create@" + centreStr(centre));
    return &g_b;
}
void fakeProcess(void* h, const float*, std::size_t) {
    Fake& f = *static_cast<Fake*>(h);
    f.note("process");
    if (f.wedge.load()) {
        f.inProcess = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWedgeSafetyMs);
        while (!f.release.load() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        f.inProcess = false;
    } else if (f.sleepMs.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(f.sleepMs.load()));
    }
}
void fakeRetune(void* h, double centre) {
    Fake& f = *static_cast<Fake*>(h);
    f.lastRetune = centre;
    f.note("retune@" + centreStr(centre));
}
std::int32_t fakePollText(void*, char*, std::size_t) { return 0; }
void fakeDestroy(void*) {}

CascadeIqDecoderApi makeApi(void* (*create)(double, double)) {
    CascadeIqDecoderApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    a.requiredRateHz = 0.0;
    a.preferredRateHz = 0.0;
    a.create = create;
    a.process = &fakeProcess;
    a.retune = &fakeRetune;
    a.poll_text = &fakePollText;
    a.destroy = &fakeDestroy;
    return a;
}

LoadedPlugin makePlugin(const char* name, const char* path, const CascadeIqDecoderApi* iq) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = path;
    p.version = "1.0.0";
    p.capabilities = CASCADE_CAP_IQ_DECODER;
    p.iqDecoder = iq;
    return p;
}

template <typename Fn>
bool waitFor(Fn ready, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

double timeMs(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A DSP block into the runner, on a thread of its own: this is the thread that sits inside the
// decoder's process() while the "GUI" thread (the test's main) retunes.
struct Block {
    PluginRunner& runner;
    std::thread t;
    explicit Block(PluginRunner& r) : runner(r) {
        t = std::thread([this] {
            std::vector<float> iq(2 * 1024, 0.0f);
            runner.processIq(iq.data(), 1024);
        });
    }
    void join() {
        if (t.joinable()) { t.join(); }
    }
    ~Block() { join(); }
};

void processOnce(PluginRunner& r) {
    std::vector<float> iq(2 * 1024, 0.0f);
    r.processIq(iq.data(), 1024);
}

std::size_t count(const std::vector<std::string>& v, const std::string& s) {
    std::size_t n = 0;
    for (const std::string& x : v) {
        if (x == s) { ++n; }
    }
    return n;
}

// Wedges plugin A inside process() on a block thread and returns once it is genuinely inside.
// The runner's mutex is then held by that thread, which is the field condition.
void wedgeA(PluginRunner& runner, std::unique_ptr<Block>& blk) {
    g_a.release = false;
    g_a.wedge = true;
    blk = std::make_unique<Block>(runner);
    CHECK(waitFor([] { return g_a.inProcess.load(); }, 5000));
}

void unwedgeA(std::unique_ptr<Block>& blk) {
    g_a.wedge = false;
    g_a.release = true;
    blk->join();
    blk.reset();
}

// --- 1..3: the wedged decoder ----------------------------------------------------------------

void checkRetuneDoesNotWaitAndArrivesAtTheNextBlock() {
    g_a.reset();
    const CascadeIqDecoderApi api = makeApi(&createA);
    PluginRunner runner;
    runner.rebuild({makePlugin("A", "C:/plugins/a.dll", &api)}, kAudioRateHz, kIqRateHz, 100.0e6);
    CHECK(g_a.createdAt.load() == 100.0e6);

    std::unique_ptr<Block> blk;
    wedgeA(runner, blk);

    const double ms = timeMs([&] { runner.retune(101.0e6); });
    std::printf("retune against a wedged decoder: %.3f ms\n", ms);
    if (ms > kMaxRetuneMs) { std::printf("FAIL retune waited %.1f ms for the runner lock\n", ms); }
    CHECK(ms <= kMaxRetuneMs);
    // Nothing reached the plugin yet - it is mid-block, and a decoder never sees a tune
    // halfway through one.
    CHECK(count(g_a.snapshot(), "retune@101000000") == 0);

    unwedgeA(blk);
    CHECK(count(g_a.snapshot(), "retune@101000000") == 0);  // the wedged block finished first

    // The NEXT block: the centre first, then the samples.
    g_a.note("-- next block --");
    processOnce(runner);
    const std::vector<std::string> log = g_a.snapshot();
    std::size_t mark = 0;
    while (mark < log.size() && log[mark] != "-- next block --") { ++mark; }
    CHECK(mark + 2 < log.size());
    if (mark + 2 < log.size()) {
        CHECK(log[mark + 1] == "retune@101000000");
        CHECK(log[mark + 2] == "process");
    }
    CHECK(g_a.lastRetune.load() == 101.0e6);
    // Applied once, and not again on the block after.
    processOnce(runner);
    CHECK(count(g_a.snapshot(), "retune@101000000") == 1);
}

void checkLatestWins() {
    g_a.reset();
    const CascadeIqDecoderApi api = makeApi(&createA);
    PluginRunner runner;
    runner.rebuild({makePlugin("A", "C:/plugins/a.dll", &api)}, kAudioRateHz, kIqRateHz, 100.0e6);

    // Three tunes behind the wedge: one callback, the last value. The first two were
    // overwritten before anyone applied them.
    std::unique_ptr<Block> blk;
    wedgeA(runner, blk);
    runner.retune(1.0e6);
    runner.retune(2.0e6);
    runner.retune(3.0e6);
    unwedgeA(blk);
    processOnce(runner);
    std::vector<std::string> log = g_a.snapshot();
    CHECK(count(log, "retune@1000000") == 0);
    CHECK(count(log, "retune@2000000") == 0);
    CHECK(count(log, "retune@3000000") == 1);
    CHECK(g_a.lastRetune.load() == 3.0e6);

    // A tune that GOT the lock cancels an older pending one: 10 MHz waits behind the wedge,
    // then 20 MHz arrives with the runner free and is applied on the spot. The block after
    // must not drag the plugin back to 10.
    g_a.reset();
    wedgeA(runner, blk);
    runner.retune(10.0e6);
    unwedgeA(blk);
    runner.retune(20.0e6);  // free now: applied immediately
    CHECK(g_a.lastRetune.load() == 20.0e6);
    processOnce(runner);
    processOnce(runner);
    log = g_a.snapshot();
    CHECK(count(log, "retune@10000000") == 0);
    CHECK(count(log, "retune@20000000") == 1);
    CHECK(g_a.lastRetune.load() == 20.0e6);

    // The unchanged value is still a no-op (the existing contract: cheap when nothing moved).
    g_a.reset();
    runner.retune(20.0e6);
    CHECK(g_a.snapshot().empty());
}

// --- 4: instances created after a pending tune ------------------------------------------------

void checkInstancesAreCreatedAgainstTheLatestCentre() {
    g_a.reset();
    g_b.reset();
    const CascadeIqDecoderApi apiA = makeApi(&createA);
    const CascadeIqDecoderApi apiB = makeApi(&createB);
    const LoadedPlugin a = makePlugin("A", "C:/plugins/a.dll", &apiA);
    const LoadedPlugin b = makePlugin("B", "C:/plugins/b.dll", &apiB);

    // startPlugin: a tune left pending behind the wedge is the real centre, so plugin B
    // wakes against it (28 plugins woke like this in the field).
    {
        PluginRunner runner;
        runner.rebuild({a}, kAudioRateHz, kIqRateHz, 100.0e6);
        std::unique_ptr<Block> blk;
        wedgeA(runner, blk);
        runner.retune(55.0e6);
        unwedgeA(blk);
        CHECK(runner.startPlugin(b));
        CHECK(g_b.createdAt.load() == 55.0e6);
    }

    // rebuild(): handed a centre of its own, read from the source after the tune. The older
    // pending one must not be applied over it on the next block.
    g_a.reset();
    {
        PluginRunner runner;
        runner.rebuild({a}, kAudioRateHz, kIqRateHz, 100.0e6);
        std::unique_ptr<Block> blk;
        wedgeA(runner, blk);
        runner.retune(77.0e6);
        unwedgeA(blk);
        g_a.reset();
        runner.rebuild({a}, kAudioRateHz, kIqRateHz, 88.0e6);
        CHECK(g_a.createdAt.load() == 88.0e6);
        processOnce(runner);
        CHECK(count(g_a.snapshot(), "retune@77000000") == 0);
        CHECK(g_a.lastRetune.load() == -1.0);
    }
}

// --- 5: a real, saturated DSP thread ----------------------------------------------------------

void checkRetuneThroughARunningPipeline() {
    g_a.reset();
    g_a.sleepMs = 150;  // every block: the field condition, thirty decoders on a slow machine
    const CascadeIqDecoderApi api = makeApi(&createA);
    Pipeline::Config cfg;
    cfg.sampleRateHz = kIqRateHz;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    PluginRunner runner;
    runner.rebuild({makePlugin("A", "C:/plugins/a.dll", &api)}, Pipeline::kAudioRateHz, kIqRateHz,
                   100.0e6);
    p.setPluginRunner(&runner);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 0; }, 30000));

    double worst = 0.0;
    int calls = 0;
    double hz = 100.0e6;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
        hz += 25000.0;  // a drag across the readout
        const double ms = timeMs([&] { runner.retune(hz); });
        ++calls;
        if (ms > worst) { worst = ms; }
        if (ms > 60.0) { std::printf("FAIL retune waited %.1f ms behind a 150 ms block\n", ms); }
        CHECK(ms <= 60.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("saturated DSP (150 ms/block): %d retunes, slowest %.3f ms\n", calls, worst);

    // The plugin ends on the LAST centre, applied by the DSP thread within a block or two.
    CHECK(waitFor([&] { return g_a.lastRetune.load() == hz; }, 2000));

    p.stop();
    p.setPluginRunner(nullptr);
}

}  // namespace

int main() {
    checkRetuneDoesNotWaitAndArrivesAtTheNextBlock();
    checkLatestWins();
    checkInstancesAreCreatedAgainstTheLatestCentre();
    checkRetuneThroughARunningPipeline();
    return testSummary("test_plugin_runner_retune");
}
