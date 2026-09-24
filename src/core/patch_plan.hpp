// patch_plan.hpp - turning a patch into what would actually be built, and
// naming everything that stops it.
//
// The graph says what is WIRED. That is not the same as what can RUN: a
// channel can be wired perfectly and sit outside the band the radio is
// capturing, or ask for a rate no whole-number decimation of the device rate
// can reach. Both draw a completely convincing patch and then decode nothing.
//
// So this is the bridge. It walks the graph and produces two things: the
// concrete strips a DSP would build, and a list of PROBLEMS, each pinned to
// the node it belongs to. The canvas marks those nodes and the inspector reads
// the reason out. Nothing here runs any DSP or touches a thread - it is a pure
// function of the graph, two numbers and the list of installed decoder
// plugins, so tests/test_patch_plan.cpp can pin every case with expected
// answers.
//
// WHY THE PROBLEMS MATTER AS MUCH AS THE PLAN. patch_runner.hpp builds and
// runs exactly what this plans - channels since 0.99.14, plugin decoders since
// 0.99.15 - but a patch that cannot work is worth saying so about immediately,
// at the node responsible, rather than after the user has waited for silence
// and started doubting the aerial. Every failure this reports is one that is
// otherwise indistinguishable from a quiet band.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_PLAN_HPP
#define CASCADE_CORE_PATCH_PLAN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/i18n.hpp"  // FOX_TR_NOOP: problemText is drawn through tr()
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"

namespace cascade::core::patch {

// The rate a demodulated audio chain is built at. The whole product works at
// 48 kHz (Pipeline::kAudioRateHz), and the decoders that ride on it are
// written against it.
inline constexpr double kChannelRateHz = 48000.0;

// How far inside the captured band a channel must sit. The edges of a
// decimating filter are not usable, so a channel exactly at Nyquist is a
// channel that hears the filter roll-off rather than the signal.
inline constexpr double kUsableBandFraction = 0.90;

enum class Problem : std::uint8_t {
    NothingFeedsIt,    // a node with an input and no wire into it
    NotFedByARadio,    // nothing upstream is a Radio, so there are no samples
    NoFrequency,       // a Channel that has not been told where to sit
    OutOfBand,         // a Channel outside what the radio is capturing
    RateUnreachable,   // no whole decimation of the device rate reaches 48 kHz
    NothingListens,    // produces something nobody consumes - advisory only
    NoPlugin,          // a Decoder node with no plugin chosen
    PluginMissing,     // the plugin it names is not installed, or failed to load
    PluginWrongFeed,   // the node's port and the plugin's input disagree
    DecoderTooFast,    // the plugin needs a rate faster than its source runs
    PluginRefused,     // create() returned no instance - set by the runner, not by compile()
    NoDevice,          // a Radio with no device chosen
    DeviceTwice,       // a Radio naming a device another Radio already uses
    RadioFailed,       // the device would not open - set by the caller, not by compile()
    NoTracks,          // a decoder wired to a Map whose module puts nothing on a map
};

// Advisory problems do not stop a patch running; they are worth saying and not
// worth refusing. A decoder wired to nothing still decodes, it just has no
// window - that is a thing people do deliberately while building a patch.
inline bool isAdvisory(Problem p) {
    return p == Problem::NothingListens || p == Problem::NoTracks;
}

inline const char* problemText(Problem p) {
    switch (p) {
        case Problem::NothingFeedsIt: return FOX_TR_NOOP("nothing feeds this");
        case Problem::NotFedByARadio: return FOX_TR_NOOP("no radio upstream of this");
        case Problem::NoFrequency: return FOX_TR_NOOP("no frequency set");
        case Problem::OutOfBand: return FOX_TR_NOOP("outside the band the radio is receiving");
        case Problem::RateUnreachable:
            return FOX_TR_NOOP("no whole division of this sample rate lands in the audio band");
        case Problem::NothingListens: return FOX_TR_NOOP("nothing is listening to this");
        case Problem::NoPlugin: return FOX_TR_NOOP("no plugin chosen for this decoder");
        case Problem::PluginMissing: return FOX_TR_NOOP("that plugin is not installed or did not load");
        case Problem::PluginWrongFeed:
            return FOX_TR_NOOP("that plugin takes a different input from the one wired here");
        case Problem::DecoderTooFast:
            return FOX_TR_NOOP("that plugin needs a faster sample rate than its source produces");
        case Problem::PluginRefused: return FOX_TR_NOOP("the plugin refused to start - it may need a different rate");
        case Problem::NoDevice: return FOX_TR_NOOP("no device chosen for this radio");
        case Problem::DeviceTwice:
            return FOX_TR_NOOP("another radio in this patch already uses that device - one device, one radio");
        case Problem::RadioFailed: return FOX_TR_NOOP("the device would not open");
        case Problem::NoTracks: return FOX_TR_NOOP("this decoder puts nothing on a map");
    }
    return FOX_TR_NOOP("this cannot run");
}

// --- decoders ----------------------------------------------------------------
//
// What the plan needs to know about one installed decoder plugin. The app
// builds this list from the plugin host; the plan never sees a module handle,
// which is what keeps it testable without a single DLL.
//
// `feed` is what the plugin eats: PortType::Iq for a CASCADE_CAP_IQ_DECODER,
// PortType::Audio for a CASCADE_CAP_DECODER. `requiredRateHz` is the ABI's own
// field: 0 means "any rate", in which case the plugin is created at whatever
// its source runs at and nothing is resampled.
struct DecoderInfo {
    std::string key;
    std::string name;
    PortType feed = PortType::Iq;
    double requiredRateHz = 0.0;
    // A PICTURE decoder (CASCADE_CAP_IMAGE_DECODER): APT, WEFAX, SSTV. It is
    // planned exactly like a text decoder of the same input - the difference
    // is only in what it produces, which is the runner's and the canvas's
    // business. Its key carries kImageKeySuffix, because one module may be
    // both a text and a picture decoder on the same input, and those are two
    // different parts.
    bool image = false;
    // Whether the module ALSO has a track source (CASCADE_CAP_TRACK_SOURCE) -
    // aircraft, ships, stations it can put on a Map part (0.99.18).
    bool tracks = false;
};

inline constexpr const char* kImageKeySuffix = "#image";

// Whether catalogue entry `i` has a TWIN that decodes straight from I/Q: the
// same module key, an I/Q input, and not a picture decoder. The parts bin
// leaves an AUDIO decoder with such a twin out (owner, 0.99.17: "if the
// decoder doesnt need sound dont show the option for it") - it would be a
// second key for the same module that only differs in needing a demodulator
// in front of it.
inline bool hasIqTwin(const std::vector<DecoderInfo>& catalogue, std::size_t i) {
    if (i >= catalogue.size()) { return false; }
    for (std::size_t j = 0; j < catalogue.size(); ++j) {
        if (j == i) { continue; }
        if (catalogue[j].key == catalogue[i].key && catalogue[j].feed == PortType::Iq &&
            !catalogue[j].image) {
            return true;
        }
    }
    return false;
}

// Where a decoder's samples come from.
//   Radio    an I/Q decoder wired straight to the radio: the whole capture,
//            with the RADIO's centre at DC - the same stream the receiver's
//            own plugin runner gives it.
//   Channel  an I/Q decoder hung off a channel: that channel's slice of the
//            band, with the CHANNEL's frequency at DC. This is what lets two
//            decoders on two frequencies run off one radio.
//   Audio    an audio decoder behind a demodulator: that channel's
//            demodulated audio.
enum class DecoderSource : std::uint8_t { Radio, Channel, Audio };

struct DecoderPlan {
    NodeId node = kNoNode;
    std::size_t plugin = 0;          // index into the catalogue compile() was given
    DecoderSource source = DecoderSource::Radio;
    NodeId channel = kNoNode;        // the feeding Channel, for Channel and Audio
    double inRateHz = 0.0;           // what the source runs at
    double rateHz = 0.0;             // what create() is told
    double centreHz = 0.0;           // RF frequency at DC; I/Q decoders only
    NodeId radio = kNoNode;          // which radio's samples it runs on
};

// The largest interpolation factor the resampler in front of a decoder may
// use. RationalResampler's prototype is about L x 16 taps, so an L in the
// hundreds of thousands - which two coprime rates produce - is tens of
// megabytes of filter per decoder, designed on the GUI thread.
inline constexpr unsigned kMaxDecoderInterp = 2048;

// How far a source rate may be nudged so the ratio stays small, in parts per
// million. The nudge is a timing error the decoder sees as a clock offset;
// 500 ppm is inside what every bit-clocked decoder already tolerates from a
// cheap receiver's crystal, and it is reported in the plan rather than hidden.
inline constexpr double kMaxRateNudgePpm = 500.0;

inline unsigned long long gcdU(unsigned long long a, unsigned long long b) {
    while (b != 0) {
        const unsigned long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// The integer input rate to build a resampler from `inHz` to `outHz` with,
// such that the reduced interpolation factor is at most kMaxDecoderInterp -
// the nearest such rate to the true one, within kMaxRateNudgePpm. Returns 0
// when there is none, which the caller treats as "cannot resample".
//
// A rate that already qualifies is returned unchanged (rounded to whole Hz),
// so the common cases - 48000 to 24000, 2400000 to 48000 - are exact.
inline unsigned resampleInputRate(double inHz, double outHz) {
    if (!(inHz >= 1.0) || !(outHz >= 1.0)) { return 0; }
    const auto out = static_cast<unsigned long long>(outHz + 0.5);
    const auto centre = static_cast<long long>(inHz + 0.5);
    const auto span = static_cast<long long>(inHz * kMaxRateNudgePpm * 1e-6);
    for (long long d = 0; d <= span; ++d) {
        for (const long long cand : {centre - d, centre + d}) {
            if (cand < 1) { continue; }
            const auto g = gcdU(out, static_cast<unsigned long long>(cand));
            if (out / g <= kMaxDecoderInterp) { return static_cast<unsigned>(cand); }
        }
    }
    return 0;
}

struct NodeProblem {
    NodeId node = kNoNode;
    Problem problem = Problem::NothingFeedsIt;
};

struct ChannelPlan {
    NodeId node = kNoNode;
    double offsetHz = 0.0;   // from the radio's centre, signed
    unsigned decimation = 0;
    double outRateHz = 0.0;
    NodeId radio = kNoNode;  // the radio it is cut from
};

// A speaker, and the channel whose demodulated audio it plays (0.99.17: every
// audio sink plays, each to its own output, rather than the first one only).
struct AudioSinkPlan {
    NodeId sink = kNoNode;
    NodeId channel = kNoNode;
    NodeId radio = kNoNode;
};

// What one radio is actually doing: its node, and the rate and centre the
// plan is measured against - the open device's readback when it is running,
// the node's own settings before it is.
struct RadioInfo {
    NodeId node = kNoNode;
    double rateHz = 0.0;
    double centreHz = 0.0;
};

struct Plan {
    std::vector<NodeId> order;
    std::vector<ChannelPlan> channels;
    std::vector<DecoderPlan> decoders;
    std::vector<AudioSinkPlan> sinks;
    std::vector<NodeProblem> problems;
    bool runnable = false;   // nothing blocking, and something to actually do
};

inline constexpr std::size_t kNoDecoder = static_cast<std::size_t>(-1);

// The catalogue entry for a module key, and - when `feed` is given - for that
// input as well. ONE MODULE MAY APPEAR TWICE: a plugin can declare both an
// audio decoder and an I/Q decoder, and those are two parts on the canvas
// with the same key and different inputs.
inline std::size_t findDecoder(const std::vector<DecoderInfo>& catalogue,
                               const std::string& key, const PortType* feed = nullptr) {
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        if (catalogue[i].key != key) { continue; }
        if (feed != nullptr && catalogue[i].feed != *feed) { continue; }
        return i;
    }
    return kNoDecoder;
}

inline const ChannelPlan* findChannel(const Plan& plan, NodeId id) {
    for (const ChannelPlan& c : plan.channels) {
        if (c.node == id) { return &c; }
    }
    return nullptr;
}

// The fastest rate any I/Q decoder wired DIRECTLY to this channel requires, or
// 0 when none requires one. Only decoders whose plugin is in the catalogue and
// really eats I/Q count - a node naming a missing plugin must not drag the
// channel up to a rate nothing will use.
inline double fastestIqDecoderOn(const Graph& g, NodeId channel,
                                 const std::vector<DecoderInfo>* catalogue) {
    if (catalogue == nullptr) { return 0.0; }
    double need = 0.0;
    for (const Wire& w : g.wires()) {
        if (w.from != channel) { continue; }
        const Node* d = g.find(w.to);
        if (d == nullptr || d->kind != NodeKind::Decoder) { continue; }
        const PortType iq = PortType::Iq;
        const std::size_t i = findDecoder(*catalogue, d->plugin, &iq);
        if (i == kNoDecoder) { continue; }
        const DecoderInfo& info = (*catalogue)[i];
        need = std::max(need, info.requiredRateHz);
    }
    return need;
}

// The whole decimation from `deviceRateHz` to kChannelRateHz, or 0 when there
// is none.
//
// EXACT, NOT NEAREST. acars::Demod and every other bit-clocked decoder in this
// product treats its symbol phases as sample offsets within a bit, so a
// fractional samples-per-bit does not degrade the decode, it walks off the
// bit. 2.4 MS/s gives 50, 1.2 gives 25, 0.24 gives 5; 2.048 MS/s reaches
// 42.667 and is refused rather than approximated.
inline unsigned wholeDecimation(double deviceRateHz, double targetHz = kChannelRateHz) {
    if (!(deviceRateHz > 0.0) || !(targetHz > 0.0)) { return 0; }
    const double exact = deviceRateHz / targetHz;
    const double rounded = std::floor(exact + 0.5);
    if (rounded < 1.0) { return 0; }
    // One part in a million: a device that reports 2399999.9 for 2.4 MS/s is
    // reporting 2.4 MS/s, and refusing it would be pedantry rather than care.
    if (std::fabs(exact - rounded) > 1e-6 * rounded) { return 0; }
    return static_cast<unsigned>(rounded);
}

// The band a channel's output must land in for a demodulator to work with it.
// Wide on purpose: the point is to accept the rates real devices produce, not
// to insist on one number.
inline constexpr double kMinChannelRateHz = 24000.0;
inline constexpr double kMaxChannelRateHz = 96000.0;

struct RateChoice {
    unsigned decimation = 0;
    double rateHz = 0.0;
    bool ok = false;
};

// The whole decimation that lands nearest kChannelRateHz while staying inside
// the band above, or nothing when the device rate has none.
//
// WHY NOT EXACTLY 48 kHz. That rule belongs to a bit-clocked DECODER -
// acars::Demod treats its symbol phases as sample offsets within a bit, so a
// fractional samples-per-bit does not degrade, it walks off the bit - and not
// to a channel. Applied to the channel it refused 2.000 MS/s and 2.048 MS/s,
// which are the two commonest rates this product sees, so every channel on a
// generator or an RTL-SDR was marked unbuildable. A channel only has to land
// somewhere a demodulator can work; wholeDecimation() above is kept for the
// day a decoder node says it needs an exact rate of its own.
inline RateChoice chooseChannelRate(double deviceRateHz) {
    RateChoice best;
    if (!(deviceRateHz > 0.0)) { return best; }
    double bestErr = 0.0;
    for (unsigned d = 1; d <= 4096; ++d) {
        const double r = deviceRateHz / static_cast<double>(d);
        if (r < kMinChannelRateHz) { break; }   // only gets smaller from here
        if (r > kMaxChannelRateHz) { continue; }
        const double err = std::fabs(r - kChannelRateHz);
        if (!best.ok || err < bestErr) {
            best = RateChoice{d, r, true};
            bestErr = err;
        }
    }
    return best;
}

// The Channel whose audio reaches a speaker, or kNoNode when none does.
//
// Walks BACKWARDS from an audio Sink: a speaker has exactly one wire into it
// (samples do not fan in), so there is one chain to follow and no choice to
// make along the way. The walk stops at the first Channel it meets, which is
// the strip that has to be resampled and played.
//
// THE FIRST AUDIO SINK WINS when a patch has several. That is a real
// ambiguity - two speakers is not a thing the sound device can honour - and
// picking the first in node order at least makes it stable between frames,
// where picking by position or by whichever was drawn last would make the
// audio change when the user moved a box.
inline NodeId listeningChannel(const Graph& g) {
    for (const Node& sink : g.nodes()) {
        if (sink.kind != NodeKind::Sink) { continue; }
        if (sink.inputs.empty() || sink.inputs[0] != PortType::Audio) { continue; }

        NodeId at = sink.id;
        // Bounded by the node count: the graph refuses cycles, so this cannot
        // loop, but a bound costs nothing and turns a future mistake into a
        // wrong answer rather than a hung audio thread.
        for (std::size_t step = 0; step < g.nodes().size() + 1; ++step) {
            NodeId feeder = kNoNode;
            for (const Wire& w : g.wires()) {
                if (w.to == at) {
                    feeder = w.from;
                    break;
                }
            }
            if (feeder == kNoNode) { break; }
            const Node* n = g.find(feeder);
            if (n == nullptr) { break; }
            if (n->kind == NodeKind::Channel) { return n->id; }
            at = feeder;
        }
    }
    return kNoNode;
}

// The Radio a node's samples come from: its first input, followed upstream
// until a Radio is reached. kNoNode for a node hanging off nothing. Bounded by
// the node count, as listeningChannel is.
inline NodeId radioOf(const Graph& g, NodeId id) {
    NodeId at = id;
    for (std::size_t step = 0; step < g.nodes().size() + 1; ++step) {
        const Node* n = g.find(at);
        if (n == nullptr) { return kNoNode; }
        if (n->kind == NodeKind::Radio) { return at; }
        NodeId feeder = kNoNode;
        for (const Wire& w : g.wires()) {
            if (w.to == at) {
                feeder = w.from;
                break;
            }
        }
        if (feeder == kNoNode) { return kNoNode; }
        at = feeder;
    }
    return kNoNode;
}

inline RadioInfo radioInfoFor(const std::vector<RadioInfo>& radios, NodeId radio) {
    for (const RadioInfo& r : radios) {
        if (r.node == radio) { return r; }
    }
    return RadioInfo{radio, 0.0, 0.0};
}

// The channel an audio sink plays: walked back from the sink to the first
// Channel, as listeningChannel does for the first sink.
inline NodeId channelFeeding(const Graph& g, NodeId sink) {
    NodeId at = sink;
    for (std::size_t step = 0; step < g.nodes().size() + 1; ++step) {
        NodeId feeder = kNoNode;
        for (const Wire& w : g.wires()) {
            if (w.to == at) {
                feeder = w.from;
                break;
            }
        }
        if (feeder == kNoNode) { return kNoNode; }
        const Node* n = g.find(feeder);
        if (n == nullptr) { return kNoNode; }
        if (n->kind == NodeKind::Channel) { return n->id; }
        at = feeder;
    }
    return kNoNode;
}

// EVERY RADIO IS ITS OWN (0.99.17). Each channel and decoder is measured
// against the radio it hangs off - its rate, its centre - taken from `radios`
// (the running device's readback, or the node's own settings before it opens).
// `checkDevices` adds the device rules: a Radio must name a device, and no two
// Radios may name the same one. It is off only for the single-receiver
// overload below, which predates radios having devices of their own.
inline Plan compile(const Graph& g, const std::vector<RadioInfo>& radios,
                    const std::vector<DecoderInfo>* catalogue = nullptr,
                    bool checkDevices = true) {
    Plan plan;
    plan.order = g.evaluationOrder();

    if (checkDevices) {
        std::vector<const Node*> seen;
        for (const Node& n : g.nodes()) {
            if (n.kind != NodeKind::Radio) { continue; }
            if (n.device.empty()) {
                plan.problems.push_back({n.id, Problem::NoDevice});
                continue;
            }
            for (const Node* earlier : seen) {
                if (sameDevice(earlier->device, n.device)) {
                    plan.problems.push_back({n.id, Problem::DeviceTwice});
                    break;
                }
            }
            seen.push_back(&n);
        }
    }

    // Which nodes have a radio somewhere upstream. Walked in evaluation order,
    // so a node's feeders are always decided before it is - that ordering is
    // the whole reason evaluationOrder exists.
    std::vector<NodeId> fed;
    const auto isFed = [&fed](NodeId id) {
        return std::find(fed.begin(), fed.end(), id) != fed.end();
    };

    for (const NodeId id : plan.order) {
        const Node* n = g.find(id);
        if (n == nullptr) { continue; }

        if (n->kind == NodeKind::Radio) {
            fed.push_back(id);
            continue;
        }

        // Does anything wire into it at all, and is any of that fed?
        bool anyInput = false;
        bool anyFedInput = false;
        for (const Wire& w : g.wires()) {
            if (w.to != id) { continue; }
            anyInput = true;
            if (isFed(w.from)) { anyFedInput = true; }
        }

        if (!n->inputs.empty() && !anyInput) {
            plan.problems.push_back({id, Problem::NothingFeedsIt});
        } else if (!anyFedInput) {
            // Wired, but to something that is not itself fed - a chain hanging
            // off nothing. Reported once, here, rather than at every node
            // along it, because the node the user has to fix is this one.
            plan.problems.push_back({id, Problem::NotFedByARadio});
        } else {
            fed.push_back(id);
        }
    }

    // Channels: where they sit, and whether the rate can get there.
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Channel) { continue; }
        if (!isFed(n.id)) { continue; }  // already reported above

        if (n.freqHz <= 0.0) {
            plan.problems.push_back({n.id, Problem::NoFrequency});
            continue;
        }
        const NodeId radio = radioOf(g, n.id);
        const RadioInfo ri = radioInfoFor(radios, radio);
        const double deviceRateHz = ri.rateHz;
        const double halfBand = 0.5 * deviceRateHz * kUsableBandFraction;
        const double offset = n.freqHz - ri.centreHz;
        if (std::fabs(offset) > halfBand) {
            plan.problems.push_back({n.id, Problem::OutOfBand});
            continue;
        }
        RateChoice rc = chooseChannelRate(deviceRateHz);
        if (!rc.ok) {
            plan.problems.push_back({n.id, Problem::RateUnreachable});
            continue;
        }
        // AN I/Q DECODER DOWNSTREAM SETS A FLOOR. The channel normally runs
        // near 48 kHz because that is what a demodulator wants, but a plugin
        // that needs 192 kHz of baseband cannot be fed from 48 kHz by any
        // amount of resampling - the bandwidth is simply gone. So the channel
        // runs at the slowest whole division of the device rate that is still
        // at least as fast as its fastest I/Q decoder asks for. A demodulator
        // on the same channel copes: its audio is resampled to the sink's
        // rate whatever the channel runs at.
        const double need = fastestIqDecoderOn(g, n.id, catalogue);
        if (need > rc.rateHz) {
            const auto d = static_cast<unsigned>(std::floor(deviceRateHz / need));
            if (d >= 1) {
                rc.decimation = d;
                rc.rateHz = deviceRateHz / static_cast<double>(d);
            }
            // d == 0: the radio itself is slower than the plugin needs. The
            // channel still runs for anything else on it; the DECODER is the
            // node that cannot, and it says so below.
        }
        plan.channels.push_back({n.id, offset, rc.decimation, rc.rateHz, radio});
    }

    // Decoders: which plugin, fed from what, at what rate.
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Decoder) { continue; }
        if (!isFed(n.id)) { continue; }  // already reported above
        if (catalogue == nullptr) { continue; }  // a caller that knows no plugins

        if (n.plugin.empty()) {
            plan.problems.push_back({n.id, Problem::NoPlugin});
            continue;
        }
        if (findDecoder(*catalogue, n.plugin) == kNoDecoder) {
            plan.problems.push_back({n.id, Problem::PluginMissing});
            continue;
        }
        // Installed - but with the input this node's port carries?
        const std::size_t idx =
            n.inputs.empty() ? kNoDecoder : findDecoder(*catalogue, n.plugin, &n.inputs[0]);
        if (idx == kNoDecoder) {
            plan.problems.push_back({n.id, Problem::PluginWrongFeed});
            continue;
        }
        const DecoderInfo& info = (*catalogue)[idx];

        DecoderPlan dp;
        dp.node = n.id;
        dp.plugin = idx;
        dp.radio = radioOf(g, n.id);
        const RadioInfo ri = radioInfoFor(radios, dp.radio);

        // What is wired into it. Exactly one wire: samples do not fan in.
        const Node* feeder = nullptr;
        for (const Wire& w : g.wires()) {
            if (w.to == n.id) {
                feeder = g.find(w.from);
                break;
            }
        }
        if (feeder == nullptr) { continue; }

        const ChannelPlan* chan = nullptr;
        if (feeder->kind == NodeKind::Radio) {
            dp.source = DecoderSource::Radio;
            dp.inRateHz = ri.rateHz;
            dp.centreHz = ri.centreHz;
        } else if (feeder->kind == NodeKind::Channel) {
            chan = findChannel(plan, feeder->id);
            dp.source = DecoderSource::Channel;
            dp.centreHz = feeder->freqHz;
        } else if (feeder->kind == NodeKind::Demod) {
            // The channel behind the demodulator.
            for (const Wire& w : g.wires()) {
                if (w.to == feeder->id) {
                    chan = findChannel(plan, w.from);
                    break;
                }
            }
            dp.source = DecoderSource::Audio;
        } else {
            continue;
        }
        if (dp.source != DecoderSource::Radio) {
            // A channel that did not make the plan carries its own problem
            // already; the decoder behind it simply has nothing to run on.
            if (chan == nullptr) { continue; }
            dp.channel = chan->node;
            dp.inRateHz = chan->outRateHz;
        }

        // The rate create() is told. "Any" means the source's own rate and
        // no resampler. Anything else must be reachable: an I/Q decoder cannot
        // be given bandwidth its source never had, while an audio decoder may
        // be upsampled, since audio past the demodulator is what it is.
        //
        // AN "ANY RATE" AUDIO DECODER IS FED 48 kHz, not the channel's own
        // rate. The receiver's plugin runner hands such a plugin post-demod
        // audio at the product's audio rate, and plugins that declare "any"
        // have only ever been exercised there - so the patch gives them the
        // same input rather than a 62.5 kHz or 47.6 kHz stream they have
        // never seen. An "any rate" I/Q decoder takes the source's rate, which
        // is exactly what the receiver's runner does with the device rate.
        double want = info.requiredRateHz;
        if (!(want > 0.0) && info.feed == PortType::Audio) { want = kChannelRateHz; }
        if (!(want > 0.0) || std::fabs(want - dp.inRateHz) <= 1e-6 * want) {
            dp.rateHz = dp.inRateHz;
        } else {
            if (info.feed == PortType::Iq && want > dp.inRateHz) {
                plan.problems.push_back({n.id, Problem::DecoderTooFast});
                continue;
            }
            if (resampleInputRate(dp.inRateHz, want) == 0) {
                plan.problems.push_back({n.id, Problem::DecoderTooFast});
                continue;
            }
            dp.rateHz = want;
        }
        plan.decoders.push_back(dp);
    }

    // Every speaker whose chain reaches a planned channel.
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Sink) { continue; }
        if (n.inputs.empty() || n.inputs[0] != PortType::Audio) { continue; }
        if (!isFed(n.id)) { continue; }
        const NodeId chan = channelFeeding(g, n.id);
        if (chan == kNoNode || findChannel(plan, chan) == nullptr) { continue; }
        plan.sinks.push_back({n.id, chan, radioOf(g, n.id)});
    }

    // A decoder wired to a Map whose module has nothing to put on one. Said on
    // the decoder, advisory: its text still works, only the map stays empty.
    if (catalogue != nullptr) {
        for (const Wire& w : g.wires()) {
            const Node* from = g.find(w.from);
            const Node* to = g.find(w.to);
            if (from == nullptr || to == nullptr || to->kind != NodeKind::Map) { continue; }
            if (from->kind != NodeKind::Decoder || from->plugin.empty()) { continue; }
            const std::size_t i = findDecoder(*catalogue, from->plugin);
            if (i != kNoDecoder && !(*catalogue)[i].tracks) {
                plan.problems.push_back({from->id, Problem::NoTracks});
            }
        }
    }

    // Anything that produces and is heard by nobody. Advisory.
    for (const Node& n : g.nodes()) {
        if (n.outputs.empty()) { continue; }
        bool heard = false;
        for (const Wire& w : g.wires()) {
            if (w.from == n.id) {
                heard = true;
                break;
            }
        }
        if (!heard) { plan.problems.push_back({n.id, Problem::NothingListens}); }
    }

    const bool blocked = std::any_of(plan.problems.begin(), plan.problems.end(),
                                     [](const NodeProblem& p) { return !isAdvisory(p.problem); });
    // A patch with nothing in it is not "runnable" - there is nothing to run.
    // Saying otherwise would put a green light on an empty canvas.
    const bool anySink = std::any_of(g.nodes().begin(), g.nodes().end(), [](const Node& n) {
        return n.kind == NodeKind::Sink || n.kind == NodeKind::Display ||
               n.kind == NodeKind::Map;
    });
    plan.runnable = !blocked && anySink;
    return plan;
}

// ONE RECEIVER'S RADIO, as the patch was before radios had devices: every
// Radio node is taken to be running at this rate and centre, and no device
// rules apply.
inline Plan compile(const Graph& g, double deviceRateHz, double radioCentreHz,
                    const std::vector<DecoderInfo>* catalogue = nullptr) {
    std::vector<RadioInfo> radios;
    for (const Node& n : g.nodes()) {
        if (n.kind == NodeKind::Radio) { radios.push_back({n.id, deviceRateHz, radioCentreHz}); }
    }
    return compile(g, radios, catalogue, false);
}

// THE MODULES A MAP SHOWS (0.99.18): the display name of the plugin behind
// every decoder wired into it - the name the host tags each track with -
// once each, in wire order. A decoder with no plugin, or one not installed,
// adds nothing.
inline std::vector<std::string> mapSources(const Graph& g, NodeId map,
                                           const std::vector<DecoderInfo>& catalogue) {
    std::vector<std::string> out;
    for (const Wire& w : g.wires()) {
        if (w.to != map) { continue; }
        const Node* from = g.find(w.from);
        if (from == nullptr || from->kind != NodeKind::Decoder) { continue; }
        const std::size_t i = findDecoder(catalogue, from->plugin);
        if (i == kNoDecoder || !catalogue[i].tracks) { continue; }
        if (std::find(out.begin(), out.end(), catalogue[i].name) == out.end()) {
            out.push_back(catalogue[i].name);
        }
    }
    return out;
}

// Every problem pinned to one node, for the canvas and the inspector.
inline std::vector<Problem> problemsFor(const Plan& plan, NodeId id) {
    std::vector<Problem> out;
    for (const NodeProblem& p : plan.problems) {
        if (p.node == id) { out.push_back(p.problem); }
    }
    return out;
}

inline bool hasBlockingProblem(const Plan& plan, NodeId id) {
    for (const NodeProblem& p : plan.problems) {
        if (p.node == id && !isAdvisory(p.problem)) { return true; }
    }
    return false;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_PLAN_HPP
