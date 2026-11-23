// test_plugin_start_worker.cpp - A PLUGIN WHOSE create() TAKES TWENTY SECONDS DOES NOT MAKE THE PROGRAM UNCONTROLLABLE (freeze diagnosis host design 7 to 10, stage two).
//
// Every create() and destroy() runs on the one start worker (core/plugin_start_worker.hpp); the control pass posts a request and goes on, and publishes what the worker
// has built (PluginRunner::pumpStarts). The times are scaled so the test runs in seconds: the product's 2 s / 30 s / 120 s are 0.3 s / 1 s / 3 s here (the seam
// PluginRunner::setStartTimingsForTest): 0.3 s, 1 s and 3 s here, and the slow create() is 2.2 s - past the report time, inside the abandon time.
//
//   A. A SLOW create(): every control pass during it completes in under 50 ms; a second plugin started meanwhile is "starting" and starts after the first (serial worker);
//      the row says "taking longer than usual" after the first time; ONE slow-start report goes out at the second, naming the plugin's public id and version; no freeze
//      report of any thread is written (a real control-thread watchdog beaten by these passes stays silent).
//   B. STOP DURING THE create(): returns at once; the instance is destroyed by the worker when create() returns, never published.
//   C. A create() THAT NEVER RETURNS: at the third time the plugin is switched off with the existing sentence, the worker thread is abandoned, a REPLACEMENT worker starts
//      a third plugin normally, and when the stuck create() finally returns its instance is dropped, never published.
//   D. THE CONTROLLABILITY BASELINE: the same slow create() started SYNCHRONOUSLY holds its caller for the whole of it (the thing stage two removes).
//   E. clear() with a create() still running returns within its bound, and keeps the module pinned.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/hang_watchdog.hpp"
#include "core/plugin_api.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host_data.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_start_worker.hpp"
#include "test_check.hpp"

namespace {

namespace fault = cascade::core::fault;
using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::HangWatchdog;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using cascade::core::StartWorker;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr double kIqRate = 2400000.0;
constexpr double kCentre = 100000000.0;
constexpr int kSlowCreateMs = 2200;   // a create() that outlasts the report time and not the abandon time (the product's "20 s" would not reach the 30 s report; a 40 s one does)

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <int N>
struct Fake {
    static inline std::atomic<int> created{0};
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<int> inCreate{0};
    static inline std::atomic<int> createOnThreads{0};
    static inline std::atomic<unsigned long long> lastThread{0};
    static inline std::atomic<int> createMs{0};          // how long create() takes (0: at once)
    static inline std::atomic<bool> hold{false};        // create() waits for release (up to 20 s: a safety net)
    static inline std::atomic<bool> release{false};
    static inline int handle = 0;
    static void reset() { created = 0; destroyed = 0; inCreate = 0; createMs = 0; hold = false; release = false; lastThread = 0; }
    static void* create(double, double) {
        ++inCreate;
        lastThread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        if (hold.load()) {
            const auto until = Clock::now() + std::chrono::seconds(20);
            while (!release.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        } else if (createMs.load() > 0) {
            const auto until = Clock::now() + std::chrono::milliseconds(createMs.load());
            while (Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        ++created;
        --inCreate;
        return &handle;
    }
    static void process(void*, const float*, size_t) {}
    static int32_t poll(void*, char*, size_t) { return 0; }
    static void destroy(void*) { ++destroyed; }
    static CascadeIqDecoderApi api() {
        CascadeIqDecoderApi a{};
        a.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
        a.requiredRateHz = 0.0;
        a.create = &create;
        a.process = &process;
        a.poll_text = &poll;
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

bool rowIs(PluginRunner& r, const std::string& name, DecoderIdleReason why, const std::string& detailHas = std::string()) {
    for (const DecoderStatus& s : r.status()) {
        if (s.plugin == name && s.reason == why && (detailHas.empty() || s.detail.find(detailHas) != std::string::npos)) { return true; }
    }
    return false;
}

// What the control thread does each pass that touches the runner: the pump, then the getters the pass calls.
double controlPass(PluginRunner& r, const std::string& key) {
    const auto t = Clock::now();
    r.pumpStarts();
    (void)r.status();
    (void)r.activeCount();
    (void)r.hasInstances(key);
    (void)r.isFeeding(key);
    r.retune(kCentre);
    return msSince(t);
}

std::atomic<int> g_slowReports{0};
std::string g_slowId, g_slowVersion;
std::atomic<bool> g_slowOver2Min{false};
void slowSink(const PluginRunner::SlowStart& s) {
    ++g_slowReports;
    g_slowId = s.publicId;
    g_slowVersion = s.version;
    g_slowOver2Min = s.overTwoMinutes;
}

std::size_t freezeFilesIn(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind("hang-", 0) == 0) { ++n; }
    }
    return n;
}

}  // namespace

int main() {
    const CascadeIqDecoderApi apiA = Fake<1>::api();   // the slow plugin
    const CascadeIqDecoderApi apiB = Fake<2>::api();   // a second plugin, started meanwhile
    const CascadeIqDecoderApi apiC = Fake<3>::api();   // a third, after an abandonment
    LoadedPlugin slowA = iqPlugin("SlowStart", &apiA);
    LoadedPlugin plainB = iqPlugin("PlainTwo", &apiB);
    LoadedPlugin plainC = iqPlugin("PlainThree", &apiC);
    const std::string keyA = cascade::core::pluginKey(slowA);
    const std::string keyB = cascade::core::pluginKey(plainB);
    const std::string keyC = cascade::core::pluginKey(plainC);
    fault::setIdentity(keyA, "2.4.1", fault::Origin::Official, nullptr, 0);
    fault::setIdentity(keyB, "1.0.0", fault::Origin::Official, nullptr, 0);
    fault::setIdentity(keyC, "1.0.0", fault::Origin::Official, nullptr, 0);
    PluginRunner::setSlowStartSink(&slowSink);
    PluginRunner::setStartTimingsForTest(300, 1000, 3000);   // the product's 2 s, 30 s and 120 s, scaled down

    // ------------------------------------------------------------------ A. THE TEST THAT DEFINES DONE
    {
        std::printf("A: a slow create(): every control pass under 50 ms, the second plugin starts after the first, the row and the report, no freeze report\n");
        Fake<1>::reset(); Fake<2>::reset();
        g_slowReports = 0;
        const fs::path dir = fs::temp_directory_path() / ("cascade-startworker-" + std::to_string(Clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(dir, ec);
        HangWatchdog wd;
        wd.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        wd.setRole(HangWatchdog::Role::Control);
        wd.start(dir.string(), 1000);   // a CONTROL watchdog with a 1 s threshold, beaten by every pass below

        PluginRunner r;
        r.setAsyncStart(true);
        r.rebuild({}, 48000.0, kIqRate, kCentre);
        Fake<1>::createMs = kSlowCreateMs;
        const auto t0 = Clock::now();
        CHECK(r.startPlugin(slowA));
        CHECK(msSince(t0) < 50.0);                                   // posting returns at once
        CHECK(rowIs(r, "SlowStart", DecoderIdleReason::Starting));   // "starting" at once
        CHECK(waitFor([] { return Fake<1>::inCreate.load() == 1; }, 3000));
        const auto tB = Clock::now();
        CHECK(r.startPlugin(plainB));                                // a second plugin started meanwhile
        CHECK(msSince(tB) < 50.0);
        CHECK(rowIs(r, "PlainTwo", DecoderIdleReason::Starting));    // ...says starting
        double worst = 0.0;
        long passes = 0;
        bool sawSlowRow = false;
        bool secondStartedEarly = false;
        while (Fake<1>::created.load() == 0) {
            worst = std::max(worst, controlPass(r, keyA));
            wd.heartbeat();
            ++passes;
            if (rowIs(r, "SlowStart", DecoderIdleReason::Starting, "\"SlowStart\" is taking longer than usual to start.")) { sawSlowRow = true; }
            const int b = Fake<2>::created.load();   // (read first: the first plugin's counter is set before the second's can be)
            const int a = Fake<1>::created.load();
            if (b != 0 && a == 0) { secondStartedEarly = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::printf("  %ld control passes during the create(); the worst took %.2f ms; slow-start reports: %d (id %s, version %s)\n", passes, worst, g_slowReports.load(),
                    g_slowId.c_str(), g_slowVersion.c_str());
        CHECK(passes > 50);
        CHECK(worst < 50.0);
        CHECK(sawSlowRow);
        CHECK(!secondStartedEarly);                                  // serial worker: the second waits for the first
        CHECK(g_slowReports.load() == 1);                            // ONE report...
        CHECK(g_slowId == "slowstart" && g_slowVersion == "2.4.1");  // ...naming the plugin's public id and version
        CHECK(!g_slowOver2Min.load());
        // the first finishes; the pump publishes it; then the second runs and is published
        CHECK(waitFor([&] { controlPass(r, keyA); wd.heartbeat(); return r.hasInstances(keyA); }, 3000));
        CHECK(waitFor([&] { controlPass(r, keyB); wd.heartbeat(); return r.hasInstances(keyB); }, 3000));
        CHECK(rowIs(r, "SlowStart", DecoderIdleReason::Running));
        CHECK(rowIs(r, "PlainTwo", DecoderIdleReason::Running));
        CHECK(Fake<2>::created.load() == 1);
        CHECK(g_slowReports.load() == 1);                            // still one
        wd.stop();
        CHECK(freezeFilesIn(dir) == 0u);                             // no control-thread or window freeze report (and no worker report: the create() was inside its 30 s budget)
        r.clear();
        CHECK(Fake<1>::destroyed.load() == 1 && Fake<2>::destroyed.load() == 1);
        fs::remove_all(dir, ec);
    }

    // ------------------------------------------------------------------ B. STOP DURING THE create()
    {
        std::printf("B: stop during the create() returns at once; the instance is destroyed by the worker when create() returns, never published\n");
        Fake<1>::reset();
        PluginRunner r;
        r.setAsyncStart(true);
        r.rebuild({}, 48000.0, kIqRate, kCentre);
        Fake<1>::createMs = 1200;
        CHECK(r.startPlugin(slowA));
        CHECK(waitFor([] { return Fake<1>::inCreate.load() == 1; }, 3000));
        const auto t = Clock::now();
        r.stopPlugin(keyA);
        CHECK(msSince(t) < 50.0);
        CHECK(!rowIs(r, "SlowStart", DecoderIdleReason::Starting));
        CHECK(waitFor([&] { r.pumpStarts(); return Fake<1>::created.load() == 1 && Fake<1>::destroyed.load() == 1; }, 5000));
        CHECK(!r.hasInstances(keyA));
        CHECK(!rowIs(r, "SlowStart", DecoderIdleReason::Running));
        r.clear();
        CHECK(Fake<1>::destroyed.load() == 1);   // once
    }

    // ------------------------------------------------------------------ C. A create() THAT NEVER RETURNS
    {
        std::printf("C: a create() that never returns: switched off at the third time, a replacement worker starts the next plugin, the late return is dropped\n");
        Fake<1>::reset(); Fake<3>::reset();
        fault::resetForTest();
        fault::setIdentity(keyA, "2.4.1", fault::Origin::Official, nullptr, 0);
        fault::setIdentity(keyC, "1.0.0", fault::Origin::Official, nullptr, 0);
        StartWorker::instance().resetForTest();
        g_slowReports = 0;
        PluginRunner r;
        r.setAsyncStart(true);
        r.rebuild({}, 48000.0, kIqRate, kCentre);
        Fake<1>::hold = true;
        CHECK(r.startPlugin(slowA));
        CHECK(waitFor([] { return Fake<1>::inCreate.load() == 1; }, 3000));
        const unsigned long long stuckThread = Fake<1>::lastThread.load();
        CHECK(r.startPlugin(plainC));                                // queued behind the stuck one
        double worst = 0.0;
        const bool switchedOff = waitFor([&] {
            worst = std::max(worst, controlPass(r, keyA));
            return rowIs(r, "SlowStart", DecoderIdleReason::CreateFailed, "was switched off for this session");
        }, 6000);
        CHECK(switchedOff);
        CHECK(worst < 50.0);
        CHECK(fault::isQuarantined(fault::slotFor(keyA)));
        CHECK(StartWorker::instance().abandoned() == 1u);
        CHECK(g_slowReports.load() == 1);
        CHECK(waitFor([&] { controlPass(r, keyC); return r.hasInstances(keyC); }, 4000));   // the REPLACEMENT worker started the third plugin
        CHECK(Fake<3>::created.load() == 1);
        CHECK(Fake<3>::lastThread.load() != stuckThread);
        CHECK(rowIs(r, "PlainThree", DecoderIdleReason::Running));
        // the stuck create() finally returns: its instance is dropped, never published, and the quarantined plugin is not called again
        Fake<1>::release = true;
        CHECK(waitFor([&] { r.pumpStarts(); return Fake<1>::inCreate.load() == 0; }, 3000));
        for (int i = 0; i < 20; ++i) { r.pumpStarts(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(!r.hasInstances(keyA));
        CHECK(Fake<1>::destroyed.load() == 0);                       // (the containment refuses a call into a switched-off plugin: the handle is dropped)
        r.clear();
        fault::resetForTest();
        StartWorker::instance().resetForTest();
    }

    // ------------------------------------------------------------------ D. THE BASELINE
    {
        std::printf("D: the same create(), synchronous, holds its caller for all of it\n");
        Fake<1>::reset();
        Fake<1>::createMs = 800;
        PluginRunner r;                                              // asyncStart off
        r.rebuild({}, 48000.0, kIqRate, kCentre);
        const auto t = Clock::now();
        CHECK(r.startPlugin(slowA));
        const double took = msSince(t);
        std::printf("  synchronous start of an 800 ms create(): the caller waited %.0f ms\n", took);
        CHECK(took >= 700.0);
        CHECK(r.hasInstances(keyA));
        r.clear();
    }

    // ------------------------------------------------------------------ E. clear() DURING A create()
    {
        std::printf("E: clear() with a create() still running returns within its bound\n");
        Fake<1>::reset();
        fault::resetForTest();
        fault::setIdentity(keyA, "2.4.1", fault::Origin::Official, nullptr, 0);
        StartWorker::instance().resetForTest();
        PluginRunner r;
        r.setAsyncStart(true);
        r.rebuild({}, 48000.0, kIqRate, kCentre);
        Fake<1>::hold = true;
        CHECK(r.startPlugin(slowA));
        CHECK(waitFor([] { return Fake<1>::inCreate.load() == 1; }, 3000));
        const auto t = Clock::now();
        r.clear();
        const double took = msSince(t);
        std::printf("  clear() with a create() in flight took %.0f ms (bound %lld ms)\n", took, static_cast<long long>(PluginRunner::kClearWait.count()));
        CHECK(took < static_cast<double>(PluginRunner::kClearWait.count()) + 200.0);
        CHECK(!r.hasInstances(keyA));
        Fake<1>::release = true;
        CHECK(waitFor([] { return Fake<1>::inCreate.load() == 0; }, 3000));
        CHECK(waitFor([&] { r.pumpStarts(); return !r.startsPending(); }, 3000));
        CHECK(!r.hasInstances(keyA));                                // the late build was never published
        fault::resetForTest();
        StartWorker::instance().resetForTest();
    }

    PluginRunner::setSlowStartSink(nullptr);
    PluginRunner::setStartTimingsForTest(0, 0, 0);
    return testSummary("test_plugin_start_worker");
}
