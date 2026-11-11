// Tests for the KML / GeoJSON writers (core/geo_export.hpp).
//
// KML is checked with a small well-formedness checker written here (tags nest, attributes quoted, only the five predefined entities,
// no character outside the XML 1.0 Char production, valid UTF-8); GeoJSON with a strict parse and structural checks against RFC 7946
// (longitude first, LineString / MultiLineString positions, null geometry). A million-point track is streamed through a counting
// stream buffer under the allocation probe to prove the streaming claim.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/geo_export.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace {

namespace geo = cascade::core::geo;
using json = nlohmann::json;

struct Xml {
    bool ok = false;
    std::string why;
    mutable std::map<std::string, int> count;
    std::vector<std::string> coordinates;   // text of every <coordinates>
    std::vector<std::string> names;
};

Xml checkXml(const std::string& s) {
    Xml x;
    std::vector<std::string> stack;
    std::size_t i = 0;
    auto bad = [&](const std::string& w) { x.why = w + " at " + std::to_string(i); return x; };
    if (s.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 0) != 0) { return bad("no declaration"); }
    std::string text;
    std::vector<std::string> textOf;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '<') {
            const std::size_t e = s.find('>', i);
            if (e == std::string::npos) { return bad("unterminated tag"); }
            std::string tag = s.substr(i + 1, e - i - 1);
            if (!tag.empty() && tag[0] == '?') { i = e + 1; continue; }
            if (!tag.empty() && tag[0] == '/') {
                const std::string n = tag.substr(1);
                if (stack.empty() || stack.back() != n) { return bad("mismatched close " + n); }
                if (n == "coordinates") { x.coordinates.push_back(text); }
                if (n == "name") { x.names.push_back(text); }
                stack.pop_back();
                text.clear();
            } else {
                const bool selfClose = !tag.empty() && tag.back() == '/';
                if (selfClose) { tag.pop_back(); }
                const std::size_t sp = tag.find(' ');
                const std::string n = tag.substr(0, sp);
                if (n.empty()) { return bad("empty tag"); }
                for (char ch : n) { if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == ':' || ch == '_' || ch == '-')) { return bad("bad tag name"); } }
                if (sp != std::string::npos) {   // attributes: name="value" pairs, value without raw < or "
                    std::size_t k = sp;
                    while (k < tag.size()) {
                        while (k < tag.size() && tag[k] == ' ') { ++k; }
                        if (k >= tag.size()) { break; }
                        const std::size_t eq = tag.find("=\"", k);
                        if (eq == std::string::npos) { return bad("attribute without quoted value"); }
                        const std::size_t q = tag.find('"', eq + 2);
                        if (q == std::string::npos) { return bad("unterminated attribute"); }
                        const std::string v = tag.substr(eq + 2, q - eq - 2);
                        if (v.find('<') != std::string::npos) { return bad("raw < in attribute"); }
                        k = q + 1;
                    }
                }
                ++x.count[n];
                if (!selfClose) { stack.push_back(n); }
                text.clear();
            }
            i = e + 1;
            continue;
        }
        if (c == '&') {
            const std::size_t e = s.find(';', i);
            if (e == std::string::npos) { return bad("bad entity"); }
            const std::string ent = s.substr(i, e - i + 1);
            if (ent != "&amp;" && ent != "&lt;" && ent != "&gt;" && ent != "&quot;" && ent != "&apos;") { return bad("unknown entity " + ent); }
            text += ent;
            i = e + 1;
            continue;
        }
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') { return bad("control character"); }
        if (c == 0xFF || c == 0xFE || c == 0xC0 || c == 0xC1) { return bad("invalid UTF-8 byte"); }
        text.push_back(static_cast<char>(c));
        ++i;
    }
    if (!stack.empty()) { return bad("unclosed " + stack.back()); }
    x.ok = true;
    return x;
}

geo::GeoPoint pt(double lon, double lat, bool alt = false, double altM = 0, std::int64_t ms = -1) {
    geo::GeoPoint p;
    p.lonDeg = lon;
    p.latDeg = lat;
    p.hasAlt = alt;
    p.altM = altM;
    p.unixMs = ms;
    return p;
}

// ---- points: coordinate order, numbers --------------------------------------------------------------------------------------

void testPointsGolden() {
    const geo::Entity ac = geo::aircraftEntity("406B3F", "BAW123", "7000");
    std::ostringstream k, g;
    geo::KmlWriter kw(k, "Aircraft", "a test");
    geo::GeoJsonWriter gw(g);
    // lon 100 / lat 10: an asymmetric pair, so a swapped order cannot pass
    CHECK(kw.point(ac, pt(100.25, 10.5, true, 11277.6, 1234567890123LL)));
    CHECK(gw.point(ac, pt(100.25, 10.5, true, 11277.6, 1234567890123LL)));
    CHECK(kw.point(geo::shipEntity("235012345", "MV Test"), pt(-0.1278, 51.5074)));
    CHECK(gw.point(geo::shipEntity("235012345", "MV Test"), pt(-0.1278, 51.5074)));
    CHECK(kw.point(geo::balloonEntity("V1234567", "RS41"), pt(-0.00000001, 0.0, true, -12.0)));   // rounds to -0
    CHECK(gw.point(geo::balloonEntity("V1234567", "RS41"), pt(-0.00000001, 0.0, true, -12.0)));
    CHECK(kw.point(geo::aprsEntity("G4ABC-9", "on the road", "/>"), pt(-180, -90)));               // corners are valid
    CHECK(gw.point(geo::aprsEntity("G4ABC-9", "on the road", "/>"), pt(-180, -90)));
    CHECK(kw.finish() && gw.finish());
    const Xml x = checkXml(k.str());
    CHECK(x.ok);
    if (!x.ok) { std::printf("  kml: %s\n", x.why.c_str()); }
    CHECK(x.count["Placemark"] == 4);
    CHECK(x.coordinates.size() == 4);
    if (x.coordinates.size() == 4) {
        CHECK(x.coordinates[0] == "100.25,10.5,11277.6");
        CHECK(x.coordinates[1] == "-0.1278,51.5074");
        CHECK(x.coordinates[2] == "0,0,-12");
        CHECK(x.coordinates[3] == "-180,-90");
    }
    CHECK(k.str().find("<when>2009-02-13T23:31:30.123Z</when>") != std::string::npos);
    CHECK(k.str().find("<kml xmlns=\"http://www.opengis.net/kml/2.2\">") != std::string::npos);
    CHECK(x.count["Document"] == 1);
    CHECK(k.str().find("<styleUrl>#aircraft</styleUrl>") != std::string::npos);

    const json j = json::parse(g.str(), nullptr, false);
    CHECK(j.is_object());
    CHECK(j["type"] == "FeatureCollection");
    CHECK(j["features"].size() == 4);
    CHECK(j["features"][0]["geometry"]["type"] == "Point");
    CHECK(j["features"][0]["geometry"]["coordinates"] == json::parse("[100.25,10.5,11277.6]"));
    CHECK(j["features"][1]["geometry"]["coordinates"] == json::parse("[-0.1278,51.5074]"));
    CHECK(j["features"][2]["geometry"]["coordinates"] == json::parse("[0,0,-12]"));
    CHECK(j["features"][3]["geometry"]["coordinates"] == json::parse("[-180,-90]"));
    CHECK(j["features"][0]["properties"]["kind"] == "aircraft");
    CHECK(j["features"][0]["properties"]["icao"] == "406B3F");
    CHECK(j["features"][0]["properties"]["squawk"] == "7000");
    CHECK(j["features"][0]["properties"]["name"] == "BAW123");
    CHECK(j["features"][0]["properties"]["time"] == "2009-02-13T23:31:30.123Z");
    CHECK(j["features"][1]["properties"]["mmsi"] == "235012345");
    CHECK(j["features"][2]["properties"]["type"] == "RS41");
    CHECK(j["features"][3]["properties"]["description"] == "on the road");
    CHECK(j["features"][3]["properties"]["symbol"] == "/>");
}

void testRejectedPoints() {
    std::ostringstream k, g;
    geo::KmlWriter kw(k, "x");
    geo::GeoJsonWriter gw(g);
    const geo::Entity e = geo::aircraftEntity("AAAAAA", "");
    const double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
    const geo::GeoPoint bad[] = {pt(nan, 0), pt(0, nan), pt(0, 90.0001), pt(0, -91), pt(180.5, 0), pt(-181, 0), pt(inf, 0), pt(0, 0, true, inf), pt(0, 0, true, nan)};
    for (const auto& p : bad) {
        CHECK(!kw.point(e, p));
        CHECK(!gw.point(e, p));
    }
    CHECK(kw.rejectedPoints() == 9 && gw.rejectedPoints() == 9);
    CHECK(kw.pointsWritten() == 0 && gw.pointsWritten() == 0);
    CHECK(kw.finish() && gw.finish());
    CHECK(checkXml(k.str()).count["Placemark"] == 0);
    const json j = json::parse(g.str());
    CHECK(j["features"].empty());   // an empty collection is valid
    // after finish nothing more is accepted
    CHECK(!kw.point(e, pt(1, 1)) && !gw.point(e, pt(1, 1)));
}

// ---- escaping ---------------------------------------------------------------------------------------------------------------

void testEscaping() {
    geo::Entity e = geo::aprsEntity("A&B<C>\"D'", "x]]></description></Placemark><Placemark>inject");
    e.name = std::string("n\"ame & <b> \x01\x02 ctl \xff\xfe bad \xC3\xA9 ok \xF0\x9F\x98\x80 \xED\xA0\x80 \xEF\xBF\xBE end");
    e.props.emplace_back("k\"ey<", "v&\"'<>");
    e.props.emplace_back("kind", "forged");   // a property cannot overwrite the reserved GeoJSON members
    std::ostringstream k, g;
    geo::KmlWriter kw(k, "doc & <name>");
    geo::GeoJsonWriter gw(g);
    CHECK(kw.point(e, pt(1, 2)));
    CHECK(gw.point(e, pt(1, 2)));
    CHECK(kw.finish() && gw.finish());
    const Xml x = checkXml(k.str());
    CHECK(x.ok);
    if (!x.ok) { std::printf("  kml: %s\n", x.why.c_str()); }
    CHECK(x.count["Placemark"] == 1);            // the injection attempt made no second Placemark
    CHECK(k.str().find("</Placemark><Placemark>inject") == std::string::npos);
    CHECK(k.str().find("\x01") == std::string::npos);
    CHECK(k.str().find("\xC3\xA9") != std::string::npos);          // valid UTF-8 kept
    CHECK(k.str().find("\xF0\x9F\x98\x80") != std::string::npos);
    CHECK(k.str().find("\xEF\xBF\xBD") != std::string::npos);      // invalid bytes became U+FFFD
    CHECK(k.str().find("\xEF\xBF\xBE") == std::string::npos);      // U+FFFE is not an XML Char
    CHECK(k.str().find("<Data name=\"k&quot;ey&lt;\">") != std::string::npos);
    const json j = json::parse(g.str(), nullptr, false);
    CHECK(j.is_object());
    if (j.is_object()) {
        CHECK(j["features"].size() == 1);
        CHECK(j["features"][0]["properties"]["kind"] == "aprs");   // not "forged"
        CHECK(j["features"][0]["properties"]["description"] == "x]]></description></Placemark><Placemark>inject");
        CHECK(j["features"][0]["properties"]["k\"ey<"] == "v&\"'<>");
        CHECK(j["features"][0]["properties"]["name"].get<std::string>().find("\xEF\xBF\xBD") != std::string::npos);
    }
    for (char c : g.str()) { CHECK(static_cast<unsigned char>(c) >= 0x20 || c == '\n'); }
    // xmlEscape alone
    CHECK(geo::detail::xmlEscape("a<b&c>d\"e'f") == "a&lt;b&amp;c&gt;d&quot;e&apos;f");
    CHECK(geo::detail::xmlEscape("") == "");
    CHECK(geo::detail::xmlEscape("tab\tnl\ncr\r") == "tab\tnl\ncr\r");
    CHECK(geo::detail::xmlEscape("\xC3") == "\xEF\xBF\xBD");        // truncated sequence at the end
    CHECK(geo::detail::xmlEscape("\xE2\x82") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(geo::detail::xmlEscape("\xC0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD");   // overlong NUL
}

// ---- tracks -----------------------------------------------------------------------------------------------------------------

void testTrackBasics() {
    std::ostringstream k, g;
    geo::KmlWriter kw(k, "t");
    geo::GeoJsonWriter gw(g);
    const geo::Entity e = geo::aircraftEntity("406B3F", "BAW123");
    CHECK(kw.beginTrack(e) && gw.beginTrack(e));
    for (int i = 0; i < 5; ++i) {
        CHECK(kw.trackPoint(pt(10.0 + i, 50.0 + i * 0.5, true, 1000.0 * i)));
        CHECK(gw.trackPoint(pt(10.0 + i, 50.0 + i * 0.5, true, 1000.0 * i)));
    }
    CHECK(!kw.trackPoint(pt(std::nan(""), 0)) && !gw.trackPoint(pt(0, 200)));   // rejected inside a track, the track goes on
    CHECK(kw.trackPoint(pt(15, 52.5, true, 5000)) && gw.trackPoint(pt(15, 52.5, true, 5000)));
    CHECK(!kw.point(e, pt(1, 1)) && !gw.point(e, pt(1, 1)));                    // a point cannot be written inside a track
    CHECK(!kw.beginTrack(e) && !gw.beginTrack(e));                              // nor a second track
    CHECK(kw.endTrack() && gw.endTrack());
    CHECK(!kw.endTrack() && !gw.endTrack());
    CHECK(kw.finish() && gw.finish());
    CHECK(kw.tracksWritten() == 1 && kw.pointsWritten() == 6 && kw.rejectedPoints() == 1);
    const Xml x = checkXml(k.str());
    CHECK(x.ok);
    CHECK(x.count["LineString"] == 1 && x.count["MultiGeometry"] == 1 && x.count["Placemark"] == 1);
    CHECK(x.coordinates.size() == 1);
    if (!x.coordinates.empty()) {
        CHECK(x.coordinates[0] == "10,50,0 11,50.5,1000 12,51,2000 13,51.5,3000 14,52,4000 15,52.5,5000");
    }
    CHECK(k.str().find("<altitudeMode>absolute</altitudeMode>") != std::string::npos);
    const json j = json::parse(g.str(), nullptr, false);
    CHECK(j.is_object());
    CHECK(j["features"].size() == 1);
    const json& geom = j["features"][0]["geometry"];
    CHECK(geom["type"] == "MultiLineString");
    CHECK(geom["coordinates"].size() == 1 && geom["coordinates"][0].size() == 6);
    CHECK(geom["coordinates"][0][0] == json::parse("[10,50,0]"));
    CHECK(geom["coordinates"][0][5] == json::parse("[15,52.5,5000]"));
    CHECK(j["features"][0]["properties"]["name"] == "BAW123");
}

void testAntimeridian() {
    std::ostringstream k, g;
    geo::KmlWriter kw(k, "t");
    geo::GeoJsonWriter gw(g);
    const geo::Entity e = geo::shipEntity("123456789", "Dateline");
    CHECK(kw.beginTrack(e) && gw.beginTrack(e));
    const geo::GeoPoint path[] = {pt(178, -1), pt(179, 0), pt(-179, 2), pt(-178, 3), pt(-179.5, 3.5), pt(179.5, 4)};   // east across, then back west across
    for (const auto& p : path) { kw.trackPoint(p); gw.trackPoint(p); }
    CHECK(kw.endTrack() && gw.endTrack());
    CHECK(kw.finish() && gw.finish());
    const json j = json::parse(g.str(), nullptr, false);
    CHECK(j.is_object());
    const json& c = j["features"][0]["geometry"]["coordinates"];
    CHECK(c.size() == 3);   // three segments: before, between the crossings, after
    if (c.size() == 3) {
        CHECK(c[0].size() == 3 && c[1].size() == 5 && c[2].size() == 2);
        // 179,0 -> -179,2 : the edge is 1 degree from the first point of 2 total, so the crossing is at latitude 1
        CHECK(c[0][2] == json::parse("[180,1]"));
        CHECK(c[1][0] == json::parse("[-180,1]"));
        CHECK(c[1][3] == json::parse("[-179.5,3.5]"));   // -178,3 -> -179.5,3.5 does not cross
        // -179.5,3.5 -> 179.5,4 crosses back: -180.5 in unwrapped longitude, the edge is halfway, latitude 3.75
        CHECK(c[1][4] == json::parse("[-180,3.75]"));
        CHECK(c[2][0] == json::parse("[180,3.75]"));
        CHECK(c[2][1] == json::parse("[179.5,4]"));
    }
    for (const auto& seg : c) { CHECK(seg.size() >= 2); for (const auto& pos : seg) { CHECK(pos.at(0) >= -180 && pos.at(0) <= 180); } }
    const Xml x = checkXml(k.str());
    CHECK(x.ok);
    CHECK(x.count["LineString"] == 3);
    // a line never spans more than 180 degrees of longitude in any segment
    for (const auto& seg : c) {
        for (std::size_t i = 1; i < seg.size(); ++i) { CHECK(std::fabs(seg[i].at(0).get<double>() - seg[i - 1].at(0).get<double>()) <= 180.0); }
    }
}

void testShortTracks() {
    const geo::Entity e = geo::balloonEntity("S1", "");
    {   // zero points
        std::ostringstream k, g;
        geo::KmlWriter kw(k, "t");
        geo::GeoJsonWriter gw(g);
        CHECK(kw.beginTrack(e) && gw.beginTrack(e) && kw.endTrack() && gw.endTrack());
        CHECK(kw.finish() && gw.finish());
        const Xml x = checkXml(k.str());
        CHECK(x.ok && x.count["Placemark"] == 1 && x.count["LineString"] == 0 && x.count["MultiGeometry"] == 0);
        const json j = json::parse(g.str());
        CHECK(j["features"].size() == 1 && j["features"][0]["geometry"].is_null());   // unlocated feature (RFC 7946)
        CHECK(j["features"][0]["properties"]["serial"] == "S1");
    }
    {   // one point: a LineString needs two
        std::ostringstream k, g;
        geo::KmlWriter kw(k, "t");
        geo::GeoJsonWriter gw(g);
        CHECK(kw.beginTrack(e) && gw.beginTrack(e));
        CHECK(kw.trackPoint(pt(1, 2)) && gw.trackPoint(pt(1, 2)));
        CHECK(kw.endTrack() && gw.endTrack());
        CHECK(kw.finish() && gw.finish());
        CHECK(checkXml(k.str()).count["LineString"] == 0);
        const json j = json::parse(g.str());
        CHECK(j["features"][0]["geometry"].is_null());
    }
    {   // a track left open is closed by finish()
        std::ostringstream k, g;
        geo::KmlWriter kw(k, "t");
        geo::GeoJsonWriter gw(g);
        CHECK(kw.beginTrack(e) && gw.beginTrack(e));
        for (int i = 0; i < 3; ++i) { kw.trackPoint(pt(i, i)); gw.trackPoint(pt(i, i)); }
        CHECK(kw.finish() && gw.finish());
        CHECK(checkXml(k.str()).ok);
        const json j = json::parse(g.str(), nullptr, false);
        CHECK(j.is_object() && j["features"][0]["geometry"]["coordinates"][0].size() == 3);
    }
    {   // several features of every kind in one file
        std::ostringstream k, g;
        geo::KmlWriter kw(k, "mixed");
        geo::GeoJsonWriter gw(g);
        CHECK(kw.point(geo::aircraftEntity("A", "A"), pt(1, 1)) && gw.point(geo::aircraftEntity("A", "A"), pt(1, 1)));
        CHECK(kw.beginTrack(e) && gw.beginTrack(e));
        kw.trackPoint(pt(1, 1)); gw.trackPoint(pt(1, 1)); kw.trackPoint(pt(2, 2)); gw.trackPoint(pt(2, 2));
        CHECK(kw.endTrack() && gw.endTrack());
        CHECK(kw.point(geo::aprsEntity("Z", ""), pt(3, 3)) && gw.point(geo::aprsEntity("Z", ""), pt(3, 3)));
        CHECK(kw.finish() && gw.finish());
        CHECK(checkXml(k.str()).count["Placemark"] == 3);
        CHECK(json::parse(g.str())["features"].size() == 3);
    }
}

// ---- streaming ---------------------------------------------------------------------------------------------------------------

struct CountingBuf : std::streambuf {
    std::uint64_t bytes = 0;
    char buf[4096];
    CountingBuf() { setp(buf, buf + sizeof buf); }
    int overflow(int c) override { bytes += static_cast<std::uint64_t>(pptr() - pbase()); setp(buf, buf + sizeof buf); if (c != EOF) { *pptr() = static_cast<char>(c); pbump(1); } return c == EOF ? 0 : c; }
    int sync() override { bytes += static_cast<std::uint64_t>(pptr() - pbase()); setp(buf, buf + sizeof buf); return 0; }
};

void testStreamingMillionPoints() {
    constexpr int kN = 1000000;
    for (int which = 0; which < 2; ++which) {
        CountingBuf cb;
        std::ostream os(&cb);
        geo::KmlWriter kw(os, "big");
        geo::GeoJsonWriter gw(os);   // (the KML header went first; only the call counts below matter, the bytes are discarded)
        const geo::Entity e = geo::aircraftEntity("406B3F", "BIG");
        const auto t0 = std::chrono::steady_clock::now();
        long news = -1;
        if (which == 0) {
            CHECK(kw.beginTrack(e));
            CHECK(kw.trackPoint(pt(0, 0, true, 0)));
            CHECK(kw.trackPoint(pt(0.0001, 0.0001, true, 1)));
            {
                AllocProbe probe;
                for (int i = 2; i < kN; ++i) { kw.trackPoint(pt(std::sin(i * 3e-4) * 100.0, std::sin(i * 1e-4) * 60.0, true, i % 12000)); }
                news = probe.news();
            }
            CHECK(kw.endTrack());
            CHECK(kw.pointsWritten() == static_cast<std::size_t>(kN));
        } else {
            CHECK(gw.beginTrack(e));
            CHECK(gw.trackPoint(pt(0, 0, true, 0)));
            CHECK(gw.trackPoint(pt(0.0001, 0.0001, true, 1)));
            {
                AllocProbe probe;
                for (int i = 2; i < kN; ++i) { gw.trackPoint(pt(std::sin(i * 3e-4) * 100.0, std::sin(i * 1e-4) * 60.0, true, i % 12000)); }
                news = probe.news();
            }
            CHECK(gw.endTrack());
            CHECK(gw.pointsWritten() == static_cast<std::size_t>(kN));
        }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        os.flush();
        std::printf("  %s: %d points, %.1f MB written in %.2f s (%.1f M points/s), heap allocations during the stream: %ld\n", which == 0 ? "KML" : "GeoJSON", kN,
                    static_cast<double>(cb.bytes) / 1e6, sec, kN / sec / 1e6, news);
        CHECK(news == 0);
        CHECK(cb.bytes > static_cast<std::uint64_t>(kN) * 12);
    }
}

void testLargeTrackIsValid() {
    // 30000 points crossing the antimeridian many times: the whole GeoJSON parses and every segment has 2+ positions in range
    std::ostringstream g, k;
    geo::GeoJsonWriter gw(g);
    geo::KmlWriter kw(k, "x");
    const geo::Entity e = geo::shipEntity("1", "loop");
    gw.beginTrack(e);
    kw.beginTrack(e);
    for (int i = 0; i < 30000; ++i) {
        double lon = std::fmod(170.0 + i * 0.05, 360.0);
        if (lon > 180.0) { lon -= 360.0; }
        gw.trackPoint(pt(lon, 10.0 + std::sin(i * 0.01)));
        kw.trackPoint(pt(lon, 10.0 + std::sin(i * 0.01)));
    }
    gw.endTrack();
    kw.endTrack();
    gw.finish();
    kw.finish();
    const json j = json::parse(g.str(), nullptr, false);
    CHECK(j.is_object());
    if (!j.is_object()) { return; }
    const json& c = j["features"][0]["geometry"]["coordinates"];
    std::size_t total = 0;
    for (const auto& seg : c) {
        CHECK(seg.size() >= 2);
        for (std::size_t i = 0; i < seg.size(); ++i) {
            const double lon = seg[i][0].get<double>();
            CHECK(lon >= -180.0 && lon <= 180.0);
            if (i > 0) { CHECK(std::fabs(lon - seg[i - 1][0].get<double>()) <= 180.0); }
        }
        total += seg.size();
    }
    std::printf("  %zu segments, %zu positions for 30000 input points\n", c.size(), total);
    CHECK(c.size() >= 4);
    CHECK(total >= 30000);
    const Xml x = checkXml(k.str());
    CHECK(x.ok);
    CHECK(x.count["LineString"] == static_cast<int>(c.size()));
}

}  // namespace

int main() {
    std::printf("geo export\n");
    testPointsGolden();
    testRejectedPoints();
    testEscaping();
    testTrackBasics();
    testAntimeridian();
    testShortTracks();
    testStreamingMillionPoints();
    testLargeTrackIsValid();
    return testSummary("test_geo_export");
}
