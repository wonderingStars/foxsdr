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
// SINCE THE WINDOW'S PORT (engine-stage4.md 6.5, group 4) THESE MEMBERS DRAW
// FROM THEIR ENGINE API SESSION ONLY. What they read is the table's: the
// CONVERTERS list (the open radio's stored setting is the entry that is ACTIVE;
// its value[3] is the radio's own centre, read live), and the facts
// fact.converter.radioKey / .name / .live / .alias / .modes / .upIgnored (API
// 0.3 section 8.3), plus FOXAPI_RX_DEVICE_OPENING and the state's tunedHz. The
// two sentences about a SoapySDR fallback and an unusable up-converter stay
// here, translated here; the engine says only WHICH, or none. Every change is
// FOXAPP_OP_SET_CONVERTER through uiSubmit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "engine/soundcard_panel.hpp"
#include "gui/converter_lo_seed.hpp"
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

// THE CONVERTER ROWS' READS OF THE TABLE, taken when listSeq has moved and kept
// (the same rule as AppWindow::uiFact): the modes this source offers, the
// converter in force, and the open radio's stored setting - the CONVERTERS
// entry that is ACTIVE, and where it is (the radio's own centre is read from
// that entry each time it is wanted: it is live, and moves with every retune).
const AppWindow::ConverterListCache& AppWindow::converterLists() {
    ConverterListCache& c = converterCache_;
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    // fact.converter.modes: "0,1,2", "0,2" or "" - the codes FOXAPP_OP_SET_CONVERTER takes.
    c.offered.clear();
    for (const char* p = uiFact("fact.converter.modes").c_str(); *p != '\0';) {
        char* end = nullptr;
        const long code = std::strtol(p, &end, 10);
        if (end == p) { break; }
        if (code >= 0 && code <= 2) { c.offered.push_back(static_cast<cc::ConverterMode>(code)); }
        p = (*end == ',') ? end + 1 : end;
    }
    // fact.converter.live: "<mode> <LO Hz> <inverted>" - the converter in force.
    c.live = cc::ConverterSetting{};
    {
        int mode = 0;
        double lo = 0.0;
        int inverted = 0;
        if (std::sscanf(uiFact("fact.converter.live").c_str(), "%d %lf %d", &mode, &lo, &inverted) == 3 && mode >= 0 &&
            mode <= 2) {
            c.live.mode = static_cast<cc::ConverterMode>(mode);
            c.live.loHz = lo;
            c.live.inverted = inverted != 0;
        }
    }
    // The open radio's stored setting: the CONVERTERS entry that is ACTIVE
    // (none: the default one, a radio nothing was ever set for).
    c.activeIndex = -1;
    c.stored = cc::ConverterSetting{};
    const int32_t entries = engineApi_->read_list(uiSession_, FOXAPI_LIST_CONVERTERS, 0, nullptr);
    for (int32_t i = 0; i < entries; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_CONVERTERS, static_cast<uint32_t>(i), &it) < 0) { break; }
        if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u) { continue; }
        c.activeIndex = i;
        if (it.value[0] >= 0.0 && it.value[0] <= 2.0) {
            c.stored.mode = static_cast<cc::ConverterMode>(static_cast<int>(it.value[0]));
        }
        c.stored.loHz = it.value[1];
        c.stored.inverted = it.value[2] != 0.0;
        break;
    }
    return c;
}

std::string AppWindow::converterAliasNote() {
    const std::string& state = uiFact("fact.converter.alias");
    // Said until the user sets a converter here themselves.
    if (state == "notCarried") {
        return tr("Opened through SoapySDR because the native driver refused this radio. The "
                  "converter set for it was not carried over: with no serial number the two "
                  "drivers cannot be shown to be the same dongle. Set one here if this radio "
                  "needs it.");
    }
    if (state == "carried") {
        return tr("Opened through SoapySDR because the native driver refused this radio - the "
                  "converter set for it still applies.");
    }
    return {};
}

std::string AppWindow::converterStatusLine(bool shortForm) {
    const ConverterListCache& c = converterLists();
    const cc::ConverterSetting& conv = c.live;
    if (!cc::converterActive(conv)) { return {}; }
    // THE RADIO'S OWN READBACK, not a sum: this line exists so that what the
    // radio reports (its lights, another program, its own display) is on the
    // screen beside the air frequency the counter shows. It is the CONVERTERS
    // entry's value[3], read live (it moves with every retune). The short form
    // names the converter by its LO alone, for a column too narrow for the full
    // name (an inverting converter's is the longest).
    double radioHz = 0.0;
    if (c.activeIndex >= 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_CONVERTERS, static_cast<uint32_t>(c.activeIndex), &it) >= 0) {
            radioHz = it.value[3];
        }
    }
    const std::string name = shortForm ? cc::converterHzText(conv.loHz) + " LO" : uiFact("fact.converter.name");
    std::string out;
    cascade::core::formatUtf8(out, tr("via %s - radio at %s"), name.c_str(),
                              cc::converterHzText(radioHz).c_str());
    return out;
}

const std::vector<cc::ConverterMode>& AppWindow::converterModesOffered() {
    // A SOUND CARD (gui::soundCardConverter): none at all in I/Q mode - the
    // typed centre is the translation - and no up-converter in real mode. The
    // engine decides (fact.converter.modes); EMPTY means the rows are not drawn.
    return converterLists().offered;
}

std::string AppWindow::converterUnusableNote() {
    if (uiFact("fact.converter.upIgnored") != "1") { return {}; }
    return tr("A sound card takes a down-converter only: the up-converter set for this card is not used.");
}

void AppWindow::drawConverterControls() {
    const std::vector<cc::ConverterMode>& offered = converterModesOffered();
    if (offered.empty()) { return; }
    const ConverterListCache& cache = converterLists();
    const std::string key = uiFact("fact.converter.radioKey");
    cc::ConverterSetting mine = cache.stored;
    // A stored mode this source cannot take reads as Off (the note below says
    // why); its LO is kept for the radio or mode that can.
    if (std::find(offered.begin(), offered.end(), mine.mode) == offered.end()) {
        mine.mode = cc::ConverterMode::Off;
    }
    const cc::ConverterSetting live = cache.live;
    // Every change below is FOXAPP_OP_SET_CONVERTER, applied by the engine's
    // next pass by changeConverter (the air frequency stays, the radio follows).
    const auto submitConverter = [this](const cc::ConverterSetting& s) {
        FoxCommand c = cascade::core::cmd::makeInt(FOXAPP_OP_SET_CONVERTER, static_cast<std::int64_t>(s.mode),
                                                   s.inverted ? 1 : 0);
        c.num[0] = s.loHz;
        uiSubmit(c);
    };

    ImGui::SeparatorText(tr("Converter"));
    // Not while a radio is being opened: the setting would land on whichever
    // radio happened to be installed at that instant.
    ImGui::BeginDisabled((uiState_.flags & FOXAPI_RX_DEVICE_OPENING) != 0u);

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
        // (The seed names the radio AND its stored setting: the engine's onConverterChanged hook used to clear it from the pass
        // when anything about the converter changed; now the window sees the change in what it reads. gui/converter_lo_seed.hpp.)
        const std::string seed = converterLoSeed(key, mine);
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
            const double airHz = uiState_.tunedHz;  // = currentAbsoluteHz(), as of the last publish
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
