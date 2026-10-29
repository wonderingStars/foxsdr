// plugin_repo_data.hpp - the CATALOGUE'S DATA: the entry types core/plugin_repo.hpp fetches and parses (shell retirement R11a, 2026-10-29).
//
// PluginCatalogEntry, PluginPlatform, CatalogScreenshot, InstalledPlugin, PluginInventory, PluginUpdate, InstallState ... are what the catalogue SAYS and what
// the install records KEEP: the store window draws them, core/config.hpp carries a few, the plugin cleanup reasons over them. None of it needs the fetcher
// (PluginRepo: the download, the hash check, the host lock, the atomic install), which is the engine's. The types moved here verbatim from plugin_repo.hpp,
// which includes this header, so every user of PluginRepo sees the same names as before; the interface includes this header and not plugin_repo.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

// One downloadable binary of one plugin, for one os/arch pair.
//
// Every field here came off the wire and is therefore untrusted. The
// invariants parseIndex() enforces before an entry is ever exposed: `file`
// non-empty, `url` an https:// URL, `sha256` exactly 64 hexadecimal digits.
// `sizeBytes` is ADVISORY - shown in the UI, never used as a buffer size or
// as a download limit.
struct PluginPlatform {
    std::string os;    // "windows", "linux", "macos" (matched case-insensitively)
    std::string arch;  // "x64", "arm64", "x86"
    std::string file;  // bare destination filename; still re-sanitised at install
    std::string url;   // https:// only
    std::string sha256;  // 64 hex digits, mandatory
    std::uint64_t sizeBytes = 0;
};

// One picture of a plugin at work, as the catalogue names it (0.99.72, the
// plugin store's page for one plugin). Like every field here it came off the
// wire: parseIndex() keeps only a picture whose `url` is https and whose `sha256`
// is exactly 64 hexadecimal digits (lower-cased), and fetchScreenshot() refuses
// anything that does not hash to it. `sizeBytes`, `width` and `height` are
// ADVISORY - for laying out a frame before the picture arrives, never a limit
// and never an allocation size (the 4 MiB cap is kMaxScreenshotBytes).
struct CatalogScreenshot {
    std::string url;      // https:// only
    std::string sha256;   // 64 lower-case hex digits; also the cache file's name
    std::string caption;  // one line under the picture; never empty
    std::uint64_t sizeBytes = 0;
    int width = 0;
    int height = 0;
};

// One plugin as the catalogue describes it.
struct PluginCatalogEntry {
    std::string id;           // stable machine identifier, e.g. "pocsag"
    std::string name;         // display name
    std::string version;      // the plugin's own version string
    std::string author;
    std::string licence;      // displayed verbatim; the host never hides it
    std::string summary;      // one line for the list
    std::string description;  // paragraph for the detail pane
    std::string homepage;
    std::string legalNotice;  // e.g. patent/export notes the author wants shown

    // --- The shop-window fields (0.99.72). ALL OPTIONAL: a catalogue that has
    // none of them parses exactly as it did before they existed, and an older
    // client ignores them (unknown keys always were). They are cosmetic, so a
    // wrong TYPE in one of them costs that field (it keeps its default and the
    // parse report says why), never the catalogue - unlike the security fields
    // above, where a half-understood document is refused whole.
    std::string category;          // "aircraft", "marine", "satellites-weather",
                                   // "meters-paging", "broadcast" or "maps-tools";
                                   // kept verbatim - an unknown word is the view's OTHER
    bool experimental = false;     // never decoded a real signal
    std::string whatsNew;          // one short paragraph about THIS version
    std::string published;         // "YYYY-MM-DD" (a malformed date is dropped)
    std::vector<CatalogScreenshot> screenshots;  // at most kMaxScreenshotsPerPlugin
    // What the binary declares, as the catalogue read it off the binary: the
    // names exactly as published ("CASCADE_CAP_IQ_DECODER", in order, known or
    // not) and the ABI bits of the ones this host knows. A name this host has
    // never heard of keeps its place in capabilityNames and sets no bit - a newer
    // catalogue naming a newer capability is not an error. 0 and empty when the
    // catalogue says nothing, which is not the same as "declares none": the
    // store draws NOT DECLARED only for an uninstalled plugin with no names.
    std::uint32_t capabilities = 0;
    std::vector<std::string> capabilityNames;

    // The plugin ABI the binary was built against. Must equal
    // CASCADE_PLUGIN_ABI_VERSION exactly to be installable.
    std::uint32_t abiVersion = 0;

    // OPTIONAL RETIREMENT FLOOR. An installed copy older than this must no
    // longer be loaded (see the version-policy section at the top of this
    // file). Empty - the normal case - means no floor: the catalogue is not
    // required to retire anything, and silence must never be read as "retire
    // everything". Compared with compareVersions(), never with strcmp.
    std::string minSupportedVersion;

    std::vector<PluginPlatform> platforms;

    // abiVersion == CASCADE_PLUGIN_ABI_VERSION. Set by parseIndex(); install()
    // re-derives it from abiVersion rather than trusting this flag, because a
    // caller could have constructed the struct by hand.
    bool compatible = false;

    // TRUE only for an entry that arrived from the REGIONAL list and survived
    // mergeRegional() (see the REGIONAL CATALOGUE block above). Set in
    // exactly one place - mergeRegional() - and NEVER by parseIndex(): a
    // "regional": true key in either document's JSON is silently ignored, so
    // neither a hostile public index nor a hostile regional one can grant an
    // entry the download-prefix exemption merely by claiming the field.
    // install() re-checks the prefix against this flag before any network
    // activity, exactly as it re-derives `compatible` rather than trusting a
    // hand-built struct.
    bool regional = false;

    // WHERE THE ENTRY CAME FROM (0.99.74, rule 8): EMPTY for the official
    // catalogue, its regional list and an override of the official address; the
    // site's address (exactly as configured) for an entry that arrived from a
    // plugin site. Set in exactly one place - mergeSite() - and NEVER by
    // parseIndex(): a "source" key in a downloaded index is ignored, so no index
    // can present its entry as the official one (or as another site's) by
    // saying so. Part of the entry's identity: (source, id).
    std::string source;

    // The platform record matching the host we are running on, or nullptr if
    // the catalogue has no build for it. The pointer aliases `platforms`, so
    // it dangles if the entry is modified or destroyed.
    const PluginPlatform* thisPlatform() const;
};

// ---------------------------------------------------------------------------
// What is installed locally, and what the catalogue last said about it
// ---------------------------------------------------------------------------

// One row of the local manifest: a plugin this product installed, as it was
// recorded at install time.
//
// The manifest is a RECORD, not an authority. Nothing here is trusted to be
// true of the disk - reconciliation checks the file and reports every
// disagreement - and nothing here is trusted to be safe: `file` is put back
// through sanitiseFileName() before any filesystem call touches it, exactly
// like a name that came off the wire.
struct InstalledPlugin {
    std::string id;       // catalogue id; with `source`, the key everything joins on
    // The plugin site this was installed from (PluginCatalogEntry::source), or
    // EMPTY for the official catalogue: a record written before sites existed has
    // no such key and reads as official, which is what it was. An installed
    // plugin whose site is no longer in the user's list keeps this and gets no
    // updates (no entry carries it any more).
    std::string source;
    std::string name;     // display name at install time, for user-facing copy
    std::string version;  // the version that was installed
    std::string file;     // bare file name inside the plugins directory
    std::string sha256;   // digest of those bytes, verified at install time

    // The plugin ABI the installed build declared. 0 means "not recorded"
    // (an older manifest, or a hand-written one) and is treated as UNKNOWN,
    // never as a mismatch - see pluginBlockReason's fail-open rule.
    std::uint32_t abiVersion = 0;

    std::int64_t installedAtUnix = 0;  // seconds since the epoch, 0 if unknown

    // Set by loadInventory() when reconciliation finds a problem. Both default
    // to FALSE - "nothing wrong found, or nothing checked yet" - so a
    // hand-built record (a test's, or the manifest's before reconciliation)
    // never means something alarming by accident.
    bool missingFromDisk = false;   // the manifest names a file that is gone
    bool digestMismatch = false;    // the file on disk is not the bytes we installed
};

// The catalogue's policy for one plugin id, as last seen and cached locally.
//
// This is the whole reason retirement works offline: the floor is remembered,
// so enforcement never needs the network. `known` false means no catalogue has
// ever been seen for this id, which is the FAIL-OPEN state.
struct CachedPolicy {
    std::string id;
    std::string source;                 // PluginCatalogEntry::source; empty = official
    bool known = false;                 // has any catalogue ever described this id?
    std::string minSupportedVersion;    // empty = no floor
    std::string catalogueVersion;       // the version the catalogue offered
    std::uint32_t abiVersion = 0;       // the ABI that catalogue build targets
};

// Everything loadInventory() found: the manifest's records, the catalogue
// policy cache, and an honest account of where the two disagree with the disk.
struct PluginInventory {
    std::vector<InstalledPlugin> plugins;
    std::vector<CachedPolicy> policies;

    // Plugin files present in the directory that no manifest record claims -
    // hand-installed, or installed before this manifest existed. They are
    // left completely alone: unmanaged means unmanaged, not unwanted.
    std::vector<std::string> unmanaged;

    // Every reconciliation finding, in words, in the order they were found.
    // The manifest is never repaired silently; if it disagreed with the disk,
    // it says so here.
    std::vector<std::string> notes;

    bool manifestPresent = false;  // a manifest file exists
    bool manifestUsable = false;   // ...and it parsed
};

// Why one installed plugin must not be loaded. Two causes, kept apart because
// the user's remedy differs: a floor is fixed by an update the catalogue
// already has; an ABI mismatch needs a rebuilt plugin that may not exist.
enum class PluginBlockReason {
    None = 0,
    BelowMinimumVersion,  // older than the cached minSupportedVersion floor
    AbiMismatch,          // built for a different plugin ABI than this host
};

// One blocked plugin, with the reason and the exact words to show for it.
struct BlockedPlugin {
    InstalledPlugin installed;
    CachedPolicy policy;
    PluginBlockReason reason = PluginBlockReason::None;
    std::string message;  // == pluginBlockMessage(installed, policy)
};

// One planned update. `entry` ALIASES the catalogue vector passed to
// planUpdates(), so a plan must not outlive the catalogue it was planned from.
struct PluginUpdate {
    std::string id;
    std::string source;  // PluginCatalogEntry::source of the entry; empty = official
    std::string fromVersion;
    std::string toVersion;
    std::string reason;  // user-facing, one line
    const PluginCatalogEntry* entry = nullptr;
};

// ---------------------------------------------------------------------------
// Installed state, by plugin id and version (0.99.72)
// ---------------------------------------------------------------------------

// What the plugin host's last scan knows about one module file, reduced to what
// installStateFor() needs. Built by the caller from PluginHost::plugins(): `id` is
// the catalogue id of the install record that names `file` (the scan itself only
// knows a module's declared name, which is not the catalogue's id), or, for a
// module no record names - a side-loaded one - its declared name. A module whose
// `id` matches no catalogue entry is simply never asked about. A record with no
// descriptor (a module refused before its description could be read) has no
// version and is left out by the caller.
struct ScannedModule {
    std::string id;
    std::string source;   // the install record's source; empty = official or side-loaded
    std::string version;  // what the module's own descriptor says it is
    std::string file;     // bare module file name
};

enum class InstallStateKind {
    NotInstalled,     // no copy of this plugin, in any version
    Installed,        // the installed version is the catalogue's
    UpdateAvailable,  // the catalogue has a newer version than the newest installed
    NewerInstalled,   // an installed copy is newer than the catalogue's
};

struct InstallState {
    InstallStateKind kind = InstallStateKind::NotInstalled;
    // The NEWEST version found installed; empty when NotInstalled. For an
    // UpdateAvailable this is the "from" of "vX to vY".
    std::string installedVersion;
};


// ---- THE PURE STATICS the window calls (shell retirement R11a): a site address read as a label, a host, a display string, a validated address; the
// default catalogue's address; the platform words. They are string arithmetic and compile-time facts, so they are free functions of this header
// (definitions: core/plugin_repo_data.cpp), and PluginRepo's statics of the same names forward to them - one body each, the tests keep calling
// PluginRepo::siteLabel and the rest.
std::string defaultPluginIndexUrl();
bool pluginIsHttpsUrl(const std::string& url);
std::string pluginSiteHost(const std::string& url);
std::string pluginSiteLabel(const std::string& siteUrl);
std::string pluginSiteDisplay(const std::string& siteUrl, std::size_t maxChars);
bool normalisePluginSiteUrl(const std::string& raw, std::string& out, std::string& error);
const char* pluginHostOs();
const char* pluginHostArch();

}  // namespace cascade::core
