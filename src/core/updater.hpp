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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
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

// Is this a version this product could actually have published?
//
// Accepts a dotted numeric core with an optional pre-release suffix, and
// nothing else: 0.58.0 and 0.57.0-nightly.20260819.b97092e both pass.
//
// THIS IS A SECURITY CHECK, not a tidiness one. The version names the
// installer that downloadUpdate writes into a temp directory, and that file is
// then handed to the shell, so an unvalidated version out of a manifest is a
// path traversal to an arbitrary executable. A structural parse is used rather
// than a list of banned characters, because ".." contains no banned character.
bool wellFormedVersion(const std::string& v);

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
//
// PROGRESS AND CANCEL are the caller's atomics, borrowed for the duration and
// both optional. The transfer stores 0..1 into `progress` as the bytes arrive
// (it stays at 0 when the server declares no Content-Length, because progress
// that lies is worse than progress that waits), and polls `cancel` before it
// connects and again between chunks, so a quit does not have to wait out the
// download. `progress` is reset to 0 on entry: a second attempt must not start
// its bar wherever the failed first one stopped.
//
// WHY THEY ARE NOT PluginRepo's. The plugin browser's bar and this one are
// different transfers that can be in flight at the same time, and PluginRepo's
// own progress_/cancel_ belong to whatever install() or fetchIndex() is doing;
// sharing them would have this bar show that transfer's percentage and a
// plugin cancel abort the app update. The caller therefore owns a pair.
bool downloadUpdate(const UpdateInfo& info, std::string& outPath, std::string& error,
                    std::atomic<float>* progress = nullptr,
                    std::atomic<bool>* cancel = nullptr);

// What one update check came back with - the whole of it, BY VALUE, so the
// worker that produced it writes nothing that anybody else owns.
struct UpdateCheckOutcome {
    bool ok = false;
    UpdateInfo info;
    std::string error;
};

// The once-per-launch update check, run off the GUI thread, AND ABANDONABLE.
//
// WHY IT IS A TYPE. The check is a blocking GET with no cancel that can reach
// it: httpsGet polls a flag only between chunks, and a check stalled inside
// WinHttpConnect / SendRequest / ReceiveResponse sits there until WinHTTP's own
// timeouts (10/10/20/30 s, per redirect hop) give up. It used to live in a
// std::async future that was a member of AppWindow, and such a future's
// destructor BLOCKS until the worker returns - so quitting while foxsdr.com
// was slow held ~AppWindow, window gone and process still running, for as long
// as those timeouts took (bug hunt 2026-09-24, updater-installer-1).
//
// WHAT MAKES ABANDONING SAFE is that the work returns its outcome by value and
// captures nothing it does not own: no `this`, no member slot to write into
// after the owner has gone. reap() waits a short grace (a check that is
// finished, or nearly, is taken right there) and otherwise hands the future to
// a detached drainer, which takes the result and throws it away. The check
// touches no static state after its network call returns, so a drainer still
// running at process exit is simply ended with the process.
class UpdateCheckTask {
public:
    using Work = std::function<UpdateCheckOutcome()>;

    // How long reap() - and so the destructor - waits before abandoning.
    static constexpr std::chrono::milliseconds kQuitGrace{250};
    // poll()'s ready-check: zero by construction, named so it is not a bare
    // literal duration (tests/test_shutdown_budget.cpp scans for those).
    static constexpr std::chrono::milliseconds kNoWait{0};

    UpdateCheckTask() = default;
    ~UpdateCheckTask();
    UpdateCheckTask(const UpdateCheckTask&) = delete;
    UpdateCheckTask& operator=(const UpdateCheckTask&) = delete;

    // Starts `work` on its own thread. Ignored while a check is in flight.
    void start(Work work);

    // True from start() until poll() has handed the outcome over, or reap()
    // has let it go.
    bool running() const { return future_.valid(); }

    // True ONCE, when the work has finished: `out` then holds its outcome.
    // Never blocks.
    bool poll(UpdateCheckOutcome& out);

    // Waits up to `grace` for the work; if it is still running, abandons it to
    // a detached drainer. Returns true when the work had finished (or nothing
    // was running), false when it was abandoned. Either way running() is false
    // afterwards and nothing this object owns is waited on again.
    bool reap(std::chrono::milliseconds grace = kQuitGrace);

private:
    std::future<UpdateCheckOutcome> future_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_UPDATER_HPP
