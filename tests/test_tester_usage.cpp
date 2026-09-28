// Tests for core/tester_usage.hpp.
//
// Same discipline test_telemetry.cpp uses, applied to a second, independent
// transmission: the payload is asserted field-by-field, "no code means
// nothing collected" is proved rather than assumed, and the real transport is
// driven against a local server on BOTH platforms - postTesterUsage sits on
// top of core/crash_upload.hpp's shared client, which allows plain http on
// loopback for exactly this reason, so no certificate is needed here at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/tester_usage.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
// Must match every other TU that includes httplib.h on non-Windows (see
// test_telemetry.cpp's own comment on this exact requirement).
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

using namespace cascade::core;
namespace fs = std::filesystem;

namespace {

// --- Token validation and extraction ----------------------------------------

void testValidTesterToken() {
    CHECK(validTesterToken(std::string(32, 'a')));
    CHECK(validTesterToken("0123456789abcdef0123456789abcdef"));
    CHECK(!validTesterToken(""));
    CHECK(!validTesterToken("short"));
    CHECK(!validTesterToken(std::string(33, 'a')));
    CHECK(!validTesterToken(std::string(32, 'Z')));  // uppercase is not hex here
    CHECK(!validTesterToken("steve@example.com"));
}

void testExtractBareToken() {
    const std::string tok(32, 'a');
    CHECK(extractTesterToken(tok) == tok);
    // Whitespace at either end, from a careless paste.
    CHECK(extractTesterToken("  " + tok + "\n") == tok);
    CHECK(extractTesterToken("").empty());
    CHECK(extractTesterToken("not a token").empty());
}

// BOTH LINK FORMATS THE SITE USES: the full https:// portal link
// (web/portal.js's manageLink, "/#t=" + token) and the bare-origin form
// without the scheme, which is what a browser's address bar shows once the
// page has loaded and is exactly as likely to be copied by hand.
void testExtractFromPortalLink() {
    const std::string tok = "0123456789abcdef0123456789abcdef";
    CHECK(extractTesterToken("https://foxsdr.com/#t=" + tok) == tok);
    CHECK(extractTesterToken("https://foxsdr.com/#t=" + tok + "\n") == tok);
    CHECK(extractTesterToken("foxsdr.com/#t=" + tok) == tok);
    // A trailing fragment key after the token, exactly as URLSearchParams
    // would be asked to split it.
    CHECK(extractTesterToken("https://foxsdr.com/#t=" + tok + "&x=1") == tok);

    // THE CONFIRMATION LINK IS NOT A CREDENTIAL THIS FIELD ACCEPTS - beta.go
    // keeps ConfirmToken and Token in separate indexes for exactly this
    // reason, and this field must not blur that boundary back together.
    CHECK(extractTesterToken("https://foxsdr.com/#c=" + tok).empty());
    CHECK(extractTesterToken("https://foxsdr.com/#beta").empty());
    CHECK(extractTesterToken("https://foxsdr.com/").empty());
}

// --- Masking (B1: the preview and the input field must never show it whole) -

void testMaskTesterToken() {
    const std::string tok = "0123456789abcdef0123456789abcdef";
    const std::string masked = maskTesterToken(tok);
    CHECK(masked == "0123...cdef");
    CHECK(masked.find(tok) == std::string::npos);
    // A short (never-real, see validTesterToken) value is masked in full,
    // not partially - it must never show more of itself than a real token
    // would.
    CHECK(maskTesterToken("abcd") == "****");
    CHECK(maskTesterToken("").empty());
}

void testMaskedPreviewJsonNeverContainsTheRealToken() {
    TesterUsageReport r;
    r.token = "0123456789abcdef0123456789abcdef";
    r.version = "0.99.42";
    r.platform = "windows";
    r.arch = "x64";
    r.session.start = "2026-09-28T12:34:56Z";
    r.session.minutes = 5;
    r.session.features = {"spectrum"};

    const std::string real = r.toJson();
    CHECK(real.find(r.token) != std::string::npos);  // the real payload DOES carry it whole

    const std::string preview = maskedPreviewJson(r);
    CHECK(preview.find(r.token) == std::string::npos);
    CHECK(preview.find(maskTesterToken(r.token)) != std::string::npos);
    // Every OTHER field still reads exactly as the real payload would -
    // masking must not silently drop or alter anything else.
    const nlohmann::json pj = nlohmann::json::parse(preview);
    CHECK(pj["version"] == "0.99.42");
    CHECK(pj["session"]["minutes"] == 5);
    CHECK(pj["features"][0] == "spectrum");
}

// --- The field inventory -----------------------------------------------------

void testPayloadContainsExactlyTheContractFields() {
    TesterUsageReport r;
    r.token = std::string(32, 'a');
    r.version = "0.99.42";
    r.platform = testerUsagePlatform();
    r.arch = "x64";
    r.session.start = "2026-09-28T12:34:56Z";
    r.session.minutes = 42;
    r.session.features = {"spectrum", "modes", "bookmarks"};
    r.session.plugins = {{"pocsag", "1.2.0", 6}};
    r.session.radios = {"rtlsdr"};

    const nlohmann::json j = nlohmann::json::parse(r.toJson());
    const std::set<std::string> allowed = {"token", "version", "platform", "arch",
                                            "session", "features", "plugins", "radios"};
    std::set<std::string> actual;
    for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
    CHECK(actual == allowed);

    const std::set<std::string> sessionAllowed = {"start", "minutes"};
    std::set<std::string> sessionActual;
    for (auto it = j["session"].begin(); it != j["session"].end(); ++it) {
        sessionActual.insert(it.key());
    }
    CHECK(sessionActual == sessionAllowed);

    CHECK(j["token"] == r.token);
    CHECK(j["version"] == "0.99.42");
    CHECK(j["session"]["minutes"] == 42);
    CHECK(j["session"]["start"] == "2026-09-28T12:34:56Z");
    CHECK(j["features"].size() == 3);
    CHECK(j["plugins"].size() == 1);
    CHECK(j["plugins"][0]["id"] == "pocsag");
    CHECK(j["plugins"][0]["version"] == "1.2.0");
    CHECK(j["plugins"][0]["minutes"] == 6);
    CHECK(j["radios"][0] == "rtlsdr");
}

void testPlatformIsOneOfTheThreeNames() {
    const std::string p = testerUsagePlatform();
    CHECK(p == "windows" || p == "linux" || p == "android");
}

// --- No code, nothing collected ----------------------------------------------

void testDisarmedRecorderCollectsNothing() {
    TesterUsageRecorder rec;
    CHECK(!rec.armed());
    rec.noteFeature("spectrum");
    rec.noteRadio("rtlsdr");
    rec.accruePlugin("pocsag", "1.2.0", 600.0);
    CHECK(rec.features().empty());
    CHECK(rec.radios().empty());
    CHECK(rec.plugins().empty());

    // Arming AFTER the fact does not retroactively collect what was ignored
    // while disarmed - a session that never opted in leaves nothing behind
    // for a later opt-in to inherit.
    rec.setArmed(true);
    CHECK(rec.features().empty());
    rec.noteFeature("spectrum");
    CHECK(rec.features() == std::vector<std::string>({"spectrum"}));
}

void testArmedRecorderDeduplicatesAndAccrues() {
    TesterUsageRecorder rec;
    rec.setArmed(true);
    rec.noteFeature("spectrum");
    rec.noteFeature("modes");
    rec.noteFeature("spectrum");  // duplicate - order-preserving, no repeat
    CHECK(rec.features() == std::vector<std::string>({"spectrum", "modes"}));

    rec.noteRadio("rtlsdr");
    rec.noteRadio("rtlsdr");
    rec.noteRadio("uhd");
    CHECK(rec.radios() == std::vector<std::string>({"rtlsdr", "uhd"}));

    // Fractional seconds accumulate rather than truncating to zero each call -
    // the same rule telemetry's SecondAccrual documents for mode-seconds.
    rec.accruePlugin("pocsag", "1.2.0", 30.0);
    rec.accruePlugin("pocsag", "1.2.0", 30.0);
    rec.accruePlugin("pocsag", "1.3.0", 0.0);  // a no-op zero delta
    const std::vector<TesterUsagePlugin> plugins = rec.plugins();
    CHECK(plugins.size() == 1);
    CHECK(plugins[0].id == "pocsag");
    CHECK(plugins[0].version == "1.2.0");
    CHECK(plugins[0].minutes == 1);  // 60s = 1 minute, rounded

    rec.reset();
    CHECK(rec.features().empty());
    CHECK(rec.radios().empty());
    CHECK(rec.plugins().empty());
}

// --- M2: side-loaded plugins get one shared bucket, never an invented id ----

void testCatalogueIdJoinsOnFileNameAndFallsBackToSideloaded() {
    std::vector<InstalledPlugin> installed;
    InstalledPlugin ip;
    ip.id = "pocsag";
    ip.name = "POCSAG";
    ip.file = "pocsag-decoder.dll";
    installed.push_back(ip);

    // Built with fs::path, not a hand-typed Windows path: a literal
    // "C:\\plugins\\..." backslash path is not portable - POSIX filesystem
    // implementations do not treat '\' as a separator at all, so .filename()
    // on it returns the whole string unchanged and the join below it is
    // testing would silently fail on Linux/WSL.
    const std::string knownPath = (fs::path("plugins") / "pocsag-decoder.dll").string();
    const std::string unknownPath = (fs::path("plugins") / "mystery.dll").string();
    CHECK(catalogueIdForPlugin(knownPath, installed) == "pocsag");
    // A side-loaded plugin with no manifest entry is reported under the fixed
    // shared bucket, never a name invented from its display name.
    CHECK(catalogueIdForPlugin(unknownPath, installed) == kSideloadedPluginId);
    CHECK(std::string(kSideloadedPluginId) == "sideloaded");
}

// --- The RFC3339 stamp --------------------------------------------------------

void testRfc3339FormatIsFixedWidthAndUtc() {
    const std::string s = rfc3339Utc(static_cast<std::time_t>(1780000000));
    CHECK(s.size() == 20);  // "YYYY-MM-DDTHH:MM:SSZ"
    CHECK(s[4] == '-' && s[7] == '-' && s[10] == 'T' && s[13] == ':' && s[16] == ':');
    CHECK(s.back() == 'Z');
}

// --- The bounded retry queue --------------------------------------------------

void testQueueBoundedToThreeDroppingTheOldest() {
    TesterUsageQueue q;
    q.push("one");
    q.push("two");
    q.push("three");
    q.push("four");
    CHECK(q.size() == 3);
    CHECK(q.items() == std::vector<std::string>({"two", "three", "four"}));

    q.removeFront();
    CHECK(q.items() == std::vector<std::string>({"three", "four"}));

    q.setItems({"a", "b", "c", "d", "e"});
    CHECK(q.items() == std::vector<std::string>({"c", "d", "e"}));

    TesterUsageQueue empty;
    CHECK(empty.empty());
}

std::string reportWithToken(const std::string& token) {
    TesterUsageReport r;
    r.token = token;
    r.version = "0.99.42";
    return r.toJson();
}

void testTokenOfReport() {
    CHECK(tokenOfReport(reportWithToken("abc123")) == "abc123");
    CHECK(tokenOfReport("not json").empty());
    CHECK(tokenOfReport("{}").empty());
    CHECK(tokenOfReport(R"({"token": 5})").empty());  // wrong type, not a string
}

// --- M1: replacing a revoked or mistyped code must not carry the old ---------
// session's report forward under the new one (or none) ------------------------

void testDropOthersRemovesReportsForADifferentToken() {
    const std::string tokA(32, 'a');
    const std::string tokB(32, 'b');
    TesterUsageQueue q;
    q.push(reportWithToken(tokA));
    q.push(reportWithToken(tokA));
    q.push(reportWithToken(tokB));
    CHECK(q.size() == 3);

    // THE REPLACE-AFTER-401 FLOW: the code that queued these reports turns
    // out to be revoked (401), the tester pastes a NEW one - the queue must
    // not hand the new code someone else's (or its own old, now-orphaned)
    // reports.
    q.dropOthers(tokB);
    CHECK(q.size() == 1);
    CHECK(tokenOfReport(q.front()) == tokB);

    // And removing the code altogether (token == "") must drop everything -
    // nothing queued under a real token ever carries an empty one.
    q.dropOthers("");
    CHECK(q.empty());

    // An unparsable entry cannot be shown to carry the right token, so it is
    // dropped like a mismatch rather than kept like a match.
    TesterUsageQueue q2;
    q2.push("garbage, not json");
    q2.push(reportWithToken(tokA));
    q2.dropOthers(tokA);
    CHECK(q2.size() == 1);
    CHECK(tokenOfReport(q2.front()) == tokA);
}

// --- M4: the AppWindow-level outcome decision, as a pure, testable seam -----

void testApplyOutcomeSentDropsTheFront() {
    TesterUsageQueue q;
    q.push("one");
    q.push("two");
    const TesterUsageOutcomeEffect e = applyTesterUsageOutcome(TesterUsageOutcome::Sent, q);
    CHECK(!e.nowInvalid);
    CHECK(!e.stop);
    CHECK(q.items() == std::vector<std::string>({"two"}));
}

void testApplyOutcomeRejectedDropsTheFrontToo() {
    TesterUsageQueue q;
    q.push("bad-report");
    const TesterUsageOutcomeEffect e = applyTesterUsageOutcome(TesterUsageOutcome::Rejected, q);
    CHECK(!e.nowInvalid);
    CHECK(!e.stop);
    CHECK(q.empty());
}

void testApplyOutcomeInvalidStopsAndMarksInvalidWithoutDropping() {
    TesterUsageQueue q;
    q.push("keep-me");
    const TesterUsageOutcomeEffect e = applyTesterUsageOutcome(TesterUsageOutcome::Invalid, q);
    CHECK(e.nowInvalid);
    CHECK(e.stop);
    // The code being invalid says nothing about the QUEUED report - it is
    // kept so a later, fixed code can still send it.
    CHECK(q.items() == std::vector<std::string>({"keep-me"}));
}

void testApplyOutcomeRetryStopsWithoutMarkingInvalidOrDropping() {
    TesterUsageQueue q;
    q.push("keep-me");
    const TesterUsageOutcomeEffect e = applyTesterUsageOutcome(TesterUsageOutcome::Retry, q);
    CHECK(!e.nowInvalid);
    CHECK(e.stop);
    CHECK(q.items() == std::vector<std::string>({"keep-me"}));
}

// --- The endpoint seam --------------------------------------------------------

void testEndpointOverride() {
#if defined(_WIN32)
    ::SetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", nullptr);
#else
    ::unsetenv("FOXSDR_TESTER_USAGE_URL");
#endif
    CHECK(testerUsageEndpoint() == "https://foxsdr.com/api/tester-usage");
#if defined(_WIN32)
    ::SetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9/");
    CHECK(testerUsageEndpoint() == "http://127.0.0.1:9/");
    ::SetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", nullptr);
#else
    ::setenv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9/", 1);
    CHECK(testerUsageEndpoint() == "http://127.0.0.1:9/");
    ::unsetenv("FOXSDR_TESTER_USAGE_URL");
#endif
    CHECK(testerUsageEndpoint() == "https://foxsdr.com/api/tester-usage");
}

// A plain http override to a NON-loopback host never opens a socket - https
// is required everywhere except loopback (core::postBounded's own rule,
// shared with the crash uploader), and 127.0.0.1:9 with nothing listening is
// as close to "definitely nobody real" as a deterministic test gets; the
// point here is the scheme/host gate, not the specific refusal.
void testNonLoopbackHttpUrlIsRefused() {
    const auto cancel = std::make_shared<UploadCancel>();
    const auto t0 = std::chrono::steady_clock::now();
    const TesterUsageOutcome o =
        postTesterUsage("http://example.invalid/api/tester-usage", "{\"probe\":1}", cancel);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("tester usage non-loopback http refusal: %.0f ms\n", ms);
    CHECK(ms < 500.0);
    CHECK(o == TesterUsageOutcome::Retry);
}

// --- TesterUsageSender: detached, never joined (see its own header) ---------

void testSenderRunsOnceAndReportsViaTakeOutcome() {
    TesterUsageSender s;
    CHECK(!s.busy());
    s.send("http://example.invalid/", "{\"probe\":1}");
    CHECK(s.busy());
    // Bounded wait for the detached worker - the scheme/host refusal is
    // effectively instant, so this never approaches a real timeout.
    std::optional<TesterUsageOutcome> got;
    for (int i = 0; i < 500 && !got.has_value(); ++i) {
        got = s.takeOutcome();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got.has_value());
    if (got.has_value()) { CHECK(*got == TesterUsageOutcome::Retry); }
    CHECK(!s.busy());
    // A second call finds nothing new.
    CHECK(!s.takeOutcome().has_value());

    // Empty url/json is a no-op.
    TesterUsageSender s2;
    s2.send("", "{}");
    CHECK(!s2.busy());

    // busy() refuses a second send while one is in flight.
    TesterUsageSender s3;
    s3.send("http://example.invalid/", "{\"a\":1}");
    CHECK(s3.busy());
    s3.send("http://example.invalid/", "{\"b\":2}");  // ignored - still sending the first
    for (int i = 0; i < 500 && s3.busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!s3.busy());
}

// Destroying a sender with a send in flight must not block the calling
// thread - this is the whole point of detaching rather than joining (see
// TesterUsageSender's header). A non-loopback host that nothing answers
// would otherwise hold the destructor for the transport's own connect
// timeout (several seconds); here it returns essentially immediately.
void testDestroyingABusySenderNeverBlocks() {
    const auto t0 = std::chrono::steady_clock::now();
    {
        TesterUsageSender s;
        s.send("http://198.51.100.1:1/", "{\"probe\":1}");  // TEST-NET-2, nothing there
        CHECK(s.busy());
    }  // destructor runs here
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("tester usage sender destruction while busy: %.0f ms\n", ms);
    CHECK(ms < 500.0);
}

// --- Nothing here logs the token ---------------------------------------------
//
// A privacy promise in PRIVACY.md is worth exactly as much as the test that
// holds it - see test_telemetry.cpp's own header for the same argument
// applied to the install id. This file's own source is scanned rather than
// exercising the GUI (which owns the only diagLogf/diagWarnf call sites that
// mention a tester-usage OUTCOME): no log-emitting call anywhere near this
// feature may pass the token or the raw paste buffer as an argument.
void testTokenNeverAppearsNearALogCall(const std::string& givenRoot) {
    fs::path repoRoot;
    if (!givenRoot.empty()) {
        repoRoot = givenRoot;
    } else {
        // Run by hand with no argument: walk up from the working directory,
        // the same fallback test_i18n.cpp and test_band_plan use.
        fs::path dir = fs::current_path();
        std::error_code ec;
        for (int level = 0; !ec && level < 10; ++level) {
            if (fs::is_regular_file(dir / "PRIVACY.md", ec) &&
                fs::is_directory(dir / "src", ec)) {
                repoRoot = dir;
                break;
            }
            if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
            dir = dir.parent_path();
        }
    }
    CHECK(!repoRoot.empty());
    if (repoRoot.empty()) { return; }

    const auto readFile = [](const fs::path& p) -> std::string {
        std::ifstream f(p, std::ios::binary);
        if (!f) { return {}; }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };

    {
        // core/tester_usage.cpp itself: the transport is silent by design
        // (see its header), so it must not contain ANY
        // diagLogf/diagWarnf/printf call at all.
        const std::string src = readFile(repoRoot / "src" / "core" / "tester_usage.cpp");
        CHECK(!src.empty());
        CHECK(src.find("diagLogf") == std::string::npos);
        CHECK(src.find("diagWarnf") == std::string::npos);
        CHECK(src.find("std::printf") == std::string::npos);
        CHECK(src.find("fprintf") == std::string::npos);
    }
    {
        // app_window.cpp is huge and logs plenty about everything else; the
        // narrower, meaningful check is that no LINE calling a log function
        // also mentions the token field or the raw input buffer.
        const std::string src = readFile(repoRoot / "src" / "gui" / "app_window.cpp");
        CHECK(!src.empty());
        std::istringstream lines(src);
        std::string line;
        bool clean = true;
        int lineNo = 0;
        while (std::getline(lines, line)) {
            ++lineNo;
            const bool logs = line.find("diagLogf(") != std::string::npos ||
                              line.find("diagWarnf(") != std::string::npos;
            if (!logs) { continue; }
            const bool leaks = line.find("testerToken_") != std::string::npos ||
                               line.find("testerCodeBuf_") != std::string::npos ||
                               line.find(".token") != std::string::npos;
            if (leaks) {
                clean = false;
                std::printf("FAIL app_window.cpp:%d logs the tester token: %s\n", lineNo,
                            line.c_str());
            }
        }
        CHECK(clean);
    }
}

// config.json IS the right place for the token (it is local, never
// transmitted by the act of saving) - stated as a test so the boundary
// between "local storage" and "never sent" is explicit rather than assumed.
void testTokenIsStoredInConfigVerbatim() {
    AppConfig cfg;
    cfg.testerToken = std::string(32, 'b');
    const std::string json = ConfigStore::serialize(cfg);
    CHECK(json.find(cfg.testerToken) != std::string::npos);

    AppConfig loaded;
    std::string error;
    const fs::path tmp = fs::temp_directory_path() /
                         ("cascade-tester-usage-config-test-" +
                          std::to_string(static_cast<long>(
#if defined(_WIN32)
                              ::GetCurrentProcessId()
#else
                              ::getpid()
#endif
                              )) +
                          ".json");
    std::string werr;
    CHECK(ConfigStore::writeFile(tmp.string(), json, werr));
    CHECK(ConfigStore::load(tmp.string(), loaded, error));
    CHECK(loaded.testerToken == cfg.testerToken);
    std::error_code ec;
    fs::remove(tmp, ec);

    // A hand-edited value that is not exactly the site's shape is refused,
    // exactly like telemetryInstallId - a name or an email must never reach
    // the wire.
    AppConfig bad;
    bad.testerToken = "not-a-real-token";
    const std::string badJson = ConfigStore::serialize(bad);
    CHECK(ConfigStore::writeFile(tmp.string(), badJson, werr));
    AppConfig loadedBad;
    CHECK(ConfigStore::load(tmp.string(), loadedBad, error));
    CHECK(loadedBad.testerToken.empty());
    fs::remove(tmp, ec);
}

// M1, at the config layer: a pending queue written under one token is
// dropped on load once the token on file has changed (a hand-edit, or a
// save that landed mid-replace), and an empty token never keeps a queue at
// all.
void testConfigDropsPendingReportsForAnotherTokenOnLoad() {
    const std::string tokA(32, 'a');
    const std::string tokB(32, 'b');
    AppConfig cfg;
    cfg.testerToken = tokB;
    cfg.testerUsagePending = {reportWithToken(tokA), reportWithToken(tokB)};
    const std::string json = ConfigStore::serialize(cfg);

    const fs::path tmp = fs::temp_directory_path() /
                         ("cascade-tester-usage-queue-token-test-" +
                          std::to_string(static_cast<long>(
#if defined(_WIN32)
                              ::GetCurrentProcessId()
#else
                              ::getpid()
#endif
                              )) +
                          ".json");
    std::string werr;
    CHECK(ConfigStore::writeFile(tmp.string(), json, werr));
    AppConfig loaded;
    std::string error;
    CHECK(ConfigStore::load(tmp.string(), loaded, error));
    CHECK(loaded.testerUsagePending.size() == 1);
    if (loaded.testerUsagePending.size() == 1) {
        CHECK(tokenOfReport(loaded.testerUsagePending[0]) == tokB);
    }

    // And with no token at all, the queue is empty regardless of what the
    // file said.
    AppConfig cfg2;
    cfg2.testerToken.clear();
    cfg2.testerUsagePending = {reportWithToken(tokA)};
    const std::string json2 = ConfigStore::serialize(cfg2);
    CHECK(ConfigStore::writeFile(tmp.string(), json2, werr));
    AppConfig loaded2;
    CHECK(ConfigStore::load(tmp.string(), loaded2, error));
    CHECK(loaded2.testerUsagePending.empty());

    std::error_code ec;
    fs::remove(tmp, ec);
}

// ---------------------------------------------------------------------------
// The real transport against a local server, and the shutdown-timing
// measurement B2/M3 asked for - Windows side (raw Winsock stub, mirroring
// test_crash_upload.cpp's StubServer) and POSIX side (httplib::Server) below.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// A local HTTP stub covering every response code the contract names, plus a
// server that never answers at all - the same shape as
// test_crash_upload.cpp's StubServer, independent of it so a change to one
// transport's fixture cannot silently break the other's coverage.
class StubServer {
public:
    enum class Mode { Ok200, Unauthorized401, TooLarge413, RateLimit429, Hang, Refuse };

    bool start(Mode mode) {
        mode_ = mode;
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        if (mode_ == Mode::Refuse) {
            // Nothing listens - as close to "the server is down" as a test
            // can get deterministically (see test_crash_upload.cpp's own
            // StubServer for the same trick).
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return true;
        }
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
        for (SOCKET s : held_) { ::closesocket(s); }
        held_.clear();
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/api/tester-usage";
    }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }

private:
    void loop() {
        while (run_) {
            fd_set rd;
            FD_ZERO(&rd);
            if (listen_ == INVALID_SOCKET) { break; }
            FD_SET(listen_, &rd);
            timeval tv{0, 100 * 1000};
            const int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n <= 0) { continue; }
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { continue; }
            std::string req;
            char buf[4096];
            std::size_t contentLength = 0;
            std::size_t headerEnd = std::string::npos;
            while (run_) {
                const int got = ::recv(c, buf, sizeof(buf), 0);
                if (got <= 0) { break; }
                req.append(buf, buf + got);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::size_t at = lowerFind(req, "content-length:");
                        if (at != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            if (headerEnd != std::string::npos) {
                std::lock_guard<std::mutex> lk(mu_);
                bodies_.push_back(req.substr(headerEnd + 4));
            }
            if (mode_ == Mode::Hang) {
                // Accepted, read, and DELIBERATELY never answered - the
                // client must not sit here, and the shutdown-timing test
                // below is what proves the application does not either.
                held_.push_back(c);
                continue;
            }
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            if (mode_ == Mode::Unauthorized401) {
                resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
            } else if (mode_ == Mode::TooLarge413) {
                resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n";
            } else if (mode_ == Mode::RateLimit429) {
                resp = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n";
            }
            ::send(c, resp, static_cast<int>(std::strlen(resp)), 0);
            ::shutdown(c, SD_BOTH);
            ::closesocket(c);
        }
    }

    static std::size_t lowerFind(const std::string& hay, const std::string& needle) {
        std::string l;
        l.reserve(hay.size());
        for (char ch : hay) {
            l.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return l.find(needle);
    }

    Mode mode_ = Mode::Ok200;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<SOCKET> held_;
};

TesterUsageOutcome sendAndWait(const std::string& url, const std::string& json) {
    TesterUsageSender s;
    s.send(url, json);
    std::optional<TesterUsageOutcome> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeOutcome();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    return got.value_or(TesterUsageOutcome::Retry);
}

void testRealServerReceivesEveryResponseCodeWindows() {
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Ok200));
        const TesterUsageOutcome o = sendAndWait(srv.url(), "{\"token\":\"a\"}");
        CHECK(o == TesterUsageOutcome::Sent);
        const std::vector<std::string> bodies = srv.bodies();
        CHECK(bodies.size() == 1);
        if (!bodies.empty()) { CHECK(bodies[0] == "{\"token\":\"a\"}"); }
    }
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Unauthorized401));
        CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Invalid);
    }
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::TooLarge413));
        CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Rejected);
    }
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::RateLimit429));
        CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Retry);
    }
    {
        StubServer srv;
        CHECK(srv.start(StubServer::Mode::Refuse));
        CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Retry);
    }
}

// --- B2/M3: shutdown must never wait on the network -------------------------
//
// Runs the SHIPPED binary with a tester code set against a server that
// accepts the connection and never answers, and compares its wall time to
// the same run with no code at all - this is the only way to see the
// shutdown path, since every other test here calls the transport directly.
// An earlier version of this feature tried a bounded ~0.9s attempt to send
// at exit and measured real, added shutdown time against exactly this kind
// of server; that attempt was removed (see TesterUsageSender's header), and
// this test is what proves it stays removed.
double runAppMs(const fs::path& cfgPath, const std::string& usageUrl, int frames) {
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
    ::SetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", usageUrl.c_str());
    ::SetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9/");
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", "http://127.0.0.1:9/");
    ::SetEnvironmentVariableA("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/");
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" --frames " + std::to_string(frames) + " 2>&1\"";
    const auto t0 = std::chrono::steady_clock::now();
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { /* drained */ }
    if (p != nullptr) { _pclose(p); }
    const auto t1 = std::chrono::steady_clock::now();
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_TESTER_USAGE_URL", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);
    ::SetEnvironmentVariableA("FOXSDR_UPDATE_URL", nullptr);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void writeConfigWithToken(const fs::path& p, const std::string& token) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"telemetryEnabled\": false,\n";
    out << "  \"updateCheckEnabled\": false,\n";
    out << "  \"testerToken\": \"" << token << "\"\n";
    out << "}\n";
}

void testShutdownNeverWaitsOnTheNetwork() {
    StubServer hang;
    CHECK(hang.start(StubServer::Mode::Hang));

    const fs::path dir =
        fs::temp_directory_path() /
        ("cascade-tester-usage-shutdown-" + std::to_string(static_cast<long>(::GetCurrentProcessId())));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path cfgNone = dir / "none.json";
    const fs::path cfgToken = dir / "token.json";
    writeConfigWithToken(cfgNone, "");
    writeConfigWithToken(cfgToken, std::string(32, 'a'));

    // A first, untimed run of each to absorb any one-off cold-start cost
    // (module loads, first-touch page faults) so the comparison is between
    // warm runs.
    runAppMs(cfgNone, hang.url(), 20);
    runAppMs(cfgToken, hang.url(), 20);

    // THE MINIMUM OF SEVERAL RUNS, not one - the same reasoning the mayhem-b200
    // flake postmortem settled on: system noise (a scheduler hiccup, another
    // process, a disk stall) only ever ADDS time, so the minimum across
    // several runs is the closest thing to "how long this actually takes"
    // that a wall-clock measurement on a shared machine can give. A single
    // sample here once differed by ~550ms on an otherwise idle box purely
    // from run-to-run noise; the minimum did not.
    auto minOf = [&](const fs::path& cfg, int n) {
        double best = -1.0;
        for (int i = 0; i < n; ++i) {
            const double ms = runAppMs(cfg, hang.url(), 90);
            if (best < 0.0 || ms < best) { best = ms; }
        }
        return best;
    };
    const double msNone = minOf(cfgNone, 5);
    const double msToken = minOf(cfgToken, 5);
    std::printf(
        "tester usage shutdown timing (min of 5): no-code=%.0f ms, code-set=%.0f ms (server "
        "hangs)\n",
        msNone, msToken);
    // "Within ~0.5s of the no-code run" - the orchestrator's own bound.
    CHECK(std::fabs(msToken - msNone) < 500.0);

    fs::remove_all(dir, ec);
}

#else  // !_WIN32

// The POSIX mirror: a local httplib::Server serving plain http, which
// postBounded's loopback exception allows without a certificate - see the
// file header.
struct RealServer {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    int nextStatus = 200;
    std::string lastBody;
    std::mutex mu;

    void start() {
        srv.Post("/api/tester-usage", [this](const httplib::Request& req, httplib::Response& res) {
            // DRAIN THE REQUEST BODY BEFORE REPLYING - the memory lesson:
            // httplib already fully populates req.body before the handler
            // runs, but the handler itself must not reply and close the
            // connection on an assumption that a hand-rolled server would
            // not get for free.
            std::lock_guard<std::mutex> lk(mu);
            lastBody = req.body;
            res.status = nextStatus;
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~RealServer() {
        srv.stop();
        if (th.joinable()) { th.join(); }
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/api/tester-usage"; }
};

TesterUsageOutcome sendAndWait(const std::string& url, const std::string& json) {
    TesterUsageSender s;
    s.send(url, json);
    std::optional<TesterUsageOutcome> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeOutcome();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    return got.value_or(TesterUsageOutcome::Retry);
}

void testRealServerReceivesEveryResponseCodePosix() {
    RealServer srv;
    srv.start();
    CHECK(srv.port > 0);

    srv.nextStatus = 200;
    CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Sent);
    {
        std::lock_guard<std::mutex> lk(srv.mu);
        CHECK(srv.lastBody == "{\"token\":\"a\"}");
    }

    srv.nextStatus = 401;
    CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Invalid);

    srv.nextStatus = 413;
    CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Rejected);

    srv.nextStatus = 429;
    CHECK(sendAndWait(srv.url(), "{\"token\":\"a\"}") == TesterUsageOutcome::Retry);
}

void testRefusedConnectionIsRetryPosix() {
    // A bound-then-released port - nothing listens, as close to "the server
    // is down" as a deterministic test gets.
    int port = 0;
    {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
        probe.stop();
    }
    CHECK(port > 0);
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/tester-usage";
    CHECK(sendAndWait(url, "{\"token\":\"a\"}") == TesterUsageOutcome::Retry);
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    const std::string givenRoot = argc > 1 ? argv[1] : std::string();
    testValidTesterToken();
    testExtractBareToken();
    testExtractFromPortalLink();
    testMaskTesterToken();
    testMaskedPreviewJsonNeverContainsTheRealToken();
    testPayloadContainsExactlyTheContractFields();
    testPlatformIsOneOfTheThreeNames();
    testDisarmedRecorderCollectsNothing();
    testArmedRecorderDeduplicatesAndAccrues();
    testCatalogueIdJoinsOnFileNameAndFallsBackToSideloaded();
    testRfc3339FormatIsFixedWidthAndUtc();
    testQueueBoundedToThreeDroppingTheOldest();
    testTokenOfReport();
    testDropOthersRemovesReportsForADifferentToken();
    testApplyOutcomeSentDropsTheFront();
    testApplyOutcomeRejectedDropsTheFrontToo();
    testApplyOutcomeInvalidStopsAndMarksInvalidWithoutDropping();
    testApplyOutcomeRetryStopsWithoutMarkingInvalidOrDropping();
    testEndpointOverride();
    testNonLoopbackHttpUrlIsRefused();
    testSenderRunsOnceAndReportsViaTakeOutcome();
    testDestroyingABusySenderNeverBlocks();
    testTokenNeverAppearsNearALogCall(givenRoot);
    testTokenIsStoredInConfigVerbatim();
    testConfigDropsPendingReportsForAnotherTokenOnLoad();
#if defined(_WIN32)
    testRealServerReceivesEveryResponseCodeWindows();
    testShutdownNeverWaitsOnTheNetwork();
#else
    testRealServerReceivesEveryResponseCodePosix();
    testRefusedConnectionIsRetryPosix();
#endif
    return testSummary("test_tester_usage");
}
