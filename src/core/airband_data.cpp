// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/airband_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "core/airband_assets.hpp"

namespace cascade::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'a' && x <= 'z') { x = static_cast<char>(x - 'a' + 'A'); }
        if (y >= 'a' && y <= 'z') { y = static_cast<char>(y - 'a' + 'A'); }
        if (x != y) { return false; }
    }
    return true;
}

// Splits one line on tabs, keeping empty fields (an airport with no IATA code
// has two tabs in a row, and dropping the empty field would shift every
// column after it).
std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool parseDouble(std::string_view s, double& out) {
    const std::string tmp(trim(s));
    if (tmp.empty()) { return false; }
    char* end = nullptr;
    out = std::strtod(tmp.c_str(), &end);
    return end == tmp.c_str() + tmp.size() && std::isfinite(out);
}

bool parseInt(std::string_view s, int& out) {
    const std::string tmp(trim(s));
    if (tmp.empty()) { return false; }
    char* end = nullptr;
    const long v = std::strtol(tmp.c_str(), &end, 10);
    if (end != tmp.c_str() + tmp.size()) { return false; }
    out = static_cast<int>(v);
    return true;
}

}  // namespace

bool airband833(int channelKhz) {
    const int r = channelKhz % 25;
    return r == 5 || r == 10 || r == 15;
}

double airbandChannelHz(int channelKhz) {
    const int r = channelKhz % 25;
    const int slotKhz = channelKhz - r;
    // The three 8.33 kHz channels in a 25 kHz slot are NAMED .x05, .x10 and
    // .x15 (and .x30/.x35/.x40 ... in the next slots) and sit 0, 25/3 and
    // 50/3 kHz above the slot. The .x00 name is the 25 kHz channel itself.
    switch (r) {
        case 5: return static_cast<double>(slotKhz) * 1000.0;
        case 10: return static_cast<double>(slotKhz) * 1000.0 + 25000.0 / 3.0;
        case 15: return static_cast<double>(slotKhz) * 1000.0 + 50000.0 / 3.0;
        default: return static_cast<double>(channelKhz) * 1000.0;
    }
}

std::vector<Airport> parseAirbandTable(std::string_view text, std::size_t* skipped) {
    std::vector<Airport> out;
    std::size_t bad = 0;
    bool haveAirport = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) { nl = text.size(); }
        const std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line.front() == '#' || trim(line).empty()) { continue; }

        const std::vector<std::string_view> f = splitTabs(line);
        if (f[0] == "A" && f.size() >= 8) {
            Airport a;
            if (!parseDouble(f[5], a.latDeg) || !parseDouble(f[6], a.lonDeg) ||
                std::fabs(a.latDeg) > 90.0 || std::fabs(a.lonDeg) > 180.0) {
                ++bad;
                haveAirport = false;   // its F lines go with it
                continue;
            }
            a.ident = std::string(trim(f[1]));
            a.iata = std::string(trim(f[2]));
            a.local = std::string(trim(f[3]));
            a.country = std::string(trim(f[4]));
            a.name = std::string(trim(f[7]));
            if (a.ident.empty()) {
                ++bad;
                haveAirport = false;
                continue;
            }
            out.push_back(std::move(a));
            haveAirport = true;
        } else if (f[0] == "F" && f.size() >= 3) {
            AirbandFreq q;
            if (!haveAirport || !parseInt(f[1], q.channelKhz) || q.channelKhz <= 0) {
                ++bad;
                continue;
            }
            q.hz = airbandChannelHz(q.channelKhz);
            q.type = std::string(trim(f[2]));
            if (f.size() >= 4) { q.desc = std::string(trim(f[3])); }
            out.back().freqs.push_back(std::move(q));
        } else {
            ++bad;
        }
    }
    for (Airport& a : out) {
        std::stable_sort(a.freqs.begin(), a.freqs.end(),
                         [](const AirbandFreq& x, const AirbandFreq& y) {
                             return x.channelKhz < y.channelKhz;
                         });
    }
    if (skipped != nullptr) { *skipped = bad; }
    return out;
}

const std::vector<Airport>& airbandTable() {
    static std::once_flag once;
    static std::vector<Airport> table;
    std::call_once(once, [] {
        const std::string_view text(reinterpret_cast<const char*>(airbanddata::kTable),
                                    airbanddata::kTableLen);
        table = parseAirbandTable(text);
    });
    return table;
}

std::vector<const Airport*> findAirports(const std::vector<Airport>& table, std::string_view code) {
    std::vector<const Airport*> out;
    const std::string_view c = trim(code);
    if (c.empty()) { return out; }
    const auto addIf = [&](auto matches) {
        for (const Airport& a : table) {
            if (!matches(a)) { continue; }
            if (std::find(out.begin(), out.end(), &a) == out.end()) { out.push_back(&a); }
        }
    };
    addIf([&](const Airport& a) { return equalsNoCase(a.ident, c); });
    addIf([&](const Airport& a) { return !a.iata.empty() && equalsNoCase(a.iata, c); });
    addIf([&](const Airport& a) { return !a.local.empty() && equalsNoCase(a.local, c); });
    return out;
}

double airbandDistanceKm(double lat1Deg, double lon1Deg, double lat2Deg, double lon2Deg) {
    const double r = 6371.0088;
    const double p1 = lat1Deg * kPi / 180.0;
    const double p2 = lat2Deg * kPi / 180.0;
    const double dp = (lat2Deg - lat1Deg) * kPi / 180.0;
    const double dl = (lon2Deg - lon1Deg) * kPi / 180.0;
    const double h = std::sin(dp / 2.0) * std::sin(dp / 2.0) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2.0) * std::sin(dl / 2.0);
    return 2.0 * r * std::asin(std::min(1.0, std::sqrt(h)));
}

std::vector<std::pair<const Airport*, double>> nearestAirports(const std::vector<Airport>& table,
                                                               double latDeg, double lonDeg,
                                                               std::size_t count) {
    std::vector<std::pair<const Airport*, double>> all;
    all.reserve(table.size());
    for (const Airport& a : table) {
        all.emplace_back(&a, airbandDistanceKm(latDeg, lonDeg, a.latDeg, a.lonDeg));
    }
    const std::size_t n = std::min(count, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(),
                      [&](const auto& x, const auto& y) {
                          if (x.second != y.second) { return x.second < y.second; }
                          return x.first < y.first;   // table order: same vector
                      });
    all.resize(n);
    return all;
}

bool airbandContinuous(const AirbandFreq& f) {
    const auto has = [](const std::string& s, const char* word) {
        std::string up = s;
        for (char& c : up) {
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
        }
        return up.find(word) != std::string::npos;
    };
    for (const char* w : {"ATIS", "AWOS", "ASOS", "VOLMET", "AWIB"}) {
        if (has(f.type, w)) { return true; }
    }
    return false;
}

std::string airbandDescWithoutType(const AirbandFreq& f) {
    // The FAA words a frequency's use the way its type already says it:
    // "NORTH CLASS B" under CLASS B, "EMERG" under EMERG, "GND METERING"
    // under GND. Said twice, it is the half of a row's name that fits.
    std::string d = f.desc;
    const std::string& t = f.type;
    if (t.empty() || d.empty()) { return d; }
    if (equalsNoCase(d, t)) { return {}; }
    if (d.size() > t.size() + 1 && d[t.size()] == ' ' && equalsNoCase(std::string_view(d).substr(0, t.size()), t)) {
        return d.substr(t.size() + 1);
    }
    if (d.size() > t.size() + 1 && d[d.size() - t.size() - 1] == ' ' &&
        equalsNoCase(std::string_view(d).substr(d.size() - t.size()), t)) {
        return d.substr(0, d.size() - t.size() - 1);
    }
    return d;
}

std::string airportGroupName(const Airport& a) {
    return a.name.empty() ? a.ident : a.ident + " " + a.name;
}

std::vector<Bookmark> airportBookmarks(const Airport& a) {
    std::vector<Bookmark> out;
    out.reserve(a.freqs.size());
    const std::string group = airportGroupName(a);
    for (const AirbandFreq& q : a.freqs) {
        Bookmark b;
        const bool is833 = airband833(q.channelKhz);
        b.name = q.type;
        if (is833) {
            // The channel NAME is what a controller says and a chart prints;
            // the row's own figure is the frequency, which differs from it.
            char ch[16];
            std::snprintf(ch, sizeof(ch), " %d.%03d", q.channelKhz / 1000, q.channelKhz % 1000);
            b.name += ch;
        }
        const std::string desc = airbandDescWithoutType(q);
        if (!desc.empty()) {
            b.name += " ";
            b.name += desc;
        }
        b.freqHz = q.hz;
        b.mode = "AM";
        b.bandwidthHz = is833 ? kAirbandBw833Hz : kAirbandBw25Hz;
        b.group = group;
        b.scan = !airbandContinuous(q);
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace cascade::core
