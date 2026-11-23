// companion_state_cache.hpp - the companion shelf's state, read OFF the engine's pass.
//
// THE FIELD FREEZE THIS EXISTS FOR (B058596B, freeze diagnosis P4). companion::stateForEntry() reads the package's companions.json and the install record
// from disk, TWICE per companion row every time the engine rebuilds its lists (Engine::buildApiLists; Engine::pluginInstallBlockedReason), on the control
// thread. On a machine whose disk or scanner answered slowly the pass stopped there and the watchdog filed a freeze.
//
// THE SHAPE. The pass reads a map (lookup(): memory only, never a file call). One worker, in the house pattern of core/disk_job.hpp, recomputes every
// companion row's state: the first time, and again when the map is older than kMaxAgeMs while somebody has looked at it within kIdleMs (the shelf is
// open or a list is being built). tick() is called once a pass, collects a finished worker and starts the next one; it never blocks. When a refresh
// changes any state, generation() moves, and the engine puts that number in its list fingerprint so the rows are rebuilt with the new word.
//
// WHAT A MISS MEANS. lookup() is false for a row the worker has not computed yet: the list shows an empty state (the window already reads that as "not
// available" and warns) for the milliseconds until the first refresh lands, and pluginInstallBlockedReason does NOT block on it (the install path checks the
// program again before it fetches anything).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/companion_programs.hpp"
#include "core/disk_job.hpp"

namespace cascade::core::companion {

class StateCache {
public:
    // How old the map may get before the next refresh, while somebody is looking. A fit or a removal shows within this plus one refresh.
    static constexpr std::chrono::milliseconds kMaxAge{2000};
    // How long after the last lookup the map keeps itself fresh. Nobody looking: no file is read at all.
    static constexpr std::chrono::milliseconds kIdle{60000};

    StateCache();

    // The state last computed for this catalogue entry (its id); false when there is none yet. Memory only.
    bool lookup(const std::string& entryId, State& out) const;

    // Once a pass. Collects a finished refresh and starts one when due (see above). `catalog` is copied only when a refresh is started, and only its companion
    // rows. Never blocks, never touches a file.
    void tick(const std::vector<PluginCatalogEntry>& catalog, const std::string& pluginsDir);

    // Moves whenever a refresh changed any state (or the first one landed). Part of the engine's list fingerprint.
    std::uint64_t generation() const;

    // A refresh is out (tests).
    bool refreshing() const;

    // TEST SEAM: called on the worker immediately before the refresh's first file call (a test binds one that sleeps: a slow disk). Empty in the product.
    static void setDiskHookForTest(std::function<void()> hook);

private:
    using Map = std::map<std::string, State>;
    mutable std::mutex mu_;
    Map states_;
    std::uint64_t generation_ = 0;
    bool computed_ = false;
    std::chrono::steady_clock::time_point computedAt_{};
    mutable std::chrono::steady_clock::time_point lastLookup_{};
    DiskJob<Map> job_{"companion state"};
};

}  // namespace cascade::core::companion
