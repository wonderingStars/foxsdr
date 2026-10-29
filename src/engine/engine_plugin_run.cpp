// engine_plugin_run.cpp - WHEN A FITTED PLUGIN RUNS, in the engine (stub campaign B2, PLUGIN-RUN; master 0.99.73, core/plugin_run.hpp).
//
// THE FEATURE. A plugin set to AUTO runs while something is USING it - one of its windows or its map is open, the radar scope shows, a
// patch node names it, it holds the speakers, the Decoder output window is open and it is a text decoder, a browser is watching it, its
// output is wired to a Text sink - and goes dormant (no instance, no samples, no polling) 30 seconds after the last of those ends. A plugin
// the user pinned (ALWAYS: "keep running", a START or a preset press) runs as every plugin used to; a STOPPED one runs nowhere. Thirty
// fitted plugins used to be thirty decoders fed on every block and thirty polls on every frame (report 12CF, 110 to 126 starved audio
// callbacks a minute); this is what ends it.
//
// WHERE IT LIVES NOW. Master decided it in the window, once a frame (AppWindow::updatePluginLifecycle), from the window's own state. The
// decision touches the DSP side (PluginRunner::startPlugin / stopPlugin: the decoders) and the GUI side (PluginUi: the track sources,
// panels and instruments), and the headless engine has to make it too - so it is the ENGINE's, once a pass, from the signals the engine
// knows (a patch node, the speakers, a text sink, a browser session, a standing duty) and the ones only a window can see (a window shown,
// a map page, the radar scope, the Decoder output window), which the window REPORTS with FOXAPP_OP_PLUGIN_USE, on change only.
//
// ONE DIRECTIVE, NO SECOND APPLIER. The GUI half is told through the directive path it already has (core/plugin_directive.hpp): a rebuild
// carries the dormant set beside the stop set, and a wake or a sleep of ONE plugin is a LIFE-ONLY directive - the same struct, posted by
// the same link, applied by the same PluginUi::apply - whose plugins/stopped/dormant are the state, not an event, so a newer directive
// replacing an older one loses nothing (the link merges a life-only into what still waits; see PluginDirectiveLink::post).
//
// THE FIRST FRAME OF A WAKE. The window's signal reaches the engine one pass late by design (docs/engine-stage4.md, decision 2: the window
// reads what a pass published and its command is applied by the NEXT pass). Nothing else may be late: commands are applied at the head
// of the pass (pumpFrameBegin), the lifecycle runs in the same pass's plugin phase, and the wake creates the instance there - the runner's
// startPlugin takes the rate and centre of the last rebuild/retune and the DSP thread feeds it from the next block it delivers. So the
// first block after the pass that applied the signal is the plugin's first block (tests/test_engine_api_03.cpp, group N).
//
// A WINDOW-LESS HOST RUNS EVERY PLUGIN (owner decision, 2026-10-26): with no front end installed (no applier: the headless engine, a Pi
// serving the web page) nothing could ever report a window, so AUTO would mean "never wake" and the decoders of a service would be
// silently starved. Such a host treats AUTO as pinned - today's behaviour, and what every test that builds an Engine alone relies on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"
#include "core/plugin_run.hpp"

namespace cascade::engine {

using cascade::core::PluginLife;
using cascade::core::PluginLifeStep;
using cascade::core::PluginRun;
using cascade::core::PluginUse;

namespace {

// The hysteresis: 30 s, or FOXSDR_DORMANT_AFTER_MS for a test that cannot wait that long (docs/DIAGNOSTICS.md). A value that is not a
// positive whole number of at most an hour is ignored.
std::int64_t dormantAfterMsFromEnvironment() {
    const char* s = std::getenv("FOXSDR_DORMANT_AFTER_MS");
    if (s == nullptr || *s == '\0') { return cascade::core::kPluginDormantAfterMs; }
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0 || v > 3600000) { return cascade::core::kPluginDormantAfterMs; }
    return static_cast<std::int64_t>(v);
}

std::int64_t pluginClockMs() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// --- how a plugin runs: the durable state --------------------------------------------------------------------------------------

PluginRun Engine::pluginRunModeById(const std::string& id) const {
    if (id.empty()) { return PluginRun::Auto; }
    const auto it = pluginRun_.find(id);
    return it == pluginRun_.end() ? PluginRun::Auto : it->second;
}

PluginRun Engine::pluginRunMode(const std::string& pluginKey) const {
    return pluginRunModeById(cascade::core::pluginRunId(pluginKey));
}

bool Engine::pluginIsStopped(const std::string& pluginKey) const {
    // An empty key is what a record with no path produces; it must never match, or one stray entry would stop every path-less plugin at
    // once.
    if (pluginKey.empty()) { return false; }
    // A plugin the guard switched off is stopped in every sense that matters (no instance anywhere, no basemap, no track info) - and a
    // person's START cannot bring it back; only RE-ENABLE can.
    if (pluginIsQuarantinedId(cascade::core::pluginRunId(pluginKey))) { return true; }
    return pluginRunMode(pluginKey) == PluginRun::Stopped;
}

std::vector<std::string> Engine::stoppedPluginIds() const {
    std::vector<std::string> out;
    for (const auto& [id, mode] : pluginRun_) {
        if (mode == PluginRun::Stopped) { out.push_back(id); }
    }
    // ...and the ones the guard switched off: the runner, the API core and the interface's directive all take "stopped" as their one word for
    // "no instance of this plugin, anywhere".
    for (const std::string& id : quarantinedIds_) {
        if (std::find(out.begin(), out.end(), id) == out.end()) { out.push_back(id); }
    }
    return out;
}

bool Engine::pluginShouldRunById(const cascade::core::LoadedPlugin& p, const std::string& id) const {
    if (pluginIsQuarantinedId(id)) { return false; }
    switch (pluginRunModeById(id)) {
        case PluginRun::Stopped: return false;
        case PluginRun::Always: return true;
        case PluginRun::Auto: break;
    }
    // A module with nothing to start has no run state to speak of, and one that is in use for as long as it is fitted (a processor in
    // the audio chain) is never put to sleep.
    if (!cascade::core::pluginCapsHaveLifecycle(p.capabilities) || cascade::core::pluginCapsStandingDuty(p.capabilities)) { return true; }
    // A WINDOW-LESS HOST TREATS AUTO AS PINNED (see the head of this file).
    if (!frontEndPresent()) { return true; }
    const auto it = pluginLife_.find(id);
    return it != pluginLife_.end() && it->second.running;
}

bool Engine::pluginShouldRun(const cascade::core::LoadedPlugin& p) const {
    return pluginShouldRunById(p, cascade::core::pluginRunId(cascade::core::pluginKey(p)));
}

bool Engine::pluginIsIdle(const std::string& pluginKey) const {
    if (pluginKey.empty()) { return false; }
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded || cascade::core::pluginKey(p) != pluginKey) { continue; }
        return pluginRunMode(pluginKey) == PluginRun::Auto && !pluginShouldRun(p);
    }
    return false;
}

std::size_t Engine::idleDecoderCount() const {
    std::size_t n = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder == nullptr && p.iqDecoder == nullptr && p.imageDecoder == nullptr) { continue; }
        if (pluginIsIdle(cascade::core::pluginKey(p))) { ++n; }
    }
    return n;
}

void Engine::pushPluginHoldSets() {
    // THE TWO SETS THE REBUILDS OBEY, by plugin id: who the user stopped, and who is set to AUTO and asleep. A plugin in the first is held
    // by its stop (and has no run state to speak of); one in the second is held because nothing is using it. A module with nothing to
    // start is in neither. The runner and the API core take them here; the plugin UI's copy rides in the next directive (pluginDormantIds_
    // is what refreshPluginRunner and the life-only directive carry).
    std::vector<std::string> stopped = stoppedPluginIds();
    std::vector<std::string> dormant;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        const std::string key = cascade::core::pluginKey(p);
        if (key.empty() || pluginIsStopped(key)) { continue; }
        if (!cascade::core::pluginCapsHaveLifecycle(p.capabilities)) { continue; }
        if (!pluginShouldRun(p)) { dormant.push_back(cascade::core::pluginRunId(key)); }
    }
    pluginRunner_.setStopped(stopped);
    pluginApi_->setStopped(stopped);
    pluginRunner_.setDormant(dormant);
    pluginDormantIds_ = std::move(dormant);
}

// --- the config: pluginRun, and the one-time migration of the old stop list -----------------------------------------------------
// (Written back by Engine::fillConfig itself, in plain assignments: tests/test_engine_main_config_roundtrip.cpp holds the list of keys the
// engine writes against the text of that function.)

void Engine::restorePluginRun(const cascade::core::AppConfig& cfg) {
    // The old stop list was kept by module FILE NAME, which carries the version, so a stop was lost the day the plugin updated; each entry
    // becomes pluginRun[<id>] = stopped here, by id, and fillConfig saves the list empty from then on. An id the map already names keeps
    // what the map says (a config written by this build and then read by an older one and back). Every other plugin is AUTO, which is the
    // absence of an entry: nothing the user is looking at stops, because nothing the user is looking at is open at launch.
    pluginRun_.clear();
    for (const auto& [id, word] : cfg.pluginRun) {
        PluginRun r = PluginRun::Auto;
        // Keep an explicit AUTO long enough to win over a stale legacy stop below. fillConfig omits AUTO again when it writes the file.
        if (cascade::core::parsePluginRun(word, r)) { pluginRun_[id] = r; }
    }
    for (const std::string& file : cfg.pluginsStopped) {
        const std::string id = cascade::core::pluginRunId(file);
        if (!id.empty() && pluginRun_.find(id) == pluginRun_.end()) { pluginRun_[id] = PluginRun::Stopped; }
    }
}

void Engine::chooseBasemapAndTrackInfo(const CascadeBasemapApi*& basemap, const CascadeTrackInfoApi*& trackInfo, std::string* basemapPlugin,
                                       std::string* trackInfoPlugin) const {
    // THE BASEMAP, if a plugin supplies one. The first loaded plugin declaring the capability wins: two basemaps cannot both be the map, and
    // picking silently by load order is more predictable than picking by some quality the user cannot see. A STOPPED plugin is skipped, and
    // that is the whole of what "stopped" means for a basemap: the map falls back to its built-in coastlines rather than keeping tiles from
    // a plugin the user has switched off. A DORMANT one is not (a basemap has no instance to create or poll). Track enrichment, by the same
    // first-wins rule and for the same reason.
    basemap = nullptr;
    trackInfo = nullptr;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.basemap != nullptr && !pluginIsStopped(cascade::core::pluginKey(lp))) {
            basemap = lp.basemap;
            if (basemapPlugin != nullptr) { *basemapPlugin = cascade::core::pluginKey(lp); }
            break;
        }
    }
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.trackInfo != nullptr && !pluginIsStopped(cascade::core::pluginKey(lp))) {
            trackInfo = lp.trackInfo;
            if (trackInfoPlugin != nullptr) { *trackInfoPlugin = cascade::core::pluginKey(lp); }
            break;
        }
    }
}

// --- starting and stopping ONE plugin's instances ------------------------------------------------------------------------------

void Engine::postPluginLife() {
    // The same directive the rebuild posts, with no rebuild in it: what the plugin UI is to HOLD (the stop set and the dormant set) and the
    // plugins to hold it over. The basemap and the track-info client are chosen again, because a stop or a start changes who may supply
    // them (a stopped plugin is skipped, a dormant one is not).
    cascade::core::PluginDirective d;
    d.lifeOnly = true;
    d.plugins = pluginHost_.plugins();
    d.stopped = stoppedPluginIds();
    d.dormant = pluginDormantIds_;
    chooseBasemapAndTrackInfo(d.basemap, d.trackInfo, &d.basemapPlugin, &d.trackInfoPlugin);
    if (!deliverPluginDirective(std::move(d))) {
        // Not an error (nothing is being unmapped), but a wake whose GUI half is late: the decoder below is created without waiting.
        cascade::core::diagLogf(
            "plugins: the interface had not applied the plugin directive (a plugin woken or put to sleep; it is not framing); the runner goes ahead without it");
    }
}

void Engine::startPluginInstances(const cascade::core::LoadedPlugin& p) {
    // ONE PLUGIN'S INSTANCES, ALONE (0.99.73). The UI half attaches its host client and creates its track source, panel and instrument
    // (through the directive, FIRST - the ABI promises attach() before any capability's create()); the runner creates its decoders
    // against the rates and centre it was last built for. Nothing else is rebuilt, so the other plugins keep their state and the stream
    // keeps its epoch.
    //
    // NO RECEIVER DECODERS WHILE THE PATCH RUNS (see refreshPluginRunner): the patch has its own instances of these plugins, and a second
    // set fed the generator's noise would overwrite what the patch finds. Stopping the patch rebuilds the receiver's set from the run
    // states.
    pushPluginHoldSets();
    postPluginLife();
    if (!patchRunning_) { (void)pluginRunner_.startPlugin(p); }
    // The mute snapshot carries "is it actually decoding", and that has just changed.
    rebuildMuteStates();
}

void Engine::stopPluginInstances(const std::string& pluginKey) {
    // A PLUGIN BEING STOPPED ALWAYS UNKEYS (stub campaign B13): the key goes up and the window closes before its instances are destroyed.
    txPluginModuleGoing(pluginKey, "the plugin that held the key was stopped");
    pluginRunner_.stopPlugin(pluginKey);
    pushPluginHoldSets();
    postPluginLife();
    rebuildMuteStates();
}

// --- the run state's one door ---------------------------------------------------------------------------------------------------

void Engine::setPluginRun(const std::string& pluginKey, PluginRun mode) {
    const std::string id = cascade::core::pluginRunId(pluginKey);
    if (id.empty()) { return; }
    const PluginRun was = pluginRunModeById(id);
    if (mode == PluginRun::Auto) {
        pluginRun_.erase(id);  // AUTO is the absence of an entry
    } else {
        pluginRun_[id] = mode;
    }
    const cascade::core::LoadedPlugin* found = nullptr;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && cascade::core::pluginKey(p) == pluginKey) {
            found = &p;
            break;
        }
    }
    PluginLife& life = pluginLife_[id];
    const std::int64_t nowMs = pluginClockMs();
    switch (mode) {
        case PluginRun::Stopped:
            life.running = false;
            if (found != nullptr && was != PluginRun::Stopped) {
                stopPluginInstances(pluginKey);
            } else {
                pushPluginHoldSets();
                postPluginLife();
            }
            break;
        case PluginRun::Always:
            life.running = true;
            life.lastWantedMs = nowMs;
            if (found != nullptr) {
                startPluginInstances(*found);
            } else {
                pushPluginHoldSets();
            }
            break;
        case PluginRun::Auto:
            // UN-PINNED (or un-stopped): the plugin is left as it is. One that was running goes on until nothing has wanted it for the
            // hysteresis - counted from now, so unticking "keep running" does not put a plugin out the very next pass. One that was
            // stopped stays asleep until something wants it.
            if (was == PluginRun::Stopped) { life.running = false; }
            life.lastWantedMs = nowMs;
            pushPluginHoldSets();
            postPluginLife();
            break;
    }
}

void Engine::recordPluginStopped(const std::string& pluginKey, bool stopped) {
    const std::string id = cascade::core::pluginRunId(pluginKey);
    if (id.empty()) { return; }
    // A STOP IS STOPPED; A START IS ALWAYS (0.99.73): pressing a preset, or START, is the deliberate "run this", and it must outlast the
    // window being closed - so it is the pinned state, not AUTO. Only recorded here: the instances are created by the rebuild that the
    // caller (applyPluginPreset) follows with, or by the next pass's lifecycle step, which finds an ALWAYS plugin that is not running and
    // wakes it.
    if (stopped) {
        pluginRun_[id] = PluginRun::Stopped;
        pluginLife_[id].running = false;
    } else {
        pluginRun_[id] = PluginRun::Always;
    }
    // Down into the runner and the API core immediately. refreshPluginRunner pushes them again before every rebuild, but a caller that
    // only records (the preset path does) still leaves the live objects agreeing with the durable state.
    pushPluginHoldSets();
    // The mute snapshot holds the same running state and is read every pass, so it has to follow here too, not only at the next rebuild.
    rebuildMuteStates();
}

void Engine::setPluginStopped(const std::string& pluginKey, bool stopped) {
    // ONE PLUGIN'S INSTANCES, NOT A WHOLESALE REBUILD (0.99.73). This used to record the stop and rebuild everything, "so the retune grant,
    // the basemap, the track-info client and the panel windows would be wired up in one place". They still are: setPluginRun does the part
    // that belongs to this plugin - its instances, its mute state, the basemap and track-info choice - and the other plugins keep their
    // state, and the stream its epoch.
    setPluginRun(pluginKey, stopped ? PluginRun::Stopped : PluginRun::Always);

    // ONLY ON A START, and only through THIS path. This is the "Start" key on the fitted-modules row, which is presently the sole place a
    // plugin transitions from stopped to running - a preset BUTTON also starts a stopped plugin (applyPluginPreset's own
    // recordPluginStopped(..., false)) but calls recordPluginStopped directly rather than through here, precisely so an explicit preset
    // press is never second-guessed by this. See maybeAutoPresetOnStart's own comment for what "already inside" means.
    if (!stopped) { maybeAutoPresetOnStart(pluginKey); }
}

// --- the signals ----------------------------------------------------------------------------------------------------------------

void Engine::applyPluginUse(const std::string& text) {
    // The window's table of what it shows, by module file name: one line "<file>\t<bits>" per plugin with at least one signal, replacing
    // the last table whole (a plugin absent from it has none). Unknown bits are dropped; a line that does not parse is skipped, never an
    // error - a report of what is on screen cannot be wrong in a way worth refusing the rest of it for.
    pluginUseReported_.clear();
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) { end = text.size(); }
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) { continue; }
        char* stop = nullptr;
        const unsigned long bits = std::strtoul(line.c_str() + tab + 1, &stop, 10);
        const std::uint32_t known = static_cast<std::uint32_t>(bits) & FOXAPP_PLUGIN_USE_ALL;
        if (stop == line.c_str() + tab + 1 || known == 0u) { continue; }
        const std::string id = cascade::core::pluginRunId(line.substr(0, tab));
        if (!id.empty()) { pluginUseReported_[id] |= known; }
    }
}

PluginUse Engine::pluginUseOf(const cascade::core::LoadedPlugin& p, const std::string& playingKey, std::size_t browserSessions) const {
    const std::string key = cascade::core::pluginKey(p);
    return pluginUseOfKeyed(p, key, cascade::core::pluginRunId(key), playingKey, browserSessions);
}

PluginUse Engine::pluginUseOfKeyed(const cascade::core::LoadedPlugin& p, const std::string& key, const std::string& id,
                                   const std::string& playingKey, std::size_t browserSessions) const {
    // THE SIGNALS AND NOTHING ELSE (core/plugin_run.hpp). Coverage accumulation, the web snapshot and the aircraft-info lookups are
    // passive consumers and do not appear here: a user who wants background feeding ticks "keep running". (The pass hands in the plugin's
    // key and id from the lists' cache: a settled pass allocates nothing.)
    PluginUse u;
    // THE WINDOW'S, reported by it (FOXAPP_OP_PLUGIN_USE) and held to what the module can actually show: a bit for a thing the plugin has
    // none of is the window's mistake and wakes nothing.
    std::uint32_t reported = 0u;
    if (const auto it = pluginUseReported_.find(id); it != pluginUseReported_.end()) { reported = it->second; }
    if ((reported & FOXAPP_PLUGIN_USE_WINDOW) != 0u && (p.imageDecoder != nullptr || p.panel != nullptr || p.instrument != nullptr)) {
        u.window = true;
    }
    if (p.trackSource != nullptr) {
        u.mapPage = (reported & FOXAPP_PLUGIN_USE_MAP_PAGE) != 0u;
        u.radarScope = (reported & FOXAPP_PLUGIN_USE_RADAR_SCOPE) != 0u;
    }
    u.decoderOutput = (reported & FOXAPP_PLUGIN_USE_DECODER_OUTPUT) != 0u && (p.decoder != nullptr || p.iqDecoder != nullptr);
    // THE ENGINE'S OWN. A running patch node names it - by file name, as the node stores it - and, of those, a decoder whose output is
    // wired to a Text sink (the only way its lines are shown).
    if (patchRunning_ && !key.empty()) {
        for (const cascade::core::patch::Node& n : patchGraph_.nodes()) {
            if (n.kind != cascade::core::patch::NodeKind::Decoder || !n.on || n.plugin != key) { continue; }
            u.patchNode = true;
            if (patchDecoderIsShown(n.id)) { u.textSink = true; }
        }
    }
    // A manual tune offers an audio-output decoder its first chance to play. Once it does, only the actual holder remains wanted by
    // playback; the tune signal is consumed after this pass's lifecycle step.
    u.playingAudio = p.audioOut != nullptr && (audioOutputTunePending_ || (!playingKey.empty() && playingKey == key));
    // A browser is watching, and this plugin publishes what a browser shows: targets or pictures.
    u.browserSession = browserSessions > 0u && (p.trackSource != nullptr || p.imageDecoder != nullptr);
    u.standing = cascade::core::pluginCapsStandingDuty(p.capabilities);
    return u;
}

// --- once a pass ----------------------------------------------------------------------------------------------------------------

void Engine::updatePluginLifecycle() {
    // First, before any wake is decided: a plugin the guard switched off since the last pass is put down (and its key let go) now.
    applyPluginQuarantines();
    if (pluginDormantAfterMs_ < 0) { pluginDormantAfterMs_ = dormantAfterMsFromEnvironment(); }
    refreshApiPluginKeys();
    const std::vector<cascade::core::LoadedPlugin>& plugins = pluginHost_.plugins();
    const std::int64_t nowMs = pluginClockMs();
    // Read once for the pass: the speakers' holder takes the runner's lock (and only a module with an audio-output table can be it), and
    // the session count the web server's.
    std::string playingKey;
    for (const cascade::core::LoadedPlugin& p : plugins) {
        if (p.loaded && p.audioOut != nullptr) {
            playingKey = pluginRunner_.playingPluginKey();
            break;
        }
    }
    const std::size_t sessions = servers_.webServer().running() ? servers_.webServer().sessionCount() : 0u;
    const bool windowless = !frontEndPresent();

    struct Change {
        std::size_t index;
        PluginLifeStep step;
    };
    std::vector<Change> changes;  // (empty on the settled pass: nothing allocates)
    for (std::size_t i = 0; i < plugins.size() && i < apiPluginKeys_.size(); ++i) {
        const cascade::core::LoadedPlugin& p = plugins[i];
        if (!p.loaded || !cascade::core::pluginCapsHaveLifecycle(p.capabilities)) { continue; }
        const std::string& key = apiPluginKeys_[i];
        if (key.empty()) { continue; }
        const std::string& id = apiPluginIds_[i];
        const PluginRun mode = pluginRunModeById(id);
        const PluginUse use = pluginUseOfKeyed(p, key, id, playingKey, sessions);
        PluginLife& life = pluginLife_[id];
        const PluginLifeStep step =
            cascade::core::stepPluginLife(mode, windowless || cascade::core::pluginWanted(use), nowMs, pluginDormantAfterMs_, life);
        if (step == PluginLifeStep::None) { continue; }
        // A WAKE OR A SLEEP IS LOGGED when the signal made it: a plugin pinned to ALWAYS is started by the user's own press, which has its
        // own line, and has no signal to name.
        if (mode == PluginRun::Auto && !windowless) {
            if (step == PluginLifeStep::Wake) {
                const char* reason = audioOutputTunePending_ && p.audioOut != nullptr && playingKey != key ? "manual tune"
                                                                                                           : cascade::core::pluginUseSignal(use);
                cascade::core::diagLogf("plugin: %s woke - %s", p.name.c_str(), reason);
            } else {
                cascade::core::diagLogf("plugin: %s dormant after 30 s without a use", p.name.c_str());
            }
        }
        changes.push_back({i, step});
    }
    audioOutputTunePending_ = false;
    if (changes.empty()) { return; }

    // THE RUNNER'S HALF OF A SLEEP FIRST (as the window did: the decoders come off before the interface lets go of its end), then the
    // hold sets and the one life directive for the whole pass, then the runner's half of every wake - AFTER the directive, so a plugin's
    // attach() has run before its decoder's create().
    for (const Change& c : changes) {
        if (c.step == PluginLifeStep::Sleep) {
            // A PLUGIN GOING DORMANT ALWAYS UNKEYS (stub campaign B13; a hardening master lacked): the key goes up and the window closes in this pass, before the instances go.
            txPluginModuleGoing(apiPluginKeys_[c.index], "the plugin that held the key went dormant");
            pluginRunner_.stopPlugin(apiPluginKeys_[c.index]);
        }
    }
    pushPluginHoldSets();
    postPluginLife();
    for (const Change& c : changes) {
        if (c.step == PluginLifeStep::Wake && !patchRunning_) { (void)pluginRunner_.startPlugin(plugins[c.index]); }
    }
    rebuildMuteStates();
}

}  // namespace cascade::engine
