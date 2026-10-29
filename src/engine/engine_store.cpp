// engine_store.cpp - the plugin store's engine half of 0.99.49 - 0.99.74 (stub campaign B3 of the second pass of the 0.99.84 merge): the
// catalogue read and its kept copy, the plugin sites, the pictures, the clean-up of old copies, and the tables that say so.
//
// WHAT MOVED. Master ran all of it in the window (AppWindow::startCatalogFetch / pollPluginAsync / loadCachedCatalogue / loadCachedSites /
// addPluginSite / removePluginSite / requestScreenshots / pumpScreenshots / cleanUpAfterUpdate / processPendingPluginRemovals ...). It is the
// ENGINE's here, for the reason every store member already was (stage 3): a headless engine serves a store - its web page lists the catalogue,
// fits and removes - and a store that works only inside a window does not. The window half (the store's page, the SITES panel, the pictures'
// textures) is the next batch's; it reads what this file puts on the table and submits the three ops (core/app_commands.hpp).
//
// THE RULES THIS FILE HOLDS (docs/engine-stage4.md 6.22, B3):
//   - NOTHING HERE WAITS ON THE NETWORK OR THE DISK IN THE PASS. A catalogue read, a picture and a clean-up each run on a worker; the pass only
//     collects one that has finished (pollPluginAsync, once a pass). Start-up is the one place a disk is read synchronously (the kept copies, the
//     queue the last session left): it runs before the first pass, like the first scan beside it. REMOVE FILE judges one orphan against the scan
//     and deletes that one file, as removeInstalledPlugin always has.
//   - ONE TRANSFER AT A TIME: a catalogue read, a module download, a picture and a clean-up never overlap (PluginRepo has one progress/cancel
//     pair, and the clean-up edits the manifest a read edits); the picture queue waits behind a read, a download, an ADD ALL run and a clean-up.
//   - THE CLEAN-UP DELETES ONLY THE OLD COPY. core::supersededPlugins is the one place the rules live (a copy a scan turned off as another copy of
//     a loaded, strictly newer, catalogue-recorded plugin; never one the manifest records); an update's own clean-up is restricted again to the
//     copies THAT file superseded (supersededBy). A delete that is refused (Windows will not delete a mapped module) is queued for the next
//     start, and the next start deletes it only if its own scan still finds it superseded.
//   - A SITE IS A STRANGER'S ADDRESS. It is held to https with a lockable host (PluginRepo::normaliseSiteUrl, asked again here whatever the
//     caller checked), listed at most kMaxPluginSites, added only with the consent the window collects (FOXAPP_OP_STORE_SITE ival[1]), its
//     plugins carry their source and are fitted only with the notice acknowledged - and A REMOTE SESSION CAN NEVER FIT ONE (the install op
//     refuses it with kSitePluginDesktopOnly; the web page's catalogue rows say the same).
//   - A FAILED READ NEVER EMPTIES THE STORE: what is on screen stays (an earlier read, or the kept copy), beside the reason.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_repo.hpp"
#include "core/utf8_text.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "core/plugin_store_reasons.hpp"

namespace cascade::engine {

using cascade::i18n::tr;

namespace {

// Reads a catalogue index from the LOCAL filesystem and parses it with the same parseIndex() the network path uses. See
// AppConfig::pluginCatalogueUrl for why this form exists: a catalogue on a corporate share, and the only way the success path of the store can
// be exercised without a live server. It grants nothing the network path does not already allow: parseIndex still refuses every non-https
// download URL and every malformed sha256, and install() re-checks both before it opens a socket - the worst a hostile local index can do is
// offer an entry that install() then refuses. The same cap applies (`cap`: the official limit, or a SITE's, which is held to the site cap
// whether it came over the network or off the user's own disk), because "it is on our own disk" is not a reason to read a 4 GB document.
//
// `rawText`, when given, receives the text that was read once it PARSED (empty otherwise) - what the catalogue cache keeps - and `report` the
// lines parseIndex gave about what it dropped.
bool readLocalCatalogue(const std::string& path, std::vector<cascade::core::PluginCatalogEntry>& out, std::string& error,
                        std::string* rawText = nullptr, std::vector<std::string>* report = nullptr,
                        std::uint64_t cap = cascade::core::PluginRepo::kMaxIndexBytes) {
    out.clear();
    error.clear();
    if (rawText != nullptr) { rawText->clear(); }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
        error = "cannot read the catalogue file \"" + path + "\": " + ec.message();
        return false;
    }
    if (size > cap) {
        error = "the catalogue file \"" + path + "\" is larger than the " + std::to_string(cap) + "-byte limit";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open the catalogue file \"" + path + "\"";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!cascade::core::PluginRepo::parseIndex(text, out, error, report)) { return false; }
    if (rawText != nullptr) { *rawText = text; }
    return true;
}

// ASCII case-insensitive file-name equality: the rule the loader and the install records use (a file name is not case-sensitive here).
bool sameFileName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return true;
}

// The watchdog pause the rescan holds, for the one synchronous delete REMOVE FILE makes.
class StorePause {
public:
    explicit StorePause(Engine& e) : e_(e) { e_.pauseWaitWatchdog(); }
    ~StorePause() { e_.resumeWaitWatchdog(); }
    StorePause(const StorePause&) = delete;
    StorePause& operator=(const StorePause&) = delete;

private:
    Engine& e_;
};

// A value that sits inside one line of a tab-separated fact: the separators become spaces.
std::string oneField(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') { c = ' '; }
    }
    return s;
}

}  // namespace

// ===========================================================================
// THE KEPT COPY
// ===========================================================================

void Engine::loadCachedCatalogue() {
    std::vector<cascade::core::PluginCatalogEntry> kept;
    std::int64_t when = 0;
    std::string error;
    if (cascade::core::PluginRepo::loadCachedIndex(pluginDir_, kept, when, error)) {
        catalog_ = std::move(kept);
        ++catalogSeq_;
        catalogueReadTime_ = when;
        catalogueFromCache_ = !catalog_.empty();
        cascade::core::diagLogf("plugin store: kept catalogue loaded - %zu plugins", catalog_.size());
    } else if (error != "no catalogue has been kept yet") {
        // NEVER A FILE NAME OR PATH IN THE LOG: the reason is PluginRepo's, in words, and names the kept file only as "the kept catalogue".
        cascade::core::diagWarnf("plugin store: kept catalogue ignored - %s", error.c_str());
        cascade::core::health::noteRecovered(cascade::core::health::Recovered::CatalogueBad);
    }
}

int Engine::catalogIndexByKey(const std::string& key) const {
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(i)];
        if (cascade::core::PluginRepo::catalogueKey(e.source, e.id) == key) { return i; }
    }
    return -1;
}

cascade::core::PluginFileRemover Engine::pluginRemoverFor() {
    if (testHooks_.pluginRemove != nullptr) { return testHooks_.pluginRemove; }
    return cascade::core::defaultPluginFileRemover();
}

// ===========================================================================
// PLUGIN SITES
// ===========================================================================

void Engine::loadCachedSites() {
    if (pluginSites_.empty() && siteStatus_.empty()) { return; }
    // The official part of whatever is on screen stays; each site's kept copy is merged under the same rules a fresh read is
    // (PluginRepo::mergeSite: official wins an id, one host).
    std::vector<cascade::core::PluginCatalogEntry> merged;
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        if (e.source.empty()) { merged.push_back(e); }
    }
    siteStatus_.clear();
    bool anyKept = false;
    for (const std::string& url : pluginSites_) {
        SiteStatus st;
        st.url = url;
        std::vector<cascade::core::PluginCatalogEntry> kept;
        std::int64_t when = 0;
        std::string error;
        if (cascade::core::PluginRepo::loadSiteCache(pluginDir_, url, kept, when, error)) {
            const cascade::core::PluginRepo::SiteMergeResult m = cascade::core::PluginRepo::mergeSite(merged, kept, url);
            merged = m.merged;
            st.state = SiteStatus::State::Kept;
            st.listed = m.added;
            st.hiddenSameId = m.hiddenSameId;
            st.hiddenOffHost = m.hiddenOffHost;
            anyKept = anyKept || m.added > 0;
        } else if (error != "no catalogue has been kept yet") {
            // The reason is PluginRepo's, in words, and names the file only as "the kept catalogue": never an address or a path in the log.
            cascade::core::diagWarnf("plugin store: a site's kept catalogue ignored - %s", error.c_str());
            cascade::core::health::noteRecovered(cascade::core::health::Recovered::CatalogueBad);
        }
        siteStatus_.push_back(std::move(st));
    }
    catalog_ = std::move(merged);
    ++catalogSeq_;
    // A list made of kept copies is not a read of this session (see catalogueFromCache_).
    if (anyKept) { catalogueFromCache_ = true; }
}

void Engine::applySiteResults(const std::vector<CatalogFetchResult::SiteResult>& results) {
    std::vector<SiteStatus> next;
    int read = 0;
    int failed = 0;
    int hidden = 0;
    for (const std::string& url : pluginSites_) {
        SiteStatus st;
        st.url = url;
        for (const CatalogFetchResult::SiteResult& r : results) {
            if (r.url != url) { continue; }
            st.listed = r.listed;
            st.hiddenSameId = r.hiddenSameId;
            st.hiddenOffHost = r.hiddenOffHost;
            if (r.ok) {
                st.state = SiteStatus::State::Read;
                ++read;
            } else {
                st.state = SiteStatus::State::Failed;
                st.error = r.error;
                ++failed;
            }
            hidden += r.hiddenSameId + r.hiddenOffHost;
            // What parseIndex() dropped, a few lines at most, as for the official catalogue - ids and words only (never the address).
            constexpr std::size_t kMaxSiteNotes = 4;
            for (std::size_t i = 0; i < r.parseReport.size() && i < kMaxSiteNotes; ++i) {
                cascade::core::diagLogf("plugin store: site note - %s", r.parseReport[i].c_str());
            }
            break;
        }
        // A site the read did not know (added while it ran) keeps its earlier status.
        if (st.state == SiteStatus::State::NotRead) {
            for (const SiteStatus& old : siteStatus_) {
                if (old.url == url) {
                    st = old;
                    break;
                }
            }
        }
        next.push_back(std::move(st));
    }
    siteStatus_ = std::move(next);
    if (!pluginSites_.empty()) {
        cascade::core::diagLogf("plugin store: %zu plugin sites - %d read, %d could not be read, %d plugins hidden", pluginSites_.size(), read, failed,
                                hidden);
    }
}

bool Engine::addPluginSite(const std::string& raw, std::string& why) {
    why.clear();
    // The list does not change under a read, a download, a picture or a clean-up in flight: the read hands back a merged list made from the
    // sites it was started with.
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) {
        why = "a transfer is already in progress";
        return false;
    }
    std::string url;
    if (!cascade::core::PluginRepo::normaliseSiteUrl(raw, url, why)) { return false; }
    if (url == cascade::core::PluginRepo::defaultIndexUrl() || url == pluginCatalogueUrl_ ||
        std::find(pluginSites_.begin(), pluginSites_.end(), url) != pluginSites_.end()) {
        why = "that site is already in the list";
        return false;
    }
    if (pluginSites_.size() >= cascade::core::AppConfig::kMaxPluginSites) {
        why = "the list of sites is full";
        return false;
    }
    pluginSites_.push_back(url);
    SiteStatus st;
    st.url = url;
    siteStatus_.push_back(std::move(st));
    // The config follows through the debounced save (the window's comparison reads fillConfig's pluginSites). The read that follows is the
    // store's own CHECK NOW; if a transfer were running it would be refused and the site would wait, "not read yet" (it cannot be: the check
    // above is the same one).
    cascade::core::diagLogf("plugin store: a plugin site was added (%zu now)", pluginSites_.size());
    startCatalogFetch();
    return true;
}

void Engine::removePluginSite(const std::string& url) {
    // Not under a transfer (see addPluginSite): the read would hand back the site's plugins into a list that had just forgotten it.
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) { return; }
    const auto it = std::find(pluginSites_.begin(), pluginSites_.end(), url);
    if (it == pluginSites_.end()) { return; }
    pluginSites_.erase(it);
    siteStatus_.erase(std::remove_if(siteStatus_.begin(), siteStatus_.end(), [&](const SiteStatus& s) { return s.url == url; }), siteStatus_.end());
    // ITS PLUGINS LEAVE THE LIST; what was installed from it stays installed (the files and the records are untouched - the records keep
    // their source, and no entry carries it any more, so they get no updates) and its kept copy goes - on a worker, two small deletes.
    catalog_.erase(std::remove_if(catalog_.begin(), catalog_.end(), [&](const cascade::core::PluginCatalogEntry& e) { return e.source == url; }),
                   catalog_.end());
    ++catalogSeq_;
    forgetStalePictures();
    cleanupPending_ = true;
    cleanupFuture_ = std::async(std::launch::async, [dir = pluginDir_, url]() {
        CleanupResult r;
        (void)cascade::core::PluginRepo::removeSiteCache(dir, url);
        return r;
    });
    cascade::core::diagLogf("plugin store: a plugin site was removed (%zu left)", pluginSites_.size());
}

// ===========================================================================
// THE READ
// ===========================================================================

void Engine::startCatalogFetch() {
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) { return; }
    // catalog_ IS LEFT AS IT IS (0.99.72). It used to be emptied here, and again when the fetch failed, so a user offline for a moment was
    // shown an empty store. Now the list on screen stays until a fetch SUCCEEDS and replaces it; a failure leaves it (or, with nothing on
    // screen, brings back the kept copy) beside the reason. What stays offered is only ever a list this client accepted once, and install()
    // re-checks every entry it is handed (ABI, https, sha256, file name) whatever list it came from.
    //
    // AND THE SELECTION AND THE CONSENT GO WITH IT. The store window clamps its own selection every frame and clears its tick whenever the
    // selection moves, but the tick is consent for ONE plugin and a fetch that replaces the whole catalogue can leave the same index
    // pointing at a different module - so it is cleared here, at the moment the ground moves. The ADD ALL tick goes too: it was given against
    // the notices the old catalogue listed. (The standard window does not take this call any more: it sees the fetch in STORE_ACTIVITY -
    // entry 1, value[1] 1 - or the catalogue it replaced, and clears its own ticks on its own thread. See AppWindow::storeLists.)
    host_->onCatalogueFetchStarting();
    catalogError_.clear();
    catalogStatus_.clear();
    installError_.clear();
    installReport_.clear();
    catalogPending_ = true;

    const std::string url = pluginCatalogueUrl_;
    const std::string dir = pluginDir_;
    // THE PLUGIN SITES (0.99.74) are read on this same worker, after the official catalogue, and what is on screen is handed over by value: a
    // site that cannot be read this time is listed from the earlier read of this session, or from its kept copy, exactly as the official
    // catalogue is.
    const std::vector<std::string> sites = pluginSites_;
    const std::vector<cascade::core::PluginCatalogEntry> shown =
        sites.empty() ? std::vector<cascade::core::PluginCatalogEntry>{} : catalog_;
    catalogFuture_ = std::async(std::launch::async, [this, url, dir, sites, shown] {
        CatalogFetchResult r;
        // Reads every site in order and merges each into r.entries, OFFICIAL WINNING an id (PluginRepo::mergeSite, which also holds each
        // site's files to its own host). `fresh` receives the merged entries of the sites READ THIS TIME: only those may write a retirement
        // floor. A site that fails never touches the list it found - it contributes its earlier entries, or none.
        const auto readSites = [&](std::vector<cascade::core::PluginCatalogEntry>& fresh) {
            r.sitesDone = true;
            for (const std::string& siteUrl : sites) {
                CatalogFetchResult::SiteResult sr;
                sr.url = siteUrl;
                std::vector<cascade::core::PluginCatalogEntry> got;
                std::string siteText;
                if (siteUrl.find("://") == std::string::npos) {
                    // A local index (the test seam, and a share): read from disk, held to the same cap.
                    sr.ok = readLocalCatalogue(siteUrl, got, sr.error, &siteText, &sr.parseReport, cascade::core::PluginRepo::kMaxSiteIndexBytes);
                } else {
                    sr.ok = pluginRepo_.fetchSiteIndex(siteUrl, got, siteText, sr.parseReport, sr.error);
                }
                if (sr.ok) {
                    sr.readTime = static_cast<std::int64_t>(std::time(nullptr));
                    if (!cascade::core::PluginRepo::saveSiteCache(dir, siteUrl, siteText, sr.readTime, sr.cacheError)) {
                        sr.cacheError = "the site loaded, but could not be kept for the next start: " + sr.cacheError;
                    }
                } else {
                    // The earlier read of this session, else the kept copy: never an empty store for a site that is only down for a moment.
                    got.clear();
                    for (const cascade::core::PluginCatalogEntry& e : shown) {
                        if (e.source == siteUrl) { got.push_back(e); }
                    }
                    if (got.empty()) {
                        std::int64_t when = 0;
                        std::string keptError;
                        if (cascade::core::PluginRepo::loadSiteCache(dir, siteUrl, got, when, keptError)) {
                            sr.readTime = when;
                        } else {
                            got.clear();
                        }
                    }
                    sr.keptShown = !got.empty();
                }
                const cascade::core::PluginRepo::SiteMergeResult merge = cascade::core::PluginRepo::mergeSite(r.entries, got, siteUrl);
                r.entries = merge.merged;
                sr.listed = merge.added;
                sr.hiddenSameId = merge.hiddenSameId;
                sr.hiddenOffHost = merge.hiddenOffHost;
                if (sr.ok) {
                    for (const cascade::core::PluginCatalogEntry& e : r.entries) {
                        if (e.source == siteUrl) { fresh.push_back(e); }
                    }
                }
                r.sites.push_back(std::move(sr));
            }
        };
        std::string rawText;  // the index as read, once it parsed - what the cache keeps
        if (url.find("://") == std::string::npos) {
            // No scheme at all: a local file (see readLocalCatalogue). A URL WITH a scheme - including http:// - goes to fetchIndex, which is
            // the single place the https-only rule is enforced.
            r.ok = readLocalCatalogue(url, r.entries, r.error, &rawText, &r.parseReport);
        } else {
            r.ok = pluginRepo_.fetchIndex(url, r.error);
            if (r.ok) {
                r.entries = pluginRepo_.entries();
                rawText = pluginRepo_.lastIndexText();
                r.parseReport = pluginRepo_.lastParseReport();
            }
        }
        if (r.ok) {
            // THE CATALOGUE IS KEPT (0.99.72), straight away and before the regional step, which can take seconds: the PUBLIC index exactly as
            // it was read, and the time of the read, beside installed.json. Only text that parsed is ever written (PluginRepo::
            // saveCatalogueCache checks again), so a refused index is never what the next start shows. The regional list is not kept: its
            // outcome is not disclosed.
            r.readTime = static_cast<std::int64_t>(std::time(nullptr));
            if (!cascade::core::PluginRepo::saveCatalogueCache(dir, rawText, r.readTime, r.cacheError)) {
                r.cacheError = "the catalogue loaded, but could not be kept for the next start: " + r.cacheError;
            }
            // REGIONAL CATALOGUE (plugin_repo.hpp). Only after a SUCCESSFUL public fetch, and only when the store is on the default catalogue
            // or the test override is set - a user pointed at a corporate share or a local file gets no foxsdr.com request at all. A regional
            // failure of any kind - transport, HTTP status, parse, oversize - is swallowed completely: no error field is set here, nothing is
            // shown, and NOTHING IS LOGGED, because the outcome itself would disclose which country answered the request. regionalState /
            // regionalAdded / regionalDropped exist ONLY for the CASCADE_PLUGIN_TEST hook (reportPluginTestResult) and the LOCAL facts.
            if (cascade::core::PluginRepo::regionalWanted(url, cascade::core::PluginRepo::regionalOverrideSet())) {
                const std::string regionalUrl = cascade::core::PluginRepo::regionalIndexUrl();
                std::vector<cascade::core::PluginCatalogEntry> regionalEntries;
                std::string regionalError;
                bool regionalOk = false;
                if (regionalUrl.find("://") == std::string::npos) {
                    // Same test seam as the public catalogue's local form.
                    regionalOk = readLocalCatalogue(regionalUrl, regionalEntries, regionalError);
                } else {
                    regionalOk = pluginRepo_.fetchRegionalIndex(regionalUrl, regionalEntries, regionalError);
                }
                if (regionalOk) {
                    const cascade::core::PluginRepo::RegionalMergeResult merge = cascade::core::PluginRepo::mergeRegional(
                        r.entries, regionalEntries, cascade::core::PluginRepo::regionalDownloadPrefix());
                    r.entries = merge.merged;
                    r.regionalState = "ok";
                    r.regionalAdded = merge.added;
                    r.regionalDropped = static_cast<int>(merge.dropped.size());
                } else {
                    // SILENT. The store shows exactly the public catalogue, as if this branch never ran.
                    r.regionalState = "failed";
                }
            }
            // THE ONLY MOMENT A RETIREMENT FLOOR IS WRITTEN TO THIS MACHINE. Enforcement reads the cache and never the network, so a floor
            // that is not cached here protects nobody - not the user who goes offline for a year, and not the one who never opens this
            // browser again. It runs on the worker because it re-hashes every installed plugin. Runs on the MERGED list, so a regional
            // plugin's floor is cached exactly like any public one (and a later fetch made outside the region cannot remove it - see
            // mergePolicies' own fail-open rule).
            //
            // THE PLUGIN SITES JOIN THE LIST HERE (0.99.74), after the regional step and before this, so the policy cache and the picture
            // prune below are judged against the MERGED list. Only what was read this time writes a floor: the official catalogue's (and the
            // regional list's) entries, and each site's that was read - a site that failed contributes its earlier entries to the list and
            // nothing to the cache.
            std::vector<cascade::core::PluginCatalogEntry> freshForPolicy = r.entries;
            if (!sites.empty()) {
                std::vector<cascade::core::PluginCatalogEntry> freshFromSites;
                readSites(freshFromSites);
                freshForPolicy.insert(freshForPolicy.end(), freshFromSites.begin(), freshFromSites.end());
            }
            std::string policyError;
            if (!cascade::core::PluginRepo::cacheCataloguePolicies(dir, freshForPolicy, policyError)) {
                r.policyError = "the catalogue loaded, but its plugin version policy could not be saved, so it will not be remembered: " + policyError;
            }
            // PICTURES THE CATALOGUE NO LONGER NAMES go from the store-cache folder (0.99.72), here on the worker with the other disk work,
            // judged against the list just read - never against a kept copy or a failure. The MERGED list (0.99.74): a site's pictures are
            // named by its entries.
            r.pruned = cascade::core::PluginRepo::pruneScreenshotCache(dir, r.entries).size();
        } else {
            // THE READ FAILED: bring back the kept copy for the pass to show if it has nothing on screen. Read here so the pass does no disk
            // work. A copy that cannot be used is simply not offered (the reason is not needed: the failure shown is the refresh's).
            std::string keptError;
            if (!cascade::core::PluginRepo::loadCachedIndex(dir, r.keptEntries, r.keptTime, keptError)) {
                r.keptEntries.clear();
                r.keptTime = 0;
            }
            // THE SITES ARE READ ANYWAY (0.99.74): a user whose official source is down - or is a share that is gone - still has the sites
            // they added. Under them goes what stands in the official catalogue's place: what was on screen, else the kept copy. Nothing is
            // pruned and no floor is written from the official part, which was not read.
            if (!sites.empty()) {
                for (const cascade::core::PluginCatalogEntry& e : shown) {
                    if (e.source.empty()) { r.entries.push_back(e); }
                }
                if (r.entries.empty()) { r.entries = r.keptEntries; }
                std::vector<cascade::core::PluginCatalogEntry> freshFromSites;
                readSites(freshFromSites);
                if (!freshFromSites.empty()) {
                    std::string policyError;
                    if (!cascade::core::PluginRepo::cacheCataloguePolicies(dir, freshFromSites, policyError)) {
                        r.policyError = "the sites loaded, but their plugin version policy could not be saved, so it will not be remembered: " +
                                        policyError;
                    }
                }
            }
        }
        return r;
    });
}

void Engine::startInstall(cascade::core::PluginCatalogEntry entry) {
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = entry.name;
    // BY CATALOGUE KEY (0.99.74): the store compares these with a card's key, which is the id for an official plugin and "<id>@<site>" for a
    // site's.
    installBusyId_ = cascade::core::PluginRepo::catalogueKey(entry.source, entry.id);
    installResultId_ = installBusyId_;
    installPending_ = true;
    const std::string dir = pluginDir_;
    installFuture_ = std::async(std::launch::async, [this, e = std::move(entry), dir]() {
        PluginInstallResult r;
        r.name = e.name;
        // Every security rule lives inside install(): ABI match, platform match, file-name sanitisation, https, the byte cap, and the sha256
        // that decides whether the temp file ever becomes a plugin.
        r.ok = pluginRepo_.install(e, dir, r.installedPath, r.error);
        if (r.ok) {
            // MANIFEST UPKEEP, without which the whole retirement feature silently no-ops: an unrecorded plugin has no id, no version and no
            // cached policy, so pluginBlockReason() fails open for it forever. This is where a plain install becomes managed - applyUpdate()
            // records itself, so this is the only install path that needs it.
            (void)cascade::core::PluginRepo::recordInstall(dir, e, r.recordError);
        }
        return r;
    });
}

void Engine::startUpdate(const cascade::core::PluginUpdate& u) {
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_ || u.entry == nullptr) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = u.entry->name;
    installBusyId_ = cascade::core::PluginRepo::catalogueKey(u.entry->source, u.entry->id);
    installResultId_ = installBusyId_;
    installPending_ = true;
    const std::string dir = pluginDir_;
    // The plan's entry aliases catalog_, which a refresh may replace while this runs, so the worker owns a copy and the plan is re-pointed
    // at it.
    installFuture_ = std::async(std::launch::async, [this, plan = u, entry = *u.entry, dir]() mutable {
        PluginInstallResult r;
        r.isUpdate = true;
        r.name = entry.name;
        plan.entry = &entry;
        // applyUpdate is install() plus recordInstall(), in that order and with the same gauntlet - nothing here shortcuts it because "it is
        // only an update".
        r.ok = pluginRepo_.applyUpdate(plan, dir, r.installedPath, r.error);
        return r;
    });
}

void Engine::pollPluginAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (catalogPending_ && catalogFuture_.valid() && catalogFuture_.wait_for(kNoWait) == std::future_status::ready) {
        CatalogFetchResult r = catalogFuture_.get();
        catalogPending_ = false;
        // Held for the CASCADE_PLUGIN_TEST report and the LOCAL facts only - see the member comment.
        regionalState_ = r.regionalState;
        regionalAdded_ = r.regionalAdded;
        regionalDropped_ = r.regionalDropped;
        // WHAT EACH PLUGIN SITE SAID (0.99.74), whether or not the official read succeeded: the status lines and the SITES panel are drawn
        // from this.
        if (r.sitesDone) { applySiteResults(r.sites); }
        if (r.ok) {
            catalog_ = std::move(r.entries);
            ++catalogSeq_;
            // READ THIS SESSION, now: the kept copy (if that is what was on screen) is replaced, and the time is this read's.
            catalogueFromCache_ = false;
            catalogueRefreshFailed_ = false;
            catalogueReadTime_ = r.readTime;
            forgetStalePictures();
            // What parseIndex() dropped and why (a picture with an http URL, a malformed hash, ...), a few lines at most: the log is the only
            // place a catalogue author's mistake is seen. Public data - ids and words, no path and nothing of this machine.
            constexpr std::size_t kMaxNoteLines = 8;
            for (std::size_t i = 0; i < r.parseReport.size() && i < kMaxNoteLines; ++i) {
                cascade::core::diagLogf("plugin store: catalogue note - %s", r.parseReport[i].c_str());
            }
            if (r.parseReport.size() > kMaxNoteLines) {
                cascade::core::diagLogf("plugin store: catalogue note - and %zu more", r.parseReport.size() - kMaxNoteLines);
            }
            // "In the catalogue" is the OFFICIAL catalogue's count (and the regional list's): a site's plugins are counted on its own status
            // line (0.99.74).
            std::size_t officialCount = 0;
            for (const cascade::core::PluginCatalogEntry& ce : catalog_) {
                if (ce.source.empty()) { ++officialCount; }
            }
            std::string statusBuf;
            cascade::core::formatUtf8(statusBuf, officialCount == 1u ? tr("%zu plugin in the catalogue") : tr("%zu plugins in the catalogue"),
                                      officialCount);
            catalogStatus_ = statusBuf;
            // "When the user last chose to look" - recorded here, never acted on: no code path reads this to decide whether to fetch (see
            // AppConfig::pluginLastUpdateCheck).
            pluginLastUpdateCheck_ = static_cast<std::int64_t>(std::time(nullptr));
            // The floors the worker just cached only take effect on the next scan, and a user who has just been told a plugin is retired should
            // not have to restart to stop running it. But a fetch that changed no floor and no file has nothing to take effect: the full rescan
            // - every decoder destroyed, every module unmapped and mapped again, on this thread, unbounded - is made only when the plugins
            // folder is not what the last scan left (rescanPluginsIfChanged). The worker leaves the manifest alone when the catalogue taught it
            // nothing (PluginRepo::cacheCataloguePolicies), so "unchanged" is a state a fetch can leave the folder in.
            rescanPluginsIfChanged();
            // A cache-write failure is red text next to a catalogue that nevertheless loaded: the list is real, the policy behind it was not
            // remembered (or the offline copy is not there), and both facts are shown.
            if (!r.policyError.empty()) {
                catalogError_ = r.policyError;
            } else if (!r.cacheError.empty()) {
                catalogError_ = r.cacheError;
            }
        } else {
            // A FAILED READ DOES NOT EMPTY THE STORE (0.99.72). What is on screen stays - an earlier read this session, or the kept copy - and
            // with nothing on screen the kept copy comes back, with the time it was read. The reason is the refresh's; the window makes
            // "Catalogue from <date>; could not refresh: <reason>" of it, fact.store.cacheTime and fact.store.fromCache.
            catalogError_ = r.error;
            catalogueRefreshFailed_ = true;
            if (r.sitesDone) {
                // The sites were read anyway (startCatalogFetch), and r.entries is the list they were merged into: what stood in the official
                // catalogue's place - the list on screen, else the kept copy - and then each site's. Same official part either way, so
                // replacing the list loses nothing.
                const bool wasEmpty = catalog_.empty();
                catalog_ = std::move(r.entries);
                ++catalogSeq_;
                forgetStalePictures();
                if (wasEmpty && !r.keptEntries.empty()) {
                    catalogueReadTime_ = r.keptTime;
                    catalogueFromCache_ = true;
                }
                if (!r.policyError.empty()) { catalogError_ += " " + r.policyError; }
            } else if (catalog_.empty() && !r.keptEntries.empty()) {
                catalog_ = std::move(r.keptEntries);
                ++catalogSeq_;
                catalogueReadTime_ = r.keptTime;
                catalogueFromCache_ = true;
            }
            if (!catalog_.empty()) {
                // The reason is NOT logged: it can carry the catalogue address, and an address is never written to the log. The count is enough.
                cascade::core::diagLogf("plugin store: catalogue refresh failed - showing %s copy of %zu plugins",
                                        catalogueFromCache_ ? "the kept" : "the earlier", catalog_.size());
            }
        }
        // PUBLISHED BEFORE THE HOST IS TOLD: the host reads the status lines through the copy (statusText, OPEN 3), and a host told first read
        // the copy from before the fetch - a bounded run's report printed "entries=0" for a fetch that had failed
        // (tests/test_catalogue_error_report.cpp).
        publishStatusText();
        host_->onCatalogueResult();
        // A bounded run's diagnostic (CASCADE_PLUGIN_TEST): the engine prints the catalogue and every entry's install-gate decision from its own
        // state, naming the frame the window had reached - armed by the bounded run's host and told the frame at the end of each of the window's
        // frames (shell retirement R9: they were fields of FrontEndFacts, handed over by the window every frame, until then; pump move step P2: the
        // window's onCatalogueResult hook used to read its own frame counter from this pass).
        if (pluginTestReport_.load(std::memory_order_relaxed)) { reportPluginTestResult(pluginTestFrame_.load(std::memory_order_relaxed)); }
    }

    // A picture that finished (0.99.72), then the next one in line (below, last).
    if (screenshotPending_ && screenshotFuture_.valid() && screenshotFuture_.wait_for(kNoWait) == std::future_status::ready) {
        ScreenshotFetchResult sr = screenshotFuture_.get();
        screenshotPending_ = false;
        screenshotInFlight_.clear();
        PictureStatus& st = pictures_[sr.sha256];
        if (sr.ok) {
            st.state = PictureState::Ready;
            st.path = sr.path;
            st.reason.clear();
        } else {
            st.state = PictureState::Failed;
            st.path.clear();
            st.reason = sr.error;
        }
        ++picturesSeq_;
    }

    if (installPending_ && installFuture_.valid() && installFuture_.wait_for(kNoWait) == std::future_status::ready) {
        PluginInstallResult r = installFuture_.get();
        installPending_ = false;
        installBusyName_.clear();
        installBusyId_.clear();
        // The point of the rescan: the file is on disk, but it is not a PLUGIN until the host has loaded and validated it - and if it fails
        // validation the user needs to see that here, immediately, rather than after a restart. It also re-runs enforcement, which is what makes
        // an update lift the retirement in the same pass it lands.
        //
        // Run on the ONE failure that still changed the disk too: applyUpdate reports false with installedPath set when the new file installed
        // but the manifest could not be written.
        if (r.ok || !r.installedPath.empty()) { rescanPlugins(); }
        if (r.ok) {
            std::string reportBuf;
            cascade::core::formatUtf8(reportBuf, r.isUpdate ? tr("Updated %s to %s") : tr("Installed %s to %s"), r.name.c_str(),
                                      r.installedPath.c_str());
            installReport_ = reportBuf;
            // THE COPY IT REPLACED GOES NOW (0.99.49 beta feedback), once the rescan above has loaded the new one and turned the old one off -
            // or, still in use, at the next start. Its sentence is joined to this line when the worker finishes. An UPDATE only: a plain install
            // beside a side-loaded older copy leaves that copy for the clean-up key, which asks first.
            if (r.isUpdate) { cleanUpAfterUpdate(r.installedPath); }
            if (!r.recordError.empty()) {
                // Honest partial state: verified bytes are installed, but the plugin is unmanaged until a later install or catalogue fetch repairs
                // the record - and an unmanaged plugin is never retired, which is the fail-open rule doing its job.
                std::string recordBuf;
                cascade::core::formatUtf8(recordBuf,
                                          tr("%s was installed, but its record could not be written: %s. "
                                             "It will not be version-checked until that is repaired."),
                                          r.name.c_str(), r.recordError.c_str());
                installError_ = recordBuf;
            }
        } else {
            installError_ = r.error;
        }
        // THE RUN KEEPS ITS OWN TALLY, here, where the single authority on whether a transfer succeeded already is. Reading installReport_ back
        // out of the panel afterwards would be a second opinion about the same event, and the two would eventually disagree.
        if (addAllRun_.active) {
            if (r.ok) {
                ++addAllRun_.installed;
            } else {
                ++addAllRun_.failed;
                addAllRun_.failures.emplace_back(r.name, r.error);
                cascade::core::diagLogf("plugin store: add all - %s FAILED: %s", r.name.c_str(), r.error.c_str());
            }
        }
    }

    // A clean-up that finished: said once in the log, in the store's line, and (an update's own) on the install report; the rescan that
    // follows drops the records of what was deleted.
    if (cleanupPending_ && cleanupFuture_.valid() && cleanupFuture_.wait_for(kNoWait) == std::future_status::ready) {
        CleanupResult r = cleanupFuture_.get();
        cleanupPending_ = false;
        for (const std::string& f : r.res.removed) { cascade::core::diagLogf("plugin: removed old copy %s", f.c_str()); }
        for (const std::string& f : r.res.queued) { cascade::core::diagLogf("plugin: old copy %s is in use - queued for the next start", f.c_str()); }
        for (const std::string& f : r.res.failed) { cascade::core::diagWarnf("plugin: old copy not removed: %s", f.c_str()); }
        if (r.report) {
            pluginCleanupReport_ = cascade::gui::pluginCleanupReport(r.res);
            if (r.afterUpdate && !pluginCleanupReport_.empty()) { installReport_ += " " + pluginCleanupReport_; }
        }
        // The host still lists the records of what was deleted; the rescan drops them and rebuilds pluginOldCopies_.
        if (!r.res.removed.empty()) { rescanPlugins(); }
    }

    // LAST, so a slot a catalogue fetch, an install or a clean-up has just freed goes to the next queued picture only when nothing else wants
    // it (pumpScreenshots).
    pumpScreenshots();
}

// ===========================================================================
// THE PICTURES (0.99.72)
// ===========================================================================

void Engine::requestScreenshots(const std::string& pluginKey, bool retryFailed) {
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        if (cascade::core::PluginRepo::catalogueKey(e.source, e.id) != pluginKey) { continue; }
        for (const cascade::core::CatalogScreenshot& s : e.screenshots) {
            // A SITE'S PICTURE IS FETCHED ONLY FROM THE SITE'S OWN HOST (0.99.74). mergeSite() held every url to it when the list was made;
            // this is the second look, at the moment of the request, because the request is what contacts an address.
            if (!e.source.empty() && !cascade::core::PluginRepo::siteFilesAllowed(s.url, e.source)) { continue; }
            PictureStatus& st = pictures_[s.sha256];
            if (st.state == PictureState::Ready || st.state == PictureState::Pending) { continue; }
            if (st.state == PictureState::Failed && !retryFailed) { continue; }
            st.state = PictureState::Pending;
            st.path.clear();
            st.reason.clear();
            screenshotQueue_.push_back(s);
            ++picturesSeq_;
        }
        return;
    }
}

Engine::PictureStatus Engine::pictureStatus(const cascade::core::CatalogScreenshot& s) const {
    const auto it = pictures_.find(s.sha256);
    return it == pictures_.end() ? PictureStatus{} : it->second;
}

void Engine::pumpScreenshots() {
    if (screenshotPending_ || screenshotQueue_.empty()) { return; }
    // ONE TRANSFER AT A TIME, and the pictures are the least of them: a catalogue fetch, an install (alone or in an ADD ALL run) or a
    // clean-up holds the slot, and the queue waits. The pending ones are not lost - they stay Pending.
    if (catalogPending_ || installPending_ || addAllRun_.active || cleanupPending_) { return; }
    const cascade::core::CatalogScreenshot s = screenshotQueue_.front();
    screenshotQueue_.pop_front();
    screenshotPending_ = true;
    screenshotInFlight_ = s.sha256;
    ++picturesSeq_;
    const std::string dir = pluginDir_;
    screenshotFuture_ = std::async(std::launch::async, [this, s, dir]() {
        ScreenshotFetchResult r;
        r.sha256 = s.sha256;
        r.ok = pluginRepo_.fetchScreenshot(dir, s, r.path, r.error);
        return r;
    });
}

void Engine::forgetStalePictures() {
    // Named by the catalogue just read, and only those: a picture it no longer lists is deleted from the disk by the worker
    // (pruneScreenshotCache), so a Ready status for it would point at nothing.
    std::set<std::string> named;
    for (const cascade::core::PluginCatalogEntry& e : catalog_) {
        for (const cascade::core::CatalogScreenshot& s : e.screenshots) { named.insert(s.sha256); }
    }
    bool changed = false;
    for (auto it = pictures_.begin(); it != pictures_.end();) {
        if (named.count(it->first) != 0) {
            ++it;
        } else {
            it = pictures_.erase(it);
            changed = true;
        }
    }
    const std::size_t before = screenshotQueue_.size();
    screenshotQueue_.erase(std::remove_if(screenshotQueue_.begin(), screenshotQueue_.end(),
                                          [&](const cascade::core::CatalogScreenshot& s) { return named.count(s.sha256) == 0; }),
                           screenshotQueue_.end());
    if (changed || before != screenshotQueue_.size()) { ++picturesSeq_; }
}

// ===========================================================================
// CLEAN-UP OF OLD COPIES (0.99.49 - 0.99.72)
// ===========================================================================

void Engine::startCleanUp(std::vector<cascade::core::SupersededPlugin> which, bool afterUpdate) {
    // The store's line says what THIS clean-up did; one that has nothing to do says nothing (an update that superseded nothing says nothing).
    pluginCleanupReport_.clear();
    if (which.empty()) { return; }
    // NOTHING TO UNLOAD FIRST: a copy the scan turned off was unmapped by the scan itself (PluginHost::scan), so in this process it is only a
    // file. Another process can still hold it - a second FoxSDR - and that is the file that is queued for the next start.
    cleanupPending_ = true;
    cleanupFuture_ = std::async(std::launch::async, [dir = pluginDir_, which = std::move(which), remover = pluginRemoverFor(), afterUpdate]() {
        CleanupResult r;
        r.report = true;
        r.afterUpdate = afterUpdate;
        r.res = cascade::core::removeSupersededPlugins(dir, which, remover);
        return r;
    });
}

void Engine::cleanUpOldVersionsConfirmed() {
    // Not under a transfer: an update in flight is about to change which copy is the newest.
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) { return; }
    installError_.clear();
    startCleanUp(pluginOldCopies_, false);
}

void Engine::cleanUpAfterUpdate(const std::string& installedPath) {
    // ONLY THE PLUGIN THAT WAS UPDATED: the copies the new file superseded, found by the scan that has just loaded it. Anything else
    // superseded is the clean-up key's to offer, not this update's to take.
    const std::string file = std::filesystem::path(installedPath).filename().string();
    startCleanUp(cascade::core::supersededBy(pluginOldCopies_, file), true);
}

void Engine::processPendingPluginRemovals() {
    const cascade::core::PluginCleanupResult res = cascade::core::processPendingRemovals(pluginDir_, pluginHost_.plugins(), pluginInventory_.plugins,
                                                                                         pluginRemoverFor());
    for (const std::string& f : res.removed) {
        cascade::core::diagLogf("plugin: removed old copy %s (queued by an earlier session)", f.c_str());
    }
    for (const std::string& f : res.dropped) { cascade::core::diagLogf("plugin: %s is no longer an old copy - left alone", f.c_str()); }
    for (const std::string& f : res.queued) { cascade::core::diagLogf("plugin: old copy %s is still in use - queued again", f.c_str()); }
    if (!res.removed.empty()) { rescanPlugins(); }
}

void Engine::removeOrphanedPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();
    std::string err;
    bool deleted = false;
    {
        // The delete only, on this thread and under a pause, as removeInstalledPlugin's is. NOTHING IS UNLOADED: the file removed is, by the
        // rule judged here, not loaded - and when it is, the rule refuses and nothing is deleted.
        StorePause hold(*this);
        deleted = cascade::core::removeOrphanedPlugin(pluginDir_, fileName, pluginHost_.plugins(), pluginInventory_.plugins, catalog_,
                                                      pluginRemoverFor(), err);
    }
    // ONE LINE, whichever way it went: the file is a third party's code or a user's own, and "what did FoxSDR delete from my plugin folder"
    // has to have an answer in the log that outlives the window.
    if (deleted) {
        cascade::core::diagLogf("plugin inventory: removed %s (not in the plugin index, not running)", fileName.c_str());
        std::string removedBuf;
        cascade::core::formatUtf8(removedBuf, tr("Removed %s"), fileName.c_str());
        installReport_ = removedBuf;
        // The folder changed, so the list is read again; a refusal changed nothing and leaves every module as it was (the window's Rescan key
        // is there for a row gone stale).
        rescanPlugins();
    } else {
        cascade::core::diagLogf("plugin inventory: did not remove %s: %s", fileName.c_str(), err.c_str());
        installError_ = err;
    }
}

// ===========================================================================
// THE TABLES (API 0.3 8.9, stub campaign B3: docs/api-0.3-additions.md 9.4)
// ===========================================================================

void Engine::buildApiListsStore(ApiListBlock& b) {
    auto fact = [&b](const std::string& key, std::string value, bool localOnly = false) {
        b.facts[key] = ApiListBlock::Fact{std::move(value), localOnly};
    };
    namespace core = cascade::core;
    // --- each entry's own: where it came from, whether the regional list offered it, and its pictures (the ones the store may ask for: a
    //     site's picture on another host is never listed - requestScreenshots would not queue it either).
    for (const core::PluginCatalogEntry& e : catalog_) {
        const std::string k = "store." + core::PluginRepo::catalogueKey(e.source, e.id) + ".";
        // The site's ADDRESS is a stranger's string the user typed: LOCAL.
        fact(k + "site", e.source, true);
        fact(k + "regional", e.regional ? "1" : "0");
        std::string pics;
        for (const core::CatalogScreenshot& s : e.screenshots) {
            if (!e.source.empty() && !core::PluginRepo::siteFilesAllowed(s.url, e.source)) { continue; }
            if (!pics.empty()) { pics += "\n"; }
            pics += s.sha256 + "\t" + oneField(s.caption);
        }
        fact(k + "pictures", std::move(pics));
        // --- THE SHOP-WINDOW FIELDS (0.99.72, stub campaign B4: the store window draws a card from these and the columns of CATALOGUE; every one
        //     present for every entry, "" when the index said nothing, so "told nothing" is told from NOT_FOUND).
        fact(k + "category", e.category);
        fact(k + "experimental", e.experimental ? "1" : "0");
        fact(k + "whatsNew", e.whatsNew);
        fact(k + "published", e.published);
        const core::PluginPlatform* here = e.thisPlatform();
        fact(k + "sha256", here != nullptr ? here->sha256 : std::string());
        // WHAT THE CATALOGUE SAYS THE BINARY DECLARES: the OR of the bits it names, or 0x80000000 when it names only capabilities this build has
        // never heard of (so the page says "declares one this build does not know", not "declares nothing"); "" when it says nothing.
        std::string caps;
        if (e.capabilities != 0u || !e.capabilityNames.empty()) {
            caps = std::to_string(e.capabilities != 0u ? e.capabilities : 0x80000000u);
        }
        fact(k + "capabilities", std::move(caps));
        // HOW IT STANDS AGAINST WHAT IS INSTALLED, by id, source and version (PluginRepo::installStateFor): none, installed, update (the catalogue's
        // is newer than the newest installed), newer (an installed copy is newer than the catalogue's); the newest installed version; and the
        // module file of that copy (which joins the entry to PLUGINS: the file the entry itself would write is not on disk for an update).
        const core::InstallState st = pluginInstallState(e);
        fact(k + "installState", st.kind == core::InstallStateKind::Installed        ? "installed"
                                 : st.kind == core::InstallStateKind::UpdateAvailable ? "update"
                                 : st.kind == core::InstallStateKind::NewerInstalled  ? "newer"
                                                                                      : "none");
        fact(k + "installedVersion", st.installedVersion);
        fact(k + "installedFile", installedModuleFile(e));
    }
    // --- the pictures themselves
    for (const auto& [sha, st] : pictures_) {
        if (st.state == PictureState::None) { continue; }
        ApiListBlock::Picture p;
        p.sha256 = sha;
        p.state = st.state == PictureState::Pending ? 1 : st.state == PictureState::Ready ? 2 : 3;
        p.path = st.path;
        p.reason = st.reason;
        p.active = screenshotPending_ && sha == screenshotInFlight_;
        b.storePictures.push_back(std::move(p));
        fact("store.picture." + sha + ".path", st.path, true);
        fact("store.picture." + sha + ".reason", st.reason);
    }
    // --- the kept copy and its freshness
    fact("store.fromCache", catalogueFromCache_ ? "1" : "0");
    fact("store.readThisSession", catalogueReadThisSession() ? "1" : "0");
    fact("store.refreshFailed", catalogueRefreshFailed_ ? "1" : "0");
    fact("store.cacheTime", std::to_string(catalogueReadTime_));
    fact("store.busyKey", installBusyId_);
    fact("store.resultKey", installResultId_);
    // --- the sites: one line each, "address TAB state TAB listed TAB hidden-same-id TAB hidden-off-host TAB error", state 0 not read, 1 the kept
    //     copy, 2 read, 3 failed. The address and the error (which can carry it) are LOCAL.
    {
        std::string lines;
        for (const SiteStatus& s : siteStatus_) {
            if (!lines.empty()) { lines += "\n"; }
            lines += oneField(s.url) + "\t" + std::to_string(static_cast<int>(s.state)) + "\t" + std::to_string(s.listed) + "\t" +
                     std::to_string(s.hiddenSameId) + "\t" + std::to_string(s.hiddenOffHost) + "\t" + oneField(s.error);
        }
        fact("store.sites", std::move(lines), true);
        fact("store.siteError", siteError_, true);
    }
    // --- the regional list's outcome: LOCAL, for the diagnostic and nothing else (the outcome itself would say which country answered)
    fact("store.regional.state", regionalState_, true);
    fact("store.regional.added", std::to_string(regionalAdded_), true);
    fact("store.regional.dropped", std::to_string(regionalDropped_), true);
    // --- the old copies the last scan found superseded, "file TAB name TAB version TAB kept file TAB kept version" - what CLEAN UP lists
    {
        std::string lines;
        for (const core::SupersededPlugin& o : pluginOldCopies_) {
            if (!lines.empty()) { lines += "\n"; }
            lines += oneField(o.file) + "\t" + oneField(o.name) + "\t" + oneField(o.version) + "\t" + oneField(o.keptFile) + "\t" +
                     oneField(o.keptVersion);
        }
        fact("store.oldCopies", std::move(lines), true);
        fact("store.cleanupReport", pluginCleanupReport_);
    }
    // --- WHERE EACH MODULE FILE CAME FROM, and whether the plugin index knows it (core::classifyPluginFiles), the verdict words the Fitted
    //     modules window names a file by: "orphan" (the index does not know it and nothing runs it: REMOVE FILE is offered), "site" (installed
    //     from a plugin site that is still listed), "removed-site" (installed from one the user has since removed: it gets no updates), "" (nothing
    //     to say). Value: the word, a TAB, and for a site the address (LOCAL: a stranger's string). One fact for EVERY file the host holds a
    //     record of, so "judged and nothing to say" is told from "no such file".
    {
        for (const core::PluginFileVerdict& v : core::classifyPluginFiles(pluginHost_.plugins(), pluginInventory_.plugins, catalog_)) {
            std::string word;
            std::string source;
            if (v.orphaned()) {
                word = "orphan";
            } else {
                for (const core::InstalledPlugin& ip : pluginInventory_.plugins) {
                    if (ip.source.empty() || !sameFileName(ip.file, v.file)) { continue; }
                    source = ip.source;
                    word = std::find(pluginSites_.begin(), pluginSites_.end(), ip.source) != pluginSites_.end() ? "site" : "removed-site";
                    break;
                }
            }
            fact("plugin.origin." + v.file, word + "\t" + oneField(source), true);
        }
    }
    // --- WHAT THE FITTED MODULES WINDOW AND THE DECODE BANK NEED OF EACH LOADED OR REFUSED MODULE THAT THE LISTS DO NOT CARRY (stub campaign
    //     B5, STORE-B2; master 0.99.72 - 0.99.77: the window used to read the install records, the catalogue and the module's own tables):
    //       fact.plugin.<file>.record   "<catalogue key> TAB <abi version> TAB <installed at, unix seconds> TAB <category>" - the install
    //                                   record of that file (the key is the id for an official plugin and "<id>@<site label>" for a site's, ""
    //                                   when the record names no id; the category is the catalogue entry's, "" when the catalogue does not
    //                                   hold it or was never read); "" when no record names the file. LOCAL (the key carries a site's host).
    //       fact.plugin.<file>.windows  one line for each window the module's own tables declare beyond a picture window (which its capability
    //                                   word says): "panel TAB <title>" and "instrument TAB <title>" - the title the module gave, which the
    //                                   window's identity carries (a plugin that is ASLEEP has no instance to read it from, and its rail row
    //                                   and its Fitted modules double-click must name the window all the same). "" for a module with none.
    //     Present for every file the host holds a record of, so "none" is told from "no such file".
    for (const core::LoadedPlugin& p : pluginHost_.plugins()) {
        const std::string file = core::pluginKey(p);
        if (file.empty()) { continue; }
        std::string record;
        for (const core::InstalledPlugin& ip : pluginInventory_.plugins) {
            if (!sameFileName(ip.file, file)) { continue; }
            const std::string key = ip.id.empty() ? std::string() : core::PluginRepo::catalogueKey(ip.source, ip.id);
            std::string category;
            if (!key.empty()) {
                for (const core::PluginCatalogEntry& ce : catalog_) {
                    if (core::PluginRepo::catalogueKey(ce.source, ce.id) == key) {
                        category = ce.category;
                        break;
                    }
                }
            }
            record = oneField(key) + "\t" + std::to_string(ip.abiVersion) + "\t" + std::to_string(ip.installedAtUnix) + "\t" + oneField(category);
            break;
        }
        fact("plugin." + file + ".record", std::move(record), true);
        std::string windows;
        if (p.loaded) {
            if (p.panel != nullptr) {
                windows += "panel\t" + oneField(p.panel->title != nullptr ? std::string(p.panel->title) : p.name);
            }
            if (p.instrument != nullptr) {
                if (!windows.empty()) { windows += "\n"; }
                windows += "instrument\t" + oneField(p.instrument->title != nullptr ? std::string(p.instrument->title) : p.name);
            }
        }
        fact("plugin." + file + ".windows", std::move(windows));
    }
}

}  // namespace cascade::engine
