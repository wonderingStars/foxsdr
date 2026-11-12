// lookup_links.hpp - builds web links for an aircraft (ICAO address or registration), a ship (MMSI) and a call sign (row M5 of
// the gaps survey). NOTHING IS FETCHED: this makes strings; the host's existing open-URL call (the wiring lane) opens one on a
// click.
//
// EVERY SITE PATTERN LIVES IN ONE TABLE (kSites below). Each row says whether the pattern is documented by the site itself.
// NONE IS: no site's own documentation of a link pattern was read in this session, so every row is UNVERIFIED and is marked so
// in the result (Link::verified == false); the screen should say "may not open the right page". The patterns are from memory of
// links the sites are known to use and can change without notice. Updating a pattern is a one-line edit of the table.
//
// ESCAPING. The subject is first validated against a strict alphabet for its kind (below), then percent-encoded byte by byte for
// everything outside the RFC 3986 unreserved set (letters, digits, "-", ".", "_", "~"), so a subject can never add a path
// segment, a query parameter, a fragment or a second scheme. Only https URLs are produced, from the literal table text.
//
// SUBJECT ALPHABETS: ICAO 24-bit address = exactly 6 hex digits; registration = 2 to 10 of A-Z 0-9 and "-"; MMSI = exactly 9
// digits; call sign = 3 to 12 of A-Z 0-9 and "/" (portable suffixes), the "/" is percent-encoded.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core::links {

enum class Subject { AircraftIcao, AircraftRegistration, ShipMmsi, CallSign };

struct Site {
    Subject subject;
    const char* id;        // stable key
    const char* label;     // shown to the user
    const char* pattern;   // https URL with one "{}" where the escaped subject goes
    bool verified;         // true only if the site's own documentation gives the pattern
    const char* note;
};

// The single table. verified == false everywhere: see the header comment.
inline const std::vector<Site>& sites() {
    static const std::vector<Site> k = {
        {Subject::AircraftIcao, "adsbx-icao", "ADS-B Exchange", "https://globe.adsbexchange.com/?icao={}", false, "from memory"},
        {Subject::AircraftIcao, "planespotters-hex", "Planespotters", "https://www.planespotters.net/hex/{}", false, "from memory"},
        {Subject::AircraftIcao, "flightaware-modes", "FlightAware", "https://flightaware.com/live/modes/{}/redirect", false,
         "from memory"},
        {Subject::AircraftRegistration, "fr24-reg", "Flightradar24", "https://www.flightradar24.com/data/aircraft/{}", false,
         "from memory"},
        {Subject::AircraftRegistration, "flightaware-reg", "FlightAware", "https://flightaware.com/resources/registration/{}",
         false, "from memory"},
        {Subject::ShipMmsi, "vesselfinder-mmsi", "VesselFinder", "https://www.vesselfinder.com/?mmsi={}", false, "from memory"},
        {Subject::ShipMmsi, "marinetraffic-mmsi", "MarineTraffic", "https://www.marinetraffic.com/en/ais/details/ships/mmsi:{}",
         false, "from memory"},
        {Subject::CallSign, "qrz", "QRZ.com", "https://www.qrz.com/db/{}", false, "from memory"},
        {Subject::CallSign, "hamqth", "HamQTH", "https://www.hamqth.com/{}", false, "from memory"},
        {Subject::CallSign, "callook", "Callook (US)", "https://callook.info/{}", false, "from memory"},
    };
    return k;
}

struct Link {
    std::string siteId;
    std::string label;
    std::string url;
    bool verified = false;
};

namespace detail {

inline bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
inline bool isAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }

inline std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

inline bool valid(Subject kind, std::string_view s) {
    switch (kind) {
        case Subject::AircraftIcao:
            if (s.size() != 6) return false;
            for (char c : s)
                if (!isHex(c)) return false;
            return true;
        case Subject::AircraftRegistration:
            if (s.size() < 2 || s.size() > 10) return false;
            for (char c : s)
                if (!isAlnum(c) && c != '-') return false;
            return true;
        case Subject::ShipMmsi:
            if (s.size() != 9) return false;
            for (char c : s)
                if (c < '0' || c > '9') return false;
            return true;
        case Subject::CallSign:
            if (s.size() < 3 || s.size() > 12) return false;
            for (char c : s)
                if (!isAlnum(c) && c != '/') return false;
            return true;
    }
    return false;
}

}  // namespace detail

// Percent-encodes everything but the RFC 3986 unreserved characters.
inline std::string percentEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '_' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// Links for one subject. An invalid subject (wrong length or alphabet) returns an empty list: no link is better than a wrong one.
// The subject is upper-cased for registrations, call signs; ICAO addresses are upper-cased hex.
inline std::vector<Link> build(Subject kind, std::string_view subject) {
    std::vector<Link> out;
    const std::string s = detail::upper(subject);
    if (!detail::valid(kind, s)) return out;
    const std::string enc = percentEncode(s);
    for (const Site& site : sites()) {
        if (site.subject != kind) continue;
        std::string url = site.pattern;
        const std::size_t at = url.find("{}");
        if (at == std::string::npos) continue;
        url.replace(at, 2, enc);
        out.push_back({site.id, site.label, std::move(url), site.verified});
    }
    return out;
}

}  // namespace cascade::core::links
