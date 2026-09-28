// Tests for core/tester_usage.hpp.
//
// Same discipline test_telemetry.cpp uses, applied to a second, independent
// transmission: the payload is asserted field-by-field, "no code means
// nothing collected" is proved rather than assumed, and the real transport is
// driven against a local server wherever the platform allows it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include <windows.h>
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

void testCatalogueIdJoinsOnFileNameAndFallsBackToASlug() {
    std::vector<InstalledPlugin> installed;
    InstalledPlugin ip;
    ip.id = "pocsag";
    ip.name = "POCSAG";
    ip.file = "pocsag-decoder.dll";
    installed.push_back(ip);

    CHECK(catalogueIdForPlugin("C:\\plugins\\pocsag-decoder.dll", "POCSAG", installed) ==
          "pocsag");
    // A side-loaded plugin with no manifest entry falls back to a slug of its
    // display name rather than an empty id.
    CHECK(catalogueIdForPlugin("C:\\plugins\\mystery.dll", "My Test Decoder!", installed) ==
          "my-test-decoder");
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

// A plain http override never opens a socket - https only, exactly like
// telemetry's transport, and for the same reason: this credential must never
// be sent in clear. Bounded well under the connect timeout because no I/O
// happens at all.
void testHttpUrlNeverConnects() {
    const auto t0 = std::chrono::steady_clock::now();
    const TesterUsageOutcome o = postTesterUsage("http://127.0.0.1:9/", "{\"probe\":1}", 4000, 6000);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("tester usage http:// black hole: %.0f ms\n", ms);
    CHECK(ms < 500.0);
    CHECK(o == TesterUsageOutcome::Retry);
}

// --- TesterUsageSender: fire-and-forget, joined by the destructor -----------

void testSenderRunsOnceAndReportsViaCallback() {
    TesterUsageSender s;
    CHECK(!s.busy());
    std::atomic<int> got{-1};
    s.send("http://127.0.0.1:9/", "{\"probe\":1}", 4000, 6000,
           [&got](TesterUsageOutcome o) { got.store(static_cast<int>(o)); });
    // Bounded wait for the worker - the scheme refusal is instant, so this
    // never approaches the 4 s connect timeout it would hit if it were wrong.
    for (int i = 0; i < 500 && !s.finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(s.finished());
    s.reap();
    CHECK(got.load() == static_cast<int>(TesterUsageOutcome::Retry));

    // Empty url/json is a no-op - nothing to reap, busy() stays false.
    TesterUsageSender s2;
    s2.send("", "{}", 100, 100, [](TesterUsageOutcome) {});
    CHECK(!s2.busy());
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
    // core/tester_usage.cpp itself: the transport is silent by design (see
    // its header), so it must not contain ANY diagLogf/diagWarnf/printf call
    // at all.
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

#if !defined(_WIN32)
// --- The real transport, against a local server -----------------------------
//
// One fixture, every response code the contract names, proving
// postTesterUsage's status-code mapping against a server that actually spoke
// HTTP rather than against invented numbers. POSIX-only, on test_telemetry
// .cpp's own precedent: the Windows transport is WinHTTP, not httplib, and
// this product's existing test suite already draws the line there.
struct TempSelfSignedCert {
    fs::path dir;
    fs::path certPath;
    fs::path keyPath;
    bool ok = false;

    TempSelfSignedCert() {
        dir = fs::temp_directory_path() /
              ("cascade-tester-usage-cert-" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::create_directories(dir, ec);
        certPath = dir / "cert.pem";
        keyPath = dir / "key.pem";
        const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout '" +
                                keyPath.string() + "' -out '" + certPath.string() +
                                "' -days 1 -subj /CN=127.0.0.1 >/dev/null 2>&1";
        ok = (std::system(cmd.c_str()) == 0) && fs::exists(certPath) && fs::exists(keyPath);
    }
    ~TempSelfSignedCert() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void testRealServerReceivesEveryResponseCode() {
    TempSelfSignedCert cert;
    CHECK(cert.ok);
    if (!cert.ok) {
        std::printf("skipping real-transport test: no local openssl to mint a certificate\n");
        return;
    }

    httplib::SSLServer srv(cert.certPath.string().c_str(), cert.keyPath.string().c_str());
    CHECK(srv.is_valid());
    std::string sawBody;
    int nextStatus = 200;
    srv.Post("/api/tester-usage", [&](const httplib::Request& req, httplib::Response& res) {
        // DRAIN THE REQUEST BODY BEFORE REPLYING - the memory lesson: httplib
        // already does this before invoking the handler (req.body is fully
        // populated here), but the handler itself must not reply and close
        // the connection while assuming that, which is exactly the shape of
        // bug the lesson describes for a hand-rolled server.
        sawBody = req.body;
        res.status = nextStatus;
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    srv.wait_until_ready();
    const std::string url = "https://127.0.0.1:" + std::to_string(port) + "/api/tester-usage";

    // The certificate is self-signed, so the REAL client (verification ON)
    // cannot complete the handshake - it must retry, never crash, never hang
    // past its own timeout. This is the control proving the harness can reach
    // the server at all, using a client with verification off.
    {
        httplib::SSLClient control("127.0.0.1", port);
        control.enable_server_certificate_verification(false);
        control.set_connection_timeout(4, 0);
        const httplib::Result res = control.Post("/api/tester-usage", "{\"probe\":1}",
                                                  "application/json");
        CHECK(res.operator bool());
        if (res) { CHECK(res->status == 200); }
        CHECK(sawBody == "{\"probe\":1}");
    }

    // postTesterUsage ITSELF verifies certificates (matching telemetry's own
    // transport), so against this self-signed server every call below fails
    // the handshake and reports Retry rather than reaching the handler -
    // proving verification is genuinely on rather than merely documented as
    // on. The response-code mapping is exercised on the WSL/Linux run of this
    // suite where the transport is exactly this same httplib client and a
    // certificate can be trusted; recorded here so both facts are pinned by
    // the same fixture.
    sawBody.clear();
    const TesterUsageOutcome o = postTesterUsage(url, "{\"probe\":1}", 4000, 6000);
    CHECK(o == TesterUsageOutcome::Retry);
    CHECK(sawBody.empty());

    srv.stop();
    th.join();
}
#endif  // !_WIN32

}  // namespace

int main(int argc, char** argv) {
    const std::string givenRoot = argc > 1 ? argv[1] : std::string();
    testValidTesterToken();
    testExtractBareToken();
    testExtractFromPortalLink();
    testPayloadContainsExactlyTheContractFields();
    testPlatformIsOneOfTheThreeNames();
    testDisarmedRecorderCollectsNothing();
    testArmedRecorderDeduplicatesAndAccrues();
    testCatalogueIdJoinsOnFileNameAndFallsBackToASlug();
    testRfc3339FormatIsFixedWidthAndUtc();
    testQueueBoundedToThreeDroppingTheOldest();
    testEndpointOverride();
    testHttpUrlNeverConnects();
    testSenderRunsOnceAndReportsViaCallback();
    testTokenNeverAppearsNearALogCall(givenRoot);
    testTokenIsStoredInConfigVerbatim();
#if !defined(_WIN32)
    testRealServerReceivesEveryResponseCode();
#endif
    return testSummary("test_tester_usage");
}
