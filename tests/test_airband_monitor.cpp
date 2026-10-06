// Tests for core/airband_monitor.hpp - cutting a list of airband channels into
// blocks one capture can hold, and running a block: several AM (and, since
// 0.99.66, NFM) channels off one radio, each squelched, mixed into one speaker.
//
// The block tests check INVARIANTS against arithmetic done here (every
// channel in exactly one block, every member inside the usable span of its
// block's centre, clear of the DC spike when the spacing allows it, the
// greedy cut taken only when the next channel could not fit) rather than
// against the planner's own output.
//
// The runner test synthesises the band: AM carriers at known offsets with
// known tones, one 100 times weaker than another, a neighbour on the 25 kHz
// channel next door, and a channel with nothing on it. Its answers are
// measured off the mixed audio with a Goertzel filter at each tone: both
// talkers heard at the same loudness, the neighbour not heard, the empty
// channel's squelch shut.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/airband_monitor.hpp"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using namespace cascade::core;

namespace {

constexpr double kPi = 3.14159265358979323846;

MonitorChannel ch(double mhz, double bw = 10000.0) { return MonitorChannel{mhz * 1e6, bw}; }

// Every block a plan makes obeys the rules the header promises. Prints what
// broke so the CHECK at the call site names its own case.
bool planHolds(const std::vector<MonitorChannel>& c, const std::vector<AirbandBlock>& blocks,
               double rate, bool expectClearOfDc) {
    const double half = 0.5 * patch::kUsableBandFraction * rate;
    std::vector<int> seen(c.size(), 0);
    double lastCentre = -1.0;
    for (const AirbandBlock& b : blocks) {
        if (b.members.empty() || b.members.size() > kMaxBlockChannels) {
            std::printf("  block of %zu\n", b.members.size());
            return false;
        }
        if (b.centreHz < lastCentre) {
            std::printf("  blocks not ascending\n");
            return false;
        }
        lastCentre = b.centreHz;
        for (const std::size_t m : b.members) {
            if (m >= c.size()) { return false; }
            ++seen[m];
            const double off = std::fabs(c[m].freqHz - b.centreHz) + 0.5 * c[m].bandwidthHz;
            if (off > half + 1.0) {
                std::printf("  %.4f MHz is %.0f Hz from centre %.4f (usable %.0f)\n",
                            c[m].freqHz / 1e6, off, b.centreHz / 1e6, half);
                return false;
            }
            if (expectClearOfDc &&
                std::fabs(c[m].freqHz - b.centreHz) - 0.5 * c[m].bandwidthHz < kDcGuardHz - 1.0) {
                std::printf("  %.4f MHz sits on the DC spike of %.4f\n", c[m].freqHz / 1e6,
                            b.centreHz / 1e6);
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (seen[i] != 1) {
            std::printf("  channel %zu in %d blocks\n", i, seen[i]);
            return false;
        }
    }
    return true;
}

// Goertzel amplitude of `f` in `x` at rate `fs`: 2|X(f)|/N, so a sine of
// amplitude a reads a.
double toneAmp(const std::vector<float>& x, std::size_t from, double f, double fs) {
    const std::size_t n = x.size() - from;
    const double w = 2.0 * kPi * f / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = from; i < x.size(); ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

struct AmTx {
    double offsetHz;   // from the capture's centre
    double carrier;    // amplitude, full scale 1
    double toneHz;
    double depth;      // modulation index
};

// What ONE monitored channel plays (0.99.66): a set of just that channel, fed
// a single transmitter on it - AM at `depth`, or FM at `deviationHz` peak - and
// run through the real runner for two seconds. Measured on the second one,
// after every filter and the carrier average have settled: the RMS of the
// mixed audio the speaker would be given, and the amplitude of the tone in it
// (so a loud noise cannot pass for a loud voice).
struct Played {
    double rms = 0.0;
    double toneAmp = 0.0;
    bool open = false;
    bool built = false;
};

Played playOne(const MonitorChannel& chn, double fs, double toneHz, double depthOrDeviationHz) {
    Played out;
    const double centre = chn.freqHz - 50000.0;   // off the DC spike, as the planner does
    const std::vector<MonitorChannel> c = {chn};
    AirbandBlock blk;
    blk.centreHz = centre;
    blk.members = {0};
    const patch::NodeId id = 3000;
    std::shared_ptr<patch::StripSet> set = buildMonitorSet(c, blk, centre, fs, -75.0f, id);
    out.built = set != nullptr;
    if (!set) { return out; }
    patch::Runner runner;
    runner.publish(set);

    const bool fm = chn.mode == MonitorMode::Nfm;
    const double offset = chn.freqHz - centre;
    const std::size_t block = static_cast<std::size_t>(fs / 100.0);   // 10 ms
    std::vector<std::complex<float>> iq(block);
    std::vector<float> outL(480), outR(480), heard;
    std::uint32_t lcg = 4242u;
    const auto noise = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5) * 2e-4;
    };
    std::uint64_t t = 0;
    for (int b = 0; b < 200; ++b) {   // 2 s
        for (std::size_t i = 0; i < block; ++i, ++t) {
            const double ts = static_cast<double>(t) / fs;
            std::complex<double> s(noise(), noise());
            if (fm) {
                // Peak deviation D at tone f is a phase swing of D/f radians.
                const double ph = 2.0 * kPi * offset * ts +
                                  (depthOrDeviationHz / toneHz) * std::sin(2.0 * kPi * toneHz * ts);
                s += std::polar(0.1, ph);
            } else {
                const double env = 0.1 * (1.0 + depthOrDeviationHz * std::cos(2.0 * kPi * toneHz * ts));
                s += env * std::polar(1.0, 2.0 * kPi * offset * ts);
            }
            iq[i] = std::complex<float>(static_cast<float>(s.real()), static_cast<float>(s.imag()));
        }
        runner.process(iq.data(), iq.size());
        CHECK(runner.pullAudio(outL.data(), outR.data(), outL.size()));
        heard.insert(heard.end(), outL.begin(), outL.end());
    }
    float lvl = 0.0f;
    runner.squelchState(id, lvl, out.open);
    const std::size_t from = heard.size() / 2;
    double sum = 0.0;
    for (std::size_t i = from; i < heard.size(); ++i) { sum += static_cast<double>(heard[i]) * heard[i]; }
    out.rms = std::sqrt(sum / static_cast<double>(heard.size() - from));
    out.toneAmp = toneAmp(heard, from, toneHz, 48000.0);
    return out;
}

}  // namespace

int main() {
    // --- planning: degenerate input --------------------------------------------
    CHECK(planAirbandBlocks({}, 2.4e6).empty());
    CHECK(planAirbandBlocks({ch(121.6)}, 0.0).empty());
    CHECK(planAirbandBlocks({ch(121.6)}, std::nan("")).empty());
    {
        // A channel with no usable frequency is ignored, not planned at 0 Hz.
        const std::vector<MonitorChannel> c = {ch(121.6), MonitorChannel{std::nan(""), 1e4},
                                               MonitorChannel{-5.0, 1e4}};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        if (b.size() == 1) { CHECK(b[0].members.size() == 1 && b[0].members[0] == 0); }
    }

    // --- O'Hare's ground and tower cluster: one RTL-SDR capture -------------
    {
        const std::vector<MonitorChannel> c = {ch(121.9), ch(120.55), ch(121.6),
                                               ch(120.75), ch(121.75), ch(121.15)};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        CHECK(planHolds(c, b, 2.4e6, true));
        if (b.size() == 1) {
            // Members come back ascending by frequency.
            CHECK(b[0].members == (std::vector<std::size_t>{1, 3, 5, 2, 4, 0}));
        }
        // Same input, same answer.
        const auto again = planAirbandBlocks(c, 2.4e6);
        CHECK(again.size() == b.size() && again[0].centreHz == b[0].centreHz);
    }

    // --- every VHF frequency O'Hare publishes: blocks, and the greedy cut ---
    {
        const double mhz[] = {118.05,  119.0,  119.25, 119.625, 120.55,  120.75, 121.15, 121.5,
                              121.6,   121.675, 121.75, 121.9,  122.95,  124.125, 124.35, 125.7,
                              126.625, 126.8,  126.9,  128.05, 128.15,  128.2,  128.575, 132.7,
                              133.0,   133.5,  133.625, 134.15, 135.4};
        std::vector<MonitorChannel> c;
        for (const double m : mhz) { c.push_back(ch(m)); }
        for (const double rate : {2.4e6, 2.048e6, 3.2e6, 6.0e6, 10.0e6, 20.0e6}) {
            const auto b = planAirbandBlocks(c, rate);
            CHECK(planHolds(c, b, rate, true));
            // GREEDY: each block's first channel did not fit in the block
            // before it - otherwise the plan used more blocks than it needed.
            const double room = patch::kUsableBandFraction * rate - 2.0 * kDcGuardHz;
            for (std::size_t i = 1; i < b.size(); ++i) {
                const MonitorChannel& firstPrev = c[b[i - 1].members.front()];
                const MonitorChannel& next = c[b[i].members.front()];
                const double extent = (next.freqHz + 0.5 * next.bandwidthHz) -
                                      (firstPrev.freqHz - 0.5 * firstPrev.bandwidthHz);
                CHECK(extent > room);
            }
            if (rate >= 20.0e6) { CHECK(b.size() == 1); }   // 118.05-135.4 fits in 18 MHz
            if (rate == 2.4e6) { CHECK(b.size() >= 6 && b.size() <= 12); }
        }
    }

    // --- a single channel: the centre moves JUST off it, out of the DC
    //     spike - not to the far edge of the band ------------------------------
    {
        const std::vector<MonitorChannel> c = {ch(121.5)};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        CHECK(planHolds(c, b, 2.4e6, true));
        if (b.size() == 1) {
            CHECK(b[0].centreHz != 121.5e6);
            CHECK(std::fabs(b[0].centreHz - 121.5e6) <= 5000.0 + kDcGuardHz + 2.0);
        }
    }
    // --- two channels either side of a clear middle: the middle it is -------
    {
        const std::vector<MonitorChannel> c = {ch(119.7), ch(120.3)};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        if (b.size() == 1) { CHECK(b[0].centreHz == 120.0e6); }
        CHECK(planHolds(c, b, 2.4e6, true));
    }
    // --- a channel ON the middle: the centre steps aside by the least it can,
    //     and every channel stays inside the band ------------------------------
    {
        const std::vector<MonitorChannel> c = {ch(119.7), ch(120.0), ch(120.3)};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        if (b.size() == 1) {
            CHECK(std::fabs(b[0].centreHz - 120.0e6) <= 5000.0 + kDcGuardHz + 2.0);
        }
        CHECK(planHolds(c, b, 2.4e6, true));
    }

    // --- dense 8.33 kHz channels: no centre clears them all, but the plan
    //     still holds every one inside the band ----------------------------
    {
        std::vector<MonitorChannel> c;
        for (int i = 0; i < 40; ++i) { c.push_back(MonitorChannel{118.0e6 + i * 25000.0 / 3.0, 6000.0}); }
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 1);
        // The cluster is narrow beside the band, so a centre to one side of
        // it clears every channel.
        CHECK(planHolds(c, b, 2.4e6, true));
    }

    // --- THE REVIEW'S CASE: a block grown to the span's limit leaves a few kHz
    //     of allowed centre, and that window sat 4 kHz inside 119.075. The
    //     block now gives its last channel back until the spike is clear. ----
    {
        const std::vector<MonitorChannel> c = {ch(118.0), ch(119.075), ch(120.146)};
        const auto b = planAirbandBlocks(c, 2.4e6);
        CHECK(b.size() == 2);
        CHECK(planHolds(c, b, 2.4e6, true));
    }

    // --- the CPU budget: a cap per block, and the rate it comes from --------
    {
        std::vector<MonitorChannel> c;
        for (int i = 0; i < 12; ++i) { c.push_back(MonitorChannel{120.0e6 + i * 25000.0, 10000.0}); }
        const auto b = planAirbandBlocks(c, 2.4e6, patch::kUsableBandFraction, 5);
        CHECK(b.size() == 3);
        if (b.size() == 3) {
            CHECK(b[0].members.size() == 5);
            CHECK(b[1].members.size() == 5);
            CHECK(b[2].members.size() == 2);
        }
        CHECK(planHolds(c, b, 2.4e6, true));
        // 0 asks for nothing and is taken as 1.
        CHECK(planAirbandBlocks(c, 2.4e6, patch::kUsableBandFraction, 0).size() == 12);
        CHECK(monitorChannelsPerBlock(2.4e6) == 10);
        CHECK(monitorChannelsPerBlock(10.0e6) == 2);
        CHECK(monitorChannelsPerBlock(100.0e6) == 1);
        CHECK(monitorChannelsPerBlock(0.0) == kMaxBlockChannels);
        CHECK(monitorChannelsPerBlock(48000.0) == kMaxBlockChannels);
    }

    // --- more channels than a set reports on: split at kMaxBlockChannels ----
    {
        std::vector<MonitorChannel> c;
        for (int i = 0; i < 100; ++i) { c.push_back(MonitorChannel{118.0e6 + i * 12500.0, 6000.0}); }
        const auto b = planAirbandBlocks(c, 20.0e6);
        CHECK(b.size() == 2);
        if (b.size() == 2) {
            CHECK(b[0].members.size() == kMaxBlockChannels);
            CHECK(b[1].members.size() == 100 - kMaxBlockChannels);
        }
        CHECK(planHolds(c, b, 20.0e6, true));
    }

    // --- a channel wider than the whole capture still gets a block ----------
    {
        const std::vector<MonitorChannel> c = {MonitorChannel{121.6e6, 10000.0}};
        const auto b = planAirbandBlocks(c, 5000.0);
        CHECK(b.size() == 1);
        if (b.size() == 1) { CHECK(b[0].centreHz != 121.6e6); }
    }

    // --- RUNNING A BLOCK: mixed, squelched, levelled, neighbour rejected ----
    {
        const double fs = 2.4e6;
        const double centre = 121.7e6;
        // Ticked: A 121.600 (strong), B 121.750 (100x weaker), C 121.900 (empty).
        const std::vector<MonitorChannel> c = {ch(121.6), ch(121.75), ch(121.9)};
        AirbandBlock blk;
        blk.centreHz = centre;
        blk.members = {0, 1, 2};
        const patch::NodeId firstId = 1000;
        std::shared_ptr<patch::StripSet> set = buildMonitorSet(c, blk, centre, fs, -75.0f, firstId);
        CHECK(set != nullptr);
        CHECK(buildMonitorSet(c, blk, centre, 1000.0, -75.0f, firstId) == nullptr);   // no rate
        CHECK(buildMonitorSet(c, AirbandBlock{}, centre, fs, -75.0f, firstId) == nullptr);
        if (set) {
            CHECK(set->mixAll);
            CHECK(set->channels.size() == 3);
            CHECK(set->channels[2].node == firstId + 2);
            CHECK(set->channels[0].strip.channelFilterTaps() > 0);
        }

        // What is on the air. The neighbour sits on the 25 kHz channel above A
        // (121.625, not ticked) at A's strength with its own tone.
        const AmTx tx[] = {
            {121.600e6 - centre, 0.1, 500.0, 0.6},
            {121.750e6 - centre, 0.001, 1200.0, 0.6},
            {121.625e6 - centre, 0.1, 2500.0, 0.6},
        };
        patch::Runner runner;
        runner.publish(set);

        const std::size_t block = 24000;   // 10 ms
        std::vector<std::complex<float>> iq(block);
        std::vector<float> outL(480), outR(480), heard;
        std::uint32_t lcg = 12345u;
        const auto noise = [&lcg]() {
            lcg = lcg * 1664525u + 1013904223u;
            return (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5) * 2e-4;
        };
        std::uint64_t t = 0;
        for (int b = 0; b < 200; ++b) {   // 2 s
            for (std::size_t i = 0; i < block; ++i, ++t) {
                const double ts = static_cast<double>(t) / fs;
                std::complex<double> s(noise(), noise());
                for (const AmTx& x : tx) {
                    const double env = x.carrier * (1.0 + x.depth * std::cos(2.0 * kPi * x.toneHz * ts));
                    s += env * std::polar(1.0, 2.0 * kPi * x.offsetHz * ts);
                }
                iq[i] = std::complex<float>(static_cast<float>(s.real()), static_cast<float>(s.imag()));
            }
            runner.process(iq.data(), iq.size());
            CHECK(runner.pullAudio(outL.data(), outR.data(), outL.size()));
            heard.insert(heard.end(), outL.begin(), outL.end());
        }

        float lvl = 0.0f;
        bool open = false;
        CHECK(runner.squelchState(firstId + 0, lvl, open) && open);
        CHECK(runner.squelchState(firstId + 1, lvl, open) && open);
        CHECK(runner.squelchState(firstId + 2, lvl, open) && !open);
        // A channel this set never had reports nothing.
        CHECK(!runner.squelchState(firstId + 3, lvl, open));

        // THE REPORTS DIE WITH THE SET (review, 2026-10-01). A flush (what a
        // plugin rescan does to the pipeline's runner) used to leave them in
        // place, so a channel open at that moment stayed open for good, and
        // the block count is how a caller tells a set that is running from
        // one that is not.
        const std::uint64_t ranBefore = runner.blocksRun();
        CHECK(ranBefore == 200);
        runner.flushNow();
        CHECK(!runner.squelchState(firstId + 0, lvl, open));
        CHECK(!runner.squelchState(firstId + 1, lvl, open));
        runner.process(iq.data(), iq.size());
        CHECK(runner.blocksRun() == ranBefore);   // nothing to run, nothing counted
        // A replacement set: the old set's identities report nothing from the
        // moment it is adopted, the new ones from its first block.
        AirbandBlock one;
        one.centreHz = centre;
        one.members = {0};
        runner.publish(buildMonitorSet(c, one, centre, fs, -75.0f, firstId + 100));
        runner.process(iq.data(), iq.size());
        CHECK(runner.blocksRun() == ranBefore + 1);
        CHECK(!runner.squelchState(firstId + 1, lvl, open));
        CHECK(runner.squelchState(firstId + 100, lvl, open));
        // A stop request clears them too.
        runner.clear();
        runner.process(iq.data(), iq.size());
        CHECK(!runner.squelchState(firstId + 100, lvl, open));
        runner.reap();

        // Judged over the last second, after every filter and the carrier
        // averages have settled.
        const std::size_t from = heard.size() / 2;
        const double a500 = toneAmp(heard, from, 500.0, 48000.0);
        const double a1200 = toneAmp(heard, from, 1200.0, 48000.0);
        const double a2500 = toneAmp(heard, from, 2500.0, 48000.0);
        std::printf("  mixed audio: A 500 Hz %.4f, B 1200 Hz %.4f, neighbour 2500 Hz %.6f\n", a500,
                    a1200, a2500);
        // Each talker at (a little under) depth 0.6 times the mix gain,
        // whatever its strength: the 40 dB between A's and B's carriers is
        // gone from the audio.
        const double want = 0.6 * kMonitorMixGain;
        CHECK(a500 > 0.5 * want && a500 < 1.1 * want);
        CHECK(a1200 > 0.5 * want && a1200 < 1.1 * want);
        CHECK(std::fabs(20.0 * std::log10(a500 / a1200)) < 2.0);
        // The neighbour on 121.625, as strong as A, is 40 dB down or more.
        CHECK(a2500 < want * 0.01);
    }

    // --- the channel filter, on one strip: an 8.33 kHz neighbour ------------
    //
    // Measured on the channel's own I/Q (what the squelch judges and what an
    // I/Q decoder would be fed): the neighbour ALONE on the air, its power in
    // the channel with and without the narrow filter; then the wanted signal
    // alone, its audio with and without.
    {
        const double fs = 2.4e6;
        struct Out {
            std::vector<float> audio;
            double iqPowerDb = 0.0;
        };
        const auto run = [&](double bw, bool wanted, bool neighbour, bool normalise) {
            patch::Strip s;
            s.configure(0.0, fs, 50);
            s.setChannelFilter(bw);
            s.setAmNormalise(normalise);
            std::vector<std::complex<float>> iq(24000), base;
            Out o;
            std::uint64_t t = 0;
            double p = 0.0;
            std::size_t np = 0;
            for (int b = 0; b < 50; ++b) {
                for (std::size_t i = 0; i < iq.size(); ++i, ++t) {
                    const double ts = static_cast<double>(t) / fs;
                    std::complex<double> v(0.0, 0.0);
                    if (wanted) { v += 0.1 * (1.0 + 0.5 * std::cos(2.0 * kPi * 500.0 * ts)); }
                    if (neighbour) {
                        v += 0.1 * (1.0 + 0.5 * std::cos(2.0 * kPi * 1800.0 * ts)) *
                             std::polar(1.0, 2.0 * kPi * (25000.0 / 3.0) * ts);
                    }
                    iq[i] = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
                }
                base.clear();
                s.process(iq.data(), iq.size(), patch::Demod::Am, o.audio, &base);
                if (b >= 25) {
                    for (const auto& z : base) { p += std::norm(z); }
                    np += base.size();
                }
            }
            o.iqPowerDb = 10.0 * std::log10(p / static_cast<double>(np) + 1e-30);
            return o;
        };
        // Off by default: no taps.
        patch::Strip plain;
        plain.configure(0.0, fs, 50);
        CHECK(plain.channelFilterTaps() == 0);
        // A bandwidth the output rate already passes is no filter either.
        plain.setChannelFilter(48000.0);
        CHECK(plain.channelFilterTaps() == 0);
        plain.setChannelFilter(6000.0);
        CHECK(plain.channelFilterTaps() > 0);
        // A reconfigure keeps the bandwidth.
        plain.configure(0.0, fs, 25);
        CHECK(plain.channelFilterTaps() > 0);

        const Out wideN = run(0.0, false, true, false);
        const Out narrowN = run(6000.0, false, true, false);
        std::printf("  8.33 neighbour in the channel: %.1f dB wide, %.1f dB narrow\n", wideN.iqPowerDb,
                    narrowN.iqPowerDb);
        CHECK(wideN.iqPowerDb > -25.0);                        // the first filter passes it
        CHECK(narrowN.iqPowerDb < wideN.iqPowerDb - 30.0);    // the second does not

        const Out wideW = run(0.0, true, false, false);
        const Out narrowW = run(6000.0, true, false, false);
        const double w5 = toneAmp(wideW.audio, wideW.audio.size() / 2, 500.0, 48000.0);
        const double n5 = toneAmp(narrowW.audio, narrowW.audio.size() / 2, 500.0, 48000.0);
        std::printf("  wanted 500 Hz: %.5f wide, %.5f narrow\n", w5, n5);
        // The wanted tone is untouched by the narrowing: 0.1 x 0.5 depth.
        CHECK_NEAR(w5, 0.05, 0.005);
        CHECK_NEAR(n5, 0.05, 0.005);

        // THE WHOLE VOICE BAND PASSES a 6 kHz channel: a 2.8 kHz tone comes
        // through the narrow filter within 1 dB of the wide one (a cutoff
        // centred on the channel's edge took it down 5 dB).
        {
            const auto tone = [&](double bw) {
                patch::Strip s;
                s.configure(0.0, fs, 50);
                s.setChannelFilter(bw);
                std::vector<std::complex<float>> iq(24000);
                std::vector<float> out;
                std::uint64_t t = 0;
                for (int b = 0; b < 40; ++b) {
                    for (std::size_t i = 0; i < iq.size(); ++i, ++t) {
                        const double ts = static_cast<double>(t) / fs;
                        iq[i] = std::complex<float>(
                            static_cast<float>(0.1 * (1.0 + 0.5 * std::cos(2.0 * kPi * 2800.0 * ts))), 0.0f);
                    }
                    s.process(iq.data(), iq.size(), patch::Demod::Am, out);
                }
                return toneAmp(out, out.size() / 2, 2800.0, 48000.0);
            };
            const double wide = tone(0.0);
            const double narrow = tone(6000.0);
            std::printf("  2.8 kHz voice tone: %.5f wide, %.5f through a 6 kHz channel\n", wide, narrow);
            CHECK(std::fabs(20.0 * std::log10(narrow / wide)) < 1.0);
        }

        // NORMALISED, the tone's level is set by its modulation depth (a
        // little under it: the level follows modulation peaks a little), not
        // by the carrier.
        const Out norm = run(6000.0, true, false, true);
        const double nAmp = toneAmp(norm.audio, norm.audio.size() / 2, 500.0, 48000.0);
        std::printf("  normalised 500 Hz at depth 0.5: %.4f\n", nAmp);
        CHECK(nAmp > 0.3 && nAmp < 0.55);
        // ...and FM is never normalised: a steady carrier, once the filter
        // has filled, is silence rather than 1/carrier.
        patch::Strip fm;
        fm.configure(0.0, fs, 50);
        fm.setAmNormalise(true);
        std::vector<std::complex<float>> q(240000, std::complex<float>(0.1f, 0.0f));
        std::vector<float> fo;
        fm.process(q.data(), q.size(), patch::Demod::Fm, fo);
        bool small = true;
        for (std::size_t i = fo.size() / 2; i < fo.size(); ++i) { small = small && std::fabs(fo[i]) < 1e-3f; }
        CHECK(small);

        // THE OPENING OF A CALL IS NOT LOUDER THAN THE REST OF IT. Noise for
        // 0.3 s, then a carrier appears: its tone 50-150 ms after the start
        // must already be within 2 dB of where it settles (the first version
        // of this averaged over 0.3 s from the noise floor, and played every
        // callsign several times too loud).
        {
            patch::Strip s;
            s.configure(0.0, fs, 50);
            s.setChannelFilter(6000.0);
            s.setAmNormalise(true);
            std::vector<std::complex<float>> iq(2400);
            std::vector<float> out;
            std::uint32_t lcg = 777u;
            std::uint64_t t = 0;
            const double startS = 0.3;
            for (int b = 0; b < 1000; ++b) {   // 1 s in 1 ms blocks
                for (std::size_t i = 0; i < iq.size(); ++i, ++t) {
                    const double ts = static_cast<double>(t) / fs;
                    lcg = lcg * 1664525u + 1013904223u;
                    const double nz = (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5) * 2e-5;
                    double v = nz;
                    if (ts >= startS) { v += 0.05 * (1.0 + 0.5 * std::cos(2.0 * kPi * 500.0 * ts)); }
                    iq[i] = std::complex<float>(static_cast<float>(v), static_cast<float>(nz));
                }
                s.process(iq.data(), iq.size(), patch::Demod::Am, out);
            }
            const auto window = [&](double a, double b) {
                const auto i0 = static_cast<std::size_t>(a * 48000.0);
                const auto i1 = static_cast<std::size_t>(b * 48000.0);
                std::vector<float> w(out.begin() + static_cast<std::ptrdiff_t>(i0),
                                     out.begin() + static_cast<std::ptrdiff_t>(i1));
                return toneAmp(w, 0, 500.0, 48000.0);
            };
            const double early = window(startS + 0.05, startS + 0.15);
            const double settled = window(startS + 0.5, startS + 0.6);
            std::printf("  call opening: %.4f at 50-150 ms, %.4f settled\n", early, settled);
            CHECK(settled > 0.3);
            CHECK(std::fabs(20.0 * std::log10(early / settled)) < 2.0);
        }
    }

    // --- AM AND NFM (0.99.66) --------------------------------------------------
    //
    // Which list rows the monitor plays: exactly "AM" and "NFM".
    {
        MonitorMode m = MonitorMode::Nfm;
        CHECK(monitorModeFor("AM", m) && m == MonitorMode::Am);
        CHECK(monitorModeFor("NFM", m) && m == MonitorMode::Nfm);
        m = MonitorMode::Am;
        for (const char* other : {"WFM", "USB", "LSB", "CW", "DSB", "RAW", "FM", "am", "Nfm", ""}) {
            CHECK(!monitorModeFor(other, m));
            CHECK(m == MonitorMode::Am);   // untouched when it says no
        }
        // A channel written the way every one was before the mode existed is AM.
        const MonitorChannel legacy{121.6e6, 10000.0};
        CHECK(legacy.mode == MonitorMode::Am);
    }

    // What buildMonitorSet makes of each mode: an NFM channel is an FM strip
    // with no carrier normalisation and a level; an AM channel is what it
    // always was. The same block holds both, in frequency order.
    {
        const double fs = 2.4e6;
        const double centre = 121.7e6;
        std::vector<MonitorChannel> c = {MonitorChannel{121.6e6, 10000.0, MonitorMode::Am},
                                         MonitorChannel{121.75e6, 12500.0, MonitorMode::Nfm}};
        AirbandBlock blk;
        blk.centreHz = centre;
        blk.members = {0, 1};
        const auto set = buildMonitorSet(c, blk, centre, fs, -75.0f, 500);
        CHECK(set != nullptr);
        if (set && set->channels.size() == 2) {
            const patch::RunningChannel& am = set->channels[0];
            const patch::RunningChannel& nfm = set->channels[1];
            CHECK(am.mode == patch::Demod::Am);
            CHECK(am.strip.amNormalise());
            CHECK(am.mixLevel == 1.0f);
            CHECK(am.strip.channelFilterTaps() > 0);
            CHECK(nfm.mode == patch::Demod::Fm);
            CHECK(!nfm.strip.amNormalise());
            CHECK(nfm.mixLevel > 1.0f);
            CHECK(nfm.strip.channelFilterTaps() > 0);   // the row's bandwidth, as AM's
            CHECK(am.squelch != nullptr && nfm.squelch != nullptr);
        }
        // Flip the first channel's mode and the first strip follows: the mapping
        // is per channel, not per set.
        c[0].mode = MonitorMode::Nfm;
        c[1].mode = MonitorMode::Am;
        const auto flipped = buildMonitorSet(c, blk, centre, fs, -75.0f, 500);
        CHECK(flipped != nullptr);
        if (flipped && flipped->channels.size() == 2) {
            CHECK(flipped->channels[0].mode == patch::Demod::Fm);
            CHECK(flipped->channels[1].mode == patch::Demod::Am);
        }
    }

    // THE LEVEL, as arithmetic: kNfmTargetPeak * rate / (2 pi deviation), the
    // deviation a fifth of the channel width held to 1.25-5 kHz; and what it
    // does with a rate or a width that is nothing.
    {
        const double twoPi = 2.0 * kPi;
        CHECK_NEAR(nfmMixLevel(12500.0, 48000.0), kNfmTargetPeak * 48000.0 / (twoPi * 2500.0), 1e-4);
        CHECK_NEAR(nfmMixLevel(25000.0, 48000.0), kNfmTargetPeak * 48000.0 / (twoPi * 5000.0), 1e-4);
        // Wider than any NFM channel is held at 5 kHz, narrower at 1.25 kHz.
        CHECK_NEAR(nfmMixLevel(150000.0, 48000.0), nfmMixLevel(25000.0, 48000.0), 1e-6);
        CHECK_NEAR(nfmMixLevel(2000.0, 48000.0), nfmMixLevel(6250.0, 48000.0), 1e-6);
        // Radians per sample shrink as the rate grows, so the level grows with it.
        CHECK_NEAR(nfmMixLevel(12500.0, 96000.0), 2.0 * nfmMixLevel(12500.0, 48000.0), 1e-4);
        CHECK(nfmMixLevel(12500.0, 0.0) == 1.0f);
        CHECK(nfmMixLevel(12500.0, std::nan("")) == 1.0f);
        CHECK(nfmMixLevel(0.0, 48000.0) == nfmMixLevel(12500.0, 48000.0));
        CHECK(nfmMixLevel(std::nan(""), 48000.0) == nfmMixLevel(12500.0, 48000.0));
    }

    // THE MEASUREMENT BEHIND THAT LEVEL: an AM channel carrying a tone at 80%
    // modulation and a 12.5 kHz NFM channel carrying a tone at 2.5 kHz
    // deviation, each alone through the real runner, play at a similar
    // loudness - at the 48 kHz channel rate of a 2.4 MS/s radio and at the
    // 47.6 kHz of an RTL-SDR at 2.048. Without the level the NFM channel is
    // the quieter by the ratio of 2.5 kHz deviation (0.33 rad/sample at 48 kHz)
    // to an 80% envelope (0.48 normalised): 0.68, 3.3 dB down - inside a
    // factor of 2, so that bound alone cannot tell a missing level from a
    // present one; the second, +-2.5 dB, does.
    for (const double fs : {2.4e6, 2.048e6}) {
        const Played am = playOne(MonitorChannel{121.6e6, 10000.0, MonitorMode::Am}, fs, 500.0, 0.8);
        const Played nfm = playOne(MonitorChannel{121.6e6, 12500.0, MonitorMode::Nfm}, fs, 1000.0, 2500.0);
        CHECK(am.built && nfm.built);
        const double ratio = nfm.rms / am.rms;
        std::printf("  at %.3f MS/s: AM 80%% rms %.4f (tone %.4f), NFM 2.5 kHz rms %.4f (tone %.4f), "
                    "NFM/AM %.2f, level %.3f\n",
                    fs / 1e6, am.rms, am.toneAmp, nfm.rms, nfm.toneAmp, ratio,
                    static_cast<double>(nfmMixLevel(12500.0, fs / patch::chooseChannelRate(fs).decimation)));
        CHECK(am.open && nfm.open);
        // What was measured is the voice tone, not noise: a pure tone's amplitude
        // is sqrt(2) times its rms.
        CHECK(am.toneAmp > 0.9 * std::sqrt(2.0) * am.rms);
        CHECK(nfm.toneAmp > 0.9 * std::sqrt(2.0) * nfm.rms);
        // Within a factor of 2 of each other, and in fact much closer.
        CHECK(ratio > 0.5 && ratio < 2.0);
        CHECK(ratio > 0.75 && ratio < 1.33);
    }

    // --- what one strip costs (printed, not asserted: it is this machine's) -
    for (const double fs : {2.4e6, 10.0e6}) {
        const patch::RateChoice rc = patch::chooseChannelRate(fs);
        patch::Strip s;
        s.configure(125000.0, fs, rc.decimation);
        s.setChannelFilter(10000.0);
        s.setAmNormalise(true);
        std::vector<std::complex<float>> iq(static_cast<std::size_t>(fs / 100.0), std::complex<float>(0.01f, 0.0f));
        std::vector<float> out;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < 100; ++b) {   // one second of samples
            out.clear();
            s.process(iq.data(), iq.size(), patch::Demod::Am, out);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  one strip, one second at %.1f MS/s: %.1f ms of CPU\n", fs / 1e6, ms);
    }

    return testSummary("test_airband_monitor");
}
