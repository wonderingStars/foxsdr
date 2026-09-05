// basemap_cache.hpp - turns a basemap plugin's tiles into GL textures.
//
// The map needs a texture id for a tile RIGHT NOW, sixty times a second. The
// plugin needs to fetch that tile over a network, which cannot happen in that
// time. This class is the join between those two facts:
//
//   - it asks the plugin for tiles the map says are visible, once per frame;
//   - a tile the plugin has ready is uploaded ONCE and kept as a texture;
//   - a tile the plugin is still fetching leaves the map with nothing to draw
//     for that square, which is a blank the map fills from the zoom above.
//
// WHY THE HOST HOLDS THE TEXTURES AND NOT THE PLUGIN. A plugin cannot touch
// OpenGL: it has no context, it may be built by a third party against a
// different runtime, and a GL call from the wrong thread is undefined
// behaviour rather than an error. So the plugin's whole job is bytes, and
// everything about the graphics API stays on this side of the boundary.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_GUI_BASEMAP_CACHE_HPP
#define CASCADE_GUI_BASEMAP_CACHE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::gui {

class BasemapCache {
public:
    ~BasemapCache();

    BasemapCache(const BasemapCache&) = delete;
    BasemapCache& operator=(const BasemapCache&) = delete;
    BasemapCache() = default;

    // Points the cache at a plugin's basemap table, creating one instance.
    // Passing nullptr detaches and releases everything, which is what happens
    // when the user removes the plugin.
    void attach(const CascadeBasemapApi* api);
    void detach();

    bool active() const { return api_ != nullptr && handle_ != nullptr; }
    const std::string& attribution() const { return attribution_; }
    std::uint32_t minZoom() const { return minZoom_; }
    std::uint32_t maxZoom() const { return maxZoom_; }
    std::uint32_t tileSize() const { return tileSize_; }

    // GL texture for a tile, or 0 when it is not available yet. Asking is what
    // starts the plugin fetching it, so a caller that stops asking for a tile
    // stops it being fetched.
    unsigned int texture(std::uint32_t z, std::uint32_t x, std::uint32_t y);

    // Once per frame, AFTER the map has asked for everything it wants: drops
    // textures nothing asked for this frame, so panning across a continent
    // cannot grow the texture set without limit.
    void endFrame();

    // The same tile as packed 24-bit RGB rows instead of a texture, for a
    // consumer with no GL context — the web server, whose browser does its own
    // drawing. Same thread contract as texture() (the plugin is only ever
    // called from the GUI thread), and the same start-the-fetch semantics:
    // asking is what makes the plugin go and get it.
    //
    // Returns CASCADE_TILE_READY with `rgb` filled (width*height*3 bytes,
    // stride removed), CASCADE_TILE_PENDING while the plugin is still
    // fetching, or CASCADE_TILE_MISSING when the tile will never exist —
    // which includes a tile the plugin delivered with dimensions that do not
    // match its own declaration, for the same reason texture() refuses one.
    // Deliberately does not touch the texture set: the caller caches results
    // on its own side, so nothing here needs remembering.
    std::int32_t rawTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                         std::vector<std::uint8_t>& rgb);

    // Status lines the plugin produced ("cannot reach the tile server"), moved
    // out for display. The failure a user will actually hit is a wrong URL,
    // and a map that just stays blank explains nothing.
    std::vector<std::string> drainText();

    std::size_t residentTiles() const { return tiles_.size(); }

    // How many times ONE drainText() may poll the plugin. See drainText's
    // implementation comment: this is the bound that keeps a talkative plugin
    // from owning the GUI thread for ever.
    static constexpr std::size_t kMaxPollsPerDrain = 32;

private:
    struct Entry {
        unsigned int tex = 0;
        std::uint64_t lastUsedFrame = 0;
        bool missing = false;  // the plugin said it never will exist
        // The frame the MISSING was recorded on. "Never will exist" is the
        // plugin's word, and a fetch that failed because the network was down
        // for a moment is the same word from the same plugin: the host cannot
        // tell a tile the server does not have from one it could not reach
        // just then. So a MISSING is believed for kMissingRetryFrames and then
        // asked about again. Before this, one dropped connection left a
        // permanent hole in the map until the next restart - which on a link
        // that flaps was most of the map, and was reported as "hard to load".
        std::uint64_t missingFrame = 0;
    };

    // About ten seconds at the frame rate this application runs at. Long
    // enough that a tile the server genuinely lacks costs one request every
    // ten seconds and not one per frame; short enough that a map recovers from
    // a dropped link while the user is still looking at it.
    static constexpr std::uint64_t kMissingRetryFrames = 600;

    static std::uint64_t key(std::uint32_t z, std::uint32_t x, std::uint32_t y) {
        return (static_cast<std::uint64_t>(z) << 58) |
               (static_cast<std::uint64_t>(y) << 29) | static_cast<std::uint64_t>(x);
    }

    void releaseAll();

    const CascadeBasemapApi* api_ = nullptr;
    void* handle_ = nullptr;
    std::string attribution_;
    std::uint32_t minZoom_ = 0;
    std::uint32_t maxZoom_ = 19;
    std::uint32_t tileSize_ = 256;

    std::unordered_map<std::uint64_t, Entry> tiles_;
    std::uint64_t frame_ = 0;
    std::vector<char> pollBuf_;
    std::string partial_;

    // A cap on resident textures. At 256x256 RGB each is 192 KB, so this is
    // about 100 MB - generous for any view, and a hard stop on a pan that
    // would otherwise keep every tile it ever touched.
    static constexpr std::size_t kMaxTiles = 512;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_BASEMAP_CACHE_HPP
