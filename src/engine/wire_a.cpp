// wire_a.cpp - WIRING LANE wire-a: the dispatcher, the persistence helpers and the facts (see wire_a.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_a.hpp"

#include <cstdio>
#include <filesystem>

#include "core/app_commands.hpp"
#include "core/file_read.hpp"
#include "core/freq_markers.hpp"
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
