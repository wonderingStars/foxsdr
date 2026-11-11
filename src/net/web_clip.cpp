// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_clip.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <system_error>

#include "net/web_bookmarks.hpp"  // validViewerToken: one token rule for every per-viewer model

namespace cascade::net {
namespace {

bool reservedDeviceName(const std::string& s) {
    std::string u;
    for (const char c : s) { u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c)))); }
    if (u == "CON" || u == "PRN" || u == "AUX" || u == "NUL") { return true; }
    if (u.size() == 4 && (u.compare(0, 3, "COM") == 0 || u.compare(0, 3, "LPT") == 0) && u[3] >= '0' && u[3] <= '9') { return true; }
    return false;
}

std::string lowerPathString(const std::filesystem::path& p) {
    std::string s = p.generic_string();
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

const char* clipResultText(ClipResult r) {
    switch (r) {
        case ClipResult::Ok: return "ok";
        case ClipResult::BadToken: return "the viewer token is not valid";
        case ClipResult::BadRoot: return "the recordings folder is not set";
        case ClipResult::BadLength: return "a clip must be between 1 and 300 seconds";
        case ClipResult::TooBig: return "that clip would be larger than the clip size limit";
        case ClipResult::ViewerBusy: return "you already have a clip recording";
        case ClipResult::HostBusy: return "the receiver is recording the most clips it allows; try again shortly";
        case ClipResult::TooManyViewers: return "too many viewers are holding clips";
        case ClipResult::NotFound: return "no such clip";
        case ClipResult::NotReady: return "that clip is still recording";
        case ClipResult::Failed: return "that clip did not record properly";
        case ClipResult::PathEscapes: return "the clip's file is not in the recordings folder";
    }
    return "unknown";
}

std::string sanitiseClipName(const std::string& in) {
    std::string out;
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        const char put = ok ? ch : '_';
        if (put == '_' && !out.empty() && out.back() == '_') { continue; }
        out.push_back(put);
        if (out.size() >= kMaxClipNameChars * 4) { break; }  // bounded work on hostile input
    }
    // Trim '_' and '-' from the ends, so a name never starts with '-' (an option look-alike) or ends in a separator.
    while (!out.empty() && (out.back() == '_' || out.back() == '-')) { out.pop_back(); }
    std::size_t lead = 0;
    while (lead < out.size() && (out[lead] == '_' || out[lead] == '-')) { ++lead; }
    out.erase(0, lead);
    if (out.size() > kMaxClipNameChars) { out.resize(kMaxClipNameChars); }
    while (!out.empty() && (out.back() == '_' || out.back() == '-')) { out.pop_back(); }
    if (out.empty()) { return "clip"; }
    if (reservedDeviceName(out)) { out.push_back('_'); }
    return out;
}

bool pathIsUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    if (root.empty() || path.empty()) { return false; }
    const std::filesystem::path r = root.lexically_normal();
    const std::filesystem::path p = path.lexically_normal();
    if (!r.has_root_path() || !p.has_root_path()) { return false; }
    auto ri = r.begin();
    auto pi = p.begin();
    for (; ri != r.end(); ++ri, ++pi) {
        if (ri->empty()) { continue; }  // a trailing separator in the root
        if (pi == p.end()) { return false; }
        if (lowerPathString(*ri) != lowerPathString(*pi)) { return false; }
    }
    // Any ".." left in the remainder would mean lexically_normal could not resolve it (it only survives at the front): refuse.
    for (; pi != p.end(); ++pi) {
        if (*pi == "..") { return false; }
    }
    return true;
}

WebClipBroker::WebClipBroker(std::filesystem::path recordingsRoot) : root_(std::move(recordingsRoot)) {}

std::size_t WebClipBroker::inFlight() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (const auto& v : viewers_) {
        for (const Clip& c : v.second.clips) {
            if (!c.info.finished) { ++n; }
        }
    }
    return n;
}

ClipResult WebClipBroker::request(const std::string& viewerToken, const ClipRequest& req, ClipPlan& plan) {
    plan = ClipPlan{};
    if (!validViewerToken(viewerToken)) { return ClipResult::BadToken; }
    if (root_.empty() || !root_.is_absolute()) { return ClipResult::BadRoot; }
    if (!std::isfinite(req.seconds) || req.seconds < kMinClipSeconds || req.seconds > kMaxClipSeconds) { return ClipResult::BadLength; }
    if (req.sampleRateHz == 0 || req.sampleRateHz > 1000000 || req.bytesPerSample == 0 || req.bytesPerSample > 8 || req.channels == 0 ||
        req.channels > 8) {
        return ClipResult::TooBig;
    }
    const double bytes = std::ceil(req.seconds * static_cast<double>(req.sampleRateHz) * req.bytesPerSample * req.channels);
    if (bytes > static_cast<double>(kMaxClipBytes)) { return ClipResult::TooBig; }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = viewers_.find(viewerToken);
    std::size_t flying = 0;
    for (const auto& v : viewers_) {
        for (const Clip& c : v.second.clips) {
            if (!c.info.finished) { ++flying; }
        }
    }
    if (it != viewers_.end()) {
        for (const Clip& c : it->second.clips) {
            if (!c.info.finished) { return ClipResult::ViewerBusy; }
        }
    }
    if (flying >= kMaxClipsInFlight) { return ClipResult::HostBusy; }
    if (it == viewers_.end()) {
        if (viewers_.size() >= kMaxClipViewers) { return ClipResult::TooManyViewers; }
        it = viewers_.emplace(viewerToken, Viewer{}).first;
    }

    const std::uint64_t id = nextId_++;
    const std::string file = "web-clip-" + sanitiseClipName(req.name) + "-" + std::to_string(id) + ".wav";
    const std::filesystem::path path = (root_ / file).lexically_normal();
    // The file name is built from [A-Za-z0-9_-] and a counter, so it is always a direct child of the root; the check is the proof.
    if (!pathIsUnderRoot(root_, path)) { return ClipResult::PathEscapes; }

    Clip c;
    c.info.clipId = id;
    c.info.fileName = file;
    c.path = path;
    it->second.clips.push_back(std::move(c));
    while (it->second.clips.size() > kMaxClipsPerViewer) {
        // Forget the oldest FINISHED clip; the in-flight one is the newest and is never at the front unless alone.
        if (it->second.clips.front().info.finished) { it->second.clips.pop_front(); } else { break; }
    }
    plan.clipId = id;
    plan.path = path;
    plan.seconds = req.seconds;
    plan.estimatedBytes = static_cast<std::uint64_t>(bytes);
    return ClipResult::Ok;
}

ClipResult WebClipBroker::finished(const std::string& viewerToken, std::uint64_t clipId, std::uint64_t bytes, bool ok) {
    if (!validViewerToken(viewerToken)) { return ClipResult::BadToken; }
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = viewers_.find(viewerToken);
    if (it == viewers_.end()) { return ClipResult::NotFound; }
    for (Clip& c : it->second.clips) {
        if (c.info.clipId != clipId) { continue; }
        if (c.info.finished) { return ClipResult::NotFound; }  // a second finish for the same clip is not a clip
        c.info.finished = true;
        c.info.bytes = bytes;
        c.info.ok = ok && bytes <= kMaxClipBytes && bytes > 0;
        return c.info.ok ? ClipResult::Ok : ClipResult::Failed;
    }
    return ClipResult::NotFound;
}

void WebClipBroker::abandon(const std::string& viewerToken) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = viewers_.find(viewerToken);
    if (it == viewers_.end()) { return; }
    for (Clip& c : it->second.clips) {
        if (!c.info.finished) {
            c.info.finished = true;
            c.info.ok = false;
        }
    }
}

ClipResult WebClipBroker::resolveDownload(const std::string& viewerToken, std::uint64_t clipId, std::filesystem::path& path) const {
    path.clear();
    if (!validViewerToken(viewerToken)) { return ClipResult::BadToken; }
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = viewers_.find(viewerToken);
    if (it == viewers_.end()) { return ClipResult::NotFound; }
    for (const Clip& c : it->second.clips) {
        if (c.info.clipId != clipId) { continue; }
        if (!c.info.finished) { return ClipResult::NotReady; }
        if (!c.info.ok) { return ClipResult::Failed; }
        if (!pathIsUnderRoot(root_, c.path)) { return ClipResult::PathEscapes; }
        // When the file exists, follow links: a reparse point planted in the root must not lead out of it.
        std::error_code ec;
        if (std::filesystem::exists(c.path, ec)) {
            const std::filesystem::path real = std::filesystem::weakly_canonical(c.path, ec);
            const std::filesystem::path realRoot = std::filesystem::weakly_canonical(root_, ec);
            if (ec || !pathIsUnderRoot(realRoot, real)) { return ClipResult::PathEscapes; }
            path = real;
        } else {
            path = c.path;
        }
        return ClipResult::Ok;
    }
    return ClipResult::NotFound;
}

std::vector<ClipInfo> WebClipBroker::list(const std::string& viewerToken) const {
    std::vector<ClipInfo> out;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = viewers_.find(viewerToken);
    if (it == viewers_.end()) { return out; }
    for (const Clip& c : it->second.clips) { out.push_back(c.info); }
    return out;
}

void WebClipBroker::forgetViewer(const std::string& viewerToken) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = viewers_.find(viewerToken);
    if (it == viewers_.end()) { return; }
    // A clip still recording stays accounted for (call abandon() first to end it): forgetting it would free the viewer's slot and
    // the host's in-flight count while the recorder still writes.
    auto& clips = it->second.clips;
    clips.erase(std::remove_if(clips.begin(), clips.end(), [](const Clip& c) { return c.info.finished; }), clips.end());
    if (clips.empty()) { viewers_.erase(it); }
}

}  // namespace cascade::net
