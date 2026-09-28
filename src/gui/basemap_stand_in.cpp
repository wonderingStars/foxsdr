// basemap_stand_in.cpp - see basemap_stand_in.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/basemap_stand_in.hpp"

#include <cstdint>
#include <vector>

#include "gui/ui_census.hpp"

namespace cascade::gui {

namespace {

constexpr std::uint32_t kTile = 256;

// One flat tile, a muted land colour, shared by every (z, x, y).
const std::vector<std::uint8_t>& tilePixels() {
    static const std::vector<std::uint8_t> px = [] {
        std::vector<std::uint8_t> v(static_cast<std::size_t>(kTile) * kTile * 3u);
        for (std::size_t i = 0; i < v.size(); i += 3) {
            v[i] = 0x9C;
            v[i + 1] = 0xB0;
            v[i + 2] = 0x8A;
        }
        return v;
    }();
    return px;
}

int g_instance = 0;   // create() must return non-null; nothing is kept in it

void* standInCreate() { return &g_instance; }

std::int32_t standInGetTile(void* handle, std::uint32_t z, std::uint32_t x, std::uint32_t y,
                            CascadeTile* out) {
    (void)handle;
    (void)z;
    (void)x;
    (void)y;
    if (out == nullptr) { return CASCADE_TILE_MISSING; }
    out->width = kTile;
    out->height = kTile;
    out->format = CASCADE_IMAGE_RGB24;
    out->stride = kTile * 3u;
    out->pixels = tilePixels().data();
    census::note("basemap:tile");
    return CASCADE_TILE_READY;
}

void standInRelease(void* handle, const CascadeTile* tile) {
    (void)handle;
    (void)tile;
}

std::int32_t standInPollText(void* handle, char* buf, size_t cap) {
    (void)handle;
    (void)buf;
    (void)cap;
    return 0;
}

void standInDestroy(void* handle) { (void)handle; }

const CascadeBasemapApi kStandIn = {
    static_cast<std::uint32_t>(sizeof(CascadeBasemapApi)),
    kStandInBasemapAttribution,
    0u,
    19u,
    kTile,
    &standInCreate,
    &standInGetTile,
    &standInRelease,
    &standInPollText,
    &standInDestroy,
};

}  // namespace

const CascadeBasemapApi* standInBasemap() { return &kStandIn; }

bool standInBasemapWanted(const char* value, bool boundedRun) {
    return boundedRun && value != nullptr && value[0] != '\0';
}

}  // namespace cascade::gui
