// plugin_directive.hpp - how the engine tells the plugin UI what to be, across threads (the pump move's step P3).
//
// THE PROBLEM IT SOLVES. The plugin UI - the track sources, panels and instruments the window polls every frame, and the
// basemap and track-info clients it draws from - is third-party code held by GUI-thread objects: plugin_abi.h polls it "from
// the GUI thread at frame rate", and PluginUi's vectors are read by reference all frame long. The engine's pass used to
// REBUILD it (create and destroy every instance) and CLEAR it (before the modules are unmapped) from inside the frame, which
// was safe only because the frame was the pass. Once the pass runs on a thread of its own, a rebuild would destroy an instance
// the GUI is inside, and a clear would let go of a module's memory the GUI still holds - undefined behaviour inside someone
// else's DLL, with no useful stack.
//
// THE DESIGN. The engine no longer touches the plugin UI. It posts a DIRECTIVE - the whole of what the UI is to be built from,
// as VALUES (the plugin list copied, the stop set, the host services, the basemap and track-info tables) - behind a mutex,
// tagged with an EPOCH that only grows. The GUI applies the NEWEST directive at the head of its frame's plugin poll, before any
// reference into the UI is taken, and acknowledges the epoch with an atomic store. Two consequences:
//
//   - A directive posted while another is waiting REPLACES it (newest wins; skipped() counts what was never applied), so a
//     burst of source changes costs the GUI one rebuild, not a queue of them.
//   - The engine may WAIT for the acknowledgement, bounded (waitApplied). That wait is the unload handshake: before the modules
//     are unmapped the engine posts a RELEASE directive ("release everything borrowed from the modules, epoch N") and unmaps
//     only once N is acknowledged - the GUI has by then destroyed every instance and let go of the basemap and the track-info
//     client, and cannot be holding any of it, because the one thread that could is the one that acknowledged.
//
// NOTHING HERE CALLS PLUGIN CODE and nothing here knows what a window is: the applier is a function the front end supplies,
// run on the thread that calls applyNewest.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_DIRECTIVE_HPP
#define CASCADE_CORE_PLUGIN_DIRECTIVE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"

namespace cascade::core {

// What the host lets a plugin do to the receiver. Supplied by the owner (the engine), so this header stays free of the
// pipeline and the source stack.
struct HostServices {
    std::function<double()> centreHz;
    std::function<double()> sampleRateHz;
    // Returns a CASCADE_TUNE_* code. The permission check is applied by
    // PluginUi BEFORE this is called, so an implementation only has to do the
    // tuning and report device-level outcomes.
    std::function<std::int32_t(double)> tune;
    // The same, told WHICH MODULE is asking (its tuneKey): preferred over `tune` when set. The engine sets this one - its
    // answer is a request queued for the pass keyed on the module, whose grant is checked again when it is applied (the pump
    // move's step P3, owner decision 1) - and a caller that cares only about the frequency (the unit tests) sets `tune`.
    std::function<std::int32_t(const std::string&, double)> tuneFrom;
    std::function<std::int64_t()> unixTimeMs;
};

// One instruction to the plugin UI: what to be built over, as values.
struct PluginDirective {
    // Assigned by the link when it is posted; only ever grows.
    std::uint64_t epoch = 0;
    // "Release everything borrowed from the modules": plugins is empty, both tables are null, and applying it leaves the UI
    // holding nothing of any module. The engine unmaps modules only after this is acknowledged.
    bool release = false;
    // The loaded plugins, COPIED: the records point into the modules' images, so a directive is only valid until the engine
    // posts its next one - which a release always is, before any unmapping.
    std::vector<LoadedPlugin> plugins;
    // The plugins the user has stopped, by module file name (or plugin id: the stop set reduces both).
    std::vector<std::string> stopped;
    // The plugins set to AUTO that nothing is using (stub campaign B2, 0.99.73, core/plugin_run.hpp): like the stop set, a plugin in it is
    // given no instance by a rebuild, by plugin id.
    std::vector<std::string> dormant;
    // "Wake or put to sleep ONE plugin, rebuild nothing": `plugins`, `stopped` and `dormant` are the STATE the UI is to hold (not an event),
    // and applying it starts the instances of a plugin that has come out of both sets and stops those of one that has gone into either. The
    // services, the basemap and the track-info table are untouched by it except that the basemap and track-info tables are the engine's
    // choice again (a stopped plugin may not supply them). Because it is state, a newer directive may stand for an older one - the link
    // merges a life-only into what still waits (post) - and applying it twice changes nothing.
    bool lifeOnly = false;
    HostServices services;
    // The basemap and track-info plugins chosen (first loaded, not stopped), or null.
    const CascadeBasemapApi* basemap = nullptr;
    const CascadeTrackInfoApi* trackInfo = nullptr;
    // ...and the module file each came from (plugin containment P1): the interface's caches call the plugin through its guard, which is keyed
    // on the plugin, and the tables alone name nobody. Empty when the table is null.
    std::string basemapPlugin;
    std::string trackInfoPlugin;
};

class PluginDirectiveLink {
public:
    // ===== The engine's side ====================================================================================================

    // Posts `d` (epoch assigned here) as the newest; a directive still waiting is replaced and counted in skipped(). Returns
    // the epoch. Any thread.
    std::uint64_t post(PluginDirective d) {
        std::uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            d.epoch = ++posted_;
            if (pending_.has_value()) {
                ++skipped_;
                if (d.lifeOnly && pending_->release) {
                    // A RELEASE STILL WAITING IS NEVER REPLACED BY A LIFE-ONLY ONE: the engine unmaps the modules once the release's epoch
                    // is acknowledged, and an acknowledgement of a newer directive that released nothing would say it was safe. The
                    // release leaves nothing to wake, so the life-only is dropped and the caller waits on the release's own epoch.
                    --posted_;
                    return pending_->epoch;
                }
                if (d.lifeOnly) {
                    // KEEP WHAT WAITS (a rebuild's services, its basemap and track-info choice), bring it to the newest state.
                    PluginDirective merged = std::move(*pending_);
                    merged.epoch = d.epoch;
                    merged.plugins = std::move(d.plugins);
                    merged.stopped = std::move(d.stopped);
                    merged.dormant = std::move(d.dormant);
                    merged.basemap = d.basemap;
                    merged.trackInfo = d.trackInfo;
                    merged.basemapPlugin = std::move(d.basemapPlugin);
                    merged.trackInfoPlugin = std::move(d.trackInfoPlugin);
                    pending_ = std::move(merged);
                    epoch = posted_;
                    d.epoch = 0;
                } else {
                    pending_ = std::move(d);
                    epoch = posted_;
                }
            } else {
                pending_ = std::move(d);
                epoch = posted_;
            }
        }
        // (Outside the lock: the hook takes locks of its own.)
        if (onPost_) { onPost_(); }
        return epoch;
    }

    // A function called, on the posting thread and without the link's lock, after every post. The engine uses it to wake a front-end
    // thread that is waiting for the pass (Engine::stepPass) and so is the very thread that has to apply this directive. Set once,
    // before any thread posts.
    void setOnPost(std::function<void()> f) { onPost_ = std::move(f); }

    // Waits until `epoch` (or a newer one) has been applied, for at most `bound`. Returns whether it was. `giveUp` is asked
    // every few milliseconds and ends the wait early (the engine passes "the control thread is stopping": a shutdown must not
    // wait on a front end that is no longer framing). Any thread.
    bool waitApplied(std::uint64_t epoch, std::chrono::milliseconds bound,
                     const std::function<bool()>& giveUp = {}) {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        std::unique_lock<std::mutex> lk(m_);
        while (applied_ < epoch) {
            if (giveUp && giveUp()) { return false; }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { return false; }
            cv_.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(5)));
        }
        return true;
    }

    // ===== The front end's side (the GUI thread) =================================================================================

    // Applies the newest directive if one is waiting - `apply` runs on THIS thread, outside the lock - then acknowledges its
    // epoch. Returns whether one was applied. Called at the head of the frame's plugin poll, before any reference into the UI
    // is taken. Counts every call (polls()): the engine uses that to tell a front end that is not framing from one that is.
    template <class F>
    bool applyNewest(F&& apply) {
        polls_.fetch_add(1, std::memory_order_acq_rel);
        std::optional<PluginDirective> d;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!pending_.has_value()) { return false; }
            d = std::move(pending_);
            pending_.reset();
        }
        apply(static_cast<const PluginDirective&>(*d));
        {
            std::lock_guard<std::mutex> lk(m_);
            if (d->epoch > applied_) { applied_ = d->epoch; }
            appliedAtomic_.store(applied_, std::memory_order_release);
            ++appliedCount_;
        }
        cv_.notify_all();
        return true;
    }

    // ===== Either side ===========================================================================================================

    std::uint64_t appliedEpoch() const { return appliedAtomic_.load(std::memory_order_acquire); }
    std::uint64_t postedEpoch() const {
        std::lock_guard<std::mutex> lk(m_);
        return posted_;
    }
    // Directives replaced before any front end applied them.
    std::uint64_t skipped() const {
        std::lock_guard<std::mutex> lk(m_);
        return skipped_;
    }
    // Directives actually applied.
    std::uint64_t appliedCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return appliedCount_;
    }
    // How many times the front end has asked (applyNewest calls, applied or not): moves while it is framing.
    std::uint64_t polls() const { return polls_.load(std::memory_order_acquire); }
    bool hasPending() const {
        std::lock_guard<std::mutex> lk(m_);
        return pending_.has_value();
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> onPost_;
    std::optional<PluginDirective> pending_;
    std::uint64_t posted_ = 0;
    std::uint64_t applied_ = 0;
    std::uint64_t skipped_ = 0;
    std::uint64_t appliedCount_ = 0;
    std::atomic<std::uint64_t> appliedAtomic_{0};
    std::atomic<std::uint64_t> polls_{0};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_DIRECTIVE_HPP
