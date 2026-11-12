// wire_c_sys.cpp - see wire_c_sys.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_sys.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

#include "core/safe_file_io.hpp"

namespace cascade::engine::wirec {

namespace fs = std::filesystem;
namespace sys = cascade::core::sys;
using nlohmann::json;

std::string utcStamp(std::int64_t unixSeconds) {
    std::time_t t = unixSeconds != 0 ? static_cast<std::time_t>(unixSeconds) : std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

namespace {

std::string slurp(const fs::path& p, std::size_t cap = 32u * 1024u * 1024u) {
    std::string out;
    (void)cascade::core::safeio::readCapped(p, cap, out);
    return out;
}

bool writeFile(const fs::path& p, const std::string& bytes) { return cascade::core::safeio::writeAtomic(p, bytes); }

}  // namespace

std::vector<sys::BundleFile> SysOps::readOwnFiles() const {
    std::vector<sys::BundleFile> out;
    for (const char* name : {"config.json", "bookmarks.json", "markers.json"}) {
        std::error_code ec;
        const fs::path p = settings_ / name;
        if (!fs::is_regular_file(p, ec)) { continue; }
        sys::BundleFile f;
        f.name = name;
        f.bytes = slurp(p);
        if (!f.bytes.empty()) { out.push_back(std::move(f)); }
    }
    return out;
}

bool SysOps::exportTo(const std::string& path, std::string& why, std::size_t* stripped) {
    if (path.empty() || path.size() > 520 || path.find('\0') != std::string::npos) {
        why = "no file named";
        return false;
    }
    const std::vector<sys::BundleFile> files = readOwnFiles();
    if (files.empty()) {
        why = "there is nothing to export yet";
        return false;
    }
    sys::ExportOptions opt;
    opt.stripIdentity = true;
    opt.createdUtc = utcStamp();
    sys::ExportReport report;
    const std::string text = sys::exportBundle(files, opt, report);
    if (report.error != sys::BundleError::None) {
        why = std::string("the bundle could not be made: ") + sys::bundleErrorName(report.error);
        return false;
    }
    if (stripped != nullptr) { *stripped = report.stripped.size(); }
    if (!writeFile(fs::path(path), text)) {
        why = "the file could not be written";
        return false;
    }
    return true;
}

bool SysOps::importPlan(const std::string& path, std::string& planText, std::string& token, std::string& notes, std::string& why) {
    haveImport_ = false;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(fs::path(path), ec)) {
        why = "no such file";
        return false;
    }
    std::string text;
    if (cascade::core::safeio::readCapped(fs::path(path), sys::kBundleMaxTextBytes, text) != cascade::core::safeio::ReadResult::Ok) {
        why = "the file could not be read, or is too large";
        return false;
    }
    sys::ImportResult r = sys::parseBundle(text);
    if (r.error != sys::BundleError::None) {
        why = std::string("that is not a FoxSDR settings bundle: ") + sys::bundleErrorName(r.error) + (r.detail.empty() ? "" : " (" + r.detail + ")");
        return false;
    }
    const sys::ImportPlan plan = sys::planImport(r, settings_);
    planText.clear();
    for (const sys::ImportPlanItem& it : plan.items) {
        planText += it.name + "\t" + (it.action == sys::ImportAction::New ? "New" : it.action == sys::ImportAction::Replace ? "Replace" : "Identical") + "\n";
    }
    notes.clear();
    for (const std::string& n : r.notes) { notes += n + "\n"; }
    token = plan.confirmToken;
    imported_ = std::move(r);
    importToken_ = token;
    haveImport_ = true;
    return true;
}

bool SysOps::importCommit(const std::string& token, std::string& why) {
    if (!haveImport_ || token.empty() || token != importToken_) {
        why = "that confirmation is not the one the plan gave; plan the import again";
        return false;
    }
    // the plan is re-made against the folder as it is NOW: a settings folder that changed since the plan makes the token stale
    if (sys::planImport(imported_, settings_).confirmToken != importToken_) {
        haveImport_ = false;
        why = "the settings changed since the plan was made; plan the import again";
        return false;
    }
    std::error_code ec;
    fs::remove_all(pendingDir(), ec);
    fs::create_directories(pendingDir(), ec);
    json names = json::array();
    for (const sys::BundleFile& f : imported_.files) {
        if (!writeFile(pendingDir() / f.name, f.bytes)) {
            fs::remove_all(pendingDir(), ec);
            why = "the import could not be staged";
            return false;
        }
        names.push_back(f.name);
    }
    json j;
    j["action"] = "import";
    j["stamp"] = utcStamp();
    j["files"] = names;
    if (!writeFile(pendingFile(), j.dump(1) + "\n")) {
        fs::remove_all(pendingDir(), ec);
        why = "the import could not be staged";
        return false;
    }
    haveImport_ = false;
    return true;
}

bool SysOps::backupNow(std::size_t keep, std::string& name, std::string& why) {
    const std::vector<sys::BundleFile> files = readOwnFiles();
    if (files.empty()) {
        why = "there is nothing to back up yet";
        return false;
    }
    sys::ExportOptions opt;
    opt.stripIdentity = true;
    opt.createdUtc = utcStamp();
    sys::ExportReport report;
    const std::string text = sys::exportBundle(files, opt, report);
    if (report.error != sys::BundleError::None) {
        why = std::string("the backup could not be made: ") + sys::bundleErrorName(report.error);
        return false;
    }
    const fs::path dir = settings_ / "backups";
    name = sys::backupFileName(utcStamp());
    if (!writeFile(dir / name, text)) {
        why = "the backup could not be written";
        return false;
    }
    sys::pruneBackups(dir, keep < 1 ? 1 : keep);
    return true;
}

bool SysOps::resetPlan(bool includePlugins, bool includeLogs, std::string& planText, std::string& token, std::string& why) {
    haveReset_ = false;
    resetOptions_.includePlugins = includePlugins;
    resetOptions_.includeLogs = includeLogs;
    const sys::ResetPlan plan = sys::planReset(settings_, data_, resetOptions_);
    if (!plan.error.empty()) {
        why = plan.error;
        return false;
    }
    planText.clear();
    for (const sys::ResetItem& it : plan.items) { planText += it.relative + (it.isFolder ? "/" : "") + "\n"; }
    planText += "(" + std::to_string(plan.leftAlone) + " other file(s) in the folder are left alone)\n";
    token = plan.confirmToken;
    resetToken_ = token;
    haveReset_ = true;
    return true;
}

bool SysOps::resetStage(const std::string& token, std::string& why) {
    if (!haveReset_ || token.empty() || token != resetToken_) {
        why = "that confirmation is not the one the plan gave; plan the reset again";
        return false;
    }
    if (sys::planReset(settings_, data_, resetOptions_).confirmToken != resetToken_) {
        haveReset_ = false;
        why = "the settings changed since the plan was made; plan the reset again";
        return false;
    }
    json j;
    j["action"] = "reset";
    j["stamp"] = utcStamp();
    j["plugins"] = resetOptions_.includePlugins;
    j["logs"] = resetOptions_.includeLogs;
    std::error_code ec;
    fs::remove_all(pendingDir(), ec);
    if (!writeFile(pendingFile(), j.dump(1) + "\n")) {
        why = "the reset could not be staged";
        return false;
    }
    haveReset_ = false;
    return true;
}

std::string SysOps::pendingKind() const {
    std::string text;
    if (cascade::core::safeio::readCapped(pendingFile(), 64u * 1024u, text) != cascade::core::safeio::ReadResult::Ok) { return std::string(); }
    const json j = json::parse(text, nullptr, false);
    return j.is_object() && j.contains("action") && j["action"].is_string() ? j["action"].get<std::string>() : std::string();
}

bool SysOps::cancelPending(std::string& why) {
    std::error_code ec;
    if (pendingKind().empty() && !fs::exists(pendingDir(), ec)) {
        why = "nothing is waiting for the next start";
        return false;
    }
    fs::remove(pendingFile(), ec);
    fs::remove_all(pendingDir(), ec);
    return true;
}

std::string applyPendingAtBoot(const fs::path& settingsDir, const fs::path& dataDir, bool& ok) {
    ok = true;
    const fs::path pf = settingsDir / "wirec-pending.json";
    std::string text;
    if (cascade::core::safeio::readCapped(pf, 64u * 1024u, text) != cascade::core::safeio::ReadResult::Ok) { return std::string(); }
    const json j = json::parse(text, nullptr, false);
    std::error_code ec;
    if (!j.is_object() || !j.contains("action") || !j["action"].is_string()) {
        fs::remove(pf, ec);   // an unreadable marker is dropped, never acted on
        return "a damaged pending-action file was dropped";
    }
    const std::string action = j["action"].get<std::string>();
    const std::string stamp = j.contains("stamp") && j["stamp"].is_string() ? j["stamp"].get<std::string>() : utcStamp();
    if (action == "import") {
        std::size_t moved = 0;
        const fs::path dir = settingsDir / "wirec-pending";
        if (j.contains("files") && j["files"].is_array()) {
            for (const json& n : j["files"]) {
                if (!n.is_string() || !sys::allowedBundleFileName(n.get<std::string>())) { continue; }   // only the three names a bundle may carry
                const std::string name = n.get<std::string>();
                const fs::path from = dir / name;
                const fs::path to = settingsDir / name;
                if (!fs::is_regular_file(from, ec)) { continue; }
                if (fs::exists(to, ec)) {
                    fs::path keep = to;
                    keep += ".before-import-" + stamp;
                    fs::remove(keep, ec);
                    fs::rename(to, keep, ec);
                    if (ec) { ok = false; return "the settings import could not keep " + name + " aside; it stays staged"; }
                }
                fs::rename(from, to, ec);
                if (ec) { ok = false; return "the settings import could not put " + name + " in place; it stays staged"; }
                ++moved;
            }
        }
        fs::remove_all(dir, ec);
        fs::remove(pf, ec);
        return "settings imported: " + std::to_string(moved) + " file(s); the earlier ones are kept as .before-import-" + stamp;
    }
    if (action == "reset") {
        sys::ResetOptions opt;
        opt.includePlugins = j.contains("plugins") && j["plugins"].is_boolean() && j["plugins"].get<bool>();
        opt.includeLogs = j.contains("logs") && j["logs"].is_boolean() && j["logs"].get<bool>();
        const sys::ResetPlan plan = sys::planReset(settingsDir, dataDir, opt);
        std::string err;
        if (!plan.error.empty() || !sys::executeReset(settingsDir, dataDir, opt, plan.confirmToken, stamp, err)) {
            ok = false;
            return "the settings reset could not be carried out (" + (plan.error.empty() ? err : plan.error) + "); it stays staged";
        }
        fs::path own = settingsDir / "wirec.json";   // the lane's own settings go with the rest
        if (fs::exists(own, ec)) {
            fs::create_directories(settingsDir / ("reset-" + stamp), ec);
            fs::rename(own, settingsDir / ("reset-" + stamp) / "wirec.json", ec);
        }
        fs::remove(pf, ec);
        return "settings reset: the files were moved into reset-" + stamp + " (nothing was deleted)";
    }
    fs::remove(pf, ec);
    return "an unknown pending action was dropped";
}

// ---- the first-run wizard ----
std::string SysOps::wizardStepName() const {
    switch (wizard_.step()) {
        case sys::WizardStep::Welcome: return "welcome";
        case sys::WizardStep::Radio: return "radio";
        case sys::WizardStep::Preset: return "preset";
        case sys::WizardStep::Privacy: return "privacy";
        case sys::WizardStep::Done: return "done";
    }
    return "welcome";
}

bool SysOps::wizardVerb(const std::string& verb, std::string& why) {
    if (verb == "next") {
        if (!wizard_.next()) { why = "make this step's choice first"; return false; }
        return true;
    }
    if (verb == "back") {
        if (!wizard_.back()) { why = "this is the first step"; return false; }
        return true;
    }
    if (verb == "skip") {
        wizard_.skip();
        return true;
    }
    if (verb.rfind("radio:", 0) == 0) {
        const std::vector<sys::RadioChoiceInfo>& all = sys::radioChoices();
        int n = -1;
        try { n = std::stoi(verb.substr(6)); } catch (const std::exception&) { n = -1; }
        if (n < 0 || static_cast<std::size_t>(n) >= all.size()) { why = "no such radio choice"; return false; }
        if (!wizard_.chooseRadio(all[static_cast<std::size_t>(n)].family)) { why = "that radio cannot be chosen now"; return false; }
        return true;
    }
    if (verb.rfind("preset:", 0) == 0) {
        if (!wizard_.choosePreset(verb.substr(7))) { why = "that set of stations is not offered for this radio"; return false; }
        return true;
    }
    if (verb == "telemetry:1" || verb == "telemetry:0") {
        wizard_.chooseTelemetry(verb == "telemetry:1");
        telemetryChosen_ = true;
        return true;
    }
    why = "unknown step";
    return false;
}

std::string SysOps::radioLines() const {
    std::string out;
    for (const sys::RadioChoiceInfo& r : sys::radioChoices()) { out += std::string(out.empty() ? "" : "\n") + r.label; }
    return out;
}

std::string SysOps::presetLines() const {
    std::string out;
    for (const sys::StarterPreset& p : wizard_.offered()) { out += std::string(out.empty() ? "" : "\n") + p.id + "\t" + p.title; }
    return out;
}

}  // namespace cascade::engine::wirec
