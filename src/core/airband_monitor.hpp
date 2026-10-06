// airband_monitor.hpp - several airband channels at once off one radio, and
// the blocks a list of them is split into when they do not all fit.
//
// THE REQUEST (a US tester, 2026-10-01): "If they are close enough together
// have multiple AM demodulators running simultaneously. If they are not close
// enough together could the SDR scan across the frequencies stopping when it
// sees a peak and tuning in. When the audio stops, continue scanning."
//
// THE SHAPE OF THE ANSWER. A radio captures a band its sample rate wide; every
// ticked channel inside 90% of it (patch_plan.hpp's kUsableBandFraction - the
// edges are filter roll-off) can be demodulated at the same time. So the
// ticked channels are cut into BLOCKS, each a set of channels one centre
// frequency can hold, and:
//   - one block: the radio sits on it and every channel plays, mixed;
//   - several:   core::Scanner in list mode walks the block centres, stops on
//                a block when any of its channels opens its squelch, and moves
//                on once they have all been quiet for the hold time.
// Within a block nothing is ever scanned past: a call on any of its channels
// is heard from its first syllable, which a one-channel scanner cannot do.
//
// THE DSP IS THE PATCH PAGE'S, reused rather than written again: one
// patch::Strip per channel (mix to DC, decimate, narrow, AM or FM), one
// dsp::Squelch each, and patch::Runner's hand-off to the receiver's audio - the
// same path the patch used before its radios had their own devices
// (pipeline.cpp, "THE PATCH TAPS THE SAME RAW BAND"). What is new is
// StripSet::mixAll: every channel's squelched audio summed into the one
// speaker.
//
// AM AND NFM (0.99.66, a tester's "MANUAL" list with a demodulator column). A
// channel plays as AM (airband, the request above) or as NFM (marine, business
// and public-service radio typed in by hand); the strip already has an FM
// discriminator, so NFM is that branch behind the same channel filter and
// squelch. WFM is NOT offered: the strip's channel rate is 24-96 kHz, wide FM
// needs 200 kHz and a de-emphasis stage, neither of which a strip has. An FM
// strip's audio is radians per sample, a far smaller number than a normalised
// AM envelope, so each NFM channel is mixed at a level derived from that
// figure (nfmMixLevel) - see tests/test_airband_monitor.cpp for the
// measurement.
//
// planAirbandBlocks() is pure and table-tested (tests/test_airband_monitor.cpp);
// buildMonitorSet() allocates and is GUI-thread only, like every set builder.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <string_view>
#include <vector>

#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"

namespace cascade::core {

// What a channel is demodulated as. AM is the default so a channel written
// {freqHz, bandwidthHz} - every one before 0.99.66 - is the AM channel it was.
enum class MonitorMode : std::uint8_t { Am, Nfm };

// The monitor's mode for a frequency-list row's mode name, or false when the
// monitor does not play it. EXACTLY "AM" and "NFM": the list's mode names are
// the demodulator's (dsp/demod.cpp), and a row in WFM, SSB, CW... is not
// something a strip plays - it is not a near miss to be rounded to one.
inline bool monitorModeFor(std::string_view bookmarkMode, MonitorMode& out) {
    if (bookmarkMode == "AM") {
        out = MonitorMode::Am;
        return true;
    }
    if (bookmarkMode == "NFM") {
        out = MonitorMode::Nfm;
        return true;
    }
    return false;
}

// One channel to monitor: where it is, how wide, and how it is demodulated.
struct MonitorChannel {
    double freqHz = 0.0;
    double bandwidthHz = 10000.0;
    MonitorMode mode = MonitorMode::Am;
};

struct AirbandBlock {
    double centreHz = 0.0;
    std::vector<std::size_t> members;   // indices into the channel list, ascending by frequency
};

// The most channels one block holds: the runner reports squelch state for
// this many channels of a set (patch::Runner::kSquelchSlots).
inline constexpr std::size_t kMaxBlockChannels = patch::Runner::kSquelchSlots;

// How far a channel's EDGE is kept from the centre of the capture. Every
// zero-IF radio has a DC spike there (an RTL-SDR's is a few kHz wide); a
// channel sitting on it hears a whistle and holds its squelch open.
inline constexpr double kDcGuardHz = 2000.0;

namespace detail {

// How far channel `c` sits clear of the DC guard when the capture is centred
// on `centreHz`: negative means it overlaps.
inline double dcClearance(const MonitorChannel& c, double centreHz) {
    return std::fabs(c.freqHz - centreHz) - 0.5 * c.bandwidthHz - kDcGuardHz;
}

// The centre for a set of member channels whose lowest lower edge is `lo` and
// highest upper edge `hi`, and how far it leaves the nearest one clear of the
// DC spike (`clearOut`; negative = a channel overlaps it). See
// planAirbandBlocks for the rule.
inline double bestCentre(const std::vector<MonitorChannel>& channels,
                         const std::vector<std::size_t>& members, double lo, double hi,
                         double half, double& clearOut) {
    // The centres every member allows: each edge within `half` of it.
    const double cLo = hi - half;
    const double cHi = lo + half;
    const double mid = 0.5 * (lo + hi);
    std::vector<double> cand;
    if (cLo <= cHi) {
        cand.push_back(std::clamp(mid, cLo, cHi));
        cand.push_back(cLo);
        cand.push_back(cHi);
        for (std::size_t m = 0; m + 1 < members.size(); ++m) {
            const double g = 0.5 * (channels[members[m]].freqHz + channels[members[m + 1]].freqHz);
            if (g >= cLo && g <= cHi) { cand.push_back(g); }
        }
        for (const std::size_t m : members) {
            const double off = 0.5 * std::max(0.0, channels[m].bandwidthHz) + kDcGuardHz + 1.0;
            for (const double g : {channels[m].freqHz - off, channels[m].freqHz + off}) {
                if (g >= cLo && g <= cHi) { cand.push_back(g); }
            }
        }
    } else {
        // One channel wider than the capture: sit just off it.
        const MonitorChannel& c = channels[members.front()];
        cand.push_back(c.freqHz + 0.5 * c.bandwidthHz + kDcGuardHz);
    }
    // Clear of the spike counts as clear: a channel 300 kHz from it is no
    // better off than one 10 kHz from it, and is nearer the band's edge.
    double best = cand.front();
    double bestScore = -1e300;
    double bestClear = -1e300;
    for (const double c : cand) {
        double clear = 1e300;
        for (const std::size_t m : members) { clear = std::min(clear, dcClearance(channels[m], c)); }
        const double score = std::min(clear, 0.0);
        const bool better = score > bestScore + 1e-6 ||
                            (std::fabs(score - bestScore) <= 1e-6 &&
                             std::fabs(c - mid) < std::fabs(best - mid));
        if (better) {
            best = c;
            bestScore = score;
            bestClear = clear;
        }
    }
    clearOut = bestClear;
    return best;
}

}  // namespace detail

// Cuts `channels` into blocks a capture `deviceRateHz` wide can hold, at most
// `maxPerBlock` channels each (the CPU budget: monitorChannelsPerBlock).
//
// GREEDY BY FREQUENCY: channels are taken lowest first, and a block grows while
// its whole extent - lowest channel's lower edge to highest channel's upper
// edge - fits in the usable span less a DC guard either side of the centre.
// Then the centre is chosen INSIDE the range every member allows: the point
// NEAREST THE BLOCK'S MIDDLE that keeps every channel off the DC spike, so
// channels stay away from the band edges, where a radio's response droops.
// Candidates are the middle, the two ends of the allowed range, the gap
// midpoints between neighbouring channels, and the points just clear of each
// channel either side.
//
// A BLOCK THAT CANNOT CLEAR THE SPIKE GIVES BACK ITS LAST CHANNEL, and the
// centre is chosen again, until it can or one channel is left: grown right to
// the limit of the span, a block's allowed centres shrink to a few kHz, and
// the review that found this put the spike 4 kHz inside a channel - which
// then holds its squelch open on the spike's whistle. The channel given back
// starts the next block.
//
// Deterministic for a given input. Channels with a non-finite or non-positive
// frequency are ignored. A channel too wide for the capture on its own still
// gets a block of its own, centred off it by the guard - the caller hears it
// as best the radio can rather than not at all.
inline std::vector<AirbandBlock> planAirbandBlocks(const std::vector<MonitorChannel>& channels,
                                                   double deviceRateHz,
                                                   double usableFraction = patch::kUsableBandFraction,
                                                   std::size_t maxPerBlock = kMaxBlockChannels) {
    std::vector<AirbandBlock> blocks;
    if (!(deviceRateHz > 0.0)) { return blocks; }
    maxPerBlock = std::clamp<std::size_t>(maxPerBlock, 1, kMaxBlockChannels);
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (std::isfinite(channels[i].freqHz) && channels[i].freqHz > 0.0) { order.push_back(i); }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return channels[a].freqHz < channels[b].freqHz;
    });
    const double half = 0.5 * usableFraction * deviceRateHz;
    const auto lowEdge = [&](std::size_t i) {
        return channels[i].freqHz - 0.5 * std::max(0.0, channels[i].bandwidthHz);
    };
    const auto highEdge = [&](std::size_t i) {
        return channels[i].freqHz + 0.5 * std::max(0.0, channels[i].bandwidthHz);
    };
    // Room for the extent AND a guard each side of the centre.
    const double room = 2.0 * half - 2.0 * kDcGuardHz;

    std::size_t k = 0;
    while (k < order.size()) {
        AirbandBlock b;
        b.members.push_back(order[k]);
        const double lo = lowEdge(order[k]);
        std::size_t j = k + 1;
        while (j < order.size() && b.members.size() < maxPerBlock) {
            double nhi = 0.0;
            for (const std::size_t m : b.members) { nhi = std::max(nhi, highEdge(m)); }
            nhi = std::max(nhi, highEdge(order[j]));
            if (nhi - lo > room) { break; }
            b.members.push_back(order[j]);
            ++j;
        }
        for (;;) {
            double hi = lo;
            for (const std::size_t m : b.members) { hi = std::max(hi, highEdge(m)); }
            double clear = 0.0;
            const double c = detail::bestCentre(channels, b.members, lo, hi, half, clear);
            if (clear >= 0.0 || b.members.size() == 1) {
                // Whole hertz: a centre is something a radio is told, and a
                // fraction of a hertz only makes the readback compare unequal.
                b.centreHz = std::round(c);
                break;
            }
            b.members.pop_back();
        }
        k += b.members.size();
        blocks.push_back(std::move(b));
    }
    return blocks;
}

// THE CPU BUDGET, in channel-samples a second. Each strip mixes and filters
// every sample the radio delivers, so a block's cost is its channels times
// the device rate, all on the receiver's one DSP thread beside the FFT and the
// VFO. Measured on this project's desk (test_airband_monitor prints it): one
// strip took 16 ms of CPU per second of 2.4 MS/s, about 6.7 ms per MS/s. The
// budget - 10 channels at 2.4 MS/s, 4 at 6 MS/s, 2 at 10 MS/s - is about a
// sixth of a core there, leaving room for machines several times slower
// (the Raspberry Pi build). Channels past it go into another block, which
// the scan visits in turn; a lower sample rate fits more in each.
inline constexpr double kMonitorBudgetSps = 24.0e6;

// How many channels one block may hold at `deviceRateHz`: the budget over the
// rate, at least one, at most the runner's kMaxBlockChannels.
inline std::size_t monitorChannelsPerBlock(double deviceRateHz) {
    if (!(deviceRateHz > 0.0)) { return kMaxBlockChannels; }
    const double n = std::floor(kMonitorBudgetSps / deviceRateHz);
    if (!(n >= 1.0)) { return 1; }
    return static_cast<std::size_t>(std::min<double>(n, static_cast<double>(kMaxBlockChannels)));
}

// The gain each channel is mixed at. Carrier-normalised AM peaks near 1 at
// full modulation; two controllers talking at once must not hit the limiter
// as a matter of course.
inline constexpr float kMonitorMixGain = 0.5f;

// THE LEVEL OF AN NFM CHANNEL (0.99.66). A strip's FM audio is the angle the
// carrier turned per sample, 2*pi*deviation/rate radians: a 2.5 kHz peak
// deviation at a 48 kHz channel rate is 0.33 - a third of what a carrier-
// normalised AM channel reads at 80% modulation (0.48, measured:
// tests/test_airband_monitor.cpp prints it) - so an NFM channel mixed at the
// AM gain would be about 3 dB quieter than the AM one beside it in the same
// speaker, and about 10 dB at a 25 kHz channel's 5 kHz. The multiplier that
// levels them is derived rather than tuned: the peak an AM channel plays at
// (kNfmTargetPeak, that measured 0.48) over the peak a channel at its nominal
// deviation gives,
//     kNfmTargetPeak * outRate / (2*pi * nominalDeviation)
// and it depends on the channel rate (24-96 kHz, whatever whole decimation of
// the radio's rate lands nearest 48 kHz), because radians per sample shrink
// as the rate grows.
//
// The nominal deviation is a fifth of the channel width: 2.5 kHz for the
// 12.5 kHz channel a row is added with, 5 kHz for a 25 kHz one (the usual
// pairing of deviation and channel), limited to 1.25-5 kHz so a row edited to
// some odd bandwidth is not turned up or down without limit.
inline constexpr double kNfmTargetPeak = 0.48;

inline float nfmMixLevel(double bandwidthHz, double outRateHz) {
    if (!(outRateHz > 0.0) || !std::isfinite(outRateHz)) { return 1.0f; }
    const double bw = std::isfinite(bandwidthHz) && bandwidthHz > 0.0 ? bandwidthHz : 12500.0;
    const double deviationHz = std::clamp(bw / 5.0, 1250.0, 5000.0);
    constexpr double kTwoPi = 6.283185307179586;
    return static_cast<float>(kNfmTargetPeak * outRateHz / (kTwoPi * deviationHz));
}

// The set that runs one block: a strip per member (offset from `centreHz`,
// narrowed to its bandwidth, AM carrier-normalised or FM, by the channel's
// mode), a squelch each at `squelchDb`, every channel's gated audio mixed into
// the receiver's speaker. Channel identities are `firstId + i` for the i-th
// member, so a caller that changes block can tell the new set's squelch
// reports from the old one's.
// Null when no whole decimation of `deviceRateHz` lands in the audio band.
// GUI THREAD ONLY - it allocates.
inline std::shared_ptr<patch::StripSet> buildMonitorSet(const std::vector<MonitorChannel>& channels,
                                                        const AirbandBlock& block,
                                                        double centreHz, double deviceRateHz,
                                                        float squelchDb, patch::NodeId firstId,
                                                        double audioRateHz = patch::kOutRateHz) {
    const patch::RateChoice rc = patch::chooseChannelRate(deviceRateHz);
    if (!rc.ok || block.members.empty()) { return nullptr; }
    auto set = std::make_shared<patch::StripSet>();
    set->mixAll = true;
    set->mixGain = kMonitorMixGain;
    set->channels.reserve(block.members.size());
    for (std::size_t i = 0; i < block.members.size(); ++i) {
        const MonitorChannel& c = channels[block.members[i]];
        patch::RunningChannel ch;
        ch.node = firstId + static_cast<patch::NodeId>(i);
        const bool nfm = c.mode == MonitorMode::Nfm;
        ch.mode = nfm ? patch::Demod::Fm : patch::Demod::Am;
        ch.strip.configure(c.freqHz - centreHz, deviceRateHz, rc.decimation);
        ch.strip.setChannelFilter(c.bandwidthHz);
        ch.strip.setAmNormalise(!nfm);   // an FM strip has no carrier to divide by
        ch.mixLevel = nfm ? nfmMixLevel(c.bandwidthHz, ch.strip.outRateHz()) : 1.0f;
        ch.squelch = std::make_unique<cascade::dsp::Squelch>(ch.strip.outRateHz());
        ch.squelchDb = squelchDb;
        ch.squelch->setThresholdDb(squelchDb);
        ch.gated.assign(patch::kMaxBlockAudio, 0.0f);
        ch.audio.assign(patch::kMaxBlockAudio, 0.0f);
        set->channels.push_back(std::move(ch));
    }
    const double inRate = set->channels.front().strip.outRateHz();
    set->toAudio = std::make_unique<cascade::dsp::RationalResampler>(
        static_cast<unsigned>(audioRateHz + 0.5), static_cast<unsigned>(inRate + 0.5));
    set->resampled.assign(set->toAudio->maxOut(patch::kMaxBlockAudio) + 8, 0.0f);
    set->mix.assign(patch::kMaxBlockAudio, 0.0f);
    return set;
}

}  // namespace cascade::core
