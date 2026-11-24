// app_window_refusal_popup.cpp - A REFUSED RADIO CHOICE SAYS WHY, the classic window's half (0.99.113; the rules, the words and the state machine are core/radio_refusal.hpp's).
//
// THE FAULT. Choosing a radio in the Source list that a Radio part of the saved patch names is refused by the engine (FOXAPI_BUSY, "in use by patch radio N") and this window drew nothing:
// the answer to a SELECT_SOURCE was never read. Two things now tell the person. The Source section's error line (fact.status.sourceError, which the engine sets with a sentence naming the
// part) needs nothing here. THIS FILE is the second: a modal over the window - a notice with two keys, "Open the Patch page" and "Close" - raised once for the refusal the person just
// caused by clicking in THIS window, because the click's ticket was noted (submitSourceChoice) and its answer comes back through actOnUiAnswers. A refusal answered to another session
// (a remote client, a script) never reaches this session's answers and so raises nothing here.
//
// It is NOT the hand-off question (that is the Slint window's and goes through the engine's hand-off state): nothing here submits FOXAPP_OP_RADIO_HANDOFF, counts a failed open or
// files a report, and no key changes the patch, a part or who holds the radio. The modal's look is the window's other modals' (drawMutePopup): centred every frame, auto-sized, not movable.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <imgui.h>

#include "gui/refusal_gate.hpp"

namespace cascade::gui {

cascade::core::refusal::RefusalPopup& AppWindow::refusalPopup() {
    if (!refusalPopup_) {
        cascade::core::refusal::RefusalPopup::Hooks h;
        h.mayShow = [] { return mayShowRefusalPopup(); };
        h.show = [this](const cascade::core::refusal::Popup& p) {
            refusalPopupText_ = p;
            refusalPopupOpenRequest_ = true;
        };
        h.hide = [] {};   // the modal closes itself on the frame it finds the state closed (drawRefusalPopup)
        h.openPatchPage = [this] { setMainViewPatch(true); };   // the same switch as the rail's view keys and the Patch row
        // The caption the Patch page draws on that part, by the id the refusal carried. The engine's PUBLISHED graph (patchLook, the one the page adopts its draft from) answers whether or not the
        // Patch page has been open: the page's own draft is only brought up to date on a frame that draws it. The draft answers when the look cannot.
        h.partCaption = [this](int part) {
            const auto id = static_cast<cascade::core::patch::NodeId>(part);
            const PatchLook& l = patchLook();
            if (l.valid) {
                if (const cascade::core::patch::Node* n = l.graph.find(id)) { return n->name; }
            }
            const cascade::core::patch::Node* n = patchDraft_.find(id);
            return n != nullptr ? n->name : std::string();
        };
        refusalPopup_ = std::make_unique<cascade::core::refusal::RefusalPopup>(std::move(h));
    }
    return *refusalPopup_;
}

// The Source list's click: the command every other path sends, with its ticket and the row's label noted so the answer is known for this window's own.
void AppWindow::submitSourceChoice(const std::string& id, const std::string& rowLabel) {
    const std::uint64_t ticket = uiSubmitTicket(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id));
    refusalPopup().noteSubmitted(ticket, rowLabel);
}

void AppWindow::drawRefusalPopup() {
    if (!refusalPopup_) { return; }
    const std::string id = refusalPopupText_.title + "##refusal_popup";
    // Opened here, at the top level, as drawMutePopup's is: OpenPopup and BeginPopupModal in one ID stack.
    if (refusalPopupOpenRequest_) {
        refusalPopupOpenRequest_ = false;
        ImGui::OpenPopup(id.c_str());
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) { return; }
    if (!refusalPopup_->open()) {   // put away by a key (or never opened): close with it
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextWrapped("%s", refusalPopupText_.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button(refusalPopupText_.goLabel.c_str())) {
        refusalPopup_->goToPatchPage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(refusalPopupText_.stayLabel.c_str())) {
        refusalPopup_->dismiss();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace cascade::gui
