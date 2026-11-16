// s5b_logic.cpp - see s5b_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s5b_logic.hpp"

#include <cmath>
#include <cstdlib>

#include "core/i18n.hpp"

namespace foxsdr::slints5b {

namespace {
// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
using foxsdr::slintlanel::numberText;
using foxsdr::slintlanel::parseNumber;
using foxsdr::slintlanel::trim;
}  // namespace

// ============================================================ analogue IF bandwidth ============================================================
bool parseIfBandwidthKHz(const std::string& text, long long& hz, std::string& err) {
    if (trim(text).empty()) { hz = 0; return true; }
    double khz = 0.0;
    if (!parseNumber(text, 0.0, 8000.0, khz, err)) { return false; }
    if (khz != 0.0 && khz < 300.0) { err = tr("allowed:") + " 300 - 8000 (0 = automatic)"; return false; }
    hz = static_cast<long long>(khz * 1000.0 + 0.5);
    return true;
}

std::string ifBandwidthWords(const std::string& fact) {
    if (fact.empty() || fact.find_first_not_of("0123456789") != std::string::npos) { return std::string(); }
    const long long hz = std::atoll(fact.c_str());
    if (hz == 0) { return tr("Automatic (from the sample rate)"); }
    return numberText(static_cast<double>(hz) / 1000.0) + " kHz";
}


// ============================================================ SoapySDR driver bandwidth and settings ============================================================
namespace {
bool toDouble(const std::string& s, double& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(out);
}
std::vector<std::string> splitBar(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) { return out; }
    std::string cur;
    for (const char ch : s) {
        if (ch == '|') { out.push_back(cur); cur.clear(); } else { cur += ch; }
    }
    out.push_back(cur);
    return out;
}
}  // namespace

std::vector<SoapySettingView> readSoapySettings(const std::function<std::string(const std::string&)>& fact, int radio) {
    std::vector<SoapySettingView> out;
    const std::string pre = "radio." + std::to_string(radio) + ".soapy.";
    const std::string cnt = fact(pre + "count");
    if (cnt.empty() || cnt.find_first_not_of("0123456789") != std::string::npos || cnt.size() > 4) { return out; }
    const int n = std::atoi(cnt.c_str());
    for (int i = 0; i < n && i < 64; ++i) {
        const std::string p = pre + std::to_string(i) + ".";
        SoapySettingView s;
        s.key = fact(p + "key");
        if (s.key.empty()) { continue; }
        s.name = fact(p + "name");
        if (s.name.empty()) { s.name = s.key; }
        s.description = fact(p + "description");
        s.units = fact(p + "units");
        s.type = fact(p + "type");
        if (s.type != "bool" && s.type != "int" && s.type != "float") { s.type = "string"; }
        s.value = fact(p + "value");
        s.def = fact(p + "default");
        double lo = 0.0, hi = 0.0;
        if (toDouble(fact(p + "min"), lo) && toDouble(fact(p + "max"), hi) && hi > lo) { s.hasRange = true; s.lo = lo; s.hi = hi; }
        s.options = splitBar(fact(p + "options"));
        s.optionNames = splitBar(fact(p + "optionNames"));
        while (s.optionNames.size() < s.options.size()) { s.optionNames.push_back(s.options[s.optionNames.size()]); }
        out.push_back(std::move(s));
    }
    return out;
}

bool readSoapyBandwidth(const std::function<std::string(const std::string&)>& fact, int radio, double& currentHz, double& loHz, double& hiHz) {
    const std::string pre = "radio." + std::to_string(radio) + ".soapy.";
    double cur = 0.0, lo = 0.0, hi = 0.0;
    if (!toDouble(fact(pre + "bandwidth"), cur) || !toDouble(fact(pre + "bandwidthLo"), lo) || !toDouble(fact(pre + "bandwidthHi"), hi)) { return false; }
    currentHz = cur; loHz = lo; hiHz = hi;
    return true;
}

std::string soapyBandwidthWords(double hz) { return numberText(hz / 1000.0) + " kHz"; }

bool parseSoapyBandwidthKHz(const std::string& text, double loHz, double hiHz, double& hz, std::string& err) {
    double khz = 0.0;
    if (!parseNumber(text, 0.000001, 1.0e9, khz, err)) { return false; }
    const double v = khz * 1000.0;
    if (v < loHz || v > hiHz) { err = tr("allowed:") + " " + numberText(loHz / 1000.0) + " - " + numberText(hiHz / 1000.0) + " kHz"; return false; }
    hz = v;
    return true;
}

bool soapyValueText(const SoapySettingView& s, const std::string& typedIn, std::string& out, std::string& err) {
    const std::string typed = s.type == "string" && s.options.empty() ? typedIn : trim(typedIn);
    if (!s.options.empty()) {
        for (const std::string& o : s.options) { if (o == typed) { out = typed; return true; } }
        err = tr("that is not one of the choices the driver lists");
        return false;
    }
    if (s.type == "bool") {
        if (typed == "1" || typed == "true") { out = "true"; return true; }
        if (typed == "0" || typed == "false") { out = "false"; return true; }
        err = tr("this setting is on or off");
        return false;
    }
    if (s.type == "int" || s.type == "float") {
        double v = 0.0;
        if (!toDouble(typed, v)) { err = tr("type a number"); return false; }
        if (s.type == "int" && v != std::floor(v)) { err = tr("type a whole number"); return false; }
        if (s.hasRange && (v < s.lo || v > s.hi)) { err = tr("allowed:") + " " + soapyRangeWords(s); return false; }
        out = typed;
        return true;
    }
    if (typed.size() > 200) { err = tr("that is too long"); return false; }
    for (const char c : typed) { if (static_cast<unsigned char>(c) < 0x20) { err = tr("no control characters"); return false; } }
    out = typed;
    return true;
}

bool soapyWriteCommand(const std::string& key, const std::string& value, std::string& out, std::string& err) {
    if (key.empty() || key.find('=') != std::string::npos) { err = tr("this setting's name cannot be written from here"); return false; }
    out = "soapy:" + key + "=" + value;
    if (out.size() > 255) { out.clear(); err = tr("that is too long"); return false; }
    return true;
}

std::string soapySettingLabel(const SoapySettingView& s) { return s.units.empty() ? s.name : s.name + " (" + s.units + ")"; }

std::string soapyRangeWords(const SoapySettingView& s) { return s.hasRange ? numberText(s.lo) + " to " + numberText(s.hi) : std::string(); }
}  // namespace foxsdr::slints5b
