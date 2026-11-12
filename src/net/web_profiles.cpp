// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_profiles.hpp"

#include <algorithm>
#include <cctype>

#include "net/web_bookmarks.hpp"  // validBookmarkText: one text rule for every per-viewer model

namespace cascade::net {
namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

const char* profileResultText(ProfileResult r) {
    switch (r) {
        case ProfileResult::Ok: return "ok";
        case ProfileResult::BadList: return "the profile list is not valid";
        case ProfileResult::Unknown: return "no such profile";
        case ProfileResult::AlreadyActive: return "that profile is already active";
        case ProfileResult::Busy: return "a profile switch is already in progress";
        case ProfileResult::RateLimited: return "profiles can be switched only every two seconds";
        case ProfileResult::NoSwitchInFlight: return "no profile switch is in progress";
    }
    return "unknown";
}

ProfileResult WebProfileBook::setNames(const std::vector<std::string>& names, const std::string& active) {
    if (names.size() > kMaxProfiles) { return ProfileResult::BadList; }
    std::vector<std::string> seen;
    for (const std::string& n : names) {
        if (!validBookmarkText(n, kMaxProfileName)) { return ProfileResult::BadList; }
        const std::string l = lower(n);
        if (std::find(seen.begin(), seen.end(), l) != seen.end()) { return ProfileResult::BadList; }
        seen.push_back(l);
    }
    std::lock_guard<std::mutex> lk(mu_);
    names_ = names;
    active_ = std::find(names_.begin(), names_.end(), active) != names_.end() ? active : std::string();
    // A switch in flight to a profile that is gone cannot complete into the list.
    if (inFlight_ && std::find(names_.begin(), names_.end(), pending_) == names_.end()) { inFlight_ = false; }
    return ProfileResult::Ok;
}

std::vector<ProfileEntry> WebProfileBook::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ProfileEntry> out;
    for (const std::string& n : names_) { out.push_back(ProfileEntry{n, n == active_}); }
    return out;
}

ProfileResult WebProfileBook::requestSwitch(const std::string& name, std::int64_t nowMs, std::string& canonicalName) {
    canonicalName.clear();
    std::lock_guard<std::mutex> lk(mu_);
    // A switch that never got an answer stops blocking after the timeout.
    if (inFlight_ && nowMs - startedMs_ >= kSwitchTimeoutMs) { inFlight_ = false; }
    const auto it = std::find(names_.begin(), names_.end(), name);
    if (it == names_.end()) { return ProfileResult::Unknown; }
    if (*it == active_) { return ProfileResult::AlreadyActive; }
    if (inFlight_) { return ProfileResult::Busy; }
    if (everStarted_ && nowMs - startedMs_ < kMinSwitchGapMs) { return ProfileResult::RateLimited; }
    inFlight_ = true;
    everStarted_ = true;
    pending_ = *it;
    startedMs_ = nowMs;
    canonicalName = *it;
    return ProfileResult::Ok;
}

ProfileResult WebProfileBook::completeSwitch(bool ok, std::int64_t nowMs) {
    (void)nowMs;
    std::lock_guard<std::mutex> lk(mu_);
    if (!inFlight_) { return ProfileResult::NoSwitchInFlight; }
    inFlight_ = false;
    if (ok && std::find(names_.begin(), names_.end(), pending_) != names_.end()) { active_ = pending_; }
    return ProfileResult::Ok;
}

bool WebProfileBook::switching(std::int64_t nowMs) const {
    std::lock_guard<std::mutex> lk(mu_);
    return inFlight_ && nowMs - startedMs_ < kSwitchTimeoutMs;
}

}  // namespace cascade::net
