// Tests for the web page's owner/listener roles (net/web_users.hpp wired into net/web_server.cpp) and, on Windows, the page served over HTTPS
// (WebServerConfig::tlsEnabled) - feat/web-secure. Driven over real sockets against the real WebServer with a stub engine session.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "net/web_auth.hpp"
#include "net/web_server.hpp"
#include "net/web_session.hpp"
#include "test_check.hpp"
#ifdef _WIN32
#include "web_tls_client.hpp"
#endif

using namespace cascade::net;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

namespace {

std::string g_root;
std::string g_scratch;

constexpr char kOwnerPassword[] = "owner-long-password";
constexpr char kListenerPassword[] = "listener-long-password";

std::string recordOf(const std::string& pw) {
    PasswordRecord rec;
    std::string error;
    return hashPassword(pw, rec, error) ? rec.serialize() : std::string();
}

// One derivation each, made once: PBKDF2 is the slow part of every sign-in here.
const std::string& ownerRecord() { static const std::string r = recordOf(kOwnerPassword); return r; }
const std::string& listenerRecord() { static const std::string r = recordOf(kListenerPassword); return r; }

WebServerConfig roomConfig(std::size_t cap = 4) {
    WebServerConfig c;
    c.enabled = true;
    c.bindAddress = "127.0.0.1";
    c.username = "admin";
    c.passwordRecord = ownerRecord();
    c.listenersEnabled = true;
    c.listenerPasswordRecord = listenerRecord();
    c.maxListeners = static_cast<int>(cap);
    return c;
}

int startOnFreePort(WebServer& server, WebServerConfig cfg, std::string& error) {
    for (int port = 18300; port < 18400; ++port) {
        cfg.port = port;
        if (server.start(cfg, error)) { return port; }
        if (!server.decision().allowed()) { return -1; }
    }
    return -1;
}

// The stand-in for the engine session behind the page.
struct StubState {
    std::atomic<int> heartbeats{0};
    std::atomic<int> submits{0};
    std::atomic<int> commands{0};
};

class StubSession final : public WebSession {
public:
    explicit StubSession(std::shared_ptr<StubState> s) : s_(std::move(s)) {}
    void heartbeat() override { ++s_->heartbeats; }
    bool context(bool, bool, WebControlContext& out) override {
        out.bookmarkIdByRow = {1, 2, 3};
        out.scanStartHz = 1e6;
        out.scanStopHz = 2e6;
        out.scanStepHz = 1e3;
        return true;
    }
    int submit(const std::vector<cascade::core::cmd::QueuedCommand>& c) override {
        ++s_->submits;
        s_->commands += static_cast<int>(c.size());
        return static_cast<int>(c.size());
    }
    int pollResults(std::vector<FoxCommandResult>*) override { return 0; }
    bool readSpectrum(SpectrumSnapshot&) override { return false; }
    int readAudio(std::uint64_t* cursor, float* out, std::size_t cap, std::uint64_t*) override {
        const auto now = Clock::now();
        if (*cursor == 0) { *cursor = 1; last_ = now; return 0; }
        if (now - last_ < std::chrono::milliseconds(10)) { return 0; }
        last_ = now;
        const std::size_t n = std::min<std::size_t>(cap, 480);
        for (std::size_t i = 0; i < n; ++i) { out[i] = 0.5f; }
        return static_cast<int>(n);
    }

private:
    std::shared_ptr<StubState> s_;
    Clock::time_point last_{};
};

WebSessionOpener opener(std::shared_ptr<StubState> st) {
    return [st](const std::string&) -> std::unique_ptr<WebSession> { return std::make_unique<StubSession>(st); };
}

RadioStatus sensitiveStatus() {
    RadioStatus s;
    s.running = true;
    s.centerHz = 100.1e6;
    s.mode = "WFM";
    s.sourceKind = "rtlsdr";
    s.sourceName = "Generic RTL2832U OEM :: SERIAL-0042";
    s.soapyArgs = "driver=rtlsdr,serial=SERIAL-0042";
    s.rxPositionSet = true;
    s.rxLatDeg = 51.5007;
    s.rxLonDeg = -0.1246;
    s.recordDir = "C:\\Users\\someone\\Recordings";
    s.recordError = "cannot write C:\\Users\\someone\\Recordings\\x.iq";
    s.sourceError = "device SERIAL-0042 failed";
    RadioStatus::SoapyDevice d;
    d.label = "Generic RTL2832U OEM :: SERIAL-0042";
    d.args = "driver=rtlsdr,serial=SERIAL-0042";
    s.devices.push_back(d);
    RadioStatus::Plugin p;
    p.name = "ADS-B";
    p.fileName = "C:\\plugins\\adsb.dll";
    p.loaded = true;
    s.plugins.push_back(p);
    s.installError = "install of C:\\x failed";
    s.audioMutedBy = "";
    RadioStatus::DecodedLine l;
    l.plugin = "ADS-B";
    l.text = "DECODED-TEXT";
    s.decoded.push_back(l);
    return s;
}

struct Reply {
    int status = 0;
    std::string body;
    std::string setCookie;
    std::string retryAfter;
    json j;
};

Reply fromResult(const httplib::Result& r) {
    Reply o;
    if (!r) { return o; }
    o.status = r->status;
    o.body = r->body;
    o.setCookie = r->get_header_value("Set-Cookie");
    o.retryAfter = r->get_header_value("Retry-After");
    o.j = json::parse(r->body, nullptr, false);
    return o;
}

std::string cookieToken(const std::string& setCookie) {
    const std::string prefix = "foxsdr_session=";
    if (setCookie.rfind(prefix, 0) != 0) { return std::string(); }
    const std::size_t end = setCookie.find(';');
    return setCookie.substr(prefix.size(), (end == std::string::npos ? setCookie.size() : end) - prefix.size());
}

struct Browser {
    explicit Browser(int port) : cli("127.0.0.1", port) {
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(15, 0);
    }
    httplib::Client cli;
    std::string token;
    httplib::Headers headers(const httplib::Headers& extra = {}) const {
        httplib::Headers h = extra;
        if (!token.empty()) { h.emplace("Cookie", "foxsdr_session=" + token); }
        return h;
    }
    Reply login(const std::string& user, const std::string& pw, const httplib::Headers& extra = {}) {
        json body;
        body["username"] = user;
        body["password"] = pw;
        const Reply r = fromResult(cli.Post("/api/login", extra, body.dump(), "application/json"));
        if (r.status == 200) { token = cookieToken(r.setCookie); }
        return r;
    }
    Reply get(const std::string& path, const httplib::Headers& extra = {}) { return fromResult(cli.Get(path, headers(extra))); }
    Reply post(const std::string& path, const std::string& body, const httplib::Headers& extra = {}) {
        return fromResult(cli.Post(path, headers(extra), body, "application/json"));
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------

void testSignIn() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    std::string error;
    const int port = startOnFreePort(server, roomConfig(2), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    CHECK(server.decision().authRequired);

    Browser owner(port), bob(port), anon(port);
    CHECK(anon.get("/api/status").status == 401);

    const Reply o = owner.login("admin", kOwnerPassword);
    CHECK(o.status == 200 && o.j.value("role", "") == "owner");
    const Reply b = bob.login("Bob", kListenerPassword);
    CHECK(b.status == 200 && b.j.value("role", "") == "listener");
    CHECK(b.setCookie.find("HttpOnly") != std::string::npos && b.setCookie.find("SameSite=Strict") != std::string::npos);
    CHECK(b.setCookie.find("Secure") == std::string::npos);   // plain HTTP: a Secure cookie would never come back
    CHECK(server.listenerCount() == 1);

    // /api/session says which role the cookie holds.
    const Reply so = owner.get("/api/session");
    CHECK(so.j.value("role", "") == "owner" && so.j.value("authenticated", false));
    const Reply sb = bob.get("/api/session");
    CHECK(sb.j.value("role", "") == "listener" && sb.j.value("name", "") == "Bob");
    CHECK(sb.j.value("listenersEnabled", false));
    const Reply sa = anon.get("/api/session");
    CHECK(sa.j.value("role", "") == "none" && !sa.j.value("authenticated", true));

    // One message for every wrong credential, so they cannot be told apart: wrong owner password, wrong listener password, unknown name
    // with the owner's password, the owner's name with the listener password.
    Browser x(port);
    const Reply w1 = x.login("admin", "not-the-password-1");
    const Reply w2 = x.login("Carol", "not-the-password-2");
    const Reply w3 = x.login("Carol", kOwnerPassword);
    const Reply w4 = x.login("admin", kListenerPassword);
    for (const Reply* w : {&w1, &w2, &w3, &w4}) { CHECK(w->status == 401 && w->body == w1.body); }

    // The name rules.
    CHECK(x.login("ADMIN", kListenerPassword).status == 400);     // the owner's account name, in another case
    CHECK(x.login("a<b>", kListenerPassword).status == 400);
    CHECK(x.login("", kListenerPassword).status == 400);
    CHECK(x.login(std::string(25, 'x'), kListenerPassword).status == 400);
    CHECK(x.login("bob", kListenerPassword).status == 409);        // taken (any case)
    CHECK(x.login("Dave", kListenerPassword).status == 200);
    CHECK(x.login("Erin", kListenerPassword).status == 503);       // the cap is 2
    CHECK(server.listenerCount() == 2);

    // Sign-out frees the seat and ends the session.
    CHECK(bob.post("/api/logout", "{}").status == 200);
    CHECK(bob.get("/api/status").status == 401);
    CHECK(server.listenerCount() == 1);

    // The sign-in limiter counts failures per client: five wrong answers and the sixth is 429, listener door included.
    Browser y(port);
    for (int i = 0; i < 5; ++i) { CHECK(y.login("Frank", "wrong-wrong-wrong").status == 401); }
    CHECK(y.login("Frank", kListenerPassword).status == 429);
    CHECK(!y.login("Frank", kListenerPassword).retryAfter.empty());
    server.stop();
    CHECK(server.listenerCount() == 0);
}

void testListenersOffByDefault() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    WebServerConfig c = roomConfig();
    c.listenersEnabled = false;
    c.listenerPasswordRecord.clear();
    std::string error;
    const int port = startOnFreePort(server, c, error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser b(port);
    CHECK(b.login("Bob", kListenerPassword).status == 401);
    CHECK(b.login("Bob", kOwnerPassword).status == 401);
    CHECK(b.get("/api/session").j.value("listenersEnabled", true) == false);
    Browser o(port);
    CHECK(o.login("admin", kOwnerPassword).status == 200);
    // The owner's page has no room features switched on: chat and admin answer sensibly.
    CHECK(o.post("/api/chat", "{\"text\":\"hi\"}").status == 403);
    CHECK(o.post("/api/admin", "{\"op\":\"note\",\"text\":\"x\"}").status == 400);
    CHECK(o.get("/api/room").j.value("listenersEnabled", true) == false);
}

void testConcurrentJoinsAtTheCap() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    std::string error;
    const int port = startOnFreePort(server, roomConfig(4), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    std::atomic<int> ok{0}, full{0}, other{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 12; ++i) {
        ts.emplace_back([&, i] {
            Browser b(port);
            const Reply r = b.login("user" + std::to_string(i), kListenerPassword);
            if (r.status == 200) { ++ok; } else if (r.status == 503) { ++full; } else { ++other; }
        });
    }
    for (std::thread& t : ts) { t.join(); }
    CHECK(ok == 4);
    CHECK(full == 8);
    CHECK(other == 0);
    CHECK(server.listenerCount() == 4);
    // The owner can still sign in to a full room.
    Browser o(port);
    CHECK(o.login("admin", kOwnerPassword).status == 200);
    const Reply st = o.get("/api/admin/state");
    CHECK(st.status == 200 && st.j["listeners"].size() == 4);
    CHECK(o.get("/api/room").j.value("listeners", 0) == 4);
}

// Every key the control API knows, with a value that parses.
const std::map<std::string, std::string>& everyOp() {
    static const std::map<std::string, std::string> m = {
        {"running", "{\"running\":true}"},
        {"centerHz", "{\"centerHz\":100000000}"},
        {"vfoOffsetHz", "{\"vfoOffsetHz\":1000}"},
        {"mode", "{\"mode\":\"NFM\"}"},
        {"bandwidthHz", "{\"bandwidthHz\":12500}"},
        {"squelchDb", "{\"squelchDb\":-50}"},
        {"volume", "{\"volume\":0.5}"},
        {"muted", "{\"muted\":true}"},
        {"dbMin", "{\"dbMin\":-100}"},
        {"dbMax", "{\"dbMax\":0}"},
        {"deemphasisIndex", "{\"deemphasisIndex\":1}"},
        {"nrEnabled", "{\"nrEnabled\":true}"},
        {"nrStrength", "{\"nrStrength\":0.5}"},
        {"notchEnabled", "{\"notchEnabled\":true}"},
        {"notchFreqHz", "{\"notchFreqHz\":1000}"},
        {"notchQ", "{\"notchQ\":30}"},
        {"autoNotch", "{\"autoNotch\":true}"},
        {"stereoEnabled", "{\"stereoEnabled\":true}"},
        {"sourceKind", "{\"sourceKind\":\"siggen\"}"},
        {"soapyArgs", "{\"soapyArgs\":\"driver=rtlsdr\"}"},
        {"antenna", "{\"antenna\":\"RX\"}"},
        {"sampleRateHz", "{\"sampleRateHz\":2000000}"},
        {"gainName", "{\"gainName\":\"LNA\",\"gainDb\":10}"},
        {"gainDb", "{\"gainName\":\"LNA\",\"gainDb\":10}"},
        {"agc", "{\"agc\":true}"},
        {"scanDevices", "{\"scanDevices\":true}"},
        {"recordIq", "{\"recordIq\":true}"},
        {"recordAudio", "{\"recordAudio\":true}"},
        {"bookmarkAdd", "{\"bookmarkAdd\":\"x\"}"},
        {"bookmarkTune", "{\"bookmarkTune\":0}"},
        {"bookmarkRemove", "{\"bookmarkRemove\":0}"},
        {"scannerActive", "{\"scannerActive\":true}"},
        {"scannerSkip", "{\"scannerSkip\":true}"},
        {"scanStartHz", "{\"scanStartHz\":1000000}"},
        {"scanStopHz", "{\"scanStopHz\":2000000}"},
        {"scanStepHz", "{\"scanStepHz\":1000}"},
        {"pluginFetch", "{\"pluginFetch\":true}"},
        {"pluginInstall", "{\"pluginInstall\":\"id\"}"},
        {"acknowledgeNotice", "{\"pluginInstall\":\"id\",\"acknowledgeNotice\":true}"},
        {"pluginRemove", "{\"pluginRemove\":\"x.dll\"}"},
        {"pluginTuneName", "{\"pluginTuneName\":\"p\",\"pluginTuneAllowed\":true}"},
        {"pluginTuneAllowed", "{\"pluginTuneName\":\"p\",\"pluginTuneAllowed\":true}"},
        {"pluginPresetName", "{\"pluginPresetName\":\"p\",\"pluginPresetIndex\":0}"},
        {"pluginPresetIndex", "{\"pluginPresetName\":\"p\",\"pluginPresetIndex\":0}"},
        {"transmitPtt", "{\"transmitPtt\":true}"},
    };
    return m;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testTheOpListIsComplete() {
    // The list above is the API surface, held to the parser's own: a key added to the control API without a line here goes red.
    const std::string src = slurp(g_root + "/src/net/web_control.cpp");
    const std::size_t a = src.find("kKnownKeys[] = {");
    const std::size_t b = src.find("};", a);
    CHECK(a != std::string::npos && b != std::string::npos);
    int found = 0;
    const std::regex quoted("\"([A-Za-z]+)\"");
    for (std::sregex_iterator it(src.begin() + static_cast<std::ptrdiff_t>(a), src.begin() + static_cast<std::ptrdiff_t>(b), quoted), end;
         it != end; ++it) {
        ++found;
        const bool known = everyOp().count((*it)[1].str()) == 1;
        if (!known) { std::printf("control key without a listener test: %s\n", (*it)[1].str().c_str()); }
        CHECK(known);
    }
    CHECK(found >= 44);
}

void testEveryOpAsAListener() {
    for (const bool mayTune : {false, true}) {
        WebServer server;
        server.setStatusProvider([] { return sensitiveStatus(); });
        auto st = std::make_shared<StubState>();
        CHECK(server.setSessionOpener(opener(st)));
        WebServerConfig c = roomConfig();
        c.listenersMayTune = mayTune;
        std::string error;
        const int port = startOnFreePort(server, c, error);
        CHECK(port > 0);
        if (port <= 0) { return; }
        Browser bob(port), owner(port);
        CHECK(bob.login("Bob", kListenerPassword).status == 200);
        CHECK(owner.login("admin", kOwnerPassword).status == 200);
        const std::set<std::string> tuning = {"centerHz", "vfoOffsetHz", "mode", "bandwidthHz", "bookmarkTune"};
        int refused = 0, accepted = 0;
        for (const auto& op : everyOp()) {
            const int before = st->submits.load();
            const Reply r = bob.post("/api/control", op.second);
            const bool isTuning = tuning.count(op.first) == 1;
            if (mayTune && isTuning) {
                CHECK(r.status == 202);
                CHECK(st->submits.load() == before + 1);
                ++accepted;
            } else {
                if (r.status != 403) { std::printf("listener op %s (tune=%d) answered %d\n", op.first.c_str(), mayTune ? 1 : 0, r.status); }
                CHECK(r.status == 403);
                CHECK(st->submits.load() == before);   // nothing reached the engine
                ++refused;
            }
        }
        CHECK(accepted == (mayTune ? 5 : 0));
        CHECK(refused == static_cast<int>(everyOp().size()) - accepted);
        // Riding a forbidden field along with a permitted one changes nothing, and neither does garbage.
        CHECK(bob.post("/api/control", "{\"centerHz\":100000000,\"volume\":1}").status == 403);
        CHECK(bob.post("/api/control", "{\"centerHz\":100000000,\"transmitPtt\":true}").status == 403);
        CHECK(bob.post("/api/control", "{\"transmitLatch\":true}").status == (mayTune ? 400 : 403));
        CHECK(bob.post("/api/control", "not json").status == (mayTune ? 400 : 403));
        // The same bodies from the owner are accepted (the owner is not diminished by any of this).
        const int before = st->submits.load();
        CHECK(owner.post("/api/control", everyOp().at("volume")).status == 202);
        CHECK(owner.post("/api/control", everyOp().at("centerHz")).status == 202);
        CHECK(st->submits.load() == before + 2);
        // Each refusal is in the audit trail, with the listener's name and nothing else.
        const Reply state = owner.get("/api/admin/state");
        int refusedOps = 0;
        for (const json& e : state.j["audit"]) {
            if (e.value("kind", "") == "refused-op") {
                ++refusedOps;
                CHECK(e.value("name", "") == "Bob");
                CHECK(e.size() == 4);   // seq, t, kind, name
            }
        }
        CHECK(refusedOps >= refused);
        server.stop();
    }
}

void testCraftedEscalationRefused() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    auto st = std::make_shared<StubState>();
    CHECK(server.setSessionOpener(opener(st)));
    std::string error;
    const int port = startOnFreePort(server, roomConfig(), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser owner(port), bob(port), carol(port);
    CHECK(owner.login("admin", kOwnerPassword).status == 200);
    CHECK(bob.login("Bob", kListenerPassword).status == 200);
    CHECK(carol.login("Carol", kListenerPassword).status == 200);

    // (a) The owner's routes, with a listener's cookie and every header and field a client might use to claim more.
    const httplib::Headers lies = {{"X-Role", "owner"}, {"X-Forwarded-For", "127.0.0.1"}, {"X-User", "admin"}, {"X-Foxsdr-Role", "owner"},
                                   {"Authorization", "Bearer owner"}, {"Forwarded", "for=127.0.0.1"}};
    CHECK(bob.get("/api/admin/state", lies).status == 403);
    CHECK(bob.get("/api/admin/state?role=owner&owner=1", lies).status == 403);
    for (const char* body : {"{\"op\":\"kick\",\"name\":\"Carol\"}", "{\"op\":\"ban\",\"name\":\"Carol\"}", "{\"op\":\"note\",\"text\":\"x\"}",
                              "{\"op\":\"kick\",\"name\":\"Carol\",\"role\":\"owner\"}", "{\"op\":\"banAddress\",\"address\":\"127.0.0.1\"}",
                              "{\"op\":\"unban\",\"name\":\"Carol\"}"}) {
        CHECK(bob.post("/api/admin", body, lies).status == 403);
    }
    CHECK(server.listenerCount() == 2);   // Carol is still here
    CHECK(carol.get("/api/status").status == 200);
    // (b) Forged and replayed cookies.
    const std::string ownerTok = owner.token;
    const std::string bobTok = bob.token;
    Browser f(port);
    for (const std::string& tok : {ownerTok.substr(0, ownerTok.size() - 1), ownerTok + "A", std::string("admin"), std::string("owner"), bobTok + bobTok,
                                   std::string(""), std::string("Bob"), std::string(300, 'A')}) {
        f.token = tok;
        CHECK(f.get("/api/admin/state").status == 401);
        CHECK(f.get("/api/status").status == 401);
    }
    // Two cookies of the same name: the first one counts, and a listener cannot smuggle the owner's value behind its own.
    {
        httplib::Headers h = {{"Cookie", "foxsdr_session=" + bobTok + "; foxsdr_session=" + ownerTok}};
        CHECK(fromResult(bob.cli.Get("/api/admin/state", h)).status == 403);
        httplib::Headers h2 = {{"Cookie", "x=1; foxsdr_session=" + bobTok}};
        CHECK(fromResult(bob.cli.Get("/api/session", h2)).j.value("role", "") == "listener");
    }
    // (c) Sign-in escalation: the owner's name with the listener password, the owner's name in other cases, extra fields in the login body.
    Browser e(port);
    CHECK(e.login("admin", kListenerPassword).status == 401);
    CHECK(e.login("Admin", kListenerPassword).status == 400);
    CHECK(fromResult(e.cli.Post("/api/login", "{\"username\":\"Eve\",\"password\":\"" + std::string(kListenerPassword) + "\",\"role\":\"owner\",\"owner\":true}",
                                "application/json"))
              .j.value("role", "") == "listener");
    // (d) A listener's session, however it is presented, is never the owner's heartbeat.
    const int beats = st->heartbeats.load();
    for (int i = 0; i < 10; ++i) { CHECK(bob.get("/api/status", lies).status == 200); }
    CHECK(st->heartbeats.load() == beats);
    CHECK(owner.get("/api/status").status == 200);
    CHECK(st->heartbeats.load() == beats + 1);
    // (e) The owner's page polling is unaffected by listeners being on.
    // (f) Chat identity cannot be borrowed: a listener's line is signed with its own name whatever the body says.
    server.stop();
}

void testStatusIsRedactedForListeners() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    std::string error;
    const int port = startOnFreePort(server, roomConfig(), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser owner(port), bob(port);
    CHECK(owner.login("admin", kOwnerPassword).status == 200);
    CHECK(bob.login("Bob", kListenerPassword).status == 200);
    const Reply o = owner.get("/api/status");
    const Reply l = bob.get("/api/status");
    CHECK(o.status == 200 && l.status == 200);
    // The owner sees everything, as before.
    CHECK(o.j.value("rxPositionSet", false) && o.j.value("recordDir", "") != "");
    CHECK(o.body.find("SERIAL-0042") != std::string::npos);
    // The listener sees the signal and not the machine.
    CHECK(l.j.value("centerHz", 0.0) == 100.1e6);
    CHECK(l.j.value("mode", "") == "WFM");
    CHECK(l.body.find("DECODED-TEXT") != std::string::npos);
    for (const char* secret : {"SERIAL-0042", "Recordings", "someone", "51.5007", "0.1246", "adsb.dll", "C:\\\\"}) {
        const bool leaked = l.body.find(secret) != std::string::npos;
        if (leaked) { std::printf("a listener's status contains %s\n", secret); }
        CHECK(!leaked);
    }
    CHECK(!l.j.value("rxPositionSet", true));
    CHECK(l.j.value("recordDir", "x").empty());
    CHECK(l.j["devices"].empty());
    // The live-update stream is redacted the same way.
    std::string stream;
    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_read_timeout(10, 0);
        (void)cli.Get("/api/events", bob.headers(), [&](const char* d, std::size_t n) {
            stream.append(d, n);
            return stream.find("\n\n", stream.find("data:")) == std::string::npos;
        });
    }
    CHECK(stream.find("event: status") != std::string::npos);
    CHECK(stream.find("DECODED-TEXT") != std::string::npos);
    for (const char* secret : {"SERIAL-0042", "Recordings", "51.5007", "adsb.dll"}) { CHECK(stream.find(secret) == std::string::npos); }
}

void testModerationEndsLiveSessionsAndStreams() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    std::string error;
    const int port = startOnFreePort(server, roomConfig(6), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser owner(port);
    CHECK(owner.login("admin", kOwnerPassword).status == 200);

    // A listener with an audio stream and an events stream open.
    const auto openStreams = [&](Browser& b, std::atomic<bool>& audioEnded, std::atomic<bool>& eventsEnded, std::atomic<int>& audioBytes,
                                 std::vector<std::thread>& ts) {
        ts.emplace_back([&, port] {
            httplib::Client cli("127.0.0.1", port);
            cli.set_read_timeout(20, 0);
            (void)cli.Get("/api/audio", b.headers(), [&](const char*, std::size_t n) { audioBytes += static_cast<int>(n); return true; });
            audioEnded = true;
        });
        ts.emplace_back([&, port] {
            httplib::Client cli("127.0.0.1", port);
            cli.set_read_timeout(20, 0);
            (void)cli.Get("/api/events", b.headers(), [&](const char*, std::size_t) { return true; });
            eventsEnded = true;
        });
    };
    const auto waitFor = [](const std::function<bool()>& f, int ms) {
        const auto t0 = Clock::now();
        while (!f() && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return f();
    };

    for (const char* how : {"kick", "ban", "banAddress"}) {
        Browser bob(port);
        CHECK(bob.login("Bob", kListenerPassword).status == 200);
        std::atomic<bool> audioEnded{false}, eventsEnded{false};
        std::atomic<int> audioBytes{0};
        std::vector<std::thread> ts;
        openStreams(bob, audioEnded, eventsEnded, audioBytes, ts);
        CHECK(waitFor([&] { return audioBytes.load() > 1000; }, 5000));   // really listening
        CHECK(server.audioListeners() == 1);
        std::string body = std::string("{\"op\":\"") + how + "\",\"name\":\"Bob\",\"address\":\"127.0.0.1\"}";
        const auto t0 = Clock::now();
        const Reply r = owner.post("/api/admin", body);
        CHECK(r.status == 200);
        // BOTH streams end, and the page's own calls are refused, within about a second.
        CHECK(waitFor([&] { return audioEnded.load() && eventsEnded.load(); }, 4000));
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        std::printf("%s ended a listener's audio and live-update streams in %lld ms\n", how, ms);
        CHECK(ms < 6000);
        for (std::thread& t : ts) { t.join(); }
        CHECK(bob.get("/api/status").status == 401);
        CHECK(server.listenerCount() == 0);
        CHECK(server.audioListeners() == 0);
        CHECK(server.eventListeners() == 0);
        if (std::string(how) == "kick") {
            Browser again(port);
            CHECK(again.login("Bob", kListenerPassword).status == 200);   // a kick bans nothing
            CHECK(owner.post("/api/admin", "{\"op\":\"kick\",\"name\":\"Bob\"}").status == 200);
        } else if (std::string(how) == "ban") {
            Browser again(port);
            CHECK(again.login("Bob", kListenerPassword).status == 403);     // by name AND by the address it came from
            Browser other(port);
            CHECK(other.login("Zed", kListenerPassword).status == 403);     // (the address is this machine's, as for every client here)
            const Reply state = owner.get("/api/admin/state");
            CHECK(state.j["bannedNames"].size() == 1 && state.j["bannedAddresses"].size() == 1);
            // The owner can still sign in: a ban never locks the owner out.
            Browser o2(port);
            CHECK(o2.login("admin", kOwnerPassword).status == 200);
            CHECK(owner.post("/api/admin", "{\"op\":\"unbanAddress\",\"address\":\"127.0.0.1\"}").status == 200);
            Browser third(port);
            CHECK(third.login("Bob", kListenerPassword).status == 403);     // still banned by name
            CHECK(owner.post("/api/admin", "{\"op\":\"unban\",\"name\":\"Bob\"}").status == 200);
            Browser fourth(port);
            CHECK(fourth.login("Bob", kListenerPassword).status == 200);
            CHECK(owner.post("/api/admin", "{\"op\":\"kick\",\"name\":\"Bob\"}").status == 200);
        } else {
            Browser again(port);
            CHECK(again.login("Bob", kListenerPassword).status == 403);
            CHECK(owner.post("/api/admin", "{\"op\":\"unbanAddress\",\"address\":\"127.0.0.1\"}").status == 200);
            CHECK(owner.post("/api/admin", "{\"op\":\"nonsense\"}").status == 400);
            CHECK(owner.post("/api/admin", "{\"op\":\"kick\",\"name\":\"\"}").status == 400);
        }
    }
    // And the owner's own session survived all of it.
    CHECK(owner.get("/api/status").status == 200);
}

void testIdleAndSessionLimits() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    auto now = std::make_shared<std::atomic<std::int64_t>>(1000000);
    CHECK(server.setClock([now] { return now->load(); }));
    WebServerConfig c = roomConfig();
    c.listenerIdleSeconds = 60;
    c.listenerSessionSeconds = 600;
    std::string error;
    const int port = startOnFreePort(server, c, error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser owner(port), bob(port), carol(port);
    CHECK(owner.login("admin", kOwnerPassword).status == 200);
    CHECK(bob.login("Bob", kListenerPassword).status == 200);
    CHECK(carol.login("Carol", kListenerPassword).status == 200);
    for (int i = 0; i < 5; ++i) {   // Carol keeps asking, every 50 s: never idle; Bob is silent
        *now += 50;
        CHECK(carol.get("/api/status").status == 200);
    }
    CHECK(bob.get("/api/status").status == 401);   // 250 s of silence against a limit of 60
    CHECK(server.listenerCount() == 1);
    for (int i = 0; i < 8; ++i) {                  // and Carol meets the SESSION limit however busy she is
        *now += 50;
        carol.get("/api/status");
    }
    CHECK(carol.get("/api/status").status == 401);
    CHECK(server.listenerCount() == 0);
    CHECK(owner.get("/api/status").status == 200);   // the owner has neither limit (its cookie lives 12 hours)
}

void testChatNoteAndEscaping() {
    // Everything below happens with the process's standard error pointed at a file, which is searched at the end: no name, address or
    // typed text may reach it (the host copies that stream into its log and its reports).
    const std::string errPath = g_scratch + "/stderr.txt";
    std::FILE* keep = std::freopen(errPath.c_str(), "w", stderr);
    (void)keep;
    {
        WebServer server;
        server.setStatusProvider([] { return sensitiveStatus(); });
        CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
        auto now = std::make_shared<std::atomic<std::int64_t>>(2000000);
        CHECK(server.setClock([now] { return now->load(); }));
        WebServerConfig c = roomConfig();
        c.chatEnabled = true;
        std::string error;
        const int port = startOnFreePort(server, c, error);
        CHECK(port > 0);
        if (port > 0) {
            Browser owner(port), bob(port);
            CHECK(owner.login("admin", kOwnerPassword).status == 200);
            CHECK(bob.login("Bob", kListenerPassword).status == 200);
            CHECK(bob.get("/api/room").j.value("chatEnabled", false));
            const std::string attack = "<script>alert('SECRET-CHAT-TEXT')</script><img src=x onerror=alert(1)> & \\\"quoted\\\"";
            CHECK(bob.post("/api/chat", "{\"text\":\"" + attack + "\"}").status == 200);
            const Reply chat = owner.get("/api/chat?since=0");
            CHECK(chat.j.value("enabled", false));
            CHECK(chat.j["lines"].size() == 1);
            const std::string html = chat.j["lines"][0].value("html", "");
            CHECK(chat.j["lines"][0].value("name", "") == "Bob" && !chat.j["lines"][0].value("owner", true));
            CHECK(html.find('<') == std::string::npos && html.find('>') == std::string::npos && html.find('"') == std::string::npos);
            CHECK(html.find("&lt;script&gt;") == 0);
            // A listener cannot sign a line as the owner: the speaker is the session's, never the body's.
            CHECK(bob.post("/api/chat", "{\"text\":\"hello\",\"name\":\"owner\",\"owner\":true,\"role\":\"owner\"}").status == 200);
            const Reply chat2 = owner.get("/api/chat?since=1");
            CHECK(chat2.j["lines"].size() == 1 && chat2.j["lines"][0].value("name", "") == "Bob" && !chat2.j["lines"][0].value("owner", true));
            // Rate limit, size limit, bad text, empty text.
            CHECK(bob.post("/api/chat", "{\"text\":\"three\"}").status == 200);
            const Reply limited = bob.post("/api/chat", "{\"text\":\"four\"}");
            CHECK(limited.status == 429 && !limited.retryAfter.empty());
            *now += 20;
            CHECK(bob.post("/api/chat", "{\"text\":\"" + std::string(201, 'x') + "\"}").status == 413);
            CHECK(bob.post("/api/chat", "{\"text\":\"   \"}").status == 400);
            CHECK(bob.post("/api/chat", "not json").status == 400);
            CHECK(fromResult(bob.cli.Post("/api/chat", bob.headers(), "{\"text\":\"x\"}", "text/plain")).status == 415);
            CHECK(Browser(port).post("/api/chat", "{\"text\":\"x\"}").status == 401);   // not signed in
            // The owner's note reaches listeners, escaped.
            CHECK(owner.post("/api/admin", "{\"op\":\"note\",\"text\":\"Closing <b>soon</b> & SECRET-NOTE-TEXT\"}").status == 200);
            const Reply room = bob.get("/api/room");
            CHECK(room.j.value("note", "") == "Closing &lt;b&gt;soon&lt;/b&gt; &amp; SECRET-NOTE-TEXT");
            CHECK(room.j.value("noteSeq", 0) == 1);
            CHECK(owner.post("/api/admin", "{\"op\":\"note\",\"text\":\"\"}").status == 200);
            CHECK(bob.get("/api/room").j.value("note", "x").empty());
            CHECK(owner.post("/api/admin", "{\"op\":\"note\",\"text\":\"" + std::string(201, 'n') + "\"}").status == 413);
            // The audit trail has events, names and nothing a person typed or an address.
            const Reply state = owner.get("/api/admin/state");
            CHECK(state.body.find("SECRET") == std::string::npos);
            std::string auditText;
            for (const json& e : state.j["audit"]) { auditText += e.dump(); }
            CHECK(auditText.find("SECRET") == std::string::npos);
            CHECK(auditText.find("127.0.0.1") == std::string::npos);
            CHECK(auditText.find("chat") != std::string::npos);
            CHECK(auditText.find("note") != std::string::npos);
            server.stop();
        }
    }
    std::fflush(stderr);
    std::FILE* back = std::freopen("CON", "w", stderr);
    (void)back;
    const std::string err = slurp(errPath);
    CHECK(err.find("SECRET") == std::string::npos);
    CHECK(err.find("Bob") == std::string::npos);
    CHECK(err.find("127.0.0.1") == std::string::npos);
}

void testChatOffByDefault() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    std::string error;
    const int port = startOnFreePort(server, roomConfig(), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    Browser owner(port), bob(port);
    CHECK(owner.login("admin", kOwnerPassword).status == 200);
    CHECK(bob.login("Bob", kListenerPassword).status == 200);
    CHECK(bob.post("/api/chat", "{\"text\":\"hi\"}").status == 403);
    CHECK(owner.post("/api/chat", "{\"text\":\"hi\"}").status == 403);
    const Reply g = bob.get("/api/chat");
    CHECK(g.status == 200 && !g.j.value("enabled", true) && g.j["lines"].empty());
    CHECK(!bob.get("/api/room").j.value("chatEnabled", true));
}

void testPageCarriesTheRoom() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    std::string error;
    const int port = startOnFreePort(server, roomConfig(), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    httplib::Client cli("127.0.0.1", port);
    const auto html = cli.Get("/");
    const auto js = cli.Get("/app.js");
    CHECK(html && js);
    if (!html || !js) { return; }
    for (const char* id : {"id=\"room\"", "id=\"roleBadge\"", "id=\"chatForm\"", "id=\"noteForm\"", "id=\"listenerList\"", "id=\"loginHint\""}) {
        CHECK(html->body.find(id) != std::string::npos);
    }
    CHECK(html->get_header_value("Content-Security-Policy").find("unsafe-inline") == std::string::npos);
    CHECK(html->body.find("<script>") == std::string::npos);   // no inline script: the room code is in /app.js
    const std::size_t at = js->body.find("THE ROOM (feat/web-secure)");
    CHECK(at != std::string::npos);
    const std::string room = at == std::string::npos ? std::string() : js->body.substr(at);
    CHECK(room.find("function roomApply(s)") != std::string::npos);
    CHECK(room.find("'/api/room'") != std::string::npos && room.find("'/api/chat") != std::string::npos && room.find("'/api/admin") != std::string::npos);
    // Text a person typed goes onto the page as text; the only innerHTML in the room's script is the server's own escaped html, and says so.
    int uses = 0;
    for (std::size_t p = room.find("innerHTML"); p != std::string::npos; p = room.find("innerHTML", p + 1)) {
        ++uses;
        const std::size_t eol = room.find('\n', p);
        const std::string line = room.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
        CHECK(line.find("escaped by the server") != std::string::npos);
    }
    CHECK(uses == 2);
    CHECK(room.find("document.write") == std::string::npos && room.find("eval(") == std::string::npos);
}

void testPolicyForRoom() {
    WebServer server;
    std::string error;
    WebServerConfig c = roomConfig();
    c.listenerPasswordRecord.clear();
    CHECK(!server.start(c, error));
    CHECK(server.decision().verdict == BindVerdict::RefusedListenerPassword);
    c = roomConfig();
    c.passwordRecord.clear();
    CHECK(!server.start(c, error) && server.decision().verdict == BindVerdict::RefusedListenerPassword);
    c = roomConfig();
    c.maxListeners = 33;
    CHECK(!server.start(c, error) && server.decision().verdict == BindVerdict::RefusedListenerLimits);
    c.maxListeners = 0;
    CHECK(!server.start(c, error) && server.decision().verdict == BindVerdict::RefusedListenerLimits);
    c = roomConfig();
    c.listenerIdleSeconds = -1;
    CHECK(!server.start(c, error) && server.decision().verdict == BindVerdict::RefusedListenerLimits);
    c = roomConfig(32);
    c.port = 18399;
    CHECK(server.start(c, error));
    server.stop();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// HTTPS through the real WebServer (Windows)
// ---------------------------------------------------------------------------------------------------------------------------------------

#ifdef _WIN32

using web_test::HttpReply;
using web_test::SchannelClient;

struct SecureBrowser {
    explicit SecureBrowser(int port, const std::string& host = "127.0.0.1") : port_(port), host_(host) {}
    bool open(std::string& err) { return c_.connectTcp(host_, port_) && c_.handshake("localhost", 0, err); }
    HttpReply request(const std::string& m, const std::string& path, const std::string& extra = std::string(), const std::string& body = std::string()) {
        std::string h = extra;
        if (!token.empty()) { h += "Cookie: foxsdr_session=" + token + "\r\n"; }
        if (!body.empty()) { h += "Content-Type: application/json\r\n"; }
        return c_.request(m, path, h, body);
    }
    HttpReply login(const std::string& user, const std::string& pw, const std::string& extra = std::string()) {
        json b;
        b["username"] = user;
        b["password"] = pw;
        HttpReply r = request("POST", "/api/login", extra, b.dump());
        if (r.status == 200) {
            const std::string sc = r.header("Set-Cookie");
            token = cookieToken(sc);
        }
        return r;
    }
    SchannelClient& raw() { return c_; }
    std::string token;

private:
    int port_;
    std::string host_;
    SchannelClient c_;
};

void testServerOverHttps() {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    auto st = std::make_shared<StubState>();
    CHECK(server.setSessionOpener(opener(st)));
    WebServerConfig c = roomConfig();
    c.tlsEnabled = true;
    std::string error;
    const int port = startOnFreePort(server, c, error);
    if (port <= 0) { std::printf("start with HTTPS failed: %s\n", error.c_str()); }
    CHECK(port > 0);
    if (port <= 0) { return; }
    const WebServer::TlsInfo info = server.tlsInfo();
    CHECK(info.active && info.port == port && info.privatePort > 0 && info.privatePort != port);
    CHECK(info.fingerprintSha256.size() == 64 && info.thumbprintSha1.size() == 40);
    CHECK(info.selfSigned && !info.keyExportable);
    CHECK(server.boundPort() == port);

    // The certificate the browser is given is the one whose fingerprint is published.
    std::string err;
    {
        SecureBrowser looker(port);
        CHECK(looker.open(err));
        CHECK(looker.raw().peerFingerprint() == info.fingerprintSha256);
    }

    // PLAIN HTTP IS NOT OFFERED on the public port: the bytes of a plain request get no HTTP answer.
    {
        SchannelClient p;
        CHECK(p.connectTcp("127.0.0.1", port));
        p.sendRawBytes("GET /api/session HTTP/1.1\r\nHost: x\r\n\r\n");
        const std::string reply = p.rawReceive(1500);
        CHECK(reply.find("HTTP/") == std::string::npos);
    }
    // The plain server is on this machine's loopback only: a program here can reach it, nothing off the machine can.
    {
        httplib::Client local("127.0.0.1", info.privatePort);
        local.set_connection_timeout(3, 0);
        const auto r = local.Get("/api/session");
        CHECK(static_cast<bool>(r) && r->status == 200);
        for (const std::string& lan : localInterfaceAddresses()) {
            SchannelClient p;
            const bool reached = p.connectTcp(lan, info.privatePort, 1500);
            if (reached) { std::printf("the private port answered on %s\n", lan.c_str()); }
            CHECK(!reached);
        }
    }

    // Sign in over HTTPS: the cookie is Secure, the session works, and wrong answers are the same wrong answers.
    // (The connection is opened only now: the plain server drops a connection that sits without a request for a few seconds, and the
    // checks above took longer than that.)
    SecureBrowser owner(port);
    CHECK(owner.open(err));
    const HttpReply bad = owner.login("admin", "wrong-wrong-wrong");
    CHECK(bad.status == 401);
    const HttpReply good = owner.login("admin", kOwnerPassword);
    CHECK(good.status == 200);
    const std::string sc = good.header("Set-Cookie");
    CHECK(sc.find("; Secure") != std::string::npos && sc.find("HttpOnly") != std::string::npos && sc.find("SameSite=Strict") != std::string::npos);
    CHECK(owner.request("GET", "/api/status").status == 200);
    CHECK(owner.request("GET", "/api/session").status == 200);
    {
        SecureBrowser anon(port);
        CHECK(anon.open(err));
        CHECK(anon.request("GET", "/api/status").status == 401);
    }
    const HttpReply out = owner.request("POST", "/api/logout", std::string(), "{}");
    CHECK(out.status == 200 && out.header("Set-Cookie").find("; Secure") != std::string::npos);

    // Roles over HTTPS: a listener, a refused op, the owner's admin.
    SecureBrowser owner2(port), bob(port);
    CHECK(owner2.open(err) && bob.open(err));
    CHECK(owner2.login("admin", kOwnerPassword).status == 200);
    CHECK(bob.login("Bob", kListenerPassword).status == 200);
    CHECK(bob.request("POST", "/api/control", std::string(), "{\"volume\":0.5}").status == 403);
    CHECK(owner2.request("POST", "/api/control", std::string(), "{\"volume\":0.5}").status == 202);
    CHECK(server.listenerCount() == 1);

    // THE LIVE EVENT STREAM through TLS: events arrive, in the SSE framing, redacted for the listener.
    {
        SecureBrowser s(port);
        CHECK(s.open(err));
        s.token = bob.token;
        std::string head;
        const std::string got = s.raw().stream("/api/events", 200, "Cookie: foxsdr_session=" + s.token + "\r\n", 10000, &head);
        CHECK(head.find("text/event-stream") != std::string::npos);
        CHECK(got.find("retry: 3000") != std::string::npos);
        CHECK(got.find("event: status") != std::string::npos);
        CHECK(got.find("SERIAL-0042") == std::string::npos);
    }
    // THE AUDIO STREAM through TLS: 16-bit samples of 0.5, which is 16384, byte for byte.
    {
        SecureBrowser s(port);
        CHECK(s.open(err));
        std::string head;
        const std::string pcm = s.raw().stream("/api/audio", 4000, "Cookie: foxsdr_session=" + bob.token + "\r\n", 15000, &head);
        CHECK(head.find("audio/L16") != std::string::npos);
        CHECK(pcm.size() >= 4000);
        int good = 0;
        for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
            const int v = static_cast<std::int16_t>(static_cast<std::uint8_t>(pcm[i]) | (static_cast<std::uint8_t>(pcm[i + 1]) << 8));
            if (v == 16384) { ++good; }
        }
        CHECK(good >= 1900);
        CHECK(server.audioListeners() <= 1);
    }

    // SHUTDOWN BUDGET: stop() with idle HTTPS connections, a half-open one and a live stream, against the product's limit (kClientIoTimeout).
    std::vector<std::unique_ptr<SecureBrowser>> idle;
    for (int i = 0; i < 3; ++i) {
        idle.push_back(std::make_unique<SecureBrowser>(port));
        CHECK(idle.back()->open(err));
    }
    SchannelClient half;
    CHECK(half.connectTcp("127.0.0.1", port));
    SecureBrowser streamer(port);
    CHECK(streamer.open(err));
    std::thread reader([&] { (void)streamer.raw().stream("/api/audio", 4000000, "Cookie: foxsdr_session=" + bob.token + "\r\n", 12000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto t0 = Clock::now();
    server.stop();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("WebServer::stop() with HTTPS on, 3 idle + 1 half-open + 1 audio stream: %lld ms (budget %lld ms)\n", ms,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kClientIoTimeout).count()));
    CHECK(ms < 3000);
    reader.join();
    CHECK(!server.running());
    CHECK(!server.tlsInfo().active);
    CHECK(server.listenerCount() == 0);
    // The ports are free again: start, stop, start.
    for (int i = 0; i < 3; ++i) {
        std::string e2;
        CHECK(server.start([&] { WebServerConfig k = c; k.port = port; return k; }(), e2));
        CHECK(server.tlsInfo().active);
        server.stop();
        CHECK(!server.tlsInfo().active);
    }
}

void testThrottleAndBansUseTheTrueAddress() {
    const std::vector<std::string> lan = localInterfaceAddresses();
    if (lan.empty()) {
        SKIP_LINUX("this computer has no LAN IPv4 address to connect from");
        return;
    }
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    WebServerConfig c = roomConfig(6);
    c.tlsEnabled = true;
    c.bindAddress = "0.0.0.0";
    std::string error;
    const int port = startOnFreePort(server, c, error);
    if (port <= 0) { std::printf("start failed: %s\n", error.c_str()); }
    CHECK(port > 0);
    if (port <= 0) { return; }
    CHECK(server.decision().reachableOffMachine);
    std::string err;

    const std::string forged = "X-Forwarded-For: 127.0.0.1\r\nX-Real-IP: 127.0.0.1\r\nForwarded: for=127.0.0.1\r\n";
    // (1) THE BAN LIST, by the address the connection really came from.
    {
        SecureBrowser owner(port);
        CHECK(owner.open(err));
        CHECK(owner.login("admin", kOwnerPassword).status == 200);
        CHECK(owner.request("POST", "/api/admin", std::string(), "{\"op\":\"banAddress\",\"address\":\"" + lan[0] + "\"}").status == 200);
    }
    SecureBrowser lanListener(port, lan[0]), localListener(port);
    CHECK(lanListener.open(err));
    CHECK(localListener.open(err));
    // Even from behind a header that says "I am 127.0.0.1", the banned LAN address is refused ...
    CHECK(lanListener.login("Mallory", kListenerPassword, forged).status == 403);
    // ... and 127.0.0.1 is not banned by it, nor by claiming to be the banned address.
    CHECK(localListener.login("Alice", kListenerPassword, "X-Forwarded-For: " + lan[0] + "\r\n").status == 200);
    // Banning loopback instead flips it round.
    CHECK(server.room().unbanAddress(lan[0], 0));
    CHECK(server.room().banAddress("127.0.0.1", 0).ok);
    SecureBrowser local3(port), lan3(port, lan[0]);
    CHECK(local3.open(err) && lan3.open(err));
    CHECK(local3.login("Zed", kListenerPassword, "X-Forwarded-For: " + lan[0] + "\r\n").status == 403);
    CHECK(lan3.login("Yan", kListenerPassword).status == 200);
    // The audit of a listener joined from the LAN names it by its handle only (the address is on the owner's live list, not in the trail).
    {
        SecureBrowser owner(port);
        CHECK(owner.open(err));
        CHECK(owner.login("admin", kOwnerPassword).status == 200);
        const HttpReply state = owner.request("GET", "/api/admin/state");
        const json sj = json::parse(state.body, nullptr, false);
        CHECK(!sj.is_discarded());
        if (!sj.is_discarded()) {
            bool seen = false;
            for (const json& l : sj["listeners"]) { seen = seen || l.value("address", "") == lan[0]; }
            CHECK(seen);   // the owner's live list shows the TRUE address, not the relay's
            std::string audit;
            for (const json& e : sj["audit"]) { audit += e.dump(); }
            CHECK(audit.find(lan[0]) == std::string::npos);
        }
    }
    CHECK(server.room().unbanAddress("127.0.0.1", 0));
    server.room().revokeAll();

    // (2) THE LIMITER: five wrong answers from the LAN address (with headers claiming to be somebody else) throttle THAT address; a client
    // on loopback is a different client and is still answered normally. Without the true address both would be 127.0.0.1 and share a count.
    SecureBrowser fromLan(port, lan[0]), fromLocal(port);
    CHECK(fromLan.open(err));
    CHECK(fromLocal.open(err));
    for (int i = 0; i < 5; ++i) { CHECK(fromLan.login("admin", "wrong-wrong-wrong", forged).status == 401); }
    CHECK(fromLan.login("admin", kOwnerPassword, forged).status == 429);   // throttled although the password is right
    const HttpReply local = fromLocal.login("admin", kOwnerPassword);
    CHECK(local.status == 200);                                           // loopback is its own client
    // A loopback client claiming to be the throttled LAN address does not inherit its count either.
    SecureBrowser local2(port);
    CHECK(local2.open(err));
    CHECK(local2.login("admin", kOwnerPassword, "X-Forwarded-For: " + lan[0] + "\r\n").status == 200);

    server.stop();
}
void testHttpsRefusals() {
    WebServer server;
    std::string error;
    WebServerConfig c = roomConfig();
    c.tlsEnabled = true;
    c.tlsCertThumbprint = "not-a-thumbprint";
    CHECK(!server.start(c, error));
    CHECK(server.decision().verdict == BindVerdict::RefusedTlsCertificate);
    c.tlsCertThumbprint = std::string(40, 'a');   // well-formed, and in no store
    c.port = 18398;
    CHECK(!server.start(c, error));
    CHECK(error.find("HTTPS") != std::string::npos);
    CHECK(error.find("no certificate with that thumbprint") != std::string::npos);
    CHECK(!server.running() && !server.tlsInfo().active);
    // A port already taken: the sentence names it, and nothing is left running.
    c.tlsCertThumbprint.clear();
    SchannelClient::wsa();
    SOCKET blocker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // a plain listener: no SO_REUSEADDR, as another program's would be
    sockaddr_in ba{};
    ba.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &ba.sin_addr);
    CHECK(bind(blocker, reinterpret_cast<sockaddr*>(&ba), sizeof(ba)) == 0);
    CHECK(listen(blocker, 4) == 0);
    int bl = sizeof(ba);
    getsockname(blocker, reinterpret_cast<sockaddr*>(&ba), &bl);
    c.port = ntohs(ba.sin_port);
    CHECK(!server.start(c, error));
    CHECK(error.find("bind") != std::string::npos);
    CHECK(!server.running());
    closesocket(blocker);
}

#endif  // _WIN32

// A DEVELOPER'S AID, not a test: UW_SERVE=<seconds> keeps a server with listeners, chat and a note up on a printed port so the page can be
// opened in a browser and looked at. The owner is admin / owner-long-password, listeners use listener-long-password. CTest never sets it.
void serveForLooking(int seconds, bool https) {
    WebServer server;
    server.setStatusProvider([] { return sensitiveStatus(); });
    CHECK(server.setSessionOpener(opener(std::make_shared<StubState>())));
    WebServerConfig c = roomConfig(6);
    c.chatEnabled = true;
#ifdef _WIN32
    c.tlsEnabled = https;
#else
    (void)https;
#endif
    std::string error;
    const int port = startOnFreePort(server, c, error);
    if (port <= 0) { std::printf("serve: could not start: %s\n", error.c_str()); return; }
    std::printf("SERVING %s://127.0.0.1:%d for %d s\n", https ? "https" : "http", port, seconds);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    server.stop();
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "../..";
    g_scratch = (fs::temp_directory_path() / ("uw-web-secure-" + std::to_string(TEST_GETPID()))).string();
    fs::create_directories(g_scratch);
    const std::string only = argc > 2 ? argv[2] : "";
    if (const char* serveFor = std::getenv("UW_SERVE")) {
        serveForLooking(std::atoi(serveFor), std::getenv("UW_HTTPS") != nullptr);
        return 0;
    }
#define RUN(t) \
    if (only.empty() || std::string(#t).find(only) != std::string::npos) { t(); }
    RUN(testSignIn);
    RUN(testListenersOffByDefault);
    RUN(testConcurrentJoinsAtTheCap);
    RUN(testTheOpListIsComplete);
    RUN(testEveryOpAsAListener);
    RUN(testCraftedEscalationRefused);
    RUN(testStatusIsRedactedForListeners);
    RUN(testModerationEndsLiveSessionsAndStreams);
    RUN(testIdleAndSessionLimits);
    RUN(testChatNoteAndEscaping);
    RUN(testChatOffByDefault);
    RUN(testPageCarriesTheRoom);
    RUN(testPolicyForRoom);
#ifdef _WIN32
    RUN(testServerOverHttps);
    RUN(testThrottleAndBansUseTheTrueAddress);
    RUN(testHttpsRefusals);
#else
    SKIP_LINUX("HTTPS for the web page uses Windows' Schannel; UNPROVEN on Linux");
#endif
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_web_secure");
}
