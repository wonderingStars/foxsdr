// wire_c.hpp - the engine side of wiring lane C (system, accessibility, web, ham operations): one registry of settings, one op, one block of facts.
//
// THE SETTINGS ARE THEIR OWN FILE. Every key of this lane lives in "wirec.json" in the per-user folder (the folder config.json lives in: the
// redirected profile of a test, the portable folder of a stick). config.json is NOT touched, so it stays loadable by every earlier version and
// byte-identical for a user who never opens one of these features. A key that is absent has its default; a key whose text is wrong is
// refused when set and ignored (default kept) when read from a damaged file. NOTHING PERSONAL is ever a value: a callsign, an address, a
// frequency list or a token is not a setting of this registry, and the source-scan test for this file holds that line.
//
// ONE OP SETS THEM: FOXAPP_OP_WIREC_SETTING (0x8A00), text "<key>\x1F<value>" (the unit separator is the one the plugin-settings op uses). The
// engine answers OK (changed and saved), NO_CHANGE (already so), NOT_FOUND (no such key), BAD_ARGUMENT (the value is not of the key's kind) or
// OUT_OF_RANGE (a number or a choice outside the key's range). Every key is published as fact.wirec.<key> (LOCAL sessions only: the file is the
// operator's), and fact.wirec.version moves with any change.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::engine::wirec {

enum class Kind { Bool, Int, Real, Choice, Text };

struct Spec {
    const char* key;
    Kind kind;
    double lo;             // Int / Real: the lowest value; Text: 0
    double hi;             // Int / Real: the highest value; Text: the longest text in bytes
    const char* def;       // the default, as text ("0" / "1" for a Bool)
    const char* choices;   // Choice: the allowed values joined with '|', else ""
};

// The registry, in the order the keys were added. Never reordered.
const Spec* specs(std::size_t& count);
const Spec* findSpec(const std::string& key);

enum class SetResult { Changed, NoChange, NotFound, BadValue, OutOfRange, SaveFailed };

class Settings {
public:
    // The file read (once) from `path`; a missing file is every default, a damaged one is every default and error() says so.
    explicit Settings(std::filesystem::path path);
    void reload();
    // One key: its current value as text (the default when never set). "" for an unknown key.
    std::string get(const std::string& key) const;
    bool getBool(const std::string& key) const { return get(key) == "1"; }
    std::int64_t getInt(const std::string& key) const;
    double getReal(const std::string& key) const;
    SetResult set(const std::string& key, const std::string& value, std::string& why);
    std::uint64_t version() const { return version_; }
    const std::string& error() const { return error_; }
    const std::filesystem::path& path() const { return path_; }
    // Every key and its value, registry order.
    void each(const std::function<void(const std::string&, const std::string&)>& fn) const;

private:
    bool save();
    std::filesystem::path path_;
    std::map<std::string, std::string> values_;   // only keys that differ from their default are stored
    std::string error_;
    std::uint64_t version_ = 1;
    mutable std::mutex m_;
};

// Everything wiring lane C keeps for one engine. The engine creates it on first use (lazily, so an engine that never meets a feature of this
// lane builds nothing and reads no file) from the per-user folder. Items add their own members here as they are wired.
class State {
public:
    explicit State(std::filesystem::path folder) : folder_(std::move(folder)), settings_(folder_ / "wirec.json") {}
    const std::filesystem::path& folder() const { return folder_; }
    Settings& settings() { return settings_; }
    const Settings& settings() const { return settings_; }

private:
    std::filesystem::path folder_;
    Settings settings_;
};

// The text a value is normalised to ("1"/"0" for a Bool, the shortest round-trip number for Int / Real). Used by set() and by the tests.
bool normalise(const Spec& s, const std::string& in, std::string& out, SetResult& bad, std::string& why);

}  // namespace cascade::engine::wirec
