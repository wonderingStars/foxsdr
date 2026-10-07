// plugin_cleanup.hpp - removing the old copies of a plugin that updates leave
// behind (0.99.49 beta feedback).
//
// WHY THERE ARE OLD COPIES AT ALL. A plugin's file name carries its version
// ("adsb-1.0.0.dll"), so installing 1.1.0 from the store ADDS a file instead
// of replacing one. The next scan loads both, resolveDuplicatePlugins
// (core/plugin_host.hpp) keeps the newer and turns the older OFF - "Ignored:
// another copy of ... is installed" - and the old file stays on disk, inert,
// for the user to remove one Remove key and one confirmation at a time. The
// tester: "Having to go through and manually delete multiple updated plugins
// is tedious, especially with needing to confirm the deletion on each one."
//
// So: an update removes the copies it superseded as soon as the new one is
// loaded, and the store has one key that removes every superseded copy at
// once. THE RULES, all in supersededPlugins() and nowhere else:
//
//   - only a copy the host itself turned off as ANOTHER COPY of a loaded
//     plugin is a candidate (LoadedPlugin::supersededBy): a file that failed
//     to load, or that is the only copy, is never touched;
//   - the copy that stays must be LOADED and STRICTLY NEWER (the host's own
//     comparator, PluginRepo::compareVersions) - never the loaded copy, never
//     the newest, never one of two copies of the same version;
//   - the copy that stays must be a CATALOGUE COPY - recorded in the store's
//     manifest (installed.json). A side-loaded plugin with no newer catalogue
//     copy is the user's own business and is left alone, however many older
//     side-loaded copies sit beside it;
//   - the file to go must NOT be one the manifest records: a managed file is
//     removed only by the Remove key, which says what it removes.
//
// A FILE THAT CANNOT BE DELETED NOW is queued: Windows will not delete a
// module that is mapped, in this process or in another copy of FoxSDR. Its
// name goes into pending-removal.json beside the manifest, and the next start
// deletes it - but only if that start's own scan still finds it superseded by
// the same rules, so a queue entry can never delete a file that has since
// become the only or the newest copy.
//
// ORPHANED FILES (0.99.69) are the other thing a plugin folder collects, and
// the rules above never touch them: a module file that the plugin index does
// not know (no install record, and not a file the catalogue publishes for this
// platform) AND that is not running (refused at load, or turned off as another
// copy). Nothing removes it, and nothing said so - the inventory's own note is
// "is installed but not recorded; it is left alone", in the diagnostics log. The
// Fitted modules window now says so on the file's row and plate and offers
// REMOVE FILE, which deletes that one file after a confirmation. THE RULE IS IN
// classifyPluginFiles() AND IS RE-APPLIED AT THE MOMENT OF REMOVAL
// (removeOrphanedPlugin): a file the index knows, or that is loaded, is never an
// orphan, whatever the window showed a moment ago.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"

namespace cascade::core {

// One old copy that may go, and the copy that stays instead.
struct SupersededPlugin {
    std::string file;         // the old copy's bare module file name
    std::string name;         // the plugin's declared id (the descriptor name)
    std::string version;      // the old copy's version
    std::string keptFile;     // the copy that stays: loaded, newer, from the catalogue
    std::string keptVersion;
};

// Every old copy the rules above allow to be removed, in scan order. Pure:
// `records` is one scan's PluginHost::plugins(), `manifest` the store's
// install records.
std::vector<SupersededPlugin> supersededPlugins(const std::vector<LoadedPlugin>& records,
                                                const std::vector<InstalledPlugin>& manifest);

// Only the old copies `keptFile` replaced - what an update of that one plugin
// cleans up, and nothing belonging to any other plugin.
std::vector<SupersededPlugin> supersededBy(const std::vector<SupersededPlugin>& all,
                                           const std::string& keptFile);

// Deletes one module file from the plugins directory. The default is
// PluginRepo::remove: the file name goes through the install sanitiser, so
// nothing outside the directory can be named, and any manifest record for it
// is dropped with it. A test hands in its own to play a locked file.
using PluginFileRemover =
    std::function<bool(const std::string& pluginsDir, const std::string& file, std::string& error)>;
PluginFileRemover defaultPluginFileRemover();

struct PluginCleanupResult {
    std::vector<std::string> removed;  // deleted now
    std::vector<std::string> queued;   // could not be deleted now: the next start will
    std::vector<std::string> failed;   // "file: reason" - neither deleted nor queued
    std::vector<std::string> dropped;  // queue entries let go untouched (no longer superseded)
};

// Deletes each file in `which`; one that cannot be deleted is queued for the
// next start. A file deleted is also taken off the queue.
PluginCleanupResult removeSupersededPlugins(const std::string& pluginsDir,
                                            const std::vector<SupersededPlugin>& which,
                                            const PluginFileRemover& remover);

// What one module file in the plugins folder is, as far as removal is
// concerned. The file is the scan's own record of it (PluginHost::plugins()).
struct PluginFileVerdict {
    std::string file;      // the module's bare file name (pluginKey)
    bool indexed = false;  // an install record names it, or the catalogue publishes it for this platform
    bool loaded = false;   // the host has it mapped and running
    // The host's own words for why it is not running (LoadedPlugin::error),
    // verbatim; empty when it gave none, and for a file that is loaded.
    std::string detail;

    // NOT KNOWN TO THE INDEX AND NOT RUNNING: the only kind of file that may be
    // offered for removal. Either flag alone protects a file.
    bool orphaned() const { return !indexed && !loaded; }
};

// One verdict per record, in scan order. Pure: `records` is one scan's
// PluginHost::plugins(), `manifest` the store's install records, `catalogue`
// the catalogue as last fetched (empty when it never was: then only the
// install records speak for the index, and a file the catalogue would have
// named is judged on being loaded alone). A record with no file name is left out.
std::vector<PluginFileVerdict> classifyPluginFiles(const std::vector<LoadedPlugin>& records,
                                                   const std::vector<InstalledPlugin>& manifest,
                                                   const std::vector<PluginCatalogEntry>& catalogue);

// Deletes the ONE module file `file` if, judged now against the same inputs,
// it is an orphan, and says why not otherwise: it is loaded, or the index knows
// it, or the last scan found no such module. Returns whether it was deleted;
// `error` holds the reason when not, in English like PluginRepo's own. The name
// handed to `remover` is the scan's spelling of it, never the caller's.
bool removeOrphanedPlugin(const std::string& pluginsDir, const std::string& file,
                          const std::vector<LoadedPlugin>& records,
                          const std::vector<InstalledPlugin>& manifest,
                          const std::vector<PluginCatalogEntry>& catalogue,
                          const PluginFileRemover& remover, std::string& error);

// THE QUEUE, pending-removal.json in the plugins directory. PluginHost loads
// only module files, so it is never mistaken for a plugin. A name the install
// sanitiser refuses is never read back from it.
const char* pendingRemovalFileName();
std::vector<std::string> loadPendingRemovals(const std::string& pluginsDir);
// An empty list deletes the file. Written by the same temp-file-and-rename
// route as the manifest.
bool savePendingRemovals(const std::string& pluginsDir, const std::vector<std::string>& files,
                         std::string& error);

// AT START, after the first scan: each queued file that this scan still finds
// superseded is deleted (or stays queued if it is still locked); every other
// queued name is dropped from the queue WITHOUT being touched.
PluginCleanupResult processPendingRemovals(const std::string& pluginsDir,
                                           const std::vector<LoadedPlugin>& records,
                                           const std::vector<InstalledPlugin>& manifest,
                                           const PluginFileRemover& remover);

}  // namespace cascade::core
