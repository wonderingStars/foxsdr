// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/track_info_cache.hpp"

#include <algorithm>
#include <cstring>

namespace cascade::gui {

namespace {
constexpr std::size_t kPollBufBytes = 4096;

// Third-party bytes about to become a std::string the GUI will render every
// frame: bounded, and stopped at the first NUL like the C string it claims to
// be. A null pointer is simply "nothing to say".
std::string copyField(const char* s, std::size_t cap) {
    if (s == nullptr) {
        return std::string();
    }
    std::size_t n = 0;
    while (n < cap && s[n] != '\0') {
        ++n;
    }
    return std::string(s, n);
}
}  // namespace

TrackInfoCache::~TrackInfoCache() { detach(); }

void TrackInfoCache::attach(const CascadeTrackInfoApi* api) {
    if (api == api_) {
        return;
    }
    detach();
    if (api == nullptr) {
        return;
    }
    void* h = api->create();
    if (h == nullptr) {
        return;
    }
    api_ = api;
    handle_ = h;
    pollBuf_.assign(kPollBufBytes, '\0');
}

void TrackInfoCache::detach() {
    if (api_ != nullptr && handle_ != nullptr) {
        api_->destroy(handle_);
    }
    api_ = nullptr;
    handle_ = nullptr;
    cache_.clear();
    age_.clear();
    partial_.clear();
}

const TrackInfoCache::Info* TrackInfoCache::get(const std::string& id, std::uint32_t kind) {
    if (!active() || id.empty()) {
        return nullptr;
    }
    auto it = cache_.find(id);
    if (it != cache_.end()) {
        return &it->second;
    }

    CascadeTrackInfo t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTrackInfo));
    const std::int32_t got = api_->get_info(handle_, id.c_str(), kind, &t);
    if (got == CASCADE_INFO_PENDING) {
        // Deliberately not recorded: the next frame asks again, which is what
        // tells the plugin the answer is still wanted.
        return nullptr;
    }

    Info info;
    if (got == CASCADE_INFO_READY) {
        info.known = true;
        info.registration = copyField(t.registration, kMaxFieldBytes);
        info.typeCode = copyField(t.typeCode, kMaxFieldBytes);
        info.typeName = copyField(t.typeName, kMaxFieldBytes);
        info.manufacturer = copyField(t.manufacturer, kMaxFieldBytes);
        info.operatorName = copyField(t.operatorName, kMaxFieldBytes);
        info.country = copyField(t.country, kMaxFieldBytes);
        // The borrow ends the moment the copies are taken.
        api_->release_info(handle_, &t);
    }
    // MISSING (and anything unrecognised) is cached as known == false, which
    // is what stops the plugin being asked about that id ever again.

    if (cache_.size() >= kMaxEntries) {
        auto oldest = age_.begin();
        for (auto a = age_.begin(); a != age_.end(); ++a) {
            if (a->second < oldest->second) {
                oldest = a;
            }
        }
        cache_.erase(oldest->first);
        age_.erase(oldest);
    }
    age_[id] = ++seq_;
    auto [ins, _] = cache_.emplace(id, std::move(info));
    return &ins->second;
}

std::vector<std::string> TrackInfoCache::drainText() {
    std::vector<std::string> out;
    if (!active()) {
        return out;
    }
    // BOUNDED PER CALL. This runs on the GUI thread, once per frame, and the
    // loop used to run until the plugin said "nothing left". A plugin that
    // produces text faster than one frame can drain it never says that — a
    // decoder stuck in a logging loop is enough, a hostile one is trivial —
    // and the frame simply never ends. 32 polls of a 4 KB buffer is 128 KB of
    // status text per frame, orders of magnitude more than anything legitimate
    // emits; whatever is still queued is taken by the next frame, the same
    // trade the tile server's per-frame cap makes.
    for (std::size_t polls = 0; polls < kMaxPollsPerDrain; ++polls) {
        const std::int32_t n = api_->poll_text(handle_, pollBuf_.data(), pollBuf_.size());
        if (n <= 0) {
            break;
        }
        const std::size_t got = std::min(static_cast<std::size_t>(n), pollBuf_.size());
        partial_.append(pollBuf_.data(), got);
        for (;;) {
            const std::size_t nl = partial_.find('\n');
            if (nl == std::string::npos) {
                break;
            }
            if (nl > 0) {
                out.push_back(partial_.substr(0, nl));
            }
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
