// plugin_repo_helper.hpp - GPL HELPER PROGRAMS in the Plugin store (wiring lane C, group 7; the contract is docs/gpl-helper-store-handoff.md of the plugins repository,
// branch oss/helperhost, which is the design this implements).
//
// A "helper" is a separate program (a GPL decoder) that a bridge plugin starts and feeds over a pipe; it is never linked into FoxSDR or into a plugin. The catalogue
// entry of a helper plugin carries, besides the bridge DLL, a `helper` object (name, version, licence, source address and commit) and, on every platform row, a list of
// COMPANION FILES: the helper program, its manifest (<stem>.helper.json) and its licence text (<stem>.LICENSE.txt). The store installs them beside the plugin as ONE
// TRANSACTION:
//   * every file is downloaded to a staging folder and checked (size cap, sha256; the helper program also must LOOK LIKE a program of this machine's kind: a Windows PE
//     image of the right machine, or an ELF of the right class and machine - a file that is not one never reaches the plugins folder; the manifest must name the helper
//     program as its `executable`);
//   * only when every file has passed, they are moved into place (a file that was there is kept as a backup until the plugin itself is in place), then the bridge DLL;
//     any failure puts everything back as it was: NOTHING is left behind;
//   * an update replaces all or none; removing the plugin removes its companions (they are listed in helpers.json, beside installed.json);
//   * the execute bit (POSIX) is set on the helper program only, after its hash matched; nothing here ever RUNS it - the bridge plugin does.
// `installAs` is a bare file name (no separator, drive letter or "..", never a ".dll", never a name the store itself uses), so a hostile index cannot write outside the
// plugins folder. A helper entry is OFFICIAL-CATALOGUE ONLY: a plugin site's helper entry is refused (a stranger's native program is a decision this store does not take
// for the user), and the install asks the user's separate consent (the op FOXAPP_OP_STORE_HELPER_CONSENT) on top of the licence acknowledgement.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/plugin_repo_data.hpp"

namespace cascade::core::helperstore {

inline constexpr std::size_t kMaxCompanionsPerRow = 8;
inline constexpr std::uint64_t kMaxHelperBytes = 128ull * 1024ull * 1024ull;   // the helper program
inline constexpr std::uint64_t kMaxCompanionTextBytes = 1ull * 1024ull * 1024ull;   // its manifest and licence text

// A bare file name this store may write beside a plugin ("" error = fine).
std::string validInstallAs(const std::string& name);

// Reads the entry's `helper` object and every platform row's `companions` from the index JSON of ONE entry (called by parseIndex after the platforms). Entry without a
// `helper` object: nothing is read (companions on a row without one are ignored: a half-understood helper document is refused whole, an absent one is not a helper).
// False with `error` for a malformed helper document: the whole catalogue is then refused, as for every security field.
bool parseHelperFields(const nlohmann::json& entryJson, PluginCatalogEntry& e, const std::string& where, std::string& error);

// Does the file look like a program of this kind of machine? os "windows" | "linux", arch "x64" | "arm64" | "x86". `error` names what is wrong.
bool looksLikeProgram(const std::filesystem::path& file, const std::string& os, const std::string& arch, std::string& error);

// ONE INSTALL'S STAGED FILES. stage() downloads and verifies; commit() moves them in (with backups); rollback() undoes a commit; finish() drops the backups and the staging folder.
class Txn {
public:
    explicit Txn(std::filesystem::path pluginsDir) : dir_(std::move(pluginsDir)) {}
    ~Txn();
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    bool stage(const PluginCatalogEntry& e, const PluginPlatform& p, std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error);
    bool commit(std::string& error);
    void rollback();
    void finish();
    // The names that were (or would be) installed, in the order of the row.
    const std::vector<std::string>& names() const { return names_; }

private:
    struct Staged {
        std::string name;
        std::filesystem::path staged;
        std::filesystem::path target;
        std::filesystem::path backup;   // "" when nothing was there
        bool movedIn = false;
        bool executable = false;
    };
    std::filesystem::path dir_;
    std::filesystem::path stageDir_;
    std::vector<Staged> files_;
    std::vector<std::string> names_;
    bool committed_ = false;
};

// helpers.json beside installed.json: plugin file name -> the companion names installed with it.
bool recordCompanions(const std::string& pluginsDir, const std::string& pluginFile, const std::vector<std::string>& names, std::string& error);
// Deletes the companions recorded for a plugin file and forgets them. True when nothing is left (or nothing was recorded).
bool removeCompanions(const std::string& pluginsDir, const std::string& pluginFile, std::string& error);
std::vector<std::string> companionsOf(const std::string& pluginsDir, const std::string& pluginFile);

// What the store shows (English; the pages translate where they draw it). Empty for an entry that is not a helper.
std::string rowLine(const PluginCatalogEntry& e);                       // "GPL helper - source: <address>"
std::string consentSentence(const PluginCatalogEntry& e);               // the reason an install is refused until the user has consented
std::vector<std::string> detailLines(const PluginCatalogEntry& e);      // the lines of the detail pane, before the install button

// The sentence an install is refused with while the user's consent for this entry has not been given, "" when the entry is not a helper.
extern const char* const kConsentRefusal;

}  // namespace cascade::core::helperstore
