// engine_patch_radios.cpp - the Engine's machinery from gui/app_window_patch_radios.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_devices.hpp"
#include "engine/rate_follow_status.hpp"
#include "engine/soundcard_panel.hpp"
#include "source/siggen_source.hpp"
#include "source/soapy_source.hpp"

#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/running_view.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "engine/tx_frequency.hpp"

namespace pc = cascade::core::patch;

namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace (cascade::gui), so
// the code that moved with them reads exactly as it did.
using namespace cascade::gui;
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// A speaker's destination by KIND: the WAV/MP3 file is named after the node,
// which the user named.
const char* destKind(const pc::AudioDest* dest) {
    if (dest == nullptr) { return "nothing"; }
    const std::string d = dest->describe();
    if (d.rfind("WAV", 0) == 0) { return "a WAV file"; }
    if (d.rfind("MP3", 0) == 0) { return "an MP3 file"; }
    return "an output device";
}

}  // namespace

std::vector<Engine::PatchDeviceChoice> Engine::patchDeviceChoices() const {
    std::vector<PatchDeviceChoice> out;
    out.push_back({pc::kGeneratorKey, tr("Signal generator")});
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // A Pluto needs its address typed in the Source panel; the patch has
        // no field for it, so it is not offered here rather than failing.
        if (d.driver == "pluto") { continue; }
        out.push_back({pc::makeDeviceKey(d.driver, d.args), d.label});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        // NOT WRAPPED: "(SoapySDR)" is only the excluded product name in
        // parentheses, nothing else to translate.
        out.push_back({pc::makeDeviceKey("soapy", d.args), d.label + " (SoapySDR)"});
    }
    // Every sound card input the Source section has listed (the list is asked
    // for when the patch page is open - see pollSoundCard).
    for (const cascade::source::SoundCardDevice& d : soundCardDevices_) {
        out.push_back({pc::makeDeviceKey("soundcard", cascade::source::soundCardDeviceArgs(d.name, d.hostApi)),
                       std::string(tr("Sound card")) + ": " + cascade::source::soundCardDeviceLabel(d)});
    }
    // The receiver's own radio, even when no list currently shows it (a
    // SoapySDR device is only listed after a scan).
    if (patchMainKeep_.valid) {
        const std::string key = pc::makeDeviceKey(patchMainKeep_.kind, patchMainKeep_.args);
        const bool listed = std::any_of(out.begin(), out.end(), [&](const PatchDeviceChoice& c) {
            return c.key == key || pc::sameDevice(c.key, key);
        });
        if (!listed) {
            std::string buf;
            cascade::core::formatUtf8(buf, tr("%s (the receiver's radio)"),
                          patchMainKeep_.label.c_str());
            out.push_back({key, buf});
        }
    }
    return out;
}

std::string Engine::patchDeviceLabel(const std::string& key) const {
    if (key.empty()) { return tr("No device chosen"); }
    for (const PatchDeviceChoice& c : patchDeviceChoices()) {
        if (c.key == key) { return c.label; }
    }
    // NOT LISTED IS NOT "NOT CONNECTED": a SoapySDR radio is listed only after
    // a scan. Its args carry its own label, which is what the list would say.
    const std::string args = pc::deviceArgs(key);
    const std::string own = pc::argField(args, "label");
    if (!own.empty()) { return own; }
    const std::string serial = pc::argField(args, "serial");
    std::string buf;
    if (serial.empty()) {
        cascade::core::formatUtf8(buf, tr("%s (not listed)"), pc::deviceDriver(key).c_str());
    } else {
        cascade::core::formatUtf8(buf, tr("%s %s (not listed)"), pc::deviceDriver(key).c_str(),
                      serial.c_str());
    }
    return buf;
}

std::vector<pc::RadioInfo> Engine::patchRadioInfos() const {
    std::vector<pc::RadioInfo> out;
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        const auto it = patchRadios_.find(n.id);
        if (it != patchRadios_.end()) {
            out.push_back({n.id, it->second->rateHz(), it->second->centreHz()});
        } else {
            // Not running yet: plan against what it is set to, so the patch
            // can be judged before the device has opened.
            out.push_back({n.id, radioRate(n), n.freqHz});
        }
    }
    return out;
}

void Engine::patchPublishSets() {
    // --- each speaker's output --------------------------------------------------
    std::set<pc::NodeId> live;
    for (const pc::AudioSinkPlan& sp : patchPlan_.sinks) {
        if (patchRadios_.count(sp.radio) == 0) { continue; }   // nothing to write yet
        const pc::Node* n = patchGraph_.find(sp.sink);
        if (n == nullptr) { continue; }
        live.insert(sp.sink);
        const std::string key = n->device.empty() ? std::string("wav") : n->device;
        const auto made = patchDestMadeFor_.find(sp.sink);
        if (made != patchDestMadeFor_.end() && made->second == key) { continue; }

        std::string err;
        std::shared_ptr<pc::AudioDest> dest;
        const std::string prefix = pc::patchFilePrefix(static_cast<unsigned>(sp.sink), n->name);
        switch (pc::outputKind(key)) {
            case pc::OutputKind::Wav:
                dest = pc::makeWavDest(recordDir_, prefix, err);
                break;
            case pc::OutputKind::Mp3: {
                dest = pc::makeMp3Dest(recordDir_, prefix, err);
                if (!dest) {
                    // THE FILE STILL GETS WRITTEN: an MP3 this build cannot
                    // make is a WAV, and the face says so.
                    std::string werr;
                    dest = pc::makeWavDest(recordDir_, prefix, werr);
                    err += dest ? " - writing WAV instead" : "; " + werr;
                }
                break;
            }
            case pc::OutputKind::Speakers:
                dest = pc::makeDeviceDest(std::string{}, err);
                break;
            case pc::OutputKind::Device:
                dest = pc::makeDeviceDest(pc::outputDeviceName(key), err);
                break;
        }
        bool replaced = false;
        for (auto& d : patchDests_) {
            if (d.first == sp.sink) {
                d.second = dest;
                replaced = true;
            }
        }
        if (!replaced) { patchDests_.emplace_back(sp.sink, dest); }
        patchDestMadeFor_[sp.sink] = key;
        if (err.empty()) {
            patchDestError_.erase(sp.sink);
        } else {
            patchDestError_[sp.sink] = err;
        }
        cascade::core::diagLogf("patch: speaker node %u -> %s%s%s",
                                static_cast<unsigned>(sp.sink), destKind(dest.get()),
                                err.empty() ? "" : " - ", err.c_str());
    }
    // Speakers that stopped playing lose their output. The file is finalised
    // when the running set that still holds it is retired.
    for (auto it = patchDests_.begin(); it != patchDests_.end();) {
        if (live.count(it->first) != 0) {
            ++it;
            continue;
        }
        patchDestMadeFor_.erase(it->first);
        patchDestError_.erase(it->first);
        it = patchDests_.erase(it);
    }

    // --- each radio's set -------------------------------------------------------
    for (auto& [id, radio] : patchRadios_) {
        const double rate = radio->rateHz();
        const std::string sig = pc::radioSignature(patchPlan_, patchGraph_, id, rate,
                                                   &patchCatalogue_, &patchApis_, patchDests_);
        const auto have = patchRadioSig_.find(id);
        if (have != patchRadioSig_.end() && have->second == sig) { continue; }
        std::shared_ptr<pc::StripSet> set = pc::buildRadioSet(
            patchPlan_, patchGraph_, id, rate, &patchCatalogue_, &patchApis_, patchDests_);
        patchRefusedBy_[id] = set->refused;
        for (const auto& d : set->decoders) {
            patchDecoderFaces_.erase(d->node);
            patchFirstLineLogged_.erase(d->node);
            const pc::Node* dn = patchGraph_.find(d->node);
            cascade::core::diagLogf("patch: decoder node %u (%s) started on radio node %u at "
                                    "%.0f S/s",
                                    static_cast<unsigned>(d->node),
                                    dn != nullptr ? dn->plugin.c_str() : "?",
                                    static_cast<unsigned>(id), d->rateHz);
        }
        for (const pc::NodeId r : set->refused) {
            const pc::Node* n = patchGraph_.find(r);
            cascade::core::diagLogf("patch: decoder node %u (%s) refused to start",
                                    static_cast<unsigned>(r),
                                    n != nullptr ? n->plugin.c_str() : "?");
        }
        radio->runner().publish(std::move(set));
        patchRadioSig_[id] = sig;
    }
    // Every demodulator's squelch, live - after any publish, so a new set's
    // channels get the setting from their first block.
    patchPushSquelch();
    patchRefused_.clear();
    for (const auto& [id, list] : patchRefusedBy_) {
        (void)id;
        patchRefused_.insert(patchRefused_.end(), list.begin(), list.end());
    }
}

void Engine::patchPressStart() {
    if (patchRunning_) {
        patchRunning_ = false;
        return;
    }
    // START WITH EVERY RADIO SWITCHED OFF SWITCHES THEM ALL ON (see
    // switchOnForStart for why).
    if (pc::switchOnForStart(patchGraph_)) { host_.onPatchGraphChanged(); }
    patchRunning_ = true;
}

void Engine::patchAllOff() {
    // EVERY RADIO OFF, and the patch stopped: the receiver gets its radio back
    // when patchApplyRunning sees the change.
    if (pc::switchAllRadiosOff(patchGraph_)) { host_.onPatchGraphChanged(); }
    patchRunning_ = false;
}

void Engine::patchPushSquelch() {
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Demod) { continue; }
        const pc::NodeId chan = demodChannel(patchGraph_, n.id);
        if (chan == pc::kNoNode) { continue; }
        const auto r = patchRadios_.find(pc::radioOf(patchGraph_, chan));
        if (r == patchRadios_.end()) { continue; }
        r->second->runner().setSquelchDb(chan, n.squelch ? n.squelchDb : pc::kSquelchOffDb);
    }
}

}  // namespace cascade::engine
