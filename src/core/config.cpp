// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"

#include "core/telemetry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core {

namespace {

// Per-field typed extraction: a key that is absent OR carries the wrong JSON
// type leaves `dst` untouched (i.e. at its default). Wrong type is treated
// like absence rather than a load failure so one hand-edited mistake cannot
// wipe the user's other settings.

void getString(const json& j, const char* key, std::string& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        dst = it->get<std::string>();
    }
}

void getDouble(const json& j, const char* key, double& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {  // integer literals accepted too
        dst = it->get<double>();
    }
}

void getFloat(const json& j, const char* key, float& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        dst = static_cast<float>(it->get<double>());
    }
}

// Booleans are strict: JSON true/false only. A number or a string that "looks
// boolean" is a wrong-typed field, and the header's rule for those is that the
// default survives.
void getBool(const json& j, const char* key, bool& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) { dst = it->get<bool>(); }
}

void getInt(const json& j, const char* key, int& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) { dst = it->get<int>(); }
}

// Unix timestamps need the full 64-bit range: an int field would overflow in
// 2038 and start reporting negative "last checked" times to the UI.
void getInt64(const json& j, const char* key, std::int64_t& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) { dst = it->get<std::int64_t>(); }
}

// Unsigned counters (launches, crashes). is_number_unsigned rejects a negative
// literal outright, so a hand-edited "-1" cannot wrap to a colossal count and
// then be reported as though the software had been launched 18 quintillion
// times.
void getUint64(const json& j, const char* key, std::uint64_t& dst) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number_unsigned()) { dst = it->get<std::uint64_t>(); }
}

// An array of strings, filtered to the entries that ARE strings. A mixed array
// is a hand-edit; keeping the usable entries loses less than discarding the
// whole list would, and the alternative - refusing the file - would wipe every
// other setting over one bad element.
void getStringArray(const json& j, const char* key, std::vector<std::string>& dst) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) { return; }
    std::vector<std::string> v;
    for (const auto& e : *it) {
        if (e.is_string()) { v.push_back(e.get<std::string>()); }
    }
    dst = std::move(v);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Written as "not >= lo" so a NaN from a hand-edited file lands on the low
// clamp instead of propagating into a filter coefficient.
double clampd(double v, double lo, double hi) {
    if (!(v >= lo)) { return lo; }
    return v > hi ? hi : v;
}

}  // namespace

std::string ConfigStore::defaultPath() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    const fs::path dir = (base && *base) ? fs::path(base) : fs::path(".");
#else
    // XDG first, then ~/.config, matching what desktop users expect.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const fs::path dir = (xdg && *xdg)    ? fs::path(xdg)
                         : (home && *home) ? fs::path(home) / ".config"
                                           : fs::path(".");
#endif
    return (dir / "foxsdr" / "config.json").string();
}

bool ConfigStore::load(const std::string& path, AppConfig& out, std::string& error) {
    // Defaults first: every early return below then already leaves `out` in
    // a fully usable state, per the header contract.
    out = AppConfig{};
    error.clear();

    std::error_code ec;
    if (!fs::exists(fs::path(path), ec)) {
        return true;  // first run: defaults, and nothing went wrong
    }

    // A directory must be rejected before it is opened. Windows refuses to
    // open one at all, so `!f` below is enough there; POSIX opens it happily
    // and then throws on the first read, which aborted the process rather
    // than reporting a bad path. Checking the type here fails the same way on
    // both platforms.
    if (fs::is_directory(fs::path(path), ec)) {
        error = "config: \"" + path + "\" is a directory, not a file";
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "config: cannot open \"" + path + "\" for reading";
        return false;
    }

    // allow_exceptions=false: a corrupt file is an expected condition here,
    // not an exceptional one; parse errors surface as a discarded value.
    const json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "config: \"" + path + "\" is not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "config: root of \"" + path + "\" is not a JSON object";
        return false;
    }

    // Schema gate before reading anything else: a different schema number
    // may have renumbered or reinterpreted fields, so none of them can be
    // trusted. Missing schemaVersion is fine (defaults to 1); present but
    // non-integer or != 1 is a mismatch.
    {
        const auto it = j.find("schemaVersion");
        if (it != j.end() && (!it->is_number_integer() || it->get<int>() != 1)) {
            out = AppConfig{};
            error = "config: \"" + path +
                    "\" has an unsupported schemaVersion (expected 1)";
            return false;
        }
    }

    getString(j, "sourceKind", out.sourceKind);
    getString(j, "soapyArgs", out.soapyArgs);
    getString(j, "soapyAntenna", out.soapyAntenna);
    getString(j, "iqFilePath", out.iqFilePath);
    getDouble(j, "centerHz", out.centerHz);
    getString(j, "mode", out.mode);
    getDouble(j, "bandwidthHz", out.bandwidthHz);
    getFloat(j, "squelchDb", out.squelchDb);
    getFloat(j, "volume", out.volume);
    getFloat(j, "dbMin", out.dbMin);
    getFloat(j, "dbMax", out.dbMax);
    getFloat(j, "splitRatio", out.splitRatio);
    getDouble(j, "vfoOffsetHz", out.vfoOffsetHz);
    getDouble(j, "sampleRateHz", out.sampleRateHz);
    getBool(j, "stereoEnabled", out.stereoEnabled);
    getInt(j, "deemphasisIndex", out.deemphasisIndex);
    getBool(j, "nrEnabled", out.nrEnabled);
    getFloat(j, "nrStrength", out.nrStrength);
    getBool(j, "notchEnabled", out.notchEnabled);
    getDouble(j, "notchFreqHz", out.notchFreqHz);
    getDouble(j, "notchQ", out.notchQ);
    getBool(j, "autoNotch", out.autoNotch);
    getBool(j, "bandPlanOverlay", out.bandPlanOverlay);
    getString(j, "pluginCatalogueUrl", out.pluginCatalogueUrl);
    getBool(j, "pluginBrowserOpen", out.pluginBrowserOpen);
    getInt64(j, "pluginLastUpdateCheck", out.pluginLastUpdateCheck);
    getStringArray(j, "pluginTuneAllowed", out.pluginTuneAllowed);
    getBool(j, "webEnabled", out.webEnabled);
    getString(j, "webBindAddress", out.webBindAddress);
    getInt(j, "webPort", out.webPort);
    getBool(j, "catEnabled", out.catEnabled);
    getBool(j, "catBindAll", out.catBindAll);
    getInt(j, "catPort", out.catPort);
    getString(j, "webUsername", out.webUsername);
    getString(j, "webPasswordRecord", out.webPasswordRecord);
    getBool(j, "updateCheckEnabled", out.updateCheckEnabled);
    getBool(j, "telemetryEnabled", out.telemetryEnabled);
    getString(j, "telemetryInstallId", out.telemetryInstallId);
    getUint64(j, "telemetryLaunches", out.telemetryLaunches);
    getUint64(j, "telemetryCrashes", out.telemetryCrashes);
    getBool(j, "telemetryCleanExit", out.telemetryCleanExit);
    getString(j, "telemetryPending", out.telemetryPending);

    // THE INSTALL ID IS VALIDATED, NOT TRUSTED. It is the one telemetry field
    // that leaves the machine as free text, so a config that put something
    // meaningful there — a name, an email, a hostname — must not be able to
    // turn an anonymous counter into an identifying one. Anything that is not
    // exactly 32 lowercase hex characters is discarded, and reporting is
    // switched off with it: continuing with a fresh id would silently re-opt
    // the user in to something they may have been trying to disable by hand.
    if (!out.telemetryInstallId.empty() && !validInstallId(out.telemetryInstallId)) {
        out.telemetryInstallId.clear();
        out.telemetryEnabled = false;
        out.telemetryPending.clear();
    }
    // An id is MINTED on first run rather than disabling reporting: with
    // reporting on by default, a fresh install legitimately has no id yet,
    // and treating that as "switch it off" would mean it never reported at
    // all. A HAND-EDITED bad id still disables it (above) - that path is a
    // user trying to intervene, and is respected.
    if (out.telemetryPending.size() > AppConfig::kMaxPendingReportBytes) {
        out.telemetryPending.clear();
    }

    // Range sanitization — each rule and its WHY is documented in the header.
    const AppConfig defaults;
    out.volume = clampf(out.volume, 0.0f, 1.0f);
    out.splitRatio = clampf(out.splitRatio, 0.1f, 0.9f);
    if (out.deemphasisIndex < 0) { out.deemphasisIndex = 0; }
    if (out.deemphasisIndex > 2) { out.deemphasisIndex = 2; }
    out.nrStrength = clampf(out.nrStrength, 0.0f, 1.0f);
    out.notchFreqHz = clampd(out.notchFreqHz, 10.0, 20000.0);
    out.notchQ = clampd(out.notchQ, 0.1, 1000.0);
    if (!(out.dbMin < out.dbMax - 10.0f)) {
        // Reset BOTH: clamping one end would invent a range nobody chose.
        out.dbMin = defaults.dbMin;
        out.dbMax = defaults.dbMax;
    }
    if (out.sourceKind != "siggen" && out.sourceKind != "file" &&
        out.sourceKind != "soapy") {
        out.sourceKind = defaults.sourceKind;
    }
    // An empty catalogue URL is a hand-edit (or a deleted value), not a
    // request for "no catalogue": restore the published default rather than
    // leaving the browser with nothing it could ever fetch. Any non-empty
    // value is kept verbatim — see the header for why validation lives in
    // PluginRepo and not in a second place here.
    if (out.pluginCatalogueUrl.empty()) {
        out.pluginCatalogueUrl = defaults.pluginCatalogueUrl;
    }
    // A timestamp before the epoch is a hand-edit or a backwards clock; either
    // way "never checked" is the only reading that cannot mislead the UI.
    if (out.pluginLastUpdateCheck < 0) {
        out.pluginLastUpdateCheck = 0;
    }
    // Web server: the port is sanitized because it drives a numeric control;
    // the ADDRESS and the password record are not, because each has exactly one
    // enforcement point (evaluateBind and PasswordRecord::parse) and a second
    // copy here could only disagree with it. See the header.
    //
    // The empty-address rule is the one exception and it is a safety rule, not
    // a validation one: net/web_policy reads "" as "every interface", so a
    // field emptied by a hand-edit would mean the opposite of the safe default.
    if (out.catPort < 1024 || out.catPort > 65535) {
        out.catPort = defaults.catPort;
    }
    if (out.webPort < 1024 || out.webPort > 65535) {
        out.webPort = defaults.webPort;
    }
    if (out.webBindAddress.empty()) {
        out.webBindAddress = defaults.webBindAddress;
    }
    if (out.webUsername.empty()) {
        out.webUsername = defaults.webUsername;
    }

    // Tune grants: drop empties and duplicates, then cap. A duplicate would
    // make revoking the permission look like it did not work — the second copy
    // would still be there — and an empty name could never match a plugin, so
    // both are noise a hand-edit or an older build could leave behind.
    {
        std::vector<std::string> grants;
        for (const std::string& g : out.pluginTuneAllowed) {
            if (g.empty()) { continue; }
            if (std::find(grants.begin(), grants.end(), g) != grants.end()) { continue; }
            grants.push_back(g);
            if (grants.size() >= AppConfig::kMaxTuneGrants) { break; }
        }
        out.pluginTuneAllowed = std::move(grants);
    }
    return true;
}

bool ConfigStore::save(const std::string& path, const AppConfig& cfg, std::string& error) {
    error.clear();
    const fs::path target(path);

    std::error_code ec;
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        // create_directories is a no-op without error on an existing
        // directory, but reports one if a FILE squats on the path.
        if (ec || !fs::is_directory(parent)) {
            error = "config: cannot create directory \"" + parent.string() +
                    "\": " + (ec ? ec.message() : "path exists and is not a directory");
            return false;
        }
    }

    json j;
    j["schemaVersion"] = cfg.schemaVersion;
    j["sourceKind"] = cfg.sourceKind;
    j["soapyArgs"] = cfg.soapyArgs;
    j["soapyAntenna"] = cfg.soapyAntenna;
    j["iqFilePath"] = cfg.iqFilePath;
    j["centerHz"] = cfg.centerHz;
    j["mode"] = cfg.mode;
    j["bandwidthHz"] = cfg.bandwidthHz;
    j["squelchDb"] = cfg.squelchDb;
    j["volume"] = cfg.volume;
    j["dbMin"] = cfg.dbMin;
    j["dbMax"] = cfg.dbMax;
    j["splitRatio"] = cfg.splitRatio;
    j["vfoOffsetHz"] = cfg.vfoOffsetHz;
    j["sampleRateHz"] = cfg.sampleRateHz;
    j["stereoEnabled"] = cfg.stereoEnabled;
    j["deemphasisIndex"] = cfg.deemphasisIndex;
    j["nrEnabled"] = cfg.nrEnabled;
    j["nrStrength"] = cfg.nrStrength;
    j["notchEnabled"] = cfg.notchEnabled;
    j["notchFreqHz"] = cfg.notchFreqHz;
    j["notchQ"] = cfg.notchQ;
    j["autoNotch"] = cfg.autoNotch;
    j["bandPlanOverlay"] = cfg.bandPlanOverlay;
    j["pluginCatalogueUrl"] = cfg.pluginCatalogueUrl;
    j["pluginBrowserOpen"] = cfg.pluginBrowserOpen;
    j["pluginLastUpdateCheck"] = cfg.pluginLastUpdateCheck;
    j["pluginTuneAllowed"] = cfg.pluginTuneAllowed;
    j["webEnabled"] = cfg.webEnabled;
    j["webBindAddress"] = cfg.webBindAddress;
    j["webPort"] = cfg.webPort;
    j["catEnabled"] = cfg.catEnabled;
    j["catBindAll"] = cfg.catBindAll;
    j["catPort"] = cfg.catPort;
    j["webUsername"] = cfg.webUsername;
    j["webPasswordRecord"] = cfg.webPasswordRecord;
    j["updateCheckEnabled"] = cfg.updateCheckEnabled;
    j["telemetryEnabled"] = cfg.telemetryEnabled;
    j["telemetryInstallId"] = cfg.telemetryInstallId;
    j["telemetryLaunches"] = cfg.telemetryLaunches;
    j["telemetryCrashes"] = cfg.telemetryCrashes;
    j["telemetryCleanExit"] = cfg.telemetryCleanExit;
    j["telemetryPending"] = cfg.telemetryPending;
    const std::string text = j.dump(4) + "\n";

    // ATOMIC WRITE. The temp file lives in the target's own directory so the
    // final rename is a same-volume move — cross-volume "renames" degrade to
    // copy+delete, which is exactly the partial-write window this exists to
    // close. The pid suffix keeps parallel test suites from colliding.
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "config: cannot create temp file \"" + tmp.string() + "\"";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);  // best effort; the write already failed
            error = "config: write to temp file \"" + tmp.string() + "\" failed";
            return false;
        }
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Target locked, permission lost, etc. The old config is untouched —
        // that is the whole point — but the temp must not accumulate.
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "config: atomic replace of \"" + path + "\" failed: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace cascade::core
