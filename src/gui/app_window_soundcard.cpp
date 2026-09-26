// app_window_soundcard.cpp - the Source section's SOUND CARD row. AppWindow
// members, kept out of app_window.cpp because they are one subject: listing
// the inputs, opening one on a worker, installing it, bringing it back at
// startup, and tuning a source that has no tuner.
//
// The source itself - the conversion, the name-and-host-API rule, the
// liveness watch - is source/soundcard_source.hpp; the rules a test can reach
// are engine/soundcard_panel.hpp.
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
#include "engine/audio_open.hpp"
#include "engine/soundcard_panel.hpp"
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

std::string rateLabel(const SoundCardRate& r) {
    const std::string hz = soundCardHzText(r.hz);
    if (!r.exclusive) { return hz; }
    std::string out;
    cascade::core::formatUtf8(out, tr("%s (exclusive)"), hz.c_str());
    return out;
}

}  // namespace

void AppWindow::launchSoundCardOpen(bool restore, const SoundCardSettings& settings) {
    if (engine_.soundCardOpenPending_) { return; }
    const std::vector<SoundCardDevice> list = engine_.soundCardDevices_;
    const bool listed = engine_.soundCardListed_;
    // A DEAD CARD ON LINUX is not opened again by its index - ALSA may have
    // given that number to another card plugged in since, which would open
    // under this card's name (gui::soundCardDeadReopenNeedsRestart). Refused
    // with the restart the application asks for after any plug or unplug;
    // the dead card stays installed, and the config goes on naming it.
    if (!restore && engine_.sourceKind_ == "soundcard" &&
        cascade::gui::soundCardDeadReopenNeedsRestart(engine_.installedSoundCardDead(), engine_.soundCardLive_, settings,
                                                      list)) {
        const std::string label = engine_.soundCardLive_.device + " (" + engine_.soundCardLive_.hostApi + ")";
        cascade::core::formatUtf8(engine_.sourceError_,
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
        !restore && cascade::gui::soundCardReopenReleasesFirst(engine_.sourceKind_ == "soundcard", engine_.soundCardLive_,
                                                               settings, list);
    SoundCardSettings previous;
    // NOTHING CHANGES (a dead card picked again, Open pressed as it runs):
    // released all the same, but tried once - there is nothing different to
    // fall back to (gui::soundCardSameSettings).
    const bool same = release && cascade::gui::soundCardSameSettings(settings, engine_.soundCardLive_);
    if (release) {
        previous = engine_.soundCardLive_;
        cascade::core::diagLogf("source: releasing the sound card %s (%s) to open it with new settings",
                                previous.device.c_str(), previous.hostApi.c_str());
        engine_.device_ = nullptr;
        engine_.soapyView_ = nullptr;
        ++engine_.sourceGen_;
        // Through installSource like every other swap: a recording of the
        // card ends here rather than taping the generator standing in.
        engine_.installSource(nullptr);
        engine_.sourceKind_ = "siggen";
        engine_.applyConverterForSource();
        // Until it is back - and for the rest of the session if neither the
        // new settings nor the old ones open - the config goes on naming the
        // card, exactly as after a restore that could not open it.
        engine_.restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
            "soundcard", engine_.cfgSoapyArgs_, engine_.cfgNativeArgs_, std::string(),
            cascade::source::soundCardIqRateHz(previous));
        engine_.soundCardRemembered_ = previous;
        engine_.restoreKeepLabel_.clear();
        followInputRate();
    }
    const std::uint64_t gen = engine_.sourceGen_;
    engine_.soundCardOpenFuture_ =
        std::async(std::launch::async, [settings, list, listed, gen, restore, release, same, previous,
                                        factory = cascade::engine::Engine::soundCardBackendFactory()] {
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
    engine_.soundCardOpenPending_ = true;
}

void AppWindow::pollSoundCard() {
    // The patch page offers every listed card to its radios, so the list is
    // asked for the first time the page is open, as it is the first time the
    // Source section's row is.
    if (patchOpen_ && !engine_.soundCardListed_ && !engine_.soundCardScanPending_ && !engine_.soundCardOpenPending_) {
        engine_.scanSoundCards();
    }
    if (engine_.soundCardScanPending_ && engine_.soundCardScanFuture_.valid() &&
        engine_.soundCardScanFuture_.wait_for(AudioOpen::kNoWait) == std::future_status::ready) {
        engine_.soundCardDevices_ = engine_.soundCardScanFuture_.get();
        engine_.soundCardListed_ = true;
        engine_.soundCardScanPending_ = false;
        cascade::core::diagLogf("source: %zu sound card input(s) listed", engine_.soundCardDevices_.size());
    }
    if (!engine_.soundCardOpenPending_ || !engine_.soundCardOpenFuture_.valid() ||
        engine_.soundCardOpenFuture_.wait_for(AudioOpen::kNoWait) != std::future_status::ready) {
        return;
    }
    SoundCardOpenResult r = engine_.soundCardOpenFuture_.get();
    engine_.soundCardOpenPending_ = false;
    if (!engine_.soundCardListed_ || !r.devices.empty()) {
        engine_.soundCardDevices_ = r.devices;
        engine_.soundCardListed_ = true;
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
        if (r.gen != engine_.sourceGen_) { return; }  // another source was chosen meanwhile
        engine_.soundCard_ = r.previous;
        engine_.restoreKeepLabel_ = std::string(tr("Sound card")) + ": " + r.previous.device;
        engine_.soundCardMissing_.clear();
        if (r.sameAsPrevious) {
            // Nothing new was asked for: one attempt, one reason.
            cascade::core::formatUtf8(engine_.sourceError_, tr("%s did not open (%s)."), label.c_str(),
                                      r.error.c_str());
        } else {
            cascade::core::formatUtf8(engine_.sourceError_,
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
        if (!r.restore && engine_.sourceKind_ == "soundcard") { engine_.soundCard_ = engine_.soundCardLive_; }
        if (missing) {
            cascade::core::formatUtf8(
                engine_.soundCardMissing_,
                tr("%s is not connected, and no other input has been opened in its place. Plug it "
                   "in and restart FoxSDR - sound cards are listed when FoxSDR starts. The setting "
                   "is kept."),
                label.c_str());
            engine_.sourceError_.clear();
        } else {
            engine_.sourceError_ = r.error;
        }
        if (r.restore) {
            // The config goes on naming the card (restoreKeep_ was set when
            // the restore asked), and now the combo says so too.
            engine_.restoreKeepLabel_ = std::string(tr("Sound card")) + ": " + r.wanted.device;
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
    const bool movedOn = r.gen != engine_.sourceGen_ || engine_.deviceOpenPending_ ||
                         (!r.released && engine_.sourceSel_ != kSoundCardRow);
    if (movedOn) {
        cascade::core::diagLogf("source: a sound card open finished after the choice moved on");
        return;
    }
    engine_.sourceSel_ = kSoundCardRow;
    engine_.device_ = nullptr;  // before setSource destroys a live device
    engine_.soapyView_ = nullptr;
    engine_.deviceArgs_.clear();
    engine_.deviceModel_.clear();
    ++engine_.sourceGen_;
    // The settings as OPENED: the rate the card is really running at. The
    // section's copy is what the user edits next; the live copy is what is
    // actually running (the centre box asks it which mode that is).
    engine_.soundCard_ = r.src->settings();
    engine_.soundCardLive_ = engine_.soundCard_;
    engine_.installSource(std::move(r.src));
    engine_.sourceKind_ = "soundcard";
    // THIS CARD'S converter, after the install put the pipeline back to Off
    // and before anything reads the air centre below (see
    // soundCardConverter for what a card can have in front of it).
    engine_.applyConverterForSource();
    engine_.restoreKeep_ = cascade::gui::RememberedSource{};
    engine_.restoreKeepLabel_.clear();
    engine_.soundCardMissing_.clear();
    if (r.restoredPrevious) {
        // THE NEW SETTINGS WERE REFUSED and the card is running again as it
        // was - which is what the section now shows (soundCard_ above).
        cascade::core::formatUtf8(engine_.sourceError_,
                                  tr("%s did not open with the new settings (%s). It is running again as "
                                     "it was."),
                                  label.c_str(), r.error.c_str());
    } else {
        engine_.sourceError_ = r.error;
    }
    followInputRate();
    // THE VFO STAYS WHERE IT WAS if that is inside what the card receives (a
    // saved session's offset is restored before the card finishes opening);
    // otherwise it is brought inside, exactly as a click near the edge is -
    // and CENTRED when the filter is wider than the whole span (WFM's 150 kHz
    // on a 96 kHz card), where there is no inside to bring it to.
    const double off = engine_.pipeline_.vfoOffsetHz();
    const double inside = cascade::gui::vfoOffsetInsideSpan(off, engine_.pipeline_.inputRateHz(), engine_.vfoBandwidthHz_);
    if (inside != off) {
        engine_.pipeline_.setVfoOffsetHz(inside);
        engine_.vfoOffsetKhz_ = static_cast<float>(inside / 1000.0);
    }
    cascade::core::diagLogf("source: opened the sound card %s (%s) at %.0f Hz, %s%s", engine_.soundCard_.device.c_str(),
                            engine_.soundCard_.hostApi.c_str(), engine_.soundCard_.cardRateHz,
                            engine_.soundCard_.format == SoundCardFormat::IqStereo ? "I/Q" : "real",
                            r.restoredPrevious ? " - as it was; the new settings were refused" : "");
}

std::string AppWindow::soundCardReceivesText() const {
    // THE AIR, as the counter reads it: through the converter kept for the
    // SECTION's card, by the section's format (an I/Q card takes none).
    const cascade::core::ConverterSetting stored = cascade::core::converterFor(
        engine_.converters_,
        engine_.resolveConverterKey(cascade::gui::soundCardConverterKey(engine_.soundCard_.device, engine_.soundCard_.hostApi)));
    const cascade::gui::SoundCardAirSpan span = cascade::gui::soundCardAirSpan(engine_.soundCard_, stored);
    const std::string lo = soundCardHzText(span.loHz);
    const std::string hi = soundCardHzText(span.hiHz);
    std::string line;
    cascade::core::formatUtf8(line, tr("Receives %s to %s."), lo.c_str(), hi.c_str());
    return line;
}

void AppWindow::drawSoundCardControls() {
    // The list is asked for the first time the row is shown, never at
    // startup for a user who does not use a sound card.
    if (!engine_.soundCardListed_ && !engine_.soundCardScanPending_ && !engine_.soundCardOpenPending_) { engine_.scanSoundCards(); }

    const std::string label = engine_.soundCard_.device.empty()
                                  ? std::string()
                                  : engine_.soundCard_.device + " (" + engine_.soundCard_.hostApi + ")";
    if (engine_.soundCardOpenPending_) {
        std::string line;
        cascade::core::formatUtf8(line, tr("Opening %s..."), label.c_str());
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", line.c_str());
    } else if (engine_.soundCardScanPending_) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("Scanning for devices..."));
    }
    if (engine_.soundCardListed_ && engine_.soundCardDevices_.empty()) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("No sound card inputs found."));
    }

    const bool busy = engine_.soundCardOpenPending_ || engine_.soundCardScanPending_ || engine_.deviceOpenPending_;
    ImGui::BeginDisabled(busy);

    // THE INPUT. Named by device and host API; a saved card that is not in
    // the list keeps its name in the preview (the missing line below says why).
    const int at = cascade::source::matchSoundCard(engine_.soundCardDevices_, engine_.soundCard_.device, engine_.soundCard_.hostApi,
                                                   engine_.soundCard_.pickedFromList)
                       .at;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Device##soundcard_device")),
                          label.empty() ? "-" : label.c_str())) {
        for (std::size_t i = 0; i < engine_.soundCardDevices_.size(); ++i) {
            const SoundCardDevice& d = engine_.soundCardDevices_[i];
            ImGui::PushID(static_cast<int>(i));
            const bool sel = static_cast<int>(i) == at;
            if (ImGui::Selectable(cascade::source::soundCardDeviceLabel(d).c_str(), sel) && !sel) {
                engine_.soundCard_.device = d.name;
                engine_.soundCard_.hostApi = d.hostApi;
                // Chosen from THIS list: it names exactly this entry, even
                // when an identical card sits beside it (matchSoundCard).
                engine_.soundCard_.pickedFromList = true;
                engine_.soundCardMissing_.clear();
                // Keep the rate if the new card offers it; otherwise its own.
                const auto rates = cascade::source::soundCardRatesFor(d, engine_.soundCard_.format);
                const bool offered = std::any_of(rates.begin(), rates.end(), [&](const SoundCardRate& r) {
                    return r.hz == engine_.soundCard_.cardRateHz;
                });
                if (!offered && !rates.empty()) {
                    engine_.soundCard_.cardRateHz = rates.back().hz;
                    for (const SoundCardRate& r : rates) {
                        if (r.hz == d.defaultRateHz) { engine_.soundCard_.cardRateHz = r.hz; }
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
    int fmt = engine_.soundCard_.format == SoundCardFormat::IqStereo ? 1 : 0;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(labelAboveIfNeeded(trId("Format##soundcard_format"), 160.0f), &fmt, formats, 2)) {
        engine_.soundCard_.format = fmt == 1 ? SoundCardFormat::IqStereo : SoundCardFormat::RealMono;
    }
    if (engine_.soundCard_.format == SoundCardFormat::RealMono) {
        const char* channels[] = {tr("Left"), tr("Right")};
        int ch = engine_.soundCard_.channel == 1 ? 1 : 0;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(labelAboveIfNeeded(trId("Channel##soundcard_channel"), 160.0f), &ch, channels, 2)) {
            engine_.soundCard_.channel = ch;
        }
    } else {
        ImGui::Checkbox(trId("Swap I/Q##soundcard_swap"), &engine_.soundCard_.swapIq);
        soundCardCentreMhz_ = engine_.soundCard_.iqCentreHz / 1.0e6;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputDouble(labelAboveIfNeeded(trId("Centre (MHz)##soundcard_centre"), 160.0f),
                               &soundCardCentreMhz_, 0.0, 0.0, "%.6f",
                               ImGuiInputTextFlags_EnterReturnsTrue) &&
            std::isfinite(soundCardCentreMhz_)) {
            engine_.soundCard_.iqCentreHz = soundCardCentreMhz_ * 1.0e6;
            // A card that is already running IN I/Q MODE takes the new
            // centre at once: it is only a record of where the external
            // receiver is tuned, and the whole receiver follows it. A card
            // running in real mode keeps it for the next Open (see
            // soundCardCentreAppliesLive) - APP_SOUNDCARD_IQ_CENTRE decides,
            // when it is applied.
            if (cascade::gui::soundCardCentreAppliesLive(engine_.sourceKind_ == "soundcard", engine_.soundCardOpenPending_,
                                                         engine_.soundCardLive_.format)) {
                submitCommand(cascade::core::cmd::makeNum(FOXAPP_OP_SOUNDCARD_IQ_CENTRE,
                                                          engine_.soundCard_.iqCentreHz));
            }
        }
    }

    // THE RATE: what this card offers for this format.
    std::vector<SoundCardRate> rates;
    if (at >= 0) {
        rates = cascade::source::soundCardRatesFor(engine_.soundCardDevices_[static_cast<std::size_t>(at)],
                                                   engine_.soundCard_.format);
    }
    std::string ratePreview = soundCardHzText(engine_.soundCard_.cardRateHz);
    for (const SoundCardRate& r : rates) {
        if (r.hz == engine_.soundCard_.cardRateHz) { ratePreview = rateLabel(r); }
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Sample rate##soundcard_rate"), 160.0f),
                          ratePreview.c_str())) {
        for (const SoundCardRate& r : rates) {
            const bool sel = r.hz == engine_.soundCard_.cardRateHz;
            if (ImGui::Selectable(rateLabel(r).c_str(), sel)) { engine_.soundCard_.cardRateHz = r.hz; }
            if (r.exclusive && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Exclusive mode: FoxSDR has the card to itself while it "
                                           "runs, at a rate its own hardware runs at."));
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    const bool canOpen = !engine_.soundCard_.device.empty() || at >= 0;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(trId("Open##soundcard_open"))) {
        // SELECT_SOURCE "soundcard:open": opens the card THIS FORM describes
        // (its settings are not in the id yet - docs/engine-stage1.md, OPEN).
        submitCommand(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // WHAT IT RECEIVES, before and after Open, from the settings shown.
    {
        const std::string line = soundCardReceivesText();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", line.c_str());
        // NO MME NOTE ANY MORE: MME entries are not listed at all (see
        // "ONLY HOST APIs WHOSE DEVICES KEEP THEIR IDENTITY" in
        // source/soundcard_source.hpp), so every rate offered here is one the
        // card really takes.
        ImGui::PopStyleColor();
    }
    if (!engine_.soundCardMissing_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", engine_.soundCardMissing_.c_str());
        ImGui::PopStyleColor();
    }
}

}  // namespace cascade::gui
