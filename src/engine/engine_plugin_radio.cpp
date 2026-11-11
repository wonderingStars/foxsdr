// engine_plugin_radio.cpp - PLUGINS PER RADIO (multi-radio engine, stage 6; docs/multi-radio-records/engine-stages-5-6.md).
//
// Until this stage the decoders ran on radio 1 only. Now every receiver has its own PluginRunner (the first radio's is the engine's pluginRunner_, as it always was; radios 2 to 8
// get one when they are added and lose it when they are removed), and each plugin runs on ONE radio, the one the user chooses: a decoder instance is fed the I/Q, the audio and
// the decoder taps of its radio and of no other, its host calls are answered from its radio (get_state, the controls it queues, the centre it asks for), and a plugin that throws
// or hangs is contained exactly as before - the containment (core/plugin_fault.hpp) is per plugin slot, not per radio, so a fault on radio 2 trips the plugin and leaves radio 1's
// audio and plugins as they were.
//
// THE CHOICE. `pluginRadio_` (plugin id -> radio number 2..8; no entry = radio 1) is the durable state; it is saved in the settings file (`pluginRadio`) and restored after the radios
// are. FOXAPI_OP_PLUGIN_RADIO sets it (text: the module file name; ival[0]: the radio; ival[1] = 1: and start the plugin); fact.plugin.<module file>.radio reads it, and
// fact.radio.<n>.plugins lists the decoders running on radio n. A radio that is removed sends its plugins back to radio 1 and leaves them STOPPED (the design is silent; the user
// put them on a radio that is gone, and a decoder that starts by itself on another radio's signal would be a surprise). A number for a radio that is not open reads as radio 1.
//
// WHAT STAYS ON RADIO 1, on purpose: a plugin that transmits (CASCADE_CAP_TX_AUDIO) - the transmitter has one audio ring and one writer, and it follows radio 1's frequency (or the
// radio that is the Pluto, transmitRadioSlot); the audio mute by preset (the mute machine is one latch with one popup, and it mutes radio 1); the patch page's own decoder
// instances (they have their own runners and are not receivers).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>

#include "core/diag_log.hpp"
#include "core/plugin_run.hpp"
#include "engine/engine.hpp"
#include "engine/transmit_radio.hpp"

namespace cc = cascade::core;

namespace cascade::engine {

// --- who runs where ----------------------------------------------------------------------------------------------------------------------

std::uint32_t Engine::radioOfPlugin(const std::string& moduleKeyOrId) const {
    if (moduleKeyOrId.empty()) { return 1u; }
    const std::string id = cc::pluginRunId(moduleKeyOrId);
    const auto it = pluginRadio_.find(id);
    if (it == pluginRadio_.end() || it->second < 2) { return 1u; }
    // A plugin that transmits is on radio 1, whatever the map says.
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && p.txAudio != nullptr && cc::pluginRunId(cc::pluginKey(p)) == id) { return 1u; }
    }
    return receiverLive(static_cast<std::uint32_t>(it->second)) ? static_cast<std::uint32_t>(it->second) : 1u;
}

std::vector<cc::LoadedPlugin> Engine::pluginsOnRadio(std::size_t slot) const {
    std::vector<cc::LoadedPlugin> out;
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        if (radioOfPlugin(cc::pluginKey(p)) == static_cast<std::uint32_t>(slot) + 1u) { out.push_back(p); }
    }
    return out;
}

void Engine::rebuildRunnerOf(std::size_t slot) {
    if (slot >= kMaxReceivers || !receivers_[slot]) { return; }
    SlotScope scope(this, slot);
    cc::PluginRunner& r = runnerOf(slot);
    cc::Pipeline& pipe = rx().pipeline_;
    pipe.setPluginRunner(nullptr);
    if (patchRunning_) {
        // THE PATCH HAS THE DECODERS (0.99.18; see refreshPluginRunner): none on any receiver while it runs.
        r.clear();
    } else {
        r.rebuild(pluginsOnRadio(slot), cc::Pipeline::kAudioRateHz, pipe.inputRateHz(), pipe.activeSource().centerFrequencyHz());
        pipe.setPluginRunner(&r);
    }
}

void Engine::rebuildAllRunners() {
    // The API core knows each module's radio by its file name (a rescan can bring a new file name for the same plugin id).
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        const std::string key = cc::pluginKey(p);
        if (!key.empty()) { pluginApi_->setClientRadio(key, radioOfPlugin(key)); }
    }
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) { rebuildRunnerOf(slot); }
}

void Engine::createRadioRunner(std::size_t slot) {
    if (slot == 0u || slot >= kMaxReceivers || radioRunners_[slot]) { return; }
    auto r = std::make_unique<cc::PluginRunner>();
    r->setRadio(static_cast<std::uint32_t>(slot) + 1u);
    r->setStreamClock(pluginApi_->streamClock(static_cast<std::uint32_t>(slot) + 1u));
    radioRunners_[slot] = std::move(r);
    pushPluginHoldSets();   // the stop and dormant sets, so a plugin the user stopped is not started here
    rebuildRunnerOf(slot);
}

void Engine::destroyRadioRunner(std::size_t slot) {
    if (slot == 0u || slot >= kMaxReceivers) { return; }
    const std::uint32_t radio = static_cast<std::uint32_t>(slot) + 1u;
    // THE PLUGINS THAT RAN ON THIS RADIO go back to radio 1 and stay stopped. Their instances die with the runner (cleared below, with the pipeline detached first); the
    // durable choice is dropped and the stop is recorded the way the user's own is (setPluginRun), which also tells the API core and the plugin UI.
    std::vector<std::string> moved;
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        const std::string key = cc::pluginKey(p);
        const auto it = pluginRadio_.find(cc::pluginRunId(key));
        if (it != pluginRadio_.end() && it->second == static_cast<int>(radio)) { moved.push_back(key); }
    }
    // Entries for plugins that are not loaded now are dropped as well (a choice for a radio that no longer exists).
    for (auto it = pluginRadio_.begin(); it != pluginRadio_.end();) {
        it = (it->second == static_cast<int>(radio)) ? pluginRadio_.erase(it) : std::next(it);
    }
    if (radioRunners_[slot]) {
        if (receivers_[slot]) { receivers_[slot]->pipeline_.setPluginRunner(nullptr); }
        radioRunners_[slot]->clear();
    }
    for (const std::string& key : moved) {
        pluginApi_->setClientRadio(key, 1u);
        if (!pluginIsStopped(key)) { setPluginRun(key, cc::PluginRun::Stopped); }
    }
    radioRunners_[slot].reset();
    if (!moved.empty()) {
        cc::diagLogf("plugins: radio %u was closed - %zu plugin(s) that ran on it are back on radio 1, stopped", radio, moved.size());
    }
}

bool Engine::movePluginToRadio(const std::string& moduleKey, std::uint32_t radio, std::string& why) {
    const cc::LoadedPlugin* found = nullptr;
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && cc::pluginKey(p) == moduleKey) {
            found = &p;
            break;
        }
    }
    if (found == nullptr) {
        why = "no plugin with that name is installed";
        return false;
    }
    if (radio < 1u || radio > kMaxReceivers || !receiverLive(radio)) {
        why = "there is no radio with that number";
        return false;
    }
    if (found->txAudio != nullptr && radio != 1u) {
        why = "a plugin that transmits runs on radio 1: the transmitter follows it";
        return false;
    }
    const std::uint32_t old = radioOfPlugin(moduleKey);
    if (old == radio) { return true; }
    const std::string id = cc::pluginRunId(moduleKey);
    // LEAVE: its instances on the old radio's runner end (destroy() runs there, with the runner's lock dropped as for any stop); a plugin that is keyed cannot be here (it is on
    // radio 1 and refused above).
    runnerOf(old - 1u).stopPlugin(moduleKey);
    if (radio == 1u) {
        pluginRadio_.erase(id);
    } else {
        pluginRadio_[id] = static_cast<int>(radio);
    }
    pluginApi_->setClientRadio(moduleKey, radio);
    // ARRIVE: built against the new radio's own rate and centre, if it is meant to be running.
    if (!patchRunning_ && !pluginIsStopped(moduleKey) && pluginShouldRun(*found)) { (void)runnerOf(radio - 1u).startPlugin(*found); }
    rebuildMuteStates();
    cc::diagLogf("plugins: %s moved from radio %u to radio %u", found->name.c_str(), old, radio);
    return true;
}

FoxCommandResult Engine::pluginRadioOp(const FoxCommand& c, const std::string& text, FoxCommandResult res) {
    const auto refuse = [&res](std::int32_t status, const std::string& why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why.c_str());
        return res;
    };
    if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no plugin named"); }
    if (c.ival[0] < 1 || c.ival[0] > static_cast<long long>(kMaxReceivers)) { return refuse(FOXAPI_OUT_OF_RANGE, "radio numbers run from 1 to " + std::to_string(kMaxReceivers)); }
    const std::uint32_t radio = static_cast<std::uint32_t>(c.ival[0]);
    bool known = false;
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) { known = known || (p.loaded && cc::pluginKey(p) == text); }
    if (!known) { return refuse(FOXAPI_NOT_FOUND, "no plugin with that name is installed"); }
    if (!receiverLive(radio)) { return refuse(FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    const bool already = radioOfPlugin(text) == radio;
    if (!already) {
        std::string why;
        if (!movePluginToRadio(text, radio, why)) { return refuse(why.rfind("there is no radio", 0) == 0 ? FOXAPI_NO_DEVICE : FOXAPI_UNSUPPORTED, why); }
    }
    // ival[1] = 1: and run it there (a stop is cleared; the start is the DECODE bank's own: setPluginStopped -> ALWAYS).
    // The start applies the plugin's first preset (maybeAutoPresetOnStart): to the radio the plugin RUNS ON, not to the one this command happens to be scoped to (the session's focus), so a
    // decoder started on radio 2 tunes radio 2 (screen stage S6: found by runs-on-radio2 - the preset went to the wrong radio).
    if (c.ival[1] == 1) {
        RadioWork work(*this, static_cast<std::size_t>(radio) - 1u);
        setPluginStopped(text, false);
    }
    res.applied[0] = static_cast<double>(radio);
    if (already && c.ival[1] != 1) { res.status = FOXAPI_NO_CHANGE; }
    return res;
}

// --- the runners as one ---------------------------------------------------------------------------------------------------------------

bool Engine::pluginFeedingAnywhere(const std::string& moduleKey) const {
    bool any = false;
    forEachRunner([&](const cc::PluginRunner& r) { any = any || r.isFeeding(moduleKey); });
    return any;
}

void Engine::stopPluginEverywhere(const std::string& moduleKey) {
    forEachRunner([&](cc::PluginRunner& r) { r.stopPlugin(moduleKey); });
}

std::string Engine::playingPluginKeyAnywhere() const {
    std::string key;
    forEachRunner([&](const cc::PluginRunner& r) {
        if (key.empty()) { key = r.playingPluginKey(); }
    });
    return key;
}

std::vector<cc::DecoderStatus> Engine::allDecoderStatus() const {
    std::vector<cc::DecoderStatus> out = pluginRunner_.status();
    for (std::size_t slot = 1; slot < kMaxReceivers; ++slot) {
        if (!radioRunners_[slot]) { continue; }
        std::vector<cc::DecoderStatus> more = radioRunners_[slot]->status();
        out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    return out;
}

std::uint64_t Engine::runnersStatusGeneration() const {
    std::uint64_t sum = 0;
    forEachRunner([&](const cc::PluginRunner& r) { sum += r.statusGeneration(); });
    return sum;
}

void Engine::pollAllImages(std::vector<cc::HostImage>& out) {
    if (!anyExtraRunner()) {
        pluginRunner_.pollImages(out);
        return;
    }
    // Each runner polls into its own accumulator (a decoder offers a picture once); the caller's vector follows their concatenation, an entry that has not changed being moved
    // over as it is (no pixel copy), as ImageFeed::copyTo does.
    std::vector<cc::HostImage> next;
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        if (slot != 0u && !radioRunners_[slot]) { continue; }
        std::vector<cc::HostImage>& acc = imageAcc_[slot];
        runnerOf(slot).pollImages(acc);
        for (const cc::HostImage& e : acc) {
            bool reused = false;
            for (cc::HostImage& old : out) {
                if (old.plugin == e.plugin && old.revision == e.revision && old.sequence == e.sequence && old.width == e.width && old.height == e.height) {
                    next.push_back(std::move(old));
                    reused = true;
                    break;
                }
            }
            if (!reused) { next.push_back(e); }
        }
    }
    out = std::move(next);
}

// --- the transmitter ------------------------------------------------------------------------------------------------------------------

std::size_t Engine::transmitRadioSlot() const {
    if (!transmitter_.haveSink() || transmitArgs_.empty()) { return 0u; }
    return transmitRadioForViews(true, transmitArgs_, kMaxReceivers, [this](std::size_t slot) {
        const std::unique_ptr<Receiver>& r = receivers_[slot];
        return r ? TransmitView{&r->sourceKind_, &r->deviceArgs_, r->device_ != nullptr} : TransmitView{};
    });
}

}  // namespace cascade::engine
