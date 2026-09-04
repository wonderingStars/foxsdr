// Tests for the web server (net/web_server.hpp), driven over real HTTP against
// a real listener on loopback.
//
// WHY OVER THE WIRE RATHER THAN AGAINST THE CLASS. The bind policy is already
// unit-tested in isolation (tests/test_web_policy.cpp); what cannot be proved
// that way is that the server actually WIRES the decision to the request path —
// that a 401 really comes back without a cookie, that the cookie the login
// hands out really opens the gate, and that logging out really shuts it again.
// Those are properties of the assembled thing, and a mock would only restate
// the assumption being tested.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include <httplib.h>

#include "core/plugin_abi.h"
#include "core/plugin_ui.hpp"
#include "dsp/demod.hpp"
#include "net/web_auth.hpp"
#include "net/web_server.hpp"
#include "test_check.hpp"

using namespace cascade::net;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "a-good-long-password";

std::string realRecord() {
    PasswordRecord rec;
    std::string error;
    if (!hashPassword(kPassword, rec, error)) {
        return std::string();
    }
    return rec.serialize();
}

// Finds a port this machine will actually let us have. Fixed ports collide with
// whatever the developer already has listening and with a second copy of this
// suite; scanning avoids both without needing the policy to accept port 0.
int startOnFreePort(WebServer& server, WebServerConfig cfg, std::string& error) {
    for (int port = 18073; port < 18173; ++port) {
        cfg.port = port;
        if (server.start(cfg, error)) {
            return port;
        }
        // A policy refusal will fail identically on every port, so stop rather
        // than grinding through a hundred of them.
        if (!server.decision().allowed()) {
            return -1;
        }
    }
    return -1;
}

WebServerConfig loopbackConfig() {
    WebServerConfig cfg;
    cfg.enabled = true;
    cfg.bindAddress = "127.0.0.1";
    cfg.username = "admin";
    return cfg;
}

// Pulls the session token out of a Set-Cookie header.
std::string tokenFromSetCookie(const std::string& setCookie) {
    const std::string prefix = "foxsdr_session=";
    if (setCookie.rfind(prefix, 0) != 0) {
        return std::string();
    }
    const std::size_t end = setCookie.find(';');
    const std::size_t from = prefix.size();
    return setCookie.substr(from, (end == std::string::npos ? setCookie.size() : end) - from);
}

std::string loginBody(const std::string& user, const std::string& password) {
    return std::string("{\"username\":\"") + user + "\",\"password\":\"" + password + "\"}";
}

RadioStatus sampleStatus() {
    RadioStatus s;
    s.running = true;
    s.centerHz = 100000000.0;
    s.sampleRateHz = 2000000.0;
    s.vfoOffsetHz = 300000.0;
    s.bandwidthHz = 150000.0;
    s.mode = "WFM";
    s.sourceName = "SigGen";
    s.signalDb = -42.5f;
    return s;
}

void testLoopbackWithoutPasswordServesOpenly() {
    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    CHECK(server.running());
    CHECK(server.boundPort() == port);
    CHECK(!server.decision().authRequired);
    CHECK(!server.decision().reachableOffMachine);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    auto page = cli.Get("/");
    CHECK(static_cast<bool>(page));
    if (page) {
        CHECK(page->status == 200);
        CHECK(page->body.find("FoxSDR") != std::string::npos);
        // The strict policy the three-route split exists to allow.
        const std::string csp = page->get_header_value("Content-Security-Policy");
        CHECK(csp.find("default-src 'none'") != std::string::npos);
        CHECK(csp.find("frame-ancestors 'none'") != std::string::npos);
        CHECK(csp.find("unsafe-inline") == std::string::npos);
        CHECK(page->get_header_value("X-Content-Type-Options") == "nosniff");
    }

    CHECK(static_cast<bool>(cli.Get("/app.css")));
    CHECK(static_cast<bool>(cli.Get("/app.js")));

    // No cookie, and the API answers anyway: this binding requires no auth.
    auto status = cli.Get("/api/status");
    CHECK(static_cast<bool>(status));
    if (status) {
        CHECK(status->status == 200);
        CHECK(status->body.find("\"mode\":\"WFM\"") != std::string::npos);
        CHECK(status->body.find("\"centerHz\":1") != std::string::npos);
    }

    server.stop();
    CHECK(!server.running());
}

void testPasswordGateOnLoopback() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();
    CHECK(!cfg.passwordRecord.empty());

    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    // A password is set, so it is enforced even here (policy rule 3).
    CHECK(server.decision().authRequired);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // Locked without a cookie.
    auto denied = cli.Get("/api/status");
    CHECK(static_cast<bool>(denied));
    if (denied) {
        CHECK(denied->status == 401);
    }

    // A fabricated cookie must not open it.
    const httplib::Headers forgedHeaders = {{"Cookie", "foxsdr_session=not-a-real-token"}};
    auto forged = cli.Get("/api/status", forgedHeaders);
    CHECK(static_cast<bool>(forged));
    if (forged) {
        CHECK(forged->status == 401);
    }

    // The session endpoint is reachable unauthenticated and reports the truth.
    auto session = cli.Get("/api/session");
    CHECK(static_cast<bool>(session));
    if (session) {
        CHECK(session->status == 200);
        CHECK(session->body.find("\"authRequired\":true") != std::string::npos);
        CHECK(session->body.find("\"authenticated\":false") != std::string::npos);
    }

    // Wrong password. The derivation count is sampled around each of the two
    // failing logins below because the identical BODY they return is only half
    // of not distinguishing them — see the assertion after them.
    const std::uint64_t beforeBad = pbkdf2CallCount();
    auto bad = cli.Post("/api/login", loginBody("admin", "wrong-password-here"),
                        "application/json");
    const std::uint64_t badCost = pbkdf2CallCount() - beforeBad;
    CHECK(static_cast<bool>(bad));
    if (bad) {
        CHECK(bad->status == 401);
        CHECK(bad->get_header_value("Set-Cookie").empty());
    }

    // Right password, wrong user — and the message must not distinguish them.
    const std::uint64_t beforeWrongUser = pbkdf2CallCount();
    auto wrongUser = cli.Post("/api/login", loginBody("root", kPassword),
                              "application/json");
    const std::uint64_t wrongUserCost = pbkdf2CallCount() - beforeWrongUser;
    CHECK(static_cast<bool>(wrongUser));
    if (wrongUser) {
        CHECK(wrongUser->status == 401);
        CHECK(bad && wrongUser->body == bad->body);
    }

    // The wrong user name must cost the same NUMBER of key derivations as the
    // wrong password, or the two cases tell themselves apart by TIMING and the
    // shared message above buys nothing. verifyLogin() guarantees this and is
    // tested directly in test_web_auth.cpp — what is pinned HERE is that the
    // HTTP handler actually CALLS it: reverting this route to the short-
    // circuiting `user == expectedUser && verifyPassword(...)` spelling leaves
    // every other assertion in this suite green and drops this one to zero.
    // Counted, not timed, so a loaded machine cannot make it flake; the client
    // is synchronous and single-threaded, so each Post's derivation has landed
    // before the sample after it is taken.
    CHECK(badCost == 1u);
    CHECK(wrongUserCost == badCost);

    // A form-encoded post is refused before any password work happens (CSRF).
    auto wrongType = cli.Post("/api/login", "username=admin&password=x",
                              "application/x-www-form-urlencoded");
    CHECK(static_cast<bool>(wrongType));
    if (wrongType) {
        CHECK(wrongType->status == 415);
    }

    // Correct credentials.
    auto ok = cli.Post("/api/login", loginBody("admin", kPassword), "application/json");
    CHECK(static_cast<bool>(ok));
    std::string token;
    if (ok) {
        CHECK(ok->status == 200);
        token = tokenFromSetCookie(ok->get_header_value("Set-Cookie"));
        CHECK(!token.empty());
        // The cookie must carry the flags that make it hard to steal or
        // replay from another origin.
        const std::string cookie = ok->get_header_value("Set-Cookie");
        CHECK(cookie.find("HttpOnly") != std::string::npos);
        CHECK(cookie.find("SameSite=Strict") != std::string::npos);
    }
    CHECK(server.sessionCount() == 1);

    const httplib::Headers authed = {{"Cookie", "foxsdr_session=" + token}};

    auto allowed = cli.Get("/api/status", authed);
    CHECK(static_cast<bool>(allowed));
    if (allowed) {
        CHECK(allowed->status == 200);
        CHECK(allowed->body.find("\"sourceName\":\"SigGen\"") != std::string::npos);
    }

    // Logging out closes it again. The cookie must be sent WITH the logout, or
    // the server has no token to revoke — the browser client does this through
    // credentials:'same-origin', and omitting it here originally made this
    // assertion fail against a server that was behaving correctly.
    auto out = cli.Post("/api/logout", authed, "", "application/json");
    CHECK(static_cast<bool>(out));
    if (out) {
        CHECK(out->status == 200);
    }
    auto afterLogout = cli.Get("/api/status", authed);
    CHECK(static_cast<bool>(afterLogout));
    if (afterLogout) {
        // The token was revoked server-side, so presenting it again fails even
        // though the client still holds the string.
        CHECK(afterLogout->status == 401);
    }

    server.stop();
}

void testLoginThrottleOverHttp() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    for (std::size_t i = 0; i < kMaxLoginAttempts; ++i) {
        auto r = cli.Post("/api/login", loginBody("admin", "definitely-wrong"),
                          "application/json");
        CHECK(static_cast<bool>(r));
        if (r) {
            CHECK(r->status == 401);
        }
    }
    // The next attempt is refused before the password is even checked.
    auto blocked = cli.Post("/api/login", loginBody("admin", "definitely-wrong"),
                            "application/json");
    CHECK(static_cast<bool>(blocked));
    if (blocked) {
        CHECK(blocked->status == 429);
        CHECK(!blocked->get_header_value("Retry-After").empty());
    }
    // Even the CORRECT password is refused while the client is throttled —
    // otherwise the limit would only slow down an attacker who never guesses
    // right, which is not the interesting case.
    auto correctButBlocked =
        cli.Post("/api/login", loginBody("admin", kPassword), "application/json");
    CHECK(static_cast<bool>(correctButBlocked));
    if (correctButBlocked) {
        CHECK(correctButBlocked->status == 429);
    }

    server.stop();
}

void testSessionExpiresOnTheInjectedClock() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    std::atomic<std::int64_t> fakeNow{1000};
    WebServer server;
    CHECK(server.setClock([&fakeNow]() { return fakeNow.load(); }));

    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    auto ok = cli.Post("/api/login", loginBody("admin", kPassword), "application/json");
    CHECK(static_cast<bool>(ok));
    std::string token;
    if (ok) {
        CHECK(ok->status == 200);
        token = tokenFromSetCookie(ok->get_header_value("Set-Cookie"));
    }
    const httplib::Headers authed = {{"Cookie", "foxsdr_session=" + token}};

    auto before = cli.Get("/api/status", authed);
    CHECK(static_cast<bool>(before));
    if (before) {
        CHECK(before->status == 200);
    }

    // Step past the session lifetime.
    fakeNow.store(1000 + kSessionTtlSeconds);

    auto after = cli.Get("/api/status", authed);
    CHECK(static_cast<bool>(after));
    if (after) {
        CHECK(after->status == 401);
    }

    server.stop();
}

void testOffMachineBindWithoutPasswordNeverListens() {
    WebServerConfig cfg = loopbackConfig();
    cfg.bindAddress = "0.0.0.0";  // no password set

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port < 0);
    CHECK(!server.running());
    CHECK(server.boundPort() == -1);
    CHECK(!error.empty());
    CHECK(server.decision().verdict == BindVerdict::RefusedPasswordRequired);
    // The refusal must be the POLICY's, not a failed bind — the distinction
    // the header promises callers can make.
    CHECK(!server.decision().allowed());
}

void testSpectrumEndpoint() {
    WebServer server;
    // A provider that reports a fresh frame once, then nothing newer — the
    // same contract Pipeline::getLatestFrame has.
    server.setSpectrumProvider([](SpectrumSnapshot& inOut) {
        if (inOut.seq >= 7) {
            return false;
        }
        inOut.seq = 7;
        inOut.centerHz = 100000000.0;
        inOut.spanHz = 2000000.0;
        inOut.dbBins.assign(8, kSpectrumDbMax);   // full scale
        inOut.dbBins[0] = kSpectrumDbMin;         // and one at the floor
        return true;
    });

    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    auto fresh = cli.Get("/api/spectrum?since=0");
    CHECK(static_cast<bool>(fresh));
    if (fresh) {
        CHECK(fresh->status == 200);
        CHECK(fresh->body.find("\"fresh\":true") != std::string::npos);
        CHECK(fresh->body.find("\"seq\":7") != std::string::npos);
        CHECK(fresh->body.find("\"bins\":") != std::string::npos);
        // 8 bins: floor then seven at full scale -> 0x00 then 7 x 0xFF.
        std::vector<std::uint8_t> expected(8, 0xFF);
        expected[0] = 0x00;
        CHECK(fresh->body.find(base64Encode(expected)) != std::string::npos);
    }

    // Asking again with the cursor the server just handed back yields no frame.
    auto stale = cli.Get("/api/spectrum?since=7");
    CHECK(static_cast<bool>(stale));
    if (stale) {
        CHECK(stale->status == 200);
        CHECK(stale->body.find("\"fresh\":false") != std::string::npos);
        CHECK(stale->body.find("\"bins\":") == std::string::npos);
    }

    // A nonsense cursor must not throw out of the handler.
    auto junk = cli.Get("/api/spectrum?since=not-a-number");
    CHECK(static_cast<bool>(junk));
    if (junk) {
        CHECK(junk->status == 200);
    }

    server.stop();
}

void testInvalidUtf8FromAPluginDoesNotKillTheApi() {
    // A DECODER PLUGIN MUST NOT BE ABLE TO TAKE DOWN THE BROWSER INTERFACE.
    // Decoders turn radio noise into text: POCSAG and RTTY will happily emit
    // whatever is on the air, and the Inmarsat-C decoder ships knowing its
    // constants are unverified. nlohmann THROWS on invalid UTF-8 by default,
    // so one stray byte above 127 in one decoded line would otherwise throw
    // inside the status handler and blank the whole readout — every panel,
    // not just that line.
    WebServer server;
    server.setStatusProvider([]() {
        RadioStatus s = sampleStatus();
        // A lone 0xFF and a truncated multi-byte sequence: neither is valid
        // UTF-8, and both are exactly what a misconfigured decoder emits.
        RadioStatus::DecodedLine bad;
        bad.plugin = "POCSAG";
        bad.text = std::string("noise \xFF\xFE then \xE2\x82 truncated");
        s.decoded.push_back(bad);
        // The same hazard by a different route: a track label from a plugin.
        RadioStatus::Track t;
        t.id = "ABC123";
        t.label = std::string("bad\xC3");
        t.plugin = "ADS-B";
        s.tracks.push_back(t);
        return s;
    });

    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    auto r = cli.Get("/api/status");
    CHECK(static_cast<bool>(r));
    if (r) {
        // The whole point: 200 with a usable body, not an empty 500.
        CHECK(r->status == 200);
        CHECK(!r->body.empty());
        // And the rest of the readout still arrived intact alongside it.
        CHECK(r->body.find("\"mode\":\"WFM\"") != std::string::npos);
        CHECK(r->body.find("POCSAG") != std::string::npos);
        CHECK(r->body.find("ABC123") != std::string::npos);
    }

    server.stop();
}

void testStaleCursorFromAPreviousRunRecovers() {
    // THE BUG THIS GUARDS. Frame sequence numbers restart at zero when the
    // application does, but a browser left open across a restart keeps its
    // cursor. Without the impossible-cursor reset it asks for ever for a
    // frame newer than any that will exist: the status poll keeps working, so
    // the page looks connected while the spectrum and waterfall are frozen
    // permanently, and only a manual reload recovers. Found by killing the
    // app under a live page and watching it never come back.
    WebServer server;
    server.setSpectrumProvider([](SpectrumSnapshot& inOut) {
        // A freshly started receiver: its newest frame is sequence 5.
        if (inOut.seq >= 5) {
            return false;
        }
        inOut.seq = 5;
        inOut.centerHz = 100000000.0;
        inOut.spanHz = 2000000.0;
        inOut.dbBins.assign(8, kSpectrumDbMax);
        return true;
    });

    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // A cursor from the previous run — far beyond anything this process has
    // produced. It must still be served a frame.
    auto stale = cli.Get("/api/spectrum?since=358049");
    CHECK(static_cast<bool>(stale));
    if (stale) {
        CHECK(stale->status == 200);
        CHECK(stale->body.find("\"fresh\":true") != std::string::npos);
        CHECK(stale->body.find("\"seq\":5") != std::string::npos);
    }

    // And normal cursor behaviour is untouched: having been given frame 5,
    // asking again with 5 yields nothing new rather than the same frame twice
    // (which would double every waterfall row).
    auto repeat = cli.Get("/api/spectrum?since=5");
    CHECK(static_cast<bool>(repeat));
    if (repeat) {
        CHECK(repeat->status == 200);
        CHECK(repeat->body.find("\"fresh\":false") != std::string::npos);
    }

    server.stop();
}

void testRestartRevokesSessions() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    auto ok = cli.Post("/api/login", loginBody("admin", kPassword), "application/json");
    CHECK(static_cast<bool>(ok));
    std::string token;
    if (ok) {
        token = tokenFromSetCookie(ok->get_header_value("Set-Cookie"));
    }
    CHECK(server.sessionCount() == 1);

    // Re-applying settings restarts the listener, and every session goes with
    // it — the binding or the password may have just changed.
    cfg.port = port;
    CHECK(server.start(cfg, error));
    CHECK(server.sessionCount() == 0);

    httplib::Client cli2("127.0.0.1", port);
    cli2.set_connection_timeout(5, 0);
    const httplib::Headers staleHeaders = {{"Cookie", "foxsdr_session=" + token}};
    auto after = cli2.Get("/api/status", staleHeaders);
    CHECK(static_cast<bool>(after));
    if (after) {
        CHECK(after->status == 401);
    }

    server.stop();
}

void testControlQueue() {
    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // A valid request is ACCEPTED (202, not 200 — it has not been carried out
    // yet) and lands in the queue for the application to drain.
    auto ok = cli.Post("/api/control", "{\"centerHz\":144800000,\"mode\":\"NFM\"}",
                       "application/json");
    CHECK(static_cast<bool>(ok));
    if (ok) {
        CHECK(ok->status == 202);
    }
    std::vector<ControlRequest> drained = server.takePendingControls();
    CHECK(drained.size() == 1);
    if (drained.size() == 1) {
        CHECK(drained[0].centerHz.value_or(0.0) == 144800000.0);
        CHECK(drained[0].mode.value_or(cascade::dsp::DemodMode::RAW) ==
              cascade::dsp::DemodMode::NFM);
    }
    // Draining empties it: a request must not be applied twice.
    CHECK(server.takePendingControls().empty());

    // A rejected request reaches the queue at all.
    auto bad = cli.Post("/api/control", "{\"gaain\":30}", "application/json");
    CHECK(static_cast<bool>(bad));
    if (bad) {
        CHECK(bad->status == 400);
    }
    CHECK(server.takePendingControls().empty());

    // Form-encoded is refused before anything is parsed (CSRF), and this is
    // the endpoint that moves the radio, so it matters more here than at login.
    auto wrongType = cli.Post("/api/control", "centerHz=144800000",
                              "application/x-www-form-urlencoded");
    CHECK(static_cast<bool>(wrongType));
    if (wrongType) {
        CHECK(wrongType->status == 415);
    }
    CHECK(server.takePendingControls().empty());

    server.stop();
}

void testControlRequiresAuth() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // THE ONE THAT MATTERS: an unauthenticated caller must not be able to move
    // the receiver, and must not get a request into the queue by trying.
    auto denied = cli.Post("/api/control", "{\"centerHz\":144800000}",
                           "application/json");
    CHECK(static_cast<bool>(denied));
    if (denied) {
        CHECK(denied->status == 401);
    }
    CHECK(server.takePendingControls().empty());

    auto login = cli.Post("/api/login", loginBody("admin", kPassword),
                          "application/json");
    CHECK(static_cast<bool>(login));
    std::string token;
    if (login) {
        token = tokenFromSetCookie(login->get_header_value("Set-Cookie"));
    }
    const httplib::Headers authed = {{"Cookie", "foxsdr_session=" + token}};

    auto allowed = cli.Post("/api/control", authed, "{\"centerHz\":144800000}",
                            "application/json");
    CHECK(static_cast<bool>(allowed));
    if (allowed) {
        CHECK(allowed->status == 202);
    }
    CHECK(server.takePendingControls().size() == 1);

    server.stop();
}

void testControlQueueIsBounded() {
    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // An application that stopped draining must not let a client grow this
    // without limit.
    const std::size_t sent = WebServer::kMaxQueuedControls + 10;
    for (std::size_t i = 0; i < sent; ++i) {
        const std::string body =
            "{\"centerHz\":" + std::to_string(100000000 + i) + "}";
        auto r = cli.Post("/api/control", body, "application/json");
        CHECK(static_cast<bool>(r));
    }
    const std::vector<ControlRequest> drained = server.takePendingControls();
    CHECK(drained.size() == WebServer::kMaxQueuedControls);
    // The OLDEST were dropped: for a control surface the most recent
    // instruction is the one that matters, so the newest must survive.
    if (!drained.empty()) {
        CHECK(drained.back().centerHz.value_or(0.0) ==
              static_cast<double>(100000000 + sent - 1));
    }

    server.stop();
}

void testTileEndpoint() {
    WebServer server;
    RadioStatus withBasemap = sampleStatus();
    withBasemap.basemap.active = true;
    withBasemap.basemap.attribution = "(c) OpenStreetMap contributors";
    withBasemap.basemap.minZoom = 0;
    withBasemap.basemap.maxZoom = 19;
    withBasemap.basemap.tileSize = 256;
    server.setStatusProvider([withBasemap]() { return withBasemap; });
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // The status carries what the browser needs to plan fetches and to credit
    // the imagery — the attribution requirement must survive the transport.
    auto status = cli.Get("/api/status");
    CHECK(static_cast<bool>(status));
    if (status) {
        CHECK(status->body.find("\"basemap\"") != std::string::npos);
        CHECK(status->body.find("(c) OpenStreetMap contributors") != std::string::npos);
        CHECK(status->body.find("\"tileSize\":256") != std::string::npos);
    }

    // A tile the server does not hold answers 202 AND records the want.
    auto first = cli.Get("/api/tile/3/1/2");
    CHECK(static_cast<bool>(first));
    if (first) {
        CHECK(first->status == 202);
        CHECK(first->body.find("\"pending\":true") != std::string::npos);
    }
    // Asking twice records once: the pump must not fetch the same tile twice
    // because two polls raced.
    CHECK(static_cast<bool>(cli.Get("/api/tile/3/1/2")));
    std::vector<WebServer::TileRequest> wants = server.takePendingTileRequests();
    CHECK(wants.size() == 1);
    if (wants.size() == 1) {
        CHECK(wants[0].z == 3 && wants[0].x == 1 && wants[0].y == 2);
    }
    // Taking drains: the same want must not be served twice.
    CHECK(server.takePendingTileRequests().empty());

    // A published tile is served byte-for-byte as image/bmp.
    const std::vector<std::uint8_t> fakeBmp = {'B', 'M', 1, 2, 3, 4, 5};
    server.setTile(3, 1, 2, fakeBmp);
    auto served = cli.Get("/api/tile/3/1/2");
    CHECK(static_cast<bool>(served));
    if (served) {
        CHECK(served->status == 200);
        CHECK(served->get_header_value("Content-Type") == "image/bmp");
        CHECK(served->body.size() == fakeBmp.size());
        CHECK(std::equal(fakeBmp.begin(), fakeBmp.end(), served->body.begin(),
                         [](std::uint8_t a, char b) {
                             return a == static_cast<std::uint8_t>(b);
                         }));
    }

    // An EMPTY publish means the plugin said the tile will never exist: 404,
    // which is what stops the browser retrying it, and nothing re-recorded.
    server.setTile(3, 4, 5, {});
    auto missing = cli.Get("/api/tile/3/4/5");
    CHECK(static_cast<bool>(missing));
    if (missing) {
        CHECK(missing->status == 404);
    }
    CHECK(server.takePendingTileRequests().empty());

    // Addresses outside the XYZ grid are refused, not recorded: x and y are
    // bounded by 2^z, and z by any real tile server's range.
    auto badZ = cli.Get("/api/tile/23/0/0");
    CHECK(static_cast<bool>(badZ) && badZ->status == 400);
    auto badX = cli.Get("/api/tile/3/8/0");   // 2^3 tiles means x <= 7
    CHECK(static_cast<bool>(badX) && badX->status == 400);
    auto junk = cli.Get("/api/tile/3/nope/0");
    CHECK(static_cast<bool>(junk));
    if (junk) {
        CHECK(junk->status == 404);   // does not match the route's digit pattern
    }
    CHECK(server.takePendingTileRequests().empty());

    // clearTiles drops the stored tile AND the queue: after a plugin change
    // the old source's imagery must not be served as the new one's.
    server.clearTiles();
    auto afterClear = cli.Get("/api/tile/3/1/2");
    CHECK(static_cast<bool>(afterClear));
    if (afterClear) {
        CHECK(afterClear->status == 202);
    }
    CHECK(server.takePendingTileRequests().size() == 1);

    // The request queue is bounded; whatever is dropped costs the browser a
    // retry, not a tile.
    for (std::uint32_t i = 0; i < WebServer::kMaxQueuedTileRequests + 10; ++i) {
        auto r = cli.Get("/api/tile/10/" + std::to_string(i) + "/0");
        CHECK(static_cast<bool>(r) && r->status == 202);
    }
    CHECK(server.takePendingTileRequests().size() == WebServer::kMaxQueuedTileRequests);

    // The store is bounded too, evicting the least recently served: after
    // publishing one over the cap, the untouched oldest tile is gone (202
    // again) while a freshly served one survives.
    for (std::uint32_t i = 0; i < WebServer::kMaxStoredTiles; ++i) {
        server.setTile(15, i, 1, fakeBmp);
    }
    // Serve tile 0 so it is recently used; tile 1 becomes the oldest.
    CHECK(static_cast<bool>(cli.Get("/api/tile/15/0/1")));
    server.setTile(15, 30000, 2, fakeBmp);   // one over the cap
    auto evicted = cli.Get("/api/tile/15/1/1");
    CHECK(static_cast<bool>(evicted));
    if (evicted) {
        CHECK(evicted->status == 202);
    }
    auto kept = cli.Get("/api/tile/15/0/1");
    CHECK(static_cast<bool>(kept));
    if (kept) {
        CHECK(kept->status == 200);
    }

    server.stop();
}

void testTileRequiresAuth() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // Tiles reveal where the user is looking; and an unauthenticated caller
    // must not be able to fill the request queue either.
    auto denied = cli.Get("/api/tile/3/1/2");
    CHECK(static_cast<bool>(denied));
    if (denied) {
        CHECK(denied->status == 401);
    }
    CHECK(server.takePendingTileRequests().empty());

    server.stop();
}

void testAudioStreamsRealSamples() {
    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }

    // A constant 0.5 means every sample on the wire must be 16384 — a
    // deterministic check on the whole path (ring, cursor, float->PCM,
    // chunked transfer) rather than just "some bytes arrived".
    std::atomic<bool> producing{true};
    std::thread producer([&server, &producing]() {
        std::vector<float> block(480, 0.5f);   // 10 ms at 48 kHz
        while (producing.load()) {
            server.pushAudio(block.data(), block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::string received;
    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(10, 0);
        auto res = cli.Get("/api/audio", [&received](const char* data, std::size_t len) {
            received.append(data, len);
            return received.size() < 2000;  // enough to prove it streams
        });
        // Returning false from a content receiver CANCELS the request, so the
        // result is Error::Canceled rather than a success — this stream has no
        // end of its own to reach. The proof that it worked is the bytes, which
        // are checked below.
        CHECK(!res);
        CHECK(res.error() == httplib::Error::Canceled);
    }
    producing.store(false);
    producer.join();

    CHECK(received.size() >= 2000);
    CHECK(received.size() % 2 == 0);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i + 1 < received.size(); i += 2) {
        const auto lo = static_cast<std::uint8_t>(received[i]);
        const auto hi = static_cast<std::uint8_t>(received[i + 1]);
        const auto s = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(lo) | (static_cast<std::uint16_t>(hi) << 8));
        if (s != 16384) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);

    server.stop();
}

void testAudioRequiresAuth() {
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = realRecord();

    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    // Audio is receiver output — what the set is actually hearing — so it is
    // gated exactly like everything else.
    auto denied = cli.Get("/api/audio");
    CHECK(static_cast<bool>(denied));
    if (denied) {
        CHECK(denied->status == 401);
    }
    CHECK(server.audioListeners() == 0);

    server.stop();
}

void testAudioListenerCap() {
    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) {
        return;
    }

    std::atomic<bool> producing{true};
    std::thread producer([&server, &producing]() {
        std::vector<float> block(480, 0.25f);
        while (producing.load()) {
            server.pushAudio(block.data(), block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Occupy every slot.
    std::atomic<bool> holdOpen{true};
    std::atomic<int> streaming{0};
    std::vector<std::thread> listeners;
    for (std::size_t i = 0; i < WebServer::kMaxAudioListeners; ++i) {
        listeners.emplace_back([port, &holdOpen, &streaming]() {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(5, 0);
            cli.set_read_timeout(30, 0);
            bool counted = false;
            cli.Get("/api/audio", [&](const char*, std::size_t) {
                if (!counted) {
                    counted = true;
                    streaming.fetch_add(1);
                }
                return holdOpen.load();
            });
        });
    }
    // Wait until every slot is genuinely in use, bounded so a failure reports
    // rather than hanging the suite.
    for (int i = 0; i < 200 && streaming.load() < static_cast<int>(WebServer::kMaxAudioListeners);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(streaming.load() == static_cast<int>(WebServer::kMaxAudioListeners));

    // One more must be refused rather than accepted and left to starve the
    // rest of the API of worker threads.
    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(5, 0);
        auto extra = cli.Get("/api/audio");
        CHECK(static_cast<bool>(extra));
        if (extra) {
            CHECK(extra->status == 503);
        }
    }

    holdOpen.store(false);
    for (std::thread& t : listeners) {
        t.join();
    }
    producing.store(false);
    producer.join();

    server.stop();
}

// ---------------------------------------------------------------------------
// The served client script
// ---------------------------------------------------------------------------
// WHY A REAL PARSE AND NOT A SUBSTRING CHECK. app.js is assembled from a
// handful of raw string literals, and nothing about how it is spliced together
// is checked by the C++ type system: a section left out of the join, a stray
// brace, an editing accident inside a literal. All of those still serve a
// 200 with plausible-looking content, and the script then dies at the first
// bad statement, taking every handler registered below it with it - which is
// how a whole feature disappears from the page with no server-side symptom.
// Only parsing the assembled article can see that, so the suite parses it.
//
// Skipped, loudly, when node is not installed: a test that silently passes
// because its tool is missing is worse than no test.

// Fetches /app.js from a freshly started loopback server. Empty on failure.
std::string fetchAppJs() {
    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    if (port <= 0) {
        return std::string();
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Get("/app.js");
    std::string body;
    if (res && res->status == 200) {
        body = res->body;
    }
    server.stop();
    return body;
}

fs::path scratchDir() {
    const fs::path dir =
        fs::temp_directory_path() / ("cascade_webjs_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p.string(), std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs a command with stdout+stderr captured to `log`. Returns the exit status.
int runCaptured(const std::string& command, const fs::path& log, std::string& out) {
    const std::string full = command + " > \"" + log.string() + "\" 2>&1";
    const int rc = std::system(full.c_str());
    out = readFile(log);
    return rc;
}

bool haveNode(const fs::path& dir) {
    std::string ignored;
    return runCaptured("node --version", dir / "node_version.txt", ignored) == 0;
}

void testServedScriptParses() {
    const std::string js = fetchAppJs();
    CHECK(js.size() > 10000u);  // the whole client, not one truncated section

    const fs::path dir = scratchDir();
    if (!haveNode(dir)) {
        std::printf("SKIP testServedScriptParses: node is not on PATH\n");
        return;
    }
    const fs::path jsPath = dir / "app.js";
    CHECK(writeFile(jsPath, js));

    std::string output;
    const int rc = runCaptured("node --check \"" + jsPath.string() + "\"",
                               dir / "check.txt", output);
    // The parse error, not just "it failed": the line number is what makes a
    // truncated literal diagnosable.
    if (rc != 0) {
        std::printf("node --check failed:\n%s\n", output.c_str());
    }
    CHECK(rc == 0);
}

// The pan is bound to POINTER events, which is what makes it work under a
// finger. Structural, because the failure it guards is structural: a page that
// binds only mousedown/mousemove pans under a mouse and is completely inert on
// a touchscreen, and every one of these tokens is load-bearing for that.
void testMapPanIsBoundToPointerEvents() {
    const std::string js = fetchAppJs();
    CHECK(js.find("$('map').addEventListener('pointerdown'") != std::string::npos);
    CHECK(js.find("window.addEventListener('pointermove'") != std::string::npos);
    CHECK(js.find("'pointerup', 'pointercancel'") != std::string::npos);
    CHECK(js.find("setPointerCapture") != std::string::npos);
    // And NOT the mouse-only binding it replaced, which would silently take
    // the gesture back on a desktop while leaving touch broken.
    CHECK(js.find("$('map').addEventListener('mousedown'") == std::string::npos);

    // touch-action:none on the canvas, or the browser claims the drag as a
    // page scroll before a single pointermove is delivered.
    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port > 0) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(5, 0);
        auto css = cli.Get("/app.css");
        CHECK(static_cast<bool>(css));
        if (css) {
            CHECK(css->status == 200);
            // INSIDE THE #map RULE, not merely somewhere in the file: the
            // comment above that rule names the property too, so a plain
            // substring search passes with the declaration deleted - which it
            // did, the first time this was written.
            // Anchored at the start of a line so it is the canvas's OWN rule,
            // not the `body[data-view="map"] #map` override that precedes it.
            const std::string sel = "\n#map { ";
            const std::size_t at = css->body.find(sel);
            CHECK(at != std::string::npos);
            const std::size_t end =
                (at == std::string::npos) ? std::string::npos : css->body.find('}', at);
            CHECK(end != std::string::npos);
            const std::string rule = (at == std::string::npos || end == std::string::npos)
                                         ? std::string()
                                         : css->body.substr(at, end - at);
            CHECK(rule.find("touch-action:none") != std::string::npos);
        }
    }
    server.stop();
}

// THE BROWSER MUST FADE ON THE SAME CURVE THE DESKTOP DOES. Two views of one
// set of targets that disagree about which are current is worse than either
// rule alone, so this runs the SERVED helper under node and compares it, value
// for value, with core::trackPresentation - the desktop's own function.
void testTrackFadeMatchesTheDesktopRule() {
    const std::string js = fetchAppJs();
    CHECK(js.find("function trackAlpha(") != std::string::npos);

    const fs::path dir = scratchDir();
    if (!haveNode(dir)) {
        std::printf("SKIP testTrackFadeMatchesTheDesktopRule: node is not on PATH\n");
        return;
    }
    const fs::path jsPath = dir / "app_fade.js";
    CHECK(writeFile(jsPath, js));

    // Lifts the three constants and the function out of the served script and
    // calls them. Extraction rather than execution because the script's own
    // top level touches the DOM, which node has not got.
    const char* kDriver = R"NODE(
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
function block(name) {
  const i = src.indexOf('function ' + name + '(');
  if (i < 0) throw new Error('missing function ' + name);
  let depth = 0, opened = false;
  for (let j = i; j < src.length; j++) {
    if (src[j] === '{') { depth++; opened = true; }
    else if (src[j] === '}') { depth--; if (opened && depth === 0) return src.slice(i, j + 1); }
  }
  throw new Error('unterminated function ' + name);
}
function decl(name) {
  const m = src.match(new RegExp('^const ' + name + ' = .*;$', 'm'));
  if (!m) throw new Error('missing const ' + name);
  return m[0];
}
const prelude = [decl('TRACK_FADE_MS'), decl('TRACK_DROP_MS'),
                 decl('TRACK_MIN_ALPHA'), block('trackAlpha')].join('\n');
const alpha = new Function(prelude + '\nreturn trackAlpha;')();
const out = [];
for (const arg of process.argv.slice(3)) {
  const parts = arg.split(':');
  out.push(alpha(Number(parts[0]), Number(parts[1])).toFixed(6));
}
console.log(out.join(' '));
)NODE";
    const fs::path driver = dir / "fade_driver.js";
    CHECK(writeFile(driver, kDriver));

    // Every boundary that matters, for every kind the host names plus one it
    // does not: fresh, the fade instant, the middle of the ramp, one
    // millisecond before the drop, the drop instant, and past it.
    struct Case {
        std::uint64_t ageMs;
        std::uint32_t kind;
    };
    const std::vector<Case> cases = {
        {0, CASCADE_TRACK_AIRCRAFT},        {29999, CASCADE_TRACK_AIRCRAFT},
        {30000, CASCADE_TRACK_AIRCRAFT},    {45000, CASCADE_TRACK_AIRCRAFT},
        {59999, CASCADE_TRACK_AIRCRAFT},    {60000, CASCADE_TRACK_AIRCRAFT},
        {3600000, CASCADE_TRACK_AIRCRAFT},  {0, CASCADE_TRACK_VESSEL},
        {420000, CASCADE_TRACK_VESSEL},     {600000, CASCADE_TRACK_VESSEL},
        {1800000, CASCADE_TRACK_STATION},   {2700000, CASCADE_TRACK_STATION},
        {3600000, CASCADE_TRACK_STATION},   {120000, CASCADE_TRACK_SATELLITE},
        {360000, CASCADE_TRACK_SATELLITE},  {600000, CASCADE_TRACK_SATELLITE},
        {0, 99u},                           {2700000, 99u},
        {3600000, 99u},
    };

    std::string args;
    for (const Case& c : cases) {
        args += " " + std::to_string(c.ageMs) + ":" + std::to_string(c.kind);
    }
    std::string output;
    const int rc = runCaptured("node \"" + driver.string() + "\" \"" + jsPath.string() + "\"" + args,
                               dir / "fade.txt", output);
    if (rc != 0) {
        std::printf("fade driver failed:\n%s\n", output.c_str());
    }
    CHECK(rc == 0);

    std::vector<double> served;
    {
        std::istringstream in(output);
        double v = 0.0;
        while (in >> v) {
            served.push_back(v);
        }
    }
    // Whole-container comparison of the SIZE first, and then indexing that the
    // size has been checked against - never assertions guarded by a size CHECK
    // that would run out of bounds in exactly the failing run.
    CHECK(served.size() == cases.size());
    if (served.size() == cases.size()) {
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const cascade::core::TrackPresentation p =
                cascade::core::trackPresentation(cases[i].ageMs, cases[i].kind);
            const double want = p.visible ? static_cast<double>(p.alpha) : 0.0;
            const bool same = std::fabs(served[i] - want) < 1e-4;
            if (!same) {
                std::printf("fade mismatch age=%llu kind=%u served=%.6f desktop=%.6f\n",
                            static_cast<unsigned long long>(cases[i].ageMs),
                            static_cast<unsigned>(cases[i].kind), served[i], want);
            }
            CHECK(same);
        }
    }
}

// THE BROWSER'S WATERFALL IS A MEASUREMENT, and until this test it was the
// only one of the three in this product with nothing holding it to that.
//
// The rule is the same one tests/test_waterfall_view.cpp pins for the desktop:
// a waterfall encodes signal strength as brightness, so a STRONGER signal must
// never render DARKER than a weaker one. Break that and the picture still
// looks like a waterfall - it just answers the question wrongly, everywhere,
// with nothing on screen to say so.
//
// It is checked here rather than assumed because this ramp has already been
// rewritten once: it was a blue-to-red jet map, and was re-toned to phosphor
// so the page and the desktop read as one instrument. That change was correct
// and it was also exactly the kind of change that can quietly cost the
// property, since the old table happened to be monotone too.
//
// The endpoints are pinned as literals against the DESKTOP's documented
// anchors rather than by calling waterfallColor(): this test binary does not
// link the GUI, and dragging imgui into the web suite to compare five bytes
// would be a worse trade than writing them down with the reason.
void testServedWaterfallRampIsMonotone() {
    const std::string js = fetchAppJs();
    CHECK(js.find("function colormap(") != std::string::npos);
    CHECK(js.find("const WF_STOPS = ") != std::string::npos);

    const fs::path dir = scratchDir();
    if (!haveNode(dir)) {
        std::printf("SKIP testServedWaterfallRampIsMonotone: node is not on PATH\n");
        return;
    }
    const fs::path jsPath = dir / "app_wf.js";
    CHECK(writeFile(jsPath, js));

    // Lifts the table and the interpolator out of the served script and runs
    // the real one, rather than re-implementing it here - a re-implementation
    // would only ever prove that two copies of my own arithmetic agree.
    //
    // The result goes through a Uint8ClampedArray because that is what the
    // page writes into, and its rounding is half-to-EVEN. Checking the floats
    // instead would pass on a table whose rounded output dips.
    const char* kDriver = R"NODE(
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
function block(name) {
  const i = src.indexOf('function ' + name + '(');
  if (i < 0) throw new Error('missing function ' + name);
  let depth = 0, opened = false;
  for (let j = i; j < src.length; j++) {
    if (src[j] === '{') { depth++; opened = true; }
    else if (src[j] === '}') { depth--; if (opened && depth === 0) return src.slice(i, j + 1); }
  }
  throw new Error('unterminated function ' + name);
}
function decl(name) {
  const m = src.match(new RegExp('^const ' + name + ' = .*;$', 'm'));
  if (!m) throw new Error('missing const ' + name);
  return m[0];
}
const prelude = [decl('WF_STOPS'), block('colormap')].join(String.fromCharCode(10));
const cm = new Function(prelude + String.fromCharCode(10) + 'return colormap;')();
const px = new Uint8ClampedArray(3);
const out = [];
for (let n = 0; n < 256; n++) {
  const c = cm(n / 255);
  px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
  out.push(px[0] + ' ' + px[1] + ' ' + px[2]);
}
console.log(out.join(String.fromCharCode(10)));
)NODE";
    const fs::path driver = dir / "wf_driver.js";
    CHECK(writeFile(driver, kDriver));

    std::string output;
    const int rc = runCaptured(
        "node \"" + driver.string() + "\" \"" + jsPath.string() + "\"", dir / "wf.txt", output);
    if (rc != 0) {
        std::printf("waterfall driver failed:\n%s\n", output.c_str());
    }
    CHECK(rc == 0);

    struct Rgb {
        int r = 0;
        int g = 0;
        int b = 0;
    };
    std::vector<Rgb> ramp;
    {
        std::istringstream in(output);
        Rgb c;
        while (in >> c.r >> c.g >> c.b) {
            ramp.push_back(c);
        }
    }
    // The size is checked FIRST and every index below is inside a block that
    // the size has already satisfied - never an assertion guarded by a size
    // CHECK that would then read out of bounds in exactly the failing run.
    CHECK(ramp.size() == 256u);
    if (ramp.size() != 256u) {
        return;
    }

    // The desktop's own anchors, from gui/waterfall_view.hpp's documented
    // contract: quiet phosphor at the floor, cream at full scale. The floor is
    // deliberately NOT black - a signal a few dB above the noise has to stay
    // visible against the panel behind it.
    CHECK(ramp.front().r == 6 && ramp.front().g == 20 && ramp.front().b == 10);
    CHECK(ramp.back().r == 240 && ramp.back().g == 235 && ramp.back().b == 180);

    // BT.601 luma, the same measure the desktop's test uses, across every one
    // of the 256 levels the page can actually emit.
    const auto luma = [](const Rgb& c) {
        return 0.299 * static_cast<double>(c.r) + 0.587 * static_cast<double>(c.g) +
               0.114 * static_cast<double>(c.b);
    };
    int dips = 0;
    for (std::size_t i = 1; i < ramp.size(); ++i) {
        if (luma(ramp[i]) < luma(ramp[i - 1]) - 1e-9) {
            if (dips < 4) {
                std::printf("waterfall ramp darkens at %zu: %.2f -> %.2f\n", i,
                            luma(ramp[i - 1]), luma(ramp[i]));
            }
            ++dips;
        }
    }
    CHECK(dips == 0);
    // And it must actually SPAN a range: a table that never darkens but also
    // never brightens would pass the check above and encode nothing.
    CHECK(luma(ramp.back()) - luma(ramp.front()) > 150.0);
}

}  // namespace

int main() {
    testLoopbackWithoutPasswordServesOpenly();
    testPasswordGateOnLoopback();
    testLoginThrottleOverHttp();
    testSessionExpiresOnTheInjectedClock();
    testOffMachineBindWithoutPasswordNeverListens();
    testSpectrumEndpoint();
    testInvalidUtf8FromAPluginDoesNotKillTheApi();
    testStaleCursorFromAPreviousRunRecovers();
    testRestartRevokesSessions();
    testControlQueue();
    testControlRequiresAuth();
    testControlQueueIsBounded();
    testTileEndpoint();
    testTileRequiresAuth();
    testAudioStreamsRealSamples();
    testAudioRequiresAuth();
    testAudioListenerCap();
    testServedScriptParses();
    testMapPanIsBoundToPointerEvents();
    testTrackFadeMatchesTheDesktopRule();
    testServedWaterfallRampIsMonotone();
    return testSummary("test_web_server");
}
