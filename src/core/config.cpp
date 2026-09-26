// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"

#include "core/bias_tee_memory.hpp"

#include "core/plugin_api.hpp"
#include "core/telemetry.hpp"
// clampScopeRangeNm(): the radar scope's ladder of range steps.
//
// THE ONE PLACE core/ REACHES INTO gui/, and it is a considered exception
// rather than a slip. The legal set of ranges is a property of the VIEW - it
// is derived from what the scope can draw rings and labels for - while
// keeping a value the view has no meaning for out of the renderer is this
// sanitizer's whole job. Writing the ladder out a second time here is exactly
// how the two would come to disagree, and the disagreement would be silent:
// the file would load, the renderer would draw, and only the rings would be
// wrong. The header is ImGui-free and pulls in no GL, no window and no
// plugin instance, so nothing about this include reaches the application
// shell.
#include "core/transmitter.hpp"
#include "dsp/modulator.hpp"
#include "gui/demod_scope.hpp"
#include "gui/rail_banks.hpp"
#include "gui/scope_view.hpp"
// sanitiseSerialPortName() and serialBaudSupported(): the GPS port fields are
// repaired by the port layer's own rules, so a name or a rate this file let
// through is one the port layer will accept. A second copy of either rule
// here is how the two would come to disagree.
#include "core/serial_port.hpp"

#include <algorithm>
#include <cmath>
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

// The shared rule for the five string lists the config carries
// (pluginTuneAllowed, pluginsStopped, pluginMuteOverride, closedWindows,
// keyBindings): drop empties, drop duplicates, cap the length. One function
// rather than five loops, because the lists are the same shape and a rule that applied to
// only some of them would be a rule nobody could rely on. Name every caller
// here when another list arrives — an enumeration that stops being exhaustive
// is worse than none.
//
// closedWindows holds ImGui window identities rather than plugin file names,
// and keyBindings holds "actionId=chord" lines. Both are the same shape and
// want the same treatment, and keeping one rule is worth more than further
// functions that would only differ in their names.
std::vector<std::string> sanitisePluginNames(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    for (const std::string& n : in) {
        if (n.empty()) { continue; }
        if (std::find(out.begin(), out.end(), n) != out.end()) { continue; }
        out.push_back(n);
        if (out.size() >= AppConfig::kMaxTuneGrants) { break; }
    }
    return out;
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
    getBool(j, "lookForNetworkUsrps", out.lookForNetworkUsrps);
    getString(j, "nativeArgs", out.nativeArgs);
    // THE BIAS TEE, PER RADIO (AppConfig::biasTee). Element-wise tolerant: an
    // entry that is not a bool, or whose key is not "<kind>|<args>", is
    // skipped, and an "on" for a radio with no serial is dropped - the file
    // may say it, but no rule of this application would have written it, and
    // it would put power on whichever radio next answered in that socket.
    {
        const auto it = j.find("biasTee");
        if (it != j.end() && it->is_object()) {
            for (auto e = it->begin(); e != it->end(); ++e) {
                if (!e.value().is_boolean()) { continue; }
                const std::string& key = e.key();
                const std::size_t bar = key.find('|');
                if (bar == std::string::npos || bar == 0) { continue; }
                const bool on = e.value().get<bool>();
                if (on && !biasTeeArgsNameARadio(key.substr(bar + 1))) { continue; }
                if (out.biasTee.size() >= kBiasTeeMemoryCap) { break; }
                out.biasTee[key] = on;
            }
        }
        // THE RTL-SDR's OLD MEMORY of one dongle is carried over, under the
        // same rule. "nativeBiasT" - the one bool every other family opened
        // with - is deliberately NOT read: it cannot say which radio it was
        // meant for, so carrying it over would power every one of them, and
        // the next save drops it (see config.hpp).
        std::string rtlArgs;
        bool rtlOn = false;
        getString(j, "rtlBiasTArgs", rtlArgs);
        getBool(j, "rtlBiasT", rtlOn);
        if (!rtlArgs.empty() && (!rtlOn || biasTeeArgsNameARadio(rtlArgs))) {
            const std::string key = biasTeeRadioKey("rtlsdr", rtlArgs);
            if (out.biasTee.count(key) == 0 && out.biasTee.size() < kBiasTeeMemoryCap) {
                out.biasTee[key] = rtlOn;
            }
        }
    }
    // The converters, element-wise tolerant like userPresets: an entry that is
    // not an object is skipped, and every other rule (unknown mode = off, a bad
    // LO = off, no radio = dropped, the cap) is sanitiseConverters', below.
    {
        const auto it = j.find("converters");
        if (it != j.end() && it->is_array()) {
            std::map<std::string, ConverterSetting> conv;
            for (const auto& e : *it) {
                if (!e.is_object()) { continue; }
                std::string radio;
                std::string mode;
                ConverterSetting s;
                getString(e, "radio", radio);
                getString(e, "mode", mode);
                getDouble(e, "loHz", s.loHz);
                getBool(e, "inverted", s.inverted);
                s.mode = converterModeFromKey(mode);
                conv[radio] = s;
            }
            out.converters = std::move(conv);
        }
    }
    getString(j, "plutoUri", out.plutoUri);
    getString(j, "soapyAntenna", out.soapyAntenna);
    getString(j, "iqFilePath", out.iqFilePath);
    if (const auto sc = j.find("soundCard"); sc != j.end() && sc->is_object()) {
        getString(*sc, "device", out.soundCard.device);
        getString(*sc, "hostApi", out.soundCard.hostApi);
        getDouble(*sc, "rateHz", out.soundCard.rateHz);
        getString(*sc, "format", out.soundCard.format);
        getInt(*sc, "channel", out.soundCard.channel);
        getBool(*sc, "swapIq", out.soundCard.swapIq);
        getDouble(*sc, "centreHz", out.soundCard.centreHz);
    }
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
    getString(j, "patch", out.patch);
    // The main view: one of the two faces the window has, or the patch view
    // (the default) for anything else - never a name nothing draws.
    getString(j, "mainView", out.mainView);
    if (out.mainView != "patch" && out.mainView != "receiver") { out.mainView = "patch"; }
    getString(j, "bandPlanSelection", out.bandPlanSelection);
    // Carried as written, like bandPlanSelection: which codes are real is a
    // question for the catalogue and country tables of THIS build, answered
    // where they are applied (see config.hpp).
    getString(j, "language", out.language);
    getString(j, "country", out.country);
    // Both are a closed set of three spellings, unlike bandPlanSelection
    // (which is validated against whatever is actually installed, elsewhere,
    // by BandPlan::loadSelection). A hand-edited or future-build value this
    // build does not recognise resets to the default rather than reaching
    // gui::bandPlanSizeTierFromKey / bandPlanPaletteKindFromKey, which would
    // silently apply the identical fallback one layer further in — resetting
    // here keeps the config file itself an honest record of what loaded.
    getString(j, "bandPlanSize", out.bandPlanSize);
    if (out.bandPlanSize != "small" && out.bandPlanSize != "medium" &&
        out.bandPlanSize != "large") {
        out.bandPlanSize = "small";
    }
    getString(j, "bandPlanPalette", out.bandPlanPalette);
    if (out.bandPlanPalette != "classic" && out.bandPlanPalette != "vivid" &&
        out.bandPlanPalette != "mono") {
        out.bandPlanPalette = "classic";
    }
    // THE FREQUENCY DISPLAY STYLE, CLAMPED ON LOAD to a name the painter
    // knows - the same rule mapTrailStyle below follows, and for the same
    // reason: the file is user-editable, and an unknown value would otherwise
    // travel all the way to the draw loop for it to make the fallback
    // decision a second time. getString already leaves the default in place
    // for a non-string, so this only has to reject a string that is not one
    // of the three names.
    //
    // THE THREE NAMES ARE MIRRORED FROM gui/tune_control.hpp's
    // tunerStyleFromName, deliberately rather than by including it: core must
    // not depend on gui. tests/test_config.cpp loads each name in turn and
    // asserts the painter reads back the SAME style, so a name added on one
    // side and not the other fails there rather than becoming a setting that
    // saves and then does nothing.
    getString(j, "tunerDisplayStyle", out.tunerDisplayStyle);
    if (out.tunerDisplayStyle != "nixie" && out.tunerDisplayStyle != "neon" &&
        out.tunerDisplayStyle != "plain") {
        out.tunerDisplayStyle = "nixie";
    }
    // THE INTERFACE THEME, on the same rule: one of the six foxsdr-ui/1 preset
    // names, anything else today's bench. The names are MIRRORED from
    // gui/theme.cpp's preset table (core must not depend on gui);
    // tests/test_config.cpp holds the two lists together.
    getString(j, "uiTheme", out.uiTheme);
    if (out.uiTheme != "today" && out.uiTheme != "classic-xl" && out.uiTheme != "night" &&
        out.uiTheme != "glass" && out.uiTheme != "daylight" && out.uiTheme != "field") {
        out.uiTheme = "today";
    }
    // The counter's own settings. The scale is the two the painter draws; the
    // readings size is foxsdr-ui/1's sizes.readings range. Clamped here so a
    // hand-edited value never reaches the geometry.
    getInt(j, "counterScale", out.counterScale);
    out.counterScale = std::clamp(out.counterScale, 1, 2);
    getBool(j, "counterSwitches", out.counterSwitches);
    getFloat(j, "readingsScale", out.readingsScale);
    if (!(out.readingsScale >= 1.0f)) { out.readingsScale = 1.0f; }
    if (out.readingsScale > 3.0f) { out.readingsScale = 3.0f; }
    // Both default true, so an older config that has never heard of them
    // arrives with trails drawn and coloured - see AppConfig for why the two
    // are separate switches. Neither has a range to clamp: a bool read by
    // getBool is either the value in the file or the default.
    getBool(j, "mapTrails", out.mapTrails);
    getBool(j, "mapTrailAltitudeColours", out.mapTrailAltitudeColours);
    getInt(j, "mapTrailStyle", out.mapTrailStyle);
    // CLAMPED ON LOAD, not trusted. The file is user-editable and an unknown
    // style would otherwise reach the draw loop and select nothing at all.
    if (out.mapTrailStyle < 0 || out.mapTrailStyle > 1) { out.mapTrailStyle = 0; }
    // Clamped, not trusted - the same 16..96 gui::clampAircraftIconPx allows.
    getInt(j, "aircraftIconPx", out.aircraftIconPx);
    out.aircraftIconPx = std::clamp(out.aircraftIconPx, 16, 96);
    getInt(j, "mapTrailWidthPx", out.mapTrailWidthPx);
    out.mapTrailWidthPx = std::clamp(out.mapTrailWidthPx, 1, 96);
    // The radar scope. The range is SNAPPED TO THE LADDER the view defines,
    // never trusted: the file is user-editable, and every ring radius, every
    // ring label and the corner readout are derived from this one number, so a
    // value off the ladder would reach the renderer as a scale nobody chose.
    // Nearest rather than a reset to the default, so a hand-edit that was
    // almost right keeps what it was reaching for.
    getBool(j, "scopeMode", out.scopeMode);
    getInt(j, "scopeRangeNm", out.scopeRangeNm);
    out.scopeRangeNm = cascade::gui::clampScopeRangeNm(out.scopeRangeNm);
    // The demod scope, on exactly the same discipline: read, then snapped onto
    // the ladders that define it. Every one of these three indexes a constant
    // array in gui/demod_scope.hpp, so an unclamped hand-edit would not be a
    // wrong setting - it would be a read off the end of one.
    getBool(j, "demodScopeOpen", out.demodScopeOpen);
    getInt(j, "demodScopeSignal", out.demodScopeSignal);
    out.demodScopeSignal =
        static_cast<int>(cascade::gui::scopeSignalFromIndex(out.demodScopeSignal));
    getInt(j, "demodScopeTimebase", out.demodScopeTimebase);
    out.demodScopeTimebase = cascade::gui::clampScopeTimebase(out.demodScopeTimebase);
    getInt(j, "demodScopeGain", out.demodScopeGain);
    out.demodScopeGain = cascade::gui::clampScopeGain(out.demodScopeGain);
    getBool(j, "demodScopeAutoGain", out.demodScopeAutoGain);
    getInt(j, "demodScopeDisplay", out.demodScopeDisplay);
    out.demodScopeDisplay = cascade::gui::clampScopeDisplay(out.demodScopeDisplay);
    // The transmitter. Every index is snapped onto a table that exists; the
    // POWER deliberately is not, because this file does not know which board
    // will be opened - source::clampTxGainDb does it against the board's own
    // published range, and answers silence for anything it cannot honour.
    getBool(j, "transmitOpen", out.transmitOpen);
    getInt(j, "transmitMode", out.transmitMode);
    out.transmitMode = static_cast<int>(cascade::dsp::txModeFromIndex(out.transmitMode));
    getInt(j, "transmitInput", out.transmitInput);
    // AN UNKNOWN INPUT LANDS ON THE TONE, not on the microphone: a
    // hand-edited number this build does not know must not point a
    // transmitter at a room.
    out.transmitInput = static_cast<int>(cascade::core::txInputFromIndex(out.transmitInput));
    getDouble(j, "transmitPowerDb", out.transmitPowerDb);
    getBool(j, "transmitSplit", out.transmitSplit);
    getDouble(j, "transmitSplitHz", out.transmitSplitHz);
    if (!(out.transmitSplitHz > 0.0)) { out.transmitSplitHz = 145.5e6; }
    getDouble(j, "transmitToneHz", out.transmitToneHz);
    if (!(out.transmitToneHz > 0.0) || out.transmitToneHz > 20000.0) {
        out.transmitToneHz = 1000.0;
    }
    getBool(j, "transmitMonitor", out.transmitMonitor);
    getString(j, "transmitArgs", out.transmitArgs);
    // Same discipline for the rail's bank: read, then clamped to one that
    // exists, so a file from a build with more or fewer banks opens somewhere.
    getInt(j, "railBank", out.railBank);
    out.railBank = static_cast<int>(cascade::gui::railBankFromIndex(out.railBank));
    // The rebound keys. Read here as plain strings and understood nowhere in
    // this file: gui/key_bindings.hpp turns them into a table, and a line it
    // cannot read is dropped on its own there rather than costing the user the
    // good lines beside it.
    getStringArray(j, "keyBindings", out.keyBindings);
    // LEGACY KEYS, READ AND NEVER WRITTEN. A file saved before the map became
    // one page per plugin carries its single window's rectangle here; it is
    // read so that rectangle can seed the pages' default placement, and save()
    // below deliberately does not emit these four keys — mapPages is the
    // rectangle store now.
    getInt(j, "mapWindowWidth", out.mapWindowWidth);
    getInt(j, "mapWindowHeight", out.mapWindowHeight);
    getInt(j, "mapWindowX", out.mapWindowX);
    getInt(j, "mapWindowY", out.mapWindowY);
    // Per-plugin map pages. Element-wise tolerant like getStringArray: an
    // entry that is not an object is a hand-edit and is skipped, the usable
    // entries survive. Rectangle sanitization happens with the legacy
    // rectangle's below, so the two can never apply different rules.
    {
        const auto it = j.find("mapPages");
        if (it != j.end() && it->is_array()) {
            std::vector<AppConfig::MapPage> pages;
            for (const auto& e : *it) {
                if (!e.is_object()) { continue; }
                AppConfig::MapPage pg;
                getString(e, "plugin", pg.plugin);
                getInt(e, "x", pg.x);
                getInt(e, "y", pg.y);
                getInt(e, "width", pg.width);
                getInt(e, "height", pg.height);
                getBool(e, "open", pg.open);
                // An empty name can never match a plugin, and a duplicate
                // would make one page's geometry restore depend on which
                // entry happened to be read last — first one wins.
                if (pg.plugin.empty()) { continue; }
                const bool dup =
                    std::any_of(pages.begin(), pages.end(),
                                [&pg](const AppConfig::MapPage& q) {
                                    return q.plugin == pg.plugin;
                                });
                if (dup) { continue; }
                pages.push_back(std::move(pg));
                if (pages.size() >= AppConfig::kMaxMapPages) { break; }
            }
            out.mapPages = std::move(pages);
        }
    }
    getBool(j, "rxPositionSet", out.rxPositionSet);
    getDouble(j, "rxLatDeg", out.rxLatDeg);
    getDouble(j, "rxLonDeg", out.rxLonDeg);
    getString(j, "gpsPort", out.gpsPort);
    getInt(j, "gpsBaud", out.gpsBaud);
    getString(j, "pluginCatalogueUrl", out.pluginCatalogueUrl);
    getBool(j, "pluginBrowserOpen", out.pluginBrowserOpen);
    // The fitted modules window, open flag and rectangle. Its rectangle is
    // sanitized with the map's below, by the same lambda, so the two stores
    // can never apply different rules to the same kind of value.
    getBool(j, "fittedModulesOpen", out.fittedModulesOpen);
    getInt(j, "fittedModulesX", out.fittedModulesX);
    getInt(j, "fittedModulesY", out.fittedModulesY);
    getInt(j, "fittedModulesWidth", out.fittedModulesWidth);
    getInt(j, "fittedModulesHeight", out.fittedModulesHeight);
    getInt64(j, "pluginLastUpdateCheck", out.pluginLastUpdateCheck);
    getStringArray(j, "pluginTuneAllowed", out.pluginTuneAllowed);
    getStringArray(j, "closedWindows", out.closedWindows);
    getStringArray(j, "pluginsStopped", out.pluginsStopped);
    getStringArray(j, "pluginMuteOverride", out.pluginMuteOverride);
    // Host API level 1 (0.99.31). The grant is a list like the others; the
    // settings store is an object of objects, read element-wise: a member
    // that is not an object, or a value that is not a string, is a hand-edit
    // and is skipped rather than failing the whole file.
    getStringArray(j, "pluginSettingsAllowed", out.pluginSettingsAllowed);
    {
        const auto it = j.find("pluginSettings");
        if (it != j.end() && it->is_object()) {
            std::map<std::string, std::map<std::string, std::string>> m;
            for (auto p = it->begin(); p != it->end(); ++p) {
                if (!p.value().is_object()) { continue; }
                std::map<std::string, std::string> kv;
                for (auto e = p.value().begin(); e != p.value().end(); ++e) {
                    if (e.value().is_string()) { kv[e.key()] = e.value().get<std::string>(); }
                }
                m[p.key()] = std::move(kv);
            }
            out.pluginSettings = std::move(m);
        }
    }
    // The user's own presets. Element-wise tolerant like mapPages: an entry
    // that is not an object is a hand-edit and is skipped; every other rule
    // (key, frequency, label, caps, duplicates) is sanitiseUserPresets', below.
    {
        const auto it = j.find("userPresets");
        if (it != j.end() && it->is_array()) {
            std::vector<UserPreset> presets;
            for (const auto& e : *it) {
                if (!e.is_object()) { continue; }
                UserPreset p;
                getString(e, "plugin", p.plugin);
                getString(e, "label", p.label);
                getDouble(e, "frequencyHz", p.frequencyHz);
                int mode = 0;
                getInt(e, "demodMode", mode);
                p.demodMode = mode < 0 ? 0u : static_cast<std::uint32_t>(mode);
                getDouble(e, "bandwidthHz", p.bandwidthHz);
                presets.push_back(std::move(p));
            }
            out.userPresets = std::move(presets);
        }
    }
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
    getBool(j, "diagnosticsEnabled", out.diagnosticsEnabled);
    getBool(j, "diagnosticsMinidump", out.diagnosticsMinidump);
    getStringArray(j, "crashUploadRecent", out.crashUploadRecent);
    getUint64(j, "crashUploadWindowStart", out.crashUploadWindowStart);
    {
        // Stored as a uint64 like its neighbours and narrowed here, because the
        // count is a small number and a config that put 2^40 in it must not
        // wrap into a value the limiter reads as "nothing sent yet".
        std::uint64_t count = out.crashUploadWindowCount;
        getUint64(j, "crashUploadWindowCount", count);
        out.crashUploadWindowCount =
            (count > 1000ull) ? 1000u : static_cast<std::uint32_t>(count);
    }
    getUint64(j, "crashUploadBlockedUntil", out.crashUploadBlockedUntil);

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
        out.sourceKind != "soapy" && out.sourceKind != "rtlsdr" &&
        out.sourceKind != "hackrf" && out.sourceKind != "airspy" &&
        out.sourceKind != "airspyhf" && out.sourceKind != "sdrplay" &&
        out.sourceKind != "mirisdr" && out.sourceKind != "rx888" &&
        out.sourceKind != "pluto" && out.sourceKind != "soundcard") {
        out.sourceKind = defaults.sourceKind;
    }
    // The sound card's settings, each back to its default on its own when
    // hand-edited into nonsense (see the header).
    if (out.soundCard.format != "real" && out.soundCard.format != "iq") {
        out.soundCard.format = defaults.soundCard.format;
    }
    out.soundCard.channel = (out.soundCard.channel == 1) ? 1 : 0;
    if (!std::isfinite(out.soundCard.rateHz) || out.soundCard.rateHz < 8000.0 ||
        out.soundCard.rateHz > 768000.0) {
        out.soundCard.rateHz = defaults.soundCard.rateHz;
    }
    if (!std::isfinite(out.soundCard.centreHz)) { out.soundCard.centreHz = 0.0; }
    // AN EMPTY PLUTO ADDRESS IS NOT A CHOICE, it is a field that was cleared
    // or a key hand-edited to "". The box would come up blank with nothing
    // saying what belongs in it, so it falls back to the address the board
    // serves out of the box - the same answer a config that has never seen
    // the key gets.
    if (out.plutoUri.empty()) { out.plutoUri = defaults.plutoUri; }
    // Map window geometry is validated as ONE rectangle: any bad component
    // discards all four, so the window falls back to the size derived from the
    // monitor rather than to a rectangle half of which somebody hand-edited.
    // See the header for why zero width is the "nothing saved" sentinel and
    // why the position rides with the size.
    {
        // ONE rule for both stores, written once. A per-page rectangle failing
        // it zeroes only that entry's rectangle — the entry itself, with its
        // plugin name and open flag, survives and falls back to default
        // placement, because "where the window sat" and "whether it was open"
        // are separate decisions and only one of them went bad.
        const auto rectOk = [](int x, int y, int w, int h) {
            const bool sized = w != 0 || h != 0;
            const bool sizeOk = w >= AppConfig::kMapWindowMinPx &&
                                w <= AppConfig::kMapWindowMaxPx &&
                                h >= AppConfig::kMapWindowMinPx &&
                                h <= AppConfig::kMapWindowMaxPx;
            const bool posOk = x >= -AppConfig::kMapWindowMaxPx &&
                               x <= AppConfig::kMapWindowMaxPx &&
                               y >= -AppConfig::kMapWindowMaxPx &&
                               y <= AppConfig::kMapWindowMaxPx;
            return sized && sizeOk && posOk;
        };
        if (!rectOk(out.mapWindowX, out.mapWindowY, out.mapWindowWidth,
                    out.mapWindowHeight)) {
            out.mapWindowWidth = 0;
            out.mapWindowHeight = 0;
            out.mapWindowX = 0;
            out.mapWindowY = 0;
        }
        for (AppConfig::MapPage& pg : out.mapPages) {
            if (!rectOk(pg.x, pg.y, pg.width, pg.height)) {
                pg.x = 0;
                pg.y = 0;
                pg.width = 0;
                pg.height = 0;
            }
        }
        // The fitted modules window, under the same rule and for the same
        // reason: a half-rejected rectangle is a rectangle nobody chose. The
        // open flag is deliberately left alone — it is a separate decision,
        // and a bad rectangle must not close a window the user left open.
        if (!rectOk(out.fittedModulesX, out.fittedModulesY, out.fittedModulesWidth,
                    out.fittedModulesHeight)) {
            out.fittedModulesX = 0;
            out.fittedModulesY = 0;
            out.fittedModulesWidth = 0;
            out.fittedModulesHeight = 0;
        }
    }
    // The receiver's position is validated as ONE position, for the same reason
    // the rectangle above is validated as one rectangle: a latitude clamped to
    // a pole beside a longitude the user did type is a place nobody chose, and
    // it would then be the origin of every range, bearing and coverage wedge on
    // the map. See the header for why the flag exists rather than a sentinel.
    //
    // WRITTEN AS A POSITIVE RANGE TEST, not as the negation of one. `!(lat >
    // 90 || lat < -90)` accepts NaN; `lat >= -90 && lat <= 90` rejects it,
    // which is the same rule the preset frequency check follows and the reason
    // no separate isfinite() test is needed here.
    {
        // AND NOT THE ORIGIN. 0 N 0 E with the flag set used to load as a set
        // position, on the argument that the origin is a place and the flag,
        // not a sentinel, says whether it is meant. A real user's scope was
        // then found measuring from the Gulf of Guinea, the view dragged three
        // thousand miles to the coast it belonged on: the pair had been
        // applied from a control whose fields still read 0.00000. The one
        // receiver that could honestly sit at 0,0 is a buoy. The application
        // now refuses the pair at every door (receiverPositionAcceptable in
        // gui/scope_view.hpp), and a file that carries it opens as UNSET -
        // which is what puts the one-click offers back on the rail for the
        // user who has it.
        const bool inRange =
            cascade::gui::receiverPositionAcceptable(out.rxLatDeg, out.rxLonDeg);
        if (!out.rxPositionSet || !inRange) {
            out.rxPositionSet = false;
            out.rxLatDeg = 0.0;
            out.rxLonDeg = 0.0;
        }
    }
    // The GPS port and baud, by the port layer's rules and no others (see the
    // include note at the top). A name that sanitises to nothing loads as "no
    // port chosen", which is the honest reading of a hand-edit the port layer
    // could never open; a baud off the list is a rate the port cannot be set
    // to, so the default is restored rather than a request the driver would
    // refuse being persisted for every launch to come.
    out.gpsPort = sanitiseSerialPortName(out.gpsPort);
    if (!serialBaudSupported(out.gpsBaud)) {
        out.gpsBaud = defaults.gpsBaud;
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
    //
    // THE STOP LIST GETS THE SAME TREATMENT FROM THE SAME CODE, because it is
    // the same kind of list — module file names the user ticked — and the same
    // failure mode: a duplicate would make Start look like it did nothing,
    // since the second copy would still be stopping the plugin.
    out.pluginTuneAllowed = sanitisePluginNames(out.pluginTuneAllowed);
    // Same shape as the plugin-name lists and the same treatment: empties
    // and duplicates dropped, length capped. A window identity is longer than
    // a plugin name but is bounded by the same reasoning.
    out.closedWindows = sanitisePluginNames(out.closedWindows);
    out.pluginsStopped = sanitisePluginNames(out.pluginsStopped);
    // And the mute overrides, for the third time from the same function. A
    // duplicate here would be a preference that flipped twice - which is the
    // same as not being there at all, but only if something removes it.
    out.pluginMuteOverride = sanitisePluginNames(out.pluginMuteOverride);
    // The settings grant: the same list of module file names, the same rule.
    out.pluginSettingsAllowed = sanitisePluginNames(out.pluginSettingsAllowed);
    // The plugins' own settings, by the SAME bounds the live API enforces on
    // settings_set, so nothing a hand-edit put in the file can reach a plugin
    // that the plugin could not have written itself.
    out.pluginSettings = sanitisePluginSettings(out.pluginSettings);
    out.userPresets = sanitiseUserPresets(out.userPresets);
    out.converters = sanitiseConverters(out.converters);
    // And the rebound keys, from the same function for the fourth time. An
    // empty line could name no action, and a line repeated verbatim is one
    // rebind stated twice - both are noise a hand-edit leaves behind, and the
    // cap keeps a hostile file from growing the config without bound.
    out.keyBindings = sanitisePluginNames(out.keyBindings);
    return true;
}

std::string ConfigStore::serialize(const AppConfig& cfg) {
    json j;
    j["schemaVersion"] = cfg.schemaVersion;
    j["sourceKind"] = cfg.sourceKind;
    j["soapyArgs"] = cfg.soapyArgs;
    j["lookForNetworkUsrps"] = cfg.lookForNetworkUsrps;
    j["nativeArgs"] = cfg.nativeArgs;
    {
        json bias = json::object();
        for (const auto& [radio, on] : cfg.biasTee) { bias[radio] = on; }
        j["biasTee"] = std::move(bias);
    }
    {
        json conv = json::array();
        for (const auto& [radio, s] : cfg.converters) {
            json e;
            e["radio"] = radio;
            e["mode"] = converterModeKey(s.mode);
            e["loHz"] = s.loHz;
            e["inverted"] = s.inverted;
            conv.push_back(std::move(e));
        }
        j["converters"] = std::move(conv);
    }
    j["plutoUri"] = cfg.plutoUri;
    j["soapyAntenna"] = cfg.soapyAntenna;
    j["iqFilePath"] = cfg.iqFilePath;
    j["soundCard"] = {{"device", cfg.soundCard.device},     {"hostApi", cfg.soundCard.hostApi},
                      {"rateHz", cfg.soundCard.rateHz},     {"format", cfg.soundCard.format},
                      {"channel", cfg.soundCard.channel},   {"swapIq", cfg.soundCard.swapIq},
                      {"centreHz", cfg.soundCard.centreHz}};
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
    j["patch"] = cfg.patch;
    j["mainView"] = cfg.mainView;
    j["bandPlanSelection"] = cfg.bandPlanSelection;
    j["language"] = cfg.language;
    j["country"] = cfg.country;
    j["bandPlanSize"] = cfg.bandPlanSize;
    j["bandPlanPalette"] = cfg.bandPlanPalette;
    j["tunerDisplayStyle"] = cfg.tunerDisplayStyle;
    j["uiTheme"] = cfg.uiTheme;
    j["counterScale"] = cfg.counterScale;
    j["counterSwitches"] = cfg.counterSwitches;
    j["readingsScale"] = cfg.readingsScale;
    j["mapTrails"] = cfg.mapTrails;
    j["mapTrailAltitudeColours"] = cfg.mapTrailAltitudeColours;
    j["mapTrailStyle"] = cfg.mapTrailStyle;
    j["aircraftIconPx"] = cfg.aircraftIconPx;
    j["mapTrailWidthPx"] = cfg.mapTrailWidthPx;
    j["scopeMode"] = cfg.scopeMode;
    j["scopeRangeNm"] = cfg.scopeRangeNm;
    j["demodScopeOpen"] = cfg.demodScopeOpen;
    j["demodScopeSignal"] = cfg.demodScopeSignal;
    j["demodScopeTimebase"] = cfg.demodScopeTimebase;
    j["demodScopeGain"] = cfg.demodScopeGain;
    j["demodScopeAutoGain"] = cfg.demodScopeAutoGain;
    j["demodScopeDisplay"] = cfg.demodScopeDisplay;
    j["transmitOpen"] = cfg.transmitOpen;
    j["transmitMode"] = cfg.transmitMode;
    j["transmitInput"] = cfg.transmitInput;
    j["transmitPowerDb"] = cfg.transmitPowerDb;
    j["transmitSplit"] = cfg.transmitSplit;
    j["transmitSplitHz"] = cfg.transmitSplitHz;
    j["transmitToneHz"] = cfg.transmitToneHz;
    j["transmitMonitor"] = cfg.transmitMonitor;
    j["transmitArgs"] = cfg.transmitArgs;
    // AND NOTHING FOR THE KEY. There is no transmitPtt and no transmitLatched
    // in this object, deliberately - see the note in config.hpp. A saved key
    // would be a radio that came up transmitting.
    j["railBank"] = cfg.railBank;
    j["keyBindings"] = cfg.keyBindings;
    // The legacy single-window rectangle (mapWindowWidth/Height/X/Y) is
    // deliberately NOT written: the map is one page per plugin now, and
    // mapPages below is the rectangle store. The keys are still read (see
    // load) so an older file's rectangle seeds the pages' default placement.
    {
        json pages = json::array();
        for (const AppConfig::MapPage& pg : cfg.mapPages) {
            json e;
            e["plugin"] = pg.plugin;
            e["x"] = pg.x;
            e["y"] = pg.y;
            e["width"] = pg.width;
            e["height"] = pg.height;
            e["open"] = pg.open;
            pages.push_back(std::move(e));
        }
    // THE LEGACY SEED SURVIVES UNTIL IT HAS BEEN SPENT. mapPages replaced the
    // four mapWindow* keys, which are read as the default rectangle for a
    // page with no saved entry and are normally not written back. But
    // migration happens only when a track-capable plugin actually creates a
    // page - and a launch with no such plugin would otherwise save an empty
    // mapPages AND drop the legacy keys, destroying the user's old map
    // rectangle one plugin-less session before it could ever be inherited.
    // So while mapPages is still empty, the legacy keys are written through
    // verbatim; the first real page entry retires them.
    if (cfg.mapPages.empty() && cfg.mapWindowWidth > 0 && cfg.mapWindowHeight > 0) {
        j["mapWindowWidth"] = cfg.mapWindowWidth;
        j["mapWindowHeight"] = cfg.mapWindowHeight;
        j["mapWindowX"] = cfg.mapWindowX;
        j["mapWindowY"] = cfg.mapWindowY;
    }
        j["mapPages"] = std::move(pages);
    }
    j["rxPositionSet"] = cfg.rxPositionSet;
    j["rxLatDeg"] = cfg.rxLatDeg;
    j["rxLonDeg"] = cfg.rxLonDeg;
    j["gpsPort"] = cfg.gpsPort;
    j["gpsBaud"] = cfg.gpsBaud;
    j["pluginCatalogueUrl"] = cfg.pluginCatalogueUrl;
    j["pluginBrowserOpen"] = cfg.pluginBrowserOpen;
    j["fittedModulesOpen"] = cfg.fittedModulesOpen;
    j["fittedModulesX"] = cfg.fittedModulesX;
    j["fittedModulesY"] = cfg.fittedModulesY;
    j["fittedModulesWidth"] = cfg.fittedModulesWidth;
    j["fittedModulesHeight"] = cfg.fittedModulesHeight;
    j["pluginLastUpdateCheck"] = cfg.pluginLastUpdateCheck;
    j["pluginTuneAllowed"] = cfg.pluginTuneAllowed;
    j["closedWindows"] = cfg.closedWindows;
    j["pluginsStopped"] = cfg.pluginsStopped;
    j["pluginMuteOverride"] = cfg.pluginMuteOverride;
    j["pluginSettingsAllowed"] = cfg.pluginSettingsAllowed;
    {
        json ps = json::object();
        for (const auto& [plugin, kv] : cfg.pluginSettings) {
            json o = json::object();
            for (const auto& [k, v] : kv) { o[k] = v; }
            ps[plugin] = std::move(o);
        }
        j["pluginSettings"] = std::move(ps);
    }
    {
        json presets = json::array();
        for (const UserPreset& p : cfg.userPresets) {
            json e;
            e["plugin"] = p.plugin;
            e["label"] = p.label;
            e["frequencyHz"] = p.frequencyHz;
            e["demodMode"] = p.demodMode;
            e["bandwidthHz"] = p.bandwidthHz;
            presets.push_back(std::move(e));
        }
        j["userPresets"] = std::move(presets);
    }
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
    j["diagnosticsEnabled"] = cfg.diagnosticsEnabled;
    j["diagnosticsMinidump"] = cfg.diagnosticsMinidump;
    j["crashUploadRecent"] = cfg.crashUploadRecent;
    j["crashUploadWindowStart"] = cfg.crashUploadWindowStart;
    j["crashUploadWindowCount"] = static_cast<std::uint64_t>(cfg.crashUploadWindowCount);
    j["crashUploadBlockedUntil"] = cfg.crashUploadBlockedUntil;
    // error_handler_t::replace, like every dump() in this tree: this text
    // includes user-entered or remote strings, and a byte that is not valid
    // UTF-8 must cost one replacement character, never a throw out of a save
    // path. tests/test_json_dump_policy.cpp holds every site to this.
    return j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool ConfigStore::writeFile(const std::string& path, const std::string& text,
                             std::string& error) {
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

    // ATOMIC WRITE. The temp file lives in the target's own directory so the
    // final rename is a same-volume move — cross-volume "renames" degrade to
    // copy+delete, which is exactly the partial-write window this exists to
    // close. The pid suffix keeps parallel test suites from colliding. It is
    // NOT a thread id: gui::ConfigWriter runs this on a worker thread, but
    // that worker is the only writer of this process's temp file at a time
    // (requests are coalesced, never raced - see the file header there), so
    // the process-wide pid is still the right scope for "don't collide with
    // another cascade.exe", which is what this suffix has always been for.
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

bool ConfigStore::save(const std::string& path, const AppConfig& cfg, std::string& error) {
    // UNCHANGED BEHAVIOUR, SPLIT IN TWO. Every existing caller of save() -
    // this file's own tests among them - gets exactly the bytes and the
    // atomicity it always got; the split exists so gui::ConfigWriter can call
    // the two halves from different threads (see the header comment on both
    // functions above).
    return writeFile(path, serialize(cfg), error);
}

AppConfig startupState(AppConfig cfg) {
    // WHETHER goes; WHERE stays. See the declaration for why - and for why
    // mainView, the one face-of-the-window setting, is not touched here.
    cfg.scopeMode = false;
    cfg.demodScopeOpen = false;
    cfg.transmitOpen = false;
    cfg.pluginBrowserOpen = false;
    cfg.fittedModulesOpen = false;
    for (AppConfig::MapPage& page : cfg.mapPages) { page.open = false; }
    return cfg;
}

}  // namespace cascade::core
