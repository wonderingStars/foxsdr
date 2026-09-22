// Tests for core/patch_levels.hpp - reading a channel's level out of the
// wideband spectrum.
//
// Two ways this goes quietly wrong, and both are pinned here. Reading the
// wrong half of the band, because the spectrum is fftshifted and bin 0 is the
// most NEGATIVE offset rather than DC - that produces a plausible level for
// the wrong frequency, which is worse than no level. And averaging decibels
// instead of power, which understates anything with a peak in it: every real
// signal, and no noise floor. A suite that only ever measured flat noise would
// pass with that bug in place.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_levels.hpp"

#include <cmath>
#include <vector>

#include "test_check.hpp"

using cascade::core::patch::binForOffset;
using cascade::core::patch::channelLevelDb;
using cascade::core::patch::kDefaultChannelBandwidthHz;
using cascade::core::patch::Level;

namespace {

constexpr double kRate = 2400000.0;

// A flat floor with one bin raised, so a reading can be traced to a place.
std::vector<float> floorWithPeak(std::size_t n, float floorDb, std::size_t at, float peakDb) {
    std::vector<float> bins(n, floorDb);
    if (at < n) { bins[at] = peakDb; }
    return bins;
}

}  // namespace

int main() {
    // [1] Where an offset lands. The spectrum is FFTSHIFTED: bin 0 is
    // -rate/2, the middle is DC, the last bin is +rate/2.
    {
        CHECK_NEAR(binForOffset(0.0, kRate, 1024), 512.0, 0.001);
        CHECK_NEAR(binForOffset(-kRate / 2.0, kRate, 1024), 0.0, 0.001);
        CHECK_NEAR(binForOffset(kRate / 2.0, kRate, 1024), 1024.0, 0.001);
        CHECK_NEAR(binForOffset(kRate / 4.0, kRate, 1024), 768.0, 0.001);
        CHECK_NEAR(binForOffset(-kRate / 4.0, kRate, 1024), 256.0, 0.001);

        // Nonsense in, a refusal out - never an index.
        CHECK(binForOffset(0.0, kRate, 0) < 0.0);
        CHECK(binForOffset(0.0, 0.0, 1024) < 0.0);
    }

    // [2] A peak is found where it was put, and NOT in the mirror position.
    // This is the fftshift test: reading bin 0 as DC would find the peak at
    // the wrong offset and report the floor here.
    {
        // One quarter of the way up = -rate/4.
        const std::vector<float> bins = floorWithPeak(1024, -90.0f, 256, -20.0f);

        const Level atPeak = channelLevelDb(bins, kRate, -kRate / 4.0, 4000.0);
        CHECK(atPeak.valid);
        CHECK(atPeak.db > -60.0f);   // the peak dominates a narrow channel

        // The mirror offset must read the floor, not the peak.
        const Level mirrored = channelLevelDb(bins, kRate, kRate / 4.0, 4000.0);
        CHECK(mirrored.valid);
        CHECK(mirrored.db < -80.0f);
    }

    // [3] dB DOES NOT AVERAGE. Two bins at 0 and -20 hold about -2.6 dB of
    // power between them; averaging the decibels would say -10. Every real
    // signal has a peak in it, so this is not a corner case.
    {
        std::vector<float> two(2, 0.0f);
        two[1] = -20.0f;
        // A bandwidth wide enough to cover the whole two-bin spectrum.
        const Level l = channelLevelDb(two, kRate, 0.0, kRate);
        CHECK(l.valid);
        CHECK_NEAR(l.db, 10.0f * std::log10((1.0f + 0.01f) / 2.0f), 0.05f);
        CHECK(l.db > -5.0f);    // nowhere near the -10 a dB average would give
        CHECK(l.db < 0.0f);
    }

    // [4] A flat floor reads as that floor, whatever the span. This is the
    // case a dB-averaging bug would also pass, which is exactly why test [3]
    // exists beside it.
    {
        const std::vector<float> flat(1024, -97.5f);
        for (const double bw : {2000.0, 16000.0, 200000.0}) {
            const Level l = channelLevelDb(flat, kRate, 0.0, bw);
            CHECK(l.valid);
            CHECK_NEAR(l.db, -97.5f, 0.01f);
        }
    }

    // [5] A channel outside the spectrum is INVALID, not clamped. A clamped
    // reading is a number, and a number gets read as a measurement of the
    // frequency that was asked for.
    {
        const std::vector<float> flat(1024, -90.0f);
        CHECK(!channelLevelDb(flat, kRate, kRate, 4000.0).valid);
        CHECK(!channelLevelDb(flat, kRate, -kRate, 4000.0).valid);
        // Just inside is still valid.
        CHECK(channelLevelDb(flat, kRate, kRate * 0.49, 4000.0).valid);

        // THE CASE THAT ACTUALLY NEEDS THE GUARD, and the one the first
        // version of this test missed: a WIDE channel centred just past
        // the edge. Its span still overlaps real bins, so clamping would
        // return a level - a genuine measurement of a frequency nobody
        // asked about. A narrow one collapses to nothing and is refused by
        // the span check anyway, which is why it proves nothing.
        const double justPast = kRate * 0.55;   // centre bin ~1075 of 1024
        CHECK(!channelLevelDb(flat, kRate, justPast, kRate * 0.3).valid);
        CHECK(!channelLevelDb(flat, kRate, -justPast, kRate * 0.3).valid);
    }

    // [6] A channel narrower than one bin still has a level: the bin it sits
    // in. Returning nothing would make every narrow channel unreadable on a
    // wide device rate, which is most of them.
    {
        const std::vector<float> bins = floorWithPeak(64, -100.0f, 48, -10.0f);
        const double binWidth = kRate / 64.0;
        // Aim at bin 48's centre with a bandwidth far below one bin.
        const double offset = ((48.5 / 64.0) - 0.5) * kRate;
        const Level l = channelLevelDb(bins, kRate, offset, binWidth / 100.0);
        CHECK(l.valid);
        CHECK_NEAR(l.db, -10.0f, 0.5f);
    }

    // [7] Degenerate inputs answer "no reading" rather than a number.
    {
        const std::vector<float> empty;
        CHECK(!channelLevelDb(empty, kRate, 0.0).valid);

        const std::vector<float> flat(256, -80.0f);
        CHECK(!channelLevelDb(flat, 0.0, 0.0).valid);
        CHECK(!channelLevelDb(flat, kRate, 0.0, 0.0).valid);
        CHECK(!channelLevelDb(flat, kRate, 0.0, -5000.0).valid);
    }

    // [8] A bin holding no power at all reports a floor rather than -inf,
    // which would format as something alarming instead of "nothing here".
    {
        // -5000 dB is 10^-500, which underflows to exactly 0.0 in a double.
        // The first version used -1000 dB - 10^-100, small and denormal but
        // emphatically not zero - so the guard was never exercised and a
        // mutant that removed it survived.
        const std::vector<float> silent(64, -5000.0f);
        const Level l = channelLevelDb(silent, kRate, 0.0, 4000.0);
        CHECK(l.valid);
        CHECK(l.db <= -190.0f);
        CHECK(std::isfinite(l.db));
    }

    // [9] The default bandwidth is used when none is given, and it is the
    // same answer as passing it explicitly.
    {
        const std::vector<float> bins = floorWithPeak(2048, -95.0f, 1024, -30.0f);
        const Level implicitBw = channelLevelDb(bins, kRate, 0.0);
        const Level explicitBw = channelLevelDb(bins, kRate, 0.0, kDefaultChannelBandwidthHz);
        CHECK(implicitBw.valid);
        CHECK_NEAR(implicitBw.db, explicitBw.db, 0.0001f);
    }

    return testSummary("test_patch_levels");
}
