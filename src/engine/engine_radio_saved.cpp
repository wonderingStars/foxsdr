// engine_radio_saved.cpp - THE SAVED SETTINGS OF RADIOS 2 TO 8 (multi-radio engine, stage 5; docs/multi-radio-records/engine-stages-5-6.md).
//
// The settings file keeps RADIO 1 in its top-level keys, exactly as every build has written it. Radios 2 to 8 are the array `radios` (core/config.hpp:
// SavedRadio): one object each, in the order the user opened them. This file is the two directions:
//
//   SAVE     fillSavedRadios reads each radio's settings out of its Receiver and its *_wide_ fields (the engine-wide mirrors that are one radio's: the saved
//            device spellings are swapped in by the radio's RadioWork, as the stage 4 record says), so it runs under the slot's scope. It is called from fillConfig
//            on the pass's thread, where every other part of the snapshot is read.
//   RESTORE  restoreSavedRadios is RADIO_ADD plus the ordinary ops, in the file's order. A radio on a device opens it with the first radio's own start-up open
//            (synchronous, the control thread is not running yet); a device that is missing or busy leaves the radio on the generator, NAMED in the log and in
//            fact.radio.restore, remembered for the next save, and the other radios are restored all the same.
//
// WHAT IS NOT SAVED HERE: gains, crystal correction, bias tee, converters. They are per DEVICE (maps keyed by the radio's key) and come back when that device
// opens, for any radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/patch_devices.hpp"
#include "core/receiver_tables.hpp"
#include "core/source_fallback.hpp"
#include "core/tune_control.hpp"
#include "engine/engine.hpp"

namespace pc = cascade::core::patch;
namespace cmd = cascade::core::cmd;
using cascade::core::SavedRadio;
using namespace cascade::gui;   // kModeNames, as engine_radios.cpp does

namespace cascade::engine {

namespace {

// The AGC's fixed gain as the file keeps it: in Off the pipeline's own value is the truth (see agcFixedGainShown in engine.cpp, of which this is the same rule).
double fixedGainShown(const Receiver& rx) {
    if (rx.agcMode_ == static_cast<int>(FOXAPI_AGC_OFF)) {
        const float live = rx.pipeline_.agcFixedGainDb();
        if (std::isfinite(live)) { return static_cast<double>(live); }
    }
    return static_cast<double>(rx.agcFixedGainDb_);
}

// A mode name as the file spells it -> the mode button index, -1 for a name that is none.
int modeIndexOfName(const std::string& name) {
    for (int i = 0; i < 8; ++i) {
        if (name == kModeNames[i]) { return i; }
    }
    return -1;
}

}  // namespace

// --- SAVE --------------------------------------------------------------------------------------------------------------------------------------

SavedRadio Engine::savedRadioOf(std::size_t slot) {
    RadioWork w(*this, slot);
    SavedRadio r;
    r.number = static_cast<int>(slot) + 1;
    // THE SOURCE, by the rule the first radio's save follows: a radio whose device did not open at the restore stays NAMED (restoreKeep_) while the generator
    // stands in for it.
    const cascade::gui::SavedSource src = cascade::gui::sourceToSave(rx().sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, iqOpenPath_,
                                                                     rx().pipeline_.activeSource().sampleRateHz(), rx().restoreKeep_);
    // A second radio is the generator, a scanned device, a Pluto, an rtl_tcp server or an I/Q file (engine stage 7: it is saved as what it is, with the arguments or the path it
    // was opened with). The sound card is the first radio's alone and is saved as the generator.
    const bool file = src.kind == "file";
    const bool device = src.kind == "soapy" || cascade::gui::isNativeSourceKind(src.kind);
    r.sourceKind = (device || file) ? src.kind : std::string("siggen");
    r.args = file ? src.filePath : (device ? (src.kind == "soapy" ? src.soapyArgs : src.nativeArgs) : std::string());
    if (r.args.empty()) { r.sourceKind = "siggen"; }
    r.sampleRateHz = (device || file) ? src.sampleRateHz : 0.0;
    r.antenna = rx().deviceAntenna_;
    r.toneHz = rx().testToneOffsetHz_;
    r.centerHz = rx().pipeline_.activeSource().centerFrequencyHz();
    r.mode = kModeNames[rx().modeIndex_];
    r.bandwidthHz = rx().vfoBandwidthHz_;
    r.vfoOffsetHz = rx().pipeline_.vfoOffsetHz();
    r.squelchDb = static_cast<double>(rx().squelchDb_);
    r.squelchAuto = rx().squelchAuto_;
    r.volume = static_cast<double>(rx().volume_);
    r.muted = rx().userMuted_;
    r.deemphasisIndex = rx().deemphIndex_;
    r.stereoEnabled = rx().stereoEnabled_;
    r.nrEnabled = rx().nrEnabled_;
    r.nrStrength = static_cast<double>(rx().nrStrength_);
    r.agcMode = rx().agcMode_;
    r.agcFixedGainDb = fixedGainShown(rx());
    r.nbEnabled = rx().nbEnabled_;
    r.nbThresholdDb = static_cast<double>(rx().nbThresholdDb_);
    r.toneSquelchKind = rx().toneKind_;
    r.toneSquelchHz = rx().toneHz_;
    r.toneSquelchCode = rx().toneCode_;
    r.toneSquelchPolarity = rx().tonePol_;
    r.toneRemove = rx().toneRemove_;
    r.meterUnits = rx().meterUnits_;
    r.nrMethod = rx().nrMethod_;
    r.cwPitchHz = rx().cwPitchHz_;
    r.noiseSquelchOn = rx().noiseSquelchOn_;
    r.noiseSquelchThresholdDb = rx().noiseSquelchThresholdDb_;
    r.noiseSquelchTailMs = rx().noiseSquelchTailMs_;
    for (std::size_t i = 0; i < rx().manualNotch_.size(); ++i) {
        const Receiver::ManualNotchSlot& ns = rx().manualNotch_[i];
        if (ns.on || ns.hz != 1000.0 || ns.widthHz != 50.0) { r.notchBank.push_back({static_cast<int>(i), ns.hz, ns.widthHz, ns.on}); }
    }
    r.notchEnabled = rx().notchEnabled_;
    r.notchFreqHz = static_cast<double>(rx().notchFreqHz_);
    r.notchQ = static_cast<double>(rx().notchQ_);
    r.autoNotch = rx().autoNotch_;
    r.audioRoute = rx().audioRoute_;
    r.running = rx().pipeline_.running();
    return r;
}

std::vector<SavedRadio> Engine::savedRadios() {
    std::vector<SavedRadio> out;
    for (std::size_t slot = 1; slot < kMaxReceivers; ++slot) {
        if (!receivers_[slot] || receivers_[slot]->removing_) { continue; }
        out.push_back(savedRadioOf(slot));
    }
    return out;
}

// --- RESTORE -----------------------------------------------------------------------------------------------------------------------------------

// A saved I/Q file as a second radio: the first radio's start-up restore of a file, for the radio this thread has selected (synchronous: the control thread is not running yet).
// A file that will not open leaves the radio on the generator, remembered by its path (restoreKeep_) so the next save names it again, and says why in the radio's refusal.
void Engine::restoreExtraRadioFile(const std::string& path, double rateHz, double centerHz) {
    auto file = iqFileFactory_ ? iqFileFactory_() : std::make_unique<cascade::source::IqFileSource>();
    if (file->open(path)) {
        const double fileRadioHz = radioHzForSource("file", std::string(), centerHz);
        if (fileRadioHz >= 0.0) { file->setCenterFrequencyHz(fileRadioHz); }
        installOpenedIqFile(std::move(file), path);
        return;
    }
    rx().sourceError_ = file->lastError();
    // THE FACT, NEVER THE PATH, in the log (a file name is the user's own data; the radio-1 restore's rule).
    cascade::core::diagWarnf("radio: radio %u could not be restored on its I/Q file - it is on the signal generator and the radio stays saved", static_cast<unsigned>(scopedSlot() + 1u));
    cascade::core::health::noteRecovered(cascade::core::health::Recovered::IqFileGone);
    rx().restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen("file", std::string(), std::string(), path, rateHz);
    rx().restoreKeepLabel_ = cascade::gui::fileNameOf(path);
    rx().sourceError_ = cascade::gui::radioNotOpenedSentence(rx().restoreKeepLabel_, rx().sourceError_);
    rx().refusal_ = rx().sourceError_;
}

void Engine::restoreExtraRadioDevice(const std::string& kind, const std::string& args, double rateHz, const std::string& antenna, double centerHz) {
    if (kind == "file") {
        restoreExtraRadioFile(args, rateHz, centerHz);
        return;
    }
    // The first radio's start-up restore (applyConfig, the device branch), for the radio this thread has selected. The prefer-native rule is not applied: this
    // file is written by a build that knows the native drivers.
    if (kind != "soapy") { scanNative(); }
    rx().deviceAntenna_ = antenna;
    auto dev = openDeviceSync(kind, args, rateHz);
    if (dev) {
        const double radioHz = radioHzForSource(kind, args, centerHz);
        if (radioHz > 0.0 || !cascade::core::converterActive(converterForKey(cascade::core::converterRadioKey(kind, args)))) {
            dev->setCenterFrequencyHz(ppmPreTellHz(kind, args, dev->hasFrequencyCorrection(), radioHz));
        }
        rx().device_ = dev.get();
        rx().soapyView_ = dynamic_cast<cascade::source::SoapySource*>(rx().device_);
        rx().deviceArgs_ = args;
        rx().deviceModel_ = (kind == "soapy") ? cascade::core::sanitiseDevice(args) : modelFromNativeLabel(nativeLabelFor(kind, args));
        if (kind == "soapy") {
            cfgSoapyArgs_ = args;
        } else {
            cfgNativeArgs_ = args;
        }
        ++rx().sourceGen_;
        installSource(std::move(dev));
        rx().sourceKind_ = kind;
        adoptKiwiRadio();
        cascade::core::health::noteRadioOpen(kind);
        applyConverterForSource();
        followInputRate();
        cascade::core::diagLogf("radio: radio %u restored on %s (%s) at %.0f S/s", static_cast<unsigned>(scopedSlot() + 1u), rx().deviceModel_.c_str(), kind.c_str(),
                                rx().pipeline_.activeSource().sampleRateHz());
        return;
    }
    // THE SAVED RADIO WOULD NOT OPEN. Counted anonymously, as the first radio's is; the radio stays, on the generator, and the file goes on naming the device.
    cascade::core::health::noteRadioFail(kind, rx().sourceError_);
    const std::string whyClass = cascade::core::health::radioFailWhy(kind, rx().sourceError_);
    const bool failedBusy = cascade::core::health::classifyRadioOpen(kind, rx().sourceError_) == cascade::core::health::RadioReason::Busy;
    std::string label = (kind == "soapy") ? cascade::core::sanitiseDevice(args) : modelFromNativeLabel(nativeLabelFor(kind, args));
    if (label.empty() || label == args) { label = kind; }
    cascade::core::diagWarnf("radio: radio %u could not be restored on %s (%s): %s - it is on the signal generator and the radio stays saved",
                             static_cast<unsigned>(scopedSlot() + 1u), label.c_str(), kind.c_str(), whyClass.c_str());
    rx().restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(kind, kind == "soapy" ? args : std::string(), kind == "soapy" ? std::string() : args,
                                                                      std::string(), rateHz);
    rx().restoreKeepLabel_ = label;
    if (failedBusy && rx().restoreKeep_.valid()) {
        rx().savedRadioRetryBusy_ = true;
        rx().savedRadioRetryAttempts_ = 0;
        rx().savedRadioRetryLastFailSec_ = retryClockS();
    }
    rx().sourceError_ = cascade::gui::radioNotOpenedSentence(label, rx().sourceError_);
    rx().refusal_ = rx().sourceError_;
}

void Engine::restoreSavedRadios(const cascade::core::AppConfig& cfg) {
    radioRestoreNotes_.clear();
    const auto note = [this](int number, const std::string& why) {
        radioRestoreNotes_.push_back("Radio " + std::to_string(number) + ": " + why);
        cascade::core::diagWarnf("radio: radio %d was not restored: %s", number, why.c_str());
    };
    for (const SavedRadio& s : cfg.radios) {
        const int number = s.number;
        if (number < 2 || number > static_cast<int>(kMaxReceivers)) { continue; }   // (the loader numbered every entry; a hand-built config may not have)
        const bool generator = s.sourceKind == "siggen" || s.args.empty();
        const std::string text = generator ? std::string() : s.sourceKind + ":" + s.args;
        const cmd::QueuedCommand q = cmd::makeText(FOXAPI_OP_RADIO_ADD, text, number, 0, s.toneHz);
        restoreEntry_ = &s;
        const FoxCommandResult added = applyCommand(q.c, q.longText);
        restoreEntry_ = nullptr;
        if (added.status != FOXAPI_OK && added.status != FOXAPI_NO_CHANGE) {
            note(number, std::string(added.message[0] != '\0' ? added.message : "it could not be opened"));
            continue;
        }
        if (!receiverLive(static_cast<std::uint32_t>(number))) { continue; }
        // THE ORDINARY OPS, aimed at the new radio, in the order the first radio's restore follows: tune, then the mode (which moves the bandwidth to its own default), then the
        // bandwidth and the VFO, then the audio settings.
        const auto send = [&](FoxCommand c) {
            c.target = static_cast<std::uint32_t>(number);
            (void)applyCommand(c);
        };
        if (s.centerHz > 0.0) { send(cmd::makeNum(FOXAPI_OP_SET_CENTRE, s.centerHz)); }
        const int mode = modeIndexOfName(s.mode);
        if (mode >= 0) { send(cmd::makeInt(FOXAPI_OP_SET_MODE, mode + 1)); }   // (FOXAPI_DEMOD_* is the mode keys' order, 1-based)
        if (s.bandwidthHz > 0.0) { send(cmd::makeNum(FOXAPI_OP_SET_BANDWIDTH, s.bandwidthHz)); }
        send(cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, s.vfoOffsetHz));
        send(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, s.squelchDb));
        if (s.squelchAuto) { send(cmd::makeInt(FOXAPP_OP_SET_SQUELCH_AUTO, 1)); }
        send(cmd::makeNum(FOXAPI_OP_SET_VOLUME, s.volume));
        send(cmd::makeInt(FOXAPI_OP_SET_MUTED, s.muted ? 1 : 0));
        send(cmd::makeInt(FOXAPI_OP_SET_DEEMPHASIS, s.deemphasisIndex));
        send(cmd::makeInt(FOXAPI_OP_SET_STEREO, s.stereoEnabled ? 1 : 0));
        {
            FoxCommand nr = cmd::makeInt(FOXAPI_OP_SET_NR, s.nrEnabled ? 1 : 0, 1);
            nr.num[0] = s.nrStrength;
            send(nr);
        }
        {
            // The AGC's mode first, the fixed gain after it (the handler's own rule).
            FoxCommand agc = cmd::makeInt(FOXAPI_OP_SET_AGC, s.agcMode, 1);
            agc.num[0] = s.agcFixedGainDb;
            send(agc);
        }
        {
            FoxCommand nb = cmd::makeInt(FOXAPI_OP_SET_NB, s.nbEnabled ? 1 : 0, 1);
            nb.num[0] = s.nbThresholdDb;
            send(nb);
        }
        {
            // The tone squelch with its values (ival[1] = 1), then the remover.
            FoxCommand tq = cmd::makeInt(FOXAPI_OP_SET_TONE_SQUELCH, s.toneSquelchKind, 1);
            tq.num[0] = s.toneSquelchHz;
            tq.num[1] = static_cast<double>(s.toneSquelchCode);
            tq.num[2] = static_cast<double>(s.toneSquelchPolarity);
            send(tq);
            send(cmd::makeInt(FOXAPI_OP_SET_TONE_REMOVE, s.toneRemove ? 1 : 0));
            send(cmd::makeInt(FOXAPI_OP_SET_METER_UNITS, s.meterUnits));
            send(cmd::makeInt(FOXAPI_OP_SET_NR_METHOD, s.nrMethod));
            send(cmd::makeNum(FOXAPI_OP_SET_CW_PITCH, s.cwPitchHz));
            {
                FoxCommand nsq = cmd::makeInt(FOXAPI_OP_SET_NOISE_SQUELCH, s.noiseSquelchOn ? 1 : 0, 1);
                nsq.num[0] = s.noiseSquelchThresholdDb;
                nsq.num[1] = s.noiseSquelchTailMs;
                send(nsq);
            }
            for (const cascade::core::ManualNotchSetting& n : s.notchBank) {
                FoxCommand nc = cmd::makeInt(FOXAPI_OP_SET_NOTCH_SLOT, n.on ? 1 : 0, 1);
                nc.num[0] = n.hz;
                nc.num[1] = n.widthHz;
                nc.num[2] = static_cast<double>(n.slot);
                send(nc);
            }
        }
        {
            FoxCommand notch = cmd::makeInt(FOXAPI_OP_SET_NOTCH, s.notchEnabled ? 1 : 0, 1);
            notch.num[0] = s.notchFreqHz;
            notch.num[1] = s.notchQ;
            send(notch);
        }
        send(cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, s.autoNotch ? 1 : 0));
        (void)applyCommand(cmd::makeInt(FOXAPI_OP_RADIO_AUDIO, number, s.audioRoute));
        if (s.running) { send(cmd::makeInt(FOXAPI_OP_RUN, 1)); }
        // A radio whose device did not open is reported by name; the rest of the entry was applied to the generator that stands in for it.
        if (!generator) {
            RadioWork w(*this, static_cast<std::size_t>(number) - 1u);
            if (!rx().refusal_.empty()) { note(number, rx().refusal_); }
        }
    }
}

void Engine::telemetryNoteRadios() {
    std::uint32_t running = 0;
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        if (receivers_[slot] && receivers_[slot]->pipeline_.running()) { ++running; }
    }
    if (running > telemetryRadiosMax_) { telemetryRadiosMax_ = running; }
}

// --- THE PLUGINS' RADIO (stage 6; the choice itself is engine_plugin_radio.cpp) -------------------------------------------------------------------
//
// The file's map is read after the radios have been restored: a plugin whose radio is not open is read as radio 1 (the screen design: it "starts on the selected radio and says
// so"), in the log, and the choice is dropped rather than kept for a radio that may never come back.
void Engine::restorePluginRadios(const cascade::core::AppConfig& cfg) {
    pluginRadio_.clear();
    for (const auto& [id, radio] : cfg.pluginRadio) {
        if (radio <= 1) { continue; }
        if (!receiverLive(static_cast<std::uint32_t>(radio))) {
            cascade::core::diagLogf("plugins: a plugin's saved radio (%d) is not open - it runs on radio 1", radio);
            continue;
        }
        pluginRadio_[id] = radio;
    }
    // The runners are built from the choice: every radio's decoders again, each plugin on the radio the file put it on.
    if (!pluginRadio_.empty()) { rebuildAllRunners(); }
}

}  // namespace cascade::engine
