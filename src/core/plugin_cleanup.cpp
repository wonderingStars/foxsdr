// plugin_cleanup.cpp - see plugin_cleanup.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_cleanup.hpp"

#include "core/file_read.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core {

namespace {

// NTFS and APFS compare names without case, so the manifest's record of a
// file and the scan's name for it may differ in case alone.
bool sameFile(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
        if (x != y) { return false; }
    }
    return true;
}

bool recorded(const std::vector<InstalledPlugin>& manifest, const std::string& file) {
    return std::any_of(manifest.begin(), manifest.end(), [&](const InstalledPlugin& r) {
        return !r.missingFromDisk && sameFile(r.file, file);
    });
}

void addUnique(std::vector<std::string>& v, const std::string& s) {
    for (const std::string& e : v) {
        if (sameFile(e, s)) { return; }
    }
    v.push_back(s);
}

void removeName(std::vector<std::string>& v, const std::string& s) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& e) { return sameFile(e, s); }),
            v.end());
}

}  // namespace

std::vector<SupersededPlugin> supersededPlugins(const std::vector<LoadedPlugin>& records,
                                                const std::vector<InstalledPlugin>& manifest) {
    std::vector<SupersededPlugin> out;
    for (const LoadedPlugin& old : records) {
        // Only what the host itself turned off as another copy of a plugin.
        if (old.loaded || old.name.empty() || old.supersededBy.empty()) { continue; }
        const LoadedPlugin* kept = nullptr;
        for (const LoadedPlugin& r : records) {
            if (r.loaded && r.name == old.name && sameFile(pluginKey(r), old.supersededBy)) {
                kept = &r;
                break;
            }
        }
        // The copy that stays is loaded, STRICTLY newer, and from the catalogue.
        if (kept == nullptr) { continue; }
        if (PluginRepo::compareVersions(kept->version, old.version) <= 0) { continue; }
        const std::string keptFile = pluginKey(*kept);
        if (!recorded(manifest, keptFile)) { continue; }
        // And the file to go is not one the store manages.
        const std::string file = pluginKey(old);
        if (recorded(manifest, file)) { continue; }
        out.push_back({file, old.name, old.version, keptFile, kept->version});
    }
    return out;
}

std::vector<SupersededPlugin> supersededBy(const std::vector<SupersededPlugin>& all,
                                           const std::string& keptFile) {
    std::vector<SupersededPlugin> out;
    for (const SupersededPlugin& s : all) {
        if (sameFile(s.keptFile, keptFile)) { out.push_back(s); }
    }
    return out;
}

PluginFileRemover defaultPluginFileRemover() {
    return [](const std::string& pluginsDir, const std::string& file, std::string& error) {
        // PluginRepo::remove keeps no state of its own; the instance is only
        // the way to reach it.
        PluginRepo repo;
        return repo.remove(pluginsDir, file, error);
    };
}

const char* pendingRemovalFileName() { return "pending-removal.json"; }

std::vector<std::string> loadPendingRemovals(const std::string& pluginsDir) {
    std::vector<std::string> out;
    // Through readTextFile (core/file_read.hpp, 0.99.65): a folder squatting on this name, or a read
    // that fails, is "no queue" - the stream read this replaced threw on Linux and ended the program.
    std::string text;
    if (readTextFile(fs::path(pluginsDir) / pendingRemovalFileName(), text) != ReadResult::Ok) {
        return out;
    }
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("files") || !j["files"].is_array()) { return out; }
    for (const json& f : j["files"]) {
        if (!f.is_string()) { continue; }
        // ONLY A NAME THE INSTALL SANITISER ACCEPTS: a bare module file name,
        // no directory, no traversal - whatever else the file says.
        std::string safe;
        std::string err;
        if (!PluginRepo::sanitiseFileName(f.get<std::string>(), safe, err)) { continue; }
        addUnique(out, safe);
    }
    return out;
}

bool savePendingRemovals(const std::string& pluginsDir, const std::vector<std::string>& files,
                         std::string& error) {
    error.clear();
    const fs::path target = fs::path(pluginsDir) / pendingRemovalFileName();
    std::error_code ec;
    if (files.empty()) {
        fs::remove(target, ec);
        return true;
    }
    json j;
    j["schemaVersion"] = 1;
    j["files"] = files;
    const std::string text = j.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
    // The manifest's route: a temp file beside it, then one rename, so a
    // crash leaves the old queue or the new one and never half of either.
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write \"" + tmp.string() + "\"";
            return false;
        }
        f << text;
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            error = "cannot write \"" + tmp.string() + "\"";
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "cannot replace \"" + target.string() + "\": " + ec.message();
        return false;
    }
    return true;
}

PluginCleanupResult removeSupersededPlugins(const std::string& pluginsDir,
                                            const std::vector<SupersededPlugin>& which,
                                            const PluginFileRemover& remover) {
    PluginCleanupResult res;
    std::vector<std::string> queue = loadPendingRemovals(pluginsDir);
    const std::vector<std::string> queueBefore = queue;
    for (const SupersededPlugin& s : which) {
        std::string err;
        if (remover(pluginsDir, s.file, err)) {
            res.removed.push_back(s.file);
            removeName(queue, s.file);
        } else if (std::error_code ec; fs::exists(fs::path(pluginsDir) / s.file, ec)) {
            // STILL THERE: in use. The next start takes it.
            addUnique(queue, s.file);
            res.queued.push_back(s.file);
        } else {
            res.failed.push_back(s.file + ": " + err);
        }
    }
    if (queue != queueBefore) {
        std::string err;
        if (!savePendingRemovals(pluginsDir, queue, err)) {
            // Not queued after all: say so rather than promise the next start.
            for (const std::string& q : res.queued) { res.failed.push_back(q + ": " + err); }
            res.queued.clear();
        }
    }
    return res;
}

PluginCleanupResult processPendingRemovals(const std::string& pluginsDir,
                                           const std::vector<LoadedPlugin>& records,
                                           const std::vector<InstalledPlugin>& manifest,
                                           const PluginFileRemover& remover) {
    PluginCleanupResult res;
    const std::vector<std::string> queue = loadPendingRemovals(pluginsDir);
    if (queue.empty()) {
        // A queue file that held nothing usable goes too.
        std::string err;
        (void)savePendingRemovals(pluginsDir, {}, err);
        return res;
    }
    // THE SAME RULES AS EVER, against THIS start's scan: a queue entry is a
    // reminder, never an instruction.
    const std::vector<SupersededPlugin> now = supersededPlugins(records, manifest);
    std::vector<SupersededPlugin> still;
    for (const std::string& q : queue) {
        bool found = false;
        for (const SupersededPlugin& s : now) {
            if (sameFile(s.file, q)) {
                still.push_back(s);
                found = true;
                break;
            }
        }
        if (!found) { res.dropped.push_back(q); }
    }
    // Let go of what is no longer superseded before deleting anything, so the
    // queue written below holds only what is still locked.
    {
        std::vector<std::string> keep;
        for (const SupersededPlugin& s : still) { keep.push_back(s.file); }
        std::string err;
        (void)savePendingRemovals(pluginsDir, keep, err);
    }
    const PluginCleanupResult done = removeSupersededPlugins(pluginsDir, still, remover);
    res.removed = done.removed;
    res.queued = done.queued;
    res.failed = done.failed;
    return res;
}

}  // namespace cascade::core
