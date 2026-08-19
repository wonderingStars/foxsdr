// updater.hpp - "there is a newer FoxSDR, and here is what it fixes".
//
// WHY THIS EXISTS. Nothing ever told an installed copy that a newer one
// existed. When 0.55.0 fixed a fault that stopped EVERY earlier build from
// detecting any radio at all, there was no way to reach the people running
// one: downloads are anonymous by design, and of 49 people who took a broken
// build, 46 never came back to the site. They are still running it. This is
// how that stops being true for the next fault.
//
// WHAT IT IS NOT. It does not update anything by itself. The check is a GET
// that returns a version and some text; downloading and installing happen only
// when the user presses a button, and the same rules the plugin catalogue
// follows apply to the download: https only, a size cap, no cross-host
// redirect, and the bytes become an executable ONLY after their sha256 matches
// the digest the server published. An updater that skipped that would be a
// remote code execution feature with a friendly name.
//
// WHAT IT SENDS. The version currently running, as a query parameter, so the
// server can say what changed since. Nothing else - no install id, no
// identifier of any kind, and no cookie is kept. The usage report is a
// separate, disclosed thing (see telemetry.hpp); this is not it, and the two
// share nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_UPDATER_HPP
#define CASCADE_CORE_UPDATER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

// One release's worth of "what changed", as the server published it.
struct ReleaseNote {
    std::string version;
    std::string date;
    std::vector<std::string> notes;
    bool critical = false;
};

// The answer to "is there a newer build".
struct UpdateInfo {
    bool newer = false;       // strictly newer than what is running
    bool critical = false;    // fixes something that made this build not work
    std::string version;      // the version offered
    std::string url;          // https link to its installer
    std::string sha256;       // what the download must hash to
    std::uint64_t sizeBytes = 0;
    std::vector<ReleaseNote> notes;  // newest first, only what this build has not seen
};

// Orders two version strings: -1 older, 0 same, +1 newer.
//
// Dotted parts compare as NUMBERS. Comparing them as text is the classic way
// an update check tells everybody they are current for ever, because "0.9.0"
// sorts above "0.10.0".
//
// A pre-release suffix (0.56.0-nightly.20260819.abc1234) is OLDER than the
// release of the same number, per semver - otherwise a nightly would be
// offered as an upgrade to the stable that superseded it, permanently.
int compareVersions(const std::string& a, const std::string& b);

// Parses the /api/update document. Pure, so the whole decision is testable
// without a network: a malformed or hostile answer must not be able to make
// the application download anything.
//
// REFUSES, rather than accepting with a warning:
//   - a url that is not https, or not on the expected host
//   - a sha256 that is not 64 hex characters
//   - a version that does not parse
// because each of those is the field that makes the download safe.
bool parseUpdateManifest(const std::string& json, const std::string& currentVersion,
                         UpdateInfo& out, std::string& error);

// Asks the server. Blocking, so call it off the UI thread; every failure is
// reported through `error` and never thrown.
//
// `baseUrl` is the endpoint ("https://foxsdr.com/api/update"), `channel` is
// empty for stable or "nightly".
bool checkForUpdate(const std::string& baseUrl, const std::string& currentVersion,
                    const std::string& channel, UpdateInfo& out, std::string& error);

// Where the check goes. Overridable with FOXSDR_UPDATE_URL so a test can point
// at a local stub, and so the endpoint can move without orphaning binaries.
std::string updateEndpoint();

// Downloads the installer named by `info`, verifies its sha256, and leaves it
// in a temp directory. Returns the path through `outPath`.
//
// The digest is checked BEFORE the file is given its final name, so a partial
// or substituted download is never a runnable .exe on the user's disk - the
// same order the plugin installer uses.
bool downloadUpdate(const UpdateInfo& info, std::string& outPath, std::string& error);

}  // namespace cascade::core

#endif  // CASCADE_CORE_UPDATER_HPP
