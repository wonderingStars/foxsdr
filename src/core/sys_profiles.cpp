// sys_profiles.cpp - see sys_profiles.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_profiles.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "core/sys_hash.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core::sys {
namespace {

const char* const kProfileExt = ".foxsdr-profile";

std::string upperOf(std::string s) {
    for (auto& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

bool reservedDevice(const std::string& name) {
    std::string stem = upperOf(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') { stem.pop_back(); }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") { return true; }
    return stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' && stem[3] <= '9';
}

bool slurp(const fs::path& p, std::size_t cap, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return false; }
    const auto size = fs::file_size(p, ec);
    if (ec || size > cap) { return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// The keys that describe THIS machine rather than a way of working: secrets, identity, the usage-reporting consent, and everything that decides what listens on the network.
bool machineLocalKey(const std::string& k) {
    if (secretKeyName(k) || identityKeyName(k)) { return true; }
    for (const char* n : {"telemetryEnabled", "webEnabled", "webBindAddress", "webPort", "catEnabled", "catBindAll", "catPort", "tciEnabled", "tciBindAll", "tciPort", "udpAudioEnabled", "udpAudioHost", "udpAudioPort"}) {
        if (k == n) { return true; }
    }
    return false;
}

fs::path profilePath(const fs::path& dir, const std::string& name) { return dir / (name + kProfileExt); }

}  // namespace

bool validProfileName(const std::string& name) {
    if (name.empty() || name.size() > 40) { return false; }
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) { return false; }
    if (name.back() == ' ') { return false; }
    for (unsigned char c : name) { if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-')) { return false; } }
    return !reservedDevice(name);
}

bool saveProfile(const fs::path& dir, const std::string& name, const std::vector<BundleFile>& files, bool overwrite, const ExportOptions& opt, std::string& error) {
    error.clear();
    if (!validProfileName(name)) { error = "that is not a usable profile name"; return false; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = "cannot make the profiles folder"; return false; }
    const fs::path target = profilePath(dir, name);
    const bool exists = fs::exists(target, ec);
    if (exists && !overwrite) { error = "a profile with that name already exists"; return false; }
    if (!exists && listProfiles(dir).size() >= kMaxProfiles) { error = "too many profiles"; return false; }
    ExportReport rep;
    ExportOptions o = opt;
    o.stripIdentity = true;   // a profile never carries this install's identity
    const std::string text = exportBundle(files, o, rep);
    if (rep.error != BundleError::None) { error = std::string("cannot make the profile: ") + bundleErrorName(rep.error) + " " + rep.detail; return false; }
    const fs::path tmp = dir / (name + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { error = "cannot write the profile"; fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) { error = "cannot put the profile in place"; fs::remove(tmp, ec); return false; }
    return true;
}

std::vector<ProfileInfo> listProfiles(const fs::path& dir) {
    std::vector<ProfileInfo> out;
    std::error_code ec;
    const std::string ext = kProfileExt;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) { continue; }
        const std::string fn = it->path().filename().string();
        if (fn.size() <= ext.size() || fn.compare(fn.size() - ext.size(), ext.size(), ext) != 0) { continue; }
        const std::string name = fn.substr(0, fn.size() - ext.size());
        if (!validProfileName(name)) { continue; }
        std::string bytes;
        ProfileInfo pi;
        pi.name = name;
        if (slurp(it->path(), kBundleMaxTextBytes, bytes)) { pi.bytes = bytes.size(); pi.sha256 = sha256Hex(bytes); }
        out.push_back(std::move(pi));
        if (out.size() >= kMaxProfiles * 2) { break; }
    }
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) { return a.name < b.name; });
    return out;
}

bool deleteProfile(const fs::path& dir, const std::string& name, std::string& error) {
    error.clear();
    if (!validProfileName(name)) { error = "that is not a usable profile name"; return false; }
    std::error_code ec;
    const fs::path p = profilePath(dir, name);
    if (!fs::is_regular_file(p, ec)) { error = "no such profile"; return false; }
    if (!fs::remove(p, ec) || ec) { error = "cannot delete the profile"; return false; }
    return true;
}

SwitchPlan planSwitch(const fs::path& profilesDir, const std::string& name, const fs::path& settingsDir) {
    SwitchPlan sp;
    if (!validProfileName(name)) { sp.error = "that is not a usable profile name"; return sp; }
    std::string text;
    if (!slurp(profilePath(profilesDir, name), kBundleMaxTextBytes, text)) { sp.error = "the profile cannot be read"; return sp; }
    sp.staged = parseBundle(text);
    if (sp.staged.error != BundleError::None) { sp.error = std::string("the profile is not usable: ") + bundleErrorName(sp.staged.error); return sp; }
    // Carry this machine's own keys over from the config in force.
    std::string currentText;
    json current;
    if (slurp(settingsDir / "config.json", kBundleMaxFileBytes, currentText)) {
        current = json::parse(currentText, nullptr, false);
        if (current.is_discarded() || !current.is_object()) { current = json::object(); }
    } else {
        current = json::object();
    }
    for (auto& f : sp.staged.files) {
        if (f.name != "config.json") { continue; }
        json fresh = json::parse(f.bytes, nullptr, false);
        if (fresh.is_discarded() || !fresh.is_object()) { sp.error = "the profile's config is not an object"; return sp; }
        // The profile never decides what this machine keeps secret or listens on: drop the profile's own values for those keys, then take the machine's.
        std::vector<std::string> drop;
        for (auto it = fresh.begin(); it != fresh.end(); ++it) { if (machineLocalKey(it.key())) { drop.push_back(it.key()); } }
        for (const auto& k : drop) { fresh.erase(k); }
        for (auto it = current.begin(); it != current.end(); ++it) {
            if (machineLocalKey(it.key())) { fresh[it.key()] = it.value(); sp.carriedKeys.push_back(it.key()); }
        }
        f.bytes = fresh.dump(2, ' ', false, json::error_handler_t::replace);
    }
    // The importer's "listeners were forced off" notes do not apply here: this machine's own network switches were just carried over.
    sp.staged.notes.clear();
    if (!sp.carriedKeys.empty()) { sp.staged.notes.push_back("This machine's own passwords, tokens, anonymous id and network switches were kept; the profile does not change them."); }
    sp.plan = planImport(sp.staged, settingsDir);
    return sp;
}

bool applySwitch(const SwitchPlan& sp, const fs::path& settingsDir, const std::string& token, const std::string& stamp, std::string& error) {
    if (!sp.error.empty()) { error = sp.error; return false; }
    return commitImport(sp.staged, settingsDir, token, stamp, error);
}

bool RadioProfileMap::validRadioKey(const std::string& key) {
    if (key.empty() || key.size() > 48) { return false; }
    std::size_t digitRun = 0, hexRun = 0;
    for (unsigned char c : key) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+')) { return false; }
        digitRun = std::isdigit(c) ? digitRun + 1 : 0;
        hexRun = std::isxdigit(c) ? hexRun + 1 : 0;
        if (digitRun >= 6 || hexRun >= 12) { return false; }   // looks like a serial number or an id: not a model name
    }
    return true;
}

bool RadioProfileMap::set(const std::string& radioKey, const std::string& profileName) {
    if (!validRadioKey(radioKey) || !validProfileName(profileName)) { return false; }
    if (map_.count(radioKey) == 0 && map_.size() >= kMaxEntries) { return false; }
    map_[radioKey] = profileName;
    return true;
}

std::string RadioProfileMap::profileFor(const std::string& radioKey) const {
    const auto it = map_.find(radioKey);
    return it == map_.end() ? std::string() : it->second;
}

std::size_t RadioProfileMap::prune(const std::vector<ProfileInfo>& existing) {
    std::set<std::string> names;
    for (const auto& p : existing) { names.insert(p.name); }
    std::size_t dropped = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (!names.count(it->second)) { it = map_.erase(it); ++dropped; } else { ++it; }
    }
    return dropped;
}

std::string RadioProfileMap::toJson() const {
    json j;
    j["version"] = 1;
    json m = json::object();
    for (const auto& kv : map_) { m[kv.first] = kv.second; }
    j["radios"] = std::move(m);
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

RadioProfileMap RadioProfileMap::fromJson(const std::string& text) {
    RadioProfileMap out;
    if (text.size() > 64 * 1024) { return out; }
    int depth = 0;
    for (char c : text) { if (c == '[' || c == '{') { if (++depth > 16) { return out; } } else if (c == ']' || c == '}') { --depth; } }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return out; }
    const auto r = j.find("radios");
    if (r == j.end() || !r->is_object()) { return out; }
    for (auto it = r->begin(); it != r->end(); ++it) {
        if (!it.value().is_string()) { continue; }
        out.set(it.key(), it.value().get<std::string>());
        if (out.size() >= kMaxEntries) { break; }
    }
    return out;
}

std::vector<SyncItem> planSync(const std::vector<SyncEntry>& local, const std::vector<SyncEntry>& shared, const std::map<std::string, std::string>& base) {
    std::map<std::string, const SyncEntry*> l, s;
    for (const auto& e : local) { l[e.name] = &e; }
    for (const auto& e : shared) { s[e.name] = &e; }
    std::set<std::string> names;
    for (const auto& kv : l) { names.insert(kv.first); }
    for (const auto& kv : s) { names.insert(kv.first); }
    std::vector<SyncItem> out;
    for (const auto& n : names) {
        SyncItem item;
        item.name = n;
        const auto li = l.find(n), si = s.find(n);
        const auto bi = base.find(n);
        const bool haveBase = bi != base.end();
        if (li != l.end() && si == s.end()) {
            item.action = (haveBase && bi->second == li->second->sha256) ? SyncAction::None : SyncAction::PushToShared;   // unchanged since the last sync and gone from shared: another PC deleted it; never resurrect, never delete
        } else if (li == l.end() && si != s.end()) {
            item.action = (haveBase && bi->second == si->second->sha256) ? SyncAction::None : SyncAction::PullFromShared;
        } else {
            const std::string& a = li->second->sha256;
            const std::string& b = si->second->sha256;
            if (a == b) { item.action = SyncAction::None; }
            else if (haveBase && a == bi->second) { item.action = SyncAction::PullFromShared; }
            else if (haveBase && b == bi->second) { item.action = SyncAction::PushToShared; }
            else { item.action = SyncAction::Conflict; }
        }
        out.push_back(item);
    }
    return out;
}

}  // namespace cascade::core::sys
