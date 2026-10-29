// app_window_store.cpp - the PLUGIN STORE WINDOW and what it keeps of the engine's table (stub campaign B4, STORE-B1; master 0.99.72 - 0.99.77).
//
// Master drew the store from the window's own catalogue (`catalog_`, the sites, the pictures, the install records); the engine owns all of that now
// (stub campaign B3: src/engine/engine_store.cpp) and the window reads it from the table: CATALOGUE and twenty-odd facts an entry, STORE_UPDATES,
// STORE_ACTIVITY, STORE_BLOCKED, STORE_PICTURES, PLUGINS and the store's own facts (docs/api-0.3-additions.md 8.9 and 9.4). It changes things
// only by commands on its session. THE PAGE ITSELF - the tabs, the cards, the plate, the SITES panel, the pictures - is gui/plugin_store_view.cpp,
// which is master's own; this file is the wiring: the model, built once when listSeq moves (storeLists), and the window around the view and what
// the view asked for (drawPluginStoreWindow).
//
// THE SETTLED FRAME ALLOCATES NOTHING AND READS NOTHING. The model is rebuilt from the table once for each move of listSeq and kept; a frame
// that follows an unchanged state makes no read of the table at all except the live transfer figure (and only while one is moving), and what it
// sets on the kept model is assigned into strings that already hold the room (tests/test_plugin_store_window.cpp C holds the property).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/plugin_repo_data.hpp"
#include "gui/page_geometry.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

// THE PLUGIN STORE'S TABLE (window port, group 10; API 0.3 section 8.9 and 9.4; the cache is gui/plugin_store_view.hpp's StoreListCache). Two
// tiers, both read when listSeq moves, and a third thing read live.
//
// THE RAIL TIER (every call): CATALOGUE's count (is there a catalogue), fact.store.readThisSession (the rail's IDLE and the one automatic read),
// STORE_UPDATES (the plans' ids and their count), STORE_ACTIVITY (entry 1, a fetch or a download in flight; entry 2, the ADD ALL run),
// STORE_BLOCKED with the two facts of each retired row, and PLUGINS' records (the host's, joined to the catalogue by file name below, and the
// count it did not load). The rail's chip, the retired rows and the plugins key draw from it whether the window is open or not.
//
// THE CONSENTS ARE CLEARED HERE, on the window's own thread. A tick the user gave is consent for a notice that was listed: the engine used to reach
// into the deck from ITS thread at the two moments it must go, and the window now WATCHES the table for them: STORE_ACTIVITY's first entry with
// value[1] 1 (a catalogue fetch began) clears all of it; entry 2 present and then gone (the run ended) clears the ADD ALL tick, as does a new
// addAllSummary line; and the full tier, which can see the catalogue itself, clears all of it when the catalogue it reads is not the one it read
// last (a fetch of a local index.json can start and land between two reads, so no transfer entry is ever seen - the rows are the evidence then).
//
// THE FULL TIER (`full`: only from the window's draw, only while it is drawn): one StoreRow a CATALOGUE entry - its columns, the facts of its card
// (fact.store.<key>.*), the PLUGINS record whose detail is the entry's installed file (ASCII case ignored: the engine's own rule), with that
// module's `fact.plugin.<file>.error`, and the pictures STORE_PICTURES says it has got to - through fillStoreModule; then the model's own: the
// sites, the old copies and the clean-up's report, the kept copy's freshness, the busy and result keys. Strings and vectors keep their room, so a
// refresh that finds nothing bigger allocates only what the table's get_setting does. `got <= i` is the end of a list that changed under the read:
// what was read stands.
//
// EVERY CALL, whatever moved: the ADD ALL summary's edge, and for a full call the three things that are not the lists' - the status lines the
// engine handed over (statusLines_: the catalogue's line and error, the install report and error, the ADD ALL summary and its level), the rows'
// blockedReason (it depends on the deck's open page and tick, which move between refreshes) and the transfer's PROGRESS, which moves with no list
// moving and is read live (STORE_ACTIVITY's first entry, only while one is present).
cascade::gui::StoreListCache& AppWindow::storeLists(bool full) {
    cascade::gui::StoreListCache& c = *storeCache_;
    cascade::gui::PluginStoreDeck& deck = *pluginStoreDeck_;
    if (uiSession_ == nullptr) { return c; }
    // THE RUN'S RESULT LINE ARRIVING is the run having ended, however briefly the run itself was on the table.
    if (statusLines_.addAllSummary != c.lastAddAllSummary) {
        c.lastAddAllSummary = statusLines_.addAllSummary;
        if (!c.lastAddAllSummary.empty()) { deck.addAllAck = false; }
    }
    const auto entry = [](FoxListItem& it) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
    };
    const auto size = [this](uint32_t list) {
        return std::max<int32_t>(0, engineApi_->read_list(uiSession_, list, 0, nullptr));
    };
    FoxListItem it{};
    const auto isPlanned = [&c](const std::string& id) {
        if (id.empty()) { return false; }
        for (const std::string& p : c.planIds) {
            if (p == id) { return true; }
        }
        return false;
    };

    if (c.railSeq != uiState_.listSeq) {
        c.railSeq = uiState_.listSeq;
        ++c.railRefreshes;
        c.haveCatalogue = size(FOXAPI_LIST_CATALOGUE) > 0;
        c.model.haveCatalogue = c.haveCatalogue;
        // A LIST READ THIS SESSION, not a list: the kept copy of an earlier session fills CATALOGUE at start-up and is no answer to "have we looked".
        readFactText("fact.store.readThisSession", c.key);
        c.readThisSession = c.key == "1";

        const int32_t plans = size(FOXAPI_LIST_STORE_UPDATES);
        c.updates = static_cast<std::size_t>(plans);
        c.planIds.resize(c.updates);
        std::size_t planned = 0;
        for (int32_t i = 0; i < plans; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_STORE_UPDATES, static_cast<uint32_t>(i), &it) <= i) { break; }
            c.planIds[planned++] = it.name;
        }
        c.planIds.resize(planned);

        // What the store is doing: at most two entries, told apart by id.
        c.transfer = false;
        c.transferKind = 0;
        c.transferName.clear();
        c.addAll = false;
        c.addAllPosition = 0;
        c.addAllTotal = 0;
        c.addAllName.clear();
        const int32_t doing = size(FOXAPI_LIST_STORE_ACTIVITY);
        for (int32_t i = 0; i < doing; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_STORE_ACTIVITY, static_cast<uint32_t>(i), &it) <= i) { break; }
            if (it.id == 1u) {
                c.transfer = true;
                c.transferKind = static_cast<int>(it.value[1]);
                c.transferName = it.name;
            } else if (it.id == 2u) {
                c.addAll = true;
                c.addAllPosition = static_cast<std::uint64_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
                c.addAllTotal = static_cast<std::uint64_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
                c.addAllName = it.name;
            }
        }
        // A catalogue fetch began: whatever was ticked was for the catalogue that is going. A run ended: the tick
        // was for that run, which took the notices it covered.
        if (c.transfer && c.transferKind == 1) { cascade::gui::forgetCatalogueConsent(deck); }
        if (c.addAllWasRunning && !c.addAll) { deck.addAllAck = false; }
        c.addAllWasRunning = c.addAll;
        // The model's lamp, label and ADD ALL line (their words are the window's own, in the language in force: a
        // switch moves listSeq, so they are made again here).
        c.model.busy = c.transfer;
        c.model.busyLabel = c.transfer ? cascade::gui::storeBusyLabel(c.transferKind, c.transferName) : std::string();
        c.model.addAllRunning = c.addAll;
        c.model.addAllProgress =
            c.addAll ? cascade::gui::storeAddAllLine(c.addAllPosition, c.addAllTotal, c.addAllName) : std::string();

        // The host's records (PLUGINS), and how many it did not load.
        const int32_t plugins = size(FOXAPI_LIST_PLUGINS);
        c.hosts.resize(static_cast<std::size_t>(plugins));
        std::size_t held = 0;
        c.refused = 0;
        c.idleDecoders = 0;
        for (int32_t i = 0; i < plugins; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i), &it) <= i) { break; }
            cascade::gui::StoreListCache::Host& h = c.hosts[held++];
            h.file = it.detail;
            h.caps = static_cast<std::uint32_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
            h.loaded = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
            h.stopped = (it.flags & FOXAPI_ITEM_STOPPED) != 0u;
            h.tune = (it.flags & FOXAPI_ITEM_TUNE_ALLOWED) != 0u;
            h.asleep = (static_cast<int>(it.value[3]) & 4) != 0;
            if (!h.loaded) { ++c.refused; }
            if (h.loaded && h.asleep && (h.caps & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER)) != 0u) { ++c.idleDecoders; }
        }
        c.hosts.resize(held);

        // The retired modules, with the two facts of each (keyed by the FILE: a module may be retired with no
        // catalogue fetched at all).
        const int32_t retired = size(FOXAPI_LIST_STORE_BLOCKED);
        c.blocked.resize(static_cast<std::size_t>(retired));
        std::size_t rows = 0;
        for (int32_t i = 0; i < retired; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_STORE_BLOCKED, static_cast<uint32_t>(i), &it) <= i) { break; }
            cascade::gui::StoreBlockedRow& b = c.blocked[rows++];
            b.file = it.name;
            c.key.assign("fact.blocked.");
            c.key += b.file;
            c.key += ".id";
            readFactText(c.key.c_str(), b.id);
            c.key.assign("fact.blocked.");
            c.key += b.file;
            c.key += ".message";
            readFactText(c.key.c_str(), b.message);
            b.hasPlan = isPlanned(b.id);
        }
        c.blocked.resize(rows);
    }

    if (!full) { return c; }

    cascade::gui::PluginStoreModel& m = c.model;
    if (c.fullSeq != uiState_.listSeq) {
        c.fullSeq = uiState_.listSeq;
        ++c.refreshes;
        const auto fact = [&](const char* key, std::string& out) { readFactText(key, out); };
        fact("fact.store.url", m.sourceUrl);
        // WHAT THIS MACHINE IS, as the catalogue spells it ("windows/x64"): what the page's header line says the size and build are OF.
        m.hostPlatform.assign(cascade::core::pluginHostOs());
        m.hostPlatform += '/';
        m.hostPlatform += cascade::core::pluginHostArch();
        m.maxSites = cascade::core::AppConfig::kMaxPluginSites;

        // THE KEPT COPY AND HOW FRESH IT IS, and the transfer's keys. The fitting key is drawn on the catalogue key of a DOWNLOAD only (a
        // catalogue read has no plugin), and the page of the plugin an outcome is about is the only one that shows it.
        fact("fact.store.fromCache", c.scratch);
        m.catalogueFromCache = c.scratch == "1";
        fact("fact.store.refreshFailed", c.scratch);
        m.refreshFailed = c.scratch == "1";
        fact("fact.store.cacheTime", c.scratch);
        m.catalogueReadTime = std::strtoll(c.scratch.c_str(), nullptr, 10);
        fact("fact.store.busyKey", m.busyId);
        if (!(c.transfer && c.transferKind == 2)) { m.busyId.clear(); }
        fact("fact.store.resultKey", m.resultId);

        // THE PLUGIN SITES the user added, the old copies updates left, and what the last clean-up did.
        fact("fact.store.sites", c.scratch);
        cascade::gui::parseStoreSites(c.scratch, m.sites);
        fact("fact.store.oldCopies", c.scratch);
        cascade::gui::parseStoreOldCopies(c.scratch, m.oldCopies);
        fact("fact.store.cleanupReport", m.cleanupReport);

        // THE PICTURES, every one the engine was asked for and still knows (STORE_PICTURES), by digest: a card's pictures take their state from
        // here. A Ready one's path and a Failed one's reason are the item's `detail`, or the whole fact when the item had to cut it.
        const int32_t pictures = size(FOXAPI_LIST_STORE_PICTURES);
        c.picturesNow.resize(static_cast<std::size_t>(pictures));
        std::size_t known = 0;
        c.pictureBusy = false;
        for (int32_t i = 0; i < pictures; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_STORE_PICTURES, static_cast<uint32_t>(i), &it) <= i) { break; }
            cascade::gui::StoreListCache::PictureNow& pn = c.picturesNow[known++];
            pn.sha256 = it.name;
            const int state = static_cast<int>(it.value[0]);
            pn.state = state == 1 ? cascade::gui::PictureState::Pending
                       : state == 2 ? cascade::gui::PictureState::Ready
                       : state == 3 ? cascade::gui::PictureState::Failed
                                    : cascade::gui::PictureState::None;
            pn.path.clear();
            pn.reason.clear();
            if (pn.state == cascade::gui::PictureState::Pending) { c.pictureBusy = true; }
            const bool cut = std::strlen(it.detail) >= sizeof(it.detail) - 1u;
            if (pn.state == cascade::gui::PictureState::Ready || pn.state == cascade::gui::PictureState::Failed) {
                std::string& into = pn.state == cascade::gui::PictureState::Ready ? pn.path : pn.reason;
                if (cut) {
                    c.key.assign("fact.store.picture.");
                    c.key += pn.sha256;
                    c.key += pn.state == cascade::gui::PictureState::Ready ? ".path" : ".reason";
                    readFactText(c.key.c_str(), into);
                } else {
                    into = it.detail;
                }
            }
        }
        c.picturesNow.resize(known);

        cascade::gui::StoreRow& r = c.row;
        const auto rowFact = [&](const char* leaf, std::string& out) {
            c.key.assign("fact.store.");
            c.key += r.id;
            c.key += '.';
            c.key += leaf;
            readFactText(c.key.c_str(), out);
        };
        const int32_t entries = size(FOXAPI_LIST_CATALOGUE);
        m.modules.resize(static_cast<std::size_t>(entries));
        std::size_t rows = 0;
        std::uint64_t hash = 1469598103934665603ull;
        for (int32_t i = 0; i < entries; ++i) {
            entry(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_CATALOGUE, static_cast<uint32_t>(i), &it) <= i) { break; }
            r.id = it.detail;
            r.name = it.name;
            r.abi = static_cast<std::uint32_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
            r.sizeBytes = static_cast<std::uint64_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
            r.installed = (it.flags & FOXAPI_ITEM_INSTALLED) != 0u;
            r.compatible = (it.flags & FOXAPI_ITEM_COMPATIBLE) != 0u;
            r.buildHere = (it.flags & FOXAPI_ITEM_BUILD_HERE) != 0u;
            rowFact("version", r.version);
            rowFact("author", r.author);
            rowFact("licence", r.licence);
            rowFact("description", r.description);
            rowFact("summary", r.summary);
            rowFact("homepage", r.homepage);
            rowFact("legal", r.legal);
            rowFact("floor", r.floor);
            rowFact("platforms", r.platforms);
            rowFact("file", r.file);
            rowFact("blocked", r.blocked);
            rowFact("updateReason", r.updateReason);
            // The card's own (stub campaign B4).
            rowFact("category", r.category);
            rowFact("whatsNew", r.whatsNew);
            rowFact("published", r.published);
            rowFact("sha256", r.sha256);
            rowFact("site", r.site);
            rowFact("installState", r.installState);
            rowFact("installedVersion", r.installedVersion);
            rowFact("installedFile", r.installedFile);
            rowFact("experimental", c.scratch);
            r.experimental = c.scratch == "1";
            rowFact("regional", c.scratch);
            r.regional = c.scratch == "1";
            rowFact("capabilities", c.scratch);
            r.haveCapabilities = !c.scratch.empty();
            r.capabilities = r.haveCapabilities ? static_cast<std::uint32_t>(std::strtoul(c.scratch.c_str(), nullptr, 10)) : 0u;
            rowFact("pictures", c.scratch);
            cascade::gui::parseStorePictures(c.scratch, r.pictures);
            for (cascade::gui::StorePicture& sp : r.pictures) {
                for (const cascade::gui::StoreListCache::PictureNow& pn : c.picturesNow) {
                    if (pn.sha256 != sp.sha256) { continue; }
                    sp.state = pn.state;
                    sp.path = pn.path;
                    sp.reason = pn.reason;
                    break;
                }
            }
            r.hasPlan = isPlanned(r.id);
            // THE JOIN to the host's record: the module file of the newest installed copy of this plugin (an update's "from" copy - the file
            // the entry itself would write is not on disk), else the file this machine's build installs under, against a PLUGINS record's
            // file name, ASCII case ignored. No match on an installed entry is a retired module.
            r.haveHost = false;
            r.hostFile.clear();
            r.hostLoaded = false;
            r.hostStopped = false;
            r.hostTune = false;
            r.hostCaps = 0;
            r.hostError.clear();
            const std::string& joinFile = !r.installedFile.empty() ? r.installedFile : r.file;
            if (!joinFile.empty()) {
                for (const cascade::gui::StoreListCache::Host& h : c.hosts) {
                    if (!equalsFileNameAscii(h.file, joinFile)) { continue; }
                    r.haveHost = true;
                    r.hostFile = h.file;
                    r.hostLoaded = h.loaded;
                    r.hostStopped = h.stopped;
                    r.hostTune = h.tune;
                    r.hostCaps = h.caps;
                    c.key.assign("fact.plugin.");
                    c.key += h.file;
                    c.key += ".error";
                    readFactText(c.key.c_str(), r.hostError);
                    break;
                }
            }
            hash = cascade::gui::foldStoreRow(hash, r);
            cascade::gui::fillStoreModule(m.modules[rows++], r);
        }
        m.modules.resize(rows);
        // A DIFFERENT CATALOGUE from the one last read: the ticks were consent for the old one's notices.
        if (c.haveCatalogueHash && hash != c.catalogueHash) { cascade::gui::forgetCatalogueConsent(deck); }
        c.haveCatalogueHash = true;
        c.catalogueHash = hash;
        m.pictureBusy = c.pictureBusy;
    }

    // What is not the lists', every frame (a string given what its room holds allocates nothing).
    m.sourceStatus = statusLines_.catalogStatus;
    m.sourceError = statusLines_.catalogError;
    m.resultReport = statusLines_.installReport;
    m.resultError = statusLines_.installError;
    m.addAllSummary = statusLines_.addAllSummary;
    m.addAllFailed = statusLines_.addAllFailed;
    // THE TICK BELONGS TO ONE MODULE, the one whose page is open, and the reason each row gives is asked of it that way: every
    // other row is asked as unacknowledged, so a module with a notice stays blocked until it is the one on the
    // page (a tick given to the plugin the user just read about must not unblock another's GET key).
    for (std::size_t i = 0; i < m.modules.size(); ++i) {
        cascade::gui::setStoreBlockedReason(m.modules[i], deck.legalAck && m.modules[i].id == deck.pageId);
    }
    // THE PROGRESS, LIVE: the transfer's own figure, which moves with no list moving. 0 while the server has not
    // sent a length; drawn only while something is busy.
    m.progress = 0.0f;
    if (c.transfer) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_STORE_ACTIVITY, 0, &it) > 0 && it.id == 1u) {
            m.progress = static_cast<float>(it.value[0]);
        }
    }
    return c;
}

void AppWindow::drawPluginStoreWindow() {
    // RESET FIRST, AND UNCONDITIONALLY. The fitted window reads this flag to decide whether
    // IT has to print the install/remove outcome, and a store window that is closed prints
    // nothing - so the reset has to happen on every frame, before the open test.
    pluginBrowserDrawnThisFrame_ = false;
    // VERIFICATION ONLY, same house rule as FOXSDR_OPEN_KEY_BINDINGS and
    // FOXSDR_OPEN_FEATURE_REQUEST: the store's open flag is in no config and no remote
    // control, so nothing a bounded self-capture can supply would put it on screen. Once,
    // at the first frame, so a session that closes it again is not fought.
    static const bool openForCapture = std::getenv("FOXSDR_OPEN_PLUGIN_STORE") != nullptr;
    static bool openedForCapture = false;
    if (openForCapture && !openedForCapture) {
        openedForCapture = true;
        pluginBrowseOpen_ = true;
    }
    // THE CONSENTS ARE WATCHED EVERY FRAME, the window open or shut (window port, group 10): the rail tier of the table's cache notices a fetch
    // that began and an ADD ALL run that ended - where the engine used to call into the window from ITS thread for both, to clear the deck's
    // ticks - and clears them here, on the window's own.
    (void)storeLists(false);
    if (!pluginBrowseOpen_) {
        // THE PICTURES ARE KEPT FOR THE WINDOW'S LIFE and no longer: closing it deletes every texture, and the next opening starts on the grid.
        if (pluginStoreView_) { pluginStoreView_->releasePictures(); }
        if (pluginStoreDeck_) { pluginStoreDeck_->pageId.clear(); }
        return;
    }
    uiNotePanel("plugin store");
    testerUsage_->noteFeature("plugins");
    // THE CACHE IS READ BEFORE ANYTHING ASKS IT A QUESTION. storeShouldCheckOnOpen settles on its first call, so asked of a cache that has never
    // been read ("no catalogue") it would send a FETCH for a catalogue another path - the web remote's, a bounded run's hook - had already read.
    cascade::gui::StoreListCache& store = storeLists(true);
    // THE FIRST OPEN READS THE CATALOGUE (0.99.16, gui/store_first_open.hpp): once per session, never at startup, never retried on its own.
    //
    // "A catalogue read" means one read THIS session (0.99.72): the kept copy loaded at start-up fills the window but is not a read, so the
    // first open still asks the network once and the kept copy is what shows until it answers.
    if (cascade::gui::storeShouldCheckOnOpen(storeFirstOpen_, true, store.readThisSession, store.transfer || store.pictureBusy)) {
        uiSubmit(cascade::core::cmd::make(FOXAPI_OP_STORE_FETCH));
    }

    // A REAL OPERATING SYSTEM WINDOW, by the same two devices the map pages use: an opening rectangle that overhangs the main window, and
    // NoAutoMerge to say outright what the overhang only implies - a window that FITS inside the main viewport is otherwise merged into it, and a
    // store dragged small would silently stop being its own window.
    ImGuiWindowClass storeClass;
    storeClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&storeClass);
    // WHERE THE USER LEFT IT (0.99.72), and only if that place still exists on this machine; otherwise it OPENS INSIDE THE MAIN WINDOW at its
    // preferred size, clamped to what fits (gui/page_geometry.hpp). A saved rectangle on a display that has since been unplugged restores
    // off-screen, which is what the same two functions guard the map pages against.
    constexpr float kStoreW = 1480.0f;
    constexpr float kStoreH = 980.0f;
    {
        std::vector<ScreenRect> workAreas;
        const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        for (int mi = 0; mi < pio.Monitors.Size; ++mi) {
            const ImGuiPlatformMonitor& mon = pio.Monitors[mi];
            workAreas.push_back(ScreenRect{mon.WorkPos.x, mon.WorkPos.y, mon.WorkSize.x, mon.WorkSize.y});
        }
        if (workAreas.empty()) {
            const ImGuiViewport* mv = ImGui::GetMainViewport();
            workAreas.push_back(ScreenRect{mv->WorkPos.x, mv->WorkPos.y, mv->WorkSize.x, mv->WorkSize.y});
        }
        if (storeWinW_ > 0 && storeWinH_ > 0 &&
            mapGeometryOnScreen(storeWinX_, storeWinY_, storeWinW_, storeWinH_, workAreas)) {
            int sx = storeWinX_, sy = storeWinY_, sw = storeWinW_, sh = storeWinH_;
            mapClampRestoredSize(sx, sy, sw, sh, workAreas);
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(sx), static_cast<float>(sy)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(sw), static_cast<float>(sh)), ImGuiCond_FirstUseEver);
        } else {
            float sx = 0.0f;
            float sy = 0.0f;
            float sw = kStoreW;
            float sh = kStoreH;
            const ImGuiViewport* mv = ImGui::GetMainViewport();
            cascade::gui::pageOpenInside(mv->Pos.x, mv->Pos.y, mv->Size.x, mv->Size.y, kStoreW, kStoreH, sx, sy, sw, sh);
            ImGui::SetNextWindowPos(ImVec2(sx, sy), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_FirstUseEver);
        }
    }
    bool open = true;
    // The visible half of the title is the plate's own word; the ### half is the stable identity, and it is NOT "###pluginstore" - that id
    // belongs to the rail row, and two widgets sharing one id is how a window's drag state and a rail row's press end up in the same hash
    // bucket. beginPage draws the cabinet this window is, with its name and keys on the rail.
    const bool drawn = beginPage("Plugin store###pluginstorewindow", tr("PLUGIN STORE"), &open, 0, kStoreW, kStoreH);
    // The frame's close button is a real close: it puts the key on the rail back to off.
    if (!open) { pluginBrowseOpen_ = false; }
    // READ BACK EVERY FRAME, which is the whole of the persistence (see the fitted window): ImGui's own .ini is switched off in this
    // application. The position is read even when Begin answered false; the SIZE is not, because a collapsed window reports its
    // title-bar-only height. And not while maximised or rolled up by its own keys.
    if (!pageGeometryTransient("Plugin store###pluginstorewindow")) {
        const ImVec2 wpos = ImGui::GetWindowPos();
        storeWinX_ = static_cast<int>(wpos.x);
        storeWinY_ = static_cast<int>(wpos.y);
        if (drawn) {
            const ImVec2 wsize = ImGui::GetWindowSize();
            storeWinW_ = static_cast<int>(wsize.x);
            storeWinH_ = static_cast<int>(wsize.y);
        }
    }
    if (drawn) {
        // --- what the window is told: the table's, kept when listSeq moves ---------------
        const cascade::gui::PluginStoreModel& model = store.model;

        // --- the cabinet, and the view as content ---------------------------
        // Whether PluginStoreView::draw actually ran; see the request block below for why that is not the same question as "the window drew".
        bool viewDrawn = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (dl != nullptr && avail.x > 8.0f && avail.y > 8.0f) {
            // THE CABINET IS THE PAGE'S OWN - beginPage drew it, screws, rail and keys - so the view goes straight into the well. No title
            // plate of its own: the window already carries the word.
            const float pad = 8.0f;
            const float faceW = avail.x - pad * 2.0f;
            const float faceH = avail.y - pad * 2.0f;
            if (faceW > 40.0f && faceH > 40.0f) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::SetCursorScreenPos(ImVec2(tl.x + pad, tl.y + pad));
                // THE VIEW'S OWN BODY SCROLLS; this pane holds exactly the view. See cascade::gui::storeFaceWindowFlags.
                ImGui::BeginChild("##storeface", ImVec2(faceW, faceH), ImGuiChildFlags_None, cascade::gui::storeFaceWindowFlags());
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (cascade::gui::census::enabled()) {
                    const ImVec2 wp = ImGui::GetWindowPos();
                    const ImVec2 ws = ImGui::GetWindowSize();
                    cascade::gui::census::rect("store:window", wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
                }
                pluginStoreView_->draw(faceW, faceH, model, *pluginStoreDeck_);
                // THE CURSOR THE VIEW LEAVES BEHIND HAS TO BE DECLARED: a cursor moved past a window's content extent with no item after it is
                // exactly what ImGui's own check reports. A zero-size Dummy is that item.
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::EndChild();
                viewDrawn = true;
            } else {
                ImGui::SetCursorScreenPos(ImVec2(tl.x + pad, tl.y + pad));
                ImGui::TextDisabled("%s", tr("Too small - drag the window larger."));
            }
        }

        // --- what the window ASKED for --------------------------------------
        // THE VIEW ASKS, THIS SENDS, and the engine re-tests every one of these when the command applies: a request is raised inside a draw and
        // answered after it, and the state it was raised against can have moved on in between.
        //
        // ONLY IF THE VIEW ACTUALLY DREW THIS FRAME. Its requests are cleared at the top of ITS draw and nowhere else, so a frame that skipped
        // the draw - the window dragged too small, which is a state a user can hold with the mouse button down - would still be holding last
        // frame's answers and would apply them again, once per frame.
        //
        // ...AND THE SAME TEST ANSWERS THE OTHER QUESTION. "Drawn" means the VIEW drew, not merely that the window did: the fitted window reads
        // this flag to decide whether IT prints the install/remove outcome.
        pluginBrowserDrawnThisFrame_ = viewDrawn;
        if (!viewDrawn) {
            endPage();
            return;
        }
        namespace cmd = cascade::core::cmd;
        cascade::gui::PluginStoreDeck& deck = *pluginStoreDeck_;
        if (pluginStoreView_->checkNowRequested()) { uiSubmit(cmd::make(FOXAPI_OP_STORE_FETCH)); }
        if (pluginStoreView_->cancelRequested()) { uiSubmit(cmd::make(FOXAPI_OP_STORE_CANCEL)); }
        // CLEAN UP OLD VERSIONS, confirmed once for all of them (the view's own confirmation). What goes is what the engine's last scan calls
        // superseded NOW, not the frame's list.
        if (pluginStoreView_->cleanupRequested()) { uiSubmit(cmd::makeInt(FOXAPP_OP_STORE_CLEAN_OLD, 0)); }
        // THE BULK KEYS, and the engine re-plans from live state rather than from the plan they were drawn against. The acknowledgement is the
        // deck's own tick, which is not the page's. UPDATE ALL is the same queue restricted to updates (ival[1] 1), which need no
        // acknowledgement: a plugin that is fitted has had its notice shown.
        if (pluginStoreView_->addAllRequested()) {
            // The ENGINE plans the run (OPEN 5); this says only what it cannot know - the ADD ALL tick, and which one module's own notice tick
            // is on (the module whose page is open, as the model asks it).
            const int sel = storeModuleIndexById(model, deck.pageId);
            const std::string acknowledged =
                (deck.legalAck && sel >= 0 && sel < static_cast<int>(model.modules.size())) ? model.modules[static_cast<std::size_t>(sel)].id
                                                                                           : std::string();
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, acknowledged, deck.addAllAck ? 1 : 0));
        }
        if (pluginStoreView_->updateAllRequested()) { uiSubmit(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, std::string(), 0, 1)); }
        const int fitIdx = pluginStoreView_->fitRequested();
        if (fitIdx >= 0 && fitIdx < static_cast<int>(model.modules.size())) {
            // STORE_INSTALL RE-TESTS, never trusts: the view only offers the key where the reason was empty, but the predicate covers a
            // transfer in flight and an acknowledgement that may have been cleared since the key was drawn - and it is the single gate the web
            // control path goes through as well. The acknowledgement belongs to the plugin whose page is open and to no other, so a fit asked
            // for from any other card is asked for unacknowledged. (The page is named by catalogue key, 0.99.74: for a site's plugin the key is
            // not the id.)
            const std::string& feKey = model.modules[static_cast<std::size_t>(fitIdx)].id;
            const bool ack = feKey == deck.pageId && deck.legalAck;
            uiSubmit(cmd::makeText(FOXAPI_OP_STORE_INSTALL, feKey, ack ? 1 : 0));
        }
        const int updIdx = pluginStoreView_->updateRequested();
        if (updIdx >= 0 && updIdx < static_cast<int>(model.modules.size())) {
            // The plan is looked up again when it is applied (APP_STORE_UPDATE) - by the key, which names the module the user clicked: a
            // catalogue replaced between the draw and the apply answers "no catalogue entry with that key", where an index could have named
            // another.
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_UPDATE, model.modules[static_cast<std::size_t>(updIdx)].id));
        }
        // THE SITES PANEL's two requests (0.99.74). The view has refused what it could (storeCheckSiteAdd, and the consent box that has to be
        // ticked before ADD does anything - it raises the request only then, which is why the consent goes out as 1); the engine asks again at
        // the op (the same validator the config loader uses, the list it holds now, no transfer in flight) and ADD saves through the engine's
        // own config write and starts a read; REMOVE forgets the site and its kept copy and leaves what was installed.
        if (!pluginStoreView_->addSiteRequested().empty()) {
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_SITE, pluginStoreView_->addSiteRequested(), 0, 1));
        }
        if (!pluginStoreView_->removeSiteRequested().empty()) {
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_SITE, pluginStoreView_->removeSiteRequested(), 1, 0));
        }
        // THE PICTURES OF THE PAGE THAT IS OPEN: asked for on the edge of opening it (a failed one is tried again then), and on later frames
        // only when the table lists one nobody has asked for - never for the grid, and at most once for a move of listSeq, so a refusal is
        // not sent again every frame.
        if (!pluginStoreView_->pageOpened().empty()) {
            uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, pluginStoreView_->pageOpened(), 1));
            storePicturesWantSeq_ = uiState_.listSeq;
        } else if (!pluginStoreView_->pageId().empty() && storePicturesWantSeq_ != uiState_.listSeq) {
            const int at = storeModuleIndexById(model, pluginStoreView_->pageId());
            if (at >= 0) {
                for (const cascade::gui::StorePicture& pic : model.modules[static_cast<std::size_t>(at)].pictures) {
                    if (pic.state != cascade::gui::PictureState::None) { continue; }
                    uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, pluginStoreView_->pageId(), 0));
                    storePicturesWantSeq_ = uiState_.listSeq;
                    break;
                }
            }
        }
    }
    endPage();
}

}  // namespace cascade::gui
