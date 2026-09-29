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

#include "core/host_image.hpp"
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
    // The display range changed (the spectrum's axis and the waterfall's
    // colour map read it).
    virtual void onDisplayRange(float dbMin, float dbMax);
    // The bookmark list changed under a command (the list's cached view).
    virtual void onBookmarksChanged();
    // A plugin preset was applied: open what that plugin contributes.
    virtual void openPluginWindowsFor(const cascade::core::LoadedPlugin& p);
    // The receiver's position was applied: the typed fields follow, every map
    // page and the scope move home, the coverage map starts again.
    virtual void onReceiverPositionApplied(double latDeg, double lonDeg);
    // The converter for the radio in use changed: the LO field re-seeds (and
    // its error clears when `clearLoError`).
    virtual void onConverterChanged(bool clearLoError);
    // The patch graph was changed by the engine (a radio's settings followed
    // its device, START switched radios on): the canvas re-serialises.
    virtual void onPatchGraphChanged();
    // A patch decoder node's newest picture, for its face.
    virtual void onPatchPicture(cascade::core::patch::NodeId node, cascade::core::HostImage&& img);
    // A GPS fix was applied: the rail's position fold reveals itself.
    virtual void onGpsFixApplied();
    // The store: a catalogue fetch starts (the store window forgets the
    // consent ticks it held for the old catalogue); a fetch or install
    // result was collected (the bounded-run test hook reports); an ADD ALL
    // run finished (its tick was for that run).
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
    virtual std::vector<std::string> drainTrackInfoText();

    // --- WHAT THE WINDOW KNOWS THAT THE ENGINE ASKS ---------------------------------
    // The Patch page is on screen (the patch runtime lists the sound cards
    // for its inspector only then).
    virtual bool patchPageOpen() const;
    // For the one publish: a track's enrichment from the track-info plugin,
    // and the decoded pictures. (The web listener's state, the counter's face
    // and the basemap plugin's figures are HANDED OVER by the front end since
    // engine/stage3b-pre - Engine::setFrontEndFacts, docs/engine-stage3.md
    // OPEN 4 - rather than asked for here; BasemapFacts is their type.)
    struct BasemapFacts {
        bool active = false;
        std::uint32_t minZoom = 0;
        std::uint32_t maxZoom = 0;
        std::uint32_t tileSize = 0;
        std::string attribution;
    };
    virtual void enrichWebTrack(cascade::net::RadioStatus::Track& w);
    virtual void fillWebImages(cascade::net::RadioStatus& s);

private:
    std::chrono::steady_clock::time_point born_;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HOST_HPP
