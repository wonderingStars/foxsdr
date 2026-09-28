// plugin_settings_ui.cpp - see plugin_settings_ui.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_settings_ui.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

#include "core/plugin_api.hpp"

namespace cascade::core {

namespace {

// Bytes before the first NUL, never reading past `cap`: every string in a
// CascadeSettingSpec is a fixed array the plugin filled, and nothing promises
// it is terminated.
std::size_t boundedLength(const char* s, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && s[n] != '\0') { ++n; }
    return n;
}

// Length of the UTF-8 sequence starting at s[i], or 0 when it is not a
// well-formed one (overlong forms, surrogates and code points above U+10FFFF
// are refused, as the store refuses them).
std::size_t utf8SequenceAt(const std::string& s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80u) { return 1; }
    std::size_t len = 0;
    unsigned cp = 0;
    if (c >= 0xC2u && c <= 0xDFu) {
        len = 2;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0u && c <= 0xEFu) {
        len = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        len = 4;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (i + len > s.size()) { return 0; }
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0u) != 0x80u) { return 0; }
        cp = (cp << 6) | (b(i + k) & 0x3Fu);
    }
    if (len == 3 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) { return 0; }
    if (len == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) { return 0; }
    return len;
}

// True when `s` is valid UTF-8 with no C0 control character and no DEL - the
// shape of a single-line field value.
bool isCleanLine(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20u || c == 0x7Fu) { return false; }
        const std::size_t n = utf8SequenceAt(s, i);
        if (n == 0) { return false; }
        i += n;
    }
    return true;
}

// A plugin-authored display string, made safe to hand to ImGui: bounded,
// malformed bytes replaced by '?', control characters by a space, and "##"
// collapsed to "#" so a label can never cut an ImGui widget id in two.
std::string cleanPluginText(const char* p, std::size_t cap) {
    const std::string raw(p, boundedLength(p, cap));
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x20u || c == 0x7Fu) {
            out += ' ';
            ++i;
            continue;
        }
        const std::size_t n = utf8SequenceAt(raw, i);
        if (n == 0) {
            out += '?';
            ++i;
            continue;
        }
        if (c == '#' && !out.empty() && out.back() == '#') {
            ++i;
            continue;
        }
        out.append(raw, i, n);
        i += n;
    }
    return out;
}

std::string trimSpaces(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) { --b; }
    return s.substr(a, b - a);
}

// The decimal grammar, checked by hand so the answer never depends on the C
// locale (strtod would take "1,5" in some of them) and so "12abc", "0x10",
// "inf" and "nan" - all of which strtod accepts in part or in whole - are not
// numbers here.
bool isDecimalNumber(const std::string& t) {
    std::size_t i = 0;
    const std::size_t n = t.size();
    const auto digit = [&](std::size_t k) { return k < n && t[k] >= '0' && t[k] <= '9'; };
    if (i < n && (t[i] == '+' || t[i] == '-')) { ++i; }
    std::size_t intDigits = 0;
    while (digit(i)) { ++i; ++intDigits; }
    std::size_t fracDigits = 0;
    if (i < n && t[i] == '.') {
        ++i;
        while (digit(i)) { ++i; ++fracDigits; }
    }
    if (intDigits + fracDigits == 0) { return false; }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < n && (t[i] == '+' || t[i] == '-')) { ++i; }
        std::size_t expDigits = 0;
        while (digit(i)) { ++i; ++expDigits; }
        if (expDigits == 0) { return false; }
    }
    return i == n;
}

}  // namespace

std::vector<SettingField> settingFieldsFrom(const CascadeSettingsUiApi* api) {
    std::vector<SettingField> out;
    if (api == nullptr) { return out; }
    if (api->structSize != static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi))) {
        return out;
    }
    if (api->specs == nullptr || api->count == 0u || api->count > CASCADE_MAX_SETTING_SPECS) {
        return out;
    }
    for (std::uint32_t i = 0; i < api->count; ++i) {
        const CascadeSettingSpec& s = api->specs[i];
        // Size FIRST: until it agrees, no other field is at an offset this
        // host has confirmed.
        if (s.structSize != static_cast<std::uint32_t>(sizeof(CascadeSettingSpec))) { continue; }
        if (s.kind != CASCADE_SETTING_TEXT && s.kind != CASCADE_SETTING_NUMBER &&
            s.kind != CASCADE_SETTING_BOOL) {
            continue;  // a later host's kind: skipped, never refused
        }
        const std::string key(s.key, boundedLength(s.key, CASCADE_SETTING_KEY_CHARS));
        if (key.size() >= CASCADE_SETTING_KEY_CHARS || !validSettingKey(key.c_str())) {
            continue;  // a field that could never be stored is not a field
        }
        bool repeat = false;
        for (const SettingField& f : out) { repeat = repeat || f.key == key; }
        if (repeat) { continue; }  // two boxes for one value would fight

        SettingField f;
        f.key = key;
        f.kind = s.kind;
        f.label = cleanPluginText(s.label, CASCADE_SETTING_LABEL_CHARS);
        if (f.label.empty()) { f.label = key; }  // never an unlabelled box
        if (s.kind != CASCADE_SETTING_BOOL) {
            f.placeholder = cleanPluginText(s.placeholder, CASCADE_SETTING_PLACEHOLDER_CHARS);
        }
        switch (s.kind) {
            case CASCADE_SETTING_TEXT: {
                std::size_t m = s.maxLength;
                if (m == 0u) { m = 1u; }
                if (m > CASCADE_SETTING_VALUE_BYTES - 1u) { m = CASCADE_SETTING_VALUE_BYTES - 1u; }
                f.maxLength = m;
                break;
            }
            case CASCADE_SETTING_NUMBER:
                f.maxLength = kMaxNumberChars;
                break;
            default:
                f.maxLength = 1u;
                break;
        }
        // The default is shown as a value, so it has to be one this field
        // could hold: a default the field would refuse is dropped rather
        // than shown as though it were stored.
        const std::string def(s.defaultValue,
                              boundedLength(s.defaultValue, CASCADE_SETTING_DEFAULT_CHARS));
        std::string storedDefault;
        if (prepareSettingEdit(f, def, storedDefault) == SettingEditResult::Accepted) {
            f.defaultValue = storedDefault;
        }
        out.push_back(std::move(f));
    }
    return out;
}

SettingEditResult prepareSettingEdit(const SettingField& f, const std::string& typed,
                                     std::string& stored) {
    stored.clear();
    switch (f.kind) {
        case CASCADE_SETTING_TEXT:
            if (typed.size() > f.maxLength) { return SettingEditResult::TooLong; }
            if (!isCleanLine(typed)) { return SettingEditResult::BadText; }
            stored = typed;
            return SettingEditResult::Accepted;
        case CASCADE_SETTING_NUMBER: {
            const std::string t = trimSpaces(typed);
            if (t.size() > kMaxNumberChars) { return SettingEditResult::TooLong; }
            if (t.empty()) { return SettingEditResult::Accepted; }  // "no number"
            if (!isDecimalNumber(t)) { return SettingEditResult::NotANumber; }
            // The grammar allows 1e999, which is not a number anyone can use.
            const double v = std::strtod(t.c_str(), nullptr);
            if (!std::isfinite(v)) { return SettingEditResult::NotANumber; }
            stored = t;
            return SettingEditResult::Accepted;
        }
        case CASCADE_SETTING_BOOL:
            stored = (typed == "1") ? "1" : "";
            return SettingEditResult::Accepted;
        default:
            return SettingEditResult::Refused;
    }
}

SettingEditResult commitSettingEdit(PluginApiCore& api, const std::string& pluginName,
                                    const SettingField& f, const std::string& typed) {
    std::string stored;
    const SettingEditResult r = prepareSettingEdit(f, typed, stored);
    if (r != SettingEditResult::Accepted) { return r; }
    const std::int32_t rc = api.settingsUiSet(pluginName, f.key.c_str(), stored.c_str());
    if (rc == CASCADE_API_OK) { return SettingEditResult::Accepted; }
    // LIMIT is the one failure a user can act on ("something else has to go
    // first") and the one this store realistically returns here - `stored`
    // was already size- and UTF-8-checked above, so BAD_ARGUMENT/OUT_OF_RANGE
    // would mean this function's own validation disagreed with the store's.
    return rc == CASCADE_API_LIMIT ? SettingEditResult::StoreFull : SettingEditResult::StoreError;
}

std::string settingFieldValue(const PluginApiCore& api, const std::string& pluginName,
                              const SettingField& f) {
    std::string v;
    if (api.settingsUiGet(pluginName, f.key.c_str(), v) == CASCADE_API_OK) { return v; }
    return f.defaultValue;
}

bool settingBoolChecked(const std::string& value) { return value == "1"; }

}  // namespace cascade::core
