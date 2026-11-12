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
