// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/track_photo_cache.hpp"

#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#include "core/plugin_calls.hpp"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace cascade::gui {

namespace {

unsigned int glUpload(const std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    unsigned int tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) { return 0u; }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Packed rows, not OpenGL's 4-byte default: a photo whose width is not a
    // multiple of four would shear (basemap_cache.cpp, the same upload).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    return tex;
}

void glRelease(unsigned int tex) {
    if (tex != 0u) { glDeleteTextures(1, &tex); }
}

// Third-party text about to be drawn every frame: bounded, stopped at the
// first NUL, control bytes made spaces, and never cut through a UTF-8
// sequence (a photographer's name is often not ASCII).
std::string copyCredit(const char* s, std::size_t cap) {
    std::string out;
    if (s == nullptr) { return out; }
    std::size_t n = 0;
    while (n < cap && s[n] != '\0') { ++n; }
    out.assign(s, n);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20u || c == 0x7f) { c = ' '; }
    }
    if (n == cap) {
        // Truncated: drop a trailing sequence the cut left incomplete.
        std::size_t lead = out.size();
        while (lead > 0 && (static_cast<unsigned char>(out[lead - 1]) & 0xC0u) == 0x80u) {
            --lead;
        }
        if (lead > 0) {
            const unsigned char b = static_cast<unsigned char>(out[lead - 1]);
            const std::size_t want = (b >= 0xF0u) ? 4u : (b >= 0xE0u) ? 3u : (b >= 0xC0u) ? 2u : 1u;
            if (out.size() - (lead - 1) < want) { out.resize(lead - 1); }
        }
    }
    // Nothing but spaces is no credit at all.
    if (out.find_first_not_of(' ') == std::string::npos) { out.clear(); }
    return out;
}

}  // namespace

bool photoLinkIsSafe(const char* link, std::size_t cap) {
    if (link == nullptr) { return false; }
    static const char kScheme[] = "https://";
    const std::size_t schemeLen = sizeof(kScheme) - 1;
    std::size_t n = 0;
    while (n < cap && link[n] != '\0') {
        const unsigned char c = static_cast<unsigned char>(link[n]);
        // Printable ASCII only, and none of the characters that would let a
        // string become more than one argument or leave its quotes.
        if (c <= 0x20u || c >= 0x7fu || c == '"' || c == '\'' || c == '<' || c == '>' ||
            c == '\\' || c == '`') {
            return false;
        }
        ++n;
    }
    if (n == cap) { return false; }  // no terminator inside the bound
    if (n <= schemeLen || std::strncmp(link, kScheme, schemeLen) != 0) { return false; }
    // A host must follow: "https:///x" and "https://" alone are not pages.
    return link[schemeLen] != '/';
}

bool photoPixelsAreSane(const CascadeTrackPhoto& p) {
    if (p.pixels == nullptr || p.format != CASCADE_IMAGE_RGB24) { return false; }
    if (p.width == 0u || p.height == 0u || p.width > CASCADE_TRACK_PHOTO_MAX_DIM ||
        p.height > CASCADE_TRACK_PHOTO_MAX_DIM) {
        return false;
    }
    return p.stride >= p.width * 3u;
}

PhotoFit fitPhoto(std::uint32_t width, std::uint32_t height, float maxW, float maxH) {
    PhotoFit f;
    // Written as positive tests so a NaN bound draws nothing.
    if (width == 0u || height == 0u || !(maxW > 0.0f) || !(maxH > 0.0f)) { return f; }
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    float s = maxW / w;
    if (maxH / h < s) { s = maxH / h; }
    if (s > 1.0f) { s = 1.0f; }
    f.w = w * s;
    f.h = h * s;
    return f;
}

TrackPhotoCache::TrackPhotoCache() : upload_(glUpload), release_(glRelease) {}

TrackPhotoCache::~TrackPhotoCache() { detach(); }

void TrackPhotoCache::setTextureHooks(Upload upload, Release release) {
    upload_ = std::move(upload);
    release_ = std::move(release);
}

void TrackPhotoCache::attach(const CascadeTrackPhotoApi* api, void* handle, cascade::core::fault::Slot* slot) {
    if (api == api_ && handle == handle_) { return; }
    detach();
    if (api == nullptr || handle == nullptr) { return; }
    api_ = api;
    handle_ = handle;
    slot_ = slot;
}

void TrackPhotoCache::letGo(const std::string& id, Entry& e) {
    if (e.photo.texture != 0u) {
        release_(e.photo.texture);
        e.photo.texture = 0u;
    }
    // No borrow is outstanding: every READY was released as it was copied.
    cascade::core::calls::forgetPhoto(slot_, api_, handle_, id.c_str());
}

void TrackPhotoCache::detach() {
    if (active()) {
        for (auto& kv : entries_) { letGo(kv.first, kv.second); }
    }
    entries_.clear();
    api_ = nullptr;
    handle_ = nullptr;
    slot_ = nullptr;
}

TrackPhotoCache::State TrackPhotoCache::request(const std::string& id, std::uint32_t kind,
                                                const Photo** out) {
    if (out != nullptr) { *out = nullptr; }
    if (!active() || id.empty()) { return State::Unavailable; }

    auto it = entries_.find(id);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxShown) { return State::Unavailable; }
        it = entries_.emplace(id, Entry{}).first;
    }
    Entry& e = it->second;
    e.wanted = true;
    if (e.state == State::Ready) {
        if (out != nullptr) { *out = &e.photo; }
        return e.state;
    }
    if (e.state == State::Missing) { return e.state; }

    // Pending: ask again. That the host still asks is what tells the plugin
    // the photo is still wanted.
    CascadeTrackPhoto p{};
    p.structSize = static_cast<std::uint32_t>(sizeof(CascadeTrackPhoto));
    const std::int32_t got = cascade::core::calls::getPhoto(slot_, api_, handle_, id.c_str(), kind, &p);
    if (got == CASCADE_PHOTO_PENDING) { return State::Pending; }
    if (got != CASCADE_PHOTO_READY) {
        e.state = State::Missing;
        return e.state;
    }

    // READY. Refused - shown as no photo at all - unless the picture can be
    // copied AND it says whose it is AND where it came from.
    // Everything the plugin lent is READ UNDER THE GUARD (plugin containment P2, fault::copy*FromPlugin): a pointer that cannot be read is the
    // plugin's fault, not the process's. The checks run on the host's own copies.
    namespace fault = cascade::core::fault;
    std::string linkCopy;
    std::string creditCopy;
    bool ok = photoPixelsAreSane(p) && fault::copyStringFromPlugin(slot_, fault::Entry::GetInfo, p.link, kMaxLinkBytes, linkCopy) &&
              photoLinkIsSafe(linkCopy.c_str(), kMaxLinkBytes);
    std::string credit;
    if (ok) {
        ok = fault::copyStringFromPlugin(slot_, fault::Entry::GetInfo, p.credit, kMaxCreditBytes, creditCopy);
        if (ok) {
            credit = copyCredit(creditCopy.c_str(), kMaxCreditBytes);
            ok = !credit.empty();
        }
    }
    unsigned int tex = 0u;
    if (ok) {
        const std::size_t rowBytes = static_cast<std::size_t>(p.width) * 3u;
        std::vector<std::uint8_t> packed(rowBytes * p.height);
        ok = fault::copyRowsFromPlugin(slot_, fault::Entry::GetInfo, packed.data(), rowBytes, p.pixels, p.stride, p.height);
        if (ok) { tex = upload_(packed.data(), p.width, p.height); }
        ok = tex != 0u;
    }
    if (ok) {
        e.photo.texture = tex;
        e.photo.width = p.width;
        e.photo.height = p.height;
        e.photo.credit = std::move(credit);
        e.photo.link = linkCopy;
    }
    // The borrow ends here whatever the host made of it.
    cascade::core::calls::releasePhoto(slot_, api_, handle_, &p);
    e.state = ok ? State::Ready : State::Missing;
    if (ok && out != nullptr) { *out = &e.photo; }
    return e.state;
}

void TrackPhotoCache::endFrame() {
    if (!active()) { return; }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second.wanted) {
            letGo(it->first, it->second);
            it = entries_.erase(it);
        } else {
            it->second.wanted = false;
            ++it;
        }
    }
}

}  // namespace cascade::gui
