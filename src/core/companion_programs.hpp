// companion_programs.hpp - THE COMPANION APPLICATIONS' PROGRAMS: what the store shows about a companion, how its program is obtained (downloaded by the website copy, found
// inside the package by the Microsoft Store copy), and the check that stops a program whose bytes are not the ones the catalogue (or the package) named.
//
// WHAT A COMPANION IS. A companion application is another person's open-source decoder program (GPL and similar) that a small FoxSDR bridge plugin starts as a separate
// process and talks to over a pipe. The plugin catalogue lists the bridges on a shelf (category "companions") and each row carries a `companion` object: the upstream
// project, the licence as the row states it, the plain sentence saying why it is a separate program, the address of its source, and `program` (file, url, sha256, status).
// The bridge looks for its program in the folder the user named, in its own folder and in a `helpers` folder under it (the plugins repository's helper_bridge); this file
// puts the verified program in `<plugins folder>/helpers/`.
//
// THE TWO COPIES.
//   * the WEBSITE copy downloads the program when the bridge is installed (https only, size cap, SHA-256 against the catalogue's, a program image of this machine's kind),
//     as one transaction with the bridge: a failure leaves nothing behind. A row whose program is not published (empty address or hash, or status not "published") is
//     shown as not available and cannot be fitted: nothing is fetched, nothing is promised.
//   * the MICROSOFT STORE copy (runningInPackage()) NEVER downloads a program (Store policy 10.2.2 and 10.2.3 are why): the package holds the programs under
//     `<package root>/companions/` with a list `companions.json` (written by installer/msix/build-msix.ps1: id, file, path, sha256) and the host copies the program
//     from the package into the helpers folder, after checking it against the package's own list. No transport is ever asked.
//
// THE CHECK BEFORE A COMPANION RUNS (checkBeforeStart). A program is run only if its bytes are the SHA-256 recorded when it was installed (the catalogue's hash, checked
// at download; the package list's hash, for a packaged copy). The record is `companions.json` beside `installed.json`. The check runs when the program is installed AND
// every time the host builds the plugin's decoder instance (PluginRunner), i.e. before every start; a program that changed on disk, vanished, or never matched is refused
// with a plain sentence and the refusal is counted on the companion fault path (kind BREAK). WHAT IT DOES NOT COVER: a restart of the program that the bridge itself
// makes inside one running instance (after a crash) is the bridge's own, which re-checks the program's size and time every two seconds and stops with "broken" when they
// change (plugins repository, helper_bridge); the host does not see those restarts.
//
// WHAT PROTECTS THE HASH, AND WHAT DOES NOT. The hash comes from the catalogue, which is fetched over https from the official address: someone who can alter the catalogue
// in transit (a TLS-intercepting proxy with a certificate this PC trusts) or at its source (the catalogue repository's account) can name another program and its hash;
// the hash then proves only that the download is the file the catalogue named, not that FoxSDR's owner approved that file. The plugin DLLs of the catalogue stand on the
// same footing today (the catalogue is not signed). Closing it is the owner's key: a catalogue row may carry `companion.program.signature`, an envelope of
// core/sys_manifest_sig.hpp over the statement "foxsdr-companion-program v1 / id / file / sha256", checked against ownerTrustedKeys(). That list is EMPTY until the owner
// fills in his public key (sys_manifest_sig.cpp kOwnerKeyHex), so the check is BUILT AND INERT: with no key a program is accepted on its hash alone, with a key a missing or
// wrong signature refuses the download. The PACKAGE copy is protected by the package's own signature (the Store signs the package) and needs none of this.
//
// THREADS: all functions are plain and reentrant except where noted; the install transaction runs on the install worker; checkBeforeStart runs on whatever thread builds a
// plugin's instances (the engine's control thread) and takes no lock of its own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/plugin_repo_data.hpp"
#include "core/sys_ed25519.hpp"

namespace cascade::core::companion {

inline constexpr std::uint64_t kMaxProgramBytes = 128ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxSignatureBytes = 4096;

// ---- THE CATALOGUE'S ROW ---------------------------------------------------------------------------------------------------------------------------------------------

// Reads the entry's `companion` object (and the identifier sentence out of its description) into e.companion. NEVER fails the catalogue: a field of the wrong type is dropped
// and an unusable program block makes the program "not available" (program.usable false), so an older store that ignores the key and a newer one that reads it never
// disagree about what is safe to fetch. An entry without the object leaves e.companion.present false.
void parseCompanionFields(const nlohmann::json& entryJson, PluginCatalogEntry& e);

// A bare program file name this store may write ("" = fine): letters, digits, '.', '_' and '-', ends in ".exe" on Windows, no separator, drive letter, "..", never a ".dll".
std::string validProgramName(const std::string& name);

// The sentence a row says about identifiers, cut out of its description ("" when the row has none).
std::string identifierSentence(const std::string& description);

// ---- THE CHECK -------------------------------------------------------------------------------------------------------------------------------------------------------

enum class Check { Ok, NoHash, Missing, Unreadable, Mismatch, NotAProgram };
struct CheckResult {
    Check kind = Check::Missing;
    std::string actual;   // the digest read, when it was read
};
// Hashes `file` and compares it with `expectedSha256` (64 hex digits, either case). An empty or malformed expectation is NoHash: nothing is ever "equal to nothing".
CheckResult checkProgram(const std::filesystem::path& file, const std::string& expectedSha256);
// The plain sentence for a failed check, naming the program file.
std::string refusalSentence(Check c, const std::string& programFile);

// The owner's signature on a catalogue row's program (inert until a key is supplied). The statement that is signed:
std::string signedStatement(const std::string& id, const std::string& file, const std::string& sha256);
enum class Signature { NotEnforced, Verified, Missing, Refused };
void setSignatureKeysForTest(bool use, const std::vector<sys::Ed25519PublicKey>& keys);   // the test seam: `use` false = the product's ownerTrustedKeys()
// `trustedKeys` is ownerTrustedKeys() in the product and a test key in tests. Empty list: NotEnforced whatever the row carries.
Signature checkRowSignature(const std::string& id, const ProgramInfo& program, const std::vector<sys::Ed25519PublicKey>& trustedKeys);

// ---- THE PLACES ------------------------------------------------------------------------------------------------------------------------------------------------------

struct Places {
    bool packaged = false;                    // running from an MSIX package: no program is ever downloaded
    std::filesystem::path packageDir;         // <package root>/companions
    std::filesystem::path pluginsDir;
};
// `pluginsDir/helpers`, where the bridges look.
std::filesystem::path programFolder(const std::filesystem::path& pluginsDir);
// The places of this process: runningInPackage(), the folder beside the executable, `pluginsDir`. A test sets the override first.
Places placesFor(const std::string& pluginsDir);
void setPackagedForTest(int state, const std::filesystem::path& packageDir = {});   // -1 = the real answer, 0 = not packaged, 1 = packaged with this companions folder

struct PackageProgram {
    std::string id, file, path, sha256;       // path: relative to the package's companions folder
    std::uint64_t size = 0;
};
// Reads `<packageDir>/companions.json`. False with `error` when the list is absent or malformed (no program is then "in the package").
bool readPackageList(const std::filesystem::path& packageDir, std::vector<PackageProgram>& out, std::string& error);

// ---- WHAT THE SHELF SAYS ---------------------------------------------------------------------------------------------------------------------------------------------

enum class State {
    NotAvailable,   // the catalogue has no published program (website copy) or the package holds none (Store copy)
    InPackage,      // the Store copy holds the program inside its package; fitting the bridge copies it into place
    Download,       // the website copy: the catalogue gives an address and a hash; fitting the bridge downloads it
    Installed,      // the program is in place and is the one recorded
    Changed,        // a program is recorded but the file is gone or is not the size recorded
};
State stateOf(const PluginCatalogEntry& e, const Places& places, const std::vector<PackageProgram>& packageList, const std::string& pluginFile);
// The state of one catalogue entry in THIS copy, from the places (placesFor), the package list (read only when packaged) and the install record. NotAvailable for an entry
// with no companion object and for one from a plugin site.
State stateForEntry(const PluginCatalogEntry& e, const std::string& pluginsDir);
// The bridge's file name this machine's build installs under ("" with no build for this machine or a name the store refuses): the key of its record.
std::string bridgeFileOf(const PluginCatalogEntry& e);
const char* stateWord(State s);          // "unavailable" | "package" | "download" | "installed" | "changed"
// One English sentence for the state (the window translates it where it draws).
std::string stateSentence(State s);

// ---- THE INSTALL TRANSACTION -----------------------------------------------------------------------------------------------------------------------------------------
// stage() obtains the program into a staging folder under the helpers folder and verifies it (download + hash, or copy from the package + hash); commit() moves it
// into `helpers/` (a program already there is kept as a backup); rollback() puts it back; finish() drops the backup and the staging folder. record() writes the
// companions.json line once the bridge is in place.
class Txn {
public:
    explicit Txn(std::filesystem::path pluginsDir);
    ~Txn();
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    // `error` is the store's own kind of sentence ("\"<name>\": ..."). A row with no companion object, or one from a plugin site, stages nothing and returns true.
    bool stage(const PluginCatalogEntry& e, std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error);
    bool commit(std::string& error);
    void rollback();
    void finish();
    bool active() const { return active_; }
    // Which hash was verified and from where ("download" | "package").
    const std::string& sha256() const { return sha_; }
    const std::string& via() const { return via_; }
    const std::string& fileName() const { return file_; }
    std::uint64_t size() const { return size_; }

private:
    std::filesystem::path dir_, stageDir_, staged_, target_, backup_;
    std::string sha_, via_, file_;
    std::uint64_t size_ = 0;
    bool active_ = false, movedIn_ = false, committed_ = false;
};

// ---- THE RECORD (companions.json beside installed.json) --------------------------------------------------------------------------------------------------------------
struct Record {
    std::string plugin;    // the bridge's file name in the plugins folder
    std::string id;        // the catalogue id
    std::string file;      // the program's file name (in helpers/)
    std::string sha256;
    std::uint64_t size = 0;
    std::string via;       // "download" | "package"
};
bool recordProgram(const std::string& pluginsDir, const Record& r, std::string& error);
bool readRecord(const std::string& pluginsDir, const std::string& pluginFile, Record& out);
// Forgets the record of a plugin file without touching any program (the failure path of an install that has already put the program in place).
bool forgetRecord(const std::string& pluginsDir, const std::string& pluginFile);
// Deletes the program recorded for a plugin file (when no other bridge's record names the same file) and forgets the record. True when nothing is left.
bool removeProgram(const std::string& pluginsDir, const std::string& pluginFile, std::string& error);

// ---- BEFORE EVERY START ----------------------------------------------------------------------------------------------------------------------------------------------
struct StartVerdict {
    bool allowed = true;
    std::string sentence;      // English, plain, names the program; empty when allowed
};
// `pluginPath`: the bridge's absolute path (LoadedPlugin::path). A plugin with no record is not a catalogue companion: allowed (a program the user chose by hand is theirs).
// A packaged copy first makes sure the helpers copy IS the package's program (re-copied from the package when it is missing or differs), never downloading.
// A refusal is counted with noteHostFault(CASCADE_FAULT_BREAK).
StartVerdict checkBeforeStart(const std::string& pluginPath);

// ---- THE FAULT PATH --------------------------------------------------------------------------------------------------------------------------------------------------
// A fault the HOST found (the check above), counted like the plugin's own report_fault: kind 1..4 (CASCADE_FAULT_*). Nothing but the kind and a count is kept.
void noteHostFault(std::uint32_t kind);
// Adds what the host found since the last call to `sums` (index kind-1) and clears it; called by companion::drain.
void takeHostFaults(std::array<std::uint64_t, 4>& sums);

}  // namespace cascade::core::companion
