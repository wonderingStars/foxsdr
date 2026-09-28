// tester_usage.hpp - what a BETA TESTER's own build reports back, once they
// have pasted their tester code into Beta tester (SYSTEM bank).
//
// THIS IS NOT THE ANONYMOUS USAGE REPORT (core/telemetry.hpp). Telemetry is
// on by default, opt-out, and never identifies a person - it counts an
// install, never a tester. This is the opposite shape: OFF until a tester
// pastes the code their tester-portal entry carries, tied to that one entry
// on foxsdr.com, and it exists to answer one question the owner cannot answer
// any other way - which of the features a tester SAID they would cover did
// they actually run. Pasting the code IS the opt-in; with no code, nothing
// here collects or sends anything, ever.
//
// WHAT IT SENDS, once a session ends: the app version, platform and
// architecture, when the session started and how long it ran, which
// BetaFeatures ids were used, which installed plugins ran and for how long,
// and which radio driver kinds were opened. NEVER a frequency, NEVER decoded
// content, NEVER a position - the same exclusions telemetry.hpp documents,
// because this payload is built from the same kind of counters and the same
// discipline applies.
//
// THE TOKEN IS A CREDENTIAL, not an anonymous id. It is the tester's own
// management token from the beta portal (foxsdrWebsite's beta.go), the same
// one that opens https://foxsdr.com/#t=<token>. It is stored in the config
// file because the tester typed it once and should not have to again; it is
// NEVER written to a log line, a diagnostics bundle or a crash report (see
// tests/test_tester_usage.cpp), and it is masked everywhere it is displayed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TESTER_USAGE_HPP
#define CASCADE_CORE_TESTER_USAGE_HPP

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_repo.hpp"

namespace cascade::core {

// One plugin's contribution to a session: the CATALOGUE id (never the
// display name - two installs of the same decoder in different directories
// would otherwise report as two different plugins, and the site's dashboard
// joins on the catalogue id the way everything else about a plugin does),
// the version that ran, and how many whole minutes it was actually fed
// samples (never merely loaded).
struct TesterUsagePlugin {
    std::string id;
    std::string version;
    std::uint64_t minutes = 0;
};

// One session's worth of counters - built once, at the point a session ends,
// from whatever TesterUsageRecorder accumulated during it.
struct TesterUsageSession {
    std::string start;    // RFC3339 UTC, e.g. "2026-09-28T12:34:56Z"
    std::uint64_t minutes = 0;  // whole minutes the session ran, 0..1440
    std::vector<std::string> features;       // BetaFeatures ids, deduplicated
    std::vector<TesterUsagePlugin> plugins;  // deduplicated by id
    std::vector<std::string> radios;         // driver kinds, deduplicated
};

// What is actually transmitted, exactly the contract's shape and nothing
// else - see the field inventory test in tests/test_tester_usage.cpp.
struct TesterUsageReport {
    std::string token;
    std::string version;    // the running app's version string
    std::string platform;   // "windows" | "linux" | "android"
    std::string arch;       // "x64" | "arm64"
    TesterUsageSession session;

    // Compact JSON, exactly the contract's fields. error_handler_t::replace
    // like every dump() in this tree (tests/test_json_dump_policy.cpp) - a
    // plugin name or a hand-typed token is not this process's own text, and a
    // bad byte in either must cost one replacement character, never a throw.
    std::string toJson() const;
};

// "2026-09-28T12:34:56Z" - the one RFC3339 stamp this file needs, built the
// same way crash_upload.cpp's epochStamp() is (gmtime_s/gmtime_r), so a
// changed clock or timezone can never make the two disagree about what "now"
// was.
std::string rfc3339Utc(std::time_t t);

// True when `token` is exactly 32 lowercase hex characters - the same shape
// core::validInstallId checks, because the site mints both with
// randomID(16)/hex.EncodeToString (beta.go, useragent.go). A hand-edited
// config that put something meaningful here (an email, a name) is refused
// rather than sent.
bool validTesterToken(const std::string& token);

// Finds a tester's token in whatever they pasted: the bare 32-character
// token, or the whole private portal link the site hands them
// (https://foxsdr.com/#t=<token>, web/portal.js's manageLink - the token
// lives in the URL FRAGMENT, never a query string, so it never reaches a
// server log). Whitespace at either end is trimmed first. Returns "" for
// anything else, including a confirmation link (#c=), which opens no portal
// and is not a credential this field accepts.
std::string extractTesterToken(const std::string& input);

// "windows" | "linux" | "android" - the platform value the contract wants,
// read from the same compile-time targets telemetry.hpp's osDescription()
// switches on, so the two can never name the platform two different ways.
std::string testerUsagePlatform();

// https://foxsdr.com/api/tester-usage, overridden by FOXSDR_TESTER_USAGE_URL
// - the same seam shape as telemetryEndpoint() and crashUploadEndpoint(), so
// a test or a self-capture points this at a local stub the same way it
// points at theirs.
std::string testerUsageEndpoint();

// What happened to one POST, matching the contract's response rules exactly:
//   Sent      2xx - the server accepted it; drop this report
//   Invalid   401 - the code is no longer valid; stop sending, keep the code
//   Rejected  400/413 - malformed or oversized; log it and drop it, it will
//             never succeed by being retried
//   Retry     429, or any transport/network failure - keep it for the next
//             launch, bounded (see TesterUsageQueue below)
enum class TesterUsageOutcome { Sent, Invalid, Rejected, Retry };

// One blocking POST. Callers choose their own thread and timeout (see
// TesterUsageSender) - this function never spawns one itself, so a test can
// call it directly against a local server with no thread to synchronise
// with. https only, exactly like telemetry's transport and for the same
// reason: a tester's token is a credential, and sending it in clear would put
// it on the wire for any network in between to collect. A non-https url, or
// one that fails to parse, returns Retry without opening a socket - the same
// "nothing sent, no throw" contract every seam like this one keeps.
TesterUsageOutcome postTesterUsage(const std::string& url, const std::string& json,
                                    int connectTimeoutMs, int rwTimeoutMs);

// Fire-and-forget, one report at a time, mirroring core::TelemetryReporter:
// NOT detached. The destructor joins, because a detached thread still writing
// into a WinHTTP/socket handle while the process tears down is exactly the
// hazard telemetry.hpp's header documents, and it applies here identically.
//
// `onDone` runs ON THE WORKER THREAD once the POST returns or times out -
// callers must marshal anything that touches GUI state back to their own
// thread rather than acting on it directly (see AppWindow::testerUsagePoll).
class TesterUsageSender {
public:
    TesterUsageSender() = default;
    ~TesterUsageSender();
    TesterUsageSender(const TesterUsageSender&) = delete;
    TesterUsageSender& operator=(const TesterUsageSender&) = delete;

    // No-op when `url` or `json` is empty, or a send is already running.
    void send(const std::string& url, const std::string& json, int connectTimeoutMs,
              int rwTimeoutMs, std::function<void(TesterUsageOutcome)> onDone);

    bool busy() const;
    // True once the outstanding send (if any) has finished running onDone -
    // the caller reaps it with reap() before starting another.
    bool finished() const;
    // Joins a finished thread so busy() clears and a new send() can start.
    // Safe to call when nothing is running.
    void reap();

private:
    std::thread thread_;
    std::atomic<bool> done_{true};
};

// The bounded queue of finished-session reports still waiting to be sent -
// AppConfig::testerUsagePending's in-memory shape. "At most the last 3
// unsent reports": pushing a fourth drops the OLDEST, because for a queue
// that exists only to survive a bad network, the most recent sessions are
// the ones still worth telling the site about.
class TesterUsageQueue {
public:
    static constexpr std::size_t kMax = 3;

    void push(const std::string& reportJson);
    void removeFront();
    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    const std::string& front() const { return items_.front(); }
    const std::vector<std::string>& items() const { return items_; }
    void setItems(std::vector<std::string> items);

private:
    std::vector<std::string> items_;
};

// Records what a session actually used, from wherever the real event
// happens - a mode change, a device finishing its open, a decoder that just
// decoded something, a network server serving a client on ITS OWN thread.
// Mutex-guarded because of that last case: net/cat_server.cpp and
// net/web_server.cpp call into this from threads that are not the GUI
// thread's, and every operation here is cheap enough (a scan of a handful of
// short strings) that a shared lock costs nothing worth avoiding.
//
// ARMED BY THE PRESENCE OF A TOKEN, not by anything else. AppWindow calls
// setArmed(!token.empty()) whenever the token field changes; every note*/
// accrue* call below is a no-op while disarmed, which is what makes "no
// code, nothing collected" hold for every call site rather than needing each
// one to remember to check.
class TesterUsageRecorder {
public:
    void setArmed(bool armed) { armed_.store(armed, std::memory_order_relaxed); }
    bool armed() const { return armed_.load(std::memory_order_relaxed); }

    // Marks one BetaFeatures id used this session. Ignored when disarmed,
    // empty, or already noted; bounded like telemetry's panel list against a
    // caller that somehow loops.
    void noteFeature(const std::string& id);

    // Marks one radio driver kind opened this session - AppConfig::sourceKind's
    // plain vocabulary ("rtlsdr", "uhd", "siggen"...), never args, never a
    // serial: this is a session record, not a device inventory.
    void noteRadio(const std::string& kind);

    // Adds `seconds` of running time to one plugin, identified by its
    // CATALOGUE id and the version that was running. Called once per
    // accrual tick for every plugin currently being fed (see
    // AppWindow::testerUsageAccrue) - fractional seconds accumulate in a
    // double and are only rounded to whole minutes when the session's report
    // is built, on the same "do not truncate every tiny delta to zero" rule
    // telemetry's SecondAccrual documents.
    void accruePlugin(const std::string& id, const std::string& version, double seconds);

    std::vector<std::string> features() const;
    std::vector<std::string> radios() const;
    std::vector<TesterUsagePlugin> plugins() const;

    // Back to empty, for the session that starts next.
    void reset();

    static constexpr std::size_t kMaxFeatures = 32;
    static constexpr std::size_t kMaxRadios = 16;
    static constexpr std::size_t kMaxPlugins = 64;

private:
    std::atomic<bool> armed_{false};
    mutable std::mutex mu_;
    std::vector<std::string> features_;
    std::vector<std::string> radios_;
    struct Accrual {
        std::string version;
        double seconds = 0.0;
    };
    std::map<std::string, Accrual> plugins_;  // keyed by catalogue id
};

// Joins a loaded plugin (identified by its module FILE NAME, as
// std::filesystem::path(loadedPluginPath).filename() gives it) to the
// catalogue id its install record carries. Falls back to a lower-cased,
// hyphenated slug of the display name for a side-loaded plugin with no
// manifest entry - a best-effort id rather than nothing, made from the same
// name the catalogue itself would have used had this plugin ever been
// listed in one.
std::string catalogueIdForPlugin(const std::string& fileName, const std::string& displayName,
                                  const std::vector<InstalledPlugin>& installed);

}  // namespace cascade::core

#endif  // CASCADE_CORE_TESTER_USAGE_HPP
