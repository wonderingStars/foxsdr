// wire_a_airband.cpp - WIRING LANE wire-a, second pass: the airband monitor's per-channel settings (AIRBAND_CHANNEL; see the operation's comment in core/wire_a_ops.hpp and core/airband_gate.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace ab = cascade::core::airband;
using nlohmann::json;

cascade::core::MonitorGateFactory WireA::airbandGates() {
    airbandChannelLoadOnce();
    return airbandTable_.factory(e_.recordDir_);
}

// Once a pass: the receiver's runner lets go of the sets it has retired. A channel's file output closes when its set is destroyed, and a set the monitor replaced (a rebuild, a stop) waited for the next publish to be.
void WireA::airbandReap() {
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (e_.receivers_[slot]) { e_.receivers_[slot]->pipeline_.patchRunner().reap(); }
    }
}

void WireA::airbandChannelLoadOnce() {
    if (airbandTableLoaded_ || dir().empty()) { return; }
    airbandTableLoaded_ = true;
    const json j = json::parse(readFile("airband-channels.json"), nullptr, false);
    if (j.is_discarded() || !j.is_array()) { return; }
    for (const json& t : j) {
        if (!t.is_string()) { continue; }
        const ab::ParseResult p = ab::parse(t.get<std::string>());
        if (p.ok) { (void)airbandTable_.set(p.config); }   // a line that no longer passes is left out, not repaired
    }
    bump();
}

void WireA::airbandChannelSave() {
    json a = json::array();
    for (const auto& kv : airbandTable_.all()) { a.push_back(ab::serialise(kv.second)); }
    saveFile("airband-channels.json", a.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

WireA::Result WireA::airbandChannelOp(const FoxCommand& c, const std::string& text, Result res) {
    airbandChannelLoadOnce();
    switch (c.ival[0]) {
        case 0: {
            const ab::ParseResult p = ab::parse(text);
            if (!p.ok && p.problem == ab::Problem::None) { return refused(res, FOXAPI_BAD_ARGUMENT, std::string("the text is not FOXAIRCH1|...: ") + p.why); }
            if (!p.ok) { return refused(res, FOXAPI_OUT_OF_RANGE, ab::problemText(p.problem)); }
            if (p.config.output == ab::OutputKind::WavFile && e_.recordDir_.empty()) { return refused(res, FOXAPI_OUT_OF_RANGE, "a file output needs a recordings folder, and none is set"); }
            const ab::AirbandChannelConfig* have = airbandTable_.find(static_cast<double>(p.config.freqHz));
            if (have != nullptr && *have == p.config) { return refused(res, FOXAPI_NO_CHANGE, "those are the settings already"); }
            (void)airbandTable_.set(p.config);
            res.applied[0] = static_cast<double>(p.config.freqHz);
            break;
        }
        case 1: {
            if (!std::isfinite(c.num[0]) || c.num[0] < 0.0) { return refused(res, FOXAPI_OUT_OF_RANGE, "num[0] is the channel's frequency in hertz"); }
            if (!airbandTable_.clear(static_cast<std::int64_t>(std::llround(c.num[0])))) { return refused(res, FOXAPI_NO_CHANGE, "that channel has no settings of its own"); }
            break;
        }
        case 2:
            if (airbandTable_.size() == 0) { return refused(res, FOXAPI_NO_CHANGE, "no channel has settings of its own"); }
            airbandTable_.clearAll();
            break;
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 set, 1 clear one or 2 clear all");
    }
    airbandChannelSave();
    // listening now: the block on the air is built again so the change is heard (its squelches start again)
    if (e_.airbandListening_ && e_.airbandTuned_) { e_.airbandTuneBlock(e_.airbandBlock_); }
    bump();
    return res;
}

void WireA::airbandChannelFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("airband.channels", std::to_string(airbandTable_.size()));
    for (const auto& kv : airbandTable_.all()) { fact("airband.channel." + std::to_string(kv.first), ab::serialise(kv.second)); }
}

}  // namespace cascade::engine
