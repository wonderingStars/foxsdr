// app_window_soundcard.cpp - the Source section's SOUND CARD row. AppWindow
// members, kept out of app_window.cpp because they are one subject: listing
// the inputs, opening one on a worker, installing it, bringing it back at
// startup, and tuning a source that has no tuner.
//
// The source itself - the conversion, the name-and-host-API rule, the
// liveness watch - is source/soundcard_source.hpp; the rules a test can reach
// are gui/soundcard_panel.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/audio_open.hpp"
#include "gui/soundcard_panel.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardRate;
using cascade::source::SoundCardSettings;

namespace {

// "192 kHz", "44.1 kHz", "7.0500 MHz": the Source section's frequency words.
std::string soundCardHzText(double hz) {
    char buf[40];
    if (std::fabs(hz) >= 1.0e6) {
        std::snprintf(buf, sizeof(buf), "%.4f MHz", hz / 1.0e6);
    } else if (std::fabs(std::fmod(hz, 1000.0)) < 0.5) {
        std::snprintf(buf, sizeof(buf), "%.0f kHz", hz / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f kHz", hz / 1000.0);
    }
    return buf;
}

std::string rateLabel(const SoundCardRate& r) {
    const std::string hz = soundCardHzText(r.hz);
    if (!r.exclusive) { return hz; }
    std::string out;
    cascade::core::formatUtf8(out, tr("%s (exclusive)"), hz.c_str());
    return out;
}

}  // namespace

void AppWindow::scanSoundCards() {
    if (soundCardScanPending_) { return; }
    // ON A WORKER: every rate of every input is asked of the host API, and a
    // WASAPI device answers each one by activating an audio client.
    soundCardScanFuture_ = std::async(std::launch::async, [] {
        return cascade::source::makePortAudioSoundCardBackend()->listDevices();
    });
    soundCardScanPending_ = true;
}

void AppWindow::launchSoundCardOpen(bool restore) {
    if (soundCardOpenPending_) { return; }
    const SoundCardSettings settings = soundCard_;
    const std::vector<SoundCardDevice> list = soundCardDevices_;
    const bool listed = soundCardListed_;
    const std::uint64_t gen = sourceGen_;
    soundCardOpenFuture_ = std::async(std::launch::async, [settings, list, listed, gen, restore] {
        SoundCardOpenResult r;
        r.gen = gen;
        r.restore = restore;
        // PortAudio's list does not change while the application runs (every
        // PortAudio user in the process shares one snapshot), so a list the
        // section already has is the list; only a first open enumerates.
        r.devices = listed ? list : cascade::source::makePortAudioSoundCardBackend()->listDevices();
        auto src = std::make_unique<cascade::source::SoundCardSource>();
        if (src->openWith(settings, r.devices)) {
            r.error = src->lastError();  // a coerced rate, or nothing
            r.src = std::move(src);
        } else {
            r.error = src->lastError();
        }
        return r;
    });
    soundCardOpenPending_ = true;
}

void AppWindow::pollSoundCard() {
    // The patch page offers every listed card to its radios, so the list is
    // asked for the first time the page is open, as it is the first time the
    // Source section's row is.
    if (patchOpen_ && !soundCardListed_ && !soundCardScanPending_ && !soundCardOpenPending_) {
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
    const std::string label = soundCard_.device + " (" + soundCard_.hostApi + ")";
    if (!r.src) {
        // NOTHING ELSE IS OPENED IN ITS PLACE. The live source stays what it
        // was - the generator after a failed restore, whatever was running
        // after a failed Open - and the reason is on screen.
        const bool missing = !soundCard_.device.empty() &&
                             cascade::source::findSoundCard(r.devices, soundCard_.device,
                                                            soundCard_.hostApi) < 0;
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
            restoreKeepLabel_ = std::string(tr("Sound card")) + ": " + soundCard_.device;
        }
        cascade::core::diagWarnf("source: the sound card %s (%s) did not open%s",
                                 soundCard_.device.c_str(), soundCard_.hostApi.c_str(),
                                 missing ? " - it is not in the list of inputs" : "");
        return;
    }
    // THE CHOICE MAY HAVE MOVED ON while the card was opening: another source
    // installed (sourceGen_ moved), another row chosen, or a radio open under
    // way. The open card is then simply closed again.
    if (r.gen != sourceGen_ || sourceSel_ != kSoundCardRow || deviceOpenPending_) {
        cascade::core::diagLogf("source: a sound card open finished after the choice moved on");
        return;
    }
    device_ = nullptr;  // before setSource destroys a live device
    soapyView_ = nullptr;
    deviceArgs_.clear();
    deviceModel_.clear();
    ++sourceGen_;
    // The settings as OPENED: the rate the card is really running at.
    soundCard_ = r.src->settings();
    pipeline_.setSource(std::move(r.src));
    sourceKind_ = "soundcard";
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    soundCardMissing_.clear();
    sourceError_ = r.error;
    followInputRate();
    // THE VFO STAYS WHERE IT WAS if that is inside what the card receives (a
    // saved session's offset is restored before the card finishes opening);
    // otherwise it is brought inside, exactly as a click near the edge is.
    const double lim = 0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
    const double off = pipeline_.vfoOffsetHz();
    if (lim > 0.0 && std::fabs(off) > lim) {
        const double clamped = std::clamp(off, -lim, lim);
        pipeline_.setVfoOffsetHz(clamped);
        vfoOffsetKhz_ = static_cast<float>(clamped / 1000.0);
    }
    cascade::core::diagLogf("source: opened the sound card %s (%s) at %.0f Hz, %s", soundCard_.device.c_str(),
                            soundCard_.hostApi.c_str(), soundCard_.cardRateHz,
                            soundCard_.format == SoundCardFormat::IqStereo ? "I/Q" : "real");
}

void AppWindow::reapSoundCardWorkers() {
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

std::string AppWindow::soundCardPatchArgs(const std::string& keyArgs) const {
    return soundCardArgsForPatch(keyArgs, soundCard_);
}

bool AppWindow::retuneFixedCentre(double centerHz) {
    if (sourceKind_ != "soundcard") { return false; }
    cascade::source::IqSource& src = pipeline_.activeSource();
    const FixedCentreTune t = tuneWithFixedCentre(centerHz, pipeline_.vfoOffsetHz(),
                                                  src.centerFrequencyHz(), src.sampleRateHz());
    if (!t.inside) {
        const std::string want = soundCardHzText(t.wantAbsHz);
        const std::string lo = soundCardHzText(std::max(0.0, t.loHz));
        const std::string hi = soundCardHzText(t.hiHz);
        cascade::core::formatUtf8(tuneMismatchNote_,
                                  tr("%s is outside what the sound card receives (%s to %s)."),
                                  want.c_str(), lo.c_str(), hi.c_str());
        return true;
    }
    tuneMismatchNote_.clear();
    setVfoToAbsoluteHz(t.wantAbsHz, false);
    return true;
}

void AppWindow::drawSoundCardControls() {
    // The list is asked for the first time the row is shown, never at
    // startup for a user who does not use a sound card.
    if (!soundCardListed_ && !soundCardScanPending_ && !soundCardOpenPending_) { scanSoundCards(); }

    const std::string label = soundCard_.device.empty()
                                  ? std::string()
                                  : soundCard_.device + " (" + soundCard_.hostApi + ")";
    if (soundCardOpenPending_) {
        std::string line;
        cascade::core::formatUtf8(line, tr("Opening %s..."), label.c_str());
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", line.c_str());
    } else if (soundCardScanPending_) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("Scanning for devices..."));
    }
    if (soundCardListed_ && soundCardDevices_.empty()) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("No sound card inputs found."));
    }

    const bool busy = soundCardOpenPending_ || soundCardScanPending_ || deviceOpenPending_;
    ImGui::BeginDisabled(busy);

    // THE INPUT. Named by device and host API; a saved card that is not in
    // the list keeps its name in the preview (the missing line below says why).
    const int at = cascade::source::findSoundCard(soundCardDevices_, soundCard_.device, soundCard_.hostApi);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Device##soundcard_device")),
                          label.empty() ? "-" : label.c_str())) {
        for (std::size_t i = 0; i < soundCardDevices_.size(); ++i) {
            const SoundCardDevice& d = soundCardDevices_[i];
            ImGui::PushID(static_cast<int>(i));
            const bool sel = static_cast<int>(i) == at;
            if (ImGui::Selectable(cascade::source::soundCardDeviceLabel(d).c_str(), sel) && !sel) {
                soundCard_.device = d.name;
                soundCard_.hostApi = d.hostApi;
                soundCardMissing_.clear();
                // Keep the rate if the new card offers it; otherwise its own.
                const auto rates = cascade::source::soundCardRatesFor(d, soundCard_.format);
                const bool offered = std::any_of(rates.begin(), rates.end(), [&](const SoundCardRate& r) {
                    return r.hz == soundCard_.cardRateHz;
                });
                if (!offered && !rates.empty()) {
                    soundCard_.cardRateHz = rates.back().hz;
                    for (const SoundCardRate& r : rates) {
                        if (r.hz == d.defaultRateHz) { soundCard_.cardRateHz = r.hz; }
                    }
                }
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // THE FORMAT, and what goes with it.
    const char* formats[] = {tr("Real (mono)"), tr("I/Q (stereo)")};
    int fmt = soundCard_.format == SoundCardFormat::IqStereo ? 1 : 0;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(labelAboveIfNeeded(trId("Format##soundcard_format"), 160.0f), &fmt, formats, 2)) {
        soundCard_.format = fmt == 1 ? SoundCardFormat::IqStereo : SoundCardFormat::RealMono;
    }
    if (soundCard_.format == SoundCardFormat::RealMono) {
        const char* channels[] = {tr("Left"), tr("Right")};
        int ch = soundCard_.channel == 1 ? 1 : 0;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(labelAboveIfNeeded(trId("Channel##soundcard_channel"), 160.0f), &ch, channels, 2)) {
            soundCard_.channel = ch;
        }
    } else {
        ImGui::Checkbox(trId("Swap I/Q##soundcard_swap"), &soundCard_.swapIq);
        soundCardCentreMhz_ = soundCard_.iqCentreHz / 1.0e6;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputDouble(labelAboveIfNeeded(trId("Centre (MHz)##soundcard_centre"), 160.0f),
                               &soundCardCentreMhz_, 0.0, 0.0, "%.6f",
                               ImGuiInputTextFlags_EnterReturnsTrue) &&
            std::isfinite(soundCardCentreMhz_)) {
            soundCard_.iqCentreHz = soundCardCentreMhz_ * 1.0e6;
            // A card that is already running in I/Q mode takes the new
            // centre at once: it is only a record of where the external
            // receiver is tuned, and the whole receiver follows it.
            if (sourceKind_ == "soundcard" && !soundCardOpenPending_) {
                applyRetuneNow(soundCard_.iqCentreHz, false);
            }
        }
    }

    // THE RATE: what this card offers for this format.
    std::vector<SoundCardRate> rates;
    if (at >= 0) {
        rates = cascade::source::soundCardRatesFor(soundCardDevices_[static_cast<std::size_t>(at)],
                                                   soundCard_.format);
    }
    std::string ratePreview = soundCardHzText(soundCard_.cardRateHz);
    for (const SoundCardRate& r : rates) {
        if (r.hz == soundCard_.cardRateHz) { ratePreview = rateLabel(r); }
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Sample rate##soundcard_rate"), 160.0f),
                          ratePreview.c_str())) {
        for (const SoundCardRate& r : rates) {
            const bool sel = r.hz == soundCard_.cardRateHz;
            if (ImGui::Selectable(rateLabel(r).c_str(), sel)) { soundCard_.cardRateHz = r.hz; }
            if (r.exclusive && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Exclusive mode: FoxSDR has the card to itself while it "
                                           "runs, at a rate its own hardware runs at."));
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    const bool canOpen = !soundCard_.device.empty() || at >= 0;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(trId("Open##soundcard_open"))) {
        sourceError_.clear();
        launchSoundCardOpen(false);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // WHAT IT RECEIVES, before and after Open, from the settings shown.
    {
        const double rate = cascade::source::soundCardIqRateHz(soundCard_);
        const double centre = cascade::source::soundCardCentreHz(soundCard_);
        const std::string lo = soundCardHzText(std::max(0.0, centre - rate / 2.0));
        const std::string hi = soundCardHzText(centre + rate / 2.0);
        std::string line;
        cascade::core::formatUtf8(line, tr("Receives %s to %s."), lo.c_str(), hi.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", line.c_str());
        // MME TAKES ANY RATE AND WINDOWS RESAMPLES IT. Measured on this
        // product's own bench: a headset microphone whose WASAPI entry offers
        // 48 kHz alone lists every rate from 8 to 384 kHz under MME. A rate
        // above the card's own there adds no bandwidth, only a wider picture
        // of nothing - which is exactly the trap a VLF listener must not fall
        // into.
        if (soundCard_.hostApi == "MME") {
            ImGui::TextWrapped("%s", tr("MME accepts any rate and Windows resamples to it, so a rate "
                                        "above the card's own adds no bandwidth. The Windows WASAPI "
                                        "entry offers the rates the card really runs at."));
        }
        ImGui::PopStyleColor();
    }
    if (!soundCardMissing_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", soundCardMissing_.c_str());
        ImGui::PopStyleColor();
    }
}

}  // namespace cascade::gui
