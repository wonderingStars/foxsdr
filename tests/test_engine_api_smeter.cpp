// test_engine_api_smeter.cpp - THE S-METER IN dBm AND S UNITS, AND ITS CALIBRATION THROUGH THE ENGINE (engine wiring lane 1, group B).
//
// The real engine through the C table and its own passes. Ops: 0x0115 SET_METER_UNITS (per radio), FOXAPP_OP_CAL_MEASURE 0x8612 and FOXAPP_OP_CAL_CLEAR 0x8613 (per radio's device).
//
//   A  defaults: dBFS units, nothing calibrated, the dBm figure is the dBFS number standing in as an estimate, signalDb and sMeter untouched by anything below;
//   B  the display units are each radio's own, and a unit past dBm is refused;
//   C  a calibration is MEASURED (3 s of the radio's own level against a known dBm), after which the radio reads that dBm and S9 (one band only: the other band is still an estimate;
//      the squelch scale and the dBFS figures do not move), is shared by every radio on that device and not by a radio on another, and can be cleared one band or whole;
//   D  every refusal changes nothing: a level outside -150..+30 dBm, a band that is not one, a radio that is not running, a second measurement while one runs, a clear with
//      nothing to clear;
//   E  saved and restored: the table per device and each radio's units, in an in-process restart; a calibration text that does not parse is dropped at the loader.
//
// The device here is the signal generator (key "siggen"): its level is steady, which is what a measurement needs. Gain compensation is the block's (test_smeter_cal); the generator
// has no gain stages, so the engine's sum of them is 0 here and compensation is not exercised through the engine (UNVERIFIED there).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include "wire1_rig.hpp"

namespace {

FoxCommand calMeasure(std::uint32_t target, double knownDbm, bool explicitBand = false, double lo = 0.0, double hi = 0.0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_CAL_MEASURE, knownDbm, explicitBand ? 1 : 0, nullptr);
    c.num[1] = lo;
    c.num[2] = hi;
    c.target = target;
    return c;
}
FoxCommand calClear(std::uint32_t target, bool all, double hz = 0.0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_CAL_CLEAR, hz, all ? 1 : 0, nullptr);
    c.target = target;
    return c;
}

void startRunning(Rig& r, std::uint32_t radio) {
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, radio, 0.0, 1)).status(), FOXAPI_OK);
    r.pump(30);
}

bool measure(Rig& r, std::uint32_t radio, double knownDbm) {
    if (r.send(calMeasure(radio, knownDbm)).status() != FOXAPI_OK) { return false; }
    CHECK_EQ(r.fact("smeter.cal.state"), std::string("measuring"));
    return r.until([&] { return r.fact("smeter.cal.state") == "done"; }, 30000);
}

void testADefaults() {
    std::printf("A: defaults\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    startRunning(r, 1);
    for (std::uint32_t n : {1u, 2u}) {
        const FoxReceiverState s = r.radio(n);
        CHECK_EQ(s.meterUnits, 0u);
        CHECK_EQ(s.calibrated, 0u);
        // uncalibrated: the dBFS number stands in as an estimate, clamped to the meter's range
        CHECK(std::fabs(s.signalDbm - std::min(30.0, std::max(-200.0, s.signalDb))) < 1e-9);
    }
    CHECK_EQ(r.fact("smeter.cal.state"), std::string("idle"));
    CHECK_EQ(r.fact("smeter.cal.devices"), std::string("0"));
}

void testBUnits() {
    std::printf("B: each radio's own display units\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_METER_UNITS, 2, 0.0, FOXAPI_METER_S)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).meterUnits == FOXAPI_METER_S; }, 5000));
    CHECK_EQ(r.radio(1).meterUnits, 0u);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_METER_UNITS, 1, 0.0, FOXAPI_METER_DBM)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).meterUnits == FOXAPI_METER_DBM; }, 5000));
    CHECK_EQ(r.radio(2).meterUnits, static_cast<std::uint32_t>(FOXAPI_METER_S));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_METER_UNITS, 1, 0.0, 3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_METER_UNITS, 1, 0.0, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_METER_UNITS, 9, 0.0, 1)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.radio(1).meterUnits, static_cast<std::uint32_t>(FOXAPI_METER_DBM));
}

void testCCalibration() {
    std::printf("C: a calibration is measured and read back\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    startRunning(r, 1);
    startRunning(r, 2);
    // the HF band (the radio is at 100 MHz by default: tune it to 7.1 MHz)
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 7100000.0)).status(), FOXAPI_OK);
    r.pump(20);
    const FoxReceiverState before = r.radio(1);
    CHECK_EQ(before.calibrated, 0u);
    CHECK(measure(r, 1, -73.0));
    CHECK(r.fact("smeter.cal.message").find("offset") != std::string::npos);
    CHECK_EQ(r.fact("smeter.cal.devices"), std::string("1"));
    const FoxReceiverState after = r.radio(1);
    CHECK_EQ(after.calibrated, 1u);
    std::printf("  signalDb %.2f dBFS -> signalDbm %.2f dBm, sUnits %.3f\n", after.signalDb, after.signalDbm, after.sUnits);
    CHECK(std::fabs(after.signalDbm - (-73.0)) < 1.5);   // the level it was calibrated at reads as that level (the generator is steady to a dB or so)
    CHECK(std::fabs(after.sUnits - 9.0) < 0.3);          // S9 at HF
    // dBFS figures and the squelch scale are what they were
    CHECK(std::fabs(after.signalDb - before.signalDb) < 3.0);
    CHECK(std::fabs(after.sMeter - std::min(1.0, std::max(0.0, (after.signalDb + 120.0) / 120.0))) < 1e-9);
    // radio 2 is on the same device (the generator): it reads calibrated too, at ITS frequency
    CHECK(r.until([&] { return r.radio(2).calibrated == 1u; }, 5000) || r.radio(2).calibrated == 0u);   // (radio 2 sits at 100 MHz: the VHF band, which is not calibrated)
    CHECK_EQ(r.radio(2).calibrated, 0u);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 14200000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).calibrated == 1u; }, 5000));
    // the other band is still an estimate
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 145000000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).calibrated == 0u; }, 5000));
    {
        const FoxReceiverState s = r.radio(1);
        CHECK(std::fabs(s.signalDbm - std::min(30.0, std::max(-200.0, s.signalDb))) < 1e-9);
    }
    // a measurement on the VHF band, with its own known level: S9 there is -93 dBm
    CHECK(measure(r, 1, -93.0));
    CHECK_EQ(r.fact("smeter.cal.devices"), std::string("1"));   // still one device
    CHECK(r.until([&] { return r.radio(1).calibrated == 1u; }, 5000));
    CHECK(std::fabs(r.radio(1).sUnits - 9.0) < 0.3);
    // clear the VHF band alone (the entry that covers the radio's frequency), then everything
    CHECK_EQ(r.send(calClear(1, false)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).calibrated == 0u; }, 5000));
    CHECK_EQ(r.radio(2).calibrated, 1u);   // the HF band is still there
    CHECK_EQ(r.send(calClear(1, true)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).calibrated == 0u; }, 5000));
    CHECK_EQ(r.fact("smeter.cal.devices"), std::string("0"));
    // an explicit band
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 3600000.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(calMeasure(1, -80.0, true, 3.0e6, 4.0e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("smeter.cal.state") == "done"; }, 30000));
    CHECK(r.until([&] { return r.radio(1).calibrated == 1u; }, 5000));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 7100000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).calibrated == 0u; }, 5000));   // outside 3 to 4 MHz
}

void testDRefusals() {
    std::printf("D: every refusal changes nothing\n");
    Rig r;
    CHECK_EQ(r.add(2, "", false).status(), FOXAPI_OK);   // (added stopped)
    startRunning(r, 1);
    CHECK_EQ(r.send(calMeasure(1, 31.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(calMeasure(1, -151.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(calMeasure(1, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);   // (a number that is not a number is refused at the door)
    CHECK_EQ(r.send(calMeasure(1, -73.0, true, 5.0e6, 4.0e6)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(calMeasure(1, -73.0, true, -1.0, 4.0e6)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(calMeasure(1, -73.0, true, 0.0, 7.0e9)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(calMeasure(2, -73.0)).status(), FOXAPI_BUSY);   // radio 2 is not running
    CHECK_EQ(r.send(calMeasure(9, -73.0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.fact("smeter.cal.state"), std::string("idle"));
    CHECK_EQ(r.send(calClear(1, false)).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(r.send(calClear(1, true)).status(), FOXAPI_NOT_FOUND);
    // a second measurement while one runs
    CHECK_EQ(r.send(calMeasure(1, -73.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(calMeasure(1, -60.0)).status(), FOXAPI_BUSY);
    CHECK(r.until([&] { return r.fact("smeter.cal.state") == "done"; }, 30000));
    // the radio stopping in the middle of a measurement fails it, in words
    CHECK_EQ(r.send(calMeasure(1, -73.0, true, 0.0, 1.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("smeter.cal.state") == "failed"; }, 10000));
    CHECK(r.fact("smeter.cal.message").find("stopped") != std::string::npos);
    CHECK_EQ(r.fact("smeter.cal.devices"), std::string("1"));   // only the first measurement stored anything
}

void testESavedAndRestored() {
    std::printf("E: saved with the units, restored, junk dropped\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        startRunning(a, 1);
        CHECK_EQ(a.send(a.aimed(FOXAPI_OP_SET_CENTRE, 1, 7100000.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(a.aimed(FOXAPI_OP_SET_METER_UNITS, 1, 0.0, FOXAPI_METER_S)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(a.aimed(FOXAPI_OP_SET_METER_UNITS, 2, 0.0, FOXAPI_METER_DBM)).status(), FOXAPI_OK);
        CHECK(measure(a, 1, -73.0));
        a.pump(10);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.meterUnits, 1);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].meterUnits, 2);
        CHECK_EQ(file.smeterCal.size(), static_cast<std::size_t>(1));
        CHECK(file.smeterCal.count("siggen") == 1);
        const json j = json::parse(ConfigStore::serialize(Access::snapshot(a.eng)));
        CHECK(j.contains("smeterCal") && j["smeterCal"].contains("siggen"));
        CHECK_EQ(j["meterUnits"].get<int>(), 1);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).meterUnits, static_cast<std::uint32_t>(FOXAPI_METER_S));
        CHECK_EQ(b.radio(2).meterUnits, static_cast<std::uint32_t>(FOXAPI_METER_DBM));
        CHECK_EQ(b.fact("smeter.cal.devices"), std::string("1"));
        startRunning(b, 1);
        CHECK_EQ(b.radio(1).centreHz, 7100000.0);
        CHECK(b.until([&] { return b.radio(1).calibrated == 1u; }, 5000));
        CHECK(std::fabs(b.radio(1).sUnits - 9.0) < 0.3);
        const AppConfig again = throughTheFile(b.eng);
        CHECK(again.smeterCal == file.smeterCal);
    }
    // a file whose calibration text is damaged: that device's calibration is dropped whole, the rest of the file loads
    {
        AppConfig junk = file;
        junk.smeterCal["siggen"] = "7000000 not-a-number\n";
        junk.smeterCal["other"] = std::string(9000, 'x');
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(junk), "test", back, error));
        CHECK(back.smeterCal.empty());
        CHECK_EQ(back.meterUnits, 1);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_smeter\n");
    engine_api_test::hermetic("smeter");
    Access::installHooks();
    testADefaults();
    testBUnits();
    testCCalibration();
    testDRefusals();
    testESavedAndRestored();
    return check::finish("test_engine_api_smeter");
}
