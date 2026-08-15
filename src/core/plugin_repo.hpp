// Plugin catalogue client - the in-app "plugin store".
//
// WHAT IT DOES. Fetches an index.json published at a fixed HTTPS origin,
// exposes the entries it describes, and - only when the user explicitly asks -
// downloads one plugin binary and installs it into the plugins directory that
// PluginHost scans.
//
// WHY IT IS WRITTEN THE WAY IT IS. Everything in this file exists to move a
// NATIVE DLL from the internet into a directory the host will later hand to
// LoadLibrary. plugin_host.hpp says plainly that once a module is mapped there
// is no in-process defence against it: DllMain has already run, and a plugin
// that corrupts the heap takes the product down in unrelated code hours later.
// So every defence has to happen HERE, in the window between "a stranger's
// JSON said so" and "the file is sitting in the plugins directory". cascade is
// sold; a bug in this file is a remote-code-execution path into a paying
// customer's machine.
//
// THE SEVEN RULES, each enforced in code and covered by a test in
// tests/test_plugin_repo.cpp:
//
//  1. HTTPS ONLY, with the platform's default certificate validation left
//     fully on (name checks, chain checks, and revocation - which is
//     explicitly ENABLED rather than merely not disabled). A http:// URL is
//     refused by a portable, pre-network scheme check, so the refusal happens
//     the same way whether or not a network stack is present.
//
//  2. SHA-256 IS MANDATORY. An index entry with no hash, a short hash, or a
//     non-hex hash is not "unverified", it is refused outright at parse time.
//     The download lands in a TEMP file, is hashed while it streams, and is
//     moved into the plugins directory only on an exact match. A mismatch
//     deletes the temp file and reports expected vs actual. The hash comes
//     from Windows CNG (BCRYPT_SHA256_ALGORITHM); nothing here hand-rolls a
//     digest and nothing new is linked beyond bcrypt.lib/winhttp.lib.
//
//  3. CROSS-HOST REDIRECTS ARE REFUSED, not followed. WinHTTP's automatic
//     redirect handling is switched off and 3xx responses are resolved here,
//     so a compromised origin cannot bounce the download to an attacker's
//     host. Same-host, same-port, still-https redirects are followed, up to a
//     small hop limit. (This is why the catalogue must serve binaries from an
//     origin that does not redirect elsewhere - raw.githubusercontent.com
//     serves the bytes directly; a GitHub *release asset* URL would redirect
//     to objects.githubusercontent.com and be refused.)
//
//  4. A HARD BYTE CAP, independent of anything the index or the server
//     claims: kMaxPluginBytes for a plugin, kMaxIndexBytes for the index. A
//     hostile or broken server cannot fill the user's disk, and sizeBytes
//     from the index is advisory only - it is never trusted as an allocation
//     size or as a stopping condition.
//
//  5. abiVersion MUST EQUAL CASCADE_PLUGIN_ABI_VERSION EXACTLY, or the entry
//     is marked incompatible and install() refuses it. Same reasoning as the
//     loader's exact-match rule: a near-miss ABI is how a struct layout change
//     becomes a memory-corruption bug days later.
//
//  6. FILE NAMES FROM THE INDEX ARE UNTRUSTED INPUT. sanitiseFileName() is
//     the single path-traversal guard: a bare filename of [A-Za-z0-9._-]
//     ending in .dll, no separators, no "..", no drive letters, no NTFS
//     alternate-data-stream colons, no Windows reserved device names, and a
//     length bound. An index entry must never be able to write outside the
//     plugins directory, and this is the function that makes that true.
//
//  7. NOTHING INSTALLS WITHOUT AN EXPLICIT install() CALL. There is no
//     auto-update path, no background fetch, no "install on discovery". The
//     catalogue is inert data until a human acts on it. applyUpdate() (below)
//     does NOT weaken this: it is one user-initiated update of one plugin,
//     and it is a thin wrapper around this same install(), so an update goes
//     through the identical gauntlet - exact ABI, filename sanitiser,
//     mandatory sha256, temp-file staging, byte cap, no cross-host redirect.
//     There is deliberately no shortcut for "it is only an update"; an update
//     IS an install, and it is the riskiest install, because the user already
//     trusts the thing being replaced.
//
// TESTABILITY. parseIndex(), sanitiseFileName() and the SHA-256 wrapper are
// static and pure (or filesystem-only), so the entire security-critical
// surface is exercised offline; only the transport itself needs a network.
//
// ---------------------------------------------------------------------------
// VERSION POLICY: RETIREMENT, NOT AUTO-UPDATE (P10)
// ---------------------------------------------------------------------------
//
// THE PROBLEM. A user who installed a plugin a year ago keeps running it
// against a host that has moved on. The plugin still loads (its ABI still
// matches), it half-works, and the resulting bug report describes a
// combination nobody has ever built or tested. Chasing those costs more than
// the plugin is worth.
//
// THE OBVIOUS FIX IS THE WRONG ONE. Silently downloading a newer build on
// every launch would fix the staleness and buy a much larger problem: native
// code arriving from the network with no human in the loop, which is exactly
// the shape of the xz and event-stream supply-chain attacks, and background
// network access in a paid product that promises none. So there is no
// automatic download here. None.
//
// WHAT IS DONE INSTEAD. The catalogue may publish, per plugin, a
// minSupportedVersion: a floor below which an INSTALLED copy must no longer be
// loaded. The floor is CACHED in the local manifest the moment a catalogue is
// seen, and enforcement then reads the cache, never the network. That gives
// the stronger guarantee: a user who is offline for a year, or who never opens
// the plugin browser again, is still protected, because the decision does not
// depend on an update having succeeded. Retirement is local and certain;
// updating is remote and optional.
//
//   - pluginBlockReason() is the ONE predicate that decides. The GUI, the
//     loader and any diagnostic must all read it; two places deciding this is
//     how a plugin ends up blocked in the list and loaded in the process.
//   - It FAILS OPEN on absence of information. No cached policy for this id -
//     a fresh install, a catalogue never fetched, a hand-installed private
//     plugin that is in no catalogue at all - means LOAD. Blocking on ignorance
//     would brick every offline user and every in-house plugin, which is a
//     worse product than the stale-plugin bug this exists to prevent.
//   - It FAILS CLOSED once a floor is known: below the floor is blocked, with
//     or without a network, for as long as the cache remembers - and the cache
//     is only ever refreshed by a catalogue, never cleared by one being
//     unreachable.
//   - ABI mismatch is reported as a SEPARATE reason, because the remedy
//     differs: a floor is fixed by an update the catalogue already has, an ABI
//     mismatch needs a rebuilt plugin that may not exist yet. Telling a user
//     to "update" a plugin no update can fix is a support ticket.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
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
    std::string id;       // catalogue id, the key everything joins on
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
    std::string fromVersion;
    std::string toVersion;
    std::string reason;  // user-facing, one line
    const PluginCatalogEntry* entry = nullptr;
};

class PluginRepo {
public:
    PluginRepo() = default;

    // Holds atomics for the GUI's progress/cancel; copying one would be
    // meaningless and is not needed.
    PluginRepo(const PluginRepo&) = delete;
    PluginRepo& operator=(const PluginRepo&) = delete;

    // Hard transfer caps (rule 4). 64 MiB is roughly two orders of magnitude
    // more than any plausible decoder DLL, so it never gets in a real
    // plugin's way while still bounding a hostile server's damage to one
    // bounded temp file that is deleted on failure.
    static constexpr std::uint64_t kMaxPluginBytes = 64ull * 1024ull * 1024ull;
    static constexpr std::uint64_t kMaxIndexBytes = 4ull * 1024ull * 1024ull;

    // Upper bound on a catalogue filename. Long enough for any sane plugin,
    // short enough that pluginsDir + name cannot approach a path limit.
    static constexpr std::size_t kMaxFileNameChars = 128;

    // Redirect hops allowed, all of which must stay on the same host (rule 3).
    static constexpr int kMaxRedirects = 4;

    // The published catalogue index:
    //   https://raw.githubusercontent.com/wonderingStars/foxsdr-plugins/master/index.json
    // raw.githubusercontent.com is used rather than a release-asset URL
    // precisely because it serves the bytes directly - see rule 3 above. The
    // repository may be private, in which case the fetch fails with an HTTP
    // status and the UI shows an empty catalogue; that is a supported state,
    // not an error condition to shout about.
    static std::string defaultIndexUrl();

    // Host identity used to pick a platform record. Exposed so tests and the
    // UI agree with the matcher instead of re-deriving it.
    static const char* hostOs();
    static const char* hostArch();

    // ---- Parsing (pure, offline, and the security-critical surface) -------
    //
    // Parses a catalogue index. Returns false with `error` set and `out`
    // EMPTY on any problem. Deliberately all-or-nothing: a malformed or
    // tampered index is refused wholesale rather than partially trusted,
    // because "which half of this document do I believe" is not a question
    // with a safe answer.
    //
    // Requires: a JSON object root; schemaVersion == 1; a "plugins" array;
    // each element an object with a non-empty id/name/version; each platform
    // record carrying an https url and a well-formed 64-hex sha256. Unknown
    // fields anywhere are ignored, so the server can add fields without
    // breaking older clients.
    //
    // NOT checked here: whether `file` is a safe filename. That single
    // decision belongs to sanitiseFileName() at install time, so there is
    // exactly one enforcement point; refusing an entire catalogue because one
    // entry has an odd filename would deny the user every other plugin.
    static bool parseIndex(const std::string& json, std::vector<PluginCatalogEntry>& out,
                           std::string& error);

    // THE PATH-TRAVERSAL GUARD (rule 6). Accepts only a bare filename made of
    // [A-Za-z0-9._-], starting with a letter or digit, containing no "..",
    // ending in ".dll" (case-insensitive) with at least one character before
    // the extension, no longer than kMaxFileNameChars, and not a Windows
    // reserved device name. Everything else is refused with a reason.
    //
    // Note what the character class alone already excludes: '/' and '\\'
    // (separators), ':' (drive letters and NTFS alternate data streams), '%'
    // and '$' (environment/share syntax), whitespace and control characters,
    // and every non-ASCII byte (so no Unicode look-alike can impersonate an
    // existing plugin). The remaining rules cover what the class cannot.
    static bool sanitiseFileName(const std::string& raw, std::string& out, std::string& error);

    // True if `url` is an https:// URL with something after the scheme.
    // Portable and pre-network, so the http:// refusal is provable offline
    // and cannot be reached around by a transport quirk.
    static bool isHttpsUrl(const std::string& url);

    // ---- SHA-256 (Windows CNG; see rule 2) --------------------------------
    //
    // Lowercase hex of the SHA-256 digest. `data` may be null when n == 0.
    // On a non-Windows build both return false with a "not supported"
    // message rather than silently succeeding with no integrity check.
    static bool sha256Hex(const void* data, std::size_t n, std::string& hexOut,
                          std::string& error);
    static bool sha256File(const std::string& path, std::string& hexOut, std::string& error);

    // THE INTEGRITY DECISION, as one named function that install() calls.
    //
    // It is factored out for a specific reason: the rest of install() cannot
    // run without a live HTTPS server, so without this seam the single most
    // important comparison in the module would be the one line no offline
    // test could ever prove. Here it is exhaustively testable, and a mutation
    // that makes it always succeed turns tests/test_plugin_repo.cpp red.
    //
    // True only if BOTH strings are well-formed 64-digit hex AND equal
    // ignoring case. Requiring well-formedness on both sides is what stops
    // two empty strings, two truncated strings, or two copies of "unknown"
    // from being read as a successful verification.
    static bool sha256Matches(const std::string& expectedHex, const std::string& actualHex);

    // ---- Version comparison (pure) ----------------------------------------
    //
    // Orders two version strings. Returns < 0 if a is older, 0 if they are the
    // same version, > 0 if a is newer.
    //
    // THE BUG THIS EXISTS TO NOT HAVE: strcmp ordering puts "1.10.0" BEFORE
    // "1.9.0", because '1' < '9'. Every segment that is a number is compared
    // as a number here, so 1.10.0 is correctly newer than 1.9.0. Nothing in
    // this module may order versions any other way.
    //
    // The rules, all of them, because an updater that guesses is an updater
    // that churns:
    //   - Segments are split on '.'; a missing or empty segment counts as 0,
    //     so "1.2" == "1.2.0" == "1.2." and "" == "0".
    //   - Two all-digit segments compare NUMERICALLY, with leading zeros
    //     ignored ("1.02" == "1.2") and with no integer conversion anywhere,
    //     so a 40-digit segment can neither overflow nor wrap.
    //   - A digit segment outranks a non-digit one: "1.0.0" > "1.0.0-rc1",
    //     matching the SemVer intuition that a release beats its prereleases,
    //     and - the reason it is written this way - making an unparseable
    //     version conservative in BOTH directions. Garbage in the catalogue
    //     never looks newer than a real installed version (no churn), and
    //     garbage installed never looks older than the catalogue (no
    //     surprise reinstall).
    //   - Two non-digit segments compare byte-wise, case-sensitively. That is
    //     arbitrary, but it is deterministic and total, which is what the
    //     no-downgrade rule needs.
    static int compareVersions(const std::string& a, const std::string& b);

    // ---- The local manifest (what is installed) ---------------------------
    //
    // The plugins directory holds DLLs and nothing else - no version, no id,
    // no provenance. The manifest is the small JSON file that remembers those,
    // and the cached catalogue policy with them. It lives INSIDE the plugins
    // directory so it travels with what it describes; PluginHost only ever
    // looks at ".dll", and sanitiseFileName() only ever accepts ".dll", so a
    // ".json" here can neither be loaded nor be collided with by a catalogue
    // entry.
    static const char* manifestFileName();  // "installed.json"
    static std::string manifestPath(const std::string& pluginsDir);

    // Parses manifest text. Returns false with `error` set for a document-level
    // problem (not JSON, not an object, unknown schemaVersion) - in which case
    // the caller must treat the manifest as ABSENT, which is the fail-open
    // state, not as an instruction to block anything.
    //
    // Per-ROW problems are handled differently on purpose: a row with no id,
    // no file, or a file name that fails sanitiseFileName() is DROPPED with a
    // note rather than failing the whole document. One bad row must not erase
    // the record of the other five, and a dropped row degrades to "unmanaged
    // file", which is the safe direction.
    static bool parseManifest(const std::string& text, std::vector<InstalledPlugin>& plugins,
                              std::vector<CachedPolicy>& policies,
                              std::vector<std::string>& notes, std::string& error);

    static std::string serialiseManifest(const std::vector<InstalledPlugin>& plugins,
                                         const std::vector<CachedPolicy>& policies);

    // ATOMIC, by the same temp-file-plus-rename route as ConfigStore: a crash,
    // a full disk or a locked target leaves the complete old manifest or the
    // complete new one, never a half-written hybrid that would read as corrupt
    // and silently drop every retirement floor.
    static bool saveManifest(const std::string& pluginsDir,
                             const std::vector<InstalledPlugin>& plugins,
                             const std::vector<CachedPolicy>& policies, std::string& error);

    // Reads the manifest AND the directory, and reconciles them. Returns false
    // with `error` set only when the manifest existed and could not be used;
    // `out` is filled in either case, because a missing or corrupt manifest is
    // an ordinary state (first run, hand-edit) and the caller still needs the
    // directory listing. Reconciliation reports - and never silently repairs:
    //   - a record whose file is gone            -> missingFromDisk + note
    //   - a file whose bytes are not what we
    //     installed (re-hashed, not assumed)     -> digestMismatch + note
    //   - a plugin file no record claims         -> unmanaged + note
    static bool loadInventory(const std::string& pluginsDir, PluginInventory& out,
                              std::string& error);

    // ---- Cached catalogue policy ------------------------------------------
    //
    // The policy for `id`, or a CachedPolicy with known == false if no
    // catalogue has ever described it. Never throws, never blocks.
    static CachedPolicy policyFor(const std::vector<CachedPolicy>& policies,
                                  const std::string& id);

    // Folds a freshly fetched catalogue into the cache. An id the catalogue
    // lists is REPLACED wholesale (the catalogue is authoritative for what it
    // publishes, including dropping a floor it no longer wants). An id the
    // catalogue does NOT list keeps its previous policy: a catalogue that is
    // unreachable, truncated to fewer entries, or replaced by an attacker's
    // empty document must not be able to un-retire a plugin.
    static void mergePolicies(std::vector<CachedPolicy>& cached,
                              const std::vector<PluginCatalogEntry>& catalogue);

    // load -> mergePolicies -> save, for the one call the UI makes after a
    // successful fetch. This is the ONLY moment a policy is written.
    static bool cacheCataloguePolicies(const std::string& pluginsDir,
                                       const std::vector<PluginCatalogEntry>& catalogue,
                                       std::string& error);

    // ---- Retirement: the single predicate ---------------------------------
    //
    // Whether this installed plugin must be refused, and why. PURE: no
    // filesystem, no network, no clock - so the GUI, the loader and the tests
    // all get the same answer, and the answer is provable offline.
    //
    // FAIL OPEN on missing information, every time: an unrecorded ABI (0), an
    // unknown policy, a policy with no floor, or an installed record with no
    // version all return None. FAIL CLOSED only on a positive statement: the
    // cached floor exists and the installed version is below it.
    static PluginBlockReason pluginBlockReason(const InstalledPlugin& installed,
                                               const CachedPolicy& policy);

    // The words the user sees, and the only place they are composed. Empty
    // when the plugin is fine, so `if (!msg.empty())` is the whole UI test.
    //
    // This is UI copy, not a log line: it names the plugin, the version
    // installed, what is required, and WHAT TO DO - and it tells the truth
    // about the remedy, which is why the two reasons read differently. A
    // version floor is fixed by an update the catalogue already has, so it
    // says to update. An ABI mismatch needs a rebuilt plugin from the author,
    // so it does NOT tell the user to click Update: sending someone round a
    // loop that cannot work is worse than saying nothing.
    static std::string pluginBlockMessage(const InstalledPlugin& installed,
                                          const CachedPolicy& policy);

    // Every installed plugin that must not be loaded, with its reason and
    // message already composed. For the Plugins panel's red rows.
    static std::vector<BlockedPlugin> blockedPlugins(
        const std::vector<InstalledPlugin>& installed,
        const std::vector<CachedPolicy>& policies);

    // Same question, badge-cheap: evaluates the predicate only, composing no
    // strings, so a GUI can ask it every frame without allocating.
    static std::size_t blockedCount(const std::vector<InstalledPlugin>& installed,
                                    const std::vector<CachedPolicy>& policies);

    // ---- Update planning (pure) -------------------------------------------
    //
    // What the user COULD update, given a catalogue and what is installed.
    // Plans nothing, downloads nothing, writes nothing: it is a pure function
    // of its two arguments, so the whole policy is testable offline.
    //
    // An update is planned when, and only when, the plugin is installed AND
    // the catalogue entry is usable on this host AND either
    //   - the catalogue version is NEWER than the installed one, or
    //   - the versions are equal but the installed build targets a different
    //     plugin ABI and the catalogue's build targets ours (a rebuild at the
    //     same version number - the one case where "same version" is still an
    //     upgrade).
    //
    // NEVER a downgrade. If the catalogue version is older than what is
    // installed, nothing is planned, whatever else is true - not for an ABI
    // mismatch, not for a retirement floor. A catalogue that has gone
    // backwards is either a mistake or an attack, and neither is a reason to
    // replace a working newer plugin with an older one.
    //
    // Also skipped: an entry with no build for this os/arch; an entry whose
    // abiVersion is not exactly this host's; an installed record whose file
    // reconciliation found missing (the user deleted it - updating would
    // resurrect it); and any plugin that is installed but absent from the
    // catalogue, which is left completely alone.
    //
    // What it deliberately does NOT do is pre-filter for safety - a hostile
    // file name, for instance, is still planned and then refused by
    // applyUpdate(). Every security gate lives in install(), at ONE
    // enforcement point; a second, weaker copy here would be a place for the
    // two to drift apart.
    static std::vector<PluginUpdate> planUpdates(const std::vector<PluginCatalogEntry>& catalogue,
                                                 const std::vector<InstalledPlugin>& installed);

    // ---- Network ----------------------------------------------------------
    //
    // Fetches and parses the index at `url`. Returns false with `error` set on
    // any failure - unreachable origin, TLS failure, private repository (404),
    // oversized or malformed document - and never throws. On failure entries()
    // is left EMPTY rather than stale: a catalogue that could not be verified
    // this time must not keep offering installs from last time.
    bool fetchIndex(const std::string& url, std::string& error);

    const std::vector<PluginCatalogEntry>& entries() const { return entries_; }

    // Downloads, verifies and installs one entry into `pluginsDir`, creating
    // that directory if needed. On success `installedPath` is the installed
    // file. Returns false with `error` on anything at all, and NEVER leaves a
    // partial or unverified file behind: the download goes to a
    // pid-suffixed ".part" temp file in the destination directory (same
    // volume, so the final move is an atomic rename) and is deleted on every
    // failure path.
    //
    // Refuses, in this order and all before any network activity: an entry
    // whose abiVersion is not this host's; an entry with no build for this
    // os/arch; a file name that fails sanitiseFileName(); a malformed sha256;
    // a non-https url.
    bool install(const PluginCatalogEntry& e, const std::string& pluginsDir,
                 std::string& installedPath, std::string& error);

    // Records one installed plugin in the manifest, and caches the catalogue
    // policy that came with it. Call it after a successful install() so the
    // plugin becomes managed - an unrecorded plugin is not broken, it is just
    // invisible to the retirement check (which then fails open for it).
    //
    // Upserts by id and writes the whole manifest atomically. Returns false
    // with `error` on a bad entry (no build for this host, unsanitisable file
    // name) or a failed write.
    static bool recordInstall(const std::string& pluginsDir, const PluginCatalogEntry& e,
                              std::string& error);

    // Performs ONE planned update, at the user's request. There is no bulk,
    // no background and no automatic caller: the user presses Update on one
    // plugin and this runs.
    //
    // It is a wrapper around install(), which is the point - the same ABI
    // check, filename sanitiser, mandatory sha256, temp-file staging, byte cap
    // and cross-host redirect refusal apply, because an update IS an install.
    //
    // ALL-OR-NOTHING PER PLUGIN. A failed download, a failed hash or a
    // cancellation leaves the previously installed file exactly as it was
    // (install() only ever renames a fully verified temp file over it) and
    // leaves the manifest untouched, so the plugin keeps working at its old
    // version and the next plan proposes the same update again.
    //
    // The one partial state, reported honestly rather than hidden: if the file
    // installs but the manifest cannot be written, this returns FALSE with
    // `installedPath` set and an error saying so. The plugin on disk is the
    // new, verified one; only the record is stale, and re-running the update
    // repairs it (installing identical verified bytes again is harmless).
    //
    // Blocking: it downloads, so call it off the UI thread. cancel() aborts it
    // like any other transfer.
    bool applyUpdate(const PluginUpdate& u, const std::string& pluginsDir,
                     std::string& installedPath, std::string& error);

    // Deletes one installed plugin. `fileName` goes through the same
    // sanitiser as install, so a caller cannot be tricked into deleting
    // something outside pluginsDir.
    bool remove(const std::string& pluginsDir, const std::string& fileName, std::string& error);

    // 0..1 for the transfer in flight. Stays at 0 when the server sends no
    // Content-Length (progress that lies is worse than progress that waits).
    float progress() const { return progress_.load(std::memory_order_relaxed); }

    // Asks the transfer in flight to stop; the install then fails cleanly with
    // "cancelled" and removes its temp file. Safe to call from another thread.
    // install()/fetchIndex() clear the flag on entry, so a cancel that arrives
    // between two operations does not silently kill the next one.
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }

private:
    std::vector<PluginCatalogEntry> entries_;
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_{false};
};

}  // namespace cascade::core
