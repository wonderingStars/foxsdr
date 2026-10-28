// plugin_ui_port.hpp - THE PLUGIN UI PLANE: the one C++ interface between an interface (the window) and the plugin UI the engine
// hosts for it, and the data that crosses it (shell retirement R4, stage 7h; docs/engine-stage4.md 6.25, docs/api-0.3-additions.md 9.15).
//
// WHY IT IS NOT A TABLE LIST, FACT OR OP. The plugin ABI promises that a track source, panel, instrument, basemap and track-info instance is
// created, polled and destroyed on the INTERFACE's thread (docs/api-0.3-additions.md 8.15.2), and the directive that builds them carries
// pointers into module images (core/plugin_directive.hpp). Pointers into a module are not a list item, a fact or an op payload; the table's
// TRACKS list is a gap and stays one. So the plane keeps its C++ shape: this interface, reached through ONE table member,
// `get_local_extension(engine, FOXAPI_EXT_PLUGIN_UI, args, &out)` (foxsdr_api.h; LOCAL sessions only, capability bit
// FOXAPI_CAP_LOCAL_EXT, REFUSED to a REMOTE session). It was `HostPluginFeed` in engine/engine_shell.hpp, the HOST class of the fenced door;
// from R4 the door has no HOST class and no feed, and this is the whole of what the window has of the plugin UI.
//
// WHAT THIS HEADER MAY CONTAIN: data and pure virtuals, and nothing the fence denies the interface. It is the one new header the interface
// includes for this plane; tests/test_engine_shell_guard.cpp holds that it includes nothing of the engine, and counts its virtuals
// (kPortMethodsBudget).
//
// THE THREAD CONTRACT (the ABI's, unchanged): the thread that attaches is THE INTERFACE'S thread. Every call below except `disconnect` and the
// destructor is made on it; the engine's side counts a call from any other thread (PluginUiPortImpl::offThreadCalls, the tripwire the tests
// assert is zero). The engine applies a directive inline only where it runs on that thread (or no control thread runs), otherwise it posts it
// and waits, bounded, for `applyNewestDirective` - the unload handshake (api-0.3-additions.md 8.15.3).
//
// LIFETIME OF THE POINTERS INTO MODULES. `PluginDirectiveFacts::basemap` / `trackInfo` point into module images. They are valid from the
// `after` step of the directive that carried them until the `after` step of the next RELEASE (which has them null); the interface lets go of
// them in that step, on its own thread, before the engine unmaps (the handshake). `release()` is the interface's own release of the same
// kind; `disconnect()` ends the engine's use of this interface's applier.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_UI_PORT_HPP
#define CASCADE_CORE_PLUGIN_UI_PORT_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"
#include "foxsdr_api.h"

namespace cascade::core {

// ==================================================================================================================================
// THE DATA - moved here verbatim from engine/engine_shell.hpp (R4), which names each back in namespace cascade::engine.
// ==================================================================================================================================

// THE TRACK LIST AND WHAT THE TRACK-INFO PLUGIN SAYS OF IT, the window's plugin poll as it hands it to the engine (pump move steps P2 and P3;
// named WebExtras until stage 5e). The track-info plugin's answers for the tracks on screen used to be asked of the window while the engine
// published, reading the window's TrackInfoCache - GUI-thread state - from the pass. The window builds this once a frame and hands it over by
// VALUE (PluginUiFrontEnd::trackFeed); the engine's side COPIES it into an immutable object of its own, which the pass reads - the two
// threads never see one object.
struct TrackFeedInfo {
    std::string id;           // the track's id and kind: the key (the vector is sorted by id, then kind)
    unsigned kind = 0;
    unsigned infoState = 0;   // 1 = known, 2 = the source does not have it (RadioStatus::Track::infoState)
    std::string registration;
    std::string acType;
    std::string acOperator;
    std::string acCountry;
};
struct TrackFeed {
    std::vector<TrackFeedInfo> tracks;
    // THE TRACKS THE LAST PLUGIN POLL FOUND, as the window drew them: the visible ones (the staleness rule the desktop applies); the pass
    // applies it again.
    std::vector<HostTrack> polled;
};

// The basemap plugin's figures, handed over once a frame (engine/stage3b-pre OPEN 4).
struct BasemapFacts {
    bool active = false;
    std::uint32_t minZoom = 0;
    std::uint32_t maxZoom = 0;
    std::uint32_t tileSize = 0;
    std::string attribution;
};

// WHAT THE BOUNDED-RUN STATUS REPORT PRINTS OF THE PLUGIN UI (CASCADE_PLUGIN_STATUS; pump move step P3): the tracks, the number of paths and
// the panels the window's poll found. Handed over inside PluginUiFrontEnd at the end of a bounded run that asked for the report, and printed
// by the engine, which owns the rest of the report (Engine::reportPluginStatus).
struct PluginUiReport {
    std::vector<HostTrack> tracks;
    std::size_t paths = 0;
    std::vector<HostPanel> panels;
};

// WHAT THE WINDOW ANSWERS A PAGE'S TILE REQUEST WITH (stage 5e). The page asks the engine's web server for a basemap tile, which records the
// want; the window - whose plugin client is the only thing that can produce one - drains the wants once a frame (PluginUiPort::serveTiles)
// and publishes what its plugin has.
enum class WebTileAnswer { Pending, Missing, Ready };  // Pending: asked of the plugin, ask again later (nothing is published)
struct WebTileRequest {
    std::uint32_t z = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};
// Fills `bmp` (a 24-bit BMP) and answers Ready; Missing is the plugin saying the tile will never exist (published empty, so the page stops
// asking); Pending publishes nothing.
using WebTileSource = std::function<WebTileAnswer(const WebTileRequest&, std::vector<std::uint8_t>& bmp)>;

// What one plugin directive says to the WINDOW (the rest of it - the plugins, their services - is the plugin UI's, applied inside the
// port): a release (the engine is about to unmap the modules), and the basemap and track-info tables to attach.
struct PluginDirectiveFacts {
    bool release = false;
    // One plugin woken or put to sleep, nothing rebuilt (stub campaign B2): the basemap and track-info tables are the engine's choice again,
    // but nothing was created that a window has to show.
    bool lifeOnly = false;
    const CascadeBasemapApi* basemap = nullptr;
    const CascadeTrackInfoApi* trackInfo = nullptr;
};

// WHAT THE FRONT END HANDS OVER, ONCE A FRAME (PluginUiPort::setFrontEnd): the facts only the window knows - the counter's face (the web page
// mirrors it), the basemap plugin's figures, the track feed - and a bounded run's two diagnostics. ALL BY VALUE: the engine's side copies what
// it keeps, so the window may refill this object at once; nothing in it is shared between the two threads.
struct PluginUiFrontEnd {
    // The product's default face is the Nixie counter; an empty name is "no face handed over" and shows the engine's default.
    std::string tunerDisplayStyle;
    BasemapFacts basemap;
    // The track feed (above), when there is one: false is "no tracks on screen" (every pass of a headless engine), and the pass then lists
    // none.
    bool hasTrackFeed = false;
    TrackFeed trackFeed;
    // A BOUNDED RUN'S DIAGNOSTIC (CASCADE_PLUGIN_TEST): whether to print the catalogue report when a fetch resolves, and the window's frame
    // count to name in it.
    bool pluginTestReport = false;
    int frame = 0;
    // A BOUNDED RUN'S DIAGNOSTIC (CASCADE_PLUGIN_STATUS): when true, `pluginUi` and `pluginImages` are what the window's plugin poll and its
    // picture buffer hold, and the engine prints the report with them right after its first teardown phase (Engine::shutdownQuiesce). The
    // window sets it for the one hand-over made at the end of a bounded run that asked for the report, never in a product run.
    bool pluginStatusReport = false;
    PluginUiReport pluginUi;
    std::vector<HostImage> pluginImages;
};

// ==================================================================================================================================
// THE PORT
// ==================================================================================================================================
class PluginUiPort {
public:
    // The window's two steps around the plugin UI's own: BEFORE it rebuilds (a release folds the map pages' geometry into the saved list
    // while the instances still stand) and AFTER (attach the basemap and the track-info client, show a demonstration instrument).
    struct Steps {
        std::function<void(const PluginDirectiveFacts&)> before;
        std::function<void(const PluginDirectiveFacts&)> after;
    };
    virtual ~PluginUiPort() = default;

    // --- the nine host accessors (references into the plugin UI's vectors, valid for the frame: the directive applies at the head of the
    // frame only) ---
    virtual const std::vector<HostTrack>& tracks() const = 0;
    // THE TARGETS A TRACK SOURCE HEARS AND CANNOT PLACE YET (stub campaign B16): after every placed row, counted on the list's heading, and
    // never drawn. PluginUi keeps them apart from tracks() so that every reader of tracks() draws or measures from a position.
    virtual const std::vector<HostTrack>& unplacedTracks() const = 0;
    virtual const std::vector<HostPath>& paths() const = 0;
    virtual const std::vector<HostPanel>& panels() const = 0;
    virtual const std::vector<HostInstrument>& instruments() const = 0;
    virtual const std::vector<std::string>& trackPluginNames() const = 0;
    virtual bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg, double& outAltM) = 0;
    // Polls the plugins (the frame's one poll, after the head-of-frame directive).
    virtual void poll() = 0;
    // The receiver plugins' decoded pictures, brought up to date in `out` from the engine's one picture accumulator (stage 5e): a poll made on
    // the caller's thread, once a frame.
    virtual void pollImages(std::vector<HostImage>& out) = 0;
    // Applies the newest directive, if one is waiting, on this thread and acknowledges it to the engine (the head of the frame).
    virtual void applyNewestDirective() = 0;

    // --- what only the window can hand the engine ---
    // The frame's facts for the publish (see PluginUiFrontEnd); copied, once a frame.
    virtual void setFrontEnd(const PluginUiFrontEnd& facts) = 0;
    // THE BROWSER MAP'S TILES, ONE CALL A FRAME (stage 5e/5f). `clearFirst` drops every stored tile and pending request (the basemap plugin
    // changed or went away: tiles of the old source must not be served under the new one). `serve` false stops there. Otherwise the tiles
    // the page asked for since the last call are drained and the first `maxTiles` are asked of `source` and published; the rest are dropped
    // (the page asks again). `source` is the window's GL-class basemap client: this is called on the interface's thread, and the tripwire
    // counts a call from any other.
    virtual void serveTiles(bool clearFirst, bool serve, std::size_t maxTiles, const WebTileSource& source) = 0;
    // THE INTERFACE'S OWN RELEASE of everything borrowed from the modules: the same directive the engine posts before it unmaps, applied now
    // on this thread - the `before` step (fold the map pages), the plugin UI emptied (every instance destroyed), the `after` step (detach the
    // basemap and the track-info client, GL current). The window calls it before it hands the engine its shutdown release; the engine's
    // release that follows finds nothing to do.
    virtual void release() = 0;
    // The engine no longer applies a directive through this port (the window is about to go): after this the engine drives its own plugin UI
    // again. Idempotent; the destructor calls it.
    virtual void disconnect() = 0;
};

// What the interface passes to get_local_extension(FOXAPI_EXT_PLUGIN_UI): the common prefix (the interface's LOCAL session, which the engine
// checks) and the two steps. `out` receives a PluginUiPort* the caller owns (delete through the virtual destructor, on the thread that
// attached). The engine answers FOXAPI_BUSY while another port is attached.
struct PluginUiAttach : FoxLocalExtensionArgs {
    PluginUiPort::Steps steps;
};

// A port that does nothing and holds nothing: what a window holds when the engine cannot serve the extension (an engine without
// FOXAPI_CAP_LOCAL_EXT, a refused attach), so that no call site needs a null check. Defined in core/plugin_ui_port.cpp.
std::unique_ptr<PluginUiPort> makeNullPluginUiPort();

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_UI_PORT_HPP
