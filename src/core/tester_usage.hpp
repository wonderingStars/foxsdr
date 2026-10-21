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
// tests/test_tester_usage.cpp), and it is masked everywhere it is displayed
// - including the input field itself (ImGuiInputTextFlags_Password) and the
// "Show what is sent" preview, which shows the payload the server actually
// receives with the token masked the same way the Code line is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TESTER_USAGE_HPP
#define CASCADE_CORE_TESTER_USAGE_HPP

#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/crash_upload.hpp"
#include "core/plugin_repo.hpp"

namespace cascade::core {

// One plugin's contribution to a session: the CATALOGUE id (never the
// display name - two installs of the same decoder in different directories
// would otherwise report as two different plugins, and the site's dashboard
// joins on the catalogue id the way everything else about a plugin does),
// the version that ran, and how many whole minutes it was actually fed
// samples (never merely loaded).
//
// A plugin with NO install-manifest record - a side-loaded .dll/.so, or one
// whose manifest entry failed to parse - is never given a made-up id: it is
// reported under the fixed id kSideloadedPluginId with an empty version, so
// a tester who genuinely ran an unreleased or hand-built decoder still
// counts as having exercised "plugins" without inventing an identity for a
// binary the catalogue has never seen. See catalogueIdForPlugin below.
struct TesterUsagePlugin {
    std::string id;
    std::string version;
    std::uint64_t minutes = 0;
};

// The bucket every plugin with no install-manifest record is reported under.
// One shared id rather than a per-file slug: a slug would look like an
// identity the catalogue never assigned, and multiple side-loaded plugins
// are meant to fold into one signal ("something unreleased was used") rather
// than multiply into several invented ones.
inline constexpr char kSideloadedPluginId[] = "sideloaded";

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

// The site's own bounds on the fields it will accept, applied here so a
// report the app builds is one the site can actually store rather than
// something it has to be trusted to reject cleanly:
//   - version and token are length-capped (48 and 64 characters) rather than
//     rejected outright - a truncated version string is still informative,
//     and the token is always exactly 32 characters when it is valid at all
//     (validTesterToken), so this cap is a backstop, not a real limit.
//   - radio kinds are lower-cased (the site's own vocabulary is lower-case;
//     AppConfig::sourceKind's is too, so this should be a no-op in practice)
//     and re-deduplicated afterwards, since two different-case spellings of
//     one kind must not count as two.
//   - session.start is clamped into [now-365d, now+10min] - the site's own
//     window for "plausibly this session, not a clock gone wrong or a report
//     that sat in the retry queue for a year." `now` is the wall clock AT
//     THE TIME THIS FUNCTION RUNS, which is every debounced save while the
//     session is live and once more at the point it is queued - not the
//     time it is eventually SENT, which this function has no way to know.
//   - each plugin's minutes are clamped to session.minutes: a plugin cannot
//     plausibly have run longer than the session that fed it, and rounding
//     each independently (SecondAccrual's own carried-remainder scheme) can
//     otherwise put a plugin one minute over by coincidence. Plugins are
//     already folded by id alone (TesterUsageRecorder::accruePlugin) rather
//     than by id+version - the map holds one Accrual per id, so
//     "duplicates folded by id+version" already holds a fortiori.
// Returns a new report; `r` is not modified.
TesterUsageReport finalizeTesterUsageReport(TesterUsageReport r, std::time_t now);

// The bounds finalizeTesterUsageReport enforces, named so a test can assert
// against the same numbers rather than repeating them as magic constants.
inline constexpr std::size_t kTesterUsageMaxVersionChars = 48;
inline constexpr std::size_t kTesterUsageMaxTokenChars = 64;
inline constexpr std::int64_t kTesterUsageMaxSessionAgeSec = 365LL * 24 * 60 * 60;
inline constexpr std::int64_t kTesterUsageMaxSessionFutureSec = 10LL * 60;

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

// True when `token` is exactly 40 lowercase hex characters - the APP token's
// own shape, and deliberately a DIFFERENT length from validTesterToken's 32:
// the two credentials must never be confusable, either by a user pasting one
// into the wrong box or by code that forgot which flow it was in. Minted by
// the site as randomID(20) (see core/tester_link.hpp for how it is obtained
// and used) rather than randomID(16), for exactly this reason - see
// PORTAL-LINK-VERDICT.md finding 6(d).
bool validAppToken(const std::string& token);

// Trims `input` and returns it only if it is exactly a valid app token (see
// validAppToken) - no URL-fragment parsing, because the portal shows the app
// token as plain text with its own copy button (unlike the portal token,
// which is also embedded in a manageLink). Returns "" for anything else,
// including a valid 32-hex PORTAL token: this function is for the box that
// accepts an APP token only.
std::string extractAppToken(const std::string& input);

// First 4 and last 4 characters, joined with "...", the same shape wherever
// the token is shown at all - the "Code:" line and the "Show what is sent"
// preview both call this rather than each inventing their own masking, so
// there is exactly one place that decides how much of a credential a screen
// may show. A token 8 characters or shorter (never a real one - see
// validTesterToken) is masked in full rather than partially, so a
// hand-edited short value cannot leak more of itself than a real token
// would.
std::string maskTesterToken(const std::string& token);

// The exact bytes TesterUsageReport::toJson() would produce for `report`,
// EXCEPT the token field, which is masked exactly like maskTesterToken()
// masks it on screen. This is what "Show what is sent" actually displays -
// never the real toJson() output - so the preview cannot become a second
// place the token leaks in full.
std::string maskedPreviewJson(const TesterUsageReport& report);

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

// One blocking POST, on TOP OF core/crash_upload.hpp's shared transport
// (postBounded) rather than a second hand-rolled WinHTTP/httplib client:
// the two reporters have identical requirements (https except on loopback,
// so a test can use a plain socket; abortable mid-flight; a status code read
// back, nothing else). `cancel` is crash_upload.hpp's own UploadCancel -
// see TesterUsageSender below for why a SHARED cancel token, rather than a
// per-call one, is what makes a fire-and-forget sender safe to abandon
// without joining it.
TesterUsageOutcome postTesterUsage(const std::string& url, const std::string& json,
                                    const std::shared_ptr<UploadCancel>& cancel);

// Fire-and-forget, one report at a time. UNLIKE core::TelemetryReporter, the
// worker thread is DETACHED, not joined - see the header's own "never delays
// exit" requirement and AppWindow's shutdown sequence, which must never wait
// on a network call at all (0.99.42's exit-time attempt measured ~0.9-3.9s
// of added shutdown time against an unresponsive server and was removed
// entirely: this object no longer tries to be clever about a bounded exit
// send, it just never blocks).
//
// SAFE TO DETACH because nothing the worker touches after send() returns is
// owned by this object or its caller: `cancel_`, the in-flight flag and the
// outcome slot are each a separate heap allocation shared by shared_ptr, so
// the worker holds its own reference and keeps them alive for exactly as
// long as it needs them, whether or not this object - or the AppWindow that
// owns it - still exists. The destructor cancels an in-flight send (closing
// the transport's handle unblocks the worker promptly, the same mechanism
// core::CrashUploader uses) purely so an abandoned socket does not linger
// longer than it has to; it does not need to, and does not, wait for the
// worker to actually finish.
class TesterUsageSender {
public:
    TesterUsageSender() = default;
    ~TesterUsageSender();
    TesterUsageSender(const TesterUsageSender&) = delete;
    TesterUsageSender& operator=(const TesterUsageSender&) = delete;

    // No-op when `url` or `json` is empty, or a send is already running
    // (busy()). Detaches its worker thread immediately - there is nothing
    // to join.
    void send(const std::string& url, const std::string& json);

    // True from send() until the worker has produced an outcome (whether or
    // not takeOutcome() has been called yet).
    bool busy() const;

    // Non-blocking. Returns the finished send's outcome and clears it, or
    // nullopt if nothing has finished since the last call (or nothing was
    // ever sent). Once this returns a value, busy() is false and a new
    // send() may start.
    std::optional<TesterUsageOutcome> takeOutcome();

private:
    std::shared_ptr<std::atomic<bool>> inFlight_;
    std::shared_ptr<std::atomic<int>> outcomeSlot_;  // -1 = none yet
    std::shared_ptr<UploadCancel> cancel_;
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

    // Removes every entry whose own "token" field is not exactly `token` -
    // the fix for replacing a revoked or mistyped code: a report queued
    // under the OLD code must never be sent under the new one (it would
    // reach the wrong tester's entry on the site), and a report queued
    // before the code was removed must not survive into a later opt-in
    // under a different one. Called whenever the token changes (paste,
    // replace, remove) and once at start-up after folding in whatever the
    // config carried - see AppWindow::testerUsageStartup and
    // drawTesterUsageSection. An unparsable entry is dropped too: it cannot
    // be shown to have the right token, so it is treated as having the
    // wrong one.
    void dropOthers(const std::string& token);

    // Rewrites the "token" field of EVERY item to `newToken`, in place -
    // unlike dropOthers, which discards a mismatched item, this is for the
    // one case where an old report must be KEPT and relabelled: the portal-
    // to-app-token migration exchange (core/tester_link.hpp), which must not
    // throw away a session queued under the portal token just because the
    // credential it will be sent under changed (PORTAL-LINK-VERDICT.md
    // finding 6b). An item that fails to parse as a JSON object is left
    // completely untouched rather than dropped: it already survived
    // load-time filtering once, and silently losing it here would be worse
    // than sending it under a now-stale token one more time.
    void rewriteToken(const std::string& newToken);

private:
    std::vector<std::string> items_;
};

// The token embedded in a report's own JSON ("" if the text does not parse
// or has no such field) - what dropOthers() above compares against, and
// exposed on its own so a test can assert the comparison directly.
std::string tokenOfReport(const std::string& reportJson);

// The state change one finished send's outcome causes - pulled out of
// AppWindow::testerUsagePoll into a pure function so the decision itself
// (drop the front on Sent/Rejected, stop and mark the code invalid on
// Invalid, stop but keep it on Retry) is asserted directly by
// tests/test_tester_usage.cpp rather than only indirectly through the GUI
// class that calls it. Mutates `queue` (removeFront on Sent/Rejected only);
// never touches the token or the config - those are the caller's.
struct TesterUsageOutcomeEffect {
    bool nowInvalid = false;  // caller should set testerTokenInvalid = true
    bool stop = false;        // caller should stop trying more sends THIS session
};
TesterUsageOutcomeEffect applyTesterUsageOutcome(TesterUsageOutcome outcome,
                                                  TesterUsageQueue& queue);

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
    // CATALOGUE id (or kSideloadedPluginId) and the version that was
    // running - callers pass an empty version for the sideloaded bucket,
    // since it may cover several different unreleased binaries at once.
    // Called once per accrual tick for every plugin currently being fed
    // (see AppWindow::testerUsageAccrue) - fractional seconds accumulate in
    // a double and are only rounded to whole minutes when the session's
    // report is built, on the same "do not truncate every tiny delta to
    // zero" rule telemetry's SecondAccrual documents.
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
// catalogue id its install record carries. Returns kSideloadedPluginId for
// a plugin with no manifest entry (a side-loaded .dll/.so, or a manifest
// that failed to parse) - never a name made up from the display name, which
// would look like an identity the catalogue assigned when it did not (see
// kSideloadedPluginId's own comment).
std::string catalogueIdForPlugin(const std::string& fileName,
                                  const std::vector<InstalledPlugin>& installed);

}  // namespace cascade::core

#endif  // CASCADE_CORE_TESTER_USAGE_HPP
