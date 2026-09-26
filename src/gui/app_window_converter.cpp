// app_window_converter.cpp - the up- or down-converter in front of the radio
// (0.99.36): the Source section's Converter rows, the per-radio memory behind
// them, and the sentences that say what a converter means for a tune. AppWindow
// members, kept out of app_window.cpp because they are one subject.
//
// THE OWNER APPROVED THIS FROM A BETA TESTER'S REQUEST. He receives VLF - SAQ
// Grimeton on 17.2 kHz - through home-built up-converters with 2 MHz, 100 MHz
// and 125 MHz local oscillators; the common Ham-It-Up style converters use
// 125 MHz. Until now he had to tune the radio to 125.0172 MHz and do the
// subtraction himself, and every preset, bookmark, band plan entry and
// decoder frequency in the application was 125 MHz away from what he heard.
//
// WHAT THIS FILE DOES NOT DO IS CONVERT. core/freq_converter.hpp holds the
// arithmetic and the pipeline applies it (Pipeline::activeSource() speaks the
// AIR frequency; see source/converter_view.hpp). This file decides WHICH
// setting is in force - the installed radio's own, remembered under
// core::converterRadioKey - and lets the user change it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "engine/soundcard_panel.hpp"
#include "gui/theme.hpp"
#include "engine/tune_control.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;

namespace {

// The three local oscillators the tester uses, 125 MHz first because it is
// the common Ham-It-Up's.
constexpr double kQuickLoHz[] = {125.0e6, 100.0e6, 2.0e6};

// The LO a mode switched on with none remembered starts from.
constexpr double kDefaultLoHz = 125.0e6;

}  // namespace

std::string AppWindow::converterAliasNote() {
    const std::string raw = engine_.converterRawKeyNow();
    // Said until the user sets a converter here themselves.
    if (engine_.converterNotCarried_.count(raw) != 0 && !cc::converterActive(engine_.pipeline_.converter())) {
        return tr("Opened through SoapySDR because the native driver refused this radio. The "
                  "converter set for it was not carried over: with no serial number the two "
                  "drivers cannot be shown to be the same dongle. Set one here if this radio "
                  "needs it.");
    }
    if (engine_.converterKeyAlias_.count(raw) == 0) { return {}; }
    if (!cc::converterActive(engine_.pipeline_.converter())) { return {}; }
    return tr("Opened through SoapySDR because the native driver refused this radio - the "
              "converter set for it still applies.");
}

std::string AppWindow::converterStatusLine(bool shortForm) {
    const cc::ConverterSetting conv = engine_.pipeline_.converter();
    if (!cc::converterActive(conv)) { return {}; }
    // THE RADIO'S OWN READBACK, not a sum: this line exists so that what the
    // radio reports (its lights, another program, its own display) is on the
    // screen beside the air frequency the counter shows. The short form names
    // the converter by its LO alone, for a column too narrow for the full
    // name (an inverting converter's is the longest).
    const double radioHz = engine_.pipeline_.rawSource().centerFrequencyHz();
    const std::string name =
        shortForm ? cc::converterHzText(conv.loHz) + " LO" : engine_.converterName(conv);
    std::string out;
    cascade::core::formatUtf8(out, tr("via %s - radio at %s"), name.c_str(),
                              cc::converterHzText(radioHz).c_str());
    return out;
}

std::vector<cc::ConverterMode> AppWindow::converterModesOffered() const {
    // A SOUND CARD (gui::soundCardConverter): none at all in I/Q mode - the
    // typed centre is the translation - and no up-converter in real mode.
    if (engine_.sourceKind_ == "soundcard") {
        if (engine_.soundCardLive_.format == cascade::source::SoundCardFormat::IqStereo) { return {}; }
        return {cc::ConverterMode::Off, cc::ConverterMode::Down};
    }
    return {cc::ConverterMode::Off, cc::ConverterMode::Up, cc::ConverterMode::Down};
}

std::string AppWindow::converterUnusableNote() const {
    if (engine_.sourceKind_ != "soundcard") { return {}; }
    const cc::ConverterSetting stored = cc::converterFor(engine_.converters_, engine_.converterRadioKeyNow());
    if (!cascade::gui::soundCardConverter(stored, engine_.soundCardLive_.format).upRefused) { return {}; }
    return tr("A sound card takes a down-converter only: the up-converter set for this card is not used.");
}

void AppWindow::drawConverterControls() {
    const std::vector<cc::ConverterMode> offered = converterModesOffered();
    if (offered.empty()) { return; }
    const std::string key = engine_.converterRadioKeyNow();
    const auto stored = engine_.converters_.find(key);
    cc::ConverterSetting mine =
        stored == engine_.converters_.end() ? cc::ConverterSetting{} : stored->second;
    // A stored mode this source cannot take reads as Off (the note below says
    // why); its LO is kept for the radio or mode that can.
    if (std::find(offered.begin(), offered.end(), mine.mode) == offered.end()) {
        mine.mode = cc::ConverterMode::Off;
    }
    const cc::ConverterSetting live = engine_.pipeline_.converter();
    // Every change below is APP_SET_CONVERTER, applied at the top of the next
    // frame by changeConverter (the air frequency stays, the radio follows).
    const auto submitConverter = [this](const cc::ConverterSetting& s) {
        FoxCommand c = cascade::core::cmd::makeInt(FOXAPP_OP_SET_CONVERTER, static_cast<std::int64_t>(s.mode),
                                                   s.inverted ? 1 : 0);
        c.num[0] = s.loHz;
        submitCommand(c);
    };

    ImGui::SeparatorText(tr("Converter"));
    // Not while a radio is being opened: the setting would land on whichever
    // radio happened to be installed at that instant.
    ImGui::BeginDisabled(engine_.deviceOpenPending_);

    // --- the mode ----------------------------------------------------------------
    std::vector<const char*> modeNames;
    int mode = 0;
    for (std::size_t i = 0; i < offered.size(); ++i) {
        modeNames.push_back(offered[i] == cc::ConverterMode::Up     ? tr("Up-converter")
                            : offered[i] == cc::ConverterMode::Down ? tr("Down-converter")
                                                                    : tr("Off"));
        if (offered[i] == mine.mode) { mode = static_cast<int>(i); }
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##converter_mode", &mode, modeNames.data(), static_cast<int>(modeNames.size()))) {
        cc::ConverterSetting next = mine;
        next.mode = offered[static_cast<std::size_t>(mode)];
        if (next.mode != cc::ConverterMode::Off && !cc::converterLoValid(next.loHz)) {
            next.loHz = kDefaultLoHz;
        }
        submitConverter(next);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          tr("An up- or down-converter between the antenna and this radio. FoxSDR "
                             "then shows and tunes the frequency on the air, and tells the radio "
                             "the converted one. Remembered for this radio only."));
    }
    // The SoapySDR fallback for a dongle the native driver refused: the same
    // radio under another key, and the converter set for it still in force.
    if (const std::string note = converterAliasNote(); !note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }
    if (const std::string note = converterUnusableNote(); !note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }

    if (mine.mode != cc::ConverterMode::Off) {
        // --- the local oscillator -------------------------------------------------
        // Seeded from the remembered LO whenever the radio or the LO changes
        // under it; while the user types, what they type stays.
        const std::string seed = key + "\x1f" + cc::converterHzText(mine.loHz);
        if (converterLoSeededFor_ != seed) {
            std::snprintf(converterLoBuf_, sizeof(converterLoBuf_), "%s",
                          cc::converterLoValid(mine.loHz) ? cc::converterHzText(mine.loHz).c_str()
                                                          : "");
            converterLoSeededFor_ = seed;
            converterLoBad_ = false;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Local oscillator"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputText("##converter_lo", converterLoBuf_, sizeof(converterLoBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Type it in Hz, kHz or MHz - 125 MHz, 125000 kHz and "
                                       "125000000 Hz are the same. A number with no unit is MHz."));
        }
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            double lo = 0.0;
            if (cc::parseConverterLoHz(converterLoBuf_, lo)) {
                converterLoBad_ = false;
                if (lo != mine.loHz) {
                    cc::ConverterSetting next = mine;
                    next.loHz = lo;
                    submitConverter(next);
                } else {
                    converterLoSeededFor_.clear();  // tidy the text back to its canonical form
                }
            } else {
                converterLoBad_ = true;
            }
        }
        // The three the tester's converters use, as one-press choices.
        for (int i = 0; i < 3; ++i) {
            if (i > 0) { ImGui::SameLine(); }
            const std::string label = cc::converterHzText(kQuickLoHz[i]) + "##converter_quick" +
                                      std::to_string(i);
            const bool on = mine.loHz == kQuickLoHz[i];
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(label.c_str()) && !on) {
                cc::ConverterSetting next = mine;
                next.loHz = kQuickLoHz[i];
                submitConverter(next);
            }
            if (on) { ImGui::PopStyleColor(); }
        }

        // --- inversion ------------------------------------------------------------
        bool inv = mine.inverted;
        if (ImGui::Checkbox(trId("Inverts the spectrum (LO above the signal)"), &inv)) {
            cc::ConverterSetting next = mine;
            next.inverted = inv;
            submitConverter(next);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s", tr("Tick this when the converter's oscillator sits ABOVE the signal (a "
                         "high-side LO): the radio is then tuned to the oscillator minus the "
                         "frequency, and the band arrives back to front. FoxSDR turns it the "
                         "right way round. Most up-converters, a Ham-It-Up included, do not."));
        }

        if (converterLoBad_) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
            std::string msg;
            cascade::core::formatUtf8(msg, tr("could not read frequency \"%s\""), converterLoBuf_);
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        }

        // --- what it means right now ---------------------------------------------
        if (cc::converterActive(live)) {
            // THE STATION, not the band centre: with the VFO parked off-centre
            // the centre can sit below 0 Hz on the air (a 16.4 kHz station
            // with the VFO 300 kHz up), which is true and says nothing useful.
            // The status column carries the radio's own centre readback.
            const double airHz = engine_.currentAbsoluteHz();
            const double radioHz = cc::radioFromAir(live, airHz);
            std::string line;
            cascade::core::formatUtf8(line, tr("Listening on %s on the air - %s at the radio."),
                                      cc::converterHzText(airHz).c_str(),
                                      cc::converterHzText(radioHz).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndDisabled();
}

}  // namespace cascade::gui
