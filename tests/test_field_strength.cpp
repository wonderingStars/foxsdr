// Tests for core/field_strength.hpp - loss / antenna-factor tables, the dBuV/m readout, free-space path loss and Friis.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/field_strength.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "test_check.hpp"

using namespace cascade::core;

namespace {

LossTable loadOk(const std::string& t, FreqUnit u = FreqUnit::MHz) {
    LossTable tb;
    const LossParseResult r = tb.parse(t, u);
    CHECK(r.ok());
    return tb;
}
LossParseResult loadErr(const std::string& t, FreqUnit u = FreqUnit::MHz) {
    LossTable tb;
    const LossParseResult r = tb.parse(t, u);
    CHECK(!r.ok());
    CHECK(tb.empty());
    return r;
}

void testParse() {
    std::printf("  table parse: separators, comment, one header, units\n");
    LossTable t = loadOk("Frequency,Factor\n# a user table\n30,10.0\n100; 12.5\n300\t18\n1000 25 # last\r\n");
    CHECK(t.size() == 4u);
    CHECK(t.points()[0].fHz == 30e6 && t.points()[0].db == 10.0);
    CHECK(t.points()[1].fHz == 100e6 && t.points()[1].db == 12.5);
    CHECK(t.points()[3].fHz == 1000e6 && t.points()[3].db == 25.0);
    CHECK_NEAR(t.minHz(), 30e6, 1e-3);
    CHECK_NEAR(t.maxHz(), 1000e6, 1e-3);
    CHECK(loadOk("1000,1\n2000,2\n", FreqUnit::Hz).points()[1].fHz == 2000.0);
    CHECK(loadOk("1,1\n2,2\n", FreqUnit::kHz).points()[1].fHz == 2000.0);
    CHECK(loadOk("1,1\n2,2\n", FreqUnit::GHz).points()[1].fHz == 2e9);
    CHECK(loadOk("1.5e2 , -3.5e0\n").points()[0].fHz == 150e6);
    CHECK(loadOk("100,5").size() == 1u);                          // no trailing newline
    CHECK(loadOk("100,5\n").points()[0].db == 5.0);
}

void testParseErrors() {
    std::printf("  table parse errors: each code with its line; non-monotonic data is refused, never sorted\n");
    LossParseResult r = loadErr("10,1\n20,2\n15,3\n");
    CHECK(r.error == LossError::NotIncreasing && r.line == 3u);
    CHECK(loadErr("10,1\n10,2\n").error == LossError::NotIncreasing);   // equal is not increasing
    CHECK(loadErr("10,1\nabc,2\n").error == LossError::BadNumber);
    CHECK(loadErr("Freq,AF\nFreq,AF\n10,1\n").error == LossError::BadNumber);   // a second header line
    CHECK(loadErr("10,1\nFreq,AF\n").error == LossError::BadNumber);            // a header after data
    CHECK(loadErr("10\n").error == LossError::BadNumber);
    CHECK(loadErr("10,1,5\n").error == LossError::BadNumber);
    CHECK(loadErr("10,\n").error == LossError::BadNumber);
    CHECK(loadErr("10,nan\n").error == LossError::BadNumber);
    CHECK(loadErr("10,inf\n").error == LossError::BadNumber);
    CHECK(loadErr("10,0x10\n").error == LossError::BadNumber);
    CHECK(loadErr("0,1\n").error == LossError::BadNumber);                       // f must be > 0
    CHECK(loadErr("-5,1\n").error == LossError::BadNumber);
    CHECK(loadErr("1e300,1\n").error == LossError::BadNumber);
    CHECK(loadErr("10,401\n").error == LossError::BadNumber);
    CHECK(loadErr("10,-401\n").error == LossError::BadNumber);
    CHECK(loadErr("# nothing\n\n").error == LossError::Empty);
    CHECK(loadErr("").error == LossError::Empty);
    CHECK(loadErr("Frequency,Factor\n").error == LossError::Empty);              // a header and no data
    CHECK(loadErr(std::string(kLossMaxLine + 1, '#') + "\n").error == LossError::LineTooLong);
    CHECK(loadErr(std::string(kLossMaxBytes + 1, '\n')).error == LossError::TooLarge);
    std::string many;
    for (std::size_t i = 1; i <= kLossMaxPoints; ++i) { many += std::to_string(i) + ",1\n"; }
    CHECK(loadOk(many, FreqUnit::Hz).size() == kLossMaxPoints);
    many += std::to_string(kLossMaxPoints + 1) + ",1\n";
    CHECK(loadErr(many, FreqUnit::Hz).error == LossError::TooManyPoints);
    LossTable t;
    CHECK(t.parse(nullptr, 3, FreqUnit::Hz).error == LossError::BadNumber);
    CHECK(lossErrorText(LossError::NotIncreasing) != nullptr);
    // A failed parse empties a table that held data.
    CHECK(t.parse("10,1\n20,2\n", FreqUnit::Hz).ok() && t.size() == 2u);
    CHECK(!t.parse("10,1\n5,2\n", FreqUnit::Hz).ok() && t.empty());
    // setPoints applies the same rules.
    CHECK(t.setPoints({{10, 1}, {20, 2}}).ok() && t.size() == 2u);
    CHECK(t.setPoints({}).error == LossError::Empty);
    CHECK(t.setPoints({{20, 1}, {10, 2}}).error == LossError::NotIncreasing);
    CHECK(t.setPoints({{std::nan(""), 1}}).error == LossError::BadNumber);
}

void testLookup() {
    std::printf("  lookup: linear in dB against log frequency, ends held and flagged, one point, empty\n");
    const LossTable t = loadOk("10,0\n100,20\n1000,20\n10000,0\n", FreqUnit::Hz);
    LookupResult r = t.lookup(10.0);
    CHECK(r.inRange() && r.valueDb == 0.0);
    r = t.lookup(100.0);
    CHECK(r.inRange() && r.valueDb == 20.0);
    r = t.lookup(std::sqrt(10.0 * 100.0));                          // half way in log f between 10 and 100
    CHECK(r.inRange());
    CHECK_NEAR(r.valueDb, 10.0, 1e-9);
    r = t.lookup(55.0);                                             // half way in LINEAR f would be 10 dB: log f says more
    CHECK_NEAR(r.valueDb, 20.0 * std::log10(5.5), 1e-9);
    CHECK(t.lookup(5.0).status == LookupStatus::Below && t.lookup(5.0).valueDb == 0.0);
    CHECK(t.lookup(20000.0).status == LookupStatus::Above && t.lookup(20000.0).valueDb == 0.0);
    CHECK(t.lookup(10000.0).inRange());
    CHECK(t.lookup(0.0).status == LookupStatus::Empty);
    CHECK(t.lookup(-3.0).status == LookupStatus::Empty);
    CHECK(t.lookup(std::nan("")).status == LookupStatus::Empty);
    CHECK(LossTable{}.lookup(100.0).status == LookupStatus::Empty);
    const LossTable one = loadOk("100,7\n", FreqUnit::Hz);
    CHECK(one.lookup(100.0).inRange() && one.lookup(100.0).valueDb == 7.0);
    CHECK(one.lookup(50.0).status == LookupStatus::Below && one.lookup(50.0).valueDb == 7.0);
    CHECK(one.lookup(500.0).status == LookupStatus::Above);
    // Monotone between points of a monotone table (no overshoot from the interpolation).
    const LossTable m = loadOk("1,0\n10,5\n100,9\n1000,30\n", FreqUnit::MHz);
    double prev = -1e9;
    for (double f = 1e6; f <= 1e9; f *= 1.07) {
        const double v = m.lookup(f).valueDb;
        CHECK(v >= prev - 1e-12);
        prev = v;
    }
}

void testFormulas() {
    std::printf("  formulas: dBm->dBuV, antenna factor 9.73/(lambda sqrt G), FSPL constants, EIRP field, and the loop closed\n");
    CHECK_NEAR(dbmToDbuv(0.0), 106.9897, 1e-3);                     // 1 mW into 50 ohm = 223.6 mV = 106.99 dBuV
    CHECK_NEAR(dbuvToDbm(dbmToDbuv(-73.2, 75.0), 75.0), -73.2, 1e-9);
    CHECK_NEAR(dbmToDbuv(0.0, 75.0), 108.7506, 1e-3);
    // V = sqrt(P R): 1 mW into 50 ohm.
    CHECK_NEAR(20.0 * std::log10(std::sqrt(0.001 * 50.0) * 1e6), dbmToDbuv(0.0), 1e-9);
    // Antenna factor: the page's 9.73 / (lambda sqrt G) at 50 ohm.
    for (double f : {30e6, 100e6, 433.92e6, 1e9}) {
        for (double g : {0.0, 2.15, 9.0}) {
            const double lam = kSpeedOfLight / f;
            const double afPage = 20.0 * std::log10(9.73 / (lam * std::sqrt(std::pow(10.0, g / 10.0))));
            CHECK_NEAR(antennaFactorFromGainDb(f, g), afPage, 0.01);
            CHECK_NEAR(gainDbiFromAntennaFactor(f, antennaFactorFromGainDb(f, g)), g, 1e-9);
        }
    }
    CHECK_NEAR(antennaFactorFromGainDb(100e6, 0.0), 20.0 * std::log10(100.0) - 29.79, 0.02);   // the usual 20 log f(MHz) - G - 29.79
    // Free-space path loss: the constants of the page.
    CHECK_NEAR(fsplDb(1.0, 1.0), -147.5522, 1e-3);
    CHECK_NEAR(fsplDb(1000.0, 1e9), 92.4478, 1e-3);                 // d in km, f in GHz: +92.45
    CHECK_NEAR(fsplDb(10.0, 100e6) - fsplDb(1.0, 100e6), 20.0, 1e-9);   // 20 dB per decade of distance
    CHECK_NEAR(fsplDb(1.0, 200e6) - fsplDb(1.0, 100e6), 6.0206, 1e-3);
    CHECK(fsplDb(0.0, 1e6) == 0.0 && fsplDb(1.0, 0.0) == 0.0 && fsplDb(std::nan(""), 1e6) == 0.0 && fsplDb(-1.0, 1e6) == 0.0);
    // The 1 mW / 1 m field: 104.77 dBuV/m.
    CHECK_NEAR(fieldFromEirpDbuvPerM(0.0, 1.0), 104.77, 0.01);
    CHECK_NEAR(fieldFromEirpDbuvPerM(0.0, 10.0), 84.77, 0.01);
    CHECK_NEAR(fieldFromEirpDbuvPerM(30.0, 1.0), 134.77, 0.01);
    CHECK(fieldFromEirpDbuvPerM(0.0, 0.0) == 0.0);
    // THE LOOP: EIRP -> Friis -> volts at 50 ohm -> + antenna factor of the receiving antenna = the field of the radiator. Independent of
    // the receiving gain, because the factor and the received power cancel it.
    for (double f : {30e6, 145e6, 433.92e6, 2.4e9}) {
        for (double d : {1.0, 3.0, 30.0}) {
            for (double gr : {0.0, 6.0}) {
                const double eirp = 13.0;
                const double pr = friisReceivedDbm(eirp, 0.0, gr, d, f);          // EIRP already holds the transmit gain
                const double v = dbmToDbuv(pr);
                const double e = v + antennaFactorFromGainDb(f, gr);
                CHECK_NEAR(e, fieldFromEirpDbuvPerM(eirp, d), 1e-6);
            }
        }
    }
}

void testReadout() {
    std::printf("  readout from tables: AF + cable - preamp, held values flagged, empty tables\n");
    const LossTable af = loadOk("30,15\n300,24\n1000,35\n", FreqUnit::MHz);
    const LossTable cable = loadOk("30,0.5\n1000,3\n", FreqUnit::MHz);
    FieldReadout r = fieldStrengthFromTables(-60.0, 300e6, af, cable);
    CHECK(r.valid && r.allInRange());
    CHECK_NEAR(r.afDb, 24.0, 1e-9);
    CHECK_NEAR(r.cableLossDb, cable.lookup(300e6).valueDb, 1e-12);
    CHECK_NEAR(r.dbuvPerM, dbmToDbuv(-60.0) + 24.0 + r.cableLossDb, 1e-9);
    r = fieldStrengthFromTables(-60.0, 300e6, af, cable, 20.0);          // a 20 dB preamp ahead of the analyser
    CHECK_NEAR(r.dbuvPerM, dbmToDbuv(-60.0) + 24.0 + r.cableLossDb - 20.0, 1e-9);
    r = fieldStrengthFromTables(-60.0, 10e6, af, cable);                 // below both tables
    CHECK(r.valid && r.afHeld && r.cableHeld && !r.allInRange());
    CHECK_NEAR(r.afDb, 15.0, 1e-12);
    r = fieldStrengthFromTables(-60.0, 300e6, af, LossTable{});          // no cable table: no cable term
    CHECK(r.valid && r.allInRange() && r.cableLossDb == 0.0);
    CHECK(!fieldStrengthFromTables(-60.0, 300e6, LossTable{}, cable).valid);
    CHECK(!fieldStrengthFromTables(-60.0, 0.0, af, cable).valid);
    CHECK(!fieldStrengthFromTables(std::nan(""), 300e6, af, cable).valid);
    r = fieldStrengthFromTables(-60.0, 300e6, af, cable, 0.0, 75.0);
    CHECK_NEAR(r.dbuvPerM, dbmToDbuv(-60.0, 75.0) + 24.0 + r.cableLossDb, 1e-9);
}

void testFileLoader() {
    std::printf("  file loader\n");
    namespace fs = std::filesystem;
    const std::string stem = "rfmon-loss-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path ok = fs::path(stem + "-ok.tmp");
    const fs::path big = fs::path(stem + "-big.tmp");
    {
        std::ofstream f(ok, std::ios::binary);
        f << "10,1\n20,2\n";
    }
    {
        std::ofstream f(big, std::ios::binary);
        f << std::string(kLossMaxBytes + 10, ' ');
    }
    LossTable t;
    CHECK(t.loadFile(ok.string(), FreqUnit::MHz).ok() && t.size() == 2u);
    CHECK(t.loadFile(big.string(), FreqUnit::MHz).error == LossError::TooLarge && t.empty());
    CHECK(t.loadFile(stem + "-missing.tmp", FreqUnit::MHz).error == LossError::Unreadable);
    std::error_code ec;
    fs::remove(ok, ec);
    fs::remove(big, ec);
}

void testFuzz() {
    std::printf("  fuzz: random bytes and mutated tables never crash; whatever parses is strictly increasing and in range\n");
    std::mt19937 g(91);
    const std::string seed = "Frequency,Factor\n# user\n30,10.5\n100;12\n300\t18\n1000 25 # end\n";
    std::size_t parsed = 0, refused = 0;
    for (int it = 0; it < 20000; ++it) {
        std::string s;
        if (it % 3 == 0) {
            const std::size_t len = g() % 300;
            for (std::size_t i = 0; i < len; ++i) { s.push_back(static_cast<char>(g() & 0xFF)); }
        } else {
            s = seed;
            const int muts = 1 + static_cast<int>(g() % 4);
            for (int k = 0; k < muts && !s.empty(); ++k) {
                const std::size_t at = g() % s.size();
                switch (g() % 4) {
                    case 0: s[at] = static_cast<char>(g() & 0xFF); break;
                    case 1: s.erase(at, 1 + g() % 6); break;
                    case 2: s.insert(at, 1, "0123456789-+.eE ,;\t#\n"[g() % 21]); break;
                    default: s.resize(at); break;
                }
            }
        }
        LossTable t;
        const LossParseResult r = t.parse(s, FreqUnit::MHz);
        if (r.ok()) {
            ++parsed;
            CHECK(!t.empty());
            double prev = 0;
            for (const auto& p : t.points()) {
                CHECK(std::isfinite(p.fHz) && p.fHz > prev && p.fHz <= kLossMaxFreqHz);
                CHECK(std::isfinite(p.db) && std::fabs(p.db) <= kLossMaxValueDb);
                prev = p.fHz;
            }
            const LookupResult l = t.lookup(t.minHz());
            CHECK(l.inRange() || t.size() == 1u);
            CHECK(std::isfinite(l.valueDb));
        } else {
            ++refused;
            CHECK(t.empty());
        }
    }
    CHECK(parsed > 100u);
    CHECK(refused > 1000u);
}

}  // namespace

int main() {
    testParse();
    testParseErrors();
    testLookup();
    testFormulas();
    testReadout();
    testFileLoader();
    testFuzz();
    return testSummary("test_field_strength");
}
