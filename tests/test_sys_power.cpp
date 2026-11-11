// Tests for core/sys_keep_awake.hpp, core/sys_resume_watch.hpp and core/sys_device_watch.hpp (lane sysmodels): the three reliability models.
//
// No hardware, no power setting, no radio: the keep-awake guard is driven through an injected function table (the one real call is a read-back of a
// system-required request held for a few milliseconds and released, on a thread of its own); the resume watcher and the device watcher are fed synthetic events
// through the same paths the OS uses (the device watcher's own window receives a real WM_DEVICECHANGE sent to it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/sys_device_watch.hpp"
#include "core/sys_keep_awake.hpp"
#include "core/sys_resume_watch.hpp"
#include "test_check.hpp"

using namespace cascade::core::sys;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool waitFor(Pred p, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (p()) { return true; }
        std::this_thread::sleep_for(5ms);
    }
    return p();
}

// ---------------------------------------------------------------- keep awake
struct FakeOs {
    std::mutex mu;
    std::vector<unsigned long> calls;
    std::vector<std::thread::id> threads;
    unsigned long answer = 1;   // previous state returned; 0 = the OS refused
    KeepAwake::Fns fns() {
        KeepAwake::Fns f;
        f.setState = [this](unsigned long flags) {
            std::lock_guard<std::mutex> lk(mu);
            calls.push_back(flags);
            threads.push_back(std::this_thread::get_id());
            return answer;
        };
        return f;
    }
    int sets() { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto c : calls) { if (c != kEsContinuous) { ++n; } } return n; }
    int clears() { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto c : calls) { if (c == kEsContinuous) { ++n; } } return n; }
};

void testKeepAwakeCounting() {
    FakeOs os;
    KeepAwake k(os.fns());
    CHECK(k.holders() == 0 && !k.active());
    {
        auto a = k.acquire();
        CHECK(k.holders() == 1 && k.active());
        CHECK(os.sets() == 1 && os.clears() == 0);
        CHECK(os.calls.at(0) == (kEsContinuous | kEsSystemRequired));   // the display is NOT held unless asked
        auto b = k.acquire();
        auto c = k.acquire();
        CHECK(k.holders() == 3 && os.sets() == 1);                       // the OS is asked once for three holds
        b.reset();
        b.reset();                                                       // a second release does nothing
        CHECK(k.holders() == 2 && os.clears() == 0 && k.active());
        auto moved = std::move(c);
        CHECK(!c && moved);
        CHECK(k.holders() == 2);
        a.reset();
        CHECK(k.holders() == 1 && os.clears() == 0);
        moved.reset();
        CHECK(k.holders() == 0 && !k.active() && os.clears() == 1);
    }
    CHECK(os.calls.back() == kEsContinuous);
    CHECK(os.threads.at(0) == os.threads.at(1));                         // one thread sets and clears
    CHECK(os.threads.at(0) != std::this_thread::get_id());
    // A second period asks again.
    { auto l = k.acquire(); CHECK(k.active()); }
    CHECK(os.sets() == 2 && os.clears() == 2 && k.setCalls() == 2 && k.clearCalls() == 2);
    CHECK(os.threads.at(2) == os.threads.at(3));
    // Move-assignment releases what the target held.
    {
        auto x = k.acquire();
        auto y = k.acquire();
        CHECK(k.holders() == 2);
        x = std::move(y);
        CHECK(k.holders() == 1);
    }
    CHECK(k.holders() == 0 && !k.active());
}

void testKeepAwakeExitPaths() {
    FakeOs os;
    KeepAwake k(os.fns());
    // An exception unwinding through a scope that holds a lease releases it.
    try {
        auto l = k.acquire();
        CHECK(k.active());
        throw 42;
    } catch (int) {}
    CHECK(k.holders() == 0 && !k.active() && os.clears() == 1);
    // An early return.
    auto early = [&](bool leave) -> int { auto l = k.acquire(); if (leave) { return 1; } return 2; };
    CHECK(early(true) == 1 && k.holders() == 0);
    CHECK(early(false) == 2 && k.holders() == 0);
    CHECK(os.sets() == 3 && os.clears() == 3);
    // The guard's destructor with a lease still out clears the state (the lease is released first here, as scope order guarantees; the guard owns no leftover).
    {
        FakeOs os2;
        {
            KeepAwake k2(os2.fns());
            auto l = k2.acquire();
            CHECK(os2.sets() == 1);
        }
        CHECK(os2.clears() == 1);
    }
    // The display request is a flag of its own.
    {
        FakeOs os3;
        KeepAwake k3(os3.fns(), /*alsoKeepDisplayOn=*/true);
        { auto l = k3.acquire(); }
        CHECK(os3.calls.at(0) == (kEsContinuous | kEsSystemRequired | kEsDisplayRequired));
    }
}

void testKeepAwakeRefusal() {
    FakeOs os;
    os.answer = 0;   // the OS refuses
    KeepAwake k(os.fns());
    auto l = k.acquire();
    CHECK(k.holders() == 1 && !k.active());   // the failure is visible, the hold is still counted
    CHECK(os.sets() == 1);
    l.reset();
    CHECK(k.holders() == 0 && os.clears() == 1 && !k.active());
    // An empty function table never crashes.
    KeepAwake none(KeepAwake::Fns{});
    { auto x = none.acquire(); CHECK(!none.active()); }
    CHECK(none.holders() == 0);
}

void testKeepAwakeConcurrency() {
    FakeOs os;
    KeepAwake k(os.fns());
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 300; ++i) {
                auto l = k.acquire();
                if (i % 7 == 0) { std::this_thread::yield(); }
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(k.holders() == 0 && !k.active());
    CHECK(os.sets() == os.clears() && os.sets() >= 1);
    // The OS saw strictly alternating request / clear, each pair from one thread, never two requests in a row.
    bool alternating = true;
    for (std::size_t i = 0; i < os.calls.size(); ++i) {
        const bool wantSet = (i % 2 == 0);
        if ((os.calls[i] != kEsContinuous) != wantSet) { alternating = false; }
        if (i % 2 == 1 && os.threads[i] != os.threads[i - 1]) { alternating = false; }
    }
    CHECK(alternating);
    std::printf("keep-awake concurrency: %d request/clear pairs for 2400 acquire/release\n", os.sets());
}

// The ONE real call: the request is made on the holder thread, its effect is read back from the OS (SetThreadExecutionState returns the previous state, so a repeat of
// the same call reads what the first one set), and the release is read back the same way. System-required only, held for the duration of this scope.
void testKeepAwakeRealReadBack() {
#ifdef _WIN32
    std::atomic<unsigned long> firstPrev{0}, readBack{0}, heldAtClear{0}, afterClear{0};
    KeepAwake::Fns real = KeepAwake::realFns();
    KeepAwake::Fns wrapped;
    wrapped.setState = [&](unsigned long flags) -> unsigned long {
        if (flags == kEsContinuous) {
            heldAtClear = real.setState(kEsContinuous);   // the previous state = what was in force until now
            afterClear = real.setState(kEsContinuous);    // and now: what is left
            return heldAtClear;
        }
        firstPrev = real.setState(flags);
        readBack = real.setState(flags);                  // the previous state = what the line above set
        return firstPrev;
    };
    {
        KeepAwake k(wrapped);
        auto l = k.acquire();
        CHECK(k.active());   // the OS accepted it
    }
    CHECK(firstPrev.load() != 0);
    CHECK((readBack.load() & kEsSystemRequired) != 0);
    CHECK((readBack.load() & kEsContinuous) != 0);
    CHECK((readBack.load() & kEsDisplayRequired) == 0);
    CHECK((heldAtClear.load() & kEsSystemRequired) != 0);   // still in force right up to the release
    CHECK((afterClear.load() & kEsSystemRequired) == 0);    // and gone after it
    std::printf("keep-awake real read-back: set -> previous 0x%lx, repeat read 0x%lx, at clear 0x%lx, after clear 0x%lx\n", firstPrev.load(), readBack.load(), heldAtClear.load(),
                afterClear.load());
#else
    SKIP_LINUX("SetThreadExecutionState exists only on Windows");
#endif
}

// ---------------------------------------------------------------- resume gate
void testResumeGate() {
    CHECK(powerEventFromPbt(4) == PowerEvent::Suspend);
    CHECK(powerEventFromPbt(7) == PowerEvent::ResumeSuspend);
    CHECK(powerEventFromPbt(18) == PowerEvent::ResumeAutomatic);
    CHECK(powerEventFromPbt(0) == PowerEvent::Other);
    CHECK(powerEventFromPbt(9) == PowerEvent::Other);

    ResumeGateConfig cfg;
    cfg.settleMs = 4000;
    cfg.minIntervalMs = 30000;
    {   // one resume: not due until the settle time has passed, then once
        ResumeGate g(cfg);
        CHECK(!g.due(0) && g.nextDueMs() == -1);
        g.onEvent(PowerEvent::ResumeAutomatic, 1000);
        CHECK(g.nextDueMs() == 5000);
        CHECK(!g.due(1000) && !g.due(4999));
        CHECK(g.due(5000));
        CHECK(!g.due(5001) && !g.due(99999));   // fired once
    }
    {   // a burst of resumes (automatic, then user input) is one action, at the FIRST one's time
        ResumeGate g(cfg);
        g.onEvent(PowerEvent::ResumeAutomatic, 0);
        g.onEvent(PowerEvent::ResumeSuspend, 2500);
        CHECK(g.nextDueMs() == 4000);
        CHECK(g.due(4000));
        CHECK(!g.due(4001));
    }
    {   // suspend cancels a pending action and shuts the gate until the next resume
        ResumeGate g(cfg);
        g.onEvent(PowerEvent::ResumeAutomatic, 0);
        g.onEvent(PowerEvent::Suspend, 1000);
        CHECK(g.suspended() && g.nextDueMs() == -1);
        CHECK(!g.due(10000));
        g.onEvent(PowerEvent::ResumeAutomatic, 20000);
        CHECK(!g.suspended() && g.nextDueMs() == 24000);
        CHECK(g.due(24000));
    }
    {   // the rate limit defers, never drops
        ResumeGate g(cfg);
        g.onEvent(PowerEvent::ResumeAutomatic, 0);
        CHECK(g.due(4000));
        g.onEvent(PowerEvent::Suspend, 5000);
        g.onEvent(PowerEvent::ResumeAutomatic, 10000);        // would be due at 14000, but the last action was at 4000: not before 34000
        CHECK(g.nextDueMs() == 34000);
        CHECK(!g.due(20000) && !g.due(33999));
        CHECK(g.due(34000));
        // after the interval has passed, the next resume waits only the settle time
        g.onEvent(PowerEvent::ResumeAutomatic, 100000);
        CHECK(g.nextDueMs() == 104000);
    }
    {   // other events change nothing; zero settle is due at once; suspend before any resume
        ResumeGate g({0, 0});
        g.onEvent(PowerEvent::Other, 0);
        CHECK(!g.due(0));
        g.onEvent(PowerEvent::ResumeAutomatic, 7);
        CHECK(g.due(7));
        ResumeGate h(cfg);
        h.onEvent(PowerEvent::Suspend, 0);
        h.onEvent(PowerEvent::Suspend, 1);
        CHECK(h.suspended() && !h.due(100000));
    }
    {   // a clock step backwards (never expected, but a gate must not fire early because of it)
        ResumeGate g(cfg);
        g.onEvent(PowerEvent::ResumeAutomatic, 10000);
        CHECK(!g.due(5000) && g.due(14000));
    }
}

void testResumeWatcher() {
    ResumeGateConfig cfg;
    cfg.settleMs = 60;
    cfg.minIntervalMs = 400;
    std::atomic<int> ran{0};
    ResumeWatcher w(cfg, [&] { ++ran; });
    CHECK(!w.osRegistered() && w.actionsRun() == 0);
    CHECK(w.start(/*registerWithOs=*/false));
    CHECK(!w.osRegistered());
    w.inject(PowerEvent::ResumeAutomatic);
    w.inject(PowerEvent::ResumeSuspend);   // the same burst
    CHECK(waitFor([&] { return ran.load() == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(ran.load() == 1 && w.actionsRun() == 1 && w.eventsSeen() == 2);
    // Suspend then nothing: no action.
    w.inject(PowerEvent::Suspend);
    std::this_thread::sleep_for(300ms);
    CHECK(ran.load() == 1);
    // A resume inside the rate-limit interval is deferred, not dropped.
    const auto t0 = std::chrono::steady_clock::now();
    w.inject(PowerEvent::ResumeAutomatic);
    CHECK(waitFor([&] { return ran.load() == 2; }));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited >= 0);
    w.stop();
    w.stop();   // idempotent
    w.inject(PowerEvent::ResumeAutomatic);   // after stop: counted, no action, no crash
    std::this_thread::sleep_for(150ms);
    CHECK(ran.load() == 2);
}

void testResumeWatcherOsRegistration() {
#ifdef _WIN32
    ResumeGateConfig cfg;
    std::atomic<int> ran{0};
    ResumeWatcher w(cfg, [&] { ++ran; });
    const bool ok = w.start(/*registerWithOs=*/true);
    CHECK(ok);
    CHECK(w.osRegistered());
    w.stop();
    CHECK(!w.osRegistered());
    CHECK(ran.load() == 0);   // no power event happened: a registration alone triggers nothing
    std::printf("resume watcher: OS registration %s and was released\n", ok ? "succeeded" : "FAILED");
#else
    SKIP_LINUX("RegisterSuspendResumeNotification exists only on Windows");
#endif
}

void testResumeWatcherConcurrency() {
    ResumeGateConfig cfg;
    cfg.settleMs = 20;
    cfg.minIntervalMs = 100;
    std::atomic<int> ran{0};
    ResumeWatcher w(cfg, [&] { ++ran; });
    CHECK(w.start(false));
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) { w.inject((i + t) % 3 == 0 ? PowerEvent::Suspend : PowerEvent::ResumeAutomatic); }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(w.eventsSeen() == 800);
    w.inject(PowerEvent::ResumeAutomatic);
    CHECK(waitFor([&] { return ran.load() >= 1; }));
    w.stop();
    CHECK(ran.load() >= 1 && ran.load() <= 5);
}

// ---------------------------------------------------------------- device arrival
const char* kRtl = "\\\\?\\USB#VID_0BDA&PID_2838#00000001#{a5dcbf10-6530-11d2-901f-00c04fb951ed}";
const char* kWinUsb = "\\\\?\\USB#VID_0BDA&PID_2838#00000001#{dee824ef-729b-4a0e-9c14-b7117d33a817}";

void testParse() {
    auto p = parseDeviceInterfacePath(kRtl);
    CHECK(p.valid && p.vid == 0x0BDA && p.pid == 0x2838 && p.isUsbRadioClass);
    CHECK(p.classGuid == "a5dcbf10-6530-11d2-901f-00c04fb951ed");
    p = parseDeviceInterfacePath(kWinUsb);
    CHECK(p.valid && p.isUsbRadioClass && p.classGuid == "dee824ef-729b-4a0e-9c14-b7117d33a817");
    // Upper-case GUID, a composite interface (MI_00) and a long instance id.
    p = parseDeviceInterfacePath("\\\\?\\usb#vid_1d50&pid_604b&mi_00#7&1b2c3d4e&0&0000#{DEE824EF-729B-4A0E-9C14-B7117D33A817}");
    CHECK(p.valid && p.vid == 0x1D50 && p.pid == 0x604B && p.isUsbRadioClass);
    // A USB interface of some OTHER class is valid but not a radio class.
    p = parseDeviceInterfacePath("\\\\?\\USB#VID_046D&PID_C52B#5&2a&0&1#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK(p.valid && !p.isUsbRadioClass);
    // Not USB, hubs without ids, bad shape, empty, over-long.
    CHECK(!parseDeviceInterfacePath("").valid);
    CHECK(!parseDeviceInterfacePath("\\\\?\\HID#VID_0BDA&PID_2838#1#{a5dcbf10-6530-11d2-901f-00c04fb951ed}").valid);
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#ROOT_HUB30#4&1&0&0#{f18a0e88-c30c-11d0-8815-00a0c906bed8}").valid);
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDA&PID_283#1#{a5dcbf10-6530-11d2-901f-00c04fb951ed}").valid);       // short pid
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDG&PID_2838#1#{a5dcbf10-6530-11d2-901f-00c04fb951ed}").valid);      // non-hex
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDA&PID_2838#1#a5dcbf10-6530-11d2-901f-00c04fb951ed").valid);       // no braces
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDA&PID_2838#1#{a5dcbf10-6530-11d2-901f-00c04fb951e}").valid);      // short guid
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDA&PID_2838{a5dcbf10-6530-11d2-901f-00c04fb951ed}").valid);        // no separators
    CHECK(!parseDeviceInterfacePath("\\\\?\\USB#VID_0BDA&PID_2838X#1#{a5dcbf10-6530-11d2-901f-00c04fb951ed}").valid);    // junk after the pid
    CHECK(!parseDeviceInterfacePath(std::string(600, 'A')).valid);
    CHECK(!parseDeviceInterfacePath(std::string(kRtl) + std::string(600, 'x')).valid);
    // Fuzz: random prefixes and mutations of valid paths never crash and never claim a radio class for a wrong GUID.
    std::uint64_t s = 99;
    auto next = [&] { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<std::uint32_t>(s >> 33); };
    for (int i = 0; i < 4000; ++i) {
        std::string m = (i & 1) ? kRtl : kWinUsb;
        const int n = 1 + static_cast<int>(next() % 3);
        for (int k = 0; k < n; ++k) { m[next() % m.size()] = static_cast<char>(next()); }
        if (next() % 4 == 0) { m.resize(next() % m.size()); }
        const auto r = parseDeviceInterfacePath(m);
        if (r.isUsbRadioClass) { CHECK(r.valid && (r.classGuid == "a5dcbf10-6530-11d2-901f-00c04fb951ed" || r.classGuid == "dee824ef-729b-4a0e-9c14-b7117d33a817")); }
    }
}

void testCoalescer() {
    ArrivalConfig cfg;
    cfg.settleMs = 1500;
    cfg.maxWaitMs = 10000;
    const auto rtl = parseDeviceInterfacePath(kRtl);
    const auto other = parseDeviceInterfacePath("\\\\?\\USB#VID_046D&PID_C52B#5&2a&0&1#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ArrivalSummary out;
    {   // a re-plug burst (several interfaces, both classes) is one action, after the quiet time
        ArrivalCoalescer c(cfg);
        CHECK(!c.due(0, out));
        CHECK(c.onEvent(DeviceChange::Removal, rtl, 100));
        CHECK(c.onEvent(DeviceChange::Arrival, rtl, 600));
        CHECK(c.onEvent(DeviceChange::Arrival, parseDeviceInterfacePath(kWinUsb), 900));
        CHECK(!c.onEvent(DeviceChange::Arrival, other, 950));    // not a radio class: ignored
        CHECK(!c.onEvent(DeviceChange::Arrival, DeviceInterfaceInfo{}, 960));   // not valid: ignored
        CHECK(!c.due(1000, out) && !c.due(2399, out));
        CHECK(c.due(2400, out) && out.arrivals == 2 && out.removals == 1);
        CHECK(!c.pending() && !c.due(99999, out));
    }
    {   // events that never go quiet still act every maxWait
        ArrivalConfig c2 = cfg;
        ArrivalCoalescer c(c2);
        for (int t = 0; t < 10000; t += 1000) { CHECK(c.onEvent(DeviceChange::Arrival, rtl, t)); CHECK(!c.due(t, out)); }
        CHECK(c.due(10000, out) && out.arrivals == 10);
    }
    {   // a second burst after the first is its own
        ArrivalCoalescer c(cfg);
        c.onEvent(DeviceChange::Arrival, rtl, 0);
        CHECK(c.due(1500, out) && out.arrivals == 1);
        c.onEvent(DeviceChange::Removal, rtl, 5000);
        CHECK(c.due(6500, out) && out.arrivals == 0 && out.removals == 1);
    }
}

void testDeviceWatcherInjected() {
#ifdef _WIN32
    ArrivalConfig cfg;
    cfg.settleMs = 120;
    cfg.maxWaitMs = 2000;
    std::mutex mu;
    std::vector<ArrivalSummary> got;
    DeviceArrivalWatcher w(cfg, [&](const ArrivalSummary& s) { std::lock_guard<std::mutex> lk(mu); got.push_back(s); });
    CHECK(!w.running());
    CHECK(!w.inject(DeviceChange::Arrival, kRtl));   // not started: refused
    CHECK(w.start(/*registerWithOs=*/false));
    CHECK(w.running() && !w.osRegistered());
    CHECK(w.inject(DeviceChange::Removal, kRtl));
    CHECK(w.inject(DeviceChange::Arrival, kRtl));
    CHECK(w.inject(DeviceChange::Arrival, kWinUsb));
    CHECK(w.inject(DeviceChange::Arrival, "\\\\?\\USB#VID_046D&PID_C52B#5&2a&0&1#{4d1e55b2-f16f-11cf-88cb-001111000030}"));   // another class
    CHECK(w.inject(DeviceChange::Arrival, "garbage"));
    CHECK(w.eventsSeen() == 5 && w.eventsCounted() == 3);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return got.size() == 1; }));
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(got.size() == 1 && got[0].arrivals == 2 && got[0].removals == 1);
    }
    // A second burst later is a second action.
    CHECK(w.inject(DeviceChange::Arrival, kRtl));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return got.size() == 2; }));
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(got.size() == 2 && got[1].arrivals == 1 && got[1].removals == 0);
    }
    CHECK(w.actionsRun() == 2);
    // Starting twice is harmless; stop ends the window; inject after stop is refused; stop twice is harmless.
    CHECK(w.start(false));
    w.stop();
    CHECK(!w.running());
    CHECK(!w.inject(DeviceChange::Arrival, kRtl));
    w.stop();
#else
    SKIP_LINUX("the device-arrival window exists only on Windows");
#endif
}

void testDeviceWatcherConcurrency() {
#ifdef _WIN32
    ArrivalConfig cfg;
    cfg.settleMs = 100;
    cfg.maxWaitMs = 3000;
    std::atomic<int> arrivals{0};
    std::atomic<int> removals{0};
    DeviceArrivalWatcher w(cfg, [&](const ArrivalSummary& s) { arrivals += s.arrivals; removals += s.removals; });
    CHECK(w.start(false));
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) { w.inject((i + t) % 2 ? DeviceChange::Arrival : DeviceChange::Removal, (i & 1) ? kRtl : kWinUsb); }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(w.eventsSeen() == 400 && w.eventsCounted() == 400);
    CHECK(waitFor([&] { return arrivals.load() + removals.load() == 400; }));
    CHECK(arrivals.load() == 200 && removals.load() == 200);
    w.stop();
#else
    SKIP_LINUX("the device-arrival window exists only on Windows");
#endif
}

void testDeviceWatcherOsRegistration() {
#ifdef _WIN32
    ArrivalConfig cfg;
    DeviceArrivalWatcher w(cfg, [](const ArrivalSummary&) {});
    const bool ok = w.start(/*registerWithOs=*/true);
    CHECK(ok);
    CHECK(w.osRegistered());
    w.stop();
    CHECK(!w.osRegistered() && !w.running());
    std::printf("device watcher: both interface classes registered with the OS (%s) and released\n", ok ? "yes" : "NO");
#else
    SKIP_LINUX("RegisterDeviceNotification exists only on Windows");
#endif
}

// SOURCE SCAN: the watcher never opens, enumerates or resets a device, never logs, and never stores a serial.
void testWatcherSource() {
    const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
    for (const char* f : {"src/core/sys_device_watch.cpp", "src/core/sys_device_watch.hpp"}) {
        std::ifstream in(root / f, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(!text.empty());
        std::string code;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t e = text.find('\n', pos);
            if (e == std::string::npos) { e = text.size(); }
            const std::string line = text.substr(pos, e - pos);
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line.compare(first, 2, "//") != 0) { code += line + "\n"; }
            pos = e + 1;
        }
        for (const char* banned : {"CreateFile", "SetupDi", "WinUsb_", "DeviceIoControl", "usb_device.hpp", "winusb_device", "CM_Get", "diagLog", "printf", "fprintf", "std::cerr", "std::cout", "OutputDebug", "serial"}) {
            const bool hit = code.find(banned) != std::string::npos;
            if (hit) { std::printf("scan: %s names %s\n", f, banned); }
            CHECK(!hit);
        }
    }
    for (const char* f : {"src/core/sys_keep_awake.cpp", "src/core/sys_resume_watch.cpp"}) {
        std::ifstream in(root / f, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(!text.empty());
        CHECK(text.find("PowerSetRequest") == std::string::npos && text.find("SetSuspendState") == std::string::npos && text.find("powercfg") == std::string::npos &&
              text.find("SetSystemPowerState") == std::string::npos);   // nothing that CHANGES a power setting or sleeps the machine
    }
}

}  // namespace

int main() {
    testKeepAwakeCounting();
    testKeepAwakeExitPaths();
    testKeepAwakeRefusal();
    testKeepAwakeConcurrency();
    testKeepAwakeRealReadBack();
    testResumeGate();
    testResumeWatcher();
    testResumeWatcherOsRegistration();
    testResumeWatcherConcurrency();
    testParse();
    testCoalescer();
    testDeviceWatcherInjected();
    testDeviceWatcherConcurrency();
    testDeviceWatcherOsRegistration();
    testWatcherSource();
    return testSummary("test_sys_power");
}
