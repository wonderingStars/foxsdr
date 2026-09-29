// The SDRplay diagnostic probe (src/source/sdrplay_probe.hpp), held to its
// promises through the same fake SDRplay API the driver's tests use:
//
//   - the eight steps run in order, and every call in them is on the record;
//   - a slow call is waited for and called SLOW, never abandoned;
//   - a call still inside at the limit is called HUNG and the probe ends
//     cleanly, making no further call;
//   - the file carries no raw serial and no user path;
//   - the bias tee is never switched on without the separate confirmation.
//
// No SDRplay hardware exists on the machine these were written on: this is
// the probe's behaviour against the published interface, not a real RSP's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_probe.hpp"
#include "test_check.hpp"

using cascade::source::ProbeStatus;
using cascade::source::SdrPlayProbeOptions;
using cascade::source::SdrPlayProbeResult;
namespace abi = cascade::source::sdrplay_abi;
using fakesdrplay::FakeSdrPlayApi;

namespace {

// Short windows so the whole sequence runs in a few seconds; the sequence
// and the rules are the ones the real run uses.
//
// FOR THE TESTS THAT JUDGE THE DELIVERED RATE the rate window is 150 ms, not
// less: the fake's service thread sleeps on this machine's 15.6 ms timer tick
// and delivers in bursts of that size, so a 40 ms window held two or three of
// them and read 75% or 117% of a rate delivered exactly (measured, the first
// green attempt). 150 ms keeps a tick under 11%, inside the 25% rule. The
// real run streams kProbeStreamPerRate, three seconds.
SdrPlayProbeOptions quickOptions(std::chrono::milliseconds perRate = std::chrono::milliseconds(15)) {
    SdrPlayProbeOptions o;
    o.streamPerRate = perRate;
    o.streamPerStep = std::chrono::milliseconds(15);
    o.foxsdrVersion = "0.99.50-test (0000000)";
    o.osDescription = "Windows 11 (test)";
    return o;
}

// A fake whose service thread delivers at whatever rate the block is set
// for, 1 ms at a time - the most a 10 MS/s plan asks for in 1 ms is 10000.
void startRealisticService(FakeSdrPlayApi& fake) {
    fake.serviceFollowsRate.store(true);
    fake.startService(10000, std::chrono::microseconds(1000), std::chrono::milliseconds(5000));
}

// A step by index that cannot read past the end: in a run that went wrong the
// list may be short, and a CHECK that records-and-continues must not be
// followed by an out-of-bounds read in exactly that run.
cascade::source::SdrPlayProbeStep step(const SdrPlayProbeResult& r, std::size_t i) {
    return i < r.steps.size() ? r.steps[i] : cascade::source::SdrPlayProbeStep{};
}

std::size_t at(const std::string& hay, const std::string& needle) {
    const std::size_t p = hay.find(needle);
    return p == std::string::npos ? static_cast<std::size_t>(-1) : p;
}

}  // namespace

void testTheProbeRunsTheWholeSequenceInOrder() {
    FakeSdrPlayApi fake;
    fake.addDevice("2406000R2X", abi::kRspDxR2);
    startRealisticService(fake);
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake.table, quickOptions(std::chrono::milliseconds(150)));
    fake.stopService();
    const std::string& t = r.report;
    // A SAMPLE FOR A HUMAN, only when asked: FOXSDR_PROBE_SAMPLE=<path>
    // writes this fake run's report there, for documentation.
    if (const char* sample = std::getenv("FOXSDR_PROBE_SAMPLE")) {
        if (FILE* f = std::fopen(sample, "wb")) {
            std::fwrite(t.data(), 1, t.size(), f);
            std::fclose(f);
        }
    }

    CHECK(r.steps.size() == 8);
    CHECK(!r.hung);
    for (const auto& s : r.steps) {
        const bool ok = s.status == ProbeStatus::Pass || s.status == ProbeStatus::Skipped;
        CHECK(ok);
        if (!ok) {
            std::printf("     step %d %s: %s - %s\n", s.number, s.name.c_str(),
                        cascade::source::probeStatusName(s.status), s.detail.c_str());
        }
    }
    // The header and the three sections, in that order.
    CHECK(t.rfind("FoxSDR SDRplay diagnostic\n", 0) == 0);
    CHECK(t.find("probe version: 1") != std::string::npos);
    CHECK(t.find("FoxSDR version: 0.99.50-test") != std::string::npos);
    CHECK(t.find("OS: Windows 11 (test)") != std::string::npos);
    CHECK(at(t, "\nSUMMARY\n") < at(t, "\nEVENTS"));
    CHECK(at(t, "\nEVENTS") < at(t, "\nDETAIL"));
    // The eight steps, in order, in the detail.
    const char* marks[] = {"[1 open and start]", "[2 sample rates]", "[3 frequencies]",
                           "[4 LNA states]",     "[5 antennas and HDR]", "[6 bias tee]",
                           "[7 IF modes]",       "[8 uninit and close]"};
    for (int i = 0; i < 7; ++i) { CHECK(at(t, marks[i]) < at(t, marks[i + 1])); }

    // The vendor calls, in the order the specification's own example makes
    // them (section 4): Open, version, lock, list, select, unlock, parameters,
    // Init ... Uninit, release, close.
    const int iOpen = fake.indexOf("Open");
    const int iVer = fake.indexOf("ApiVersion");
    const int iLock = fake.indexOf("LockDeviceApi");
    const int iList = fake.indexOf("GetDevices");
    const int iSel = fake.indexStarting("SelectDevice(");
    const int iInit = fake.indexOf("Init");
    const int iFirstUpdate = fake.indexStarting("Update(");
    const int iUninit = fake.indexOf("Uninit");
    const int iRel = fake.indexOf("ReleaseDevice");
    const int iClose = fake.indexOf("Close");
    CHECK(iOpen == 0);
    CHECK(iOpen < iVer && iVer < iLock && iLock < iList && iList < iSel && iSel < iInit);
    CHECK(iInit < iFirstUpdate && iFirstUpdate < iUninit && iUninit < iRel && iRel < iClose);

    // Every rate, every LNA state of the RSPdx-R2 (28, spec section 2.9.1),
    // the A/B/C switch and HDR both ways, and the six low-IF modes.
    CHECK(fake.countStarting(FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, 0)) == 28 + 1);
    CHECK(fake.countStarting(FakeSdrPlayApi::updateCall(0, abi::Update_RspDx_AntennaControl)) == 4);
    CHECK(fake.countStarting(FakeSdrPlayApi::updateCall(0, abi::Update_RspDx_HdrEnable)) == 2);
    CHECK(fake.countStarting(FakeSdrPlayApi::updateCall(
              abi::Update_Dev_Fs | abi::Update_Tuner_IfType | abi::Update_Tuner_BwType |
                  abi::Update_Ctrl_Decimation,
              0)) == 6);
    // One measured line per offered rate, counted in the DETAIL section only
    // (a summary line may quote one too).
    int rateLines = 0;
    const std::size_t detail = t.find("\nDETAIL");
    for (std::size_t p = t.find("\n  rate ", detail == std::string::npos ? 0 : detail);
         p != std::string::npos; p = t.find("\n  rate ", p + 1)) {
        ++rateLines;
    }
    CHECK(rateLines == 19);
    // Each measured window says what arrived against what was set.
    CHECK(t.find("rate 2000000 (fsHz 2000000") != std::string::npos);
    CHECK(t.find("% of 2000000)") != std::string::npos);
    // Every call carries its entry and exit wall times and its duration.
    CHECK(t.find("-> sdrplay_api_Open() | ") != std::string::npos);
    CHECK(t.find("<- Success (0) in ") != std::string::npos);
    // The LNA table is read back from the service's gain events.
    CHECK(t.find("LNAstate 27: grChanged") != std::string::npos);
    // It ends with the radio released and the bias tee off.
    CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 0);
    CHECK(fake.devParams.rspDxParams.biasTEnable == 0);
}

void testAFiveSecondUpdateIsSlowAndNotAbandoned() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    startRealisticService(fake);
    // The FIRST Update of the run - the probe's first rate change - takes five
    // seconds, five times the receiver's own kControlWait.
    fake.slowUpdateNumber.store(0);
    fake.slowUpdateMs.store(5000);
    const auto t0 = std::chrono::steady_clock::now();
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake.table, quickOptions(std::chrono::milliseconds(150)));
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    fake.stopService();

    // Waited for, not given up on.
    CHECK(ms >= 5000);
    CHECK(!r.hung);
    CHECK(step(r, 1).status == ProbeStatus::Slow);
    CHECK(step(r, 1).detail.find("took 5") != std::string::npos);
    CHECK(r.report.find("  SLOW") != std::string::npos);
    // (The header names the HUNG rule itself, so the check is for the verdict.)
    CHECK(r.report.find("STILL INSIDE") == std::string::npos);
    CHECK(r.report.find("ended early on a HUNG call") == std::string::npos);
    // ...and the run carried on to the end.
    CHECK(step(r, 7).status == ProbeStatus::Pass);
    CHECK(fake.called("Close"));
    CHECK(fake.countStarting(FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, 0)) == 10 + 1);
}

void testAHungCallEndsTheProbeCleanly() {
    // On the heap and never destroyed: a worker is left inside the fake.
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
    fake->addDevice("1811003EFB", abi::kRsp1A);
    startRealisticService(*fake);
    fake->hangInUpdate.store(true);
    SdrPlayProbeOptions o = quickOptions();
    // A stand-in for kProbeCallLimit's thirty seconds, so the test does not
    // take them; the rule is the same, and the real limit is pinned below.
    static_assert(cascade::source::kProbeCallLimit == std::chrono::milliseconds(30000),
                  "the probe's limit is thirty seconds");
    o.callLimit = std::chrono::milliseconds(1500);
    const auto t0 = std::chrono::steady_clock::now();
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake->table, o);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    CHECK(r.hung);
    CHECK(ms >= 1400);
    CHECK(ms < 4000);
    CHECK(step(r, 0).status == ProbeStatus::Pass);
    CHECK(step(r, 1).status == ProbeStatus::Hung);
    for (int i = 2; i < 8; ++i) { CHECK(step(r, static_cast<std::size_t>(i)).status == ProbeStatus::NotRun); }
    CHECK(r.report.find("STILL INSIDE after") != std::string::npos);
    CHECK(r.report.find("ended early on a HUNG call") != std::string::npos);
    // NOTHING AFTER THE HUNG CALL: not the next Update, not Uninit, not
    // ReleaseDevice, not Close - each would queue behind the thread still in.
    CHECK(fake->countStarting("Update(") == 1);
    CHECK(!fake->calls.empty() && fake->calls.back().rfind("Update(", 0) == 0);
    CHECK(!fake->called("Uninit"));
    CHECK(!fake->called("ReleaseDevice"));
    CHECK(!fake->called("Close"));

    fake->releaseUpdateHang.store(true);
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());
    fake->stopService();
}

void testTheReportCarriesNoSerialAndNoUserPath() {
    FakeSdrPlayApi fake;
    fake.addDevice("2406000R2X", abi::kRspDxR2);
    // A DLL somebody put under their own profile, the path a load failure
    // or a user-installed copy would report.
    fake.table.loadDetail = "C:\\Users\\alice.smith\\AppData\\Local\\SDRplay\\x64\\sdrplay_api.dll";
    startRealisticService(fake);
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake.table, quickOptions());
    fake.stopService();
    CHECK(r.report.find("2406000R2X") == std::string::npos);
    CHECK(r.report.find("alice") == std::string::npos);
    CHECK(r.report.find(cascade::source::sdrPlayProbeSerialHash("2406000R2X")) != std::string::npos);
    // The hash is stable - two reports from one radio can be matched.
    CHECK(cascade::source::sdrPlayProbeSerialHash("2406000R2X") ==
          cascade::source::sdrPlayProbeSerialHash("2406000R2X"));
    CHECK(cascade::source::sdrPlayProbeSerialHash("2406000R2X") !=
          cascade::source::sdrPlayProbeSerialHash("2406000R2Y"));
    CHECK(cascade::source::sdrPlayProbeSerialHash("").size() == 16);
}

void testTheBiasTeeIsNeverOnWithoutTheConfirmation() {
    const unsigned char models[] = {abi::kRsp1A, abi::kRsp2, abi::kRspDuo, abi::kRspDx,
                                    abi::kRspDxR2};
    for (unsigned char hw : models) {
        {   // NOT CONFIRMED: the bias tee is written OFF, and never ON.
            FakeSdrPlayApi fake;
            if (hw == abi::kRspDuo) {
                fake.addRspDuo("1809001ABC");
            } else {
                fake.addDevice("1811003EFB", hw);
            }
            startRealisticService(fake);
            const SdrPlayProbeResult r =
                cascade::source::runSdrPlayProbe(fake.table, quickOptions());
            fake.stopService();
            CHECK(!fake.biasTeeEverOn.load());
            CHECK(r.report.find("not tested - it was not separately confirmed") != std::string::npos);
            CHECK(step(r, 5).status == ProbeStatus::Pass);
        }
        {   // CONFIRMED: on once, and off again by the end.
            FakeSdrPlayApi fake;
            if (hw == abi::kRspDuo) {
                fake.addRspDuo("1809001ABC");
            } else {
                fake.addDevice("1811003EFB", hw);
            }
            startRealisticService(fake);
            SdrPlayProbeOptions o = quickOptions();
            o.biasTeeOn = true;
            const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake.table, o);
            fake.stopService();
            CHECK(fake.biasTeeEverOn.load());
            CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 0);
            CHECK(fake.chA.rsp2TunerParams.biasTEnable == 0);
            CHECK(fake.chA.rspDuoTunerParams.biasTEnable == 0);
            CHECK(fake.devParams.rspDxParams.biasTEnable == 0);
            CHECK(r.report.find("bias tee ON (confirmed)") != std::string::npos);
        }
    }
}

void testNoApiIsAReportThatSaysSo() {
    abi::Api empty;
    empty.resolved = false;
    empty.loadDetail = "sdrplay_api.dll not found (C:\\Users\\bob\\x)";
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(empty, quickOptions());
    CHECK(step(r, 0).status == ProbeStatus::Error);
    CHECK(step(r, 0).detail == "SDRplay API not installed");
    for (int i = 1; i < 8; ++i) { CHECK(step(r, static_cast<std::size_t>(i)).status == ProbeStatus::NotRun); }
    CHECK(r.callsMade == 0);
    CHECK(r.report.find("bob") == std::string::npos);
}

// The GUI's child command line: the bias switch appears ONLY when the
// separate confirmation was given, and both paths survive a space.
void testTheChildCommandLineCarriesTheBiasSwitchOnlyWhenConfirmed() {
    const std::string off = cascade::source::sdrPlayProbeCommandLine(
        "C:\\Program Files\\FoxSDR\\cascade.exe", "C:\\Users\\A B\\x\\probe.txt", false);
    CHECK(off == "\"C:\\Program Files\\FoxSDR\\cascade.exe\" --sdrplay-probe "
                 "\"C:\\Users\\A B\\x\\probe.txt\"");
    CHECK(off.find("bias") == std::string::npos);
    const std::string on = cascade::source::sdrPlayProbeCommandLine("/opt/foxsdr/cascade",
                                                                    "/tmp/p.txt", true);
    CHECK(on == "\"/opt/foxsdr/cascade\" --sdrplay-probe \"/tmp/p.txt\" --sdrplay-probe-bias-tee");
}

// A refusal part-way through step 1 (the radio is in use elsewhere) stops the
// run, and step 8 still closes what was opened - timed, like every call.
void testARefusedSelectStillClosesWhatWasOpened() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    fake.selectResult = abi::Fail;
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(fake.table, quickOptions());
    CHECK(step(r, 0).status == ProbeStatus::Error);
    CHECK(step(r, 0).detail.find("SelectDevice") != std::string::npos);
    for (int i = 1; i < 7; ++i) { CHECK(step(r, static_cast<std::size_t>(i)).status == ProbeStatus::NotRun); }
    CHECK(step(r, 7).status == ProbeStatus::Pass);
    CHECK(fake.called("UnlockDeviceApi"));
    CHECK(fake.called("Close"));
    CHECK(!fake.called("ReleaseDevice"));
    CHECK(!fake.called("Init"));
    CHECK(r.report.find("[8 uninit and close]\n  not run") == std::string::npos);
}

// THE LAUNCHER, ACROSS A REAL PROCESS BOUNDARY. SdrPlayProbeChild starts its
// OWN executable again - in the app that is cascade.exe, here it is this test
// binary, whose main() answers --sdrplay-probe below the same way (a file,
// exit 1) but against an EMPTY table, so no SDRplay install on the machine
// running the suite is ever touched. What is proved is the part only a real
// child can prove: the process starts, the quoted path with a space in it
// arrives intact, running() goes false without anyone waiting on it, the
// exit code comes back, and the bias switch reaches the child only when
// asked for.
int childMode(int argc, char** argv) {
    const bool bias = argc >= 4 && std::strcmp(argv[3], "--sdrplay-probe-bias-tee") == 0;
    abi::Api empty;
    empty.resolved = false;
    empty.loadDetail = "none (test child)";
    const SdrPlayProbeResult r = cascade::source::runSdrPlayProbe(empty, SdrPlayProbeOptions{});
    if (FILE* f = std::fopen(argv[2], "wb")) {
        const std::string head = std::string("child ok bias=") + (bias ? "on" : "off") + "\n";
        std::fwrite(head.data(), 1, head.size(), f);
        std::fwrite(r.report.data(), 1, r.report.size(), f);
        std::fclose(f);
    }
    return 1;
}

void testTheLauncherStartsAChildAndHearsItFinish() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
                         ("foxsdr probe launcher " + std::to_string(
                                                         static_cast<unsigned long long>(
                                                             std::chrono::steady_clock::now()
                                                                 .time_since_epoch()
                                                                 .count())));
    fs::create_directories(dir, ec);
    for (int pass = 0; pass < 2; ++pass) {
        const bool bias = pass == 1;
        const fs::path out = dir / (bias ? "with bias.txt" : "no bias.txt");
        cascade::source::SdrPlayProbeChild child;
        std::string error;
        const bool started = child.start(out.string(), bias, error);
        CHECK(started);
        if (!started) {
            std::printf("     could not start the child: %s\n", error.c_str());
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        while (child.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(!child.running());
        CHECK(child.exitCode() == 1);
        std::string text;
        if (FILE* f = std::fopen(out.string().c_str(), "rb")) {
            char buf[4096];
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
            std::fclose(f);
        }
        CHECK(text.rfind(bias ? "child ok bias=on\n" : "child ok bias=off\n", 0) == 0);
        CHECK(text.find("SDRplay API not installed") != std::string::npos);
        fs::remove(out, ec);
    }
    fs::remove(dir, ec);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--sdrplay-probe") == 0) { return childMode(argc, argv); }
    testTheLauncherStartsAChildAndHearsItFinish();
    testTheChildCommandLineCarriesTheBiasSwitchOnlyWhenConfirmed();
    testARefusedSelectStillClosesWhatWasOpened();
    testTheProbeRunsTheWholeSequenceInOrder();
    testTheReportCarriesNoSerialAndNoUserPath();
    testTheBiasTeeIsNeverOnWithoutTheConfirmation();
    testNoApiIsAReportThatSaysSo();
    testAFiveSecondUpdateIsSlowAndNotAbandoned();
    // Last: it leaves a worker inside its fake until it releases it.
    testAHungCallEndsTheProbeCleanly();
    return testSummary("test_sdrplay_probe");
}
