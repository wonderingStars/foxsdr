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
// ---------------------------------------------------------------------------
// REGIONAL CATALOGUE (offered in some countries only)
// ---------------------------------------------------------------------------
//
// A second, SMALL catalogue may exist at foxsdr.com, for plugins the site
// chooses to offer only to some connections (by IP-derived country, decided
// entirely on the server; this client neither knows nor asks why). It is
// fetched at EXACTLY THE SAME MOMENT as the public one, extending rule 7's
// promise rather than opening a second timer: there is no independent
// schedule for it, no background poll, and no fetch that the public one did
// not also just make. If the public fetch fails, the regional one is never
// even attempted (regionalWanted() is not consulted) - the browser shows
// exactly what it showed before this feature existed.
//
// A regional failure - unreachable, wrong content, anything - is swallowed
// completely: no error field is set, nothing is shown, and NOTHING IS
// LOGGED. A log line that said "1 regional plugin offered" would itself be a
// disclosure of which country the request was answered from, and a
// diagnostics bundle travels further than the fact it would be encoding.
//
// PUBLIC ALWAYS WINS. mergeRegional() drops a regional entry whole - not
// merely the clashing field - if its id collides with a public one, so a
// compromised or stale regional list can never shadow a public plugin with
// different bytes under the same name.
//
// isRegionalDownloadUrl() is the second enforcement of the same idea rule 3
// already states for the public path: a plugin marked regional must download
// from the one origin that offered it. It is checked twice on purpose - once
// when the entry is merged in, and again inside install() itself - because
// install() does not trust its caller to have gone through the merge.
//
// `regional` on PluginCatalogEntry is trusted from exactly one place:
// mergeRegional(). parseIndex() never reads a "regional" key from either
// document, so a hostile public OR regional index cannot grant itself the
// download prefix's home-field advantage by simply saying so in JSON.
//
// ---------------------------------------------------------------------------
// PLUGIN SITES (0.99.74): A THIRD PARTY'S OWN index.json
// ---------------------------------------------------------------------------
//
// A user may paste the address of someone else's index.json into the store
// (AppConfig::pluginSites, at most kMaxPluginSites) and the store lists, installs
// and updates that site's plugins beside the official ones, marked as coming from
// somewhere else. The seven rules above are NOT relaxed for it - a site's
// entries go through the same parseIndex(), the same install() gauntlet - and
// the following is added on top, each in one place:
//
//  8. A SITE'S PLUGIN IS KNOWN BY (source, id), NOT BY id. `source` is empty for
//     the official catalogue (and its regional list, and an override of the
//     official address) and the site's address for a site. Every join that used
//     to be by id alone - installStateFor, planUpdates, mergePolicies,
//     policyFor, recordInstall, the manifest itself - is by the pair, so a site
//     that reuses an official id can neither be taken for the official plugin,
//     nor update it, nor replace or lift its retirement floor. `source` is set
//     in exactly one place on a catalogue entry - mergeSite() - and parseIndex()
//     never reads a "source" key from a downloaded index, for the reason
//     `regional` is not read either.
//
//  9. OFFICIAL WINS. mergeSite() drops a site's entry WHOLE when its id is
//     already listed (official, regional, an earlier site, or earlier in the
//     same site), and counts it; the store says "N hidden: same id as a plugin
//     already listed". A site can never shadow, replace or retire an official
//     plugin.
//
// 10. ONE HOST. Every platform url and every picture url of a site's entry must
//     have the SAME HOST (and port) as the site's own index address
//     (siteFilesAllowed), else that entry is dropped whole at the merge - and
//     install() checks it again, before any socket, exactly as it checks the
//     regional prefix. docs/THIRD-PARTY-SITES.md states the same rule to the
//     people who host a site. (A site given as a local file path has no host to
//     lock to: the file is the user's own, read from their own disk, exactly as
//     the official address may be a local file.)
//
// 11. A SITE NEVER OVERWRITES ANOTHER PLUGIN'S FILE. install() refuses a site's
//     entry whose destination file name is already recorded for a different
//     plugin, or is in the folder with no record at all (a side-loaded module).
//     Without this a site could name its file after an official module and
//     replace it with its own bytes, which no id rule can see.
//
// 12. A SITE'S MODULE NEVER TAKES A DECLARED NAME FROM AN OFFICIAL ONE, AT LOAD.
//     Rules 8 to 11 are the store's; this one is the loader's, because the
//     name that decides which of two modules runs is the one the BINARY declares
//     (its descriptor's `name`), which the index cannot vouch for. When two
//     loaded modules declare one name, PluginHost used to keep the higher
//     version, so a module installed from a site that declared an official
//     plugin's name with a higher version switched the official one off.
//     resolveDuplicatePlugins() now reads each file's install record
//     (InstalledPlugin::source, handed to PluginHost::scan as PluginFileOrigins
//     by AppWindow::rescanPlugins): an OFFICIAL record holds its name against
//     every SITE record of it whatever the versions, and the site's module is
//     reported not loaded - "Not loaded: it declares the same name as the
//     official plugin ..." - in the host's ordinary reason (LoadedPlugin::error,
//     which the Fitted modules row and the log print verbatim). Two site
//     modules, an unrecorded (hand-copied) module and a scan with no records
//     behave as before. See core/plugin_host.hpp.
//
// A site is read after the official catalogue with the regional list's short
// timeouts and a small cap (kMaxSiteIndexBytes), and a site's failure never
// blanks the official list. What it sends is the request itself: the address
// and the path, nothing else, like every other catalogue read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/health_events.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo_data.hpp"

namespace cascade::core {

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

    // The regional list is a handful of entries, so its cap is small on
    // purpose: 256 KiB is generous for that and stingy for anything a hostile
    // foxsdr.com-shaped host might try to hand back instead.
    static constexpr std::uint64_t kMaxRegionalIndexBytes = 256ull * 1024ull;

    // A PLUGIN SITE'S INDEX (0.99.74): a hobbyist's list of a handful of plugins.
    // 1 MiB is generous for that, a quarter of what the official catalogue may be,
    // and the bound on what a site the user added can make the store read.
    static constexpr std::uint64_t kMaxSiteIndexBytes = 1024ull * 1024ull;

    // How many sites the store keeps (AppConfig::kMaxPluginSites says the same).
    static constexpr std::size_t kMaxSites = 8;

    // Upper bound on a catalogue filename. Long enough for any sane plugin,
    // short enough that pluginsDir + name cannot approach a path limit.
    static constexpr std::size_t kMaxFileNameChars = 128;

    // Redirect hops allowed, all of which must stay on the same host (rule 3).
    static constexpr int kMaxRedirects = 4;

    // ONE PICTURE, AT MOST 4 MiB (0.99.72). A screenshot is a 1040 x 650 PNG of a
    // few hundred kilobytes; this is the bound on what a hostile or broken server
    // can make the store write, enforced against the bytes that ARRIVE (the same
    // stance as rule 4, and sizeBytes in the catalogue is never trusted for it).
    static constexpr std::uint64_t kMaxScreenshotBytes = 4ull * 1024ull * 1024ull;

    // How many pictures one plugin may list; parseIndex() keeps the first this many
    // and says it dropped the rest. A page fetches its pictures one after another,
    // so this is also the most one page can ask the network for.
    static constexpr std::size_t kMaxScreenshotsPerPlugin = 8;

    // The catalogue cache's two files in the plugins directory, and the
    // directory of cached pictures. Named here so the plugin folder's signature
    // (core/plugin_dir_signature.cpp) and the tests agree with the writer.
    static const char* catalogueCacheFileName();      // "catalogue.json"
    static const char* catalogueCacheTimeFileName();  // "catalogue.json.time"
    static const char* screenshotCacheDirName();      // "store-cache"

    // The published catalogue index:
    //   https://raw.githubusercontent.com/wonderingStars/foxsdr-plugins/master/index.json
    // raw.githubusercontent.com is used rather than a release-asset URL
    // precisely because it serves the bytes directly - see rule 3 above. The
    // repository may be private, in which case the fetch fails with an HTTP
    // status and the UI shows an empty catalogue; that is a supported state,
    // not an error condition to shout about.
    static std::string defaultIndexUrl() { return defaultPluginIndexUrl(); }

    // ---- Regional catalogue (see the REGIONAL CATALOGUE block above) ------
    //
    // Where the regional list is asked for: FOXSDR_REGIONAL_CATALOGUE_URL if
    // set (https URL, or a local path with no "://" - the same test seam
    // readLocalCatalogue() gives the public catalogue), otherwise
    // https://foxsdr.com/api/plugins/regional.
    static std::string regionalIndexUrl();

    // True iff FOXSDR_REGIONAL_CATALOGUE_URL is set to a non-empty value.
    // Read fresh on every call - see regionalWanted()'s note on why a
    // statically-linked test binary needs this read twice, once through each
    // env-setting API.
    static bool regionalOverrideSet();

    // "https://foxsdr.com/plugins/regional/" - the exact, case-sensitive
    // prefix every regional download URL must start with. A function rather
    // than a constant so a test can pass a different one without touching
    // the real host name.
    static std::string regionalDownloadPrefix();

    // True only if `url` starts with `prefix` byte-for-byte, and the
    // remainder is exactly "<region>/<file>": `region` two lowercase ASCII
    // letters, `file` starting with a letter or digit, made of
    // [A-Za-z0-9._-], containing no "..", at most kMaxFileNameChars long, and
    // with nothing else after it - no query, fragment, extra slash, port or
    // userinfo, because those would already have had to appear before the
    // prefix's own trailing "/plugins/regional/" and so are refused by the
    // exact-prefix comparison, or inside `file`, where the character class
    // excludes them outright. Deliberately independent of sanitiseFileName():
    // this checks a URL PATH SEGMENT, not a destination file name for THIS
    // host, so a Windows build correctly accepts a regional entry's ".so"
    // platform URL too - install() re-sanitises the actual destination name
    // on its own, separately, as it always has.
    static bool isRegionalDownloadUrl(const std::string& url, const std::string& prefix);

    // Should the regional list even be asked for, given the catalogue URL the
    // user is currently pointed at and whether the test override is set?
    // True when `catalogueUrl` equals defaultIndexUrl() (the ordinary case)
    // OR `overrideSet` is true (the test seam always wins, so a test can
    // exercise the regional path against a fixture without also using the
    // default catalogue URL). A user who has pointed the store at a corporate
    // share or a local file gets neither - no foxsdr.com request is made on
    // their behalf.
    static bool regionalWanted(const std::string& catalogueUrl, bool overrideSet);

    // What mergeRegional() decided. `merged` is every public entry, unchanged
    // and in the same order, followed by the regional entries that survived,
    // each with `regional` set true. `added` is how many of those there are.
    // `dropped` is one (id, reason) pair per regional entry that was refused
    // wholesale - used by tests only; the running application never shows or
    // logs it (see the REGIONAL CATALOGUE block: a regional outcome is never
    // disclosed).
    struct RegionalMergeResult {
        std::vector<PluginCatalogEntry> merged;
        int added = 0;
        std::vector<std::pair<std::string, std::string>> dropped;
    };

    // Folds `regionalEntries` into `publicEntries`. PURE: no filesystem, no
    // network. A regional entry is dropped WHOLE - never partially trusted -
    // when any of these holds:
    //   - its id equals any public entry's id (public always wins);
    //   - its id appears more than once within regionalEntries itself
    //     (ambiguity is refused, not resolved by picking either copy);
    //   - ANY of its platform URLs fails isRegionalDownloadUrl(url,
    //     downloadPrefix) - one bad platform condemns the whole entry,
    //     including a platform whose own URL was fine.
    // A regional entry that survives carries NO screenshots (0.99.72): a picture
    // is fetched from the address its entry names, and a regional list must not be
    // able to make the store contact an address of its choosing.
    static RegionalMergeResult mergeRegional(const std::vector<PluginCatalogEntry>& publicEntries,
                                             const std::vector<PluginCatalogEntry>& regionalEntries,
                                             const std::string& downloadPrefix);

    // ---- Plugin sites (see the PLUGIN SITES block above) ------------------
    //
    // THE KEY AN ENTRY IS NAMED BY WHEREVER ONE STRING HAS TO STAND FOR IT (the
    // store's page, busy and result ids, its census names, ImGui ids, ADD ALL's
    // queue): the plain id for an official entry - so every name and id that
    // existed before sites is unchanged - and "<id>@<site label>" for a site's.
    // Unique within one merged catalogue because mergeSite() drops a repeated id.
    static std::string catalogueKey(const std::string& source, const std::string& id);

    // The host (and port, when not the default) of an https site address, lower-
    // cased: "raw.githubusercontent.com". Empty for anything that has none - a
    // local path, a non-https address, an address with userinfo ("a@b"), a
    // backslash, a control character or a space in its authority - so an
    // address that cannot be locked to a host is never read as locked to "".
    static std::string urlHost(const std::string& url) { return pluginSiteHost(url); }

    // What the store calls a site: its host, or - for a local file - the file's
    // name. Never empty for a non-empty address.
    static std::string siteLabel(const std::string& siteUrl) { return pluginSiteLabel(siteUrl); }

    // The address as one short line for a list: host + path, cut in the middle
    // with "..." to at most `maxChars` bytes (UTF-8 safe: only ASCII is cut).
    static std::string siteDisplay(const std::string& siteUrl, std::size_t maxChars) { return pluginSiteDisplay(siteUrl, maxChars); }

    // THE ONE VALIDATOR OF A SITE ADDRESS - the config's loader, the store's ADD
    // and the merge all ask it. Trims; refuses empty, longer than 2048 bytes, a
    // control character, and any address with "://" that is not https with a
    // lockable host (urlHost). A value with no "://" is a local file path, as the
    // official address may be. On success `out` is the trimmed address; on
    // failure `error` is a reason in English and `out` is empty.
    static bool normaliseSiteUrl(const std::string& raw, std::string& out, std::string& error) { return normalisePluginSiteUrl(raw, out, error); }

    // The host lock (rule 10): may a file at `url` belong to the site at
    // `siteUrl`? True iff `siteUrl` is a local path (no host to lock to), or
    // `url` is https and urlHost(url) is non-empty and equals urlHost(siteUrl).
    static bool siteFilesAllowed(const std::string& url, const std::string& siteUrl);

    // The site's kept copy in the plugins folder (rule: not counted by the folder
    // signature): "catalogue-<first 16 hex of sha256(siteUrl)>.json" and the same
    // + ".time". isSiteCacheFile() is true for exactly those two names and their
    // ".part" - nothing else - and is what core/plugin_dir_signature.cpp skips.
    static std::string siteCacheFileName(const std::string& siteUrl);
    static std::string siteCacheTimeFileName(const std::string& siteUrl);
    static bool isSiteCacheFile(const std::string& fileName);

    // What mergeSite() decided. `merged` is every entry of `listed`, unchanged
    // and in the same order, followed by the site's entries that survived, each
    // carrying source = the site's address. `added` is how many of those there
    // are; the two `hidden` counts are the entries dropped, each counted once,
    // the host lock first (it is the graver finding).
    struct SiteMergeResult {
        std::vector<PluginCatalogEntry> merged;
        int added = 0;
        int hiddenSameId = 0;   // id already listed: official wins
        int hiddenOffHost = 0;  // a platform or picture url on another host
    };

    // Folds one site's parsed entries into the list already shown. PURE. An entry
    // is dropped WHOLE when any platform url or picture url fails
    // siteFilesAllowed(), or when its id equals any entry already in the merged
    // list (official, regional, an earlier site, or earlier in this site's own
    // list). A survivor has `source` forced to siteUrl whatever it held, and
    // `regional` cleared: those two flags are this function's to give.
    static SiteMergeResult mergeSite(const std::vector<PluginCatalogEntry>& listed,
                                     const std::vector<PluginCatalogEntry>& siteEntries,
                                     const std::string& siteUrl);

    // Host identity used to pick a platform record. Exposed so tests and the
    // UI agree with the matcher instead of re-deriving it.
    static const char* hostOs() { return pluginHostOs(); }
    static const char* hostArch() { return pluginHostArch(); }

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
    //
    // THE SHOP-WINDOW FIELDS (0.99.72: category, experimental, whatsNew,
    // published, screenshots, capabilities) are optional and cosmetic, so they
    // are read more gently than everything above: a field of the wrong type, a
    // malformed date, a capability that is not a string, and a screenshots entry
    // that is not an object, lacks a url, sha256 or caption, has a url that is not
    // https or a sha256 that is not 64 hexadecimal digits, are each DROPPED - the
    // plugin and the catalogue survive - and, when `report` is given, one English
    // line per drop says which plugin, which item and why (nothing is appended
    // when nothing was dropped). `report` is for the log and for tests; the
    // caller owns it and parseIndex() never clears it. On a REFUSED index it may
    // hold what was noted before the refusal: read it only after a true return.
    static bool parseIndex(const std::string& json, std::vector<PluginCatalogEntry>& out,
                           std::string& error, std::vector<std::string>* report = nullptr);

    // The ABI bit a catalogue capability NAME stands for ("CASCADE_CAP_IQ_DECODER"
    // -> CASCADE_CAP_IQ_DECODER), or 0 for a name this host does not know. The
    // match is exact and case-sensitive: the names are the C macro names the
    // catalogue generator reads off the binary.
    static std::uint32_t capabilityBitForName(const std::string& name);

    // ---- Installed state by id and version (0.99.72) -----------------------
    //
    // Whether the plugin `e` describes is installed, and how its version stands
    // against the catalogue's - judged by plugin id and compareVersions(), NEVER
    // by file name (a file name carries its version, so the file the 1.8.1 entry
    // would write has never been on disk beside an installed 1.8.0).
    //
    // WHAT COUNTS AS INSTALLED, for `e.id`:
    //   - a scanned module whose id is `e.id` (the scan wins: it is the version
    //     the module itself declares, so a file replaced by hand is seen as what
    //     it is);
    //   - an install record for `e.id` whose file is not missing from the disk,
    //     UNLESS the scan knows that very file (then the scan's word was taken). A
    //     record the scan does not know is a module that was turned aside - a
    //     retired one - and is still installed.
    // Several copies: the newest version speaks. NotInstalled when there are none.
    //
    // THEN: the catalogue's version newer -> UpdateAvailable{installedVersion};
    // equal -> Installed; older -> NewerInstalled. The one more UpdateAvailable,
    // the same one planUpdates() plans: the versions are equal, the catalogue
    // entry targets this host's ABI and an install record says the installed
    // build targeted another (a rebuild at the same version number).
    //
    // PURE: no disk, no clock. It does not ask whether the entry can be
    // installed on this host (a build for it, an ABI): that is the install gate's
    // question, and an update that cannot run still is an update that exists.
    //
    // "SCANNED MODULE WHOSE ID IS e.id" AND "INSTALL RECORD FOR e.id" ARE BOTH BY
    // (source, id) (0.99.74): a record or module from another source - a site's
    // plugin that shares an official id, or the other way round - is not this
    // plugin and says nothing about it.
    static InstallState installStateFor(const PluginCatalogEntry& e,
                                        const std::vector<InstalledPlugin>& installed,
                                        const std::vector<ScannedModule>& scanned);

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
    static bool isHttpsUrl(const std::string& url) { return pluginIsHttpsUrl(url); }

    // ---- SHA-256 (Windows CNG; see rule 2) --------------------------------
    //
    // Lowercase hex of the SHA-256 digest. `data` may be null when n == 0.
    // On a non-Windows build both return false with a "not supported"
    // message rather than silently succeeding with no integrity check.
    static bool sha256Hex(const void* data, std::size_t n, std::string& hexOut,
                          std::string& error);
    static bool sha256File(const std::string& path, std::string& hexOut, std::string& error);

    // --- shared https transport -------------------------------------------
    //
    // Exposed so the update check and the update download use THIS client
    // rather than a second one. Every rule that makes a plugin download safe -
    // https only, no automatic redirect, a byte cap, certificate and
    // revocation checking left at their strict defaults - is enforced in one
    // place, and a second implementation would be a second place for those to
    // drift apart. The updater fetches an executable; it is the last thing
    // that should have its own bespoke networking.

    // Fetches a small text document. `maxBytes` bounds it so a hostile or
    // broken server cannot make the caller allocate without limit.
    static bool fetchText(const std::string& url, std::uint64_t maxBytes, std::string& out,
                          std::string& error);

    // Downloads to `destPath`, verifying sha256 BEFORE the file takes its
    // final name: a partial or substituted download is never left on disk as a
    // runnable file. Writes through a ".part" sibling and renames on success.
    //
    // STATIC, so it has no instance to take progress_/cancel_ from — the
    // caller passes its own pair, or nullptr for neither. Both are optional
    // and behave exactly as the member transfers': 0..1 stored as the bytes
    // arrive (left at 0 when the server declares no length), and the flag
    // polled between chunks so the transfer can be stopped. The updater is the
    // caller that needs them: an app-update download with no cancel is one
    // that a quit has to wait out.
    //
    // `failure`, when given, says WHICH KIND of failure ended it (0.99.64): the
    // update download counts a failed transfer and a failed verification
    // separately (core/health_events.hpp) and must not read that off the error
    // text.
    enum class FetchFailure {
        None,       // it succeeded
        Cancelled,  // the caller's flag stopped it: not a failure of anything
        Refused,    // refused before any transfer (not https, a malformed digest)
        Transfer,   // the download itself failed: network, HTTP status, size cap
        Verify,     // the bytes arrived and did not hash to the published digest
        Place       // the disk: the temporary file, the hash read, the final move
    };
    static bool fetchVerifiedFile(const std::string& url, const std::string& expectedSha256,
                                  const std::string& destPath, std::uint64_t maxBytes,
                                  std::string& error, std::atomic<float>* progress = nullptr,
                                  std::atomic<bool>* cancel = nullptr,
                                  FetchFailure* failure = nullptr);

    // THE TRANSPORT SEAM FOR TESTS (0.99.64). Every download and every document
    // this class fetches goes through one function, httpsGet, and until now the
    // only way to prove what happens after a transfer - a digest that does not
    // match, a body that is not a catalogue, a server that answers 500 - was a
    // live HTTPS server, which no test has. When this is set, httpsGet hands the
    // URL to it INSTEAD of opening a connection: it feeds `sink` the bytes the
    // "server" sends (and returns false with `error` for one that failed), and
    // every rule that is enforced AFTER the transfer - the hash, the cap, the
    // parse, the temporary file and the rename - runs for real. The https-only,
    // redirect and host rules are enforced BEFORE httpsGet and are untouched.
    // Empty (the default, and in every shipped build: nothing but a test calls
    // this) restores the network. Not thread-safe against a transfer already in
    // flight: set it before one starts.
    using TestTransport = std::function<bool(const std::string& url, std::uint64_t maxBytes,
                                             const std::function<bool(const void*, std::size_t)>& sink,
                                             std::string& error)>;
    static void setTransportForTest(TestTransport transport);

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

    // THE SENTENCE A USER IS SHOWN for module file `file` when loadInventory
    // found its bytes are not the ones sha256-verified at install
    // (InstalledPlugin::digestMismatch), or EMPTY when no record for that file
    // says so. Matched case-insensitively, as NTFS names are. The fitted
    // modules window draws it on the module's plate: the re-hash exists to
    // catch a manual overwrite, disk corruption or something worse, and a
    // finding nobody is told about is a finding thrown away.
    static std::string changedSinceInstallNote(const std::vector<InstalledPlugin>& records,
                                               const std::string& file);

    // ---- Cached catalogue policy ------------------------------------------
    //
    // The policy for `id`, or a CachedPolicy with known == false if no
    // catalogue has ever described it. Never throws, never blocks.
    //
    // BY (source, id) (0.99.74, rule 8). The two-argument form is the OFFICIAL
    // plugin's (source empty) and is what every caller that predates sites
    // meant; a plugin from a site asks with its own source and can never be
    // answered with an official plugin's floor, nor the reverse.
    static CachedPolicy policyFor(const std::vector<CachedPolicy>& policies,
                                  const std::string& id);
    static CachedPolicy policyFor(const std::vector<CachedPolicy>& policies,
                                  const std::string& source, const std::string& id);

    // Folds a freshly fetched catalogue into the cache. An id the catalogue
    // lists is REPLACED wholesale (the catalogue is authoritative for what it
    // publishes, including dropping a floor it no longer wants). An id the
    // catalogue does NOT list keeps its previous policy: a catalogue that is
    // unreachable, truncated to fewer entries, or replaced by an attacker's
    // empty document must not be able to un-retire a plugin.
    //
    // "AN ID" IS A (source, id) PAIR (0.99.74): an entry from a site replaces
    // only the cached policy of the same site's same id, so a site that lists an
    // official id - which the merge drops before it gets here, and which this
    // would not let through if it did not - can never replace or lift an
    // official plugin's retirement floor.
    static void mergePolicies(std::vector<CachedPolicy>& cached,
                              const std::vector<PluginCatalogEntry>& catalogue);

    // load -> mergePolicies -> save, for the one call the UI makes after a
    // successful fetch. This is the ONLY moment a policy is written.
    //
    // It writes ONLY WHEN THERE IS SOMETHING TO WRITE (0.99.63): a usable
    // manifest whose cached policies the merge left exactly as they were is not
    // rewritten, so its last-write time stays put. The plugin rescan reads that
    // time (core/plugin_dir_signature.hpp) to tell a changed folder from an
    // unchanged one, and a rewrite of identical bytes was reading as a change.
    // A missing or corrupt manifest is written, a new or moved policy is written.
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
    //
    // AN INSTALLED PLUGIN UPDATES ONLY FROM ITS OWN SOURCE (0.99.74): the record
    // and the entry are joined by (source, id), so a plugin installed from a
    // site is offered nothing by the official catalogue or by another site, and
    // an official plugin is offered nothing by a site, whatever ids they share.
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

    // The regional list's sibling of fetchIndex(), kept deliberately separate
    // rather than a second call to fetchIndex(): it does NOT touch entries_
    // or progress_ - the public catalogue this instance is holding is not
    // this call's business either way - and it uses kMaxRegionalIndexBytes,
    // not kMaxIndexBytes. It DOES share cancel_, passed to httpsGet exactly
    // as fetchIndex() passes it, so the destructor's pluginRepo_.cancel()
    // still aborts a stalled regional fetch; unlike fetchIndex() it does not
    // RESET cancel_ on entry, because a caller runs this immediately after a
    // successful public fetch and a cancel meant for that fetch must still
    // be able to stop this one rather than being silently cleared first.
    // Returns false with `error` set and `out` empty on any failure -
    // transport, HTTP status, oversized or malformed document; the caller
    // (AppWindow::startCatalogFetch) is the one place that decides a failure
    // here is swallowed rather than shown.
    bool fetchRegionalIndex(const std::string& url, std::vector<PluginCatalogEntry>& out,
                            std::string& error);

    // A PLUGIN SITE'S INDEX (0.99.74): the regional list's sibling in how it is
    // read - it does NOT touch entries_ or progress_, it shares cancel_ without
    // resetting it, and it uses the same short timeouts (4 s to connect, 8 s to
    // receive) so a site that is down costs the store seconds and not minutes -
    // but with kMaxSiteIndexBytes as its cap, a real parse report for the log,
    // and its failure SHOWN (the store prints it verbatim against the site).
    // `rawText` receives the text that parsed, for the site's kept copy. Returns
    // false with `error` set and `out` empty on any failure - not https, the
    // transport, an HTTP status, over the cap, not a catalogue. Not counted in
    // the anonymous health counters, which are the official catalogue's.
    bool fetchSiteIndex(const std::string& url, std::vector<PluginCatalogEntry>& out,
                        std::string& rawText, std::vector<std::string>& report,
                        std::string& error);

    const std::vector<PluginCatalogEntry>& entries() const { return entries_; }

    // The raw text of the index the last SUCCESSFUL fetchIndex() read (what
    // saveCatalogueCache() keeps), and the parse report of that same read (the
    // lines parseIndex() gave: what it dropped and why). Both empty after a
    // fetchIndex() that failed, exactly as entries() is. Read on the thread that
    // ran the fetch, after it returned.
    const std::string& lastIndexText() const { return lastIndexText_; }
    const std::vector<std::string>& lastParseReport() const { return lastParseReport_; }

    // ---- The catalogue cache (0.99.72) --------------------------------------
    //
    // The index the last good read produced, kept on disk beside installed.json
    // as `catalogue.json` with the time of that read in `catalogue.json.time`
    // (seconds since the epoch, one line), so the store can show the last known
    // list when the network is away and say how old it is, instead of an empty
    // window. It is the PUBLIC index only: the regional list's outcome is never
    // disclosed or kept (see the REGIONAL CATALOGUE block).
    //
    // saveCatalogueCache() is called ONLY after a read that parsed - never with
    // text that was refused, because a cache that holds what the client refused
    // would offer it next start. Each file is written whole to "<name>.part" and
    // renamed over the old one (the manifest's route), so a crash leaves the old
    // pair or the new file, never half of either; the pair is written text first.
    // Returns false with `error` on a bad argument (empty text, over
    // kMaxIndexBytes) or a failed write, and writes nothing in the first case.
    //
    // The plugin folder's signature (core/plugin_dir_signature.cpp) does NOT
    // count these two files - nor their ".part" - because a catalogue fetch that
    // changed them is not a change to which plugins there are, and counting them
    // made every fetch look like one (the 0.99.63 rescan fault).
    static bool saveCatalogueCache(const std::string& pluginsDir, const std::string& rawIndexText,
                                   std::int64_t readTimeUnix, std::string& error);

    // Reads the cache back through the ordinary parseIndex(), so a cache is held
    // to every rule a fetched index is. True with `entries` filled and `readTime`
    // set (0 when the time file is missing or unreadable: "not recorded", which
    // the caller draws as an unknown date, never as 1970). False with `error` -
    // a reason in English - when there is no cache, it cannot be read, or it does
    // not parse (a corrupt cache is IGNORED, never repaired and never deleted);
    // `entries` is empty then.
    static bool loadCachedIndex(const std::string& pluginsDir,
                                std::vector<PluginCatalogEntry>& entries, std::int64_t& readTime,
                                std::string& error);

    // ---- A site's kept copy (0.99.74) ---------------------------------------
    //
    // The same two-file, text-first, atomic route as the official kept copy, under
    // the site's own names (siteCacheFileName / siteCacheTimeFileName) and with
    // kMaxSiteIndexBytes as the cap. Like it, only text that parsed is ever kept,
    // and a corrupt copy is ignored, never repaired and never deleted.
    // loadSiteCache() returns the entries as parseIndex() read them - `source` is
    // NOT set: the caller runs them through mergeSite(), which is also where the
    // host lock is applied to a kept copy exactly as to a fresh read.
    static bool saveSiteCache(const std::string& pluginsDir, const std::string& siteUrl,
                              const std::string& rawIndexText, std::int64_t readTimeUnix,
                              std::string& error);
    static bool loadSiteCache(const std::string& pluginsDir, const std::string& siteUrl,
                              std::vector<PluginCatalogEntry>& entries, std::int64_t& readTime,
                              std::string& error);
    // Deletes the site's two kept files (and a ".part" a killed write left). True
    // when none remains afterwards. Touches no other file.
    static bool removeSiteCache(const std::string& pluginsDir, const std::string& siteUrl);

    // ---- Screenshots (0.99.72) ----------------------------------------------
    //
    // WHERE A PICTURE LIVES: "<pluginsDir>/store-cache/<sha256>.png". The name is
    // the catalogue's digest of the bytes, so a cached file is its own proof (it is
    // re-hashed before it is trusted) and two plugins naming one picture share it.
    // Empty for a digest that is not 64 hexadecimal digits.
    static std::string screenshotCachePath(const std::string& pluginsDir,
                                           const std::string& sha256);

    // Makes sure the picture is in the cache and says where. A cached file whose
    // bytes hash to `s.sha256` is returned WITHOUT any request; a cached file that
    // does not is deleted and fetched again. Otherwise: https only (refused before
    // any socket), the same transport as every other transfer here but with a short
    // leash - 4 s to connect, 15 s to receive - and a hard cap of
    // kMaxScreenshotBytes against the bytes that arrive; the body is streamed to
    // "<sha256>.png.part" while it is hashed, a digest that is not the catalogue's
    // deletes the part and refuses, and only a match is renamed into place. Nothing
    // is left behind on any failure. The request carries the URL and nothing else.
    // Blocking: off the UI thread. cancel() stops it. Does NOT touch progress():
    // that bar belongs to installs.
    bool fetchScreenshot(const std::string& pluginsDir, const CatalogScreenshot& s,
                         std::string& localPath, std::string& error);

    // Deletes the cached pictures the catalogue no longer names (and any ".part" a
    // killed run left), and returns the file names it deleted. It touches only files
    // of the shape the cache writes - "<64 hex>.png" and "*.part" - so anything
    // else a person put in the folder stays. Call it with a catalogue that was
    // just read successfully, never with the cached or an empty fallback.
    static std::vector<std::string> pruneScreenshotCache(
        const std::string& pluginsDir, const std::vector<PluginCatalogEntry>& catalogue);

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
    // a non-https url; a `regional` entry whose url is not under
    // regionalDownloadPrefix() (the SECOND check of that rule - see the
    // REGIONAL CATALOGUE block at the top of this file - because this
    // function does not trust its caller to have gone through
    // mergeRegional()); a site's entry (non-empty `source`) whose url is not on
    // the site's own host (rule 10, again, for the same reason), and one whose
    // destination file belongs to a different plugin or to no record at all
    // (rule 11).
    //
    // The anonymous failed-install count (health::notePluginInstallFailed) is the
    // official catalogue's: a failure on a site's entry is not counted.
    bool install(const PluginCatalogEntry& e, const std::string& pluginsDir,
                 std::string& installedPath, std::string& error);

    // Records one installed plugin in the manifest, and caches the catalogue
    // policy that came with it. Call it after a successful install() so the
    // plugin becomes managed - an unrecorded plugin is not broken, it is just
    // invisible to the retirement check (which then fails open for it).
    //
    // Upserts by (source, id) and writes the whole manifest atomically; the
    // record carries the entry's `source`. Returns false with `error` on a bad
    // entry (no build for this host, unsanitisable file name) or a failed write.
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

    // Deletes a plugin the HOST renamed aside rather than loaded. A retired or
    // ABI-mismatched plugin is quarantined as "<fileName><suffix>" (see
    // AppWindow::quarantineBlockedPlugins), and that name can never be passed
    // to remove() above: the sanitiser requires a ".dll" ending, deliberately,
    // because it is the same allow-list that refuses "evil.dll.exe". Loosening
    // it to let a quarantined name through would weaken every install.
    //
    // So `fileName` here is the plugin's REAL name and is sanitised exactly as
    // remove() sanitises it; `suffix` is appended only AFTER that check
    // passes, and is itself required to be a plain extension. The caller
    // therefore cannot reach outside pluginsDir by either argument.
    //
    // Exists because without it a user whose plugin is retired for an ABI
    // change has no way to delete it from the application at all: no catalogue
    // update can fix an ABI mismatch, so the panel offers no action, and the
    // file stays quarantined forever.
    bool removeQuarantined(const std::string& pluginsDir, const std::string& fileName,
                           const std::string& suffix, std::string& error);

    // 0..1 for the transfer in flight. Stays at 0 when the server sends no
    // Content-Length (progress that lies is worse than progress that waits).
    float progress() const { return progress_.load(std::memory_order_relaxed); }
    // TESTS ONLY - nothing in the product calls it: what a transfer's worker would have stored, so a test can
    // watch the live read of progress() (the plugin store's STORE_ACTIVITY, API 0.3 8.9) without a network.
    void setProgressForTest(float p) { progress_.store(p, std::memory_order_relaxed); }

    // Asks the transfer in flight to stop; the install then fails cleanly with
    // "cancelled" and removes its temp file. Safe to call from another thread.
    // install()/fetchIndex() clear the flag on entry, so a cancel that arrives
    // between two operations does not silently kill the next one.
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }

private:
    // install() as it always was, plus the CLASS of the failure it ended in
    // (0.99.64): install() counts a failed install anonymously by class
    // (core/health_events.hpp) and the class is known only where the failure
    // happens. `failClass` starts as Other - a refusal before any transfer.
    bool installImpl(const PluginCatalogEntry& e, const std::string& pluginsDir,
                     std::string& installedPath, std::string& error,
                     health::InstallClass& failClass);

    std::vector<PluginCatalogEntry> entries_;
    std::string lastIndexText_;                // see lastIndexText()
    std::vector<std::string> lastParseReport_;  // see lastParseReport()
    std::atomic<float> progress_{0.0f};
    std::atomic<bool> cancel_{false};
};

}  // namespace cascade::core
