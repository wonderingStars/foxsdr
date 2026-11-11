// web_profiles.hpp - named profiles a viewer can list and switch to from the browser (feat/webmodels).
//
// WHAT THIS IS. A profile is a saved receiver setup the host already keeps by NAME (a band, a mode, a gain: the host owns what is inside).
// The web page today has none. This model holds only the list of NAMES the host publishes and decides whether a switch REQUEST from a viewer
// may go to the host: it never reads or writes a profile's contents and it never invents a name.
//
// RULES (tested in tests/test_web_profiles.cpp):
//   1. The published list is validated: every name is valid text (no control characters, valid UTF-8, 1..kMaxProfileName bytes, not blank)
//      and names that differ only in letter case are duplicates; the whole list is refused if it holds more than kMaxProfiles or any bad
//      name, so the page never shows a half-understood list. (An empty list is valid: no profiles.)
//   2. A switch names a profile EXACTLY as published (byte for byte). A name that is not in the list is refused, however close it is: the
//      viewer's text is only ever compared with the list, never passed on.
//   3. One switch at a time: while a switch is in flight, another is refused (Busy) until completeSwitch() reports the host's answer or the
//      in-flight switch times out (kSwitchTimeoutMs on the caller's clock).
//   4. A rate limit: at least kMinSwitchGapMs between the START of one switch and the next, so a page cannot flap the receiver.
//   5. Switching to the profile that is already active is answered AlreadyActive and does not go to the host.
//   6. Thread-safe; the clock is the caller's (a millisecond counter), so a test needs no sleeps.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::net {

inline constexpr std::size_t kMaxProfiles = 32;
inline constexpr std::size_t kMaxProfileName = 48;
inline constexpr std::int64_t kMinSwitchGapMs = 2000;
inline constexpr std::int64_t kSwitchTimeoutMs = 15000;

enum class ProfileResult {
    Ok,             // the switch may go to the host (it is now in flight)
    BadList,        // setNames: refused, the old list stays
    Unknown,        // not in the list
    AlreadyActive,
    Busy,           // a switch is in flight
    RateLimited,
    NoSwitchInFlight,  // completeSwitch with nothing pending
};

const char* profileResultText(ProfileResult r);

struct ProfileEntry {
    std::string name;
    bool active = false;
};

class WebProfileBook {
public:
    // The host publishes its profiles and which one is active ("" = none / unknown; a name not in the list is also treated as none).
    ProfileResult setNames(const std::vector<std::string>& names, const std::string& active);
    std::vector<ProfileEntry> list() const;

    // A viewer asks for `name`. Ok means the caller must now tell the host to switch to the returned canonical name (identical bytes to
    // the published one) and later call completeSwitch().
    ProfileResult requestSwitch(const std::string& name, std::int64_t nowMs, std::string& canonicalName);
    // The host's answer. On success the profile becomes active; on failure the active one is unchanged.
    ProfileResult completeSwitch(bool ok, std::int64_t nowMs);
    bool switching(std::int64_t nowMs) const;

private:
    mutable std::mutex mu_;
    std::vector<std::string> names_;
    std::string active_;
    bool inFlight_ = false;
    std::string pending_;
    std::int64_t startedMs_ = 0;
    bool everStarted_ = false;
};

}  // namespace cascade::net
