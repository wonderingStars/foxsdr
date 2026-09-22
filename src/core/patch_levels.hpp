// patch_levels.hpp - how strong the signal on a planned channel is, read out
// of the spectrum the pipeline already publishes.
//
// This is the first thing on the Patch page that is LIVE. A patch that draws
// correctly and says it can run still tells you nothing about whether anything
// is actually arriving on the frequencies you typed, and "no messages" and
// "nothing is being transmitted" look identical from the outside. A level per
// channel separates them.
//
// WHY THE SPECTRUM RATHER THAN THE SAMPLES. The pipeline already computes a
// wideband dB spectrum every frame and hands it to the GUI, so reading a
// channel's level out of it costs an index and an average. Splitting the
// channel out of raw baseband instead would mean a mixer and a filter per
// channel running somewhere, which is the real-time work this page has not
// earned yet - and the answer to "is there anything there" is the same either
// way. It is a measurement, not a decode.
//
// THE ONE ARITHMETIC RULE. dB DOES NOT AVERAGE. A channel spanning two bins at
// 0 dB and -20 dB holds about -2.6 dB of power, not -10; averaging the decibel
// figures understates anything with a peak in it, which is every real signal
// and no noise floor. So the bins are converted to power, averaged, and
// converted back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_LEVELS_HPP
#define CASCADE_CORE_PATCH_LEVELS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cascade::core::patch {

// The width a channel is measured over when nothing better is known. Wide
// enough to hold an AM voice channel or an ACARS burst, narrow enough that a
// neighbour 25 kHz away does not leak into the reading.
inline constexpr double kDefaultChannelBandwidthHz = 16000.0;

struct Level {
    float db = 0.0f;
    bool valid = false;
};

// Which bin an offset from the centre lands in. The spectrum is FFTSHIFTED, so
// bin 0 is the most negative offset and the last bin the most positive - a
// caller that assumed bin 0 was DC would read every channel in the wrong half
// of the band, which looks like a plausible level and is the wrong signal.
inline double binForOffset(double offsetHz, double sampleRateHz, std::size_t bins) {
    if (bins == 0 || !(sampleRateHz > 0.0)) { return -1.0; }
    return (offsetHz / sampleRateHz + 0.5) * static_cast<double>(bins);
}

// The power in a channel, in dB, averaged across the bins it covers.
//
// Returns invalid when the channel falls outside the spectrum rather than
// clamping to the edge: a clamped reading is a number, and a number is read as
// a measurement of the frequency that was asked for.
inline Level channelLevelDb(const std::vector<float>& dbBins, double sampleRateHz,
                            double offsetHz,
                            double bandwidthHz = kDefaultChannelBandwidthHz) {
    Level out;
    const std::size_t n = dbBins.size();
    // DEFENCE IN DEPTH, and a break-it run correctly reports it as such:
    // deleting this line changes no observable behaviour, because a
    // non-positive rate makes binForOffset answer -1 and a non-positive
    // bandwidth collapses the span, and both are refused below. It stays
    // because reading the guard is how the next person learns the three
    // inputs that are nonsense, rather than deducing it from two other
    // functions.
    if (n == 0 || !(sampleRateHz > 0.0) || !(bandwidthHz > 0.0)) { return out; }

    const double centre = binForOffset(offsetHz, sampleRateHz, n);
    if (centre < 0.0 || centre > static_cast<double>(n)) { return out; }

    const double halfSpan =
        0.5 * (bandwidthHz / sampleRateHz) * static_cast<double>(n);
    // A channel narrower than the resolution still has a level, and it is
    // the bin it sits in: the bandwidth guard above refuses anything <= 0,
    // so the half span is always positive and floor(c-h) is always below
    // ceil(c+h). There is deliberately no `if (hi <= lo) hi = lo + 1` here -
    // it cannot fire, and a break-it run proved it by deleting it with no
    // test able to notice. The only way hi <= lo is the CLAMP below taking
    // the span outside the spectrum, which is an out-of-range channel and
    // is refused rather than widened.
    long lo = static_cast<long>(std::floor(centre - halfSpan));
    long hi = static_cast<long>(std::ceil(centre + halfSpan));
    lo = std::max<long>(lo, 0);
    hi = std::min<long>(hi, static_cast<long>(n));
    if (hi <= lo) { return out; }

    // POWER, then average, then back to dB - see the note at the top.
    double sum = 0.0;
    for (long i = lo; i < hi; ++i) {
        sum += std::pow(10.0, static_cast<double>(dbBins[static_cast<std::size_t>(i)]) / 10.0);
    }
    const double mean = sum / static_cast<double>(hi - lo);
    // A bin holding exactly zero power would give -inf, which formats as
    // something alarming rather than as "nothing here".
    out.db = (mean > 0.0) ? static_cast<float>(10.0 * std::log10(mean)) : -200.0f;
    out.valid = true;
    return out;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_LEVELS_HPP
