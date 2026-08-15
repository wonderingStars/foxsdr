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
//     catalogue is inert data until a human acts on it.
//
// TESTABILITY. parseIndex(), sanitiseFileName() and the SHA-256 wrapper are
// static and pure (or filesystem-only), so the entire security-critical
// surface is exercised offline; only the transport itself needs a network.
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
