// track_photo_cache.hpp - the photo a track-info plugin offers for a target
// whose details are open (CASCADE_CAP_TRACK_PHOTO, 0.99.98).
//
// THE OPPOSITE OF TrackInfoCache'S POLICY, ON PURPOSE. A registry answer is
// copied and kept so each id is asked about once. A photo is kept only while
// it is on screen: photo services allow display and forbid storage
// (Planespotters.net's terms forbid writing an image to storage or keeping it
// after it stops being displayed). So a target whose details were not drawn
// since the last endFrame() has its texture deleted and the plugin is told to
// forget it, and opening those details again asks again.
//
// The decisions - is this answer usable, is this link safe to hand to the
// shell, how big is the picture drawn - are pure functions below, tested
// against a scripted fake of the C table. The texture itself goes through two
// hooks, OpenGL by default, so the tests need no GL context.
//
// GUI THREAD ONLY, like every other plugin-facing surface in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_GUI_TRACK_PHOTO_CACHE_HPP
#define CASCADE_GUI_TRACK_PHOTO_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "core/plugin_abi.h"
#include "core/plugin_fault.hpp"

namespace cascade::gui {

// A link the host may open with the shell: "https://", then printable ASCII
// with no space and no quote, at most `cap` bytes, NUL-terminated inside them.
// The photo's page is opened UNCHANGED (the services require that), so the
// only choice the host has is whether to open it at all - and a plugin must
// not be able to hand ShellExecute a file path, a different scheme or an
// argument.
bool photoLinkIsSafe(const char* link, std::size_t cap);

// The picture an answer describes is one the host can copy: RGB24, both sides
// in 1..CASCADE_TRACK_PHOTO_MAX_DIM, a stride that covers a row, pixels.
bool photoPixelsAreSane(const CascadeTrackPhoto& p);

// The size the picture is drawn at: inside maxW x maxH, aspect kept, never
// enlarged past its own pixels (a 200 px thumbnail blown up to the panel's
// width is a blur, not a better picture). Zero when nothing can be drawn.
struct PhotoFit {
    float w = 0.0f;
    float h = 0.0f;
};
PhotoFit fitPhoto(std::uint32_t width, std::uint32_t height, float maxW, float maxH);

class TrackPhotoCache {
public:
    enum class State {
        Unavailable,  // no photo source, or too many targets shown at once
        Pending,      // asked; the plugin is fetching
        Missing,      // no photo of this target, or one the host refused
        Ready,
    };

    struct Photo {
        unsigned int texture = 0u;
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
        std::string credit;
        std::string link;
    };

    // `rgb` is tightly packed rows (width * 3 bytes each). Returns 0 on failure.
    using Upload = std::function<unsigned int(const std::uint8_t* rgb, std::uint32_t width,
                                              std::uint32_t height)>;
    using Release = std::function<void(unsigned int)>;

    TrackPhotoCache();
    ~TrackPhotoCache();
    TrackPhotoCache(const TrackPhotoCache&) = delete;
    TrackPhotoCache& operator=(const TrackPhotoCache&) = delete;

    // `handle` is the track-info instance the photos are asked on. The caller
    // detaches this BEFORE destroying that instance.
    // `slot` is the plugin's containment slot (core/plugin_fault.hpp): every call into the plugin, and every read of memory it lends, is made under it.
    void attach(const CascadeTrackPhotoApi* api, void* handle, cascade::core::fault::Slot* slot = nullptr);
    // Forgets every shown id with the plugin and deletes every texture.
    void detach();
    bool active() const { return api_ != nullptr && handle_ != nullptr; }

    // For a target whose details are being drawn THIS frame. Asking is what
    // starts the fetch, and asking every frame is what keeps the photo. `out`
    // is set only for Ready and is good until the next call into the cache.
    State request(const std::string& id, std::uint32_t kind, const Photo** out);

    // Once per frame, after drawing: every id not requested since the last
    // call is let go - texture deleted, plugin told to forget it.
    void endFrame();

    std::size_t shown() const { return entries_.size(); }

    // Tests only: replace the OpenGL texture calls.
    void setTextureHooks(Upload upload, Release release);

    // Targets whose photos may be held at once. The host draws two (the
    // details window and the rail's section); four leaves room, and a cap
    // keeps a fault in a caller from fetching a photo per target on the map.
    static constexpr std::size_t kMaxShown = 4;
    static constexpr std::size_t kMaxCreditBytes = 160;
    static constexpr std::size_t kMaxLinkBytes = 512;

private:
    struct Entry {
        State state = State::Pending;
        bool wanted = false;
        Photo photo;
    };

    void letGo(const std::string& id, Entry& e);

    const CascadeTrackPhotoApi* api_ = nullptr;
    void* handle_ = nullptr;
    cascade::core::fault::Slot* slot_ = nullptr;
    std::unordered_map<std::string, Entry> entries_;
    Upload upload_;
    Release release_;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRACK_PHOTO_CACHE_HPP
