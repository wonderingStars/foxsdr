// app_window_handover.cpp - THE LEGACY WINDOW'S WAY BACK TO THE NEW ONE (engine fixes 7; core/window_handover.hpp has the rules, the order and the test seams).
//
// A "Window layout" section in the SYSTEM bank, beside Display (where the theme and the interface size are): one line saying what the Legacy window is, one key "Switch to the new
// window". The key checks that the new program is there (the window stays, with a sentence, when it is not), asks when a recording or a patch is running, and otherwise ends this run
// the ordinary way: run() returns, the engine stops (its radios close, the last save is made, the settings lock goes), and main() - once this object and the service are gone -
// starts the new window and does not wait. AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdlib>
#include <string>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/window_handover.hpp"
#include "gui/bench_rail.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;
namespace handover = cascade::core::handover;

namespace {
// FOR THE UI CENSUS ONLY (a no-op unless FOXSDR_UI_CENSUS names a file): the rectangle of the item just drawn, so a test aims its press.
void censusRect(const char* name) {
    if (!cascade::gui::census::enabled()) { return; }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
}
}  // namespace

void AppWindow::drawWindowSection() {
    // VERIFICATION ONLY, the house rule FOXSDR_OPEN_LANGUAGE follows: the section's open state is ImGui's own storage, so nothing in a config file can open it for a headless capture.
    if (std::getenv("FOXSDR_OPEN_WINDOW_LAYOUT") != nullptr) { ImGui::SetNextItemOpen(true, ImGuiCond_Once); }
    if (!railSection(trId("Window layout"), false, "LEGACY", cascade::gui::theme::kPhosphor, false)) { return; }
    railHint(tr("This is the Legacy window, FoxSDR's previous layout. The new window is the current one, on the same settings and the same radio."));
    if (!handoverNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", handoverNote_.c_str());
        ImGui::PopStyleColor();
    }
    if (handoverAsking_) {
        ImGui::TextWrapped("%s", handoverAsk_.c_str());
        if (ImGui::Button(trId("Switch"))) { handoverGo(); }
        censusRect("handover-switch-yes");
        ImGui::SameLine();
        if (ImGui::Button(trId("Stay here"))) { handoverAsking_ = false; }
        return;
    }
    if (ImGui::Button(trId("Switch to the new window"))) { handoverChoose(); }
    censusRect("handover-switch");
}

void AppWindow::handoverChoose() {
    if (handoverRequested_) { return; }
    const std::string missing = handover::preflight(handover::Window::New);
    if (!missing.empty()) {
        handoverNote_ = missing;   // stays here, with the sentence
        return;
    }
    handoverNote_.clear();
    // What is running, as the status column reads it: a take that is active (I/Q or audio), a patch that is running.
    handover::Busy busy;
    for (const int idx : {statusTakeIq_, statusTakeAudio_}) {
        if (idx >= 0 && readTake(idx, idx == statusTakeIq_ ? 1.0 : 2.0).active) { busy.recording = true; }
    }
    busy.patch = (uiState_.flags & FOXAPI_RX_PATCH_RUNNING) != 0u;
    if (busy.any()) {
        handoverAsk_ = handover::busyQuestion(busy, handover::Window::New);
        handoverAsking_ = true;
        return;
    }
    handoverGo();
}

void AppWindow::handoverGo() {
    handoverAsking_ = false;
    handoverRequested_ = true;
    closeRequested_ = true;   // run()'s loop ends; main() does the rest once the engine is stopped and the lock is let go
    cascade::core::diagLogf("window: switching to the new window");
}

void AppWindow::handoverPoll() {
    if (handoverAt_ < 0) {
        const char* e = std::getenv(handover::kAtEnv);
        handoverAt_ = e != nullptr ? std::atoi(e) : 0;
    }
    ++handoverFrames_;
    if (handoverAt_ > 0 && handoverFrames_ == handoverAt_) { handoverChoose(); }
}

}  // namespace cascade::gui
