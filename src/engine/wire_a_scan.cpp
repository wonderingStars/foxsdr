// wire_a_scan.cpp - WIRING LANE wire-a, the scanner: scan plans (SCAN_PLAN, SCAN_BACK, SCAN_SAVE_HIT), the scan's own trigger (SCAN_TRIGGER) and the screen C extras
// (SCANNER_EXTRAS: priority channels, lockouts, per-entry thresholds, mute while scanning). The ops are documented in core/wire_a_ops.hpp.
//
// HOW IT JOINS THE SCANNER. core/scanner.hpp is not changed: a plan EXPANDS into the scanner's list mode (ScanPlan::toParams), already in the wanted direction, and the extras are fields
// of its Params that default to off. Five bracketed lines in the engine call this file: scannerParams() (adjustScannerParams: the plan's list, the extras), scannerFrame() (scanOpen: the trigger
// in place of the squelch's answer, with the level for the per-entry thresholds; scanTune: a plan stop tunes its range's mode and bandwidth), configureScanner() (scanConfigured: the trigger's
// floors start again) and the two places another scan is started (planOff). A plan scan is the one scanner started on the selected radio, as SCANNER_RUN starts it.
//
// SCAN_BACK reconfigures the scanner with the plan's list turned so that it starts at the point BEFORE the one the scan is on, so the scan carries on from there in the plan's direction
// (the scanner's reconfigure re-emits a first tune on its next tick, which is the retune).
//
// PERSISTENCE: scan-plan.json beside markers.json: the plan, whether the trigger is on and the extras (priority, configured lockouts, thresholds, mute); written on a worker half a second after a
// change and at the exit drain. A hermetic engine keeps memory only. The operator's lockouts (extras 1) are the scanner's own and are not saved.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/bookmark.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace t = cascade::core::tuning;

namespace {
constexpr std::size_t kMaxPriority = 64;
constexpr std::size_t kMaxLocked = 1000;
constexpr std::size_t kMaxThresholds = 1000;

bool parseHzList(const std::string& text, std::size_t maxN, std::vector<double>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find(';', pos);
        if (end == std::string::npos) { end = text.size(); }
        std::string item = text.substr(pos, end - pos);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\n' || item.front() == '\r')) { item.erase(item.begin()); }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\n' || item.back() == '\r')) { item.pop_back(); }
        if (!item.empty()) {
            char* e = nullptr;
            const double v = std::strtod(item.c_str(), &e);
            if (*e != '\0' || !std::isfinite(v) || v <= 0.0 || v > 1e13) { return false; }
            if (out.size() >= maxN) { return false; }
            out.push_back(v);
        }
        pos = end + 1;
    }
    return true;
}

std::string hzListText(const std::vector<double>& v) {
    std::string s;
    char buf[40];
    for (const double hz : v) {
        std::snprintf(buf, sizeof buf, "%.3f", hz);
        if (!s.empty()) { s += ';'; }
        s += buf;
    }
    return s;
}
}  // namespace

// ---- the engine's hooks -------------------------------------------------------------------------------------------------------------------------

void WireA::adjustScannerParams(cascade::core::ScannerParams& p) {
    scanLoadOnce();
    if (planOn_) {
        if (const auto q = plan_.toParams(p)) {
            p = *q;
            if (rotateToHz_ > 0.0) {
                const auto it = std::find_if(p.list.begin(), p.list.end(), [this](double hz) { return std::fabs(hz - rotateToHz_) < 0.5; });
                if (it != p.list.end()) { std::rotate(p.list.begin(), it, p.list.end()); }
                rotateToHz_ = 0.0;
            }
        }
    }
    p.priorityHz = priorityHz_;
    p.priorityIntervalMs = priorityIntervalMs_;
    p.lockedHz = lockedHz_;
    p.muteWhileScanning = muteWhileScanning_;
    p.thresholdDb.clear();
    if (!p.list.empty() && (!thresholds_.empty() || !fields_.empty())) {
        bool any = false;
        std::vector<double> th(p.list.size(), std::numeric_limits<double>::quiet_NaN());
        for (std::size_t i = 0; i < p.list.size(); ++i) {
            for (const auto& [hz, db] : thresholds_) {
                if (std::fabs(hz - p.list[i]) <= 1.0) { th[i] = db; any = true; break; }
            }
            if (std::isnan(th[i]) && !fields_.empty()) {
                // a threshold a bookmark carries (the first bookmark at that frequency that has one)
                for (const cascade::core::Bookmark& b : e_.freqMgr_.list()) {
                    if (std::fabs(b.freqHz - p.list[i]) > 1.0) { continue; }
                    const BmFields* f = fieldsFor(b.freqHz, b.mode);
                    if (f != nullptr && !std::isnan(f->thresholdDb)) { th[i] = f->thresholdDb; any = true; break; }
                }
            }
        }
        if (any) { p.thresholdDb = std::move(th); }
    }
}

bool WireA::scanOpen(bool squelchOpen, long long slot, double levelDb) {
    if (!triggerOn_ || slot < 0) { return squelchOpen; }
    return trigger_.update(static_cast<std::size_t>(slot), levelDb);
}

void WireA::scanTune(double hz) {
    if (planOn_) {
        if (const t::ScanRange* r = plan_.rangeOf(hz)) {
            tuneTo(hz, r->mode, r->bandwidthHz);
            return;
        }
    }
    e_.tuneAbsoluteHz(hz);
}

// ---- persistence --------------------------------------------------------------------------------------------------------------------------------

void WireA::scanLoadOnce() {
    if (scanLoaded_ || dir().empty()) { return; }
    scanLoaded_ = true;
    const std::string text = readFile("scan-plan.json");
    if (text.empty()) { return; }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    auto bad = [this](const std::string& why) { scanError_ = why; bump(); };
    if (j.is_discarded() || !j.is_object() || !j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return bad("the scan file is not valid"); }
    t::ScanPlan plan;
    if (j.contains("plan") && j["plan"].is_object() && !j["plan"].empty()) {
        std::string err;
        if (!plan.fromJson(j["plan"], &err)) { return bad("the scan plan in the scan file was refused: " + err); }
    }
    plan_ = plan;
    trigger_.setConfig(plan_.trigger());
    triggerOn_ = j.value("triggerOn", false);
    if (j.contains("extras") && j["extras"].is_object()) {
        const nlohmann::json& x = j["extras"];
        std::vector<double> pr, lk;
        if (x.contains("priorityHz") && x["priorityHz"].is_array()) {
            for (const auto& v : x["priorityHz"]) { if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > 0.0 && pr.size() < kMaxPriority) { pr.push_back(v.get<double>()); } }
        }
        if (x.contains("lockedHz") && x["lockedHz"].is_array()) {
            for (const auto& v : x["lockedHz"]) { if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > 0.0 && lk.size() < kMaxLocked) { lk.push_back(v.get<double>()); } }
        }
        priorityHz_ = pr;
        lockedHz_ = lk;
        priorityIntervalMs_ = x.contains("priorityIntervalMs") && x["priorityIntervalMs"].is_number() ? std::max(0.0, std::min(3.6e6, x["priorityIntervalMs"].get<double>())) : 0.0;
        muteWhileScanning_ = x.value("mute", false);
        thresholds_.clear();
        if (x.contains("thresholds") && x["thresholds"].is_array()) {
            for (const auto& v : x["thresholds"]) {
                if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number() && thresholds_.size() < kMaxThresholds) { thresholds_.emplace_back(v[0].get<double>(), v[1].get<double>()); }
            }
        }
    }
    bump();
}

void WireA::scanSave() {
    if (!scanLoaded_ && dir().empty()) { return; }
    nlohmann::json th = nlohmann::json::array();
    for (const auto& [hz, db] : thresholds_) { th.push_back({hz, db}); }
    const nlohmann::json j = {{"version", 1},
                              {"plan", plan_.ranges().empty() ? nlohmann::json::object() : plan_.toJson()},
                              {"triggerOn", triggerOn_},
                              {"extras", {{"priorityHz", priorityHz_}, {"priorityIntervalMs", priorityIntervalMs_}, {"lockedHz", lockedHz_}, {"mute", muteWhileScanning_}, {"thresholds", th}}}};
    saveFile("scan-plan.json", j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

void WireA::scanPass() {
    scanLoadOnce();
    // The plan scan ended (stopped by the scanner's own ops or by the user's hand): the cursor's place is of no use.
    if (!e_.scanner_.active()) { cursor_ = t::ScanCursor(); }
    if (scanDirty_ && e_.clockS() >= scanDueS_) {
        scanDirty_ = false;
        scanSave();
    }
}

// ---- the ops ------------------------------------------------------------------------------------------------------------------------------------

WireA::Result WireA::scanOp(const FoxCommand& c, const std::string& text, Result res) {
    scanLoadOnce();
    switch (c.op) {
        case FOXAPP_OP_SCAN_PLAN:
            return scanPlanOp(c, text, res);
        case FOXAPP_OP_SCANNER_EXTRAS:
            return scanExtrasOp(c, text, res);
        case FOXAPP_OP_SCAN_TRIGGER: {
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 off or 1 on"); }
            if (!text.empty()) {
                t::ScanTrigger probe;
                std::string err;
                const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
                if (j.is_discarded() || !probe.configFromJson(j, &err)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the trigger was refused: " + (j.is_discarded() ? std::string("not JSON") : err)); }
                plan_.trigger() = probe.config();
                (void)trigger_.setConfig(probe.config());
            }
            const bool on = c.ival[0] == 1;
            if (on == triggerOn_ && text.empty()) { return refused(res, FOXAPI_NO_CHANGE, on ? "the scan trigger is on already" : "the scan trigger is off already"); }
            triggerOn_ = on;
            trigger_.reset();
            scanDirty_ = true;
            scanDueS_ = e_.clockS() + 0.5;
            bump();
            return res;
        }
        case FOXAPP_OP_SCAN_BACK: {
            if (!planOn_ || !e_.scanner_.active()) { return refused(res, FOXAPI_NO_CHANGE, "no plan scan is running"); }
            std::string err;
            const auto pts = plan_.expand(&err);
            if (!pts || pts->hz.empty()) { return refused(res, FOXAPI_FAILED, "the plan has no points"); }
            const double here = e_.scanner_.currentHz();
            cursor_.load(*pts, here);
            const double prev = cursor_.back();
            rotateToHz_ = prev;
            e_.configureScanner();
            e_.scanner_.start(e_.clockS() * 1000.0);
            res.applied[0] = prev;
            bump();
            return res;
        }
        case FOXAPP_OP_SCAN_SAVE_HIT: {
            if (plan_.ranges().empty()) { return refused(res, FOXAPI_NO_CHANGE, "there is no scan plan to take the mode from"); }
            const double hz = c.num[0] > 0.0 ? c.num[0] : e_.currentAbsoluteHz();
            const auto b = t::bookmarkFromHit(hz, plan_, e_.freqMgr_.list());
            if (!b.has_value()) { return refused(res, FOXAPI_NO_CHANGE, "that frequency is a bookmark already, or is not one that can be saved"); }
            e_.freqMgr_.add(*b);
            res.applied[0] = hz;
            return res;
        }
        default:
            return refused(res, FOXAPI_UNSUPPORTED, "not a scanner operation");
    }
}

WireA::Result WireA::scanPlanOp(const FoxCommand& c, const std::string& text, Result res) {
    switch (c.ival[0]) {
        case 0: {
            const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
            t::ScanPlan next = plan_;
            std::string err;
            if (j.is_discarded() || !next.fromJson(j, &err)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the plan was refused: " + (j.is_discarded() ? std::string("not JSON") : err)); }
            if (!next.ranges().empty() && !next.expand(&err)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the plan was refused: " + err); }
            plan_ = next;
            (void)trigger_.setConfig(plan_.trigger());
            trigger_.reset();
            if (planOn_ && e_.scanner_.active()) { e_.configureScanner(); }
            scanDirty_ = true;
            scanDueS_ = e_.clockS() + 0.5;
            bump();
            return res;
        }
        case 1: {
            if (plan_.ranges().empty()) { return refused(res, FOXAPI_NO_CHANGE, "there is no scan plan"); }
            if (e_.rx().tuneLocked_) { return refused(res, FOXAPI_DENIED, "this radio's frequency is locked"); }
            std::string err;
            if (!plan_.expand(&err)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the plan cannot be scanned: " + err); }
            e_.scanTicked_ = false;
            if (e_.airbandListening_ || e_.airbandStartPending_) { e_.airbandStop(std::string()); }
            if (e_.scanner_.active() && e_.scannerSlot_ != e_.scopedSlot()) { e_.scanner_.stop(); }
            e_.scannerSlot_ = e_.scopedSlot();
            planOn_ = true;
            rotateToHz_ = 0.0;
            e_.configureScanner();
            e_.scanner_.start(e_.clockS() * 1000.0);
            bump();
            return res;
        }
        case 2: {
            if (plan_.ranges().empty() && !planOn_) { return refused(res, FOXAPI_NO_CHANGE, "there is no scan plan"); }
            if (planOn_ && e_.scanner_.active()) { e_.scanner_.stop(); }
            planOn_ = false;
            plan_.clear();
            scanDirty_ = true;
            scanDueS_ = e_.clockS() + 0.5;
            bump();
            return res;
        }
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 set, 1 start or 2 clear");
    }
}

WireA::Result WireA::scanExtrasOp(const FoxCommand& c, const std::string& text, Result res) {
    bool reconfigure = true;
    switch (c.ival[0]) {
        case 0: {
            std::vector<double> v;
            if (!parseHzList(text, kMaxPriority, v)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the priority list is frequencies in Hz separated by semicolons, at most 64"); }
            if (!(c.num[0] >= 0.0) || c.num[0] > 3.6e6) { return refused(res, FOXAPI_OUT_OF_RANGE, "the interval is 0 to 3600000 ms"); }
            priorityHz_ = v;
            priorityIntervalMs_ = c.num[0];
            break;
        }
        case 1: {
            if (!(c.num[1] >= 0.0) || c.num[1] > 8.64e7) { return refused(res, FOXAPI_OUT_OF_RANGE, "the time is 0 to 86400000 ms (0 = until cleared)"); }
            if (c.num[0] > 0.0) {
                if (c.num[0] > 1e13) { return refused(res, FOXAPI_OUT_OF_RANGE, "that is not a frequency"); }
                e_.scanner_.lockOut(c.num[0], c.num[1]);
            } else {
                if (!e_.scanner_.active()) { return refused(res, FOXAPI_NO_CHANGE, "the scanner is not running"); }
                e_.scanner_.lockOutCurrent(c.num[1]);
                e_.scannerHasExpected_ = false;
            }
            reconfigure = false;
            break;
        }
        case 2:
            e_.scanner_.clearLockouts();
            reconfigure = false;
            break;
        case 3:
            muteWhileScanning_ = c.ival[1] != 0;
            break;
        case 4: {
            std::vector<std::pair<double, double>> v;
            std::size_t pos = 0;
            while (pos < text.size()) {
                std::size_t end = text.find('\n', pos);
                if (end == std::string::npos) { end = text.size(); }
                std::string line = text.substr(pos, end - pos);
                pos = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                if (line.empty()) { continue; }
                const std::size_t tab = line.find('\t');
                if (tab == std::string::npos || v.size() >= kMaxThresholds) { return refused(res, FOXAPI_BAD_ARGUMENT, "thresholds are lines of Hz, a tab and dB, at most 1000"); }
                char *e1 = nullptr, *e2 = nullptr;
                const double hz = std::strtod(line.c_str(), &e1);
                const double db = std::strtod(line.c_str() + tab + 1, &e2);
                if (e1 != line.c_str() + tab || *e2 != '\0' || !std::isfinite(hz) || hz <= 0.0 || hz > 1e13 || !std::isfinite(db) || db < -200.0 || db > 50.0) {
                    return refused(res, FOXAPI_BAD_ARGUMENT, "a threshold line is not a frequency, a tab and a level between -200 and 50 dB");
                }
                v.emplace_back(hz, db);
            }
            thresholds_ = std::move(v);
            break;
        }
        case 5: {
            std::vector<double> v;
            if (!parseHzList(text, kMaxLocked, v)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the locked list is frequencies in Hz separated by semicolons, at most 1000"); }
            lockedHz_ = v;
            break;
        }
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 to 5");
    }
    if (reconfigure) {
        if (e_.scanner_.active()) { e_.configureScanner(); }
        scanDirty_ = true;
        scanDueS_ = e_.clockS() + 0.5;
    }
    bump();
    return res;
}

void WireA::scanFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("scanner.extras", "1");
    fact("scan.plan", plan_.ranges().empty() ? std::string() : plan_.toJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    fact("scan.plan.active", planOn_ && e_.scanner_.active() ? "1" : "0");
    fact("scan.trigger", trigger_.configToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    fact("scan.trigger.on", triggerOn_ ? "1" : "0");
    fact("scan.priority", hzListText(priorityHz_));
    fact("scan.priority.interval", std::to_string(static_cast<long long>(priorityIntervalMs_)));
    fact("scan.locked", hzListText(lockedHz_));
    fact("scan.thresholds", std::to_string(thresholds_.size()));
    fact("scan.mute", muteWhileScanning_ ? "1" : "0");
    fact("scan.error", scanError_);
}

}  // namespace cascade::engine
