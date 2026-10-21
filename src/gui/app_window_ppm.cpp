// app_window_ppm.cpp - the crystal correction's CONTROLS ("PPM frequency correction", 0.99.56): the Source section's switch and value, and the
// Settings section that carries the same controls. The correction itself - the per-radio memory, applying the value to the open radio, the
// retune, the patch page's radios - is the engine's (engine/engine_ppm.cpp); this file draws what the engine publishes (fact.ppm.*, read
// through ppmLists()) and sends FOXAPP_OP_SET_PPM, and keeps one local draft: the value being typed.
//
// THE OWNER ASKED FOR IT: "add a PPM frequency correction function that you can toggle on in settings". So it is a section of its own under
// Settings (the SYSTEM bank, drawPpmSection), and the same controls sit in the Source section beside the converter (drawPpmControls), because
// that is where every setting that belongs to ONE radio is set - the switch is global, the value is that radio's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/ppm_correction.hpp"
#include "core/utf8_text.hpp"
#include "gui/bench_rail.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;

// THE ENGINE'S PPM FACTS, read again only when listSeq has moved (they move with the switch, the radio, the value and a refusal).
const AppWindow::PpmCache& AppWindow::ppmLists() {
    PpmCache& c = ppmCache_;
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    std::string text;
    readFactText("fact.ppm.on", text);
    c.on = text == "1";
    readFactText("fact.ppm.method", c.method);
    readFactText("fact.ppm.value", text);
    c.stored = std::atof(text.c_str());
    readFactText("fact.ppm.whole", text);
    c.wholeRadio = !text.empty();
    c.wholeValue = std::atoi(text.c_str());
    readFactText("fact.ppm.error", c.error);
    readFactText("fact.ppm.card", c.card);
    readFactText("fact.ppm.model", c.model);
    readFactText("fact.ppm.serial", c.serial);
    readFactText("fact.ppm.key", c.key);
    return c;
}

// The RECEIVER card's "PPM +1.5" line while the switch is on and the radio takes a correction; "" otherwise.
std::string AppWindow::ppmCardLine() {
    const PpmCache& p = ppmLists();
    if (!p.on || p.card.empty()) { return {}; }
    std::string out;
    cascade::core::formatUtf8(out, tr("PPM %+.1f"), std::atof(p.card.c_str()));
    return out;
}

void AppWindow::drawPpmControls() {
    ImGui::SeparatorText(tr("Frequency correction"));
    drawPpmBody();
}

void AppWindow::drawPpmSection() {
    // THE SAME CONTROLS UNDER SETTINGS (the SYSTEM bank - the owner asked for
    // a correction "you can toggle on in settings"), with the value in force
    // on the row's chip. The Source section carries them too, beside the
    // converter, because that is where a radio's own settings are set.
    const PpmCache& p = ppmLists();
    const std::string card = ppmCardLine();
    const char* chip = !p.on ? tr("OFF") : (card.empty() ? tr("ON") : card.c_str());
    if (!railSection(trId("Frequency correction###ppm"), false, chip, cascade::gui::theme::kPhosphor, p.on)) {
        return;
    }
    drawPpmBody();
}

void AppWindow::drawPpmBody() {
    const PpmCache& p = ppmLists();
    // Not while a radio is being opened: the value would land on whichever
    // radio happened to be installed at that instant.
    ImGui::BeginDisabled((uiState_.flags & FOXAPI_RX_DEVICE_OPENING) != 0u);
    bool on = p.on;
    if (ImGui::Checkbox(trId("PPM frequency correction"), &on)) {
        uiSubmit(cascade::core::cmd::makeNumInt(FOXAPP_OP_SET_PPM, p.stored, on ? 1 : 0));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", tr("Corrects the error of this radio's crystal, in parts per million. Positive "
                     "when stations show up below their real frequency. Tune to a signal whose "
                     "frequency you know exactly and change the value until it sits where it "
                     "should. The switch is for every radio; the value is remembered for each "
                     "radio."));
    }
    if (p.on) {
        if (p.method == "na") {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", tr("Not used for the signal generator, a sound card or an I/Q "
                                        "file - none has a crystal of its own to correct."));
            ImGui::PopStyleColor();
        } else {
            // WHICH RADIO the value is for - the serial is what tells two
            // identical dongles apart. On screen only; never logged.
            std::string whose;
            if (p.serial.empty()) {
                cascade::core::formatUtf8(whose, tr("For %s"), p.model.c_str());
            } else {
                cascade::core::formatUtf8(whose, tr("For %s, serial %s"), p.model.c_str(), p.serial.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", whose.c_str());
            ImGui::PopStyleColor();

            // Seeded from the remembered value whenever the radio or the
            // value changes under it; while the user types, what they type
            // stays.
            const std::string seed = p.key + "\x1f" + cc::ppmText(p.stored);
            if (ppmEditSeededFor_ != seed) {
                ppmEdit_ = p.stored;
                ppmEditSeededFor_ = seed;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool stepped = ImGui::InputDouble("##ppm_value", &ppmEdit_, cc::kPpmStep, 1.0,
                                                    "%+.1f ppm", ImGuiInputTextFlags_EnterReturnsTrue);
            if (stepped || ImGui::IsItemDeactivatedAfterEdit()) {
                const double v = cc::sanitisePpm(ppmEdit_);
                ppmEdit_ = v;
                if (v != p.stored) {
                    uiSubmit(cascade::core::cmd::makeNumInt(FOXAPP_OP_SET_PPM, v, 1));
                } else {
                    ppmEditSeededFor_.clear();  // tidy a clamped entry back to its value
                }
            }

            // HOW it is applied, which is not the same promise: a radio that
            // corrects itself corrects its sample rate too; the retune fixes
            // the centre frequency and nothing else.
            std::string how;
            if (p.method == "radio") {
                if (p.wholeRadio) {
                    cascade::core::formatUtf8(how, tr("Corrected in the radio itself, in whole ppm: %+d."), p.wholeValue);
                } else {
                    how = tr("Corrected in the radio itself.");
                }
            } else {
                how = tr("Corrected by retuning - the centre frequency only; the sample rate keeps "
                         "its error.");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", how.c_str());
            ImGui::PopStyleColor();
            if (!p.error.empty()) {
                std::string msg;
                cascade::core::formatUtf8(msg, tr("The radio refused the correction: %s"), p.error.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
                ImGui::TextWrapped("%s", msg.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndDisabled();
}

}  // namespace cascade::gui
