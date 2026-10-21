// app_window_import_dialog.cpp - the "Import a frequency list" window (0.99.80).
//
// WHY IT EXISTS. A field report (SWL 20807, French interface, FoxSDR 0.99.78): "frequency export
// works fine, but import does not. When I type the name of the file to load, it shows an error
// (import impossible: cannot open)". The Import key sat disabled until something was typed, the
// box wanted a PATH, and the only other way in - dropping the file anywhere on the window - was
// written nowhere a person would look. The owner's request: "make a drag and drop window for it
// when they try to import".
//
// WHAT IT IS. A centred modal with a drop zone drawn as a target in words (the drop itself is
// the window's own GLFW callback, which imports a dropped file wherever the pointer is - nothing
// here accepts it), a BROWSE key that opens the system's file dialog (only where there is one:
// the 0.99.74 rule, never a key that does nothing) and Cancel. It opens
//   * when Import is pressed with its box empty - the Bookmarks section's, or the AIRBAND
//     preset's (rows then join that preset), and
//   * when any import comes back with no rows (pollDiskJobs), carrying the failure sentence.
// It closes itself when an import comes back WITH rows (pollDiskJobs), and stays open, showing the
// new sentence, when one comes back with none. A file dropped while it is open goes to the
// target it remembers (consumePendingDrop below, which the frame loop calls).
//
// THE STATE is four members of AppWindow (importDialogOpen_, importDialogQueued_,
// importDialogInto_, importDialogReason_); openImportDialog() only sets them, because the keys
// that ask are drawn inside the rail, before the root level exists, and ImGui wants OpenPopup and
// BeginPopupModal in one ID stack - the deferral drawMutePopup() uses.
//
// A FILE OF ITS OWN because app_window.cpp is at the edge of what an MSVC object file holds
// (error C1128, see app_window_disk_work.cpp); the call sites stay there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <imgui.h>

#include "core/file_pick.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

void AppWindow::openImportDialog(const cascade::core::ImportInto& into, const std::string& reason) {
    importDialogInto_ = into;
    importDialogReason_ = reason;
    importDialogOpen_ = true;
    // ALWAYS queued: drawImportDialog() opens the popup unless it is already showing, so a window
    // that is down by the time the frame draws it (closed by Escape or Cancel earlier in this very
    // frame, say) comes back for a failure that arrived since, and one that is up is not re-opened.
    importDialogQueued_ = true;
}

void AppWindow::consumePendingDrop() {
    if (pendingDropPath_.empty()) { return; }
    const std::string dropped = std::move(pendingDropPath_);
    pendingDropPath_.clear();
    // While the "Import a frequency list" window is open a file dropped on the window goes where
    // that window says (a preset's rows join the preset); otherwise as the file says.
    importBookmarkFile(dropped, importDialogOpen_ ? importDialogInto_ : cascade::core::ImportInto{});
}

void AppWindow::drawImportDialog() {
    const char* const title = trId("Import a frequency list###import_list");
    if (importDialogQueued_) {
        importDialogQueued_ = false;
        if (importDialogOpen_ && !ImGui::IsPopupOpen(title)) { ImGui::OpenPopup(title); }
    }
    // Centred on the main window and re-centred every frame with NoMove, for the reason
    // drawMutePopup() gives: an auto-resizing popup placed on its first frame lands off centre.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        // Not up: never opened, or closed some other way. The state follows the window, so a file
        // dropped next goes where a drop goes with no window, not to a stale target.
        importDialogOpen_ = false;
        importDialogReason_.clear();
        return;
    }
    // ESCAPE CLOSES IT, read here: Dear ImGui 1.92 closes a popup on Escape but leaves a MODAL one
    // open (NavUpdateCancelRequest skips ImGuiWindowFlags_Modal), so without this line the key did
    // nothing - measured by tests/test_import_dialog_app.cpp's `escape` scenario. Like Cancel, it
    // forgets the sentence.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { importDialogOpen_ = false; }
    // An import the window was waiting for came back with rows (pollDiskJobs cleared the state), or
    // it was cancelled: closed.
    if (!importDialogOpen_) {
        importDialogReason_.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool forPreset = !importDialogInto_.group.empty();
    const float fontPx = ImGui::GetFontSize();
    const float gap = ImGui::GetStyle().ItemSpacing.y;

    // THE DROP ZONE'S SIZE: 380 x 110 at least, wider for a longer translation (a window is as
    // wide as its zone, and the sentences wrap at that width), never wider than the window.
    const char* const dropLine = tr("Drop an SDR# frequencies.xml or a CSV here");
    const char* const anyLine = tr("Anywhere on the FoxSDR window works too.");
    const ImVec2 dropSize = ImGui::CalcTextSize(dropLine);
    const ImVec2 anySize = ImGui::CalcTextSize(anyLine);
    const float pad = 1.4f * fontPx;
    float zoneW = std::max({380.0f, dropSize.x + 2.0f * pad, anySize.x + 2.0f * pad});
    zoneW = std::min(zoneW, std::max(380.0f, vp->Size.x - 64.0f));
    const float zoneH = std::max(110.0f, dropSize.y + gap + anySize.y + 2.0f * pad);
    const float left = ImGui::GetCursorPosX();  // the content's left edge, for centring below
    const auto centre = [&](float itemW) { ImGui::SetCursorPosX(left + std::max(0.0f, (zoneW - itemW) * 0.5f)); };

    // 1. WHY IT OPENED, when an import failed: the sentence the section's note holds, muted.
    if (!importDialogReason_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        ImGui::PushTextWrapPos(left + zoneW);
        ImGui::Text("%s", importDialogReason_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    // 2. WHERE THE ROWS GO, for an AIRBAND preset's.
    if (forPreset) {
        ImGui::PushTextWrapPos(left + zoneW);
        ImGui::Text("%s", cascade::core::formatText(tr("The rows will join the preset \"%s\"."),
                                                    importDialogInto_.group.c_str())
                              .c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }

    // 3. THE DROP ZONE: a target in words. Nothing here takes the drop.
    {
        const ImVec2 lo = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(zoneW, zoneH));
        const ImVec2 hi(lo.x + zoneW, lo.y + zoneH);
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        dl->AddRect(lo, hi, cascade::gui::theme::kInkMuted, 8.0f, 0, 1.5f);
        const float textH = dropSize.y + gap + anySize.y;
        const float y0 = lo.y + (zoneH - textH) * 0.5f;
        dl->AddText(ImVec2(lo.x + (zoneW - dropSize.x) * 0.5f, y0), ImGui::GetColorU32(ImGuiCol_Text), dropLine);
        dl->AddText(ImVec2(lo.x + (zoneW - anySize.x) * 0.5f, y0 + dropSize.y + gap),
                    cascade::gui::theme::kInkMuted, anyLine);
        census::rect("import:dropzone", lo.x, lo.y, hi.x, hi.y);
    }

    const bool reading = bookmarkImportPending();
    const float keyW = std::max(120.0f, 7.0f * fontPx);

    // 4. BROWSE, only where there is a file dialog to open (the seam stands in for one in a
    //    bounded run): "or", then the key.
    if (filePickOffered_) {
        const char* const orWord = tr("or");
        centre(ImGui::CalcTextSize(orWord).x);
        ImGui::TextUnformatted(orWord);
        centre(keyW);
        ImGui::BeginDisabled(reading);
        if (ImGui::Button(trId("Browse...##import_browse"), ImVec2(keyW, 0.0f))) {
            // WHERE THE DIALOG OPENS: beside the typed path, else the recordings folder, where
            // Export writes. A bare name is looked for there too (importBookmarkFile), and
            // pickStartFolder takes the PARENT of a file path, so a name that does not exist
            // there still opens the dialog in that folder (pickStartPath, core/file_pick.hpp).
            char* const field = forPreset ? airbandImportPath_ : bookmarkImportPath_;
            const std::size_t fieldCap = forPreset ? sizeof(airbandImportPath_) : sizeof(bookmarkImportPath_);
            cascade::core::FilePickRequest rq;
            rq.title = tr("Choose a frequency list");
            rq.startFrom = cascade::core::pickStartPath(field, recordDir_, "frequencies.xml");
            rq.lists = true;
            rq.listsName = tr("Frequency lists (*.xml, *.csv)");
            rq.allFilesName = tr("All files (*.*)");
            std::string chosen;
            if (pickFile(rq, chosen) && !chosen.empty()) {
                // What is read is what the box then shows; the rows go where this window says.
                cascade::core::formatUtf8(field, fieldCap, "%s", chosen.c_str());
                importBookmarkFile(chosen, importDialogInto_);
            }
        }
        census::rect("import:browse", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
    }

    // 5. A read that is taking long says so, in the section's sentence.
    if (reading && bookmarkImportElapsedS() >= 1.0) {
        ImGui::TextDisabled(tr("Waiting for the disk to open the file: %.0f s"), bookmarkImportElapsedS());
    }

    // 6. CANCEL. Escape closes the window too (read at the top), and both forget the sentence.
    ImGui::Spacing();
    centre(keyW);
    if (ImGui::Button(trId("Cancel##import_cancel"), ImVec2(keyW, 0.0f))) {
        importDialogOpen_ = false;
        importDialogReason_.clear();
        ImGui::CloseCurrentPopup();
    }
    census::rect("import:cancel", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                 ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);

    ImGui::EndPopup();
}

}  // namespace cascade::gui
