// engine_wire_c.cpp - the engine's side of wiring lane C: the op block 0x8A00-0x8AFF, its facts, its once-a-pass pump.
// The settings registry and the state are in wire_c.{hpp,cpp}; each feature of the lane adds its own file next to this one and one `case` here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "core/plugin_repo_helper.hpp"   // wire-c
#include "core/store_sort.hpp"
#include "engine/wire_c.hpp"

namespace cascade::engine {

namespace wc = cascade::engine::wirec;

wc::State& Engine::wireC() {
    std::lock_guard<std::mutex> lk(wireCMutex_);   // (the first caller may be the control pass or the fact builder: whichever asks first builds it)
    if (!wireC_) {
        const std::filesystem::path folder = wireCFolderForTest_.empty()
            ? std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path()
            : std::filesystem::path(wireCFolderForTest_);
        wireC_ = std::make_unique<wc::State>(folder);
    }
    return *wireC_;
}

std::int32_t Engine::applyWireC(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    (void)res;
    switch (c.op) {
        case FOXAPP_OP_WIREC_SETTING: {
            const std::size_t sep = text.find('\x1F');
            if (sep == std::string::npos || sep == 0) {
                why = "the text is \"<key>\\x1F<value>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            switch (wireC().settings().set(text.substr(0, sep), text.substr(sep + 1), why)) {
                case wc::SetResult::Changed: ++wireCVersion_; return FOXAPI_OK;
                case wc::SetResult::NoChange: return FOXAPI_NO_CHANGE;
                case wc::SetResult::NotFound: return FOXAPI_NOT_FOUND;
                case wc::SetResult::BadValue: return FOXAPI_BAD_ARGUMENT;
                case wc::SetResult::OutOfRange: return FOXAPI_OUT_OF_RANGE;
                case wc::SetResult::SaveFailed: return FOXAPI_FAILED;
            }
            return FOXAPI_FAILED;
        }
        case FOXAPP_OP_STORE_HELPER_CONSENT: {
            if (commandFromRemote_) {
                why = "the plugin store's helper programs are fitted from this computer only";
                return FOXAPI_DENIED;
            }
            for (const cascade::core::PluginCatalogEntry& ce : catalog_) {
                if (cascade::core::PluginRepo::catalogueKey(ce.source, ce.id) != text) { continue; }
                if (!ce.helper.present) {
                    why = "that plugin does not run a separate program";
                    return FOXAPI_NOT_FOUND;
                }
                const bool had = wireCHelperConsent_.count(text) != 0;
                if (c.ival[0] != 0) { wireCHelperConsent_.insert(text); } else { wireCHelperConsent_.erase(text); }
                if ((c.ival[0] != 0) == had) {
                    why = "already so";
                    return FOXAPI_NO_CHANGE;
                }
                ++wireCVersion_;
                return FOXAPI_OK;
            }
            why = "no catalogue entry with that key";
            return FOXAPI_NOT_FOUND;
        }
        default:
            break;
    }
    why = "not an operation of this block";
    return FOXAPI_UNSUPPORTED;
}

void Engine::wireCFacts(const std::function<void(const std::string&, const std::string&)>& fact) {
    wc::State& s = wireC();
    s.settings().each([&fact](const std::string& key, const std::string& value) { fact("wirec." + key, value); });
    fact("wirec.version", std::to_string(wireCVersion()));
    fact("wirec.error", s.settings().error());
    // POWER AND RESOURCES (wire_c_power.hpp). "" is "not available" - a reading that could not be made is never shown as 0.
    // (always published, so a client can read them before the first pass has made the power object; the perf.* facts are read at the call: wireCLiveFact)
    fact("sys.keepawake", s.hasPower() && s.power().keepAwakeHeld() && s.power().keepAwakeActive() ? "1" : "0");
    fact("sys.resume.count", std::to_string(s.resumeActions));
}

// The resource read-out and the reduced-effects switches, read at the call from the sampler's last published figures. "" is "not available" (never 0).
bool Engine::wireCLiveFact(const std::string& key, std::string& value) {
    if (key.rfind("perf.", 0) != 0) { return false; }
    wc::State& s = wireC();
    wc::PerfFacts f;
    if (s.hasPower()) { f = s.power().perf(); }
    if (key == "perf.cpu_percent") { value = f.cpuPercent; }
    else if (key == "perf.memory_mb") { value = f.memoryMb; }
    else if (key == "perf.system_memory_percent") { value = f.systemMemoryPercent; }
    else if (key == "perf.gpu_percent") { value = f.gpuPercent; }
    else if (key == "perf.reduced") { value = f.reduced ? "1" : "0"; }
    else if (key == "perf.switch.ui_frame_cap_hz") { value = std::to_string(f.switches.uiFrameCapHz); }
    else if (key == "perf.switch.waterfall_rows_per_second") { value = std::to_string(f.switches.waterfallRowsPerSecond); }
    else if (key == "perf.switch.animations") { value = f.switches.animations ? "1" : "0"; }
    else if (key == "perf.switch.glow_and_shadows") { value = f.switches.glowAndShadows ? "1" : "0"; }
    else if (key == "perf.switch.three_d_views_live") { value = f.switches.threeDViewsLive ? "1" : "0"; }
    else if (key == "perf.switch.background_map_redraw") { value = f.switches.backgroundMapRedraw ? "1" : "0"; }
    else { return false; }
    return true;
}

std::uint64_t Engine::wireCVersion() {
    return wireCVersion_ + wireC().settings().version();
}

// Anything recording right now: an audio or I/Q take, the IF or multiplex take of any radio, or a scheduled capture that is starting or recording. The state, not the
// start and stop sites, so a take that ends any way at all ends the hold (wire_c_power.hpp).
bool Engine::wireCRecordingBusy() const {
    for (const auto& r : receivers_) {
        if (r && (r->iqRecorder_.recording() || r->audioRecorder_.recording() || r->ifRecorder_.recording() || r->mpxRecorder_.recording())) { return true; }
    }
    for (const auto& [id, rt] : scheduleRt_) {
        (void)id;
        if (rt.state == "recording" || rt.state == "starting") { return true; }
    }
    return false;
}

// What the engine does after the PC wakes: the radios are looked for again (USB ones re-enumerate while it sleeps) and a saved radio that was waiting is tried at
// once, the same two steps the hot-plug notice takes. It does NOT close and reopen a radio that is open and healthy: whether that is wanted is not settled.
void Engine::wireCResumeAction() {
    scanNative();
    scanSoapy();
    if (rx().savedRadioRetryBusy_) {
        rx().savedRadioRetryAttempts_ = 0;
        rx().savedRadioRetryLastFailSec_ = -1.0e9;
    }
}

// A take a plugin asked for: its label first in the file name ("tg-1234_foxsdr_audio_....wav"). Only while a plugin's take is being started.
void Engine::wireCLabelTake(cascade::core::Recorder::OpenRequest& req) {
    if (wireCStartLabel_.empty() || req.path.empty()) { return; }
    const std::filesystem::path p(req.path);
    req.path = (p.parent_path() / (wireCStartLabel_ + "_" + p.filename().string())).string();
}

// THE RECORDER A PLUGIN MAY ASK FOR (row P1). The request was checked against the SETTINGS grant when it was made and is checked again here (a grant taken away, a plugin
// stopped or unloaded in between). The take is the radio's own audio take, started and stopped exactly as the user's Record key does; one take per radio, never one
// the user started (a start while the radio is recording is refused and logged), and a plugin can end only the take it started. A plugin that goes away has its take ended.
void Engine::wireCPluginRecorder() {
    cascade::core::PluginApiCore& api = *pluginApi_;
    std::vector<cascade::core::PluginApiCore::RecorderRequest> reqs;
    std::vector<int> ages;
    for (const WireCDeferredStart& d : wireCDeferred_) {
        reqs.push_back(d.req);
        ages.push_back(d.passes);
    }
    wireCDeferred_.clear();
    api.takeRecorderRequests(reqs);
    ages.resize(reqs.size(), 0);
    for (std::size_t qi = 0; qi < reqs.size(); ++qi) {
        const auto& r = reqs[qi];
        if (!api.recorderRequestStillAllowed(r)) { continue; }
        const std::uint32_t radio = api.clientRadio(r.client);
        if (!receiverLive(radio)) { continue; }
        RadioWork asked(*this, static_cast<std::size_t>(radio) - 1u);
        const std::size_t slot = static_cast<std::size_t>(radio) - 1u;
        if (r.start) {
            if (rx().audioRecorder_.recording() || rx().audioStart_.pending() || wireCPluginTakes_.count(slot) != 0) {
                cascade::core::diagLogf("recorder: a plugin's take was refused, this radio is already recording");
                continue;
            }
            if (takeFinishing(false)) {
                // the previous file is still being closed: the recorder would queue the start and forget the label, so it waits here instead
                if (ages[qi] < 120) { wireCDeferred_.push_back({r, ages[qi] + 1}); }
                else { cascade::core::diagLogf("recorder: a plugin's take was dropped, the previous file was not closed in time"); }
                continue;
            }
            wireCStartLabel_ = r.label;
            const bool started = startAudioRecording();
            wireCStartLabel_.clear();
            if (started) { wireCPluginTakes_[slot] = r.client; }
            else { cascade::core::diagLogf("recorder: a plugin's take could not start"); }
        } else {
            const auto it = wireCPluginTakes_.find(slot);
            if (it != wireCPluginTakes_.end() && it->second == r.client) {
                stopAudioRecording();
                wireCPluginTakes_.erase(it);
            }
        }
    }
    // a take whose plugin has gone (stopped, unloaded, detached), or that the user ended: ended / forgotten
    for (auto it = wireCPluginTakes_.begin(); it != wireCPluginTakes_.end();) {
        const std::size_t slot = it->first;
        const cascade::core::PluginApiCore::RecorderRequest probe{it->second, true, {}};   // (a START is allowed only while the plugin is live, not stopped and holds the grant)
        bool drop = false;
        if (slot >= receivers_.size() || !receivers_[slot]) {
            drop = true;
        } else {
            RadioWork asked(*this, slot);
            if (!api.recorderRequestStillAllowed(probe)) {
                stopAudioRecording();
                drop = true;
            } else if (!rx().audioRecorder_.recording() && !rx().audioStart_.pending()) {
                drop = true;   // the take is over (the user's Stop, or an error)
            }
        }
        it = drop ? wireCPluginTakes_.erase(it) : std::next(it);
    }
}

// THE ORDER OF THE STORE'S LIST (row W9; core/store_sort.hpp): the catalogue's own order by default (today's), or by name, or newest first. The sort is stable and
// total (ties fall back to name, id, site), so a list never shuffles between two builds of the block. The popularity and rating orders need data from a server that
// does not exist, so they are not offered.
std::vector<std::size_t> Engine::wireCStoreOrder() {
    const std::string key = wireC().settings().get("store.sort");
    if (key == "name") { return cascade::core::sortStoreEntries(catalog_, cascade::core::StoreStatsMap{}, cascade::core::StoreSortKey::Name); }
    if (key == "updated") { return cascade::core::sortStoreEntries(catalog_, cascade::core::StoreStatsMap{}, cascade::core::StoreSortKey::Updated); }
    std::vector<std::size_t> idx(catalog_.size());
    for (std::size_t i = 0; i < idx.size(); ++i) { idx[i] = i; }
    return idx;
}

void Engine::pumpWireC() {
    wireCPluginRecorder();
    wc::State& s = wireC();
    wc::Power& p = s.power();
    if (!s.powerStarted) {
        s.powerStarted = true;
        p.startSampler();
        p.startResumeWatch();
    }
    const std::string mode = s.settings().get("perf.mode");
    p.setPerfMode(mode == "on" ? cascade::core::sys::EffectsMode::On : mode == "off" ? cascade::core::sys::EffectsMode::Off : cascade::core::sys::EffectsMode::Auto);
    if (p.keepAwakePass(s.settings().getBool("sys.keep_awake") && wireCRecordingBusy())) { ++wireCVersion_; }
    // WEB: the cross-origin allow-list (net/web_wire_c.hpp). The server takes a new list at any time; a list it refuses is logged and the old one stays.
    {
        const std::string origins = s.settings().get("web.cors_origins");
        if (origins != s.corsApplied) {
            std::vector<std::string> lines;
            std::size_t from = 0;
            while (from <= origins.size()) {
                std::size_t to = origins.find('\n', from);
                if (to == std::string::npos) { to = origins.size(); }
                std::string line = origins.substr(from, to - from);
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                if (!line.empty()) { lines.push_back(line); }
                from = to + 1;
            }
            std::string err;
            if (servers().webServer().setCors(lines, err)) { s.corsApplied = origins; }
            else { s.corsApplied = origins; cascade::core::diagLogf("web: the cross-origin list was refused (%s)", err.c_str()); }
        }
    }
    if (p.takeResume()) {
        if (s.settings().getBool("sys.resume_reopen")) {
            cascade::core::diagLogf("power: the PC woke from sleep; looking for the radios again");
            wireCResumeAction();
            ++s.resumeActions;
        }
        ++wireCVersion_;
    }
}

}  // namespace cascade::engine
