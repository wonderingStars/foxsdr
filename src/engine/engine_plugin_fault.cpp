// engine_plugin_fault.cpp - the engine's half of the plugin containment (plugin containment P1, docs/engine-stage4.md 6.28; the guard and its
// counters are core/plugin_fault.hpp, the crash-loop breaker is core/plugin_marker.hpp).
//
// WHAT THE ENGINE DOES ABOUT A PLUGIN THAT WAS CONTAINED:
//
//   AT START (beginPluginContainment, before the first scan) the previous run's marker and the strike records are read and planned
//   (planStart). The load gate then refuses, by file name, every plugin the plan skips - a suspect of last session, a plugin disabled by a
//   second strike, or all of them in safe mode - and a site's file that is not the bytes that were installed; a refused module is never
//   mapped. The person is told once (the start notices, as a fact) and re-enables with one key (FOXAPP_OP_PLUGIN_REENABLE).
//
//   WHILE RUNNING (applyPluginQuarantines, once a pass) the containment's events are drained: a plugin that faulted, threw, or never
//   returned (core/plugin_fault.hpp) was switched off by the guard at the moment it happened; here the PASS puts it down - the key it held
//   is let go first (the B13 rule, "a module going away always unkeys"), the runner's instances are removed, and one life directive carries
//   it, as a stopped plugin, to the interface, whose instances of it go the same way. The module stays mapped (core/plugin_host.cpp).
//
//   AT AN ORDERLY STOP (Engine::markCleanExit) the strikes of a plugin that ran clean are taken off and the marker is cleared.
//
// WHO MAY DO WHAT. Quarantining is the guard's alone (nothing a session sends can switch a plugin off), and re-enabling is a LOCAL
// application op that the API table refuses every remote session at submit and again here where it is applied.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"
#include "core/plugin_marker.hpp"
#include "core/plugin_run.hpp"
#include "core/thread_beats.hpp"

namespace cascade::engine {

namespace {

std::string lowerId(const std::string& keyOrId) {
    std::string id = cascade::core::pluginRunId(keyOrId);
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return id;
}

}  // namespace

void Engine::beginPluginContainment(const std::string& settingsDir) {
    if (containment_.started()) { return; }
    containment_.begin(settingsDir, safePlugins_, static_cast<std::int64_t>(std::time(nullptr)));
    ++pluginFaultGen_;
}

void Engine::installPluginLoadGate() {
    pluginHost_.setLoadGate([this](const std::string& fileName, cascade::core::ModuleVerdict& verdict, std::string& sentence) {
        cascade::core::Containment::Gate g;
        if (containment_.gate(fileName, g)) {
            verdict = g.verdict;
            sentence = g.sentence;
            return true;
        }
        // A plugin site's file that is not the bytes the install record vouches for is not loaded (core/plugin_module_check.hpp).
        cascade::core::ModuleCheck tampered;
        if (cascade::core::siteFileTampered(pluginInventory_.plugins, fileName, tampered)) {
            verdict = tampered.verdict;
            sentence = "Not loaded: " + tampered.detail + ".";
            return true;
        }
        return false;
    });
}

bool Engine::pluginIsQuarantinedId(const std::string& runId) const {
    if (quarantinedIds_.empty()) { return false; }   // the settled pass: nothing to search
    return std::find(quarantinedIds_.begin(), quarantinedIds_.end(), runId) != quarantinedIds_.end();
}

std::string Engine::pluginFaultSentence(const std::string& pluginKey) const {
    const std::string id = lowerId(pluginKey);
    if (!pluginIsQuarantinedId(id)) { return std::string(); }
    const cascade::core::fault::Slot* s = cascade::core::fault::slotFor(id);
    const cascade::core::fault::Why why = cascade::core::fault::quarantineWhy(s);
    const char* what = why == cascade::core::fault::Why::Wedge       ? "stopped answering"
                       : why == cascade::core::fault::Why::Exception ? "threw an error"
                                                                     : "faulted";
    return std::string("It ") + what + " in " + cascade::core::fault::entryWord(cascade::core::fault::quarantineEntry(s)) +
           " and was switched off for this session; the rest of FoxSDR carried on. RE-ENABLE it to try it again.";
}

void Engine::applyPluginQuarantines() {
    // The first-pass clock (core/plugin_marker.hpp): an instance that has been alive a few seconds is `running`.
    cascade::core::marker::promoteFirstPass(
        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),
        cascade::core::marker::kFirstPassMs);

    // THE TRIPWIRES (core/plugin_fault.hpp Misuse, plugin containment P2): the first time a plugin breaks each rule of the API it is said once,
    // here, where a lock may be taken - the count itself was a relaxed atomic add on whatever thread the plugin called from. The counts stay
    // in fault::health(); the host has already refused the call with the ABI's own answer.
    for (const cascade::core::fault::MisuseNotice& note : cascade::core::fault::drainMisuseNotices()) {
        cascade::core::diagWarnf("plugin: %s %s (%s); the call was refused or clamped and the session carries on", note.id.c_str(),
                                 note.kind == cascade::core::fault::Misuse::HostReentry
                                     ? "was called by FoxSDR from inside a call into the same plugin, which the plugin ABI promises never to do"
                                     : "broke a rule of the plugin API",
                                 cascade::core::fault::misuseWord(note.kind));
    }

    const std::vector<cascade::core::fault::Event> events = cascade::core::fault::drainEvents();
    if (events.empty()) { return; }
    bool changed = false;
    for (const cascade::core::fault::Event& ev : events) {
        if (ev.kind != cascade::core::fault::Event::Kind::Quarantine) { continue; }
        // The module file(s) of this run id: THE KEY THE REST OF THE ENGINE FILES THE PLUGIN UNDER.
        std::string moduleFile;
        for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
            const std::string key = cascade::core::pluginKey(p);
            if (lowerId(key) == ev.id) {
                moduleFile = key;
                break;
            }
        }
        if (std::find(quarantinedIds_.begin(), quarantinedIds_.end(), ev.id) == quarantinedIds_.end()) { quarantinedIds_.push_back(ev.id); }
        changed = true;
        cascade::core::diagWarnf("plugin: %s was switched off for this session (%s in %s); its instances are being removed", ev.id.c_str(),
                                 cascade::core::fault::whyWord(ev.why), cascade::core::fault::entryWord(ev.entry));
        if (moduleFile.empty()) { continue; }
        // B13: A PLUGIN THAT IS GOING AWAY ALWAYS UNKEYS - first, before anything of it is touched. (If it held the key, the transmitter is
        // un-keyed and the arming window closed; if it did not, this changes nothing.)
        txPluginModuleGoing(moduleFile, "the plugin that held the key was switched off after a fault");
        // ...its decoders out of the runner (nothing of the plugin is called: the guard refuses, and the handles are leaked on purpose)...
        pluginRunner_.stopPlugin(moduleFile);
        // ...and the notice on its row, in the module's own list.
        PluginNotice n;
        n.level = 2;
        n.text = pluginFaultSentence(moduleFile);
        pluginNotices_[moduleFile] = std::move(n);
    }
    if (changed) {
        ++pluginFaultGen_;
        // ONE directive for the interface: the plugin is a stopped plugin from now on (stoppedPluginIds includes it), so the interface's
        // track sources, panels and instruments of it go through the same directive that a stop sends - no second applier.
        pushPluginHoldSets();
        postPluginLife();
        rebuildMuteStates();
    }
}

bool Engine::reenablePlugin(const std::string& pluginKeyOrId, std::string& why) {
    const std::string id = lowerId(pluginKeyOrId);
    if (id.empty()) {
        why = "no plugin named";
        return false;
    }
    bool changed = false;
    // The standing record (a plugin that was skipped at this start, or disabled by two strikes): forgotten as a strike count that blocks,
    // kept as a probation that watches.
    if (containment_.reenable(id)) { changed = true; }
    // The live quarantine (a plugin the guard switched off this session): cleared. The module is still mapped - a quarantined module is
    // never unmapped - so the rescan below finds it and builds it afresh.
    if (cascade::core::fault::release(cascade::core::fault::slotFor(id))) { changed = true; }
    const auto at = std::find(quarantinedIds_.begin(), quarantinedIds_.end(), id);
    if (at != quarantinedIds_.end()) {
        quarantinedIds_.erase(at);
        changed = true;
    }
    for (auto it = pluginNotices_.begin(); it != pluginNotices_.end();) {
        it = lowerId(it->first) == id ? pluginNotices_.erase(it) : std::next(it);
    }
    if (!changed) {
        why = "that plugin is not switched off";
        return false;
    }
    ++pluginFaultGen_;
    cascade::core::diagLogf("plugin: %s re-enabled by the user", id.c_str());
    // A full rescan: a plugin that was never mapped is mapped now, and one that was is built again over the stream it ran on.
    rescanPlugins();
    return true;
}

}  // namespace cascade::engine
