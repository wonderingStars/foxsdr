// test_engine_api_wire_c_web.cpp - the web server features of wiring lane C, over real HTTP on loopback against the engine's own web server (src/net/web_wire_c.*,
// engine_wire_c.cpp). One section per row, each driven through the engine's setting op and the server's real routes.
//
//   C1  CORS (W7a): by default no cross-origin header is ever added; with an allow-list an allowed origin gets Access-Control-Allow-Origin and Vary, a denied one gets none,
//       an allowed preflight is a 204 with the allowed methods, two Origin headers are denied; refusals (a bad origin, a path in an origin, credentials) change nothing;
//       emptying the list switches it off again; the sign-in gate is NOT weakened (a protected route is still 401 to an allowed origin)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include <chrono>
#include <thread>

#include "net/web_auth.hpp"
#include "net/web_server.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

struct WebRig {
    Rig rig;
    int port = -1;
    explicit WebRig(const fs::path& dir) : rig(dir) {
        cascade::net::WebServerConfig cfg;
        cfg.enabled = true;
        cfg.bindAddress = "127.0.0.1";
        cfg.username = "admin";
        cascade::net::PasswordRecord rec;
        std::string error;
        CHECK(cascade::net::hashPassword("a-good-long-password", rec, error));
        cfg.passwordRecord = rec.serialize();
        for (int p = 18420; p < 18520; ++p) {
            cfg.port = p;
            if (rig.eng.servers().webServer().start(cfg, error)) { port = p; break; }
        }
        CHECK(port > 0);
    }
    ~WebRig() { rig.eng.servers().webServer().stop(); }
    // the engine takes the setting on its next passes
    void settle() { rig.pass(4); }
};

httplib::Headers headers(std::initializer_list<std::pair<const char*, const char*>> h) {
    httplib::Headers out;
    for (const auto& p : h) { out.emplace(p.first, p.second); }
    return out;
}

void testCors(const fs::path& dir) {
    WebRig w(dir);
    if (w.port <= 0) { return; }
    httplib::Client cli("127.0.0.1", w.port);
    cli.set_connection_timeout(5, 0);
    const char* kOrigin = "https://radio.example";

    // default: no list, no header
    {
        auto r = cli.Get("/api/session", headers({{"Origin", kOrigin}}));
        CHECK(static_cast<bool>(r));
        if (r) {
            CHECK(!r->has_header("Access-Control-Allow-Origin"));
            CHECK(r->status == 200);
        }
    }
    CHECK(w.rig.status(setting("web.cors_origins", std::string(kOrigin) + "\nhttp://192.168.1.20:8080")) == FOXAPI_OK);
    w.settle();
    {
        auto r = cli.Get("/api/session", headers({{"Origin", kOrigin}}));
        CHECK(static_cast<bool>(r));
        if (r) {
            CHECK(r->get_header_value("Access-Control-Allow-Origin") == kOrigin);
            CHECK(r->get_header_value("Vary").find("Origin") != std::string::npos);
            CHECK(!r->has_header("Access-Control-Allow-Credentials"));
        }
        auto lan = cli.Get("/api/session", headers({{"Origin", "http://192.168.1.20:8080"}}));
        CHECK(static_cast<bool>(lan) && lan->get_header_value("Access-Control-Allow-Origin") == "http://192.168.1.20:8080");
        auto denied = cli.Get("/api/session", headers({{"Origin", "https://evil.example"}}));
        CHECK(static_cast<bool>(denied));
        if (denied) { CHECK(!denied->has_header("Access-Control-Allow-Origin")); }
        auto none = cli.Get("/api/session");
        CHECK(static_cast<bool>(none));
        if (none) { CHECK(!none->has_header("Access-Control-Allow-Origin")); }
    }
    {   // an allowed preflight
        auto r = cli.Options("/api/control", headers({{"Origin", kOrigin}, {"Access-Control-Request-Method", "POST"}, {"Access-Control-Request-Headers", "Content-Type"}}));
        CHECK(static_cast<bool>(r));
        if (r) {
            CHECK(r->status == 204);
            CHECK(r->get_header_value("Access-Control-Allow-Origin") == kOrigin);
            CHECK(r->get_header_value("Access-Control-Allow-Methods").find("POST") != std::string::npos);
        }
        // a preflight from a denied origin is not answered as one
        auto bad = cli.Options("/api/control", headers({{"Origin", "https://evil.example"}, {"Access-Control-Request-Method", "POST"}}));
        CHECK(static_cast<bool>(bad));
        if (bad) {
            CHECK(bad->status != 204);
            CHECK(!bad->has_header("Access-Control-Allow-Origin"));
        }
    }
    {   // CORS is not authentication: a protected route is still refused to an allowed origin
        auto r = cli.Get("/api/status", headers({{"Origin", kOrigin}}));
        CHECK(static_cast<bool>(r));
        if (r) { CHECK(r->status == 401); }
    }
    {   // two Origin headers at once are denied
        httplib::Headers two;
        two.emplace("Origin", kOrigin);
        two.emplace("Origin", "https://other.example");
        auto r = cli.Get("/api/session", two);
        CHECK(static_cast<bool>(r));
        if (r) { CHECK(!r->has_header("Access-Control-Allow-Origin")); }
    }
    // refusals change nothing
    CHECK(w.rig.status(setting("web.cors_origins", "ftp://x.example")) == FOXAPI_BAD_ARGUMENT);
    CHECK(w.rig.status(setting("web.cors_origins", "https://a.example/path")) == FOXAPI_BAD_ARGUMENT);
    CHECK(w.rig.status(setting("web.cors_origins", "not an origin")) == FOXAPI_BAD_ARGUMENT);
    CHECK(w.rig.status(setting("web.cors_credentials", "on")) == FOXAPI_OUT_OF_RANGE);
    CHECK(w.rig.fact("wirec.web.cors_origins") == std::string(kOrigin) + "\nhttp://192.168.1.20:8080");
    // off again
    CHECK(w.rig.status(setting("web.cors_origins", "")) == FOXAPI_OK);
    w.settle();
    {
        auto r = cli.Get("/api/session", headers({{"Origin", kOrigin}}));
        CHECK(static_cast<bool>(r));
        if (r) { CHECK(!r->has_header("Access-Control-Allow-Origin")); }
    }
}

// Signs in over loopback and returns the Cookie header value ("" on failure).
std::string signIn(httplib::Client& cli) {
    auto r = cli.Post("/api/login", "{\"username\":\"admin\",\"password\":\"a-good-long-password\"}", "application/json");
    if (!r || r->status != 200) { return std::string(); }
    const std::string sc = r->get_header_value("Set-Cookie");
    const std::size_t end = sc.find(';');
    return sc.substr(0, end);
}

void testBookmarks(const fs::path& dir) {
    WebRig w(dir);
    if (w.port <= 0) { return; }
    httplib::Client cli("127.0.0.1", w.port);
    cli.set_connection_timeout(5, 0);
    // not signed in: 401 on every route
    CHECK(cli.Get("/api/mybookmarks")->status == 401);
    CHECK(cli.Post("/api/mybookmarks", "{}", "application/json")->status == 401);
    CHECK(cli.Delete("/api/mybookmarks/1")->status == 401);
    CHECK(cli.Get("/api/mybookmarks/export")->status == 401);
    const std::string c1 = signIn(cli);
    CHECK(!c1.empty());
    if (c1.empty()) { return; }
    const httplib::Headers h1 = headers({{"Cookie", c1.c_str()}});
    const std::string one = "{\"name\":\"Tower\",\"freqHz\":118300000,\"mode\":\"AM\",\"bandwidthHz\":8000,\"group\":\"Air\",\"tags\":[\"Tower\",\"vhf\"]}";
    auto add = cli.Post("/api/mybookmarks", h1, one, "application/json");
    CHECK(static_cast<bool>(add));
    std::uint64_t id = 0;
    if (add) {
        CHECK(add->status == 200);
        const auto j = nlohmann::json::parse(add->body, nullptr, false);
        CHECK(!j.is_discarded() && j.value("ok", false));
        id = j.value("id", 0ull);
        CHECK(id != 0u);
    }
    {   // listed, with its group
        auto r = cli.Get("/api/mybookmarks", h1);
        CHECK(static_cast<bool>(r) && r->status == 200);
        const auto j = nlohmann::json::parse(r->body, nullptr, false);
        CHECK(!j.is_discarded() && j["bookmarks"].size() == 1u);
        CHECK(j["bookmarks"][0]["name"] == "Tower");
        CHECK(j["bookmarks"][0]["mode"] == "AM");
        CHECK(j["bookmarks"][0]["tags"].size() == 2u);
        CHECK(j["groups"].size() == 1u && j["groups"][0] == "Air");
    }
    {   // search
        auto hit = cli.Get("/api/mybookmarks?text=tow&min=118000000&max=119000000", h1);
        CHECK(static_cast<bool>(hit) && nlohmann::json::parse(hit->body)["bookmarks"].size() == 1u);
        auto miss = cli.Get("/api/mybookmarks?text=nothing-like-it", h1);
        CHECK(static_cast<bool>(miss) && nlohmann::json::parse(miss->body)["bookmarks"].empty());
    }
    {   // refusals: each is a 400 with a sentence and changes nothing
        const char* bad[] = {"{\"name\":\"\",\"freqHz\":1000000,\"mode\":\"AM\"}", "{\"name\":\"x\",\"freqHz\":0,\"mode\":\"AM\"}",
                             "{\"name\":\"x\",\"freqHz\":1000000,\"mode\":\"BOGUS\"}", "not json", "[1,2]",
                             "{\"name\":\"x\",\"freqHz\":1000000,\"mode\":\"AM\",\"bandwidthHz\":1}"};
        for (const char* b : bad) {
            auto r = cli.Post("/api/mybookmarks", h1, b, "application/json");
            CHECK(static_cast<bool>(r));
            if (r) {
                if (r->status != 400) { std::printf("      '%s' answered %d\n", b, r->status); }
                CHECK(r->status == 400);
                const auto j = nlohmann::json::parse(r->body, nullptr, false);
                CHECK(!j.is_discarded() && !j.value("ok", true) && !j.value("error", std::string()).empty());
            }
        }
        auto dup = cli.Post("/api/mybookmarks", h1, one, "application/json");   // a duplicate
        CHECK(static_cast<bool>(dup) && dup->status == 400);
        auto r = cli.Get("/api/mybookmarks", h1);
        CHECK(nlohmann::json::parse(r->body)["bookmarks"].size() == 1u);
    }
    {   // update, a missing id is a 404
        const std::string upd = "{\"name\":\"Tower 2\",\"freqHz\":118400000,\"mode\":\"AM\",\"bandwidthHz\":8000}";
        auto r = cli.Put(("/api/mybookmarks/" + std::to_string(id)).c_str(), h1, upd, "application/json");
        CHECK(static_cast<bool>(r) && r->status == 200);
        auto l = cli.Get("/api/mybookmarks", h1);
        CHECK(nlohmann::json::parse(l->body)["bookmarks"][0]["name"] == "Tower 2");
        auto missing = cli.Put("/api/mybookmarks/99999", h1, upd, "application/json");
        CHECK(static_cast<bool>(missing) && missing->status == 404);
    }
    {   // export, delete (a second delete is a 404), import back with replace
        auto ex = cli.Get("/api/mybookmarks/export", h1);
        CHECK(static_cast<bool>(ex) && ex->status == 200);
        CHECK(ex->get_header_value("Content-Disposition").find("foxsdr-bookmarks.json") != std::string::npos);
        auto del = cli.Delete(("/api/mybookmarks/" + std::to_string(id)).c_str(), h1);
        CHECK(static_cast<bool>(del) && del->status == 200);
        auto del2 = cli.Delete(("/api/mybookmarks/" + std::to_string(id)).c_str(), h1);
        CHECK(static_cast<bool>(del2) && del2->status == 404);
        auto imp = cli.Post("/api/mybookmarks/import?replace=1", h1, ex->body, "application/json");
        CHECK(static_cast<bool>(imp) && imp->status == 200);
        if (imp) { CHECK(nlohmann::json::parse(imp->body)["imported"] == 1); }
        auto bad = cli.Post("/api/mybookmarks/import", h1, "{broken", "application/json");
        CHECK(static_cast<bool>(bad) && bad->status == 400);
        // the server's own body cap still holds: a file over it never reaches the model
        auto big = cli.Post("/api/mybookmarks/import", h1, std::string(20000, ' '), "application/json");
        CHECK(!big || big->status == 413 || big->status == 400);
    }
    // sign out: the set goes with the sign-in; a new sign-in starts empty
    auto out = cli.Post("/api/logout", h1, "", "application/json");
    CHECK(static_cast<bool>(out) && out->status == 200);
    CHECK(cli.Get("/api/mybookmarks", h1)->status == 401);
    const std::string c2 = signIn(cli);
    CHECK(!c2.empty() && c2 != c1);
    if (!c2.empty()) {
        auto r = cli.Get("/api/mybookmarks", headers({{"Cookie", c2.c_str()}}));
        CHECK(static_cast<bool>(r) && r->status == 200);
        CHECK(nlohmann::json::parse(r->body)["bookmarks"].empty());
    }
}

void testMetrics(const fs::path& dir) {
    WebRig w(dir);
    if (w.port <= 0) { return; }
    httplib::Client cli("127.0.0.1", w.port);
    cli.set_connection_timeout(5, 0);
    const std::string cookie = signIn(cli);
    CHECK(!cookie.empty());
    const httplib::Headers h = headers({{"Cookie", cookie.c_str()}});
    // off by default: as if it did not exist, signed in or not
    {
        auto a = cli.Get("/metrics");
        auto b = cli.Get("/metrics", h);
        CHECK(static_cast<bool>(a) && a->status == 404);
        CHECK(static_cast<bool>(b) && b->status == 404);
    }
    CHECK(w.rig.status(setting("web.metrics", "on")) == FOXAPI_OK);
    w.settle();
    // an event the host posts (a decoder's line): only its COUNT may reach the page
    w.rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "TOP-SECRET-MESSAGE-TEXT 1234567", 148.0e6, 0);
    w.rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "another line", 148.0e6, 0);
    {
        auto none = cli.Get("/metrics");   // signed out: refused, like every protected page
        CHECK(static_cast<bool>(none) && none->status == 401);
    }
    std::string body;
    bool seen = false;
    for (int i = 0; i < 80 && !seen; ++i) {
        w.rig.pass(2);
        auto r = cli.Get("/metrics", h);
        if (r && r->status == 200) {
            body = r->body;
            seen = body.find("foxsdr_decodes_total{decoder=\"pocsag-decoder\"} 2") != std::string::npos;
            if (i == 0) { CHECK(r->get_header_value("Content-Type").find("text/plain; version=0.0.4") != std::string::npos); }
        }
        if (!seen) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    CHECK(seen);
    CHECK(body.find("# TYPE foxsdr_decodes_total counter") != std::string::npos);
    CHECK(body.find("foxsdr_connected_clients 1") != std::string::npos);
    CHECK(body.find("foxsdr_dropped_samples_total") != std::string::npos);
    CHECK(body.find("TOP-SECRET") == std::string::npos && body.find("another line") == std::string::npos && body.find("148000000") == std::string::npos);   // never the text or the frequency
    // switched off again
    CHECK(w.rig.status(setting("web.metrics", "off")) == FOXAPI_OK);
    w.settle();
    auto off = cli.Get("/metrics", h);
    CHECK(static_cast<bool>(off) && off->status == 404);
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_web");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("web");
    int n = 0;
    for (auto fn : {testCors, testBookmarks, testMetrics}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_web");
}
