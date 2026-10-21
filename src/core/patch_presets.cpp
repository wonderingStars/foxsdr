// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_presets.hpp"

#include <algorithm>
#include <utility>

#include "core/diag_log.hpp"
#include "core/patch_io.hpp"
#include "core/utf8_text.hpp"

namespace cascade::core {

namespace {

// The whole character at s[i], or 0 when the byte there does not start one
// (a stray continuation byte, a lead byte whose sequence is broken off, or a
// byte no UTF-8 sequence may start with).
std::size_t wholeCharLen(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    if (c < 0x80) {
        len = 1;
    } else if (c >= 0xC2 && c < 0xE0) {
        len = 2;
    } else if (c >= 0xE0 && c < 0xF0) {
        len = 3;
    } else if (c >= 0xF0 && c < 0xF5) {
        len = 4;
    }
    if (len == 0 || i + len > s.size()) { return 0; }
    for (std::size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) { return 0; }
    }
    return len;
}

void trimSpaces(std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && s[a] == ' ') { ++a; }
    std::size_t b = s.size();
    while (b > a && s[b - 1] == ' ') { --b; }
    s = s.substr(a, b - a);
}

// One form per letter whichever case was typed: every code point through
// upperSimple, so "Łódź" and "ŁÓDŹ" fold alike and "Cafe" and "Café" do not.
std::string foldName(std::string_view name) {
    const std::string clean = cleanPatchPresetName(name);
    std::string out;
    out.reserve(clean.size());
    std::size_t i = 0;
    while (i < clean.size()) { utf8Append(out, upperSimple(utf8Decode(clean, i))); }
    return out;
}

bool reserved(std::string_view name) { return patchPresetNamesMatch(name, kPreviousPatchSlotName); }

// The text rules shared by save, the previous slot and the config loader.
// Ok, TooLarge or NotAPatch.
PatchPresetStatus checkText(const std::string& text) {
    if (text.size() > kMaxPatchPresetTextBytes) { return PatchPresetStatus::TooLarge; }
    if (!patchPresetTextParses(text)) { return PatchPresetStatus::NotAPatch; }
    return PatchPresetStatus::Saved;
}

}  // namespace

std::string cleanPatchPresetName(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t len = wholeCharLen(in, i);
        if (len == 0) {
            ++i;  // not a character: dropped, byte by byte
            continue;
        }
        const auto c = static_cast<unsigned char>(in[i]);
        const bool c0 = len == 1 && (c < 0x20u || c == 0x7Fu);
        // U+0080..U+009F, the C1 controls: C2 80 .. C2 9F.
        const bool c1 = len == 2 && c == 0xC2u && static_cast<unsigned char>(in[i + 1]) < 0xA0u;
        if (!c0 && !c1) { out.append(in.substr(i, len)); }
        i += len;
    }
    trimSpaces(out);
    if (out.size() > kMaxPatchPresetNameBytes) {
        out.resize(utf8Floor(out.c_str(), kMaxPatchPresetNameBytes));
        trimSpaces(out);
    }
    return out;
}

bool patchPresetNamesMatch(std::string_view a, std::string_view b) {
    return foldName(a) == foldName(b);
}

bool patchPresetTextParses(const std::string& text) { return patch::parse(text).ok; }

void PatchPresetStore::assign(std::vector<PatchPreset> list, std::string previous) {
    list_ = std::move(list);
    previous_ = std::move(previous);
    sort();
}

int PatchPresetStore::find(std::string_view name) const {
    const std::string key = foldName(name);
    if (key.empty()) { return -1; }
    for (std::size_t i = 0; i < list_.size(); ++i) {
        if (foldName(list_[i].name) == key) { return static_cast<int>(i); }
    }
    return -1;
}

PatchPresetStatus PatchPresetStore::save(std::string_view name, const std::string& text,
                                         bool overwrite) {
    const std::string clean = cleanPatchPresetName(name);
    if (clean.empty()) { return PatchPresetStatus::EmptyName; }
    if (reserved(clean)) { return PatchPresetStatus::ReservedName; }
    if (const PatchPresetStatus t = checkText(text); t != PatchPresetStatus::Saved) { return t; }
    const int at = find(clean);
    if (at >= 0) {
        if (!overwrite) { return PatchPresetStatus::Exists; }
        // The spelling just typed wins: it is the one the user last chose.
        list_[static_cast<std::size_t>(at)] = PatchPreset{clean, text};
        sort();
        return PatchPresetStatus::Overwritten;
    }
    if (list_.size() >= kMaxPatchPresets) { return PatchPresetStatus::ListFull; }
    list_.push_back(PatchPreset{clean, text});
    sort();
    return PatchPresetStatus::Saved;
}

PatchPresetStatus PatchPresetStore::rename(std::string_view from, std::string_view to) {
    const int at = find(from);
    if (at < 0) { return PatchPresetStatus::NotFound; }
    const std::string clean = cleanPatchPresetName(to);
    if (clean.empty()) { return PatchPresetStatus::EmptyName; }
    if (reserved(clean)) { return PatchPresetStatus::ReservedName; }
    const int other = find(clean);
    if (other >= 0 && other != at) { return PatchPresetStatus::Exists; }
    list_[static_cast<std::size_t>(at)].name = clean;
    sort();
    return PatchPresetStatus::Renamed;
}

PatchPresetStatus PatchPresetStore::remove(std::string_view name) {
    const int at = find(name);
    if (at < 0) { return PatchPresetStatus::NotFound; }
    list_.erase(list_.begin() + at);
    return PatchPresetStatus::Deleted;
}

bool PatchPresetStore::keepPrevious(const std::string& text) {
    if (checkText(text) != PatchPresetStatus::Saved) { return false; }
    previous_ = text;
    return true;
}

void PatchPresetStore::sort() {
    // Folded first, so case does not split the list in two; the raw name
    // breaks a tie only between entries a hand-edit made equal.
    std::stable_sort(list_.begin(), list_.end(), [](const PatchPreset& a, const PatchPreset& b) {
        const std::string fa = foldName(a.name);
        const std::string fb = foldName(b.name);
        return fa != fb ? fa < fb : a.name < b.name;
    });
}

std::vector<PatchPreset> sanitisePatchPresets(std::vector<PatchPreset> in,
                                              const std::vector<std::size_t>& positions) {
    std::vector<PatchPreset> out;
    std::vector<std::string> keys;
    for (std::size_t k = 0; k < in.size(); ++k) {
        PatchPreset& p = in[k];
        const std::size_t i = k < positions.size() ? positions[k] : k;
        p.name = cleanPatchPresetName(p.name);
        if (p.name.empty()) {
            diagLogf("config: patch preset %zu dropped - it has no usable name", i);
            continue;
        }
        if (reserved(p.name)) {
            diagLogf("config: patch preset %zu dropped - its name is reserved", i);
            continue;
        }
        const PatchPresetStatus t = checkText(p.text);
        if (t == PatchPresetStatus::TooLarge) {
            diagLogf("config: patch preset %zu dropped - its text is %zu bytes, over %zu", i,
                     p.text.size(), kMaxPatchPresetTextBytes);
            continue;
        }
        if (t == PatchPresetStatus::NotAPatch) {
            diagLogf("config: patch preset %zu dropped - its text is not a patch", i);
            continue;
        }
        std::string key = foldName(p.name);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
            diagLogf("config: patch preset %zu dropped - its name repeats an earlier one", i);
            continue;
        }
        if (out.size() >= kMaxPatchPresets) {
            diagLogf("config: %zu patch preset(s) past the first %zu dropped", in.size() - k,
                     kMaxPatchPresets);
            break;
        }
        keys.push_back(std::move(key));
        out.push_back(std::move(p));
    }
    return out;
}

std::string sanitisePatchPresetPrevious(std::string text) {
    if (text.empty()) { return text; }
    const PatchPresetStatus t = checkText(text);
    if (t == PatchPresetStatus::Saved) { return text; }
    diagLogf("config: the previous-patch slot dropped - its text is %s",
             t == PatchPresetStatus::TooLarge ? "too large" : "not a patch");
    return std::string();
}

}  // namespace cascade::core
