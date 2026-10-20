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
#include "net/web_server.hpp"

namespace cascade::engine {

class EngineHost {
public:
    EngineHost();
    virtual ~EngineHost();

    // --- CLOCKS ---------------------------------------------------------------
    // The frame clock the moved code always read (ImGui::GetTime() in the
    // window): recordings' start times, the bookmark save's debounce, the
    // scanner's dwell, the audio watchdog's cadence, the radio reopen's
    // hold-off. frameClockRunning() stands for "an ImGui context exists", the
    // guard the bookmark flush put in front of it.
    virtual bool frameClockRunning() const;
    virtual double frameTimeS() const;
    // The wall-clock the usage report measures a session with (glfwGetTime()
    // in the window).
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
    // command (FOXAPP_OP_TRACK_INFO_LINE) and its answers for the web status over in FrontEndFacts::webExtras.)
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
    // Plugins: before a rescan (the map pages' geometry is folded into the
    // saved list), before the modules are unmapped (the basemap and the
    // track-info cache let go of what they borrowed), the basemap and the
    // track-info plugin chosen by a rebuild, a demonstration instrument that
    // opens its own window, and the track-info plugin's status lines.
    virtual void beforePluginRescan();
    virtual void onPluginsUnloading();
    virtual void attachBasemap(const CascadeBasemapApi* api);
    virtual void attachTrackInfo(const CascadeTrackInfoApi* api);
    virtual void showDemonstrationInstrument(const cascade::core::HostInstrument& in);

    // --- WHAT THE WINDOW KNOWS THAT THE ENGINE ASKS ---------------------------------
    // (patchPageOpen left with window port group 13: the window tells the engine
    // by FOXAPP_OP_SET_PATCH_PAGE_OPEN, Engine::patchPageOpen_.)
    // (A track's enrichment from the track-info plugin and the decoded pictures the web status lists were asked for here
    // until the pump move's step P2; the front end HANDS THEM OVER now, Engine::setFrontEndFacts, like the web listener's
    // state, the counter's face and the basemap plugin's figures since engine/stage3b-pre - docs/engine-stage3.md OPEN 4.
    // BasemapFacts is the basemap's type.)
    struct BasemapFacts {
        bool active = false;
        std::uint32_t minZoom = 0;
        std::uint32_t maxZoom = 0;
        std::uint32_t tileSize = 0;
        std::string attribution;
    };

private:
    std::chrono::steady_clock::time_point born_;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HOST_HPP
