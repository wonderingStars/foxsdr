// photo_store.hpp - where the target photos' pixels live in the new window: MEMORY ONLY, exactly as long as the photo cache holds them.
//
// gui/track_photo_cache.hpp keeps a photo only while a surface asks for it every frame and lets it go (texture released, the plugin told to forget it) the
// first frame nobody does; the photo service's terms forbid storing an image or holding it after it stops being displayed. The cache's "texture" is a
// handle made by two hooks; here the handle is an id into this store and the hooks copy the pixels in and erase them out. Nothing is written to disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef FOXSDR_SLINT_PHOTO_STORE_HPP
#define FOXSDR_SLINT_PHOTO_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gui/track_photo_cache.hpp"

namespace foxsdr::slintkit {

class PhotoStore {
public:
    struct Pixels { std::vector<std::uint8_t> rgb; std::uint32_t w = 0u; std::uint32_t h = 0u; };

    // The cache's texture hooks, pointed at this store (which must outlive the cache's use of them).
    void install(cascade::gui::TrackPhotoCache& cache) {
        cache.setTextureHooks(
            [this](const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) -> unsigned int {
                Pixels p;
                p.rgb.assign(rgb, rgb + static_cast<std::size_t>(w) * h * 3u);
                p.w = w; p.h = h;
                const unsigned int id = nextId_++;
                held_[id] = std::move(p);
                return id;
            },
            [this](unsigned int id) { held_.erase(id); });
    }
    const Pixels* find(unsigned int id) const { const auto it = held_.find(id); return it == held_.end() ? nullptr : &it->second; }
    std::size_t size() const { return held_.size(); }

private:
    std::unordered_map<unsigned int, Pixels> held_;
    unsigned int nextId_ = 1u;
};

}  // namespace foxsdr::slintkit

#endif
