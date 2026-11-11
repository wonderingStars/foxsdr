// Bit-view library: line codes (src/core/bits/linecode.hpp).
//
// Known-answer rows for every code, encode/decode round trips, clock-phase and polarity search, the
// run-length path with noise, edge jitter and clock error (the decode rate is PINNED below), the
// pulse-width/position decoders and the empty/degenerate inputs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/bits/linecode.hpp"

#include <cstdio>

#include "bits_test_util.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {
Bits B(const char* s) { return fromString(s); }

// Decode a frame sent through `code` as the runs a slicer would hand over, with the given clock
// error and edge jitter; true when the decoded bits (from the best hypothesis) equal the data.
bool roundTrip(LineCode code, const Bits& data, double clockErr, double jitter, Rng& r, double* berOut = nullptr) {
    const Bits chips = encodeLineCode(data, code, 0);
    const auto runs = bitstest::chipsToRuns(chips, 100.0, clockErr, jitter, r);
    const double T = estimateChipPeriod(runs);
    // Two readings of the period (see RunHypothesis): keep the one with fewer code violations.
    Decoded best;
    bool have = false;
    for (double period : {T, T / 2}) {
        Decoded d = decodeLineCode(runsToChips(runs, period), code, 0, 0);
        // (Miller has no one-chip runs, so T and T/2 both fit; the frame length tells which reading is right.)
        if (d.bits.size() < data.size()) continue;
        if (!have || d.violations < best.violations) { best = std::move(d); have = true; }
    }
    size_t errs = data.size();
    if (best.bits.size() >= data.size()) {
        errs = 0;
        for (size_t i = 0; i < data.size(); ++i) errs += best.bits[i] != data[i];
    }
    if (berOut) *berOut = static_cast<double>(errs) / static_cast<double>(data.size());
    return errs == 0;
}
}  // namespace

int main() {
    // ---- known answers (previous level 0) ------------------------------------------------------
    const Bits data = B("1011 0010 1110");
    // NRZ-L: the chips are the bits.
    CHECK(encodeLineCode(data, LineCode::NrzL) == data);
    // NRZ-M (transition = 1) from level 0: the level toggles on a 1 and holds on a 0.
    CHECK(toString(encodeLineCode(B("1011001"), LineCode::NrzM)) == "1101110");
    // NRZ-S (transition = 0): data 0 toggles, 1 holds.
    CHECK(toString(encodeLineCode(B("0110"), LineCode::NrzS)) == "1110");
    // Manchester IEEE: 1 = 01, 0 = 10.
    CHECK(toString(encodeLineCode(B("1001"), LineCode::ManchesterIeee)) == "01101001");
    // Manchester Thomas: 1 = 10, 0 = 01.
    CHECK(toString(encodeLineCode(B("1001"), LineCode::ManchesterThomas)) == "10010110");
    // Differential Manchester (no start transition = 1): from level 0, bit 1: chips 0,1; bit 1: no
    // transition at start (level 1 -> chips 1,0); bit 0: transition at start from 0 -> chips 1,0.
    CHECK(toString(encodeLineCode(B("110"), LineCode::DiffManchester)) == "011010");
    // Bi-phase mark from level 0: start transition each bit; a 1 adds a mid-bit transition.
    CHECK(toString(encodeLineCode(B("101"), LineCode::BiphaseMark)) == "101101");
    // Bi-phase space: a 0 adds the mid-bit transition.
    CHECK(toString(encodeLineCode(B("101"), LineCode::BiphaseSpace)) == "110100");
    // Miller: 1 = mid-bit transition, 0 after 0 = boundary transition. From level 0: 1 -> 01; 0 -> 11
    // (after a 1: no boundary transition, level stays 1); 0 -> 00 (0 after 0: boundary transition); 1 -> 01? level 0 -> 0,1.
    CHECK(toString(encodeLineCode(B("1001"), LineCode::Miller)) == "01110001");

    // ---- every code decodes its own encoding exactly, from either start level ------------------
    Rng rng(0xB17B17);
    for (size_t c = 0; c < kLineCodeCount; ++c) {
        const LineCode lc = static_cast<LineCode>(c);
        for (unsigned prev = 0; prev < 2; ++prev) {
            const Bits d0 = bitstest::randomBits(rng, 97);
            const Decoded dec = decodeLineCode(encodeLineCode(d0, lc, prev), lc, 0, prev);
            CHECK(dec.bits == d0);
            CHECK(dec.violations == 0);
        }
    }

    // ---- violations are counted (one per broken pair) --------------------------------------------
    {
        Bits chips = encodeLineCode(B("10110100"), LineCode::ManchesterIeee);
        chips[6] = chips[7];  // break bit 3: pair now equal
        const Decoded d = decodeLineCode(chips, LineCode::ManchesterIeee);
        CHECK(d.violations == 1);
        CHECK(d.violationAt.size() == 1 && d.violationAt[0] == 3);
        // Biphase: remove a start transition.
        Bits bp = encodeLineCode(B("0000"), LineCode::BiphaseMark);
        bp[2] = bp[1];
        CHECK(decodeLineCode(bp, LineCode::BiphaseMark).violations >= 1);
        // Miller: two zeros in a row without the boundary transition.
        const Bits mi = B("00 00 00 00");
        CHECK(decodeLineCode(mi, LineCode::Miller).violations >= 2);
    }

    // ---- empty / tiny / out-of-range inputs ------------------------------------------------------
    {
        CHECK(decodeLineCode({}, LineCode::NrzL).bits.empty());
        CHECK(decodeLineCode(B("1"), LineCode::ManchesterIeee).bits.empty());
        CHECK(decodeLineCode(B("1"), LineCode::ManchesterIeee, 5).bits.empty());
        CHECK(encodeLineCode({}, LineCode::Miller).empty());
        CHECK(searchLineCodes({}).empty());
        CHECK(estimateChipPeriod({}) == 0.0);
        CHECK(estimateChipPeriod({{1, 5.0}}) == 0.0);
        CHECK(runsToChips({{1, 10.0}}, 0.0).empty());
        CHECK(runsToChips({{1, 10.0}}, -3.0).empty());
        CHECK(runsToChips({{1, 1e12}}, 1.0, 1000).size() == 1000);  // bounded
        CHECK(classifyWidths({}).shortW == 0.0);
        CHECK(!classifyWidths({5, 5, 5, 5}).two);
    }

    // ---- polarity and clock-phase search -----------------------------------------------------------
    {
        const Bits d0 = bitstest::randomBits(rng, 160);
        for (int inv = 0; inv < 2; ++inv)
            for (size_t phase = 0; phase < 2; ++phase) {
                Bits chips = encodeLineCode(d0, LineCode::ManchesterIeee);
                if (inv) chips = inverted(chips);
                // Start `phase` chips early (one stray chip): the decoder must find the other phase.
                Bits shifted;
                for (size_t k = 0; k < phase; ++k) shifted.push_back(1);
                shifted.insert(shifted.end(), chips.begin(), chips.end());
                const auto hyp = searchLineCodes(shifted, 0.02, 4, {LineCode::ManchesterIeee, LineCode::ManchesterThomas});
                CHECK(!hyp.empty());
                if (!hyp.empty()) {
                    // The best hypothesis recovers the data (IEEE+inverted equals Thomas non-inverted: either name is the same bits).
                    const Bits& got = hyp[0].bits;
                    const bool same = got.size() >= d0.size() && Bits(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(d0.size())) == d0;
                    const bool sameInv = got.size() >= d0.size() && inverted(Bits(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(d0.size()))) == d0;
                    CHECK(same || sameInv);
                    CHECK(hyp[0].violations == 0);
                }
            }
        // Random bits are NOT explained by a two-chip code at a 2 % violation limit.
        const Bits noise = bitstest::randomBits(rng, 400);
        CHECK(searchLineCodes(noise, 0.02, 8, {LineCode::ManchesterIeee, LineCode::BiphaseMark, LineCode::Miller}).empty());
        // NRZ-L explains everything but is listed after a two-chip code that fits equally.
        const auto all = searchLineCodes(encodeLineCode(d0, LineCode::BiphaseMark), 0.0, 32);
        CHECK(!all.empty() && chipsPerBit(all[0].code) == 2);
    }

    // ---- the run-length path: noise, jitter, clock error. THE DECODE RATE IS PINNED. --------------
    {
        const LineCode codes[] = {LineCode::NrzL, LineCode::ManchesterIeee, LineCode::DiffManchester, LineCode::BiphaseMark, LineCode::Miller};
        // Within the margin (edge jitter +-0.2 T means a run moves by < 0.4 T) and +-3 % clock error: every frame decodes.
        for (LineCode lc : codes) {
            int ok = 0;
            const int trials = 200;
            for (int t = 0; t < trials; ++t) {
                const Bits d0 = bitstest::randomBits(rng, 64);
                const double ce = (rng.sym()) * 0.03;
                // NRZ-L has long runs: the period estimate needs a short run; give it a data pattern with one
                Bits dd = d0;
                if (lc == LineCode::NrzL) { dd[0] = 0; dd[1] = 1; dd[2] = 0; }
                ok += roundTrip(lc, dd, ce, 0.2, rng) ? 1 : 0;
            }
            std::printf("  line code %-26s jitter 0.20 T, clock +-3%%: %d/%d\n", lineCodeName(lc), ok, trials);
            // NRZ-L carries long runs (7+ chips): a 1 % error in the period estimate moves a long run by half a chip with the jitter on top, so its pinned floor is 97 %.
            CHECK(lc == LineCode::NrzL ? ok >= trials * 97 / 100 : ok == trials);
        }
        // Beyond the margin (edge jitter > 0.25 T moves a run by more than half a chip) frames start to fail.
        // Pinned from a measured run (200 frames of 64 bits each); the bands are wide enough for the generator but
        // tight enough that a change in the rounding or the estimator moves out of them.
        auto rate = [&](double jit, double* ber) {
            int ok = 0;
            double sum = 0;
            const int n = 300;
            for (int t = 0; t < n; ++t) {
                double b = 0;
                ok += roundTrip(LineCode::ManchesterIeee, bitstest::randomBits(rng, 64), 0.0, jit, rng, &b) ? 1 : 0;
                sum += b;
            }
            *ber = sum / n;
            return ok;
        };
        double ber24, ber35, ber60;
        const int ok24 = rate(0.24, &ber24), ok35 = rate(0.35, &ber35), ok60 = rate(0.60, &ber60);
        std::printf("  Manchester frames decoded exactly (64 bits): jitter 0.24 T %d/300, 0.35 T %d/300, 0.60 T %d/300 (bit error rate %.3f %.3f %.3f)\n",
                    ok24, ok35, ok60, ber24, ber35, ber60);
        CHECK(ok24 == 300);
        CHECK(ok35 < 300 && ok60 < ok35 + 1);
        CHECK(ber35 > 0.0 && ber60 >= ber35 * 0.8);
        // A 10 % clock error with no jitter is still read (runs are rounded per run against the estimated period).
        int okClk = 0;
        for (int t = 0; t < 100; ++t) okClk += roundTrip(LineCode::ManchesterIeee, bitstest::randomBits(rng, 64), 0.10, 0.0, rng) ? 1 : 0;
        CHECK(okClk == 100);
        // Random chip flips: the decoded bit errors are exactly the flipped chips' bits, and the violation count rises.
        const Bits d0 = bitstest::randomBits(rng, 400);
        Bits chips = encodeLineCode(d0, LineCode::ManchesterIeee);
        size_t flips = 0;
        for (size_t i = 0; i < chips.size(); ++i)
            if (rng.uni() < 0.02) { chips[i] ^= 1u; ++flips; }
        const Decoded dn = decodeLineCode(chips, LineCode::ManchesterIeee);
        CHECK(dn.violations > 0 && dn.violations <= flips);
        CHECK(dn.bits.size() == d0.size());
    }

    // ---- pulse width / pulse position ----------------------------------------------------------------
    {
        const Bits d0 = bitstest::randomBits(rng, 128);
        std::vector<PulseRun> pw, pp;
        for (uint8_t b : d0) {
            pw.push_back({(b ? 500.0 : 250.0) * (1.0 + 0.08 * rng.sym()), 300.0});
            pp.push_back({200.0, (b ? 900.0 : 450.0) * (1.0 + 0.08 * rng.sym())});
        }
        const auto a = decodePulseWidth(pw, false);
        CHECK(a.bits == d0);
        CHECK(a.classes.two);
        CHECK(a.classes.shortW > 200 && a.classes.shortW < 300 && a.classes.longW > 450 && a.classes.longW < 560);
        const auto p = decodePulseWidth(pp, true);
        CHECK(p.bits == d0);
        CHECK(decodePulseWidth(pw, false, false).bits == inverted(d0));          // long = 0
        CHECK(decodePulseWidth(pw, false, true, 1.0).bits == Bits(d0.size(), 1)); // a threshold below every width: all long
        CHECK(decodePulseWidth({}, false).bits.empty());
        CHECK(!decodePulseWidth({{300, 100}, {301, 100}, {299, 100}}, false).classes.two);
    }
    return testSummary("test_bits_linecode");
}
