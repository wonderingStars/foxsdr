// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/basemap_cache.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace cascade::gui {

namespace {
constexpr std::size_t kPollBufBytes = 4096;
}

BasemapCache::~BasemapCache() { detach(); }

void BasemapCache::attach(const CascadeBasemapApi* api) {
    if (api == api_) { return; }
    detach();
    if (api == nullptr) { return; }
    void* h = api->create();
    if (h == nullptr) { return; }
    api_ = api;
    handle_ = h;
    attribution_ = api->attribution != nullptr ? api->attribution : "";
    minZoom_ = api->minZoom;
    maxZoom_ = api->maxZoom;
    tileSize_ = api->tileSize;
    pollBuf_.assign(kPollBufBytes, '\0');
}

void BasemapCache::detach() {
    releaseAll();
    if (api_ != nullptr && handle_ != nullptr) { api_->destroy(handle_); }
    api_ = nullptr;
    handle_ = nullptr;
    attribution_.clear();
    partial_.clear();
}

void BasemapCache::releaseAll() {
    for (auto& kv : tiles_) {
        if (kv.second.tex != 0u) { glDeleteTextures(1, &kv.second.tex); }
    }
    tiles_.clear();
}

unsigned int BasemapCache::texture(std::uint32_t z, std::uint32_t x, std::uint32_t y) {
    if (!active()) { return 0u; }
    const std::uint64_t k = key(z, x, y);
    auto it = tiles_.find(k);
    if (it != tiles_.end()) {
        // A MISSING is believed for a while and then asked about again - see
        // Entry::missingFrame. A texture is kept for as long as it is used.
        if (it->second.missing && frame_ - it->second.missingFrame >= kMissingRetryFrames) {
            tiles_.erase(it);
        } else {
            it->second.lastUsedFrame = frame_;
            return it->second.tex;  // 0 when the plugin said MISSING
        }
    }

    CascadeTile t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTile));
    const std::int32_t got = api_->get_tile(handle_, z, x, y, &t);
    if (got == CASCADE_TILE_PENDING) {
        // Deliberately NOT recorded. The next frame asks again, which is what
        // tells the plugin the tile is still wanted; caching "pending" would
        // mean a tile that arrives is never picked up.
        return 0u;
    }
    if (got == CASCADE_TILE_MISSING) {
        Entry e;
        e.missing = true;
        e.lastUsedFrame = frame_;
        e.missingFrame = frame_;
        tiles_[k] = e;  // remembered so a tile that cannot exist is asked for
                        // once per retry window, not once per frame
        return 0u;
    }
    if (got != CASCADE_TILE_READY) { return 0u; }

    // Third-party numbers about to bound a read and size a texture. A stride
    // shorter than a row would walk off the end of the plugin's buffer on the
    // last line, exactly as it would for a decoded image.
    const std::size_t rowBytes = static_cast<std::size_t>(t.width) * 3u;
    const bool sane = t.pixels != nullptr && t.width == tileSize_ &&
                      t.height == tileSize_ && t.format == CASCADE_IMAGE_RGB24 &&
                      t.stride >= rowBytes && t.width <= CASCADE_TILE_SIZE_MAX;
    unsigned int tex = 0u;
    if (sane) {
        std::vector<std::uint8_t> packed(rowBytes * t.height);
        for (std::uint32_t row = 0; row < t.height; ++row) {
            std::memcpy(packed.data() + static_cast<std::size_t>(row) * rowBytes,
                        t.pixels + static_cast<std::size_t>(row) * t.stride, rowBytes);
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Rows are packed above, which is NOT the 4-byte default OpenGL
        // assumes; without this a tile whose width is not a multiple of four
        // shears.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei>(t.width),
                     static_cast<GLsizei>(t.height), 0, GL_RGB, GL_UNSIGNED_BYTE,
                     packed.data());
    }
    // Released whether or not it was believed: the borrow is over the moment
    // the copy is taken, and a plugin must never be left holding a tile
    // because the host disliked its dimensions.
    api_->release_tile(handle_, &t);

    Entry e;
    e.tex = tex;
    e.lastUsedFrame = frame_;
    e.missing = (tex == 0u);
    e.missingFrame = frame_;
    tiles_[k] = e;
    return tex;
}

std::int32_t BasemapCache::rawTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                                   std::vector<std::uint8_t>& rgb) {
    rgb.clear();
    if (!active()) { return CASCADE_TILE_MISSING; }

    CascadeTile t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTile));
    const std::int32_t got = api_->get_tile(handle_, z, x, y, &t);
    if (got == CASCADE_TILE_PENDING) { return CASCADE_TILE_PENDING; }
    if (got != CASCADE_TILE_READY) { return CASCADE_TILE_MISSING; }

    // The same third-party-numbers check texture() applies, for the same
    // reason: the stride is about to bound a read of the plugin's buffer.
    const std::size_t rowBytes = static_cast<std::size_t>(t.width) * 3u;
    const bool sane = t.pixels != nullptr && t.width == tileSize_ &&
                      t.height == tileSize_ && t.format == CASCADE_IMAGE_RGB24 &&
                      t.stride >= rowBytes && t.width <= CASCADE_TILE_SIZE_MAX;
    if (sane) {
        rgb.resize(rowBytes * t.height);
        for (std::uint32_t row = 0; row < t.height; ++row) {
            std::memcpy(rgb.data() + static_cast<std::size_t>(row) * rowBytes,
                        t.pixels + static_cast<std::size_t>(row) * t.stride, rowBytes);
        }
    }
    api_->release_tile(handle_, &t);
    return sane ? CASCADE_TILE_READY : CASCADE_TILE_MISSING;
}

void BasemapCache::endFrame() {
    ++frame_;
    if (tiles_.size() <= kMaxTiles) { return; }
    // Evict what this frame did not ask for, oldest first. Panning across a
    // continent otherwise keeps every tile it ever crossed.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> byAge;  // (lastUsed, key)
    byAge.reserve(tiles_.size());
    for (const auto& kv : tiles_) {
        byAge.emplace_back(kv.second.lastUsedFrame, kv.first);
    }
    std::sort(byAge.begin(), byAge.end());
    const std::size_t drop = tiles_.size() - kMaxTiles;
    for (std::size_t i = 0; i < drop && i < byAge.size(); ++i) {
        auto it = tiles_.find(byAge[i].second);
        if (it == tiles_.end()) { continue; }
        if (it->second.tex != 0u) { glDeleteTextures(1, &it->second.tex); }
        tiles_.erase(it);
    }
}

std::vector<std::string> BasemapCache::drainText() {
    std::vector<std::string> out;
    if (!active()) { return out; }
    // BOUNDED PER CALL, exactly as TrackInfoCache::drainText is. This runs on
    // the GUI thread, once per frame, and the loop used to run until the
    // plugin said "nothing left". A plugin that produces text faster than one
    // frame can drain it never says that — a wrong tile URL makes every
    // request fail and failures are what a plugin logs, so this one arrives by
    // accident rather than malice — and the frame simply never ends. 32 polls
    // of a 4 KB buffer is 128 KB of status text per frame, orders of magnitude
    // more than anything legitimate emits; whatever is still queued is taken
    // by the next frame, the same trade the tile eviction cap makes.
    for (std::size_t polls = 0; polls < kMaxPollsPerDrain; ++polls) {
        const std::int32_t n =
            api_->poll_text(handle_, pollBuf_.data(), pollBuf_.size());
        if (n <= 0) { break; }
        const std::size_t got = std::min(static_cast<std::size_t>(n), pollBuf_.size());
        partial_.append(pollBuf_.data(), got);
        for (;;) {
            const std::size_t nl = partial_.find('\n');
            if (nl == std::string::npos) { break; }
            if (nl > 0) { out.push_back(partial_.substr(0, nl)); }
            partial_.erase(0, nl + 1);
        }
        if (partial_.size() > 4096) {
            out.push_back(partial_);
            partial_.clear();
        }
    }
    return out;
}

}  // namespace cascade::gui
