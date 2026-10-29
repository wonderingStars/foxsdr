// test_plugin_hostile.cpp - THE BREAK-IT OF THE PLUGIN CONTAINMENT (plugin containment P2, docs/engine-stage4.md 6.29): a hostile plugin module
// (tests/fixtures/hostile_plugin.cpp) misbehaves in every guarded entry point, one abuse at a time, and the containment of P1 is held to every
// one of its rules.
//
//   A  THE MATRIX. Every guarded entry point of core/plugin_calls.hpp (create and destroy through each of the seven capabilities that have
//      them; the polls, the hand-backs, the columns, the tile and info calls, attach, press, the preset table) x three abuses:
//        a FAULT   an access violation in the plugin (Windows: the structured exception the guard absorbs)
//        a THROW   a C++ exception across the C boundary (every platform)
//        a LOOP    a call that does not return within four of its budgets (every platform; the budget is stood down to 100 ms)
//      and for each: the process is alive; the plugin is switched off (a fault and a throw at once, a loop when the monitor finds it wedged);
//      the entry point is the one that did it; the counters of its id and version say so once (the totals, the health row, the events); the
//      anonymous ledger gets the right words and no others; a second call is refused without reaching the plugin; and the blame names the
//      plugin and the entry - the crash report of an absorbed fault (`blamed-plugin`, `blamed-plugin-entry`), the freeze report of a loop.
//      The fixture's referee (every rule the HOST owes a plugin) records no violation by the host through any of it.
//   B  THE LIES a plugin can tell in what it hands BACK - counts past the room given, wild and null pixel pointers, strings with no end,
//      NaN everywhere, a panel with 99 columns - none of which any static check can see: the host survives each, clamps or refuses it,
//      counts the ones that are provably out of bounds (Misuse::BadReturn) and never switches the plugin off for a lie it could handle.
//   C  LOAD AND UNLOAD. A descriptor that lies about itself (a wild name, a wild table array, a duplicated capability, a wild title, an
//      attribution that is garbage, a capability word with no table behind it), a fault in DllMain or in the descriptor query: refused with
//      a verdict, never mapped as a working plugin, the process alive. A fault in DllMain(PROCESS_DETACH) at unload is absorbed.
//   D  THE CONTROLS. A create() that fails honestly is not a fault and is not counted; a healthy plugin leaves every counter at zero.
//
// Where a scenario needs structured exceptions (a fault in the plugin's code, a wild pointer the host reads) it runs on Windows and SKIPs elsewhere
// with a message - the guard itself documents that POSIX runs a plugin unguarded. Everything else runs everywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/hang_watchdog.hpp"
#include "core/health_events.hpp"
#include "core/plugin_calls.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "core/telemetry.hpp"
#include "core/thread_beats.hpp"
#include "gui/basemap_cache.hpp"
#include "core/tune_control.hpp"
#include "gui/track_info_cache.hpp"
#include "hostile_support.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace fault = cascade::core::fault;
namespace calls = cascade::core::calls;
namespace beats = cascade::core::beats;
namespace health = cascade::core::health;
using cascade::core::HangWatchdog;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using hostile::countLines;
using hostile::readFile;
using hostile::setEnv;
using hostile::setMode;

namespace {

fs::path g_root;
std::string g_fixture;

// (PluginUi's own private bounds, restated: the figures a plugin must not be able to exceed in what the interface holds)
constexpr std::size_t kMaxTracksPerPlugin = 4000;
constexpr std::size_t kMaxRowsPerPanel = 2000;

// ---- the anonymous ledger, fed from the containment's totals exactly as the engine's pass feeds it (engine.cpp, the recovery readings) ------
health::RecoveryReadings readingsNow() {
    health::RecoveryReadings r;
    const fault::Totals t = fault::totals();
    r.pluginFaults = t.faults;
    r.pluginQuarantines = t.quarantines;
    r.pluginWedges = t.wedges;
    r.pluginOveruses = t.overruns;
    return r;
}

unsigned ledgerCount(const char* word) {
    const auto counts = health::globalLedger()->counts();
    const auto it = counts.find(std::string("recovered.") + word);
    return it == counts.end() ? 0u : it->second;
}

void armLedger() {
    auto ledger = health::globalLedger();
    ledger->reset();
    ledger->arm("", cascade::core::newInstallId(), false);
}

// ---- what one row of the matrix is ---------------------------------------------------------------------------------------------------------
struct Ctx {
    const LoadedPlugin* lp = nullptr;
    fault::Slot* s = nullptr;
    void* h = nullptr;
    CascadeImage img{};
    CascadeTile tile{};
    CascadeTrackInfo info{};
    CascadeHostApi host{};
};

struct Row {
    const char* label;
    fault::Entry entry;
    void (*arrange)(Ctx&);   // with the plugin behaving: what the call needs (a handle, a borrowed image)
    void (*act)(Ctx&);       // exactly ONE guarded call of `entry`
};

void none(Ctx&) {}

CascadeInstrumentState g_state;
CascadePanelRow g_rows[4];
CascadeTrack g_tracks[4];
CascadePath g_paths[4];
char g_headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS];
char g_text[256];
CascadePreset g_preset;

const Row kRows[] = {
    // ---- create, through each capability that has one
    {"create/decoder", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->decoder, 48000u); }},
    {"create/image", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->imageDecoder, 0.0, 0.0); }},
    {"create/tracks", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackSource); }},
    {"create/panel", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->panel); }},
    {"create/instrument", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->instrument); }},
    {"create/basemap", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->basemap); }},
    {"create/info", fault::Entry::Create, none, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackInfo); }},
    // ---- destroy
    {"destroy/decoder", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->decoder, 48000u); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->decoder, c.h); }},
    {"destroy/image", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->imageDecoder, 0.0, 0.0); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->imageDecoder, c.h); }},
    {"destroy/tracks", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackSource); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->trackSource, c.h); }},
    {"destroy/panel", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->panel); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->panel, c.h); }},
    {"destroy/instrument", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->instrument); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->instrument, c.h); }},
    {"destroy/basemap", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->basemap); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->basemap, c.h); }},
    {"destroy/info", fault::Entry::Destroy, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackInfo); },
     [](Ctx& c) { (void)calls::destroy(c.s, c.lp->trackInfo, c.h); }},
    // ---- poll-text, through the four capabilities that have one
    {"poll-text/decoder", fault::Entry::PollText, [](Ctx& c) { c.h = calls::create(c.s, c.lp->decoder, 48000u); },
     [](Ctx& c) { (void)calls::pollText(c.s, c.lp->decoder, c.h, g_text, sizeof(g_text)); }},
    {"poll-text/image", fault::Entry::PollText, [](Ctx& c) { c.h = calls::create(c.s, c.lp->imageDecoder, 0.0, 0.0); },
     [](Ctx& c) { (void)calls::pollText(c.s, c.lp->imageDecoder, c.h, g_text, sizeof(g_text)); }},
    {"poll-text/basemap", fault::Entry::PollText, [](Ctx& c) { c.h = calls::create(c.s, c.lp->basemap); },
     [](Ctx& c) { (void)calls::pollText(c.s, c.lp->basemap, c.h, g_text, sizeof(g_text)); }},
    {"poll-text/info", fault::Entry::PollText, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackInfo); },
     [](Ctx& c) { (void)calls::pollText(c.s, c.lp->trackInfo, c.h, g_text, sizeof(g_text)); }},
    // ---- the pictures
    {"poll-image", fault::Entry::PollImage, [](Ctx& c) { c.h = calls::create(c.s, c.lp->imageDecoder, 0.0, 0.0); },
     [](Ctx& c) {
         c.img = CascadeImage{};
         c.img.structSize = sizeof(CascadeImage);
         (void)calls::pollImage(c.s, c.lp->imageDecoder, c.h, &c.img);
     }},
    {"release-image", fault::Entry::ReleaseImage,
     [](Ctx& c) {
         c.h = calls::create(c.s, c.lp->imageDecoder, 0.0, 0.0);
         c.img = CascadeImage{};
         c.img.structSize = sizeof(CascadeImage);
         (void)calls::pollImage(c.s, c.lp->imageDecoder, c.h, &c.img);
     },
     [](Ctx& c) { calls::releaseImage(c.s, c.lp->imageDecoder, c.h, &c.img); }},
    // ---- the tracks
    {"poll-tracks", fault::Entry::PollTracks, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackSource); },
     [](Ctx& c) { (void)calls::pollTracks(c.s, c.lp->trackSource, c.h, g_tracks, 4); }},
    {"poll-paths", fault::Entry::PollPaths, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackSource); },
     [](Ctx& c) { (void)calls::pollPaths(c.s, c.lp->trackSource, c.h, g_paths, 4); }},
    // ---- the instrument and the panel
    {"poll-state", fault::Entry::PollState, [](Ctx& c) { c.h = calls::create(c.s, c.lp->instrument); },
     [](Ctx& c) {
         g_state = CascadeInstrumentState{};
         g_state.structSize = sizeof(CascadeInstrumentState);
         (void)calls::pollState(c.s, c.lp->instrument, c.h, &g_state);
     }},
    {"columns/panel", fault::Entry::Columns, [](Ctx& c) { c.h = calls::create(c.s, c.lp->panel); },
     [](Ctx& c) { (void)calls::columns(c.s, c.lp->panel, c.h, g_headings); }},
    {"columns/instrument", fault::Entry::Columns, [](Ctx& c) { c.h = calls::create(c.s, c.lp->instrument); },
     [](Ctx& c) { (void)calls::columns(c.s, c.lp->instrument, c.h, g_headings); }},
    {"poll-rows/panel", fault::Entry::PollRows, [](Ctx& c) { c.h = calls::create(c.s, c.lp->panel); },
     [](Ctx& c) { (void)calls::pollRows(c.s, c.lp->panel, c.h, g_rows, 4); }},
    {"poll-rows/instrument", fault::Entry::PollRows, [](Ctx& c) { c.h = calls::create(c.s, c.lp->instrument); },
     [](Ctx& c) { (void)calls::pollRows(c.s, c.lp->instrument, c.h, g_rows, 4); }},
    // ---- the basemap and the track-info client
    {"get-tile", fault::Entry::GetTile, [](Ctx& c) { c.h = calls::create(c.s, c.lp->basemap); },
     [](Ctx& c) {
         c.tile = CascadeTile{};
         c.tile.structSize = sizeof(CascadeTile);
         (void)calls::getTile(c.s, c.lp->basemap, c.h, 0, 0, 0, &c.tile);
     }},
    {"release-tile", fault::Entry::ReleaseTile,
     [](Ctx& c) {
         c.h = calls::create(c.s, c.lp->basemap);
         c.tile = CascadeTile{};
         c.tile.structSize = sizeof(CascadeTile);
         (void)calls::getTile(c.s, c.lp->basemap, c.h, 0, 0, 0, &c.tile);
     },
     [](Ctx& c) { calls::releaseTile(c.s, c.lp->basemap, c.h, &c.tile); }},
    {"get-info", fault::Entry::GetInfo, [](Ctx& c) { c.h = calls::create(c.s, c.lp->trackInfo); },
     [](Ctx& c) {
         c.info = CascadeTrackInfo{};
         c.info.structSize = sizeof(CascadeTrackInfo);
         (void)calls::getInfo(c.s, c.lp->trackInfo, c.h, "ABC123", CASCADE_TRACK_AIRCRAFT, &c.info);
     }},
    {"release-info", fault::Entry::ReleaseInfo,
     [](Ctx& c) {
         c.h = calls::create(c.s, c.lp->trackInfo);
         c.info = CascadeTrackInfo{};
         c.info.structSize = sizeof(CascadeTrackInfo);
         (void)calls::getInfo(c.s, c.lp->trackInfo, c.h, "ABC123", CASCADE_TRACK_AIRCRAFT, &c.info);
     },
     [](Ctx& c) { calls::releaseInfo(c.s, c.lp->trackInfo, c.h, &c.info); }},
    // ---- the small ones
    {"attach", fault::Entry::Attach, none, [](Ctx& c) { calls::attach(c.s, c.lp->hostClient, &c.host); }},
    {"press", fault::Entry::Press, none, [](Ctx& c) { calls::press(c.s, c.lp->imageInput, 3, 4); }},
    {"preset-count", fault::Entry::PresetCount, none, [](Ctx& c) { (void)calls::presetCount(c.s, c.lp->preset); }},
    {"preset-get", fault::Entry::PresetGet, none,
     [](Ctx& c) {
         g_preset = CascadePreset{};
         g_preset.structSize = sizeof(CascadePreset);
         (void)calls::presetGet(c.s, c.lp->preset, 0, &g_preset);
     }},
};
constexpr std::size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

enum class Abuse { Fault, Throw, Loop };
const char* abuseWord(Abuse a) { return a == Abuse::Fault ? "av" : a == Abuse::Throw ? "throw" : "spin"; }

const fault::HealthRow* healthOf(const std::vector<fault::HealthRow>& rows, const char* id) {
    for (const fault::HealthRow& r : rows) {
        if (r.id == id) { return &r; }
    }
    return nullptr;
}

std::size_t countMatching(const fs::path& dir, const char* prefix) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind(prefix, 0) == 0) { ++n; }
    }
    return n;
}

std::string newestWithPrefix(const fs::path& dir, const char* prefix) {
    std::string text;
    fs::file_time_type best{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind(prefix, 0) != 0) { continue; }
        const auto t = fs::last_write_time(e.path(), ec);
        if (text.empty() || t > best) {
            best = t;
            text = readFile(e.path());
        }
    }
    return text;
}

struct Matrix {
    PluginHost host;
    const LoadedPlugin* lp = nullptr;
    fs::path crashDir;
    fs::path hangDir;
    fs::path logFile;
    HangWatchdog watchdog;
    bool crashHandlers = false;
};

// One row, one abuse. Returns nothing: every claim is a CHECK, and the row's label is printed first so a failure names it.
void runRow(Matrix& m, const Row& row, Abuse abuse) {
    const char* word = fault::entryWord(row.entry);
    std::printf("    %-20s %-5s %s\n", row.label, abuseWord(abuse), word);
    fault::resetForTest();
    fault::dropBudgetOverridesForTest();
    (void)fault::drainEvents();
    armLedger();
    health::RecoveryWatch ledgerFeed;
    ledgerFeed.poll(readingsNow());   // the baseline: nothing yet

    Ctx c;
    c.lp = m.lp;
    c.s = fault::slotFor(m.lp->path);
    std::string violationsBefore;
    const std::size_t violBefore = hostile::violations(*m.lp, &violationsBefore);

    row.arrange(c);                   // the plugin behaving
    CHECK(!fault::isQuarantined(c.s));
    CHECK(fault::totals().faults == 0 && fault::totals().quarantines == 0);

    std::error_code ec;
    if (abuse == Abuse::Fault && m.crashHandlers) {
        for (const auto& e : fs::directory_iterator(m.crashDir, ec)) { fs::remove(e.path(), ec); }
    }
    fs::remove(m.logFile, ec);
    hostile::setEnv("HOSTILE_PLUGIN_LOG", m.logFile.string().c_str());
    const std::string token = std::string(word) + "-" + abuseWord(abuse);
    setMode(token.c_str());
    if (abuse == Abuse::Loop) {
        HangWatchdog::resetWedgeFileCountForTest();            // (a session writes at most eight worker freeze reports; each row is a session's first)
        fault::overrideBudgetForTest(row.entry, 100);          // wedged at 400 ms
        hostile::setEnv("HOSTILE_PLUGIN_SPIN_MS", "1300");
    }
    const auto t0 = std::chrono::steady_clock::now();
    row.act(c);                       // THE PROCESS IS ALIVE: this line ran
    const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    setMode(nullptr);
    hostile::setEnv("HOSTILE_PLUGIN_SPIN_MS", nullptr);

    // ---- the plugin is switched off, by the entry that did it
    const fault::Why want = abuse == Abuse::Fault ? fault::Why::Fault : abuse == Abuse::Throw ? fault::Why::Exception : fault::Why::Wedge;
    CHECK(fault::isQuarantined(c.s));
    CHECK(fault::quarantineWhy(c.s) == want);
    CHECK(fault::quarantineEntry(c.s) == row.entry);
    const fault::Totals t = fault::totals();
    if (abuse == Abuse::Loop) {
        CHECK(tookMs >= 1200);                                  // it really was inside the call that long
        CHECK(t.overruns == 1 && t.wedges == 1 && t.quarantines == 1 && t.faults == 0);
    } else {
        CHECK(t.faults == 1 && t.quarantines == 1 && t.overruns == 0 && t.wedges == 0);
    }

    // ---- counted per id and version
    const std::vector<fault::HealthRow> rows = fault::health();
    const fault::HealthRow* h = healthOf(rows, "hp-m");
    CHECK(h != nullptr);
    if (h != nullptr) {
        CHECK(h->version == "1.2.3");
        CHECK(h->quarantines == 1);
        CHECK(h->quarantined == want);
        CHECK(h->lastEntry == row.entry);
        if (abuse == Abuse::Loop) {
            CHECK(h->wedges == 1 && h->overruns == 1 && h->faults == 0);
        } else {
            CHECK(h->faults == 1 && h->wedges == 0 && h->overruns == 0);
        }
        for (std::size_t k = 0; k < fault::kMisuseCount; ++k) { CHECK(h->misuse[k] == 0); }   // an abuse of the guard is not an abuse of the API
    }

    // ---- the events the engine's pass acts on
    std::size_t faultEvents = 0, quarantineEvents = 0, overrunEvents = 0, wedgeEvents = 0;
    for (const fault::Event& e : fault::drainEvents()) {
        CHECK(e.id == "hp-m");
        CHECK(e.entry == row.entry);
        faultEvents += e.kind == fault::Event::Kind::Fault ? 1u : 0u;
        quarantineEvents += e.kind == fault::Event::Kind::Quarantine ? 1u : 0u;
        overrunEvents += e.kind == fault::Event::Kind::Overrun ? 1u : 0u;
        wedgeEvents += e.kind == fault::Event::Kind::Wedge ? 1u : 0u;
    }
    CHECK(quarantineEvents == 1);
    if (abuse == Abuse::Loop) {
        CHECK(overrunEvents == 1 && wedgeEvents == 1 && faultEvents == 0);
    } else {
        CHECK(faultEvents == 1 && overrunEvents == 0 && wedgeEvents == 0);
    }

    // ---- the anonymous ledger gets the right words and no others
    ledgerFeed.poll(readingsNow());
    CHECK(ledgerCount("pluginquarantine") == 1);
    if (abuse == Abuse::Loop) {
        CHECK(ledgerCount("pluginwedge") == 1 && ledgerCount("pluginoveruse") == 1 && ledgerCount("pluginfault") == 0);
    } else {
        CHECK(ledgerCount("pluginfault") == 1 && ledgerCount("pluginwedge") == 0 && ledgerCount("pluginoveruse") == 0);
    }

    // ---- never called again
    {
        const std::string before = readFile(m.logFile);
        Ctx again;
        again.lp = m.lp;
        again.s = c.s;
        again.h = c.h;
        again.img = c.img;
        again.tile = c.tile;
        again.info = c.info;
        row.act(again);   // the same call, with the plugin's mode off: the guard refuses it, the plugin's own log is the witness
        CHECK(readFile(m.logFile) == before);
        CHECK(fault::totals().faults == t.faults && fault::totals().overruns == t.overruns);   // a refusal is not an event
    }

    // ---- the blame names the plugin and the entry
    if (abuse == Abuse::Fault && m.crashHandlers) {
#if defined(_WIN32)
        CHECK(countMatching(m.crashDir, "crash-") == 1);
        const std::string report = newestWithPrefix(m.crashDir, "crash-");
        CHECK(report.find("reason: fault in a plugin, absorbed") != std::string::npos);
        CHECK(countLines(report, std::string("blamed-plugin: ") + fault::publicIdOf(c.s)) == 1);
        CHECK(countLines(report, "blamed-plugin-version: 1.2.3") == 1);
        CHECK(countLines(report, std::string("blamed-plugin-entry: ") + word) == 1);
        // not the report of a death: an absorbed fault is a survived event
        CHECK(!cascade::core::crashReportWrittenByProcess(m.crashDir.string(), static_cast<unsigned long>(::GetCurrentProcessId()),
                                                          std::chrono::system_clock::now() - std::chrono::hours(1)));
#endif
    }
    if (abuse == Abuse::Loop) {
        // the monitor found it: a freeze report of the worker family that names the plugin and the entry
        std::string found;
        std::error_code lec;
        for (const auto& e : fs::directory_iterator(m.hangDir, lec)) {
            const std::string text = readFile(e.path());
            if (countLines(text, std::string("blamed-plugin-entry: ") + word) == 1 &&
                countLines(text, std::string("blamed-plugin: ") + fault::publicIdOf(c.s)) == 1) {
                found = text;
            }
        }
        CHECK(!found.empty());
        if (!found.empty()) {
            CHECK(countLines(found, "kind: hang") == 1);
            CHECK(found.find("a call into a plugin did not return within its budget") != std::string::npos);
            CHECK(countLines(found, "threshold-ms: 100") == 1);
        }
        CHECK(beats::callsInFlight() == 0);   // the table was left clean
        for (const auto& e : fs::directory_iterator(m.hangDir, lec)) { fs::remove(e.path(), lec); }
    }

    // ---- the host broke no rule towards the plugin
    std::string violationsAfter;
    const std::size_t violAfter = hostile::violations(*m.lp, &violationsAfter);
    if (violAfter != violBefore) { std::printf("      REFEREE: %s\n", violationsAfter.c_str()); }
    CHECK(violAfter == violBefore);

    hostile::setEnv("HOSTILE_PLUGIN_LOG", nullptr);
    fault::dropBudgetOverridesForTest();
}

// ---- A ----------------------------------------------------------------------------------------------------------------------------------
void testMatrix(Matrix& m) {
    std::printf("  A: every guarded entry point x a fault, a throw and a loop\n");
    CHECK(m.lp != nullptr && m.lp->loaded);
    if (m.lp == nullptr || !m.lp->loaded) { return; }
    CHECK(m.lp->decoder && m.lp->imageDecoder && m.lp->imageInput && m.lp->trackSource && m.lp->panel && m.lp->instrument && m.lp->basemap &&
          m.lp->trackInfo && m.lp->hostClient && m.lp->preset);

    // every Entry the guard knows (but Load and Unload, which section C holds) is in the table at least once
    for (std::size_t e = 0; e < fault::kEntryCount; ++e) {
        const fault::Entry entry = static_cast<fault::Entry>(e);
        if (entry == fault::Entry::Load || entry == fault::Entry::Unload) { continue; }
        bool seen = false;
        for (const Row& r : kRows) { seen = seen || r.entry == entry; }
        if (!seen) { std::printf("FAIL no row for entry %s\n", fault::entryWord(entry)); }
        CHECK(seen);
    }

#if defined(_WIN32)
    const bool structured = true;
#else
    const bool structured = false;
#endif
    if (structured) {
        m.crashDir = g_root / "crash";
        fs::create_directories(m.crashDir);
        cascade::core::CrashHandlerConfig cfg;
        cfg.crashDir = m.crashDir.string();
        cfg.enabled = true;
        cascade::core::installCrashHandlers(cfg);
        m.crashHandlers = true;
        std::printf("  -- a fault in the plugin's code (Windows: structured exceptions)\n");
        for (const Row& r : kRows) { runRow(m, r, Abuse::Fault); }
        cascade::core::setCrashCaptureEnabled(false, false);
        m.crashHandlers = false;
    } else {
        std::printf("  -- a fault in the plugin's code: SKIPPED, the guard absorbs only where the operating system has structured exceptions\n");
    }

    std::printf("  -- an exception across the C boundary\n");
    for (const Row& r : kRows) { runRow(m, r, Abuse::Throw); }

    std::printf("  -- a call that does not return (the budget stood down to 100 ms, wedged at 400)\n");
    m.hangDir = g_root / "hang";
    std::error_code ec;
    fs::remove_all(m.hangDir, ec);
    fs::create_directories(m.hangDir);
    HangWatchdog::resetWedgeFileCountForTest();
    m.watchdog.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    m.watchdog.start(m.hangDir.string(), 600000);   // the frame loop's threshold is far away: only the calls can be what is reported
    // one row per entry (the first that has it): the loop is the same code whichever capability it is in
    bool done[fault::kEntryCount] = {};
    for (const Row& r : kRows) {
        const std::size_t e = static_cast<std::size_t>(r.entry);
        if (done[e]) { continue; }
        done[e] = true;
        runRow(m, r, Abuse::Loop);
    }
    m.watchdog.stop();
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------------

std::uint64_t misuseOf(fault::Misuse k) {
    const std::vector<fault::HealthRow> rows = fault::health();
    const fault::HealthRow* h = healthOf(rows, "hp-m");
    return h != nullptr ? h->misuse[static_cast<std::size_t>(k)] : 0u;
}

// A fresh containment for one lie: nothing switched off, nothing counted.
void freshState() {
    fault::resetForTest();
    fault::dropBudgetOverridesForTest();
    (void)fault::drainEvents();
    setMode(nullptr);
}

void testLies(Matrix& m) {
    std::printf("  B: lies in what the plugin hands back\n");
    const LoadedPlugin& lp = *m.lp;
    fault::Slot* const slot = fault::slotFor(lp.path);
    const std::size_t violBefore = hostile::violations(lp);
    unsigned long code = 0;

    // ---- a poll_text that says it wrote more than it was given room for (the decoder's and the image decoder's, in the runner)
    {
        freshState();
        cascade::core::PluginRunner runner;
        runner.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        CHECK(runner.hasInstances(cascade::core::pluginKey(lp)));
        setMode("poll-text-lie");
        const bool alive = hostile::survives(
            [&] {
                std::vector<float> audio(480, 0.0f);
                for (int i = 0; i < 6; ++i) { runner.processAudio(audio.data(), audio.size()); }
                (void)runner.drainText();
            },
            &code);
        setMode(nullptr);
        CHECK(alive);
        CHECK(misuseOf(fault::Misuse::BadReturn) >= 2);   // both instances said it, every block
        CHECK(!fault::isQuarantined(slot));                // a lie the host can clamp is not a fault
        runner.clear();
    }

    // ---- the track source, the panel and the instrument, in the interface's per-frame poll
    struct UiLie {
        const char* mode;
        bool switchedOff;           // the lie is one the host can only answer by switching the plugin off (a pointer it cannot read)
        fault::Entry entry;
        fault::Misuse counted;      // the tripwire that must have moved (when !switchedOff)
    };
    const UiLie uiLies[] = {
        {"poll-tracks-lie", false, fault::Entry::PollTracks, fault::Misuse::BadReturn},
        {"poll-tracks-nan", false, fault::Entry::PollTracks, fault::Misuse::BadReturn},
        {"poll-paths-lie", true, fault::Entry::PollPaths, fault::Misuse::BadReturn},
        {"poll-rows-lie", false, fault::Entry::PollRows, fault::Misuse::BadReturn},
        {"columns-lie", false, fault::Entry::Columns, fault::Misuse::BadReturn},
        {"poll-state-lie", false, fault::Entry::PollState, fault::Misuse::BadReturn},
    };
    for (const UiLie& lie : uiLies) {
        std::printf("    ui: %s\n", lie.mode);
        freshState();
        cascade::core::PluginUi ui;
        setMode(lie.mode);   // (columns are read at create: the mode is on for the build too)
        bool alive = hostile::survives([&] { ui.rebuild({lp}); }, &code);
        CHECK(alive);
        alive = hostile::survives(
            [&] {
                ui.poll();
                ui.poll();
            },
            &code);
        setMode(nullptr);
        CHECK(alive);
        if (lie.switchedOff) {
            CHECK(fault::isQuarantined(slot));
            CHECK(fault::quarantineEntry(slot) == lie.entry);
        } else {
            CHECK(!fault::isQuarantined(slot));
            if (std::strcmp(lie.mode, "poll-tracks-nan") != 0) { CHECK(misuseOf(lie.counted) >= 1); }
        }
        // whatever came back is what the host would draw: bounded, on the globe, every cell a C string
        CHECK(ui.trackCount() <= kMaxTracksPerPlugin);
        for (const cascade::core::HostTrack& t : ui.tracks()) {
            CHECK(t.t.latDeg >= -90.0 && t.t.latDeg <= 90.0 && t.t.lonDeg >= -180.0 && t.t.lonDeg <= 180.0);
        }
        for (const cascade::core::HostPanel& p : ui.panels()) {
            CHECK(p.headings.size() >= 1 && p.headings.size() <= CASCADE_PANEL_MAX_COLUMNS);
            CHECK(p.rows.size() <= kMaxRowsPerPanel);
            for (const CascadePanelRow& r : p.rows) {
                for (const auto& cell : r.cells) { CHECK(std::memchr(cell, 0, CASCADE_PANEL_CELL_CHARS) != nullptr); }
            }
        }
        for (const cascade::core::HostInstrument& in : ui.instruments()) {
            for (const auto& text : in.state.text) { CHECK(std::memchr(text, 0, CASCADE_INSTRUMENT_TEXT_CHARS) != nullptr); }
            for (const CascadePanelRow& r : in.rows) {
                for (const auto& cell : r.cells) { CHECK(std::memchr(cell, 0, CASCADE_PANEL_CELL_CHARS) != nullptr); }
            }
        }
        ui.clear();
    }

    // ---- the picture decoder's pixels: numbers the host can check, and a pointer it cannot
    struct PixelLie {
        const char* mode;
        bool switchedOff;
    };
    const PixelLie pixelLies[] = {
        {"poll-image-lie", true},    // 64 x 64 over a buffer readable for 64 bytes: the second row is unreadable
        {"poll-image-wild", true},   // a garbage pointer
        {"poll-image-null", false},  // NULL where the ABI says never: refused by the check, counted
        {"poll-image-huge", false},  // dimensions past the ABI's bound: refused by the check, counted
    };
    for (const PixelLie& lie : pixelLies) {
        std::printf("    image: %s\n", lie.mode);
        freshState();
        cascade::core::PluginRunner runner;
        runner.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        std::vector<cascade::core::HostImage> images;
        setMode(lie.mode);
        const bool alive = hostile::survives([&] { runner.pollImages(images); }, &code);
        setMode(nullptr);
        CHECK(alive);
        if (lie.switchedOff) {
            CHECK(fault::isQuarantined(slot));
            CHECK(fault::quarantineEntry(slot) == fault::Entry::PollImage);
            CHECK(fault::quarantineWhy(slot) == fault::Why::Fault);
        } else {
            CHECK(!fault::isQuarantined(slot));
            CHECK(misuseOf(fault::Misuse::BadReturn) >= 1);
        }
        for (const cascade::core::HostImage& img : images) {
            const std::size_t bpp = img.format == CASCADE_IMAGE_RGB24 ? 3u : 1u;
            CHECK(img.pixels.empty() || img.pixels.size() == static_cast<std::size_t>(img.width) * img.height * bpp);
        }
        runner.clear();
    }

    // ---- the basemap's tiles
    struct TileLie {
        const char* mode;
        bool switchedOff;
    };
    const TileLie tileLies[] = {{"get-tile-lie", true}, {"get-tile-wild", true}, {"get-tile-null", false}};
    for (const TileLie& lie : tileLies) {
        std::printf("    tile: %s\n", lie.mode);
        freshState();
        cascade::gui::BasemapCache cache;
        cache.attach(lp.basemap, lp.path);
        CHECK(cache.active());
        std::vector<std::uint8_t> rgb;
        std::int32_t got = 0;
        setMode(lie.mode);
        const bool alive = hostile::survives([&] { got = cache.rawTile(1, 0, 0, rgb); }, &code);
        setMode(nullptr);
        CHECK(alive);
        CHECK(got == CASCADE_TILE_MISSING);   // nothing believed
        if (lie.switchedOff) {
            CHECK(fault::isQuarantined(slot));
            CHECK(fault::quarantineEntry(slot) == fault::Entry::GetTile);
        } else {
            CHECK(!fault::isQuarantined(slot));
            CHECK(misuseOf(fault::Misuse::BadReturn) >= 1);
        }
        cache.detach();
    }
    // (and the same plugin, truthful, is believed: the guard does not make a good tile unreadable)
    {
        freshState();
        cascade::gui::BasemapCache cache;
        cache.attach(lp.basemap, lp.path);
        std::vector<std::uint8_t> rgb;
        CHECK(cache.rawTile(1, 0, 0, rgb) == CASCADE_TILE_READY);
        CHECK(rgb.size() == 256u * 256u * 3u);
        CHECK(!fault::isQuarantined(slot));
        cache.detach();
    }

    // ---- the track-info client's strings
    for (const char* mode : {"get-info-wild", "get-info-unterminated"}) {
        std::printf("    info: %s\n", mode);
        freshState();
        cascade::gui::TrackInfoCache cache;
        cache.attach(lp.trackInfo, lp.path);
        CHECK(cache.active());
        const cascade::gui::TrackInfoCache::Info* info = nullptr;
        setMode(mode);
        const bool alive = hostile::survives([&] { info = cache.get("ABC123", CASCADE_TRACK_AIRCRAFT); }, &code);
        setMode(nullptr);
        CHECK(alive);
        CHECK(info == nullptr || !info->known);   // a record read from a pointer that could not be read is no record
        CHECK(fault::isQuarantined(slot));
        CHECK(fault::quarantineEntry(slot) == fault::Entry::GetInfo);
        cache.detach();
    }
    {
        // truthful strings are read, whole
        freshState();
        cascade::gui::TrackInfoCache cache;
        cache.attach(lp.trackInfo, lp.path);
        const cascade::gui::TrackInfoCache::Info* info = cache.get("ABC123", CASCADE_TRACK_AIRCRAFT);
        CHECK(info != nullptr && info->known && info->registration == "G-HOST" && info->typeCode == "A319");
        CHECK(!fault::isQuarantined(slot));
        cache.detach();
    }

    // ---- the preset table
    {
        freshState();
        setMode("preset-count-lie");
        const std::uint32_t said = calls::presetCount(slot, lp.preset);
        setMode(nullptr);
        CHECK(said == 1000000u);
        CHECK(cascade::gui::cappedPresetCount(said, 64) <= 64u);   // the host's own cap is what the menu is built from
        setMode("preset-get-lie");
        CascadePreset ps{};
        ps.structSize = sizeof(ps);
        const bool alive = hostile::survives([&] { (void)calls::presetGet(slot, lp.preset, 7, &ps); }, &code);
        setMode(nullptr);
        CHECK(alive);
        CHECK(!cascade::gui::presetIsValid(ps));   // NaN frequency, a mode that is not one, a negative bandwidth: not a preset
    }

    std::string viol;
    const std::size_t violAfter = hostile::violations(lp, &viol);
    if (violAfter != violBefore) { std::printf("      REFEREE: %s\n", viol.c_str()); }
    CHECK(violAfter == violBefore);
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------------
// Each lie is a copy of the fixture of its own (a descriptor is read once, at the scan, so the mode is set before the scan and the copy is
// the one plugin in a folder of its own). The module must be refused with a verdict or an error, never mapped as a working plugin, and the
// process must be alive to say so.
void testLoadLies() {
    std::printf("  C: a descriptor that lies, a fault in DllMain or in the query\n");
#if !defined(_WIN32)
    std::printf("    SKIPPED: a wild pointer in a descriptor is absorbed only where the operating system has structured exceptions\n");
    return;
#else
    const char* modes[] = {"dllmain-av",       "query-av",           "desc-wild-name",           "desc-wild-tables",
                           "entry-dup",        "panel-title-wild",   "instrument-title-wild",    "basemap-attribution-wild",
                           "caps-word-lie"};
    std::size_t n = 0;
    for (const char* mode : modes) {
        const std::string id = std::string("hp-l") + std::to_string(n++);
        const fs::path dir = g_root / ("load-" + id);
        std::error_code ec;
        fs::create_directories(dir);
        fs::copy_file(g_fixture, dir / hostile::leafOf(id), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        fault::resetForTest();
        setMode(mode);
        PluginHost host;
        const bool alive = hostile::survives([&] { host.scan(dir.string()); });
        setMode(nullptr);
        CHECK(alive);
        const LoadedPlugin* lp = hostile::find(host, id);
        std::printf("    %-26s -> %s\n", mode, lp == nullptr ? "(no record)" : (lp->loaded ? "LOADED" : lp->error.c_str()));
        // refused, whatever the reason: either not mapped at all or mapped and judged unusable - never a working plugin
        CHECK(lp == nullptr || !lp->loaded);
        host.unloadAll();
    }

    // THE ONE LIE THE HOST DOES NOT REFUSE (6.28 rule 17): a capability entry whose tableSize is not the host's sizeof. The entry's size is advisory;
    // the table's own structSize, checked before anything else in it is read, is the size the host trusts - so the plugin loads.
    {
        const fs::path dir = g_root / "load-entry-size";
        std::error_code ec;
        fs::create_directories(dir);
        fs::copy_file(g_fixture, dir / hostile::leafOf("hp-s"), fs::copy_options::overwrite_existing, ec);
        fault::resetForTest();
        setMode("entry-size");
        PluginHost host;
        const bool alive = hostile::survives([&] { host.scan(dir.string()); });
        setMode(nullptr);
        CHECK(alive);
        const LoadedPlugin* lp = hostile::find(host, "hp-s");
        CHECK(lp != nullptr && lp->loaded);
        host.unloadAll();
    }

    // a fault in DllMain(PROCESS_DETACH): absorbed at unload (the loader runs it inside FreeLibrary, which the host calls under the guard)
    {
        const fs::path dir = g_root / "load-detach";
        std::error_code ec;
        fs::create_directories(dir);
        fs::copy_file(g_fixture, dir / hostile::leafOf("hp-d"), fs::copy_options::overwrite_existing, ec);
        fault::resetForTest();
        PluginHost host;
        host.scan(dir.string());
        const LoadedPlugin* lp = hostile::find(host, "hp-d");
        CHECK(lp != nullptr && lp->loaded);
        setMode("dllmain-detach-av");
        const bool alive = hostile::survives([&] { host.unloadAll(); });
        setMode(nullptr);
        CHECK(alive);   // THE PROCESS IS ALIVE: the unload is a guarded call
        std::printf("    dllmain-detach-av: unload %s\n",
                    fault::isQuarantinedKey(hostile::leafOf("hp-d")) ? "absorbed, plugin switched off" : "returned clean");
    }
#endif
}

// ---- D ----------------------------------------------------------------------------------------------------------------------------------
void testControls(Matrix& m) {
    std::printf("  D: the controls - what must NOT be counted\n");
    const LoadedPlugin& lp = *m.lp;
    fault::Slot* const slot = fault::slotFor(lp.path);
    freshState();
    // a create() that fails honestly is the ABI's own answer: no instance, no fault, nothing counted, nothing switched off
    setMode("create-null");
    void* h = calls::create(slot, lp.decoder, 48000u);
    setMode(nullptr);
    CHECK(h == nullptr);
    CHECK(!fault::isQuarantined(slot));
    CHECK(fault::totals().faults == 0 && fault::totals().quarantines == 0);
    // a healthy plugin run through every kind of instance leaves every counter at zero and the referee silent
    freshState();
    const std::size_t violBefore = hostile::violations(lp);
    cascade::core::PluginRunner runner;
    runner.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
    cascade::core::PluginUi ui;
    ui.rebuild({lp});
    std::vector<float> audio(480, 0.0f);
    std::vector<cascade::core::HostImage> images;
    for (int i = 0; i < 20; ++i) {
        runner.processAudio(audio.data(), audio.size());
        runner.pollImages(images);
        ui.poll();
    }
    (void)runner.drainText();
    CHECK(ui.trackCount() >= 1);
    CHECK(!images.empty());
    runner.clear();
    ui.clear();
    const std::vector<fault::HealthRow> rows = fault::health();
    const fault::HealthRow* row = healthOf(rows, "hp-m");
    CHECK(row != nullptr);
    if (row != nullptr) {
        CHECK(row->faults == 0 && row->quarantines == 0 && row->overruns == 0 && row->wedges == 0);
        for (std::size_t k = 0; k < fault::kMisuseCount; ++k) { CHECK(row->misuse[k] == 0); }
    }
    CHECK(!fault::isQuarantined(slot));
    std::string viol;
    const std::size_t violAfter = hostile::violations(lp, &viol);
    if (violAfter != violBefore) { std::printf("      REFEREE: %s\n", viol.c_str()); }
    CHECK(violAfter == violBefore);   // the host used every kind of instance by the book
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: test_plugin_hostile <hostile fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    g_root = fs::temp_directory_path() / ("foxsdr-test-plugin-hostile-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                   ::GetCurrentProcessId()
#else
                                                                                   1
#endif
                                                                                       )));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / "plugins");
    fs::copy_file(g_fixture, g_root / "plugins" / hostile::leafOf("hp-m"), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    setMode(nullptr);

    Matrix m;
    m.host.scan((g_root / "plugins").string());
    m.lp = hostile::find(m.host, "hp-m");
    m.logFile = g_root / "hostile.log";
    if (m.lp == nullptr || !m.lp->loaded) {
        std::printf("the fixture did not load: %s\n", m.lp != nullptr ? m.lp->error.c_str() : "not found");
    }
    testMatrix(m);
    testLies(m);
    testControls(m);
    testLoadLies();

    m.host.unloadAll();
    fs::remove_all(g_root, ec);
    return testSummary("test_plugin_hostile");
}
