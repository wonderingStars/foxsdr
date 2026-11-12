// The workspace's saved layout - see workspace_prefs.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "workspace_prefs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "view3d_prefs.hpp"

namespace foxsdr::slinthost {

namespace {
std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else { cur += c; }
    }
    out.push_back(cur);
    return out;
}
bool whole(const std::string& s, long& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') { return false; }
    out = v;
    return true;
}
}  // namespace

std::string workspaceBody(const SavedWorkspace& w) {
    std::string out = "lay=" + std::to_string(std::clamp(w.layout, 0, 2)) + ",pg=" + std::to_string(std::clamp(w.page, 0, 7)) + ",sel=" + std::to_string(std::clamp(w.selected, 1, 8)) +
                      ",lk=" + std::string(w.linkOn ? "1" : "0") + ",ex=" + std::to_string(w.excluded & 0xFFu) + ",sp=";
    for (std::size_t i = 0; i < w.splits.size(); ++i) {
        if (i > 0) { out += '.'; }
        out += std::to_string(std::clamp(static_cast<int>(std::lround(static_cast<double>(w.splits[i]) * 100.0)), 10, 90));
    }
    if (w.dotStyle != 0) { out += ",dg=" + std::to_string(std::clamp(w.dotStyle, 0, 1)); }
    if ((w.locked & 0xFFu) != 0u) { out += ",fl=" + std::to_string(w.locked & 0xFFu); }
    if (w.highContrast) { out += ",hc=1"; }
    return out;
}

SavedWorkspace parseWorkspaceBody(const std::string& body) {
    SavedWorkspace w;
    w.present = true;
    for (const std::string& kv : splitOn(body, ',')) {
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos) { continue; }
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        long n = 0;
        if (k == "lay" && whole(v, n)) { w.layout = static_cast<int>(std::clamp(n, 0L, 2L)); }
        else if (k == "pg" && whole(v, n)) { w.page = static_cast<int>(std::clamp(n, 0L, 7L)); }
        else if (k == "sel" && whole(v, n)) { w.selected = static_cast<int>(std::clamp(n, 1L, 8L)); }
        else if (k == "lk" && whole(v, n)) { w.linkOn = n != 0; }
        else if (k == "ex" && whole(v, n)) { w.excluded = static_cast<std::uint32_t>(std::clamp(n, 0L, 255L)); }
        else if (k == "dg" && whole(v, n)) { w.dotStyle = static_cast<int>(std::clamp(n, 0L, 1L)); }
        else if (k == "hc" && whole(v, n)) { w.highContrast = n != 0; }
        else if (k == "fl" && whole(v, n)) { w.locked = static_cast<std::uint32_t>(std::clamp(n, 0L, 255L)); }
        else if (k == "sp") {
            const std::vector<std::string> parts = splitOn(v, '.');
            for (std::size_t i = 0; i < parts.size() && i < w.splits.size(); ++i) {
                if (whole(parts[i], n)) { w.splits[i] = static_cast<float>(std::clamp(n, 10L, 90L)) / 100.0f; }
            }
        }
    }
    return w;
}

SavedWorkspace workspaceFromSettings(const std::string& view3dText) {
    const std::string body = foxsdr::slintview3d::ViewPrefs::parse(view3dText).raw(foxsdr::slintview3d::kRawWorkspace);
    if (body.empty()) { return SavedWorkspace{}; }
    return parseWorkspaceBody(body);
}

std::string settingsWithWorkspace(const std::string& view3dText, const SavedWorkspace& w) {
    foxsdr::slintview3d::ViewPrefs p = foxsdr::slintview3d::ViewPrefs::parse(view3dText);
    p.setRaw(foxsdr::slintview3d::kRawWorkspace, workspaceBody(w));
    return p.serialize();
}

namespace {
int percent(float v) { return std::clamp(static_cast<int>(std::lround(static_cast<double>(v) * 100.0)), 10, 90); }
}  // namespace

void WorkspaceMemory::load(const std::string& view3dText) {
    saved_ = workspaceFromSettings(view3dText);
    dirty_ = false;
    armed_ = saved_.present && (saved_.selected > 1 || saved_.page > 0);
}

bool WorkspaceMemory::userSelected(int radio, int page) {
    armed_ = false;
    const int r = std::clamp(radio, 1, 8), p = std::clamp(page, 0, 7);
    if (saved_.selected == r && saved_.page == p && saved_.present) { return false; }
    saved_.selected = r;
    saved_.page = p;
    saved_.present = true;
    dirty_ = true;
    return true;
}

bool WorkspaceMemory::userPage(int page) {
    armed_ = false;
    const int p = std::clamp(page, 0, 7);
    if (saved_.page == p) { return false; }
    saved_.page = p;
    saved_.present = true;
    dirty_ = true;
    return true;
}

bool WorkspaceMemory::setDotStyle(int style) {
    const int s = std::clamp(style, 0, 1);
    if (saved_.dotStyle == s) { return false; }
    saved_.dotStyle = s;
    saved_.present = true;
    dirty_ = true;
    return true;
}

bool WorkspaceMemory::setHighContrast(bool on) {
    if (saved_.highContrast == on) { return false; }
    saved_.highContrast = on;
    saved_.present = true;
    dirty_ = true;
    return true;
}

bool WorkspaceMemory::setLocked(std::uint32_t mask) {
    const std::uint32_t m = mask & 0xFFu;
    if (saved_.locked == m) { return false; }
    saved_.locked = m;
    saved_.present = true;
    dirty_ = true;
    return true;
}

bool WorkspaceMemory::observe(int layout, const std::array<float, 8>& splits, bool linkOn, std::uint32_t excluded) {
    bool changed = false;
    const int lay = std::clamp(layout, 0, 2);
    if (saved_.layout != lay) {
        saved_.layout = lay;
        // a page is a page OF A LAYOUT (Four: radios 1-4, 5-8; Two: pairs; One: one radio): the saved page follows the layout the user chose, onto the page that holds the saved selection
        const int size = lay == 2 ? 4 : lay == 1 ? 2 : 1;
        saved_.page = std::clamp((saved_.selected - 1) / size, 0, 7);
        changed = true;
    }
    for (std::size_t i = 0; i < splits.size(); ++i) {
        if (percent(saved_.splits[i]) != percent(splits[i])) { saved_.splits[i] = static_cast<float>(percent(splits[i])) / 100.0f; changed = true; }
    }
    if (saved_.linkOn != linkOn) { saved_.linkOn = linkOn; changed = true; }
    if ((saved_.excluded & 0xFFu) != (excluded & 0xFFu)) { saved_.excluded = excluded & 0xFFu; changed = true; }
    if (changed) { saved_.present = true; dirty_ = true; }
    return changed;
}

bool WorkspaceMemory::restoreDue(long tick, std::uint32_t openMask, int& radio, int& page) {
    if (!armed_) { return false; }
    if (tick > kRestoreWindowTicks) { armed_ = false; return false; }
    const int slot = saved_.selected - 1;
    if (slot < 0 || slot > 7 || ((openMask >> slot) & 1u) == 0u) { return false; }
    armed_ = false;
    radio = saved_.selected;
    page = saved_.page;
    return true;
}

}  // namespace foxsdr::slinthost
