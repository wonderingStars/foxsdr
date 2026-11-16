// s4b_listing.cpp - see s4b_listing.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_listing.hpp"

#include <cmath>
#include <cstdlib>

namespace cascade::engine::s4b {

namespace {

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) { --b; }
    return s.substr(a, b - a);
}

bool number(const std::string& text, double& out) {
    const std::string t = trimmed(text);
    if (t.empty() || t.size() > 24) { return false; }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}

}  // namespace

bool buildListing(const ListingText& in, cascade::net::ReceiverListing& out, std::string& problem) {
    problem.clear();
    out = cascade::net::ReceiverListing{};
    out.enabled = in.enabled;
    out.name = trimmed(in.name);
    out.location = trimmed(in.location);
    out.version = in.version;
    if (!in.enabled) { return true; }
    // position: both or neither
    const std::string lat = trimmed(in.latitude), lon = trimmed(in.longitude);
    if (!lat.empty() || !lon.empty()) {
        if (lat.empty() || lon.empty()) { problem = "type both the latitude and the longitude, or neither"; return false; }
        if (!number(lat, out.latitude) || !number(lon, out.longitude)) { problem = "the latitude and the longitude are numbers in degrees"; return false; }
        out.hasPosition = true;
    }
    double mc = 0.0;
    if (!number(in.maxClients, mc) || mc != std::floor(mc) || mc < 1.0 || mc > 1000.0) { problem = "the listener limit is a whole number from 1 to 1000"; return false; }
    out.maxClients = static_cast<int>(mc);
    // the bands
    std::size_t from = 0, index = 0;
    const std::string& b = in.bands;
    while (from <= b.size() && !trimmed(b).empty()) {
        std::size_t to = b.find_first_of("|\n", from);
        if (to == std::string::npos) { to = b.size(); }
        const std::string line = trimmed(b.substr(from, to - from));
        from = to + 1;
        if (line.empty()) { continue; }
        ++index;
        const std::string where = "band " + std::to_string(index) + ": ";
        const std::size_t s1 = line.find(';');
        const std::size_t s2 = s1 == std::string::npos ? std::string::npos : line.find(';', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos || line.find(';', s2 + 1) != std::string::npos) { problem = where + "write a name, the centre in MHz and the sample rate in MHz, separated by semicolons"; return false; }
        cascade::net::ListedBand band;
        band.name = trimmed(line.substr(0, s1));
        double centreMhz = 0.0, rateMhz = 0.0;
        if (!number(line.substr(s1 + 1, s2 - s1 - 1), centreMhz) || !number(line.substr(s2 + 1), rateMhz)) { problem = where + "the centre and the sample rate are numbers in MHz"; return false; }
        band.centerHz = std::round(centreMhz * 1e6);
        band.sampleRateHz = std::round(rateMhz * 1e6);
        out.bands.push_back(std::move(band));
        if (out.bands.size() > cascade::net::kListingMaxBands) { problem = "at most 32 bands can be listed"; return false; }
    }
    problem = cascade::net::validateListing(out);
    return problem.empty();
}

}  // namespace cascade::engine::s4b
