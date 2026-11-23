// companion_state_cache.cpp - see companion_state_cache.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/companion_state_cache.hpp"

#include <utility>

namespace cascade::core::companion {

namespace {
std::mutex g_hookMu;
std::function<void()> g_hook;
void hook() {
    std::function<void()> h;
    { std::lock_guard<std::mutex> lk(g_hookMu); h = g_hook; }
    if (h) { h(); }
}
}  // namespace

StateCache::StateCache() = default;

void StateCache::setDiskHookForTest(std::function<void()> h) {
    std::lock_guard<std::mutex> lk(g_hookMu);
    g_hook = std::move(h);
}

bool StateCache::lookup(const std::string& entryId, State& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    lastLookup_ = std::chrono::steady_clock::now();
    const auto it = states_.find(entryId);
    if (it == states_.end()) { return false; }
    out = it->second;
    return true;
}

std::uint64_t StateCache::generation() const {
    std::lock_guard<std::mutex> lk(mu_);
    return generation_;
}

bool StateCache::refreshing() const { return job_.pending(); }

void StateCache::tick(const std::vector<PluginCatalogEntry>& catalog, const std::string& pluginsDir) {
    if (job_.pending()) {
        Map done;   // (only here: MSVC's std::map allocates its head node on construction, and the pass allocates nothing when nothing changed)
        if (!job_.poll(done)) { return; }   // still out: nothing else to do this pass
        std::lock_guard<std::mutex> lk(mu_);
        const bool changed = !computed_ || done != states_;
        states_ = std::move(done);
        computed_ = true;
        computedAt_ = std::chrono::steady_clock::now();
        if (changed) { ++generation_; }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool looked = lastLookup_.time_since_epoch().count() != 0 && now - lastLookup_ < kIdle;
        const bool due = !computed_ || (looked && now - computedAt_ >= kMaxAge);
        if (!due) { return; }
    }
    std::vector<PluginCatalogEntry> rows;
    for (const PluginCatalogEntry& e : catalog) {
        if (e.companion.present && e.source.empty()) { rows.push_back(e); }
    }
    if (rows.empty() && !catalog.empty()) {
        // nothing to compute: a computed (empty) map is as good as any, and costs no thread
        std::lock_guard<std::mutex> lk(mu_);
        if (!computed_ || !states_.empty()) { ++generation_; }
        states_.clear();
        computed_ = true;
        computedAt_ = now;
        return;
    }
    if (rows.empty()) { return; }   // the catalogue has not arrived yet: try again when it has
    job_.request([rows = std::move(rows), dir = pluginsDir]() -> Map {
        hook();
        Map out;
        const Places places = placesFor(dir);
        std::vector<PackageProgram> list;
        if (places.packaged) {
            std::string ignored;
            (void)readPackageList(places.packageDir, list, ignored);   // no list: nothing is "in the package"
        }
        for (const PluginCatalogEntry& e : rows) { out[e.id] = stateOf(e, places, list, bridgeFileOf(e)); }
        return out;
    });
}

}  // namespace cascade::core::companion
