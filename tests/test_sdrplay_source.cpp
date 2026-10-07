// What the SDRplay driver can be held to without an RSP, without the SDRplay
// API installed and without the SDRplay service running - which is every
// condition this driver was written under.
//
// The seam is the function table in src/source/sdrplay_api_decl.hpp: the
// driver makes every call through it, and tests/sdrplay_fake_api.hpp fills it
// with a fake that answers the way the vendor's header says the service does.
// So what is proved here is the DRIVER'S BEHAVIOUR - the call sequence, the
// exact bytes written into the parameter block, the update reasons, the sample
// conversion, the fault handling and the teardown bound - against the
// published interface and against SoapySDRPlay3's known-working call order.
//
// WHAT IS NOT PROVED HERE, and it has to be said plainly: that a real RSP
// behaves as its header describes. No test in this file has met the service.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
#include "core/leak_on_purpose.hpp"
#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

using cascade::source::SdrPlaySource;
using cascade::source::SdrPlayRatePlan;
namespace abi = cascade::source::sdrplay_abi;
using fakesdrplay::FakeSdrPlayApi;

namespace {

// Open a source against a fake carrying one device of the given model.
// Returns false without asserting, so a test can prove a REFUSAL too.
bool openOn(SdrPlaySource& src, FakeSdrPlayApi& fake, const std::string& args = std::string()) {
    src.setApiForTest(&fake.table);
    return src.open(args);
}

// --- 0. the Linux loader's SONAME, pinned ----------------------------------

#if !defined(_WIN32)
// LINUX PORT (2026-09-15). processSdrPlayApi()'s loadInto() now dlopen()s a
// real SONAME on this platform instead of leaving api.resolved permanently
// false with a "Windows-only" message - src/source/sdrplay_source.cpp mirrors
// the Windows LoadLibrary branch almost line for line: try the name the
// vendor installer registers first, then a bare fallback for a dev machine
// with only the unversioned symlink. What a unit test CAN pin without the
// real API installed (this machine has neither) is the exact string asked
// for, because that string is what makes the difference between "found" and
// "not found" on a real Linux box with SDRplay's .run installer applied -
// get the SONAME wrong and every RSP silently goes back to being invisible on
// this platform, exactly as it was before this port.
void testLinuxSoNameIsTheVendorInstalledOne() {
    // The API's own major version (3), NOT an FoxSDR or distro version - see
    // sdrPlayApiSoName()'s comment for how this was confirmed (SDRplay's
    // Linux .run installer registers this exact SONAME with ldconfig, and
    // SoapySDRPlay3 - the reference open-source consumer - links against the
    // same major version).
    const std::string so = cascade::source::sdrPlayApiSoName();
    CHECK(so == "libsdrplay_api.so.3");

    // THE SHAPE THE STRING MUST HAVE, independent of the exact version
    // pinned above - so a future SDRplay API v4 update to this constant still
    // has to look like a SONAME and not, say, a bare "sdrplay_api" or a path.
    // CHECK() records and continues rather than stopping the test, so the
    // digits-only check below is gated on the prefix actually being there -
    // otherwise a broken prefix does not merely fail one CHECK, it throws out
    // of substr() and the run never reaches testSummary() at all.
    const std::string prefix = "libsdrplay_api.so.";
    CHECK(so.rfind(prefix, 0) == 0);
    if (so.rfind(prefix, 0) == 0) {
        const std::string majorDigits = so.substr(prefix.size());
        CHECK(!majorDigits.empty());
        for (char c : majorDigits) { CHECK(c >= '0' && c <= '9'); }
    }
}
#endif

// --- 1. no API installed --------------------------------------------------

void testMissingApiIsEmptyAndSaysWhy() {
    // A table that resolved nothing is exactly what processSdrPlayApi()
    // returns on a machine with no SDRplay install - which is this one.
    abi::Api absent;
    CHECK(absent.resolved == false);
    const std::vector<cascade::source::NativeDeviceInfo> rows =
        cascade::source::enumerateSdrPlayWith(absent);
    CHECK(rows.empty());

    // The sentence the Source section shows instead of a radio. Pinned
    // verbatim because it is the only instruction the user gets, and a
    // rewording that drops "3.x" or "sdrplay.com" sends them nowhere.
    const std::string advice = cascade::source::sdrPlayApiAdvice(false, 0.0f);
    CHECK(advice ==
          "SDRplay radios need the SDRplay API from sdrplay.com, version 3.x - install it and "
          "restart FoxSDR.");

    // And an API that IS present and new enough has nothing to say.
    CHECK(cascade::source::sdrPlayApiAdvice(true, 3.15f).empty());
    CHECK(cascade::source::sdrPlayApiAdvice(true, 3.07f).empty());

    // Opening against an absent table fails rather than crashing, and says the
    // same thing.
    SdrPlaySource src;
    src.setApiForTest(&absent);
    CHECK(src.open("") == false);
    CHECK(std::string(src.lastError()) == advice);
}

// --- 2. an API that is too old --------------------------------------------

void testOldApiIsRefusedWithASentence() {
    FakeSdrPlayApi fake;
    fake.version = 3.05f;  // older than every header whose layout we checked
    fake.addDevice("1234567890", abi::kRsp1A);

    SdrPlaySource src;
    CHECK(openOn(src, fake) == false);

    const std::string err = src.lastError();
    CHECK(err.find("3.05") != std::string::npos);
    CHECK(err.find("sdrplay.com") != std::string::npos);
    CHECK(err.find("3.07") != std::string::npos);

    // The API was opened to ask, and CLOSED again when the answer was no - a
    // refused version must not leave a connection to the service behind.
    CHECK(fake.openCount == 1);
    CHECK(fake.closeCount == 1);
    // And nothing was selected.
    CHECK(fake.selectCount == 0);

    // Enumeration is empty for the same reason, and says so rather than
    // listing radios it cannot drive.
    CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());

    // The boundary: 3.07 is accepted, 3.06 is not.
    CHECK(abi::versionAtLeast(3.07f, abi::kMinApiVersion));
    CHECK(!abi::versionAtLeast(3.06f, abi::kMinApiVersion));
}

// --- 3. enumeration -------------------------------------------------------

void testEnumerationLabelsAndArgs() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    fake.addDevice("2002000ABC", abi::kRspDx);

    const std::vector<cascade::source::NativeDeviceInfo> rows =
        cascade::source::enumerateSdrPlayWith(fake.table);
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        CHECK(rows[0].driver == "sdrplay");
        CHECK(rows[0].label == "SDRplay RSP1A (serial 1811003EFB)");
        CHECK(rows[0].args == "serial=1811003EFB");
        CHECK(rows[1].label == "SDRplay RSPdx (serial 2002000ABC)");
        CHECK(rows[1].args == "serial=2002000ABC");
    }

    // The list is read under the API's own device lock and the lock is
    // released again - an enumeration that kept it would block every other
    // application's open.
    CHECK(fake.indexOf("LockDeviceApi") >= 0);
    CHECK(fake.indexOf("GetDevices") > fake.indexOf("LockDeviceApi"));
    CHECK(fake.indexOf("UnlockDeviceApi") > fake.indexOf("GetDevices"));

    // Enumeration does NOT select anything: listing radios must not take one
    // away from whatever is using it.
    CHECK(fake.selectCount == 0);
    CHECK(!fake.called("Init"));

    // The model names, including one we have never heard of.
    CHECK(cascade::source::sdrPlayModelName(abi::kRsp1B) == "RSP1B");
    CHECK(cascade::source::sdrPlayModelName(abi::kRspDuo) == "RSPduo");
    CHECK(cascade::source::sdrPlayModelName(abi::kRspDxR2) == "RSPdx-R2");
    CHECK(cascade::source::sdrPlayModelName(99) == "RSP (hw 99)");
}

// --- 4. open, select and init: the order and the parameters ---------------

void testOpenSelectInitOrderAndParameters() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);

    SdrPlaySource src;
    CHECK(openOn(src, fake, "serial=1811003EFB"));
    CHECK(src.isOpen());

    // THE EXACT SEQUENCE, in the order SoapySDRPlay3 performs it: connect to
    // the service, check its version, take the device lock, read the list,
    // select, release the lock, silence the API's own tracing, get the
    // parameter block.
    const std::vector<std::string> want = {"Open",
                                           "ApiVersion",
                                           "LockDeviceApi",
                                           "GetDevices",
                                           "SelectDevice(tuner=1,mode=0)",
                                           "UnlockDeviceApi",
                                           "DebugEnable",
                                           "GetDeviceParams"};
    CHECK(fake.calls == want);

    // Nothing streams until start(): an open radio is not a running one.
    CHECK(!fake.initialised);
    CHECK(!src.running());

    CHECK(src.start());
    CHECK(src.running());
    CHECK(fake.initialised);

    // THE STATE THE RADIO IS ACTUALLY STARTED IN, snapshotted by the fake at
    // the moment of Init rather than read afterwards.
    CHECK(fake.atInit.taken);
    // 2 MS/s is the API's own default ADC rate at zero IF, nothing decimated
    // (0.99.44). Until then it was the reference's 6 MHz front end at the
    // 1.62 MHz IF, which two RSP2 field logs show delivering 6 MS/s - see
    // testEveryRatePlanDeliversTheRateItClaims.
    CHECK_NEAR(fake.atInit.fsHz, 2000000.0, 1.0);
    CHECK(fake.atInit.ifType == abi::IF_Zero);
    CHECK(fake.atInit.decEnable == 0);
    CHECK(fake.atInit.decFactor == 1);
    CHECK_NEAR(fake.atInit.rfHz, 100000000.0, 1.0);
    CHECK(fake.atInit.bwType == abi::BW_1_536);
    CHECK(fake.atInit.gRdB == 40);
    CHECK(fake.atInit.LNAstate == 0);
    CHECK(fake.atInit.agcEnable == abi::AGC_DISABLE);
    CHECK(fake.atInit.agcSetPoint == -60);
    // A zero-IF front end with the corrections off puts a carrier in the
    // middle of the display and an image beside every signal.
    CHECK(fake.atInit.dcEnable == 1);
    CHECK(fake.atInit.iqEnable == 1);

    CHECK_NEAR(src.sampleRateHz(), 2000000.0, 1.0);
    CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 1.0);
    CHECK(std::string(src.driverKey()) == "sdrplay");
    CHECK(std::string(src.name()) == "SDRplay RSP1A");
    CHECK(src.serialNo() == "1811003EFB");
    CHECK(src.selfPaced());
    CHECK(src.autoGainSupported());

    double lo = 0.0;
    double hi = 0.0;
    CHECK(src.frequencyRangeHz(lo, hi));
    CHECK_NEAR(lo, 1000.0, 0.5);
    CHECK_NEAR(hi, 2000000000.0, 0.5);
}

void testOpenBySerialSuffixAndIndex() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    fake.addDevice("2002000ABC", abi::kRspDx);

    {
        // The short form a user reads off another tool's listing still finds
        // the radio.
        SdrPlaySource src;
        CHECK(openOn(src, fake, "serial=00ABC"));
        CHECK(src.serialNo() == "2002000ABC");
    }
    {
        SdrPlaySource src;
        CHECK(openOn(src, fake, "index=1"));
        CHECK(src.serialNo() == "2002000ABC");
    }
    {
        SdrPlaySource src;
        CHECK(openOn(src, fake, ""));  // the first
        CHECK(src.serialNo() == "1811003EFB");
    }
    {
        SdrPlaySource src;
        CHECK(openOn(src, fake, "serial=nosuchradio") == false);
        CHECK(std::string(src.lastError()).find("no SDRplay device matches") != std::string::npos);
    }
}

// --- 5. tuning ------------------------------------------------------------

void testTune() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.calls.clear();

    CHECK(src.setCenterFrequencyHz(101100000.0));
    CHECK_NEAR(src.centerFrequencyHz(), 101100000.0, 1.0);
    CHECK_NEAR(fake.chA.tunerParams.rfFreq.rfHz, 101100000.0, 1.0);
    // Tuner_Frf and nothing else: an Update carrying reasons that did not
    // change makes the service redo work the radio did not need.
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(abi::Update_Tuner_Frf, 0)}));

    // Refused, not clamped, outside the range - a receiver that silently
    // listens somewhere else is worse than one that says no.
    fake.calls.clear();
    CHECK(src.setCenterFrequencyHz(2500000000.0) == false);
    CHECK(std::string(src.lastError()).find("outside") != std::string::npos);
    CHECK_NEAR(src.centerFrequencyHz(), 101100000.0, 1.0);
    CHECK(fake.calls.empty());

    CHECK(src.setCenterFrequencyHz(500.0) == false);
    CHECK_NEAR(src.centerFrequencyHz(), 101100000.0, 1.0);
}

// --- 5b. no live Update the radio does not need (0.99.36) --------------------
//
// THREE FIELD LOGS, ONE SHAPE. 0.95.0 (RSP1A, API 3.15), 0.96.2 (RSP1A, API
// 3.09) and 0.99.27 (RSP1, API 3.15) each read "opened SDRplay ... (sdrplay)"
// followed by the retune failing - five seconds later with
// ServiceNotResponding, or abandoned at kControlWait - and that retune is the
// carry-across tune AppWindow::finishDeviceOpen issues milliseconds after the
// stream was started. It was sent even when there was nothing to change. Why
// the service does not answer an Update that early is NOT known (no RSP here);
// what is proved below is that FoxSDR no longer sends one: a tune written
// before start() reaches the radio through Init, and a tune to where the radio
// already is makes no vendor call at all.

void testAnUnchangedFrequencySendsNoUpdate() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.calls.clear();

    // open() leaves the radio at 100 MHz (applyKnownStateLocked).
    CHECK(src.setCenterFrequencyHz(100000000.0));
    CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 1.0);
    CHECK(fake.countStarting("Update(") == 0);

    // A real change still goes to the radio, once.
    CHECK(src.setCenterFrequencyHz(101100000.0));
    CHECK(fake.countStarting("Update(") == 1);
    // ...and the same frequency again is nothing.
    CHECK(src.setCenterFrequencyHz(101100000.0));
    CHECK(fake.countStarting("Update(") == 1);
    src.stop();
    src.closeDevice();
}

void testATuneBeforeStartLeavesNothingToSendAfterInit() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    fake.calls.clear();

    // What the open worker now does with the frequency being carried across:
    // written while nothing streams, so it is only the parameter block.
    CHECK(src.setCenterFrequencyHz(97300000.0));
    CHECK(fake.countStarting("Update(") == 0);

    // The stream starts ON that frequency...
    CHECK(src.start());
    CHECK(fake.atInit.taken);
    CHECK_NEAR(fake.atInit.rfHz, 97300000.0, 1.0);

    // ...and the carry-across tune the GUI still makes once the radio is
    // installed (applyRetuneNow, for its range check and readback) finds it
    // already there: no Update immediately after Init.
    CHECK(src.setCenterFrequencyHz(97300000.0));
    CHECK(fake.countStarting("Update(") == 0);
    src.stop();
    src.closeDevice();
}

// --- 5b'. a REFUSED change leaves the parameter block where the radio is ------
//
// THE INDEPENDENT REVIEW OF 0c59853 (blocking). Every setter writes the field
// into the service's parameter block and THEN sends the Update - and on an
// ordinary refusal (abi::Fail: the radio is alive and simply said no) the
// field was never put back. 0c59853's "nothing to send when nothing changes"
// then read that field as the radio's state: the reviewer's probe - refuse a
// retune, ask for the same frequency again - got true, sent nothing, and read
// back a frequency the radio had never gone to. The block is also what Init
// programs on the next start, so a refused bias-tee ON would have powered the
// antenna socket at the next START while the panel said off.
void testARefusedChangeLeavesTheBlockWhereTheRadioIs() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    // THE REVIEWER'S PROBE.
    fake.updateResult = abi::Fail;
    CHECK(src.setCenterFrequencyHz(101100000.0) == false);
    CHECK(!src.deviceDead());  // an ordinary refusal, not a dead service
    CHECK_NEAR(fake.chA.tunerParams.rfFreq.rfHz, 100000000.0, 1.0);
    CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 1.0);
    fake.updateResult = abi::Success;
    fake.calls.clear();
    const bool retried = src.setCenterFrequencyHz(101100000.0);
    const int updates = fake.countStarting("Update(");
    std::printf("retry after a refused retune: returned %s, %d Update(s) sent, readback %.0f Hz\n",
                retried ? "true" : "false", updates, src.centerFrequencyHz());
    CHECK(retried);
    CHECK(updates == 1);
    CHECK_NEAR(src.centerFrequencyHz(), 101100000.0, 1.0);

    // THE GAINS: a refused IF or LNA change must be re-sent when asked again.
    fake.updateResult = abi::Fail;
    const int grBefore = fake.chA.tunerParams.gain.gRdB;
    const int lnaBefore = fake.chA.tunerParams.gain.LNAstate;
    CHECK(src.setGainDb("IF", -30.0) == false);
    CHECK(fake.chA.tunerParams.gain.gRdB == grBefore);
    CHECK(src.setGainDb("LNA", 5.0) == false);
    CHECK(fake.chA.tunerParams.gain.LNAstate == lnaBefore);
    fake.updateResult = abi::Success;
    fake.calls.clear();
    CHECK(src.setGainDb("IF", -30.0));
    CHECK(src.setGainDb("LNA", 5.0));
    CHECK(fake.countStarting("Update(") == 2);
    CHECK(fake.chA.tunerParams.gain.gRdB == 30);
    CHECK(fake.chA.tunerParams.gain.LNAstate == 5);

    // THE SAMPLE RATE: every field of the plan comes back.
    const double fsBefore = fake.devParams.fsFreq.fsHz;
    const int ifBefore = static_cast<int>(fake.chA.tunerParams.ifType);
    const int bwBefore = static_cast<int>(fake.chA.tunerParams.bwType);
    const int decBefore = fake.chA.ctrlParams.decimation.decimationFactor;
    const int decEnBefore = fake.chA.ctrlParams.decimation.enable;
    fake.updateResult = abi::Fail;
    CHECK(src.setSampleRateHz(8000000.0) == false);
    CHECK(fake.devParams.fsFreq.fsHz == fsBefore);
    CHECK(static_cast<int>(fake.chA.tunerParams.ifType) == ifBefore);
    CHECK(static_cast<int>(fake.chA.tunerParams.bwType) == bwBefore);
    CHECK(fake.chA.ctrlParams.decimation.decimationFactor == decBefore);
    CHECK(fake.chA.ctrlParams.decimation.enable == decEnBefore);
    CHECK_NEAR(src.sampleRateHz(), 2000000.0, 1.0);

    // THE BIAS TEE: a refused ON must not be left for the next Init to apply.
    CHECK(src.setBiasT(true) == false);
    CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 0);
    CHECK(src.biasT() == false);
    CHECK(src.setRfNotch(true) == false);
    CHECK(fake.devParams.rsp1aParams.rfNotchEnable == 0);

    // THE AGC SET POINT: the mirror as well as the block.
    fake.updateResult = abi::Success;
    CHECK(src.setAutoGain(true));
    fake.updateResult = abi::Fail;
    CHECK(src.setAgcSetPointDbfs(-30) == false);
    CHECK(fake.chA.ctrlParams.agc.setPoint_dBfs == -60);
    CHECK(src.agcSetPointDbfs() == -60);
    fake.updateResult = abi::Success;

    // AND WHAT THE NEXT START PROGRAMMES is the radio's real state.
    src.stop();
    CHECK(src.start());
    CHECK(fake.atInit.taken);
    CHECK_NEAR(fake.atInit.rfHz, 101100000.0, 1.0);
    CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 0);
    src.stop();
    src.closeDevice();
}

void testARefusedAntennaChangeLeavesTheBlockWhereTheRadioIs() {
    FakeSdrPlayApi fake;
    fake.addDevice("2208054321", abi::kRspDx);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.updateResult = abi::Fail;
    CHECK(src.setAntenna("Antenna C") == false);
    CHECK(fake.devParams.rspDxParams.antennaSel == abi::RspDx_ANTENNA_A);
    CHECK(src.antenna() == "Antenna A");
    CHECK(src.setBiasT(true) == false);
    CHECK(fake.devParams.rspDxParams.biasTEnable == 0);
    CHECK(src.setHdrMode(true) == false);
    CHECK(fake.devParams.rspDxParams.hdrEnable == 0);
    CHECK(src.hdrMode() == false);
    fake.updateResult = abi::Success;
    src.stop();
    src.closeDevice();
}

// --- 5c. every sentence after a lost session names the restart that works ---
//
// 0.99.28 made a lost session process-wide (Api::sessionLost): no scan and no
// open() reaches the API again until FoxSDR restarts. Two sentences written
// before that still told the user "restart the SDRplay API service, then open
// the radio again" - and opening it again is exactly what cannot work: the
// receiver closes the dead radio first, the open is refused, and the user is
// left on the signal generator having done what the screen said.
void testEverySentenceAfterALostSessionNamesTheRestartThatWorks() {
    const std::string hung = cascade::source::sdrPlayControlHungSentence();
    CHECK(hung.find("restart FoxSDR") != std::string::npos);
    CHECK(hung.find("open the radio again") == std::string::npos);

    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.updateResult = abi::ServiceNotResponding;
    CHECK(src.setCenterFrequencyHz(101100000.0) == false);
    CHECK(src.deviceDead());
    const std::string shown = src.lastError();
    std::printf("after ServiceNotResponding the receiver shows \"%s\"\n", shown.c_str());
    CHECK(shown.find("restart FoxSDR") != std::string::npos);
    CHECK(shown.find("open the radio again") == std::string::npos);

    // ...and the proof the old instruction was wrong: opening it again is
    // refused while this process lives.
    src.stop();
    src.closeDevice();
    SdrPlaySource again;
    CHECK(openOn(again, fake) == false);
}

// --- 5d. a radio the vendor DLL is lost to takes no setting at all ---------
//
// THE REVIEW OF 6e308c3. Once a control has been abandoned inside the vendor's
// Update, the stream has been declared stalled or the service has answered
// ServiceNotResponding, every setter still WROTE the parameter block, and each
// "nothing changed" shortcut ran before anything looked at that state. The
// reviewer's probe after an abandoned 101.1 MHz retune: asking 101.1 MHz again
// answered true with nothing sent (the abandoned write was still in the block),
// and asking 102 MHz answered false with the block now reading 102 MHz - under
// a worker of ours that may still be inside Update reading it. The comment
// over undoRefused claimed the block was left alone; it was not.
//
// The rule pinned here: in any of those three states EVERY setter refuses
// before it touches anything - the same-value request included - and the
// block, the readbacks and the call log are exactly what they were.

struct BlockBytes {
    abi::DevParamsT dev;
    abi::RxChannelParamsT a;
    abi::RxChannelParamsT b;
};

BlockBytes blockOf(const FakeSdrPlayApi& f) {
    BlockBytes s;
    std::memcpy(&s.dev, &f.devParams, sizeof(s.dev));
    std::memcpy(&s.a, &f.chA, sizeof(s.a));
    std::memcpy(&s.b, &f.chB, sizeof(s.b));
    return s;
}

bool sameBlock(const BlockBytes& x, const BlockBytes& y) {
    return std::memcmp(&x.dev, &y.dev, sizeof(x.dev)) == 0 &&
           std::memcmp(&x.a, &y.a, sizeof(x.a)) == 0 &&
           std::memcmp(&x.b, &y.b, sizeof(x.b)) == 0;
}

// Every setter an RSP1A has, each with a value that changes something and,
// where the setter has a "nothing changed" shortcut, with the value it is
// already at. Returns how many answered true; the answer must be none.
int everySetterAccepted(SdrPlaySource& src, FakeSdrPlayApi& fake, double blockHz,
                        const char* when) {
    const BlockBytes before = blockOf(fake);
    const std::size_t callsBefore = fake.callCount();
    const double hz = src.centerFrequencyHz();
    const double rate = src.sampleRateHz();
    const double ifDb = src.gainDb("IF");
    const double lna = src.gainDb("LNA");
    const bool agc = src.autoGain();
    const int setPoint = src.agcSetPointDbfs();
    const bool bias = src.biasT();
    const bool notch = src.rfNotch();
    const bool dab = src.dabNotch();
    const std::string ant = src.antenna();

    struct Try {
        const char* what;
        bool ok;
    };
    // A braced list is evaluated left to right, so this is also the order the
    // calls are made in.
    const std::vector<Try> tries = {
        {"retune to the frequency the block holds", src.setCenterFrequencyHz(blockHz)},
        {"retune to 102 MHz", src.setCenterFrequencyHz(102000000.0)},
        {"the rate it is at", src.setSampleRateHz(2000000.0)},
        {"8 MS/s", src.setSampleRateHz(8000000.0)},
        {"the IF gain it is at", src.setGainDb("IF", -40.0)},
        {"IF gain -30 dB", src.setGainDb("IF", -30.0)},
        {"the LNA state it is at", src.setGainDb("LNA", 0.0)},
        {"LNA state 5", src.setGainDb("LNA", 5.0)},
        {"AGC off (it is off)", src.setAutoGain(false)},
        {"AGC on", src.setAutoGain(true)},
        {"AGC set point -30", src.setAgcSetPointDbfs(-30)},
        {"the antenna it is on", src.setAntenna("RX")},
        {"bias tee on", src.setBiasT(true)},
        {"FM notch on", src.setRfNotch(true)},
        {"DAB notch on", src.setDabNotch(true)},
    };
    int accepted = 0;
    for (const Try& t : tries) {
        if (t.ok) {
            ++accepted;
            std::printf("     %s: \"%s\" was ACCEPTED\n", when, t.what);
        }
    }
    const bool blockKept = sameBlock(before, blockOf(fake));
    const std::size_t sent = fake.callCount() - callsBefore;
    std::printf("%s: %d of %zu setters accepted, %zu vendor call(s), block %s, retune reads "
                "%.0f Hz\n",
                when, accepted, tries.size(), sent, blockKept ? "unchanged" : "WRITTEN",
                fake.chA.tunerParams.rfFreq.rfHz);
    CHECK(blockKept);
    CHECK(sent == 0);
    // The readbacks are the radio's, not the requests'.
    CHECK(src.centerFrequencyHz() == hz);
    CHECK(src.sampleRateHz() == rate);
    CHECK(src.gainDb("IF") == ifDb);
    CHECK(src.gainDb("LNA") == lna);
    CHECK(src.autoGain() == agc);
    CHECK(src.agcSetPointDbfs() == setPoint);
    CHECK(src.biasT() == bias);
    CHECK(src.rfNotch() == notch);
    CHECK(src.dabNotch() == dab);
    CHECK(src.antenna() == ant);
    // ...and what the user is shown still says what to do about it.
    CHECK(std::string(src.lastError()).find("restart FoxSDR") != std::string::npos);
    CHECK(src.deviceDead());
    return accepted;
}

void testALostRadioTakesNoSettingAtAll() {
    {   // 1. AN ABANDONED CONTROL - the reviewer's probe. On the heap and never
        // destroyed, like every test that abandons a worker inside its fake.
        FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
        fake->addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
        CHECK(openOn(*src, *fake));
        CHECK(src->start());
        // THIS TEST IS ABOUT THE RADIO ONCE IT HAS BEEN GIVEN UP FOR GOOD, so the
        // grace for a late answer (kControlGrace) is taken away and the give-up
        // comes at kControlWait, as it did before the grace existed.
        src->setControlGraceForTest(std::chrono::milliseconds(0));
        fake->hangInUpdate.store(true);
        CHECK(src->setCenterFrequencyHz(101100000.0) == false);  // kControlWait, then abandoned
        CHECK(fake->insideUpdate.load());
        CHECK(src->deviceDead());
        // The abandoned retune's OWN write stays where it is: the worker may
        // still be reading it. Nothing after it may add to it.
        CHECK_NEAR(fake->chA.tunerParams.rfFreq.rfHz, 101100000.0, 1.0);
        CHECK(everySetterAccepted(*src, *fake, 101100000.0, "after an abandoned control") == 0);
        fake->releaseUpdateHang.store(true);
        for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(fake->leftUpdate.load());
    }
    {   // 2. A STREAM DECLARED STALLED: nothing ever called back.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
        CHECK(src.start());
        std::vector<std::complex<float>> buf(4096);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (!src.faulted() && std::chrono::steady_clock::now() < deadline) {
            (void) src.read(buf.data(), buf.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(src.faulted());
        CHECK(everySetterAccepted(src, fake, 100000000.0, "after the stream stalled") == 0);
        src.stop();
        src.closeDevice();
    }
    {   // 3. THE SERVICE ANSWERED ServiceNotResponding - streaming, and then
        // stopped, where every setter used to fall through to "not streaming,
        // the block IS the radio" and answer true.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        fake.updateResult = abi::ServiceNotResponding;
        CHECK(src.setCenterFrequencyHz(101100000.0) == false);
        CHECK(src.deviceDead());
        CHECK_NEAR(fake.chA.tunerParams.rfFreq.rfHz, 100000000.0, 1.0);  // a refusal: put back
        CHECK(everySetterAccepted(src, fake, 100000000.0, "after ServiceNotResponding, live") ==
              0);
        src.stop();
        CHECK(fake.countStarting("Uninit") == 0);
        CHECK(everySetterAccepted(src, fake, 100000000.0, "after ServiceNotResponding, stopped") ==
              0);
        src.closeDevice();
    }
}

// --- 5d-2. the lost-session rule is the SESSION'S, and a fresh one lifts it --
//
// THE THIRD REVIEW OF fix/rsp-fallback. testALostRadioTakesNoSettingAtAll pins
// the refusal for the object whose OWN call lost the service. The reviewer's
// probe then found three things nothing pinned:
//   - a second radio (B) streaming beside the one that lost the service (A)
//     still took a setting: its LNA change answered true and sent one Update
//     into the session the process had already declared lost, because every
//     setter asked only about its own object's flags;
//   - open() clearing those flags is what lets the SAME object take settings
//     again once the service really is back (here, a fresh fake's table), and
//     removing that reset broke nothing;
//   - a radio the USER stopped while healthy must not be called stalled by
//     reads that come back empty afterwards, and nothing but a probe said so.

// Every setter an RSPdx has - all ten, the RSPdx being the model that has all
// of them - each asked for the value it is already at and for one that changes
// something. `second` picks the other set of changing values, so a second pass
// over the same object is a real change as well.
struct SetterPass {
    int accepted = 0;
    int tried = 0;
    int updates = 0;  // Update( calls the pass sent
};

SetterPass everyRspDxSetter(SdrPlaySource& src, FakeSdrPlayApi& fake, bool second,
                            const char* when) {
    const int updatesBefore = fake.countStarting("Update(");
    struct Try {
        const char* what;
        bool ok;
    };
    // A braced list is evaluated left to right, so each readback used as an
    // argument is read after the setter before it has run.
    const std::vector<Try> tries = {
        {"retune to where it is", src.setCenterFrequencyHz(src.centerFrequencyHz())},
        {"retune", src.setCenterFrequencyHz(second ? 96000000.0 : 95000000.0)},
        {"the rate it is at", src.setSampleRateHz(src.sampleRateHz())},
        {"a new rate", src.setSampleRateHz(second ? 2000000.0 : 8000000.0)},
        {"the IF gain it is at", src.setGainDb("IF", src.gainDb("IF"))},
        {"a new IF gain", src.setGainDb("IF", second ? -40.0 : -30.0)},
        {"the LNA state it is at", src.setGainDb("LNA", src.gainDb("LNA"))},
        {"a new LNA state", src.setGainDb("LNA", second ? 5.0 : 3.0)},
        // Off as it is, on, a set point under it, and off again - so the AGC
        // ends where it began and a manual IF gain in the next pass is legal.
        {"the AGC as it is (off)", src.setAutoGain(src.autoGain())},
        {"AGC on", src.setAutoGain(true)},
        {"a new AGC set point", src.setAgcSetPointDbfs(second ? -40 : -30)},
        {"AGC off again", src.setAutoGain(false)},
        {"the antenna it is on", src.setAntenna(src.antenna())},
        {"a new antenna", src.setAntenna(second ? "Antenna A" : "Antenna B")},
        {"bias tee the other way", src.setBiasT(!src.biasT())},
        {"FM notch the other way", src.setRfNotch(!src.rfNotch())},
        {"DAB notch the other way", src.setDabNotch(!src.dabNotch())},
        {"HDR mode as it is", src.setHdrMode(src.hdrMode())},
        {"HDR mode the other way", src.setHdrMode(!src.hdrMode())},
    };
    SetterPass p;
    p.tried = static_cast<int>(tries.size());
    for (const Try& t : tries) {
        if (t.ok) {
            ++p.accepted;
        } else {
            // No reason printed: every call has run by now, so lastError()
            // holds the LAST refusal's message, not this one's.
            std::printf("     %s: \"%s\" was refused\n", when, t.what);
        }
    }
    p.updates = fake.countStarting("Update(") - updatesBefore;
    std::printf("%s: %d of %d setters accepted, %d Update(s)\n", when, p.accepted, p.tried,
                p.updates);
    return p;
}

struct Readbacks {
    double hz, rate, ifDb, lna;
    bool agc;
    int setPoint;
    std::string ant;
    bool bias, notch, dab, hdr;
};

Readbacks readbacksOf(SdrPlaySource& s) {
    return {s.centerFrequencyHz(), s.sampleRateHz(), s.gainDb("IF"), s.gainDb("LNA"),
            s.autoGain(),          s.agcSetPointDbfs(), s.antenna(),  s.biasT(),
            s.rfNotch(),           s.dabNotch(),        s.hdrMode()};
}

bool sameReadbacks(const Readbacks& x, const Readbacks& y) {
    return x.hz == y.hz && x.rate == y.rate && x.ifDb == y.ifDb && x.lna == y.lna &&
           x.agc == y.agc && x.setPoint == y.setPoint && x.ant == y.ant && x.bias == y.bias &&
           x.notch == y.notch && x.dab == y.dab && x.hdr == y.hdr;
}

void testALostSessionRefusesTheOtherRadiosSettingsToo() {
    FakeSdrPlayApi fake;
    fake.addDevice("2305000AAA", abi::kRspDx);
    fake.addDevice("2305000BBB", abi::kRspDx);
    SdrPlaySource a;
    SdrPlaySource b;
    CHECK(openOn(a, fake, "index=0"));
    CHECK(a.start());
    CHECK(openOn(b, fake, "index=1"));
    CHECK(b.start());  // started last, so the fake's acknowledgements are B's
    // B is healthy and live: a setting it is given is sent and taken.
    CHECK(b.setGainDb("LNA", 2.0));
    CHECK(fake.countStarting("Update(") == 1);

    // A's retune is answered ServiceNotResponding: A is dead and the process's
    // session is lost.
    fake.updateResult = abi::ServiceNotResponding;
    CHECK(a.setCenterFrequencyHz(101100000.0) == false);
    CHECK(a.deviceDead());
    // From here anything B sent would be TAKEN - so a refusal below is B's
    // own, not the fake's.
    fake.updateResult = abi::Success;

    const BlockBytes before = blockOf(fake);
    const std::size_t callsBefore = fake.callCount();
    const Readbacks was = readbacksOf(b);
    const SetterPass p = everyRspDxSetter(b, fake, false, "B after A lost the session");
    const std::size_t sent = fake.callCount() - callsBefore;
    std::printf("B after A lost the session: %zu vendor call(s), block %s\n", sent,
                sameBlock(before, blockOf(fake)) ? "unchanged" : "WRITTEN");
    CHECK(p.accepted == 0);
    CHECK(p.updates == 0);
    CHECK(sent == 0);
    CHECK(sameBlock(before, blockOf(fake)));
    CHECK(sameReadbacks(was, readbacksOf(b)));
    // ...and what B shows is the sentence that says what to do.
    CHECK(std::string(b.lastError()).find("restart the SDRplay API service, then restart FoxSDR") !=
          std::string::npos);

    a.stop();
    a.closeDevice();
    b.stop();
    b.closeDevice();
}

// THE TEARDOWN OF THE OTHER RADIO, the half the test above left out (GitHub
// issue 5, the 0.99.43 RSP2 report, 2026-09-28). A lost session means "no
// further SDRplay API calls are made until FoxSDR is restarted" - the log says
// exactly that - but only start(), open(), the scan and the setters asked the
// table. stop() and closeDevice() asked this object's own flags, so a second
// radio still streaming when the first one lost the session went into
// sdrplay_api_Uninit, ReleaseDevice and Close on the dead service: three more
// unbounded calls, each of which the 0.99.43 log shows taking five seconds to
// answer ServiceNotResponding on the GUI thread (15:40:19.9 to 15:40:24.9).
void testALostSessionKeepsTheOtherRadiosTeardownOffTheVendorDll() {
    FakeSdrPlayApi fake;
    fake.addDevice("2305000AAA", abi::kRsp2);
    fake.addDevice("2305000BBB", abi::kRsp2);
    SdrPlaySource a;
    SdrPlaySource b;
    CHECK(openOn(a, fake, "index=0"));
    CHECK(a.start());
    CHECK(openOn(b, fake, "index=1"));
    CHECK(b.start());

    // A's stop answers ServiceNotResponding, as the report's did: A is dead
    // and the process's session is lost.
    fake.uninitResult = abi::ServiceNotResponding;
    a.stop();
    CHECK(a.deviceDead());
    fake.uninitResult = abi::Success;  // anything B sends from here would be taken

    const int uninitBefore = fake.countStarting("Uninit");
    const int releaseBefore = fake.countStarting("ReleaseDevice");
    const int closeBefore = fake.closeCount;
    b.stop();
    b.closeDevice();
    const int uninits = fake.countStarting("Uninit") - uninitBefore;
    const int releases = fake.countStarting("ReleaseDevice") - releaseBefore;
    std::printf("B's teardown after A lost the session: %d Uninit, %d ReleaseDevice, %d Close\n",
                uninits, releases, fake.closeCount - closeBefore);
    CHECK(uninits == 0);
    CHECK(releases == 0);
    CHECK(fake.closeCount == closeBefore);
    CHECK(!b.running());
    CHECK(!b.isOpen());
    a.closeDevice();
}

// AND THE LINE THE LOG KEEPS OF A REFUSED OPEN MUST STILL SAY WHAT TO DO. The
// 0.99.43 report's 15:42:47.168 line ends "...until it is restarted - restart "
// because the session sentence, prefixed with "source: SDRplay open failed - ",
// is longer than a log line (DiagLog::kLineBytes). The panel had the whole
// sentence; the log - which is what reaches us - lost the instruction.
void testARefusedOpenAfterALossLogsTheWholeInstruction() {
    FakeSdrPlayApi fake;
    fake.addDevice("2305000CCC", abi::kRsp2);
    {
        SdrPlaySource a;
        CHECK(openOn(a, fake));
        CHECK(a.start());
        fake.uninitResult = abi::ServiceNotResponding;
        a.stop();
        a.closeDevice();
    }
    SdrPlaySource other;
    CHECK(!openOn(other, fake));
    std::string line;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find("SDRplay open") != std::string::npos) { line = l; }
    }
    std::printf("the refused open's log line: %s\n", line.c_str());
    CHECK(!line.empty());
    CHECK(line.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes) - 1u);
    CHECK(line.find("restart the SDRplay API service, then") != std::string::npos);
    CHECK(line.find("FoxSDR") != std::string::npos);
    // ...and what the panel shows is still the whole sentence.
    CHECK(std::string(other.lastError()) ==
          std::string(cascade::source::sdrPlaySessionLostSentence()));
}

void testTheSameRadioReopenedAfterALossTakesSettingsAgain() {
    for (int way = 0; way < 2; ++way) {
        const bool stall = (way == 1);
        // Declared before the source so both outlive it. One fake at a time
        // (see FakeSdrPlayApi's constructor): the lost one goes before the
        // fresh one is made.
        std::unique_ptr<FakeSdrPlayApi> lost(new FakeSdrPlayApi());
        std::unique_ptr<FakeSdrPlayApi> fresh;
        SdrPlaySource src;
        lost->addDevice("2305000CCC", abi::kRspDx);
        CHECK(openOn(src, *lost));
        if (stall) {
            src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
            CHECK(src.start());
            std::vector<std::complex<float>> buf(4096);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            while (!src.faulted() && std::chrono::steady_clock::now() < deadline) {
                (void) src.read(buf.data(), buf.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            CHECK(src.start());
            lost->updateResult = abi::ServiceNotResponding;
            CHECK(src.setCenterFrequencyHz(101100000.0) == false);
        }
        CHECK(src.deviceDead());
        src.stop();
        src.closeDevice();

        // THE SERVICE IS BACK: a fresh table, whose session is not lost, and
        // the SAME object opened on it.
        lost.reset();
        fresh.reset(new FakeSdrPlayApi());
        fresh->addDevice("2305000CCC", abi::kRspDx);
        CHECK(openOn(src, *fresh));
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        const char* stopped = stall ? "reopened after a stall, stopped"
                                    : "reopened after a service loss, stopped";
        const char* live = stall ? "reopened after a stall, live"
                                 : "reopened after a service loss, live";
        const SetterPass s = everyRspDxSetter(src, *fresh, false, stopped);
        CHECK(s.accepted == s.tried);
        CHECK(s.updates == 0);  // not streaming: the block IS the radio
        CHECK(src.start());
        const SetterPass l = everyRspDxSetter(src, *fresh, true, live);
        CHECK(l.accepted == l.tried);
        CHECK(l.updates > 0);
        CHECK(!src.deviceDead());
        src.stop();
        src.closeDevice();
    }
}

void testAUserStopIsNotAStall() {
    FakeSdrPlayApi fake;
    fake.addDevice("2305000DDD", abi::kRspDx);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    src.setStreamStallLimitForTest(std::chrono::milliseconds(300));
    CHECK(src.start());
    src.stop();  // the user's STOP, on a healthy radio

    // Read past the stall limit four times over, as the pipeline's source
    // thread does while the radio sits stopped.
    std::vector<std::complex<float>> buf(4096);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    int reads = 0;
    while (std::chrono::steady_clock::now() < until) {
        (void) src.read(buf.data(), buf.size());
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("user-stopped radio read %d time(s) over 1200 ms against a 300 ms limit: %s\n",
                reads, src.faulted() ? "FAULTED" : "not faulted");
    CHECK(!src.faulted());
    CHECK(!src.deviceDead());
    const SetterPass s = everyRspDxSetter(src, fake, false, "user-stopped, read past the limit");
    CHECK(s.accepted == s.tried);
    CHECK(s.updates == 0);
    CHECK(src.start());
    const SetterPass l = everyRspDxSetter(src, fake, true, "restarted after the user's stop, live");
    CHECK(l.accepted == l.tried);
    CHECK(l.updates > 0);
    CHECK(!src.faulted());
    src.stop();
    src.closeDevice();
}

// --- 5e. a change refused HALF-WAY reads back where the radio stopped --------
//
// THE REVIEW OF 6e308c3, point 3. Two setters send two Updates, and the second
// can be refused after the first was taken:
//   - an RSP2 on Hi-Z asked for Antenna B comes off the AM port first (taken)
//     and then moves the antenna switch (refused): the radio is on port 2, and
//     antenna() still said "Hi-Z";
//   - setAutoGain(false) turns the AGC off (taken) and then restores our IF
//     gain (refused): it answered TRUE, and gainDb("IF") reported a number the
//     radio is not at.
// Either way the caller must hear false, and every readback must be what the
// radio actually accepted.
void testAChangeRefusedHalfWayReadsBackWhereTheRadioStopped() {
    {   // THE RSP2's ANTENNA.
        FakeSdrPlayApi fake;
        fake.addDevice("1706012347", abi::kRsp2);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        CHECK(src.setAntenna("Hi-Z"));
        CHECK(fake.chA.rsp2TunerParams.amPortSel == abi::Rsp2_AMPORT_1);
        CHECK(fake.chA.rsp2TunerParams.antennaSel == abi::Rsp2_ANTENNA_A);
        fake.calls.clear();
        fake.refuseUpdateAfter = 1;  // the AM port is taken, the antenna switch refused
        const bool ok = src.setAntenna("Antenna B");
        std::printf("RSP2 Hi-Z -> Antenna B, switch refused: returned %s, %d Update(s), antenna() "
                    "reads \"%s\", AM port %d, antenna sel %d\n",
                    ok ? "true" : "false", fake.countStarting("Update("), src.antenna().c_str(),
                    static_cast<int>(fake.chA.rsp2TunerParams.amPortSel),
                    static_cast<int>(fake.chA.rsp2TunerParams.antennaSel));
        CHECK(ok == false);
        CHECK(fake.countStarting("Update(") == 2);  // both were really sent
        CHECK(fake.chA.rsp2TunerParams.amPortSel == abi::Rsp2_AMPORT_2);    // taken
        CHECK(fake.chA.rsp2TunerParams.antennaSel == abi::Rsp2_ANTENNA_A);  // put back
        CHECK(src.antenna() == "Antenna A");  // port 2, switch on A: where the radio is
        CHECK(!src.deviceDead());             // an ordinary refusal
        // ...and asking again sends only the switch, because the port is done.
        fake.calls.clear();
        CHECK(src.setAntenna("Antenna B"));
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_Rsp2_AntennaControl, abi::Update_Ext1_None)}));
        CHECK(src.antenna() == "Antenna B");
        src.stop();
        src.closeDevice();
    }
    {   // THE AGC GOING OFF.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        CHECK(src.setGainDb("IF", -33.0));
        CHECK(src.setAutoGain(true));
        // The service's AGC loop owns gRdB while it runs and leaves its own
        // number there - which is why coming off it restores ours at all.
        fake.chA.tunerParams.gain.gRdB = 47;
        fake.calls.clear();
        fake.refuseUpdateAfter = 1;  // AGC off is taken, the IF gain restore refused
        const bool ok = src.setAutoGain(false);
        std::printf("AGC off taken, IF restore refused: returned %s, %d Update(s), autoGain %d, "
                    "gainDb(IF) %.0f, block gRdB %d\n",
                    ok ? "true" : "false", fake.countStarting("Update("),
                    src.autoGain() ? 1 : 0, src.gainDb("IF"),
                    static_cast<int>(fake.chA.tunerParams.gain.gRdB));
        CHECK(ok == false);
        CHECK(fake.countStarting("Update(") == 2);
        CHECK(fake.chA.ctrlParams.agc.enable == abi::AGC_DISABLE);  // taken
        CHECK(src.autoGain() == false);                              // ...and said so
        CHECK(fake.chA.tunerParams.gain.gRdB == 47);                 // put back
        CHECK_NEAR(src.gainDb("IF"), -47.0, 1e-9);                   // where the radio IS
        CHECK(!src.deviceDead());
        // ...and our number is then a real change, sent once.
        fake.calls.clear();
        CHECK(src.setGainDb("IF", -33.0));
        CHECK(fake.countStarting("Update(") == 1);
        CHECK(fake.chA.tunerParams.gain.gRdB == 33);
        CHECK_NEAR(src.gainDb("IF"), -33.0, 1e-9);
        src.stop();
        src.closeDevice();
    }
}

// --- 6. gains -------------------------------------------------------------

void testGains() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    const std::vector<cascade::source::GainInfo> g = src.gains();
    CHECK(g.size() == 2);
    if (g.size() == 2) {
        CHECK(g[0].name == "IF");
        // A NEGATIVE GAIN, because the hardware's number is a REDUCTION and a
        // slider whose right-hand end is quieter is the inconsistency this
        // sign flip exists to prevent.
        CHECK_NEAR(g[0].minDb, -59.0, 1e-9);
        CHECK_NEAR(g[0].maxDb, -20.0, 1e-9);
        CHECK(g[0].unit == cascade::source::GainUnit::Decibels);
        CHECK(g[1].name == "LNA");
        // An index into a table the API owns, not decibels - printing step 7
        // as "7.0 dB" would be a number in a unit the radio does not use.
        CHECK(g[1].unit == cascade::source::GainUnit::Steps);
        CHECK_NEAR(g[1].minDb, 0.0, 1e-9);
        CHECK_NEAR(g[1].maxDb, 9.0, 1e-9);  // RSP1A: LNAstate 0..9
    }

    fake.calls.clear();
    CHECK(src.setGainDb("IF", -25.0));
    CHECK(fake.chA.tunerParams.gain.gRdB == 25);
    CHECK_NEAR(src.gainDb("IF"), -25.0, 1e-9);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, 0)}));

    // CLAMPED, not refused, and gainDb reports what was programmed.
    CHECK(src.setGainDb("IF", -5.0));
    CHECK(fake.chA.tunerParams.gain.gRdB == 20);
    CHECK_NEAR(src.gainDb("IF"), -20.0, 1e-9);
    CHECK(src.setGainDb("IF", -200.0));
    CHECK(fake.chA.tunerParams.gain.gRdB == 59);
    CHECK_NEAR(src.gainDb("IF"), -59.0, 1e-9);

    fake.calls.clear();
    CHECK(src.setGainDb("LNA", 3.0));
    CHECK(fake.chA.tunerParams.gain.LNAstate == 3);
    CHECK_NEAR(src.gainDb("LNA"), 3.0, 1e-9);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, 0)}));

    CHECK(src.setGainDb("LNA", 99.0));
    CHECK(fake.chA.tunerParams.gain.LNAstate == 9);

    CHECK(src.setGainDb("TUNER", 5.0) == false);
    CHECK(std::string(src.lastError()).find("no gain called") != std::string::npos);

    // The per-model LNA counts, from the reference's own table.
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRsp1) == 4);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRsp2) == 9);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRsp1A) == 10);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRsp1B) == 10);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRspDuo) == 10);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRspDx) == 28);
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRspDxR2) == 28);
}

void testAgc() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    CHECK(src.setGainDb("IF", -33.0));

    fake.calls.clear();
    CHECK(src.setAutoGain(true));
    CHECK(src.autoGain());
    CHECK(fake.chA.ctrlParams.agc.enable == abi::AGC_CTRL_EN);
    CHECK(fake.chA.ctrlParams.agc.setPoint_dBfs == -60);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(abi::Update_Ctrl_Agc, 0)}));

    // With the AGC on, a hand-set IF gain is refused rather than written into
    // a field the loop owns - the panel must not show a number the radio is
    // not at.
    fake.calls.clear();
    CHECK(src.setGainDb("IF", -44.0) == false);
    CHECK(std::string(src.lastError()).find("AGC") != std::string::npos);
    CHECK(fake.calls.empty());

    fake.calls.clear();
    CHECK(src.setAgcSetPointDbfs(-40));
    CHECK(fake.chA.ctrlParams.agc.setPoint_dBfs == -40);
    CHECK(src.agcSetPointDbfs() == -40);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(abi::Update_Ctrl_Agc, 0)}));

    // Coming off the AGC puts OUR number back, so the panel and the radio
    // agree again.
    fake.calls.clear();
    CHECK(src.setAutoGain(false));
    CHECK(!src.autoGain());
    CHECK(fake.chA.ctrlParams.agc.enable == abi::AGC_DISABLE);
    CHECK(fake.chA.tunerParams.gain.gRdB == 33);
    CHECK((fake.calls == std::vector<std::string>{
                            FakeSdrPlayApi::updateCall(abi::Update_Ctrl_Agc, 0),
                            FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, 0)}));

    // The service's own calibrated figure, which with the AGC running is the
    // only honest answer to "what gain is the radio at".
    CHECK(!src.haveCurrentGainDb());
    fake.fireGainChange(42.5);
    CHECK(src.haveCurrentGainDb());
    CHECK_NEAR(src.currentGainDb(), 42.5, 1e-9);
}

// --- 7. antennas ----------------------------------------------------------

void testAntennasPerModel() {
    // The pure table first.
    CHECK((cascade::source::sdrPlayAntennas(abi::kRsp1A, abi::RspDuoMode_Unknown) ==
          std::vector<std::string>{"RX"}));
    CHECK((cascade::source::sdrPlayAntennas(abi::kRspDx, abi::RspDuoMode_Unknown) ==
          std::vector<std::string>{"Antenna A", "Antenna B", "Antenna C"}));
    CHECK((cascade::source::sdrPlayAntennas(abi::kRspDxR2, abi::RspDuoMode_Unknown) ==
          std::vector<std::string>{"Antenna A", "Antenna B", "Antenna C"}));
    CHECK((cascade::source::sdrPlayAntennas(abi::kRsp2, abi::RspDuoMode_Unknown) ==
          std::vector<std::string>{"Antenna A", "Antenna B", "Hi-Z"}));
    CHECK((cascade::source::sdrPlayAntennas(abi::kRspDuo, abi::RspDuoMode_Single_Tuner) ==
          std::vector<std::string>{"Tuner 1", "Tuner 2"}));

    {   // An RSP1A has one input and accepts its own name.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK((src.antennas() == std::vector<std::string>{"RX"}));
        CHECK(src.antenna() == "RX");
        CHECK(src.setAntenna("RX"));
        CHECK(src.setAntenna("Antenna B") == false);
    }

    {   // The RSPdx's A/B/C switch lives in DevParams and is asked for through
        // the EXTENSION word, which is the one Update in the driver whose
        // first reason is None.
        FakeSdrPlayApi fake;
        fake.addDevice("2002000ABC", abi::kRspDx);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        CHECK(src.antenna() == "Antenna A");
        CHECK(fake.devParams.rspDxParams.antennaSel == abi::RspDx_ANTENNA_A);

        fake.calls.clear();
        CHECK(src.setAntenna("Antenna C"));
        CHECK(src.antenna() == "Antenna C");
        CHECK(fake.devParams.rspDxParams.antennaSel == abi::RspDx_ANTENNA_C);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_None, abi::Update_RspDx_AntennaControl)}));
    }

    {   // An RSPduo is narrowed to single-tuner AT SELECT - there is no later
        // call that can change the mode - and its two tuners are presented as
        // two antennas.
        FakeSdrPlayApi fake;
        fake.addRspDuo("1809001ABC");
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(fake.indexStarting("SelectDevice(tuner=1,mode=1)") >= 0);
        CHECK((src.antennas() == std::vector<std::string>{"Tuner 1", "Tuner 2"}));
        CHECK(src.antenna() == "Tuner 1");

        // STOPPED: the tuner is fixed at SelectDevice, so moving it is a
        // release and a re-select - and everything we had programmed has to
        // survive the round trip, because the parameter block was the
        // service's memory and is gone.
        fake.chA.tunerParams.gain.gRdB = 37;
        fake.calls.clear();
        CHECK(src.setAntenna("Tuner 2"));
        CHECK(src.antenna() == "Tuner 2");
        CHECK(fake.indexOf("ReleaseDevice") == 0);
        CHECK(fake.indexStarting("SelectDevice(tuner=2,mode=1)") == 1);
        CHECK(fake.indexOf("GetDeviceParams") > 1);
        CHECK(fake.chB.tunerParams.gain.gRdB == 37);
        CHECK_NEAR(fake.chB.tunerParams.rfFreq.rfHz, 100000000.0, 1.0);

        // LIVE: the API has one call for exactly this, and it is the only way
        // to move the tuner without dropping the stream.
        CHECK(src.start());
        fake.calls.clear();
        CHECK(src.setAntenna("Tuner 1"));
        CHECK(src.antenna() == "Tuner 1");
        CHECK((fake.calls == std::vector<std::string>{"SwapRspDuoActiveTuner"}));
        CHECK(fake.releaseCount == 1);  // and NOT a second release-and-reselect
    }

    {   // An RSPduo another application already holds offers no single-tuner
        // mode, and is refused rather than selected into a mode it cannot be
        // in.
        FakeSdrPlayApi fake;
        fake.addDevice("1809001ABC", abi::kRspDuo, abi::RspDuoMode_Slave, abi::Tuner_A);
        SdrPlaySource src;
        CHECK(openOn(src, fake) == false);
        CHECK(std::string(src.lastError()).find("already in use") != std::string::npos);
        CHECK(fake.selectCount == 0);
    }
}

// --- 8. rates and decimation ----------------------------------------------

// EVERY PLAN DELIVERS THE RATE IT IS FILED UNDER, by the only arithmetic the
// SDRplay API specification gives (v3.15, sdrplay_api_Init: the processing
// chain is ReadUSBdata, DCoffsetCorrection, Agc, DownConvert, Decimate,
// IQimbalanceCorrection - and of those only Decimate is said to change the
// rate). That makes the output fsHz / decimationFactor in ZERO-IF, and
// nothing in the specification says what it is in LOW-IF.
//
// WHY THIS IS A TEST (GitHub issue 5, the 0.99.43 RSP2 report). Until 0.99.44
// 2 MS/s was produced as SoapySDRPlay3 does it - a 6 MHz ADC at the 1.62 MHz
// low IF - on the reference's assumption that the service then hands over
// 2 MS/s. Two RSP2 field logs say it handed over the ADC rate instead: the
// 0.99.27 log's first minute is 359978976 samples in 178561 blocks of 2016
// (6.0 MS/s), and the 0.99.43 log is 168956928 samples in 83808 blocks of
// 2016 in the 28.2 s before the stream stopped (6.0 MS/s) - while FoxSDR
// called the radio 2 MS/s and built its whole chain for that. A plan whose
// output rate is fsHz / decM leaves no room for that disagreement.
void testEveryRatePlanDeliversTheRateItClaims() {
    for (const double r : cascade::source::sdrPlaySupportedRatesHz()) {
        SdrPlayRatePlan p;
        CHECK(cascade::source::sdrPlayRatePlan(r, p));
        const double delivered = p.fsHz / static_cast<double>(p.decEnable != 0 ? p.decM : 1u);
        const bool zeroIf = p.ifType == abi::IF_Zero;
        if (!zeroIf || std::fabs(delivered - r) > 0.5) {
            std::printf("     %.0f S/s: fsHz %.0f, IF %d, decimation %u (%s) delivers %.0f S/s\n", r,
                        p.fsHz, static_cast<int>(p.ifType), p.decM,
                        p.decEnable != 0 ? "on" : "off", delivered);
        }
        CHECK(zeroIf);
        CHECK_NEAR(delivered, r, 0.5);
        // The API's own default ADC rate is its floor here (spec: fsHz
        // "default: 2000000.0"), and decimation is on exactly when it divides.
        CHECK(p.fsHz >= 2000000.0 - 0.5);
        CHECK((p.decEnable != 0) == (p.decM > 1));
        CHECK(p.decM == 1 || p.decM == 2 || p.decM == 4 || p.decM == 8 || p.decM == 16 ||
              p.decM == 32);
    }
}

// THE DISAGREEMENT IS SAID IN THE LOG WHEN IT HAPPENS ANYWAY. Whatever the
// plan, the service is the authority on what it delivers; if the samples that
// arrive run at a rate far from the one FoxSDR set, the next report must say
// so in words rather than leave it to be worked out from the health line's
// arithmetic, as issue 5's was.
void testADeliveredRateFarFromTheSetOneIsSaid() {
    for (int way = 0; way < 2; ++way) {
        const bool triple = (way == 0);
        FakeSdrPlayApi fake;
        fake.addDevice("2305000DDD", abi::kRsp2);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK_NEAR(src.sampleRateHz(), 2000000.0, 1.0);
        CHECK(src.start());
        // Just over a second of blocks of 2016, at three times the set rate
        // (the report) or at the set rate (the control), paced against the
        // clock so the measured span is real.
        const unsigned int kBlockN = 2016;
        std::vector<short> xi(kBlockN, 100);
        std::vector<short> xq(kBlockN, -100);
        std::vector<std::complex<float>> buf(1u << 16);
        const double rate = triple ? 6000000.0 : 2000000.0;
        const auto t0 = std::chrono::steady_clock::now();
        double sent = 0.0;
        for (;;) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (elapsed > 1.25) { break; }
            while (sent < elapsed * rate) {
                fake.pushSamples(xi.data(), xq.data(), kBlockN);
                sent += kBlockN;
                while (src.read(buf.data(), buf.size()) == buf.size()) {}
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        while (src.read(buf.data(), buf.size()) > 0) {}
        cascade::core::DiagLog::instance().resetForTest();
        src.stop();
        std::string said;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (l.find("delivered") != std::string::npos) { said = l; }
        }
        std::printf("%s: %s\n", triple ? "three times the set rate" : "the set rate",
                    said.empty() ? "(nothing said)" : said.c_str());
        if (triple) {
            CHECK(!said.empty());
            CHECK(said.find("2000000 S/s") != std::string::npos);
        } else {
            CHECK(said.empty());
        }
        src.closeDevice();
    }
}

void testRatePlans() {
    // THE TABLE, all of it zero-IF since 0.99.44: the binary fractions of
    // 2 MS/s are the 2 MS/s ADC decimated by powers of two, and the audio
    // rates a 3.072 MS/s ADC decimated. Until 0.99.44 the binary fractions
    // came from the reference's 6 MHz LOW-IF front end, which this test used
    // to pin; the RSP2 field logs in testEveryRatePlanDeliversTheRateItClaims
    // are why it no longer does.
    SdrPlayRatePlan p;

    CHECK(cascade::source::sdrPlayRatePlan(2000000.0, p));
    CHECK_NEAR(p.fsHz, 2000000.0, 1.0);
    CHECK(p.ifType == abi::IF_Zero);
    CHECK(p.decEnable == 0);
    CHECK(p.decM == 1);
    CHECK(p.bwType == abi::BW_1_536);

    CHECK(cascade::source::sdrPlayRatePlan(62500.0, p));
    CHECK_NEAR(p.fsHz, 2000000.0, 1.0);
    CHECK(p.ifType == abi::IF_Zero);
    CHECK(p.decEnable == 1);
    CHECK(p.decM == 32);
    CHECK(p.wideBandSignal == 1);
    CHECK(p.bwType == abi::BW_0_200);

    CHECK(cascade::source::sdrPlayRatePlan(500000.0, p));
    CHECK(p.decM == 4);
    CHECK(p.ifType == abi::IF_Zero);
    CHECK_NEAR(p.fsHz, 2000000.0, 1.0);

    CHECK(cascade::source::sdrPlayRatePlan(384000.0, p));
    CHECK(p.ifType == abi::IF_Zero);
    CHECK(p.decM == 8);
    CHECK(p.decEnable == 1);
    CHECK(p.wideBandSignal == 1);
    CHECK_NEAR(p.fsHz, 3072000.0, 1.0);
    CHECK(p.bwType == abi::BW_0_300);

    CHECK(cascade::source::sdrPlayRatePlan(768000.0, p));
    CHECK(p.decM == 4);
    CHECK_NEAR(p.fsHz, 3072000.0, 1.0);
    CHECK(p.bwType == abi::BW_0_600);

    CHECK(cascade::source::sdrPlayRatePlan(8000000.0, p));
    CHECK(p.ifType == abi::IF_Zero);
    CHECK(p.decEnable == 0);
    CHECK(p.decM == 1);
    CHECK_NEAR(p.fsHz, 8000000.0, 1.0);
    CHECK(p.bwType == abi::BW_8_000);

    CHECK(cascade::source::sdrPlayRatePlan(3000000.0, p));
    CHECK_NEAR(p.fsHz, 3000000.0, 1.0);
    CHECK(p.bwType == abi::BW_1_536);

    CHECK(cascade::source::sdrPlayRatePlan(1234567.0, p) == false);

    // The filter ladder's boundaries.
    CHECK(cascade::source::sdrPlayBwForRate(250000.0) == abi::BW_0_200);
    CHECK(cascade::source::sdrPlayBwForRate(300000.0) == abi::BW_0_300);
    CHECK(cascade::source::sdrPlayBwForRate(600000.0) == abi::BW_0_600);
    CHECK(cascade::source::sdrPlayBwForRate(1536000.0) == abi::BW_1_536);
    CHECK(cascade::source::sdrPlayBwForRate(5000000.0) == abi::BW_5_000);
    CHECK(cascade::source::sdrPlayBwForRate(7000000.0) == abi::BW_7_000);
    CHECK(cascade::source::sdrPlayBwForRate(8000000.0) == abi::BW_8_000);

    const std::vector<double> rates = cascade::source::sdrPlaySupportedRatesHz();
    CHECK(rates.size() == 19);
    CHECK_NEAR(rates.front(), 62500.0, 1.0);
    CHECK_NEAR(rates.back(), 10000000.0, 1.0);
    for (std::size_t i = 1; i < rates.size(); ++i) { CHECK(rates[i] > rates[i - 1]); }
    // Every published rate has a plan; a menu entry the driver cannot produce
    // is a trap.
    for (double r : rates) { CHECK(cascade::source::sdrPlayRatePlan(r, p)); }
}

void testSetSampleRateWritesThePlanInOneUpdate() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    CHECK(src.supportedSampleRatesHz() == cascade::source::sdrPlaySupportedRatesHz());

    fake.calls.clear();
    CHECK(src.setSampleRateHz(8000000.0));
    CHECK_NEAR(src.sampleRateHz(), 8000000.0, 1.0);
    CHECK_NEAR(fake.devParams.fsFreq.fsHz, 8000000.0, 1.0);
    CHECK(fake.chA.tunerParams.ifType == abi::IF_Zero);
    CHECK(fake.chA.tunerParams.bwType == abi::BW_8_000);
    CHECK(fake.chA.ctrlParams.decimation.enable == 0);
    // ONE Update carrying every reason that changed - and ONLY those: the
    // radio starts at zero IF since 0.99.44, so the IF type is not among them
    // (it was, from the old 1.62 MHz low-IF start).
    CHECK(fake.callCount() == 1);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(
              abi::Update_Dev_Fs | abi::Update_Tuner_BwType, 0)}));

    fake.calls.clear();
    CHECK(src.setSampleRateHz(384000.0));
    CHECK_NEAR(src.sampleRateHz(), 384000.0, 1.0);
    CHECK_NEAR(fake.devParams.fsFreq.fsHz, 3072000.0, 1.0);
    CHECK(fake.chA.ctrlParams.decimation.enable == 1);
    CHECK(fake.chA.ctrlParams.decimation.decimationFactor == 8);
    CHECK(fake.chA.ctrlParams.decimation.wideBandSignal == 1);
    CHECK(fake.callCount() == 1);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Dev_Fs | abi::Update_Ctrl_Decimation |
                                abi::Update_Tuner_BwType,
                            0)}));

    // A rate that is not on the list is COERCED to the nearest, as the Soapy
    // path does, so a config that asked for something plausible still opens.
    CHECK(src.setSampleRateHz(2100000.0));
    CHECK_NEAR(src.sampleRateHz(), 2048000.0, 1.0);
}

// --- 9. streaming ---------------------------------------------------------

void testStreamingDeliversEverySampleInOrder() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    // Values chosen so every one is distinguishable and the ends of the int16
    // range are covered.
    const short xi[] = {0, 1, -1, 32767, -32768, 1000, -1000, 7};
    const short xq[] = {-7, 2, -2, -32768, 32767, -500, 500, 0};
    const unsigned int n = 8;
    fake.pushSamples(xi, xq, n);

    std::complex<float> got[16];
    std::size_t total = 0;
    for (int tries = 0; tries < 20 && total < n; ++tries) {
        total += src.read(got + total, 16 - total);
    }
    CHECK(total == n);
    for (unsigned int k = 0; k < n && k < total; ++k) {
        CHECK_NEAR(got[k].real(), static_cast<double>(xi[k]) / 32768.0, 1e-6);
        CHECK_NEAR(got[k].imag(), static_cast<double>(xq[k]) / 32768.0, 1e-6);
    }
    // In [-1, 1): the most negative int16 reaches exactly -1 and the most
    // positive never reaches +1, which is the whole reason the scale is 32768
    // and not 32767.
    CHECK_NEAR(got[4].real(), -1.0, 1e-9);
    CHECK(got[3].real() < 1.0);

    // A second block continues where the first left off; nothing is lost and
    // nothing is reordered.
    const short xi2[] = {11, 12, 13};
    const short xq2[] = {21, 22, 23};
    fake.pushSamples(xi2, xq2, 3);
    std::size_t second = 0;
    for (int tries = 0; tries < 20 && second < 3; ++tries) {
        second += src.read(got + second, 3 - second);
    }
    CHECK(second == 3);
    CHECK_NEAR(got[0].real(), 11.0 / 32768.0, 1e-6);
    CHECK_NEAR(got[2].imag(), 23.0 / 32768.0, 1e-6);

    CHECK(src.droppedSamples() == 0);

    // An empty ring answers zero after a BOUNDED wait, which is the self-paced
    // contract's "nothing yet, retry" - not a block and not a busy spin.
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t none = src.read(got, 4);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    CHECK(none == 0);
    CHECK(waited < 200);

    // A callback that arrives after stop() is dropped rather than filling a
    // ring nobody will drain.
    src.stop();
    CHECK(!src.running());
}

void testStreamHealthLine() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    const short xi[] = {1, 2, 3, 4};
    const short xq[] = {5, 6, 7, 8};
    fake.pushSamples(xi, xq, 4);
    fake.pushSamples(xi, xq, 4);

    const std::string line = src.streamHealthLine();
    // THE SAME FORMAT SoapySource WRITES, word for word, so a report from an
    // RSP is comparable with one from any other radio.
    CHECK(line.find("source: stream health - reads 2, with samples 2, timeouts 0, overflows 0, "
                    "errors 0, longest gap ") == 0);
    CHECK(line.find("8 samples in ") != std::string::npos);

    // The window starts again once it has been reported.
    CHECK(src.streamHealthLine().empty());
}

// --- 10. faults -----------------------------------------------------------

void testDeviceRemovedFaultsTheSource() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    CHECK(!src.faulted());
    CHECK(!src.deviceDead());

    fake.fireDeviceRemoved();

    // Without this the pipeline's source loop cannot tell a dead radio from a
    // quiet one, and retries forever on a pegged core.
    CHECK(src.faulted());
    CHECK(src.deviceDead());
    CHECK(src.faultedWhile() == "device removed");
    CHECK(std::string(src.lastError()).find("device removed") != std::string::npos);

    // And read() answers immediately rather than spending the full wait on a
    // device that is gone.
    std::complex<float> buf[4];
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(src.read(buf, 4) == 0);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                t0)
              .count() < 15);
}

void testDeviceFailureAndMasterLossAlsoFault() {
    {
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        abi::EventParamsT p{};
        fake.fireEvent(abi::DeviceFailure, p);
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "device failure");
    }
    {
        FakeSdrPlayApi fake;
        fake.addRspDuo("1809001ABC");
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        abi::EventParamsT p{};
        p.rspDuoModeParams.modeChangeType = abi::MasterDllDisappeared;
        fake.fireEvent(abi::RspDuoModeChange, p);
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "master stream lost");
    }
    {
        // A mode change that is NOT the master going away is not a fault - a
        // slave attaching is ordinary traffic.
        FakeSdrPlayApi fake;
        fake.addRspDuo("1809001ABC");
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        abi::EventParamsT p{};
        p.rspDuoModeParams.modeChangeType = abi::SlaveAttached;
        fake.fireEvent(abi::RspDuoModeChange, p);
        CHECK(!src.faulted());
    }
}

void testOverloadIsCountedAndAcknowledged() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.calls.clear();

    fake.fireOverload(true);
    CHECK(src.overloadEvents() == 1);
    // THE ACKNOWLEDGEMENT IS NOT OPTIONAL: the service keeps re-reporting an
    // overload until it is acknowledged, so a host that only logs the event
    // gets a log full of it and a service that never moves on.
    //
    // NOT SENT FROM fireOverload() ITSELF ANY MORE (2026-09-28): the event
    // callback only latches which tuner needs acknowledging now (it runs on
    // what stands in for the service's own thread here), and read() is what
    // actually issues the Update, through the same bounded, serialised path
    // every other control uses - see sdrplay_source.hpp's Link::
    // pendingOverloadAckA/B and ackOverloadLocked. One read() call drains it.
    std::complex<float> buf[4];
    (void) src.read(buf, 4);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));

    fake.calls.clear();
    fake.fireOverload(false);
    CHECK(src.overloadEvents() == 1);  // corrected is not a second overload
    (void) src.read(buf, 4);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));

    // An overload does NOT fault the source: it is the radio telling us the
    // gain is too high, not the radio going away.
    CHECK(!src.faulted());
    src.stop();
}

// THE ACKNOWLEDGEMENT GOES TO THE TUNER THE EVENT IS ABOUT. An RSPduo's
// tuner can be swapped on a LIVE stream (SwapRspDuoActiveTuner), and through
// 0.99.34 the acknowledgement was addressed with the tuner copied into the
// Link at Init - the old one - so after a swap to Tuner 2 an overload on
// Tuner 2 was acknowledged for Tuner 1, and the service, told nothing about
// the tuner that was overloading, kept re-reporting it.
void testOverloadAckFollowsALiveTunerSwap() {
    FakeSdrPlayApi fake;
    fake.addRspDuo("1809001ABC");
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.antenna() == "Tuner 1");
    CHECK(src.start());

    // Before any swap the service reports, and is answered on, Tuner A.
    // (2026-09-28: the acknowledgement is now deferred to read() - see
    // testOverloadIsCountedAndAcknowledged - so one read() call drains it
    // before each check below.)
    std::complex<float> buf[4];
    fake.calls.clear();
    fake.fireOverload(true, abi::Tuner_A);
    (void) src.read(buf, 4);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));
    CHECK(fake.lastUpdateTuner == abi::Tuner_A);

    // LIVE swap to Tuner 2, then Tuner 2 overloads.
    CHECK(src.setAntenna("Tuner 2"));
    CHECK(src.antenna() == "Tuner 2");
    CHECK(fake.releaseCount == 0);  // the live swap, not a release-and-reselect
    fake.calls.clear();
    fake.lastUpdateTuner = abi::Tuner_Neither;
    fake.fireOverload(true, abi::Tuner_B);
    CHECK(src.overloadEvents() == 2);
    (void) src.read(buf, 4);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));
    CHECK(fake.lastUpdateTuner == abi::Tuner_B);

    // ...and the "corrected" that follows is answered on the same tuner.
    fake.lastUpdateTuner = abi::Tuner_Neither;
    fake.fireOverload(false, abi::Tuner_B);
    (void) src.read(buf, 4);
    CHECK(fake.lastUpdateTuner == abi::Tuner_B);
    src.stop();
}

// THE OVERLAP THE 2026-09-28 CODE REVIEW FOUND: the event callback used to
// call sdrplay_api_Update straight from what stands in here for the
// service's own thread, completely unserialised against a GUI-thread
// control's own bounded worker. That is a genuine, unguarded race into a
// vendor DLL this whole driver is otherwise built around never entering
// except through one bounded, mutex-serialised path - and a plausible
// account of the field reports' sdrplay_api_Fail, though not a confirmed
// one (see scratchpad/bugs0928/sdrplay/mirics-windex.md).
//
// Proven with the fake's overlap detector: a retune's Update call is held
// open (updateDelayMs) comfortably inside kControlWait, and an overload is
// fired and drained while it is still open. maxConcurrentInUpdate answers
// whether a second thread ever landed inside the vendor call while the
// first was still there - 1 is correct, 2 is the bug this fixes.
void testOverloadDuringRetuneNeverOverlapsTheVendorCall() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.calls.clear();
    fake.updateDelayMs.store(80);  // comfortably inside kControlWait (1000 ms)

    std::thread retuneThread([&]() { (void) src.setCenterFrequencyHz(101.0e6); });
    // Wait until the retune's worker is ACTUALLY inside the fake's Update -
    // the race this test drives is "does the overload's ack land while the
    // retune's call is still open", not "does it merely land after the
    // retune was requested".
    while (fake.concurrentInUpdate.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    fake.fireOverload(true);
    std::complex<float> buf[4];
    (void) src.read(buf, 4);  // drains the ack - through devMutex_, if the fix holds
    retuneThread.join();

    CHECK(fake.maxConcurrentInUpdate.load() == 1);
    src.stop();
}

// M2 of the round-3 review: drainPendingOverloadAcks must TRY devMutex_, not
// wait for it - measured, with the pre-fix (blocking lock_guard) code,
// read() taking 2453 ms behind a single slow GUI-thread control. Same setup
// as the overlap test above, but this time the retune's own Update is slow
// AND eventually answers (updateDelayMs, autoAck left off so the retune
// itself does not need to succeed for this - only that its worker holds
// devMutex_ for the whole delay), and what is measured is read()'s own
// wall-clock time, not just whether calls overlapped.
void testOverloadAckNeverBlocksReadBehindABusyDevMutex() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.calls.clear();
    fake.updateDelayMs.store(300);  // comfortably inside kControlWait (1000 ms)

    std::thread retuneThread([&]() { (void) src.setCenterFrequencyHz(101.0e6); });
    while (fake.concurrentInUpdate.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    fake.fireOverload(true);
    std::complex<float> buf[4];
    const auto t0 = std::chrono::steady_clock::now();
    (void) src.read(buf, 4);
    const long long readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
    std::printf("read() behind a busy devMutex_ took %lld ms (control delay 300 ms)\n", readMs);
    // Generous bound (kReadWait is 20 ms; this allows for scheduler noise
    // under a loaded CI box) - the point is "nowhere near 300 ms", which the
    // pre-fix code would be.
    CHECK(readMs < 100);
    // The ack was NOT sent yet - the try_lock failed while the retune's
    // worker was still inside thunkUpdate, so the flag was left set rather
    // than blocked on.
    CHECK(!fake.called(FakeSdrPlayApi::updateCall(abi::Update_Ctrl_OverloadMsgAck, 0)));

    retuneThread.join();  // the retune's own (slow) call finishes, releasing devMutex_

    // A LATER read() drains the still-pending ack.
    (void) src.read(buf, 4);
    CHECK(fake.called(FakeSdrPlayApi::updateCall(abi::Update_Ctrl_OverloadMsgAck, 0)));
    src.stop();
}

// L4 of the round-3 review: a stale pendingOverloadAck must not survive a
// stop()/start() cycle. Fires an overload and stops BEFORE any read() drains
// it (deliberately - this is the exact gap: nothing ever cleared the flag on
// that path before startStreamingLocked() was made to), then starts again
// and checks the brand new session's first read() sends no acknowledgement.
void testStaleOverloadAckDoesNotSurviveRestart() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    fake.fireOverload(true);
    src.stop();  // no read() in between - the flag is still set

    CHECK(src.start());
    fake.calls.clear();
    std::complex<float> buf[4];
    (void) src.read(buf, 4);
    CHECK(!fake.called(FakeSdrPlayApi::updateCall(abi::Update_Ctrl_OverloadMsgAck, 0)));
    src.stop();
}

// L5 of the round-3 review: an overload naming Tuner_Neither is acknowledged
// on link.tuner (the fallback for an event that names no tuner), which for a
// single-tuner model is Tuner_A.
//
// CORRECTED for the 0.99.59 RSPdx-R2 report. This test used to assert that the
// ordinary control tuner (device_.tuner, which updateLocked() sends with every
// retune) was Tuner_Neither for a single-tuner model and "legitimately
// different" from the acknowledgement's Tuner_A. That expectation was the
// defect: no vendor source selects or updates a single-tuner RSP with
// Tuner_Neither (see testTheRspDxR2UpdatesNameTunerAWithTheExactReasons), so a
// retune and the acknowledgement of an overload are now addressed to the SAME
// tuner, and this test pins that they cannot drift apart again.
void testOverloadNamingNeitherTunerAcksOnLinkTuner() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    CHECK(src.setCenterFrequencyHz(102.0e6));
    const abi::TunerSelectT controlTuner = fake.lastUpdateTuner;

    fake.fireOverload(true, abi::Tuner_Neither);
    std::complex<float> buf[4];
    (void) src.read(buf, 4);

    CHECK(controlTuner == abi::Tuner_A);
    CHECK(fake.lastUpdateTuner == abi::Tuner_A);
    CHECK(fake.lastUpdateTuner == controlTuner);
    src.stop();
}

// --- 11. the switches -----------------------------------------------------

void testBiasTeeAndNotchesPerModel() {
    {   // RSP1A: bias tee in the channel block, notches in DevParams - the
        // notch is in front of the whole front end, which is why the API puts
        // it there and not beside the tuner.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        CHECK(src.biasTeeSupported());
        CHECK(src.rfNotchSupported());
        CHECK(src.dabNotchSupported());
        CHECK(!src.hdrModeSupported());

        fake.calls.clear();
        CHECK(src.setBiasT(true));
        CHECK(src.biasT());
        CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 1);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_Rsp1a_BiasTControl, 0)}));

        fake.calls.clear();
        CHECK(src.setRfNotch(true));
        CHECK(fake.devParams.rsp1aParams.rfNotchEnable == 1);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_Rsp1a_RfNotchControl, 0)}));

        fake.calls.clear();
        CHECK(src.setDabNotch(true));
        CHECK(fake.devParams.rsp1aParams.rfDabNotchEnable == 1);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_Rsp1a_RfDabNotchControl, 0)}));
    }

    {   // RSPdx: every one of these is in DevParams and asked for through the
        // extension word.
        FakeSdrPlayApi fake;
        fake.addDevice("2002000ABC", abi::kRspDx);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        CHECK(src.hdrModeSupported());

        fake.calls.clear();
        CHECK(src.setBiasT(true));
        CHECK(fake.devParams.rspDxParams.biasTEnable == 1);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_None, abi::Update_RspDx_BiasTControl)}));

        fake.calls.clear();
        CHECK(src.setHdrMode(true));
        CHECK(src.hdrMode());
        CHECK(fake.devParams.rspDxParams.hdrEnable == 1);
        // At 3.15 the HDR bandwidth member is where we declared it, so it is
        // written; below 3.15 it moves and the driver leaves it alone.
        CHECK(fake.callCount() == 2);
        CHECK(fake.calls[0] ==
              FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_HdrEnable));
        CHECK(fake.calls[1] ==
              FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_HdrBw));
        CHECK(fake.chA.rspDxTunerParams.hdrBw == abi::RspDx_HDRMODE_BW_1_700);
    }

    {   // THE LAYOUT GATE. On a 3.11 service rspDxTunerParams sits four bytes
        // lower in the channel block, so writing the HDR bandwidth would land
        // on the wrong member. The driver enables HDR and leaves the bandwidth
        // at the API's default instead of guessing.
        FakeSdrPlayApi fake;
        fake.version = 3.11f;
        fake.addDevice("2002000ABC", abi::kRspDx);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        fake.calls.clear();
        CHECK(src.setHdrMode(true));
        CHECK(fake.devParams.rspDxParams.hdrEnable == 1);
        CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                abi::Update_None, abi::Update_RspDx_HdrEnable)}));
        CHECK(fake.chA.rspDxTunerParams.hdrBw == 0);
    }

    {   // RSP1: no bias tee, no notches, no HDR - and the switches say so
        // rather than writing into a member the hardware does not have.
        FakeSdrPlayApi fake;
        fake.addDevice("0001", abi::kRsp1);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(!src.biasTeeSupported());
        CHECK(!src.rfNotchSupported());
        CHECK(!src.dabNotchSupported());
        CHECK(!src.hdrModeSupported());
        CHECK(src.setBiasT(true) == false);
        CHECK(src.setRfNotch(true) == false);
        CHECK(src.setDabNotch(true) == false);
        CHECK(src.setHdrMode(true) == false);
        CHECK(!src.biasT());
    }

    {   // RSP2 has an FM notch and no DAB one.
        FakeSdrPlayApi fake;
        fake.addDevice("0002", abi::kRsp2);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.rfNotchSupported());
        CHECK(!src.dabNotchSupported());
        CHECK(src.setDabNotch(true) == false);
    }
}

// --- 11b. the RSPdx-R2, from the vendor's own specification ---------------
//
// THE 0.99.46 RSPdx-R2 REPORT (API 3.15, Windows 11): every control the user
// touched - frequency, antenna, LNA - was abandoned at kControlWait. The first
// thing to rule out was that the R2 falls into an unknown-model path and is
// sent a reason or a parameter block the service never answers. Every number
// below is taken from SDRplay API Specification 3.15 (Revision 3.15, 10 May
// 2024, "Added RSPdxR2 Support"), not from memory or from another project:
//
//   p6   #define SDRPLAY_RSPdxR2_ID (7)
//   p4   "Note: for the RSPdxR2, use RSPdx update and structure parameters."
//   p7   sdrplay_api_Update_Tuner_Gr 0x00008000, _Tuner_Frf 0x00020000;
//        sdrplay_api_Update_RspDx_HdrEnable 0x01, _BiasTControl 0x02,
//        _AntennaControl 0x04, _RfNotchControl 0x08, _RfDabNotchControl 0x10,
//        _HdrBw 0x20 (all in ReasonForUpdateExtension1T)
//   p17  RspDx_ANTENNA_A/B/C = 0/1/2; RSPDX_NUM_LNA_STATES 28
//   p27  Update_Tuner_Gr covers "gain->gRdB or gain->LNAstate"
//   p28  the RspDx_* reasons name deviceParams->devParams->rspDxParams.*
//
// So the R2 must be driven exactly as an RSPdx: the switches through the
// extension word with the first reason None, and the LNA and the frequency
// through the ordinary tuner reasons with the extension word None.
void testTheRspDxR2IsDrivenWithTheRspDxReasonsAndBlock() {
    static_assert(abi::kRspDxR2 == 7, "SDRPLAY_RSPdxR2_ID, spec p6");
    static_assert(abi::kRspDx == 4, "SDRPLAY_RSPdx_ID, spec p6");
    static_assert(abi::Update_RspDx_HdrEnable == 0x00000001u, "spec p7");
    static_assert(abi::Update_RspDx_BiasTControl == 0x00000002u, "spec p7");
    static_assert(abi::Update_RspDx_AntennaControl == 0x00000004u, "spec p7");
    static_assert(abi::Update_RspDx_RfNotchControl == 0x00000008u, "spec p7");
    static_assert(abi::Update_RspDx_RfDabNotchControl == 0x00000010u, "spec p7");
    static_assert(abi::Update_RspDx_HdrBw == 0x00000020u, "spec p7");
    static_assert(abi::Update_Tuner_Gr == 0x00008000u, "spec p7");
    static_assert(abi::Update_Tuner_Frf == 0x00020000u, "spec p7");
    CHECK(cascade::source::sdrPlayLnaStateCount(abi::kRspDxR2) == 28);

    FakeSdrPlayApi fake;
    fake.addDevice("2406000R2X", abi::kRspDxR2);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.hardwareVersion() == abi::kRspDxR2);
    CHECK(std::string(src.name()).find("RSPdx-R2") != std::string::npos);
    CHECK((src.antennas() == std::vector<std::string>{"Antenna A", "Antenna B", "Antenna C"}));
    CHECK(src.biasTeeSupported());
    CHECK(src.rfNotchSupported());
    CHECK(src.dabNotchSupported());
    CHECK(src.hdrModeSupported());
    CHECK(src.start());

    // The three controls the report names, in its order.
    fake.calls.clear();
    CHECK(src.setCenterFrequencyHz(145500000.0));
    CHECK(fake.chA.tunerParams.rfFreq.rfHz == 145500000.0);
    CHECK((fake.calls == std::vector<std::string>{
                             FakeSdrPlayApi::updateCall(abi::Update_Tuner_Frf, abi::Update_Ext1_None)}));

    fake.calls.clear();
    CHECK(src.setAntenna("Antenna B"));
    CHECK(fake.devParams.rspDxParams.antennaSel == abi::RspDx_ANTENNA_B);
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                             abi::Update_None, abi::Update_RspDx_AntennaControl)}));
    CHECK(src.antenna() == "Antenna B");

    fake.calls.clear();
    CHECK(src.setGainDb("LNA", 27.0));  // the top of RSPDX_NUM_LNA_STATES
    CHECK(fake.chA.tunerParams.gain.LNAstate == 27);
    CHECK((fake.calls == std::vector<std::string>{
                             FakeSdrPlayApi::updateCall(abi::Update_Tuner_Gr, abi::Update_Ext1_None)}));

    // And every RSPdx switch, through the extension word only.
    fake.calls.clear();
    CHECK(src.setBiasT(true));
    CHECK(fake.devParams.rspDxParams.biasTEnable == 1);
    CHECK(src.setRfNotch(true));
    CHECK(fake.devParams.rspDxParams.rfNotchEnable == 1);
    CHECK(src.setDabNotch(true));
    CHECK(fake.devParams.rspDxParams.rfDabNotchEnable == 1);
    CHECK(src.setHdrMode(true));
    CHECK(fake.devParams.rspDxParams.hdrEnable == 1);
    CHECK((fake.calls ==
           std::vector<std::string>{
               FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_BiasTControl),
               FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_RfNotchControl),
               FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_RfDabNotchControl),
               FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_HdrEnable),
               FakeSdrPlayApi::updateCall(abi::Update_None, abi::Update_RspDx_HdrBw)}));
    // None of the other models' fields was touched on the way.
    CHECK(fake.chA.rsp1aTunerParams.biasTEnable == 0);
    CHECK(fake.chA.rsp2TunerParams.biasTEnable == 0);
    CHECK(fake.chA.rspDuoTunerParams.biasTEnable == 0);
    CHECK(fake.devParams.rsp1aParams.rfNotchEnable == 0);
    src.stop();
}

// --- 11b. the tuner argument of sdrplay_api_Update ---------------------------
//
// THE 0.99.59 RSPdx-R2 REPORT (SDRplay API 3.15): the radio opens and streams
// for 22 s, the FIRST live control (an antenna change) is never answered, and
// the SDRplay API Service is found STOPPED afterwards. The user says a
// frequency change and starting a decoder end the same way, in two installs.
//
// What every Update this driver sent carried, before this was found, was
// `device_.tuner` - and for every model but the RSPduo the driver had set that
// to sdrplay_api_Tuner_Neither (0) before SelectDevice. The log line says it:
// "SDRplay opened RSPdx-R2, API 3.15, tuner 0". Nothing in the vendor's own
// code does that for a single-tuner model:
//   - SDRplay API Specification 3.15, section 4 (the example program, pp34-36):
//     only an RSPduo has `tuner` or `rspDuoMode` assigned before
//     sdrplay_api_SelectDevice; any other device is selected exactly as
//     sdrplay_api_GetDevices returned it, and every sdrplay_api_Update passes
//     `chosenDevice->tuner`.
//   - SDRplay's own RSPdxR2 ExtIO (github.com/SDRplay/ExtIO_SDRplay,
//     RSPdxR2/src/ExtIO_SDRplay.cpp): the same, and it picks the channel block
//     with `chosenDev->tuner == sdrplay_api_Tuner_A ? rxChannelA : rxChannelB`,
//     which only works if a single-tuner RSP is Tuner_A.
//   - SoapySDRPlay3 Settings.cpp: `device.tuner` is assigned only for an
//     RSPduo; every sdrplay_api_Update passes it.
// The specification (sdrplay_api_DeviceT, p8) says `tuner` is "Set by the
// application and used during sdrplay_api_SelectDevice() to indicate which
// tuner(s) is to be used", and sdrplay_api_Update's `tuner` "Specifies which
// tuner(s) to apply the update to"; Tuner_Neither is the enum's 0.
//
// The suite was blind to it because the fake's GetDevices answered
// Tuner_Neither for every single-tuner model and the call log never recorded
// the tuner of an Update at all - so "SelectDevice(tuner=0,mode=0)" was pinned
// as the correct opening sequence.
namespace {

std::string updatesText(const std::vector<FakeSdrPlayApi::UpdateRecord>& v) {
    std::string s;
    for (const FakeSdrPlayApi::UpdateRecord& u : v) {
        char b[80];
        std::snprintf(b, sizeof(b), "[tuner %d reason 0x%08x ext1 0x%08x]",
                      static_cast<int>(u.tuner), u.reason, u.ext1);
        s += b;
    }
    return s;
}

FakeSdrPlayApi::UpdateRecord upd(abi::TunerSelectT tuner, unsigned int reason, unsigned int ext1) {
    return FakeSdrPlayApi::UpdateRecord{tuner, reason, ext1};
}

// Compares and, on a difference, says BOTH sides - CHECK alone prints only the
// expression, and "what did it actually send" is the whole question here.
bool updatesAre(const FakeSdrPlayApi& fake, const std::vector<FakeSdrPlayApi::UpdateRecord>& want) {
    const std::string got = updatesText(fake.updates);
    const std::string exp = updatesText(want);
    if (got != exp) {
        std::printf("  Update calls expected %s\n  Update calls actual   %s\n", exp.c_str(),
                    got.c_str());
    }
    return got == exp;
}

}  // namespace

// Every control the report names, and the rest of the RSPdx family's, on a
// radio whose service reports Tuner_A for it (what the vendor's own clients
// rely on): the exact tuner, reason and extension word of every Update.
void testTheRspDxR2UpdatesNameTunerAWithTheExactReasons() {
    constexpr abi::TunerSelectT A = abi::Tuner_A;
    FakeSdrPlayApi fake;
    fake.addDevice("2406000R2X", abi::kRspDxR2, abi::RspDuoMode_Unknown, abi::Tuner_A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.hardwareVersion() == abi::kRspDxR2);
    // The device is selected as the service listed it: tuner A, no RSPduo mode.
    CHECK(fake.indexStarting("SelectDevice(tuner=1,mode=0)") >= 0);
    CHECK(src.start());

    fake.updates.clear();
    // The report's antenna change, and the other two ports.
    CHECK(src.setAntenna("Antenna B"));
    CHECK(src.setAntenna("Antenna C"));
    CHECK(src.setAntenna("Antenna A"));
    // The report's frequency change.
    CHECK(src.setCenterFrequencyHz(145500000.0));
    // LNA state and IF gain: one reason, two fields.
    CHECK(src.setGainDb("LNA", 27.0));
    CHECK(src.setGainDb("IF", -30.0));
    // A sample-rate change from the 2 MS/s open rate to 8 MS/s: the ADC rate
    // and the bandwidth move, the zero-IF type and the (absent) decimation
    // do not.
    CHECK(src.setSampleRateHz(8000000.0));
    // What starting a decoder asks of the source: a retune and a rate (a
    // decoder preset's 2.4 MS/s is coerced to the nearest supported, 2.048).
    CHECK(src.setCenterFrequencyHz(1090000000.0));
    CHECK(src.setSampleRateHz(2400000.0));
    // And the RSPdx's own switches, which live in the extension word only.
    CHECK(src.setBiasT(true));
    CHECK(src.setRfNotch(true));
    CHECK(src.setDabNotch(true));
    CHECK(src.setHdrMode(true));

    const unsigned int none = static_cast<unsigned int>(abi::Update_None);
    const unsigned int ext0 = static_cast<unsigned int>(abi::Update_Ext1_None);
    const unsigned int frf = static_cast<unsigned int>(abi::Update_Tuner_Frf);
    const unsigned int gr = static_cast<unsigned int>(abi::Update_Tuner_Gr);
    const unsigned int fsBw = static_cast<unsigned int>(abi::Update_Dev_Fs) |
                              static_cast<unsigned int>(abi::Update_Tuner_BwType);
    const unsigned int ant = static_cast<unsigned int>(abi::Update_RspDx_AntennaControl);
    CHECK(updatesAre(
        fake,
        {upd(A, none, ant),  // antenna B
         upd(A, none, ant),  // antenna C
         upd(A, none, ant),  // antenna A
         upd(A, frf, ext0),  // 145.5 MHz
         upd(A, gr, ext0),   // LNA state 27
         upd(A, gr, ext0),   // IF gain reduction 30
         upd(A, fsBw, ext0), // 8 MS/s
         upd(A, frf, ext0),  // 1090 MHz
         upd(A, fsBw, ext0), // 2.048 MS/s
         upd(A, none, static_cast<unsigned int>(abi::Update_RspDx_BiasTControl)),
         upd(A, none, static_cast<unsigned int>(abi::Update_RspDx_RfNotchControl)),
         upd(A, none, static_cast<unsigned int>(abi::Update_RspDx_RfDabNotchControl)),
         upd(A, none, static_cast<unsigned int>(abi::Update_RspDx_HdrEnable)),
         upd(A, none, static_cast<unsigned int>(abi::Update_RspDx_HdrBw))}));
    // The antenna the last change left, written into the RSPdx block and not
    // into another model's.
    CHECK(fake.devParams.rspDxParams.antennaSel == abi::RspDx_ANTENNA_A);
    src.stop();
}

// The same radio when the service lists it with NO tuner (Tuner_Neither) - the
// value this driver used to force. It must still be selected and updated as
// the one tuner an RSPdx-R2 has.
void testASingleTunerRspIsTunerAEvenWhenTheServiceListsNone() {
    constexpr abi::TunerSelectT A = abi::Tuner_A;
    FakeSdrPlayApi fake;
    fake.addDevice("2406000R2X", abi::kRspDxR2, abi::RspDuoMode_Unknown, abi::Tuner_Neither);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(fake.indexStarting("SelectDevice(tuner=1,mode=0)") >= 0);
    CHECK(fake.indexStarting("SelectDevice(tuner=0") < 0);
    CHECK(src.start());
    fake.updates.clear();
    CHECK(src.setAntenna("Antenna B"));
    CHECK(src.setCenterFrequencyHz(145500000.0));
    CHECK(updatesAre(
        fake, {upd(A, static_cast<unsigned int>(abi::Update_None),
                   static_cast<unsigned int>(abi::Update_RspDx_AntennaControl)),
               upd(A, static_cast<unsigned int>(abi::Update_Tuner_Frf),
                   static_cast<unsigned int>(abi::Update_Ext1_None))}));
    src.stop();
}

// Every single-tuner model, not only the one in the report: a retune and an
// LNA change each name Tuner_A. (The RSPduo's tuner is the user's choice and
// is pinned by its own tests.)
void testEverySingleTunerModelUpdatesTunerA() {
    const unsigned char models[] = {abi::kRsp1,  abi::kRsp1A, abi::kRsp1B,
                                    abi::kRsp2,  abi::kRspDx,  abi::kRspDxR2};
    for (unsigned char hw : models) {
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", hw, abi::RspDuoMode_Unknown, abi::Tuner_A);
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        fake.updates.clear();
        CHECK(src.setCenterFrequencyHz(102000000.0));
        CHECK(src.setGainDb("LNA", 1.0));
        CHECK(fake.updates.size() == 2);
        for (const FakeSdrPlayApi::UpdateRecord& u : fake.updates) {
            if (u.tuner != abi::Tuner_A) {
                std::printf("  model hwVer %u sent an Update naming tuner %d\n",
                            static_cast<unsigned>(hw), static_cast<int>(u.tuner));
            }
            CHECK(u.tuner == abi::Tuner_A);
        }
        src.stop();
    }
}

// --- 12. teardown ---------------------------------------------------------

void testStopAndCloseAreBoundedAndIdempotent() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    const unsigned long long strandedBefore = SdrPlaySource::linksStranded();

    const auto t0 = std::chrono::steady_clock::now();
    src.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    // The budget this driver is allowed on a teardown. Measured rather than
    // argued, because elapsed time is the only thing that still fails when the
    // bound is deleted.
    CHECK(stopMs < 500);
    CHECK(!src.running());
    CHECK(fake.called("Uninit"));
    // Nothing was stranded: the fake's Uninit stops the callbacks the way the
    // API's contract says the service's does.
    CHECK(SdrPlaySource::linksStranded() == strandedBefore);

    // Idempotent.
    fake.calls.clear();
    src.stop();
    CHECK(fake.calls.empty());

    src.closeDevice();
    CHECK(!src.isOpen());
    CHECK(fake.called("ReleaseDevice"));
    // The connection to the service is closed when the last user lets go of
    // it, and exactly once.
    CHECK(fake.openCount == 1);
    CHECK(fake.closeCount == 1);

    // Idempotent, and safe on a closed source.
    src.closeDevice();
    CHECK(fake.closeCount == 1);
    CHECK(src.start() == false);
    CHECK(std::string(src.lastError()).find("no SDRplay device is open") != std::string::npos);
}

void testCloseWithoutOpenIsSafe() {
    // The destructor path on a source that never opened: nothing is called and
    // nothing crashes. Deliberately with NO fake, because this must not reach
    // for the process API either - a test that LoadLibrary'd the vendor DLL
    // would be testing this machine, not the driver.
    SdrPlaySource src;
    src.closeDevice();
    CHECK(!src.isOpen());
    CHECK(!src.running());
    CHECK(!src.faulted());
    CHECK(src.read(nullptr, 0) == 0);
}

void testFailuresOnTheOpeningPathUnwind() {
    {   // SelectDevice refused: no session left open, nothing half-selected.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        fake.selectResult = abi::HwVerError;
        SdrPlaySource src;
        CHECK(openOn(src, fake) == false);
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("SelectDevice") != std::string::npos);
        CHECK(fake.openCount == 1);
        CHECK(fake.closeCount == 1);
        // The device lock was RELEASED even though the select failed: a
        // refusal that keeps the API's lock blocks every other application.
        CHECK(fake.called("UnlockDeviceApi"));
    }
    {   // GetDeviceParams refused: the device is RELEASED again rather than
        // left ours with no way to drive it.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        fake.getParamsResult = abi::Fail;
        SdrPlaySource src;
        CHECK(openOn(src, fake) == false);
        CHECK(!src.isOpen());
        CHECK(fake.releaseCount == 1);
        CHECK(fake.closeCount == 1);
    }
    {   // Init refused: open, but not running, and re-startable.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        fake.initResult = abi::HwError;
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start() == false);
        CHECK(!src.running());
        CHECK(std::string(src.lastError()).find("Init failed") != std::string::npos);
        fake.initResult = abi::Success;
        CHECK(src.start());
        CHECK(src.running());
    }
    {   // The service never answers an Update. The parameter is programmed and
        // the driver carries on with a warning rather than blocking the GUI
        // for ever - but it does not spend more than kUpdateWait doing it.
        FakeSdrPlayApi fake;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        fake.autoAck = false;
        SdrPlaySource src;
        CHECK(openOn(src, fake));
        CHECK(src.start());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(src.setCenterFrequencyHz(120000000.0));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms >= 400);
        CHECK(ms < 900);
        CHECK_NEAR(fake.chA.tunerParams.rfFreq.rfHz, 120000000.0, 1.0);
    }
}

// --- 12b. the sentence has to REACH the Source section --------------------

void testSkipReasonReachesTheSourcePanel() {
    // THE ONE INSTRUCTION AN RSP OWNER GETS, AND THE HALF OF IT THAT NEVER
    // ARRIVED. The Source section composes its sentence from the process
    // table's own fields - resolved, and the version the table records - and
    // the table records a version only on a session that SUCCEEDED. A too-old
    // API fails the version gate inside sessionAcquire, so nothing is ever
    // written into api.version, so the panel asked sdrPlayApiAdvice(true,
    // 0.0f) and was answered with silence: the user saw an empty Source
    // section and the "update the API" sentence existed only in the log.
    //
    // This is that exact pair of values, and it is why the accessor below has
    // to exist rather than the panel simply reading harder.
    CHECK(cascade::source::sdrPlayApiAdvice(true, 0.0f).empty());

    {
        FakeSdrPlayApi fake;
        fake.version = 3.05f;
        fake.addDevice("1234567890", abi::kRsp1A);
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());

        // WHAT THE ENUMERATION SKIPPED FOR, kept verbatim. The same string it
        // logs, so the screen and the log cannot describe the same machine
        // differently.
        const std::string skip = cascade::source::sdrPlayLastEnumerationSkip();
        CHECK(skip == cascade::source::sdrPlayApiAdvice(true, 3.05f));
        CHECK(skip.find("3.05") != std::string::npos);
        CHECK(skip.find("3.07") != std::string::npos);
        CHECK(skip.find("sdrplay.com") != std::string::npos);

        // ...AND THROUGH THE RULE THE PANEL ITSELF USES, called with the
        // values scanNative() really has: resolved true, version zero. The
        // version is asserted rather than only the equality, because two
        // empty strings are equal - and an empty panel sentence is precisely
        // the defect this test exists to keep out.
        const std::string shown = cascade::source::sdrPlayPanelAdvice(true, 0.0f, skip);
        CHECK(shown.find("3.05") != std::string::npos);
        CHECK(shown == skip);
    }

    {
        // THE MISSING CASE STILL SAYS WHAT IT ALWAYS SAID. It reached the
        // panel before because sdrPlayApiAdvice answers on `resolved` alone,
        // and it has to go on reaching it by the new route as well.
        abi::Api absent;
        CHECK(cascade::source::enumerateSdrPlayWith(absent).empty());
        const std::string skip = cascade::source::sdrPlayLastEnumerationSkip();
        CHECK(skip == cascade::source::sdrPlayApiAdvice(false, 0.0f));
        CHECK(cascade::source::sdrPlayPanelAdvice(false, 0.0f, skip) == skip);
    }

    {
        // AND AN API THAT WORKED SAYS NOTHING. An enumeration that got as far
        // as the device list clears the reason, so a panel drawn after a good
        // scan has nothing to show - which is the state every machine with a
        // working install is in, and a stale sentence there would send its
        // owner off to reinstall an API that is already fine.
        FakeSdrPlayApi fake;
        fake.version = 3.15f;
        fake.addDevice("1811003EFB", abi::kRsp1A);
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).size() == 1);
        CHECK(cascade::source::sdrPlayLastEnumerationSkip().empty());
        CHECK(cascade::source::sdrPlayPanelAdvice(true, 3.15f, std::string()).empty());
    }
}

// --- 12c. a service that stops answering ----------------------------------
//
// TWO HANG REPORTS, ONE RSP1A, API 3.15 (0.96.1). The log showed the device
// opened and then every call answering sdrplay_api_ServiceNotResponding (14) -
// retune, LNA, AGC, Uninit - and then a SCAN from the Source section, on the
// GUI thread, going into enumerateSdrPlayWith and never coming back. Both
// halves of that are covered below: the enumeration must be bounded and must
// say why, and the open device must declare itself dead instead of being
// hammered for fifty seconds (the health line recorded 1910 timeouts).

void testServiceNotRespondingMakesTheDeviceDead() {
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());
    CHECK(!src.faulted());

    // The service goes away with the radio still open. A retune is what the
    // user's next click produces, and it is the call the report names first.
    fake.updateResult = abi::ServiceNotResponding;
    CHECK(src.setCenterFrequencyHz(101100000.0) == false);

    // NOT a refused request: a dead receiver. This is what lets Pipeline's
    // source thread latch the fault and stop, which is the whole difference
    // between a released radio and 1910 timeouts.
    CHECK(src.faulted());
    CHECK(src.deviceDead());
    CHECK(src.faultedWhile() == "retune");
    CHECK(std::string(src.lastError()).find("service stopped answering") != std::string::npos);

    // ...and an ordinary refusal is still an ordinary refusal. Without this
    // the check above would pass for any failing Update at all.
    // The fake is declared BEFORE the source that holds a pointer into it: the
    // source's destructor closes the device through that table, and locals die
    // in reverse order of declaration, so a fake declared after it was already
    // gone (AddressSanitizer: stack-use-after-scope in sessionIsLost).
    FakeSdrPlayApi fake2;
    SdrPlaySource other;
    fake2.addDevice("1811003EFC", abi::kRsp1A);
    CHECK(openOn(other, fake2));
    CHECK(other.start());
    fake2.updateResult = abi::Fail;
    CHECK(other.setCenterFrequencyHz(101100000.0) == false);
    CHECK(!other.faulted());
    CHECK(!other.deviceDead());
}

// --- 12d. a service that stops answering a CONTROL -------------------------
//
// A THIRD HANG REPORT, THE SAME RSP1A, API 3.09 (0.96.2). 0.96.1 bounded the
// SCAN and made the device dead on the first ServiceNotResponding, and both
// of those held: the log shows "opened SDRplay RSP1A" at 16:03:14 and, five
// seconds later, "source: SDRplay retune failed - sdrplay_api_ServiceNotResponding
// (14)" followed by the released-radio line. The hang was filed at 16:03:20 -
// i.e. DURING those five seconds, not after them. sdrplay_api_Update itself
// took about five seconds to answer, on the GUI thread, which is the hang
// watchdog's whole threshold; by the time the watchdog walked the stack the
// call had returned and the GUI thread was in the next frame's SwapBuffers,
// which is what the report shows and why it reads like a graphics fault.
//
// So the vendor call every LIVE CONTROL makes is bounded the way the scan's
// is: run on a worker, waited on for kControlWait, and ABANDONED on expiry.
// The service is wedged either way; what changes is that the window does not
// go with it.

// --- 12c. a control the service answers LATE -------------------------------
//
// THE 0.99.46 RSPdx-R2 REPORT (API 3.15). An LNA change was abandoned at
// kControlWait and the radio refused for the whole process on the spot:
//
//   13:02:03.648 SDRplay API session lost - a control was abandoned inside
//                sdrplay_api_Update; ...
//   13:02:03.648 SDRplay LNA state change abandoned - the service did not
//                answer within 1000 ms; the radio is released
//
// SDRplay API Specification 3.15 gives sdrplay_api_Update NO time bound, and
// says of it (3.17, p27): "If required it will stop the stream, change the
// values and then start the stream again, otherwise it will make the changes
// directly." The one second was this driver's own number, justified by a
// comment that said a healthy Update "only QUEUES the request" - which the
// specification does not say. And the report's own stream-health line shows
// the service still delivering 1.28 s after that Update was issued, i.e. still
// working on it when FoxSDR had already given the radio up.
//
// So the wait on the caller's thread stays one second (the GUI's budget), and
// what changes is what happens next: the worker is left to answer for up to
// kControlGrace, nothing of ours enters the DLL meanwhile, and a SUCCESSFUL
// late answer gives the radio back.

namespace {

bool logHas(const std::string& needle) {
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

// Waits for every call the fake is inside to come out - the late worker's
// Update returning, which is what the driver is waiting to hear about.
bool waitUntilOutOfUpdate(FakeSdrPlayApi& fake, int ms) {
    for (int i = 0; i < ms / 5 && fake.concurrentInUpdate.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fake.concurrentInUpdate.load() == 0;
}

long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

}  // namespace

void testALateAnswerGivesTheRadioBack() {
    // On the heap and never destroyed, like every test that leaves a worker
    // inside its fake past the setter's return.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("2406000R2X", abi::kRspDxR2);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    cascade::core::DiagLog::instance().resetForTest();

    // The service takes one and a half seconds over the LNA change, then
    // takes it.
    fake->updateDelayMs.store(1500);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(src->setGainDb("LNA", 5.0) == false);
    const long long waited = msSince(t0);
    // The caller's wait is still kControlWait - the GUI's budget is unchanged.
    CHECK(waited >= 900);
    CHECK(waited < 1450);

    // NOT GIVEN UP. The receiver is not declared dead while the service may
    // still answer...
    CHECK(!src->faulted());
    CHECK(!src->deviceDead());
    CHECK(std::string(src->lastError()).find(cascade::source::sdrPlayControlPendingSentence()) !=
          std::string::npos);
    // ...the block keeps the request the worker is carrying...
    CHECK(fake->chA.tunerParams.gain.LNAstate == 5);
    // ...and nothing else of ours enters the DLL meanwhile: a second control
    // is held, not sent, and costs the caller nothing.
    const std::size_t callsBefore = fake->callCount();
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(src->setCenterFrequencyHz(101100000.0) == false);
    CHECK(msSince(t1) < 250);
    CHECK(fake->callCount() == callsBefore);
    CHECK(std::string(src->lastError()).find(cascade::source::sdrPlayControlPendingSentence()) !=
          std::string::npos);
    // A scan is held too, without a single vendor call.
    CHECK(cascade::source::enumerateSdrPlayWith(fake->table).empty());
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          cascade::source::sdrPlayControlPendingSentence());
    CHECK(fake->callCount() == callsBefore);

    // The service answers.
    CHECK(waitUntilOutOfUpdate(*fake, 3000));
    // The READER notices, with no control needed to prompt it.
    std::complex<float> buf[64];
    (void) src->read(buf, 64);
    CHECK(!src->faulted());
    CHECK(!src->deviceDead());
    // The readback is where the radio now is.
    CHECK(src->gainDb("LNA") == 5.0);
    CHECK(logHas("SDRplay LNA state change answered after"));
    CHECK(!logHas("SDRplay API session lost"));

    // THE RADIO IS USABLE AGAIN: a new control is sent and taken...
    fake->updateDelayMs.store(0);
    fake->calls.clear();
    CHECK(src->setCenterFrequencyHz(101100000.0));
    CHECK((fake->calls == std::vector<std::string>{
                              FakeSdrPlayApi::updateCall(abi::Update_Tuner_Frf, abi::Update_Ext1_None)}));
    // ...a scan goes to the service again...
    CHECK(cascade::source::enumerateSdrPlayWith(fake->table).size() == 1);
    // ...and a stop is a real stop, with the Uninit that lets the next open
    // find the radio free.
    fake->calls.clear();
    src->stop();
    CHECK(fake->called("Uninit"));
    src->closeDevice();
    CHECK(fake->called("ReleaseDevice"));
}

void testALateRefusalPutsTheBlockBackAndALateDeadServiceIsDead() {
    {   // A LATE REFUSAL: the service is alive and said no. The radio stays
        // usable and the block goes back to where the radio is.
        FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
        fake->addDevice("2406000R2X", abi::kRspDxR2);
        SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
        CHECK(openOn(*src, *fake));
        CHECK(src->start());
        fake->updateDelayMs.store(1500);
        fake->updateResult = abi::GainUpdateError;
        CHECK(src->setGainDb("LNA", 5.0) == false);
        CHECK(!src->faulted());
        CHECK(waitUntilOutOfUpdate(*fake, 3000));
        fake->updateResult = abi::Success;
        fake->updateDelayMs.store(0);
        std::complex<float> buf[16];
        (void) src->read(buf, 16);
        CHECK(!src->faulted());
        CHECK(fake->chA.tunerParams.gain.LNAstate == 0);
        CHECK(src->gainDb("LNA") == 0.0);
        CHECK(std::string(src->lastError()).find("LNA state change failed") != std::string::npos);
        fake->calls.clear();
        CHECK(src->setGainDb("LNA", 3.0));
        CHECK((fake->calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                                  abi::Update_Tuner_Gr, abi::Update_Ext1_None)}));
        src->stop();
        src->closeDevice();
    }
    {   // A LATE sdrplay_api_ServiceNotResponding: the service took its time
        // to say it is gone, and gone it is - named as such, not as a hang.
        FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
        fake->addDevice("2406000R2Y", abi::kRspDxR2);
        SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
        CHECK(openOn(*src, *fake));
        CHECK(src->start());
        fake->updateDelayMs.store(1500);
        fake->updateResult = abi::ServiceNotResponding;
        CHECK(src->setAntenna("Antenna B") == false);
        CHECK(!src->faulted());
        CHECK(waitUntilOutOfUpdate(*fake, 3000));
        std::complex<float> buf[16];
        (void) src->read(buf, 16);
        CHECK(src->faulted());
        CHECK(src->deviceDead());
        CHECK(std::string(src->lastError()).find("stopped answering") != std::string::npos);
        CHECK(cascade::source::enumerateSdrPlayWith(fake->table).empty());
        CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
              cascade::source::sdrPlaySessionLostSentence());
        fake->calls.clear();
        src->stop();
        CHECK(!fake->called("Uninit"));
    }
}

void testAControlThatNeverAnswersIsGivenUpAfterTheGrace() {
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("2406000R2Z", abi::kRspDxR2);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    // Short, so the test does not take ten seconds; the rule is the same.
    src->setControlGraceForTest(std::chrono::milliseconds(1500));
    // Shorter still, to prove the stall rule stands aside while a control is
    // waiting - the specification says an Update may stop and restart the
    // stream, so silence then is not evidence of a dead service.
    src->setStreamStallLimitForTest(std::chrono::milliseconds(200));
    CHECK(src->start());

    fake->hangInUpdate.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(src->setAntenna("Antenna C") == false);
    CHECK(msSince(t0) < 1450);
    CHECK(!src->faulted());

    // Inside the grace, with the stream silent: still not given up.
    std::complex<float> buf[16];
    while (msSince(t0) < 1300) { (void) src->read(buf, 16); }
    CHECK(!src->faulted());

    // Past it: given up for good, through the reader, with the hang sentence.
    while (msSince(t0) < 1700) { (void) src->read(buf, 16); }
    CHECK(src->faulted());
    CHECK(src->deviceDead());
    CHECK(src->faultedWhile() == "antenna change");
    CHECK(std::string(src->lastError()).find(cascade::source::sdrPlayControlHungSentence()) !=
          std::string::npos);
    CHECK(cascade::source::enumerateSdrPlayWith(fake->table).empty());
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          cascade::source::sdrPlaySessionLostSentence());
    fake->calls.clear();
    src->stop();
    CHECK(!fake->called("Uninit"));

    fake->releaseUpdateHang.store(true);
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());
    // An answer after the give-up changes nothing: the radio stays given up.
    (void) src->read(buf, 16);
    CHECK(src->deviceDead());
}

void testAStopWhileAControlIsWaitingNeverEntersTheVendorDll() {
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("2406000R2W", abi::kRspDxR2);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    fake->hangInUpdate.store(true);
    CHECK(src->setGainDb("LNA", 7.0) == false);
    CHECK(!src->faulted());

    // The user presses STOP inside the grace. The worker is still in the DLL,
    // so the Uninit that would queue behind it is not made (the 0.96.4 hang
    // report) and the radio is given up for the process.
    const unsigned long long strandedBefore = SdrPlaySource::linksStranded();
    fake->calls.clear();
    const auto t0 = std::chrono::steady_clock::now();
    src->stop();
    CHECK(msSince(t0) < 250);
    CHECK(!fake->called("Uninit"));
    CHECK(SdrPlaySource::linksStranded() == strandedBefore + 1);
    CHECK(cascade::source::enumerateSdrPlayWith(fake->table).empty());
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          cascade::source::sdrPlaySessionLostSentence());
    src->closeDevice();
    CHECK(!fake->called("ReleaseDevice"));

    fake->releaseUpdateHang.store(true);
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());
}

void testAWedgedControlIsAbandonedAndTheDeviceIsDead() {
    // THE FAKE AND THE SOURCE ARE ON THE HEAP AND NEITHER IS DESTROYED, for
    // the reason the enumeration test below gives: a worker is abandoned
    // inside the fake's Update and the driver has given up on ever hearing
    // from it again. It is released and waited for at the end, which is as
    // close to safe as an abandonment gets.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    CHECK(!src->faulted());

    // The service wedges with the radio open and streaming. The user's next
    // click is a retune - the call the report names.
    // THIS TEST IS ABOUT THE RADIO ONCE IT HAS BEEN GIVEN UP FOR GOOD, so the
    // grace for a late answer (kControlGrace) is taken away and the give-up
    // comes at kControlWait, as it did before the grace existed.
    src->setControlGraceForTest(std::chrono::milliseconds(0));
    fake->hangInUpdate.store(true);

    // THE RETUNE RUNS ON ITS OWN THREAD so that this test can still report
    // rather than hang when the bound is missing. That thread stands in for
    // the GUI thread exactly: it is the one that calls the setter and the one
    // whose elapsed time the watchdog would have judged.
    std::atomic<bool> returned{false};
    std::atomic<bool> retuned{true};
    std::atomic<long long> elapsedMs{-1};
    std::thread caller([&]() {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = src->setCenterFrequencyHz(101100000.0);
        elapsedMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
        retuned.store(ok);
        returned.store(true);
    });

    // Generous, and deliberately shorter than the vendor's own five seconds:
    // what is being ruled out is a call that does not come back at all.
    for (int i = 0; i < 400 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // THE DEFECT, IN ONE LINE. Before the bound this is the check that fails,
    // because the setter is still inside sdrplay_api_Update and will be until
    // the service answers - which in the field was five seconds and in
    // principle is never.
    CHECK(returned.load());
    CHECK(fake->insideUpdate.load());

    if (returned.load()) {
        const long long ms = elapsedMs.load();
        // Refused, not silently accepted: the radio is not where the caller
        // asked for and must not be reported as if it were.
        CHECK(retuned.load() == false);
        // It waited the bound rather than returning for some unrelated
        // reason...
        CHECK(ms >= 900);
        // ...and it came back inside it, with room for a machine that is
        // building and testing in parallel.
        CHECK(ms < 3000);
        // AND INSIDE THE THRESHOLD THAT FILED THE REPORT. This is the whole
        // fault expressed as arithmetic; if kControlWait ever grows past the
        // watchdog's frame threshold, this is what goes red.
        CHECK(ms < static_cast<long long>(cascade::core::HangWatchdog::kDefaultThresholdMs));
        if (ms >= 3000) { std::printf("     retune took %lld ms\n", ms); }

        // A DEAD RECEIVER, through the same path a returned
        // ServiceNotResponding takes - which is what lets Pipeline's source
        // thread latch the fault and stop instead of reading a service that
        // is gone.
        CHECK(src->faulted());
        CHECK(src->deviceDead());
        CHECK(src->faultedWhile() == "retune");
        CHECK(std::string(src->lastError())
                  .find(cascade::source::sdrPlayControlHungSentence()) != std::string::npos);

        // ...AND NOTHING TOUCHES THE API AGAIN FOR THIS DEVICE. There is a
        // thread of ours parked inside the vendor DLL; a second control would
        // park another one, and the panel's sliders are not short of clicks.
        const std::size_t callsBefore = fake->callCount();
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(src->setGainDb("LNA", 2.0) == false);
        CHECK(src->setSampleRateHz(6000000.0) == false);
        const long long deadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t1)
                                     .count();
        CHECK(fake->callCount() == callsBefore);
        CHECK(deadMs < 250);
    } else {
        std::printf(
            "     the retune never returned - the checks that depend on it were not run\n");
    }

    // Let the abandoned worker leave before this process does.
    fake->releaseUpdateHang.store(true);
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());
    // The caller thread is this test's own, not the driver's: it is joinable
    // once the vendor call it may still be inside has been released.
    for (int i = 0; i < 500 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (caller.joinable()) { caller.join(); }
}

// --- 12e. the teardown that follows an abandoned control -------------------
//
// A FOURTH HANG REPORT (0.96.4, RSP1B, API 3.15, Windows 10.0.22631) - and
// the first one where the frozen call is this driver's own TEARDOWN rather
// than a control. Its log tail is 0.96.3 working exactly as written:
//
//   warn source: SDRplay retune abandoned - the service did not answer
//                within 1000 ms; the radio is released
//   info source: SDRplay controls are refused for this device from here -
//                a worker is still inside sdrplay_api_Update
//
// and its GUI thread, symbolised against the 0.96.4 map, is
//
//   ntdll -> KERNELBASE -> sdrplay_api.dll -> sdrplay_api.dll
//         -> SdrPlaySource::stopStreamingLocked +129
//         -> SdrPlaySource::stop +51
//         -> Pipeline::quiesceSourceThreadLocked +58 -> Pipeline::start +207
//         -> AppWindow::drawToolbar +603 -> drawUi -> run -> main
//
// The fault latched by the abandonment is what STOPS the pipeline, and
// stopping the pipeline is what calls stop() - on the GUI thread, straight
// into the sdrplay_api_Uninit the abandoned worker's device is already
// holding. 0.96.3 bounded the control and handed the window to the teardown
// behind it.
//
// So the rule this pins: once a worker has been abandoned inside the vendor
// DLL, or the service has declared itself gone, NOTHING of ours enters that
// DLL for this device again - not Uninit, not ReleaseDevice, not Close. The
// handle is left to the thread that is still in there, which is the only one
// that could ever make another call safe, and the teardown returns bounded.

void testATeardownAfterAnAbandonedControlNeverEntersTheVendorDll() {
    // THE FAKE AND THE SOURCE ARE ON THE HEAP AND NEITHER IS DESTROYED, for
    // the reason the test above gives: a worker is abandoned inside the fake's
    // Update, and destroying the object it is standing in would be the one
    // thing an abandonment must never do.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());

    // The service wedges and the user's retune is abandoned inside it: the
    // exact state the report's last two log lines describe.
    // THIS TEST IS ABOUT THE RADIO ONCE IT HAS BEEN GIVEN UP FOR GOOD, so the
    // grace for a late answer (kControlGrace) is taken away and the give-up
    // comes at kControlWait, as it did before the grace existed.
    src->setControlGraceForTest(std::chrono::milliseconds(0));
    fake->hangInUpdate.store(true);
    std::atomic<bool> retuneReturned{false};
    std::thread caller([&]() {
        src->setCenterFrequencyHz(101100000.0);
        retuneReturned.store(true);
    });
    for (int i = 0; i < 400 && !retuneReturned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(retuneReturned.load());
    if (caller.joinable()) { caller.join(); }
    CHECK(src->deviceDead());
    // The worker is STILL IN THERE, which is the whole premise: everything
    // below asks what the teardown does about a device it cannot enter.
    CHECK(fake->insideUpdate.load());
    CHECK(!fake->leftUpdate.load());

    // THE SERVICE ANSWERS EVENTUALLY, so that a teardown which does go into
    // the DLL comes back and this test REPORTS rather than hangs. The field's
    // user got no such rescue - the report was filed while the frame was still
    // stuck - so this bound is generous to the defect, not to the fix.
    std::thread rescue([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        fake->releaseUpdateHang.store(true);
    });

    const unsigned long long strandedBefore = SdrPlaySource::linksStranded();
    const std::size_t callsBefore = fake->callCount();

    // THE TWO CALLS THE REPORT'S STACK IS INSIDE, on this test's own thread -
    // which stands in for the GUI thread exactly as the sibling test's caller
    // does.
    const auto t0 = std::chrono::steady_clock::now();
    src->stop();
    const long long stopMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    const auto t1 = std::chrono::steady_clock::now();
    src->closeDevice();
    const long long closeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1)
            .count();
    std::printf("     stop %lld ms, close %lld ms after an abandoned control\n", stopMs, closeMs);

    // 1. NOTHING OF OURS WENT BACK INTO THE VENDOR DLL. Asserted as "did not
    //    enter" and not only as elapsed time, because a call that is quick
    //    today because the test released the wedge is still the call that
    //    froze the user's window.
    CHECK(!fake->uninitEnteredWhileWedged.load());
    CHECK(!fake->releaseEnteredWhileWedged.load());
    CHECK(!fake->called("Uninit"));
    CHECK(fake->releaseCount == 0);
    CHECK(fake->closeCount == 0);
    CHECK(fake->callCount() == callsBefore);

    // 2. AND BOTH CAME BACK WELL INSIDE THE THRESHOLD THAT FILED THE REPORT.
    //    This is the fault expressed as arithmetic: the teardown is on the
    //    same thread and in the same frame as the control that preceded it.
    CHECK(stopMs < 500);
    CHECK(closeMs < 500);
    CHECK(stopMs + closeMs <
          static_cast<long long>(cascade::core::HangWatchdog::kDefaultThresholdMs));

    // 3. WHAT THE CALLER IS LEFT WITH. Stopped and closed as far as this
    //    process is concerned...
    CHECK(!src->running());
    CHECK(!src->isOpen());
    // ...and the Link is STRANDED rather than freed, because we never called
    // Uninit and therefore nothing has told the service to stop calling our
    // callbacks. There is no moment at which freeing it would be safe.
    CHECK(SdrPlaySource::linksStranded() == strandedBefore + 1);

    // Let the abandoned worker leave before this process does.
    rescue.join();
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());

    // AND THE SAME RULE WITH NO ABANDONED WORKER AT ALL. A service that
    // ANSWERS sdrplay_api_ServiceNotResponding has said the thing holding the
    // USB handle is gone; a teardown that then calls Uninit, ReleaseDevice and
    // Close is three more calls into it, each of which the 0.96.4 stack shows
    // can block for as long as it likes. Nothing here is abandoned, so this
    // fake can live and die on the stack.
    FakeSdrPlayApi gone;
    gone.addDevice("1811003EFC", abi::kRsp1A);
    SdrPlaySource dead;
    CHECK(openOn(dead, gone));
    CHECK(dead.start());
    gone.updateResult = abi::ServiceNotResponding;
    CHECK(dead.setCenterFrequencyHz(101100000.0) == false);
    CHECK(dead.deviceDead());
    gone.calls.clear();
    dead.stop();
    dead.closeDevice();
    CHECK(!gone.called("Uninit"));
    CHECK(!gone.called("ReleaseDevice"));
    CHECK(!gone.called("Close"));
    CHECK(!dead.isOpen());
    CHECK(!dead.running());

    // ...WHILE A DEVICE THAT WAS MERELY UNPLUGGED IS TORN DOWN NORMALLY. The
    // radio is gone and the service is fine, so Uninit and ReleaseDevice are
    // the calls that let the next one be opened; skipping them here would
    // trade a hang nobody has for a radio that cannot be re-plugged.
    FakeSdrPlayApi pulled;
    pulled.addDevice("1811003EFD", abi::kRsp1A);
    SdrPlaySource unplugged;
    CHECK(openOn(unplugged, pulled));
    CHECK(unplugged.start());
    pulled.fireDeviceRemoved();
    CHECK(unplugged.deviceDead());
    unplugged.stop();
    unplugged.closeDevice();
    CHECK(pulled.called("Uninit"));
    CHECK(pulled.called("ReleaseDevice"));
    CHECK(pulled.closeCount == 1);
}

// --- 12f. stop()'s OWN Uninit is a THIRD way the service goes quiet --------
//
// THE 0.97.1 HANG REPORT (RSPdx, Windows 10.0.28000), and a third shape after
// 12c's dead-on-a-control and 12e's abandoned-control-then-teardown. Its log
// tail is
//
//   warn source: SDRplay Uninit failed - sdrplay_api_ServiceNotResponding (14)
//   info source: stream health - reads 10, with samples 10, timeouts 2012, ...
//   warn source: SDRplay start failed - SDRplay Init failed:
//                sdrplay_api_AlreadyInitialised (9)
//   warn source: SDRplay enumeration abandoned - the SDRplay service did not
//                answer within 3 s - restart the SDRplay API service
//   info source: SDRplay scans are held off for 60 s
//   info source: closing SDRplay RSPdx before opening another device
//
// and the GUI thread, symbolised against the 0.97.0 map, is
//
//   ntdll -> KERNELBASE -> sdrplay_api.dll -> sdrplay_api.dll
//         -> SdrPlaySource::closeDevice +189
//         -> SdrPlaySource::~SdrPlaySource -> `scalar deleting destructor'
//         -> Pipeline::setSource -> AppWindow::selectSource
//         -> AppWindow::drawSourceSection -> drawMenuColumn -> drawUi -> run
//         -> main
//
// stopStreamingLocked() had already set initialised_ = false by the time
// closeDevice() ran (the log's Uninit failure is stop()'s own, minutes
// earlier), so closeDevice() skipped stopStreamingLocked() entirely and went
// straight to its OWN unguarded call: sdrplay_api_ReleaseDevice, gated only by
// vendorUnreachableLocked(). That gate was false, because nothing before this
// fix ever told it stop()'s Uninit had already heard the service was gone -
// noteIfServiceDead was wired to updateLocked's failures only. The user
// picking a different source in the Source combo is what destroys the old one
// and calls closeDevice(); the log's last line is that exact click.

void testStopsOwnUninitGoingServiceNotRespondingKeepsCloseDeviceOffTheVendorDll() {
    // THE FAKE AND THE SOURCE ARE ON THE HEAP AND NEITHER IS DESTROYED before
    // the hang is released, for the same reason the sibling tests give: this
    // test proves the defect by letting the wedge actually run, on a thread
    // this test owns, so a build without the fix REPORTS rather than hangs
    // the whole suite.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1810012345", abi::kRspDx);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    CHECK(!src->faulted());

    // stop()'s OWN Uninit answers the service-is-gone error - the report's
    // first log line, and a DIFFERENT call than the one 12c/12d/12e already
    // cover (those are updateLocked's Update, not stopStreamingLocked's
    // Uninit).
    fake->uninitResult = abi::ServiceNotResponding;
    src->stop();
    CHECK(!src->running());

    // ...AND THE SERVICE STAYS WEDGED FOR THE NEXT CALL IN, which in the
    // field was closeDevice()'s ReleaseDevice a few log lines later (a start()
    // and an enumeration were refused/abandoned in between; neither touches
    // this device's ReleaseDevice, so this test skips straight to the call
    // the report's stack is actually inside).
    fake->hangInReleaseDevice.store(true);

    std::atomic<bool> returned{false};
    std::atomic<long long> elapsedMs{-1};
    std::thread caller([&]() {
        const auto t0 = std::chrono::steady_clock::now();
        src->closeDevice();
        elapsedMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
        returned.store(true);
    });

    // Generous, and deliberately shorter than "forever": what is being ruled
    // out is a call that does not come back at all.
    for (int i = 0; i < 400 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // THE DEFECT, IN ONE LINE. Before the fix this is the check that fails:
    // closeDevice() is still inside sdrplay_api_ReleaseDevice, because
    // vendorUnreachableLocked() never learned the service was already gone.
    CHECK(returned.load());

    if (returned.load()) {
        const long long ms = elapsedMs.load();
        CHECK(ms < static_cast<long long>(cascade::core::HangWatchdog::kDefaultThresholdMs));
        if (ms >= static_cast<long long>(cascade::core::HangWatchdog::kDefaultThresholdMs)) {
            std::printf("     closeDevice() took %lld ms\n", ms);
        }
        // FIXED THE RIGHT WAY, NOT BY LUCK: closeDevice() never entered
        // ReleaseDevice at all, because stop()'s Uninit failure now marks the
        // service gone the same way a control's failure already does -
        // vendorUnreachableLocked() correctly skipped the call rather than
        // happening to win a race against the fake's hang.
        CHECK(!fake->insideReleaseDevice.load());
        CHECK(!fake->called("ReleaseDevice"));
        CHECK(src->deviceDead());
        CHECK(std::string(src->lastError()).find("service stopped answering") !=
              std::string::npos);
        CHECK(!src->isOpen());
    } else {
        std::printf(
            "     closeDevice() never returned - the checks that depend on it were not run\n");
    }

    // Let a worker that DID go in leave before this process does (only
    // reachable on the unfixed code path, where insideReleaseDevice went
    // true).
    fake->releaseReleaseDeviceHang.store(true);
    for (int i = 0; i < 500 && fake->insideReleaseDevice.load() && !fake->leftReleaseDevice.load();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int i = 0; i < 500 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (caller.joinable()) { caller.join(); }
}

// THE 0.97.1 CRASH REPORT (2026-09-17, an RSP1 whose API service kept going
// quiet): "access violation" inside VCRUNTIME140 (memcpy), called from
// SdrPlaySource::streamCallbackA, called from sdrplay_api.dll, on the vendor's
// own thread. Its log is the whole story in four lines:
//
//   SDRplay Uninit failed - sdrplay_api_ServiceNotResponding (14)
//   SDRplay start failed - SDRplay Init failed: sdrplay_api_AlreadyInitialised (9)
//   closing Mirics MSi2500 before opening another device
//   SDRplay closed without ReleaseDevice ...
//
// A REFUSED Uninit has not stopped the stream - the second line is the vendor
// library saying so in its own words - so the library still holds our callback
// and the address of our Link. stop() used to strand the Link only when a
// callback happened to be INSIDE it at that instant; otherwise the Link died
// with the source when the device was closed, and the next block the service
// delivered was copied into a ring that had been freed.
//
// What is asserted is the accounting (the Link was stranded), because the
// defect itself is a use-after-free and a test that provokes one proves
// nothing reliably: it may crash, or may quietly write into memory the
// allocator has not reused yet. Only once the Link is known to be stranded are
// blocks delivered through the callback the fake still holds, which is then a
// defined thing to do.
void testARefusedUninitStrandsTheLinkBecauseTheServiceStillHoldsTheCallback() {
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1810012345", abi::kRsp1);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    CHECK(!src->faulted());

    const unsigned long long strandedBefore = SdrPlaySource::linksStranded();

    // The service refuses the stop. It answered, quickly, and no callback is
    // inside us at this moment - the case the old accounting called safe.
    fake->uninitResult = abi::ServiceNotResponding;
    src->stop();
    CHECK(!src->running());

    // THE DEFECT, IN ONE LINE: the stream was never stopped, so the Link must
    // outlive the source. Before the fix this count does not move.
    const bool stranded = SdrPlaySource::linksStranded() == strandedBefore + 1;
    CHECK(stranded);

    // The field order: the device is closed and the source goes away.
    src->closeDevice();
    delete src;

    // ...AND THE SERVICE, WHICH WAS NEVER STOPPED, DELIVERS ANOTHER BLOCK.
    if (stranded) {
        const short xi[4] = {100, 200, 300, 400};
        const short xq[4] = {-100, -200, -300, -400};
        for (int i = 0; i < 64; ++i) { fake->pushSamples(xi, xq, 4); }
        // Still here: the blocks landed in a Link that is alive, and were
        // dropped, because a stopped source accepts nothing.
        CHECK(true);
    } else {
        std::printf("     SKIPPED: delivering a block after the source was freed - the Link was "
                    "not stranded, so that delivery would be the use-after-free itself\n");
    }
}

// --- 12g. a LOST session is never entered again, by anything --------------
//
// THE 0.99.27 CRASH REPORT (2026-09-24, an RSP1 on API 3.15, Windows
// 10.0.26200): "access violation" inside VCRUNTIME140, called from
// sdrplay_api.dll +7098, called from enumerateSdrPlayVendor +445 on the
// enumeration worker. Disassembled against the shipped build (id ...E174),
// +445 is the return from `call [Api+0x50]` - sdrplay_api_GetDevices, handed
// the worker's own 16 x 96-byte array - and the 0.95.0 hang inside the same
// vendor function parked at +6914, its wait for the service. So the copy that
// faulted ran after the service's wait came back, inside the vendor's code.
//
// The log is what this driver did before that call:
//
//   15:12:35 SDRplay retune abandoned - the service did not answer within 1000 ms
//   15:13:28 SDRplay stopped without Uninit - ... this process cannot use it again
//   15:13:48, 15:16:15, 15:20:41 SDRplay enumeration abandoned (each followed
//            ~17 s later by "GetDevices failed - ServiceNotResponding (14)")
//   15:18:18 SDRplay closed without ReleaseDevice (the session is ORPHANED)
//   15:18:38 SDRplay open failed - GetDevices failed: ServiceNotResponding
//   ...and ~15:22 the next scan's GetDevices is the crash.
//
// The file header already says the session is "finished until FoxSDR is
// restarted" once a worker has been abandoned inside the DLL or the service
// has declared itself gone - but only the DEVICE that saw it obeyed. The scan
// and every later open() went straight back through the same process-wide
// session, which can never be closed again (closing it is one more unbounded
// call into a DLL still holding our thread). This pins the process-wide rule:
// once the session is lost, a scan and an open() refuse WITHOUT entering the
// vendor table at all.
void testALostSessionIsNeverEnteredAgainByAScanOrAnOpen() {
    // THE FAKE AND THE SOURCE ARE ON THE HEAP AND NEITHER IS DESTROYED, for the
    // reason 12d gives: a worker is abandoned inside the fake's Update.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1706012345", abi::kRsp1);
    SdrPlaySource* src = cascade::core::leakOnPurpose(new SdrPlaySource());
    CHECK(openOn(*src, *fake));
    CHECK(src->start());

    // 15:12:35 - the retune is abandoned inside a wedged service.
    // THIS TEST IS ABOUT THE RADIO ONCE IT HAS BEEN GIVEN UP FOR GOOD, so the
    // grace for a late answer (kControlGrace) is taken away and the give-up
    // comes at kControlWait, as it did before the grace existed.
    src->setControlGraceForTest(std::chrono::milliseconds(0));
    fake->hangInUpdate.store(true);
    std::atomic<bool> retuneReturned{false};
    std::thread caller([&]() {
        src->setCenterFrequencyHz(14230000.0);
        retuneReturned.store(true);
    });
    for (int i = 0; i < 400 && !retuneReturned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(retuneReturned.load());
    if (caller.joinable()) { caller.join(); }
    CHECK(src->deviceDead());
    CHECK(fake->insideUpdate.load());

    // 15:13:28 and 15:18:18 - stopped without Uninit, closed without
    // ReleaseDevice, and the session orphaned rather than closed.
    src->stop();
    src->closeDevice();
    CHECK(fake->closeCount == 0);

    // Minutes later: the scan hold-off (if any) has long run out.
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();

    // THE CRASHING CALL. A scan must list nothing and enter nothing.
    const std::size_t callsBefore = fake->callCount();
    const int openBefore = fake->openCount;
    const int getDevicesBefore = fake->countStarting("GetDevices");
    const std::vector<cascade::source::NativeDeviceInfo> devs =
        cascade::source::enumerateSdrPlayWith(fake->table);
    CHECK(devs.empty());
    // THE DEFECT, IN ONE LINE: before the fix the scan's worker went through
    // LockDeviceApi and GetDevices on the orphaned session.
    CHECK(fake->countStarting("GetDevices") == getDevicesBefore);
    CHECK(fake->callCount() == callsBefore);
    CHECK(fake->openCount == openBefore);
    if (fake->callCount() != callsBefore) {
        std::printf("     the scan entered the vendor table: %s\n", fake->joined().c_str());
    }
    // The panel says why, verbatim - and names BOTH restarts, because unlike
    // the hold-off this never expires on its own.
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          std::string(cascade::source::sdrPlaySessionLostSentence()));
    CHECK(std::string(cascade::source::sdrPlaySessionLostSentence()).find("restart FoxSDR") !=
          std::string::npos);
    // ...and it stays that way when the scan is asked again, hold-off or not.
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();
    CHECK(cascade::source::enumerateSdrPlayWith(fake->table).empty());
    CHECK(fake->callCount() == callsBefore);

    // 15:18:38 - and an open() from anywhere else (the patch page's radio)
    // must refuse the same way, without a single vendor call.
    SdrPlaySource* other = cascade::core::leakOnPurpose(new SdrPlaySource());
    const std::size_t callsBeforeOpen = fake->callCount();
    const bool reopened = openOn(*other, *fake);
    CHECK(!reopened);
    CHECK(fake->callCount() == callsBeforeOpen);
    if (fake->callCount() != callsBeforeOpen) {
        std::printf("     the open entered the vendor table: %s\n", fake->joined().c_str());
    }
    CHECK(std::string(other->lastError()).find(cascade::source::sdrPlaySessionLostSentence()) !=
          std::string::npos);

    // Let the abandoned worker leave before this process does - and only then
    // tear down anything the unfixed code may have opened, because its
    // ReleaseDevice would otherwise queue behind the wedged Update.
    fake->releaseUpdateHang.store(true);
    for (int i = 0; i < 500 && !fake->leftUpdate.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftUpdate.load());
    if (reopened) { other->closeDevice(); }

    // THE OTHER WAY IN: nothing abandoned, the service simply ANSWERS
    // sdrplay_api_ServiceNotResponding (12c's shape, and the log's 15:14:05
    // line). The device orphans its session all the same, so the table is
    // just as finished. Nothing is left inside this fake, so it lives on the
    // stack.
    {
        FakeSdrPlayApi gone;
        gone.addDevice("1706012346", abi::kRsp1);
        SdrPlaySource dead;
        CHECK(openOn(dead, gone));
        CHECK(dead.start());
        gone.updateResult = abi::ServiceNotResponding;
        CHECK(dead.setCenterFrequencyHz(101100000.0) == false);
        dead.stop();
        dead.closeDevice();
        cascade::source::sdrPlayClearEnumerationHoldOffForTest();
        const int getsBefore = gone.countStarting("GetDevices");
        CHECK(cascade::source::enumerateSdrPlayWith(gone.table).empty());
        CHECK(gone.countStarting("GetDevices") == getsBefore);
        CHECK(gone.closeCount == 0);
    }

    // AND THE LATCH IS THE TABLE'S, NOT THE PROCESS'S BY ACCIDENT: a healthy
    // table (in the field there is only one; here, a fresh fake) still scans,
    // lists its radio and leaves the panel empty.
    {
        FakeSdrPlayApi healthy;
        healthy.addDevice("1706012347", abi::kRsp1);
        cascade::source::sdrPlayClearEnumerationHoldOffForTest();
        CHECK(cascade::source::enumerateSdrPlayWith(healthy.table).size() == 1);
        CHECK(healthy.called("GetDevices"));
        CHECK(cascade::source::sdrPlayLastEnumerationSkip().empty());
    }
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();
}

void testAHealthyControlIsStillSynchronousAndAcknowledged() {
    // The bound must not have changed the ordinary path. A service that
    // answers is updated on the spot, the acknowledgement flag is still
    // waited for, and nothing is marked dead.
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource src;
    CHECK(openOn(src, fake));
    CHECK(src.start());

    const auto t0 = std::chrono::steady_clock::now();
    CHECK(src.setCenterFrequencyHz(101100000.0));
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(src.centerFrequencyHz() == 101100000.0);
    // Well inside the bound: the fake acknowledges the moment Update returns,
    // so this is the cost of the machinery and nothing else.
    CHECK(ms < 900);
    CHECK(fake.called(FakeSdrPlayApi::updateCall(abi::Update_Tuner_Frf, abi::Update_Ext1_None)));
    CHECK(!src.faulted());
    CHECK(!src.deviceDead());

    // ...and a control that is issued while nothing is streaming still makes
    // no vendor call at all, which is the one path the worker must not take.
    SdrPlaySource idle;
    FakeSdrPlayApi fake2;
    fake2.addDevice("1811003EFC", abi::kRsp1A);
    CHECK(openOn(idle, fake2));
    const int updatesBefore = fake2.countStarting("Update(");
    CHECK(idle.setCenterFrequencyHz(102300000.0));
    CHECK(fake2.countStarting("Update(") == updatesBefore);
}

void testAWedgedEnumerationIsAbandonedAndThenHeldOff() {
    // THE FAKE IS ON THE HEAP AND IS NOT DESTROYED HERE. This test abandons a
    // worker that is parked inside the fake's GetDevices; it is released at
    // the end and waited for, and only then would destruction be safe - but
    // the whole point of the abandonment is that the driver has given up on
    // ever hearing from it. Owning it for the life of the process is the same
    // judgement the driver makes about a stranded Link, and for the same
    // reason: a leak is survivable, a use-after-free on somebody else's thread
    // is not.
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1811003EFB", abi::kRsp1A);
    fake->hangInGetDevices.store(true);
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<cascade::source::NativeDeviceInfo> devs =
        cascade::source::enumerateSdrPlayWith(fake->table);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    CHECK(devs.empty());
    // Bounded by kEnumerateWait, not by the service. The upper bound is
    // generous because this machine builds and tests in parallel; what it
    // rules out is the unbounded wait that produced the report.
    CHECK(ms >= 2500);
    CHECK(ms < 10000);
    if (ms >= 10000) { std::printf("     enumeration took %lld ms\n", ms); }
    CHECK(fake->insideGetDevices.load());

    // The panel gets the sentence, verbatim, not a paraphrase.
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          std::string(cascade::source::sdrPlayServiceHungSentence()));
    CHECK(cascade::source::sdrPlayEnumerationHeldOff());
    // ONE CHIP, ONE ROUTE: a worker parked inside the API is "abandoned" as
    // far as every other route to the same chip is concerned, and the question
    // is answered while that worker is still inside GetDevices - from atomics,
    // never from the session mutex it may be holding.
    {
        const cascade::source::SdrPlayApiState st = cascade::source::sdrPlayApiStateOf(fake->table);
        CHECK(st.installed);
        CHECK(st.workerAbandoned);
        CHECK(!st.sessionLost);  // an abandoned scan is not a lost session
    }

    // AND THE NEXT SCAN DOES NOT TOUCH THE API AT ALL. The source combo scans
    // every time it opens, so without the hold-off each of those would spend
    // another three seconds and abandon another worker in a service that is
    // still wedged.
    const std::size_t callsBefore = fake->callCount();
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<cascade::source::NativeDeviceInfo> again =
        cascade::source::enumerateSdrPlayWith(fake->table);
    const long long heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t1)
                                 .count();
    CHECK(again.empty());
    CHECK(heldMs < 250);
    CHECK(fake->callCount() == callsBefore);
    CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
          std::string(cascade::source::sdrPlayServiceHungSentence()));

    // Let the abandoned worker leave before this process does, so nothing is
    // still inside the fake when the run ends.
    fake->releaseHang.store(true);
    for (int i = 0; i < 500 && !fake->leftGetDevices.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->leftGetDevices.load());
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();
}

void testAHealthyEnumerationIsStillSynchronousAndClearsTheSentence() {
    // The bound must not have changed the ordinary path: a service that
    // answers is enumerated on the spot and leaves nothing on the panel.
    FakeSdrPlayApi fake;
    fake.addDevice("1811003EFB", abi::kRsp1A);
    cascade::source::sdrPlayClearEnumerationHoldOffForTest();
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<cascade::source::NativeDeviceInfo> devs =
        cascade::source::enumerateSdrPlayWith(fake.table);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(devs.size() == 1);
    CHECK(ms < 1000);
    CHECK(cascade::source::sdrPlayLastEnumerationSkip().empty());
    CHECK(!cascade::source::sdrPlayEnumerationHeldOff());
    {   // A healthy table is not quarantined (one chip, one route).
        const cascade::source::SdrPlayApiState st = cascade::source::sdrPlayApiStateOf(fake.table);
        CHECK(st.installed && !st.sessionLost && !st.workerAbandoned);
    }
    CHECK(fake.called("GetDevices"));
    // The session is balanced: opened once, closed once, whichever thread did it.
    CHECK(fake.openCount == 1);
    CHECK(fake.closeCount == 1);
}

// --- 13. the interface description itself ---------------------------------

void testAbiLayoutIsPinnedToTheVersionsWeChecked() {
    // The static_asserts in sdrplay_api_decl.hpp are the real guard and they
    // fire at COMPILE time; these are here so the numbers appear in a test
    // report too, and so that the two facts the driver's version gates depend
    // on are stated where a reader will see them.
    CHECK(sizeof(abi::RxChannelParamsT) == 144);
    CHECK(sizeof(abi::DeviceT) == 96);
    CHECK(sizeof(abi::DevParamsT) == 64);
    CHECK(offsetof(abi::RxChannelParamsT, rspDuoTunerParams) == 124);
    // The one member that moved: 136 at 3.07 and 3.11, 140 from 3.15.
    CHECK(offsetof(abi::RxChannelParamsT, rspDxTunerParams) == 140);
    CHECK(offsetof(abi::DeviceT, valid) == 76);
    CHECK(abi::kMinApiVersion < abi::kValidFieldSinceVersion);
    CHECK(abi::kValidFieldSinceVersion < abi::kRspDxTunerLayoutVersion);
}

// --- the SDRplay API Service behind a refused Open (0.99.55) ---------------
//
// THE FIELD REPORT: an RSP2 Pro on 0.99.52, API 3.15 - "SDRplay open failed -
// the SDRplay service did not answer: sdrplay_api_Fail (1). Check that the
// SDRplay API service is running." on seven launches, and nothing that told
// him WHICH state the service was in or how to fix it. The service query is
// faked here (setSdrPlayServiceQueryForTest); the real Service Control
// Manager path is in test_sdrplay_service.cpp.

cascade::source::SdrPlayServiceStatus fakeService(cascade::source::SdrPlayServiceState state) {
    cascade::source::SdrPlayServiceStatus s;
    s.state = state;
    s.startType = cascade::source::SdrPlayServiceStart::Manual;
    if (state != cascade::source::SdrPlayServiceState::NotApplicable) {
        s.serviceName = "SDRplayAPIService";
    }
    return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

int ringLinesContaining(const char* needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

void testARefusedOpenSaysWhatWindowsSaysTheServiceIsDoing() {
    using cascade::source::SdrPlayServiceState;
    using cascade::source::SdrPlayServiceTrouble;
    cascade::core::DiagLog::instance().resetForTest();
    cascade::source::sdrPlayServiceResetNoteForTest();
    int queries = 0;
    SdrPlayServiceState answer = SdrPlayServiceState::Stopped;
    cascade::source::setSdrPlayServiceQueryForTest([&queries, &answer]() {
        ++queries;
        return fakeService(answer);
    });

    const std::string base =
        "the SDRplay service did not answer: Fail (1). Check that the SDRplay API service is "
        "running.";
    const std::string stopped =
        "The SDRplay API Service is stopped - press RESTART SDRPLAY SERVICE to start it.";
    {
        FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;  // what the tester's service answered, in 0 ms
        fake.addDevice("2305000CCC", abi::kRsp2);
        SdrPlaySource src;
        CHECK(!openOn(src, fake));
        // THE PANEL: the driver's own sentence, then what Windows says.
        std::printf("the refused open's panel sentence: %s\n", src.lastError());
        CHECK(std::string(src.lastError()) == base + " " + stopped);
        CHECK(queries == 1);
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::OpenFailed);
        CHECK(!fake.called("Close"));  // Open failed: there is nothing to close

        // THE LOG: the open-failed line keeps the driver's sentence (a log
        // line is kLineBytes long) and the service's state has a line of its
        // own - ONCE, however many times the user presses Refresh.
        CHECK(ringLinesContaining("SDRplay open failed - the SDRplay service did not answer") == 1);
        CHECK(ringLinesContaining("RESTART SDRPLAY SERVICE") == 0);
        CHECK(ringLinesContaining("SDRplay API Service - stopped, manual start (SDRplayAPIService)") == 1);
        CHECK(!src.open(""));
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        CHECK(queries == 4);
        CHECK(ringLinesContaining("SDRplay API Service - stopped") == 1);

        // THE SCAN'S SENTENCE, which is what the Source section shows with no
        // radio open: the same two halves.
        const std::string skip = cascade::source::sdrPlayLastEnumerationSkip();
        CHECK(skip == base + " " + stopped);
        CHECK(cascade::source::sdrPlayPanelAdvice(true, 0.0f, skip) == skip);

        // THE STATE CHANGES (the service started, but still refuses): a new
        // sentence, and exactly one new log line.
        answer = SdrPlayServiceState::Running;
        CHECK(!src.open(""));
        CHECK(endsWith(src.lastError(),
                       " The SDRplay API Service is running but did not answer - press RESTART "
                       "SDRPLAY SERVICE."));
        CHECK(ringLinesContaining("SDRplay API Service - running") == 1);
        CHECK(ringLinesContaining("SDRplay API Service - stopped") == 1);

        // THE SERVICE ANSWERS AGAIN: the trouble record is cleared, which is
        // what takes the RESTART key off the panel.
        fake.openResult = abi::Success;
        const int before = queries;
        CHECK(openOn(src, fake));
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::None);
        CHECK(queries == before);  // Windows is asked only when Open failed
        src.closeDevice();
    }
    {
        // OFF WINDOWS NOTHING IS ASKED AND NOTHING IS ADDED: the sentence is
        // exactly what it was before 0.99.55, with no trailing space.
        answer = SdrPlayServiceState::NotApplicable;
        FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;
        SdrPlaySource src;
        CHECK(!openOn(src, fake));
        CHECK(std::string(src.lastError()) == base);
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        CHECK(cascade::source::sdrPlayLastEnumerationSkip() == base);
    }
    cascade::source::setSdrPlayServiceQueryForTest(nullptr);
    cascade::source::sdrPlayServiceResetNoteForTest();
}

// WHAT A FINISHED RESTART MAY DO, on the table it was pressed for. The
// tester's case - a process that NEVER got a session - reopens in place. A
// LOST session is never reopened in this process: markSessionLost latched it
// because a worker of ours may still be inside the vendor DLL, and a new
// service does not bring that thread back.
void testAServiceRestartReopensOnlyASessionThatWasNeverAcquired() {
    using cascade::source::SdrPlayAfterRestart;
    using cascade::source::SdrPlayServiceState;
    using cascade::source::SdrPlayServiceTrouble;
    cascade::source::setSdrPlayServiceQueryForTest(
        []() { return fakeService(SdrPlayServiceState::Stopped); });

    {  // NEVER ACQUIRED: rescan and reopen, no FoxSDR restart.
        FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;
        fake.addDevice("2305000CCC", abi::kRsp2);
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        CHECK(!cascade::source::sdrPlayLastEnumerationSkip().empty());
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::OpenFailed);
        // A service that merely refused an Open has nobody parked inside it:
        // not a quarantine (one chip, one route).
        {
            const cascade::source::SdrPlayApiState st = cascade::source::sdrPlayApiStateOf(fake.table);
            CHECK(!st.sessionLost && !st.workerAbandoned);
        }

        // The restart ran but Windows still does not report the service
        // running: nothing is cleared, nothing is reopened.
        CHECK(cascade::source::sdrPlayApplyServiceRestart(fake.table, false) ==
              SdrPlayAfterRestart::ServiceNotRunning);
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::OpenFailed);
        CHECK(!cascade::source::sdrPlayLastEnumerationSkip().empty());

        // Now it is running and answers.
        fake.openResult = abi::Success;
        CHECK(cascade::source::sdrPlayApplyServiceRestart(fake.table, true) ==
              SdrPlayAfterRestart::ReopenInProcess);
        CHECK(cascade::source::sdrPlayLastEnumerationSkip().empty());
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::None);
        CHECK(!cascade::source::sdrPlayEnumerationHeldOff());
        CHECK(!fake.table.sessionLost);
        const std::vector<cascade::source::NativeDeviceInfo> rows =
            cascade::source::enumerateSdrPlayWith(fake.table);
        CHECK(rows.size() == 1u);
        SdrPlaySource src;
        CHECK(!rows.empty() && openOn(src, fake, rows[0].args));
        CHECK(src.isOpen());
        src.closeDevice();
    }

    {  // LOST: the latch stays, and so does the refusal.
        FakeSdrPlayApi fake;
        fake.addDevice("2305000CCC", abi::kRsp2);
        {
            SdrPlaySource a;
            CHECK(openOn(a, fake));
            CHECK(a.start());
            fake.uninitResult = abi::ServiceNotResponding;
            a.stop();
            a.closeDevice();
        }
        CHECK(fake.table.sessionLost);
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::SessionLost);
        // ONE CHIP, ONE ROUTE: the lock-free view every SoapySDR open reads
        // says the same, and keeps saying it after a restart attempt.
        CHECK(cascade::source::sdrPlayApiStateOf(fake.table).sessionLost);

        CHECK(cascade::source::sdrPlayApplyServiceRestart(fake.table, true) ==
              SdrPlayAfterRestart::RestartFoxSdr);
        CHECK(fake.table.sessionLost);
        CHECK(cascade::source::sdrPlayApiStateOf(fake.table).sessionLost);
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::SessionLost);
        // ...and nothing enters the vendor DLL afterwards: not an open, not a
        // scan. The same refusal a lost session has always given.
        const int opensBefore = fake.openCount;
        const int getDevicesBefore = fake.countStarting("GetDevices");
        SdrPlaySource b;
        CHECK(!openOn(b, fake));
        CHECK(std::string(b.lastError()) ==
              std::string(cascade::source::sdrPlaySessionLostSentence()));
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        CHECK(cascade::source::sdrPlayLastEnumerationSkip() ==
              std::string(cascade::source::sdrPlaySessionLostSentence()));
        CHECK(fake.openCount == opensBefore);
        CHECK(fake.countStarting("GetDevices") == getDevicesBefore);
    }

    {  // A SESSION STATE THAT CANNOT BE READ WITHOUT WAITING - an abandoned
       // scan's worker parked inside sdrplay_api_Open holds exactly this
       // mutex - is not waited for, and is not treated as clear.
        FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;
        CHECK(cascade::source::enumerateSdrPlayWith(fake.table).empty());
        std::lock_guard<std::mutex> held(fake.table.sessionMutex);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(cascade::source::sdrPlayApplyServiceRestart(fake.table, true) ==
              SdrPlayAfterRestart::RestartFoxSdr);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
        CHECK(cascade::source::sdrPlayServiceTrouble(fake.table) == SdrPlayServiceTrouble::OpenFailed);
    }
    cascade::source::setSdrPlayServiceQueryForTest(nullptr);
    cascade::source::sdrPlayServiceResetNoteForTest();
}

}  // namespace

int main() {
#if !defined(_WIN32)
    testLinuxSoNameIsTheVendorInstalledOne();
#endif
    testMissingApiIsEmptyAndSaysWhy();
    testOldApiIsRefusedWithASentence();
    testEnumerationLabelsAndArgs();
    testOpenSelectInitOrderAndParameters();
    testOpenBySerialSuffixAndIndex();
    testTune();
    testAnUnchangedFrequencySendsNoUpdate();
    testATuneBeforeStartLeavesNothingToSendAfterInit();
    testARefusedChangeLeavesTheBlockWhereTheRadioIs();
    testARefusedAntennaChangeLeavesTheBlockWhereTheRadioIs();
    testEverySentenceAfterALostSessionNamesTheRestartThatWorks();
    testAChangeRefusedHalfWayReadsBackWhereTheRadioStopped();
    testGains();
    testAgc();
    testAntennasPerModel();
    testRatePlans();
    testEveryRatePlanDeliversTheRateItClaims();
    testADeliveredRateFarFromTheSetOneIsSaid();
    testSetSampleRateWritesThePlanInOneUpdate();
    testStreamingDeliversEverySampleInOrder();
    testStreamHealthLine();
    testDeviceRemovedFaultsTheSource();
    testDeviceFailureAndMasterLossAlsoFault();
    testOverloadIsCountedAndAcknowledged();
    testOverloadAckFollowsALiveTunerSwap();
    testOverloadDuringRetuneNeverOverlapsTheVendorCall();
    testOverloadAckNeverBlocksReadBehindABusyDevMutex();
    testStaleOverloadAckDoesNotSurviveRestart();
    testOverloadNamingNeitherTunerAcksOnLinkTuner();
    testBiasTeeAndNotchesPerModel();
    testTheRspDxR2IsDrivenWithTheRspDxReasonsAndBlock();
    testTheRspDxR2UpdatesNameTunerAWithTheExactReasons();
    testASingleTunerRspIsTunerAEvenWhenTheServiceListsNone();
    testEverySingleTunerModelUpdatesTunerA();
    testStopAndCloseAreBoundedAndIdempotent();
    testCloseWithoutOpenIsSafe();
    testFailuresOnTheOpeningPathUnwind();
    testSkipReasonReachesTheSourcePanel();
    testServiceNotRespondingMakesTheDeviceDead();
    testARefusedUninitStrandsTheLinkBecauseTheServiceStillHoldsTheCallback();
    testAHealthyControlIsStillSynchronousAndAcknowledged();
    testAHealthyEnumerationIsStillSynchronousAndClearsTheSentence();
    testARefusedOpenSaysWhatWindowsSaysTheServiceIsDoing();
    testAServiceRestartReopensOnlyASessionThatWasNeverAcquired();
    // The three that abandon or hang a worker inside their own fake go last,
    // in the order they were written, and each releases and waits for its own
    // before it returns.
    testAWedgedControlIsAbandonedAndTheDeviceIsDead();
    testATeardownAfterAnAbandonedControlNeverEntersTheVendorDll();
    testStopsOwnUninitGoingServiceNotRespondingKeepsCloseDeviceOffTheVendorDll();
    testALostSessionIsNeverEnteredAgainByAScanOrAnOpen();
    testALostRadioTakesNoSettingAtAll();
    testALostSessionRefusesTheOtherRadiosSettingsToo();
    testALostSessionKeepsTheOtherRadiosTeardownOffTheVendorDll();
    testARefusedOpenAfterALossLogsTheWholeInstruction();
    testTheSameRadioReopenedAfterALossTakesSettingsAgain();
    testAUserStopIsNotAStall();
    testALateAnswerGivesTheRadioBack();
    testALateRefusalPutsTheBlockBackAndALateDeadServiceIsDead();
    testAControlThatNeverAnswersIsGivenUpAfterTheGrace();
    testAStopWhileAControlIsWaitingNeverEntersTheVendorDll();
    // LAST, and deliberately: it abandons a worker inside its own fake and
    // releases it again, and nothing that follows should have to reason about
    // a thread this one left running.
    testAWedgedEnumerationIsAbandonedAndThenHeldOff();
    testAbiLayoutIsPinnedToTheVersionsWeChecked();
    return testSummary("test_sdrplay_source");
}
