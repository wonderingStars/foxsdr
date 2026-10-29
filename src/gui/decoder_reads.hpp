// decoder_reads.hpp - WHAT THE DECODERS SECTION, THE DECODER OUTPUT WINDOW, THE PRESET BARS, THE MARKS ON
// THE SPECTRUM, THE MUTE BANNER AND DIALOG AND THE RECEIVER-CONTROL ROWS READ OF THE ENGINE'S TABLE (window
// port, group 8; docs/engine-stage4.md 6.5, docs/api-0.3-additions.md section 8.7).
//
// These sections used to ask the engine's members: the plugin host's records, the runner's status lines and
// its feeding test, the mute snapshot (muteStates_), the mute state and the popup's subject, the grants, the
// marks and the decoder log. They draw from the lists the window reads when listSeq moves (PLUGINS,
// DECODERS, PRESETS, USER_PRESETS, MARKERS, MUTE_CAUSES, STALE_GRANTS) and the facts, and this file holds
// the rules that turn those entries into what the members were handed before - as pure functions of plain
// rows, with no engine and no ImGui, so tests/test_decoders_mute_grants.cpp can hold each against the
// engine's own answer for the same plugins.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DECODER_READS_HPP
#define CASCADE_GUI_DECODER_READS_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"
#include "core/running_view.hpp"
#include "gui/module_census.hpp"

namespace cascade::gui {

// ONE PLUGIN AS THE WINDOW READS IT: the PLUGINS entry, merged with its DECODERS entry (a decoder), its PRESETS
// and the operator's own (USER_PRESETS). Rows are in the host's order. Only a LOADED plugin has presets and a
// DECODERS entry; a refused file is a row with a name, a file and its capability word.
struct PluginRow {
    std::string name;     // the display name (the file name when the plugin has none): PLUGINS name
    std::string file;     // the module file name, the key every op and every grant takes: PLUGINS detail
    std::string userKey;  // core::userPresetKey(file): what USER_PRESETS files this plugin's saved presets under
    std::uint32_t caps = 0;  // the capability word as declared: PLUGINS value[0], CASCADE_CAP_*
    bool loaded = false;     // PLUGINS ACTIVE
    bool stopped = false;    // PLUGINS STOPPED: a loaded plugin the user stopped
    // HOW IT RUNS (PLUGINS value[3], stub campaign B2): 0 AUTO, 1 ALWAYS (pinned), 2 STOPPED; and whether it is ASLEEP - AUTO and nothing is
    // using it, so it has no instance. Asleep is not stopped and not failing: it costs nothing and wakes by itself.
    int runMode = 0;
    bool asleep = false;
    bool tuneAllowed = false;      // PLUGINS TUNE_ALLOWED
    bool settingsAllowed = false;  // PLUGINS SETTINGS_ALLOWED
    // A DECODERS entry (every loaded decoder has one; the display name is unique among loaded plugins):
    bool decoder = false;
    bool fed = false;     // DECODERS FED: fed signal now (false with the receiver stopped)
    bool mutes = false;   // DECODERS MUTES: "mute audio while running" is on
    std::string idle;     // DECODERS detail: why the decoder is idle; "" running
    // WHAT THE DECODE BANK GROUPS AND THE WINDOW ROWS NAME BY (stub campaign B5; fact.plugin.<file>.record and .windows, API 0.3 9.5): the
    // catalogue's category of the plugin the install record names ("" with no record or no catalogue entry - the bank then has no heading for
    // it), the run id (the file name without its version: what scopes the bank's widget ids, so two files of one plugin are one section), and
    // the titles of the module's own panel and instrument ("" when it has none; the table's own, so an ASLEEP module's window can be named).
    std::string category;
    std::string runId;
    std::string panelTitle;
    std::string instrumentTitle;
    // The preset keys this plugin offers, as the mute snapshot held them: the plugin's own (raw index, bounded
    // label, the bandwidth it declared), then the operator's (1000 + ordinal). `ownPresets` of them are the
    // plugin's.
    std::vector<cascade::core::MutePreset> presets;
    std::size_t ownPresets = 0;
};

// One plugin muting the audio now (MUTE_CAUSES): its display name and the module file the Stop key acts on.
struct MuteCauseRow {
    std::string name;
    std::string key;
};

// A tune grant no loaded module holds (STALE_GRANTS): the module file, and whether the host holds a record of it
// that did not load (the file is on the disk and was refused) or none at all.
struct StaleGrantRow {
    std::string file;
    bool refused = false;
};

// --- the two per-file facts of stub campaign B5 (API 0.3 9.5) ----------------------------------------------

// fact.plugin.<file>.record: "<catalogue key> TAB <abi version> TAB <installed at, unix seconds> TAB <category>" - the install record that names the
// file ("" when none does). The key is "" when the record names no id. Written into `out`, whose strings keep their room.
struct PluginRecordFact {
    bool have = false;            // a record names the file
    std::string key;              // the catalogue key (the id; "<id>@<site label>" for a plugin from a site); "" with no id
    std::uint32_t abiVersion = 0;
    std::int64_t installedAtUnix = 0;
    std::string category;         // the catalogue entry's category; "" when the catalogue does not hold it
};

inline void parsePluginRecord(const std::string& text, PluginRecordFact& out) {
    out.have = !text.empty();
    out.key.clear();
    out.abiVersion = 0;
    out.installedAtUnix = 0;
    out.category.clear();
    if (!out.have) { return; }
    const std::size_t t1 = text.find('\t');
    if (t1 == std::string::npos) { return; }
    out.key.assign(text, 0, t1);
    const std::size_t t2 = text.find('\t', t1 + 1);
    if (t2 == std::string::npos) { return; }
    out.abiVersion = static_cast<std::uint32_t>(std::strtoul(text.c_str() + t1 + 1, nullptr, 10));
    const std::size_t t3 = text.find('\t', t2 + 1);
    if (t3 == std::string::npos) { return; }
    out.installedAtUnix = static_cast<std::int64_t>(std::strtoll(text.c_str() + t2 + 1, nullptr, 10));
    out.category.assign(text, t3 + 1, std::string::npos);
}

// Just the category of a record, for the rail (which needs nothing else of it).
inline void pluginRecordCategory(const std::string& text, std::string& category) {
    category.clear();
    std::size_t at = 0;
    for (int tab = 0; tab < 3; ++tab) {
        at = text.find('\t', at);
        if (at == std::string::npos) { return; }
        ++at;
    }
    category.assign(text, at, std::string::npos);
}

// fact.plugin.<file>.windows: "panel TAB <title>" and "instrument TAB <title>", a line each; "" for a module with none.
inline void pluginWindowTitles(const std::string& text, std::string& panelTitle, std::string& instrumentTitle) {
    panelTitle.clear();
    instrumentTitle.clear();
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) { end = text.size(); }
        const std::size_t tab = text.find('\t', at);
        if (tab != std::string::npos && tab < end) {
            if (text.compare(at, tab - at, "panel") == 0) {
                panelTitle.assign(text, tab + 1, end - tab - 1);
            } else if (text.compare(at, tab - at, "instrument") == 0) {
                instrumentTitle.assign(text, tab + 1, end - tab - 1);
            }
        }
        at = end + 1;
    }
}

// --- the conversions from the table's entries to what the members were handed ------------------------------

// A plugin's own preset, as Engine::rebuildMuteStates made it from the plugin's table: the label or, when the
// plugin left it empty, the plugin's name (core presetLabel); the bandwidth only when it is a bandwidth
// (0 < Hz < 1e12; most presets declare none); whether it moves the DEVICE centre; the RAW index get() was called
// with.
inline cascade::core::MutePreset ownPresetOf(double frequencyHz, double bandwidthHz, bool deviceCentre,
                                             std::uint32_t rawIndex, const char* label, const std::string& pluginName) {
    cascade::core::MutePreset mp;
    mp.frequencyHz = frequencyHz;
    mp.bandwidthHz = (bandwidthHz > 0.0 && bandwidthHz < 1e12) ? bandwidthHz : 0.0;
    mp.deviceCentre = deviceCentre;
    mp.index = rawIndex;
    mp.label = label[0] != '\0' ? std::string(label) : pluginName;
    return mp;
}

// One of the operator's own, as userPresetToMutePreset made it: an ABSOLUTE tuned frequency (never the device
// centre), recorded at 1000 + its ordinal among that plugin's saved presets.
inline cascade::core::MutePreset savedPresetOf(double frequencyHz, double bandwidthHz, std::size_t ordinal,
                                               const char* label) {
    cascade::core::MutePreset mp;
    mp.frequencyHz = frequencyHz;
    mp.bandwidthHz = bandwidthHz;
    mp.deviceCentre = false;
    mp.index = 1000u + static_cast<std::uint32_t>(ordinal);
    mp.label = label;
    return mp;
}

// A mark on the spectrum, as the plugin set it (the plugin ABI's CascadeMarker, which pluginMarkerGeometry
// takes): the list serves the id, the label, the frequency, the width and the colour, and three item flags
// for the kind and the two drawing flags.
inline CascadeMarker markerOf(std::uint64_t id, const char* label, double freqHz, double widthHz, std::uint32_t colourRgba,
                              bool span, bool dashed, bool spectrumOnly) {
    CascadeMarker m{};
    m.structSize = static_cast<std::uint32_t>(sizeof(m));
    m.id = static_cast<std::uint32_t>(id);
    m.kind = span ? CASCADE_MARKER_SPAN : CASCADE_MARKER_POINT;
    m.flags = (dashed ? CASCADE_MARKER_FLAG_DASHED : 0u) | (spectrumOnly ? CASCADE_MARKER_FLAG_SPECTRUM_ONLY : 0u);
    m.freqHz = freqHz;
    m.widthHz = widthHz;
    m.colourRgba = colourRgba;
    std::size_t n = 0;
    while (label[n] != '\0' && n + 1 < sizeof(m.label)) {
        m.label[n] = label[n];
        ++n;
    }
    m.label[n] = '\0';
    return m;
}

// --- what the decoders section judges ------------------------------------------------------------------------

// The decoders the rail's STOP keys are about, as Engine::runnableDecoders built them: every LOADED decoder, in
// the host's order, with whether the user stopped it and whether it is fed. (`feeding` is the table's FED, which
// already includes the receiver running; decoderIsRunning asks for it again, harmlessly.)
inline std::vector<RunnableDecoder> runnableOf(const std::vector<PluginRow>& rows) {
    std::vector<RunnableDecoder> out;
    for (const PluginRow& r : rows) {
        if (!r.loaded || !r.decoder) { continue; }
        RunnableDecoder d;
        d.name = r.name;
        d.key = r.file;
        d.stopped = r.stopped;
        d.feeding = r.fed;
        d.idle = r.asleep;
        out.push_back(std::move(d));
    }
    return out;
}

// How many decoders are running (running_view.hpp's decoderIsRunning), with no allocation: the section asks
// every frame it is open.
inline int runningDecoderCount(const std::vector<PluginRow>& rows, bool receiverRunning) {
    int n = 0;
    for (const PluginRow& r : rows) {
        if (!r.loaded || !r.decoder) { continue; }
        RunnableDecoder d;
        d.stopped = r.stopped;
        d.feeding = r.fed;
        if (decoderIsRunning(d, receiverRunning)) { ++n; }
    }
    return n;
}

// The keys STOP ALL names (stopAllKeys over runnableOf), in the host's order.
inline std::vector<std::string> runningDecoderKeys(const std::vector<PluginRow>& rows, bool receiverRunning) {
    return stopAllKeys(runnableOf(rows), receiverRunning);
}

// Is the runner's status list empty (PluginRunner::status().empty())? The runner writes a line for EVERY loaded
// plugin that is not stopped - a decoder's, an audio processor's, or "provides no decoder this build can drive" -
// and none for a stopped or a refused one. So it is empty exactly when no loaded plugin is running.
inline bool runnerStatusEmpty(const std::vector<PluginRow>& rows) {
    for (const PluginRow& r : rows) {
        if (r.loaded && !r.stopped) { return false; }
    }
    return true;
}

// The module file a plugin's DISPLAY name stands for (Engine::pluginKeyForDisplayName over the table's rows: the first
// row of that name; "" for an empty name or none): what a map page's or a plugin window's rail row sends
// FOXAPP_OP_PLUGIN_AUTO_PRESET with, because the pages and windows carry the display name and the op takes the file.
inline std::string moduleFileForName(const std::vector<PluginRow>& rows, const std::string& displayName) {
    if (displayName.empty()) { return {}; }
    for (const PluginRow& r : rows) {
        if (r.name == displayName) { return r.file; }
    }
    return {};
}

// The census of the rows against one capability mask (module_census.hpp's rule, over the table's entries). A
// loaded module "provides" when it declared a bit of the mask: the host proves a declared bit has its table
// behind it before it lets a module load.
inline ModuleCensus censusOf(const std::vector<PluginRow>& rows, std::uint32_t capMask) {
    ModuleCensus c;
    for (const PluginRow& r : rows) {
        countModule(c, r.name, r.loaded, r.caps, r.loaded && (r.caps & capMask) != 0u, r.stopped, capMask);
    }
    return c;
}

// What a preset's tooltip promises, from the capability word (a loaded plugin's tables are its declared bits):
// the windows it will open - a map page, a picture or a panel - or, for a text decoder, the Decoder output window.
inline bool opensWindowOfItsOwn(const PluginRow& r) {
    return (r.caps & (CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PANEL)) != 0u;
}
inline bool isTextDecoder(const PluginRow& r) { return (r.caps & CASCADE_CAP_DECODER) != 0u; }

// The names, joined as the banner and the dialog say them ("A", "A and B", "A, B and C"): core's one place,
// over the causes' names.
inline std::vector<std::string> namesOf(const std::vector<MuteCauseRow>& causes) {
    std::vector<std::string> out;
    out.reserve(causes.size());
    for (const MuteCauseRow& c : causes) { out.push_back(c.name); }
    return out;
}

// The keys the Stop key acts on, joined by newlines as FOXAPP_OP_DECODER_STOP_LIST takes them.
inline std::string joinedKeys(const std::vector<std::string>& keys) {
    std::string out;
    for (const std::string& k : keys) { out += (out.empty() ? "" : "\n") + k; }
    return out;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DECODER_READS_HPP
