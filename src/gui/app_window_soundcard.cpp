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

// THE WHOLE FORM, IN ONE COMMAND (engine/stage3b-pre fields-to-commands round
// 2, docs/engine-stage3.md OPEN item 1): every widget below used to write
// engine_.soundCard_ in place; each now takes a COPY of the current form,
// changes its one field, and applies the copy back through this - at once
// (applyCommand, not submitCommand), so nothing else drawn later in the same
// frame (the rate list, the "Receives..." preview, all of which still read
// engine_.soundCard_ directly) ever sees a stale value.
void submitSoundCardForm(cascade::engine::Engine& engine, const SoundCardSettings& s) {
    const cascade::core::cmd::QueuedCommand q = cascade::gui::soundCardFormCommand(s);
    engine.applyCommand(q.c, q.longText);
}

std::string rateLabel(const SoundCardRate& r) {
    const std::string hz = soundCardHzText(r.hz);
    if (!r.exclusive) { return hz; }
    std::string out;
    cascade::core::formatUtf8(out, tr("%s (exclusive)"), hz.c_str());
    return out;
}

}  // namespace

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
    // A COMMAND (engine/stage3b-pre, docs/engine-stage3.md OPEN 2), applied
    // at once as the direct call was; the engine decides whether a list is
    // actually taken (not listed, not listing, no card opening).
    if (!engine_.soundCardListed_) {
        (void)engine_.applyCommand(cascade::core::cmd::make(FOXAPP_OP_SOUND_CARDS_WANTED));
    }

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
                SoundCardSettings form = engine_.soundCard_;
                form.device = d.name;
                form.hostApi = d.hostApi;
                // Chosen from THIS list: it names exactly this entry, even
                // when an identical card sits beside it (matchSoundCard).
                form.pickedFromList = true;
                engine_.applyCommand(cascade::core::cmd::makeInt(
                    FOXAPP_OP_CLEAR_STATUS, cascade::core::cmd::FOXAPP_STATUS_SOUND_CARD_MISSING));
                // Keep the rate if the new card offers it; otherwise its own.
                const auto rates = cascade::source::soundCardRatesFor(d, form.format);
                const bool offered = std::any_of(rates.begin(), rates.end(), [&](const SoundCardRate& r) {
                    return r.hz == form.cardRateHz;
                });
                if (!offered && !rates.empty()) {
                    form.cardRateHz = rates.back().hz;
                    for (const SoundCardRate& r : rates) {
                        if (r.hz == d.defaultRateHz) { form.cardRateHz = r.hz; }
                    }
                }
                submitSoundCardForm(engine_, form);
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
        SoundCardSettings form = engine_.soundCard_;
        form.format = fmt == 1 ? SoundCardFormat::IqStereo : SoundCardFormat::RealMono;
        submitSoundCardForm(engine_, form);
    }
    if (engine_.soundCard_.format == SoundCardFormat::RealMono) {
        const char* channels[] = {tr("Left"), tr("Right")};
        int ch = engine_.soundCard_.channel == 1 ? 1 : 0;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(labelAboveIfNeeded(trId("Channel##soundcard_channel"), 160.0f), &ch, channels, 2)) {
            SoundCardSettings form = engine_.soundCard_;
            form.channel = ch;
            submitSoundCardForm(engine_, form);
        }
    } else {
        bool swapIq = engine_.soundCard_.swapIq;
        if (ImGui::Checkbox(trId("Swap I/Q##soundcard_swap"), &swapIq)) {
            SoundCardSettings form = engine_.soundCard_;
            form.swapIq = swapIq;
            submitSoundCardForm(engine_, form);
        }
        soundCardCentreMhz_ = engine_.soundCard_.iqCentreHz / 1.0e6;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputDouble(labelAboveIfNeeded(trId("Centre (MHz)##soundcard_centre"), 160.0f),
                               &soundCardCentreMhz_, 0.0, 0.0, "%.6f",
                               ImGuiInputTextFlags_EnterReturnsTrue) &&
            std::isfinite(soundCardCentreMhz_)) {
            const double iqCentreHz = soundCardCentreMhz_ * 1.0e6;
            // THE FORM ALWAYS TAKES IT (the record of where the external
            // receiver is tuned, kept for the next Open either way).
            SoundCardSettings form = engine_.soundCard_;
            form.iqCentreHz = iqCentreHz;
            submitSoundCardForm(engine_, form);
            // A card that is already running IN I/Q MODE ALSO takes it at
            // once: the whole receiver follows it live. A card running in
            // real mode keeps only the form update above, for the next Open
            // (see soundCardCentreAppliesLive) - APP_SOUNDCARD_IQ_CENTRE
            // decides, when it is applied.
            if (cascade::gui::soundCardCentreAppliesLive(engine_.sourceKind_ == "soundcard", engine_.soundCardOpenPending_,
                                                         engine_.soundCardLive_.format)) {
                engine_.submitCommand(cascade::core::cmd::makeNum(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, iqCentreHz));
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
            if (ImGui::Selectable(rateLabel(r).c_str(), sel)) {
                SoundCardSettings form = engine_.soundCard_;
                form.cardRateHz = r.hz;
                submitSoundCardForm(engine_, form);
            }
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
        engine_.submitCommand(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"));
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
