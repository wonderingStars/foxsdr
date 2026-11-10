// survey3d_logic.cpp - see survey3d_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "survey3d_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace foxsdr::slintview3d {

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

bool parseLeadingNumber(const std::string& text, double& value, std::string& rest) {
    const std::string t = trim(text);
    std::size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) { ++i; }
    const std::size_t digits = i;
    bool dot = false;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) != 0 || (!dot && t[i] == '.'))) { if (t[i] == '.') { dot = true; } ++i; }
    if (i == digits || (i == digits + 1 && dot)) { return false; }
    value = std::strtod(t.substr(0, i).c_str(), nullptr);
    if (!std::isfinite(value)) { return false; }
    rest = lower(trim(t.substr(i)));
    return true;
}

bool parseFrequencyHz(const std::string& text, double& hz) {
    double v = 0.0;
    std::string rest;
    if (!parseLeadingNumber(text, v, rest) || v < 0.0) { return false; }
    if (rest.rfind("ghz", 0) == 0) { hz = v * 1e9; }
    else if (rest.rfind("mhz", 0) == 0) { hz = v * 1e6; }
    else if (rest.rfind("khz", 0) == 0) { hz = v * 1e3; }
    else if (rest.rfind("hz", 0) == 0) { hz = v; }
    else if (rest.empty()) { hz = v < 30000.0 ? v * 1e6 : v; }
    else { return false; }
    return hz > 0.0;
}

bool parseWidthHz(const std::string& text, double& hz) {
    double v = 0.0;
    std::string rest;
    if (!parseLeadingNumber(text, v, rest) || v < 0.0) { return false; }
    if (rest.rfind("mhz", 0) == 0) { hz = v * 1e6; }
    else if (rest.rfind("khz", 0) == 0) { hz = v * 1e3; }
    else if (rest.rfind("hz", 0) == 0 || rest.empty()) { hz = v; }
    else { return false; }
    return true;
}

bool parseDb(const std::string& text, double& db) {
    double v = 0.0;
    std::string rest;
    if (!parseLeadingNumber(text, v, rest)) { return false; }
    if (!rest.empty() && rest.rfind("db", 0) != 0) { return false; }
    db = v;
    return true;
}

bool parsePercent(const std::string& text, double& pct) {
    double v = 0.0;
    std::string rest;
    if (!parseLeadingNumber(text, v, rest)) { return false; }
    if (!rest.empty() && rest[0] == '/') {   // "seen / made": the plugin's own count of dwells
        double of = 0.0;
        std::string tail;
        if (!parseLeadingNumber(rest.substr(1), of, tail) || !(of > 0.0) || v < 0.0) { return false; }
        pct = std::clamp(100.0 * v / of, 0.0, 100.0);
        return true;
    }
    if (!rest.empty() && rest[0] != '%') { return false; }
    pct = std::clamp(v, 0.0, 100.0);
    return true;
}

bool channelOfCells(const std::vector<std::string>& headings, const std::vector<std::string>& cells, SurveyChannel& out) {
    SurveyChannel c;
    const auto cell = [&](const char* head) -> const std::string* {
        for (std::size_t i = 0; i < headings.size() && i < cells.size(); ++i) { if (lower(headings[i]) == head) { return &cells[i]; } }
        return nullptr;
    };
    const std::string* f = cell("frequency");
    if (f == nullptr || !parseFrequencyHz(*f, c.freqHz)) { return false; }
    c.freqText = trim(*f);
    for (std::size_t i = 0; i < cells.size(); ++i) { c.raw += (i != 0 ? " | " : "") + cells[i]; }
    if (const std::string* w = cell("width")) { c.haveWidth = parseWidthHz(*w, c.widthHz); }
    if (const std::string* p = cell("peak db")) { c.havePeak = parseDb(*p, c.peakDb); }
    if (const std::string* o = cell("over floor")) { c.haveOver = parseDb(*o, c.overDb); }
    if (const std::string* s = cell("seen")) { c.haveSeen = parsePercent(*s, c.seenPct); c.seenText = trim(*s); }
    if (const std::string* b = cell("band")) { c.band = trim(*b); }
    if (const std::string* g = cell("guess")) { c.guess = trim(*g); }
    out = c;
    return true;
}

double floorDb(const std::vector<SurveyChannel>& ch) {
    std::vector<double> v;
    double lowestPeak = 1e9;
    for (const SurveyChannel& c : ch) {
        if (c.havePeak && c.haveOver) { v.push_back(c.peakDb - c.overDb); }
        if (c.havePeak) { lowestPeak = std::min(lowestPeak, c.peakDb); }
    }
    if (!v.empty()) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
    return lowestPeak < 1e8 ? lowestPeak - 10.0 : -110.0;
}

void spanOf(const std::vector<SurveyChannel>& ch, double& loHz, double& hiHz) {
    if (ch.empty()) { loHz = 144.0e6; hiHz = 148.0e6; return; }
    double lo = 1e30, hi = -1e30;
    for (const SurveyChannel& c : ch) { lo = std::min(lo, c.freqHz); hi = std::max(hi, c.freqHz); }
    if (hi - lo < 1.0) { loHz = lo - 200e3; hiHz = hi + 200e3; return; }
    const double pad = std::max(50e3, 0.06 * (hi - lo));
    loHz = lo - pad;
    hiHz = hi + pad;
}

void ridgeGrid(const std::vector<SurveyChannel>& ch, double loHz, double hiHz, double floorDbV, int rows, int cols, std::vector<float>& out) {
    rows = std::max(2, rows);
    cols = std::max(2, cols);
    out.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), static_cast<float>(floorDbV));
    const double span = std::max(1.0, hiHz - loHz);
    const double colHz = span / static_cast<double>(cols - 1);
    for (const SurveyChannel& c : ch) {
        const double peak = c.havePeak ? c.peakDb : (c.haveOver ? floorDbV + c.overDb : floorDbV);
        if (peak <= floorDbV) { continue; }
        const double seen = c.haveSeen ? c.seenPct : 100.0;
        const double halfW = std::max(c.haveWidth ? c.widthHz / 2.0 : 0.0, colHz * 1.5);   // at least three columns across
        const double c0 = (c.freqHz - halfW - loHz) / colHz, c1 = (c.freqHz + halfW - loHz) / colHz;
        const int a = std::max(0, static_cast<int>(std::floor(c0))), b = std::min(cols - 1, static_cast<int>(std::ceil(c1)));
        const int kc = std::clamp(static_cast<int>(std::lround((c.freqHz - loHz) / colHz)), 0, cols - 1);   // the column nearest the channel's frequency stands at the full peak
        for (int r = 0; r < rows; ++r) {
            const double occ = 100.0 * static_cast<double>(r) / static_cast<double>(rows - 1);
            if (occ > seen + 1e-9) { break; }
            if (c.freqHz >= loHz && c.freqHz <= hiHz) {
                float& top = out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(kc)];
                top = std::max(top, static_cast<float>(peak));
            }
            for (int k = a; k <= b; ++k) {
                const double f = loHz + colHz * static_cast<double>(k);
                const double u = std::fabs(f - c.freqHz) / halfW;
                if (u >= 1.0) { continue; }
                const double bump = 0.5 * (1.0 + std::cos(3.14159265358979323846 * u));
                const float v = static_cast<float>(floorDbV + (peak - floorDbV) * bump);
                float& cell = out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(k)];
                cell = std::max(cell, v);
            }
        }
    }
}

void levelRange(const std::vector<SurveyChannel>& ch, double floorDbV, double& loDb, double& hiDb) {
    double top = floorDbV + 30.0;
    for (const SurveyChannel& c : ch) { if (c.havePeak) { top = std::max(top, c.peakDb + 3.0); } }
    loDb = std::floor(floorDbV / 10.0) * 10.0;
    hiDb = std::ceil(top / 10.0) * 10.0;
    if (hiDb - loDb < 30.0) { hiDb = loDb + 30.0; }
}

std::string formatSurveyMHz(double hz, double spanHz) {
    const double step = spanHz / 4.0;
    int decimals = 3;
    if (step >= 1e6) { decimals = 1; }
    else if (step >= 1e5) { decimals = 2; }
    else if (step >= 1e3) { decimals = 3; }
    else if (step >= 100.0) { decimals = 4; }
    else { decimals = 6; }
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", decimals, hz / 1e6);
    return b;
}

}  // namespace foxsdr::slintview3d
