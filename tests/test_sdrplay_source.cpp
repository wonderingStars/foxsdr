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
#include <string>
#include <thread>
#include <vector>

#include "core/hang_watchdog.hpp"
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
                                           "SelectDevice(tuner=0,mode=0)",
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
    // 2 MS/s is produced from the 6 MHz front end at the 1.62 MHz IF, not from
    // a 2 MHz ADC rate - the reference's own arrangement, and getting it wrong
    // puts the receiver 1.62 MHz off frequency.
    CHECK_NEAR(fake.atInit.fsHz, 6000000.0, 1.0);
    CHECK(fake.atInit.ifType == abi::IF_1_620);
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

void testRatePlans() {
    // THE TABLE THE REFERENCE USES, and the distinction that matters: the
    // binary fractions of 2 MS/s come from the 6 MHz LOW-IF front end, and the
    // audio rates from a ZERO-IF front end run fast and decimated. Mixing them
    // up puts the receiver 1.62 MHz off frequency.
    SdrPlayRatePlan p;

    CHECK(cascade::source::sdrPlayRatePlan(2000000.0, p));
    CHECK_NEAR(p.fsHz, 6000000.0, 1.0);
    CHECK(p.ifType == abi::IF_1_620);
    CHECK(p.decEnable == 0);
    CHECK(p.decM == 1);
    CHECK(p.bwType == abi::BW_1_536);

    CHECK(cascade::source::sdrPlayRatePlan(62500.0, p));
    CHECK_NEAR(p.fsHz, 6000000.0, 1.0);
    CHECK(p.ifType == abi::IF_1_620);
    CHECK(p.decEnable == 1);
    CHECK(p.decM == 32);
    CHECK(p.bwType == abi::BW_0_200);

    CHECK(cascade::source::sdrPlayRatePlan(500000.0, p));
    CHECK(p.decM == 4);
    CHECK(p.ifType == abi::IF_1_620);
    CHECK_NEAR(p.fsHz, 6000000.0, 1.0);

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
    // ONE Update carrying every reason that changed. Three separate ones would
    // be three disturbances to a live stream for what the API does at once.
    CHECK(fake.calls.size() == 1);
    CHECK((fake.calls ==
          std::vector<std::string>{FakeSdrPlayApi::updateCall(
              abi::Update_Dev_Fs | abi::Update_Tuner_IfType | abi::Update_Tuner_BwType, 0)}));

    fake.calls.clear();
    CHECK(src.setSampleRateHz(384000.0));
    CHECK_NEAR(src.sampleRateHz(), 384000.0, 1.0);
    CHECK_NEAR(fake.devParams.fsFreq.fsHz, 3072000.0, 1.0);
    CHECK(fake.chA.ctrlParams.decimation.enable == 1);
    CHECK(fake.chA.ctrlParams.decimation.decimationFactor == 8);
    CHECK(fake.chA.ctrlParams.decimation.wideBandSignal == 1);
    CHECK(fake.calls.size() == 1);
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
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));

    fake.calls.clear();
    fake.fireOverload(false);
    CHECK(src.overloadEvents() == 1);  // corrected is not a second overload
    CHECK((fake.calls == std::vector<std::string>{FakeSdrPlayApi::updateCall(
                            abi::Update_Ctrl_OverloadMsgAck, 0)}));

    // An overload does NOT fault the source: it is the radio telling us the
    // gain is too high, not the radio going away.
    CHECK(!src.faulted());
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
        CHECK(fake.calls.size() == 2);
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
    SdrPlaySource other;
    FakeSdrPlayApi fake2;
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

void testAWedgedControlIsAbandonedAndTheDeviceIsDead() {
    // THE FAKE AND THE SOURCE ARE ON THE HEAP AND NEITHER IS DESTROYED, for
    // the reason the enumeration test below gives: a worker is abandoned
    // inside the fake's Update and the driver has given up on ever hearing
    // from it again. It is released and waited for at the end, which is as
    // close to safe as an abandonment gets.
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
    fake->addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource* src = new SdrPlaySource();
    CHECK(openOn(*src, *fake));
    CHECK(src->start());
    CHECK(!src->faulted());

    // The service wedges with the radio open and streaming. The user's next
    // click is a retune - the call the report names.
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
        const std::size_t callsBefore = fake->calls.size();
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(src->setGainDb("LNA", 2.0) == false);
        CHECK(src->setSampleRateHz(6000000.0) == false);
        const long long deadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t1)
                                     .count();
        CHECK(fake->calls.size() == callsBefore);
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
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
    fake->addDevice("1811003EFB", abi::kRsp1A);
    SdrPlaySource* src = new SdrPlaySource();
    CHECK(openOn(*src, *fake));
    CHECK(src->start());

    // The service wedges and the user's retune is abandoned inside it: the
    // exact state the report's last two log lines describe.
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
    const std::size_t callsBefore = fake->calls.size();

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
    CHECK(fake->calls.size() == callsBefore);

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
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
    fake->addDevice("1810012345", abi::kRspDx);
    SdrPlaySource* src = new SdrPlaySource();
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
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
    fake->addDevice("1810012345", abi::kRsp1);
    SdrPlaySource* src = new SdrPlaySource();
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
    FakeSdrPlayApi* fake = new FakeSdrPlayApi();
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

    // AND THE NEXT SCAN DOES NOT TOUCH THE API AT ALL. The source combo scans
    // every time it opens, so without the hold-off each of those would spend
    // another three seconds and abandon another worker in a service that is
    // still wedged.
    const std::size_t callsBefore = fake->calls.size();
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<cascade::source::NativeDeviceInfo> again =
        cascade::source::enumerateSdrPlayWith(fake->table);
    const long long heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t1)
                                 .count();
    CHECK(again.empty());
    CHECK(heldMs < 250);
    CHECK(fake->calls.size() == callsBefore);
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
    testGains();
    testAgc();
    testAntennasPerModel();
    testRatePlans();
    testSetSampleRateWritesThePlanInOneUpdate();
    testStreamingDeliversEverySampleInOrder();
    testStreamHealthLine();
    testDeviceRemovedFaultsTheSource();
    testDeviceFailureAndMasterLossAlsoFault();
    testOverloadIsCountedAndAcknowledged();
    testBiasTeeAndNotchesPerModel();
    testStopAndCloseAreBoundedAndIdempotent();
    testCloseWithoutOpenIsSafe();
    testFailuresOnTheOpeningPathUnwind();
    testSkipReasonReachesTheSourcePanel();
    testServiceNotRespondingMakesTheDeviceDead();
    testARefusedUninitStrandsTheLinkBecauseTheServiceStillHoldsTheCallback();
    testAHealthyControlIsStillSynchronousAndAcknowledged();
    testAHealthyEnumerationIsStillSynchronousAndClearsTheSentence();
    // The three that abandon or hang a worker inside their own fake go last,
    // in the order they were written, and each releases and waits for its own
    // before it returns.
    testAWedgedControlIsAbandonedAndTheDeviceIsDead();
    testATeardownAfterAnAbandonedControlNeverEntersTheVendorDll();
    testStopsOwnUninitGoingServiceNotRespondingKeepsCloseDeviceOffTheVendorDll();
    // LAST, and deliberately: it abandons a worker inside its own fake and
    // releases it again, and nothing that follows should have to reason about
    // a thread this one left running.
    testAWedgedEnumerationIsAbandonedAndThenHeldOff();
    testAbiLayoutIsPinnedToTheVersionsWeChecked();
    return testSummary("test_sdrplay_source");
}
