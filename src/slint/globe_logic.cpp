// globe_logic.cpp - see globe_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace foxsdr::globe {

namespace {

std::string upper(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
    return s.substr(a, b - a);
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) { out.push_back(cur); }
    return out;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }
bool endsWith(const std::string& s, const char* p) {
    const std::size_t n = std::char_traits<char>::length(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
// a plain decimal number at the start of s ("1.36", "-9", "5 570" is not one); false when none
bool leadingNumber(const std::string& s, double& out, std::size_t* used = nullptr) {
    const char* b = s.c_str();
    char* e = nullptr;
    const double v = std::strtod(b, &e);
    if (e == b) { return false; }
    out = v;
    if (used != nullptr) { *used = static_cast<std::size_t>(e - b); }
    return true;
}
bool isLocator4(const std::string& t) {
    if (t.size() != 4) { return false; }
    const auto up = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
    return up(t[0]) >= 'A' && up(t[0]) <= 'R' && up(t[1]) >= 'A' && up(t[1]) <= 'R' && std::isdigit(static_cast<unsigned char>(t[2])) &&
           std::isdigit(static_cast<unsigned char>(t[3]));
}

}  // namespace

// ---- which plugin page offers which view ----

ViewKind viewKindFor(const std::string& name) {
    if (name == "Propagation Tools") { return ViewKind::Propagation; }
    if (name == "FT8") { return ViewKind::Ft8; }
    if (name == "GOES HRIT / LRIT") { return ViewKind::Goes; }
    if (name == "Solar System View") { return ViewKind::SolarSystem; }
    if (name == "Satellites") { return ViewKind::Satellites; }
    return ViewKind::None;
}

const char* flatViewLabel(ViewKind k) {
    switch (k) {
        case ViewKind::Propagation: return "PICTURE";
        case ViewKind::Ft8: return "LIST";
        case ViewKind::Goes: return "FLAT";
        case ViewKind::SolarSystem: return "PAGES";
        case ViewKind::Satellites: return "FLAT MAP";
        default: return "";
    }
}
const char* globeViewLabel(ViewKind k) {
    switch (k) {
        case ViewKind::SolarSystem: return "3D ORBITS";
        case ViewKind::None: return "";
        default: return "3D GLOBE";
    }
}

GlobeCamera defaultCamera(ViewKind k) {
    GlobeCamera c;
    switch (k) {
        case ViewKind::Goes: c.centreLatDeg = 0.0; c.centreLonDeg = -75.2; break;
        case ViewKind::SolarSystem: c.centreLatDeg = 55.0; c.centreLonDeg = 20.0; break;   // for the solar system: tilt and spin
        default: break;
    }
    return c;
}

void dragCamera(GlobeCamera& cam, double dxPx, double dyPx, double wellPx) {
    if (!(wellPx > 1.0) || !std::isfinite(dxPx) || !std::isfinite(dyPx)) { return; }
    const double radiusPx = 0.45 * wellPx * cam.zoom;
    const double degPerPx = (180.0 / foxsdr::globe::kPi) / radiusPx;
    cam.centreLonDeg = normalizeLon(cam.centreLonDeg - dxPx * degPerPx);
    cam.centreLatDeg = std::clamp(cam.centreLatDeg + dyPx * degPerPx, -85.0, 85.0);
}

void zoomCamera(GlobeCamera& cam, double steps) {
    if (!std::isfinite(steps)) { return; }
    cam.zoom = std::clamp(cam.zoom * std::pow(1.25, steps), kZoomMin, kZoomMax);
}

// ---- what a view remembers ----

const char* prefsName(ViewKind k) {
    switch (k) {
        case ViewKind::Propagation: return "propagation";
        case ViewKind::Ft8: return "ft8";
        case ViewKind::Goes: return "goes";
        case ViewKind::SolarSystem: return "solar";
        case ViewKind::Satellites: return "satellites";
        default: return "";
    }
}

float latitudeToElevation(double latDeg) {
    const double lat = std::clamp(latDeg, -85.0, 85.0);
    return static_cast<float>(4.0 + (lat + 85.0) / 170.0 * 85.0);
}
double elevationToLatitude(float elevationDeg) {
    return std::clamp((static_cast<double>(elevationDeg) - 4.0) / 85.0 * 170.0 - 85.0, -85.0, 85.0);
}

slintview3d::ViewState prefsDefaults(ViewKind k) {
    slintview3d::ViewState p;
    p.on = false;
    switch (k) {
        case ViewKind::Propagation: p.opt = {0, 1, 0, 0}; break;      // every band, the greyline on
        case ViewKind::Ft8: p.opt = {0, 1, 0, 0}; break;              // every band, the 1 h window
        case ViewKind::SolarSystem: p.opt = {1, 1, 0, 0}; break;      // log scale, the Moon shown
        case ViewKind::Satellites: p.opt = {1, 1, 0, 0}; break;       // footprint and ground tracks on
        default: break;
    }
    return p;
}

slintview3d::ViewState toPrefs(const ViewState& v, ViewKind k) {
    slintview3d::ViewState p = prefsDefaults(k);
    p.on = v.globeOn;
    p.zoom = static_cast<float>(std::clamp(v.camera.zoom, 0.35, 5.0));
    if (k == ViewKind::SolarSystem) {
        double spin = std::fmod(v.spinDeg, 360.0);
        if (spin < 0.0) { spin += 360.0; }
        p.azimuth = static_cast<float>(spin > 180.0 ? spin - 360.0 : spin);
        p.elevation = static_cast<float>(std::clamp(v.tiltDeg, 5.0, 85.0));
    } else {
        p.azimuth = static_cast<float>(normalizeLon(v.camera.centreLonDeg));
        p.elevation = latitudeToElevation(v.camera.centreLatDeg);
    }
    switch (k) {
        case ViewKind::Propagation: p.opt[0] = std::clamp(v.bandId, 0, 5); p.opt[1] = v.greyline ? 1 : 0; break;
        case ViewKind::Ft8: p.opt[0] = std::clamp(v.bandId, 0, 12); p.opt[1] = v.windowMinutes <= 15 ? 0 : (v.windowMinutes <= 60 ? 1 : 2); break;
        case ViewKind::Goes: p.opt[0] = std::clamp(v.goesSatellite, 0, 1); break;
        case ViewKind::SolarSystem: p.opt[0] = v.logScale ? 1 : 0; p.opt[1] = v.showMoon ? 1 : 0; break;
        case ViewKind::Satellites: p.opt[0] = v.footprints ? 1 : 0; p.opt[1] = v.groundTracks ? 1 : 0; break;
        default: break;
    }
    p.opt[3] = v.cameraSet ? 1 : 0;
    return p;
}

void fromPrefs(ViewState& v, ViewKind k, const slintview3d::ViewState& p) {
    v.globeOn = p.on;
    v.cameraSet = p.opt[3] != 0;
    if (v.cameraSet) {
        v.camera.zoom = std::clamp(static_cast<double>(p.zoom), kZoomMin, kZoomMax);
        if (k == ViewKind::SolarSystem) {
            v.spinDeg = std::fmod(static_cast<double>(p.azimuth) + 360.0, 360.0);
            v.tiltDeg = std::clamp(static_cast<double>(p.elevation), 5.0, 85.0);
        } else {
            v.camera.centreLonDeg = normalizeLon(p.azimuth);
            v.camera.centreLatDeg = elevationToLatitude(p.elevation);
        }
    }
    switch (k) {
        case ViewKind::Propagation: v.bandId = std::clamp(p.opt[0], 0, 5); v.greyline = p.opt[1] != 0; break;
        case ViewKind::Ft8: {
            v.bandId = std::clamp(p.opt[0], 0, 12);
            const int w = std::clamp(p.opt[1], 0, 2);
            v.windowMinutes = w == 0 ? 15 : (w == 1 ? 60 : 1440);
            break;
        }
        case ViewKind::Goes: v.goesSatellite = std::clamp(p.opt[0], 0, 1); break;
        case ViewKind::SolarSystem: v.logScale = p.opt[0] != 0; v.showMoon = p.opt[1] != 0; break;
        case ViewKind::Satellites: v.footprints = p.opt[0] != 0; v.groundTracks = p.opt[1] != 0; break;
        default: break;
    }
}

// ---- numbers and words ----

std::string kmText(double km) {
    if (!std::isfinite(km) || km < 0.0) { return "-"; }
    const long long whole = std::llround(km);
    std::string digits = std::to_string(whole);
    std::string out;
    int n = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++n % 3 == 0 && i != 0) { out.insert(out.begin(), ' '); }
    }
    return out + " km";
}

std::string bearingText(double deg) {
    if (!std::isfinite(deg)) { return "-"; }
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) { d += 360.0; }
    int r = static_cast<int>(std::lround(d));
    if (r == 360) { r = 0; }
    return std::to_string(r) + "\xC2\xB0";
}

std::string latLonText(const LatLon& p) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %c %.2f %c", std::fabs(p.lat), p.lat >= 0 ? 'N' : 'S', std::fabs(p.lon), p.lon >= 0 ? 'E' : 'W');
    return buf;
}

// ---- bands ----

namespace {
const Band kBands[] = {
    {1, "160 m", 1.8, 2.0},    {2, "80 m", 3.5, 4.0},     {3, "60 m", 5.25, 5.45},   {4, "40 m", 7.0, 7.3},     {5, "30 m", 10.1, 10.15},
    {6, "20 m", 14.0, 14.35},  {7, "17 m", 18.068, 18.168}, {8, "15 m", 21.0, 21.45}, {9, "12 m", 24.89, 24.99}, {10, "10 m", 28.0, 29.7},
    {11, "6 m", 50.0, 54.0},   {12, "4 m", 70.0, 70.5},   {13, "2 m", 144.0, 148.0},
};
}  // namespace

const Band* bandOfMhz(double mhz) {
    for (const Band& b : kBands) {
        if (mhz >= b.loMhz - 1e-9 && mhz <= b.hiMhz + 1e-9) { return &b; }
    }
    return nullptr;
}
const Band* bandById(int id) { return (id >= 1 && id <= static_cast<int>(sizeof(kBands) / sizeof(kBands[0]))) ? &kBands[id - 1] : nullptr; }
int bandCount() { return static_cast<int>(sizeof(kBands) / sizeof(kBands[0])); }

std::uint32_t bandColour(int bandId) {
    switch (bandId) {
        case 6: return 0xFFF5A524u;    // 20 m orange
        case 4: return 0xFF6FD3FFu;    // 40 m cyan
        case 8: return 0xFFFF6B9Du;    // 15 m pink
        case 10: return 0xFF5FD28Au;   // 10 m green
        case 1: return 0xFFB48EF5u;
        case 2: return 0xFFE8D44Du;
        case 3: return 0xFF9AA5B1u;
        case 5: return 0xFFF08A5Du;
        case 7: return 0xFF4DD0C4u;
        case 9: return 0xFFC3E86Du;
        case 11: return 0xFFFF9E80u;
        case 12: return 0xFF80CBC4u;
        case 13: return 0xFFCE93D8u;
        default: return 0xFFFFFFFFu;
    }
}

std::uint32_t hopColour(int hops) {
    if (hops <= 1) { return 0xFFF5A524u; }
    if (hops == 2) { return 0xFF6FD3FFu; }
    if (hops == 3) { return 0xFFFF6B9Du; }
    return 0xFF5FD28Au;
}

// ---- locators in text ----

std::string locatorIn(const std::string& text) {
    std::string cur;
    auto accept = [](const std::string& tok) {
        if (tok.size() != 4 && tok.size() != 6) { return false; }
        LatLon p;
        return locatorToLatLon(tok, p);
    };
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += c;
        } else {
            if (accept(cur)) { return cur; }
            cur.clear();
        }
    }
    return std::string();
}

// ---- 1. propagation ----

namespace {
// The beacon list of ncdxf.org/beacon/beaconlocations.html, read 2026-11-10 (the page prints the zeros of FN30as and GF05tj with a slash).
const Beacon kBeacons[kBeaconCount] = {
    {"4U1UN", "New York City", "FN30as"}, {"VE8AT", "Inuvik, NT", "CP38gh"},    {"W6WX", "Mt. Umunhum", "CM97bd"},  {"KH6RS", "Maui", "BL10ts"},
    {"ZL6B", "Masterton", "RE78tw"},      {"VK6RBP", "Rolystone", "OF87av"},   {"JA2IGY", "Mt. Asama", "PM84jk"},  {"RR9O", "Novosibirsk", "NO14kx"},
    {"VR2B", "Hong Kong", "OL72bg"},      {"4S7B", "Colombo", "MJ96wv"},       {"ZS6DN", "Pretoria", "KG33xi"},    {"5Z4B", "Kikuyu", "KI88hr"},
    {"4X6TU", "Tel Aviv", "KM72jb"},      {"OH2B", "Lohja", "KP20eh"},         {"CS3B", "Sao Jorge", "IM12jt"},    {"LU4AA", "Buenos Aires", "GF05tj"},
    {"OA4B", "Lima", "FH17mw"},           {"YV5B", "Caracas", "FK60nd"},
};
}  // namespace

const Beacon* beaconByCall(const std::string& call) {
    const std::string u = upper(call);
    for (const Beacon& b : kBeacons) {
        if (u == b.call) { return &b; }
    }
    return nullptr;
}
const Beacon& beaconAt(int index) { return kBeacons[std::clamp(index, 0, kBeaconCount - 1)]; }

PropagationFacts readPropagationPanel(const std::vector<PanelRowText>& rows) {
    PropagationFacts f;
    bool inBeacons = false;
    for (const PanelRowText& r : rows) {
        if (r.kind == 1) {
            inBeacons = !r.cells.empty() && startsWith(r.cells[0], "Beacons on air now");
            if (inBeacons) { f.panelSeen = true; }
            continue;
        }
        if (r.kind == 2) { inBeacons = false; continue; }
        if (r.cells.empty()) { continue; }
        const std::string head = trim(r.cells[0]);
        if (head == "Locator" || head == "Target") {
            std::string rest;
            for (std::size_t i = 1; i < r.cells.size(); ++i) { rest += r.cells[i] + " "; }
            const std::string loc = locatorIn(rest);
            if (head == "Locator") { f.panelSeen = true; f.homeLocator = loc; }
            else { f.targetLocator = loc; }
            continue;
        }
        if (!inBeacons) { continue; }
        BeaconOnAir b;
        for (const std::string& cell : r.cells) {
            for (const std::string& tok : split(cell)) {
                if (b.beacon == nullptr) { b.beacon = beaconByCall(tok); }
            }
            const std::string c = trim(cell);
            if (endsWith(c, " MHz")) {
                double v = 0.0;
                if (leadingNumber(c, v)) { b.freqMhz = v; }
            }
            if (endsWith(c, " s left")) {
                double v = 0.0;
                if (leadingNumber(c, v)) { b.secondsLeft = static_cast<int>(v); }
            }
        }
        if (b.beacon != nullptr) { f.onAir.push_back(b); }
    }
    return f;
}

namespace {
GlobePath makePath(const std::string& label, const LatLon& from, const LatLon& to) {
    GlobePath p;
    p.label = label;
    p.from = from;
    p.to = to;
    p.distKm = distanceKm(from, to);
    p.bearingDeg = bearingDeg(from, to);
    p.hops = hopCount(p.distKm, kMaxHopKm);
    p.argb = hopColour(p.hops);
    return p;
}
}  // namespace

PropagationScene buildPropagationScene(const PropagationFacts& facts, bool rxKnown, const LatLon& rx, int bandFilter) {
    PropagationScene s;
    s.beaconsListed = static_cast<int>(facts.onAir.size());
    LatLon loc;
    if (!facts.homeLocator.empty() && locatorToLatLon(facts.homeLocator, loc)) {
        s.homeKnown = true;
        s.home = loc;
        s.homeSource = "the plugin's locator";
    } else if (rxKnown) {
        s.homeKnown = true;
        s.home = rx;
        s.homeSource = "the receiver position";
    }
    if (!s.homeKnown) { return s; }
    s.markers.push_back({"HOME", s.home, 1, 0xFFFFFFFFu});
    LatLon target;
    if (!facts.targetLocator.empty() && locatorToLatLon(facts.targetLocator, target) && distanceKm(s.home, target) > 1.0) {
        GlobePath p = makePath("TARGET " + facts.targetLocator, s.home, target);
        s.markers.push_back({p.label, target, 2, p.argb});
        s.paths.push_back(std::move(p));
    }
    for (const BeaconOnAir& b : facts.onAir) {
        if (b.beacon == nullptr) { continue; }
        const Band* band = b.freqMhz > 0.0 ? bandOfMhz(b.freqMhz) : nullptr;
        if (bandFilter != 0 && (band == nullptr || band->id != bandFilter)) { continue; }
        LatLon at;
        if (!locatorToLatLon(b.beacon->grid, at) || distanceKm(s.home, at) < 1.0) { continue; }
        GlobePath p = makePath(std::string(b.beacon->call) + " " + b.beacon->place, s.home, at);
        p.beacon = true;
        p.freqMhz = b.freqMhz;
        p.bandId = band != nullptr ? band->id : 0;
        s.markers.push_back({b.beacon->call, at, 0, p.argb});
        s.paths.push_back(std::move(p));
    }
    return s;
}

std::string pathChipText(const GlobePath& p) { return p.label + " " + kmText(p.distKm) + " " + bearingText(p.bearingDeg); }

// ---- 2. FT8 ----

bool parseFt8Line(const std::string& text, Ft8Decode& out) {
    const std::size_t tilde = text.find(" ~ ");
    if (tilde == std::string::npos) { return false; }
    const std::vector<std::string> head = split(text.substr(0, tilde));
    if (head.size() != 4) { return false; }
    const std::string& t = head[0];
    if (t.size() != 6) { return false; }
    for (char c : t) { if (!std::isdigit(static_cast<unsigned char>(c))) { return false; } }
    Ft8Decode d;
    d.hour = std::stoi(t.substr(0, 2));
    d.minute = std::stoi(t.substr(2, 2));
    d.second = std::stoi(t.substr(4, 2));
    if (d.hour > 23 || d.minute > 59 || d.second > 59) { return false; }
    double v = 0.0;
    std::size_t used = 0;
    if (!leadingNumber(head[1], v, &used) || used != head[1].size()) { return false; }
    d.snr = static_cast<int>(v);
    if (!leadingNumber(head[2], v, &used) || used != head[2].size()) { return false; }
    d.dt = v;
    if (!leadingNumber(head[3], v, &used) || used != head[3].size()) { return false; }
    d.hz = static_cast<int>(v);
    d.message = trim(text.substr(tilde + 3));
    if (d.message.empty()) { return false; }
    out = d;
    return true;
}

std::int64_t ft8LineTimeMs(const Ft8Decode& d, std::int64_t nowUnixMs) {
    constexpr std::int64_t kDay = 86400000;
    const std::int64_t dayStart = (nowUnixMs / kDay) * kDay;
    std::int64_t t = dayStart + (static_cast<std::int64_t>(d.hour) * 3600 + d.minute * 60 + d.second) * 1000;
    if (t > nowUnixMs + 60000) { t -= kDay; }
    return t;
}

bool looksLikeCallsign(const std::string& token) {
    if (token.size() < 3 || token.size() > 12) { return false; }
    bool digit = false, letter = false;
    for (char c : token) {
        if (std::isdigit(static_cast<unsigned char>(c))) { digit = true; }
        else if (std::isalpha(static_cast<unsigned char>(c)) && std::isupper(static_cast<unsigned char>(c))) { letter = true; }
        else if (c != '/') { return false; }
    }
    return digit && letter && !isLocator4(token);
}

bool ft8Heard(const std::string& message, Ft8Heard& out) {
    std::vector<std::string> tok = split(upper(message));
    if (tok.size() < 2) { return false; }
    std::size_t sender = tok.size();
    if (tok[0] == "CQ") {
        for (std::size_t i = 1; i < tok.size(); ++i) {
            if (looksLikeCallsign(tok[i])) { sender = i; break; }
        }
    } else if (looksLikeCallsign(tok[0]) && tok.size() >= 2 && looksLikeCallsign(tok[1])) {
        sender = 1;
    }
    if (sender >= tok.size()) { return false; }
    if (sender + 1 >= tok.size()) { return false; }
    const std::string& last = tok.back();
    if (last == "RR73" || !isLocator4(last)) { return false; }
    out.call = tok[sender];
    out.locator = last;
    return true;
}

bool Ft8SpotStore::addLine(const std::string& text, std::int64_t atMs, double dialMhz, const LatLon& rx) {
    Ft8Decode d;
    Ft8Heard h;
    if (!parseFt8Line(text, d) || !ft8Heard(d.message, h)) { return false; }
    LatLon at;
    if (!locatorToLatLon(h.locator, at)) { return false; }
    Ft8Spot s;
    s.atMs = atMs;
    const Band* b = bandOfMhz(dialMhz);
    s.bandId = b != nullptr ? b->id : 0;
    s.call = h.call;
    s.locator = h.locator;
    s.at = at;
    s.snr = d.snr;
    s.km = distanceKm(rx, at);
    s.bearingDeg = bearingDeg(rx, at);
    add(s);
    return true;
}

void Ft8SpotStore::add(const Ft8Spot& s) {
    for (Ft8Spot& e : spots_) {
        if (e.call == s.call && e.bandId == s.bandId) { e = s; return; }
    }
    if (spots_.size() >= kCap) { spots_.erase(spots_.begin()); }
    spots_.push_back(s);
}

std::vector<Ft8Spot> Ft8SpotStore::visible(std::int64_t nowMs, std::int64_t windowMs, int bandFilter) {
    const std::int64_t day = 24LL * 3600LL * 1000LL;
    spots_.erase(std::remove_if(spots_.begin(), spots_.end(), [&](const Ft8Spot& s) { return nowMs - s.atMs > day; }), spots_.end());
    std::vector<Ft8Spot> out;
    for (const Ft8Spot& s : spots_) {
        if (nowMs - s.atMs > windowMs) { continue; }
        if (bandFilter != 0 && s.bandId != bandFilter) { continue; }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Ft8Spot& a, const Ft8Spot& b) { return a.atMs < b.atMs; });
    return out;
}

Ft8Summary summariseSpots(const std::vector<Ft8Spot>& spots) {
    Ft8Summary s;
    std::vector<std::string> calls;
    std::vector<int> bands;
    for (const Ft8Spot& sp : spots) {
        if (std::find(calls.begin(), calls.end(), sp.call) == calls.end()) { calls.push_back(sp.call); }
        if (std::find(bands.begin(), bands.end(), sp.bandId) == bands.end()) { bands.push_back(sp.bandId); }
        if (!s.hasFurthest || sp.km > s.furthest.km) { s.hasFurthest = true; s.furthest = sp; }
    }
    s.stations = static_cast<int>(calls.size());
    s.bands = static_cast<int>(bands.size());
    return s;
}

// ---- 3. GOES ----

const GoesSatellite& goesSatellite(int index) {
    static const GoesSatellite sats[kGoesSatelliteCount] = {{"GOES-East", -75.2}, {"GOES-West", -137.0}};
    return sats[std::clamp(index, 0, kGoesSatelliteCount - 1)];
}

bool goesScanAngle(const LatLon& g, double satLonDeg, double& xRad, double& yRad) {
    // GOES-R Product User Guide, fixed-grid navigation: GRS 80 ellipsoid, perspective height H = 42 164.16 km
    constexpr double req = 6378.137, rpol = 6356.752314, h = 42164.16;
    const double e2 = (req * req - rpol * rpol) / (req * req);
    const double phi = g.lat * kDegToRad;
    const double dl = (g.lon - satLonDeg) * kDegToRad;
    const double phic = std::atan((rpol * rpol) / (req * req) * std::tan(phi));
    const double rc = rpol / std::sqrt(1.0 - e2 * std::cos(phic) * std::cos(phic));
    const double sx = h - rc * std::cos(phic) * std::cos(dl);
    const double sy = -rc * std::cos(phic) * std::sin(dl);
    const double sz = rc * std::sin(phic);
    // Behind the limb: the line of sight from the satellite to the point must leave the ground on the satellite's side. With X = H - sx the point's distance along the
    // satellite's axis, the surface normal there is (X, Y, Z req^2/rpol^2) / req^2 and (S - P) . n > 0 reduces, on the ellipsoid, to H X > req^2.
    if (h * (h - sx) <= req * req) { return false; }
    xRad = std::asin(-sy / std::sqrt(sx * sx + sy * sy + sz * sz));
    yRad = std::atan(sz / sx);
    return true;
}

bool goesPicturePoint(const LatLon& g, double satLonDeg, double& u, double& v) {
    double x = 0.0, y = 0.0;
    if (!goesScanAngle(g, satLonDeg, x, y)) { return false; }
    const double uu = 0.5 + x / (2.0 * kGoesFrameHalfRad);
    const double vv = 0.5 - y / (2.0 * kGoesFrameHalfRad);
    if (uu < 0.0 || uu > 1.0 || vv < 0.0 || vv > 1.0) { return false; }
    u = uu;
    v = vv;
    return true;
}

LookAngle goesLook(const LatLon& rx, double satLonDeg) { return lookAngle(rx, LatLon{0.0, satLonDeg}, kGoesAltitudeKm); }

// ---- 4. the solar system ----

namespace {
std::string planetNameLower(const std::string& s) { std::string r = s; for (char& c : r) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return r; }
int planetIndexOf(const std::string& name) {
    const std::string n = planetNameLower(trim(name));
    for (int i = 0; i < solar::kPlanetCount; ++i) {
        if (n == planetNameLower(solar::planetName(i))) { return i; }
    }
    return -1;
}
}  // namespace

SolarPanelFacts readSolarPanel(const std::vector<PanelRowText>& rows) {
    SolarPanelFacts f;
    bool in = false;
    for (const PanelRowText& r : rows) {
        if (r.kind == 1) { in = !r.cells.empty() && startsWith(r.cells[0], "Planets from the Earth"); continue; }
        if (r.kind == 2) { in = false; continue; }
        if (!in || r.cells.empty()) { continue; }
        const std::string head = trim(r.cells[0]);
        const std::string lower = planetNameLower(head);
        if (lower == "earth") {
            for (std::size_t i = 1; i < r.cells.size(); ++i) {
                const std::string c = trim(r.cells[i]);
                double v = 0.0;
                if (endsWith(c, " AU") && leadingNumber(c, v)) { f.haveEarthSun = true; f.earthSunAu = v; }
            }
            continue;
        }
        if (lower == "moon") {
            f.haveMoon = true;
            for (std::size_t i = 1; i < r.cells.size(); ++i) {
                const std::string c = trim(r.cells[i]);
                double v = 0.0;
                if (endsWith(c, " km")) {
                    std::string digits;
                    for (char ch : c) { if (std::isdigit(static_cast<unsigned char>(ch))) { digits += ch; } else if (ch == '.') { break; } }
                    if (!digits.empty()) { f.moonKm = std::stod(digits); }
                } else if (endsWith(c, "% lit") && leadingNumber(c, v)) {
                    f.haveMoonLit = true;
                    f.moonLitPct = v;
                }
            }
            continue;
        }
        const int pi = planetIndexOf(head);
        if (pi < 0) { continue; }
        PanelPlanet p;
        p.planet = pi;
        for (std::size_t i = 1; i < r.cells.size(); ++i) {
            const std::string c = trim(r.cells[i]);
            double v = 0.0;
            if (startsWith(c, "elong ")) {
                std::size_t used = 0;
                if (leadingNumber(c.substr(6), v, &used)) {
                    p.haveElong = true;
                    p.elongDeg = v;
                    const std::string rest = trim(c.substr(6 + used));
                    p.evening = !rest.empty() && (rest[0] == 'E' || rest[0] == 'e');
                }
            } else if (startsWith(c, "r ") && endsWith(c, " AU")) {
                if (leadingNumber(c.substr(2), v)) { p.haveSunAu = true; p.sunAu = v; }
            } else if (endsWith(c, " AU") && !p.haveEarthAu && leadingNumber(c, v)) {
                p.haveEarthAu = true;
                p.earthAu = v;
            }
        }
        f.planets.push_back(p);
    }
    return f;
}

PanelCheck checkAgainstPanel(const SolarPanelFacts& panel, double jd) {
    PanelCheck c;
    auto note = [&](const char* what, const std::string& name, double diff, double allowed, double& worst) {
        ++c.compared;
        if (diff > worst) { worst = diff; }
        if (diff > allowed) { c.ok = false; if (c.worst.empty()) { c.worst = name + " " + what; } }
    };
    for (const PanelPlanet& p : panel.planets) {
        const solar::PlanetFacts f = solar::planetFacts(static_cast<solar::Planet>(p.planet), jd);
        const std::string name = solar::planetName(p.planet);
        if (p.haveEarthAu) { note("AU", name, std::fabs(p.earthAu - f.earthDistanceAu), 0.005 + 0.002, c.worstAuError); }
        if (p.haveSunAu) { note("r", name, std::fabs(p.sunAu - f.sunDistanceAu), 0.005 + 0.002, c.worstAuError); }
        if (p.haveElong) {
            note("elongation", name, std::fabs(p.elongDeg - f.elongationDeg), 0.5 + 0.3, c.worstElongError);
            ++c.compared;
            if (p.evening != f.evening && f.elongationDeg > 1.0 && f.elongationDeg < 179.0) { c.ok = false; if (c.worst.empty()) { c.worst = name + " side"; } }
        }
    }
    if (panel.haveEarthSun) { note("Earth r", "Earth", std::fabs(panel.earthSunAu - solar::earthSunDistanceAu(jd)), 0.0005 + 0.0005, c.worstAuError); }
    return c;
}

namespace {
std::uint32_t planetColour(int i) {
    static const std::uint32_t c[solar::kPlanetCount] = {0xFFB5B5B5u, 0xFFE8C26Au, 0xFFFF7A59u, 0xFFD8A878u, 0xFFEED79Fu, 0xFF8ADBE8u, 0xFF5B7BFFu};
    return c[std::clamp(i, 0, solar::kPlanetCount - 1)];
}
}  // namespace

OrbitScene buildOrbitScene(double jd, bool logScale, bool showMoon) {
    OrbitScene s;
    s.jd = jd;
    s.logScale = logScale;
    s.showMoon = showMoon;
    s.outerAu = 31.0;
    for (int i = 0; i < solar::kPlanetCount; ++i) {
        const auto p = static_cast<solar::Planet>(i);
        std::vector<solar::Xyz> ring;
        for (const solar::Xyz& q : solar::orbitRing(p, jd, 180)) { ring.push_back(solar::displayPosition(q, s.outerAu, logScale)); }
        s.rings.push_back(std::move(ring));
        BodyView b;
        b.name = solar::planetName(i);
        b.facts = solar::planetFacts(p, jd);
        b.pos = solar::displayPosition(b.facts.helio, s.outerAu, logScale);
        b.argb = planetColour(i);
        b.radiusPx = (i == 3 || i == 4) ? 6.0 : (i >= 5 ? 5.0 : 4.0);
        s.bodies.push_back(std::move(b));
    }
    {
        std::vector<solar::Xyz> ring;
        for (const solar::Xyz& q : solar::earthOrbitRing(jd, 180)) { ring.push_back(solar::displayPosition(q, s.outerAu, logScale)); }
        s.rings.push_back(std::move(ring));
    }
    const solar::Xyz earthHelio = solar::earthHeliocentric(jd);
    s.earth = solar::displayPosition(earthHelio, s.outerAu, logScale);
    s.moon = solar::moonFacts(jd);
    const double mx = s.moon.geocentric.x, my = s.moon.geocentric.y;
    const double mn = std::sqrt(mx * mx + my * my);
    constexpr double ringRadius = 0.05;   // display units: a ring about the Earth, not to scale
    s.moonDisplay = {s.earth.x + (mn > 0.0 ? mx / mn : 1.0) * ringRadius, s.earth.y + (mn > 0.0 ? my / mn : 0.0) * ringRadius, s.earth.z};
    return s;
}

std::string planetChipText(const BodyView& b) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %.2f AU %d\xC2\xB0 %c", b.name.c_str(), b.facts.earthDistanceAu, static_cast<int>(std::lround(b.facts.elongationDeg)),
                  b.facts.evening ? 'E' : 'W');
    return buf;
}

std::string dateText(double jd) {
    // Meeus ch. 7, Julian day to calendar date
    const double z0 = std::floor(jd + 0.5);
    const double alpha = std::floor((z0 - 1867216.25) / 36524.25);
    const double a = z0 + 1.0 + alpha - std::floor(alpha / 4.0);
    const double b = a + 1524.0;
    const double c = std::floor((b - 122.1) / 365.25);
    const double d = std::floor(365.25 * c);
    const double e = std::floor((b - d) / 30.6001);
    const int day = static_cast<int>(b - d - std::floor(30.6001 * e));
    const int month = static_cast<int>(e < 14.0 ? e - 1.0 : e - 13.0);
    const int year = static_cast<int>(month > 2 ? c - 4716.0 : c - 4715.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return buf;
}

double dateOfOffset(double todayJd, int dayOffset) { return todayJd + std::clamp(dayOffset, -kDayOffsetLimit, kDayOffsetLimit); }

// ---- 5. satellites ----

SatReadout satReadout(const SatIn& sat, bool rxKnown, const LatLon& rx) {
    SatReadout r;
    r.valid = true;
    r.label = sat.label.empty() ? sat.id : sat.label;
    r.sub = sat.at;
    r.hasAlt = sat.hasAlt;
    r.altKm = sat.altKm;
    if (sat.hasAlt) { r.footprintDeg = footprintAngularRadiusDeg(sat.altKm, 0.0); }
    if (rxKnown && sat.hasAlt) {
        r.look = lookAngle(rx, sat.at, sat.altKm);
        r.hasLook = true;
    }
    return r;
}

const char* orbitClass(double altKm) {
    if (std::fabs(altKm - 35786.0) < 500.0) { return "GEO"; }
    if (altKm < 2000.0) { return "LEO"; }
    if (altKm < 35000.0) { return "MEO"; }
    return "HEO";
}

std::uint32_t orbitClassColour(double altKm) {
    const std::string c = orbitClass(altKm);
    if (c == "LEO") { return 0xFFF5A524u; }
    if (c == "MEO") { return 0xFF6FD3FFu; }
    if (c == "GEO") { return 0xFFFF6B9Du; }
    return 0xFF5FD28Au;
}

}  // namespace foxsdr::globe
