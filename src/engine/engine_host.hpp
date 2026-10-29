// engine_host.hpp - what the Engine asks of whatever front end holds it
// (engine extraction stage 3a, docs/engine-stage3.md).
//
// WHY THIS EXISTS. The receiver's machinery moved out of the window verbatim,
// and in a few places it had always reached into the window: to tell the
// spectrum its new range, to put the map pages' home on the receiver's new
// position, to pause the hang watchdog across a plugin rescan, to read the
// frame clock. Each of those is one call here, made exactly where the moved
// code made it before, and the window (gui::AppWindow) answers it exactly as
// the old line did. A front end that is not a window - the headless engine of
// step 2, the tests in tests/test_engine_headless.cpp - uses the defaults in
// engine_host.cpp: clocks from std::chrono, no views, no store window.
//
// EVERY HOOK IS CALLED SYNCHRONOUSLY, ON THE ENGINE'S CALLING THREAD - the GUI
// thread in stage 3a, which is what keeps this stage free of any threading
// change. In 3b the engine runs on its own control thread, and every hook
// that touches window state becomes a checklist item: it has to become an
// event the window drains on its own thread (docs/engine-stage3.md lists
// which are which).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_HOST_HPP
#define CASCADE_ENGINE_ENGINE_HOST_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "engine/engine_shell.hpp"
#include "net/web_server.hpp"

namespace cascade::engine {

class EngineHost {
public:
    EngineHost();
    virtual ~EngineHost();

    // --- CLOCKS ---------------------------------------------------------------
    // (The FRAME CLOCK left with the pump move's step P4: the engine runs on its own steady clock, Engine::clockS(), because the
    // pass is not the frame any more and a clock that stands still when the window does - a drag of its title bar, a modal loop -
    // froze the scanner's dwell, the bookmark save's debounce, the audio watchdog and the radio reopen's hold-off with it.)
    // The wall-clock the usage report measures a session with. One clock for every host of the process (engine_host.cpp, processEpoch): the window
    // application's host replaced the service's after the restore stamped the session's start (Engine::setHost, R7b to R8; removed by R9), and a host that replaced another
    // had to leave this alone (the shell's adapter did), so the stamp and the reads that follow it are on one clock - which stays true of every host a test builds.
    virtual double wallTimeS() const;

    // --- THE HANG WATCHDOG ------------------------------------------------------
    // Paused across deliberate blocking work on the calling thread (a plugin
    // rescan, a bounded device open).
    virtual void pauseWatchdog();
    virtual void resumeWatchdog();

    // --- WHAT THE WINDOW DOES WHEN THE ENGINE HAS DONE SOMETHING -----------------
    // (THE PUMP MOVE, step P2, took six of these out - each wrote state the window draws from, and a pass on a thread of its
    // own must not: onDisplayRange and onConverterChanged are gone because the window follows the state it already reads
    // (the display range and the stored converter), onBookmarksChanged because nothing used it, openPluginWindowsFor because
    // a preset applied is an EVENT now (FOXAPI_EVENT_PRESET_APPLIED, API 0.3 8.14) the window answers on its own thread, and
    // drainTrackInfoText, enrichWebTrack and fillWebImages because the window hands the track-info plugin's lines in as a
    // command (FOXAPP_OP_TRACK_INFO_LINE) and its answers for the web status over in FrontEndFacts::trackFeed.)
    // (onReceiverPositionApplied left with window port group 14, part a: the receiver's position is in the state, and
    // the window follows it - the typed fields, every map page's and Patch Map part's home, the scope, the coverage map
    // - on its own thread when it moves. API 0.3 8.11.1.)
    // (onPatchGraphChanged left with window port group 13: the engine's own
    // changes to the patch graph move fact.patch.version, which the window
    // reads in the block it draws from - API 0.3 8.12.)
    // (onPatchPicture left with window port group 13, part c: a patch decoder's picture is the engine's now - the
    // newest of each node is read through read_image, and announced by FOXAPI_EVENT_DECODER_IMAGE, API 0.3 8.12.)
    // (onGpsFixApplied left with window port group 14, part b: the GPS list's entry going to Fixed is the window's cue.)
    // The store: a catalogue fetch starts; a fetch or install result was
    // collected (the bounded-run test hook reports); an ADD ALL run finished.
    // THE STANDARD WINDOW IMPLEMENTS ONLY onCatalogueResult (window port,
    // group 10): the first and the last were its way of clearing the consent
    // ticks it holds - for the old catalogue, for the run - from the ENGINE's
    // thread, into state the window reads and writes in its own frame. It now
    // watches the table for both (STORE_ACTIVITY, the catalogue itself) and
    // clears them on its own thread, so no code in src/gui overrides either;
    // they stay for a front end that wants to be told.
    virtual void onCatalogueFetchStarting();
    virtual void onCatalogueResult();
    virtual void onAddAllFinished();
    // (ADD ALL is planned by the Engine itself since engine/stage3b-pre -
    // engine/add_all_plan.hpp, docs/engine-stage3.md OPEN 5.)
    // (THE PUMP MOVE'S STEP P3 took the last five out, the plugin UI's: beforePluginRescan, onPluginsUnloading, attachBasemap,
    // attachTrackInfo and showDemonstrationInstrument. The plugin UI is the front end's - its instances, its basemap cache, its
    // track-info cache and its windows are polled and drawn on its own thread - so the engine no longer reaches them: it posts a
    // PluginDirective (core/plugin_directive.hpp) the front end applies at the head of its frame, and an unload waits for the
    // front end's acknowledgement before it unmaps a module. The map pages' geometry is folded into the saved list, the basemap
    // and the track-info client let go, and a demonstration instrument's window opened, by the front end's own applier.)

    // --- WHAT THE WINDOW KNOWS THAT THE ENGINE ASKS ---------------------------------
    // (patchPageOpen left with window port group 13: the window tells the engine
    // by FOXAPP_OP_SET_PATCH_PAGE_OPEN, Engine::patchPageOpen_.)
    // (A track's enrichment from the track-info plugin and the decoded pictures the web status lists were asked for here
    // until the pump move's step P2; the front end HANDS THEM OVER now, Engine::setFrontEndFacts, like the web listener's
    // state, the counter's face and the basemap plugin's figures since engine/stage3b-pre - docs/engine-stage3.md OPEN 4.
    // BasemapFacts is the basemap's type.)
    using BasemapFacts = cascade::engine::BasemapFacts;  // (moved to engine_shell.hpp with the fence: the interface names it)

private:
    std::chrono::steady_clock::time_point born_;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HOST_HPP
