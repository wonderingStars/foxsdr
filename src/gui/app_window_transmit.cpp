// app_window_transmit.cpp - THE TRANSMIT PAGE'S PLUGIN CONTROLS: ARM FOR PLUGINS, OFF and DATA (flat), and the frame that tells the engine the page is still on screen (stub campaign
// B13, TX-PLUGIN; master 0.99.74's CASCADE_CAP_TX_AUDIO). AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
//
// SAFETY-CRITICAL, AND THE WINDOW'S PART OF IT IS DELIBERATELY SMALL. A plugin may key the transmitter only while the operator has GRANTED it (on the plugin's page in Fitted
// modules) and ARMED a window here (5, 15 or 60 minutes) - and in this tree both of those, and the key itself, are the ENGINE's (engine/engine_tx_plugin.cpp). The window holds no
// state that could key a radio and no function that could: it DRAWS fact.tx.plugin.* and SENDS three LOCAL ops that are not on the web profile (docs/api-0.3-additions.md 9.10):
//   - FOXAPP_OP_TX_PLUGIN_ARM, from the 5 / 15 / 60 MIN keys and OFF - and, once a frame, "the TRANSMIT page is still on screen" (ival[0] -1) while a window is open, or "the page
//     is not on screen" (OFF, reason 1) when it is not. The engine holds the window as a LEASE on this window's session: it lasts while the session is open, beats and keeps saying
//     the page is live, and a window that crashed, froze or closed its page lets it go within a second (so nothing this file fails to do can leave a plugin able to key);
//   - FOXAPP_OP_TX_DATA_FLAT, from the DATA (flat) tick;
//   - FOXAPP_OP_PLUGIN_TX_GRANT, from the plate's GRANT / REVOKE TRANSMIT key (app_window_fitted.cpp).
// The page never touches the transmitter, never decides whether a plugin may key, and does not count the minutes: the countdown is the engine's seconds (fact.tx.plugin.secondsLeft).
//
// WHAT IS HELD HERE, by tests/test_tx_audio_page_app.cpp (the real binary: the keys drawn and where, a press with no radio open REFUSED with the reason on the page, DATA (flat)
// a real tick) and tests/test_tx_audio_app.cpp (the real engine behind the page's own commands: what a press, a closed page and a silent window do).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/theme.hpp"
#include "gui/transmit_page.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;
namespace cmd = cascade::core::cmd;

namespace {

// A fact that is a whole decimal number, 0 when it is not one (or is not there).
int factInt(const std::string& text) { return text.empty() ? 0 : std::atoi(text.c_str()); }

}  // namespace

void AppWindow::drawTransmitPluginBlock() {
    // THE OPERATOR'S HALF OF A PLUGIN'S KEY, and it lives on this page for the reason the PTT does: every control that can put RF out of a connector is where somebody looking
    // at the lamp can reach it. A plugin that has been granted TRANSMIT (on its page in Fitted modules) can key this transmitter only while a window opened HERE is open - 5, 15
    // or 60 minutes, closing itself, never saved, never restored - and only while this page is on screen. OFF closes it at once. The keys are enabled with no radio open, because
    // pressing one then is how the page says why it cannot (the refusal below): a greyed key that explains nothing is a key that looks broken.
    ImGui::Separator();
    const bool armed = uiFact("fact.tx.plugin.armed") == "1";
    const int armedMinutes = armed ? factInt(uiFact("fact.tx.plugin.minutes")) : 0;
    ImGui::TextUnformatted(tr("ARM FOR PLUGINS"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Lets a plugin you have granted TRANSMIT key this radio, for\n"
                                   "the time you choose. It chooses nothing else: the\n"
                                   "frequency, the power and the mode are the ones on this page,\n"
                                   "and any fault, closing this page or the time running out\n"
                                   "stops it. Nothing here is remembered next time."));
    }
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        const int minutes = cascade::core::TxArming::kChoicesMinutes[i];
        const bool on = armed && armedMinutes == minutes;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, cascade::gui::theme::vec(cascade::gui::theme::kBrassDark));
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::legible(cascade::gui::theme::kPhosphor,
                                                                                                        cascade::gui::theme::kBrassDark)));
        }
        const char* label = i == 0 ? trId("5 MIN##txarm5") : i == 1 ? trId("15 MIN##txarm15") : trId("60 MIN##txarm60");
        // A PRESS IS A REQUEST, sent once a frame by submitTransmitPluginArming: the engine judges it (a radio open, a plugin granted) and the refusal comes back as a fact.
        if (ImGui::Button(label, ImVec2(70.0f, 0.0f))) { transmitPluginArmRequest_ = minutes; }
        if (cascade::gui::census::enabled()) {
            // The key drawn, and where (tests/test_tx_audio_page_app).
            const std::string nm = "transmit:arm" + std::to_string(minutes);
            cascade::gui::census::note(nm);
            cascade::gui::census::rect(nm, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        }
        if (on) { ImGui::PopStyleColor(2); }
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("OFF##txarmoff"), ImVec2(70.0f, 0.0f))) { transmitPluginArmRequest_ = 0; }
    cascade::gui::census::note("transmit:armoff");
    cascade::gui::census::note(armed ? "transmit:armed" : "transmit:notarmed");
    if (armed) {
        char left[16];
        cascade::gui::formatTxArmCountdown(left, sizeof(left), factInt(uiFact("fact.tx.plugin.secondsLeft")));
        std::string line;
        cascade::core::formatUtf8(line, tr("ARMED: %s may key this transmitter, %s left."), uiFact("fact.tx.plugin.name").c_str(), left);
        const std::string& keyed = uiFact("fact.tx.plugin.keyed");
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(!keyed.empty() ? cascade::gui::theme::kAlarmHot : cascade::gui::theme::kAmber));
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
        if (!keyed.empty()) {
            std::string onAirLine;
            cascade::core::formatUtf8(onAirLine, tr("ON AIR for %s."), keyed.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAlarmHot));
            ImGui::TextWrapped("%s", onAirLine.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkFaint));
        ImGui::TextWrapped("%s", tr("Not armed: no plugin can key this transmitter."));
        ImGui::PopStyleColor();
    }
    // WHY THE LAST ARM WAS REFUSED, in the words the page prints under the keys (the engine publishes the reason as a number; the sentence is this file's, translated).
    const int refusal = factInt(uiFact("fact.tx.plugin.refusal"));
    if (refusal != 0) {
        cascade::gui::census::note("transmit:refusal", refusal);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAlarmHot));
        ImGui::TextWrapped("%s", tr(cascade::gui::txArmRefusalText(static_cast<cascade::core::TxArming::Refusal>(refusal))));
        ImGui::PopStyleColor();
    }
    // WHY THE WINDOW LAST CLOSED, when it did not close by the operator's own OFF: the sentence the log carries, so a plugin that lost its key never leaves the person at the desk
    // wondering.
    if (!armed) {
        const std::string& why = uiFact("fact.tx.plugin.reason");
        if (!why.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextWrapped("%s", why.c_str());
            ImGui::PopStyleColor();
        }
    }
    // DATA (flat): for a plugin's audio only, and the engine's setting (session-only, on by default); drawn from its fact, a click is a request.
    bool dataFlat = uiFact("fact.tx.plugin.dataFlat") != "0";
    if (ImGui::Checkbox(trId("DATA (flat)"), &dataFlat)) { uiSubmit(cmd::makeInt(FOXAPP_OP_TX_DATA_FLAT, dataFlat ? 1 : 0)); }
    if (cascade::gui::census::enabled()) {
        cascade::gui::census::note(uiFact("fact.tx.plugin.dataFlat") != "0" ? "transmit:dataflat:on" : "transmit:dataflat:off");
        cascade::gui::census::rect("transmit:dataflat", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("A plugin's audio goes out as NFM with the pre-emphasis off, whatever\n"
                                   "mode is selected above: a data modem's tones must reach the far\n"
                                   "receiver flat. Untick it to send the plugin's audio in the mode\n"
                                   "selected above instead. The microphone and the tone are never\n"
                                   "affected."));
    }
}

void AppWindow::submitTransmitPluginArming() {
    if (uiSession_ == nullptr) { return; }
    // WHAT THE PAGE ASKED THIS FRAME is sent alone: the engine judges it, and from the next frame "still here" says the page is on screen (gui::txPluginArmCommands is the rule, so the
    // tests that stand in for this frame say the same thing).
    TxPluginArmFrame f;
    f.pageLive = transmitPageLive_;
    f.pressed = transmitPluginArmRequest_;
    f.armedShown = uiFact("fact.tx.plugin.armed") == "1";
    f.nowS = ImGui::GetTime();
    FoxCommand batch[kTxPluginArmMaxCommands];
    const std::uint32_t n = txPluginArmCommands(f, transmitPluginArm_, batch);
    for (std::uint32_t i = 0; i < n; ++i) { uiSubmit(batch[i]); }
}

}  // namespace cascade::gui
