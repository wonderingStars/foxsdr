// layouts_prefs.cpp - see layouts_prefs.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "layouts_prefs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "view3d_prefs.hpp"

namespace foxsdr::slintlayouts {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else { cur += c; }
    }
    out.push_back(cur);
    return out;
}

std::string lowerOf(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool whole(const std::string& s, long long& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') { return false; }
    out = v;
    return true;
}

Layout clamped(Layout l) {
    l.bank = std::clamp(l.bank, 0, 4);
    l.sections &= 0x3FFu;
    l.view = std::clamp(l.view, 0, 2);
    l.splitPct = std::clamp(l.splitPct, 10, 90);
    l.workspace = std::clamp(l.workspace, 0, 2);
    l.selected = std::clamp(l.selected, 0, 8);
    l.mode = std::clamp(l.mode, 0, 7);
    if (!l.tuning) { l.tuneHz = 0; }
    return l;
}

std::string one(const Layout& l) {
    std::string s = "n=" + l.name + ",b=" + std::to_string(l.bank) + ",s=" + std::to_string(l.sections) + ",f=" + (l.railFolded ? "1" : "0") + ",v=" + std::to_string(l.view) +
                    ",p=" + std::to_string(l.splitPct) + ",w=" + std::to_string(l.workspace) + ",k=" + (l.link ? "1" : "0") + ",d=" + (l.wf3d ? "1" : "0");
    if (l.selected > 0) { s += ",r=" + std::to_string(l.selected); }
    if (l.tuning) { s += ",t=" + std::to_string(l.tuneHz) + ",m=" + std::to_string(l.mode); }
    return s;
}

}  // namespace

std::string cleanName(const std::string& raw) {
    std::string t;
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f || c == ';' || c == '|' || c == ',' || c == '=' || c == ':' || c == '~') { t += ' '; } else { t += static_cast<char>(c); }
    }
    std::string out;   // runs of spaces collapse, ends trimmed
    for (char c : t) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) { continue; }
        out += c;
    }
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) { --cut; }   // not inside a UTF-8 sequence
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    }
    return out;
}

const std::vector<Layout>& builtins() {
    static const std::vector<Layout> kList = [] {
        std::vector<Layout> v;
        Layout air;
        air.name = "Airband"; air.builtin = true; air.bank = 2; air.sections = 1u << 4; air.splitPct = 40; air.workspace = 1;
        Layout hf;
        hf.name = "HF DX"; hf.builtin = true; hf.bank = 0; hf.sections = (1u << 1) | (1u << 2); hf.splitPct = 25; hf.workspace = 0;
        v.push_back(clamped(air));
        v.push_back(clamped(hf));
        return v;
    }();
    return kList;
}

std::string body(const std::vector<Layout>& user) {
    std::string out;
    for (const Layout& l : user) {
        if (l.builtin || cleanName(l.name).empty()) { continue; }
        if (!out.empty()) { out += '|'; }
        out += one(clamped(l));
    }
    return out;
}

std::vector<Layout> parse(const std::string& text) {
    std::vector<Layout> out;
    for (const std::string& entry : split(text, '|')) {
        Layout l;
        bool named = false;
        for (const std::string& kv : split(entry, ',')) {
            const std::size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) { continue; }
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "n") { l.name = cleanName(v); named = !l.name.empty(); continue; }
            long long n = 0;
            if (!whole(v, n)) { continue; }
            if (k == "b") { l.bank = static_cast<int>(std::clamp<long long>(n, 0, 4)); }
            else if (k == "s") { l.sections = static_cast<std::uint32_t>(std::clamp<long long>(n, 0, 0x3FF)); }
            else if (k == "f") { l.railFolded = n != 0; }
            else if (k == "v") { l.view = static_cast<int>(std::clamp<long long>(n, 0, 2)); }
            else if (k == "p") { l.splitPct = static_cast<int>(std::clamp<long long>(n, 10, 90)); }
            else if (k == "w") { l.workspace = static_cast<int>(std::clamp<long long>(n, 0, 2)); }
            else if (k == "k") { l.link = n != 0; }
            else if (k == "d") { l.wf3d = n != 0; }
            else if (k == "r") { l.selected = static_cast<int>(std::clamp<long long>(n, 0, 8)); }
            else if (k == "t") { l.tuning = true; l.tuneHz = std::clamp<long long>(n, 0, 20000000000LL); }
            else if (k == "m") { l.mode = static_cast<int>(std::clamp<long long>(n, 0, 7)); }
        }
        if (!named) { continue; }
        out.push_back(clamped(l));
        if (out.size() >= kMaxUserLayouts) { break; }
    }
    return out;
}

void LayoutBook::load(const std::string& view3dText) {
    const foxsdr::slintview3d::ViewPrefs p = foxsdr::slintview3d::ViewPrefs::parse(view3dText);
    user_ = parse(p.raw(foxsdr::slintview3d::kRawLayouts));
    // a user layout that has a stock layout's name (a hand-edited file) is dropped: a name is one layout
    user_.erase(std::remove_if(user_.begin(), user_.end(), [](const Layout& l) {
        for (const Layout& b : builtins()) { if (lowerOf(b.name) == lowerOf(l.name)) { return true; } }
        return false;
    }), user_.end());
    dirty_ = false;
}

std::vector<Layout> LayoutBook::all() const {
    std::vector<Layout> v = builtins();
    v.insert(v.end(), user_.begin(), user_.end());
    return v;
}

const Layout* LayoutBook::find(const std::string& name) const {
    const std::string key = lowerOf(cleanName(name));
    for (const Layout& b : builtins()) { if (lowerOf(b.name) == key) { return &b; } }
    for (const Layout& u : user_) { if (lowerOf(u.name) == key) { return &u; } }
    return nullptr;
}

SaveResult LayoutBook::save(const std::string& rawName, Layout layout) {
    const std::string name = cleanName(rawName);
    if (name.empty()) { return SaveResult::BadName; }
    for (const Layout& b : builtins()) { if (lowerOf(b.name) == lowerOf(name)) { return SaveResult::ReservedName; } }
    layout.name = name;
    layout.builtin = false;
    layout = clamped(layout);
    for (Layout& u : user_) {
        if (lowerOf(u.name) == lowerOf(name)) { u = layout; dirty_ = true; return SaveResult::Replaced; }
    }
    if (user_.size() >= kMaxUserLayouts) { return SaveResult::Full; }
    user_.push_back(layout);
    dirty_ = true;
    return SaveResult::Saved;
}

SaveResult LayoutBook::rename(const std::string& from, const std::string& rawTo) {
    const std::string to = cleanName(rawTo);
    if (to.empty()) { return SaveResult::BadName; }
    const std::string key = lowerOf(cleanName(from));
    for (const Layout& b : builtins()) { if (lowerOf(b.name) == lowerOf(to)) { return SaveResult::ReservedName; } }
    const auto src = std::find_if(user_.begin(), user_.end(), [&](const Layout& u) { return lowerOf(u.name) == key; });
    if (src == user_.end()) { return SaveResult::BadName; }
    for (auto it = user_.begin(); it != user_.end(); ++it) { if (it != src && lowerOf(it->name) == lowerOf(to)) { return SaveResult::ReservedName; } }
    src->name = to;
    dirty_ = true;
    return SaveResult::Saved;
}

bool LayoutBook::remove(const std::string& name) {
    const std::string key = lowerOf(cleanName(name));
    const auto it = std::find_if(user_.begin(), user_.end(), [&](const Layout& u) { return lowerOf(u.name) == key; });
    if (it == user_.end()) { return false; }
    user_.erase(it);
    dirty_ = true;
    return true;
}

std::string LayoutBook::text(const std::string& view3dText) const {
    foxsdr::slintview3d::ViewPrefs p = foxsdr::slintview3d::ViewPrefs::parse(view3dText);
    p.setRaw(foxsdr::slintview3d::kRawLayouts, body(user_));
    return p.serialize();
}

std::vector<Step> stepsOf(const Layout& l) {
    std::vector<Step> s{Step::Bank, Step::Sections, Step::Rail, Step::View, Step::Split, Step::Workspace, Step::Link};
    if (l.selected > 0) { s.push_back(Step::Selection); }
    s.push_back(Step::Wf3d);
    if (l.tuning) { s.push_back(Step::Tuning); }
    return s;
}

Layout capture(const Window& w, bool includeTuning) {
    Layout l;
    l.bank = w.bank;
    l.sections = w.sections;
    l.railFolded = w.railFolded;
    l.view = w.view >= 0 && w.view <= 2 ? w.view : 0;   // a plugin page is not part of a layout
    l.splitPct = w.splitPct;
    l.workspace = w.workspace;
    l.link = w.link;
    l.wf3d = w.wf3d;
    l.selected = w.selected;
    l.tuning = includeTuning;
    if (includeTuning) { l.tuneHz = w.tunedHz; l.mode = w.mode; }
    return clamped(l);
}

int apply(const Layout& l, const Window& now, const Actions& act) {
    int held = 0;
    for (const Step s : stepsOf(l)) {
        switch (s) {
            case Step::Bank: if (act.setBank) { act.setBank(l.bank); } break;
            case Step::Sections: if (act.setSections) { act.setSections(l.bank, l.sections); } break;
            case Step::Rail: if (l.railFolded != now.railFolded && act.setRail) { act.setRail(l.railFolded); } break;
            case Step::View:
                if (l.view == now.view) { break; }
                if (now.patchRunning) { held |= kHeldView; break; }   // the tab is not left while a patch runs: leaving it would stop the patch
                if (act.setView) { act.setView(l.view); }
                break;
            case Step::Split: if (act.setSplitPct) { act.setSplitPct(l.splitPct); } break;
            case Step::Workspace: if (l.workspace != now.workspace && act.setWorkspace) { act.setWorkspace(l.workspace); } break;
            case Step::Link: if (l.link != now.link && act.setLink) { act.setLink(l.link); } break;
            case Step::Selection:
                if (l.selected < 1 || l.selected > 8 || ((now.openRadios >> (l.selected - 1)) & 1u) == 0u) { held |= kHeldSelection; break; }
                if (l.selected != now.selected && act.selectRadio) { act.selectRadio(l.selected); }
                break;
            case Step::Wf3d: if (l.wf3d != now.wf3d && act.setWf3d) { act.setWf3d(l.wf3d); } break;
            case Step::Tuning:
                if (act.tune && l.tuneHz > 0) { act.tune(l.tuneHz); }
                if (act.setMode) { act.setMode(l.mode); }
                break;
        }
    }
    return held;
}

}  // namespace foxsdr::slintlayouts
