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

    const int rc = testSummary("test_plugin_cleanup");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);
    }
    return rc;
}
