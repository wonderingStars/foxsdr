// Tests for core/patch_blueprints.hpp - a whole working patch for one plugin,
// placed with one pick.
//
// The point of a blueprint is that it RUNS the moment it lands. So every
// placement below is put through the real compile() with the real catalogue
// and must come out with no blocking problem: a blueprint that drew a
// convincing chain and then planned nothing would be exactly the "looks
// wired, decodes nothing" failure the patch page exists to rule out.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_blueprints.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "test_check.hpp"

using cascade::core::patch::Blueprint;
using cascade::core::patch::BlueprintTune;
using cascade::core::patch::compile;
using cascade::core::patch::DecoderInfo;
using cascade::core::patch::DecoderSource;
using cascade::core::patch::demodIndexForAbi;
using cascade::core::patch::Graph;
using cascade::core::patch::isAdvisory;
using cascade::core::patch::kImageKeySuffix;
using cascade::core::patch::kMaxRadios;
using cascade::core::patch::kNoNode;
using cascade::core::patch::listBlueprints;
using cascade::core::patch::mapSources;
using cascade::core::patch::Node;
using cascade::core::patch::NodeId;
using cascade::core::patch::NodeKind;
using cascade::core::patch::Place;
using cascade::core::patch::placeBlueprint;
using cascade::core::patch::Placed;
using cascade::core::patch::Plan;
using cascade::core::patch::PortType;
using cascade::core::patch::Problem;
using cascade::core::patch::problemsFor;
using cascade::core::patch::RadioInfo;
using cascade::core::patch::unwiredRadio;
using cascade::core::patch::Wire;

namespace {

constexpr double kDefaultRate = 2.0e6;   // the patch's own kPatchDefaultRateHz

DecoderInfo part(const std::string& key, const std::string& name, PortType feed,
                 double rate = 0.0, bool tracks = false, bool image = false) {
    DecoderInfo d;
    d.key = key;
    d.name = name;
    d.feed = feed;
    d.requiredRateHz = rate;
    d.tracks = tracks;
    d.image = image;
    return d;
}

// What the installed plugins look like on a typical machine.
std::vector<DecoderInfo> catalogue() {
    return {
        part("pocsag.dll", "POCSAG", PortType::Audio, 22050.0),
        part("adsb.dll", "ADS-B", PortType::Iq, 2.0e6, true),
        // One module, both inputs: only the I/Q part is a blueprint.
        part("acars.dll", "ACARS", PortType::Audio, 48000.0),
        part("acars.dll", "ACARS", PortType::Iq, 0.0),
        part(std::string("apt.dll") + kImageKeySuffix, "NOAA APT", PortType::Audio, 0.0, false,
             true),
        part("ais.dll", "AIS", PortType::Audio, 48000.0, true),
        part("bare.dll", "Bare", PortType::Audio),
    };
}

BlueprintTune tune(const std::string& label, double f, int mode, double rate = 0.0) {
    BlueprintTune t;
    t.label = label;
    t.freqHz = f;
    t.mode = mode;
    t.rateHz = rate;
    return t;
}

std::map<std::string, std::vector<BlueprintTune>> tunes() {
    return {
        {"pocsag.dll", {tune("POCSAG 153.350", 153.35e6, 0)}},
        {"adsb.dll", {tune("ADS-B", 1090.0e6, -1, 2.4e6)}},
        {"acars.dll", {tune("ACARS 131.550", 131.55e6, 2)}},
        {"apt.dll", {tune("NOAA 15", 137.62e6, 1), tune("NOAA 19", 137.1e6, 1)}},
        {"ais.dll", {tune("AIS A", 161.975e6, 0), tune("AIS B", 162.025e6, 0)}},
        // bare.dll publishes no preset at all
    };
}

const Blueprint* byTitle(const std::vector<Blueprint>& list, const std::string& title) {
    for (const Blueprint& b : list) {
        if (b.title == title) { return &b; }
    }
    return nullptr;
}

// Each radio as compile() would see it before it opens: its own rate (or the
// patch default) and its own centre.
std::vector<RadioInfo> radios(const Graph& g) {
    std::vector<RadioInfo> out;
    for (const Node& n : g.nodes()) {
        if (n.kind != NodeKind::Radio) { continue; }
        out.push_back({n.id, n.rateHz > 0.0 ? n.rateHz : kDefaultRate, n.freqHz});
    }
    return out;
}

std::size_t blocking(const Plan& p) {
    std::size_t n = 0;
    for (const auto& np : p.problems) {
        if (!isAdvisory(np.problem)) { ++n; }
    }
    return n;
}

bool wired(const Graph& g, NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    const Wire w{from, fromPort, to, toPort};
    return std::find(g.wires().begin(), g.wires().end(), w) != g.wires().end();
}

NodeId addedOf(const Graph& g, const Placed& p, NodeKind kind) {
    for (const NodeId id : p.added) {
        const Node* n = g.find(id);
        if (n != nullptr && n->kind == kind) { return id; }
    }
    return kNoNode;
}

bool overlap(const Node& a, const Node& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The page's starter: one radio on a real device, nothing wired.
Graph seeded() {
    Graph g;
    const NodeId r = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 60.0f, 80.0f);
    g.mutableNode(r)->device = "rtlsdr|serial=00000001";
    return g;
}

}  // namespace

int main() {
    const std::vector<DecoderInfo> cat = catalogue();
    const std::vector<Blueprint> list = listBlueprints(cat, tunes());

    // --- the ABI's mode numbering ----------------------------------------------
    {
        CHECK(demodIndexForAbi(0u) == -1);   // UNCHANGED
        CHECK(demodIndexForAbi(1u) == 0);    // NFM
        CHECK(demodIndexForAbi(2u) == 1);    // WFM
        CHECK(demodIndexForAbi(5u) == 4);    // USB
        CHECK(demodIndexForAbi(8u) == 7);    // RAW
        CHECK(demodIndexForAbi(9u) == -1);   // a newer plugin's number
    }

    // --- the list ----------------------------------------------------------------
    {
        // pocsag 1 + adsb 1 + acars (I/Q only) 1 + apt 2 + ais 2 + bare 1.
        CHECK(list.size() == 8u);
        // The audio twin of a module that decodes I/Q is not offered.
        int acars = 0;
        for (const Blueprint& b : list) {
            if (b.decoder.key == "acars.dll") {
                ++acars;
                CHECK(b.decoder.feed == PortType::Iq);
            }
        }
        CHECK(acars == 1);
        // A label that only repeats the plugin's name adds nothing to the title.
        CHECK(byTitle(list, "ADS-B") != nullptr);
        CHECK(byTitle(list, "ADS-B: ADS-B") == nullptr);
        // A picture part finds its presets under its MODULE key.
        const Blueprint* noaa = byTitle(list, "NOAA APT: NOAA 19");
        CHECK(noaa != nullptr);
        if (noaa != nullptr) {
            CHECK(noaa->decoder.image);
            CHECK(noaa->tune.freqHz == 137.1e6);
        }
        // A plugin with no preset is still one blueprint, untuned.
        const Blueprint* bare = byTitle(list, "Bare");
        CHECK(bare != nullptr);
        if (bare != nullptr) { CHECK(bare->tune.freqHz == 0.0); }
        // Sorted without regard to case.
        for (std::size_t i = 1; i < list.size(); ++i) {
            CHECK(cascade::core::patch::detail::lowered(list[i - 1].title) <=
                  cascade::core::patch::detail::lowered(list[i].title));
        }
        // No catalogue, no blueprints - and no crash on an empty preset map.
        CHECK(listBlueprints({}, tunes()).empty());
        CHECK(listBlueprints(cat, {}).size() == 6u);
    }

    // --- an I/Q decoder on an empty canvas: ADS-B ------------------------------------
    {
        Graph g;
        const Blueprint* adsb = byTitle(list, "ADS-B");
        CHECK(adsb != nullptr);
        if (adsb != nullptr) {
            const Placed p = placeBlueprint(g, *adsb, "rtlsdr|serial=00000001", kDefaultRate);
            CHECK(p.result == Place::Ok);
            CHECK(!p.reusedRadio);
            CHECK(!p.noFrequency);
            CHECK(p.originX == 60.0f && p.originY == 80.0f);
            const Node* r = g.find(p.radio);
            CHECK(r != nullptr);
            if (r != nullptr) {
                CHECK(r->device == "rtlsdr|serial=00000001");
                // Centred ON the frequency, at the preset's rate - the whole
                // raw band is what an I/Q decoder is handed.
                CHECK(r->freqHz == 1090.0e6);
                CHECK(r->centreChosen);
                CHECK(r->rateHz == 2.4e6);
            }
            // Radio, Spectrum, Decoder, Text out, Map - no channel, no demod.
            CHECK(p.added.size() == 5u);
            CHECK(p.channel == kNoNode);
            CHECK(g.count(NodeKind::Channel) == 0u);
            CHECK(g.count(NodeKind::Demod) == 0u);
            const NodeId spec = addedOf(g, p, NodeKind::Display);
            const NodeId text = addedOf(g, p, NodeKind::Sink);
            const NodeId map = addedOf(g, p, NodeKind::Map);
            CHECK(wired(g, p.radio, 0, spec, 0));
            CHECK(wired(g, p.radio, 0, p.decoder, 0));
            CHECK(wired(g, p.decoder, 0, text, 0));
            CHECK(wired(g, p.decoder, 1, map, 0));
            CHECK(g.find(p.decoder)->plugin == "adsb.dll");

            const Plan plan = compile(g, radios(g), &cat);
            CHECK(blocking(plan) == 0u);
            CHECK(plan.runnable);
            CHECK(plan.decoders.size() == 1u);
            if (!plan.decoders.empty()) {
                CHECK(plan.decoders[0].source == DecoderSource::Radio);
                CHECK(plan.decoders[0].rateHz == 2.0e6);   // resampled to what it needs
            }
            CHECK(mapSources(g, map, cat) == std::vector<std::string>({"ADS-B"}));
        }
    }

    // --- an audio decoder on the starter radio: POCSAG ------------------------------
    {
        Graph g = seeded();
        const NodeId starter = g.nodes()[0].id;
        CHECK(unwiredRadio(g) == starter);
        const Blueprint* pocsag = byTitle(list, "POCSAG: POCSAG 153.350");
        CHECK(pocsag != nullptr);
        if (pocsag != nullptr) {
            const Placed p = placeBlueprint(g, *pocsag, "siggen", kDefaultRate);
            CHECK(p.result == Place::Ok);
            // The starter is taken over, not joined by a second radio that
            // would get the generator on a one-dongle machine.
            CHECK(p.reusedRadio);
            CHECK(p.radio == starter);
            CHECK(g.count(NodeKind::Radio) == 1u);
            const Node* r = g.find(starter);
            CHECK(r->device == "rtlsdr|serial=00000001");   // its device untouched
            // A quarter of the default rate below the channel: off DC.
            CHECK(r->freqHz == 153.35e6 - 0.5e6);
            CHECK(r->rateHz == 0.0);   // no rate asked for, none imposed
            const Node* ch = g.find(p.channel);
            CHECK(ch != nullptr);
            if (ch != nullptr) {
                CHECK(ch->freqHz == 153.35e6);
                CHECK(ch->name == "POCSAG 153.350");
            }
            const NodeId demod = addedOf(g, p, NodeKind::Demod);
            CHECK(g.find(demod)->mode == 0);   // NFM
            CHECK(wired(g, starter, 0, p.channel, 0));
            CHECK(wired(g, p.channel, 0, demod, 0));
            CHECK(wired(g, demod, 0, p.decoder, 0));
            // A text decoder with no map side gets no Map.
            CHECK(g.count(NodeKind::Map) == 0u);
            CHECK(g.count(NodeKind::Sink) == 1u);

            const Plan plan = compile(g, radios(g), &cat);
            CHECK(blocking(plan) == 0u);
            CHECK(plan.runnable);
            CHECK(plan.channels.size() == 1u);
            CHECK(plan.decoders.size() == 1u);
            if (!plan.decoders.empty()) {
                CHECK(plan.decoders[0].source == DecoderSource::Audio);
                CHECK(plan.decoders[0].rateHz == 22050.0);
            }
            CHECK(unwiredRadio(g) == kNoNode);
        }
    }

    // --- a second workflow lands below the first, on a radio of its own -------------
    {
        Graph g = seeded();
        const Placed first = placeBlueprint(g, *byTitle(list, "ADS-B"), "unused", kDefaultRate);
        float bottom = 0.0f;
        for (const Node& n : g.nodes()) { bottom = std::max(bottom, n.y + n.h); }
        const Placed second =
            placeBlueprint(g, *byTitle(list, "AIS: AIS A"), "rtlsdr|serial=00000002", kDefaultRate);
        CHECK(first.reusedRadio);
        CHECK(second.result == Place::Ok);
        CHECK(!second.reusedRadio);
        CHECK(second.radio != first.radio);
        CHECK(g.find(second.radio)->device == "rtlsdr|serial=00000002");
        CHECK(second.originY >= bottom);
        // Nothing overlaps anything, across both workflows.
        bool anyOverlap = false;
        for (std::size_t i = 0; i < g.nodes().size(); ++i) {
            for (std::size_t j = i + 1; j < g.nodes().size(); ++j) {
                if (overlap(g.nodes()[i], g.nodes()[j])) { anyOverlap = true; }
            }
        }
        CHECK(!anyOverlap);
        // An audio decoder with a track source gets both a Text out and a Map.
        CHECK(addedOf(g, second, NodeKind::Sink) != kNoNode);
        CHECK(addedOf(g, second, NodeKind::Map) != kNoNode);
        const Plan plan = compile(g, radios(g), &cat);
        CHECK(blocking(plan) == 0u);
        CHECK(plan.decoders.size() == 2u);
    }

    // --- a station too low to sit a quarter-rate below goes above ------------------
    {
        Graph g;
        Blueprint lf;
        lf.title = "Bare: low";
        lf.decoder = part("bare.dll", "Bare", PortType::Audio);
        lf.tune = tune("low", 77.5e3, 3);
        const Placed p = placeBlueprint(g, lf, "siggen", kDefaultRate);
        CHECK(g.find(p.radio)->freqHz == 77.5e3 + 0.5e6);
        CHECK(g.find(addedOf(g, p, NodeKind::Demod))->mode == 3);
        const Plan plan = compile(g, radios(g), &cat);
        CHECK(problemsFor(plan, p.channel).empty());
    }

    // --- a plugin with no preset: placed, wired, and says what is missing ----------
    {
        Graph g = seeded();
        const double before = g.nodes()[0].freqHz;
        const Placed p = placeBlueprint(g, *byTitle(list, "Bare"), "siggen", kDefaultRate);
        CHECK(p.result == Place::Ok);
        CHECK(p.noFrequency);
        CHECK(g.find(p.radio)->freqHz == before);   // nothing to tune it to
        CHECK(g.find(p.channel)->freqHz == 0.0);
        CHECK(g.find(p.channel)->name == "Bare");
        const Plan plan = compile(g, radios(g), &cat);
        const std::vector<Problem> got = problemsFor(plan, p.channel);
        CHECK(std::find(got.begin(), got.end(), Problem::NoFrequency) != got.end());
    }

    // --- a picture decoder: no Text out, still runnable through its Spectrum --------
    {
        Graph g;
        const Blueprint* noaa = byTitle(list, "NOAA APT: NOAA 15");
        CHECK(noaa != nullptr);
        if (noaa != nullptr) {
            const Placed p = placeBlueprint(g, *noaa, "siggen", kDefaultRate);
            CHECK(g.count(NodeKind::Sink) == 0u);
            CHECK(g.find(p.decoder)->plugin == std::string("apt.dll") + kImageKeySuffix);
            CHECK(g.find(addedOf(g, p, NodeKind::Demod))->mode == 1);   // WFM
            const Plan plan = compile(g, radios(g), &cat);
            CHECK(blocking(plan) == 0u);
            CHECK(plan.runnable);
            CHECK(plan.decoders.size() == 1u);
        }
    }

    // --- every radio in use and five of them: refused, nothing placed ---------------
    {
        Graph g;
        for (std::size_t i = 0; i < kMaxRadios; ++i) {
            const NodeId r = g.addNode(NodeKind::Radio, "Radio");
            const NodeId s = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
            g.connect(r, 0, s, 0);
        }
        const std::size_t nodes = g.nodes().size();
        const std::size_t wires = g.wires().size();
        const Placed p = placeBlueprint(g, *byTitle(list, "ADS-B"), "siggen", kDefaultRate);
        CHECK(p.result == Place::RadiosFull);
        CHECK(p.added.empty());
        CHECK(g.nodes().size() == nodes);
        CHECK(g.wires().size() == wires);
        // ...but a FREE radio among the five is still taken over.
        g.removeNode(g.nodes()[1].id);   // the first radio's spectrum
        const Placed q = placeBlueprint(g, *byTitle(list, "ADS-B"), "siggen", kDefaultRate);
        CHECK(q.result == Place::Ok);
        CHECK(q.reusedRadio);
    }

    // --- LISTEN TO THE TOWER: AM channels into speakers, no decoder ------------------
    {
        using cascade::core::patch::kListenDcClearHz;
        using cascade::core::patch::ListenChannel;
        using cascade::core::patch::listenCentre;
        using cascade::core::patch::listenInBand;
        Blueprint tower;
        tower.title = "Listen to the tower: EGLL";
        // Out of order on purpose: the rows come out ascending.
        tower.listen = {{118700000.0, "TWR 118.700"},
                        {118500000.0, "TWR 118.500"},
                        {124475000.0, "TWR 124.475"}};

        Graph g = seeded();
        const NodeId starter = g.nodes()[0].id;
        const Placed p = placeBlueprint(g, tower, "unused", kDefaultRate);
        CHECK(p.result == Place::Ok);
        CHECK(p.reusedRadio);
        CHECK(p.radio == starter);
        CHECK(p.decoder == kNoNode);
        // 2 MS/s holds 118.5 and 118.7 together but not 124.475 as well.
        CHECK(p.listenPlaced == 2u);
        CHECK(p.listenDropped == 1u);
        CHECK(g.count(NodeKind::Channel) == 2u);
        CHECK(g.count(NodeKind::Demod) == 2u);
        CHECK(g.count(NodeKind::Sink) == 2u);
        CHECK(g.count(NodeKind::Display) == 1u);
        CHECK(g.count(NodeKind::Decoder) == 0u);

        const Node* r = g.find(starter);
        const double centre = r->freqHz;
        CHECK(r->centreChosen);
        std::vector<const Node*> chans;
        for (const Node& n : g.nodes()) {
            if (n.kind == NodeKind::Channel) { chans.push_back(&n); }
        }
        CHECK(chans.size() == 2u);
        if (chans.size() == 2u) {
            CHECK(chans[0]->freqHz == 118500000.0);   // ascending, top row first
            CHECK(chans[1]->freqHz == 118700000.0);
            CHECK(chans[0]->y < chans[1]->y);
            CHECK(chans[0]->name == "TWR 118.500");
            for (const Node* c : chans) {
                // Inside the band and clear of the DC spike at the centre.
                CHECK(listenInBand(c->freqHz, centre, kDefaultRate));
                CHECK(std::fabs(c->freqHz - centre) >= kListenDcClearHz);
            }
        }
        for (const Node& n : g.nodes()) {
            if (n.kind == NodeKind::Demod) {
                CHECK(n.mode == 2);   // AM
                CHECK(n.squelch);
            }
            if (n.kind == NodeKind::Sink) {
                CHECK(n.device == "speakers");   // heard, not written to a file
                CHECK(!n.inputs.empty() && n.inputs[0] == PortType::Audio);
            }
        }
        bool anyOverlap = false;
        for (std::size_t i = 0; i < g.nodes().size(); ++i) {
            for (std::size_t j = i + 1; j < g.nodes().size(); ++j) {
                if (overlap(g.nodes()[i], g.nodes()[j])) { anyOverlap = true; }
            }
        }
        CHECK(!anyOverlap);

        // It RUNS: no blocking problem, two channels and two speakers planned.
        const Plan plan = compile(g, radios(g), &cat);
        CHECK(blocking(plan) == 0u);
        CHECK(plan.runnable);
        CHECK(plan.channels.size() == 2u);
        CHECK(plan.sinks.size() == 2u);

        // A single channel never has the centre on it, and a channel exactly
        // in the middle of a pair pushes the centre off it too.
        const double one = listenCentre({{120000000.0, "a"}}, kDefaultRate);
        CHECK(std::fabs(one - 120000000.0) >= kListenDcClearHz);
        CHECK(listenInBand(120000000.0, one, kDefaultRate));
        const double three = listenCentre(
            {{119000000.0, "a"}, {119500000.0, "b"}, {120000000.0, "c"}}, kDefaultRate);
        CHECK(std::fabs(three - 119500000.0) >= kListenDcClearHz);
        for (const double f : {119000000.0, 119500000.0, 120000000.0}) {
            CHECK(listenInBand(f, three, kDefaultRate));
        }
        CHECK(listenCentre({}, kDefaultRate) == 0.0);

        // With every radio busy it is refused like any blueprint.
        Graph full;
        for (std::size_t i = 0; i < kMaxRadios; ++i) {
            const NodeId rr = full.addNode(NodeKind::Radio, "Radio");
            const NodeId ss = full.addNode(NodeKind::Display, "Spectrum", PortType::Iq);
            full.connect(rr, 0, ss, 0);
        }
        CHECK(placeBlueprint(full, tower, "siggen", kDefaultRate).result == Place::RadiosFull);
    }

    return testSummary("test_patch_blueprints");
}
