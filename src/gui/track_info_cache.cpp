// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/track_info_cache.hpp"

#include "core/plugin_calls.hpp"

#include <algorithm>
#include <cstring>

namespace cascade::gui {

namespace {
constexpr std::size_t kPollBufBytes = 4096;

// (A string the plugin lent is read through fault::copyStringFromPlugin below - bounded, stopped at the first NUL like the C string it claims
// to be, a null pointer "nothing to say", and a pointer that cannot be read the plugin's fault rather than the process's.)
}  // namespace

TrackInfoCache::~TrackInfoCache() { detach(); }

void TrackInfoCache::attach(const CascadeTrackInfoApi* api, const std::string& pluginKey) {
    if (api == api_) {
        return;
    }
    detach();
    if (api == nullptr) {
        return;
    }
    cascade::core::fault::Slot* const slot = cascade::core::fault::slotFor(pluginKey);
    if (!pluginKey.empty() && cascade::core::fault::isQuarantined(slot)) {
        return;   // switched off this session
    }
    void* h = cascade::core::calls::create(slot, api);
    if (h == nullptr) {
        return;
    }
    api_ = api;
    handle_ = h;
    slot_ = slot;
    pollBuf_.assign(kPollBufBytes, '\0');
}

void TrackInfoCache::detach() {
    if (api_ != nullptr && handle_ != nullptr) {
        cascade::core::calls::destroy(slot_, api_, handle_);
    }
    api_ = nullptr;
    handle_ = nullptr;
    slot_ = nullptr;
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
    const std::int32_t got = cascade::core::calls::getInfo(slot_, api_, handle_, id.c_str(), kind, &t);
    if (got == CASCADE_INFO_PENDING) {
        // Deliberately not recorded: the next frame asks again, which is what
        // tells the plugin the answer is still wanted.
        return nullptr;
    }

    Info info;
    if (got == CASCADE_INFO_READY) {
        namespace fault = cascade::core::fault;
        bool readable = true;
        const auto field = [&](const char* s) {
            std::string v;
            // (the first string that cannot be read switches the plugin off, which refuses the reads after it)
            if (readable) { readable = fault::copyStringFromPlugin(slot_, fault::Entry::GetInfo, s, kMaxFieldBytes, v); }
            return v;
        };
        info.registration = field(t.registration);
        info.typeCode = field(t.typeCode);
        info.typeName = field(t.typeName);
        info.manufacturer = field(t.manufacturer);
        info.operatorName = field(t.operatorName);
        info.country = field(t.country);
        info.known = readable;
        // The borrow ends the moment the copies are taken.
        cascade::core::calls::releaseInfo(slot_, api_, handle_, &t);
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
        const std::int32_t n = cascade::core::calls::pollText(slot_, api_, handle_, pollBuf_.data(), pollBuf_.size());
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
