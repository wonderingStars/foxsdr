// app_window_fitted.cpp - the FITTED MODULES WINDOW and what it keeps of the engine's table (stub campaign B5, STORE-B2; master 0.99.63 - 0.99.77), and
// the three small members that name a plugin's windows from what the module DECLARES (declaredTrackPluginNames, openPluginOwnWindow).
//
// THE PANEL ITSELF - the operating panel's rows, the module's page, the IDLE / KEEP RUNNING keys, the chips, the groups under the store's headings, the
// command plate - is gui/plugins_view.cpp, which is master's own and was carried by the 0.99.84 merge; the branch's wiring of it was the 0.99.42 one (a
// model of what a LoadedPlugin says and the engine's records, nine fields from the install record and the catalogue missing). This file is the 0.99.72
// wiring over the table: the model is built once when listSeq moves (fittedLists) from PLUGINS, the per-file facts of API 0.3 8.8 and 9.4 / 9.5 and
// PLUGIN_COMMANDS, and the window draws it (drawFittedModulesWindow); a key changes things only by a command on the window's session.
//
// THE FILE SIZE IS THE TABLE'S (PLUGINS value[1], measured by the engine when the block is rebuilt): nothing in this file - and nothing in the draw function
// of the window - asks the file system anything. A stat a module a frame, on the thread that draws the window, in a folder that may be a network or a
// synchronised profile or a scanner's, once froze whole frames (0.99.63); tests/test_fitted_modules_no_disk.cpp is the source scan that keeps it out.
//
// A MODULE THAT IS ASLEEP IS LISTED ALL THE SAME (0.99.73): the window never asks an instance anything. Whether the module runs and whether it is
// asleep is PLUGINS value[3]; what windows it declares is its capability word and fact.plugin.<file>.windows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "core/utf8_text.hpp"
#include "gui/decoder_reads.hpp"
#include "gui/page_geometry.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/plugin_window_ids.hpp"
#include "gui/plugins_view.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

// THE TRACK SOURCES THE TABLE DECLARES (master 0.99.73), by display name, in the host's order.
std::vector<std::string> AppWindow::declaredTrackPluginNames() {
    std::vector<std::string> out;
    for (const PluginRow& r : pluginLists().plugins) {
        if (!r.loaded || (r.caps & CASCADE_CAP_TRACK_SOURCE) == 0u || r.stopped) { continue; }
        out.push_back(r.name);
    }
    return out;
}

// THE ONE WINDOW A DOUBLE-CLICK ON A FITTED MODULES ROW OPENS (0.99.77), the owner (2026-10-13): "when I double click on the plugin it should show the
// plugin window". The same ids the draw loops and the rail rows build from the module's declared names, so it is the window the rail's own row would
// toggle; only ONE of a module's windows, in the order fittedOwnWindowOfCaps says, where a preset press opens them all. Marking it shown is the whole of
// the work: reportPluginUse sends it with the next poll, and the engine's lifecycle starts a plugin that was asleep.
bool AppWindow::openPluginOwnWindow(const std::string& pluginKey) {
    for (const PluginRow& r : pluginLists().plugins) {
        if (!r.loaded || r.file != pluginKey) { continue; }
        // A STOPPED plugin has no instances, and so no window could draw: the rail lists no rows for one, and a stopped tracker has no map page.
        // Nothing is marked shown for it.
        if (r.stopped) { return false; }
        switch (fittedOwnWindowOfCaps(r.caps, r.loaded)) {
            case FittedOwnWindow::Panel:
                pluginWindows_.show(panelWindowId(r.name, r.panelTitle.empty() ? r.name : r.panelTitle));
                return true;
            case FittedOwnWindow::Picture:
                pluginWindows_.show(imageWindowId(r.name));
                return true;
            case FittedOwnWindow::Instrument:
                pluginWindows_.show(instrumentWindowIdFor(r.name, r.instrumentTitle.empty() ? r.name : r.instrumentTitle));
                return true;
            case FittedOwnWindow::Map:
                ensureMapPage(r.name).open = true;
                return true;
            case FittedOwnWindow::None:
                return false;
        }
        return false;
    }
    return false;
}

// THE FITTED MODULES WINDOW'S MODEL (window port, group 9; API 0.3 section 8.8 and 9.4 / 9.5; the cache is in app_window.hpp and gui/plugins_view.hpp).
// Read when listSeq has moved and only from the window's own draw: PLUGINS first - one record a plugin file the host holds, loaded or not, in the
// host's order, with how it runs and whether it is asleep - and for each the eight whole texts of fact.plugin.<file>.* (path, version, author, licence,
// error, idle, notice, integrity), then the three of stub campaign B5 (the install record, and where the file came from: orphaned, from a site, from a
// site since removed), then PLUGIN_COMMANDS filed under the module they name, then fact.plugin.dir and the store's old copies and clean-up report.
// The rows go through the same builder a test holds equal to the one that reads a host's own records (makeFittedModuleFromRow). Strings and vectors
// keep their room, so a refresh that finds nothing bigger allocates only what the table's get_setting does (about eleven of those a module). `got <= i`
// is the end of a list that changed under the read: what was read stands.
cascade::gui::FittedListCache& AppWindow::fittedLists() {
    cascade::gui::FittedListCache& c = *fittedCache_;
    // WHAT IS NOT THE LISTS', set every call (a frame): the receiver's run state - it gates FED for every
    // module at once, which is why the panel states it rather than leaving it to be inferred from four idle
    // rows - and the install sentence, which depends on whether the store window drew this frame. ONLY WHEN
    // THE STORE DID NOT PRINT IT: the install report and error are written by an install AND by a remove, so
    // both windows can hold the same sentence; printing it in both reads as two separate outcomes, and in
    // neither loses a failed remove entirely. They are the status lines the engine handed over after this
    // frame's commands (statusLines_), so a remove's sentence and the row it removed arrive in the same frame.
    // (A string assigned something no longer than its room allocates nothing.)
    c.model.receiverRunning = (uiState_.flags & FOXAPI_RX_RUNNING) != 0u;
    if (!pluginBrowserDrawnThisFrame_) {
        c.model.report = statusLines_.installReport;
        c.model.error = statusLines_.installError;
    } else {
        c.model.report.clear();
        c.model.error.clear();
    }
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    ++c.refreshes;
    const auto entry = [](FoxListItem& it) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
    };
    FoxListItem it{};
    cascade::gui::FittedRow& r = c.row;
    const auto fact = [&](const char* leaf, std::string& out) {
        c.key.assign("fact.plugin.");
        c.key += r.file;
        c.key += '.';
        c.key += leaf;
        readFactText(c.key.c_str(), out);
    };

    const int32_t plugins = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGINS, 0, nullptr));
    c.model.modules.resize(static_cast<std::size_t>(plugins));
    std::size_t rows = 0;
    cascade::gui::PluginRecordFact record;
    for (int32_t i = 0; i < plugins; ++i) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i), &it) <= i) { break; }
        r.file = it.detail;
        r.name = it.name;
        r.caps = static_cast<std::uint32_t>(it.value[0]);
        r.sizeBytes = static_cast<std::uint64_t>(it.value[1] > 0 ? it.value[1] : 0);
        r.noticeLevel = static_cast<std::uint32_t>(it.value[2] > 0 ? it.value[2] : 0);
        r.runMode = static_cast<int>(it.value[3]) & 3;
        r.asleep = (static_cast<int>(it.value[3]) & 4) != 0;
        r.loaded = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        r.stopped = (it.flags & FOXAPI_ITEM_STOPPED) != 0u;
        r.tuneAllowed = (it.flags & FOXAPI_ITEM_TUNE_ALLOWED) != 0u;
        r.settingsAllowed = (it.flags & FOXAPI_ITEM_SETTINGS_ALLOWED) != 0u;
        r.matched = (it.flags & FOXAPI_ITEM_MATCHED) != 0u;
        r.askedSettings = (it.flags & FOXAPI_ITEM_ASKED_SETTINGS) != 0u;
        fact("path", r.path);
        fact("version", r.version);
        fact("author", r.author);
        fact("licence", r.licence);
        fact("error", r.error);
        fact("idle", r.idle);
        fact("notice", r.notice);
        fact("integrity", r.integrity);
        cascade::gui::FittedModule& m = c.model.modules[rows++];
        cascade::gui::fillFittedModule(m, r);

        // THE INSTALL RECORD, by file: the catalogue key it gave the module (what the catalogue's glyph, pictures and description are found by), the
        // plugin ABI the fitted build was made for, when it was fitted, and the catalogue's category. A module with no record is a hand-fitted
        // one and keeps its file name for an id.
        fact("record", c.text);
        cascade::gui::parsePluginRecord(c.text, record);
        if (record.have) {
            m.abiVersion = record.abiVersion;
            m.fittedAtUnix = record.installedAtUnix;
            m.category = record.category;
            if (!record.key.empty()) {
                // The catalogue KEY (0.99.74): the id for an official plugin, "<id>@<site>" for one installed from a site - what the store's model
                // names its modules by.
                m.catalogueId = record.key;
                m.id = record.key;
            }
        }
        // WHERE IT CAME FROM (stub campaign B3's verdict words): ORPHANED (0.99.69) - the plugin index does not know the file and nothing runs it -
        // or installed from a plugin site (0.99.74), and a site the user has since removed is said so, and that it gets no updates: nothing lists
        // it any more. The address is the install record's source.
        c.key.assign("fact.plugin.origin.");
        c.key += r.file;
        readFactText(c.key.c_str(), c.text);
        if (!c.text.empty()) {
            const std::size_t tab = c.text.find('\t');
            const std::string word = c.text.substr(0, tab);
            if (word == "orphan") {
                m.orphaned = true;
            } else if (word == "site" || word == "removed-site") {
                const std::string label = cascade::core::PluginRepo::siteLabel(tab == std::string::npos ? std::string() : c.text.substr(tab + 1));
                m.originNote = cascade::core::formatText(word == "site" ? tr("Installed from %s, not from the FoxSDR catalogue.")
                                                                         : tr("Installed from %s, a site you removed. It gets no updates."),
                                                         label.c_str());
            }
        }
    }
    c.model.modules.resize(rows);

    const int32_t keys = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_COMMANDS, 0, nullptr));
    for (int32_t i = 0; i < keys; ++i) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_COMMANDS, static_cast<uint32_t>(i), &it) <= i) { break; }
        for (cascade::gui::FittedModule& m : c.model.modules) {
            if (m.file != it.detail) { continue; }
            m.commands.push_back(cascade::gui::FittedModule::Command{static_cast<std::uint32_t>(it.id), it.name});
            break;
        }
    }
    readFactText("fact.plugin.dir", c.model.directory);
    // THE OLD COPIES UPDATES LEFT BEHIND, and what the last clean-up did (the store's facts: "CLEAN UP OLD VERSIONS (n)" is at the foot of the list when
    // there are any).
    readFactText("fact.store.oldCopies", c.text);
    cascade::gui::parseStoreOldCopies(c.text, c.model.oldCopies);
    readFactText("fact.store.cleanupReport", c.model.cleanupReport);
    return c;
}

void AppWindow::drawFittedModulesWindow() {
    // VERIFICATION ONLY, the plugin store's own seam above for its sibling: nothing in a
    // config opens this window since 0.79.1, and a translated module list can only be
    // judged by looking at it. Once, at the first frame, so a session that closes it again
    // is not fought.
    static const bool openForCapture = std::getenv("FOXSDR_OPEN_FITTED_MODULES") != nullptr;
    static bool openedForCapture = false;
    if (openForCapture && !openedForCapture) {
        openedForCapture = true;
        fittedWindowOpen_ = true;
    }
    cascade::gui::FittedModulesDeck& deck = *fittedDeck_;
    if (!fittedWindowOpen_) {
        // The pictures of a page are kept for the window's life, and no longer; the catalogue's record of the page's module goes with it.
        deck.pictures.releaseAll();
        deck.pageFile.clear();
        cascade::gui::FittedListCache& shut = *fittedCache_;
        if (!shut.cataloguePage.empty() || !shut.model.catalogue.empty()) {
            shut.model.catalogue.clear();
            shut.cataloguePage.clear();
            shut.catalogueSeq = 0;
            shut.catalogueListSeq = 0;
            for (cascade::gui::FittedModule& m : shut.model.modules) { m.catalogueIndex = -1; }
        }
        return;
    }
    uiNotePanel("fitted modules");

    ImGuiWindowClass fittedClass;
    fittedClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&fittedClass);
    // WHERE THE USER LEFT IT, and only if that place still exists on this machine; otherwise
    // a different slot from the store's, so opening both for the first time does not stack
    // one exactly on the other.
    placeSavedFeatureWindow(7, fittedWinX_, fittedWinY_, fittedWinW_, fittedWinH_, 1060.0f, 720.0f);
    bool open = true;
    const bool drawn = beginPage("Fitted modules###fittedmoduleswindow", tr("FITTED MODULES"), &open, 0, 1060.0f, 720.0f);
    if (!open) { fittedWindowOpen_ = false; }
    // READ BACK EVERY FRAME, which is the whole of the persistence: ImGui's own .ini is
    // switched off in this application, so unless the rectangle is copied out here and into
    // AppConfig it exists only until the process ends. The POSITION is read even when Begin
    // answered false - a collapsed window is not gone and can still be dragged - but the
    // SIZE is not, because a collapsed window reports its title-bar-only height and
    // persisting that would corrupt the real one. ...and NOT while the window is maximised
    // or rolled up by its own keys: that rectangle is the key's.
    if (!pageGeometryTransient("Fitted modules###fittedmoduleswindow")) {
        const ImVec2 wpos = ImGui::GetWindowPos();
        fittedWinX_ = static_cast<int>(wpos.x);
        fittedWinY_ = static_cast<int>(wpos.y);
        if (drawn) {
            const ImVec2 wsize = ImGui::GetWindowSize();
            fittedWinW_ = static_cast<int>(wsize.x);
            fittedWinH_ = static_cast<int>(wsize.y);
        }
    }
    if (drawn) {
        // --- what the window is told ----------------------------------------
        // FROM THE TABLE ONLY (window port, group 9; API 0.3 sections 8.8, 9.4 and 9.5): the model is the cache's, built when listSeq moves, so a
        // frame in which nothing changed builds nothing (fittedLists also sets the receiver's run state and the install sentence, which are not the
        // lists'). NO FILE IS ASKED ANYTHING HERE: the size of each file is PLUGINS' value[1], which the engine measured when it made the block.
        cascade::gui::FittedListCache& cache = fittedLists();
        cascade::gui::FittedModulesModel& model = cache.model;
        // THE PAGE THAT IS OPEN needs the catalogue's record of its module (pictures, description, what is new): the store's own model, which the
        // table's cache keeps for the store window and which is copied here once for each move of it - never for a frame in which nothing moved, and
        // never with no page open. Every module is joined to it by the catalogue key its install record gave it.
        if (deck.pageFile.empty()) {
            if (!cache.cataloguePage.empty() || !model.catalogue.empty()) {
                model.catalogue.clear();
                cache.cataloguePage.clear();
                cache.catalogueSeq = 0;
                cache.catalogueListSeq = 0;
                for (cascade::gui::FittedModule& m : model.modules) { m.catalogueIndex = -1; }
            }
        } else {
            const cascade::gui::StoreListCache& store = storeLists(true);
            if (cache.cataloguePage != deck.pageFile || cache.catalogueSeq != store.fullSeq || cache.catalogueListSeq != cache.listSeq) {
                model.catalogue = store.model.modules;
                for (cascade::gui::FittedModule& m : model.modules) {
                    m.catalogueIndex = -1;
                    if (m.catalogueId.empty()) { continue; }
                    for (std::size_t ci = 0; ci < model.catalogue.size(); ++ci) {
                        if (model.catalogue[ci].id == m.catalogueId) {
                            m.catalogueIndex = static_cast<int>(ci);
                            break;
                        }
                    }
                }
                cache.cataloguePage = deck.pageFile;
                cache.catalogueSeq = store.fullSeq;
                cache.catalogueListSeq = cache.listSeq;
            }
        }

        // --- the cabinet ----------------------------------------------------
        // NO TITLE PLATE HERE: the window already carries the word.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        cascade::gui::FittedModulesAction act;
        if (dl != nullptr && avail.x > 8.0f && avail.y > 8.0f) {
            // THE CABINET IS THE PAGE'S OWN NOW - beginPage drew it, screws, rail and keys - so
            // the face goes straight into the well.
            const float pad = 8.0f;
            const float faceW = avail.x - pad * 2.0f;
            const float faceH = avail.y - pad * 2.0f;
            if (faceW > 40.0f && faceH > 40.0f) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::SetCursorScreenPos(ImVec2(tl.x + pad, tl.y + pad));
                ImGui::BeginChild("##fittedface", ImVec2(faceW, faceH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                act = cascade::gui::drawFittedModulesPanel(deck, model);
                ImGui::EndChild();
            } else {
                ImGui::SetCursorScreenPos(ImVec2(tl.x + pad, tl.y + pad));
                ImGui::TextDisabled("%s", tr("Too small - drag the window larger."));
            }
        }

        // --- what the frame's keys asked for --------------------------------
        // APPLIED AFTER THE PANEL, never inside it, and as COMMANDS on the window's session (uiSubmit): every one of these rebuilds the plugin set -
        // rescan unloads and re-loads every module, a stop or a start puts that plugin's instances down or up, a remove deletes a file and rescans -
        // and the engine does it in its next pass, so the model the panel was drawn from is the model it replaces only at the next refresh of the
        // cache, never mid-frame.
        namespace cmd = cascade::core::cmd;
        switch (act.kind) {
            case cascade::gui::FittedModulesAction::Kind::Rescan:
                uiSubmit(cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
                break;
            case cascade::gui::FittedModulesAction::Kind::Start:
                // START is the user's own press: the plugin is set to ALWAYS (the engine does it: B2) and so runs whether or not anything uses it.
                uiSubmit(cmd::makeText(FOXAPI_OP_DECODER_START, act.file));
                break;
            case cascade::gui::FittedModulesAction::Kind::Stop:
                uiSubmit(cmd::makeText(FOXAPI_OP_DECODER_STOP, act.file));
                break;
            case cascade::gui::FittedModulesAction::Kind::SetKeepRunning:
                // The "keep running" tick: ALWAYS, or back to AUTO (running until the clock takes it down, if nothing is using it).
                uiSubmit(cmd::makeText(FOXAPP_OP_PLUGIN_RUN_MODE, act.file, act.flag ? 1 : 0));
                break;
            case cascade::gui::FittedModulesAction::Kind::Remove:
                uiSubmit(cmd::makeText(FOXAPI_OP_STORE_REMOVE, act.file));
                break;
            case cascade::gui::FittedModulesAction::Kind::RemoveOrphan:
                // REMOVE FILE on an orphan: the engine judges it again against its own scan before it deletes anything (a row gone stale removes
                // nothing, and the install sentence says so).
                uiSubmit(cmd::makeText(FOXAPP_OP_STORE_CLEAN_OLD, act.file, 1));
                break;
            case cascade::gui::FittedModulesAction::Kind::SetTune:
                uiSubmit(cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, act.file, 1, act.flag ? 1 : 0));
                break;
            case cascade::gui::FittedModulesAction::Kind::SetSettings:
                uiSubmit(cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, act.file, 2, act.flag ? 1 : 0));
                break;
            case cascade::gui::FittedModulesAction::Kind::SetTransmit:
                // Not on the table yet (stub campaign B13): the model never offers the key, so there is nothing to send.
                break;
            case cascade::gui::FittedModulesAction::Kind::Command:
                // Queued for the plugin to take with poll_command; nothing of the plugin's runs here, on the GUI thread's frame.
                uiSubmit(cmd::makeText(FOXAPI_OP_PLUGIN_COMMAND, act.file, static_cast<std::int64_t>(act.commandId)));
                break;
            case cascade::gui::FittedModulesAction::Kind::ResetWindows:
                // Safe from inside this page's own body: beginPage has already returned, and endPage reads none of the state this clears.
                resetPageWindows();
                break;
            case cascade::gui::FittedModulesAction::Kind::CleanUp:
                // CLEAN UP OLD VERSIONS, confirmed once for all of them (the panel's own confirmation). What goes is what the engine's last scan
                // calls superseded NOW, not the frame's list.
                uiSubmit(cmd::makeInt(FOXAPP_OP_STORE_CLEAN_OLD, 0));
                break;
            case cascade::gui::FittedModulesAction::Kind::OpenWindow:
                // A double-click on a row (0.99.77). The panel only asks for a module that has a window of its own and is not stopped; were the
                // module gone since, there is nothing to open and the list is simply what it was.
                (void)openPluginOwnWindow(act.file);
                break;
            case cascade::gui::FittedModulesAction::Kind::None:
                break;
        }
        // THE PICTURES OF THE PAGE THAT IS OPEN, asked for as the store asks (on the edge of opening it - a failed one is tried again then - and later
        // only when the table lists one nobody has asked for, at most once for a move of listSeq: never once a frame, a command a frame fills the
        // session's queue), by the catalogue key its install record gave the module.
        if (!deck.pageOpenedCatalogueId.empty()) {
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, deck.pageOpenedCatalogueId, 1));
            fittedPicturesWantSeq_ = uiState_.listSeq;
            deck.pageOpenedCatalogueId.clear();
        } else if (!deck.pageFile.empty() && fittedPicturesWantSeq_ != uiState_.listSeq) {
            for (const cascade::gui::FittedModule& fm : model.modules) {
                if (fm.file != deck.pageFile || fm.catalogueId.empty() || fm.catalogueIndex < 0 ||
                    fm.catalogueIndex >= static_cast<int>(model.catalogue.size())) {
                    continue;
                }
                for (const cascade::gui::StorePicture& pic : model.catalogue[static_cast<std::size_t>(fm.catalogueIndex)].pictures) {
                    if (pic.state != cascade::gui::PictureState::None) { continue; }
                    uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, fm.catalogueId, 0));
                    fittedPicturesWantSeq_ = uiState_.listSeq;
                    break;
                }
                break;
            }
        }
    }
    endPage();
}

}  // namespace cascade::gui
