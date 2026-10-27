// A headless run keeps the window's half of the settings file (engine extraction stage 7c).
//
// THE PROPERTY. The settings file is ONE file with two halves: the receiver's (the engine writes it: source,
// tuning, DSP, plugins, the web and CAT servers, the usage journal) and the window's (view state: the patch
// canvas, the themes, the rail, the key bindings, the map pages, the diagnostics and update switches ...).
// A headless engine on the user's file has no window, so it holds none of the window's half in any member -
// and it must save the file back with every one of those fields exactly as it read it, or a night of
// `foxsdr-engine --use-config` would wipe the desktop's layout. What makes that true is one line:
// Engine::publishConfig starts from the config AS LOADED (loadedCfg_), so a field no half of the tree writes
// goes back unchanged. That line was added by the 0.99.84 merge as a stopgap and is the only thing between the
// headless engine and a silent wipe; nothing tested it. This test does, on the real executable.
//
// WHAT IS ASSERTED, per run of `foxsdr-engine --config <file> --seconds 2` over a file in which EVERY field
// the window writes - and every field no half of the tree writes yet - holds a value that is not the default:
//   1. the run restored the file and ended cleanly (exit 0, "settings file in use (restored)");
//   2. every key the ENGINE does not write comes back byte for byte - the serialised value of that key is the
//      same text - except exactly what core::startupState clears (the open flags: the window starts on the
//      bench whatever was showing at the last exit, and says so by restoring them closed - so the engine,
//      which restores through the same function, writes them back closed too; the host clears them again on
//      its next load, which is why the difference cannot be seen from the window; docs/engine-stage4.md 6.17
//      records it as a finding, and this test pins it both ways: those fields DO come back cleared, and
//      nothing else moves);
//   3. no key is exempt without being one the engine really writes: the list of keys allowed to differ is held
//      against the assignments in Engine::fillConfig, Engine::telemetryJournal and ServerHost::store, so the
//      list cannot grow to cover a window field;
//   4. no vacuity: every key under 2 holds a non-default value in the file the run was given (a field the
//      seed left at its default could be lost without anyone seeing it);
//   5. the engine's half WAS restored and written back through the engine (a handful of the receiver's own
//      settings come back as seeded), so 2 is not "the file was left alone": the file was rewritten, with a
//      new usage journal (launches + 1) and the clean-exit marker;
//   6. a second run over the first run's output changes no key that is not the engine's and no more than
//      the first did (the round trip is stable).
//
// Break-it (docs/engine-stage4.md 6.17): publishConfig starts from a default AppConfig instead of loadedCfg_
// (every window field is lost: RED); or one field dropped after fillConfig (`c.keyBindings.clear()`): RED naming it.
//
// HERMETIC: every per-user folder is a scratch tree, the settings file is a scratch path named on the command
// line, the volume is seeded 0 (a service restores the user's volume, and this test must be silent), and the
// real %APPDATA% config is never named by anything here (the profile in use is checked first).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/patch_io.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

// THE KEYS THE ENGINE WRITES - and only those. Each is assigned by Engine::fillConfig, Engine::telemetryJournal
// or ServerHost::store (asserted below from the source text), so a restored value may legitimately come back
// changed (a radio that did not open, a clamp, a counter that moved). Everything else in the file is the
// window's, or nobody's yet, and must come back as it went in.
const std::set<std::string>& engineWritten() {
    static const std::set<std::string> keys = {
        // ServerHost::store
        "webEnabled", "webBindAddress", "webPort", "webUsername", "webPasswordRecord", "catEnabled", "catBindAll",
        "catPort",
        // Engine::fillConfig
        "sourceKind", "soapyAntenna", "soapyArgs", "nativeArgs", "lookForNetworkUsrps", "biasTee", "converters",
        "airspy", "audioBufferMs", "audioBufferAutoMs", "ppmCorrection", "ppm", "autoGainByRadio",
        "manualGainByRadio", "plutoUri", "rtlTcpAddr", "iqFilePath", "soundCard", "centerHz", "mode", "bandwidthHz",
        "squelchDb", "squelchAuto", "volume", "dbMin", "dbMax", "vfoOffsetHz", "sampleRateHz", "stereoEnabled", "deemphasisIndex",
        "nrEnabled", "nrStrength", "notchEnabled", "notchFreqHz", "notchQ", "autoNotch", "bandPlanSelection",
        "transmitOpen", "transmitMode", "transmitInput", "transmitPowerDb", "transmitSplit", "transmitSplitHz",
        "transmitToneHz", "transmitMonitor", "transmitArgs", "rxPositionSet", "rxLatDeg", "rxLonDeg",
        "pluginCatalogueUrl", "pluginLastUpdateCheck", "pluginTuneAllowed", "pluginSettingsAllowed", "pluginTransmitAllowed",
        "pluginSettings", "pluginsStopped", "pluginRun", "pluginSites", "closedWindows", "pluginMuteOverride", "userPresets",
        // Engine::telemetryJournal and the clean-exit flag
        "telemetryEnabled", "telemetryInstallId", "telemetryFirstRun", "telemetryFirstVersion", "telemetryLaunches",
        "telemetryCrashes", "telemetryExitsDied", "telemetryExitsKilled", "telemetryExitsEnded",
        "telemetryExitsUnknown", "telemetrySessionStarted", "telemetryCleanExit", "telemetryPending",
    };
    return keys;
}

std::string readSource(const fs::path& p) { return emt::readFile(p); }

// The text of the function whose definition starts with `signature`: from the first '{' after it to its match.
std::string bodyAt(const std::string& s, const std::string& signature) {
    const std::size_t at = s.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t open = s.find('{', at);
    if (open == std::string::npos) { return std::string(); }
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') { ++depth; }
        if (s[i] == '}' && --depth == 0) { return s.substr(open, i - open + 1); }
    }
    return std::string();
}

// Every `cfg.<name> =` / `c.<name> =` (or `.clear()` / `[`) in `body`: the fields a function writes.
std::set<std::string> assignedFields(const std::string& body, const char* var) {
    std::set<std::string> out;
    const std::regex re(std::string("\\b") + var + "\\.([A-Za-z0-9_]+)\\s*(=[^=]|\\.clear\\(|\\[)");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it) { out.insert((*it)[1].str()); }
    return out;
}

// A patch document the loader accepts (a magic line and a view line): what the patch canvas saves, in miniature.
std::string patchText(float zoom) {
    return std::string(cascade::core::patch::kPatchMagic) + " 1\nview 10 20 " + std::to_string(zoom) + "\n";
}

// EVERY field the window writes (AppWindow::currentConfig, diagnosticsJournal) and every one no half writes yet
// (the merge's gap), set to a value that is not the default and that the loader keeps. The receiver's half is
// seeded only where the headless engine restores it exactly (and the volume is zero: a service plays what it
// restores).
AppConfig seeded() {
    AppConfig c;
    // --- the receiver's half: enough to see the engine restored and re-wrote it -----------------------------------
    c.volume = 0.0f;
    c.mode = "USB";
    c.squelchDb = -80.0f;
    // AUTOMATIC SQUELCH is the ENGINE's since stub campaign B12 (it was seeded below as a field no half writes): a headless engine that restores it ON measures the
    // channel and moves the threshold, which is exactly what it is for - and exactly what the squelchDb == -80 check further down must not see. So it is seeded OFF here;
    // that the switch comes back from a config and goes into the next one is held by test_engine_api_squelch (A1: fillConfig and applyConfig, both ways).
    c.squelchAuto = false;
    c.nrEnabled = true;
    c.nrStrength = 0.75f;
    c.notchEnabled = true;
    c.notchFreqHz = 1500.0;
    c.deemphasisIndex = 1;
    c.stereoEnabled = false;
    // --- the window's half (AppWindow::currentConfig) -------------------------------------------------------------
    c.patch = patchText(3.0f);
    c.mainView = "receiver";
    c.splitRatio = 0.65f;
    c.bandPlanOverlay = false;
    c.language = "de";
    c.country = "DE";
    c.bandPlanSize = "large";
    c.bandPlanPalette = "vivid";
    c.tunerDisplayStyle = "neon";
    c.meterStyleVolume = "led";
    c.meterStyleRate = "needle";
    c.uiTheme = "night";
    c.counterScale = 2;
    c.counterSwitches = false;
    c.readingsScale = 1.5f;
    c.mapTrails = false;
    c.mapTrailAltitudeColours = false;
    c.mapTrailStyle = 1;
    c.aircraftIconPx = 64;
    c.mapTrailWidthPx = 6;
    c.scopeMode = true;           // startupState clears these five open flags (below)
    c.scopeRangeNm = 100;
    c.demodScopeOpen = true;
    c.demodScopeSignal = 2;
    c.demodScopeTimebase = 5;
    c.demodScopeGain = 4;
    c.demodScopeAutoGain = false;
    c.demodScopeDisplay = 1;
    c.railBank = 2;
    c.keyBindings = {"tune.up=Ctrl+Up", "record.toggle=Ctrl+R"};
    AppConfig::MapPage page;
    page.plugin = "Radar Sweep";
    page.x = 120;
    page.y = 80;
    page.width = 900;
    page.height = 640;
    page.open = true;             // cleared by startupState; the rectangle is kept
    c.mapPages = {page};
    c.gpsPort = "COM7";
    c.gpsBaud = 4800;
    c.pluginBrowserOpen = true;   // cleared by startupState
    c.fittedModulesOpen = true;   // cleared by startupState
    c.fittedModulesX = 40;
    c.fittedModulesY = 50;
    c.fittedModulesWidth = 700;
    c.fittedModulesHeight = 500;
    // diagnosticsJournal's:
    c.updateCheckEnabled = false;
    c.updateChannel = "beta";   // 0.99.88 (master), merged in 0.99.90: the field the saturation check below found missing from the seed
    c.diagnosticsEnabled = false;
    c.diagnosticsMinidump = true;
    c.crashUploadRecent = {"deadbeef:1700000000", "cafef00d:1700000100"};
    c.crashUploadWindowStart = 1700000000;
    c.crashUploadWindowCount = 3;
    c.crashUploadBlockedUntil = 1800000000;
    // --- fields NO half of the tree writes yet: they ride on loadedCfg_ alone ----------------------------------------
    c.radioSetupDontShow = true;
    c.waterfallRangeMode = "manual";
    c.patchPresets = {{"Airband", patchText(1.0f)}, {"Marine", patchText(2.0f)}};
    c.patchPresetPrevious = patchText(4.0f);   // the "previous" slot holds a patch document, not a name
    c.railCollapsed = true;
    c.statusCollapsed = true;
    c.displayOpen = true;
    c.interfaceScale = "150";
    c.bookmarkMarkers = false;
    c.bookmarkStackNames = false;
    c.spectrumTraceMode = "peak";
    c.spectrumAverageMs = 250;
    c.mapListOnScreenOnly = true;
    c.pluginSites = {"https://plugins.example.test/index.json"};
    c.pluginStoreX = 11;
    c.pluginStoreY = 22;
    c.pluginStoreWidth = 800;
    c.pluginStoreHeight = 600;
    c.pluginRun["Radar Sweep"] = "always";   // (the engine's since stub batch B2 - how a plugin runs; asserted back below, as seeded)
    c.pluginTransmitAllowed = {"Radar Sweep"};   // (the engine's since stub batch B13 - the TRANSMIT grant, saved with the other two; the arming window is never in the file)
    c.messagesSeen = {"msg-0099", "msg-0100"};
    c.testerToken = "0123456789abcdef0123456789abcdef";   // 32 hex: the loader discards anything else
    c.testerTokenInvalid = true;
    c.testerAppToken = "0123456789abcdef0123456789abcdef01234567";  // 40 hex
    c.testerAppTokenName = "Test Tester";
    c.testerUsageCurrent = "{\"u\":1}";
    c.testerUsagePending = {"{\"token\":\"0123456789abcdef0123456789abcdef01234567\",\"u\":2}"};  // under the active (app) token
    return c;
}

// The file as the loader makes of it (a value the loader clamps or drops is what it is, not what was typed):
// serialise, load, serialise again.
std::string normalised(const AppConfig& c) {
    const fs::path tmp = fs::temp_directory_path() / ("foxsdr-rt-norm-" + std::to_string(std::rand()) + ".json");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << ConfigStore::serialize(c);
    }
    AppConfig loaded;
    std::string err;
    const bool ok = ConfigStore::load(tmp.string(), loaded, err);
    std::error_code ec;
    fs::remove(tmp, ec);
    return ok ? ConfigStore::serialize(loaded) : std::string();
}

json parseFile(const fs::path& p) {
    const std::string text = emt::readFile(p);
    json j = json::parse(text, nullptr, false);
    return j;
}

std::string valueText(const json& j, const std::string& key) {
    const auto it = j.find(key);
    return it == j.end() ? std::string("<absent>") : it->dump();
}

struct Check {
    int differing = 0;
    std::vector<std::string> changed;  // keys that differ, engine-written or not
};

// For every key of `expect`: the engine's keys are skipped, every other key's serialised text must be equal.
Check compareNonEngine(const json& expect, const json& got, const char* what) {
    Check r;
    for (auto it = expect.begin(); it != expect.end(); ++it) {
        const std::string& key = it.key();
        if (valueText(expect, key) != valueText(got, key)) {
            r.changed.push_back(key);
            if (engineWritten().count(key) == 0) {
                ++r.differing;
                std::printf("  %s: window/neutral key '%s' CHANGED\n    was: %.200s\n    now: %.200s\n", what, key.c_str(),
                            valueText(expect, key).c_str(), valueText(got, key).c_str());
            }
        }
    }
    // A key the run ADDED that the file did not have, and the engine does not write.
    for (auto it = got.begin(); it != got.end(); ++it) {
        if (expect.find(it.key()) == expect.end() && engineWritten().count(it.key()) == 0) {
            ++r.differing;
            std::printf("  %s: key '%s' was ADDED by the run\n", what, it.key().c_str());
        }
    }
    return r;
}

}  // namespace

int main() {
    std::printf("test_engine_main_config_roundtrip\n");
#ifdef CASCADE_SOURCE_DIR
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
#else
    const fs::path src = fs::current_path() / "src";
#endif

    // --- 3. the list of exempt keys is the engine's own writes, from the source text -------------------------
    {
        const std::string engineCpp = readSource(src / "engine" / "engine.cpp");
        const std::string serverCpp = readSource(src / "engine" / "server_host.cpp");
        CHECK(!engineCpp.empty() && !serverCpp.empty());
        const std::string fill = bodyAt(engineCpp, "void Engine::fillConfig(");
        const std::string journal = bodyAt(engineCpp, "void Engine::telemetryJournal(");
        const std::string store = bodyAt(serverCpp, "void ServerHost::store(");
        CHECK(!fill.empty() && !journal.empty() && !store.empty());
        std::set<std::string> written = assignedFields(fill, "cfg");
        for (const std::string& f : assignedFields(journal, "cfg")) { written.insert(f); }
        for (const std::string& f : assignedFields(store, "c")) { written.insert(f); }
        for (const std::string& key : engineWritten()) {
            if (written.count(key) == 0) { std::printf("  '%s' is exempt but no engine function writes it\n", key.c_str()); }
            CHECK(written.count(key) == 1);
        }
        // ...and the other way: a field the engine starts writing must be added to the list on purpose.
        for (const std::string& f : written) {
            if (engineWritten().count(f) == 0) { std::printf("  the engine writes '%s' and the test does not know it\n", f.c_str()); }
            CHECK(engineWritten().count(f) == 1);
        }
    }

    const fs::path root = emt::scratch("engine-roundtrip");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);
    CHECK(ConfigStore::defaultPath().rfind(home.string(), 0) == 0);  // the real %APPDATA% is out of reach

    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const fs::path cfgPath = home / "rt" / "config.json";
    fs::create_directories(cfgPath.parent_path());

    // THE SEED: serialised, as the loader will see it.
    const AppConfig seedCfg = seeded();
    const std::string seedText = normalised(seedCfg);
    CHECK(!seedText.empty());
    {
        std::ofstream out(cfgPath, std::ios::binary);
        out << seedText;
    }
    const json seedJson = json::parse(seedText);
    // What startupState makes of it (the window's restore, which the engine's is): the expected file, for the
    // keys that are not the engine's.
    AppConfig loadedSeed;
    {
        std::string err;
        CHECK(ConfigStore::load(cfgPath.string(), loadedSeed, err));
    }
    const json expectJson = json::parse(ConfigStore::serialize(cascade::core::startupState(loadedSeed)));

    // --- 4. no vacuity: every key the run must keep holds a non-default value --------------------------------
    const json defaultJson = json::parse(ConfigStore::serialize(AppConfig{}));
    std::vector<std::string> unsaturated;
    for (auto it = seedJson.begin(); it != seedJson.end(); ++it) {
        if (engineWritten().count(it.key()) != 0) { continue; }
        if (valueText(seedJson, it.key()) == valueText(defaultJson, it.key())) { unsaturated.push_back(it.key()); }
    }
    // schemaVersion is a constant, not a setting.
    unsaturated.erase(std::remove(unsaturated.begin(), unsaturated.end(), std::string("schemaVersion")), unsaturated.end());
    for (const std::string& k : unsaturated) { std::printf("  the seed leaves '%s' at its default: it proves nothing\n", k.c_str()); }
    CHECK(unsaturated.empty());

    // --- 1/2/5. the first run --------------------------------------------------------------------------------
    const emt::RunResult first =
        emt::run(engineExe, {"--config", cfgPath.string(), "--seconds", "2"}, cwd, root / "out", "first", 120000);
    std::printf("  first run: exit %d after %.2f s\n%s", first.exitCode, first.seconds, first.err.c_str());
    CHECK(first.started && !first.timedOut);
    CHECK(first.exitCode == 0);
    CHECK(emt::contains(first.err, ": running, settings file in use (restored)\n"));
    CHECK(first.out.empty());
    CHECK(fs::exists(cfgPath));
    const json afterFirst = parseFile(cfgPath);
    CHECK(afterFirst.is_object());
    if (!afterFirst.is_object()) { return testSummary("test_engine_main_config_roundtrip"); }

    const Check c1 = compareNonEngine(expectJson, afterFirst, "first run");
    CHECK(c1.differing == 0);

    // The one stated difference, pinned in both directions: the open flags startupState clears are cleared.
    CHECK(seedJson["scopeMode"] == true && afterFirst["scopeMode"] == false);
    CHECK(seedJson["demodScopeOpen"] == true && afterFirst["demodScopeOpen"] == false);
    CHECK(seedJson["pluginBrowserOpen"] == true && afterFirst["pluginBrowserOpen"] == false);
    CHECK(seedJson["fittedModulesOpen"] == true && afterFirst["fittedModulesOpen"] == false);
    // (A run that LOST the pages - the mutant of this test's break-it - must read as red, not crash the reader.)
    const bool pageThere = afterFirst["mapPages"].is_array() && afterFirst["mapPages"].size() == 1;
    CHECK(pageThere);
    if (pageThere) {
        CHECK(seedJson["mapPages"][0]["open"] == true && afterFirst["mapPages"][0]["open"] == false);
        // ...and nothing else of those pages moved: the rectangles are the window's and survive.
        CHECK(afterFirst["mapPages"][0]["x"] == 120 && afterFirst["mapPages"][0]["width"] == 900);
    }
    CHECK(afterFirst["fittedModulesWidth"] == 700 && afterFirst["scopeRangeNm"] == 100);
    // HOW EACH PLUGIN RUNS is the engine's since stub batch B2 (it is no longer a field nobody writes, so the saturation check above skips it
    // - this is what holds it instead): a pinned plugin that is not even fitted here keeps its pin, as seeded, run after run.
    CHECK(afterFirst["pluginRun"].is_object() && afterFirst["pluginRun"]["Radar Sweep"] == "always" && afterFirst["pluginRun"].size() == 1);
    // THE PLUGIN SITES are the engine's since stub batch B3 (the store's list of other people's indexes; the engine restores it, reads each
    // site's kept copy and writes the list back): the seeded address comes back as seeded, in order, run after run - and with no kept copy
    // and no network the run reads nothing of it.
    CHECK(afterFirst["pluginSites"].is_array() && afterFirst["pluginSites"].size() == 1 &&
          afterFirst["pluginSites"][0] == "https://plugins.example.test/index.json");

    // 5. the engine's half was restored and written back through the engine, and the file was REWRITTEN.
    CHECK(afterFirst["mode"] == "USB");
    CHECK(afterFirst["squelchDb"] == -80.0);
    CHECK(afterFirst["nrEnabled"] == true);
    CHECK(afterFirst["notchEnabled"] == true);
    CHECK(afterFirst["deemphasisIndex"] == 1);
    CHECK(afterFirst["stereoEnabled"] == false);
    CHECK(afterFirst["telemetryLaunches"] == 1);                   // the usage journal started (0 -> 1)
    CHECK(afterFirst["telemetryCleanExit"] == true);               // the marker, last of all
    CHECK(!afterFirst["telemetryInstallId"].get<std::string>().empty());
    CHECK(seedJson["telemetryLaunches"] == 0 && seedJson["telemetryCleanExit"] == true);
    CHECK(emt::contains(first.err, "foxsdr-engine: settings saved "));

    // --- 6. a second run over the first's output: stable ------------------------------------------------------
    const emt::RunResult second =
        emt::run(engineExe, {"--config", cfgPath.string(), "--seconds", "2"}, cwd, root / "out", "second", 120000);
    std::printf("  second run: exit %d after %.2f s\n", second.exitCode, second.seconds);
    CHECK(second.started && !second.timedOut && second.exitCode == 0);
    const json afterSecond = parseFile(cfgPath);
    CHECK(afterSecond.is_object());
    if (afterSecond.is_object()) {
        const Check c2 = compareNonEngine(afterFirst, afterSecond, "second run");
        CHECK(c2.differing == 0);
        CHECK(afterSecond["pluginRun"] == afterFirst["pluginRun"]);
        CHECK(afterSecond["pluginSites"] == afterFirst["pluginSites"]);
        CHECK(afterSecond["telemetryLaunches"] == 2);
        CHECK(afterSecond["telemetryCrashes"] == 0);       // the first run ended cleanly: nothing to count
        CHECK(afterSecond["telemetryCleanExit"] == true);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_config_roundtrip");
}
