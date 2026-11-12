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

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_web");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("web");
    int n = 0;
    for (auto fn : {testCors}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_web");
}
