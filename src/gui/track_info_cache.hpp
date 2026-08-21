// track_info_cache.hpp - joins a track-info plugin's answers to the maps.
//
// The same join BasemapCache is for tiles: the map wants a label RIGHT NOW,
// sixty times a second, and the plugin may need a network round trip to
// answer. So asking is non-blocking (the ABI requires it), a PENDING answer
// costs nothing and is simply asked again next frame, and every READY or
// MISSING answer is COPIED into this cache so each id is asked about until it
// is answered once - never per frame for ever. The copy also ends the borrow
// immediately, so the plugin is never left holding strings across a frame.
//
// GUI THREAD ONLY, like every other plugin-facing surface in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_GUI_TRACK_INFO_CACHE_HPP
#define CASCADE_GUI_TRACK_INFO_CACHE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::gui {

class TrackInfoCache {
public:
    struct Info {
        bool known = false;  // false = the source answered "not in my data"
        std::string registration;
        std::string typeCode;
        std::string typeName;
        std::string manufacturer;
        std::string operatorName;
        std::string country;
    };

    ~TrackInfoCache();

    TrackInfoCache(const TrackInfoCache&) = delete;
    TrackInfoCache& operator=(const TrackInfoCache&) = delete;
    TrackInfoCache() = default;

    // Points the cache at a plugin's table, creating one instance. nullptr
    // detaches and drops every cached answer - a different plugin's answers
    // must not be served as this one's.
    void attach(const CascadeTrackInfoApi* api);
    void detach();

    bool active() const { return api_ != nullptr && handle_ != nullptr; }

    // What is known about track `id`, or nullptr while nothing is (never
    // asked, or the plugin is still looking it up). Asking is what starts the
    // plugin looking, so call this for the targets on screen each frame.
    const Info* get(const std::string& id, std::uint32_t kind);

    // Status lines from the plugin ("cannot reach the registry"), moved out
    // for display - the failure a user will actually hit is no network.
    std::vector<std::string> drainText();

    std::size_t size() const { return cache_.size(); }

    // Bounds. Entries past the cap evict oldest-first; a string field longer
    // than kMaxFieldBytes is truncated rather than trusted.
    static constexpr std::size_t kMaxEntries = 2048;
    static constexpr std::size_t kMaxFieldBytes = 128;

    // How many times ONE drainText() may poll the plugin. See drainText's
    // implementation comment: this is the bound that keeps a talkative plugin
    // from owning the GUI thread for ever.
    static constexpr std::size_t kMaxPollsPerDrain = 32;

private:
    const CascadeTrackInfoApi* api_ = nullptr;
    void* handle_ = nullptr;

    std::unordered_map<std::string, Info> cache_;
    std::unordered_map<std::string, std::uint64_t> age_;
    std::uint64_t seq_ = 0;
    std::vector<char> pollBuf_;
    std::string partial_;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRACK_INFO_CACHE_HPP
