// The SDRplay stream that went silent: the 0.99.27 RSP2 report (2026-09-24)
// and the 0.97.0 RSPdx hang report before it.
//
// BOTH LOGS END THE SAME WAY, and that is the evidence this file is built on:
//
//   info source: stream health - reads 10, with samples 10, timeouts 1681, ...
//                longest gap 84582 ms, 20160 samples in 85 s        (0.99.27)
//   info source: stream health - reads 10, with samples 10, timeouts 2012, ...
//                                                                    (0.97.0)
//   warn source: SDRplay Uninit failed - sdrplay_api_ServiceNotResponding (14)
//   warn source: SDRplay start failed - SDRplay Init failed:
//                sdrplay_api_AlreadyInitialised (9)
//
// A health window is closed, and the next one opened, by the stream callback
// itself - and until 0.99.32 the callback that closed it also WROTE the line:
// the diagnostic log's lock, then fwrite and fflush to the log file, on the
// vendor's own thread. Exactly ten blocks followed that write in both reports,
// on two different radios, and then the service never delivered again. A
// stall at a random moment would leave a random count in the last window; the
// same count twice says the stall began at the one moment the callback did
// something other than copy samples. In the RSP2 log it is 1 ms after the
// line's timestamp, sixty seconds after the stream started.
//
// WHAT THE REPORT SUSPECTED AND THE LOG RULES OUT: a plugin reload blocking
// the callback. Four reloads (12:29:58 to 12:30:28) sit inside the FIRST
// window, whose longest gap was 45 ms with no overflow, and WORKBENCH - the
// whole-band I/Q plugin - was loaded at 12:30:38, seven seconds after the
// stream had already stopped. The second test below holds that line in code:
// a reload storm that keeps the plugin runner's lock for 150 ms at a time
// never reaches the vendor's thread.
//
// WHAT CANNOT BE PROVED HERE: how long the real service tolerates a held
// callback. The fake's threshold is a stand-in (see startService in
// sdrplay_fake_api.hpp). What IS proved is the design rule: the stream
// callback waits on nothing another thread can hold.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_runner.hpp"
#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

using cascade::source::SdrPlaySource;
namespace abi = cascade::source::sdrplay_abi;
using fakesdrplay::FakeSdrPlayApi;

namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned int kBlock = 2016;  // the RSP2 log: 359978976 / 178561 reads
constexpr auto kPeriod = std::chrono::microseconds(1000);
// Far above scheduling noise on a loaded build machine, far below the
// 400 ms the tests hold things for. See the file header: a stand-in.
constexpr auto kWedgeAfter = std::chrono::milliseconds(100);

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

bool ringHas(const char* needle) {
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

// The pipeline's side of the ring, as Pipeline::sourceThreadBody does it:
// read() in 10 ms chunks until told to stop.
struct Reader {
    SdrPlaySource& src;
    std::atomic<bool> run{true};
    std::atomic<unsigned long long> got{0};
    std::thread t;
    explicit Reader(SdrPlaySource& s) : src(s) {
        t = std::thread([this]() {
            std::vector<std::complex<float>> buf(20000);
            while (run.load()) {
                const std::size_t n = src.read(buf.data(), buf.size());
                got.fetch_add(n);
                if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
        });
    }
    ~Reader() {
        run.store(false);
        if (t.joinable()) { t.join(); }
    }
};

// --- 1. the callback never waits on the diagnostic log -----------------------
//
// THE REPRODUCTION. Another thread holds the log's lock for 400 ms - what a
// write stuck behind a slow disk looks like to everyone else who logs, and on
// the tester's machine the disk was busy: the plugin store was writing twelve
// DLLs to it in those two minutes. The first health window (20 ms here, 60 s
// in the field) closes inside that hold. Before 0.99.32 the callback that
// closed it went into diagLogf and stayed there for the rest of the 400 ms;
// the service wedged; the stream never came back; stop()'s Uninit answered 14
// - the field log, line for line.
void testTheStreamCallbackNeverWaitsOnTheDiagnosticLog() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1706012347", abi::kRsp2);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    src.setStreamHealthWindowForTest(std::chrono::milliseconds(20));
    CHECK(src.start());
    Reader reader(src);

    unsigned long long blocksWhileHeld = 0;
    {
        std::unique_lock<std::mutex> held = cascade::core::DiagLog::instance().lockForTest();
        fake.startService(kBlock, kPeriod, kWedgeAfter);
        const auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        blocksWhileHeld = fake.serviceBlocks.load();
        std::printf("held the log lock %lld ms: %llu blocks delivered, longest callback %lld us\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               Clock::now() - t0)
                                               .count()),
                    blocksWhileHeld, fake.longestCallbackUs.load());
    }
    // Let the service run on after the lock is released, so "it came back"
    // and "it never stopped" can both be told apart from "it died".
    const unsigned long long atRelease = fake.serviceBlocks.load();
    CHECK(waitFor([&] { return fake.serviceBlocks.load() > atRelease + 50; }, 3000));
    std::printf("longest callback %lld us, wedged %d, blocks %llu, read %llu samples\n",
                fake.longestCallbackUs.load(), fake.serviceWedged.load() ? 1 : 0,
                fake.serviceBlocks.load(), reader.got.load());

    // THE RULE: the vendor's thread was never held.
    CHECK(fake.longestCallbackUs.load() < 20000);
    CHECK(!fake.serviceWedged.load());
    // ...and the stream ran THROUGH the hold rather than around it. A fixed
    // callback delivers a few hundred blocks in 400 ms; a blocked one at most
    // the one that went in.
    CHECK(blocksWhileHeld > 100);
    // The samples reached the reader, and the health line still reached the
    // log - written by the READER, once the lock came free. A fix that simply
    // stopped logging would pass the three checks above and fail this one.
    CHECK(reader.got.load() > 100ull * kBlock);
    CHECK(waitFor([] { return ringHas("source: stream health - reads "); }, 3000));

    reader.run.store(false);
    if (reader.t.joinable()) { reader.t.join(); }
    fake.stopService();
    src.stop();
    CHECK(!ringHas("Uninit failed"));
    src.closeDevice();
}

// --- 2. a plugin reload storm never reaches the vendor's thread ---------------
//
// THE HYPOTHESIS THE REPORT CAME WITH, held to account. A real Pipeline owns
// the source, a real PluginRunner feeds an I/Q decoder off the DSP thread,
// and the test thread plays the GUI: it rebuilds the runner again and again
// with a create() that takes 150 ms - and rebuild() runs create() WITH THE
// RUNNER'S LOCK HELD (plugin_runner.cpp), so every reload stalls the DSP
// thread's processIq for that long. That is the heaviest thing a reload does
// to anyone else. The vendor's callback is two hops away from it - its own
// lock-free ring, then the pipeline's - and must not notice.
struct SlowDecoder {
    std::atomic<unsigned long long> frames{0};
    std::atomic<int> created{0};
};
SlowDecoder g_slow;

void* slowCreate(double, double) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    g_slow.created.fetch_add(1);
    return &g_slow;
}
void slowProcess(void*, const float*, std::size_t frames) { g_slow.frames.fetch_add(frames); }
std::int32_t slowPoll(void*, char*, std::size_t) { return 0; }
void slowDestroy(void*) {}

void testAPluginReloadStormNeverReachesTheVendorThread() {
    FakeSdrPlayApi fake;
    fake.addDevice("1706012347", abi::kRsp2);

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;  // headless: the chain runs, no device is opened
    cascade::core::Pipeline p(cfg);

    auto owned = std::make_unique<SdrPlaySource>();
    SdrPlaySource* src = owned.get();
    src->setApiForTest(&fake.table);
    CHECK(src->open(""));
    p.setSource(std::move(owned));

    CascadeIqDecoderApi iq{};
    iq.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    iq.create = &slowCreate;
    iq.process = &slowProcess;
    iq.poll_text = &slowPoll;
    iq.destroy = &slowDestroy;
    cascade::core::LoadedPlugin plug;
    plug.loaded = true;
    plug.name = "Slow whole-band decoder";
    plug.path = "C:/plugins/slow.dll";
    plug.version = "1.0.0";
    plug.capabilities = CASCADE_CAP_IQ_DECODER;
    plug.iqDecoder = &iq;

    cascade::core::PluginRunner runner;
    runner.rebuild({plug}, cascade::core::Pipeline::kAudioRateHz, cfg.sampleRateHz, 97.0e6);
    p.setPluginRunner(&runner);
    p.start();
    fake.startService(kBlock, kPeriod, kWedgeAfter);

    CHECK(waitFor([] { return g_slow.frames.load() > 100000; }, 5000));

    long long longestReloadMs = 0;
    for (int i = 0; i < 8; ++i) {
        const auto t0 = Clock::now();
        runner.rebuild({plug}, cascade::core::Pipeline::kAudioRateHz, cfg.sampleRateHz, 97.0e6);
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (ms > longestReloadMs) { longestReloadMs = ms; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const unsigned long long framesAfterStorm = g_slow.frames.load();
    CHECK(waitFor([&] { return g_slow.frames.load() > framesAfterStorm + 100000; }, 5000));
    std::printf("reloads: longest %lld ms, %d created; vendor callback longest %lld us, "
                "wedged %d, %llu blocks; decoder fed %llu frames\n",
                longestReloadMs, g_slow.created.load(), fake.longestCallbackUs.load(),
                fake.serviceWedged.load() ? 1 : 0, fake.serviceBlocks.load(),
                g_slow.frames.load());

    // The reload really did hold the plugin lock that long - without this the
    // test would pass against a reload that never blocked anybody.
    CHECK(longestReloadMs >= 150);
    CHECK(g_slow.created.load() == 9);
    // ...and the vendor's thread never felt it.
    CHECK(fake.longestCallbackUs.load() < 20000);
    CHECK(!fake.serviceWedged.load());
    CHECK(!p.faulted());

    p.setPluginRunner(nullptr);
    p.stop();
    fake.stopService();
    runner.clear();
}

// --- 3. a START after the service stopped answering never enters Init ---------
//
// The second half of both logs: stop()'s Uninit answers 14, and then every
// press of START calls sdrplay_api_Init on the same dead service and is told
// AlreadyInitialised - in 0.97.0, in 0.99.27, and still in 0.99.31, because
// 0.99.28's lost-session latch guards open() and the scan, and a START on a
// radio that is still open is neither. Init is as unbounded as every other
// call into that DLL. The user was shown "SDRplay Init failed:
// sdrplay_api_AlreadyInitialised (9)"; the sentence that says what to DO is
// the one 0.99.28 wrote for exactly this state.
void testAStartAfterTheServiceStoppedAnsweringNeverEntersInit() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1706012347", abi::kRsp2);

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);

    auto owned = std::make_unique<SdrPlaySource>();
    SdrPlaySource* src = owned.get();
    src->setApiForTest(&fake.table);
    CHECK(src->open(""));
    CHECK(src->setCenterFrequencyHz(97300000.0));
    p.setSource(std::move(owned));
    p.start();
    fake.startService(kBlock, kPeriod, kWedgeAfter);
    CHECK(waitFor([&] { return fake.serviceBlocks.load() > 20; }, 3000));

    // Whatever wedged it, the service is gone: the field state at 12:30:31.
    fake.serviceWedged.store(true);
    p.stop();  // the user's STOP: Uninit answers 14
    CHECK(ringHas("SDRplay Uninit failed - ServiceNotResponding (14)"));
    CHECK(src->deviceDead());
    CHECK(fake.countStarting("Init") == 1);

    // POINT 3 OF THE REPORT: "asked for 0.000000 MHz" 27 ms after this stop.
    // Not a stale readback from the released radio - the driver still reports
    // the last frequency and rate it was given.
    CHECK(src->centerFrequencyHz() == 97300000.0);
    CHECK(src->sampleRateHz() == 2000000.0);
    // And the request itself reaches nothing: 0 Hz is refused by the range
    // check before any vendor call, and even an in-range tune on the stopped,
    // dead radio only writes the parameter block.
    const int updatesBefore = fake.countStarting("Update(");
    CHECK(!src->setCenterFrequencyHz(0.0));
    CHECK(src->setCenterFrequencyHz(96000000.0));
    CHECK(fake.countStarting("Update(") == updatesBefore);

    // The user's START, twice, as in the log.
    p.start();
    CHECK(waitFor([&] { return p.faulted(); }, 3000));
    const std::string shown = p.faultMessage();
    std::printf("after START: fault shown \"%s\"\n", shown.c_str());
    p.stop();
    p.start();
    CHECK(waitFor([&] { return p.faulted(); }, 3000));
    p.stop();

    CHECK(fake.countStarting("Init") == 1);
    CHECK(!ringHas("SDRplay start failed"));
    CHECK(shown.find("restart the SDRplay API service") != std::string::npos);
    CHECK(shown.find("Init failed") == std::string::npos);
    // The one-shot open() path was already refusing (0.99.28); still is.
    SdrPlaySource again;
    again.setApiForTest(&fake.table);
    CHECK(!again.open(""));
    CHECK(fake.countStarting("Init") == 1);

    fake.stopService();
}

// --- 4. the EVENT callback leaves its log lines for read() --------------------
//
// The same rule on the service's other callback. An overload event used to be
// logged from inside it; now it is counted, acknowledged (that Update is the
// vendor's own documented pattern and stays) and left for the next read().
void testTheEventCallbackNeverWaitsOnTheDiagnosticLog() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1706012347", abi::kRsp2);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    CHECK(src.start());

    long long firedMs = -1;
    {
        std::unique_lock<std::mutex> held = cascade::core::DiagLog::instance().lockForTest();
        std::thread service([&]() {
            const auto t0 = Clock::now();
            fake.fireOverload(true);
            firedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
                          .count();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        held.unlock();
        service.join();
    }
    std::printf("overload event returned in %lld ms with the log held for 300 ms\n", firedMs);
    CHECK(firedMs >= 0 && firedMs < 100);
    CHECK(src.overloadEvents() == 1);
    CHECK(fake.called(FakeSdrPlayApi::updateCall(abi::Update_Ctrl_OverloadMsgAck,
                                                  abi::Update_Ext1_None)));
    // Not logged by the service's thread...
    CHECK(!ringHas("ADC OVERLOAD"));
    // ...but by the next read(), which is the pipeline's.
    std::complex<float> buf[16];
    (void) src.read(buf, 16);
    CHECK(ringHas("source: SDRplay ADC OVERLOAD - reduce the gain or add attenuation"));

    src.stop();
    src.closeDevice();
}

// --- 5. a stream that goes quiet is NAMED, and the radio is left alone -------
//
// THE SILENT HALF OF EVERY LOG ABOVE (0.99.36). Between the last block and the
// user's next click, FoxSDR said nothing at all: 49 s (0.95.0 RSP1A), 51 s
// (0.99.27 RSP1), 85 s (0.99.27 RSP2), 99 s (0.97.0 RSPdx) of a receiver that
// called itself running on an empty ring. Nothing raises a fault because no
// call is in flight to be refused. Here the service stops delivering without
// a word - serviceWedged, exactly the fake's model of those logs - and the
// receiver must say so within kStreamStallLimit (shortened for the test), in
// the pinned sentence, and then tear down WITHOUT entering the vendor DLL: the
// 0.97.0 RSPdx report is a GUI thread that never came back from a teardown
// call into that wedged service.
void testASilentStreamIsNamedAndTheRadioIsLeftAlone() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1706012347", abi::kRspDx);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    // 500 ms: far above the fake's 1 ms period and any scheduling hiccup on a
    // machine building in parallel, far below the 3 s the check waits.
    src.setStreamStallLimitForTest(std::chrono::milliseconds(500));
    CHECK(src.start());
    Reader reader(src);
    fake.startService(kBlock, kPeriod, kWedgeAfter);

    // A HEALTHY STREAM RUNS WELL PAST THE LIMIT AND IS NOT ACCUSED OF ANYTHING.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(!src.faulted());
    CHECK(reader.got.load() > 100ull * kBlock);

    // The service stops calling us. Nothing is asked of it, nothing refuses.
    fake.serviceWedged.store(true);
    const auto t0 = Clock::now();
    const bool named = waitFor([&] { return src.faulted(); }, 3000);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("stream went quiet: fault raised %s after %lld ms (limit 500 ms)\n",
                named ? "yes" : "NO", ms);
    CHECK(named);
    CHECK(src.deviceDead());
    const std::string shown = src.lastError();
    std::printf("the receiver shows \"%s\"\n", shown.c_str());
    CHECK(shown.find(cascade::source::sdrPlayStreamStalledSentence()) != std::string::npos);
    CHECK(shown.find("restart FoxSDR") != std::string::npos);
    CHECK(ringHas("source: SDRplay stream stalled"));
    // Review note 4: the teardown line says WHICH of the three it was.
    // (Checked after the stop below.)

    // THE TEARDOWN NEVER ENTERS THE WEDGED SERVICE, and the session is not
    // handed to anything else in this process.
    reader.run.store(false);
    if (reader.t.joinable()) { reader.t.join(); }
    src.stop();
    src.closeDevice();
    CHECK(fake.countStarting("Uninit") == 0);
    CHECK(fake.countStarting("ReleaseDevice") == 0);
    CHECK(ringHas("source: SDRplay stopped without Uninit - the stream stopped delivering samples"));
    CHECK(!ringHas("stopped without Uninit - the service stopped answering"));
    SdrPlaySource again;
    again.setApiForTest(&fake.table);
    CHECK(!again.open(""));
    fake.stopService();
}

// ...AND A SERVICE THAT NEVER DELIVERS A FIRST BLOCK IS CAUGHT THE SAME WAY.
void testAStreamThatNeverStartsIsNamedToo() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(src.start());  // no service thread: not one callback will come
    Reader reader(src);
    CHECK(waitFor([&] { return src.faulted(); }, 3000));
    CHECK(std::string(src.lastError()).find(cascade::source::sdrPlayStreamStalledSentence()) !=
          std::string::npos);
    reader.run.store(false);
    if (reader.t.joinable()) { reader.t.join(); }
    src.stop();
    src.closeDevice();
    CHECK(fake.countStarting("Uninit") == 0);
}

// --- 6. a WHOLE-PROCESS freeze is not a stalled service (review note 1) -------
//
// Laptop sleep, a paused VM, a debugger break: every thread stops - the
// service's callback thread in this process AND the pipeline's reader - and
// the clock does not. On resume the first read can come back empty before the
// first callback, with "no callback for ages" true of the clock and false of
// the service. A stall is therefore only declared when the READER has been
// seeing nothing, read after read, for the whole limit.
void testAProcessFreezeIsNotAStall() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    src.setStreamStallLimitForTest(std::chrono::milliseconds(500));
    CHECK(src.start());
    fake.startService(kBlock, kPeriod, kWedgeAfter);
    {
        Reader reader(src);
        CHECK(waitFor([&] { return reader.got.load() > 20ull * kBlock; }, 3000));
    }  // the reader stops...
    fake.stopService();  // ...and so does the service's thread: the freeze
    std::vector<std::complex<float>> buf(20000);
    while (src.read(buf.data(), buf.size()) > 0) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // frozen, > the limit

    // Resume: the reader's first read lands before the service's first block.
    CHECK(src.read(buf.data(), buf.size()) == 0);
    const bool falselyAccused = src.faulted();
    std::printf("first empty read after a 1.2 s freeze (limit 0.5 s): %s\n",
                falselyAccused ? "STALL DECLARED" : "no stall");
    CHECK(!falselyAccused);
    fake.startService(kBlock, kPeriod, kWedgeAfter);
    {
        Reader reader(src);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        CHECK(reader.got.load() > 20ull * kBlock);
    }
    CHECK(!src.faulted());
    CHECK(!ringHas("stream stalled"));
    fake.stopService();
    src.stop();
    CHECK(fake.countStarting("Uninit") == 1);  // a healthy radio is torn down normally
    src.closeDevice();
}

// --- 7. the stall clock starts when Init RETURNS (review note 2) --------------
//
// Init is unbounded and the stream cannot deliver before it returns, so time
// spent inside it is not silence. A reader already running (the pipeline's
// thread) sees "accepting" before Init begins.
void testTheStallClockStartsAfterInit() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    fake.initDelayMs.store(900);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    fake.startService(kBlock, kPeriod, kWedgeAfter);
    Reader reader(src);  // reading before and throughout Init
    CHECK(src.start());  // 900 ms inside Init, then the service delivers at once
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const bool accused = src.faulted();
    std::printf("slow Init (900 ms, limit 300 ms): %s\n", accused ? "STALL DECLARED" : "no stall");
    CHECK(!accused);
    CHECK(reader.got.load() > 0);
    reader.run.store(false);
    if (reader.t.joinable()) { reader.t.join(); }
    fake.stopService();
    src.stop();
    src.closeDevice();
}

}  // namespace

int main() {
    testTheStreamCallbackNeverWaitsOnTheDiagnosticLog();
    testAPluginReloadStormNeverReachesTheVendorThread();
    testAStartAfterTheServiceStoppedAnsweringNeverEntersInit();
    testTheEventCallbackNeverWaitsOnTheDiagnosticLog();
    testASilentStreamIsNamedAndTheRadioIsLeftAlone();
    testAStreamThatNeverStartsIsNamedToo();
    testAProcessFreezeIsNotAStall();
    testTheStallClockStartsAfterInit();
    return testSummary("test_sdrplay_stall");
}
