// patch_blueprints.hpp - a whole working patch for one plugin, placed with
// one pick.
//
// WHAT IT IS FOR (owner, 2026-10-26: "preset blue prints for all the plugins
// ... i should be able to select in the list adsb and it places a full work
// flow for that app on the patch panel"). Until now a user who wanted ADS-B
// on the patch had to know that it eats raw I/Q, that it needs 2 MS/s, that
// it lives at 1090 MHz and that a Map is what shows its aircraft - and then
// place and wire four parts by hand. A plugin already knows every one of
// those facts: its preset table (CASCADE_CAP_PRESET) says where it listens,
// in which mode and at what rate, and its decoder tables say what it eats.
// A blueprint is those facts turned into nodes and wires.
//
// ONE BLUEPRINT PER PART PER PRESET. The parts are the parts bin's own - the
// catalogue AppWindow::rebuildPatchCatalogue builds, with the same rule that
// an audio decoder whose module also decodes I/Q is left out - so the list
// can never offer a part the bin would not. A module with three presets
// (APRS Europe, North America, ISS) is three blueprints; a module with none
// is still one, placed with no frequency, and the channel then says "no
// frequency set" where the user types one.
//
// WHAT A BLUEPRINT BUILDS, by the decoder's input:
//
//   I/Q decoder    Radio --> Decoder        the radio centred ON the preset's
//                    \-> Spectrum            frequency, at its rate: exactly
//                                            what the receiver's own preset
//                                            does for an I/Q decoder, which is
//                                            handed the whole raw band
//   Audio decoder  Radio --> Channel --> Demod --> Decoder
//                    \-> Spectrum
//                                            the channel ON the frequency and
//                                            the radio a quarter of its rate
//                                            below it, so the signal is not
//                                            sitting on the radio's DC spike
//
// and after the decoder a Text out for a text decoder and a Map for a module
// with a track source. The Spectrum is there for every blueprint: it is what
// shows the user the signal is actually arriving, and a Display is one of
// the parts compile() counts as "something to run".
//
// THE RADIO IS REUSED WHEN ONE IS FREE. The page opens with one Radio on the
// receiver's own radio and nothing wired to it. A blueprint that added a
// second Radio there would get whatever device is left - on a machine with
// one dongle, the signal generator - and the workflow would decode nothing.
// So the first Radio with nothing wired out of it is taken over (retuned to
// the blueprint and moved to where the workflow is laid out); only when
// every radio is already in use is a new one added, and at kMaxRadios that
// is refused rather than half-placed.
//
// A PURE FUNCTION OF THE GRAPH, like patch_plan.hpp: the app hands in the
// catalogue and the presets already read out of the plugins, so
// tests/test_patch_blueprints.cpp pins every arrangement without a DLL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_BLUEPRINTS_HPP
#define CASCADE_CORE_PATCH_BLUEPRINTS_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"

namespace cascade::core::patch {

// --- where a plugin listens ----------------------------------------------------
//
// One CascadePreset, copied into plain fields by the app (core must not
// include the ABI's C structs to be tested). The app has already refused the
// presets gui::presetIsValid refuses, so freqHz here is a real frequency.
struct BlueprintTune {
    std::string label;          // the preset's label, already through gui::presetLabel
    double freqHz = 0.0;        // where to listen; 0 = the plugin publishes no preset
    int mode = -1;              // a host mode index (demodIndexForAbi), -1 for "unchanged"
    double rateHz = 0.0;        // the device rate it asks for, 0 for "no preference"
};

// The ABI's demodulator numbering onto the host's mode list (kModeNames:
// NFM, WFM, AM, DSB, USB, CW, LSB, RAW) - the same table applyPluginPreset
// uses, and as deliberately its own: a plugin compiled today must not change
// meaning because the host reordered its buttons. -1 for UNCHANGED and for a
// number past RAW, which a newer plugin may send.
inline int demodIndexForAbi(std::uint32_t abiMode) {
    if (abiMode == 0u || abiMode > 8u) { return -1; }
    return static_cast<int>(abiMode) - 1;
}

// One channel of a LISTEN blueprint: where it is and what it is called.
struct ListenChannel {
    double hz = 0.0;
    std::string name;
};

struct Blueprint {
    // What the list shows before any "(picture)" the app adds: the plugin's
    // name, then the preset's label when it says something the name does not.
    std::string title;
    DecoderInfo decoder;        // the part the workflow is built round
    BlueprintTune tune;
    // A LISTEN BLUEPRINT (owner, 2026-10-27: "add the listen to the tower to
    // the patch panel plugin presets"): no decoder at all, but these channels,
    // each demodulated as AM into a speaker of its own - the Airband section's
    // LISTEN, as a patch. Empty for a plugin's blueprint.
    std::vector<ListenChannel> listen;
};

// --- where a listen blueprint puts the radio -----------------------------------
//
// The centre that gets the MOST of the channels inside the usable band
// (patch_plan.hpp's kUsableBandFraction - outside it compile() refuses the
// channel), with no channel within kListenDcClearHz of it - a zero-IF radio's
// DC spike sits on the centre, and an airband channel on it would hear a
// carrier that is not there - and of those the one nearest the middle of the
// channels it holds, so they sit centred in the band. Candidates are every
// kListenCentreStepHz across a band's width round EACH channel, not only
// round the middle of them all: one far-off channel must not stop the close
// ones from being found together.
inline constexpr double kListenDcClearHz = 25000.0;
inline constexpr double kListenCentreStepHz = 25000.0;

inline bool listenInBand(double hz, double centreHz, double rateHz) {
    return std::fabs(hz - centreHz) <= 0.5 * rateHz * kUsableBandFraction;
}

inline double listenCentre(const std::vector<ListenChannel>& channels, double rateHz) {
    if (channels.empty() || !(rateHz > 0.0)) { return 0.0; }
    const int reach =
        static_cast<int>(std::ceil(0.5 * rateHz * kUsableBandFraction / kListenCentreStepHz));
    double best = channels.front().hz;
    int bestCount = -1;
    double bestOff = 0.0;
    for (const ListenChannel& anchor : channels) {
        for (int k = -reach; k <= reach; ++k) {
            const double c = anchor.hz + static_cast<double>(k) * kListenCentreStepHz;
            if (!(c > 0.0)) { continue; }
            int count = 0;
            bool clear = true;
            double lo = 0.0;
            double hi = 0.0;
            for (const ListenChannel& ch : channels) {
                if (std::fabs(ch.hz - c) < kListenDcClearHz) { clear = false; }
                if (!listenInBand(ch.hz, c, rateHz)) { continue; }
                lo = (count == 0) ? ch.hz : std::min(lo, ch.hz);
                hi = (count == 0) ? ch.hz : std::max(hi, ch.hz);
                ++count;
            }
            if (!clear || count == 0) { continue; }
            const double off = std::fabs(c - 0.5 * (lo + hi));
            if (count > bestCount || (count == bestCount && off < bestOff)) {
                best = c;
                bestCount = count;
                bestOff = off;
            }
        }
    }
    return best;
}

namespace detail {
inline std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}
}  // namespace detail

// EVERY BLUEPRINT THE PARTS BIN'S CATALOGUE CAN MAKE, sorted by title without
// regard to case so ADS-B sits beside AIS and not after every capital.
// `tunes` is keyed by MODULE key (a picture part's key without its
// kImageKeySuffix), because a preset belongs to the plugin, not to one of its
// decoder tables.
inline std::vector<Blueprint> listBlueprints(
    const std::vector<DecoderInfo>& catalogue,
    const std::map<std::string, std::vector<BlueprintTune>>& tunes) {
    std::vector<Blueprint> out;
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        const DecoderInfo& info = catalogue[i];
        // The parts bin's own rule: an audio part whose module also decodes
        // I/Q is not offered.
        if (!info.image && info.feed == PortType::Audio && hasIqTwin(catalogue, i)) { continue; }
        std::string module = info.key;
        if (info.image) {
            const std::string suffix = kImageKeySuffix;
            if (module.size() >= suffix.size() &&
                module.compare(module.size() - suffix.size(), suffix.size(), suffix) == 0) {
                module.erase(module.size() - suffix.size());
            }
        }
        const auto it = tunes.find(module);
        std::vector<BlueprintTune> list;
        if (it != tunes.end()) { list = it->second; }
        if (list.empty()) { list.push_back(BlueprintTune{}); }
        for (const BlueprintTune& t : list) {
            Blueprint bp;
            bp.decoder = info;
            bp.tune = t;
            // "ADS-B" for the one preset that letters only the plugin's name;
            // "APRS: Europe 144.800" where the label adds something.
            bp.title = info.name;
            if (!t.label.empty() && t.label != info.name) { bp.title += ": " + t.label; }
            out.push_back(std::move(bp));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Blueprint& a, const Blueprint& b) {
        return detail::lowered(a.title) < detail::lowered(b.title);
    });
    return out;
}

// The first Radio with nothing wired out of it, or kNoNode. The app asks this
// BEFORE placing, because only a NEW radio needs a device found for it (and
// finding one reads the native device list).
inline NodeId unwiredRadio(const Graph& g) {
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Radio) { continue; }
        const bool wired = std::any_of(g.wires().begin(), g.wires().end(),
                                       [&n](const Wire& w) { return w.from == n.id; });
        if (!wired) { return n.id; }
    }
    return kNoNode;
}

// --- the layout ----------------------------------------------------------------
//
// Left to right in the order the samples flow, a column per stage, with the
// Spectrum under the Radio and the Map under the Text out. World units.
inline constexpr float kBlueprintColumnGap = 64.0f;
inline constexpr float kBlueprintRowGap = 40.0f;
// Below everything already on the canvas, so a blueprint never lands on top
// of the user's own nodes.
inline constexpr float kBlueprintBelowGap = 72.0f;
// Where the first workflow on an empty canvas goes: the starter radio's spot.
inline constexpr float kBlueprintHomeX = 60.0f;
inline constexpr float kBlueprintHomeY = 80.0f;

enum class Place : std::uint8_t {
    Ok,
    RadiosFull,   // every radio is in use and the patch already has kMaxRadios
};

struct Placed {
    Place result = Place::Ok;
    NodeId radio = kNoNode;
    bool reusedRadio = false;     // an unwired Radio was taken over
    NodeId decoder = kNoNode;
    NodeId channel = kNoNode;     // audio decoders only
    std::vector<NodeId> added;    // every node this placement created
    float originX = 0.0f;         // the workflow's top-left, world units
    float originY = 0.0f;
    bool noFrequency = false;     // no preset: nothing was tuned
    // A listen blueprint: how many of its channels were placed, and how many
    // were left out because they do not fit in one radio's band.
    std::size_t listenPlaced = 0;
    std::size_t listenDropped = 0;
};

// The rest of a LISTEN blueprint, once placeBlueprint has the radio and the
// corner: the radio retuned by listenCentre(), a Spectrum under it, and one
// row per channel that fits - Channel on its frequency, Demod in AM (index 2
// of the host's list) with the squelch on, a Speaker set to the speakers
// rather than the default WAV file, since the point is to hear it - in
// ascending frequency. Channels outside the band are counted, not placed.
inline Placed placeListen(Graph& g, const Blueprint& bp, Placed out, double defaultRateHz) {
    std::vector<ListenChannel> channels = bp.listen;
    std::stable_sort(channels.begin(), channels.end(),
                     [](const ListenChannel& a, const ListenChannel& b) { return a.hz < b.hz; });
    double rate = defaultRateHz;
    double centre = 0.0;
    if (Node* r = g.mutableNode(out.radio)) {
        r->on = true;
        if (r->rateHz > 0.0) { rate = r->rateHz; }
        centre = listenCentre(channels, rate);
        r->freqHz = centre;
        r->centreChosen = true;
    }

    float radioW = 0.0f;
    float radioH = 0.0f;
    defaultNodeSize(NodeKind::Radio, radioW, radioH);
    const auto add = [&g, &out](NodeKind kind, const std::string& name, PortType feed, float x,
                                float y) {
        const NodeId id = g.addNode(kind, name, feed, x, y);
        out.added.push_back(id);
        return id;
    };
    const NodeId spectrum = add(NodeKind::Display, "Spectrum", PortType::Iq, out.originX,
                                out.originY + radioH + kBlueprintRowGap);
    g.connect(out.radio, 0, spectrum, 0);

    // Columns: radio (with the wider Spectrum under it - the rows run down
    // past it), channel, demodulator, speaker. A row is as tall as the
    // tallest of its three parts.
    float w = 0.0f;
    float h = 0.0f;
    float specW = 0.0f;
    defaultNodeSize(NodeKind::Display, specW, h);
    defaultNodeSize(NodeKind::Radio, w, h);
    const float chanX = out.originX + std::max(w, specW) + kBlueprintColumnGap;
    defaultNodeSize(NodeKind::Channel, w, h);
    float rowH = h;
    const float demodX = chanX + w + kBlueprintColumnGap;
    defaultNodeSize(NodeKind::Demod, w, h);
    rowH = std::max(rowH, h);
    const float sinkX = demodX + w + kBlueprintColumnGap;
    defaultNodeSize(NodeKind::Sink, w, h);
    rowH = std::max(rowH, h) + kBlueprintRowGap;

    float y = out.originY;
    for (const ListenChannel& c : channels) {
        if (!listenInBand(c.hz, centre, rate)) {
            ++out.listenDropped;
            continue;
        }
        const NodeId ch = add(NodeKind::Channel, c.name, PortType::Iq, chanX, y);
        if (Node* n = g.mutableNode(ch)) { n->freqHz = c.hz; }
        g.connect(out.radio, 0, ch, 0);
        const NodeId dm = add(NodeKind::Demod, "AM", PortType::Iq, demodX, y);
        if (Node* n = g.mutableNode(dm)) {
            n->mode = 2;   // AM
            n->squelch = true;
        }
        g.connect(ch, 0, dm, 0);
        const NodeId spk = add(NodeKind::Sink, c.name, PortType::Audio, sinkX, y);
        if (Node* n = g.mutableNode(spk)) { n->device = "speakers"; }
        g.connect(dm, 0, spk, 0);
        if (out.channel == kNoNode) { out.channel = ch; }
        ++out.listenPlaced;
        y += rowH;
    }
    return out;
}

// Places `bp` on `g`. `newRadioDevice` is the device a NEW radio is opened on
// (ignored when an unwired radio is reused); `defaultRateHz` is what a radio
// with no rate of its own runs at, for the quarter-rate offset.
inline Placed placeBlueprint(Graph& g, const Blueprint& bp, const std::string& newRadioDevice,
                             double defaultRateHz) {
    Placed out;
    const NodeId reuse = unwiredRadio(g);
    if (reuse == kNoNode && g.count(NodeKind::Radio) >= kMaxRadios) {
        out.result = Place::RadiosFull;
        return out;
    }

    // Below everything that is already there, the reused radio excepted (it
    // is about to move to the workflow's corner).
    bool any = false;
    float minX = 0.0f;
    float maxY = 0.0f;
    for (const Node& n : g.nodes()) {
        if (n.id == reuse) { continue; }
        if (!any) {
            minX = n.x;
            maxY = n.y + n.h;
            any = true;
        } else {
            minX = std::min(minX, n.x);
            maxY = std::max(maxY, n.y + n.h);
        }
    }
    out.originX = any ? minX : kBlueprintHomeX;
    out.originY = any ? maxY + kBlueprintBelowGap : kBlueprintHomeY;

    // A column's left edge, from the widest node in each earlier column.
    float colX = out.originX;
    const auto nextColumn = [&colX](NodeKind widest) {
        float w = 0.0f;
        float h = 0.0f;
        defaultNodeSize(widest, w, h);
        colX += w + kBlueprintColumnGap;
    };
    const auto add = [&g, &out](NodeKind kind, const std::string& name, PortType feed, float x,
                                float y) {
        const NodeId id = g.addNode(kind, name, feed, x, y);
        out.added.push_back(id);
        return id;
    };

    // --- the radio ---------------------------------------------------------
    if (reuse != kNoNode) {
        out.radio = reuse;
        out.reusedRadio = true;
        if (Node* r = g.mutableNode(reuse)) {
            r->x = out.originX;
            r->y = out.originY;
        }
    } else {
        // Not translated: a node's name is data saved with the patch, as the
        // parts bin's own labels are.
        out.radio = add(NodeKind::Radio, "Radio", PortType::Iq, out.originX, out.originY);
        if (Node* r = g.mutableNode(out.radio)) { r->device = newRadioDevice; }
    }
    if (!bp.listen.empty()) { return placeListen(g, bp, out, defaultRateHz); }
    const bool iq = bp.decoder.feed == PortType::Iq;
    const bool tuned = bp.tune.freqHz > 0.0;
    out.noFrequency = !tuned;
    if (Node* r = g.mutableNode(out.radio)) {
        r->on = true;
        if (bp.tune.rateHz > 0.0) { r->rateHz = bp.tune.rateHz; }
        if (tuned) {
            // Below the channel, or above it for a station so low that below
            // would be a centre under 0 Hz no radio without a converter has.
            const double quarter = 0.25 * (r->rateHz > 0.0 ? r->rateHz : defaultRateHz);
            double centre = bp.tune.freqHz;
            if (!iq) {
                centre = bp.tune.freqHz - quarter > 0.0 ? bp.tune.freqHz - quarter
                                                        : bp.tune.freqHz + quarter;
            }
            r->freqHz = centre;
            r->centreChosen = true;
        }
    }
    float radioW = 0.0f;
    float radioH = 0.0f;
    defaultNodeSize(NodeKind::Radio, radioW, radioH);
    const NodeId spectrum = add(NodeKind::Display, "Spectrum", PortType::Iq, out.originX,
                                out.originY + radioH + kBlueprintRowGap);
    g.connect(out.radio, 0, spectrum, 0);
    nextColumn(NodeKind::Radio);

    // --- what feeds the decoder ----------------------------------------------
    NodeId feeder = out.radio;
    if (!iq) {
        const std::string chName = bp.tune.label.empty() ? bp.decoder.name : bp.tune.label;
        out.channel = add(NodeKind::Channel, chName, PortType::Iq, colX, out.originY);
        if (Node* c = g.mutableNode(out.channel)) { c->freqHz = tuned ? bp.tune.freqHz : 0.0; }
        g.connect(out.radio, 0, out.channel, 0);
        nextColumn(NodeKind::Channel);
        const NodeId demod = add(NodeKind::Demod, "Demod", PortType::Iq, colX, out.originY);
        if (Node* d = g.mutableNode(demod)) { d->mode = bp.tune.mode >= 0 ? bp.tune.mode : 0; }
        g.connect(out.channel, 0, demod, 0);
        nextColumn(NodeKind::Demod);
        feeder = demod;
    }

    // --- the decoder ---------------------------------------------------------
    out.decoder = add(NodeKind::Decoder, bp.decoder.name, bp.decoder.feed, colX, out.originY);
    if (Node* d = g.mutableNode(out.decoder)) { d->plugin = bp.decoder.key; }
    g.connect(feeder, 0, out.decoder, 0);
    nextColumn(NodeKind::Decoder);

    // --- where its output goes -------------------------------------------------
    // A picture decoder shows its picture on its own face; its text port has
    // nothing to say, so it gets no Text out.
    float y = out.originY;
    if (!bp.decoder.image) {
        const NodeId text = add(NodeKind::Sink, "Text out", PortType::Text, colX, y);
        g.connect(out.decoder, 0, text, 0);
        float w = 0.0f;
        float h = 0.0f;
        defaultNodeSize(NodeKind::Sink, w, h);
        y += h + kBlueprintRowGap;
    }
    if (bp.decoder.tracks) {
        const NodeId map = add(NodeKind::Map, "Map", PortType::Track, colX, y);
        g.connect(out.decoder, 1, map, 0);
    }
    return out;
}

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_BLUEPRINTS_HPP
