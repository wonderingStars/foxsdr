// engine_wire_b.cpp - the wire-b lane's operations in the engine (block 0x8900-0x89FF; docs/multi-radio-records/wire-b.md).
//
// Engine::applyWireB is called from Engine::applyCommand's default branch for any op in the block, under the RadioWork of the command's radio for the
// ops radio_scope.hpp lists. Each op group lives in its own section below; the pure work is in the model headers (core/field_strength.hpp ...).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/app_commands.hpp"
#include "core/field_strength.hpp"
#include "engine/engine.hpp"
#include "engine/engine_wire_b.hpp"
#include "engine/wire_b_sigmeas.hpp"

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

void Engine::wireBFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    if (wireB_ == nullptr) { return; }
    std::lock_guard<std::mutex> lk(wireB_->mutex);
    for (const auto& kv : wireB_->facts) { fact(kv.first, kv.second); }
    fact("field.afPoints", std::to_string(wireB_->antennaFactor.size()));
    fact("field.cablePoints", std::to_string(wireB_->cableLoss.size()));
    fact("field.show", wireB_->showField ? "1" : "0");
}

}  // namespace cascade::engine
