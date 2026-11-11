// Tests for dsp/sigmeas_cursors.hpp: harmonic and sideband cursors, level readout under a spectrum,
// and the eight-cursor model (free, locked spacing, harmonic).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_cursors.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

// complex tones (freq, amplitude) in noise-free sum
std::vector<Cf> tones(std::size_t n, double rate, const std::vector<std::pair<double, double>>& t) {
    std::vector<Cf> x(n, Cf(0, 0));
    for (const auto& ta : t) {
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = sg::kTwoPi * ta.first * static_cast<double>(i) / rate + 0.37 * ta.first;
            x[i] += Cf(static_cast<float>(ta.second * std::cos(ph)), static_cast<float>(ta.second * std::sin(ph)));
        }
    }
    return x;
}

}  // namespace

int main() {
    // --- harmonic cursors ----------------------------------------------------------------------------------------------
    {
        const auto m = harmonicCursors(1500.0, 0.0, 10000.0);
        CHECK(m.size() == 6);   // 1.5 .. 9 kHz
        if (m.size() == 6) {
            CHECK(m[0].kind == MarkKind::Fundamental && m[0].order == 1);
            CHECK(m[1].kind == MarkKind::Harmonic && m[1].order == 2);
            CHECK_NEAR(m[5].freqHz, 9000.0, 1e-9);
        }
        // a window that starts above the fundamental: the harmonics inside it only
        const auto w = harmonicCursors(1500.0, 4000.0, 8000.0);
        CHECK(w.size() == 3 && w[0].order == 3);
        // negative base (a negative baseband frequency): harmonics walk down
        const auto neg = harmonicCursors(-2000.0, -9000.0, 0.0);
        CHECK(neg.size() == 4 && neg[3].freqHz == -8000.0);
        CHECK(harmonicCursors(1000.0, 0.0, 100000.0, 5).size() == 5);   // capped
        CHECK(harmonicCursors(0.0, 0.0, 100.0).empty());
        CHECK(harmonicCursors(std::nan(""), 0.0, 100.0).empty());
        CHECK(harmonicCursors(10.0, 100.0, 0.0).empty());
        CHECK(harmonicCursors(10.0, 1000.0, 2000.0, 200).size() == 101);   // 1000..2000 step 10, k=100..200
    }
    // --- sideband cursors ---------------------------------------------------------------------------------------------------
    {
        const auto m = sidebandCursors(5000.0, 300.0, 3, 0.0, 20000.0);
        CHECK(m.size() == 7);
        if (m.size() == 7) {
            CHECK(m[0].kind == MarkKind::Carrier && m[0].freqHz == 5000.0);
            CHECK(m[1].kind == MarkKind::UpperSideband && m[1].freqHz == 5300.0 && m[1].order == 1);
            CHECK(m[2].kind == MarkKind::LowerSideband && m[2].freqHz == 4700.0);
            CHECK_NEAR(m[5].freqHz, 5900.0, 1e-9);
        }
        CHECK(sidebandCursors(5000.0, 300.0, 3, 4800.0, 5400.0).size() == 2);   // carrier and +300 only
        CHECK(sidebandCursors(5000.0, 0.0, 3, 0.0, 10000.0).empty());
        CHECK(sidebandCursors(5000.0, 300.0, 0, 0.0, 10000.0).empty());
        CHECK(sidebandCursors(std::nan(""), 300.0, 3, 0.0, 10000.0).empty());
        CHECK(sidebandCursors(5000.0, 300.0, 3, 10.0, 5.0).empty());
    }
    // --- levels under a spectrum -----------------------------------------------------------------------------------------------
    {
        const double rate = 48000.0;
        // fundamental 2000.4 Hz amplitude 0.5, second harmonic 0.05 (-20 dB), third 0.005 (-40 dB)
        const auto x = tones(65536, rate, {{2000.4, 0.5}, {4000.8, 0.05}, {6001.2, 0.005}});
        const Psd psd = welchPsd(x.data(), x.size(), rate, 8192);
        CHECK(psd.valid);
        auto marks = harmonicCursors(2000.0, 0.0, 7000.0);   // hand-placed 0.4 Hz off: the snap finds the line
        CHECK(marks.size() == 3);
        readLevels(psd, marks, 100.0);
        if (marks.size() == 3) {
            CHECK_NEAR(marks[0].snappedHz, 2000.4, 3.0);
            CHECK_NEAR(marks[1].snappedHz, 4000.8, 3.0);
            CHECK_NEAR(marks[2].snappedHz, 6001.2, 3.0);
            CHECK_NEAR(marks[0].levelDbfs - marks[1].levelDbfs, 20.0, 1.0);
            CHECK_NEAR(marks[0].levelDbfs - marks[2].levelDbfs, 40.0, 1.0);
        }
        double s, l;
        CHECK(!snapToPeak(Psd{}, 1000.0, 50.0, &s, &l) && std::isnan(s) && std::isnan(l));
        CHECK(!snapToPeak(psd, std::nan(""), 50.0, &s, &l));
        CHECK(!snapToPeak(psd, 1000.0, -5.0, &s, &l));
        CHECK(!snapToPeak(psd, 1e9, 50.0, &s, &l));   // far outside the spectrum
    }
    // --- the eight cursors: free ---------------------------------------------------------------------------------------------------
    {
        CursorSet8 c;
        CHECK(c.mode() == CursorMode::Free);
        CHECK(c.setCursor(3, 7777.0));
        CHECK_NEAR(c.frequency(3), 7777.0, 1e-12);
        CHECK_NEAR(c.frequency(2), 3000.0, 1e-12);   // the others stay
        CHECK(c.setReference(2));
        const auto r = c.readout();
        CHECK_NEAR(r[3].offsetHz, 7777.0 - 3000.0, 1e-9);
        CHECK_NEAR(r[3].ratio, 7777.0 / 3000.0, 1e-12);
        CHECK_NEAR(r[2].offsetHz, 0.0, 1e-12);
        CHECK_NEAR(c.offsetHz(5, 0), 5000.0, 1e-12);
        CHECK_NEAR(c.ratio(7, 0), 8.0, 1e-12);
        CHECK(c.spacingRmsHz() > 100.0);   // not regular after the move
        // refusals leave it unchanged
        CHECK(!c.setCursor(8, 1.0));
        CHECK(!c.setCursor(0, std::nan("")));
        CHECK(!c.setCursor(0, std::numeric_limits<double>::infinity()));
        CHECK(!c.setReference(8));
        CHECK_NEAR(c.frequency(0), 1000.0, 1e-12);
        CHECK(std::isnan(c.frequency(8)) && std::isnan(c.offsetHz(8, 0)) && std::isnan(c.ratio(0, 8)));
        // the range flags
        CHECK(c.setRange(1500.0, 6500.0));
        const auto rr = c.readout();
        CHECK(!rr[0].inRange && rr[1].inRange && !rr[3].inRange);
        CHECK(!c.setRange(5.0, 5.0));
        CHECK(!c.setRange(std::nan(""), 5.0));
    }
    // --- locked spacing ----------------------------------------------------------------------------------------------------------------
    {
        CursorSet8 c;
        CHECK(c.setCursor(0, 2000.0) && c.setCursor(1, 2500.0));   // free: now not uniform at the front
        CHECK(c.setMode(CursorMode::LockedSpacing));
        // the mean spacing of (2000, 2500, 3000, ..., 8000) = (8000 - 2000) / 7
        CHECK_NEAR(c.spacingHz(), 6000.0 / 7.0, 1e-9);
        CHECK_NEAR(c.spacingRmsHz(), 0.0, 1e-9);
        CHECK(c.setSpacing(500.0));
        CHECK_NEAR(c.frequency(0), 2000.0, 1e-9);
        CHECK_NEAR(c.frequency(7), 5500.0, 1e-9);
        // moving cursor 4 to 4000 moves them all: cursor 0 = 4000 - 4*500
        CHECK(c.setCursor(4, 4000.0));
        CHECK_NEAR(c.frequency(0), 2000.0, 1e-9);
        CHECK(c.setCursor(4, 4600.0));
        CHECK_NEAR(c.frequency(0), 2600.0, 1e-9);
        CHECK_NEAR(c.frequency(7), 6100.0, 1e-9);
        CHECK_NEAR(c.spacingRmsHz(), 0.0, 1e-9);
        CHECK(c.setSpacing(-250.0) && std::fabs(c.frequency(7) - (2600.0 - 7 * 250.0)) < 1e-9);   // a negative pitch is legal
        CHECK(!c.setSpacing(std::nan("")));
    }
    // --- harmonic mode -------------------------------------------------------------------------------------------------------------------
    {
        CursorSet8 c;
        CHECK(c.setCursor(0, 1234.5));
        CHECK(c.setMode(CursorMode::Harmonic));
        for (std::size_t i = 0; i < 8; ++i) { CHECK_NEAR(c.frequency(i), 1234.5 * static_cast<double>(i + 1), 1e-9); }
        // dragging the 3rd harmonic to 5000 makes the fundamental 5000/3
        CHECK(c.setCursor(2, 5000.0));
        CHECK_NEAR(c.baseHz(), 5000.0 / 3.0, 1e-9);
        CHECK_NEAR(c.frequency(7), 8.0 * 5000.0 / 3.0, 1e-9);
        const auto r = c.readout();
        for (std::size_t i = 0; i < 8; ++i) { CHECK_NEAR(r[i].ratio, static_cast<double>(i + 1), 1e-9); }
        CHECK(!c.setSpacing(100.0));    // a harmonic set's pitch is its base
        CHECK(!c.setCursor(1, 0.0));    // a zero base would collapse the set
        CursorSet8 z;
        z.setCursor(0, 0.0);
        CHECK(!z.setMode(CursorMode::Harmonic));
        CHECK(z.mode() == CursorMode::Free);
    }
    // --- snapping the set to real lines (eight lines 1 kHz apart) and reading their offsets --------------------------------------------------
    {
        const double rate = 48000.0;
        std::vector<std::pair<double, double>> lines;
        for (int k = 0; k < 8; ++k) { lines.push_back({-3500.0 + 1000.0 * k + 0.7 * k, 0.1 + 0.05 * k}); }
        const auto x = tones(65536, rate, lines);
        const Psd psd = welchPsd(x.data(), x.size(), rate, 8192);
        CursorSet8 c;
        for (std::size_t i = 0; i < 8; ++i) { c.setCursor(i, -3500.0 + 1000.0 * static_cast<double>(i) + 40.0); }   // 40 Hz off each
        CHECK(c.snapToPeaks(psd, 100.0));
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK_NEAR(c.frequency(i), lines[i].first, 3.0);
        }
        const auto r = c.readout();
        CHECK_NEAR(r[7].offsetHz, 7000.0 + 0.7 * 7, 5.0);
        for (std::size_t i = 1; i < 8; ++i) { CHECK(r[i].levelDbfs > r[i - 1].levelDbfs); }   // amplitudes rise with k
        CHECK_NEAR(c.meanSpacingHz(), 1000.7, 1.0);
        // levels read without snapping, in a locked set
        CHECK(c.setMode(CursorMode::LockedSpacing));
        CHECK(!c.snapToPeaks(psd, 100.0));        // a snap would break the lock
        CHECK(c.layLevels(psd, 100.0));
        CHECK(std::isfinite(c.readout()[0].levelDbfs));
        CHECK(!c.layLevels(Psd{}, 100.0));
        CursorSet8 free2;
        CHECK(!free2.snapToPeaks(Psd{}, 100.0));
    }
    return testSummary("test_sigmeas_cursors");
}
