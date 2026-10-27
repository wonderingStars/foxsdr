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
// It closes itself when an import comes back WITH rows (the engine's answer: importOnAnswer), and
// stays open, showing the new sentence, when one comes back with none. A file dropped while it is
// open goes to the target it remembers (AppWindow::submitPendingDrop, which the frame loop calls).
//
// THE STATE is four members of AppWindow (importDialogOpen_, importDialogQueued_,
// importDialogInto_, importDialogReason_); openImportDialog() only sets them, because the keys
// that ask are drawn inside the rail, before the root level exists, and ImGui wants OpenPopup and
// BeginPopupModal in one ID stack - the deferral drawMutePopup() uses.
//
// HOW AN IMPORT GOES, since the engine owns the bookmark list (stub campaign B8; master read the file
// on a DiskJob and added the rows in the window):
//   1. THE PATH is what was typed, pasted, dropped or chosen, cleaned (cleanImportPath, 0.99.82: the
//      quotes of "Copy as path" or a terminal's drag-and-drop, the Enter or space a paste leaves, a
//      file:// address with its %XX escapes) - BEFORE the op, here, so what the engine is sent is a path.
//   2. A PATH WITH A ROOT (a drive, a share, a leading slash) goes to the engine as it stands.
//   3. ANY OTHER - a bare name, lists/marine.csv - is looked for where an import box looks (0.99.80,
//      0.99.82): the working directory first, then the recordings folder, where Export writes. That is
//      disk work, so it runs on a worker (importFrequencyFileIn, the one rule) and the op names the
//      place found - or, when none opened, the last place tried, so the failure says where it looked.
//      (The worker reads and parses the file to find out; the engine reads it again when it imports. A
//      relative path is a typed name, and the cost is a file read, never a frame.)
//   4. THE ENGINE IMPORTS (FOXAPP_OP_BOOKMARK_IMPORT_FILE, LOCAL only) - ON A WORKER since stub campaign
//      B19 (the read was the pass's own, and a pass that waits for a disk holds every answer behind it),
//      so the op's answer is only that the read is under way, and what it found is the fact
//      bookmarks.import.* (the engine's note, whole, and whether it read rows - even rows already here).
//      That fact opens this window or closes it (importLookupPoll, once a frame). Until B19 the answer
//      itself was FAILED when it read nothing.
// An import for an Airband PRESET (ImportInto with a group) is the section's own (airbandImportFile:
// its rows take the preset's name and tick, which BOOKMARK_IMPORT_FILE cannot carry).
//
// A FILE OF ITS OWN because app_window.cpp is at the edge of what an MSVC object file holds
// (error C1128); the call sites stay there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/file_pick.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/disk_job.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;
using cascade::core::DiskJob;

// What the lookup worker hands back: the file it opened, or the last place it looked.
struct ImportLookup {
    std::string tried;
};

struct ImportJobs {
    DiskJob<ImportLookup> lookup{"Import file lookup"};
    cascade::core::ImportInto into;       // WHO ASKED, for the lookup in flight (the window's thread only)
    std::string path;                     // what it was asked to look for
    std::string againPath;                // asked for while one was out: the last one, sent when the first is back
    cascade::core::ImportInto againInto;
    bool again = false;
};

void AppWindow::openImportDialog(const cascade::core::ImportInto& into, const std::string& reason) {
    importDialogInto_ = into;
    importDialogReason_ = reason;
    importDialogOpen_ = true;
    // ALWAYS queued: drawImportDialog() opens the popup unless it is already showing, so a window
    // that is down by the time the frame draws it (closed by Escape or Cancel earlier in this very
    // frame, say) comes back for a failure that arrived since, and one that is up is not re-opened.
    importDialogQueued_ = true;
}

// A READ IS OUT while the lookup of a path with no root is on its worker here, or the engine's own read of the file is on its worker there
// (fact.bookmarks.importing).
bool AppWindow::bookmarkImportPending() {
    if (importJobs_ != nullptr && importJobs_->lookup.pending()) { return true; }
    return uiFact("fact.bookmarks.importing") == "1";
}

// How long the read now out has been out, for the line that says the window is waiting on the disk and not frozen. The lookup's own clock, or - the
// engine's read being another thread's - the time since this window first saw it pending.
double AppWindow::bookmarkImportElapsedS() {
    if (importJobs_ != nullptr && importJobs_->lookup.pending()) { return importJobs_->lookup.elapsedS(); }
    if (uiFact("fact.bookmarks.importing") != "1") {
        importOutSinceS_ = -1.0;
        return 0.0;
    }
    const double now = ImGui::GetTime();
    if (importOutSinceS_ < 0.0) { importOutSinceS_ = now; }
    return now - importOutSinceS_;
}

// THE IMPORT, FROM ANY OF ITS FOUR WAYS IN (the Import key, a Browse, a dropped file, the script's seam).
void AppWindow::importBookmarkFile(const std::string& path, const cascade::core::ImportInto& into) {
    // What was typed, pasted or dropped, as a path (cleanImportPath, 0.99.82).
    const std::string p = cascade::core::cleanImportPath(path);
    if (!into.group.empty()) {
        // A PRESET'S IMPORT: the Airband section's, which puts the preset's name and tick on every row (its worker reads the file the same way).
        airbandImportFile(p, into);
        return;
    }
    // Nothing left of what was typed (a pasted line of white space): the box is empty, and an empty box opens the window.
    if (p.empty()) {
        openImportDialog(into, std::string());
        return;
    }
    const std::filesystem::path given{std::u8string(p.begin(), p.end())};
    if (given.has_root_path()) {
        // A path with a root is opened as it stands: nothing to look for, so nothing for a worker to do.
        importSendOp(p, into);
        return;
    }
    // ANY OTHER PATH is looked for in two places (above), on a worker. ONE LOOKUP AT A TIME, and a list asked for while one is out is not
    // lost: it is remembered (the last one) and sent when the first is back.
    if (!importJobs_) { importJobs_ = std::make_shared<ImportJobs>(); }
    ImportJobs& j = *importJobs_;
    if (j.lookup.pending()) {
        j.againPath = p;
        j.againInto = into;
        j.again = true;
        return;
    }
    std::string dir;
    readFactText("fact.recorder.dir", dir);
    j.into = into;
    j.path = p;
    const auto hook = diskHookForTest_;
    j.lookup.request([p, dir, hook]() -> ImportLookup {
        if (hook) { hook(); }
        ImportLookup o;
        // The core's own rule (importFrequencyFileIn): a relative path is made absolute against the working directory and tried; when that
        // does not open it is tried in the recordings folder. `tried` is the file found, or the last place looked.
        (void)cascade::core::importFrequencyFileIn(p, dir, o.tried);
        return o;
    });
}

// THE OP: one BOOKMARK_IMPORT_FILE, its answer awaited (importOnAnswer).
void AppWindow::importSendOp(const std::string& path, const cascade::core::ImportInto& into) {
    const std::uint64_t ticket = uiSubmitTicket(cascade::core::cmd::makeText(FOXAPP_OP_BOOKMARK_IMPORT_FILE, path));
    // (A session that is full answers BUSY and nothing is sent: the press is repeated.)
    if (ticket == 0u) { return; }
    ImportAsk ask;
    ask.ticket = ticket;
    ask.into = into;
    ask.path = path;
    importAsks_.push_back(std::move(ask));
}

// Once a frame: a finished lookup is sent on to the engine, and a list asked for while it was out is looked for next.
void AppWindow::importLookupPoll() {
    importOutcomePoll();
    if (!importJobs_) { return; }
    ImportJobs& j = *importJobs_;
    ImportLookup o;
    if (!j.lookup.poll(o)) { return; }
    // A worker that threw (an allocation): nothing was found and nothing is known about where it looked.
    if (j.lookup.threw() || o.tried.empty()) {
        const std::string reason = cascade::core::formatText(tr("the file \"%s\" could not be read"), j.path.c_str());
        openImportDialog(j.into, cascade::core::formatText(tr("Could not import: %s"), reason.c_str()));
    } else {
        importSendOp(o.tried, j.into);
    }
    if (j.again) {
        j.again = false;
        std::string next;
        next.swap(j.againPath);
        const cascade::core::ImportInto nextInto = j.againInto;
        j.againInto = cascade::core::ImportInto{};
        importBookmarkFile(next, nextInto);
    }
}

// THE ENGINE'S ANSWER to an import this window sent: that the read is under way (OK), or why not (an empty path). What the read FOUND comes later, as
// the fact bookmarks.import.* (importOutcomePoll).
void AppWindow::importOnAnswer(const FoxCommandResult& r) {
    const auto it = std::find_if(importAsks_.begin(), importAsks_.end(), [&](const ImportAsk& k) { return k.ticket == r.ticket; });
    if (it == importAsks_.end()) { return; }
    const ImportAsk ask = *it;
    importAsks_.erase(it);
    if (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE) {
        // The read is out: its outcome is waited for. (The engine holds one read and the last one asked for; this list is only the targets, and
        // importOutcomePoll drops the ones a later import superseded.)
        importOut_.push_back(ask);
        return;
    }
    openImportDialog(ask.into, r.message);
}

// ONCE A FRAME: an import the engine has finished reading (fact.bookmarks.import.seq moved). Rows (even rows already in the list) close the "Import a
// frequency list" window and forget its sentence; none open it - already open, it takes the new sentence - with the engine's note whole and the target of
// the import that failed (the section's note says it too, as it always did).
void AppWindow::importOutcomePoll() {
    const std::string& seqText = uiFact("fact.bookmarks.import.seq");
    if (seqText.empty()) { return; }
    const std::uint64_t seq = std::strtoull(seqText.c_str(), nullptr, 10);
    if (seq == importSeqSeen_) { return; }
    importSeqSeen_ = seq;
    const bool ok = uiFact("fact.bookmarks.import.ok") == "1";
    const std::string note = uiFact("fact.bookmarks.import.note");
    const std::string path = uiFact("fact.bookmarks.import.path");
    // WHO ASKED: the oldest ask for this path (the engine runs them in the order they were asked, the last-asked-wins ones dropped), and every older
    // ask is one the engine never read - a list dropped while another was being read, replaced by a later one.
    cascade::core::ImportInto into;
    const auto it = std::find_if(importOut_.begin(), importOut_.end(), [&](const ImportAsk& k) { return k.path == path; });
    if (it != importOut_.end()) {
        into = it->into;
        importOut_.erase(importOut_.begin(), it + 1);
    } else if (!importOut_.empty()) {
        into = importOut_.back().into;
        importOut_.clear();
    }
    if (ok) {
        importDialogOpen_ = false;
        importDialogQueued_ = false;
        importDialogReason_.clear();
        return;
    }
    openImportDialog(into, note);
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
            char* const field = forPreset ? airband_.importPath : bookmarkImportPath_;
            const std::size_t fieldCap = forPreset ? sizeof(airband_.importPath) : sizeof(bookmarkImportPath_);
            cascade::core::FilePickRequest rq;
            rq.title = tr("Choose a frequency list");
            std::string recordDir;
            readFactText("fact.recorder.dir", recordDir);
            rq.startFrom = cascade::core::pickStartPath(field, recordDir, "frequencies.xml");
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
