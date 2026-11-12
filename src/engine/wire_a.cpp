// wire_a.cpp - WIRING LANE wire-a: the dispatcher, the persistence helpers and the facts (see wire_a.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_a.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "core/app_commands.hpp"
#include "core/file_read.hpp"
#include "core/freq_markers.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

WireA& WireA::of(Engine& e) {
    if (!e.wireA_) { e.wireA_ = std::make_shared<WireA>(e); }
    return *static_cast<WireA*>(e.wireA_.get());
}

WireA::Result WireA::refused(Result r, std::int32_t status, const std::string& why) {
    r.status = status;
    r.flags |= FOXAPI_RESULT_REFUSED;
    std::snprintf(r.message, sizeof(r.message), "%s", why.c_str());
    return r;
}

FoxCommandResult WireA::apply(const FoxCommand& c, const std::string& text, FoxCommandResult res) {
    (void)text;
    switch (c.op) {
        case FOXAPP_OP_HISTORY_STEP:
            return historyStep(c, res);
        case FOXAPP_OP_CHANNEL:
            return channelOp(c, text, res);
        default:
            return refused(res, FOXAPI_UNSUPPORTED, "this operation is not in this build");
    }
}

void WireA::pass() {
    historyPass();
    drainSaves();
}

void WireA::facts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    historyFacts(fact);
    channelFacts(fact);
    tunedFacts(fact);
}

void WireA::tunedFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    for (std::size_t slot = 0; slot < history_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        Receiver& r = *e_.receivers_[slot];
        const std::string k = "tuned." + std::to_string(slot + 1) + ".";
        const double hz = r.pipeline_.activeSource().centerFrequencyHz() + r.pipeline_.vfoOffsetHz();
        fact(k + "hz", std::to_string(static_cast<long long>(std::llround(hz))));
        fact(k + "mode", r.modeIndex_ >= 0 && r.modeIndex_ < 8 ? cascade::gui::kModeNames[r.modeIndex_] : "");
        fact(k + "bandwidthHz", std::to_string(static_cast<long long>(std::llround(r.vfoBandwidthHz_))));
    }
}

// ---- shared helpers -------------------------------------------------------------------------------------------------------------------------

void WireA::tuneTo(double hz, const std::string& mode, double bandwidthHz) {
    for (int m = 0; m < 8; ++m) {
        if (mode == cascade::gui::kModeNames[m]) {
            if (m != e_.rx().modeIndex_) { e_.setModeIndex(m); }
            break;
        }
    }
    e_.tuneAbsoluteHz(hz);
    if (bandwidthHz > 0.0) {
        // The same clamp as SET_BANDWIDTH: no narrower than the engine's floor, no wider than the channel can carry.
        const double hi = cascade::gui::kVfoBwMaxChanFrac * e_.rx().pipeline_.channelRateHz();
        e_.rx().vfoBandwidthHz_ = std::max(cascade::gui::kVfoBwMinHz, std::min(bandwidthHz, hi));
        e_.applyChannelBandwidth();
        e_.rx().bandwidthIndex_ = cascade::gui::bandwidthStepIndex(e_.rx().vfoBandwidthHz_);
    }
}

// ---- persistence ----------------------------------------------------------------------------------------------------------------------------

std::string WireA::dir() const {
    if (e_.markerPath_.empty()) { return std::string(); }   // a hermetic engine: never touch the disk
    return std::filesystem::path(e_.markerPath_).parent_path().string();
}

void WireA::saveFile(const std::string& name, const std::string& text) {
    const std::string d = dir();
    if (d.empty()) { return; }
    auto& s = savers_[name];
    if (!s) { s = std::make_unique<cascade::gui::BackgroundSaver>("wire-a", &cascade::core::FreqMarkers::writeFile); }
    s->request((std::filesystem::path(d) / name).string(), text);
}

std::string WireA::readFile(const std::string& name) const {
    const std::string d = dir();
    if (d.empty()) { return std::string(); }
    std::string text;
    if (cascade::core::readTextFile(std::filesystem::path(d) / name, text) != cascade::core::ReadResult::Ok) { return std::string(); }
    return text;
}

void WireA::drainSaves() {
    bool ok = true;
    std::string err;
    for (auto& kv : savers_) { (void)kv.second->poll(ok, err); }
}

}  // namespace cascade::engine
