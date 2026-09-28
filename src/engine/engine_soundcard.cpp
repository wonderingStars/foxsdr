// engine_soundcard.cpp - the Engine's machinery from gui/app_window_soundcard.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "engine/audio_open.hpp"
#include "engine/soundcard_panel.hpp"

#include "engine/bias_tee.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/rate_follow_status.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/running_view.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "engine/tx_frequency.hpp"


namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace (cascade::gui), so
// the code that moved with them reads exactly as it did.
using namespace cascade::gui;
using cascade::i18n::tr;
using cascade::i18n::trId;
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardRate;
using cascade::source::SoundCardSettings;

cascade::source::SoundCardSource::BackendFactory Engine::soundCardBackendFactory() {
    if (testHooks_.soundCardBackend != nullptr) { return testHooks_.soundCardBackend; }
    return {};
}

void Engine::scanSoundCards() {
    if (soundCardScanPending_) { return; }
    // ON A WORKER: every rate of every input is asked of the host API, and a
    // WASAPI device answers each one by activating an audio client.
    soundCardScanFuture_ = std::async(std::launch::async, [factory = soundCardBackendFactory()] {
        return (factory ? factory() : cascade::source::makePortAudioSoundCardBackend())->listDevices();
    });
    soundCardScanPending_ = true;
}

void Engine::launchSoundCardOpen(bool restore, const SoundCardSettings& settings) {
    if (soundCardOpenPending_) { return; }
    const std::vector<SoundCardDevice> list = soundCardDevices_;
    const bool listed = soundCardListed_;
    // A DEAD CARD ON LINUX is not opened again by its index - ALSA may have
    // given that number to another card plugged in since, which would open
    // under this card's name (gui::soundCardDeadReopenNeedsRestart). Refused
    // with the restart the application asks for after any plug or unplug;
    // the dead card stays installed, and the config goes on naming it.
    if (!restore && sourceKind_ == "soundcard" &&
        cascade::gui::soundCardDeadReopenNeedsRestart(installedSoundCardDead(), soundCardLive_, settings,
                                                      list)) {
        const std::string label = soundCardLive_.device + " (" + soundCardLive_.hostApi + ")";
        cascade::core::formatUtf8(sourceError_,
                                  tr("%s has stopped. Sound cards are listed when FoxSDR starts, and on "
                                     "Linux another card plugged in since can take its place in that list, "
                                     "so it is not opened again: restart FoxSDR after plugging or "
                                     "unplugging a card."),
                                  label.c_str());
        cascade::core::diagWarnf("source: the sound card %s has stopped; not reopened by its ALSA index "
                                 "(restart FoxSDR)",
                                 label.c_str());
        return;
    }
    // RE-OPENING THE CARD THAT IS RUNNING - a new rate, channel or format on
    // the same card. Windows will not open a second stream on a card in
    // exclusive use, nor exclusive mode on a card that is already streaming,
    // so the new settings cannot be opened beside the old stream (the second
    // review's probe: three of four everyday changes on a 192 kHz card were
    // refused). The running card is therefore released FIRST, here: the
    // pipeline goes back to the generator - setSource quiesces the source
    // thread, and the card's close is waited for within kCloseWaitMs, as on
    // every source switch - and the worker opens the new settings with the
    // old ones to fall back on (source::openSoundCardOrRestore). A different
    // card keeps the other order: it opens while the old one still runs.
    const bool release =
        !restore && cascade::gui::soundCardReopenReleasesFirst(sourceKind_ == "soundcard", soundCardLive_,
                                                               settings, list);
    SoundCardSettings previous;
    // NOTHING CHANGES (a dead card picked again, Open pressed as it runs):
    // released all the same, but tried once - there is nothing different to
    // fall back to (gui::soundCardSameSettings).
    const bool same = release && cascade::gui::soundCardSameSettings(settings, soundCardLive_);
    if (release) {
        previous = soundCardLive_;
        cascade::core::diagLogf("source: releasing the sound card %s (%s) to open it with new settings",
                                previous.device.c_str(), previous.hostApi.c_str());
        device_ = nullptr;
        soapyView_ = nullptr;
        ++sourceGen_;
        // Through installSource like every other swap: a recording of the
        // card ends here rather than taping the generator standing in.
        installSource(nullptr);
        sourceKind_ = "siggen";
        applyConverterForSource();
        // Until it is back - and for the rest of the session if neither the
        // new settings nor the old ones open - the config goes on naming the
        // card, exactly as after a restore that could not open it.
        restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
            "soundcard", cfgSoapyArgs_, cfgNativeArgs_, std::string(),
            cascade::source::soundCardIqRateHz(previous));
        soundCardRemembered_ = previous;
        restoreKeepLabel_.clear();
        followInputRate();
    }
    const std::uint64_t gen = sourceGen_;
    soundCardOpenFuture_ =
        std::async(std::launch::async, [settings, list, listed, gen, restore, release, same, previous,
                                        factory = soundCardBackendFactory()] {
            SoundCardOpenResult r;
            r.gen = gen;
            r.restore = restore;
            r.wanted = settings;
            r.released = release;
            r.previous = previous;
            r.sameAsPrevious = same;
            // PortAudio's list does not change while the application runs
            // (every PortAudio user in the process shares one snapshot), so a
            // list the section already has is the list; only a first open
            // enumerates.
            r.devices = listed ? list
                               : (factory ? factory() : cascade::source::makePortAudioSoundCardBackend())
                                     ->listDevices();
            cascade::source::SoundCardOpenOutcome out = cascade::source::openSoundCardOrRestore(
                settings, r.devices, (release && !same) ? &previous : nullptr, factory);
            r.restoredPrevious = out.restoredPrevious;
            // A coerced rate on success; why it was refused otherwise.
            r.error = (out.src && !out.restoredPrevious) ? out.note : out.refused;
            r.previousRefused = out.previousRefused;
            r.src = std::move(out.src);
            return r;
        });
    soundCardOpenPending_ = true;
}

void Engine::pollSoundCard() {
    // The patch page offers every listed card to its radios, so the list is
    // asked for the first time the page is open, as it is the first time the
    // Source section's row is.
    // Only once the user has asked for a device list (patchListsWanted_,
    // 0.99.40): the patch is the view the application opens on and is
    // switched to and fro, and showing it asks for nothing.
    if (host_.patchPageOpen() && patchListsWanted_ && !soundCardListed_ && !soundCardScanPending_ &&
        !soundCardOpenPending_) {
        scanSoundCards();
    }
    if (soundCardScanPending_ && soundCardScanFuture_.valid() &&
        soundCardScanFuture_.wait_for(AudioOpen::kNoWait) == std::future_status::ready) {
        soundCardDevices_ = soundCardScanFuture_.get();
        soundCardListed_ = true;
        soundCardScanPending_ = false;
        cascade::core::diagLogf("source: %zu sound card input(s) listed", soundCardDevices_.size());
    }
    if (!soundCardOpenPending_ || !soundCardOpenFuture_.valid() ||
        soundCardOpenFuture_.wait_for(AudioOpen::kNoWait) != std::future_status::ready) {
        return;
    }
    SoundCardOpenResult r = soundCardOpenFuture_.get();
    soundCardOpenPending_ = false;
    if (!soundCardListed_ || !r.devices.empty()) {
        soundCardDevices_ = r.devices;
        soundCardListed_ = true;
    }
    const std::string label = r.wanted.device + " (" + r.wanted.hostApi + ")";
    if (!r.src && r.released) {
        // NEITHER THE NEW SETTINGS NOR THE OLD ONES OPENED. The card was
        // released for this open, so the generator is what is running; the
        // config goes on naming the card (restoreKeep_, set at the release),
        // the combo says it is not open, and both reasons are on screen. The
        // section shows the settings the card last RAN with - what the next
        // start will try - and nothing claims it is running.
        cascade::core::diagWarnf("source: the sound card %s did not open with new settings, nor as it was",
                                 label.c_str());
        if (r.gen != sourceGen_) { return; }  // another source was chosen meanwhile
        soundCard_ = r.previous;
        restoreKeepLabel_ = std::string(tr("Sound card")) + ": " + r.previous.device;
        soundCardMissing_.clear();
        if (r.sameAsPrevious) {
            // Nothing new was asked for: one attempt, one reason.
            cascade::core::formatUtf8(sourceError_, tr("%s did not open (%s)."), label.c_str(),
                                      r.error.c_str());
        } else {
            cascade::core::formatUtf8(sourceError_,
                                      tr("%s did not open with the new settings (%s), and reopening it as "
                                         "it was failed too (%s)."),
                                      label.c_str(), r.error.c_str(), r.previousRefused.c_str());
        }
        return;
    }
    if (!r.src) {
        // NOTHING ELSE IS OPENED IN ITS PLACE. The live source stays what it
        // was - the generator after a failed restore, whatever was running
        // after a failed Open - and the reason is on screen.
        const cascade::source::SoundCardMatch m = cascade::source::matchSoundCard(
            r.devices, r.wanted.device, r.wanted.hostApi, r.wanted.pickedFromList);
        // Missing - not two identical cards, which the error names.
        const bool missing = !r.wanted.device.empty() && m.at < 0 && m.candidates.empty();
        // THE SECTION SHOWS WHAT IS RUNNING. With another card still running
        // (a different card's Open failed), its controls go back to that
        // card's settings; the message says which card did not open.
        if (!r.restore && sourceKind_ == "soundcard") { soundCard_ = soundCardLive_; }
        if (missing) {
            cascade::core::formatUtf8(
                soundCardMissing_,
                tr("%s is not connected, and no other input has been opened in its place. Plug it "
                   "in and restart FoxSDR - sound cards are listed when FoxSDR starts. The setting "
                   "is kept."),
                label.c_str());
            sourceError_.clear();
        } else {
            sourceError_ = r.error;
        }
        if (r.restore) {
            // The config goes on naming the card (restoreKeep_ was set when
            // the restore asked), and now the combo says so too.
            restoreKeepLabel_ = std::string(tr("Sound card")) + ": " + r.wanted.device;
        }
        cascade::core::diagWarnf("source: the sound card %s did not open%s", label.c_str(),
                                 missing ? " - it is not in the list of inputs" : "");
        return;
    }
    // THE CHOICE MAY HAVE MOVED ON while the card was opening: another source
    // installed (sourceGen_ moved), another row chosen, or a radio open under
    // way. The open card is then simply closed again. A card RELEASED for this
    // open is not given up because the combo was moved without choosing
    // anything: nothing else is running in its place but the stand-in.
    const bool movedOn = r.gen != sourceGen_ || deviceOpenPending_ ||
                         (!r.released && sourceSel_ != kSoundCardRow);
    if (movedOn) {
        cascade::core::diagLogf("source: a sound card open finished after the choice moved on");
        return;
    }
    sourceSel_ = kSoundCardRow;
    device_ = nullptr;  // before setSource destroys a live device
    soapyView_ = nullptr;
    deviceArgs_.clear();
    deviceModel_.clear();
    ++sourceGen_;
    // The settings as OPENED: the rate the card is really running at. The
    // section's copy is what the user edits next; the live copy is what is
    // actually running (the centre box asks it which mode that is).
    soundCard_ = r.src->settings();
    soundCardLive_ = soundCard_;
    installSource(std::move(r.src));
    sourceKind_ = "soundcard";
    // THIS CARD'S converter, after the install put the pipeline back to Off
    // and before anything reads the air centre below (see
    // soundCardConverter for what a card can have in front of it).
    applyConverterForSource();
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    soundCardMissing_.clear();
    if (r.restoredPrevious) {
        // THE NEW SETTINGS WERE REFUSED and the card is running again as it
        // was - which is what the section now shows (soundCard_ above).
        cascade::core::formatUtf8(sourceError_,
                                  tr("%s did not open with the new settings (%s). It is running again as "
                                     "it was."),
                                  label.c_str(), r.error.c_str());
    } else {
        sourceError_ = r.error;
    }
    followInputRate();
    // THE VFO STAYS WHERE IT WAS if that is inside what the card receives (a
    // saved session's offset is restored before the card finishes opening);
    // otherwise it is brought inside, exactly as a click near the edge is -
    // and CENTRED when the filter is wider than the whole span (WFM's 150 kHz
    // on a 96 kHz card), where there is no inside to bring it to.
    const double off = pipeline_.vfoOffsetHz();
    const double inside = cascade::gui::vfoOffsetInsideSpan(off, pipeline_.inputRateHz(), vfoBandwidthHz_);
    if (inside != off) {
        pipeline_.setVfoOffsetHz(inside);
        vfoOffsetKhz_ = static_cast<float>(inside / 1000.0);
    }
    cascade::core::diagLogf("source: opened the sound card %s (%s) at %.0f Hz, %s%s", soundCard_.device.c_str(),
                            soundCard_.hostApi.c_str(), soundCard_.cardRateHz,
                            soundCard_.format == SoundCardFormat::IqStereo ? "I/Q" : "real",
                            r.restoredPrevious ? " - as it was; the new settings were refused" : "");
}

bool Engine::installedSoundCardDead() {
    if (sourceKind_ != "soundcard") { return false; }
    // The card's own latch (SoundCardSource::deviceDead() is its faulted()),
    // read through the air view, which forwards it - so a card that died
    // while the receiver was stopped counts too.
    return pipeline_.faulted() || pipeline_.activeSource().faulted();
}

void Engine::reapSoundCardWorkers() {
    // The same trade as reapPendingDeviceOpen: a worker a few milliseconds
    // from done is collected, and one still inside the host API is handed to
    // a detached thread whose only job is to take the result and destroy it -
    // which closes a card that opened after quit rather than leaking it.
    if (soundCardOpenPending_ && soundCardOpenFuture_.valid()) {
        if (soundCardOpenFuture_.wait_for(AudioOpen::kQuitGrace) == std::future_status::ready) {
            (void)soundCardOpenFuture_.get();
        } else {
            std::thread([f = std::move(soundCardOpenFuture_)]() mutable { (void)f.get(); }).detach();
        }
        soundCardOpenPending_ = false;
    }
    if (soundCardScanPending_ && soundCardScanFuture_.valid()) {
        if (soundCardScanFuture_.wait_for(AudioOpen::kQuitGrace) == std::future_status::ready) {
            (void)soundCardScanFuture_.get();
        } else {
            std::thread([f = std::move(soundCardScanFuture_)]() mutable { (void)f.get(); }).detach();
        }
        soundCardScanPending_ = false;
    }
}

std::string Engine::soundCardPatchArgs(const std::string& keyArgs) const {
    return soundCardArgsForPatch(keyArgs, soundCard_);
}

bool Engine::retuneFixedCentre(double centerHz) {
    if (sourceKind_ != "soundcard") { return false; }
    cascade::source::IqSource& src = pipeline_.activeSource();
    // The filter's width is part of where the VFO can go: a tune reported as
    // inside is one setVfoToAbsoluteHz takes exactly as asked, never clamped.
    const FixedCentreTune t = tuneWithFixedCentre(centerHz, pipeline_.vfoOffsetHz(),
                                                  src.centerFrequencyHz(), src.sampleRateHz(),
                                                  vfoBandwidthHz_);
    if (t.tooWide) {
        const std::string bw = soundCardHzText(vfoBandwidthHz_);
        const std::string span = soundCardHzText(src.sampleRateHz());
        cascade::core::formatUtf8(tuneMismatchNote_,
                                  tr("The %s filter is wider than the %s the sound card receives; "
                                     "narrow it to tune."),
                                  bw.c_str(), span.c_str());
        return true;
    }
    if (!t.inside) {
        const std::string want = soundCardHzText(t.wantAbsHz);
        const std::string lo = soundCardHzText(std::max(0.0, t.loHz));
        const std::string hi = soundCardHzText(t.hiHz);
        cascade::core::formatUtf8(tuneMismatchNote_,
                                  tr("%s is outside what the sound card can tune to with this "
                                     "filter (%s to %s)."),
                                  want.c_str(), lo.c_str(), hi.c_str());
        return true;
    }
    tuneMismatchNote_.clear();
    setVfoToAbsoluteHz(t.wantAbsHz, false);
    return true;
}

}  // namespace cascade::engine
