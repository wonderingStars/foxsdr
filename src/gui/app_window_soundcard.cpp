// app_window_soundcard.cpp - the Source section's SOUND CARD row. AppWindow
// members, kept out of app_window.cpp because they are one subject: the
// panel under the combo (the Device, Format, Channel, Swap I/Q, Centre and
// Sample rate controls, the Open key and "Receives X to Y.").
//
// IT DRAWS FROM THE WINDOW'S ENGINE API SESSION ONLY (window port group 11,
// API 0.3 section 8.10): the inputs, the rates the form's card offers and the
// form itself are the table's SOUND_CARDS, SOUND_CARD_RATES and
// SOUND_CARD_FORM (read when listSeq moves, AppWindow::sourceLists), where the
// listing stands is the state's sourceFlags word, and what a control changes
// goes by uiSubmit: FOXAPP_OP_SOUND_CARD_PICK for a card chosen from the list,
// FOXAPP_OP_SOUND_CARD_FORM carrying ONE field (its mask names it) for each of
// the others, SOUNDCARD_IQ_CENTRE after every centre committed, and
// SELECT_SOURCE "soundcard:open" for Open. Listing the inputs, opening one on
// a worker, installing it and bringing it back at startup are the engine's
// (engine_soundcard.cpp).
//
// The source itself - the conversion, the name-and-host-API rule, the
// liveness watch - is source/soundcard_source.hpp; the rules a test can reach
// are core/soundcard_panel.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/soundcard_panel.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardSettings;

namespace {

std::string rateLabel(double hz, bool exclusive) {
    const std::string text = soundCardHzText(hz);
    if (!exclusive) { return text; }
    std::string out;
    cascade::core::formatUtf8(out, tr("%s (exclusive)"), text.c_str());
    return out;
}

// ONE FIELD OF THE FORM, in one command (API 0.3 8.10): the mask names the field the command carries, and every
// other field keeps the engine's own value - so a control that changed one thing cannot write back a stale copy of
// another that a pass changed since the window last read it (a worker's open result replaces the whole form).
cascade::core::cmd::QueuedCommand oneField(const SoundCardSettings& s, std::int64_t field) {
    return cascade::gui::soundCardFormCommand(s, field);
}

}  // namespace

std::string AppWindow::soundCardReceivesText() {
    // THE AIR, as the counter reads it: through the converter kept for the
    // SECTION's card, by the section's format (an I/Q card takes none) - the
    // two figures the engine worked out (fact.soundcard.span).
    const SourceListCache& c = sourceLists();
    const std::string lo = soundCardHzText(c.spanLoHz);
    const std::string hi = soundCardHzText(c.spanHiHz);
    std::string line;
    cascade::core::formatUtf8(line, tr("Receives %s to %s."), lo.c_str(), hi.c_str());
    return line;
}

void AppWindow::drawSoundCardControls() {
    // The list is asked for the first time the row is shown, never at
    // startup for a user who does not use a sound card: the engine's pass
    // lists the inputs while the combo's row is the sound card and nothing
    // is listed, being listed or opening (it was a command from this frame,
    // every frame until the list arrived).
    const SourceListCache& c = sourceLists();
    const SourceListCache::Form& form = c.form;
    const std::uint32_t src = uiState_.sourceFlags;
    const bool cardOpening = (src & FOXAPI_SRC_CARD_OPENING) != 0u;
    const bool cardScanning = (src & FOXAPI_SRC_CARD_SCANNING) != 0u;
    const bool cardListed = (src & FOXAPI_SRC_CARD_LISTED) != 0u;

    const std::string& label = c.cardLabel;
    if (cardOpening) {
        std::string line;
        cascade::core::formatUtf8(line, tr("Opening %s..."), label.c_str());
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", line.c_str());
    } else if (cardScanning) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("Scanning for devices..."));
    }
    if (cardListed && c.cards.empty()) {
        ImGui::TextColored(cascade::gui::theme::warning(), "%s", tr("No sound card inputs found."));
    }

    const bool busy = cardOpening || cardScanning || (uiState_.flags & FOXAPI_RX_DEVICE_OPENING) != 0u;
    ImGui::BeginDisabled(busy);

    // THE INPUT. Named by device and host API; a saved card that is not in
    // the list keeps its name in the preview (the missing line below says why).
    // `at` is the entry the form's card is (SOUND_CARDS' ACTIVE), matched the way the open matches it.
    const int at = c.cardAt;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Device##soundcard_device")),
                          label.empty() ? "-" : label.c_str())) {
        for (std::size_t i = 0; i < c.cards.size(); ++i) {
            const SourceListCache::Card& d = c.cards[i];
            ImGui::PushID(static_cast<int>(i));
            const bool sel = static_cast<int>(i) == at;
            // A click on the row the form's card already is does NOTHING, as it never did: the pick marks the card
            // as chosen from the list, and the engine applies it whenever it is sent.
            if (ImGui::Selectable(d.name.c_str(), sel) && !sel) {
                // The engine puts the card into the form (chosen from THIS list: it names exactly this entry, even
                // when an identical card sits beside it), clears the "not connected" line and keeps the rate if the
                // new card offers it in the form's format, else its own default, else its highest.
                uiSubmit(cascade::core::cmd::makeInt(FOXAPP_OP_SOUND_CARD_PICK, static_cast<std::int64_t>(d.id)));
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // THE FORMAT, and what goes with it. A click here shows the OLD dependent controls for one more pass (the
    // Channel combo where Swap I/Q and Centre should appear, the rate list and the "Receives" line): the form is
    // the engine's, and what a click sends lands at its next pass.
    const char* formats[] = {tr("Real (mono)"), tr("I/Q (stereo)")};
    int fmt = form.iqStereo ? 1 : 0;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(labelAboveIfNeeded(trId("Format##soundcard_format"), 160.0f), &fmt, formats, 2)) {
        SoundCardSettings s;
        s.format = fmt == 1 ? SoundCardFormat::IqStereo : SoundCardFormat::RealMono;
        uiSubmit(oneField(s, FOXAPP_SOUND_CARD_FIELD_FORMAT));
    }
    if (!form.iqStereo) {
        const char* channels[] = {tr("Left"), tr("Right")};
        int ch = form.channel == 1 ? 1 : 0;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(labelAboveIfNeeded(trId("Channel##soundcard_channel"), 160.0f), &ch, channels, 2)) {
            SoundCardSettings s;
            s.channel = ch;
            uiSubmit(oneField(s, FOXAPP_SOUND_CARD_FIELD_CHANNEL));
        }
    } else {
        bool swapIq = form.swapIq;
        if (ImGui::Checkbox(trId("Swap I/Q##soundcard_swap"), &swapIq)) {
            SoundCardSettings s;
            s.swapIq = swapIq;
            uiSubmit(oneField(s, FOXAPP_SOUND_CARD_FIELD_SWAP_IQ));
        }
        // THE CENTRE IS A DRAFT: the box shows the figure typed until the engine's form carries it (the fact is
        // read a pass after the Return that sent it), or two seconds have gone by and the engine's is the truth.
        // It was re-seeded from the engine's field on every frame.
        if (soundCardCentrePending_ &&
            (form.centreHz == soundCardCentreSentHz_ || ImGui::GetTime() - soundCardCentreEditedAt_ > 2.0)) {
            soundCardCentrePending_ = false;
        }
        if (!soundCardCentrePending_) { soundCardCentreMhz_ = form.centreHz / 1.0e6; }
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputDouble(labelAboveIfNeeded(trId("Centre (MHz)##soundcard_centre"), 160.0f),
                               &soundCardCentreMhz_, 0.0, 0.0, "%.6f",
                               ImGuiInputTextFlags_EnterReturnsTrue) &&
            std::isfinite(soundCardCentreMhz_)) {
            const double iqCentreHz = soundCardCentreMhz_ * 1.0e6;
            soundCardCentrePending_ = true;
            soundCardCentreSentHz_ = iqCentreHz;
            soundCardCentreEditedAt_ = ImGui::GetTime();
            // THE FORM ALWAYS TAKES IT (the record of where the external
            // receiver is tuned, kept for the next Open either way).
            SoundCardSettings s;
            s.iqCentreHz = iqCentreHz;
            uiSubmit(oneField(s, FOXAPP_SOUND_CARD_FIELD_CENTRE));
            // A card that is already running IN I/Q MODE ALSO takes it at
            // once: the whole receiver follows it live. A card running in
            // real mode keeps only the form update above, for the next Open:
            // the engine answers NO_CHANGE when no I/Q card is running, so
            // this is sent after every centre committed and the window does
            // not consult the live card (the two commands reach the engine in
            // the order they are sent).
            uiSubmit(cascade::core::cmd::makeNum(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, iqCentreHz));
        }
    }

    // THE RATE: what this card offers for this format (SOUND_CARD_RATES; empty when the form's card is not listed).
    std::string ratePreview = soundCardHzText(form.rateHz);
    for (const SourceListCache::CardRate& r : c.cardRates) {
        if (r.active) { ratePreview = rateLabel(r.hz, r.exclusive); }
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Sample rate##soundcard_rate"), 160.0f),
                          ratePreview.c_str())) {
        for (const SourceListCache::CardRate& r : c.cardRates) {
            if (ImGui::Selectable(rateLabel(r.hz, r.exclusive).c_str(), r.active)) {
                SoundCardSettings s;
                s.cardRateHz = r.hz;
                uiSubmit(oneField(s, FOXAPP_SOUND_CARD_FIELD_RATE));
            }
            if (r.exclusive && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Exclusive mode: FoxSDR has the card to itself while it "
                                           "runs, at a rate its own hardware runs at."));
            }
            if (r.active) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    const bool canOpen = !form.device.empty() || at >= 0;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(trId("Open##soundcard_open"))) {
        // SELECT_SOURCE "soundcard:open": opens the card THE ENGINE'S FORM describes
        // (its settings are not in the id yet - docs/engine-stage1.md, OPEN).
        uiSubmit(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"));
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
    if (!statusLines_.soundCardMissing.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", statusLines_.soundCardMissing.c_str());
        ImGui::PopStyleColor();
    }
}

}  // namespace cascade::gui
