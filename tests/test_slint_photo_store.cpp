// The new window's photo store (src/slint/photo_store.hpp): the target photos' pixels live in memory only, exactly while the photo cache holds them.
// Driven through the real TrackInfoCache / TrackPhotoCache and a scripted fake of the plugin's C tables (no network, no GL, no disk).
//   A. a shown target's photo is READY, its pixels are in the store, bytes exactly the plugin's rows (padding stripped), the link and credit are the plugin's, unchanged;
//   B. DESELECT: a frame in which nobody asks, and the store is empty (count 0), the plugin was told to forget the id;
//   C. asking again fetches again (a photo is never kept for later);
//   D. a plugin that offers no photo: no entry, nothing in the store.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstring>
#include <string>
#include <vector>

#include "../src/slint/photo_store.hpp"
#include "gui/track_info_cache.hpp"
#include "test_check.hpp"

using cascade::gui::TrackInfoCache;
using cascade::gui::TrackPhotoCache;

namespace {
void* const kHandle = reinterpret_cast<void*>(0x77);
std::vector<std::uint8_t> g_pix;
std::vector<std::string> g_forgot;
int g_gets = 0;
bool g_offer = true;
const char* kCredit = "Photo: A. Smith / Planespotters.net";
const char* kLink = "https://www.planespotters.net/photo/123/abc?x=1&y=2";

void* infoCreate() { return kHandle; }
void infoDestroy(void*) {}
std::int32_t infoGet(void*, const char*, std::uint32_t, CascadeTrackInfo*) { return CASCADE_INFO_MISSING; }
void infoRelease(void*, const CascadeTrackInfo*) {}
std::int32_t infoPoll(void*, char*, size_t) { return 0; }

std::int32_t getPhoto(void*, const char*, std::uint32_t, CascadeTrackPhoto* out) {
    ++g_gets;
    if (!g_offer) { return CASCADE_PHOTO_MISSING; }
    g_pix.assign(static_cast<std::size_t>(16) * 4, 0xEE);   // stride 16 for 4 px of 3 bytes: 4 bytes of padding per row
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 12; ++c) { g_pix[r * 16 + c] = static_cast<std::uint8_t>(10 * r + c); } }
    out->width = 4; out->height = 4; out->format = CASCADE_IMAGE_RGB24; out->stride = 16;
    out->pixels = g_pix.data(); out->credit = kCredit; out->link = kLink;
    return CASCADE_PHOTO_READY;
}
void relPhoto(void*, const CascadeTrackPhoto*) {}
void forgetPhoto(void*, const char* id) { g_forgot.push_back(id); }
}  // namespace

int main() {
    const CascadeTrackPhotoApi photoApi = {static_cast<std::uint32_t>(sizeof(CascadeTrackPhotoApi)), &getPhoto, &relPhoto, &forgetPhoto};
    CascadeTrackInfoApi infoApi{};
    infoApi.structSize = static_cast<std::uint32_t>(sizeof(infoApi));
    infoApi.create = &infoCreate; infoApi.get_info = &infoGet; infoApi.release_info = &infoRelease; infoApi.poll_text = &infoPoll; infoApi.destroy = &infoDestroy;

    foxsdr::slintkit::PhotoStore store;   // declared before the cache, as in PluginKit
    TrackInfoCache cache;
    store.install(cache.photos());
    cache.attach(&infoApi, std::string(), &photoApi);
    CHECK(cache.photos().active());

    // A
    const TrackPhotoCache::Photo* p = nullptr;
    TrackPhotoCache::State st = cache.photos().request("4D2025", CASCADE_TRACK_AIRCRAFT, &p);
    if (st == TrackPhotoCache::State::Pending) { st = cache.photos().request("4D2025", CASCADE_TRACK_AIRCRAFT, &p); }
    CHECK(st == TrackPhotoCache::State::Ready && p != nullptr);
    if (st == TrackPhotoCache::State::Ready && p != nullptr) {
        CHECK(store.size() == 1u);
        const foxsdr::slintkit::PhotoStore::Pixels* px = store.find(p->texture);
        CHECK(px != nullptr && px->w == 4u && px->h == 4u && px->rgb.size() == 48u);
        bool same = px != nullptr;
        for (int r = 0; same && r < 4; ++r) { for (int c = 0; c < 12; ++c) { if (px->rgb[r * 12 + c] != static_cast<std::uint8_t>(10 * r + c)) { same = false; } } }
        CHECK(same);
        CHECK(p->credit == kCredit);
        CHECK(p->link == kLink);   // unchanged
    }
    // B: deselect
    cache.photos().endFrame();                 // the frame in which it was asked
    CHECK(store.size() == 1u);
    cache.photos().endFrame();                 // a frame in which nobody asked: let go
    CHECK(store.size() == 0u);
    CHECK(cache.photos().shown() == 0u);
    CHECK(g_forgot.size() == 1u && g_forgot[0] == "4D2025");
    // C
    const int before = g_gets;
    cache.photos().request("4D2025", CASCADE_TRACK_AIRCRAFT, &p);
    CHECK(g_gets > before);
    cache.photos().endFrame(); cache.photos().endFrame();
    CHECK(store.size() == 0u);
    // D
    g_offer = false;
    cache.photos().request("4D2026", CASCADE_TRACK_AIRCRAFT, &p);
    cache.photos().request("4D2026", CASCADE_TRACK_AIRCRAFT, &p);
    CHECK(p == nullptr || store.size() == 0u);
    CHECK(store.size() == 0u);
    cache.detach();
    return testSummary("test_slint_photo_store");
}
