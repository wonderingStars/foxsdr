// plugin_ui_port.cpp - the engine's side of the plugin UI plane (engine/plugin_ui_port.hpp; shell retirement R4, docs/engine-stage4.md 6.25).
//
// The bodies of engine_shell.cpp's PluginFeedImpl, the bounded-run report's hand-over and the browser map's tile service, moved here
// unchanged in what they do: the same calls on the same objects, in the same order, on the same (interface) thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/plugin_ui_port.hpp"

#include <utility>

#include "core/diag_log.hpp"
#include "core/plugin_directive.hpp"
#include "engine/engine.hpp"
#include "engine/image_feed.hpp"
#include "engine/server_host.hpp"
#include "net/web_server.hpp"

namespace cascade::engine {

namespace cc = cascade::core;

PluginUiPortImpl::PluginUiPortImpl(Engine& engine, Steps steps)
    : engine_(engine),
      steps_(std::move(steps)),
      ui_(engine.pluginApiShared()),
      directives_(engine.pluginDirectives()),
      guiThread_(std::this_thread::get_id()) {
    // THE PLUGIN UI'S APPLIER: the engine applies a directive through it inline while it runs on this thread, and posts it for the frame's
    // head to apply while its control thread does (Engine::setPluginUiApplier). Installed through attachPluginUi, which also lets go of what
    // the engine's own plugin UI built before there was anyone to build it for and posts the current state, so a window that arrives after
    // the first scan misses no instrument.
    connected_ = engine_.attachPluginUi([this](const cc::PluginDirective& d) { apply(d); });
}

PluginUiPortImpl::~PluginUiPortImpl() {
    // THE OWNER DISCONNECTS FIRST (AppWindow::~AppWindow does, as its last act): the applier's steps reach back into the window's members, which are destroyed
    // before this object is when the window goes, so an applier left installed for even that long is a pointer into a half-destroyed window. Said, because it is a
    // fault in the owner and not in the engine - and disconnected all the same.
    if (connected_) {
        cascade::core::diagWarnf("plugin ui: the port was destroyed while still connected - its owner should disconnect it first");
    }
    disconnect();
}

void PluginUiPortImpl::touch() const {
    if (std::this_thread::get_id() != guiThread_) { offThread_.fetch_add(1, std::memory_order_relaxed); }
}

const std::vector<cc::HostTrack>& PluginUiPortImpl::tracks() const {
    touch();
    return ui_.tracks();
}
const std::vector<cc::HostTrack>& PluginUiPortImpl::unplacedTracks() const {
    touch();
    return ui_.unplacedTracks();
}
const std::vector<cc::HostPath>& PluginUiPortImpl::paths() const {
    touch();
    return ui_.paths();
}
const std::vector<cc::HostPanel>& PluginUiPortImpl::panels() const {
    touch();
    return ui_.panels();
}
const std::vector<cc::HostInstrument>& PluginUiPortImpl::instruments() const {
    touch();
    return ui_.instruments();
}
const std::vector<std::string>& PluginUiPortImpl::trackPluginNames() const {
    touch();
    return ui_.trackPluginNames();
}
bool PluginUiPortImpl::altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg, double& outAltM) {
    touch();
    return ui_.altitudeNear(plugin, id, latDeg, lonDeg, outAltM);
}
void PluginUiPortImpl::poll() {
    touch();
    ui_.poll();
}

void PluginUiPortImpl::pollImages(std::vector<cc::HostImage>& out) {
    touch();
    // THE PICTURES THE WINDOW DRAWS ARE READ FROM THE ENGINE'S ONE ACCUMULATOR (stage 5e: a decoder offers a picture once, so only one thing may
    // poll the runner - the engine's image feed, which also encodes them for the web page).
    engine_.servers().images().copyTo(out);
}

void PluginUiPortImpl::applyNewestDirective() {
    touch();
    (void)directives_.applyNewest([this](const cc::PluginDirective& d) { apply(d); });
}

void PluginUiPortImpl::setFrontEnd(const cc::PluginUiFrontEnd& f) {
    touch();
    // THE TRACK FEED IS COPIED, NOT SHARED. The window refills the object it passes the moment this returns; the pass reads the engine's
    // copy for as long as it likes. (A pointer to the caller's object here would be a data race the pass loses.)
    FrontEndFacts facts;
    if (!f.tunerDisplayStyle.empty()) { facts.tunerDisplayStyle = f.tunerDisplayStyle; }
    facts.basemap = f.basemap;
    if (f.hasTrackFeed) { facts.trackFeed = std::make_shared<const TrackFeed>(f.trackFeed); }
    facts.pluginTestReport = f.pluginTestReport;
    facts.frame = f.frame;
    if (f.pluginStatusReport) {
        auto st = std::make_shared<PluginStatusFacts>();
        st->ui = f.pluginUi;
        st->images = f.pluginImages;
        facts.pluginStatus = std::move(st);
    }
    engine_.setFrontEndFacts(std::move(facts));
}

// The basemap's tiles, the one thing the window still serves the page (stage 5e). The web server's tile store is thread-safe; this is the
// window's frame draining the wants and publishing the answers, with the bound the frame has always had.
void PluginUiPortImpl::serveTiles(bool clearFirst, bool serve, std::size_t maxTiles, const cc::WebTileSource& source) {
    touch();
    cascade::net::WebServer& web = engine_.servers().webServer();
    if (clearFirst) { web.clearTiles(); }
    if (!serve) { return; }
    const std::vector<cascade::net::WebServer::TileRequest> wants = web.takePendingTileRequests();
    std::size_t served = 0;
    for (const cascade::net::WebServer::TileRequest& r : wants) {
        if (served >= maxTiles) { break; }
        std::vector<std::uint8_t> bmp;
        switch (source(cc::WebTileRequest{r.z, r.x, r.y}, bmp)) {
            case cc::WebTileAnswer::Pending:
                continue;  // asking is what started the plugin fetching; the page's retry lands after it has had time to finish
            case cc::WebTileAnswer::Missing: bmp.clear(); break;  // published empty: "the plugin said the tile will never exist"
            case cc::WebTileAnswer::Ready: break;
        }
        web.setTile(r.z, r.x, r.y, std::move(bmp));
        ++served;
    }
}

void PluginUiPortImpl::release() {
    touch();
    // The release directive the engine posts before it unmaps, applied by the interface itself, now: the window's step (fold the map pages),
    // every instance destroyed, the window's second step (let go of the basemap and the track-info client).
    cc::PluginDirective d;
    d.release = true;
    apply(d);
}

void PluginUiPortImpl::disconnect() {
    if (connected_) {
        connected_ = false;
        engine_.setPluginUiApplier({});
        // (one line on every path that ends an interface: the run's normal end, a window that could not be created, a constructor that threw, the
        // software-GL relaunch - the tests that drive those paths read it)
        cascade::core::diagLogf("plugin ui: the port is disconnected - the engine drives its own plugin UI again");
    }
}

void PluginUiPortImpl::apply(const cc::PluginDirective& d) {
    cc::PluginDirectiveFacts f;
    f.release = d.release;
    f.lifeOnly = d.lifeOnly;
    f.basemap = d.basemap;
    f.trackInfo = d.trackInfo;
    if (steps_.before) { steps_.before(f); }
    ui_.apply(d);
    if (steps_.after) { steps_.after(f); }
}

std::unique_ptr<cc::PluginUiPort> attachPluginUiPort(Engine& engine, cc::PluginUiPort::Steps steps) {
    auto port = std::make_unique<PluginUiPortImpl>(engine, std::move(steps));
    if (!port->attached()) { return nullptr; }
    return port;
}

cc::PluginUi& rawPluginUi(cc::PluginUiPort& port) { return static_cast<PluginUiPortImpl&>(port).ui(); }
PluginUiPortImpl& rawPluginUiPort(cc::PluginUiPort& port) { return static_cast<PluginUiPortImpl&>(port); }

}  // namespace cascade::engine
