// app_window_patch.cpp - the patch page's information pane, its inspector drawer's fold key and the canvas's own view keys (0.99.49 beta
// feedback; 0.99.50). AppWindow members, kept out of app_window.cpp (which is at the object-file limit, src/gui/
// app_window_disk_state.hpp) and out of app_window_patch_radios.cpp (which is about the page's radios) because they are one subject: how
// the page is LAID OUT and how its view is moved.
//
// WHAT THE PAGE LOOKED LIKE BEFORE. A tester on a small laptop could not pan the patch at all (the canvas panned on a middle-button drag
// and a touchpad has no middle button), nothing on the page said how to zoom, and the information column - "This patch can run", "Nothing
// selected", the help - took a full-height strip off the canvas's width while the top right of the page, beside the parts bin, sat empty.
//
//   - A drag on empty canvas pans, and the wheel pans (Ctrl+wheel zooms about the pointer): that is the canvas's own code
//     (gui/patch_view.cpp, the maths in gui/patch_view_math.hpp), identical to master's;
//   - the "+", "-" and "Fit" keys in the canvas's bottom-right corner are drawPatchViewKeys: the ways that need no wheel, no Ctrl and no
//     middle button;
//   - the information pane is drawPatchInfoPane: what the patch as a whole is doing, the selection's problems and the help, in the top
//     band's right-hand part at a fixed height with its own scroll, so the canvas below has the page's whole width;
//   - the selected node's controls are in a drawer at the canvas's right that folds to a strip (drawPatchInspectorFoldKey).
//
// ON THE TABLE (docs/engine-stage4.md 6.5): the pane draws the engine's plan (patchLists().plan, the PATCH_PROBLEMS list and the facts, read
// when listSeq moved) and this frame's readings, and reads the page's draft for the selection; the view keys move the VIEW, which is the
// window's own (patchUi_.view, kept in the config's patch document). Nothing here is a command and nothing here reads an engine field.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/patch_plan.hpp"
#include "gui/fonts.hpp"
#include "gui/patch_view.hpp"
#include "gui/patch_view_math.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"
#include "gui/window_chrome.hpp"

namespace cascade::gui {
using cascade::i18n::tr;

void AppWindow::drawPatchInfoPane(float x, float y, float w, float h) {
    // THE PATCH PAGE'S INFORMATION PANE (0.99.49 beta feedback): what the patch as a whole is doing, the selected node's problems, and the
    // help - everything the old full-height column said, in the top band's right-hand part at a fixed height with its own scroll, so the
    // canvas below has the page's whole width.
    const PatchListCache& lists = patchLists();
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    cascade::gui::census::note("patch:info");
    cascade::gui::census::rect("patch:info", x, y, x + w, y + h);
    if (ImGui::BeginChild("##patchinfo", ImVec2(w, h), true)) {
        // WHAT THE PATCH AS A WHOLE IS DOING, first, because it is the question someone opening this page is actually asking. Phosphor for
        // working, rust for not - and never amber, which belongs to numbers.
        if (lists.plan.runnable) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::good());
            // Wrapped, not TextUnformatted (an Opus review's M4): a translation a third longer than the English - or this same English at the
            // interface size's larger fonts - must go onto a second line rather than off the pane's edge.
            ImGui::TextWrapped("%s", tr("This patch can run."));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
            ImGui::TextWrapped("%s", tr("This patch cannot run yet."));
        }
        ImGui::PopStyleColor();
        // The selected channel's own level, spelled out. The node face has room for a number; this has room to say what it is a number OF.
        for (const auto& r : patchReadings_) {
            if (r.node != patchUi_.selected) { continue; }
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::Text(tr("%.1f dB on this channel"), static_cast<double>(r.db));
            ImGui::PopStyleColor();
            break;
        }
        if (!lists.plan.channels.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::Text(tr("%zu channel(s), decimate by %u"), lists.plan.channels.size(), lists.plan.channels[0].decimation);
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        const cascade::core::patch::Node* sel = patchDraft_.find(patchUi_.selected);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        if (sel == nullptr) {
            ImGui::TextWrapped("%s", tr("Nothing selected."));
            ImGui::Spacing();
            ImGui::TextWrapped("%s", tr(
                "Click a node to set what it does. Drag from one port to "
                "another to wire them; drag a node by its title bar to move "
                "it. Delete removes whatever is selected."));
            ImGui::Spacing();
            // HOW TO MOVE ROUND IT (0.99.49): the wheel pans now, and a touchpad user has no middle button - say what does work.
            ImGui::TextWrapped("%s", tr(
                "Drag empty canvas or scroll to move round the patch. Ctrl+scroll or "
                "the + and - keys zoom; Fit shows every node."));
        } else {
            // The node, by kind and name; its settings are in the drawer.
            ImGui::TextWrapped("%s  %s", tr(cascade::gui::patch::kindCaption(sel->kind)), sel->name.c_str());
            if (patchInspectorFolded_) {
                ImGui::TextWrapped("%s", tr("Its settings are folded away at the right of the "
                                            "canvas - press << to open them."));
            }
        }
        ImGui::PopStyleColor();
        if (sel != nullptr) {
            // THE NODE'S PROBLEMS, here where they are read rather than under its controls: an advisory is worth saying and not worth
            // alarming about, so it is muted rather than rust.
            for (const cascade::core::patch::Problem pr : cascade::core::patch::problemsFor(lists.plan, sel->id)) {
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::core::patch::isAdvisory(pr)
                                                         ? cascade::gui::theme::vec(cascade::gui::theme::kInkMuted)
                                                         : cascade::gui::theme::bad());
                ImGui::TextWrapped("%s", tr(cascade::core::patch::problemText(pr)));
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();
}

void AppWindow::drawPatchInspectorFoldKey() {
    // The bench's lettered key, at the drawer's top right: ">>" folds the drawer to a strip and gives the canvas its width, "<<" on the strip
    // opens it again. The canvas keeps its selection either way. (The fold is the window's own state: it is a layout preference, not a
    // command, and it is not kept in the config.)
    const float keyW = cascade::gui::uiscale::px(24.0f);
    const float keyH = cascade::gui::uiscale::px(20.0f);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    // Centred on the folded strip; at the right of the open drawer.
    const ImVec2 tl(patchInspectorFolded_ ? ImGui::GetWindowPos().x + (ImGui::GetWindowWidth() - keyW) * 0.5f
                                          : std::max(cur.x, right - keyW),
                    cur.y);
    const ImVec2 br(tl.x + keyW, tl.y + keyH);
    const bool pressed = benchWordKey(ImGui::GetWindowDrawList(), tl, br, "", true, "patchinspfold");
    engraveFoldChevrons(ImGui::GetWindowDrawList(), tl, br, patchInspectorFolded_, ImGui::IsItemActive());
    cascade::gui::census::rect("patchinspfold", tl.x, tl.y, br.x, br.y);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", patchInspectorFolded_ ? tr("Open the inspector: the selected node's settings")
                                                      : tr("Fold the inspector away to give the canvas its width"));
    }
    if (pressed) { patchInspectorFolded_ = !patchInspectorFolded_; }
    // An item under the key, so the cursor moved past it is one ImGui has been told about (it refuses a cursor that extends a window by
    // itself).
    ImGui::SetCursorScreenPos(ImVec2(cur.x, br.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void AppWindow::drawPatchViewKeys(float originX, float originY, float width, float height) {
    // Three lettered keys in the canvas's bottom-right corner, the bench's own benchWordKey: zoom out, zoom in (about the canvas centre), and
    // Fit - every node in view (gui::patch::fitView; an empty patch goes home). The wheel and a drag on empty canvas move the view too; these
    // are the ways that need no wheel, no Ctrl and no middle button.
    const float keyH = cascade::gui::uiscale::px(24.0f);
    const float gap = cascade::gui::uiscale::px(4.0f);
    const float inset = cascade::gui::uiscale::px(10.0f);
    const float stepW = cascade::gui::uiscale::px(28.0f);
    const char* fitWord = tr("Fit");
    const float fitW = std::max(
        cascade::gui::uiscale::px(44.0f),
        cascade::gui::fonts::ui()->CalcTextSizeA(cascade::gui::fonts::tinyPx(), FLT_MAX, 0.0f, fitWord).x +
            cascade::gui::uiscale::px(16.0f));
    const float totalW = stepW * 2.0f + fitW + gap * 2.0f;
    if (width < totalW + inset * 2.0f || height < keyH + inset * 2.0f) { return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    struct ViewKey {
        const char* word;
        const char* id;
        const char* tip;
        float w;
    };
    // "+" AND "-" ARE ENGRAVED, NOT LETTERED: at the size a key's word is cut they are a dot and a dash, so they are struck as two bars the
    // width of the key's own hairline scaled up - the way the node's close key is.
    const ViewKey keys[3] = {
        {"", "patchzoomout", tr("Zoom out (Ctrl+scroll also zooms)"), stepW},
        {"", "patchzoomin", tr("Zoom in (Ctrl+scroll also zooms)"), stepW},
        {fitWord, "patchfit", tr("Show every node on the canvas"), fitW},
    };
    float x = originX + width - inset - totalW;
    const float y = originY + height - inset - keyH;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 tl(x, y);
        const ImVec2 br(x + keys[i].w, y + keyH);
        const bool pressed = benchWordKey(dl, tl, br, keys[i].word, true, keys[i].id);
        const bool held = ImGui::IsItemActive();
        cascade::gui::census::rect("patchviewkey:", i, tl.x, tl.y, br.x, br.y);
        if (i < 2) {
            // The same ink benchWordKey cuts a word in, and the same one-pixel drop while the key is held.
            const ImU32 ink = cascade::gui::theme::toneHex(
                0x2A251C, 255, held ? cascade::gui::theme::ink::ActiveText : cascade::gui::theme::ink::CtrlText);
            const float cx = (tl.x + br.x) * 0.5f;
            const float cy = (tl.y + br.y) * 0.5f + (held ? 1.0f : 0.0f);
            const float arm = keyH * 0.22f;
            const float th = std::max(1.5f, cascade::gui::uiscale::px(2.0f));
            dl->AddLine(ImVec2(cx - arm, cy), ImVec2(cx + arm, cy), ink, th);
            if (i == 1) { dl->AddLine(ImVec2(cx, cy - arm), ImVec2(cx, cy + arm), ink, th); }
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", keys[i].tip); }
        if (pressed) {
            // The page's own graph, which holds every edit (the fit is of what is on the canvas, not of the engine's copy a pass behind).
            if (i == 2) {
                patchUi_.view = cascade::gui::patch::fitView(patchDraft_, width, height, cascade::gui::uiscale::factor());
            } else {
                patchUi_.view = cascade::gui::patch::zoomStepView(patchUi_.view, width, height, i == 1);
            }
            patchUi_.dirty = true;
        }
        x += keys[i].w + gap;
    }
}

}  // namespace cascade::gui
