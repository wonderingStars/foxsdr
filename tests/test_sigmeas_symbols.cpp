// Tests for dsp/sigmeas_symbols.hpp: cursor period/rate, bit-period fit, symbol extraction to a
// file, FSK analysis and the OOK/ASK pulse-width slicer. Baud pinned within 0.5 % on generated
// signals with +-8 % edge jitter, noise, a carrier offset and a 300 ppm clock error.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_symbols.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define SIGMEAS_PID() _getpid()
#else
#include <unistd.h>
#define SIGMEAS_PID() getpid()
#endif

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

double relErr(double got, double want) { return std::fabs(got - want) / want; }

// Smallest bit-error rate of `got` against `sent` over start offsets 0..maxShift of `sent`.
double bestBer(const std::vector<std::uint8_t>& got, const std::vector<int>& sent, std::size_t maxShift) {
    double best = 1.0;
    const std::size_t m = std::min<std::size_t>(got.size(), 400);
    if (m < 50) { return 1.0; }
    for (std::size_t s = 0; s <= maxShift && s + m <= sent.size(); ++s) {
        std::size_t err = 0;
        for (std::size_t i = 0; i < m; ++i) {
            if (got[i] != sent[s + i]) { ++err; }
        }
        best = std::min(best, static_cast<double>(err) / static_cast<double>(m));
    }
    return best;
}

std::string scratchName(const char* tag) {
    return std::string("sigmeas-") + tag + "-" + std::to_string(static_cast<long>(SIGMEAS_PID())) + ".tmp";
}

}  // namespace

int main() {
    // --- cursors ---------------------------------------------------------------------------------
    {
        const CursorPeriod c = periodFromCursors(0.010, 0.0225, 15.0);
        CHECK(c.valid);
        CHECK_NEAR(c.periodS, 0.0125 / 15.0, 1e-12);
        CHECK_NEAR(c.rateHz, 1200.0, 1e-6);
        CHECK(!periodFromCursors(0.02, 0.01, 4.0).valid);  // backwards
        CHECK(!periodFromCursors(0.0, 0.01, 0.0).valid);   // no symbols
        CHECK(!periodFromCursors(0.0, std::nan(""), 4.0).valid);
        CHECK(std::string(periodFromCursors(0.0, 0.01, -2.0).reason) == why::kBadParam);
    }

    // --- bit period from run lengths ---------------------------------------------------------------
    {
        sg::Rng rng(31);
        std::vector<double> runs;
        const double T = 37.3;
        for (int i = 0; i < 200; ++i) {
            const int u = 1 + static_cast<int>(rng.uni() * 4.0);
            runs.push_back(u * T + T * 0.06 * rng.gauss());
        }
        const PeriodFit f = fitSymbolPeriod(runs);
        CHECK(f.valid);
        CHECK(relErr(f.period, T) < 0.005);
        CHECK(!f.ambiguous);
        // every run the same length: the fit takes that length as the period and says it is ambiguous
        std::vector<double> even;
        for (int i = 0; i < 100; ++i) { even.push_back(2.0 * T); }
        const PeriodFit e = fitSymbolPeriod(even);
        CHECK(e.valid && e.ambiguous);   // 0101...: every run alike
        CHECK(relErr(e.period, 2.0 * T) < 0.005);
        // a snap of a 4 %-off guess
        const PeriodFit g = refineCursorPeriod(runs, T * 1.04);
        CHECK(g.valid);
        CHECK(relErr(g.period, T) < 0.005);
        // failure paths
        CHECK(std::string(fitSymbolPeriod({1.0, 2.0}).reason) == why::kTooShort);
        CHECK(std::string(fitSymbolPeriod({}).reason) == why::kTooShort);
        CHECK(std::string(fitSymbolPeriod({1.0, -2.0, 3.0}).reason) == why::kBadParam);
        CHECK(std::string(fitSymbolPeriod({1.0, std::nan(""), 3.0}).reason) == why::kBadParam);
        CHECK(std::string(fitSymbolPeriod(runs, 0.0).reason) == why::kBadParam);
        // runs with no common unit
        const std::vector<double> chaos = {10.0, 27.0, 43.0, 14.0, 31.0, 19.0, 52.0, 23.0, 37.0, 11.0, 29.0, 47.0};
        const PeriodFit c = fitSymbolPeriod(chaos, 8.0);
        CHECK(!c.valid);
        CHECK(std::string(c.reason) == why::kNoStructure);
        CHECK(!refineCursorPeriod({}, 10.0).valid);
        CHECK(!refineCursorPeriod(runs, -1.0).valid);
    }

    // --- symbol extraction and the file ---------------------------------------------------------------
    {
        std::vector<double> track(1000);
        for (std::size_t i = 0; i < track.size(); ++i) {
            track[i] = (static_cast<std::size_t>(static_cast<double>(i) / 20.0) % 2 == 0) ? 1.0 : -1.0;
        }
        const std::vector<float> v = extractSymbols(track, 0.0, 20.0);
        CHECK(v.size() == 50);
        bool ok = v.size() == 50;
        for (std::size_t i = 0; ok && i < v.size(); ++i) { ok = (i % 2 == 0) ? v[i] > 0.99f : v[i] < -0.99f; }
        CHECK(ok);
        CHECK(extractSymbols(track, 0.0, 20.0, 7).size() == 7);
        CHECK(extractSymbols({}, 0.0, 20.0).empty());
        CHECK(extractSymbols(track, 0.0, 0.0).empty());
        CHECK(extractSymbols(track, std::nan(""), 20.0).empty());

        const std::string fn = scratchName("f32");
        CHECK(writeSymbolFile(fn, v, SymbolFileFormat::Float32LE));
        {
            std::ifstream in(fn, std::ios::binary);
            std::vector<float> back(v.size());
            in.read(reinterpret_cast<char*>(back.data()), static_cast<std::streamsize>(back.size() * 4));
            CHECK(static_cast<std::size_t>(in.gcount()) == v.size() * 4);
            CHECK(back == v);
        }
        const std::string ft = scratchName("txt");
        CHECK(writeSymbolFile(ft, v, SymbolFileFormat::Text));
        {
            std::ifstream in(ft);
            std::vector<float> back;
            float f;
            while (in >> f) { back.push_back(f); }
            CHECK(back.size() == v.size());
            CHECK(!back.empty() && std::fabs(back[0] - v[0]) < 1e-5f);
        }
        const std::string fb = scratchName("bit");
        CHECK(writeSymbolFile(fb, v, SymbolFileFormat::BitString, 0.0));
        {
            std::ifstream in(fb);
            std::string all, line;
            while (std::getline(in, line)) { all += line; }
            CHECK(all.size() == v.size());
            CHECK(all.substr(0, 6) == "101010");
        }
        std::remove(fn.c_str());
        std::remove(ft.c_str());
        std::remove(fb.c_str());
        CHECK(!writeSymbolFile("Z:/no/such/folder/sigmeas-x.bin", v, SymbolFileFormat::Float32LE));
    }

    // --- FSK ------------------------------------------------------------------------------------------------
    struct F { double baud, rate, shift, fo, snr; };
    for (const F c : {F{1200.0, 48000.0, 1000.0, 2000.0, 28.0}, F{4800.0, 96000.0, 3000.0, -5000.0, 32.0},
                      F{45.45, 8000.0, 170.0, 300.0, 26.0}}) {
        sg::Rng rng(32);
        const double e = 300e-6;
        const std::size_t n = static_cast<std::size_t>(c.rate * std::max(0.5, 400.0 / c.baud));
        std::vector<int> sent;
        auto x = sg::fsk(n, c.rate * (1.0 + e), c.fo, c.shift, c.baud, 0.08, rng, &sent);
        sg::addNoise(x, c.snr, rng);
        const FskResult r = analyseFsk(x.data(), x.size(), c.rate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(relErr(r.baudHz, c.baud / (1.0 + e)) < 0.005);
            CHECK(relErr(r.shiftHz, c.shift / (1.0 + e)) < 0.01);
            CHECK_NEAR(r.centreHz, c.fo / (1.0 + e), 0.03 * c.shift);
            CHECK(r.markHz > r.spaceHz);
            CHECK(r.jitterUi < 0.12);
            CHECK(bestBer(r.bits, sent, 40) < 0.02);
            if (relErr(r.baudHz, c.baud / (1.0 + e)) >= 0.005) {
                std::printf("  fsk baud %.2f got %.3f\n", c.baud, r.baudHz);
            }
        } else {
            std::printf("  fsk baud %.2f refused: %s\n", c.baud, r.reason);
        }
    }
    {
        // an unmodulated carrier and noise are not FSK; zeros have no carrier
        sg::Rng rng(33);
        auto cw = sg::fmAm(20000, 48000.0, 1000.0, 0.0, 0.0);
        sg::addNoise(cw, 20.0, rng);
        const FskResult a = analyseFsk(cw.data(), cw.size(), 48000.0);
        CHECK(!a.valid);
        std::vector<Cf> nz(20000);
        for (Cf& c : nz) { c = Cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())); }
        CHECK(!analyseFsk(nz.data(), nz.size(), 48000.0).valid);
        std::vector<Cf> zero(2000, Cf(0, 0));
        CHECK(std::string(analyseFsk(zero.data(), zero.size(), 48000.0).reason) == why::kNoSignal);
        CHECK(std::string(analyseFsk(nullptr, 0, 48000.0).reason) == why::kEmpty);
        CHECK(std::string(analyseFsk(cw.data(), cw.size(), -5.0).reason) == why::kBadRate);
        CHECK(std::string(analyseFsk(cw.data(), 100, 48000.0).reason) == why::kTooShort);
        SymbolParams bad;
        bad.tolerance = 0.0;
        CHECK(std::string(analyseFsk(cw.data(), cw.size(), 48000.0, bad).reason) == why::kBadParam);
        bad = SymbolParams{};
        bad.minSeparation = 1.5;
        CHECK(std::string(analyseFsk(cw.data(), cw.size(), 48000.0, bad).reason) == why::kBadParam);
        cw[10] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(analyseFsk(cw.data(), cw.size(), 48000.0).reason) == why::kNonFinite);
        // a baud floor the signal does not meet
        sg::Rng r2(34);
        auto x = sg::fsk(48000, 48000.0, 0.0, 1000.0, 1200.0, 0.05, r2);
        SymbolParams p;
        p.minBaudHz = 5000.0;
        CHECK(!analyseFsk(x.data(), x.size(), 48000.0, p).valid);
        // maxBits caps the bit list
        p = SymbolParams{};
        p.maxBits = 10;
        const FskResult cap = analyseFsk(x.data(), x.size(), 48000.0, p);
        CHECK(cap.valid && cap.bits.size() == 10);
    }

    // --- OOK / ASK ----------------------------------------------------------------------------------------
    {
        sg::Rng rng(35);
        const double rate = 96000.0, baud = 2000.0, e = 300e-6;
        std::vector<int> sent;
        auto x = sg::ook(96000, rate * (1.0 + e), 4000.0, baud, 0.08, rng, 3.0, &sent);
        sg::addNoise(x, 18.0, rng);
        const OokResult r = sliceOok(x.data(), x.size(), rate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(relErr(r.baudHz, baud / (1.0 + e)) < 0.005);
            CHECK(r.onLevel > 2.0 * r.offLevel);
            CHECK(r.pulseCount > 20);
            CHECK(r.jitterUi < 0.12);
            CHECK(bestBer(r.bits, sent, 40) < 0.02);
            if (relErr(r.baudHz, baud / (1.0 + e)) >= 0.005) { std::printf("  ook baud got %.3f\n", r.baudHz); }
        } else {
            std::printf("  ook refused: %s\n", r.reason);
        }
    }
    {
        // pulse-width keying (433 MHz sensors): a 1 is on-3 off-1, a 0 is on-1 off-1 (units of 250 us)
        sg::Rng rng(36);
        std::vector<int> units;
        for (int k = 0; k < 120; ++k) {
            const int bit = rng.bit();
            for (int u = 0; u < (bit ? 3 : 1); ++u) { units.push_back(1); }
            units.push_back(0);
        }
        const double rate = 250000.0, baud = 4000.0;  // unit = 250 us
        auto x = sg::ook(static_cast<std::size_t>(rate * 0.25), rate, 20000.0, baud, 0.03, rng, 4.0, nullptr, &units);
        sg::addNoise(x, 18.0, rng);
        const OokResult r = sliceOok(x.data(), x.size(), rate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(relErr(r.baudHz, baud) < 0.01);
            CHECK(r.pulseWidthKeyed);
            CHECK(relErr(r.shortWidthS, 250e-6) < 0.05);
            CHECK(relErr(r.longWidthS, 750e-6) < 0.05);
            CHECK(r.pulses.size() <= 256 && r.pulses.size() > 10);
            CHECK(!r.pulses.empty() && r.pulses[0].widthS > 0.0);
            CHECK(r.repeatPeriodS > 0.0);
        } else {
            std::printf("  pwm refused: %s\n", r.reason);
        }
    }
    {
        // an FM signal (constant envelope) has no on/off structure; noise and zeros are refused
        sg::Rng rng(37);
        auto fmx = sg::fmAm(20000, 48000.0, 0.0, 500.0, 3000.0);
        sg::addNoise(fmx, 20.0, rng);
        CHECK(!sliceOok(fmx.data(), fmx.size(), 48000.0).valid);
        std::vector<Cf> nz(20000);
        for (Cf& c : nz) { c = Cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())); }
        CHECK(!sliceOok(nz.data(), nz.size(), 48000.0).valid);
        std::vector<Cf> zero(2000, Cf(0, 0));
        CHECK(!sliceOok(zero.data(), zero.size(), 48000.0).valid);
        CHECK(std::string(sliceOok(nullptr, 0, 48000.0).reason) == why::kEmpty);
        CHECK(std::string(sliceOok(fmx.data(), fmx.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(sliceOok(fmx.data(), 50, 48000.0).reason) == why::kTooShort);
        fmx[3] = Cf(0.0f, std::numeric_limits<float>::infinity());
        CHECK(std::string(sliceOok(fmx.data(), fmx.size(), 48000.0).reason) == why::kNonFinite);
        SymbolParams bad;
        bad.maxBaudHz = -1.0;
        CHECK(std::string(sliceOok(zero.data(), zero.size(), 48000.0, bad).reason) == why::kBadParam);
    }
    return testSummary("test_sigmeas_symbols");
}
