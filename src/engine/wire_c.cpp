// wire_c.cpp - the settings registry of wiring lane C (see wire_c.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/diag_log.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::engine::wirec {

namespace {

using nlohmann::json;

// The registry. APPEND ONLY (a key is never renamed or removed: a saved file names it). Each row is documented in docs/multi-radio-records/wire-c.md.
// A feature that opens a network port, a device or a file is OFF ("0") here.
constexpr Spec kSpecs[] = {
    // --- system (sysmodels) ---
    {"sys.resume_reopen", Kind::Bool, 0, 0, "1", ""},         // reopen the radio and the audio after the PC wakes from sleep
    {"sys.backup_on_close", Kind::Bool, 0, 0, "0", ""},       // back the settings up when FoxSDR closes
    {"sys.backup_keep", Kind::Int, 1, 50, "10", ""},          // how many backups to keep
    {"sys.sync_folder", Kind::Text, 0, 260, "", ""},          // a shared folder for syncing profiles (empty = off)
    {"perf.mode", Kind::Choice, 0, 0, "auto", "auto|on|off"}, // reduced effects: automatic, always, never
    // --- accessibility (access) ---
    {"access.units", Kind::Choice, 0, 0, "automatic", "automatic|metric|imperial|aviation|nautical"},
    {"access.number_format", Kind::Choice, 0, 0, "dot", "dot|windows"},
    {"access.digits", Kind::Choice, 0, 0, "latin", "latin|native|language"},
    {"access.palette", Kind::Choice, 0, 0, "normal", "normal|deuteranopia|protanopia|tritanopia|highcontrast"},
    // --- web (webmodels) ---
    {"web.cors_origins", Kind::Text, 0, 2000, "", ""},        // allowed origins, one per line (empty = no cross-origin page may read)
    {"web.cors_credentials", Kind::Bool, 0, 0, "0", ""},      // answering credentialed cross-origin requests (needs HTTPS: refused unless the web server is secure)
};
constexpr std::size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

bool validText(const std::string& s, std::size_t maxBytes) {
    if (s.size() > maxBytes) { return false; }
    for (unsigned char ch : s) {
        if ((ch < 0x20u && ch != '\n') || ch == 0x7Fu) { return false; }   // no control character except the line break of a list
    }
    return true;
}

std::string numberText(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

}  // namespace

const Spec* specs(std::size_t& count) {
    count = kSpecCount;
    return kSpecs;
}

const Spec* findSpec(const std::string& key) {
    for (const Spec& s : kSpecs) {
        if (key == s.key) { return &s; }
    }
    return nullptr;
}

bool normalise(const Spec& s, const std::string& in, std::string& out, SetResult& bad, std::string& why) {
    switch (s.kind) {
        case Kind::Bool:
            if (in == "1" || in == "true" || in == "on" || in == "yes") { out = "1"; return true; }
            if (in == "0" || in == "false" || in == "off" || in == "no") { out = "0"; return true; }
            bad = SetResult::BadValue;
            why = "a switch is 1 or 0";
            return false;
        case Kind::Int:
        case Kind::Real: {
            if (in.empty() || in.size() > 32) { bad = SetResult::BadValue; why = "that is not a number"; return false; }
            char* end = nullptr;
            const double v = std::strtod(in.c_str(), &end);
            if (end == in.c_str() || *end != '\0' || !std::isfinite(v)) { bad = SetResult::BadValue; why = "that is not a number"; return false; }
            if (s.kind == Kind::Int && v != std::floor(v)) { bad = SetResult::BadValue; why = "a whole number is wanted"; return false; }
            if (v < s.lo || v > s.hi) {
                bad = SetResult::OutOfRange;
                why = "the value must be from " + numberText(s.lo) + " to " + numberText(s.hi);
                return false;
            }
            out = numberText(v);
            return true;
        }
        case Kind::Choice: {
            const std::string all = std::string("|") + s.choices + "|";
            if (in.find('|') == std::string::npos && all.find("|" + in + "|") != std::string::npos) { out = in; return true; }
            bad = SetResult::OutOfRange;
            why = std::string("the choices are ") + s.choices;
            return false;
        }
        case Kind::Text:
            if (!validText(in, static_cast<std::size_t>(s.hi))) {
                bad = SetResult::BadValue;
                why = "that text is too long or holds a control character";
                return false;
            }
            out = in;
            return true;
    }
    bad = SetResult::BadValue;
    return false;
}

Settings::Settings(std::filesystem::path path) : path_(std::move(path)) { reload(); }

void Settings::reload() {
    std::lock_guard<std::mutex> lk(m_);
    values_.clear();
    error_.clear();
    std::string text;
    const auto r = cascade::core::safeio::readCapped(path_, 256u * 1024u, text);
    if (r == cascade::core::safeio::ReadResult::Missing) { ++version_; return; }
    if (r != cascade::core::safeio::ReadResult::Ok) {
        error_ = "the wirec settings file cannot be read";
        ++version_;
        return;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error_ = "the wirec settings file is damaged; the defaults are in force";
        cascade::core::diagWarnf("wirec: %s", error_.c_str());
        ++version_;
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const Spec* s = findSpec(it.key());
        if (s == nullptr) { continue; }   // a key a newer build wrote: ignored (and dropped at the next save)
        std::string raw = it.value().is_string() ? it.value().get<std::string>()
                          : it.value().is_boolean() ? std::string(it.value().get<bool>() ? "1" : "0")
                          : it.value().is_number() ? numberText(it.value().get<double>()) : std::string();
        std::string norm;
        SetResult bad = SetResult::BadValue;
        std::string why;
        if (normalise(*s, raw, norm, bad, why) && norm != s->def) { values_[it.key()] = norm; }
    }
    ++version_;
}

std::string Settings::get(const std::string& key) const {
    const Spec* s = findSpec(key);
    if (s == nullptr) { return std::string(); }
    std::lock_guard<std::mutex> lk(m_);
    const auto it = values_.find(key);
    return it == values_.end() ? std::string(s->def) : it->second;
}

std::int64_t Settings::getInt(const std::string& key) const { return static_cast<std::int64_t>(std::strtoll(get(key).c_str(), nullptr, 10)); }
double Settings::getReal(const std::string& key) const { return std::strtod(get(key).c_str(), nullptr); }

SetResult Settings::set(const std::string& key, const std::string& value, std::string& why) {
    const Spec* s = findSpec(key);
    if (s == nullptr) { why = "no such setting"; return SetResult::NotFound; }
    std::string norm;
    SetResult bad = SetResult::BadValue;
    if (!normalise(*s, value, norm, bad, why)) { return bad; }
    std::lock_guard<std::mutex> lk(m_);
    const auto it = values_.find(key);
    const std::string current = it == values_.end() ? std::string(s->def) : it->second;
    if (current == norm) { why = "already so"; return SetResult::NoChange; }
    const std::map<std::string, std::string> before = values_;
    if (norm == s->def) { values_.erase(key); } else { values_[key] = norm; }
    if (!save()) {
        values_ = before;   // not kept: what a restart brings back is what was reported
        why = "the setting could not be saved";
        return SetResult::SaveFailed;
    }
    ++version_;
    return SetResult::Changed;
}

// (m_ is held by the caller)
bool Settings::save() {
    if (values_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        return !ec;
    }
    json j = json::object();
    for (const auto& [k, v] : values_) { j[k] = v; }
    return cascade::core::safeio::writeAtomic(path_, j.dump(1, ' ', false, json::error_handler_t::replace) + "\n");
}

void Settings::each(const std::function<void(const std::string&, const std::string&)>& fn) const {
    for (const Spec& s : kSpecs) { fn(s.key, get(s.key)); }
}

}  // namespace cascade::engine::wirec
