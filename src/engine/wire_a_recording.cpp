// wire_a_recording.cpp - WIRING LANE wire-a, recording: daily folders (SET_RECORD_DAILY, fact.record.daily).
//
// DAILY FOLDERS (core/rec_daily.hpp, row 19): a take goes into <recordings folder>/<the day it starts>/... . The date is taken when the file is asked for (a take that crosses midnight stays where it began) in
// UTC (the default) or local time. The hook is the one place every take's place is decided (Engine::applyRecordName, engine_recname.cpp), called before the file-name template so that the template's own folders
// come below the day's. The folder is made by the model's containment-proving creator (no link, junction or file in the way, never outside the recordings folder); if it cannot be made the take keeps its
// default place and the log says so (with the reason, never the path). The default style is None: nothing changes until the user asks. The setting is kept in record-daily.json beside markers.json.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/rec_daily.hpp"
#include "core/status_clock.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace rd = cascade::core::recdaily;

namespace {
bool parseDaily(const std::string& text, rd::Options& out, std::string& err) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "the text is not a JSON object"; return false; }
    rd::Options o;
    o.style = rd::Style::None;   // nothing changes until the user asks
    if (j.contains("style")) {
        if (!j["style"].is_number_integer()) { err = "the style is a whole number from 0 to 4"; return false; }
        const long long s = j["style"].get<long long>();
        if (s < 0 || s > 4) { err = "the style is a whole number from 0 to 4"; return false; }
        o.style = static_cast<rd::Style>(s);
    }
    if (j.contains("utc")) {
        if (!j["utc"].is_boolean()) { err = "utc is true or false"; return false; }
        o.clock = j["utc"].get<bool>() ? rd::Clock::Utc : rd::Clock::Local;
    }
    if (j.contains("template")) {
        if (!j["template"].is_string()) { err = "the template is text"; return false; }
        o.customTemplate = j["template"].get<std::string>();
    }
    // The model's own check of the result: a sample instant, the same rules as a take's.
    const long long sample = 1'790'000'000'000LL;
    const rd::Expanded e = rd::expandFolder(o, sample);
    if (!e.ok) { err = e.error; return false; }
    out = o;
    return true;
}
}  // namespace

std::string WireA::dailyJson() const {
    return nlohmann::json{{"style", static_cast<int>(daily_.style)}, {"utc", daily_.clock == rd::Clock::Utc}, {"template", daily_.customTemplate}}.dump();
}

void WireA::dailyLoadOnce() {
    if (dailyLoaded_ || dir().empty()) { return; }
    dailyLoaded_ = true;
    const std::string text = readFile("record-daily.json");
    if (text.empty()) { return; }
    rd::Options o;
    std::string err;
    if (parseDaily(text, o, err)) {
        daily_ = o;
    } else {
        cascade::core::diagWarnf("record: record-daily.json was refused (%s); takes keep their default place", err.c_str());
    }
}

WireA::Result WireA::dailyOp(const FoxCommand& c, const std::string& text, Result res) {
    (void)c;
    dailyLoadOnce();
    rd::Options o;
    std::string err;
    if (!parseDaily(text, o, err)) { return refused(res, FOXAPI_BAD_ARGUMENT, err); }
    const rd::Options before = daily_;
    daily_ = o;
    const std::string now = dailyJson();
    daily_ = before;
    if (now == dailyJson()) { return refused(res, FOXAPI_NO_CHANGE, "that is in force already"); }
    daily_ = o;
    saveFile("record-daily.json", now + "\n");
    bump();
    return res;
}

void WireA::applyDaily(cascade::core::Recorder::OpenRequest& req) {
    dailyLoadOnce();
    if (daily_.style == rd::Style::None && daily_.customTemplate.empty()) { return; }
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rd::Options o = daily_;
    if (o.clock == rd::Clock::Local) { o.localOffsetMinutes = cascade::core::statusclock::systemUtcOffsetMinutes(nowMs); }
    const rd::Expanded e = rd::expandFolder(o, nowMs);
    if (!e.ok) {
        cascade::core::diagWarnf("record: the day's folder could not be named (%s); the take keeps its default place", e.error.c_str());
        return;
    }
    if (e.rel.empty()) { return; }
    std::error_code ec;
    const std::filesystem::path root(req.directory.empty() ? std::string(".") : req.directory);
    std::filesystem::create_directories(root, ec);   // (the recorder makes it anyway: it must exist for the containment proof)
    const rd::Ensured made = rd::ensureFolder(root, e.rel);
    if (!made.ok) {
        cascade::core::diagWarnf("record: the day's folder could not be made (%s); the take keeps its default place", made.error.c_str());
        return;
    }
    if (!req.path.empty()) { req.path = (made.dir / std::filesystem::path(req.path).filename()).string(); }
    req.directory = made.dir.string();
}

void WireA::recordingFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("record.daily", dailyJson());
}

}  // namespace cascade::engine
