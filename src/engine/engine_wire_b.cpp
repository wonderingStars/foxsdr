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
