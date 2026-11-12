// sys_profiles.hpp - settings profiles (named sets of settings for a radio or a use), the switch plan, the radio-to-profile map, and a folder-sync planner.
//
// A PROFILE is a settings bundle (sys_bundle.hpp) kept under a name in the profiles folder as "<name>.foxsdr-profile". Because it is made by exportBundle it never contains a password,
// a token or the install identity. SWITCHING is therefore not "replace config.json with the profile's": that would wipe this machine's web password and anonymous id. planSwitch() builds
// the new config.json from the profile's and then CARRIES OVER this machine's own secret and identity keys from the config in force (the same key rules the bundle uses), and the result
// goes through the same plan / confirm-token / commit path as an import, so a switch never overwrites silently and keeps the previous files as "<name>.before-import-<stamp>".
//
// PER RADIO: RadioProfileMap maps a radio key to a profile name (the key is a model-level name such as "rtlsdr" or "airspyhf" - NEVER a serial number, which would put a device identifier on disk), validated and bounded
// when read from a file.
//
// MULTI-PC SYNC, without a service: the sync unit is the profile file and the user's own shared folder (a cloud-drive folder, a network share, a stick) carries it. planSync() is the pure three-way
// comparison against the last-synced hash of each profile: only one side changed -> copy that way; both changed to different contents -> CONFLICT (nothing is copied, the user chooses);
// same contents -> nothing. It never deletes. A sync service of our own (an account, a server) is an owner decision and is not built here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_PROFILES_HPP
#define CASCADE_CORE_SYS_PROFILES_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/sys_bundle.hpp"

namespace cascade::core::sys {

inline constexpr std::size_t kMaxProfiles = 64;

// 1-40 characters: letters, digits, space, underscore, hyphen; starts with a letter or digit; no trailing space; not a Windows device name.
bool validProfileName(const std::string& name);

struct ProfileInfo {
    std::string name;
    std::uint64_t bytes = 0;
    std::string sha256;   // of the file
};

// Writes <dir>/<name>.foxsdr-profile from the files (secrets and identity dropped by exportBundle). Refuses an invalid name, an existing profile unless overwrite, or more than kMaxProfiles.
bool saveProfile(const std::filesystem::path& dir, const std::string& name, const std::vector<BundleFile>& files, bool overwrite, const ExportOptions& opt, std::string& error);
std::vector<ProfileInfo> listProfiles(const std::filesystem::path& dir);   // sorted by name; files of any other shape are not profiles
bool deleteProfile(const std::filesystem::path& dir, const std::string& name, std::string& error);

struct SwitchPlan {
    std::string error;
    ImportResult staged;                     // the new files, already merged with this machine's own keys
    ImportPlan plan;                         // against the settings folder in force
    std::vector<std::string> carriedKeys;    // top-level key names carried over from the current config (names only)
};

// Reads the profile, merges, and plans against `settingsDir` (the folder holding the current config.json). Writes nothing.
SwitchPlan planSwitch(const std::filesystem::path& profilesDir, const std::string& name, const std::filesystem::path& settingsDir);
// Applies it through commitImport (token required).
bool applySwitch(const SwitchPlan& sp, const std::filesystem::path& settingsDir, const std::string& token, const std::string& stamp, std::string& error);

class RadioProfileMap {
public:
    static constexpr std::size_t kMaxEntries = 64;
    static bool validRadioKey(const std::string& key);   // 1-48 chars of letters, digits and - _ . + (a run of 6 or more digits, or 12 or more hex digits, looks like a serial number or an id and is refused)
    bool set(const std::string& radioKey, const std::string& profileName);   // false if either is invalid or the map is full
    void remove(const std::string& radioKey) { map_.erase(radioKey); }
    std::string profileFor(const std::string& radioKey) const;               // "" = none
    std::size_t size() const { return map_.size(); }
    // Drops entries whose profile no longer exists.
    std::size_t prune(const std::vector<ProfileInfo>& existing);
    std::string toJson() const;
    static RadioProfileMap fromJson(const std::string& text);   // never throws; bad entries are dropped, the rest kept (bounded)

private:
    std::map<std::string, std::string> map_;
};

struct SyncEntry {
    std::string name;
    std::string sha256;
    std::int64_t modified = 0;   // seconds; informational only (the decision is by hash)
};
enum class SyncAction { None, PushToShared, PullFromShared, Conflict };
struct SyncItem {
    std::string name;
    SyncAction action = SyncAction::None;
};
// `base` = the hash each profile had at the last successful sync (by name); an absent name means never synced.
std::vector<SyncItem> planSync(const std::vector<SyncEntry>& local, const std::vector<SyncEntry>& shared, const std::map<std::string, std::string>& base);

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_PROFILES_HPP
