// view3d_prefs.cpp - see view3d_prefs.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "view3d_prefs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace foxsdr::slintview3d {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else { cur += c; }
    }
    out.push_back(cur);
    return out;
}

bool number(const std::string& s, float& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) { return false; }
    out = static_cast<float>(v);
    return true;
}

ViewState clampState(ViewState s) {
    scene3d::Camera c;
    c.azimuthDeg = s.azimuth;
    c.elevationDeg = s.elevation;
    c.zoom = s.zoom;
    c = scene3d::clamped(c);
    s.azimuth = c.azimuthDeg;
    s.elevation = c.elevationDeg;
    s.zoom = c.zoom;
    for (int& o : s.opt) { o = std::clamp(o, 0, 15); }
    return s;
}

// a view's name: lower-case letters and digits (the settings text's own alphabet), so a stray word never becomes a view
bool goodName(const std::string& n) {
    if (n.empty() || n.size() > 24) { return false; }
    return std::all_of(n.begin(), n.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool isRawName(const std::string& n) { return n == kRawWorkspace || n == kRawLayouts; }
std::size_t rawLimit(const std::string& n) { return n == kRawLayouts ? kRawLayoutsLimit : 256; }

std::string fmtF(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v));
    return b;
}

}  // namespace

ViewPrefs ViewPrefs::parse(const std::string& text) {
    ViewPrefs p;
    for (const std::string& entry : split(text, ';')) {
        const std::size_t colon = entry.find(':');
        if (colon == std::string::npos || colon == 0) { continue; }
        const std::string name = entry.substr(0, colon);
        const std::string body = entry.substr(colon + 1);
        if (!goodName(name)) { continue; }
        if (isRawName(name)) { if (!body.empty() && body.size() <= rawLimit(name)) { p.raw_[name] = body; } continue; }
        ViewState s;
        for (const std::string& kv : split(body, ',')) {
            const std::size_t eq = kv.find('=');
            if (eq == std::string::npos) { continue; }
            const std::string k = kv.substr(0, eq);
            float v = 0.0f;
            if (!number(kv.substr(eq + 1), v)) { continue; }
            if (k == "on") { s.on = v != 0.0f; }
            else if (k == "az") { s.azimuth = v; }
            else if (k == "el") { s.elevation = v; }
            else if (k == "zoom") { s.zoom = v; }
            else if (k.size() == 2 && k[0] == 'o' && k[1] >= '0' && k[1] <= '3') { s.opt[static_cast<std::size_t>(k[1] - '0')] = static_cast<int>(std::lround(v)); }
        }
        p.views_[name] = clampState(s);
    }
    return p;
}

std::string ViewPrefs::serialize() const {
    std::string out;
    const auto add = [&](const std::string& piece) { if (!out.empty()) { out += ';'; } out += piece; };
    for (const auto& [name, s] : views_) {
        std::string e = name + ":on=" + (s.on ? "1" : "0") + ",az=" + fmtF(s.azimuth) + ",el=" + fmtF(s.elevation) + ",zoom=" + fmtF(s.zoom);
        for (std::size_t i = 0; i < s.opt.size(); ++i) { if (s.opt[i] != 0) { e += ",o" + std::to_string(i) + "=" + std::to_string(s.opt[i]); } }
        add(e);
    }
    for (const auto& [name, body] : raw_) { add(name + ":" + body); }
    return out;
}

std::string ViewPrefs::raw(const std::string& name) const {
    const auto it = raw_.find(name);
    return it == raw_.end() ? std::string() : it->second;
}

void ViewPrefs::setRaw(const std::string& name, const std::string& body) {
    if (!isRawName(name)) { return; }
    if (body.empty() || body.size() > rawLimit(name) || body.find(';') != std::string::npos) { raw_.erase(name); return; }
    raw_[name] = body;
}

ViewState ViewPrefs::get(const std::string& name, const ViewState& defaults) const {
    const auto it = views_.find(name);
    return it == views_.end() ? clampState(defaults) : it->second;
}

void ViewPrefs::set(const std::string& name, const ViewState& s) { views_[name] = clampState(s); }

scene3d::Camera cameraOf(const ViewState& s) {
    scene3d::Camera c;
    c.azimuthDeg = s.azimuth;
    c.elevationDeg = s.elevation;
    c.zoom = s.zoom;
    return c;
}

void setCamera(ViewState& s, const scene3d::Camera& c) {
    s.azimuth = c.azimuthDeg;
    s.elevation = c.elevationDeg;
    s.zoom = c.zoom;
}

}  // namespace foxsdr::slintview3d
