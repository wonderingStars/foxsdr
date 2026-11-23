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
    return testSummary("test_sdrplay_errpath");
}
