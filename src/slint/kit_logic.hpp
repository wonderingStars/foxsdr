// kit_logic.hpp - THE PLUGIN KIT'S PURE LOGIC (docs/engine-stage4.md 6.37): what the DECODE bank, the plugin pages and the
// Fitted modules page show is computed from the C API table's lists and facts by these functions, and the rules the ImGui
// window applies to them (the fitted state's precedence, the chip words, the plugin-use table, the preset-lit test, the
// settings form's pending/ticket/hold machine, the map pages' book-keeping) are stated here once, as plain functions over
// plain data. No Slint type and no engine header appears here - foxsdr_api.h, the plugin ABI and the two core data headers
// the plugin-UI port hands out (plugin_ui_data.hpp, host_image.hpp) - so the file is tested without a window or an engine
// (tests/test_slint_host_kit.cpp reads a stub table).
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour, never for drawing): the list reader is
// AppWindow::pluginLists (app_window.cpp), the fitted facts AppWindow::fittedLists (app_window_fitted.cpp), the states and
// sentences plugins_view.cpp:57-170, the plugin-use table app_window_plugin_use.cpp, the chips app_window.cpp:15217-15560 and
// instrument_face.cpp:139, the preset test core/plugin_ui.cpp onPreset, the form machine gui/plugin_settings_form.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"

namespace foxsdr::slintkit {

// ---- the table, as the kit reads it (the live one is the C API; a test's is a stub) ----
class TableReader {
public:
    virtual ~TableReader() = default;
    // read_list's contract: the count (>= 0) and `out` filled when index < count; negative = refused.
    virtual std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) = 0;
    // get_setting of a "fact.*" key: false when the engine does not hold it (out empty).
    virtual bool fact(const std::string& key, std::string& out) = 0;
};

// ---- one plugin as the window reads it: PLUGINS, merged with DECODERS, PRESETS, USER_PRESETS and the per-file facts ----
struct Preset {
    double hz = 0.0;
    double bwHz = 0.0;          // 0 = the preset declares none
    bool deviceCentre = false;  // CASCADE_PRESET_DEVICE_CENTRE: compared against the device centre, not the VFO
    bool mine = false;          // the operator's own (USER_PRESETS): index is 1000 + its ordinal
    std::uint32_t index = 0;    // the RAW index FOXAPI_OP_PLUGIN_PRESET takes (a plugin's), or 1000 + ordinal (mine)
    std::string label;
};

struct PluginRow {
    std::string name;      // the display name (PLUGINS name)
    std::string file;      // the module file, the key every op takes (PLUGINS detail)
    std::string runId;     // the file without its version
    std::uint32_t caps = 0;
    bool loaded = false, stopped = false, asleep = false, switchedOff = false;
    bool tuneAllowed = false, settingsAllowed = false, matched = false, askedSettings = false;
    int runMode = 0;       // 0 AUTO 1 ALWAYS 2 STOPPED
    std::uint64_t fileBytes = 0;
    std::uint32_t noticeLevel = 0;
    // a DECODERS entry (every loaded decoder has one)
    bool decoder = false, fed = false, mutes = false;
    std::string idle;      // DECODERS detail: why it is idle, "" running
    // two facts of a loaded module: the catalogue category and the titles of its own windows
    std::string category, panelTitle, instrumentTitle;
    std::vector<Preset> presets;   // the plugin's own first, then the operator's
    std::size_t ownPresets = 0;
    // THE FITTED PAGE'S FACTS (read only while that page shows: ReadOptions::fittedFacts)
    std::string path, version, author, licence, error, idleDetail, fault, notice, integrity, originWord, originSite;
    bool hasRecord = false;
    std::string catalogueKey;
    std::uint32_t abiVersion = 0;
    std::int64_t fittedAtUnix = 0;
    std::uint32_t txBits = 0;
    std::vector<cascade::core::HostCommand> commands;
};

struct MuteCause { std::string name, key; };
struct StaleGrant { std::string file; bool refused = false; };
struct SettingSpec {
    std::uint64_t id = 0;        // the PLUGIN_SETTINGS entry's number: fact.plugin.setting.<id>
    std::string key, label, placeholder, value;
    std::uint32_t kind = 0;      // CASCADE_SETTING_* (0 TEXT 1 NUMBER 2 BOOL 3 PATH; 4 would be PASSWORD when the core serves it)
    std::size_t maxLength = 0;
};
struct SettingsSet { std::string file; std::vector<SettingSpec> fields; };
struct BlockedRow { std::string file, why, id, message; };

struct KitLists {
    std::vector<PluginRow> rows;
    std::vector<MuteCause> muteCauses;
    std::vector<StaleGrant> staleGrants;
    std::vector<SettingsSet> settings;
    std::vector<BlockedRow> blocked;
    std::string lastDenied, muteSubject, pluginDir, catalogueUrl, enforceError, installReport, installError;
    bool muteKeptRunning = false;
    bool txArmed = false;
};
struct ReadOptions {
    bool fittedFacts = false;   // the Fitted page's eleven facts and PLUGIN_COMMANDS per module
    bool settings = false;      // PLUGIN_SETTINGS and the spec/value facts (a form is showing)
    bool blocked = false;       // STORE_BLOCKED and its two facts (the DECODE bank shows)
};
void readKitLists(TableReader& table, KitLists& out, const ReadOptions& options);
const PluginRow* rowByName(const std::vector<PluginRow>& rows, const std::string& displayName);   // the LOADED row of that name
const PluginRow* rowByFile(const std::vector<PluginRow>& rows, const std::string& file);
std::string fileForName(const std::vector<PluginRow>& rows, const std::string& displayName);     // "" when none is loaded
const SettingsSet* settingsFor(const std::vector<SettingsSet>& sets, const std::string& file);

// ---- the decoder output: the ring's tail, read when decoderLinesTotal moved ----
struct DecoderLine {
    std::uint64_t id = 0;
    std::string plugin, text;
    int tone = 0;   // 0 normal, 1 WARN, 2 ERROR (the log API's levels ride the line's first word)
};
int lineTone(const std::string& text);
// Appends every line whose id is above `lastId` (oldest first) and trims the ring to `cap`; returns how many came.
std::size_t readDecoderLinesTail(TableReader& table, std::uint64_t& lastId, std::deque<DecoderLine>& ring, std::size_t cap = 500);

// ---- the fitted state (plugins_view.cpp:57-170, the same precedence) ----
enum class FittedState { Fed, NotFed, NoSignal, Stopped, Refused, Idle };
FittedState fittedState(const PluginRow& r, bool receiverRunning);
const char* fittedStateWord(FittedState s);                         // FED, NOT DECODING, TAKES NO SIGNAL, STOPPED, REFUSED, IDLE
std::string fittedStateSentence(const PluginRow& r, bool receiverRunning);
bool fittedOffersStart(const PluginRow& r);                         // stopped or asleep: the key says START
int fittedStateIndex(FittedState s);                                // the chip order: FED, IDLE, NOT DECODING, TAKES NO SIGNAL, STOPPED, REFUSED
std::string fittedVerdictLine(bool receiverRunning, const std::string& directory);
std::string abiText(std::uint32_t abiVersion, std::uint32_t thisBuild);
std::string fittedDateText(std::int64_t unixSeconds);                // "fitted 2026-11-03", "" none
struct ReachRow { std::string key, detail; bool outward = false; };
std::vector<ReachRow> reachRows(std::uint32_t caps);                // one row per declared capability, in the ABI's order
bool capsOfferKeepRunning(std::uint32_t caps);                       // a lifecycle and no standing duty
std::string txPlateSentence(std::uint32_t txBits, bool armed);       // "", or the plate's sentence (transmit_page.hpp)

// ---- the chip words of the DECODE bank's rows ----
std::string imageChip(const cascade::core::HostImage* img, bool asleep);           // IDLE / WAIT / RX / IMG
std::string panelChip(const cascade::core::HostPanel* panel);                      // IDLE / "7 ROW"
std::string instrumentChip(const cascade::core::HostInstrument* in, bool unread);  // instrument_face.cpp:139
std::string fittedChipWord(std::size_t fed, std::size_t decoders);                 // "3/28"
std::string decodersChipWord(std::size_t fed);                                     // "2 FED"
std::string targetsChipWord(std::size_t visible);                                  // "14 TGT"

// ---- what the window shows of each plugin, told to the engine (FOXAPP_OP_PLUGIN_USE) ----
struct OpenWindows {
    std::set<std::string> windows;    // windowKey(...) of every shown picture / panel / instrument page
    std::set<std::string> mapPages;   // the display names of the open map pages
    bool decoderOutput = false;
    bool radarScope = false;          // the radar scope shows (the SCOPE tab): every track source is in use (docs 6.57; FOXAPP_PLUGIN_USE_RADAR_SCOPE)
};
std::string windowKey(const char* kind, const std::string& plugin, const std::string& title = std::string());  // "image:<name>", "panel:<name>", "instrument:<name>|<title>"
bool windowBelongsTo(const std::string& key, const std::string& plugin);
std::string pluginUseText(const std::vector<PluginRow>& rows, const OpenWindows& open);   // "" when nothing is shown

// ---- presets ----
bool presetLit(const Preset& p, double deviceCentreHz, double tunedHz);   // core/plugin_ui.cpp onPreset's rule
std::string presetTooltip(const PluginRow& r, const Preset& p, bool lit);
std::string presetSaveLabel(double tunedHz);                              // "+ Save 145.5000 MHz as a preset"

// ---- the settings form's machine: an edit sent, its ticket, the answer, the hold (gui/plugin_settings_form.hpp) ----
enum class Verdict { Accepted, TooLong, NotANumber, BadText, StoreFull, StoreError, Refused };
Verdict verdictFromAnswer(int status, double applied0);
const char* verdictSentence(Verdict v);   // "Not saved: ...", "" for Accepted
constexpr int kSettingHoldTicks = 30;
class SettingsEdits {
public:
    // An edit was sent: the box keeps `expect` until the answer comes.
    void sent(const std::string& file, const std::string& key, std::uint64_t ticket, const std::string& expect);
    // A send that could not be made: the box says the store refused it.
    void refusedAtOnce(const std::string& file, const std::string& key);
    // The engine's answer by ticket. False when no box waits for it.
    bool deliver(std::uint64_t ticket, Verdict verdict);
    // Once a tick: the hold of an accepted edit runs down.
    void tick();
    // What the box shows now: the pending or held text, else the store's value.
    std::string shown(const std::string& file, const std::string& key, const std::string& storeValue) const;
    bool pending(const std::string& file, const std::string& key) const;
    std::string error(const std::string& file, const std::string& key) const;   // the refusal sentence, "" none
    std::size_t size() const { return edits_.size(); }

private:
    struct Edit { bool pending = false; std::uint64_t ticket = 0; std::string expect; int hold = 0; Verdict last = Verdict::Accepted; };
    static std::string keyOf(const std::string& file, const std::string& key) { return file + '\x1F' + key; }
    std::map<std::string, Edit> edits_;
};

// ---- the map pages: one per track source the plugin UI instantiates, kept across polls ----
struct MapPage {
    std::string plugin;       // the display name (what HostTrack::plugin carries)
    std::string file;
    bool open = false;
    bool satellite = false;   // sticky: every track seen was a satellite
    std::size_t visibleCount = 0, unplacedCount = 0;
    double centreLat = 20.0, centreLon = 0.0, spanDeg = 180.0;   // the view; fitted to the tracks on the first ones
    bool viewFitted = false;
    std::string followedId, selectedId;
};
void syncMapPages(std::vector<MapPage>& pages, const std::vector<std::string>& trackPluginNames, const std::vector<PluginRow>& rows);
MapPage* findMapPage(std::vector<MapPage>& pages, const std::string& plugin);
// The view that holds every placed track (the Fit key): false when none has a position.
bool fitView(const std::vector<cascade::core::HostTrack>& tracks, double& centreLat, double& centreLon, double& spanDeg, double aspect);

// ---- the page titles and the rows' tooltips ----
std::string imageTitle(const std::string& plugin);          // "<plugin> image"
std::string mapTitle(const std::string& plugin);            // "<plugin> map"
std::string countText(std::size_t shown, std::size_t total, std::size_t unplaced);   // "14 targets, 2 heard without a position"

}  // namespace foxsdr::slintkit
