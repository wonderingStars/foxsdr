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

#include "engine/wire_c_power.hpp"
#include "core/rds_log.hpp"
#include "engine/wire_c_cat.hpp"
#include "engine/wire_c_ham.hpp"
#include "engine/wire_c_net.hpp"
#include "engine/wire_c_sys.hpp"
#include "engine/s4_outputs.hpp"
#include "engine/s4b_servers.hpp"
#include "engine/s4b_swl.hpp"

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

// True for a key whose value lives in the protected store and is never published (the lane S4 outputs' password and webhook address).
bool isSecretKey(const std::string& key);

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
    const std::string& secretNote() const { return secretNote_; }
    const std::filesystem::path& path() const { return path_; }
    // Every key and its value, registry order.
    void each(const std::function<void(const std::string&, const std::string&)>& fn) const;

private:
    bool save();
    std::filesystem::path path_;
    std::map<std::string, std::string> values_;   // only keys that differ from their default are stored
    std::string error_;
    std::string secretNote_;   // what the protected store could not open
    std::uint64_t version_ = 1;
    mutable std::mutex m_;
};

// Everything wiring lane C keeps for one engine. The engine creates it on first use (lazily, so an engine that never meets a feature of this
// lane builds nothing and reads no file) from the per-user folder. Items add their own members here as they are wired.
class State {
public:
    State(std::filesystem::path folder, std::filesystem::path dataDir) : folder_(std::move(folder)), dataDir_(std::move(dataDir)), settings_(folder_ / "wirec.json"), sys_(folder_, dataDir_) {}
    const std::filesystem::path& folder() const { return folder_; }
    SysOps& sys() { return sys_; }
    QsoBook& qsos() { return qsoBook_; }
    cascade::engine::s4b::SwlBook& swl() { return swlBook_; }   // screen lane S4b: the listening (SWL) log
    std::string swlError, swlExportLast;
    std::uint64_t listingVersion = 0;        // screen lane S4b: the settings version the public listing was last built from
    bool listingLive = false;
    std::string listingPreview, listingProblem;   // the exact document the web page serves (empty while off) and what is wrong with the words
    CatEmulation catEmu;                  // CAT emulation for logging programs (row L7)
    cascade::core::rdslog::Logger rdsLog; // the RDS log (engine lane E1): a worker of its own, started only while the user's switch is on
    std::uint64_t rdsLogSeen = 0;          // what the last published fact block counted of the log (lines, drops, the error)
    std::string rdsLogApplied = "0||rds_{date}";   // the settings it was last configured with (the defaults: nothing to do at the first pass)
    // the carrying-out of a confirmed open (plugin_abi.h request_open_url / _library_folder); empty = the system's own (wire_c_open.hpp). TESTS set it.
    std::function<bool(const std::string& what, bool isFolder, std::string& why)> openTarget;
    std::string openLast;   // the last confirmed open's outcome, a sentence
    std::string catEmuApplied = "0|kenwood-ts480|0|0";   // the settings it was last configured with (the defaults: nothing to do at the first pass)
    std::uint64_t catEmuSeen = 0;         // clients and commands the last published fact block counted
    void shutdown() {                     // the engine's teardown: every listener and session this lane holds is let go
        catEmu.stop();
        rdsLog.stop();
        outputs.stop();
        servers.stop();
        radioInfo.configure(false, "127.0.0.1", 0, false);
    }
    cascade::engine::s4b::ServerSet servers;   // screen lane S4b: the servers other programs connect to (audio over TCP ...), each off until the user turns it on
    std::uint64_t serversSettingsVersion = 0;  // the settings version the servers were last configured from
    std::string serversSig;                    // what the last published servers facts counted
    bool serversHooked = false;                // the engine's hooks were given to the servers
    cascade::engine::s4::Hub outputs;     // screen lane S4: the decoded-text feed sent to MQTT / a webhook, and the keyword alerts (off until the user turns one on)
    std::uint64_t outputsSeq = 0;         // the host event ring's sequence the outputs have read up to
    std::uint64_t outputsSettingsVersion = 0;   // the settings version the outputs were last configured from
    std::int64_t outputsInfluxMs = 0;     // the clock the level output's interval is counted on
    std::string outputsStatSig;           // what the last published outputs facts counted
    bool outputsLive = false;             // the outputs were on at the last pass (the first pass after they come on starts from the ring's present end: no backlog is sent)
    RadioInfoFollow radioInfo;            // the radio information followed (row H6)
    std::string radioInfoApplied = "0|127.0.0.1|0|0";   // the settings it was last configured with (the defaults: nothing to apply at the first pass, so no counter moves)
    std::string hamError, hamExportLast, hamCabrilloProblems;
    // What the system operations last said (facts fact.sys.*): strings the ops fill and the engine publishes.
    std::string sysError;
    Settings& settings() { return settings_; }
    const Settings& settings() const { return settings_; }

    // The power and resource work (wire_c_power.hpp), made on first use from `powerSeams` (the real ones in the program; a test sets them before the first pass).
    PowerSeams powerSeams;
    Power& power() {
        std::lock_guard<std::mutex> lk(powerMu_);
        if (!power_) { power_ = std::make_unique<Power>(powerSeams); }
        return *power_;
    }
    bool hasPower() const {
        std::lock_guard<std::mutex> lk(powerMu_);
        return power_ != nullptr;
    }
    bool powerStarted = false;            // the worker and the watcher are started (control pass only)
    std::uint64_t perfSeen = 0;           // the last perf version the engine's fingerprint counted
    std::uint64_t resumeActions = 0;      // resume actions the engine has carried out
    std::string corsApplied;              // the web.cors_origins text the web server was last given ("" = off, the default)
    bool metricsApplied = false;          // whether the metrics page was last switched on
    std::int64_t metricsLastMs = 0;       // when the engine last published a snapshot to it
    std::uint64_t eventsTallied = 0;      // the host event ring's sequence the decode tally has counted up to
    std::map<std::string, std::uint64_t> decodeTally;   // events per plugin since start (a count of lines, never their text)

private:
    std::filesystem::path folder_;
    std::filesystem::path dataDir_;
    Settings settings_;
    SysOps sys_;
    QsoBook qsoBook_{folder_ / "qso-log.json"};
    cascade::engine::s4b::SwlBook swlBook_{folder_ / "swl-log.json"};
    mutable std::mutex powerMu_;
    std::unique_ptr<Power> power_;
};

// The text a value is normalised to ("1"/"0" for a Bool, the shortest round-trip number for Int / Real). Used by set() and by the tests.
bool normalise(const Spec& s, const std::string& in, std::string& out, SetResult& bad, std::string& why);

}  // namespace cascade::engine::wirec
