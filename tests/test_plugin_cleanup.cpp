// Tests for core/plugin_cleanup.hpp - removing the old copies of a plugin that
// store updates leave behind (0.99.49 beta feedback: "Having to go through and
// manually delete multiple updated plugins is tedious, especially with needing
// to confirm the deletion on each one").
//
// Every rule, on real files in a scratch plugins directory:
//   [1] the host marks the copy it turned off, and which copy runs instead;
//   [2] an update's old copy goes, and the new one - loaded, newest, recorded
//       in the manifest - stays;
//   [3] TWO old copies of one plugin both go;
//   [4] a SIDE-LOADED plugin with no newer catalogue copy is never touched,
//       nor its older side-loaded copies, nor an old catalogue copy beside a
//       newer side-loaded one;
//   [5] never the loaded copy, never a copy of the same version, never a file
//       that failed to load, never a file the manifest records;
//   [6] a LOCKED file (the remover refuses, as Windows does for a mapped
//       module) is queued, and the next start deletes it - but only if that
//       start still finds it superseded; a queued name that has since become
//       the newest copy is let go untouched;
//   [7] one update cleans up its own plugin and nobody else's;
//   [8] the queue never names anything outside the plugins directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_cleanup.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::InstalledPlugin;
using cascade::core::LoadedPlugin;
using cascade::core::SupersededPlugin;

namespace {

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

std::string mod(const std::string& stem) { return stem + kExt; }

fs::path g_root;

// A fresh plugins directory holding `files`, each a few bytes.
fs::path freshDir(const std::string& tag, const std::vector<std::string>& files) {
    const fs::path d = g_root / tag;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    for (const std::string& f : files) {
        std::ofstream o(d / f, std::ios::binary);
        o << "module " << f;
    }
    return d;
}

bool exists(const fs::path& dir, const std::string& f) { return fs::exists(dir / f); }

// One scan's record for a file: loaded or turned off in favour of another.
LoadedPlugin rec(const fs::path& dir, const std::string& file, const std::string& name,
                 const std::string& version, bool loaded, const std::string& supersededBy = "") {
    LoadedPlugin p;
    p.path = (dir / file).string();
    p.name = name;
    p.version = version;
    p.loaded = loaded;
    p.supersededBy = supersededBy;
    if (!loaded) { p.error = "Ignored: another copy"; }
    return p;
}

InstalledPlugin installed(const std::string& id, const std::string& file, const std::string& version) {
    InstalledPlugin r;
    r.id = id;
    r.name = id;
    r.file = file;
    r.version = version;
    return r;
}

std::set<std::string> files(const std::vector<SupersededPlugin>& v) {
    std::set<std::string> out;
    for (const SupersededPlugin& s : v) { out.insert(s.file); }
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Plays a module Windows will not let go of: refuses the named files.
cascade::core::PluginFileRemover lockedRemover(std::set<std::string> locked) {
    return [locked](const std::string& dir, const std::string& file, std::string& error) {
        if (locked.count(file) != 0) {
            error = "the file is in use";
            return false;
        }
        return cascade::core::defaultPluginFileRemover()(dir, file, error);
    };
}

}  // namespace

int main() {
    std::printf("test_plugin_cleanup\n");
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_root = fs::temp_directory_path() / ("foxsdr_plugin_cleanup_" + std::to_string(pid));
    const auto remover = cascade::core::defaultPluginFileRemover();

    // --- [1] the host says which copy it turned off, and for which --------------
    {
        std::vector<LoadedPlugin> r;
        LoadedPlugin a;
        a.path = "/p/" + mod("adsb-1.0.0");
        a.name = "ADS-B";
        a.version = "1.0.0";
        a.loaded = true;
        LoadedPlugin b = a;
        b.path = "/p/" + mod("adsb-1.1.0");
        b.version = "1.1.0";
        r = {a, b};
        CHECK(cascade::core::resolveDuplicatePlugins(r) == 1u);
        CHECK(!r[0].loaded && r[0].supersededBy == mod("adsb-1.1.0"));
        CHECK(r[1].loaded && r[1].supersededBy.empty());
    }

    // --- [2] an update's old copy goes; the new one stays --------------------------
    {
        const fs::path d = freshDir("update", {mod("adsb-1.0.0"), mod("adsb-1.1.0")});
        const std::vector<LoadedPlugin> scan = {
            rec(d, mod("adsb-1.0.0"), "ADS-B", "1.0.0", false, mod("adsb-1.1.0")),
            rec(d, mod("adsb-1.1.0"), "ADS-B", "1.1.0", true)};
        const std::vector<InstalledPlugin> manifest = {installed("adsb", mod("adsb-1.1.0"), "1.1.0")};
        const auto s = cascade::core::supersededPlugins(scan, manifest);
        CHECK(s.size() == 1u);
        CHECK(!s.empty() && s[0].file == mod("adsb-1.0.0") && s[0].keptFile == mod("adsb-1.1.0"));
        CHECK(!s.empty() && s[0].version == "1.0.0" && s[0].keptVersion == "1.1.0");
        const auto res = cascade::core::removeSupersededPlugins(d.string(), s, remover);
        CHECK(res.removed.size() == 1u && contains(res.removed, mod("adsb-1.0.0")));
        CHECK(res.queued.empty() && res.failed.empty());
        CHECK(!exists(d, mod("adsb-1.0.0")));
        CHECK(exists(d, mod("adsb-1.1.0")));  // THE LOADED, NEWEST COPY STAYS
    }

    // --- [3] two old copies of one plugin both go -----------------------------------
    {
        const fs::path d =
            freshDir("twoold", {mod("apt-1.0.0"), mod("apt-1.0.5"), mod("apt-1.2.0")});
        const std::vector<LoadedPlugin> scan = {
            rec(d, mod("apt-1.0.0"), "NOAA APT", "1.0.0", false, mod("apt-1.2.0")),
            rec(d, mod("apt-1.0.5"), "NOAA APT", "1.0.5", false, mod("apt-1.2.0")),
            rec(d, mod("apt-1.2.0"), "NOAA APT", "1.2.0", true)};
        const std::vector<InstalledPlugin> manifest = {installed("apt", mod("apt-1.2.0"), "1.2.0")};
        const auto s = cascade::core::supersededPlugins(scan, manifest);
        CHECK(files(s) == (std::set<std::string>{mod("apt-1.0.0"), mod("apt-1.0.5")}));
        const auto res = cascade::core::removeSupersededPlugins(d.string(), s, remover);
        CHECK(res.removed.size() == 2u);
        CHECK(!exists(d, mod("apt-1.0.0")) && !exists(d, mod("apt-1.0.5")));
        CHECK(exists(d, mod("apt-1.2.0")));
    }

    // --- [4] side-loaded plugins ------------------------------------------------------
    {
        const fs::path d = freshDir("sideload", {mod("mine-2.0.0"), mod("mine-1.0.0"),
                                                 mod("solo"), mod("x-1.0.0"), mod("x-9.0.0")});
        const std::vector<LoadedPlugin> scan = {
            // A side-loaded plugin and an OLDER side-loaded copy of it: the
            // newer is loaded, but no catalogue ever supplied it.
            rec(d, mod("mine-1.0.0"), "Mine", "1.0.0", false, mod("mine-2.0.0")),
            rec(d, mod("mine-2.0.0"), "Mine", "2.0.0", true),
            // A side-loaded plugin on its own.
            rec(d, mod("solo"), "Solo", "0.1", true),
            // An old CATALOGUE copy beside a newer SIDE-LOADED one: the one
            // that runs is not a catalogue copy, so nothing is decided for the
            // user either way.
            rec(d, mod("x-1.0.0"), "X", "1.0.0", false, mod("x-9.0.0")),
            rec(d, mod("x-9.0.0"), "X", "9.0.0", true)};
        const std::vector<InstalledPlugin> manifest = {installed("x", mod("x-1.0.0"), "1.0.0")};
        CHECK(cascade::core::supersededPlugins(scan, manifest).empty());
        CHECK(cascade::core::supersededPlugins(scan, {}).empty());
    }

    // --- [5] never the loaded copy, a same-version copy, a refused file, a recorded file
    {
        const fs::path d = freshDir("never", {mod("a-1.0.0"), mod("a-1.0.0b"), mod("b-1.0.0"),
                                              mod("b-2.0.0"), mod("c-1.0.0"), mod("c-2.0.0")});
        std::vector<LoadedPlugin> scan = {
            // Two copies of ONE VERSION: the host kept the first; neither is
            // "newest", so neither goes.
            rec(d, mod("a-1.0.0"), "A", "1.0.0", true),
            rec(d, mod("a-1.0.0b"), "A", "1.0.0", false, mod("a-1.0.0")),
            // A file that failed to load: no descriptor, no id, not a copy of
            // anything.
            rec(d, mod("b-1.0.0"), "", "", false),
            rec(d, mod("b-2.0.0"), "B", "2.0.0", true),
            // The old copy is itself recorded by the manifest (a hand-edited
            // or half-written one): only the Remove key removes a managed file.
            rec(d, mod("c-1.0.0"), "C", "1.0.0", false, mod("c-2.0.0")),
            rec(d, mod("c-2.0.0"), "C", "2.0.0", true)};
        scan[2].error = "not a plugin";
        const std::vector<InstalledPlugin> manifest = {
            installed("a", mod("a-1.0.0"), "1.0.0"), installed("b", mod("b-2.0.0"), "2.0.0"),
            installed("c", mod("c-2.0.0"), "2.0.0"), installed("c-old", mod("c-1.0.0"), "1.0.0")};
        CHECK(cascade::core::supersededPlugins(scan, manifest).empty());
        // And a copy whose "kept" copy is not loaded after all (a record from a
        // scan that went wrong) is not removed either.
        std::vector<LoadedPlugin> gone = {
            rec(d, mod("b-1.0.0"), "B", "1.0.0", false, mod("b-2.0.0")),
            rec(d, mod("b-2.0.0"), "B", "2.0.0", false)};
        CHECK(cascade::core::supersededPlugins(gone, manifest).empty());
        // Nothing in the directory was touched by any of that.
        for (const char* f : {"a-1.0.0", "a-1.0.0b", "b-1.0.0", "b-2.0.0", "c-1.0.0", "c-2.0.0"}) {
            CHECK(exists(d, mod(f)));
        }
    }

    // --- [6] a locked file is queued, and the next start deletes it ------------------
    {
        const fs::path d = freshDir("locked", {mod("sat-1.0.0"), mod("sat-1.1.0")});
        const std::vector<LoadedPlugin> scan = {
            rec(d, mod("sat-1.0.0"), "Satellites", "1.0.0", false, mod("sat-1.1.0")),
            rec(d, mod("sat-1.1.0"), "Satellites", "1.1.0", true)};
        const std::vector<InstalledPlugin> manifest = {
            installed("sat", mod("sat-1.1.0"), "1.1.0")};
        const auto s = cascade::core::supersededPlugins(scan, manifest);
        CHECK(s.size() == 1u);
        const auto res =
            cascade::core::removeSupersededPlugins(d.string(), s, lockedRemover({mod("sat-1.0.0")}));
        CHECK(res.removed.empty());
        CHECK(res.queued.size() == 1u && contains(res.queued, mod("sat-1.0.0")));
        CHECK(exists(d, mod("sat-1.0.0")));  // still there, and said so
        CHECK(exists(d, cascade::core::pendingRemovalFileName()));
        CHECK(cascade::core::loadPendingRemovals(d.string()) ==
              std::vector<std::string>{mod("sat-1.0.0")});
        // Queued twice is queued once.
        (void)cascade::core::removeSupersededPlugins(d.string(), s, lockedRemover({mod("sat-1.0.0")}));
        CHECK(cascade::core::loadPendingRemovals(d.string()).size() == 1u);

        // THE NEXT START, and it is STILL locked: stays queued.
        auto next = cascade::core::processPendingRemovals(d.string(), scan, manifest,
                                                          lockedRemover({mod("sat-1.0.0")}));
        CHECK(next.removed.empty() && next.queued.size() == 1u);
        CHECK(cascade::core::loadPendingRemovals(d.string()).size() == 1u);

        // The next start after that, with nothing holding it: deleted, and the
        // queue is gone with it.
        next = cascade::core::processPendingRemovals(d.string(), scan, manifest, remover);
        CHECK(next.removed.size() == 1u && contains(next.removed, mod("sat-1.0.0")));
        CHECK(!exists(d, mod("sat-1.0.0")));
        CHECK(exists(d, mod("sat-1.1.0")));
        CHECK(cascade::core::loadPendingRemovals(d.string()).empty());
        CHECK(!exists(d, cascade::core::pendingRemovalFileName()));
    }
    // ...and a queued name that is NO LONGER superseded is let go untouched: the
    // user deleted the new copy, so the old one is now the one that runs.
    {
        const fs::path d = freshDir("requeue", {mod("pager-1.0.0")});
        std::string err;
        CHECK(cascade::core::savePendingRemovals(d.string(), {mod("pager-1.0.0")}, err));
        const std::vector<LoadedPlugin> scan = {rec(d, mod("pager-1.0.0"), "Pager", "1.0.0", true)};
        const auto next = cascade::core::processPendingRemovals(d.string(), scan, {}, remover);
        CHECK(next.removed.empty() && next.queued.empty());
        CHECK(next.dropped.size() == 1u && contains(next.dropped, mod("pager-1.0.0")));
        CHECK(exists(d, mod("pager-1.0.0")));
        CHECK(cascade::core::loadPendingRemovals(d.string()).empty());
    }

    // --- [7] an update cleans up its own plugin and nobody else's ----------------------
    {
        const std::vector<SupersededPlugin> all = {
            {mod("adsb-1.0.0"), "ADS-B", "1.0.0", mod("adsb-1.1.0"), "1.1.0"},
            {mod("apt-1.0.0"), "NOAA APT", "1.0.0", mod("apt-1.2.0"), "1.2.0"},
            {mod("adsb-0.9.0"), "ADS-B", "0.9.0", mod("adsb-1.1.0"), "1.1.0"}};
        const auto mine = cascade::core::supersededBy(all, mod("adsb-1.1.0"));
        CHECK(files(mine) == (std::set<std::string>{mod("adsb-1.0.0"), mod("adsb-0.9.0")}));
        CHECK(cascade::core::supersededBy(all, mod("nothing")).empty());
    }

    // --- [8] the queue never names anything outside the directory ----------------------
    {
        const fs::path d = freshDir("hostile", {});
        {
            std::ofstream q(d / cascade::core::pendingRemovalFileName(), std::ios::binary);
            q << "{\"schemaVersion\": 1, \"files\": [\"../escape" << kExt << "\", \"C:\\\\x" << kExt
              << "\", \"notes.txt\", \"" << mod("ok-1.0.0") << "\", 7]}";
        }
        const auto q = cascade::core::loadPendingRemovals(d.string());
        CHECK(q == std::vector<std::string>{mod("ok-1.0.0")});
        // A queue that is not JSON at all is an empty queue.
        {
            std::ofstream bad(d / cascade::core::pendingRemovalFileName(), std::ios::binary);
            bad << "not json";
        }
        CHECK(cascade::core::loadPendingRemovals(d.string()).empty());
    }

    // --- [9] ORPHANED FILES (0.99.69): the classification --------------------------------
    //
    // A module file is an orphan when the index does not know it AND it is not
    // running. Either alone protects it. The index knows a file by an install
    // record (any record, whatever the inventory found of its file) or by the
    // catalogue publishing that file name for THIS platform.
    const std::string kStrayError = "plugin reports ABI version 2, expected 3";
    const auto catalogueNaming = [](const std::string& file, bool thisPlatform) {
        cascade::core::PluginCatalogEntry e;
        e.id = "cat";
        e.name = "Catalogue entry";
        cascade::core::PluginPlatform p;
        p.os = thisPlatform ? cascade::core::PluginRepo::hostOs() : "plan9";
        p.arch = thisPlatform ? cascade::core::PluginRepo::hostArch() : "mips";
        p.file = file;
        e.platforms.push_back(p);
        return e;
    };
    const auto refused = [&](const fs::path& dir, const std::string& file, const std::string& error) {
        LoadedPlugin p = rec(dir, file, "", "", false);
        p.error = error;
        return p;
    };
    // The folder the table's files live in. Names are the scan's.
    const fs::path od = freshDir("orphans", {mod("managed"), mod("running"), mod("both"),
                                             mod("stray-bad"), mod("stray-old"), mod("catalogued"),
                                             mod("Mixed-Case"), mod("elsewhere"), mod("lost-record"),
                                             mod("never-scanned")});
    std::vector<LoadedPlugin> orecs = {
        refused(od, mod("managed"), "refused: no entry point"),   // recorded, not running
        rec(od, mod("running"), "Running", "1.0.0", true),         // running, unrecorded
        rec(od, mod("both"), "Both", "1.0.0", true),               // running and recorded
        refused(od, mod("stray-bad"), kStrayError),                // neither: an orphan
        rec(od, mod("stray-old"), "Old", "0.9.0", false, mod("stray-new")),   // an old side-loaded copy
        refused(od, mod("catalogued"), "refused: no entry point"), // the catalogue names it
        refused(od, mod("mixed-case"), "refused: no entry point"), // the record spells it otherwise
        refused(od, mod("elsewhere"), ""),                         // another platform's file name: an orphan
        refused(od, mod("lost-record"), "refused: no entry point"),
    };
    LoadedPlugin pathless;   // a record with no file name has nothing to be an orphan of
    orecs.push_back(pathless);
    InstalledPlugin lost = installed("lost", mod("lost-record"), "1.0.0");
    lost.missingFromDisk = true;   // the inventory could not find it; the name is still the index's
    const std::vector<InstalledPlugin> omanifest = {
        installed("managed", mod("managed"), "1.0.0"), installed("both", mod("both"), "1.0.0"),
        installed("mixed", mod("MIXED-CASE"), "1.0.0"), lost};
    const std::vector<cascade::core::PluginCatalogEntry> ocatalogue = {
        catalogueNaming(mod("catalogued"), true), catalogueNaming(mod("elsewhere"), false)};

    {
        const auto v = cascade::core::classifyPluginFiles(orecs, omanifest, ocatalogue);
        CHECK(v.size() == 9u);   // the pathless record is not a file
        const auto find = [&](const std::string& f) -> const cascade::core::PluginFileVerdict* {
            for (const auto& x : v) {
                if (x.file == f) { return &x; }
            }
            return nullptr;
        };
        struct Want { const char* stem; bool indexed; bool loaded; bool orphaned; };
        const Want table[] = {
            {"managed", true, false, false},      {"running", false, true, false},
            {"both", true, true, false},          {"stray-bad", false, false, true},
            {"stray-old", false, false, true},    {"catalogued", true, false, false},
            {"mixed-case", true, false, false},   {"elsewhere", false, false, true},
            {"lost-record", true, false, false},
        };
        for (const Want& w : table) {
            const auto* x = find(mod(w.stem));
            CHECK(x != nullptr);
            if (x == nullptr) { continue; }
            if (x->indexed != w.indexed || x->loaded != w.loaded || x->orphaned() != w.orphaned) {
                std::printf("  %s: indexed %d loaded %d orphaned %d\n", w.stem, x->indexed, x->loaded,
                            x->orphaned());
            }
            CHECK(x->indexed == w.indexed);
            CHECK(x->loaded == w.loaded);
            CHECK(x->orphaned() == w.orphaned);
        }
        // The host's own words come through verbatim for an orphan, and a running
        // file has none.
        const auto* bad = find(mod("stray-bad"));
        CHECK(bad != nullptr && bad->detail == kStrayError);
        const auto* old = find(mod("stray-old"));
        CHECK(old != nullptr && old->detail == "Ignored: another copy");
        const auto* run = find(mod("running"));
        CHECK(run != nullptr && run->detail.empty());
        // Nothing known at all (no manifest, no catalogue fetched yet): the running
        // file is still protected, and every refused one is an orphan.
        const auto bare = cascade::core::classifyPluginFiles(orecs, {}, {});
        int orphans = 0;
        for (const auto& x : bare) { orphans += x.orphaned() ? 1 : 0; }
        CHECK(orphans == 7);   // all nine files bar the two that are running
    }

    // --- [10] ...and the removal, on real files -------------------------------------------
    const auto remains = [&](const std::string& stem) { return exists(od, mod(stem)); };
    {
        std::string err;
        // The orphan goes, and ONLY it.
        CHECK(cascade::core::removeOrphanedPlugin(od.string(), mod("stray-bad"), orecs, omanifest,
                                                  ocatalogue, remover, err));
        CHECK(err.empty());
        CHECK(!remains("stray-bad"));
        for (const char* other : {"managed", "running", "both", "stray-old", "catalogued", "Mixed-Case",
                                  "elsewhere", "lost-record", "never-scanned"}) {
            CHECK(remains(other));
        }
        // The same file again: it is gone and the scan record is stale, and the
        // answer is the filesystem's, not a pretence.
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("stray-bad"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(!err.empty());

        // A file the index knows, a file that is running, a file the catalogue names:
        // refused with the reason, and the file is still there.
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("managed"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(err.find("plugin index") != std::string::npos);
        CHECK(remains("managed"));
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("running"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(err.find("running") != std::string::npos);
        CHECK(remains("running"));
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("both"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(remains("both"));
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("catalogued"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(err.find("plugin index") != std::string::npos);
        CHECK(remains("catalogued"));
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("lost-record"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(remains("lost-record"));

        // A file on disk the last scan did not find is not offered either, and a name
        // that tries to leave the folder names nothing: the file outside survives.
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("never-scanned"), orecs, omanifest,
                                                   ocatalogue, remover, err));
        CHECK(err.find("last scan") != std::string::npos);
        CHECK(remains("never-scanned"));
        const fs::path outside = od.parent_path() / mod("outside-the-folder");
        { std::ofstream o(outside, std::ios::binary); o << "x"; }
        for (const std::string hostile : {std::string("../") + mod("outside-the-folder"), std::string(""),
                                          std::string("..")}) {
            err.clear();
            CHECK(!cascade::core::removeOrphanedPlugin(od.string(), hostile, orecs, omanifest, ocatalogue,
                                                       remover, err));
            CHECK(!err.empty());
        }
        CHECK(fs::exists(outside));

        // A locked orphan (the remover refuses) is reported, not queued and not
        // pretended: the file is there and so is the reason.
        err.clear();
        CHECK(!cascade::core::removeOrphanedPlugin(od.string(), mod("elsewhere"), orecs, omanifest,
                                                   ocatalogue, lockedRemover({mod("elsewhere")}), err));
        CHECK(err == "the file is in use");
        CHECK(remains("elsewhere"));
        CHECK(cascade::core::loadPendingRemovals(od.string()).empty());

        // The remover is handed the SCAN'S spelling of the name, not the caller's, and
        // the name's case does not stop it matching.
        std::string handed;
        const cascade::core::PluginFileRemover spy = [&](const std::string&, const std::string& f,
                                                         std::string&) {
            handed = f;
            return true;
        };
        err.clear();
        CHECK(cascade::core::removeOrphanedPlugin(od.string(), mod("ELSEWHERE"), orecs, omanifest, {},
                                                  spy, err));
        CHECK(handed == mod("elsewhere"));
    }

    const int rc = testSummary("test_plugin_cleanup");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);
    }
    return rc;
}
