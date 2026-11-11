// Tests for the S-meter calibration maths (core/smeter_cal.hpp).
//
// The expected numbers are NOT read back from the implementation: they are the IARU Region 1 R.1
// figures as tabulated by two published pages (Wikipedia "S meter" for HF: S1 -121, S3 -109, S5 -97,
// S7 -85, S8 -79, S9 -73 dBm; hamwaves.com "Decibel & S-Readings" for HF and VHF/UHF, S9+20 dB, +40 dB),
// and the 6 dB-per-S-unit rule. The recommendation itself was not opened (UNVERIFIED against it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/smeter_cal.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <cmath>
#include <limits>

namespace {

namespace sm = cascade::core::smeter;

constexpr double kHf = 14.2e6;
constexpr double kVhf = 145.5e6;

void testFixedPointsHf() {
    // The published HF table.
    const struct { int s; double dbm; } hf[] = {{1, -121}, {2, -115}, {3, -109}, {4, -103}, {5, -97}, {6, -91}, {7, -85}, {8, -79}, {9, -73}};
    for (const auto& p : hf) {
        const sm::SReading r = sm::dbmToS(p.dbm, kHf);
        CHECK(r.valid && r.sUnit == p.s && r.overS9Db == 0.0);
        CHECK_NEAR(r.units, p.s, 1e-9);
        CHECK(sm::formatS(p.dbm, kHf) == "S" + std::to_string(p.s));
        CHECK_NEAR(sm::sToDbm(p.s, 0.0, kHf), p.dbm, 1e-9);
    }
    // Above S9: S9 + 20 dB is -53 dBm, S9 + 40 dB is -33 dBm (hamwaves table).
    sm::SReading r = sm::dbmToS(-53.0, kHf);
    CHECK(r.sUnit == 9 && std::fabs(r.overS9Db - 20.0) < 1e-9 && std::fabs(r.units - 9.0 - 20.0 / 60.0) < 1e-9);
    CHECK(sm::formatS(-53.0, kHf) == "S9+20 dB");
    CHECK(sm::formatS(-33.0, kHf) == "S9+40 dB");
    CHECK(sm::formatS(-63.0, kHf) == "S9+10 dB");
    CHECK_NEAR(sm::sToDbm(9, 40.0, kHf), -33.0, 1e-9);
    CHECK_NEAR(sm::dbmToS(-53.0, kHf).units, 9.3333333333, 1e-9);   // the design's "9.33 = S9 + 20 dB"
}

void testFixedPointsVhfAndBoundary() {
    // VHF/UHF: every figure 20 dB lower (hamwaves: S9 -93, S8 -99, S7 -105, S1 -141, S9+20 -73, S9+40 -53).
    const struct { int s; double dbm; } vhf[] = {{9, -93}, {8, -99}, {7, -105}, {1, -141}};
    for (const auto& p : vhf) {
        const sm::SReading r = sm::dbmToS(p.dbm, kVhf);
        CHECK(r.sUnit == p.s && r.overS9Db == 0.0);
        CHECK_NEAR(sm::sToDbm(p.s, 0.0, kVhf), p.dbm, 1e-9);
    }
    CHECK(sm::formatS(-73.0, kVhf) == "S9+20 dB");
    CHECK(sm::formatS(-53.0, kVhf) == "S9+40 dB");
    // The same level reads differently on the two sides: -73 dBm is S9 below 30 MHz and S9+20 dB above.
    CHECK(sm::formatS(-73.0, 29.999999e6) == "S9");
    CHECK(sm::formatS(-73.0, 30.0e6) == "S9+20 dB");   // 30.000000 MHz counts as VHF/UHF (UNVERIFIED boundary choice)
    CHECK_NEAR(sm::s9Dbm(1.8e6), -73.0, 0);
    CHECK_NEAR(sm::s9Dbm(29.7e6), -73.0, 0);
    CHECK_NEAR(sm::s9Dbm(50.0e6), -93.0, 0);
    CHECK_NEAR(sm::s9Dbm(1296.0e6), -93.0, 0);
    // A bad frequency is treated as HF.
    CHECK_NEAR(sm::s9Dbm(std::numeric_limits<double>::quiet_NaN()), -73.0, 0);
    CHECK_NEAR(sm::s9Dbm(-5.0), -73.0, 0);
    CHECK_NEAR(sm::s9Dbm(0.0), -73.0, 0);
    // 6 dB per S unit all the way: every adjacent pair differs by exactly 6 dB, on both sides.
    for (double f : {kHf, kVhf}) {
        for (int s = 1; s < 9; ++s) { CHECK_NEAR(sm::sToDbm(s + 1, 0, f) - sm::sToDbm(s, 0, f), 6.0, 1e-12); }
    }
    // The IARU relation in volts: S9 HF = 50 uV rms into 50 ohm is -73 dBm; S9 VHF = 5 uV is -93 dBm.
    const double hfV = 50e-6, vhfV = 5e-6;
    const double hfDbm = 10.0 * std::log10(hfV * hfV / 50.0 / 1e-3);
    const double vhfDbm = 10.0 * std::log10(vhfV * vhfV / 50.0 / 1e-3);
    CHECK_NEAR(hfDbm, -73.0, 0.02);
    CHECK_NEAR(vhfDbm, -93.0, 0.02);
}

void testRoundingAndClamps() {
    // Between units: rounds to the nearest S unit below S9, to the nearest dB over S9.
    CHECK(sm::formatS(-81.9, kHf) == "S8");   // 2.1 dB under S8 ... -79 - 2.9 = S7.5.. -81.9 is 8.9 under S9: 7.52 -> S8
    CHECK(sm::formatS(-82.1, kHf) == "S7");
    CHECK(sm::formatS(-73.4, kHf) == "S9");
    CHECK(sm::formatS(-72.6, kHf) == "S9");      // 0.4 dB over S9 reads S9
    CHECK(sm::formatS(-72.4, kHf) == "S9+1 dB");  // 0.6 dB over rounds to 1 dB
    CHECK(sm::formatS(-72.51, kHf) == "S9");   // 0.49 dB over S9 reads S9
    // The ends saturate: S0 below the floor, S9+60 dB at the top.
    CHECK(sm::formatS(-130.0, kHf) == "S0");
    CHECK(sm::formatS(-200.0, kHf) == "S0");
    CHECK(sm::formatS(-1000.0, kHf) == "S0");
    CHECK_NEAR(sm::dbmToS(-1000.0, kHf).units, 0.0, 0);
    CHECK(sm::formatS(0.0, kHf) == "S9+60 dB");
    CHECK(sm::formatS(-13.0, kHf) == "S9+60 dB");
    CHECK(sm::formatS(500.0, kHf) == "S9+60 dB");
    CHECK_NEAR(sm::dbmToS(500.0, kHf).units, 10.0, 1e-9);
    CHECK(sm::dbmToS(std::numeric_limits<double>::infinity(), kHf).sUnit == 9);
    CHECK(sm::dbmToS(-std::numeric_limits<double>::infinity(), kHf).sUnit == 0);
    // NaN: invalid, reads "S?".
    CHECK(!sm::dbmToS(std::numeric_limits<double>::quiet_NaN(), kHf).valid);
    CHECK(sm::formatS(std::numeric_limits<double>::quiet_NaN(), kHf) == "S?");
    CHECK(sm::formatDbm(std::numeric_limits<double>::quiet_NaN()) == "-- dBm");
    CHECK(sm::formatDbm(-85.4) == "-85 dBm");
    CHECK(sm::formatDbm(-85.6) == "-86 dBm");
    CHECK(sm::formatDbm(-500.0) == "-200 dBm");
    // sToDbm clamps its arguments.
    CHECK_NEAR(sm::sToDbm(-3, 0, kHf), sm::sToDbm(0, 0, kHf), 0);
    CHECK_NEAR(sm::sToDbm(15, 0, kHf), -73.0, 0);
    CHECK_NEAR(sm::sToDbm(9, 500.0, kHf), -13.0, 0);
    CHECK_NEAR(sm::sToDbm(9, -4.0, kHf), -73.0, 0);
}

void testOffsetAndGainCompensation() {
    double off = 0;
    // A -73 dBm generator reads -40 dBFS at gain 30 dB: offset = -33 dB.
    CHECK(sm::offsetFromMeasurement(-73.0, -40.0, off));
    CHECK_NEAR(off, -33.0, 1e-12);
    sm::CalEntry e;
    e.loHz = 10e6; e.hiHz = 30e6; e.offsetDb = off; e.gainDb = 30.0; e.gainIsDb = true;
    // The same level, the same gain: reads back -73 dBm, calibrated and exact.
    sm::CalResult r = sm::applyCal(&e, -40.0, 30.0);
    CHECK_NEAR(r.dbm, -73.0, 1e-9);
    CHECK(r.calibrated && r.exact);
    // OFFSET APPLIED: a 10 dB stronger signal at the same gain reads -63 dBm.
    CHECK_NEAR(sm::applyCal(&e, -30.0, 30.0).dbm, -63.0, 1e-9);
    // GAIN COMPENSATION, BOTH DIRECTIONS: the antenna signal is unchanged (-73 dBm). Gain +10 dB -> the meter reads
    // 10 dB higher (-30 dBFS) and the displayed dBm must stay at -73. Gain -12 dB -> reads 12 dB lower (-52 dBFS).
    CHECK_NEAR(sm::applyCal(&e, -30.0, 40.0).dbm, -73.0, 1e-9);
    CHECK_NEAR(sm::applyCal(&e, -52.0, 18.0).dbm, -73.0, 1e-9);
    CHECK(sm::applyCal(&e, -30.0, 40.0).exact);
    // Gain in steps: no compensation, and not exact away from the calibrated setting.
    e.gainIsDb = false;
    e.gainDb = 3.0;
    r = sm::applyCal(&e, -40.0, 3.0);
    CHECK_NEAR(r.dbm, -73.0, 1e-9);
    CHECK(r.calibrated && r.exact);
    r = sm::applyCal(&e, -30.0, 4.0);
    CHECK_NEAR(r.dbm, -63.0, 1e-9);   // NOT compensated
    CHECK(r.calibrated && !r.exact);
    // Uncalibrated: the dBFS stands in, flagged.
    r = sm::applyCal(nullptr, -55.0, 30.0);
    CHECK_NEAR(r.dbm, -55.0, 0);
    CHECK(!r.calibrated && !r.exact);
    // Clamps: the result stays in [-200, +30] dBm.
    e.gainIsDb = true; e.gainDb = 0.0; e.offsetDb = 150.0;
    CHECK_NEAR(sm::applyCal(&e, -10.0, 0.0).dbm, 30.0, 0);
    e.offsetDb = -190.0;
    CHECK_NEAR(sm::applyCal(&e, -100.0, 0.0).dbm, -200.0, 0);
    // Non-finite readings or gains: no crash, no NaN out.
    CHECK(!std::isnan(sm::applyCal(&e, std::numeric_limits<double>::quiet_NaN(), 0.0).dbm));
    r = sm::applyCal(&e, -50.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(!std::isnan(r.dbm) && !r.exact);
    // offsetFromMeasurement refuses junk.
    off = 123.0;
    CHECK(!sm::offsetFromMeasurement(std::numeric_limits<double>::quiet_NaN(), -40.0, off));
    CHECK(!sm::offsetFromMeasurement(-73.0, std::numeric_limits<double>::infinity(), off));
    CHECK(!sm::offsetFromMeasurement(-73.0, -200.0, off));   // the meter's floor: nothing was measured
    CHECK(!sm::offsetFromMeasurement(-200.0, -40.0, off));   // no generator goes that low
    CHECK(!sm::offsetFromMeasurement(+40.0, -40.0, off));
    CHECK(off == 123.0);                                      // untouched on refusal
    CHECK(sm::offsetFromMeasurement(-107.0, -92.5, off));
    CHECK_NEAR(off, -14.5, 1e-12);
}

void testTableBandsAndText() {
    sm::CalTable t;
    CHECK(t.size() == 0 && t.find(14e6) == nullptr);
    sm::CalEntry hf; hf.loHz = 0.0; hf.hiHz = 30e6; hf.offsetDb = -33.0; hf.gainDb = 30.0;
    sm::CalEntry vhf; vhf.loHz = 30e6; vhf.hiHz = 1.0e9; vhf.offsetDb = -25.0; vhf.gainDb = 30.0;
    CHECK(t.set(hf) && t.set(vhf) && t.size() == 2);
    // Per-band offset: 14 MHz uses the HF entry, 145 MHz the VHF entry, and the edge is [lo, hi).
    CHECK_NEAR(t.dbm(-40.0, 14e6, 30.0).dbm, -73.0, 1e-9);
    CHECK_NEAR(t.dbm(-40.0, 145e6, 30.0).dbm, -65.0, 1e-9);
    CHECK_NEAR(t.dbm(-40.0, 29.999999e6, 30.0).dbm, -73.0, 1e-9);
    CHECK_NEAR(t.dbm(-40.0, 30.0e6, 30.0).dbm, -65.0, 1e-9);
    // Outside every band: uncalibrated.
    CHECK(!t.dbm(-40.0, 2.0e9, 30.0).calibrated);
    CHECK(!t.dbm(-40.0, std::numeric_limits<double>::quiet_NaN(), 30.0).calibrated);
    // Setting the same band replaces; an overlapping narrower band wins inside itself.
    hf.offsetDb = -30.0;
    CHECK(t.set(hf) && t.size() == 2);
    CHECK_NEAR(t.dbm(-40.0, 14e6, 30.0).dbm, -70.0, 1e-9);
    sm::CalEntry narrow; narrow.loHz = 14.0e6; narrow.hiHz = 14.35e6; narrow.offsetDb = -20.0; narrow.gainDb = 30.0;
    CHECK(t.set(narrow) && t.size() == 3);
    CHECK_NEAR(t.dbm(-40.0, 14.1e6, 30.0).dbm, -60.0, 1e-9);
    CHECK_NEAR(t.dbm(-40.0, 7.1e6, 30.0).dbm, -70.0, 1e-9);
    // clearAt removes the entry find() returns.
    CHECK(t.clearAt(14.1e6) && t.size() == 2);
    CHECK_NEAR(t.dbm(-40.0, 14.1e6, 30.0).dbm, -70.0, 1e-9);
    CHECK(!t.clearAt(3.0e9));
    // Invalid entries are refused and change nothing.
    sm::CalEntry bad = hf;
    bad.hiHz = bad.loHz;                       CHECK(!t.set(bad));
    bad = hf; bad.loHz = -1.0;                 CHECK(!t.set(bad));
    bad = hf; bad.offsetDb = 250.0;            CHECK(!t.set(bad));
    bad = hf; bad.gainDb = std::numeric_limits<double>::quiet_NaN(); CHECK(!t.set(bad));
    bad = hf; bad.hiHz = std::numeric_limits<double>::infinity();    CHECK(!t.set(bad));
    CHECK(t.size() == 2);
    // The table is bounded.
    sm::CalTable full;
    for (std::size_t i = 0; i < sm::CalTable::kMaxEntries; ++i) {
        sm::CalEntry x; x.loHz = static_cast<double>(i) * 1e6; x.hiHz = x.loHz + 1e6; CHECK(full.set(x));
    }
    sm::CalEntry extra; extra.loHz = 1e9; extra.hiHz = 2e9;
    CHECK(!full.set(extra) && full.size() == sm::CalTable::kMaxEntries);
    // Text round trip.
    sm::CalTable u;
    CHECK(u.fromText(t.toText()) && u.size() == t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        CHECK(u.entries()[i].loHz == t.entries()[i].loHz && u.entries()[i].hiHz == t.entries()[i].hiHz);
        CHECK_NEAR(u.entries()[i].offsetDb, t.entries()[i].offsetDb, 1e-5);
        CHECK(u.entries()[i].gainIsDb == t.entries()[i].gainIsDb);
    }
    // JUNK: any bad line refuses the whole text and leaves the table as it was.
    const std::string junk[] = {"garbage\n", "1 2 3\n", "1e6 2e6 -33 30 7\n", "1e6 2e6 nan 30 1\n", "2e6 1e6 -33 30 1\n",
                                "1e6 2e6 -33 30 1 extra\n", "1e6 2e6 -33 30 1\n1e6 2e6 xx 30 1\n", "1e6 2e6 900 30 1\n"};
    for (const std::string& j : junk) {
        sm::CalTable v;
        v.set(hf);
        CHECK(!v.fromText(j));
        CHECK(v.size() == 1);
    }
    sm::CalTable w;
    CHECK(w.fromText("") && w.size() == 0);
    CHECK(w.fromText("\n\n  \r\n") && w.size() == 0);
    CHECK(w.fromText("1000000 2000000 -33.5 30 1\r\n\r\n3000000 4000000 -20 0 0\n") && w.size() == 2 && !w.entries()[1].gainIsDb);
    std::string many;
    for (int i = 0; i < 65; ++i) { many += std::to_string(i * 1000000) + " " + std::to_string(i * 1000000 + 500000) + " 0 0 1\n"; }
    CHECK(!w.fromText(many) && w.size() == 2);
}

void testNoAllocation() {
    sm::CalTable t;
    sm::CalEntry e; e.loHz = 0; e.hiHz = 1e9; e.offsetDb = -33.0;
    t.set(e);
    AllocProbe probe;
    double acc = 0;
    for (int i = 0; i < 1000; ++i) {
        acc += t.dbm(-40.0 + i * 0.01, 14e6, 30.0).dbm;
        acc += sm::dbmToS(-60.0 - i * 0.01, 14e6).units;
        acc += sm::sToDbm(5, 0, 14e6);
    }
    CHECK(probe.news() == 0);
    CHECK(acc != 0.0);
}

}  // namespace

int main() {
    testFixedPointsHf();
    testFixedPointsVhfAndBoundary();
    testRoundingAndClamps();
    testOffsetAndGainCompensation();
    testTableBandsAndText();
    testNoAllocation();
    return testSummary("test_smeter_cal");
}
