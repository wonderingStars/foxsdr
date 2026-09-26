// engine_host.cpp - the headless answers to EngineHost (engine_host.hpp): what
// an engine with no window gets. Clocks from std::chrono, measured from the
// host's construction; nothing to redraw; no store window, so no ADD ALL plan.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_host.hpp"

#include <utility>

#include "engine/tune_control.hpp"

namespace cascade::engine {

EngineHost::EngineHost() : born_(std::chrono::steady_clock::now()) {}
EngineHost::~EngineHost() = default;

bool EngineHost::frameClockRunning() const { return true; }
double EngineHost::frameTimeS() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - born_).count();
}
double EngineHost::wallTimeS() const { return frameTimeS(); }

void EngineHost::pauseWatchdog() {}
void EngineHost::resumeWatchdog() {}

void EngineHost::onDisplayRange(float, float) {}
void EngineHost::onBookmarksChanged() {}
void EngineHost::openPluginWindowsFor(const cascade::core::LoadedPlugin&) {}
void EngineHost::onReceiverPositionApplied(double, double) {}
void EngineHost::onConverterChanged(bool) {}
void EngineHost::onPatchGraphChanged() {}
void EngineHost::onPatchPicture(cascade::core::patch::NodeId, cascade::core::HostImage&&) {}
void EngineHost::onGpsFixApplied() {}
void EngineHost::onCatalogueFetchStarting() {}
void EngineHost::onCatalogueResult() {}
void EngineHost::onAddAllFinished() {}

EngineHost::AddAllChoice EngineHost::planAddAll(bool) {
    AddAllChoice c;
    // FOX_TR_NOOP-free on purpose: this is the engine's own English, shown
    // only by a front end that has no store window to plan with.
    c.blockedReason = "ADD ALL is planned by the store window, and there is none";
    return c;
}

void EngineHost::beforePluginRescan() {}
void EngineHost::onPluginsUnloading() {}
void EngineHost::attachBasemap(const CascadeBasemapApi*) {}
void EngineHost::attachTrackInfo(const CascadeTrackInfoApi*) {}
void EngineHost::showDemonstrationInstrument(const cascade::core::HostInstrument&) {}
std::vector<std::string> EngineHost::drainTrackInfoText() { return {}; }

bool EngineHost::patchPageOpen() const { return false; }
bool EngineHost::webListening() const { return false; }
std::string EngineHost::tunerDisplayStyle() const {
    return cascade::gui::tunerStyleName(cascade::gui::TunerStyle::Nixie);
}
EngineHost::BasemapFacts EngineHost::basemapFacts() const { return {}; }
void EngineHost::enrichWebTrack(cascade::net::RadioStatus::Track&) {}
void EngineHost::fillWebImages(cascade::net::RadioStatus&) {}

}  // namespace cascade::engine
