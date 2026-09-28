// tester_link.hpp - the beta-tester PORTAL LINK: a tester presses "Link
// FoxSDR" on their foxsdr.com portal page, the browser opens
// foxsdr://beta?t=<app token>, and this app turns that into a confirmed link
// to their own tester entry - never silently, always by showing the
// tester's own NAME first and waiting for them to press Link.
//
// THIS IS NOT tester_usage.hpp's PORTAL TOKEN FLOW (the pasted 32-hex code).
// That flow still exists and still works; this file is what REPLACES it for
// anyone who uses the link instead of copy-pasting, and what MIGRATES an
// existing pasted portal token to the same app-token identity in the
// background (see exchangePortalToken below). The two credentials are
// deliberately different lengths (32 hex vs 40 hex, see
// core::validAppToken) so a value from one flow can never be mistaken for
// the other.
//
// WHY A FILE, NOT A PIPE OR SOCKET. The first design used a named pipe so an
// already-running instance could be told about a link click without a
// second full launch. Fable's review (PORTAL-LINK-VERDICT.md finding 2)
// rejected it: a machine-wide named pipe is squattable by another user on a
// shared box, guarding it needs new Win32 ACL surface with no precedent in
// this codebase, and the SAME-USER threat it would still leave open is
// already open via config.json itself. A one-shot file in the user's own
// config directory needs none of that - same-user-only by the OS's default
// ACL on Windows and by ~/.config's usual 0700 on Linux - and the file's
// full lifecycle (write, claim-by-rename, read, delete) is ordinary
// filesystem code this codebase already trusts elsewhere (ConfigStore,
// crash_upload's sidecars).
//
// WHY A MUTEX, NOT A SECOND CONFIG WRITE. The activated process cannot just
// write the token into config.json and exit: a GUI instance that is already
// running holds its OWN in-memory copy and overwrites the whole file
// wholesale on its next save (see gui/app_window.cpp's testerUsageJournal),
// silently discarding a direct write within seconds. claimPrimaryInstance()
// answers "is a GUI instance already running" cheaply and without a race
// (PORTAL-LINK-VERDICT.md finding 3): the activated process asks it FIRST,
// before touching config at all, and only writes+exits through the running
// instance's own 1 Hz poll (AppWindow::testerLinkPoll) when the answer is
// yes.
//
// THE TOKEN IS A CREDENTIAL. Exactly like tester_usage.hpp's portal token,
// it is never written to a log line - diagLogf calls near activation must
// log only that a link was received, never the URL or the token itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TESTER_LINK_HPP
#define CASCADE_CORE_TESTER_LINK_HPP

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/crash_upload.hpp"

namespace cascade::core {

// The whole URL's own length cap, well past what a real link needs (56 bytes
// for "foxsdr://beta?t=" + 40 hex) but small enough that nothing resembling
// an attack payload fits either - PORTAL-LINK-VERDICT.md finding 9.
inline constexpr std::size_t kMaxBetaLinkUrlChars = 96;

// Parses argv[1] (main.cpp) for EXACTLY foxsdr://beta?t=<40-lowercase-hex>,
// tolerating one layer of surrounding matching quotes (a shell or shortcut
// quoting artefact - Windows protocol activation can hand the whole
// "foxsdr://..." argument to cascade.exe wrapped in double quotes). Returns
// the 40-hex token on an exact match, "" for anything else: wrong scheme, a
// path segment, any extra query parameter, uppercase hex, an embedded
// control byte, or a URL over kMaxBetaLinkUrlChars. Never partially
// recovers a token from a near-miss - this function's whole job is to be
// exact, since anything it accepted would then be trusted as a real
// activation.
std::string parseBetaLinkUrl(const std::string& arg);

// "<configDir>/link-request" - the one-shot file, never logged in full.
std::string linkRequestPath(const std::string& configDir);

// The file is refused past this size, and a claimed file older than this is
// deleted UNREAD rather than surfaced as a confirmation prompt long after
// the tester forgot clicking the link.
inline constexpr std::size_t kMaxLinkRequestBytes = 4096;
inline constexpr std::int64_t kLinkRequestMaxAgeSec = 24LL * 60 * 60;

// Writes `token` (must already be a valid app token - see
// core::validAppToken) into a fresh temp file inside `configDir` and renames
// it onto linkRequestPath() atomically, so a running instance's poll never
// observes a partial write. Creates `configDir` if it does not exist. False
// (nothing written) for an invalid token or a directory that cannot be
// created.
bool writeLinkRequestFile(const std::string& configDir, const std::string& token);

// One poll, meant to be called at ~1 Hz on the GUI thread
// (AppWindow::testerLinkPoll). If linkRequestPath(configDir) exists: claims
// it by renaming to a PID-suffixed sibling first (atomic - two instances
// polling in the same instant can never both claim it, so the loser simply
// sees nothing to claim), reads at most kMaxLinkRequestBytes, deletes the
// claimed file, and returns the token if it is a valid app token. A file
// whose own write time is older than kLinkRequestMaxAgeSec is claimed and
// deleted WITHOUT being read. Returns "" when there is nothing to claim, the
// content does not parse as a valid app token, or the file was stale.
// `now`, if non-zero, is the clock this call uses instead of std::time(nullptr) -
// a test seam only.
std::string claimLinkRequestFile(const std::string& configDir, std::time_t now = 0);

// True the first time this is called for a given `identity` within this
// logon session (Windows: identity is a mutex name, created under `Local\`
// so it cannot be squatted cross-session/cross-user; Linux: identity is a
// lock file path, held with flock(LOCK_EX|LOCK_NB)). FAILS OPEN: if the
// primitive itself cannot be created (CreateMutexA returning null; open()
// refusing the lock file), this returns true - "assume primary, continue as
// a normal launch" - rather than false, because false on the beta-link
// activation path (main.cpp) means "write the request file and exit,
// trusting another instance to poll for it", and when the check itself
// failed there is no way to know one exists to do that. The handle/fd is
// deliberately leaked for the life of the process - the OS reclaims it on
// exit, the same "for the life of this process" shape as every other
// leaked-on-purpose resource in this codebase. FoxSDR's multi-instance
// design (desktop4:SupportsMultipleInstances) is unaffected: this function
// is called once, near the top of main(), purely to ANSWER "is one already
// running" - it never refuses to let a second instance start.
bool claimPrimaryInstanceAt(const std::string& identity);

// claimPrimaryInstanceAt() with the real, fixed identity this application
// uses: `Local\FoxSDR-instance` on Windows, "<configDir>/instance.lock" on
// Linux.
bool claimPrimaryInstance(const std::string& configDir);

// https://foxsdr.com, overridden by FOXSDR_BETA_API_URL - a SEPARATE seam
// from FOXSDR_TESTER_USAGE_URL (tester_usage.hpp), because a test exercising
// both the usage reporter and the link flow at once needs to point them at
// two different stub servers.
std::string betaApiBaseUrl();

// ---------------------------------------------------------------------------
// Confirm-by-name (job D) - GET .../api/beta/app-token/me
// ---------------------------------------------------------------------------

// The tester's own sign-up name, shown verbatim in an in-window prompt and on
// SYSTEM > Beta tester ("Linked to NAME") - free text from the site, so it is
// treated exactly like every other free-text label this codebase displays
// (see core::user_presets.hpp's cleanLabel): control characters and DEL
// removed (a name is one line on a prompt, never a way to inject a fake
// second line or a terminal escape), cut to kMaxTesterNameBytes without
// splitting a UTF-8 sequence, and trailing spaces the cut can leave trimmed.
// The empty result (no name entered on the site, or a name that was nothing
// but control bytes) is returned as "" - the caller decides how to say that
// distinctly rather than formatting a prompt around a blank.
inline constexpr std::size_t kMaxTesterNameBytes = 64;
std::string sanitizeTesterName(const std::string& raw);

enum class BetaLinkOutcome {
    Ok,           // 200 {"name":"..."} - safe to show the confirmation prompt
    Invalid,      // 404 - the token means nothing to the site; show an error
    NetworkError  // anything else - say nothing was verified, try again later
};

struct BetaLinkResolved {
    BetaLinkOutcome outcome = BetaLinkOutcome::NetworkError;
    std::string name;
};

// One blocking call - see BetaLinkNameSender for the off-GUI-thread wrapper
// AppWindow actually uses. Never trusts a token until the SITE has named its
// owner: this is the whole point of PORTAL-LINK-VERDICT.md finding 1 (a
// login-CSRF style attack that binds a victim's copy to an attacker's
// entry), which is why nothing calling this may skip straight to storing the
// token.
BetaLinkResolved resolveAppTokenName(const std::string& baseUrl, const std::string& appToken,
                                     const std::shared_ptr<UploadCancel>& cancel);

// Fire-and-forget wrapper, exactly TesterUsageSender's shape (detached
// worker thread, shared heap state so detaching is safe - see that class's
// header comment for the full argument). send() is a no-op while busy().
class BetaLinkNameSender {
public:
    BetaLinkNameSender() = default;
    ~BetaLinkNameSender();
    BetaLinkNameSender(const BetaLinkNameSender&) = delete;
    BetaLinkNameSender& operator=(const BetaLinkNameSender&) = delete;

    void send(const std::string& baseUrl, const std::string& appToken);
    bool busy() const;
    std::optional<BetaLinkResolved> takeResult();

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<UploadCancel> cancel_;
};

// ---------------------------------------------------------------------------
// Migration exchange (job E) - POST .../api/beta/app-token
// ---------------------------------------------------------------------------

enum class BetaMigrationOutcome {
    Ok,           // 200 {"appToken":"...","name":"..."} - store it, forget the portal token
    Invalid,      // 404/401 - the portal token is dead; clear it, do not retry
    NetworkError  // anything else - keep the portal token, retry next launch
};

struct BetaMigrationResult {
    BetaMigrationOutcome outcome = BetaMigrationOutcome::NetworkError;
    std::string appToken;
    std::string name;
};

// One blocking call, authenticated with the OLD portal token
// (`Authorization: Bearer <portalToken>`) - idempotent on the site (the same
// tester always gets the same app token back), so calling this more than
// once for the same portal token is always safe.
BetaMigrationResult exchangePortalToken(const std::string& baseUrl, const std::string& portalToken,
                                        const std::shared_ptr<UploadCancel>& cancel);

// Fire-and-forget wrapper, same shape as BetaLinkNameSender.
class BetaMigrationSender {
public:
    BetaMigrationSender() = default;
    ~BetaMigrationSender();
    BetaMigrationSender(const BetaMigrationSender&) = delete;
    BetaMigrationSender& operator=(const BetaMigrationSender&) = delete;

    void send(const std::string& baseUrl, const std::string& portalToken);
    bool busy() const;
    std::optional<BetaMigrationResult> takeResult();

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<UploadCancel> cancel_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TESTER_LINK_HPP
