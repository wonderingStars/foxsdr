// Tests for core/lookup_links.hpp: strict subjects, escaping, one table, everything marked UNVERIFIED.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/lookup_links.hpp"

#include <random>
#include <string>

#include "test_check.hpp"

namespace lk = cascade::core::links;

static bool cleanUrl(const std::string& u) {
    // https only, one scheme, no whitespace or control bytes, no userinfo, no second '?' or '#', ASCII only
    if (u.rfind("https://", 0) != 0) return false;
    if (u.find("://", 8) != std::string::npos) return false;
    int q = 0;
    for (unsigned char c : u) {
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '#') return false;
        if (c == '?') ++q;
    }
    const std::size_t hostEnd = u.find('/', 8);
    const std::string host = u.substr(8, hostEnd == std::string::npos ? std::string::npos : hostEnd - 8);
    if (host.find('@') != std::string::npos || host.find('{') != std::string::npos) return false;
    return q <= 1 && u.find("{}") == std::string::npos;
}

int main() {
    // the table: every pattern is https with exactly one placeholder, and NONE is claimed verified
    int total = 0;
    for (const lk::Site& s : lk::sites()) {
        ++total;
        const std::string p = s.pattern;
        CHECK(p.rfind("https://", 0) == 0);
        CHECK(p.find("{}") != std::string::npos && p.find("{}") == p.rfind("{}"));
        CHECK(!s.verified);
    }
    CHECK(total >= 8);

    // aircraft by ICAO address
    auto a = lk::build(lk::Subject::AircraftIcao, "4ca7b2");
    CHECK(a.size() == 3);
    bool sawUpper = false;
    for (const auto& l : a) {
        CHECK(!l.verified);
        CHECK(cleanUrl(l.url));
        CHECK(!l.label.empty() && !l.siteId.empty());
        if (l.url.find("4CA7B2") != std::string::npos) sawUpper = true;
    }
    CHECK(sawUpper);                                  // hex is upper-cased
    CHECK(a[0].url == "https://globe.adsbexchange.com/?icao=4CA7B2");

    // registration
    auto r = lk::build(lk::Subject::AircraftRegistration, "g-eupt");
    CHECK(r.size() == 2);
    CHECK(r[0].url == "https://www.flightradar24.com/data/aircraft/G-EUPT");

    // ship
    auto s = lk::build(lk::Subject::ShipMmsi, "235009999");
    CHECK(s.size() == 2);
    CHECK(s[0].url == "https://www.vesselfinder.com/?mmsi=235009999");

    // call sign, with a portable suffix: the slash is escaped
    auto c = lk::build(lk::Subject::CallSign, "w1aw/p");
    CHECK(c.size() == 3);
    CHECK(c[0].url == "https://www.qrz.com/db/W1AW%2FP");
    for (const auto& l : c) CHECK(cleanUrl(l.url));

    // hostile subjects give NO link rather than a wrong one
    for (const char* bad : {"", "4ca7b", "4ca7b22", "4ca7bg", "4ca7b2&x=1", "4ca7b2#", "../../etc", "4ca7b2%00", " 4ca7b2", "4ca7b2\n",
                             "4ca7b2/", "https://evil.example/", "\xC3\xA9" "a7b2"}) {
        CHECK(lk::build(lk::Subject::AircraftIcao, bad).empty());
    }
    for (const char* bad : {"", "G", "G EUPT", "G-EUPT?x=1", "G-EUPT/../x", "A@B.CD", "G-EUPT-EXTRA", "G\xC3\xA9"})
        CHECK(lk::build(lk::Subject::AircraftRegistration, bad).empty());
    for (const char* bad : {"", "23500999", "2350099999", "23500999a", "-23500999", "235 00999", "235009999\n"})
        CHECK(lk::build(lk::Subject::ShipMmsi, bad).empty());
    for (const char* bad : {"", "W1", "W1AW?x", "W1AW#x", "W1AW%2F", "W1AW@evil", "W1AW/../..", "THISCALLISTOOLONGX", "W1\x01W"})
        CHECK(lk::build(lk::Subject::CallSign, bad).empty());
    std::string nul("W1AW", 4);
    nul += '\0';
    CHECK(lk::build(lk::Subject::CallSign, nul).empty());

    // random hostile subjects: any link that is produced is clean
    std::mt19937 rng(99);
    const std::string alphabet = "AB09f/?#&=%:@.\\ \"<>-\x01\xff";
    int produced = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string t;
        const int n = 1 + static_cast<int>(rng() % 12);
        for (int k = 0; k < n; ++k) t += alphabet[rng() % alphabet.size()];
        for (auto kind : {lk::Subject::AircraftIcao, lk::Subject::AircraftRegistration, lk::Subject::ShipMmsi, lk::Subject::CallSign})
            for (const auto& l : lk::build(kind, t)) {
                ++produced;
                CHECK(cleanUrl(l.url));
            }
    }
    CHECK(produced > 0);

    // percent encoding directly
    CHECK(lk::percentEncode("a b/c?d#e&f=g%h") == "a%20b%2Fc%3Fd%23e%26f%3Dg%25h");
    CHECK(lk::percentEncode("AZaz09-._~") == "AZaz09-._~");
    CHECK(lk::percentEncode(std::string("\0\xff", 2)) == "%00%FF");

    return testSummary("test_lookup_links");
}
