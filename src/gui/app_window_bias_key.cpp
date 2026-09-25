// app_window_bias_key.cpp - the deck's BIAS TEE key (2026-09-25): what a press
// and the confirmation dialog do, the one switch the key and the Source
// panel's checkbox share, and the stand-in seam the census and captures use.
// AppWindow members, kept out of app_window.cpp because they are one subject.
// The key itself is drawn in drawToolbar, among the deck's other parts.
//
// A tester asked for a bias-tee button and the owner's words were "add bias
// tee to the main panel". The rules - drawn only for a radio that has one,
// lit from the driver's readback, off at once, on only after a deliberate
// "yes" the first time per radio per session - are gui/bias_tee.hpp's
// (BiasKeyGate), where tests/test_bias_key.cpp holds them without a frame;
// tests/test_bias_key_app.cpp drives these members against the shipping
// HackRF driver on a fake USB transport.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include <imgui.h>

#include "core/i18n.hpp"
#include "gui/bias_tee.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// The stand-in's identity: a serial-named radio, so the "not asked again"
// half of the gate can be seen in a capture as well as the first question.
constexpr const char* kStandInKind = "stand-in";
constexpr const char* kStandInArgs = "serial=5A4E0001";

}  // namespace

bool AppWindow::biasTeeReachable() const {
    // biasTeePanel_.present is written after each OPEN and by nothing else, so
    // on its own it outlives the radio: a HackRF closed for the generator left
    // it true (test_bias_key_app [5] caught the key still drawn over the
    // generator). The Source panel's checkbox never showed that, because it is
    // drawn inside the open device's own panel; the deck has no such frame. So
    // the key asks the question live - a radio IS open, it is still answering,
    // and withBiasTee still reaches a bias tee on it (an RSPdx answers per
    // antenna).
    //
    // A RADIO THAT HAS STOPPED ANSWERING HAS NO KEY (repair round 1, F3). Its
    // driver refuses every transfer from then on, so a key over it could only
    // ever be pressed for nothing, and its lamp would be a readback the radio
    // can no longer confirm; the FAIL lamp beside it and the Source panel are
    // where a dead radio is reported, and the reopen that follows a driver
    // fault puts the key back with the radio.
    return biasTeePanel_.present && device_ != nullptr && !device_->deviceDead() &&
           withBiasTee(device_, [](auto&) { return true; });
}

cascade::gui::BiasTeePanel AppWindow::biasKeyPanel() const {
    if (!biasStandInActive()) {
        cascade::gui::BiasTeePanel p = biasTeePanel_;
        p.present = biasTeeReachable();
        return p;
    }
    cascade::gui::BiasTeePanel p;
    p.present = true;
    p.shown = biasStandInOn_;
    return p;
}

std::string AppWindow::biasKeyRadioNow() const {
    if (biasStandInActive()) { return biasKeyRadio(kStandInKind, kStandInArgs); }
    return biasKeyRadio(sourceKind_, deviceArgs_);
}

bool AppWindow::biasKeyMayRememberNow() const {
    // The stand-in is a serial-named radio with an unbounded memory of one.
    if (biasStandInActive()) { return biasKeyMayRemember(kStandInArgs); }
    // A real radio: exactly when an "on" switched now will be kept and put
    // back at its next open (review round 2, L2) - which is also when the
    // session may skip the question next time (the gate's mayRemember).
    return device_ != nullptr && biasTeeWillRestoreOn(biasTeePanel_, *device_, deviceArgs_);
}

void AppWindow::switchBiasTee(bool want) {
    if (biasStandInActive()) {
        // A stand-in driver: it takes the change, or refuses it and says so
        // exactly where a real driver's refusal is shown.
        if (biasStandIn_ == BiasStandIn::Accept) {
            biasStandInOn_ = want;
        } else {
            sourceError_ = "stand-in bias tee (FOXSDR_FORCE_BIAS_KEY=refuse): switching the "
                           "bias-T was refused";
        }
        return;
    }
    // The checkbox's own path, unchanged: request, then show the READBACK; a
    // refusal leaves the box, the key and the memory where they were.
    std::string err;
    biasTeeTicked(biasTeePanel_, device_, deviceArgs_, want, &err);
    if (!err.empty()) { sourceError_ = err; }
}

void AppWindow::biasKeyPressed() {
    switch (biasKeyPress(biasKeyGate_, biasKeyPanel(), biasKeyRadioNow())) {
        case BiasKeyAction::Nothing: break;
        case BiasKeyAction::SwitchOff: switchBiasTee(false); break;
        case BiasKeyAction::SwitchOn: switchBiasTee(true); break;
        case BiasKeyAction::Ask: biasKeyAskQueued_ = true; break;
    }
}

void AppWindow::biasKeyAnswered(bool turnOn) {
    if (!turnOn) {
        biasKeyCancel(biasKeyGate_);
        return;
    }
    if (biasKeyConfirm(biasKeyGate_, biasKeyPanel(), biasKeyRadioNow(),
                       biasKeyMayRememberNow())) {
        switchBiasTee(true);
    }
}

void AppWindow::drawBiasKeyConfirm() {
    const char* title = trId("Turn on the bias tee?##bias_key_confirm");
    if (biasKeyAskQueued_) {
        ImGui::OpenPopup(title);
        biasKeyAskQueued_ = false;
    }
    // Centred on the main window, every frame and not movable - the mute
    // popup's reasons (drawMutePopup), which apply to any short modal.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        // Gone without an answer (nothing here closes it that way today, but
        // a question left pending would make the NEXT press's dialog answer
        // this one): withdraw it.
        if (biasKeyGate_.asking) { biasKeyCancel(biasKeyGate_); }
        return;
    }
    // THE QUESTION MUST STILL APPLY: the radio it asked about is still the
    // open one and still has a bias tee. A radio closed or switched while the
    // dialog was up leaves nothing to say yes to.
    if (!biasKeyQuestionStands(biasKeyGate_, biasKeyPanel(), biasKeyRadioNow())) {
        biasKeyCancel(biasKeyGate_);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    // Which radio: the driver's own name for it.
    if (biasStandInActive()) {
        ImGui::TextUnformatted("stand-in radio (FOXSDR_FORCE_BIAS_KEY)");
    } else if (device_ != nullptr) {
        ImGui::TextUnformatted(device_->name());
    }
    // THE CHECKBOX'S OWN WARNING, word for word and already translated: the
    // one sentence that says why this is being asked.
    ImGui::TextWrapped("%s",
                       tr("Sends about 4.5 V up the antenna cable to power an amplifier at the "
                          "mast. Leave it off unless you have one: equipment that is not "
                          "expecting power on the connector can be damaged by it."));
    // WHAT "YES" LEAVES BEHIND, said truthfully (repair rounds 1 and 2): the
    // restore is promised only when the "on" will really be kept and put back
    // (biasTeeWillRestoreOn). Every other case - no serial or an all-zeros
    // one, an RX888 opened through its bootloader, an RTL-SDR with no EEPROM,
    // a full memory - gets ONE sentence that is true of all of them, rather
    // than one per reason: FoxSDR will not be the thing that switches it on.
    if (biasKeyMayRememberNow()) {
        ImGui::TextWrapped("%s", tr("FoxSDR will switch it on again whenever this radio is "
                                    "opened, until you turn it off."));
    } else {
        ImGui::TextWrapped("%s", tr("FoxSDR cannot remember this for this radio: it will not "
                                    "switch it on again when the radio is next opened."));
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // For the bounded-run test (tests/test_bias_key_run.cpp): the dialog was
    // up, and where its "Turn it on" key is, so a script can press it.
    cascade::gui::census::note("dialog:bias_key_confirm");
    if (ImGui::Button(trId("Turn it on##bias_key_confirm"))) {
        biasKeyAnswered(true);
        ImGui::CloseCurrentPopup();
    }
    {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        cascade::gui::census::rect("dialog:bias_key_confirm.yes", a.x, a.y, b.x, b.y);
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("Cancel##bias_key_confirm"))) {
        biasKeyAnswered(false);
        ImGui::CloseCurrentPopup();
    }
    // THE SAFE ANSWER IS THE DEFAULT: keyboard focus starts on Cancel, so a
    // second Enter or Space after pressing the key from the keyboard does not
    // put power on the connector.
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

}  // namespace cascade::gui
