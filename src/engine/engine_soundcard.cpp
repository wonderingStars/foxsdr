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
