// wire_a_band.cpp - WIRING LANE wire-a, per-band memory (BAND_MEMORY, BAND_MEMORY_NOTE; fact.bandmem.*). The ops are documented in core/wire_a_ops.hpp.
//
// core/band_memory.hpp (row 8) keeps, by the band's NAME in the loaded band plan, what the operator last set on a band. The engine watches each live radio's band once a pass (its tuned frequency looked up in
// bandPlan_; a gap is no band) instead of hooking every tune path, so a digit wheel, a bookmark, a band key, the scanner and a CAT command are all seen. On a change the band left is filed and the band entered
// is applied: volume and AGC by the engine's own ops (so their clamping and bookkeeping are the ops'), the screen-owned values (spectrum top and range, zoom) through fact.bandmem.apply for the screen.
// OFF by default. Nothing is applied for the radio's first look (there is no band it came from), and nothing at all is applied or filed while the switch is off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace t = cascade::core::tuning;
using nlohmann::json;

namespace {
const char* agcWordOf(int i) {
    static const char* const names[5] = {"auto", "fast", "medium", "slow", "off"};
    return i >= 0 && i < 5 ? names[i] : "auto";
}
int agcIndexOf(const std::string& w) {
    for (int i = 0; i < 5; ++i) { if (w == agcWordOf(i)) { return i; } }
    return -1;
}
struct FieldName { const char* name; unsigned bit; };
const FieldName kFields[] = {{"volume", t::kBandVolume}, {"agc", t::kBandAgc}, {"attenuator", t::kBandAttenuator}, {"spectrum", t::kBandSpectrum}, {"zoom", t::kBandZoom}, {"antenna", t::kBandAntenna}};
}  // namespace

void WireA::bandLoadOnce() {
    if (bandLoaded_ || dir().empty()) { return; }
    bandLoaded_ = true;
    bandMem_.setEnabledFields(t::kBandVolume | t::kBandAgc | t::kBandSpectrum | t::kBandZoom);
    const std::string text = readFile("band_memory.json");
    if (text.empty()) { return; }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return; }
    bandOn_ = j.value("enabled", false);
    if (j.contains("mask") && j["mask"].is_number_unsigned()) { bandMem_.setEnabledFields(j["mask"].get<unsigned>()); }
    if (j.contains("memory") && j["memory"].is_object()) {
        t::BandMemory loaded;
        if (loaded.fromJson(j["memory"])) { loaded.setEnabledFields(bandMem_.enabledFields()); bandMem_ = loaded; }
    }
    bump();
}

void WireA::bandSave() {
    saveFile("band_memory.json", json{{"version", 1}, {"enabled", bandOn_}, {"mask", bandMem_.enabledFields()}, {"memory", bandMem_.toJson()}}.dump(1) + "\n");
}

WireA::Result WireA::bandOp(const FoxCommand& c, const std::string& text, Result res) {
    bandLoadOnce();
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
    if (c.op == FOXAPP_OP_BAND_MEMORY_NOTE) {
        const std::size_t slot = e_.scopedSlot();
        if (slot >= bandView_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
        BandView v;
        auto take = [&](const char* name, double& dst, double lo, double hi, std::string& bad) {
            if (!j.contains(name) || j[name].is_null()) { return; }
            if (!j[name].is_number() || !std::isfinite(j[name].get<double>()) || j[name].get<double>() < lo || j[name].get<double>() > hi) { bad = std::string("the field ") + name + " is not valid"; return; }
            dst = j[name].get<double>();
        };
        std::string bad;
        take("spectrumTopDb", v.top, -200.0, 100.0, bad);
        take("spectrumRangeDb", v.range, 10.0, 200.0, bad);
        take("zoomSpanHz", v.zoom, 100.0, 1e10, bad);
        if (!bad.empty()) { return refused(res, FOXAPI_BAD_ARGUMENT, bad); }
        bandView_[slot] = v;
        return res;
    }
    bool on = bandOn_;
    unsigned mask = bandMem_.enabledFields();
    if (j.contains("enabled")) {
        if (!j["enabled"].is_boolean()) { return refused(res, FOXAPI_BAD_ARGUMENT, "enabled is true or false"); }
        on = j["enabled"].get<bool>();
    }
    if (j.contains("fields")) {
        if (!j["fields"].is_array()) { return refused(res, FOXAPI_BAD_ARGUMENT, "fields is a list of names"); }
        mask = 0;
        for (const auto& f : j["fields"]) {
            bool known = false;
            if (f.is_string()) { for (const FieldName& fn : kFields) { if (f.get<std::string>() == fn.name) { mask |= fn.bit; known = true; } } }
            if (!known) { return refused(res, FOXAPI_BAD_ARGUMENT, "a field kind is not one of volume, agc, attenuator, spectrum, zoom, antenna"); }
        }
    }
    bool changed = false;
    if (j.contains("clear") && j["clear"].is_boolean() && j["clear"].get<bool>()) { if (bandMem_.size() > 0) { bandMem_.clear(); changed = true; } }
    if (j.contains("forget")) {
        if (!j["forget"].is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "forget is a band name"); }
        if (bandMem_.forget(j["forget"].get<std::string>())) { changed = true; }
    }
    if (on != bandOn_ || mask != bandMem_.enabledFields()) { changed = true; }
    if (!changed) { return refused(res, FOXAPI_NO_CHANGE, "it is so already"); }
    bandOn_ = on;
    bandMem_.setEnabledFields(mask);
    if (!on) { for (auto& s : bandSeen_) { s = false; } }   // a later switch-on starts from the band each radio is in then
    bandDirty_ = true;
    bandDueS_ = 0.0;
    bump();
    return res;
}

void WireA::bandPass() {
    bandLoadOnce();
    if (!bandOn_) { return; }
    for (std::size_t slot = 0; slot < bandLast_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        Engine::RadioWork work(e_, slot);
        const double hz = e_.currentAbsoluteHz();
        if (!(hz > 0.0) || !std::isfinite(hz)) { continue; }
        const cascade::core::BandEntry* be = e_.bandPlan_.at(hz);
        const std::string now = be != nullptr ? be->name : std::string();
        if (!bandSeen_[slot]) { bandSeen_[slot] = true; bandLast_[slot] = now; continue; }
        if (now == bandLast_[slot]) { continue; }
        const std::string from = bandLast_[slot];
        bandLast_[slot] = now;
        Receiver& r = e_.rx();
        t::BandSettings cur;
        cur.volume = std::max(0.0, std::min(1.0, static_cast<double>(r.volume_)));
        cur.agc = agcWordOf(r.agcMode_);
        if (!std::isnan(bandView_[slot].top)) { cur.spectrumTopDb = bandView_[slot].top; }
        if (!std::isnan(bandView_[slot].range)) { cur.spectrumRangeDb = bandView_[slot].range; }
        if (!std::isnan(bandView_[slot].zoom)) { cur.zoomSpanHz = bandView_[slot].zoom; }
        const t::BandSwitch sw = bandMem_.onBandChange(from, now, cur);
        if (sw.saved) { bandDirty_ = true; bandDueS_ = e_.clockS() + 0.5; }
        if (sw.apply.has_value()) {
            const t::BandSettings& a = *sw.apply;
            if (a.volume) { (void)subCommand(FOXAPI_OP_SET_VOLUME, *a.volume, 0); }
            if (a.agc) { const int i = agcIndexOf(*a.agc); if (i >= 0) { (void)subCommand(FOXAPI_OP_SET_AGC, 0.0, i); } }
            json o = {{"radio", slot + 1}, {"band", now}, {"seq", ++bandApplySeq_}};
            if (a.spectrumTopDb) { o["spectrumTopDb"] = *a.spectrumTopDb; }
            if (a.spectrumRangeDb) { o["spectrumRangeDb"] = *a.spectrumRangeDb; }
            if (a.zoomSpanHz) { o["zoomSpanHz"] = *a.zoomSpanHz; }
            bandApply_ = o.dump();
            bump();
        }
    }
    if (bandDirty_ && e_.clockS() >= bandDueS_) { bandDirty_ = false; bandSave(); }
}

void WireA::bandFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("bandmem.on", bandOn_ ? "1" : "0");
    std::string names;
    for (const FieldName& fn : kFields) {
        if ((bandMem_.enabledFields() & fn.bit) != 0u) { if (!names.empty()) { names += ','; } names += fn.name; }
    }
    fact("bandmem.fields", names);
    fact("bandmem.bands", std::to_string(bandMem_.size()));
    fact("bandmem.apply", bandApply_);
    std::string last;
    for (std::size_t slot = 0; slot < bandLast_.size(); ++slot) {
        if (!bandSeen_[slot]) { continue; }
        if (!last.empty()) { last += '\n'; }
        last += std::to_string(slot + 1) + "\t" + bandLast_[slot];
    }
    fact("bandmem.last", last);
}

}  // namespace cascade::engine
