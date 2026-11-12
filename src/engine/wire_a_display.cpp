// wire_a_display.cpp - WIRING LANE wire-a, display: the station-database overlay (OVERLAY_LOAD, OVERLAY_LOOKUP; fact.overlay.*) and the waterfall gradient (GRADIENT_SET; fact.display.gradient).
// The ops are documented in core/wire_a_ops.hpp.
//
// OVERLAY (core/csv_overlay.hpp, row 7): the user's own CSV is read and parsed on the lane's job worker (the engine's pass does no disk work); the parsed overlay is handed to the engine in the job's onDone,
// on the control thread. A lookup is memory only. The CSV is the user's own file: it is not copied, not kept (only the parsed columns in memory), and nothing of it goes to a log or a message.
// GRADIENT (core/gradient_model.hpp, row 27): the engine validates and keeps the text; the colour table is the screen compositor's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/csv_overlay.hpp"
#include "core/gradient_model.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace t = cascade::core::tuning;
namespace gr = cascade::core::gradient;
using nlohmann::json;

void WireA::gradientLoadOnce() {
    if (gradientLoaded_ || dir().empty()) { return; }
    gradientLoaded_ = true;
    const std::string text = readFile("display-gradient.json");
    if (text.empty()) { return; }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("gradient") || !j["gradient"].is_string()) { gradientError_ = "the gradient file is not valid"; bump(); return; }
    const std::string g = j["gradient"].get<std::string>();
    const gr::ParseResult r = gr::parse(g);
    if (!r.ok) { gradientError_ = r.problem != gr::Problem::None ? gr::problemText(r.problem) : r.syntax; bump(); return; }
    gradientText_ = g;
    bump();
}

WireA::Result WireA::displayOp(const FoxCommand& c, const std::string& text, Result res) {
    if (c.op == FOXAPP_OP_GRADIENT_SET) {
        gradientLoadOnce();
        if (!text.empty()) {
            const gr::ParseResult r = gr::parse(text);
            if (!r.ok) { return refused(res, FOXAPI_BAD_ARGUMENT, r.problem != gr::Problem::None ? std::string(gr::problemText(r.problem)) : std::string("The gradient text is not valid: ") + r.syntax); }
            res.applied[0] = static_cast<double>(r.gradient.stops.size());
        }
        if (text == gradientText_) { return refused(res, FOXAPI_NO_CHANGE, "that gradient is in force already"); }
        gradientText_ = text;
        gradientError_.clear();
        saveFile("display-gradient.json", json{{"version", 1}, {"gradient", gradientText_}}.dump() + "\n");
        bump();
        return res;
    }
    if (c.op == FOXAPP_OP_OVERLAY_LOOKUP) {
        if (!overlay_ || overlay_->size() == 0) { return refused(res, FOXAPI_NO_CHANGE, "no station database is loaded"); }
        if (!std::isfinite(c.num[0]) || c.num[0] <= 0.0 || c.num[0] > 1e13 || !(c.num[1] >= 0.0) || c.num[1] > 1e9 || c.ival[0] < 0 || c.ival[0] > 8) {
            return refused(res, FOXAPI_OUT_OF_RANGE, "give a frequency, a tolerance of 0 to 1000000000 Hz and 0 to 8 rows");
        }
        const double tol = c.num[1] > 0.0 ? c.num[1] : 500.0;
        const std::size_t n = c.ival[0] == 0 ? 1u : static_cast<std::size_t>(c.ival[0]);
        json out = json::array();
        for (const t::StationOverlay::Fields& row : overlay_->lookupAll(c.num[0], tol, n)) {
            json r = json::array();
            for (const auto& kv : row) { r.push_back(json::array({kv.first, kv.second})); }
            out.push_back(std::move(r));
        }
        overlayLookup_ = out.dump();
        res.applied[0] = static_cast<double>(out.size());
        bump();
        return res;
    }
    // OVERLAY_LOAD
    if (text.empty()) {
        if (!overlay_) { return refused(res, FOXAPI_NO_CHANGE, "no station database is loaded"); }
        overlay_.reset();
        overlayLookup_.clear();
        bump();
        return res;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("path") || !j["path"].is_string() || !j.contains("config")) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is JSON with a path and a config"); }
    std::string why;
    const std::string path = j["path"].get<std::string>();
    if (!fullPath(path, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    t::OverlayConfig cfg;
    std::string err;
    if (!t::overlayConfigFromJson(j["config"], cfg, &err)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the overlay settings were refused: " + err); }
    if (!t::validOverlayConfig(cfg)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the overlay settings are not valid"); }
    WireA* self = this;
    return startJob(res, "station database", [path, cfg, self](JobCtl&) -> JobResult {
        JobResult r;
        std::string body, e;
        if (!readUserFile(path, 16u << 20, body, e)) { r.message = e; return r; }
        auto overlay = std::make_shared<t::StationOverlay>();
        if (!overlay->load(body, cfg, e)) { r.message = "the station database was refused: " + e; return r; }
        r.ok = true;
        r.message = "loaded";
        r.json = json{{"rows", overlay->size()}, {"skipped", overlay->skippedRows()}, {"cleaned", overlay->cleanedCells()}}.dump();
        r.onDone = [self, overlay]() { self->overlay_ = overlay; self->overlayLookup_.clear(); self->bump(); };
        return r;
    });
}

void WireA::displayFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("overlay.rows", std::to_string(overlay_ ? overlay_->size() : 0u));
    fact("overlay.skipped", std::to_string(overlay_ ? overlay_->skippedRows() : 0u));
    fact("overlay.cleaned", std::to_string(overlay_ ? overlay_->cleanedCells() : 0u));
    std::string cols;
    if (overlay_) {
        for (const std::string& n : overlay_->columnNames()) { if (!cols.empty()) { cols += '\n'; } cols += n; }
    }
    fact("overlay.columns", cols);
    fact("overlay.lookup", overlayLookup_);
    fact("display.gradient", gradientText_);
    fact("display.gradient.error", gradientError_);
}

}  // namespace cascade::engine
