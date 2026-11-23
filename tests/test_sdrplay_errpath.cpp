// SDRplay error paths and the vendor's call ORDER (fix lane "sdrplay", 2026-11-23).
//
// The field data of 2026-11-23 (FIELD-DATA section 6) showed three SDRplay faults that are FoxSDR's:
//   A  "SelectDevice failed: sdrplay_api_Fail (1)" seven times on one RSPduo: a SECOND radio of the same FoxSDR picked the RSP the
//      first one had open (the log shows the hand-off to the patch page and back itself worked). Also: the tuner re-select called
//      SelectDevice without LockDeviceApi.
//   B/C a stream that merely went quiet latched the whole process's SDRplay session as lost and skipped Uninit/ReleaseDevice, which left
//      the radio selected in the service ("no SDRplay device is connected" for every later open) and refused every later open.
//   D  a service that renumbered its samples backwards showed as a gap of 4,168,134,599 samples.
//
// The fake enforces the vendor's ordering (FakeSdrPlayApi::enforceOrdering): every violation answers sdrplay_api_Fail and is listed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

using cascade::source::SdrPlaySource;
namespace abi = cascade::source::sdrplay_abi;
using fakesdrplay::FakeSdrPlayApi;

namespace {

using Clock = std::chrono::steady_clock;

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

struct Reader {
    SdrPlaySource& src;
    std::atomic<bool> run{true};
    std::thread t;
    explicit Reader(SdrPlaySource& s) : src(s) {
        t = std::thread([this]() {
            std::vector<std::complex<float>> buf(20000);
            while (run.load()) {
                if (src.read(buf.data(), buf.size()) == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
        });
    }
    void stopReading() {
        run.store(false);
        if (t.joinable()) { t.join(); }
    }
    ~Reader() { stopReading(); }
};

std::string violations(const FakeSdrPlayApi& f) {
    std::string s;
    for (const std::string& v : f.orderingViolations) { s += v + "; "; }
    return s;
}

// --- A. one RSP, one owner in this process ------------------------------------

void testASecondRadioCannotSelectAnRspTheFirstHolds() {
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource first, second;
    first.setApiForTest(&fake.table);
    second.setApiForTest(&fake.table);
    CHECK(first.open(""));
    CHECK(first.start());

    // The 21 Nov RSPduo: "radio 2 added (a scanned device)" picks the RSP radio 1 has open.
    const bool opened = second.open("");
    std::printf("second open of the held RSP: %s, \"%s\"; violations: %s\n", opened ? "OPENED" : "refused", second.lastError(),
                violations(fake).c_str());
    CHECK(!opened);
    CHECK(std::string(second.lastError()).find("already open in another radio") != std::string::npos);
    // Said without a vendor call: no second SelectDevice, nothing for the service to answer Fail to.
    CHECK(fake.selectCount == 1);
    CHECK(fake.orderingViolations.empty());
    // The API lock was let go on the refusal.
    CHECK(fake.countStarting("UnlockDeviceApi") == fake.countStarting("LockDeviceApi"));

    // The first radio is untouched, and once it lets go the second one opens - the 20:30:51 line of the same log.
    CHECK(first.faulted() == false);
    first.stop();
    first.closeDevice();
    CHECK(second.open(""));
    CHECK(second.start());
    second.stop();
    second.closeDevice();
    CHECK(fake.orderingViolations.empty());
}

// The hand-off to the patch page and back (and the start-up restore): the old owner closes completely before the new one selects.
void testHandOffAwayAndBackKeepsTheVendorsOrder() {
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource receiver, patchRadio;
    receiver.setApiForTest(&fake.table);
    patchRadio.setApiForTest(&fake.table);
    CHECK(receiver.open(""));
    CHECK(receiver.start());
    for (int round = 0; round < 3; ++round) {
        receiver.stop();
        receiver.closeDevice();                // "the receiver's radio is handed to the patch page"
        CHECK(patchRadio.open(""));
        CHECK(patchRadio.start());
        patchRadio.stop();
        patchRadio.closeDevice();              // "handing it back to the receiver"
        CHECK(receiver.open(""));
        CHECK(receiver.start());
    }
    receiver.stop();
    receiver.closeDevice();
    std::printf("hand-off x3: violations: %s\n", violations(fake).c_str());
    CHECK(fake.orderingViolations.empty());
    CHECK(fake.releaseCount == fake.selectCount);
}

// SelectDevice is always between LockDeviceApi and UnlockDeviceApi - including the RSPduo tuner re-select.
void testTheTunerReselectIsBracketedByTheApiLock() {
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addRspDuo("2105000001");
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    CHECK(src.setAntenna("Tuner 2"));
    // Specification 3.4: the re-select takes the lock BEFORE ReleaseDevice (not after) and holds it through SelectDevice.
    CHECK(fake.releasedWhileLocked.size() == 1 && fake.releasedWhileLocked[0]);
    CHECK(fake.countStarting("UnlockDeviceApi") == fake.countStarting("LockDeviceApi"));
    std::printf("tuner re-select: violations: %s\n", violations(fake).c_str());
    CHECK(fake.orderingViolations.empty());
    CHECK(src.antenna() == "Tuner 2");
    src.closeDevice();
    CHECK(fake.orderingViolations.empty());
}

// A failed Init leaves nothing held: closing releases the RSP, and another radio can have it.
void testAFailedInitLeavesNothingHeld() {
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource a, b;
    a.setApiForTest(&fake.table);
    b.setApiForTest(&fake.table);
    CHECK(a.open(""));
    fake.initResult = abi::Fail;
    CHECK(!a.start());
    CHECK(std::string(a.lastError()).find("Init failed") != std::string::npos);
    a.closeDevice();
    CHECK(fake.releaseCount == 1);
    fake.initResult = abi::Success;
    CHECK(b.open(""));
    CHECK(b.start());
    b.stop();
    b.closeDevice();
    CHECK(fake.orderingViolations.empty());
}

// A failed SelectDevice leaves nothing claimed in this process, the lock is let go, and the next open works.
void testAFailedSelectLeavesNothingClaimed() {
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource a;
    a.setApiForTest(&fake.table);
    fake.selectResult = abi::Fail;
    CHECK(!a.open(""));
    CHECK(fake.countStarting("UnlockDeviceApi") == fake.countStarting("LockDeviceApi"));
    fake.selectResult = abi::Success;
    CHECK(a.open(""));
    a.closeDevice();
    CHECK(fake.orderingViolations.empty());
}

// --- B/C. a quiet stream is not a lost session ---------------------------------

void testAQuietStreamIsTornDownCleanlyAndTheNextStartRuns() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(src.start());                 // no service thread: not one callback comes (the 22 Nov RSP1A at 17:45)
    {
        Reader reader(src);
        CHECK(waitFor([&] { return src.faulted(); }, 3000));
        CHECK(std::string(src.lastError()).find("press START") != std::string::npos);
        reader.stopReading();
    }
    // The stop is a REAL Uninit - the service answers it - and the process's session is NOT lost.
    src.stop();
    CHECK(fake.countStarting("Uninit") == 1);
    CHECK(!fake.table.sessionLost);
    CHECK(!cascade::source::sdrPlayApiStateOf(fake.table).sessionLost);
    // The user's next START runs Init again instead of being refused for the rest of the session.
    CHECK(src.start());
    CHECK(!src.faulted());
    CHECK(fake.countStarting("Init") == 2);
    src.stop();
    src.closeDevice();
    // Nothing is left selected in the service, so another radio opens (the "no SDRplay device is connected" of fault C).
    CHECK(fake.releaseCount == 1);
    SdrPlaySource again;
    again.setApiForTest(&fake.table);
    CHECK(again.open(""));
    again.closeDevice();
    std::printf("quiet stream: violations: %s\n", violations(fake).c_str());
    CHECK(fake.orderingViolations.empty());
}

// ...and the protection stays where the service really is dead: its Uninit answers ServiceNotResponding -> the session IS lost.
void testADeadServiceStillLatchesTheSession() {
    cascade::core::DiagLog::instance().resetForTest();
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open(""));
    CHECK(src.start());
    fake.uninitResult = abi::ServiceNotResponding;
    src.stop();
    CHECK(fake.table.sessionLost);
    CHECK(src.deviceDead());
    src.closeDevice();
    CHECK(fake.countStarting("ReleaseDevice") == 0);
    SdrPlaySource again;
    again.setApiForTest(&fake.table);
    CHECK(!again.open(""));
}

// --- D. the sample counter ------------------------------------------------------

std::string gapsOf(const std::string& line) {
    const std::size_t at = line.find("service gaps ");
    return at == std::string::npos ? std::string() : line.substr(at);
}

void testANumberingThatGoesBackwardsIsNotFourBillionMissingSamples() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    src.setApiForTest(&fake.table);
    CHECK(src.open("") && src.start());
    short xi[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    short xq[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    fake.pushBlock(xi, xq, 8, 200000000u, 0);
    // A device-removed event renumbers the service's samples WITHOUT `reset`: this block is due at 200000008 and starts at 73167303.
    fake.pushBlock(xi, xq, 8, 73167303u, 0);
    const std::string g = gapsOf(src.streamHealthLine());
    std::printf("backwards renumbering: \"%s\"\n", g.c_str());
    CHECK(g.find("4168") == std::string::npos);
    CHECK(g == "service gaps 1 (0 samples)");
    // The baseline is re-armed from the new numbering: contiguous blocks after it are no gap.
    fake.pushBlock(xi, xq, 8, 73167311u, 0);
    CHECK(gapsOf(src.streamHealthLine()) == "service gaps 0 (0 samples)");
    // A forward jump still counts its samples.
    fake.pushBlock(xi, xq, 8, 73167400u, 0);
    CHECK(gapsOf(src.streamHealthLine()) == "service gaps 1 (81 samples)");
    src.stop();
    src.closeDevice();
}

// --- review of 885ebd7e: every vendor call after a stall is bounded --------------------------------------------------------------------------

// Runs `f` on a thread; true if it returned within `ms`. When it did not, `unhang` frees whatever it is parked in so the thread can be joined
// (a test must never hang).
bool finishesWithin(const std::function<void()>& f, int ms, const std::function<void()>& unhang) {
    std::atomic<bool> done{false};
    std::thread th([&] { f(); done.store(true); });
    const bool ok = waitFor([&] { return done.load(); }, ms);
    if (!ok) { unhang(); }
    th.join();
    return ok;
}

// Opens `a` (first RSP) and `b` (second RSP) on one fake, starts `a` and lets it stall (no service thread: nothing is ever delivered).
struct StallRig {
    FakeSdrPlayApi fake;
    SdrPlaySource a, b;
    StallRig() {
        cascade::core::DiagLog::instance().resetForTest();
        fake.enforceOrdering = true;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        fake.addDevice("1811003EFC", abi::kRsp1B);
        a.setApiForTest(&fake.table);
        b.setApiForTest(&fake.table);
        CHECK(a.open("serial=1811003EFB"));
        CHECK(b.open("serial=1811003EFC"));
        a.setStreamStallLimitForTest(std::chrono::milliseconds(300));
        CHECK(a.start());
        Reader reader(a);
        CHECK(waitFor([&] { return a.faulted(); }, 3000));
        reader.stopReading();
    }
};

// The sentence a refused open gives for a lost session.
void checkOpenRefusedAsLost(FakeSdrPlayApi& fake) {
    SdrPlaySource again;
    again.setApiForTest(&fake.table);
    bool opened = true;
    // a regressed latch pre-check would park this open on the session mutex a hung Close holds: free the Close so the test FAILS instead of hanging
    CHECK(finishesWithin([&] { opened = again.open(""); }, 2000, [&] { fake.releaseCloseHang.store(true); }));   // refused, never queued behind a parked worker
    CHECK(!opened);
    CHECK(std::string(again.lastError()) == std::string(cascade::source::sdrPlaySessionLostSentence()));
}

// ITEM 1: the stall Uninit answers (clean), then ReleaseDevice hangs. The close must still return (bounded), latch the session, and not
// block another radio close. Before the fix the clean Uninit lowered the stall flag and the release ran unbounded.
void testAStallThenAHungReleaseDeviceIsBounded() {
    StallRig r;
    r.fake.hangInReleaseDevice.store(true);
    r.a.stop();
    CHECK(r.fake.countStarting("Uninit") == 1);
    const bool returned = finishesWithin([&] { r.a.closeDevice(); }, 4000, [&] { r.fake.releaseReleaseDeviceHang.store(true); });
    CHECK(returned);
    CHECK(r.fake.table.sessionLostFlag.load());
    CHECK(ringHas("closed without ReleaseDevice"));
    CHECK(finishesWithin([&] { r.b.closeDevice(); }, 2000, [&] { r.fake.releaseReleaseDeviceHang.store(true); }));
    checkOpenRefusedAsLost(r.fake);
    r.fake.releaseReleaseDeviceHang.store(true);
    waitFor([&] { return r.fake.leftReleaseDevice.load(); }, 2000);
}

// The stall Uninit itself hangs: abandoned (already bounded), session latched, nothing else blocks.
void testAStallThenAHungUninitIsBounded() {
    StallRig r;
    r.fake.hangInUninit.store(true);
    const bool returned = finishesWithin([&] { r.a.stop(); r.a.closeDevice(); }, 6000, [&] { r.fake.releaseUninitHang.store(true); });
    CHECK(returned);
    CHECK(r.fake.table.sessionLostFlag.load());
    CHECK(r.fake.countStarting("ReleaseDevice") == 0);
    CHECK(finishesWithin([&] { r.b.closeDevice(); }, 2000, [&] { r.fake.releaseUninitHang.store(true); }));
    checkOpenRefusedAsLost(r.fake);
    r.fake.releaseUninitHang.store(true);
    waitFor([&] { return r.fake.leftUninit.load(); }, 2000);
}

// ITEM 2: the last radio closes after a stall and the session Close never returns. The worker holds the session mutex for good: the close
// must still return, and the next open must be REFUSED with the session-lost sentence, not queue behind the mutex.
void testAStallThenAHungCloseIsBoundedAndTheNextOpenIsRefused() {
    FakeSdrPlayApi fake;
    cascade::core::DiagLog::instance().resetForTest();
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource a;
    a.setApiForTest(&fake.table);
    CHECK(a.open(""));
    a.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(a.start());
    {
        Reader reader(a);
        CHECK(waitFor([&] { return a.faulted(); }, 3000));
        reader.stopReading();
    }
    fake.hangInClose.store(true);
    a.stop();
    const bool returned = finishesWithin([&] { a.closeDevice(); }, 4000, [&] { fake.releaseCloseHang.store(true); });
    CHECK(returned);
    CHECK(fake.insideClose.load());
    CHECK(fake.releaseCount == 1);
    CHECK(fake.table.sessionLostFlag.load());
    checkOpenRefusedAsLost(fake);
    // a scan is refused too, without a vendor call and without waiting for the mutex
    std::size_t listed = 1;
    CHECK(finishesWithin([&] { listed = cascade::source::enumerateSdrPlayWith(fake.table).size(); }, 2000, [] {}));
    CHECK(listed == 0);
    fake.releaseCloseHang.store(true);
    CHECK(waitFor([&] { return fake.leftClose.load(); }, 3000));   // the abandoned worker is gone before the fake is
    waitFor([&] { return false; }, 50);
}

// S1 (review of 3813659b): an open that is ALREADY WAITING on the session mutex when a hung Close is abandoned gets the mutex when that Close
// returns late. The bool mirror may never have been set (markSessionLost only try-locks), so the latch must be re-checked after the mutex: the
// waiting open is refused as lost with no vendor call, and a service restart is not answered "reopen in this process".
void testAnOpenWaitingBehindAHungCloseIsRefusedWhenItReturnsLate() {
    FakeSdrPlayApi fake;
    cascade::core::DiagLog::instance().resetForTest();
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource a, waiter;
    a.setApiForTest(&fake.table);
    waiter.setApiForTest(&fake.table);
    CHECK(a.open(""));
    a.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(a.start());
    {
        Reader reader(a);
        CHECK(waitFor([&] { return a.faulted(); }, 3000));
        reader.stopReading();
    }
    a.stop();
    fake.hangInClose.store(true);
    std::thread closer([&] { a.closeDevice(); });
    CHECK(waitFor([&] { return fake.insideClose.load(); }, 2000));   // the Close worker now holds the session mutex
    const int opensBefore = fake.openCount;
    const int selectsBefore = fake.selectCount;
    bool opened = true;
    std::thread opener([&] { opened = waiter.open(""); });          // passes the (still unset) latch pre-check and waits on the mutex
    closer.join();                                                   // the 1 s bound: abandoned, latch set
    CHECK(fake.table.sessionLostFlag.load());
    fake.releaseCloseHang.store(true);                               // the hung Close returns late and frees the mutex
    CHECK(waitFor([&] { return fake.leftClose.load(); }, 3000));
    opener.join();
    CHECK(!opened);
    CHECK(std::string(waiter.lastError()) == std::string(cascade::source::sdrPlaySessionLostSentence()));
    CHECK(fake.openCount == opensBefore);                            // no vendor call at all
    CHECK(fake.selectCount == selectsBefore);
    CHECK(cascade::source::sdrPlayApplyServiceRestart(fake.table, true) == cascade::source::SdrPlayAfterRestart::RestartFoxSdr);
}

// S3: a stalled RSPduo is not re-selected (ReleaseDevice + SelectDevice) while stopped: refused, no vendor call.
void testAStalledRspDuoRefusesATunerChange() {
    FakeSdrPlayApi fake;
    cascade::core::DiagLog::instance().resetForTest();
    fake.enforceOrdering = true;
    fake.addRspDuo("2105000001");
    SdrPlaySource a;
    a.setApiForTest(&fake.table);
    CHECK(a.open(""));
    a.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(a.start());
    {
        Reader reader(a);
        CHECK(waitFor([&] { return a.faulted(); }, 3000));
        reader.stopReading();
    }
    a.stop();                                   // clean Uninit: streamStalled_ is lowered, stalledThisOpen_ is not
    const int releasesBefore = fake.releaseCount;
    CHECK(!a.setAntenna("Tuner 2"));
    CHECK(fake.releaseCount == releasesBefore);
    CHECK(std::string(a.lastError()).find("pick the radio again") != std::string::npos);
    a.closeDevice();
}

// ITEM 3: the stall Uninit was REFUSED (Fail): START must not run Init on a stream the service still holds. (A clean Uninit lets START run:
// testAQuietStreamIsTornDownCleanlyAndTheNextStartRuns.)
void testStartAfterAStallWhoseUninitFailedIsRefused() {
    StallRig r;
    r.fake.uninitResult = abi::Fail;
    r.a.stop();
    CHECK(r.fake.countStarting("Uninit") == 1);
    const int initsBefore = r.fake.countStarting("Init");
    CHECK(!r.a.start());
    CHECK(r.fake.countStarting("Init") == initsBefore);
    CHECK(std::string(r.a.lastError()).find("pick the radio again") != std::string::npos);
    // closing still works and releases the radio (bounded: this open stalled)
    r.fake.uninitResult = abi::Success;
    r.a.closeDevice();
    CHECK(r.fake.releaseCount >= 1);
    r.b.closeDevice();
}

// The 22 Nov shape: after the stall the service answers NotInitialised ("Device has not been selected", specification 3.16) to Uninit: that is
// a clean stop, and the following START Init answers NotInitialised too - said, not hung, and the radio can then be closed and picked again.
void testANotInitialisedStallIsStoppedAndStartSaysSo() {
    StallRig r;
    r.fake.uninitResult = abi::NotInitialised;
    r.a.stop();
    r.fake.initResult = abi::NotInitialised;
    CHECK(!r.a.start());
    CHECK(std::string(r.a.lastError()).find("NotInitialised") != std::string::npos);
    r.fake.initResult = abi::Success;
    r.fake.uninitResult = abi::Success;
    r.a.closeDevice();
    SdrPlaySource again;
    again.setApiForTest(&r.fake.table);
    CHECK(again.open("serial=1811003EFB"));
    again.closeDevice();
    r.b.closeDevice();
}

// ITEM 7: two rows racing to open the same RSP from two threads: exactly one wins, and the loser failure path leaks no claim.
void testTwoRowsRacingForOneRspExactlyOneWins() {
    for (int round = 0; round < 40; ++round) {
        FakeSdrPlayApi fake;
        fake.enforceOrdering = true;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource x, y;
        x.setApiForTest(&fake.table);
        y.setApiForTest(&fake.table);
        std::atomic<bool> go{false};
        std::atomic<int> ready{0};
        bool rx = false, ry = false;
        std::thread tx([&] { ++ready; while (!go.load()) {} rx = x.open(""); });
        std::thread ty([&] { ++ready; while (!go.load()) {} ry = y.open(""); });
        while (ready.load() < 2) {}
        go.store(true);
        tx.join();
        ty.join();
        CHECK(rx != ry);   // exactly one
        CHECK(fake.orderingViolations.empty());
        SdrPlaySource& winner = rx ? x : y;
        SdrPlaySource& loser = rx ? y : x;
        CHECK(std::string(loser.lastError()).find("already open in another radio") != std::string::npos);
        loser.closeDevice();    // the loser close is harmless and must not release the winner claim
        SdrPlaySource third;
        third.setApiForTest(&fake.table);
        CHECK(!third.open(""));  // still held by the winner
        winner.closeDevice();
        CHECK(loser.open(""));   // nothing leaked: the loser itself can now have it
        loser.closeDevice();
        CHECK(fake.orderingViolations.empty());
    }
}

// ITEM 7: a hand-off while a stall teardown is in flight: the new owner is refused while the old one (slow, bounded) ReleaseDevice runs, and
// gets the radio the moment it is done - never a SelectDevice before the release.
void testAHandOffDuringAStallTeardownWaitsForTheRelease() {
    FakeSdrPlayApi fake;
    cascade::core::DiagLog::instance().resetForTest();
    fake.enforceOrdering = true;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource a, b;
    a.setApiForTest(&fake.table);
    b.setApiForTest(&fake.table);
    CHECK(a.open(""));
    a.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(a.start());
    {
        Reader reader(a);
        CHECK(waitFor([&] { return a.faulted(); }, 3000));
        reader.stopReading();
    }
    a.stop();
    fake.hangInReleaseDevice.store(true);                 // slow, not dead: freed after 300 ms, inside the 1 s bound
    std::thread closer([&] { a.closeDevice(); });
    CHECK(waitFor([&] { return fake.insideReleaseDevice.load(); }, 2000));
    bool opened = false;
    std::thread opener([&] { opened = waitFor([&] { return b.open(""); }, 4000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fake.selectCount == 1);                         // the new owner has not reached SelectDevice
    fake.releaseReleaseDeviceHang.store(true);
    closer.join();
    opener.join();
    CHECK(opened);
    CHECK(fake.orderingViolations.empty());
    CHECK(!fake.table.sessionLostFlag.load());
    b.closeDevice();
}

}  // namespace

int main() {
    testASecondRadioCannotSelectAnRspTheFirstHolds();
    testHandOffAwayAndBackKeepsTheVendorsOrder();
    testTheTunerReselectIsBracketedByTheApiLock();
    testAFailedInitLeavesNothingHeld();
    testAFailedSelectLeavesNothingClaimed();
    testAQuietStreamIsTornDownCleanlyAndTheNextStartRuns();
    testADeadServiceStillLatchesTheSession();
    testANumberingThatGoesBackwardsIsNotFourBillionMissingSamples();
    testStartAfterAStallWhoseUninitFailedIsRefused();
    testANotInitialisedStallIsStoppedAndStartSaysSo();
    testTwoRowsRacingForOneRspExactlyOneWins();
    testAHandOffDuringAStallTeardownWaitsForTheRelease();
    // The three that abandon a worker inside their fake go last; each frees it before it returns.
    testAStallThenAHungReleaseDeviceIsBounded();
    testAStallThenAHungUninitIsBounded();
    testAStallThenAHungCloseIsBoundedAndTheNextOpenIsRefused();
    testAnOpenWaitingBehindAHungCloseIsRefusedWhenItReturnsLate();
    testAStalledRspDuoRefusesATunerChange();
    return testSummary("test_sdrplay_errpath");
}
