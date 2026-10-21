// AppWindow-level tests for the beta-tester PORTAL LINK (core/tester_link.hpp,
// gui/app_window.cpp's testerSectionVisible/testerLinkPoll/drawTesterLinkPrompt) -
// the repair round after Opus's review of feature/tester-link found five
// defects no existing test caught, because tests/test_tester_link.cpp only
// covers the core-layer plumbing (URL parsing, the file, the network calls)
// and never constructs an AppWindow at all.
//
// Built the same way tests/test_converter_app_paths.cpp is: a real AppWindow,
// hermetic (empty config path unless a test names one), driven through a
// friend AppWindowTestAccess rather than through ImGui rendering - the
// decisions under test (is the section visible, what does the prompt say,
// what happens to the queue) are made in plain C++ before a single ImGui call,
// and that is what these tests reach.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The site stand-in below is a real loopback HTTP server. Included before
// <windows.h> (httplib brings in winsock2.h itself), and with the same
// CPPHTTPLIB_OPENSSL_SUPPORT choice as every other translation unit that
// includes it on non-Windows (see test_tester_link.cpp's note on the layout
// mismatch that follows from disagreeing).
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "core/tester_link.hpp"
#include "core/tester_usage.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

// Per-user directories pointed at a scratch folder, and every network-facing
// URL this feature (or telemetry/usage reporting) could reach pointed at a
// port nothing listens on - nothing here may reach a real endpoint, and
// nothing here may touch the user's real FoxSDR folders.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() /
               ("foxsdr_tester_link_app_" + std::to_string(pid));
    fs::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

std::string reportWithToken(const std::string& tok) {
    nlohmann::json j;
    j["token"] = tok;
    j["version"] = "1.0";
    return j.dump();
}

// THE SITE'S CONFIRM-BY-NAME ENDPOINT (GET /api/beta/app-token/me), in a form the
// test controls. It records the Authorization header of every lookup that
// reaches it, so a test can say WHICH token was looked up and HOW MANY TIMES by
// asking the site, and it either refuses at once or holds each lookup until the
// test lets it answer.
//
// WHY THE TEST NEEDS ONE. The lookup runs on a worker and AppWindow clears
// testerLinkResolvingToken_ the moment a later testerLinkPoll() collects its
// answer, so that field is true only while the lookup is out - and for how long
// it is out the product has no say: it is however long the host takes to fail a
// connection nothing answers. Pointed at a closed loopback port that was
// measured here at about two seconds on Windows (three runs, 2.02 to 2.05 s
// from the check to the lookup being collected), and on the Linux CI runner it
// was over before the test next looked - which is why a check of the
// field passed on one and failed on the other with the product behaving
// identically on both. A stand-in that either answers at once or does not answer
// until told makes both timelines happen on every platform, on purpose.
class SiteStandIn {
public:
    enum class Mode {
        FailAtOnce,  // answers 500 immediately: the lookup is over before anyone looks
        Hold,        // does not answer until release(): the lookup is out for as long as wanted
    };

    bool start(Mode mode) {
        mode_ = mode;
        server_.Get("/api/beta/app-token/me",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            bearers_.push_back(req.get_header_value("Authorization"));
                        }
                        if (mode_ == Mode::Hold) {
                            std::unique_lock<std::mutex> lk(mu_);
                            // The cap only stops a failed test leaving a thread parked.
                            cv_.wait_for(lk, std::chrono::seconds(30), [this] { return released_; });
                            res.status = 200;
                            res.set_content("{\"name\":\"Linked Tester\"}", "application/json");
                            return;
                        }
                        res.status = 500;
                        res.set_content("{}", "application/json");
                    });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { return false; }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }

    // Lets every held lookup answer 200 with a name.
    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        released_ = true;
        cv_.notify_all();
    }

    // The Authorization header of every lookup received, in arrival order.
    std::vector<std::string> bearers() {
        std::lock_guard<std::mutex> lk(mu_);
        return bearers_;
    }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

    ~SiteStandIn() {
        release();
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

private:
    Mode mode_ = Mode::FailAtOnce;
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool released_ = false;
    std::vector<std::string> bearers_;
};

// Waits, bounded, for `ready()` - for a worker or a server thread to do
// something the test cannot do for it. False on timeout, never a hang.
template <class Ready>
bool waitUntil(Ready ready, int budgetMs = 10000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= until) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for this test file (see AppWindow::testHooks_'s
// own comment in test_converter_app_paths.cpp for the general pattern).
struct AppWindowTestAccess {
    static void setAppToken(AppWindow& a, const std::string& tok, const std::string& name) {
        a.testerAppToken_ = tok;
        a.testerAppTokenName_ = name;
    }
    static void setPortalToken(AppWindow& a, const std::string& tok) { a.testerToken_ = tok; }
    static void setPortalTokenInvalid(AppWindow& a, bool v) { a.testerTokenInvalid_ = v; }
    static void setMigrationResolved(AppWindow& a, const std::string& appTok) {
        a.testerMigrationResolvedAppToken_ = appTok;
    }
    static void setLinkReveal(AppWindow& a, bool v) { a.testerLinkReveal_ = v; }
    static bool sectionVisible(AppWindow& a) { return a.testerSectionVisible(); }
    static std::string activeToken(AppWindow& a) { return a.activeTesterToken(); }

    static void queuePush(AppWindow& a, const std::string& tok) {
        a.testerUsageQueue_.push(reportWithToken(tok));
    }
    static std::size_t queueSize(AppWindow& a) { return a.testerUsageQueue_.size(); }
    static bool queueHasToken(AppWindow& a, const std::string& tok) {
        for (const std::string& item : a.testerUsageQueue_.items()) {
            if (cascade::core::tokenOfReport(item) == tok) { return true; }
        }
        return false;
    }

    // Exercises the REAL decision logic (AppWindow::computeLinkPending),
    // pulled out of testerLinkPoll's confirm-by-name-Ok branch precisely so
    // it is reachable with synthetic inputs rather than only through a real
    // network round trip.
    static AppWindow::TesterLinkPending computePending(
        const std::string& token, const std::string& name, bool appTokenHeld,
        bool portalTokenHeld, const std::optional<std::string>& migrationResolvedAppToken) {
        return AppWindow::computeLinkPending(token, name, appTokenHeld, portalTokenHeld,
                                             migrationResolvedAppToken);
    }
    static void setPending(AppWindow& a, const AppWindow::TesterLinkPending& p) {
        a.testerLinkPending_ = p;
    }
    static bool hasPending(AppWindow& a) { return a.testerLinkPending_.has_value(); }

    // Exactly what drawTesterLinkPrompt's Link button does - see that
    // function's own comment for why the rewrite (when it applies) happens
    // before setTesterAppToken's dropOthers.
    static void pressLink(AppWindow& a) {
        if (!a.testerLinkPending_) { return; }
        if (a.testerLinkPending_->replacingLegacyPortalToken &&
            a.testerLinkPending_->keepsLegacyQueue) {
            a.testerUsageQueue_.rewriteToken(a.testerLinkPending_->token);
        }
        a.setTesterAppToken(a.testerLinkPending_->token, a.testerLinkPending_->name);
        a.testerLinkPending_.reset();
    }
    static void pressNotNow(AppWindow& a) { a.testerLinkPending_.reset(); }

    // --link-tester's own effect (main.cpp) plus the file-poll path, for the
    // queuing test - forces the ~1 Hz throttle to expire so a poll actually
    // re-checks the file rather than being a same-second no-op.
    //
    // THE CLAIM IS NOT MADE INSIDE testerLinkPoll() ANY MORE. It runs on a
    // worker (gui/link_request_poll.hpp - a synchronous exists() in the config
    // directory froze the window, field report "hang ntdll.dll @
    // __std_fs_get_stats") and its answer is collected by a later call, so one
    // call is no longer "one poll's whole effect". This helper therefore keeps
    // calling - with the ~1 Hz gate left closed, so no second question is
    // asked - until the question it asked has been answered and acted on,
    // which is what every assertion below was written to observe.
    static void forcePoll(AppWindow& a) {
        a.testerLinkPollLast_ = -1.0e9;
        a.testerLinkPoll();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (a.linkRequestPoll_.inFlight() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            a.testerLinkPoll();
        }
    }
    // One frame's poll with the ~1 Hz gate left as it is: collects a lookup's
    // answer when it has come, asks the disk nothing new.
    static void pump(AppWindow& a) { a.testerLinkPoll(); }
    static std::string queuedLinkToken(AppWindow& a) { return a.testerLinkQueuedToken_; }
    // TRANSIENT: set when a lookup starts, cleared on the frame its answer is
    // collected. How long it is set is how long the lookup takes, which no test
    // controls on a real network - read it only where the lookup is held open.
    static std::string resolvingToken(AppWindow& a) { return a.testerLinkResolvingToken_; }
    static std::string pendingToken(AppWindow& a) {
        return a.testerLinkPending_ ? a.testerLinkPending_->token : std::string();
    }
    static bool hasLinkError(AppWindow& a) { return !a.testerLinkError_.empty(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

// --- testerSectionVisible() (finding 2) --------------------------------------

void testHiddenWithNoToken() {
    std::printf("  hidden with no token, no migration in flight, no --link-tester\n");
    AppWindow app;
    CHECK(!Access::sectionVisible(app));
}

void testVisibleWithAppToken() {
    std::printf("  visible with a confirmed app token\n");
    AppWindow app;
    Access::setAppToken(app, std::string(40, 'a'), "Ada");
    CHECK(Access::sectionVisible(app));
}

// THE BUG (finding 2): a tester holding only a LEGACY portal token - the
// migration exchange either never ran or NetworkError'd - used to see the
// section vanish, even though activeTesterToken() (and so usage reporting)
// was still armed under it.
void testVisibleWithLegacyPortalTokenAloneAfterFailedMigration() {
    std::printf("  visible with a legacy portal token alone, exactly as reporting still is\n");
    AppWindow app;
    Access::setPortalToken(app, std::string(32, 'p'));
    CHECK(Access::activeToken(app) == std::string(32, 'p'));  // still the reporting identity
    CHECK(Access::sectionVisible(app));
    // ...and an INVALID legacy token (the tester's own error is on screen)
    // must stay visible too, or there would be nowhere to see the error or
    // fix the code.
    Access::setPortalTokenInvalid(app, true);
    CHECK(Access::sectionVisible(app));
}

void testVisibleViaLinkTesterReveal() {
    std::printf("  --link-tester reveals the section with nothing else held\n");
    AppWindow app;
    CHECK(!Access::sectionVisible(app));
    Access::setLinkReveal(app, true);
    CHECK(Access::sectionVisible(app));
}

// --- computeLinkPending / the confirm and replace prompts (findings 5, 6) ---

void testFreshLinkIsNotAReplace() {
    std::printf("  a fresh link with nothing held is not a replace\n");
    const auto p = Access::computePending(std::string(40, 'a'), "Ada", false, false, std::nullopt);
    CHECK(!p.replacing);
    CHECK(!p.replacingLegacyPortalToken);
    CHECK(!p.keepsLegacyQueue);
}

void testReplacingAnAppTokenIsNotALegacyReplace() {
    std::printf("  replacing an existing APP token is not a legacy-token replace\n");
    const auto p =
        Access::computePending(std::string(40, 'b'), "Marie", /*appTokenHeld=*/true,
                               /*portalTokenHeld=*/false, std::nullopt);
    CHECK(p.replacing);
    CHECK(!p.replacingLegacyPortalToken);
}

// THE BUG (finding 5a): a legacy portal token alone used to compute
// replacing=false (only testerAppToken_ was checked), so the plain "Link
// this FoxSDR to beta tester NAME?" prompt showed instead of a replace
// prompt, and pressing Link silently dropped the legacy queue with no
// warning at all.
void testReplacingALegacyTokenWithUnknownRelationshipDiscardsAndSaysSo() {
    std::printf("  replacing a legacy token with no known relationship: discard, and it must "
                "say so via replacingLegacyPortalToken\n");
    const auto p = Access::computePending(std::string(40, 'c'), "Grace",
                                          /*appTokenHeld=*/false, /*portalTokenHeld=*/true,
                                          std::nullopt);
    CHECK(p.replacing);
    CHECK(p.replacingLegacyPortalToken);
    CHECK(!p.keepsLegacyQueue);  // unproven -> the safe default, discard
}

// THE FIX (finding 5b): when THIS session's own migration exchange already
// proved the legacy token resolves to the SAME app token now being
// confirmed, the queue is kept.
void testReplacingALegacyTokenProvenSameTesterKeepsTheQueue() {
    std::printf("  replacing a legacy token proven to be the same tester keeps the queue\n");
    const std::string appTok(40, 'd');
    const auto p = Access::computePending(appTok, "Rosalind", /*appTokenHeld=*/false,
                                          /*portalTokenHeld=*/true, appTok);
    CHECK(p.replacing);
    CHECK(p.replacingLegacyPortalToken);
    CHECK(p.keepsLegacyQueue);
}

void testReplacingALegacyTokenProvenDifferentTesterDiscards() {
    std::printf("  replacing a legacy token proven to be a DIFFERENT tester discards\n");
    const auto p = Access::computePending(std::string(40, 'e'), "Someone Else",
                                          /*appTokenHeld=*/false, /*portalTokenHeld=*/true,
                                          std::string(40, 'f'));  // migration found a DIFFERENT app token
    CHECK(p.replacing);
    CHECK(p.replacingLegacyPortalToken);
    CHECK(!p.keepsLegacyQueue);
}

// --- "Not now" writes nothing --------------------------------------------------

void testNotNowWritesNothing() {
    std::printf("  \"Not now\" writes nothing - no app token, no queue change\n");
    AppWindow app;
    Access::setPortalToken(app, std::string(32, 'p'));
    Access::queuePush(app, std::string(32, 'p'));
    const auto p = Access::computePending(std::string(40, 'a'), "Ada", false, true, std::nullopt);
    Access::setPending(app, p);
    Access::pressNotNow(app);
    CHECK(!Access::hasPending(app));
    CHECK(Access::activeToken(app) == std::string(32, 'p'));  // legacy token untouched
    CHECK(Access::queueSize(app) == 1);                        // queue untouched
    CHECK(Access::queueHasToken(app, std::string(32, 'p')));
}

// --- the replace prompt's queue effect, end to end via pressLink -------------

void testPressLinkOverUnknownLegacyTokenDropsTheQueue() {
    std::printf("  pressing Link over a legacy token of unknown relationship drops its queue\n");
    AppWindow app;
    Access::setPortalToken(app, std::string(32, 'p'));
    Access::queuePush(app, std::string(32, 'p'));
    const std::string newTok(40, 'g');
    const auto p = Access::computePending(newTok, "Katherine", false, true, std::nullopt);
    Access::setPending(app, p);
    Access::pressLink(app);
    CHECK(!Access::hasPending(app));
    CHECK(Access::activeToken(app) == newTok);
    CHECK(!Access::queueHasToken(app, std::string(32, 'p')));  // dropped - unknown relationship
    CHECK(Access::queueSize(app) == 0);
}

void testPressLinkOverProvenSameTesterKeepsTheQueue() {
    std::printf("  pressing Link over a legacy token PROVEN the same tester keeps its queue\n");
    AppWindow app;
    Access::setPortalToken(app, std::string(32, 'p'));
    Access::queuePush(app, std::string(32, 'p'));
    Access::queuePush(app, std::string(32, 'p'));
    const std::string newTok(40, 'h');
    Access::setMigrationResolved(app, newTok);  // this session's own migration found the same tester
    const auto p = Access::computePending(newTok, "Hedy", false, true, newTok);
    CHECK(p.keepsLegacyQueue);
    Access::setPending(app, p);
    Access::pressLink(app);
    CHECK(!Access::hasPending(app));
    CHECK(Access::activeToken(app) == newTok);
    // KEPT, not dropped - and rewritten onto the new identity, not left
    // sitting under a token that will never be sent under again.
    CHECK(Access::queueSize(app) == 2);
    CHECK(Access::queueHasToken(app, newTok));
    CHECK(!Access::queueHasToken(app, std::string(32, 'p')));
}

// --- the one-shot link-request file: queued, not dropped, while a decision --
// is pending (finding 7) --------------------------------------------------------

// Run once with a lookup that is still out when the test looks and once with a
// lookup that is already over, because which of the two a host produces is not
// the product's doing and the scenario must hold in both.
void testALinkArrivingWhilePendingIsQueuedNotDropped(SiteStandIn::Mode mode) {
    const bool held = (mode == SiteStandIn::Mode::Hold);
    std::printf("  a link arriving while a decision is pending is queued, not dropped - and "
                "the LATEST one wins (%s)\n",
                held ? "lookup still out when looked at" : "lookup already over when looked at");

    SiteStandIn site;
    CHECK(site.start(mode));
    setEnv("FOXSDR_BETA_API_URL", site.baseUrl());  // read when the lookup starts

    const fs::path dir = g_scratch / (held ? "linkqueue-held" : "linkqueue-failed");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path cfgPath = dir / "config.json";
    AppWindow app(cfgPath.string());  // non-hermetic: testerLinkPoll's file check runs

    // A decision is already pending (the tester has not yet pressed
    // Link/Not now on an earlier resolve).
    Access::setPending(app, Access::computePending(std::string(40, '0'), "Rita", false, false,
                                                    std::nullopt));

    const std::string configDir = dir.string();
    const std::string tokA = std::string(40, '1');
    const std::string tokB = std::string(40, '2');
    CHECK(cascade::core::writeLinkRequestFile(configDir, tokA));
    Access::forcePoll(app);
    // Claimed (the file is gone either way), but held rather than started -
    // a prompt is already up, so the OLD behaviour (silently drop) would
    // leave nothing anywhere.
    CHECK(!fs::exists(cascade::core::linkRequestPath(configDir)));
    CHECK(Access::queuedLinkToken(app) == tokA);
    CHECK(Access::resolvingToken(app).empty());  // not yet started - still gated

    // A second link click arrives before the first is ever acted on -
    // LATEST WINS.
    CHECK(cascade::core::writeLinkRequestFile(configDir, tokB));
    Access::forcePoll(app);
    CHECK(Access::queuedLinkToken(app) == tokB);
    // While the prompt is up NOTHING has been looked up: neither token has
    // reached the site.
    CHECK(site.bearers().empty());

    // The tester finally answers the earlier prompt - the gate opens, and
    // the QUEUED (latest) token is what gets started, never the discarded
    // first one.
    Access::pressNotNow(app);
    Access::forcePoll(app);
    CHECK(Access::queuedLinkToken(app).empty());

    // WHAT WAS STARTED, read where it cannot depend on timing: at the site. The
    // lookup is made on a worker, so wait (bounded) for it to arrive. It must be
    // the LATEST token, and the only one - the discarded first token never
    // reaches the site.
    const std::vector<std::string> onlyB = {"Bearer " + tokB};
    CHECK(waitUntil([&] { return !site.bearers().empty(); }));
    CHECK(site.bearers() == onlyB);

    if (held) {
        // The lookup cannot finish until released, so the field that names what
        // is being resolved is set for the whole of this block - by construction,
        // not by how slow a connection is.
        CHECK(Access::resolvingToken(app) == tokB);
        site.release();
    }
    // However long it took, let the lookup be collected.
    CHECK(waitUntil([&] {
        Access::pump(app);
        return Access::resolvingToken(app).empty();
    }));
    if (held) {
        // Answered: the prompt that comes up is for the latest token's owner.
        CHECK(Access::pendingToken(app) == tokB);
        CHECK(!Access::hasLinkError(app));
    } else {
        // Refused: nothing is put in front of the tester as if it were a link to
        // confirm, and the failure is shown rather than lost.
        CHECK(!Access::hasPending(app));
        CHECK(Access::hasLinkError(app));
    }
    // Started exactly once. Frames keep running; nothing re-queues or re-starts
    // the token, and the discarded one never turns up.
    for (int i = 0; i < 50; ++i) {
        Access::pump(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(Access::queuedLinkToken(app).empty());
    CHECK(site.bearers() == onlyB);

    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");  // what isolate() set
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    std::printf("test_tester_link_app\n");
    isolate();

    testHiddenWithNoToken();
    testVisibleWithAppToken();
    testVisibleWithLegacyPortalTokenAloneAfterFailedMigration();
    testVisibleViaLinkTesterReveal();

    testFreshLinkIsNotAReplace();
    testReplacingAnAppTokenIsNotALegacyReplace();
    testReplacingALegacyTokenWithUnknownRelationshipDiscardsAndSaysSo();
    testReplacingALegacyTokenProvenSameTesterKeepsTheQueue();
    testReplacingALegacyTokenProvenDifferentTesterDiscards();

    testNotNowWritesNothing();
    testPressLinkOverUnknownLegacyTokenDropsTheQueue();
    testPressLinkOverProvenSameTesterKeepsTheQueue();

    testALinkArrivingWhilePendingIsQueuedNotDropped(SiteStandIn::Mode::Hold);
    testALinkArrivingWhilePendingIsQueuedNotDropped(SiteStandIn::Mode::FailAtOnce);

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_tester_link_app");
}
