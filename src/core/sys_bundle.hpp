// sys_bundle.hpp - the settings bundle: ONE file that carries the program's settings and bookmarks off a machine (export), back (import) and aside (backup).
//
// FORMAT (version 1, one JSON object, at most kBundleMaxTextBytes):
//   {"format":"foxsdr-settings-bundle","version":1,"app":"<version text>","created":"<UTC stamp the caller gives>","stripped":["config.json:webPasswordRecord",...],
//    "files":[{"name":"config.json","size":N,"sha256":"<64 hex>","data":"<base64 of the file's bytes>"}, ...]}
// The file names are an ALLOW-LIST (config.json, bookmarks.json, markers.json): a name is never a path, so there is nothing to traverse. Files are STORED, not compressed, so a
// small bundle cannot expand into a large one; the importer also checks every declared size against its limits and against the data actually present BEFORE it decodes anything
// (file count, per-file bytes, total bytes, nesting depth of the JSON inside), and checks the sha256 of every decoded file against the manifest.
//
// SECRETS NEVER LEAVE. Export drops, from every JSON file and at any depth, every key whose name suggests a password, token, key, secret or credential (webPasswordRecord, testerToken,
// testerAppToken, ...), and - unless the caller turns it off - the install identity (telemetry install id and counters, the pending usage record), and lists the dropped key
// PATHS (never values) in "stripped". A bundle that arrives with such keys anyway (hand-made or from an older build) has them dropped again on import, and every network LISTENER
// switch inside an imported config is forced OFF with a note: a settings file from outside must never open a port the user did not open (the rule is that anything that listens is off
// until the user starts it).
//
// IMPORT NEVER OVERWRITES SILENTLY. parseBundle validates (all-or-nothing); planImport compares with the destination folder and makes a CONFIRM TOKEN from the staged and the present
// contents; commitImport refuses unless the token matches (so it also refuses if the destination changed after the plan was shown), keeps every replaced file as
// "<name>.before-import-<stamp>", writes each new file beside its target and renames it into place, and puts everything back if any step fails.
//
// Nothing here touches the real settings folder unless a caller hands it that folder: every function takes the folders as arguments. The wiring (the UI's Export / Import / Backup
// buttons) is a hand-off in the record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_BUNDLE_HPP
#define CASCADE_CORE_SYS_BUNDLE_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace cascade::core::sys {

inline constexpr int kBundleVersion = 1;
inline constexpr std::size_t kBundleMaxFiles = 8;
inline constexpr std::size_t kBundleMaxFileBytes = 8u * 1024 * 1024;
inline constexpr std::size_t kBundleMaxTotalBytes = 16u * 1024 * 1024;
inline constexpr std::size_t kBundleMaxTextBytes = 24u * 1024 * 1024;
inline constexpr int kBundleMaxJsonDepth = 64;

enum class BundleError {
    None,
    TooLarge,         // the text, a file, or the total is over its limit
    NotJson,
    NotABundle,       // JSON, but not this format
    UnknownVersion,
    BadManifest,      // a field missing or of the wrong type, a bad hash text
    TooManyFiles,
    EmptyBundle,
    BadName,          // not on the allow-list
    DuplicateName,
    BadBase64,
    SizeMismatch,
    HashMismatch,
    BadContent,       // a file that is not the JSON object it must be, or nested too deep
};
const char* bundleErrorName(BundleError e);

struct BundleFile {
    std::string name;
    std::string bytes;
};

bool allowedBundleFileName(const std::string& name);

struct ExportOptions {
    bool stripIdentity = true;     // also drop the telemetry install id and counters
    std::string appVersion;        // for the manifest ("" = omitted)
    std::string createdUtc;        // e.g. "20261111-221530"; the caller supplies it (no clock here), "" = omitted
};

struct ExportReport {
    BundleError error = BundleError::None;
    std::string detail;
    std::vector<std::string> stripped;   // "file:key.path" of everything dropped (no values)
};

// Builds the bundle text from the files' current bytes. Each must be on the allow-list and valid JSON (an object); a bad one fails the whole export.
std::string exportBundle(const std::vector<BundleFile>& files, const ExportOptions& opt, ExportReport& report);

struct ImportResult {
    BundleError error = BundleError::None;
    std::string detail;
    std::vector<BundleFile> files;       // validated, sanitised bytes (secrets dropped, listeners off)
    std::vector<std::string> notes;      // what the importer changed or dropped, in words
    std::string appVersion;              // from the manifest, informational
};

ImportResult parseBundle(const std::string& text);

enum class ImportAction { New, Replace, Identical };
struct ImportPlanItem {
    std::string name;
    ImportAction action = ImportAction::New;
    std::string newSha256, presentSha256;   // present = "" when the file is not there
};
struct ImportPlan {
    std::vector<ImportPlanItem> items;
    std::string confirmToken;   // "IMPORT-" + 8 hex
};

// Compares an ImportResult with the folder it would go into; writes nothing.
ImportPlan planImport(const ImportResult& r, const std::filesystem::path& destDir);

// Applies the plan. `token` must equal the plan's confirm token recomputed NOW. `stamp` names the kept copies. false with `error` set; on failure the folder is as it was.
bool commitImport(const ImportResult& r, const std::filesystem::path& destDir, const std::string& token, const std::string& stamp, std::string& error);

// The names for backups and their housekeeping.
std::string backupFileName(const std::string& stamp);                                                // "foxsdr-settings-<stamp>.foxsdr-bundle"
bool isBackupFileName(const std::string& name);                                                      // exactly that shape (stamp = 8 digits, '-', 6 digits)
std::vector<std::string> pruneBackups(const std::filesystem::path& dir, std::size_t keepNewest);     // deletes only our own backup files beyond the newest N; returns their names

// The key-name rule, exposed for tests and for the UI's "what was left out" text.
bool secretKeyName(const std::string& key);
bool identityKeyName(const std::string& key);

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_BUNDLE_HPP
