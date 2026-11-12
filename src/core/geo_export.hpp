// geo_export - KML 2.2 and GeoJSON (RFC 7946) writers for tracks and points: aircraft, ships, radiosonde balloons, APRS stations.
//
// STANDALONE. Nothing here is wired; the wiring hand-off is in docs/feature-records/core-formats.md. The writers take plain input
// structures (no dependency on the map, the plugin host or the engine) and write to a std::ostream.
//
// STREAMING. A track is written point by point (beginTrack / trackPoint / endTrack) and nothing is kept but the last point and one
// held point per segment: a track of any length costs O(1) memory, and trackPoint() performs no heap allocation (proved in the test
// with the repo's allocation probe over a million points).
//
// SOURCES (specifications only; clean room)
//  * OGC KML 2.2 (OGC 07-147r2), https://www.ogc.org/standards/kml/ : <kml xmlns="http://www.opengis.net/kml/2.2">, Document, Placemark,
//    Point and LineString with <coordinates> as "longitude,latitude[,altitude]" tuples separated by white space, longitude FIRST,
//    degrees, altitude in metres, <altitudeMode>absolute</altitudeMode>, MultiGeometry, Style / LineStyle / color (aabbggrr),
//    TimeStamp / when (xsd:dateTime). A LineString needs at least two coordinates. Not opened this session; from memory of the
//    standard, so the exact element order inside Placemark is UNVERIFIED against the schema (the order used is the one the standard's
//    own examples use: name, description, styleUrl, TimeStamp, geometry).
//  * RFC 7946 (The GeoJSON Format), https://www.rfc-editor.org/rfc/rfc7946 : position = [longitude, latitude(, altitude)] in WGS 84,
//    longitude first; a LineString has two or more positions; "geometry" may be null for an unlocated Feature; a FeatureCollection holds
//    "features"; section 3.1.9 advises cutting a geometry at the antimeridian (done here: a segment is closed at +-180 and the next
//    starts at the opposite edge, so tracks are MultiLineString); members may appear in any order. Not opened this session (from
//    memory), UNVERIFIED against the text.
//  * XML 1.0 character range (production Char): tab, LF, CR, U+0020..U+D7FF, U+E000..U+FFFD, U+10000..U+10FFFF; everything else is
//    dropped here. JSON strings are escaped by nlohmann::json (RFC 8259) with invalid UTF-8 replaced by U+FFFD.
//
// NUMBERS are written with std::to_chars (locale independent), fixed notation, 7 decimals for longitude / latitude (about 1 cm) and 1
// decimal for altitude, trailing zeros removed. Points with a non-finite value or a latitude outside -90..90 or longitude outside
// -180..180 are REJECTED and counted, never written.
//
// THREADS: one writer, one thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::geo {

enum class Kind : int { Aircraft, Ship, Balloon, Aprs, Other };

inline const char* kindName(Kind k) {
    switch (k) { case Kind::Aircraft: return "aircraft"; case Kind::Ship: return "ship"; case Kind::Balloon: return "balloon"; case Kind::Aprs: return "aprs"; default: return "other"; }
}

struct GeoPoint {
    double lonDeg = 0.0;
    double latDeg = 0.0;
    bool hasAlt = false;
    double altM = 0.0;
    std::int64_t unixMs = -1;     // < 0 = no time
};

// What is being drawn. `id` is the identifier a person would search for (ICAO hex, MMSI, sonde serial, callsign-SSID).
struct Entity {
    Kind kind = Kind::Other;
    std::string id;
    std::string name;             // label; the id is used when empty
    std::string description;
    std::vector<std::pair<std::string, std::string>> props;   // extra named values (GeoJSON properties / KML ExtendedData)
};

// Input-structure helpers: what each kind of thing carries, mapped to an Entity.
inline Entity aircraftEntity(const std::string& icaoHex, const std::string& callsign, const std::string& squawk = {}) {
    Entity e;
    e.kind = Kind::Aircraft;
    e.id = icaoHex;
    e.name = callsign.empty() ? icaoHex : callsign;
    e.props.emplace_back("icao", icaoHex);
    if (!callsign.empty()) { e.props.emplace_back("callsign", callsign); }
    if (!squawk.empty()) { e.props.emplace_back("squawk", squawk); }
    return e;
}
inline Entity shipEntity(const std::string& mmsi, const std::string& shipName) {
    Entity e;
    e.kind = Kind::Ship;
    e.id = mmsi;
    e.name = shipName.empty() ? mmsi : shipName;
    e.props.emplace_back("mmsi", mmsi);
    return e;
}
inline Entity balloonEntity(const std::string& serial, const std::string& sondeType) {
    Entity e;
    e.kind = Kind::Balloon;
    e.id = serial;
    e.name = serial;
    e.props.emplace_back("serial", serial);
    if (!sondeType.empty()) { e.props.emplace_back("type", sondeType); }
    return e;
}
inline Entity aprsEntity(const std::string& callsign, const std::string& comment, const std::string& symbol = {}) {
    Entity e;
    e.kind = Kind::Aprs;
    e.id = callsign;
    e.name = callsign;
    e.description = comment;
    if (!symbol.empty()) { e.props.emplace_back("symbol", symbol); }
    return e;
}

namespace detail {

inline bool valid(const GeoPoint& p) {
    return std::isfinite(p.lonDeg) && std::isfinite(p.latDeg) && p.latDeg >= -90.0 && p.latDeg <= 90.0 && p.lonDeg >= -180.0 && p.lonDeg <= 180.0 &&
           (!p.hasAlt || (std::isfinite(p.altM) && std::fabs(p.altM) < 1.0e9));
}

// Fixed notation with `decimals` places, trailing zeros (and a bare point) removed, "-0" written as "0". Returns the length.
inline std::size_t number(char* buf, std::size_t cap, double v, int decimals) {
    auto r = std::to_chars(buf, buf + cap, v, std::chars_format::fixed, decimals);
    std::size_t n = static_cast<std::size_t>(r.ptr - buf);
    if (std::memchr(buf, '.', n) != nullptr) {
        while (n > 0 && buf[n - 1] == '0') { --n; }
        if (n > 0 && buf[n - 1] == '.') { --n; }
    }
    if (n == 2 && buf[0] == '-' && buf[1] == '0') { buf[0] = '0'; n = 1; }
    if (n == 0) { buf[0] = '0'; n = 1; }
    return n;
}

// "lon,lat" or "lon,lat,alt" (KML) into buf; with `json` it is "[lon,lat]" / "[lon,lat,alt]". Returns the length. buf >= 96 bytes.
inline std::size_t position(char* buf, const GeoPoint& p, bool json) {
    std::size_t n = 0;
    if (json) { buf[n++] = '['; }
    n += number(buf + n, 40, p.lonDeg, 7);
    buf[n++] = ',';
    n += number(buf + n, 40, p.latDeg, 7);
    if (p.hasAlt) {
        buf[n++] = ',';
        n += number(buf + n, 30, p.altM, 1);
    }
    if (json) { buf[n++] = ']'; }
    buf[n] = '\0';
    return n;
}

// Longitude of the antimeridian crossing: from a to b with |dlon| > 180. Returns the crossing latitude by interpolating in unwrapped longitude.
inline double crossingLat(const GeoPoint& a, const GeoPoint& b) {
    const double lonB = b.lonDeg + (a.lonDeg > 0 ? 360.0 : -360.0);   // b seen on a's side
    const double edge = a.lonDeg > 0 ? 180.0 : -180.0;
    const double t = (edge - a.lonDeg) / (lonB - a.lonDeg);
    return a.latDeg + t * (b.latDeg - a.latDeg);
}

// XML 1.0 text: &, <, >, " and ' escaped; invalid UTF-8 becomes U+FFFD; characters outside the XML Char production are dropped.
inline std::string xmlEscape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        bool ok = true;
        if (c < 0x80) {
            cp = c;
        } else {
            std::size_t need = 0;
            std::uint32_t minCp = 0;
            if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; minCp = 0x80; }
            else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; minCp = 0x800; }
            else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; minCp = 0x10000; }
            else { ok = false; }
            if (ok) {
                if (i + need >= s.size()) { ok = false; }
                for (std::size_t k = 1; ok && k <= need; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (ok && (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) { ok = false; }
                if (ok) { len = need + 1; }
            }
        }
        if (!ok) { o += "\xEF\xBF\xBD"; i += 1; continue; }
        i += len;
        const bool xmlChar = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
        if (!xmlChar) { continue; }
        switch (cp) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o.append(s.substr(i - len, len));
        }
    }
    return o;
}

// "2026-11-11T16:36:45.123Z" from Unix milliseconds.
inline std::string isoUtc(std::int64_t ms) {
    if (ms < 0) { ms = 0; }
    const std::int64_t secs = ms / 1000;
    const int milli = static_cast<int>(ms % 1000);
    std::int64_t days = secs / 86400 + 719468;
    const std::int64_t rem = secs % 86400;
    const std::int64_t era = days / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) { ++y; }
    char b[40];
    std::snprintf(b, sizeof b, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(y), m, d, static_cast<int>(rem / 3600),
                  static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60), milli);
    return b;
}

inline std::string jdump(const nlohmann::json& j) { return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }

}  // namespace detail

// ---- KML --------------------------------------------------------------------------------------------------------------------

class KmlWriter {
public:
    KmlWriter(std::ostream& os, const std::string& documentName, const std::string& documentDescription = {}) : os_(os) {
        os_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n";
        os_ << "<name>" << detail::xmlEscape(documentName) << "</name>\n";
        if (!documentDescription.empty()) { os_ << "<description>" << detail::xmlEscape(documentDescription) << "</description>\n"; }
        // aabbggrr
        style("aircraft", "ff00a5ff");
        style("ship", "ffff7f00");
        style("balloon", "ff00d7ff");
        style("aprs", "ff00ff00");
        style("other", "ffffffff");
    }

    bool point(const Entity& e, const GeoPoint& p) {
        if (!usable()) { return false; }
        if (inTrack_) { return false; }
        if (!detail::valid(p)) { ++rejected_; return false; }
        placemarkHead(e, p.unixMs);
        char b[96];
        detail::position(b, p, false);
        os_ << "<Point><coordinates>" << b << "</coordinates>" << "</Point>\n</Placemark>\n";
        ++points_;
        return static_cast<bool>(os_);
    }

    bool beginTrack(const Entity& e) {
        if (!usable() || inTrack_) { return false; }
        inTrack_ = true;
        entity_ = e;
        haveLast_ = false;
        held_ = false;
        segments_ = 0;
        segOpen_ = false;
        placemarkHeadOpen_ = false;
        return true;
    }

    // Returns false for a rejected point (counted); the track continues.
    bool trackPoint(const GeoPoint& p) {
        if (!inTrack_) { return false; }
        if (!detail::valid(p)) { ++rejected_; return false; }
        if (haveLast_ && std::fabs(p.lonDeg - last_.lonDeg) > 180.0) {
            // crossing the antimeridian: close this segment at the edge, start the next at the opposite edge
            GeoPoint a = last_, b = last_;
            const double lat = detail::crossingLat(last_, p);
            a.lonDeg = last_.lonDeg > 0 ? 180.0 : -180.0;
            a.latDeg = lat;
            b.lonDeg = -a.lonDeg;
            b.latDeg = lat;
            addToSegment(a);
            endSegment();
            addToSegment(b);
        }
        addToSegment(p);
        last_ = p;
        haveLast_ = true;
        ++points_;
        return static_cast<bool>(os_);
    }

    bool endTrack() {
        if (!inTrack_) { return false; }
        endSegment();
        if (!placemarkHeadOpen_) { placemarkHead(entity_, -1); }   // no drawable segment: a Placemark with no geometry
        else { os_ << "</MultiGeometry>\n"; }
        os_ << "</Placemark>\n";
        inTrack_ = false;
        ++tracks_;
        return static_cast<bool>(os_);
    }

    bool finish() {
        if (finished_) { return true; }
        if (inTrack_) { endTrack(); }
        os_ << "</Document>\n</kml>\n";
        finished_ = true;
        return static_cast<bool>(os_);
    }

    std::size_t rejectedPoints() const { return rejected_; }
    std::size_t pointsWritten() const { return points_; }
    std::size_t tracksWritten() const { return tracks_; }

private:
    bool usable() const { return !finished_ && static_cast<bool>(os_); }

    void style(const char* id, const char* colour) {
        os_ << "<Style id=\"" << id << "\"><LineStyle><color>" << colour << "</color><width>2</width></LineStyle><IconStyle><color>" << colour
            << "</color><scale>0.8</scale></IconStyle></Style>\n";
    }

    void placemarkHead(const Entity& e, std::int64_t unixMs) {
        os_ << "<Placemark>\n<name>" << detail::xmlEscape(e.name.empty() ? e.id : e.name) << "</name>\n";
        if (!e.description.empty()) { os_ << "<description>" << detail::xmlEscape(e.description) << "</description>\n"; }
        os_ << "<styleUrl>#" << kindName(e.kind) << "</styleUrl>\n";
        if (unixMs >= 0) { os_ << "<TimeStamp><when>" << detail::isoUtc(unixMs) << "</when></TimeStamp>\n"; }
        if (!e.id.empty() || !e.props.empty()) {
            os_ << "<ExtendedData>\n";
            if (!e.id.empty()) { os_ << "<Data name=\"id\"><value>" << detail::xmlEscape(e.id) << "</value></Data>\n"; }
            os_ << "<Data name=\"kind\"><value>" << kindName(e.kind) << "</value></Data>\n";
            for (const auto& kv : e.props) {
                os_ << "<Data name=\"" << detail::xmlEscape(kv.first) << "\"><value>" << detail::xmlEscape(kv.second) << "</value></Data>\n";
            }
            os_ << "</ExtendedData>\n";
        }
    }

    void addToSegment(const GeoPoint& p) {
        if (!segOpen_) {
            if (!held_) { heldPt_ = p; held_ = true; return; }
            // second point of a segment: now the segment is drawable
            if (!placemarkHeadOpen_) { placemarkHead(entity_, -1); os_ << "<MultiGeometry>\n"; placemarkHeadOpen_ = true; }
            os_ << "<LineString><tessellate>1</tessellate>";
            if (heldPt_.hasAlt || p.hasAlt) { os_ << "<altitudeMode>absolute</altitudeMode>"; }
            os_ << "<coordinates>";
            char b[96];
            detail::position(b, heldPt_, false);
            os_ << b;
            segOpen_ = true;
            held_ = false;
            ++segments_;
        }
        char b[96];
        detail::position(b, p, false);
        os_ << ' ' << b;
    }

    void endSegment() {
        if (segOpen_) { os_ << "</coordinates></LineString>\n"; segOpen_ = false; }
        held_ = false;   // a lone point is not a line
    }

    std::ostream& os_;
    Entity entity_;
    GeoPoint last_, heldPt_;
    bool inTrack_ = false, haveLast_ = false, held_ = false, segOpen_ = false, placemarkHeadOpen_ = false, finished_ = false;
    std::size_t rejected_ = 0, points_ = 0, tracks_ = 0, segments_ = 0;
};

// ---- GeoJSON ------------------------------------------------------------------------------------------------------------------

class GeoJsonWriter {
public:
    explicit GeoJsonWriter(std::ostream& os) : os_(os) { os_ << "{\"type\":\"FeatureCollection\",\"features\":["; }

    bool point(const Entity& e, const GeoPoint& p) {
        if (!usable() || inTrack_) { return false; }
        if (!detail::valid(p)) { ++rejected_; return false; }
        sep();
        char b[96];
        detail::position(b, p, true);
        os_ << "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":" << b << "},\"properties\":" << detail::jdump(properties(e, p.unixMs)) << "}";
        ++points_;
        return static_cast<bool>(os_);
    }

    bool beginTrack(const Entity& e) {
        if (!usable() || inTrack_) { return false; }
        inTrack_ = true;
        entity_ = e;
        haveLast_ = false;
        held_ = false;
        segOpen_ = false;
        featureOpen_ = false;
        firstSeg_ = true;
        return true;
    }

    bool trackPoint(const GeoPoint& p) {
        if (!inTrack_) { return false; }
        if (!detail::valid(p)) { ++rejected_; return false; }
        if (haveLast_ && std::fabs(p.lonDeg - last_.lonDeg) > 180.0) {
            GeoPoint a = last_, b = last_;
            const double lat = detail::crossingLat(last_, p);
            a.lonDeg = last_.lonDeg > 0 ? 180.0 : -180.0;
            a.latDeg = lat;
            b.lonDeg = -a.lonDeg;
            b.latDeg = lat;
            addToSegment(a);
            endSegment();
            addToSegment(b);
        }
        addToSegment(p);
        last_ = p;
        haveLast_ = true;
        ++points_;
        return static_cast<bool>(os_);
    }

    bool endTrack() {
        if (!inTrack_) { return false; }
        endSegment();
        if (featureOpen_) {
            os_ << "]},\"properties\":" << detail::jdump(properties(entity_, -1)) << "}";
        } else {
            sep();
            os_ << "{\"type\":\"Feature\",\"geometry\":null,\"properties\":" << detail::jdump(properties(entity_, -1)) << "}";   // RFC 7946: unlocated
        }
        inTrack_ = false;
        ++tracks_;
        return static_cast<bool>(os_);
    }

    bool finish() {
        if (finished_) { return true; }
        if (inTrack_) { endTrack(); }
        os_ << "]}\n";
        finished_ = true;
        return static_cast<bool>(os_);
    }

    std::size_t rejectedPoints() const { return rejected_; }
    std::size_t pointsWritten() const { return points_; }
    std::size_t tracksWritten() const { return tracks_; }

private:
    bool usable() const { return !finished_ && static_cast<bool>(os_); }
    void sep() { if (count_++ > 0) { os_ << ','; } }

    nlohmann::json properties(const Entity& e, std::int64_t unixMs) const {
        nlohmann::json j = nlohmann::json::object();
        j["kind"] = kindName(e.kind);
        if (!e.id.empty()) { j["id"] = e.id; }
        j["name"] = e.name.empty() ? e.id : e.name;
        if (!e.description.empty()) { j["description"] = e.description; }
        if (unixMs >= 0) { j["time"] = detail::isoUtc(unixMs); }
        for (const auto& kv : e.props) { if (kv.first != "kind" && kv.first != "name" && kv.first != "id" && kv.first != "time" && kv.first != "description") { j[kv.first] = kv.second; } }
        return j;
    }

    void addToSegment(const GeoPoint& p) {
        if (!segOpen_) {
            if (!held_) { heldPt_ = p; held_ = true; return; }
            if (!featureOpen_) {
                sep();
                os_ << "{\"type\":\"Feature\",\"geometry\":{\"type\":\"MultiLineString\",\"coordinates\":[";
                featureOpen_ = true;
                firstSeg_ = true;
            }
            os_ << (firstSeg_ ? "[" : ",[");
            firstSeg_ = false;
            char b[96];
            detail::position(b, heldPt_, true);
            os_ << b;
            segOpen_ = true;
            held_ = false;
        }
        char b[96];
        detail::position(b, p, true);
        os_ << ',' << b;
    }

    void endSegment() {
        if (segOpen_) { os_ << ']'; segOpen_ = false; }
        held_ = false;
    }

    std::ostream& os_;
    Entity entity_;
    GeoPoint last_, heldPt_;
    bool inTrack_ = false, haveLast_ = false, held_ = false, segOpen_ = false, featureOpen_ = false, firstSeg_ = true, finished_ = false;
    std::size_t rejected_ = 0, points_ = 0, tracks_ = 0, count_ = 0;
};

}  // namespace cascade::core::geo
