// Tests for core/patch_plan.hpp - what a patch would actually build, and
// everything that stops it.
//
// Every problem reported here describes a patch that LOOKS right. A channel
// outside the captured band, a decoder hanging off nothing, a device rate that
// cannot be divided down to 48 kHz - each one draws a perfectly convincing
// picture and then decodes nothing, which is indistinguishable from a quiet
// band. That is why they are worth naming, and why the naming is pinned here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_plan.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/patch_graph.hpp"
#include "test_check.hpp"

using cascade::core::patch::ChannelPlan;
using cascade::core::patch::compile;
using cascade::core::patch::Connect;
using cascade::core::patch::Graph;
using cascade::core::patch::hasBlockingProblem;
using cascade::core::patch::isAdvisory;
using cascade::core::patch::kChannelRateHz;
using cascade::core::patch::kNoNode;
using cascade::core::patch::listeningChannel;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::Plan;
using cascade::core::patch::PortType;
using cascade::core::patch::Problem;
using cascade::core::patch::RateChoice;
using cascade::core::patch::chooseChannelRate;
using cascade::core::patch::problemsFor;
using cascade::core::patch::problemText;
using cascade::core::patch::wholeDecimation;

namespace {

constexpr double kRate = 2400000.0;    // 2.4 MS/s, which divides to 48 kHz
constexpr double kCentre = 131000000.0;

bool has(const Plan& p, NodeId id, Problem want) {
    const std::vector<Problem> got = problemsFor(p, id);
    return std::find(got.begin(), got.end(), want) != got.end();
}

// Count only the problems that stop a patch, so an advisory does not make a
// test about blocking failures look wrong.
std::size_t blocking(const Plan& p) {
    std::size_t n = 0;
    for (const auto& np : p.problems) {
        if (!isAdvisory(np.problem)) { ++n; }
    }
    return n;
}

// A radio, a channel on frequency, a demod and a speaker: the smallest patch
// that should simply work.
struct Working {
    Graph g;
    NodeId radio, chan, demod, spk;

    Working() {
        radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        chan = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        demod = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        g.mutableNode(chan)->freqHz = kCentre + 200000.0;
        g.connect(radio, 0, chan, 0);
        g.connect(chan, 0, demod, 0);
        g.connect(demod, 0, spk, 0);
    }
};

}  // namespace

int main() {
    // [1] The decimation is EXACT or it is refused. A bit-clocked decoder
    // treats its symbol phases as sample offsets within a bit, so a fractional
    // samples-per-bit does not degrade the decode, it walks off the bit.
    {
        CHECK(wholeDecimation(2400000.0) == 50u);
        CHECK(wholeDecimation(1200000.0) == 25u);
        CHECK(wholeDecimation(240000.0) == 5u);
        CHECK(wholeDecimation(48000.0) == 1u);

        // 2.048 MS/s reaches 42.667 and must be refused rather than rounded.
        CHECK(wholeDecimation(2048000.0) == 0u);
        CHECK(wholeDecimation(1000000.0) == 0u);

        // A device reporting 2399999.9 for 2.4 MS/s is reporting 2.4 MS/s;
        // refusing that would be pedantry rather than care.
        CHECK(wholeDecimation(2399999.9) == 50u);
        // But a rate that is genuinely a hundred hertz out is not.
        CHECK(wholeDecimation(2400100.0) == 0u);

        // Nonsense in, zero out - never a divide by zero or a huge count.
        CHECK(wholeDecimation(0.0) == 0u);
        CHECK(wholeDecimation(-2400000.0) == 0u);
        CHECK(wholeDecimation(2400000.0, 0.0) == 0u);
        CHECK(wholeDecimation(1000.0) == 0u);   // below the target entirely
    }

    // [2] The patch that should simply work, does - and says so.
    {
        Working w;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
        CHECK(p.channels.size() == 1u);
        CHECK(p.channels[0].node == w.chan);
        CHECK_NEAR(p.channels[0].offsetHz, 200000.0, 0.001);
        CHECK(p.channels[0].decimation == 50u);
        CHECK_NEAR(p.channels[0].outRateHz, kChannelRateHz, 0.001);
        CHECK(p.order.size() == w.g.nodes().size());
    }

    // [3] A channel with no frequency. It has been placed and wired and never
    // told where to sit, which is the most ordinary half-finished state there
    // is - and a channel at 0 Hz would otherwise plan an offset of minus the
    // whole centre frequency.
    {
        Working w;
        w.g.mutableNode(w.chan)->freqHz = 0.0;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, w.chan, Problem::NoFrequency));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);
    }

    // [4] A channel outside the band the radio is capturing. The usable band
    // is deliberately less than the full width: the edges are filter
    // roll-off, so a channel exactly at Nyquist hears the filter rather than
    // the signal.
    {
        Working w;
        // Comfortably inside.
        w.g.mutableNode(w.chan)->freqHz = kCentre + 1000000.0;
        CHECK(blocking(compile(w.g, kRate, kCentre)) == 0u);

        // Past the usable edge, though still inside the raw half-rate.
        w.g.mutableNode(w.chan)->freqHz = kCentre + 1190000.0;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, w.chan, Problem::OutOfBand));
        CHECK(p.channels.empty());

        // ...and the same distance the other way, which a signed comparison
        // would happily miss.
        w.g.mutableNode(w.chan)->freqHz = kCentre - 1190000.0;
        CHECK(has(compile(w.g, kRate, kCentre), w.chan, Problem::OutOfBand));
    }

    // [5] A device rate with no whole division landing in the audio band.
    //
    // THIS TEST USED TO ASSERT THE OPPOSITE, and the change was deliberate. It
    // required exactly 48 kHz, which refused 2.048 MS/s - and a rendered check
    // on the built-in generator at 2.000 MS/s then marked every channel
    // unbuildable. Those are the two commonest rates this product sees. The
    // exact-rate rule belongs to a bit-clocked DECODER, not to a channel, so a
    // channel now takes any whole division that lands in the band.
    {
        Working w;
        // The rates real devices produce all work, at sensible decimations.
        for (const double rate : {2400000.0, 2048000.0, 2000000.0, 1200000.0}) {
            const Plan ok = compile(w.g, rate, kCentre);
            CHECK(blocking(ok) == 0u);
            CHECK(ok.channels.size() == 1u);
            CHECK(ok.channels[0].outRateHz >= 24000.0);
            CHECK(ok.channels[0].outRateHz <= 96000.0);
        }

        // A rate too low to divide into the band at all still cannot, and the
        // complaint lands on the channel that cannot be built.
        //
        // THE CHANNEL HAS TO BE IN BAND FOR THIS TO BE THE RATE'S FAULT. At
        // 20 kS/s the usable band is +/-9 kHz, and the fixture's channel sits
        // 200 kHz off centre - so the first version of this test was really
        // watching OutOfBand fire and never reached the rate check at all.
        w.g.mutableNode(w.chan)->freqHz = kCentre;
        const Plan p = compile(w.g, 20000.0, kCentre);
        CHECK(has(p, w.chan, Problem::RateUnreachable));
        CHECK(!has(p, w.chan, Problem::OutOfBand));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);
    }

    // [5b] The chooser itself: nearest to 48 kHz, inside the band, or nothing.
    {
        // 2.4 MS/s reaches exactly 48 kHz, so it should choose that and not
        // something merely legal.
        const RateChoice a = chooseChannelRate(2400000.0);
        CHECK(a.ok);
        CHECK(a.decimation == 50u);
        CHECK_NEAR(a.rateHz, 48000.0, 0.001);

        // NEAREST, not neatest. The obvious guess for 2.048 MS/s is the
        // power of two - 32, giving 64 kHz - and that is not what it should
        // choose: 43 gives 47628 Hz, a hair from the 48 kHz everything else
        // in this product runs at. Same for 2.000 MS/s: 42 -> 47619, not
        // 32 -> 62500.
        const RateChoice b = chooseChannelRate(2048000.0);
        CHECK(b.ok);
        CHECK(b.decimation == 43u);
        CHECK_NEAR(b.rateHz, 2048000.0 / 43.0, 0.001);
        CHECK(std::fabs(b.rateHz - 48000.0) < 1000.0);

        const RateChoice c = chooseChannelRate(2000000.0);
        CHECK(c.ok);
        CHECK(c.decimation == 42u);
        CHECK_NEAR(c.rateHz, 2000000.0 / 42.0, 0.001);
        CHECK(std::fabs(c.rateHz - 48000.0) < 1000.0);

        // Whatever it picks, it is always inside the band.
        for (const double rate : {240000.0, 1000000.0, 3200000.0, 10000000.0}) {
            const RateChoice r = chooseChannelRate(rate);
            CHECK(r.ok);
            CHECK(r.rateHz >= 24000.0);
            CHECK(r.rateHz <= 96000.0);
            CHECK_NEAR(rate / static_cast<double>(r.decimation), r.rateHz, 0.001);
        }

        // Below the band entirely, and nonsense, answer "no".
        CHECK(!chooseChannelRate(20000.0).ok);
        CHECK(!chooseChannelRate(0.0).ok);
        CHECK(!chooseChannelRate(-2400000.0).ok);
        // And a rate so high that no decimation within the search cap reaches
        // the band - a refusal rather than a silently wrong huge decimation.
        CHECK(!chooseChannelRate(1.0e9).ok);
    }

    // [6] Nothing feeds it. A node placed and never wired.
    {
        Graph g;
        const NodeId lonely = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        g.mutableNode(lonely)->freqHz = kCentre;
        const Plan p = compile(g, kRate, kCentre);
        CHECK(has(p, lonely, Problem::NothingFeedsIt));
        CHECK(!p.runnable);

        // A Radio has no inputs, so it is never accused of this.
        Graph r;
        const NodeId radio = r.addNode(NodeKind::Radio, "R", PortType::Iq);
        CHECK(!has(compile(r, kRate, kCentre), radio, Problem::NothingFeedsIt));
    }

    // [7] Wired, but to a chain that starts nowhere. Every node in it is
    // wired, so "nothing feeds it" would be a lie; the honest answer is that
    // no radio is upstream.
    {
        Graph g;
        const NodeId chan = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
        const NodeId demod = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        g.mutableNode(chan)->freqHz = kCentre;
        CHECK(g.connect(chan, 0, demod, 0) == Connect::Ok);
        CHECK(g.connect(demod, 0, spk, 0) == Connect::Ok);

        const Plan p = compile(g, kRate, kCentre);
        // The channel is the node to fix, so that is where the complaint goes.
        CHECK(has(p, chan, Problem::NothingFeedsIt));
        CHECK(has(p, demod, Problem::NotFedByARadio));
        CHECK(has(p, spk, Problem::NotFedByARadio));
        CHECK(p.channels.empty());
        CHECK(!p.runnable);
    }

    // [8] Producing something nobody consumes is ADVISORY, not blocking -
    // people leave a decoder unwired while they are still building.
    {
        Working w;
        const NodeId spare = w.g.addNode(NodeKind::Channel, "spare", PortType::Iq);
        w.g.mutableNode(spare)->freqHz = kCentre + 100000.0;
        CHECK(w.g.connect(w.radio, 0, spare, 0) == Connect::Ok);

        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(has(p, spare, Problem::NothingListens));
        CHECK(isAdvisory(Problem::NothingListens));
        CHECK(!hasBlockingProblem(p, spare));
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);                  // an advisory does not stop it
        CHECK(p.channels.size() == 2u);     // and the strip is still planned
    }

    // [9] An empty patch is not "runnable". A green light over an empty canvas
    // would be the most confident possible way of saying nothing.
    {
        Graph g;
        const Plan p = compile(g, kRate, kCentre);
        CHECK(!p.runnable);
        CHECK(p.problems.empty());
        CHECK(p.channels.empty());
        CHECK(p.order.empty());

        // A radio on its own is not runnable either - nothing consumes it.
        Graph r;
        r.addNode(NodeKind::Radio, "R", PortType::Iq);
        CHECK(!compile(r, kRate, kCentre).runnable);
    }

    // [10] The per-node lookups the canvas and the inspector use.
    {
        Working w;
        w.g.mutableNode(w.chan)->freqHz = 0.0;
        const Plan p = compile(w.g, kRate, kCentre);

        CHECK(problemsFor(p, w.chan).size() == 1u);
        CHECK(hasBlockingProblem(p, w.chan));
        CHECK(!hasBlockingProblem(p, w.radio));
        CHECK(problemsFor(p, 9999u).empty());
        CHECK(!hasBlockingProblem(p, 9999u));
    }

    // [11] Every problem has words, and they are distinct - a message shared
    // between two causes tells the user the wrong thing about one of them.
    {
        const Problem all[] = {Problem::NothingFeedsIt, Problem::NotFedByARadio,
                               Problem::NoFrequency,    Problem::OutOfBand,
                               Problem::RateUnreachable, Problem::NothingListens};
        std::vector<std::string> seen;
        for (const Problem p : all) {
            const char* t = problemText(p);
            CHECK(t != nullptr && t[0] != '\0');
            seen.emplace_back(t);
        }
        std::sort(seen.begin(), seen.end());
        CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    }

    // [12] Several channels off one radio, which is the arrangement the whole
    // page exists for. Each gets its own offset and they do not interfere.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        const double freqs[] = {130025000.0, 130450000.0, 131725000.0};
        for (const double f : freqs) {
            const NodeId ch = g.addNode(NodeKind::Channel, "ch", PortType::Iq);
            const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
            const NodeId dec = g.addNode(NodeKind::Decoder, "ACARS", PortType::Audio);
            g.mutableNode(ch)->freqHz = f;
            CHECK(g.connect(radio, 0, ch, 0) == Connect::Ok);
            CHECK(g.connect(ch, 0, dm, 0) == Connect::Ok);
            CHECK(g.connect(dm, 0, dec, 0) == Connect::Ok);
            CHECK(g.connect(dec, 0, text, 0) == Connect::Ok);
        }

        const Plan p = compile(g, kRate, 130875000.0);
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
        CHECK(p.channels.size() == 3u);
        for (const ChannelPlan& c : p.channels) {
            CHECK(c.decimation == 50u);
            CHECK(std::fabs(c.offsetHz) <= 0.5 * kRate);
        }
    }

    // [13] Which channel reaches the speaker.
    {
        Working w;   // radio -> channel -> demod -> speaker
        CHECK(listeningChannel(w.g) == w.chan);

        // No speaker at all: nothing is being listened to, which is a normal
        // state for a patch of displays and decoders.
        Graph quiet;
        const NodeId r = quiet.addNode(NodeKind::Radio, "R", PortType::Iq);
        const NodeId c = quiet.addNode(NodeKind::Channel, "c", PortType::Iq);
        const NodeId d = quiet.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
        quiet.mutableNode(c)->freqHz = kCentre;
        CHECK(quiet.connect(r, 0, c, 0) == Connect::Ok);
        CHECK(quiet.connect(c, 0, d, 0) == Connect::Ok);
        CHECK(listeningChannel(quiet) == kNoNode);

        // A TEXT sink is not a speaker, however many of them there are.
        Graph text;
        const NodeId tr = text.addNode(NodeKind::Radio, "R", PortType::Iq);
        const NodeId tc = text.addNode(NodeKind::Channel, "c", PortType::Iq);
        const NodeId td = text.addNode(NodeKind::Decoder, "d", PortType::Iq);
        const NodeId ts = text.addNode(NodeKind::Sink, "Text", PortType::Text);
        text.mutableNode(tc)->freqHz = kCentre;
        CHECK(text.connect(tr, 0, tc, 0) == Connect::Ok);
        CHECK(text.connect(tc, 0, td, 0) == Connect::Ok);
        CHECK(text.connect(td, 0, ts, 0) == Connect::Ok);
        CHECK(listeningChannel(text) == kNoNode);

        // A speaker wired to nothing leads nowhere rather than to the first
        // channel it can find.
        Graph dangling;
        const NodeId dr = dangling.addNode(NodeKind::Radio, "R", PortType::Iq);
        const NodeId dc = dangling.addNode(NodeKind::Channel, "c", PortType::Iq);
        dangling.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        dangling.mutableNode(dc)->freqHz = kCentre;
        CHECK(dangling.connect(dr, 0, dc, 0) == Connect::Ok);
        CHECK(listeningChannel(dangling) == kNoNode);
    }

    // [14] With two chains, the speaker's OWN chain is the one played - not
    // whichever channel happens to come first.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId quietCh = g.addNode(NodeKind::Channel, "quiet", PortType::Iq);
        const NodeId quietDec = g.addNode(NodeKind::Decoder, "dec", PortType::Iq);
        const NodeId text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        const NodeId heardCh = g.addNode(NodeKind::Channel, "heard", PortType::Iq);
        const NodeId dm = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        const NodeId spk = g.addNode(NodeKind::Sink, "Speaker", PortType::Audio);
        g.mutableNode(quietCh)->freqHz = kCentre + 100000.0;
        g.mutableNode(heardCh)->freqHz = kCentre + 200000.0;
        CHECK(g.connect(radio, 0, quietCh, 0) == Connect::Ok);
        CHECK(g.connect(quietCh, 0, quietDec, 0) == Connect::Ok);
        CHECK(g.connect(quietDec, 0, text, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, heardCh, 0) == Connect::Ok);
        CHECK(g.connect(heardCh, 0, dm, 0) == Connect::Ok);
        CHECK(g.connect(dm, 0, spk, 0) == Connect::Ok);

        // quietCh was created FIRST, so a walk that picked the first channel
        // rather than following the speaker's own wires would answer wrongly.
        CHECK(listeningChannel(g) == heardCh);
    }

    // ===================== DECODER NODES (0.99.15) =====================
    //
    // A catalogue of three plugins, shaped like real ones: an I/Q decoder
    // that takes any rate, one that needs 192 kHz of baseband (AIS-like), and
    // an audio decoder that needs 22050 Hz.
    using cascade::core::patch::DecoderInfo;
    using cascade::core::patch::DecoderPlan;
    using cascade::core::patch::DecoderSource;
    const std::vector<DecoderInfo> cat = {
        {"anyiq.dll", "Any I/Q", PortType::Iq, 0.0},
        {"ais.dll", "AIS", PortType::Iq, 192000.0},
        {"pager.dll", "Pager", PortType::Audio, 22050.0},
    };
    const auto decoderPlanFor = [](const Plan& p, NodeId id) -> const DecoderPlan* {
        for (const DecoderPlan& d : p.decoders) {
            if (d.node == id) { return &d; }
        }
        return nullptr;
    };
    const auto chanPlanFor = [](const Plan& p, NodeId id) -> const ChannelPlan* {
        for (const ChannelPlan& c : p.channels) {
            if (c.node == id) { return &c; }
        }
        return nullptr;
    };

    // [D1] Without a catalogue, decoder nodes are not judged and nothing is
    // planned for them - the pre-0.99.15 behaviour, kept for any caller that
    // knows no plugins.
    {
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "d", PortType::Iq);
        CHECK(w.g.connect(w.chan, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(p.decoders.empty());
        CHECK(!has(p, dec, Problem::NoPlugin));
    }

    // [D2..D4] Each way a decoder node cannot run is named at the node.
    {
        Working w;
        const NodeId none = w.g.addNode(NodeKind::Decoder, "none", PortType::Iq);
        const NodeId gone = w.g.addNode(NodeKind::Decoder, "gone", PortType::Iq);
        const NodeId wrong = w.g.addNode(NodeKind::Decoder, "wrong", PortType::Iq);
        w.g.mutableNode(gone)->plugin = "uninstalled.dll";
        w.g.mutableNode(wrong)->plugin = "pager.dll";      // an AUDIO plugin on an I/Q port
        CHECK(w.g.connect(w.chan, 0, none, 0) == Connect::Ok);
        CHECK(w.g.connect(w.chan, 0, gone, 0) == Connect::Ok);
        CHECK(w.g.connect(w.chan, 0, wrong, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &cat);
        CHECK(has(p, none, Problem::NoPlugin));
        CHECK(has(p, gone, Problem::PluginMissing));
        CHECK(has(p, wrong, Problem::PluginWrongFeed));
        CHECK(hasBlockingProblem(p, none));
        CHECK(p.decoders.empty());
        // And none of them stops the rest of the patch: the channel and its
        // demodulator are still planned.
        CHECK(chanPlanFor(p, w.chan) != nullptr);
        // Every new problem has words.
        for (const Problem pr : {Problem::NoPlugin, Problem::PluginMissing,
                                 Problem::PluginWrongFeed, Problem::DecoderTooFast}) {
            CHECK(std::string(problemText(pr)) != "this cannot run");
        }
    }

    // [D5] An any-rate I/Q decoder on a channel runs at the channel's rate,
    // centred on the CHANNEL's frequency, not the radio's.
    {
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "d", PortType::Iq);
        w.g.mutableNode(dec)->plugin = "anyiq.dll";
        CHECK(w.g.connect(w.chan, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &cat);
        const DecoderPlan* d = decoderPlanFor(p, dec);
        const ChannelPlan* c = chanPlanFor(p, w.chan);
        CHECK(d != nullptr);
        CHECK(c != nullptr);
        if (d != nullptr && c != nullptr) {
            CHECK(d->source == DecoderSource::Channel);
            CHECK(d->channel == w.chan);
            CHECK(d->plugin == 0u);
            CHECK(d->inRateHz == c->outRateHz);
            CHECK(d->rateHz == c->outRateHz);          // "any": no resampler
            CHECK(d->centreHz == kCentre + 200000.0);  // the channel's frequency
            CHECK(c->outRateHz == kChannelRateHz);     // unchanged by an any-rate plugin
        }
    }

    // [D6] A 192 kHz I/Q decoder raises ITS channel to the slowest whole
    // division that is fast enough - 2.4 MS/s / 12 = 200 kHz - and is fed
    // 192 kHz through a resampler. The demodulator on the same channel is
    // still planned.
    {
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "ais", PortType::Iq);
        w.g.mutableNode(dec)->plugin = "ais.dll";
        CHECK(w.g.connect(w.chan, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &cat);
        const ChannelPlan* c = chanPlanFor(p, w.chan);
        const DecoderPlan* d = decoderPlanFor(p, dec);
        CHECK(c != nullptr);
        CHECK(d != nullptr);
        if (c != nullptr && d != nullptr) {
            CHECK(c->decimation == 12u);
            CHECK(c->outRateHz == 200000.0);
            CHECK(d->inRateHz == 200000.0);
            CHECK(d->rateHz == 192000.0);
        }
        CHECK(!hasBlockingProblem(p, w.demod));
    }

    // [D7] An I/Q decoder straight off the radio gets the whole capture at
    // the radio's centre, resampled to what it asks for.
    {
        const std::vector<DecoderInfo> wide = {{"adsb.dll", "ADS-B", PortType::Iq, 2000000.0}};
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId dec = g.addNode(NodeKind::Decoder, "adsb", PortType::Iq);
        g.mutableNode(dec)->plugin = "adsb.dll";
        CHECK(g.connect(radio, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(g, kRate, kCentre, &wide);
        const DecoderPlan* d = decoderPlanFor(p, dec);
        CHECK(d != nullptr);
        if (d != nullptr) {
            CHECK(d->source == DecoderSource::Radio);
            CHECK(d->channel == kNoNode);
            CHECK(d->inRateHz == kRate);
            CHECK(d->rateHz == 2000000.0);
            CHECK(d->centreHz == kCentre);
        }
    }

    // [D8] A plugin faster than anything the source can give is refused AT
    // THE DECODER - and on a channel, the channel still runs for everything
    // else rather than being dragged to a decimation of zero.
    {
        const std::vector<DecoderInfo> tooFast = {{"huge.dll", "Huge", PortType::Iq, 3000000.0}};
        Working w;
        const NodeId onChan = w.g.addNode(NodeKind::Decoder, "c", PortType::Iq);
        const NodeId onRadio = w.g.addNode(NodeKind::Decoder, "r", PortType::Iq);
        w.g.mutableNode(onChan)->plugin = "huge.dll";
        w.g.mutableNode(onRadio)->plugin = "huge.dll";
        CHECK(w.g.connect(w.chan, 0, onChan, 0) == Connect::Ok);
        CHECK(w.g.connect(w.radio, 0, onRadio, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &tooFast);
        CHECK(has(p, onChan, Problem::DecoderTooFast));
        CHECK(has(p, onRadio, Problem::DecoderTooFast));
        CHECK(p.decoders.empty());
        const ChannelPlan* c = chanPlanFor(p, w.chan);
        CHECK(c != nullptr);
        if (c != nullptr) {
            CHECK(c->decimation >= 1u);
            CHECK(c->outRateHz == kChannelRateHz);
        }
    }

    // [D9] An audio decoder behind a demodulator is fed that channel's
    // audio, resampled to the rate it asks for.
    {
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "pager", PortType::Audio);
        w.g.mutableNode(dec)->plugin = "pager.dll";
        CHECK(w.g.connect(w.demod, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &cat);
        const DecoderPlan* d = decoderPlanFor(p, dec);
        CHECK(d != nullptr);
        if (d != nullptr) {
            CHECK(d->source == DecoderSource::Audio);
            CHECK(d->channel == w.chan);
            CHECK(d->inRateHz == kChannelRateHz);
            CHECK(d->rateHz == 22050.0);
        }
    }

    // [D9b] An "any rate" AUDIO decoder is fed 48 kHz - what the receiver's own
    // plugin runner gives it - not the channel's own rate. On a 2 MS/s radio
    // the channel runs at 50 kHz, a rate such a plugin has never been handed.
    {
        const std::vector<DecoderInfo> anyAudio = {{"sstv.dll", "SSTV", PortType::Audio, 0.0}};
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "sstv", PortType::Audio);
        w.g.mutableNode(dec)->plugin = "sstv.dll";
        CHECK(w.g.connect(w.demod, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, 2000000.0, kCentre, &anyAudio);
        const DecoderPlan* d = decoderPlanFor(p, dec);
        CHECK(d != nullptr);
        if (d != nullptr) {
            CHECK(d->inRateHz != kChannelRateHz);   // the premise: 2 MS/s / 40 = 50 kHz
            CHECK(d->rateHz == kChannelRateHz);
        }
    }

    // [D9c] ONE MODULE, TWO DECODERS. A plugin may declare an audio decoder
    // and an I/Q decoder; each node gets the entry for the input its port
    // carries, not simply the first entry with that key.
    {
        const std::vector<DecoderInfo> dual = {
            {"dual.dll", "Dual (audio)", PortType::Audio, 0.0},
            {"dual.dll", "Dual (I/Q)", PortType::Iq, 0.0},
        };
        Working w;
        const NodeId onIq = w.g.addNode(NodeKind::Decoder, "iq", PortType::Iq);
        const NodeId onAudio = w.g.addNode(NodeKind::Decoder, "audio", PortType::Audio);
        w.g.mutableNode(onIq)->plugin = "dual.dll";
        w.g.mutableNode(onAudio)->plugin = "dual.dll";
        CHECK(w.g.connect(w.chan, 0, onIq, 0) == Connect::Ok);
        CHECK(w.g.connect(w.demod, 0, onAudio, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &dual);
        const DecoderPlan* a = decoderPlanFor(p, onIq);
        const DecoderPlan* b = decoderPlanFor(p, onAudio);
        CHECK(a != nullptr);
        CHECK(b != nullptr);
        if (a != nullptr) { CHECK(a->plugin == 1u); }
        if (b != nullptr) { CHECK(b->plugin == 0u); }
        CHECK(!has(p, onIq, Problem::PluginWrongFeed));
        CHECK(!has(p, onAudio, Problem::PluginWrongFeed));
    }

    // [D10] A decoder naming a MISSING plugin must not drag its channel up to
    // a rate nothing will use.
    {
        const std::vector<DecoderInfo> none;
        Working w;
        const NodeId dec = w.g.addNode(NodeKind::Decoder, "ghost", PortType::Iq);
        w.g.mutableNode(dec)->plugin = "ais.dll";   // not in THIS catalogue
        CHECK(w.g.connect(w.chan, 0, dec, 0) == Connect::Ok);
        const Plan p = compile(w.g, kRate, kCentre, &none);
        const ChannelPlan* c = chanPlanFor(p, w.chan);
        CHECK(c != nullptr);
        if (c != nullptr) { CHECK(c->outRateHz == kChannelRateHz); }
        CHECK(has(p, dec, Problem::PluginMissing));
    }

    // [D11] The resampler bound: exact ratios pass through untouched, an
    // awkward one is nudged by no more than the stated tolerance to a ratio
    // the resampler can build, and nonsense is refused.
    {
        using cascade::core::patch::gcdU;
        using cascade::core::patch::kMaxDecoderInterp;
        using cascade::core::patch::kMaxRateNudgePpm;
        using cascade::core::patch::resampleInputRate;
        CHECK(resampleInputRate(48000.0, 24000.0) == 48000u);
        CHECK(resampleInputRate(2400000.0, 48000.0) == 2400000u);
        CHECK(resampleInputRate(200000.0, 192000.0) == 200000u);   // 24/25
        // 2.048 MS/s / 3, against 192 kHz: coprime enough to need a nudge.
        const double awkward = 2048000.0 / 3.0;
        const unsigned r = resampleInputRate(awkward, 192000.0);
        CHECK(r != 0u);
        CHECK(std::fabs(static_cast<double>(r) - awkward) / awkward * 1e6 <= kMaxRateNudgePpm + 1.0);
        CHECK(192000ull / gcdU(192000ull, r) <= kMaxDecoderInterp);
        CHECK(resampleInputRate(0.0, 48000.0) == 0u);
        CHECK(resampleInputRate(48000.0, 0.0) == 0u);
    }

    // [D12] THE POINT OF ALL THIS: two decoders on two frequencies off one
    // radio, each centred on its own channel.
    {
        Graph g;
        const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
        const NodeId c1 = g.addNode(NodeKind::Channel, "c1", PortType::Iq);
        const NodeId c2 = g.addNode(NodeKind::Channel, "c2", PortType::Iq);
        const NodeId d1 = g.addNode(NodeKind::Decoder, "d1", PortType::Iq);
        const NodeId d2 = g.addNode(NodeKind::Decoder, "d2", PortType::Iq);
        const NodeId text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
        g.mutableNode(c1)->freqHz = kCentre - 300000.0;
        g.mutableNode(c2)->freqHz = kCentre + 450000.0;
        g.mutableNode(d1)->plugin = "anyiq.dll";
        g.mutableNode(d2)->plugin = "anyiq.dll";
        CHECK(g.connect(radio, 0, c1, 0) == Connect::Ok);
        CHECK(g.connect(radio, 0, c2, 0) == Connect::Ok);
        CHECK(g.connect(c1, 0, d1, 0) == Connect::Ok);
        CHECK(g.connect(c2, 0, d2, 0) == Connect::Ok);
        CHECK(g.connect(d1, 0, text, 0) == Connect::Ok);
        CHECK(g.connect(d2, 0, text, 0) == Connect::Ok);   // text fans in
        const Plan p = compile(g, kRate, kCentre, &cat);
        CHECK(p.decoders.size() == 2u);
        const DecoderPlan* a = decoderPlanFor(p, d1);
        const DecoderPlan* b = decoderPlanFor(p, d2);
        CHECK(a != nullptr);
        CHECK(b != nullptr);
        if (a != nullptr && b != nullptr) {
            CHECK(a->centreHz == kCentre - 300000.0);
            CHECK(b->centreHz == kCentre + 450000.0);
            CHECK(a->channel == c1);
            CHECK(b->channel == c2);
        }
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
    }

    // ---- 0.99.17: every radio is its own ------------------------------------

    // [M1] TWO RADIOS, TWO BANDS. Each channel is measured against the radio
    // it hangs off: a channel at 145.5 MHz is in band on a radio centred at
    // 145 MHz and would be far out of band on one at 131 MHz. Each channel,
    // and each speaker, carries its own radio.
    {
        using cascade::core::patch::RadioInfo;
        Graph g;
        const NodeId r1 = g.addNode(NodeKind::Radio, "A", PortType::Iq);
        const NodeId r2 = g.addNode(NodeKind::Radio, "B", PortType::Iq);
        g.mutableNode(r1)->device = "siggen";
        g.mutableNode(r2)->device = "rtlsdr|serial=00000001";
        const NodeId c1 = g.addNode(NodeKind::Channel, "air", PortType::Iq);
        const NodeId c2 = g.addNode(NodeKind::Channel, "2m", PortType::Iq);
        const NodeId d1 = g.addNode(NodeKind::Demod, "AM", PortType::Iq);
        const NodeId d2 = g.addNode(NodeKind::Demod, "FM", PortType::Iq);
        const NodeId s1 = g.addNode(NodeKind::Sink, "S1", PortType::Audio);
        const NodeId s2 = g.addNode(NodeKind::Sink, "S2", PortType::Audio);
        g.mutableNode(c1)->freqHz = 131.2e6;
        g.mutableNode(c2)->freqHz = 145.5e6;
        CHECK(g.connect(r1, 0, c1, 0) == Connect::Ok);
        CHECK(g.connect(r2, 0, c2, 0) == Connect::Ok);
        CHECK(g.connect(c1, 0, d1, 0) == Connect::Ok);
        CHECK(g.connect(c2, 0, d2, 0) == Connect::Ok);
        CHECK(g.connect(d1, 0, s1, 0) == Connect::Ok);
        CHECK(g.connect(d2, 0, s2, 0) == Connect::Ok);
        const std::vector<RadioInfo> radios{{r1, 2.4e6, 131.0e6}, {r2, 2.048e6, 145.0e6}};
        const Plan p = compile(g, radios);
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);
        CHECK(p.channels.size() == 2u);
        const ChannelPlan* a = cascade::core::patch::findChannel(p, c1);
        const ChannelPlan* b = cascade::core::patch::findChannel(p, c2);
        CHECK(a != nullptr && b != nullptr);
        if (a != nullptr && b != nullptr) {
            CHECK(a->radio == r1);
            CHECK(b->radio == r2);
            CHECK(std::fabs(a->offsetHz - 200000.0) < 1e-6);
            CHECK(std::fabs(b->offsetHz - 500000.0) < 1e-6);
            CHECK(a->decimation == 50u);          // 2.4 MS/s / 48 kHz
            CHECK(b->decimation == 43u);          // 2.048 MS/s -> 47628 Hz, nearest 48 kHz
        }
        // Both speakers play, each with its own channel and radio.
        CHECK(p.sinks.size() == 2u);
        if (p.sinks.size() == 2u) {
            CHECK(p.sinks[0].sink == s1 && p.sinks[0].channel == c1 && p.sinks[0].radio == r1);
            CHECK(p.sinks[1].sink == s2 && p.sinks[1].channel == c2 && p.sinks[1].radio == r2);
        }
        // The same channel measured against the OTHER radio is out of band.
        const std::vector<RadioInfo> swapped{{r1, 2.4e6, 145.0e6}, {r2, 2.048e6, 131.0e6}};
        const Plan q = compile(g, swapped);
        CHECK(has(q, c1, Problem::OutOfBand));
        CHECK(has(q, c2, Problem::OutOfBand));
        CHECK(!q.runnable);
    }

    // [M2] ONE DEVICE, ONE RADIO. A radio naming a device another radio
    // already uses is refused - by exact key, and by serial across drivers
    // (the same dongle listed natively and through SoapySDR). The generator is
    // not hardware and may be used by any number of radios. A radio with no
    // device is refused too.
    {
        using cascade::core::patch::RadioInfo;
        Graph g;
        const NodeId a = g.addNode(NodeKind::Radio, "A", PortType::Iq);
        const NodeId b = g.addNode(NodeKind::Radio, "B", PortType::Iq);
        const NodeId c = g.addNode(NodeKind::Radio, "C", PortType::Iq);
        const NodeId d = g.addNode(NodeKind::Radio, "D", PortType::Iq);
        const NodeId e = g.addNode(NodeKind::Radio, "E", PortType::Iq);
        g.mutableNode(a)->device = "rtlsdr|serial=00000001";
        g.mutableNode(b)->device = "soapy|driver=rtlsdr,serial=00000001";   // the same dongle
        g.mutableNode(c)->device = "siggen";
        g.mutableNode(d)->device = "siggen";
        // e: none chosen
        const Plan p = compile(g, std::vector<RadioInfo>{});
        CHECK(!has(p, a, Problem::DeviceTwice));    // the first one keeps it
        CHECK(has(p, b, Problem::DeviceTwice));
        CHECK(!has(p, c, Problem::DeviceTwice));
        CHECK(!has(p, d, Problem::DeviceTwice));    // the generator twice is fine
        CHECK(has(p, e, Problem::NoDevice));
        CHECK(!has(p, a, Problem::NoDevice));
        CHECK(hasBlockingProblem(p, b));
        CHECK(hasBlockingProblem(p, e));
        CHECK(std::string(problemText(Problem::DeviceTwice)).find("one device, one radio") !=
              std::string::npos);
        // Different serials on the same driver are different radios.
        g.mutableNode(b)->device = "rtlsdr|serial=00000002";
        const Plan q = compile(g, std::vector<RadioInfo>{});
        CHECK(!has(q, b, Problem::DeviceTwice));
    }

    // [M4] A DECODER THAT DOES NOT NEED SOUND DOES NOT OFFER IT: an audio
    // decoder whose module also decodes straight from I/Q has an I/Q twin (and
    // is left out of the parts bin); one without, or whose only other entry is
    // a picture decoder, does not.
    {
        using cascade::core::patch::DecoderInfo;
        using cascade::core::patch::hasIqTwin;
        std::vector<DecoderInfo> twins(5);
        twins[0] = {"pocsag.dll", "POCSAG", PortType::Audio, 0.0, false};
        twins[1] = {"pocsag.dll", "POCSAG", PortType::Iq, 0.0, false};
        twins[2] = {"aprs.dll", "APRS", PortType::Audio, 0.0, false};
        twins[3] = {"sstv.dll#image", "SSTV", PortType::Audio, 0.0, true};
        twins[4] = {"apt.dll", "APT", PortType::Audio, 0.0, false};
        CHECK(hasIqTwin(twins, 0));    // POCSAG audio: its I/Q twin exists
        CHECK(!hasIqTwin(twins, 1));   // the I/Q one itself has no OTHER I/Q twin
        CHECK(!hasIqTwin(twins, 2));   // APRS: audio only
        CHECK(!hasIqTwin(twins, 3));
        CHECK(!hasIqTwin(twins, 4));
        CHECK(!hasIqTwin(twins, 99));  // out of range is not a twin
        // A picture decoder on I/Q is not a twin of a text decoder's audio key.
        std::vector<DecoderInfo> pic{{"apt.dll", "APT", PortType::Audio, 0.0, false},
                                     {"apt.dll", "APT", PortType::Iq, 0.0, true}};
        CHECK(!hasIqTwin(pic, 0));
    }

    // [MAP] A MAP SHOWS EVERY MODULE WIRED INTO IT, from any radio: ADS-B on
    // one and AIS on another are one map's two sources. A decoder whose module
    // has no map side is named on the decoder, advisory - its text still
    // works. A map with nothing wired cannot run; one with a source can.
    {
        using cascade::core::patch::DecoderInfo;
        using cascade::core::patch::mapSources;
        using cascade::core::patch::RadioInfo;
        Graph g;
        const NodeId r1 = g.addNode(NodeKind::Radio, "R1", PortType::Iq);
        const NodeId r2 = g.addNode(NodeKind::Radio, "R2", PortType::Iq);
        g.mutableNode(r1)->device = "siggen";
        g.mutableNode(r2)->device = "siggen";
        const NodeId adsb = g.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq);
        const NodeId ais = g.addNode(NodeKind::Decoder, "AIS", PortType::Iq);
        const NodeId pager = g.addNode(NodeKind::Decoder, "Pager", PortType::Iq);
        const NodeId adsb2 = g.addNode(NodeKind::Decoder, "ADS-B again", PortType::Iq);
        g.mutableNode(adsb)->plugin = "adsb.dll";
        g.mutableNode(ais)->plugin = "ais.dll";
        g.mutableNode(pager)->plugin = "pocsag.dll";
        g.mutableNode(adsb2)->plugin = "adsb.dll";
        const NodeId map = g.addNode(NodeKind::Map, "Map", PortType::Track);
        const std::vector<DecoderInfo> mapCat{
            {"adsb.dll", "ADS-B Aircraft", PortType::Iq, 0.0, false, true},
            {"ais.dll", "AIS Ships", PortType::Iq, 0.0, false, true},
            {"pocsag.dll", "POCSAG", PortType::Iq, 0.0, false, false}};
        const std::vector<RadioInfo> radios{{r1, 2.4e6, 1090e6}, {r2, 2.4e6, 162e6}};

        // Nothing wired: the map cannot run.
        {
            const Plan p = compile(g, radios, &mapCat);
            CHECK(has(p, map, Problem::NothingFeedsIt));
            CHECK(mapSources(g, map, mapCat).empty());
        }
        CHECK(g.connect(r1, 0, adsb, 0) == Connect::Ok);
        CHECK(g.connect(r2, 0, ais, 0) == Connect::Ok);
        CHECK(g.connect(r2, 0, pager, 0) == Connect::Ok);
        CHECK(g.connect(r1, 0, adsb2, 0) == Connect::Ok);
        CHECK(g.connect(adsb, 1, map, 0) == Connect::Ok);
        CHECK(g.connect(ais, 1, map, 1) == Connect::Ok);
        CHECK(g.connect(pager, 1, map, 2) == Connect::Ok);
        CHECK(g.connect(adsb2, 1, map, 3) == Connect::Ok);
        const Plan p = compile(g, radios, &mapCat);
        const std::vector<std::string> src = mapSources(g, map, mapCat);
        // Both modules, once each - the second ADS-B adds no second source -
        // and the pager, which has no map side, adds nothing.
        CHECK(src == std::vector<std::string>({"ADS-B Aircraft", "AIS Ships"}));
        CHECK(has(p, pager, Problem::NoTracks));
        CHECK(isAdvisory(Problem::NoTracks));
        CHECK(!has(p, adsb, Problem::NoTracks));
        CHECK(!has(p, map, Problem::NothingFeedsIt));
        CHECK(blocking(p) == 0u);
        CHECK(p.runnable);   // a map is something to run
        CHECK(std::string(problemText(Problem::NoTracks)).find("map") != std::string::npos);
    }

    // [M3] The single-receiver overload applies no device rules: a radio with
    // no device is the receiver's own radio, as it was before 0.99.17.
    {
        Working w;
        const Plan p = compile(w.g, kRate, kCentre);
        CHECK(!has(p, w.radio, Problem::NoDevice));
        CHECK(p.runnable);
        CHECK(p.sinks.size() == 1u);
    }

    return testSummary("test_patch_plan");
}
