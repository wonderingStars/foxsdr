// test_engine_api_s4b_listing.cpp - screen lane S4b, G3-30: the public receiver listing through the REAL engine. FoxSDR sends the listing to NOBODY: a directory reads status.json from the web page, and the
// person registers the page's address themselves. This test proves what the page serves: nothing while the listing is off or its words are not valid, and, once on, exactly the document the engine says it
// will serve (the preview), the person's own words and nothing else. The stand-in for the directory is a plain client on 127.0.0.1 reading the page; nothing leaves this computer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using nlohmann::json;

namespace {

template <class F>
bool until(Rig& r, F f, int ms = 8000) {
    const auto t0 = nettest::Clock::now();
    while (nettest::msSince(t0) < ms) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return f();
}

int freePort() {
    std::string err;
    std::uint32_t ip = 0;
    cascade::net::sock::parseIpv4("127.0.0.1", ip);
    const cascade::net::sock::Handle h = cascade::net::sock::listenTcp(ip, 0, 1, err);
    const int p = h == cascade::net::sock::kInvalid ? 0 : cascade::net::sock::boundPort(h);
    cascade::net::sock::closeHandle(h);
    return p;
}

// The directory's stand-in: one GET of /status.json from the page; the status code and the body.
std::pair<int, std::string> readListing(Rig& rig, int port) {
    nettest::Client c;
    if (!c.connect(static_cast<std::uint16_t>(port))) { return {-1, std::string()}; }
    const std::string req = "GET /status.json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    c.sendBytes(req.data(), req.size());
    const auto t0 = nettest::Clock::now();
    while (!c.eof && nettest::msSince(t0) < 5000) { rig.pass(1); c.pump(30); }
    const std::string all(c.got.begin(), c.got.end());
    const int status = all.size() > 12 ? std::atoi(all.substr(9, 3).c_str()) : 0;
    const std::size_t b = all.find("\r\n\r\n");
    return {status, b == std::string::npos ? std::string() : all.substr(b + 4)};
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_listing");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4blisting");
    {
        Rig rig(dir);
        rig.pass(4);
        // off by default: nothing is built, nothing is served
        CHECK(rig.fact("wirec.listing.enabled") == "0" && rig.fact("listing.preview").empty() && rig.fact("listing.problem").empty() && rig.fact("listing.live") == "0");
        // the page is up on this computer only
        const int port = freePort();
        CHECK(port >= 1024);
        std::string webError;
        CHECK(Access::startWeb(rig.eng, port, webError));   // (by hand: a test engine binds nothing)
        CHECK(readListing(rig, port).first == 404);   // the listing is off: the page answers as if the route did not exist
        // words that are not valid: said in a sentence, nothing served
        CHECK(rig.status(setting("listing.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").find("name") != std::string::npos; }));
        CHECK(rig.fact("listing.preview").empty() && readListing(rig, port).first == 404);
        CHECK(rig.status(setting("listing.name", "Hilltop SDR")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").empty() && !rig.fact("listing.preview").empty(); }));
        // one latitude without a longitude, a band with the wrong shape, a number that is not: refused, nothing served
        CHECK(rig.status(setting("listing.lat", "51.5")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").find("latitude") != std::string::npos; }));
        CHECK(rig.fact("listing.preview").empty() && readListing(rig, port).first == 404);
        CHECK(rig.status(setting("listing.lon", "-0.12")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").empty(); }));
        CHECK(rig.status(setting("listing.bands", "40 m;7.1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").find("band 1") != std::string::npos; }));
        CHECK(rig.status(setting("listing.bands", "40 m;7.1;2.4|20 m;14.2;2.4")) == FOXAPI_OK);
        CHECK(rig.status(setting("listing.location", "A hill in Kent")) == FOXAPI_OK);
        CHECK(rig.status(setting("listing.max_clients", "5")) == FOXAPI_OK);
        CHECK(rig.status(setting("listing.max_clients", "0")) != FOXAPI_OK && rig.status(setting("listing.max_clients", "1001")) != FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.problem").empty() && !rig.fact("listing.preview").empty(); }));
        CHECK(until(rig, [&] { return rig.fact("listing.live") == "1"; }));   // (the page is up and has the document)
        // what the page serves is EXACTLY the preview: the person's words and the program's version, nothing else
        const std::string preview = rig.fact("listing.preview");
        const auto served = readListing(rig, port);
        CHECK(served.first == 200 && served.second == preview);
        const json d = json::parse(preview, nullptr, false);
        CHECK(d.is_object() && d["receiver"]["name"] == "Hilltop SDR" && d["receiver"]["location"] == "A hill in Kent" && d["receiver"]["gps"]["lat"] == 51.5 && d["receiver"]["gps"]["lon"] == -0.12);
        CHECK(d["max_clients"] == 5 && d["sdrs"][0]["profiles"].size() == 2 && d["sdrs"][0]["profiles"][0]["center_freq"] == 7100000 && d["sdrs"][0]["profiles"][1]["sample_rate"] == 2400000);
        CHECK(d["version"].is_string() && !d["version"].get<std::string>().empty());
        // the keys of the document: receiver (name, location, gps), max_clients, version, sdrs: nothing else (no e-mail, no altitude, no serial number, no radio model, no address)
        std::string keys;
        for (auto it = d.begin(); it != d.end(); ++it) { keys += it.key() + ","; }
        CHECK(keys == "max_clients,receiver,sdrs,version,");
        std::string rkeys;
        for (auto it = d["receiver"].begin(); it != d["receiver"].end(); ++it) { rkeys += it.key() + ","; }
        CHECK(rkeys == "gps,location,name,");
        CHECK(preview.find("127.0.0.1") == std::string::npos && preview.find("admin") == std::string::npos && preview.find("@") == std::string::npos);
        // text the model refuses is refused whole
        CHECK(rig.status(setting("listing.name", std::string("bad") + "\x01" + "name")) != FOXAPI_OK);   // (the settings op refuses a control character itself)
        CHECK(rig.fact("wirec.listing.name") == "Hilltop SDR" && readListing(rig, port).first == 200);
        CHECK(rig.status(setting("listing.name", std::string(65, 'n'))) != FOXAPI_OK && rig.status(setting("listing.location", std::string(97, 'l'))) != FOXAPI_OK);   // longer than the document allows
        // turned off: the page is a 404 again
        CHECK(rig.status(setting("listing.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("listing.preview").empty(); }));
        CHECK(readListing(rig, port).first == 404);
        CHECK(rig.fact("listing.live") == "0");
        Access::stopWeb(rig.eng);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_listing");
}
