// kit_logic.cpp - the plugin kit's pure logic (kit_logic.hpp): the ImGui window's rules over the table's entries, with no
// window and no engine. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "kit_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/instrument_beacon_math.hpp"
#include "gui/instrument_meter_math.hpp"
#include "gui/instrument_teleprinter_math.hpp"
#include "gui/instrument_weather_console_math.hpp"

namespace foxsdr::slintkit {

namespace {

constexpr std::uint32_t kSignalCaps = CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;
constexpr double kPresetToleranceHz = 5000.0;   // core/plugin_ui.hpp kPresetToleranceHz: the floor of the on-preset test
constexpr std::uint32_t kTxBitDeclared = 1u, kTxBitAsked = 2u, kTxBitHolder = 4u, kTxBitWaiting = 8u, kTxBitStopped = 16u, kTxBitGranted = 32u, kTxBitOnAir = 64u;

// A version "1.2.3" starting at s[at]: a digit, then digits and dots (core/user_presets.cpp versionAt).
bool versionAt(const std::string& s, std::size_t at) {
    if (at >= s.size() || !std::isdigit(static_cast<unsigned char>(s[at]))) { return false; }
    for (std::size_t i = at; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '-' || c == '.') { continue; }
        if (!std::isdigit(static_cast<unsigned char>(c))) { return i > at && s[i - 1] == '-'; }
    }
    return true;
}

// THE USER-PRESET KEY AND THE RUN ID, as core/user_presets.cpp and core/plugin_run.cpp make them (mirrored here so this unit links without the
// engine; tests/test_slint_host_kit.cpp pins the format against the same examples): the file name without its folder, its extension and its
// "-<version>..." tail.
std::string keyOfFile(const std::string& moduleFile) {
    std::string s = moduleFile;
    const std::size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) { s = s.substr(slash + 1); }
    const std::size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) { s.resize(dot); }
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '-' && versionAt(s, i + 1)) { return s.substr(0, i); }
    }
    return s;
}

std::string fmtN(const char* f, long long v) { std::string s; cascade::core::formatUtf8(s, f, v); return s; }   // a std::string, never a fixed buffer (test_i18n)

void entry(FoxListItem& it) { it = FoxListItem{}; it.structSize = sizeof(it); }

// The whole fact, "" when the engine does not hold it.
std::string factOf(TableReader& t, const std::string& key) { std::string s; if (!t.fact(key, s)) { s.clear(); } return s; }

Preset ownPresetOf(const FoxListItem& it, const std::string& pluginName) {
    Preset p;
    p.hz = it.value[0];
    p.bwHz = (it.value[2] > 0.0 && it.value[2] < 1e12) ? it.value[2] : 0.0;
    p.deviceCentre = (it.flags & FOXAPI_ITEM_DEVICE_CENTRE) != 0u;
    p.index = static_cast<std::uint32_t>(it.id);
    p.label = it.name[0] != '\0' ? std::string(it.name) : pluginName;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
void readKitLists(TableReader& t, KitLists& c, const ReadOptions& o) {
    FoxListItem it{};
    const auto size = [&](std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); };
    const auto get = [&](std::uint32_t list, std::int32_t i) { entry(it); return t.readList(list, static_cast<std::uint32_t>(i), &it) > i; };

    // PLUGINS, one row a file the host holds, loaded or not, in the host's order
    const std::int32_t plugins = size(FOXAPI_LIST_PLUGINS);
    c.rows.clear();
    c.rows.reserve(static_cast<std::size_t>(plugins));
    for (std::int32_t i = 0; i < plugins; ++i) {
        if (!get(FOXAPI_LIST_PLUGINS, i)) { break; }
        PluginRow r;
        r.name = it.name;
        r.file = it.detail;
        r.runId = keyOfFile(r.file);
        r.caps = static_cast<std::uint32_t>(it.value[0]);
        r.fileBytes = static_cast<std::uint64_t>(it.value[1] > 0 ? it.value[1] : 0);
        r.noticeLevel = static_cast<std::uint32_t>(it.value[2] > 0 ? it.value[2] : 0);
        const int run = static_cast<int>(it.value[3]);
        r.runMode = run & 3;
        r.asleep = (run & 4) != 0;
        r.switchedOff = (run & 8) != 0;
        r.loaded = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        r.stopped = (it.flags & FOXAPI_ITEM_STOPPED) != 0u;
        r.tuneAllowed = (it.flags & FOXAPI_ITEM_TUNE_ALLOWED) != 0u;
        r.settingsAllowed = (it.flags & FOXAPI_ITEM_SETTINGS_ALLOWED) != 0u;
        r.matched = (it.flags & FOXAPI_ITEM_MATCHED) != 0u;
        r.askedSettings = (it.flags & FOXAPI_ITEM_ASKED_SETTINGS) != 0u;
        const std::string base = "fact.plugin." + r.file + ".";
        if (r.loaded && !r.file.empty()) {
            // the record's category (the DECODE bank's headings) and the titles of its own windows
            const std::string record = factOf(t, base + "record");
            std::size_t at = 0; bool have = true;
            for (int tab = 0; tab < 3 && have; ++tab) { at = record.find('\t', at); if (at == std::string::npos) { have = false; } else { ++at; } }
            if (have) { r.category = record.substr(at); }
            if (!record.empty()) {
                r.hasRecord = true;
                const std::size_t t1 = record.find('\t');
                r.catalogueKey = record.substr(0, t1);
                if (t1 != std::string::npos) {
                    r.abiVersion = static_cast<std::uint32_t>(std::strtoul(record.c_str() + t1 + 1, nullptr, 10));
                    const std::size_t t2 = record.find('\t', t1 + 1);
                    if (t2 != std::string::npos) { r.fittedAtUnix = std::strtoll(record.c_str() + t2 + 1, nullptr, 10); }
                }
            }
            if ((r.caps & (CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT)) != 0u) {
                const std::string windows = factOf(t, base + "windows");
                std::size_t pos = 0;
                while (pos < windows.size()) {
                    std::size_t end = windows.find('\n', pos);
                    if (end == std::string::npos) { end = windows.size(); }
                    const std::size_t tab = windows.find('\t', pos);
                    if (tab != std::string::npos && tab < end) {
                        if (windows.compare(pos, tab - pos, "panel") == 0) { r.panelTitle.assign(windows, tab + 1, end - tab - 1); }
                        else if (windows.compare(pos, tab - pos, "instrument") == 0) { r.instrumentTitle.assign(windows, tab + 1, end - tab - 1); }
                    }
                    pos = end + 1;
                }
            }
        }
        if (o.fittedFacts && !r.file.empty()) {
            r.path = factOf(t, base + "path");
            r.version = factOf(t, base + "version");
            r.author = factOf(t, base + "author");
            r.licence = factOf(t, base + "licence");
            r.error = factOf(t, base + "error");
            r.idleDetail = factOf(t, base + "idle");
            r.fault = factOf(t, base + "fault");
            r.notice = factOf(t, base + "notice");
            r.integrity = factOf(t, base + "integrity");
            const std::string tx = factOf(t, base + "tx");
            r.txBits = tx.empty() ? 0u : static_cast<std::uint32_t>(std::strtoul(tx.c_str(), nullptr, 10));
            const std::string origin = factOf(t, "fact.plugin.origin." + r.file);
            if (!origin.empty()) {
                const std::size_t tab = origin.find('\t');
                r.originWord = origin.substr(0, tab);
                if (tab != std::string::npos) { r.originSite = origin.substr(tab + 1); }
            }
        }
        c.rows.push_back(std::move(r));
    }

    // DECODERS, joined by display name to the LOADED row (the host runs one loaded plugin per name)
    const std::int32_t decoders = size(FOXAPI_LIST_DECODERS);
    for (std::int32_t i = 0; i < decoders; ++i) {
        if (!get(FOXAPI_LIST_DECODERS, i)) { break; }
        for (PluginRow& r : c.rows) {
            if (!r.loaded || r.decoder || r.name != it.name) { continue; }
            r.decoder = true;
            r.fed = (it.flags & FOXAPI_ITEM_FED) != 0u;
            r.mutes = (it.flags & FOXAPI_ITEM_MUTES) != 0u;
            r.idle = it.detail;
            break;
        }
    }
    // PRESETS (the plugin's own, by its name), then USER_PRESETS (the operator's, by the user key)
    const std::int32_t presets = size(FOXAPI_LIST_PRESETS);
    for (std::int32_t i = 0; i < presets; ++i) {
        if (!get(FOXAPI_LIST_PRESETS, i)) { break; }
        for (PluginRow& r : c.rows) {
            if (!r.loaded || r.name != it.detail) { continue; }
            r.presets.push_back(ownPresetOf(it, r.name));
            break;
        }
    }
    for (PluginRow& r : c.rows) { r.ownPresets = r.presets.size(); }
    const std::int32_t saved = size(FOXAPI_LIST_USER_PRESETS);
    for (std::int32_t i = 0; i < saved; ++i) {
        if (!get(FOXAPI_LIST_USER_PRESETS, i)) { break; }
        for (PluginRow& r : c.rows) {
            if (!r.loaded || keyOfFile(r.file) != it.detail) { continue; }
            Preset p;
            p.hz = it.value[0];
            p.bwHz = it.value[2];
            p.mine = true;
            p.index = 1000u + static_cast<std::uint32_t>(r.presets.size() - r.ownPresets);
            p.label = it.name;
            r.presets.push_back(std::move(p));
            break;
        }
    }
    // the commands a module registered (the plate keys and the pressable rows), filed under the module
    if (o.fittedFacts) {
        const std::int32_t keys = size(FOXAPI_LIST_PLUGIN_COMMANDS);
        for (std::int32_t i = 0; i < keys; ++i) {
            if (!get(FOXAPI_LIST_PLUGIN_COMMANDS, i)) { break; }
            for (PluginRow& r : c.rows) {
                if (r.file != it.detail) { continue; }
                r.commands.push_back(cascade::core::HostCommand{static_cast<std::uint32_t>(it.id), it.name});
                break;
            }
        }
    }

    c.muteCauses.clear();
    const std::int32_t causes = size(FOXAPI_LIST_MUTE_CAUSES);
    for (std::int32_t i = 0; i < causes; ++i) {
        if (!get(FOXAPI_LIST_MUTE_CAUSES, i)) { break; }
        c.muteCauses.push_back({it.name, it.detail});
    }
    c.staleGrants.clear();
    const std::int32_t stale = size(FOXAPI_LIST_STALE_GRANTS);
    for (std::int32_t i = 0; i < stale; ++i) {
        if (!get(FOXAPI_LIST_STALE_GRANTS, i)) { break; }
        c.staleGrants.push_back({it.name, (it.flags & FOXAPI_ITEM_REFUSED) != 0u});
    }

    c.settings.clear();
    if (o.settings) {
        const std::int32_t n = size(FOXAPI_LIST_PLUGIN_SETTINGS);
        for (std::int32_t i = 0; i < n; ++i) {
            if (!get(FOXAPI_LIST_PLUGIN_SETTINGS, i)) { break; }
            const std::string id = fmtN("%lld", static_cast<long long>(it.id));
            const std::string spec = factOf(t, "fact.plugin.setting." + id + ".spec");
            const std::size_t us = spec.find('\x1F');
            if (us == std::string::npos || us == 0u) { continue; }   // a field the table cannot name is not drawn
            if (c.settings.empty() || c.settings.back().file != it.detail) { c.settings.emplace_back(); c.settings.back().file = it.detail; }
            SettingSpec f;
            f.id = it.id;
            f.key = spec.substr(0, us);
            f.placeholder = spec.substr(us + 1u);
            f.label = it.name;
            f.kind = static_cast<std::uint32_t>(it.value[0]);
            f.maxLength = static_cast<std::size_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
            f.value = factOf(t, "fact.plugin.setting." + id);
            c.settings.back().fields.push_back(std::move(f));
        }
    }

    c.blocked.clear();
    if (o.blocked) {
        const std::int32_t n = size(FOXAPI_LIST_STORE_BLOCKED);
        for (std::int32_t i = 0; i < n; ++i) {
            if (!get(FOXAPI_LIST_STORE_BLOCKED, i)) { break; }
            BlockedRow b;
            b.file = it.name;
            b.why = it.detail;
            b.id = factOf(t, "fact.blocked." + b.file + ".id");
            b.message = factOf(t, "fact.blocked." + b.file + ".message");
            if (b.message.empty()) { b.message = b.why; }
            c.blocked.push_back(std::move(b));
        }
        c.catalogueUrl = factOf(t, "fact.store.url");
        c.enforceError = factOf(t, "fact.status.pluginEnforceError");
        c.installReport = factOf(t, "fact.status.installReport");
        c.installError = factOf(t, "fact.status.installError");
    }
    c.muteSubject = factOf(t, "fact.mute.subject");
    c.muteKeptRunning = factOf(t, "fact.mute.keptRunning") == "1";
    c.lastDenied = factOf(t, "fact.plugin.lastDenied");
    c.pluginDir = factOf(t, "fact.plugin.dir");
    c.txArmed = factOf(t, "fact.tx.plugin.armed") == "1";
}

const PluginRow* rowByName(const std::vector<PluginRow>& rows, const std::string& displayName) {
    if (displayName.empty()) { return nullptr; }
    for (const PluginRow& r : rows) { if (r.loaded && r.name == displayName) { return &r; } }
    return nullptr;
}
const PluginRow* rowByFile(const std::vector<PluginRow>& rows, const std::string& file) {
    for (const PluginRow& r : rows) { if (r.file == file) { return &r; } }
    return nullptr;
}
std::string fileForName(const std::vector<PluginRow>& rows, const std::string& displayName) {
    const PluginRow* r = rowByName(rows, displayName);
    return r != nullptr ? r->file : std::string();
}
const SettingsSet* settingsFor(const std::vector<SettingsSet>& sets, const std::string& file) {
    for (const SettingsSet& s : sets) { if (s.file == file) { return &s; } }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------
int lineTone(const std::string& text) {
    if (text.compare(0, 5, "ERROR") == 0) { return 2; }
    if (text.compare(0, 4, "WARN") == 0) { return 1; }
    return 0;
}

std::size_t readDecoderLinesTail(TableReader& t, std::uint64_t& lastId, std::deque<DecoderLine>& ring, std::size_t cap) {
    const std::int32_t n = t.readList(FOXAPI_LIST_DECODER_LINES, 0, nullptr);
    if (n <= 0) { return 0; }
    std::vector<DecoderLine> fresh;
    FoxListItem it{};
    std::uint64_t below = ~std::uint64_t{0};
    for (std::int32_t i = n - 1; i >= 0; --i) {
        entry(it);
        if (t.readList(FOXAPI_LIST_DECODER_LINES, static_cast<std::uint32_t>(i), &it) <= i) { continue; }
        if (it.id <= lastId || it.id >= below) { break; }   // the ring moved under the read: what was read stands
        below = it.id;
        DecoderLine row;
        row.id = it.id;
        row.plugin = it.name;
        row.text = it.detail;
        row.tone = lineTone(row.text);
        fresh.push_back(std::move(row));
    }
    for (auto r = fresh.rbegin(); r != fresh.rend(); ++r) { lastId = std::max(lastId, r->id); ring.push_back(std::move(*r)); }
    while (ring.size() > cap) { ring.pop_front(); }
    return fresh.size();
}

// ---------------------------------------------------------------------------------------------------------------------------------
FittedState fittedState(const PluginRow& m, bool receiverRunning) {
    if (!m.loaded) { return FittedState::Refused; }
    if (m.stopped) { return FittedState::Stopped; }
    if ((m.caps & kSignalCaps) == 0u) { return FittedState::NoSignal; }
    if (m.fed && receiverRunning) { return FittedState::Fed; }
    if (m.asleep) { return FittedState::Idle; }
    return FittedState::NotFed;
}

const char* fittedStateWord(FittedState s) {
    switch (s) {
        case FittedState::Fed: return cascade::i18n::tr("FED");
        case FittedState::NotFed: return cascade::i18n::tr("NOT DECODING");
        case FittedState::NoSignal: return cascade::i18n::tr("TAKES NO SIGNAL");
        case FittedState::Stopped: return cascade::i18n::tr("STOPPED");
        case FittedState::Refused: return cascade::i18n::tr("REFUSED");
        case FittedState::Idle: return cascade::i18n::tr("IDLE");
    }
    return cascade::i18n::tr("UNKNOWN");
}

int fittedStateIndex(FittedState s) {
    switch (s) {
        case FittedState::Fed: return 0;
        case FittedState::Idle: return 1;
        case FittedState::NotFed: return 2;
        case FittedState::NoSignal: return 3;
        case FittedState::Stopped: return 4;
        case FittedState::Refused: return 5;
    }
    return 2;
}

bool fittedOffersStart(const PluginRow& m) { return m.stopped || m.asleep; }

std::string fittedStateSentence(const PluginRow& m, bool receiverRunning) {
    switch (fittedState(m, receiverRunning)) {
        case FittedState::Refused: return m.error.empty() ? std::string(cascade::i18n::tr("The host refused this file and recorded no reason.")) : m.error;
        case FittedState::Stopped:
            return cascade::i18n::tr("You stopped this module. It stays installed and its code stays mapped, and it is given no decoders, no map targets and no "
                   "panels of its own until you start it again.");
        case FittedState::NoSignal:
            return cascade::i18n::tr("Fitted, and it takes no signal. This module declares no decoder, so nothing is routed to it and nothing should be - it works "
                   "through the capabilities listed above.");
        case FittedState::Fed: return cascade::i18n::tr("Fitted and being fed. The receiver is running and this module has a decoder matched to the rate it is producing.");
        case FittedState::Idle:
            return cascade::i18n::tr("Fitted, and idle. Nothing is using this module, so it is not running and costs nothing. It starts by itself when you open its "
                   "window, its map or the output it writes to, and goes quiet again 30 seconds after you finish with it. Tick KEEP RUNNING to have "
                   "it run all the time.");
        case FittedState::NotFed: break;
    }
    const std::string& detail = !m.idleDetail.empty() ? m.idleDetail : m.idle;
    if (!receiverRunning) {
        if (m.fed) {
            return cascade::i18n::tr("Fitted, and fed nothing because the receiver is stopped. It has a decoder matched to the rate the receiver is set to, so "
                   "starting the receiver is all this needs.");
        }
        if (!detail.empty()) { return cascade::core::formatText(cascade::i18n::tr("Fitted, and fed nothing because the receiver is stopped. There is a second reason as well: %s"), detail.c_str()); }
        return cascade::i18n::tr("Fitted, and fed nothing because the receiver is stopped.");
    }
    if (!detail.empty()) { return detail; }
    return cascade::i18n::tr("Fitted, and not being fed. No reason was recorded for it.");
}

std::string fittedVerdictLine(bool receiverRunning, const std::string& directory) {
    std::string s = receiverRunning ? cascade::i18n::tr("The receiver is running, so a module with a matched decoder is being fed.")
                                    : cascade::i18n::tr("The receiver is stopped, so NOTHING is being fed to any module however it is set.");
    s += ' ';
    s += directory.empty() ? std::string(cascade::i18n::tr("No directory has been scanned yet.")) : cascade::core::formatText(cascade::i18n::tr("Read from %s."), directory.c_str());
    return s;
}

std::string abiText(std::uint32_t abi, std::uint32_t thisBuild) {
    if (abi == 0u) { return cascade::i18n::tr("not recorded"); }
    if (abi == thisBuild) { return cascade::core::formatText(cascade::i18n::tr("%u, matches this build"), static_cast<unsigned>(abi)); }
    return cascade::core::formatText(cascade::i18n::tr("%u, this build needs %u"), static_cast<unsigned>(abi), static_cast<unsigned>(thisBuild));
}

std::string fittedDateText(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) { return std::string(); }
    const std::time_t tt = static_cast<std::time_t>(unixSeconds);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &tt) != 0) { return std::string(); }
#else
    if (localtime_r(&tt, &tmv) == nullptr) { return std::string(); }
#endif
    char b[48];
    std::strftime(b, sizeof(b), "%Y-%m-%d", &tmv);
    return cascade::core::formatText(cascade::i18n::tr("fitted %s"), b);
}

std::vector<ReachRow> reachRows(std::uint32_t caps) {
    struct Cap { std::uint32_t bit; const char* key; const char* detail; bool outward; };
    static const Cap kCaps[] = {
        {CASCADE_CAP_DECODER, FOX_TR_NOOP("TEXT DECODER"), FOX_TR_NOOP("Takes the demodulated audio and writes lines into the Decoder output."), false},
        {CASCADE_CAP_IQ_DECODER, FOX_TR_NOOP("I/Q DECODER"), FOX_TR_NOOP("Takes the raw band from the radio and decodes it itself."), false},
        {CASCADE_CAP_IMAGE_DECODER, FOX_TR_NOOP("PICTURE DECODER"), FOX_TR_NOOP("Produces pictures the host shows in a window of its own."), false},
        {CASCADE_CAP_TRACK_SOURCE, FOX_TR_NOOP("TRACK SOURCE"), FOX_TR_NOOP("Reports positions the host draws on a map page."), false},
        {CASCADE_CAP_PANEL, FOX_TR_NOOP("PANEL"), FOX_TR_NOOP("Publishes rows the host draws as a table of its own."), false},
        {CASCADE_CAP_HOST_CLIENT, FOX_TR_NOOP("HOST CLIENT"), FOX_TR_NOOP("Reads the receiver and may ask to tune it (the receiver-control grant decides)."), true},
        {CASCADE_CAP_PRESET, FOX_TR_NOOP("PRESETS"), FOX_TR_NOOP("Offers the frequencies it works on as keys."), false},
        {CASCADE_CAP_BASEMAP, FOX_TR_NOOP("BASEMAP"), FOX_TR_NOOP("Supplies map tiles under every map page, with their attribution."), true},
        {CASCADE_CAP_TRACK_INFO, FOX_TR_NOOP("TRACK INFO"), FOX_TR_NOOP("Looks up who a target is (registration, type, operator)."), true},
        {CASCADE_CAP_INSTRUMENT, FOX_TR_NOOP("INSTRUMENT"), FOX_TR_NOOP("Feeds a face the host draws as equipment."), false},
        {CASCADE_CAP_AUDIO_OUT, FOX_TR_NOOP("AUDIO OUT"), FOX_TR_NOOP("Takes the receiver's audio."), true},
        {CASCADE_CAP_AUDIO_PROCESSOR, FOX_TR_NOOP("AUDIO PROCESSOR"), FOX_TR_NOOP("Sits in the audio chain and changes what you hear."), true},
        {CASCADE_CAP_SETTINGS_UI, FOX_TR_NOOP("SETTINGS FORM"), FOX_TR_NOOP("Declares fields the host draws and stores for it."), false},
        {CASCADE_CAP_RECEIVER_LOCATOR, FOX_TR_NOOP("RECEIVER LOCATOR"), FOX_TR_NOOP("May read the receiver position you set."), true},
        {CASCADE_CAP_TX_AUDIO, FOX_TR_NOOP("TRANSMIT AUDIO"), FOX_TR_NOOP("Hands audio to the transmitter under an operator-armed grant."), true},
        {CASCADE_CAP_IMAGE_INPUT, FOX_TR_NOOP("PICTURE INPUT"), FOX_TR_NOOP("Takes a click on its own picture."), false},
    };
    std::vector<ReachRow> out;
    for (const Cap& c : kCaps) { if ((caps & c.bit) != 0u) { out.push_back({cascade::i18n::tr(c.key), cascade::i18n::tr(c.detail), c.outward}); } }
    return out;
}

bool capsOfferKeepRunning(std::uint32_t caps) {
    // core/plugin_run.cpp: a lifecycle = any decoder, track source, panel, instrument, basemap, track info or host client; a standing duty = an
    // in-chain audio processor, or a host client with nothing else to wake it by.
    const bool lifecycle = (caps & (kSignalCaps | CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_BASEMAP |
                                    CASCADE_CAP_TRACK_INFO | CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_AUDIO_OUT)) != 0u;
    const bool standing = (caps & CASCADE_CAP_AUDIO_PROCESSOR) != 0u ||
                          ((caps & CASCADE_CAP_HOST_CLIENT) != 0u &&
                           (caps & (kSignalCaps | CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) == 0u);
    return lifecycle && !standing;
}

std::string txPlateSentence(std::uint32_t bits, bool armed) {
    const bool asked = (bits & kTxBitAsked) != 0u, granted = (bits & kTxBitGranted) != 0u;
    if ((bits & kTxBitStopped) != 0u) { return cascade::i18n::tr("Its transmit audio stopped for good: it reported a permanent error."); }
    if ((bits & kTxBitOnAir) != 0u) { return cascade::i18n::tr("ON AIR: the transmitter is keyed for it."); }
    if ((bits & kTxBitWaiting) != 0u) { return cascade::i18n::tr("Allowed and waiting: another plugin holds the transmitter."); }
    if (granted) { return armed ? cascade::i18n::tr("ARMED: it will be keyed when it asks, until the window closes.") : cascade::i18n::tr("Allowed to transmit, but you have not armed plugins on the TRANSMIT page, so it cannot."); }
    if (asked) { return cascade::i18n::tr("It has asked to transmit and is not allowed to."); }
    (void)kTxBitDeclared; (void)kTxBitHolder;
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::string imageChip(const cascade::core::HostImage* img, bool asleep) {
    if (img == nullptr || img->width == 0u) { return asleep ? cascade::i18n::tr("IDLE") : cascade::i18n::tr("WAIT"); }
    return img->complete ? cascade::i18n::tr("IMG") : "RX";   // not-translated: RX is radio vocabulary
}
std::string panelChip(const cascade::core::HostPanel* p) {
    if (p == nullptr) { return cascade::i18n::tr("IDLE"); }
    return cascade::core::formatText(cascade::i18n::tr("%d ROW"), static_cast<int>(p->rows.size()));
}
std::string instrumentChip(const cascade::core::HostInstrument* in, bool unread) {
    if (in == nullptr) { return cascade::i18n::tr("IDLE"); }
    char out[24] = {0};
    if (unread) { return cascade::i18n::tr("NEW"); }
    if (!in->have) { return cascade::i18n::tr("WAIT"); }
    const CascadeInstrumentState& s = in->state;
    switch (in->kind) {
        case CASCADE_INSTRUMENT_PAGER: return cascade::core::formatText(cascade::i18n::tr("%lld MSG"), static_cast<long long>(std::llround(s.values[0])));
        case CASCADE_INSTRUMENT_TELEPRINTER: cascade::gui::teleprinter::chipWord(s.text[0], s.values[0], out, sizeof out); return out;
        case CASCADE_INSTRUMENT_TONE_ALERT:
            return (s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u ? std::string(cascade::i18n::tr("ALERT")) : cascade::core::formatText(cascade::i18n::tr("%lld LOG"), static_cast<long long>(in->rows.size()));
        case CASCADE_INSTRUMENT_BEACON:
            cascade::gui::beacon::chipWord((s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u, s.values[1], static_cast<int>(in->rows.size()), out, sizeof out);
            return out;
        case CASCADE_INSTRUMENT_NAV_BEARING:
            return (s.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u ? fmtN("%03lld", std::llround(s.values[0]) % 360) : std::string(cascade::i18n::tr("NAV"));
        case CASCADE_INSTRUMENT_FAX: return s.text[0][0] != '\0' ? std::string(s.text[0]) : "FAX";
        case CASCADE_INSTRUMENT_METER: cascade::gui::meter::chipWord(in->have, in->rows.size(), out, sizeof out); return out;
        case CASCADE_INSTRUMENT_WEATHER_CONSOLE: cascade::gui::wxface::chipWord(s.values[6], s.values, out, sizeof out); return out;
        default: return "RX";
    }
}
std::string fittedChipWord(std::size_t fed, std::size_t decoders) { return fmtN("%lld/", static_cast<long long>(fed)) + fmtN("%lld", static_cast<long long>(decoders)); }
std::string decodersChipWord(std::size_t fed) { return cascade::core::formatText(cascade::i18n::tr("%zu FED"), fed); }
std::string targetsChipWord(std::size_t visible) { return cascade::core::formatText(cascade::i18n::tr("%d TGT"), static_cast<int>(visible)); }

// ---------------------------------------------------------------------------------------------------------------------------------
std::string windowKey(const char* kind, const std::string& plugin, const std::string& title) {
    std::string k = kind;
    k += ':';
    k += plugin;
    if (!title.empty()) { k += '|'; k += title; }
    return k;
}
bool windowBelongsTo(const std::string& key, const std::string& plugin) {
    const std::size_t colon = key.find(':');
    if (colon == std::string::npos) { return false; }
    std::size_t end = key.find('|', colon + 1);
    if (end == std::string::npos) { end = key.size(); }
    return end - colon - 1 == plugin.size() && key.compare(colon + 1, plugin.size(), plugin) == 0;
}

std::string pluginUseText(const std::vector<PluginRow>& rows, const OpenWindows& open) {
    constexpr std::uint32_t kWindow = 1u, kMapPage = 2u, kRadarScope = 4u, kDecoderOutput = 8u;   // FOXAPP_PLUGIN_USE_*
    std::string text;
    for (const PluginRow& r : rows) {
        if (!r.loaded || r.file.empty()) { continue; }
        std::uint32_t bits = 0u;
        for (const std::string& k : open.windows) { if (windowBelongsTo(k, r.name)) { bits |= kWindow; break; } }
        if ((r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u && open.mapPages.count(r.name) > 0) { bits |= kMapPage; }
        if ((r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u && open.radarScope) { bits |= kRadarScope; }   // while the scope shows, every track source is in use (app_window_plugin_use.cpp 68)
        if (open.decoderOutput && (r.caps & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER)) != 0u) { bits |= kDecoderOutput; }
        if (bits == 0u) { continue; }
        text += r.file;
        text += '\t';
        text += fmtN("%lld", bits);
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------------------------------------------------------------------------
bool presetLit(const Preset& p, double deviceCentreHz, double tunedHz) {
    if (!(p.hz > 0.0)) { return false; }
    const double f = p.deviceCentre ? deviceCentreHz : tunedHz;
    double tol = kPresetToleranceHz;
    if (p.bwHz > 0.0 && 0.5 * p.bwHz > tol) { tol = 0.5 * p.bwHz; }
    return std::fabs(f - p.hz) <= tol;
}

std::string presetTooltip(const PluginRow& r, const Preset& p, bool lit) {
    if (p.mine) {
        return lit ? cascade::core::formatText(cascade::i18n::tr("Already tuned to your preset for %s. Right-click to forget it."), r.name.c_str())
                   : cascade::core::formatText(cascade::i18n::tr("Tune to your preset for %s. Right-click to forget it."), r.name.c_str());
    }
    if (lit) { return cascade::core::formatText(cascade::i18n::tr("Already tuned to this preset of %s."), r.name.c_str()); }
    const double mhz = p.hz / 1.0e6;
    if ((r.caps & (CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PANEL)) != 0u) { return cascade::core::formatText(cascade::i18n::tr("Tune to %.4f MHz, start this plugin and open its windows"), mhz); }
    if ((r.caps & CASCADE_CAP_DECODER) != 0u) { return cascade::core::formatText(cascade::i18n::tr("Tune to %.4f MHz, start this plugin and open the Decoder output window"), mhz); }
    return cascade::core::formatText(cascade::i18n::tr("Tune to %.4f MHz and start this plugin"), mhz);
}

std::string presetSaveLabel(double tunedHz) { return cascade::core::formatText(cascade::i18n::tr("+ Save %.4f MHz as a preset"), tunedHz / 1.0e6); }

// ---------------------------------------------------------------------------------------------------------------------------------
Verdict verdictFromAnswer(int status, double applied0) {
    if (status == FOXAPI_OK || status == FOXAPI_NO_CHANGE) { return Verdict::Accepted; }
    switch (static_cast<int>(applied0)) {
        case 1: return Verdict::TooLong;
        case 2: return Verdict::NotANumber;
        case 3: return Verdict::BadText;
        case 4: return Verdict::StoreFull;
        case 5: return Verdict::StoreError;
        case 6: return Verdict::Refused;
        default: return Verdict::StoreError;
    }
}

const char* verdictSentence(Verdict v) {
    switch (v) {
        case Verdict::Accepted: return "";
        case Verdict::TooLong: return cascade::i18n::tr("Not saved: longer than this field allows.");
        case Verdict::NotANumber: return cascade::i18n::tr("Not saved: that is not a number.");
        case Verdict::BadText: return cascade::i18n::tr("Not saved: it holds a character a setting cannot.");
        case Verdict::StoreFull: return cascade::i18n::tr("Not saved: the plugin's settings are full.");
        case Verdict::StoreError: return cascade::i18n::tr("Not saved: the store refused it.");
        case Verdict::Refused: return cascade::i18n::tr("Not saved: this field is not one the host can store.");
    }
    return "";
}

void SettingsEdits::sent(const std::string& file, const std::string& key, std::uint64_t ticket, const std::string& expect) {
    Edit& e = edits_[keyOf(file, key)];
    e.pending = true;
    e.ticket = ticket;
    e.expect = expect;
    e.hold = 0;
    e.last = Verdict::Accepted;
}
void SettingsEdits::refusedAtOnce(const std::string& file, const std::string& key) {
    Edit& e = edits_[keyOf(file, key)];
    e.pending = false;
    e.ticket = 0;
    e.hold = 0;
    e.last = Verdict::StoreError;
}
bool SettingsEdits::deliver(std::uint64_t ticket, Verdict verdict) {
    bool any = false;
    for (auto& kv : edits_) {
        Edit& e = kv.second;
        if (!e.pending || e.ticket != ticket) { continue; }
        e.pending = false;
        e.ticket = 0;
        e.last = verdict;
        e.hold = verdict == Verdict::Accepted ? kSettingHoldTicks : 0;
        any = true;
    }
    return any;
}
void SettingsEdits::tick() {
    for (auto& kv : edits_) { if (kv.second.hold > 0) { --kv.second.hold; } }
}
std::string SettingsEdits::shown(const std::string& file, const std::string& key, const std::string& storeValue) const {
    const auto it = edits_.find(keyOf(file, key));
    if (it == edits_.end()) { return storeValue; }
    const Edit& e = it->second;
    if (e.pending || e.hold > 0) { return e.expect; }
    return storeValue;
}
bool SettingsEdits::pending(const std::string& file, const std::string& key) const {
    const auto it = edits_.find(keyOf(file, key));
    return it != edits_.end() && it->second.pending;
}
std::string SettingsEdits::error(const std::string& file, const std::string& key) const {
    const auto it = edits_.find(keyOf(file, key));
    if (it == edits_.end() || it->second.pending) { return std::string(); }
    return verdictSentence(it->second.last);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void syncMapPages(std::vector<MapPage>& pages, const std::vector<std::string>& names, const std::vector<PluginRow>& rows) {
    pages.erase(std::remove_if(pages.begin(), pages.end(), [&](const MapPage& p) { return std::find(names.begin(), names.end(), p.plugin) == names.end(); }), pages.end());
    for (const std::string& n : names) {
        if (findMapPage(pages, n) != nullptr) { continue; }
        MapPage p;
        p.plugin = n;
        p.file = fileForName(rows, n);
        pages.push_back(std::move(p));
    }
    for (MapPage& p : pages) { if (p.file.empty()) { p.file = fileForName(rows, p.plugin); } }
}
MapPage* findMapPage(std::vector<MapPage>& pages, const std::string& plugin) {
    for (MapPage& p : pages) { if (p.plugin == plugin) { return &p; } }
    return nullptr;
}

bool fitView(const std::vector<cascade::core::HostTrack>& tracks, double& centreLat, double& centreLon, double& spanDeg, double aspect) {
    double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
    bool any = false;
    for (const auto& t : tracks) {
        if (!cascade::core::trackHasPosition(t.t)) { continue; }
        any = true;
        latMin = std::min(latMin, t.t.latDeg); latMax = std::max(latMax, t.t.latDeg);
        lonMin = std::min(lonMin, t.t.lonDeg); lonMax = std::max(lonMax, t.t.lonDeg);
    }
    if (!any) { return false; }
    centreLat = 0.5 * (latMin + latMax);
    centreLon = 0.5 * (lonMin + lonMax);
    const double lonSpan = (lonMax - lonMin) * 1.3 + 0.1;
    const double latSpan = (latMax - latMin) * 1.3 + 0.1;
    spanDeg = std::clamp(std::max(lonSpan, aspect > 0 ? latSpan * aspect : latSpan), 0.02, 360.0);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::string imageTitle(const std::string& plugin) { return cascade::core::formatText(cascade::i18n::tr("%s image"), plugin.c_str()); }
std::string mapTitle(const std::string& plugin) { return cascade::core::formatText(cascade::i18n::tr("%s map"), plugin.c_str()); }

std::string countText(std::size_t shown, std::size_t total, std::size_t unplaced) {
    // the ImGui window's six sentences, whole, so each translates as a sentence (a plural is its own key)
    const int sh = static_cast<int>(shown), to = static_cast<int>(total), un = static_cast<int>(unplaced);
    if (shown < total) {
        return unplaced > 0 ? cascade::core::formatText(cascade::i18n::tr("%d of %d targets, %d heard without a position"), sh, to, un)
                            : cascade::core::formatText(cascade::i18n::tr("%d of %d targets"), sh, to);
    }
    if (total == 1) {
        return unplaced > 0 ? cascade::core::formatText(cascade::i18n::tr("%d target, %d heard without a position"), to, un)
                            : cascade::core::formatText(cascade::i18n::tr("%d target"), to);
    }
    return unplaced > 0 ? cascade::core::formatText(cascade::i18n::tr("%d targets, %d heard without a position"), to, un)
                        : cascade::core::formatText(cascade::i18n::tr("%d targets"), to);
}

}  // namespace foxsdr::slintkit
