// engine_wire_b.cpp - the wire-b lane's operations in the engine (block 0x8900-0x89FF; docs/multi-radio-records/wire-b.md).
//
// Engine::applyWireB is called from Engine::applyCommand's default branch for any op in the block, under the RadioWork of the command's radio for the
// ops radio_scope.hpp lists. Each op group lives in its own section below; the pure work is in the model headers (core/field_strength.hpp ...).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/field_strength.hpp"
#include "engine/engine.hpp"
#include "engine/engine_wire_b.hpp"
#include "engine/wire_b_sigmeas.hpp"
#include "source/serial_port_win.hpp"

namespace cascade::engine {

namespace fs = cascade::core;

WireBState& Engine::wireBState() {
    if (wireB_ == nullptr) { wireB_ = std::make_shared<WireBState>(); }
    return *wireB_;
}

namespace {

// One refusal, the way applyCommand's own lambda makes it.
FoxCommandResult refusal(FoxCommandResult res, std::int32_t status, const std::string& why) {
    res.status = status;
    res.flags |= FOXAPI_RESULT_REFUSED;
    std::snprintf(res.message, sizeof(res.message), "%s", why.c_str());
    return res;
}

// --- rfmon: field strength ----------------------------------------------------------------------------------------------------------------
FoxCommandResult fieldTable(WireBState& w, const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    if (c.ival[0] != 0 && c.ival[0] != 1) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the table is 0 (antenna factor) or 1 (cable loss)"); }
    if (c.ival[1] < 0 || c.ival[1] > 3) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the frequency unit is 0 Hz, 1 kHz, 2 MHz or 3 GHz"); }
    fs::LossTable& table = c.ival[0] == 0 ? w.antennaFactor : w.cableLoss;
    if (text.empty()) {
        if (table.empty()) { return refusal(res, FOXAPI_NO_CHANGE, "that table is empty already"); }
        table.clear();
        changed = true;
        res.applied[0] = 0.0;
        return res;
    }
    static const fs::FreqUnit units[4] = {fs::FreqUnit::Hz, fs::FreqUnit::kHz, fs::FreqUnit::MHz, fs::FreqUnit::GHz};
    // The model leaves its table EMPTY on a refusal, so the new one is parsed apart and swapped in only when it is good (the old table stays).
    fs::LossTable fresh;
    const fs::LossParseResult pr = fresh.parse(text.data(), text.size(), units[c.ival[1]]);
    if (!pr.ok()) {
        char why[160];
        std::snprintf(why, sizeof(why), "%s%s%zu", fs::lossErrorText(pr.error), pr.line != 0 ? ", line " : "", pr.line);
        if (pr.line == 0) { std::snprintf(why, sizeof(why), "%s", fs::lossErrorText(pr.error)); }
        return refusal(res, FOXAPI_BAD_ARGUMENT, why);
    }
    table = fresh;
    changed = true;
    res.applied[0] = static_cast<double>(table.size());
    return res;
}

FoxCommandResult fieldConfig(WireBState& w, const FoxCommand& c, FoxCommandResult res, bool& changed) {
    if (c.num[0] < -100.0 || c.num[0] > 100.0) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the preamp gain is -100 to 100 dB"); }
    if (c.num[1] < 1.0 || c.num[1] > 1000.0) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the input impedance is 1 to 1000 ohm"); }
    const bool show = c.ival[0] != 0;
    if (w.preampGainDb == c.num[0] && w.impedanceOhm == c.num[1] && w.showField == show) {
        return refusal(res, FOXAPI_NO_CHANGE, "the field-strength settings are those already");
    }
    w.preampGainDb = c.num[0];
    w.impedanceOhm = c.num[1];
    w.showField = show;
    changed = true;
    return res;
}

}  // namespace

// --- sigmeas: the measurement blocks as read operations (wire_b_sigmeas.hpp) ---------------------------------------------------------------------
FoxCommandResult Engine::sigmeasOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    struct Row { std::uint32_t op; wireb::Block block; const char* name; };
    static const Row rows[] = {
        {FOXAPP_OP_SIGMEAS_BANDWIDTH, wireb::Block::Bandwidth, "bandwidth"}, {FOXAPP_OP_SIGMEAS_MODULATION, wireb::Block::Modulation, "modulation"},
        {FOXAPP_OP_SIGMEAS_SYMBOLS, wireb::Block::Symbols, "symbols"},       {FOXAPP_OP_SIGMEAS_VSTIME, wireb::Block::VsTime, "vstime"},
        {FOXAPP_OP_SIGMEAS_BURSTS, wireb::Block::Bursts, "bursts"},          {FOXAPP_OP_SIGMEAS_HOPCHIRP, wireb::Block::HopChirp, "hopchirp"},
        {FOXAPP_OP_SIGMEAS_AUDIO, wireb::Block::Audio, "audio"},             {FOXAPP_OP_SIGMEAS_DRIFT, wireb::Block::Drift, "drift"},
        {FOXAPP_OP_SIGMEAS_HISTOGRAM, wireb::Block::Histogram, "histogram"},
        {FOXAPP_OP_PSEUDO_DOPPLER, wireb::Block::PseudoDoppler, "pseudodoppler"},
    };
    const Row* row = nullptr;
    for (const Row& r : rows) {
        if (r.op == c.op) { row = &r; }
    }
    if (row == nullptr) { return refusal(res, FOXAPI_UNSUPPORTED, "this build has no such operation"); }
    // The parameters: one JSON object (empty text = every default).
    wireb::Json params = wireb::Json::object();
    if (!text.empty()) {
        params = wireb::Json::parse(text, nullptr, false);
        if (params.is_discarded() || !params.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::SpanSpec span;
    std::string why;
    if (!wireb::spanOf(params, span, why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
    // The span from the radio tap: the newest (end_ago + duration) seconds, then the part that ends end_ago before the newest sample.
    cascade::core::Pipeline& pl = rx().pipeline_;
    const bool audio = wireb::blockWantsAudio(row->block);
    const double audioRate = static_cast<double>(cascade::core::Pipeline::kAudioRateHz);
    const double rate = audio ? audioRate : pl.channelRateHz();
    if (!(rate > 0.0) || !pl.running()) { return refusal(res, FOXAPI_FAILED, "the radio is not running"); }
    const std::size_t cap = audio ? pl.scopeAudio().capacity() : pl.scopeIq().capacity();
    const std::size_t ago = static_cast<std::size_t>(std::ceil(span.endAgoS * rate));
    const std::size_t len = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(span.durationS * rate)));
    const std::size_t want = std::min(cap, ago + len);
    std::vector<std::complex<float>> iq;
    std::vector<float> aud;
    std::size_t got = 0;
    if (audio) {
        aud.resize(want);
        got = pl.scopeAudio().snapshot(aud.data(), want);
        aud.resize(got);
    } else {
        iq.resize(want);
        got = pl.scopeIq().snapshot(iq.data(), want);
        iq.resize(got);
    }
    if (got == 0) { return refusal(res, FOXAPI_FAILED, "the radio tap holds no samples yet"); }
    // keep [got - ago - len, got - ago): cut the end, then the front
    const std::size_t endCut = std::min(ago, got - 1);
    const std::size_t used = std::min(len, got - endCut);
    const std::size_t first = got - endCut - used;
    const wireb::RunResult rr = wireb::sigmeasRun(row->block, iq.empty() ? nullptr : iq.data() + first, audio ? 0 : used, rate, aud.empty() ? nullptr : aud.data() + first,
                                                  audio ? used : 0, audioRate, params);
    if (!rr.ok) { return refusal(res, FOXAPI_BAD_ARGUMENT, rr.why); }
    cascade::core::PublishedState ps;
    const bool have = rx().receiverSnapshot_ != nullptr && rx().receiverSnapshot_->read(ps);
    wireb::Json doc = wireb::Json::object();
    doc["op"] = row->name;
    doc["radio"] = scopedSlot() + 1u;
    doc["tuned_hz"] = have ? ps.rx.tunedHz : 0.0;
    doc["rate_hz"] = rate;
    doc["samples"] = used;
    doc["valid"] = rr.valid;
    doc["reason"] = rr.reason;
    doc["result"] = rr.out;
    wireBState().facts["radio." + std::to_string(scopedSlot() + 1u) + ".sigmeas." + row->name] = doc.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
    changed = true;
    res.applied[0] = rr.valid ? 1.0 : 0.0;
    res.applied[1] = static_cast<double>(used);
    if (!rr.valid) { std::snprintf(res.message, sizeof(res.message), "%s", rr.reason.c_str()); }
    return res;
}

// --- bitview: the bit-view library as operations (wire_b_bits.hpp) --------------------------------------------------------------------------------
FoxCommandResult Engine::bitsOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    struct Row { std::uint32_t op; wireb::BitsOp which; const char* name; bool jsonText; };
    static const Row rows[] = {
        {FOXAPP_OP_BITS_KEEP, wireb::BitsOp::Keep, "keep", true},         {FOXAPP_OP_BITS_LIST, wireb::BitsOp::List, "list", true},
        {FOXAPP_OP_BITS_REDECODE, wireb::BitsOp::Redecode, "redecode", true}, {FOXAPP_OP_BITS_SWEEP, wireb::BitsOp::Sweep, "sweep", true},
        {FOXAPP_OP_BITS_FIND_CRC, wireb::BitsOp::FindCrc, "findcrc", true},   {FOXAPP_OP_BITS_FIND_SUM, wireb::BitsOp::FindSum, "findsum", true},
        {FOXAPP_OP_BITS_INFER, wireb::BitsOp::Infer, "infer", true},      {FOXAPP_OP_BITS_DIFF, wireb::BitsOp::Diff, "diff", true},
        {FOXAPP_OP_BITS_SPEC_CHECK, wireb::BitsOp::SpecCheck, "speccheck", false}, {FOXAPP_OP_BITS_SPEC_RUN, wireb::BitsOp::SpecRun, "specrun", false},
        {FOXAPP_OP_BITS_TABLE_CHECK, wireb::BitsOp::TableCheck, "tablecheck", false},
    };
    const Row* row = nullptr;
    for (const Row& r : rows) {
        if (r.op == c.op) { row = &r; }
    }
    if (row == nullptr) { return refusal(res, FOXAPI_UNSUPPORTED, "this build has no such operation"); }
    wireb::Json params = wireb::Json::object();
    if (row->jsonText && !text.empty()) {
        params = wireb::Json::parse(text, nullptr, false);
        if (params.is_discarded() || !params.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    const std::size_t slot = scopedSlot();
    WireBState& w = wireBState();
    std::vector<std::complex<float>> iq;
    double rate = 0.0;
    if (row->which == wireb::BitsOp::Keep) {
        wireb::SpanSpec span;
        std::string why;
        if (!wireb::spanOf(params, span, why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
        cascade::core::Pipeline& pl = rx().pipeline_;
        rate = pl.channelRateHz();
        if (!(rate > 0.0) || !pl.running()) { return refusal(res, FOXAPI_FAILED, "the radio is not running"); }
        const std::size_t ago = static_cast<std::size_t>(std::ceil(span.endAgoS * rate));
        const std::size_t len = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(span.durationS * rate)));
        const std::size_t want = std::min(pl.scopeIq().capacity(), ago + len);
        iq.resize(want);
        const std::size_t got = pl.scopeIq().snapshot(iq.data(), want);
        if (got == 0) { return refusal(res, FOXAPI_FAILED, "the radio tap holds no samples yet"); }
        const std::size_t endCut = std::min(ago, got - 1);
        const std::size_t used = std::min(len, got - endCut);
        const std::size_t first = got - endCut - used;
        iq = std::vector<std::complex<float>>(iq.begin() + static_cast<std::ptrdiff_t>(first), iq.begin() + static_cast<std::ptrdiff_t>(first + used));
    }
    const wireb::RunResult rr = wireb::bitsRun(row->which, w.bits[slot < w.bits.size() ? slot : 0], iq.empty() ? nullptr : iq.data(), iq.size(), rate, text, c.ival[0], params);
    if (!rr.ok) { return refusal(res, FOXAPI_BAD_ARGUMENT, rr.why); }
    wireb::Json doc = wireb::Json::object();
    doc["op"] = row->name;
    doc["radio"] = slot + 1u;
    doc["valid"] = rr.valid;
    doc["reason"] = rr.reason;
    doc["result"] = rr.out;
    w.facts["radio." + std::to_string(slot + 1u) + ".bits." + row->name] = doc.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
    changed = true;
    res.applied[0] = rr.valid ? 1.0 : 0.0;
    if (row->which == wireb::BitsOp::Keep && rr.out.contains("id") && rr.out["id"].is_number()) { res.applied[1] = rr.out["id"].get<double>(); }
    if (!rr.valid) { std::snprintf(res.message, sizeof(res.message), "%s", rr.reason.c_str()); }
    return res;
}

// --- sigmeas: the eight cursors and the marks (wire_b_cursors.hpp) ----------------------------------------------------------------------------------
FoxCommandResult Engine::cursorOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    wireb::Json params = wireb::Json::object();
    if (!text.empty()) {
        params = wireb::Json::parse(text, nullptr, false);
        if (params.is_discarded() || !params.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    const wireb::CursorOp op = c.op == FOXAPP_OP_CURSOR_SET ? wireb::CursorOp::Set : c.op == FOXAPP_OP_CURSOR_READ ? wireb::CursorOp::Read : wireb::CursorOp::Marks;
    std::vector<std::complex<float>> iq;
    double rate = 0.0;
    if (op != wireb::CursorOp::Set) {
        wireb::SpanSpec span;
        std::string why;
        if (!wireb::spanOf(params, span, why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
        cascade::core::Pipeline& pl = rx().pipeline_;
        rate = pl.channelRateHz();
        if (!(rate > 0.0) || !pl.running()) { return refusal(res, FOXAPI_FAILED, "the radio is not running"); }
        const std::size_t ago = static_cast<std::size_t>(std::ceil(span.endAgoS * rate));
        const std::size_t len = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(span.durationS * rate)));
        const std::size_t want = std::min(pl.scopeIq().capacity(), ago + len);
        iq.resize(want);
        const std::size_t got = pl.scopeIq().snapshot(iq.data(), want);
        if (got == 0) { return refusal(res, FOXAPI_FAILED, "the radio tap holds no samples yet"); }
        const std::size_t endCut = std::min(ago, got - 1);
        const std::size_t used = std::min(len, got - endCut);
        const std::size_t first = got - endCut - used;
        iq = std::vector<std::complex<float>>(iq.begin() + static_cast<std::ptrdiff_t>(first), iq.begin() + static_cast<std::ptrdiff_t>(first + used));
    }
    const std::size_t slot = scopedSlot();
    WireBState& w = wireBState();
    const wireb::RunResult rr = wireb::cursorRun(op, w.cursors[slot < w.cursors.size() ? slot : 0], iq.empty() ? nullptr : iq.data(), iq.size(), rate, params);
    if (!rr.ok) { return refusal(res, FOXAPI_BAD_ARGUMENT, rr.why); }
    wireb::Json doc = wireb::Json::object();
    doc["op"] = op == wireb::CursorOp::Marks ? "marks" : "cursors";
    doc["radio"] = slot + 1u;
    doc["valid"] = rr.valid;
    doc["reason"] = rr.reason;
    doc["result"] = rr.out;
    w.facts["radio." + std::to_string(slot + 1u) + ".sigmeas." + (op == wireb::CursorOp::Marks ? "marks" : "cursors")] = doc.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
    changed = true;
    res.applied[0] = rr.valid ? 1.0 : 0.0;
    if (!rr.valid) { std::snprintf(res.message, sizeof(res.message), "%s", rr.reason.c_str()); }
    return res;
}

// --- net-map-models: the track store (core/track_store.hpp) --------------------------------------------------------------------------------------
namespace {
bool trackKindOf(std::uint32_t k, cascade::core::tracks::Kind& out) {
    switch (k) {
        case CASCADE_TRACK_AIRCRAFT: out = cascade::core::tracks::Kind::Aircraft; return true;
        case CASCADE_TRACK_VESSEL: out = cascade::core::tracks::Kind::Ship; return true;
        case CASCADE_TRACK_STATION: out = cascade::core::tracks::Kind::Station; return true;
        default: return false;
    }
}
const char* trackKindWord(cascade::core::tracks::Kind k) {
    switch (k) {
        case cascade::core::tracks::Kind::Aircraft: return "aircraft";
        case cascade::core::tracks::Kind::Ship: return "vessel";
        case cascade::core::tracks::Kind::Station: return "station";
        case cascade::core::tracks::Kind::Balloon: return "balloon";
    }
    return "?";
}
bool trackKindFromWord(const std::string& w, cascade::core::tracks::Kind& out) {
    if (w == "aircraft") { out = cascade::core::tracks::Kind::Aircraft; return true; }
    if (w == "vessel") { out = cascade::core::tracks::Kind::Ship; return true; }
    if (w == "station") { out = cascade::core::tracks::Kind::Station; return true; }
    return false;
}
}  // namespace

FoxCommandResult Engine::tracksOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    if (c.op == FOXAPP_OP_TRACKS_CONFIG) {
        bool enabled = w.tracksEnabled, clear = false;
        double retention = w.tracksRetentionS;
        if (!q.flag("enabled", enabled) || !q.number("retention_s", retention, 60.0, 2592000.0) || !q.flag("clear", clear) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
        if (enabled == w.tracksEnabled && retention == w.tracksRetentionS && !clear) { return refusal(res, FOXAPI_NO_CHANGE, "the track store is set so already"); }
        if (clear && w.trackStore == nullptr && !enabled) { return refusal(res, FOXAPI_NO_CHANGE, "the track store holds nothing"); }
        w.tracksEnabled = enabled;
        w.tracksRetentionS = retention;
        if (enabled && w.trackStore == nullptr) { w.trackStore = std::make_unique<cascade::core::tracks::TrackStore>(); }
        if (w.trackStore != nullptr) { w.trackStore->setRetentionSec(retention); }
        if (clear) { w.trackStore = enabled ? std::make_unique<cascade::core::tracks::TrackStore>() : nullptr; if (w.trackStore) { w.trackStore->setRetentionSec(retention); } w.trackLast.clear(); }
        if (!enabled) { w.trackStore.reset(); w.trackLast.clear(); }   // (disabling drops everything held: the positions are not kept a moment longer than asked)
        changed = true;
        return res;
    }
    // TRACKS_READ
    if (w.trackStore == nullptr) { return refusal(res, FOXAPI_FAILED, "the track store is off or empty"); }
    wireb::Json out = wireb::Json::object();
    q.seen.insert("trail");
    q.seen.insert("range");
    q.seen.insert("t");
    q.seen.insert("kind");
    q.seen.insert("max");
    if (!q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
    const double nowS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (p.contains("range") && p["range"].is_boolean() && p["range"].get<bool>()) {
        double a = 0.0, b = 0.0;
        out["has_range"] = w.trackStore->timeRange(a, b);
        out["first_s"] = a; out["last_s"] = b;
    } else if (p.contains("trail")) {
        const wireb::Json& t = p["trail"];
        if (!t.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"trail\" is an object"); }
        wireb::detail::Params tq(t);
        std::string id, kind = "aircraft";
        double t0 = 0.0, t1 = nowS;
        std::size_t maxPoints = 500;
        if (!tq.word("id", id) || !tq.word("kind", kind) || !tq.number("t0", t0, 0.0, 1e12) || !tq.number("t1", t1, 0.0, 1e12) || !tq.count("max_points", maxPoints, 2, 2000) || !tq.finish()) {
            return refusal(res, FOXAPI_BAD_ARGUMENT, tq.error);
        }
        cascade::core::tracks::Kind k;
        if (!trackKindFromWord(kind, k)) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"kind\" is aircraft, vessel or station"); }
        if (!cascade::core::tracks::idOk(id)) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"id\" is 1 to 24 letters, digits and _ . / -"); }
        wireb::Json pts = wireb::Json::array();
        for (const cascade::core::tracks::Point& pt : w.trackStore->trail(k, id, t0, t1, maxPoints)) {
            pts.push_back({{"t", pt.t}, {"lat", pt.lat}, {"lon", pt.lon}, {"alt_m", wireb::detail::numberOrNull(pt.altM)}});
        }
        out["id"] = id; out["kind"] = kind; out["points"] = pts;
    } else {
        double at = nowS;
        std::string kind;
        std::size_t maxN = 500;
        if (!q.number("t", at, 0.0, 1e12) || !q.word("kind", kind) || !q.count("max", maxN, 1, 2000)) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
        std::optional<cascade::core::tracks::Kind> only;
        if (!kind.empty()) {
            cascade::core::tracks::Kind k;
            if (!trackKindFromWord(kind, k)) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"kind\" is aircraft, vessel or station"); }
            only = k;
        }
        wireb::Json list = wireb::Json::array();
        for (const cascade::core::tracks::Entry& e : w.trackStore->snapshot(at, only)) {
            if (list.size() >= maxN) { break; }
            list.push_back({{"id", e.id}, {"kind", trackKindWord(e.kind)}, {"lat", e.at.lat}, {"lon", e.at.lon}, {"alt_m", wireb::detail::numberOrNull(e.at.altM)},
                            {"interpolated", e.at.interpolated}, {"held", e.at.held}, {"age_s", e.at.ageSec}});
        }
        out["t"] = at; out["tracks"] = list;
    }
    w.facts["tracks.read"] = out.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
    changed = true;
    return res;
}

FoxCommandResult Engine::neededLibOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    (void)c;
    WireBState& w = wireBState();
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    std::string id, path;
    if (!q.word("id", id) || !q.word("path", path) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
    std::vector<const wireb::NeededLibrary*> rows;
    if (id.empty()) {
        if (!path.empty()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "a path goes with one library id"); }
        for (const wireb::NeededLibrary& r : wireb::neededLibraries()) { rows.push_back(&r); }
    } else {
        const wireb::NeededLibrary* r = wireb::findNeededLibrary(id);
        if (r == nullptr) { return refusal(res, FOXAPI_BAD_ARGUMENT, "no such library id"); }
        if (!path.empty() && r->kind != wireb::NeededKind::VendorLibrary) { return refusal(res, FOXAPI_BAD_ARGUMENT, "a file you pick is judged for a vendor library only"); }
        rows.push_back(r);
    }
    for (const wireb::NeededLibrary* r : rows) { w.neededChecks[r->libraryId] = wireb::recheckNeeded(*r, path, w.neededEnv).dump(-1, ' ', false, wireb::Json::error_handler_t::replace); }
    std::int64_t found = 0;
    for (const wireb::NeededLibrary& r : wireb::neededLibraries()) {
        const auto it = w.neededChecks.find(r.libraryId);
        if (it == w.neededChecks.end()) { continue; }
        const wireb::Json j = wireb::Json::parse(it->second, nullptr, false);
        if (j.is_object() && j.value("state", std::string()) == "found") { ++found; }
    }
    res.applied[0] = static_cast<double>(found);
    changed = true;
    return res;
}

FoxCommandResult Engine::coverageOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    if (c.op == FOXAPP_OP_COVERAGE_CONFIG) {
        bool enabled = w.coverageEnabled, clear = false;
        double lat = w.coverageLat, lon = w.coverageLon, cell = w.coverageCell, half = w.coverageHalfLife;
        if (!q.flag("enabled", enabled) || !q.number("rx_lat", lat, -90.0, 90.0) || !q.number("rx_lon", lon, -180.0, 180.0) || !q.number("cell_deg", cell, 0.001, 1.0) ||
            !q.number("half_life_s", half, 3600.0, 31536000.0) || !q.flag("clear", clear) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
        const bool havePos = (p.contains("rx_lat") && p.contains("rx_lon")) || w.coverage != nullptr;
        if (enabled && !havePos) { return refusal(res, FOXAPI_BAD_ARGUMENT, "give the receiver's position (rx_lat and rx_lon) to enable the coverage grid"); }
        if (p.contains("rx_lat") != p.contains("rx_lon")) { return refusal(res, FOXAPI_BAD_ARGUMENT, "rx_lat and rx_lon go together"); }
        if (enabled == w.coverageEnabled && !clear && !p.contains("rx_lat") && !p.contains("cell_deg") && !p.contains("half_life_s")) { return refusal(res, FOXAPI_NO_CHANGE, "the coverage grid is set so already"); }
        w.coverageEnabled = enabled;
        w.coverageLat = lat; w.coverageLon = lon; w.coverageCell = cell; w.coverageHalfLife = half;
        if (!enabled) { w.coverage.reset(); w.coverageLast.clear(); }
        else if (w.coverage == nullptr || clear || p.contains("rx_lat") || p.contains("cell_deg") || p.contains("half_life_s")) {
            cascade::core::adsbcov::Config cfg;
            cfg.rxLat = lat; cfg.rxLon = lon; cfg.cellDeg = cell; cfg.halfLifeSec = half;
            w.coverage = std::make_unique<cascade::core::adsbcov::AdsbCoverage>(cfg);
            w.coverageLast.clear();
        }
        changed = true;
        return res;
    }
    if (w.coverage == nullptr) { return refusal(res, FOXAPI_FAILED, "the coverage grid is off"); }
    double band = -1.0, minWeight = 0.05;
    std::size_t maxCells = 2000;
    if (!q.number("band", band, -1.0, 6.0) || !q.count("max_cells", maxCells, 1, 20000) || !q.number("min_weight", minWeight, 0.0, 1e12) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
    const double nowS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    wireb::Json out = wireb::Json::object();
    wireb::Json cells = wireb::Json::array();
    for (const cascade::core::adsbcov::HeatCell& h : w.coverage->heat(nowS, minWeight, maxCells)) { cells.push_back({{"lat", h.lat}, {"lon", h.lon}, {"weight", h.weight}}); }
    wireb::Json km = wireb::Json::array();
    for (const double v : w.coverage->outlineKm(nowS, static_cast<int>(band))) { km.push_back(v); }
    out["cells"] = cells; out["bin_deg"] = w.coverage->config().binDeg; out["outline_km"] = km; out["max_range_km"] = w.coverage->maxRangeKm(nowS, static_cast<int>(band));
    out["band"] = static_cast<int>(band); out["band_label"] = band >= 0 ? cascade::core::adsbcov::bandLabel(static_cast<int>(band)) : "every band";
    w.facts["coverage.read"] = out.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
    changed = true;
    return res;
}

// Once per control pass, while enabled: every track the front end hands over, at the time its plugin last heard it, when its position moved.
void Engine::pumpTracks() {
    WireBState& w = *wireB_;
    // the coverage grid takes the aircraft of the same feed (altitude in feet; the position of the receiver is the caller's)
    if (w.coverageEnabled && w.coverage != nullptr) {
        const std::shared_ptr<const TrackFeed> cf = trackFeed();
        if (cf != nullptr) {
            const double nowC = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            bool anyC = false;
            for (const cascade::core::HostTrack& ht : cf->polled) {
                const CascadeTrack& t = ht.t;
                if (t.kind != CASCADE_TRACK_AIRCRAFT || !cascade::core::trackHasPosition(t)) { continue; }
                const std::string id(t.id, strnlen(t.id, CASCADE_TRACK_ID_CHARS));
                WireBState::TrackLast now;
                now.lat = std::round(t.latDeg * 1e5) / 1e5; now.lon = std::round(t.lonDeg * 1e5) / 1e5; now.alt = std::isfinite(t.altM) ? std::round(t.altM) : -99999.0;
                const auto it = w.coverageLast.find(id);
                if (it != w.coverageLast.end() && it->second.lat == now.lat && it->second.lon == now.lon && it->second.alt == now.alt) { continue; }
                w.coverageLast[id] = now;
                if (w.coverageLast.size() > 20000) { w.coverageLast.erase(w.coverageLast.begin()); }
                (void)w.coverage->add(nowC - static_cast<double>(t.ageMs) / 1000.0, t.latDeg, t.lonDeg, std::isfinite(t.altM) ? t.altM * 3.280839895 : std::numeric_limits<double>::quiet_NaN());
                anyC = true;
            }
            if (anyC) { ++wireBVersion_; }
        }
    }
    if (!w.tracksEnabled || w.trackStore == nullptr) { return; }
    const std::shared_ptr<const TrackFeed> feed = trackFeed();
    if (feed == nullptr) { return; }
    const double nowS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    bool any = false;
    for (const cascade::core::HostTrack& ht : feed->polled) {
        const CascadeTrack& t = ht.t;
        cascade::core::tracks::Kind kind;
        if (!trackKindOf(t.kind, kind) || !cascade::core::trackHasPosition(t)) { continue; }
        const std::string id(t.id, strnlen(t.id, CASCADE_TRACK_ID_CHARS));
        WireBState::TrackLast now;
        now.lat = std::round(t.latDeg * 1e5) / 1e5;
        now.lon = std::round(t.lonDeg * 1e5) / 1e5;
        now.alt = std::isfinite(t.altM) ? std::round(t.altM) : -99999.0;
        const std::string key = std::string(1, static_cast<char>(kind)) + id;
        const auto it = w.trackLast.find(key);
        if (it != w.trackLast.end() && it->second.lat == now.lat && it->second.lon == now.lon && it->second.alt == now.alt) { continue; }
        cascade::core::tracks::Point pt;
        pt.t = nowS - static_cast<double>(t.ageMs) / 1000.0;
        pt.lat = t.latDeg;
        pt.lon = t.lonDeg;
        pt.altM = t.altM;
        (void)w.trackStore->add(kind, id, pt);
        w.trackLast[key] = now;
        if (w.trackLast.size() > 20000) { w.trackLast.erase(w.trackLast.begin()); }
        any = true;
    }
    if (any) { ++wireBVersion_; }
}

// --- net-outputs: SBS text out of the aircraft tracks (net/aircraft_out.hpp) ----------------------------------------------------------------------
FoxCommandResult Engine::sbsOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    if (c.op == FOXAPP_OP_SBS_OUT_STOP) {
        if (w.sbs == nullptr) { return refusal(res, FOXAPI_NO_CHANGE, "the SBS output is off"); }
        w.sbs->stop();
        w.sbs.reset();
        w.sbsLast.clear();
        changed = true;
        return res;
    }
    if (w.sbs != nullptr) { return refusal(res, FOXAPI_BUSY, "the SBS output is on already"); }
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    double port = 0.0;
    if (!q.number("port", port, 0.0, 65535.0) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
    cascade::net::FanoutConfig cfg = cascade::net::BeastOut::defaultConfig(static_cast<std::uint16_t>(port));
    cfg.bindAddress = "127.0.0.1";
    cfg.allowAllInterfaces = false;
    auto out = std::make_unique<cascade::net::SbsOut>(cfg);
    std::string why;
    if (!out->start(why)) { return refusal(res, FOXAPI_FAILED, why.empty() ? std::string("the output could not start") : why); }
    res.applied[0] = static_cast<double>(out->port());
    w.sbs = std::move(out);
    w.sbsLines = 0;
    w.sbsSeen = 0;
    w.sbsLast.clear();
    changed = true;
    return res;
}

// Once per control pass: the aircraft the front end handed over (the plugins' polled tracks) as SBS lines, only what changed.
void Engine::pumpSbs() {
    WireBState& w = *wireB_;
    if (w.sbs == nullptr) { return; }
    const std::shared_ptr<const TrackFeed> feed = trackFeed();
    if (feed == nullptr) { return; }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint64_t seen = 0;
    for (const cascade::core::HostTrack& ht : feed->polled) {
        const CascadeTrack& t = ht.t;
        if (t.kind != CASCADE_TRACK_AIRCRAFT || !cascade::core::trackHasPosition(t)) { continue; }
        // an ICAO address is six upper-case hex digits: anything else (an MMSI, a callsign) is not an aircraft this format can carry
        const std::string id(t.id, strnlen(t.id, CASCADE_TRACK_ID_CHARS));
        if (id.size() != 6) { continue; }
        bool hex = true;
        for (const char ch : id) { hex = hex && ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')); }
        if (!hex) { continue; }
        ++seen;
        WireBState::SbsLast now;
        now.lat = std::round(t.latDeg * 1e5) / 1e5;
        now.lon = std::round(t.lonDeg * 1e5) / 1e5;
        now.alt = std::isfinite(t.altM) ? std::round(t.altM) : -99999.0;
        now.course = std::isfinite(t.courseDeg) ? std::round(t.courseDeg) : -1.0;
        now.speed = std::isfinite(t.speedMps) ? std::round(t.speedMps * 10.0) / 10.0 : -1.0;
        auto it = w.sbsLast.find(id);
        const bool posChanged = it == w.sbsLast.end() || it->second.lat != now.lat || it->second.lon != now.lon || it->second.alt != now.alt;
        const bool velChanged = it == w.sbsLast.end() || it->second.course != now.course || it->second.speed != now.speed;
        if (!posChanged && !velChanged) { continue; }
        w.sbsLast[id] = now;
        if (w.sbsLast.size() > 4096) { w.sbsLast.erase(w.sbsLast.begin()); }
        cascade::net::SbsMessage m;
        std::snprintf(m.hexIdent, sizeof(m.hexIdent), "%s", id.c_str());
        m.generated = cascade::net::sbsTimeFromUnixMs(static_cast<std::int64_t>(nowMs));
        if (posChanged) {
            m.transmissionType = 3;
            if (std::isfinite(t.altM)) { m.altitudeFt = static_cast<int>(std::lround(t.altM * 3.280839895)); }
            m.latitude = t.latDeg;
            m.longitude = t.lonDeg;
            if (w.sbs->submit(m)) { ++w.sbsLines; }
        }
        if (velChanged && (std::isfinite(t.speedMps) || std::isfinite(t.courseDeg))) {
            cascade::net::SbsMessage v;
            std::snprintf(v.hexIdent, sizeof(v.hexIdent), "%s", id.c_str());
            v.generated = m.generated;
            v.transmissionType = 4;
            if (std::isfinite(t.speedMps)) { v.groundSpeedKt = static_cast<int>(std::lround(t.speedMps * 1.943844492)); }
            if (std::isfinite(t.courseDeg)) { v.trackDeg = t.courseDeg; }
            if (w.sbs->submit(v)) { ++w.sbsLines; }
        }
    }
    const std::uint64_t before = w.sbsLines;
    (void)before;
    if (seen != w.sbsSeen || w.sbsLines != w.sbsLinesReported) { w.sbsSeen = seen; w.sbsLinesReported = w.sbsLines; ++wireBVersion_; }
}

// --- rfmon: radios that follow a master (core/radio_matrix_model.hpp) -------------------------------------------------------------------------------
namespace {
// The absolute tuned frequency of radio `n` (1..8) from its published state; false when it is not open or has none yet.
bool tunedOf(const std::array<std::unique_ptr<Receiver>, kMaxReceivers>& rs, std::size_t slot, double& hz) {
    if (slot >= rs.size() || !rs[slot] || rs[slot]->receiverSnapshot_ == nullptr) { return false; }
    cascade::core::PublishedState ps;
    if (!rs[slot]->receiverSnapshot_->read(ps) || !(ps.rx.tunedHz > 0.0)) { return false; }
    hz = ps.rx.tunedHz;
    return true;
}
}  // namespace

FoxCommandResult Engine::followOp(const FoxCommand& c, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    const auto live = [&](long long n) { return n >= 1 && n <= static_cast<long long>(kMaxReceivers) && receivers_[static_cast<std::size_t>(n - 1)] != nullptr; };
    switch (c.op) {
        case FOXAPP_OP_FOLLOW_SET: {
            if (c.ival[0] < 1 || c.ival[0] > 8 || c.ival[1] < 1 || c.ival[1] > 8) { return refusal(res, FOXAPI_OUT_OF_RANGE, "radio numbers are 1 to 8"); }
            if (c.ival[0] == c.ival[1]) { return refusal(res, FOXAPI_BAD_ARGUMENT, "a radio cannot follow itself"); }
            if (!live(c.ival[0]) || !live(c.ival[1])) { return refusal(res, FOXAPI_NO_DEVICE, "both radios must be open"); }
            const std::size_t f = static_cast<std::size_t>(c.ival[0] - 1), m = static_cast<std::size_t>(c.ival[1] - 1);
            double fh = 0.0, mh = 0.0;
            if (!tunedOf(receivers_, f, fh) || !tunedOf(receivers_, m, mh)) { return refusal(res, FOXAPI_FAILED, "a radio has no frequency yet"); }
            const cascade::core::FollowError e = w.follow.link(static_cast<int>(f), static_cast<int>(m), fh, mh);
            switch (e) {
                case cascade::core::FollowError::None: break;
                case cascade::core::FollowError::AlreadyFollowing: return refusal(res, FOXAPI_BUSY, "that radio follows another one already (FOLLOW_CLEAR first)");
                case cascade::core::FollowError::Cycle: return refusal(res, FOXAPI_BAD_ARGUMENT, "that would make the radios follow each other in a circle");
                case cascade::core::FollowError::SameRadio: return refusal(res, FOXAPI_BAD_ARGUMENT, "a radio cannot follow itself");
                default: return refusal(res, FOXAPI_BAD_ARGUMENT, "that link is not possible");
            }
            w.followLastHz[m] = mh;
            w.followHaveLast[m] = true;
            w.followNote[f].clear();
            res.applied[0] = fh - mh;
            changed = true;
            return res;
        }
        case FOXAPP_OP_FOLLOW_CLEAR: {
            if (c.ival[0] < 0 || c.ival[0] > 8) { return refusal(res, FOXAPI_OUT_OF_RANGE, "radio numbers are 1 to 8 (0 for every link)"); }
            if (w.follow.linkCount() == 0) { return refusal(res, FOXAPI_NO_CHANGE, "no radio follows another"); }
            if (c.ival[0] == 0) {
                for (int i = 0; i < 8; ++i) { w.follow.unlink(i); w.followNote[static_cast<std::size_t>(i)].clear(); }
            } else {
                if (w.follow.masterOf(static_cast<int>(c.ival[0] - 1)) < 0) { return refusal(res, FOXAPI_NO_CHANGE, "that radio follows nothing"); }
                w.follow.unlink(static_cast<int>(c.ival[0] - 1));
                w.followNote[static_cast<std::size_t>(c.ival[0] - 1)].clear();
            }
            changed = true;
            return res;
        }
        case FOXAPP_OP_FOLLOW_LOCK: {
            if (c.ival[0] < 1 || c.ival[0] > 8) { return refusal(res, FOXAPI_OUT_OF_RANGE, "radio numbers are 1 to 8"); }
            const int f = static_cast<int>(c.ival[0] - 1);
            if (w.follow.masterOf(f) < 0) { return refusal(res, FOXAPI_NO_CHANGE, "that radio follows nothing"); }
            const bool want = c.ival[1] != 0;
            if (w.follow.locked(f) == want) { return refusal(res, FOXAPI_NO_CHANGE, want ? "the link is locked already" : "the link is not locked"); }
            w.follow.setLocked(f, want);
            changed = true;
            return res;
        }
        case FOXAPP_OP_FOLLOW_KEEP: {
            if (c.ival[0] < 1 || c.ival[0] > 8) { return refusal(res, FOXAPI_OUT_OF_RANGE, "radio numbers are 1 to 8"); }
            const int f = static_cast<int>(c.ival[0] - 1);
            const int m = w.follow.masterOf(f);
            if (m < 0) { return refusal(res, FOXAPI_NO_CHANGE, "that radio follows nothing"); }
            double fh = 0.0, mh = 0.0;
            if (!tunedOf(receivers_, static_cast<std::size_t>(f), fh) || !tunedOf(receivers_, static_cast<std::size_t>(m), mh)) { return refusal(res, FOXAPI_FAILED, "a radio has no frequency yet"); }
            w.follow.setOffset(f, fh - mh);
            res.applied[0] = fh - mh;
            changed = true;
            return res;
        }
        default:
            return refusal(res, FOXAPI_UNSUPPORTED, "this build has no such operation");
    }
}

// Once per control pass: a master whose tuned frequency moved retunes what follows it (the model's order: a master before its own followers) through the ordinary SET_FREQUENCY.
void Engine::pumpFollow() {
    WireBState& w = *wireB_;
    if (w.follow.linkCount() == 0) { return; }
    bool moved = false;
    std::array<bool, 8> known{};
    for (std::size_t i = 0; i < 8; ++i) { known[i] = receivers_[i] != nullptr; }
    for (std::size_t m = 0; m < 8; ++m) {
        bool isMaster = false;
        for (int f = 0; f < 8; ++f) { if (w.follow.masterOf(f) == static_cast<int>(m)) { isMaster = true; } }
        if (!isMaster) { continue; }
        double hz = 0.0;
        if (!tunedOf(receivers_, m, hz)) { continue; }
        const bool had = w.followHaveLast[m];
        const double last = w.followLastHz[m];
        w.followLastHz[m] = hz;
        w.followHaveLast[m] = true;
        if (!had || hz == last) { continue; }
        for (const cascade::core::FollowMove& mv : w.follow.onMasterMoved(static_cast<int>(m), hz, known)) {
            const std::size_t f = static_cast<std::size_t>(mv.radioId);
            FoxCommand cmd = cascade::core::cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, mv.newHz);
            cmd.target = static_cast<std::uint32_t>(f) + 1u;
            const FoxCommandResult r = applyCommand(cmd);
            w.followNote[f] = r.status == FOXAPI_OK ? std::string(mv.clamped ? "held at the end of the range" : "") : std::string("could not follow: ") + r.message;
            if (r.status == FOXAPI_OK) { w.followLastHz[f] = mv.newHz; }
            moved = true;
        }
    }
    if (moved) { ++wireBVersion_; }
}

// --- net-outputs: network I/Q out (net/iq_out.hpp) -----------------------------------------------------------------------------------------------
void Engine::wireBStopIqOut(std::size_t slot) {
    if (wireB_ == nullptr || slot >= wireB_->iqOut.size()) { return; }
    std::lock_guard<std::mutex> lk(wireB_->mutex);
    wireB_->follow.radioClosed(static_cast<int>(slot));   // (every link this radio was in ends with it)
    wireB_->followHaveLast[slot] = false;
    wireB_->followNote[slot].clear();
    WireBState::IqOutRadio& o = wireB_->iqOut[slot];
    if (o.out == nullptr) { return; }
    if (receivers_[slot]) { receivers_[slot]->pipeline_.setIqSink(nullptr); }   // (returns once no block is inside the sink)
    o.sink.target = nullptr;
    o.out->stop();
    o.out.reset();
    ++wireBVersion_;
}

void Engine::wireBShutdown() {
    if (wireB_ == nullptr) { return; }
    if (wireB_->sbs != nullptr) { wireB_->sbs->stop(); wireB_->sbs.reset(); }
    for (std::size_t i = 0; i < wireB_->iqOut.size(); ++i) { wireBStopIqOut(i); }
}

FoxCommandResult Engine::iqOutOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    const std::size_t slot = scopedSlot();
    WireBState::IqOutRadio& o = w.iqOut[slot < w.iqOut.size() ? slot : 0];
    if (c.op == FOXAPP_OP_IQ_OUT_STOP) {
        if (o.out == nullptr) { return refusal(res, FOXAPI_NO_CHANGE, "this radio sends no I/Q"); }
        rx().pipeline_.setIqSink(nullptr);
        o.sink.target = nullptr;
        o.out->stop();
        o.out.reset();
        changed = true;
        return res;
    }
    if (o.out != nullptr) { return refusal(res, FOXAPI_BUSY, "this radio sends I/Q already"); }
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    std::string format = "u8", dest = "127.0.0.1";
    bool header = false, udp = false;
    double port = 0.0;
    if (!q.word("format", format) || !q.flag("header", header) || !q.flag("udp", udp) || !q.number("port", port, 0.0, 65535.0) || !q.word("dest", dest) || !q.finish()) {
        return refusal(res, FOXAPI_BAD_ARGUMENT, q.error);
    }
    if (format != "u8" && format != "s16" && format != "f32") { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"format\" is u8, s16 or f32"); }
    if (dest != "127.0.0.1") { return refusal(res, FOXAPI_BAD_ARGUMENT, "the destination is 127.0.0.1: another one is the owner's decision"); }
    if (udp && port < 1.0) { return refusal(res, FOXAPI_BAD_ARGUMENT, "UDP needs a port, 1 to 65535"); }
    cascade::core::PublishedState ps;
    const bool have = rx().receiverSnapshot_ != nullptr && rx().receiverSnapshot_->read(ps);
    const double rate = rx().pipeline_.inputRateHz();
    cascade::net::IqOutConfig cfg;
    cfg.format = format == "u8" ? cascade::net::IqFormat::U8 : format == "s16" ? cascade::net::IqFormat::S16 : cascade::net::IqFormat::F32;
    cfg.header = header;
    cfg.sampleRateHz = static_cast<std::uint32_t>(std::llround(rate));
    cfg.centreHz = have ? ps.rx.centreHz : 0.0;
    cfg.udp = udp;
    cfg.tcp.bindAddress = "127.0.0.1";
    cfg.tcp.allowAllInterfaces = false;
    cfg.tcp.port = static_cast<std::uint16_t>(udp ? 0 : port);
    cfg.udpCfg.destAddress = "127.0.0.1";
    cfg.udpCfg.port = static_cast<std::uint16_t>(udp ? port : 0);
    if (header && cfg.sampleRateHz == 0) { return refusal(res, FOXAPI_FAILED, "the radio has no sample rate yet"); }
    auto out = std::make_unique<cascade::net::IqOut>(cfg);
    std::string why;
    if (!out->start(why)) { return refusal(res, FOXAPI_FAILED, why.empty() ? std::string("the output could not start") : why); }
    o.sink.target = out.get();
    o.out = std::move(out);
    o.udp = udp; o.header = header; o.format = format == "u8" ? 1 : format == "s16" ? 2 : 3; o.rateHz = rate;
    rx().pipeline_.setIqSink(&o.sink);
    res.applied[0] = udp ? 0.0 : static_cast<double>(o.out->tcpPort());
    changed = true;
    return res;
}

// --- sweepsrc: the sweep device (wire_b_sweep.hpp) -----------------------------------------------------------------------------------------------
FoxCommandResult Engine::sweepOp(const FoxCommand& c, const std::string& text, FoxCommandResult res, bool& changed) {
    WireBState& w = wireBState();
    wireb::Json p = wireb::Json::object();
    if (!text.empty()) {
        p = wireb::Json::parse(text, nullptr, false);
        if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
    }
    wireb::detail::Params q(p);
    switch (c.op) {
        case FOXAPP_OP_SWEEP_OPEN: {
            std::string kind = "tinysa", port, serial;
            double startMhz = 0.0, stopMhz = 0.0, interval = 0.0, stepMhz = 10.0, lna = 16.0, vga = 16.0, rateMsps = 20.0;
            std::size_t points = 450, fft = 1024;
            bool amp = false;
            if (!q.word("kind", kind) || !q.word("port", port) || !q.word("serial", serial) || !q.number("start_mhz", startMhz, 0.0, 1e6) || !q.number("stop_mhz", stopMhz, 0.0, 1e6) ||
                !q.count("points", points, 2, 1048576) || !q.number("interval_ms", interval, 0.0, 60000.0) || !q.number("step_mhz", stepMhz, 1.0, 20.0) || !q.number("lna_db", lna, 0.0, 40.0) ||
                !q.number("vga_db", vga, 0.0, 62.0) || !q.flag("amp", amp) || !q.number("sample_rate_msps", rateMsps, 2.0, 20.0) || !q.count("fft_size", fft, 32, 65536) || !q.finish()) {
                return refusal(res, FOXAPI_BAD_ARGUMENT, q.error);
            }
            if (kind == "hackrf") {
                // A HackRF in sweep mode (receive only): ONE range of whole MHz, 1 to 6000, LINEAR style; a USB radio has no port name.
                if (!port.empty()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "a HackRF has no port: use serial to pick one of several"); }
                if (!(stopMhz > startMhz) || startMhz < 1.0 || stopMhz > 6000.0 || startMhz != std::floor(startMhz) || stopMhz != std::floor(stopMhz)) {
                    return refusal(res, FOXAPI_BAD_ARGUMENT, "the range is whole MHz, 1 to 6000, with the stop above the start");
                }
                if (fft == 0 || (fft & (fft - 1)) != 0) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"fft_size\" is a power of two, 32 to 65536"); }
                if (w.sweep.running()) { return refusal(res, FOXAPI_BUSY, "a sweep device is open already"); }
                cascade::source::hackrf_sweep::SweepConfig hc;
                hc.plan.rangesMHz = {{static_cast<std::uint16_t>(startMhz), static_cast<std::uint16_t>(stopMhz)}};
                hc.plan.stepWidthHz = static_cast<std::uint32_t>(stepMhz * 1e6);
                hc.plan.offsetHz = hc.plan.stepWidthHz / 2;
                hc.spectrum.sampleRateHz = rateMsps * 1e6;
                hc.spectrum.fftSize = fft;
                hc.spectrum.stepHz = stepMhz * 1e6;
                hc.lnaDb = lna; hc.vgaDb = vga; hc.ampOn = amp;
                const std::string perr = cascade::source::hackrf_sweep::validate(hc.plan);
                if (!perr.empty()) { return refusal(res, FOXAPI_BAD_ARGUMENT, perr); }
                std::string why;
                if (!w.sweep.startHackRf(w.hackrfFactory, serial.empty() ? std::string() : "serial=" + serial, hc, why)) { return refusal(res, FOXAPI_FAILED, why); }
                cascade::core::diagLogf("sweep: opened a HackRF sweep device");
                changed = true;
                break;
            }
            if (!serial.empty()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"serial\" is for a HackRF"); }
            if (kind != "tinysa" && kind != "tinysa_ultra") { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"kind\" is tinysa, tinysa_ultra or hackrf"); }
            if (!cascade::source::tinysa::isValidComPortName(port)) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"port\" is a COM port name, COM1 to COM256"); }
            if (!(stopMhz > startMhz)) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the stop frequency must be above the start frequency"); }
            if (w.sweep.running()) { return refusal(res, FOXAPI_BUSY, "a sweep device is open already"); }
            std::string why;
            if (!w.sweep.start(w.sweepFactory, kind, port, kind == "tinysa" ? cascade::source::tinysa::Model::TinySa : cascade::source::tinysa::Model::TinySaUltra, startMhz * 1e6, stopMhz * 1e6, points,
                               static_cast<int>(interval), why)) { return refusal(res, FOXAPI_FAILED, why); }
            cascade::core::diagLogf("sweep: opened a %s sweep device", kind.c_str());
            changed = true;
            break;
        }
        case FOXAPP_OP_SWEEP_CLOSE:
            if (!w.sweep.running()) {
                w.sweep.stop();   // (a worker that ended by itself is joined here, so nothing is left behind)
                return refusal(res, FOXAPI_NO_CHANGE, "no sweep device is open");
            }
            w.sweep.stop();
            changed = true;
            break;
        case FOXAPP_OP_SWEEP_CONFIG: {
            std::string why;
            q.seen.insert("heatmap");
            q.seen.insert("occupancy");
            if (!q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
            if (p.contains("heatmap")) {
                const wireb::Json& h = p["heatmap"];
                if (!h.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"heatmap\" is an object"); }
                wireb::detail::Params hq(h);
                cascade::core::HeatmapConfig hc;
                std::size_t cols = hc.cols, rows = hc.maxRows;
                std::string fm = "max", tm = "max";
                if (!hq.count("cols", cols, 1, 65536) || !hq.count("rows", rows, 2, 65536) || !hq.word("freq_mode", fm) || !hq.word("time_mode", tm) || !hq.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, hq.error); }
                if ((fm != "max" && fm != "mean") || (tm != "max" && tm != "mean")) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the modes are max or mean"); }
                hc.cols = cols; hc.maxRows = rows;
                hc.freqMode = fm == "max" ? cascade::core::Decimate::Max : cascade::core::Decimate::Mean;
                hc.timeMode = tm == "max" ? cascade::core::Decimate::Max : cascade::core::Decimate::Mean;
                if (!w.sweep.configureHeatmap(hc, why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
            }
            if (p.contains("occupancy")) {
                if (p["occupancy"].is_null()) { w.sweep.clearOccupancy(); }
                else if (!p["occupancy"].is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "\"occupancy\" is an object or null"); }
                else {
                    wireb::detail::Params oq(p["occupancy"]);
                    cascade::core::OccupancyConfig oc;
                    double a = 0.0, b = 0.0, step = 0.0, fixedDb = oc.fixedFloorDb;
                    bool haveFixed = p["occupancy"].contains("fixed_floor_db");
                    if (!oq.number("start_mhz", a, 0.0, 1e6) || !oq.number("stop_mhz", b, 0.0, 1e6) || !oq.number("step_khz", step, 0.001, 1e9) || !oq.number("margin_db", oc.marginDb, 0.0, 200.0) ||
                        !oq.number("floor_percentile", oc.floorPercentile, 0.0, 0.5) || !oq.number("fixed_floor_db", fixedDb, -400.0, 400.0) || !oq.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, oq.error); }
                    if (haveFixed) { oc.fixedFloor = true; oc.fixedFloorDb = fixedDb; }
                    if (!cascade::core::makeChannelPlan(a * 1e6, b * 1e6, step * 1e3, oc.plan, &why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
                    if (!w.sweep.configureOccupancy(oc, why)) { return refusal(res, FOXAPI_BAD_ARGUMENT, why); }
                }
            }
            changed = true;
            break;
        }
        case FOXAPP_OP_SWEEP_READ: {
            std::size_t maxPoints = 512;
            double x = -1.0;
            bool occupancy = false;
            if (!q.count("max_points", maxPoints, 2, 4096) || !q.number("x", x, -1.0, 1.0) || !q.flag("occupancy", occupancy) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
            cascade::core::SweepLine line;
            if (!w.sweep.lastLine(maxPoints, line)) { return refusal(res, FOXAPI_FAILED, "no sweep has arrived yet"); }
            wireb::Json j = wireb::Json::object();
            j["start_hz"] = line.startHz; j["stop_hz"] = line.stopHz; j["time_s"] = line.timeS; j["points"] = line.db.size();
            wireb::Json db = wireb::Json::array();
            for (const float v : line.db) { db.push_back(std::isfinite(v) ? wireb::Json(static_cast<double>(v)) : wireb::Json(nullptr)); }
            j["db"] = db;
            if (x >= 0.0) { double hz = 0.0; if (w.sweep.heatmapFrequencyAt(x, hz)) { j["click_hz"] = hz; } }
            if (occupancy) { j["occupancy_csv"] = w.sweep.occupancyCsv(); }
            w.facts["sweep.read"] = j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
            changed = true;
            break;
        }
        default:
            return refusal(res, FOXAPI_UNSUPPORTED, "this build has no such operation");
    }
    return res;
}

FoxCommandResult Engine::applyWireB(const FoxCommand& c, const std::string& text, FoxCommandResult res) {
    WireBState& w = wireBState();
    std::lock_guard<std::mutex> lk(w.mutex);
    bool changed = false;
    switch (c.op) {
        case FOXAPP_OP_FIELD_TABLE:
            res = fieldTable(w, c, text, res, changed);
            break;
        case FOXAPP_OP_FIELD_CONFIG:
            res = fieldConfig(w, c, res, changed);
            break;
        case FOXAPP_OP_FIELD_READ: {
            if (w.antennaFactor.empty()) { return refusal(res, FOXAPI_FAILED, "no antenna-factor table is loaded"); }
            cascade::core::PublishedState ps;
            if (rx().receiverSnapshot_ == nullptr || !rx().receiverSnapshot_->read(ps) || !(ps.rx.tunedHz > 0.0)) {
                return refusal(res, FOXAPI_FAILED, "the radio has no frequency yet");
            }
            const fs::FieldReadout r = fs::fieldStrengthFromTables(ps.rx.signalDbm, ps.rx.tunedHz, w.antennaFactor, w.cableLoss, w.preampGainDb, w.impedanceOhm);
            if (!r.valid) { return refusal(res, FOXAPI_FAILED, "the field strength could not be worked out at this frequency"); }
            res.applied[0] = r.dbuvPerM;
            res.applied[1] = static_cast<double>((r.afHeld ? 1 : 0) | (r.cableHeld ? 2 : 0) | (ps.rx.calibrated == 0u ? 4 : 0));
            char line[200];
            std::snprintf(line, sizeof(line), "%.1f dBuV/m at %.6f MHz%s%s", r.dbuvPerM, ps.rx.tunedHz / 1e6,
                          (r.afHeld || r.cableHeld) ? " (a table does not reach this frequency: its end value is held)" : "",
                          ps.rx.calibrated == 0u ? " (level not calibrated: an estimate)" : "");
            w.facts["radio." + std::to_string(scopedSlot() + 1u) + ".field.readout"] = line;
            changed = true;
            break;
        }
        case FOXAPP_OP_SIGMEAS_BANDWIDTH:
        case FOXAPP_OP_SIGMEAS_MODULATION:
        case FOXAPP_OP_SIGMEAS_SYMBOLS:
        case FOXAPP_OP_SIGMEAS_VSTIME:
        case FOXAPP_OP_SIGMEAS_BURSTS:
        case FOXAPP_OP_SIGMEAS_HOPCHIRP:
        case FOXAPP_OP_SIGMEAS_AUDIO:
        case FOXAPP_OP_SIGMEAS_DRIFT:
        case FOXAPP_OP_SIGMEAS_HISTOGRAM:
        case FOXAPP_OP_PSEUDO_DOPPLER:
            res = sigmeasOp(c, text, res, changed);
            break;
        case FOXAPP_OP_GPU_FFT_MODE: {
            if (c.ival[0] < 0 || c.ival[0] > 2) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the GPU spectrum mode is 0 CPU, 1 Auto or 2 GPU"); }
            bool all = true;
            for (std::size_t i = 0; i < receivers_.size(); ++i) {
                if (receivers_[i] && receivers_[i]->pipeline_.gpuFftReport().mode != static_cast<int>(c.ival[0])) { all = false; }
            }
            if (all) { return refusal(res, FOXAPI_NO_CHANGE, "every open radio is on that mode already"); }
            for (std::size_t i = 0; i < receivers_.size(); ++i) {
                if (receivers_[i]) { receivers_[i]->pipeline_.setGpuFftMode(static_cast<int>(c.ival[0])); }
            }
            w.facts["gpu_fft.mode"] = std::to_string(c.ival[0]);
            res.applied[0] = static_cast<double>(c.ival[0]);
            changed = true;
            break;
        }
        case FOXAPP_OP_CURSOR_SET:
        case FOXAPP_OP_CURSOR_READ:
        case FOXAPP_OP_CURSOR_MARKS:
            res = cursorOp(c, text, res, changed);
            break;
        case FOXAPP_OP_IQ_OUT_START:
        case FOXAPP_OP_IQ_OUT_STOP:
            res = iqOutOp(c, text, res, changed);
            break;
        case FOXAPP_OP_SBS_OUT_START:
        case FOXAPP_OP_SBS_OUT_STOP:
            res = sbsOp(c, text, res, changed);
            break;
        case FOXAPP_OP_TRACKS_CONFIG:
        case FOXAPP_OP_TRACKS_READ:
            res = tracksOp(c, text, res, changed);
            break;
        case FOXAPP_OP_COVERAGE_CONFIG:
        case FOXAPP_OP_COVERAGE_READ:
            res = coverageOp(c, text, res, changed);
            break;
        case FOXAPP_OP_NEEDED_LIB_CHECK:
            res = neededLibOp(c, text, res, changed);
            break;
        case FOXAPP_OP_FOLLOW_SET:
        case FOXAPP_OP_FOLLOW_CLEAR:
        case FOXAPP_OP_FOLLOW_LOCK:
        case FOXAPP_OP_FOLLOW_KEEP:
            res = followOp(c, res, changed);
            break;
        case FOXAPP_OP_SWEEP_OPEN:
        case FOXAPP_OP_SWEEP_CLOSE:
        case FOXAPP_OP_SWEEP_CONFIG:
        case FOXAPP_OP_SWEEP_READ:
            res = sweepOp(c, text, res, changed);
            break;
        case FOXAPP_OP_MASK_LOAD: {
            if (text.empty()) {
                if (!w.maskLoaded) { return refusal(res, FOXAPI_NO_CHANGE, "no mask is loaded"); }
                w.mask = cascade::core::Mask{};
                w.maskLoaded = false;
                w.maskEnabled = false;
                for (auto& m : w.maskRadios) { m.mon.reset(); m.ev = cascade::core::MaskEvaluator{}; m.bins = 0; m.pass = true; m.anyLimited = false; }
                changed = true;
                break;
            }
            cascade::core::Mask fresh;
            const cascade::core::MaskParseResult pr = cascade::core::parseMask(text, fresh);
            if (!pr.ok()) {
                char why[160];
                if (pr.line != 0) { std::snprintf(why, sizeof(why), "%s, line %zu", cascade::core::maskErrorText(pr.error), pr.line); }
                else { std::snprintf(why, sizeof(why), "%s", cascade::core::maskErrorText(pr.error)); }
                return refusal(res, FOXAPI_BAD_ARGUMENT, why);
            }
            w.mask = fresh;
            w.maskLoaded = true;
            for (auto& m : w.maskRadios) { m.mon.reset(); m.bins = 0; }
            res.applied[0] = static_cast<double>(fresh.segments.size());
            changed = true;
            break;
        }
        case FOXAPP_OP_MASK_CONFIG: {
            wireb::Json p = wireb::Json::object();
            if (!text.empty()) {
                p = wireb::Json::parse(text, nullptr, false);
                if (p.is_discarded() || !p.is_object()) { return refusal(res, FOXAPI_BAD_ARGUMENT, "the parameters are not a JSON object"); }
            }
            wireb::detail::Params q(p);
            bool enabled = w.maskEnabled, capture = w.maskCapture;
            std::size_t raise = w.maskCfg.raiseFrames, clear = w.maskCfg.clearFrames;
            double post = w.maskPostRollS, interval = w.maskCfg.minTriggerIntervalS;
            if (!q.flag("enabled", enabled) || !q.flag("capture", capture) || !q.count("raise_frames", raise, 1, 100) || !q.count("clear_frames", clear, 1, 100) ||
                !q.number("post_roll_s", post, 0.0, 60.0) || !q.number("min_interval_s", interval, 0.0, 3600.0) || !q.finish()) { return refusal(res, FOXAPI_BAD_ARGUMENT, q.error); }
            if (enabled && !w.maskLoaded) { return refusal(res, FOXAPI_FAILED, "load a mask first (MASK_LOAD)"); }
            if (enabled == w.maskEnabled && capture == w.maskCapture && raise == w.maskCfg.raiseFrames && clear == w.maskCfg.clearFrames && post == w.maskPostRollS &&
                interval == w.maskCfg.minTriggerIntervalS) { return refusal(res, FOXAPI_NO_CHANGE, "the mask is set so already"); }
            w.maskEnabled = enabled; w.maskCapture = capture; w.maskPostRollS = post;
            w.maskCfg.raiseFrames = raise; w.maskCfg.clearFrames = clear; w.maskCfg.minTriggerIntervalS = interval;
            w.maskCfg.preRollS = 0.0;   // no I/Q pre-roll ring exists
            w.maskCfg.postRollS = post;
            for (auto& m : w.maskRadios) { m.mon.configure(w.maskCfg); }
            changed = true;
            break;
        }
        case FOXAPP_OP_FRAMELOG_CONFIG: {
            if (c.ival[0] < -1 || c.ival[0] > 1) { return refusal(res, FOXAPI_OUT_OF_RANGE, "keep failed frames is -1 (as it is), 0 or 1"); }
            if (c.ival[1] < 0 || c.ival[1] > 4096) { return refusal(res, FOXAPI_OUT_OF_RANGE, "the frame log holds 1 to 4096 frames (0 leaves it as it is)"); }
            const bool newKeep = c.ival[0] < 0 ? w.frameLog->keepBad() : c.ival[0] == 1;
            const bool capacityChange = c.ival[1] != 0;
            if (newKeep == w.frameLog->keepBad() && !capacityChange) { return refusal(res, FOXAPI_NO_CHANGE, "the frame log is set so already"); }
            if (capacityChange) { w.frameLog = std::make_unique<cascade::sigmeas::FrameLog>(static_cast<std::size_t>(c.ival[1])); }   // (a new capacity starts an empty log)
            w.frameLog->setKeepBad(newKeep);
            res.applied[0] = newKeep ? 1.0 : 0.0;
            changed = true;
            break;
        }
        case FOXAPP_OP_FRAMELOG_READ: {
            const wireb::Json j = wireb::frameLogJson(*w.frameLog, c.ival[0] != 0);
            w.facts["framelog"] = j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace);
            res.applied[0] = static_cast<double>(j["frames"].size());
            changed = true;
            break;
        }
        case FOXAPP_OP_BITS_KEEP:
        case FOXAPP_OP_BITS_LIST:
        case FOXAPP_OP_BITS_REDECODE:
        case FOXAPP_OP_BITS_SWEEP:
        case FOXAPP_OP_BITS_FIND_CRC:
        case FOXAPP_OP_BITS_FIND_SUM:
        case FOXAPP_OP_BITS_INFER:
        case FOXAPP_OP_BITS_DIFF:
        case FOXAPP_OP_BITS_SPEC_CHECK:
        case FOXAPP_OP_BITS_SPEC_RUN:
        case FOXAPP_OP_BITS_TABLE_CHECK:
            res = bitsOp(c, text, res, changed);
            break;
        default:
            return refusal(res, FOXAPI_UNSUPPORTED, "this build has no such operation");
    }
    if (changed) { ++wireBVersion_; }
    return res;
}

// What of the GPU spectrum's state a reader of the fact block would notice (not the counters, which move every pass): folded into the block's fingerprint.
std::uint64_t Engine::wireBStamp() const {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < receivers_.size(); ++i) {
        if (!receivers_[i]) { continue; }
        const cascade::core::Pipeline::GpuFftReport r = receivers_[i]->pipeline_.gpuFftReport();
        h = (h ^ static_cast<std::uint64_t>(r.mode + 1 + 4 * (r.engaged ? 1 : 0) + 8 * r.backend + 16 * static_cast<int>(r.error.size()) + 1000 * static_cast<int>(i))) * 1099511628211ull;
        h = (h ^ (r.engaged ? r.submitted / 256u : 0u)) * 1099511628211ull;
        // (a coarse step: about 2 s of a 240 kS/s radio; a counter that moving every pass would rebuild the fact block every pass)
        if (wireB_ != nullptr && wireB_->iqOut[i].out != nullptr) { h = (h ^ (wireB_->iqOut[i].out->stats().samplesIn >> 19) ^ (wireB_->iqOut[i].out->stats().tcp.accepted << 40)) * 1099511628211ull; h = (h ^ 0x5151u) * 1099511628211ull; }
        if (wireB_ != nullptr) { h = (h ^ static_cast<std::uint64_t>(wireB_->follow.masterOf(static_cast<int>(i)) + 2 + 16 * (wireB_->follow.locked(static_cast<int>(i)) ? 1 : 0) + 64 * static_cast<std::uint64_t>(wireB_->followNote[i].size()))) * 1099511628211ull; }
        if (wireB_ != nullptr && i == 0) {
            const wireb::SweepStatus s = wireB_->sweep.status();
            h = (h ^ static_cast<std::uint64_t>((s.open ? 1 : 2) + 4 * s.lastError.size() + 64 * (s.sweeps / 16u) + 100003 * (s.heatRows / 4u))) * 1099511628211ull;   // (a coarse step: the counters move every sweep)
        }
        if (wireB_ != nullptr && wireB_->maskLoaded) {
            const WireBState::MaskRadio& m = wireB_->maskRadios[i];
            h = (h ^ static_cast<std::uint64_t>((m.pass ? 1 : 2) + 4 * (m.mon.alarm() ? 1 : 0) + 8 * static_cast<int>(m.mon.events().size()) + 4096 * static_cast<int>(m.captures))) * 1099511628211ull;
        }   // (the counters themselves move every pass: a coarse step keeps the facts from going stale for long)
    }
    return h;
}

// THE LIMIT MASK, once per control pass: for every open radio the newest spectrum frame against the mask, the monitor's debounce, the log line of a new event, the capture of a breach.
void Engine::pumpWireB() {
    if (wireB_ == nullptr) { return; }
    {
        std::lock_guard<std::mutex> lk(wireB_->mutex);
        pumpFollow();
        pumpSbs();
        pumpTracks();
    }
    if (!wireB_->maskEnabled || !wireB_->maskLoaded) { return; }
    WireBState& w = *wireB_;
    std::lock_guard<std::mutex> lk(w.mutex);
    const double nowS = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    bool moved = false;
    for (std::size_t slot = 0; slot < receivers_.size(); ++slot) {
        if (!receivers_[slot]) { continue; }
        RadioWork radioWork(*this, slot);
        WireBState::MaskRadio& m = w.maskRadios[slot];
        cascade::core::Pipeline& pl = rx().pipeline_;
        // a capture in progress ends post_roll_s after it began
        if (m.captureStartS >= 0.0 && nowS - m.captureStartS >= w.maskPostRollS) {
            stopIqRecording();
            m.captureStartS = -1.0;
            m.captureNote = "captured";
            moved = true;
        }
        cascade::core::SpectrumFrame frame;
        frame.seq = m.seq;
        if (!pl.getLatestFrame(frame) || frame.dbBins.empty()) { continue; }
        m.seq = frame.seq;
        cascade::core::PublishedState ps;
        if (rx().receiverSnapshot_ == nullptr || !rx().receiverSnapshot_->read(ps) || !(ps.rx.sampleRateHz > 0.0)) { continue; }
        if (m.bins != frame.dbBins.size() || m.centre != ps.rx.centreHz || m.span != ps.rx.sampleRateHz) {
            if (!m.ev.prepare(w.mask, ps.rx.centreHz, ps.rx.sampleRateHz, frame.dbBins.size())) { continue; }
            m.bins = frame.dbBins.size(); m.centre = ps.rx.centreHz; m.span = ps.rx.sampleRateHz;
        }
        cascade::core::MaskResult r;
        if (!m.ev.evaluate(frame.dbBins.data(), frame.dbBins.size(), r)) { continue; }
        m.mon.update(r, nowS);
        if (m.pass != r.pass || m.anyLimited != r.anyLimited || std::fabs(m.margin - r.worstMarginDb) >= 0.5) { moved = true; }
        m.pass = r.pass; m.anyLimited = r.anyLimited; m.margin = std::isfinite(r.worstMarginDb) ? r.worstMarginDb : 0.0; m.marginFreq = r.worstFreqHz;
        // one log line per event, when it opens (kind, margin, frequency, frames: no identifier)
        const auto& ev = m.mon.events();
        const std::size_t total = ev.size() + m.mon.droppedEvents();
        if (total > m.lastLogged && !ev.empty()) {
            cascade::core::diagLogf("%s", cascade::core::formatBreachLine(ev.back()).c_str());
            m.lastLogged = total;
            moved = true;
        }
        const cascade::core::CaptureTrigger t = m.mon.takeTrigger();
        if (t.pending && w.maskCapture) {
            if (rx().iqRecorder_.recording()) { m.captureNote = "not started: this radio is recording already"; }
            else if (!startIqRecording()) { m.captureNote = "not started: the recording could not start"; }
            else { m.captureStartS = nowS; ++m.captures; m.captureNote = "recording"; }
            moved = true;
        }
    }
    if (moved) { ++wireBVersion_; }
}

void Engine::wireBFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    // The GPU spectrum of every open radio (the pipeline keeps the numbers; nothing is stored here).
    for (std::size_t i = 0; i < receivers_.size(); ++i) {
        if (!receivers_[i]) { continue; }
        const cascade::core::Pipeline::GpuFftReport r = receivers_[i]->pipeline_.gpuFftReport();
        wireb::Json j = wireb::Json::object();
        j["mode"] = r.mode; j["engaged"] = r.engaged; j["backend"] = r.backend == 1 ? "gpu" : "cpu"; j["submitted"] = r.submitted; j["completed_gpu"] = r.completedGpu;
        j["completed_cpu"] = r.completedCpu; j["dropped_frames"] = r.droppedFrames; j["device_lost"] = r.deviceLost; j["error"] = r.error;
        fact("radio." + std::to_string(i + 1) + ".gpufft", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    {
        // the needed libraries: always there (the table, even before any wire-b op ran), with the last check of each
        std::unique_lock<std::mutex> nlk;
        if (wireB_ != nullptr) { nlk = std::unique_lock<std::mutex>(wireB_->mutex); }
        wireb::Json rows = wireb::Json::array();
        for (const wireb::NeededLibrary& r : wireb::neededLibraries()) {
            const bool have = wireB_ != nullptr && wireB_->neededChecks.find(r.libraryId) != wireB_->neededChecks.end();
            rows.push_back(wireb::neededToJson(r, have ? wireb::Json::parse(wireB_->neededChecks[r.libraryId], nullptr, false) : wireb::Json()));
        }
        fact("needed.libs", rows.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    if (wireB_ == nullptr) { return; }
    std::lock_guard<std::mutex> lk(wireB_->mutex);
    {
        const wireb::SweepStatus s = wireB_->sweep.status();
        if (!s.kind.empty()) {
            wireb::Json j = wireb::Json::object();
            j["open"] = s.open; j["kind"] = s.kind; j["sweeps"] = s.sweeps; j["refused"] = s.refused; j["last_error"] = s.lastError; j["points"] = s.points; j["start_hz"] = s.startHz;
            j["stop_hz"] = s.stopHz; j["heat_rows"] = s.heatRows; j["heat_cols"] = s.heatCols; j["occ_channels"] = s.occChannels; j["occ_sweeps"] = s.occSweeps;
            fact("sweep.state", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
        }
    }
    if (wireB_->coverage != nullptr) {
        const cascade::core::adsbcov::Stats s = wireB_->coverage->stats();
        const double nowS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        wireb::Json j = wireb::Json::object();
        j["enabled"] = wireB_->coverageEnabled; j["cells"] = wireB_->coverage->cellCount(); j["added"] = s.added; j["bad_input"] = s.badInput; j["too_far"] = s.tooFar; j["stale"] = s.stale;
        j["future"] = s.future; j["max_range_km"] = wireB_->coverage->maxRangeKm(nowS);
        fact("coverage.state", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    if (wireB_->trackStore != nullptr || wireB_->tracksEnabled) {
        wireb::Json j = wireb::Json::object();
        const cascade::core::tracks::Stats s = wireB_->trackStore != nullptr ? wireB_->trackStore->stats() : cascade::core::tracks::Stats{};
        j["enabled"] = wireB_->tracksEnabled; j["retention_s"] = wireB_->tracksRetentionS; j["added"] = s.added; j["replaced"] = s.replaced; j["refused"] = s.refused; j["too_old"] = s.tooOld;
        j["evicted"] = s.tracksEvicted;
        j["tracks"] = wireB_->trackStore != nullptr ? wireB_->trackStore->snapshot(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()).size() : 0u;
        fact("tracks.state", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    if (wireB_->sbs != nullptr) {
        wireb::Json j = wireb::Json::object();
        j["running"] = true; j["port"] = wireB_->sbs->port(); j["lines_sent"] = wireB_->sbsLines; j["tracks_seen"] = wireB_->sbsSeen; j["clients_accepted"] = wireB_->sbs->stats().accepted;
        fact("sbs.out", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    {
        wireb::Json rows = wireb::Json::array();
        for (std::size_t i = 0; i < receivers_.size(); ++i) {
            if (!receivers_[i]) { continue; }
            cascade::core::PublishedState ps;
            const bool have = receivers_[i]->receiverSnapshot_ != nullptr && receivers_[i]->receiverSnapshot_->read(ps);
            const int master = wireB_->follow.masterOf(static_cast<int>(i));
            rows.push_back({{"radio", i + 1}, {"state", receivers_[i]->pipeline_.running() ? "running" : "stopped"}, {"tuned_hz", have ? ps.rx.tunedHz : 0.0},
                            {"mode", have ? static_cast<int>(ps.rx.demodMode) : 0}, {"level_dbfs", have ? ps.rx.signalDb : -200.0}, {"muted", have && (ps.rx.flags & FOXAPI_RX_MUTED) != 0u},
                            {"follows", master < 0 ? 0 : master + 1}, {"locked", wireB_->follow.locked(static_cast<int>(i))}, {"offset_hz", wireB_->follow.offsetOf(static_cast<int>(i))},
                            {"follow_note", wireB_->followNote[i]}});
        }
        if (!rows.empty()) { fact("matrix.rows", rows.dump(-1, ' ', false, wireb::Json::error_handler_t::replace)); }
    }
    for (std::size_t i = 0; i < receivers_.size(); ++i) {
        if (!receivers_[i] || wireB_->iqOut[i].out == nullptr) { continue; }
        const WireBState::IqOutRadio& o = wireB_->iqOut[i];
        const cascade::net::IqStats s = o.out->stats();
        wireb::Json j = wireb::Json::object();
        j["running"] = true; j["udp"] = o.udp; j["port"] = o.udp ? 0 : o.out->tcpPort(); j["format"] = o.format == 1 ? "u8" : o.format == 2 ? "s16" : "f32"; j["header"] = o.header;
        j["rate_hz"] = o.rateHz; j["samples_in"] = s.samplesIn; j["samples_dropped"] = s.samplesDropped; j["clients_accepted"] = s.tcp.accepted;
        fact("radio." + std::to_string(i + 1) + ".iqout", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
    }
    if (wireB_->maskLoaded) {
        for (std::size_t i = 0; i < receivers_.size(); ++i) {
            if (!receivers_[i]) { continue; }
            const WireBState::MaskRadio& m = wireB_->maskRadios[i];
            wireb::Json j = wireb::Json::object();
            j["enabled"] = wireB_->maskEnabled; j["loaded"] = true; j["pass"] = m.pass; j["limited"] = m.anyLimited; j["worst_margin_db"] = m.margin; j["worst_freq_hz"] = m.marginFreq;
            j["alarm"] = m.mon.alarm(); j["captures"] = m.captures; j["capture_note"] = m.captureNote;
            wireb::Json evs = wireb::Json::array();
            for (const cascade::core::BreachEvent& e : m.mon.events()) {
                evs.push_back({{"start_s", e.startS}, {"end_s", e.endS}, {"frames", e.frames}, {"worst_margin_db", e.worstMarginDb}, {"worst_freq_hz", e.worstFreqHz},
                               {"kind", e.kind == cascade::core::MaskKind::Upper ? "upper" : "lower"}, {"open", e.open}});
            }
            j["events"] = evs;
            fact("radio." + std::to_string(i + 1) + ".mask", j.dump(-1, ' ', false, wireb::Json::error_handler_t::replace));
        }
    }
    for (const auto& kv : wireB_->facts) { fact(kv.first, kv.second); }
    fact("field.afPoints", std::to_string(wireB_->antennaFactor.size()));
    fact("field.cablePoints", std::to_string(wireB_->cableLoss.size()));
    fact("field.show", wireB_->showField ? "1" : "0");
}

}  // namespace cascade::engine
