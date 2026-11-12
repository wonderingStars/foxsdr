// Tests for the public receiver listing (net/receiver_listing.hpp) and its route GET /status.json in the web server: the model, its limits at
// every boundary, the exact document, and the route over real HTTP on loopback (off by default, no sign-in needed, nothing but the user's own
// fields, not counted as a browser being used).
//
// The document's shape comes from public OpenWebRX receivers' own status pages and the project's API documentation (see the header). What the
// directory requires of it is UNVERIFIED: no test connects to the directory, and none may.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "net/receiver_listing.hpp"
#include "net/web_auth.hpp"
#include "net/web_server.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nlohmann::json;

namespace {

ReceiverListing good() {
    ReceiverListing l;
    l.enabled = true;
    l.name = "Hilltop SDR";
    l.location = "Exampleton, Region";
    l.hasPosition = true;
    l.latitude = 51.5;
    l.longitude = -0.12;
    l.bands = {{"2 m", 145000000.0, 2400000.0}, {"40 m", 7100000.0, 192000.0}};
    l.maxClients = 4;
    l.version = "v0.99.109";
    return l;
}

std::string bad(ReceiverListing l) { return validateListing(l); }

struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 33);
    }
};

void collectKeys(const json& j, const std::string& prefix, std::set<std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string k = prefix.empty() ? it.key() : prefix + "." + it.key();
            out.insert(k);
            collectKeys(it.value(), k, out);
        }
    } else if (j.is_array()) {
        for (const auto& e : j) { collectKeys(e, prefix + "[]", out); }
    }
}

void testLimits() {
    CHECK(validateListing(good()).empty());
    CHECK(validateListing(ReceiverListing{}).empty());                    // off: nothing to publish, always valid
    {
        ReceiverListing off = good();
        off.enabled = false;
        off.name.clear();
        off.maxClients = -9;
        CHECK(validateListing(off).empty() && listingJson(off).empty());   // an invalid listing that is OFF publishes nothing
    }
    ReceiverListing l = good();
    l.name.clear();
    CHECK(!validateListing(l).empty() && std::string(validateListing(l)).find("type a name") != std::string::npos);
    l.name = "x";
    CHECK(validateListing(l).empty());
    l.name = std::string(64, 'n');
    CHECK(validateListing(l).empty());
    l.name = std::string(65, 'n');
    CHECK(!validateListing(l).empty());
    l = good();
    l.name = "tab\there";
    CHECK(!validateListing(l).empty());
    l.name = std::string("nul\0inside", 10);
    CHECK(!validateListing(l).empty());
    l.name = "del\x7F";
    CHECK(!validateListing(l).empty());
    l.name = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x93\xA1";   // e-acute, euro, satellite dish: valid UTF-8
    CHECK(validateListing(l).empty());
    for (const char* badUtf8 : {"\xC0\x80", "\xC3", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF", "\x80", "ab\xE2\x28\xA1"}) {
        l.name = badUtf8;
        CHECK(!validateListing(l).empty());
    }
    l = good();
    l.location.clear();
    CHECK(validateListing(l).empty());                                      // a location may be empty
    l.location = std::string(96, 'l');
    CHECK(validateListing(l).empty());
    l.location = std::string(97, 'l');
    CHECK(!validateListing(l).empty());
    // position
    l = good();
    l.latitude = 90.0; l.longitude = 180.0;
    CHECK(validateListing(l).empty());
    l.latitude = -90.0; l.longitude = -180.0;
    CHECK(validateListing(l).empty());
    l.latitude = 90.0001;
    CHECK(!validateListing(l).empty());
    l.latitude = 0; l.longitude = 180.5;
    CHECK(!validateListing(l).empty());
    l.latitude = std::numeric_limits<double>::quiet_NaN(); l.longitude = 0;
    CHECK(!validateListing(l).empty());
    l.latitude = 0; l.longitude = std::numeric_limits<double>::infinity();
    CHECK(!validateListing(l).empty());
    l.hasPosition = false;                                                  // not published, so not judged
    CHECK(validateListing(l).empty());
    // bands
    l = good();
    l.bands.clear();
    CHECK(validateListing(l).empty());
    l.bands.assign(32, ListedBand{"b", 1e6, 1e5});
    CHECK(validateListing(l).empty());
    l.bands.assign(33, ListedBand{"b", 1e6, 1e5});
    CHECK(!validateListing(l).empty());
    auto oneBand = [&](ListedBand b) { ReceiverListing x = good(); x.bands = {b}; return validateListing(x); };
    CHECK(oneBand({"ok", 1.0, 1.0}).empty() && oneBand({"ok", 1.0e12, 1.0e10}).empty());
    CHECK(!oneBand({"", 1e6, 1e5}).empty() && !oneBand({std::string(49, 'b'), 1e6, 1e5}).empty() && oneBand({std::string(48, 'b'), 1e6, 1e5}).empty());
    CHECK(!oneBand({"b\n", 1e6, 1e5}).empty());
    CHECK(!oneBand({"b", 0.5, 1e5}).empty() && !oneBand({"b", 1.0e12 + 1.0, 1e5}).empty() && !oneBand({"b", -5.0, 1e5}).empty());
    CHECK(!oneBand({"b", std::nan(""), 1e5}).empty() && !oneBand({"b", 1e6, std::nan("")}).empty());
    CHECK(!oneBand({"b", 1e6, 0.5}).empty() && !oneBand({"b", 1e6, 1.0e10 + 1.0}).empty() && !oneBand({"b", 1e6, 0.0}).empty());
    // listener limit and version
    l = good();
    l.maxClients = 1; CHECK(validateListing(l).empty());
    l.maxClients = 1000; CHECK(validateListing(l).empty());
    l.maxClients = 0; CHECK(!validateListing(l).empty());
    l.maxClients = 1001; CHECK(!validateListing(l).empty());
    l.maxClients = -1; CHECK(!validateListing(l).empty());
    l = good();
    l.version.clear(); CHECK(validateListing(l).empty());
    l.version = std::string(32, 'v'); CHECK(validateListing(l).empty());
    l.version = std::string(33, 'v'); CHECK(!validateListing(l).empty());
    l.version = "v\xC3\xA9"; CHECK(!validateListing(l).empty());
    l.version = "v1\n"; CHECK(!validateListing(l).empty());
}

void testDocument() {
    const std::string doc = listingJson(good());
    CHECK(!doc.empty());
    const json j = json::parse(doc, nullptr, false);
    CHECK(!j.is_discarded());
    const json want = json::parse(R"({"receiver":{"name":"Hilltop SDR","location":"Exampleton, Region","gps":{"lat":51.5,"lon":-0.12}},
        "max_clients":4,"version":"v0.99.109",
        "sdrs":[{"name":"Hilltop SDR","type":"FoxSDR","profiles":[{"name":"2 m","center_freq":145000000,"sample_rate":2400000},
                                                                {"name":"40 m","center_freq":7100000,"sample_rate":192000}]}]})");
    CHECK(j == want);
    // exactly these keys and no others: nothing private can have slipped in
    std::set<std::string> keys;
    collectKeys(j, "", keys);
    const std::set<std::string> allowed = {"receiver", "receiver.name", "receiver.location", "receiver.gps", "receiver.gps.lat", "receiver.gps.lon", "max_clients",
                                           "version", "sdrs", "sdrs[].name", "sdrs[].type", "sdrs[].profiles",
                                           "sdrs[].profiles[].name", "sdrs[].profiles[].center_freq", "sdrs[].profiles[].sample_rate"};
    CHECK(keys == allowed);
    CHECK(doc.find("admin") == std::string::npos && doc.find("asl") == std::string::npos && doc.find("serial") == std::string::npos);
    // without a position there is no gps key at all
    ReceiverListing np = good();
    np.hasPosition = false;
    np.latitude = 12.0;
    const json j2 = json::parse(listingJson(np));
    CHECK(!j2["receiver"].contains("gps") && listingJson(np).find("12") == std::string::npos);
    // no bands: an empty list, still a document
    ReceiverListing nb = good();
    nb.bands.clear();
    CHECK(json::parse(listingJson(nb))["sdrs"][0]["profiles"].empty());
    // characters that need escaping survive as data, not as structure
    ReceiverListing q = good();
    q.name = "A \"quoted\" \\ name </script>";
    const json j3 = json::parse(listingJson(q));
    CHECK(j3["receiver"]["name"] == q.name && j3["sdrs"].size() == 1);
    // frequencies are whole hertz
    ReceiverListing fr = good();
    fr.bands = {{"x", 7100000.4, 192000.6}};
    const json j4 = json::parse(listingJson(fr));
    CHECK(j4["sdrs"][0]["profiles"][0]["center_freq"] == 7100000 && j4["sdrs"][0]["profiles"][0]["sample_rate"] == 192001);
    // invalid or off: empty
    ReceiverListing inv = good();
    inv.name.clear();
    CHECK(listingJson(inv).empty());
    ReceiverListing off = good();
    off.enabled = false;
    CHECK(listingJson(off).empty());
}

void testTextProperty() {
    Rng rng{20261112};
    for (int i = 0; i < 20000; ++i) {
        std::string s;
        const std::size_t n = rng.next() % 12;
        for (std::size_t k = 0; k < n; ++k) { s.push_back(static_cast<char>(rng.next() & 0xFF)); }
        if (listingTextOk(s, 64)) {
            // anything accepted must be strict UTF-8 without controls: nlohmann's strict dump throws on invalid UTF-8
            bool threw = false;
            try { (void)json(s).dump(-1, ' ', false, json::error_handler_t::strict); } catch (const json::exception&) { threw = true; }
            CHECK(!threw);
            for (char c : s) { CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) != 0x7F); }
        }
    }
    // and valid text is accepted: random scalar values encoded by hand
    for (int i = 0; i < 4000; ++i) {
        std::string s;
        const int n = 1 + static_cast<int>(rng.next() % 8);
        for (int k = 0; k < n; ++k) {
            std::uint32_t cp = rng.next() % 0x110000;
            if (cp < 0x20 || cp == 0x7F || (cp >= 0xD800 && cp <= 0xDFFF)) { cp = 0x41; }
            if (cp < 0x80) { s.push_back(static_cast<char>(cp)); }
            else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
            else if (cp < 0x10000) { s.push_back(static_cast<char>(0xE0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
            else { s.push_back(static_cast<char>(0xF0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        }
        CHECK(listingTextOk(s, 64));
    }
}

int startOnFreePort(WebServer& server, WebServerConfig cfg, std::string& error) {
    for (int port = 18273; port < 18373; ++port) {
        cfg.port = port;
        if (server.start(cfg, error)) { return port; }
        if (!server.decision().allowed()) { return -1; }
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

void testRoute() {
    WebServer server;
    std::atomic<int> usage{0};
    CHECK(server.setUsageCallback([&usage]() { ++usage; }));
    std::string error;
    // A server WITH a password: the crawler has no cookie, and the route must still answer when (and only when) the listing is on.
    PasswordRecord rec;
    std::string hashErr;
    CHECK(hashPassword("a-good-long-password", rec, hashErr));
    WebServerConfig cfg = loopbackConfig();
    cfg.passwordRecord = rec.serialize();
    const int port = startOnFreePort(server, cfg, error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);

    // OFF BY DEFAULT: a fresh server answers 404.
    auto r0 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r0) && r0->status == 404);
    // the same as a path that was never served
    auto r0b = cli.Get("/nothing-here.json");
    CHECK(static_cast<bool>(r0b) && r0b->status == 404 && r0->body == r0b->body);
    // a disabled listing: still 404
    ReceiverListing off = good();
    off.enabled = false;
    CHECK(server.setReceiverListing(off, error) && error.empty());
    auto r1 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r1) && r1->status == 404);

    // an invalid enabled listing is refused with a sentence and changes nothing
    ReceiverListing inv = good();
    inv.name.clear();
    CHECK(!server.setReceiverListing(inv, error) && !error.empty());
    auto r2 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r2) && r2->status == 404);

    // ON: served with no cookie, as JSON, with the page's usual hardening headers, and exactly the document
    const int usageBefore = usage.load();
    CHECK(server.setReceiverListing(good(), error) && error.empty());
    auto r3 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r3));
    if (r3) {
        CHECK(r3->status == 200);
        CHECK(r3->get_header_value("Content-Type").find("application/json") == 0);
        CHECK(r3->get_header_value("X-Content-Type-Options") == "nosniff");
        CHECK(r3->get_header_value("Cache-Control") == "no-store");
        CHECK(r3->body == listingJson(good()));
        CHECK(r3->get_header_value("Set-Cookie").empty());
    }
    CHECK(usage.load() == usageBefore);                 // a crawler is not a browser being used
    auto page = cli.Get("/");
    CHECK(static_cast<bool>(page) && page->status == 200 && usage.load() == usageBefore + 1);   // the page itself still is
    // the sign-in still guards everything else
    auto api = cli.Get("/api/status");
    CHECK(static_cast<bool>(api) && api->status == 401);
    // a second invalid listing leaves the first in force
    CHECK(!server.setReceiverListing(inv, error));
    auto r4 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r4) && r4->status == 200 && r4->body == listingJson(good()));
    // updated
    ReceiverListing g2 = good();
    g2.name = "Renamed";
    CHECK(server.setReceiverListing(g2, error));
    auto r5 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r5) && r5->status == 200 && json::parse(r5->body)["receiver"]["name"] == "Renamed");
    // other methods are not routes
    auto post = cli.Post("/status.json", "{}", "application/json");
    CHECK(static_cast<bool>(post) && post->status != 200);
    // turned off again: 404 at once
    CHECK(server.setReceiverListing(off, error));
    auto r6 = cli.Get("/status.json");
    CHECK(static_cast<bool>(r6) && r6->status == 404);
    server.stop();
    // the listing outlives a stop and a start of the server only if the host sets it again; a fresh server has none (checked above)
}

void testConcurrency() {
    WebServer server;
    std::string error;
    const int port = startOnFreePort(server, loopbackConfig(), error);
    CHECK(port > 0);
    if (port <= 0) { return; }
    std::atomic<bool> stop{false};
    std::atomic<int> bad404{0}, badBody{0}, ok{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(5, 0);
            while (!stop.load()) {
                auto r = cli.Get("/status.json");
                if (!r) { continue; }
                if (r->status == 404) { ++bad404; continue; }
                const json j = json::parse(r->body, nullptr, false);
                if (r->status != 200 || j.is_discarded() || !j.contains("receiver") || !j.contains("sdrs")) { ++badBody; } else { ++ok; }
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 300; ++i) {
            ReceiverListing l = good();
            l.name = "n" + std::to_string(i);
            std::string e;
            ReceiverListing off = good();
            off.enabled = false;
            server.setReceiverListing(i % 7 == 0 ? off : l, e);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    writer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : readers) { t.join(); }
    CHECK(badBody.load() == 0);          // never a half-written or malformed document
    CHECK(ok.load() > 0 && bad404.load() > 0);
    server.stop();
}

}  // namespace

int main() {
    testLimits();
    testDocument();
    testTextProperty();
    testRoute();
    testConcurrency();
    return testSummary("test_receiver_listing");
}
