// Command-line options for the application: frequency, mode, radio, start running, open an I/Q file, profile folder. Strict validation (every problem is
// reported, nothing is guessed), and a help text built from one option table so the wiring lane can translate the strings.
//
// STANDALONE. Nothing here is wired: no main() and no engine start-up reads it (docs/feature-records/recording-models.md, Part 9). The parser works on
// a vector of UTF-8 arguments WITHOUT the program name, touches no file, reads no environment and has no global state; the optional `fileExists` /
// `dirExists` callbacks in ParseConfig let the caller add existence checks.
//
// SYNTAX. `--name value` and `--name=value`; short forms -f -m -r -s -i -p -h; `--` ends the options. One bare argument is taken as an I/Q file to open
// (the way a file association or a drag onto the program passes it) when its extension is a known I/Q one. Every option may be given once.
// FREQUENCY VALUES. Digits with an optional single decimal point and an optional unit: Hz, k / kHz, M / MHz, G / GHz (case-insensitive: `m` is MHz here,
// as in radio practice, never milli); no sign, no exponent, no thousands separators, no locale decimal comma. 1 Hz .. 10 THz.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cascade::core::cli {

struct Options {
    std::optional<double> frequencyHz;
    std::optional<std::string> mode;          // canonical spelling from the mode list
    std::optional<std::string> radio;         // a radio's name or key, as the radio list shows it
    bool startRunning = false;
    std::optional<std::string> iqFile;
    std::optional<double> iqSampleRateHz;     // for a raw I/Q file
    std::optional<double> iqCenterHz;         // for a raw I/Q file
    std::optional<std::string> profileDir;
    bool help = false;
};

struct ParseConfig {
    // The mode names the program knows, canonical spelling (dsp/demod.hpp modeName: NFM WFM AM DSB USB LSB CW RAW). Matching is case-insensitive.
    std::vector<std::string> modes = {"NFM", "WFM", "AM", "DSB", "USB", "LSB", "CW", "RAW"};
    // Extensions an I/Q file may have (source/iq_readers.hpp openAuto), lowercase with the dot.
    std::vector<std::string> iqExtensions = {".wav", ".rf64", ".cu8", ".cs16", ".cf32", ".sigmf-meta", ".sigmf-data"};
    std::function<bool(const std::string&)> fileExists;     // optional: when set, an I/Q file that does not exist is an error
    // Options that another part of the start-up owns (today: --config PATH, --use-config, --frames N in engine/process_boot.cpp): (name, takes a value).
    // They are skipped, not errors, and handed back in ParseResult::passedThrough so the caller can check they were meant.
    std::vector<std::pair<std::string, bool>> passThrough;
    std::function<bool(const std::string&)> dirExists;      // optional: when set, a profile folder that does not exist is an error (a missing one may also be created later: leave unset)
};

struct ParseResult {
    bool ok = false;                          // no errors (help requests are ok)
    Options options;
    std::vector<std::string> errors;
    std::vector<std::string> passedThrough;   // the skipped arguments, in order
};

// One row of the option table: used for the help text and for translation.
struct OptionDoc {
    const char* longName;
    const char* shortName;                    // "" when none
    const char* argument;                     // "" for a flag
    const char* description;
};

inline const std::vector<OptionDoc>& optionTable() {
    static const std::vector<OptionDoc> t = {
        {"--freq", "-f", "FREQUENCY", "Tune to this frequency on start (for example 7.074M, 145500k, 100000000)."},
        {"--mode", "-m", "MODE", "Select this mode on start (NFM, WFM, AM, DSB, USB, LSB, CW, RAW)."},
        {"--radio", "-r", "NAME", "Open this radio on start (its name as the radio list shows it)."},
        {"--start", "-s", "", "Start running as soon as the program has opened."},
        {"--iq-file", "-i", "FILE", "Open this I/Q recording instead of a radio (.wav, .cu8, .cs16, .cf32, .sigmf-meta). A lone file name does the same."},
        {"--iq-rate", "", "RATE", "Sample rate of a raw I/Q file (.cu8, .cs16, .cf32), for example 2.4M."},
        {"--iq-center", "", "FREQUENCY", "Centre frequency of a raw I/Q file."},
        {"--profile", "-p", "FOLDER", "Use this folder for the settings and data (a separate profile)."},
        {"--help", "-h", "", "Show this help and exit."},
    };
    return t;
}

inline std::string helpText(const std::string& programName = "foxsdr") {
    std::string s = "Usage: " + programName + " [options] [I/Q file]\n\nOptions:\n";
    for (const OptionDoc& o : optionTable()) {
        std::string left = "  ";
        if (o.shortName[0] != '\0') { left += std::string(o.shortName) + ", "; } else { left += "    "; }
        left += o.longName;
        if (o.argument[0] != '\0') { left += std::string(" ") + o.argument; }
        while (left.size() < 32) { left.push_back(' '); }
        s += left + " " + o.description + "\n";
    }
    s += "\nFrequencies and rates: digits with an optional unit (Hz, k, M, G); 7.074M is 7,074,000 Hz.\n";
    s += "An I/Q file cannot be combined with --radio.\n";
    return s;
}

namespace detail {

inline std::string lower(std::string s) { for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } } return s; }

// Strict frequency / rate parser. Empty optional + *why on refusal.
inline std::optional<double> parseHz(const std::string& text, std::string* why) {
    auto bad = [&](const char* m) { if (why != nullptr) { *why = m; } return std::optional<double>{}; };
    if (text.empty()) { return bad("empty value"); }
    std::size_t i = 0;
    std::string intPart, fracPart;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') { intPart.push_back(text[i++]); }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') { fracPart.push_back(text[i++]); }
    }
    if (intPart.empty() && fracPart.empty()) { return bad("not a number (digits, an optional decimal point and an optional unit)"); }
    if (intPart.size() > 14 || fracPart.size() > 12) { return bad("too many digits"); }
    const std::string unit = lower(text.substr(i));
    int unitExp = 0;
    if (unit.empty() || unit == "hz") { unitExp = 0; }
    else if (unit == "k" || unit == "khz") { unitExp = 3; }
    else if (unit == "m" || unit == "mhz") { unitExp = 6; }
    else if (unit == "g" || unit == "ghz") { unitExp = 9; }
    else { return bad("unknown unit (use Hz, k, M or G)"); }
    // The value is the EXACT decimal the user typed times the unit, rounded once: "2.4M" is 2400000, not 2400000.0000000005 as digit-by-digit
    // floating-point arithmetic would give.
    const std::string text10 = (intPart.empty() ? std::string("0") : intPart) + "." + (fracPart.empty() ? std::string("0") : fracPart) + "e" + std::to_string(unitExp);
    double v = 0.0;
    const auto res = std::from_chars(text10.data(), text10.data() + text10.size(), v);
    if (res.ec != std::errc() || res.ptr != text10.data() + text10.size()) { return bad("not a number"); }
    if (!(v >= 1.0)) { return bad("must be at least 1 Hz"); }
    if (v > 1e13) { return bad("above 10 THz"); }
    return v;
}

// A path or name from the command line: non-empty, no control characters, bounded.
inline bool plainText(const std::string& s, std::size_t maxLen, std::string* why) {
    if (s.empty()) { if (why != nullptr) { *why = "empty value"; } return false; }
    if (s.size() > maxLen) { if (why != nullptr) { *why = "too long"; } return false; }
    for (unsigned char c : s) { if (c < 32 || c == 127) { if (why != nullptr) { *why = "contains a control character"; } return false; } }
    return true;
}

// Characters Windows does not allow in a path (the colon only as a drive letter at index 1).
inline bool pathChars(const std::string& s, std::string* why) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') { if (why != nullptr) { *why = std::string("the character '") + c + "' is not allowed in a path"; } return false; }
        if (c == ':' && !(i == 1 && s.size() > 2 && (s[2] == '\\' || s[2] == '/') && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))) { if (why != nullptr) { *why = "a colon is only allowed in a drive prefix such as C:\\"; } return false; }
    }
    return true;
}

inline std::string extensionOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) { return {}; }
    return lower(name.substr(dot));
}

inline std::size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) { prev[j] = j; }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) { cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u)}); }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace detail

inline ParseResult parse(const std::vector<std::string>& args, const ParseConfig& cfg = {}) {
    ParseResult r;
    auto err = [&](const std::string& m) { r.errors.push_back(m); };
    if (args.size() > 64) { err("too many arguments (at most 64)"); r.ok = false; return r; }
    for (const std::string& a : args) { if (a.size() > 4096) { err("an argument is longer than 4096 bytes"); r.ok = false; return r; } }

    Options& o = r.options;
    bool seenFreq = false, seenMode = false, seenRadio = false, seenStart = false, seenIq = false, seenRate = false, seenCenter = false, seenProfile = false, seenHelp = false;
    std::vector<std::string> positional;

    auto canonicalOption = [&](const std::string& name) -> std::string {
        for (const OptionDoc& d : optionTable()) { if (name == d.longName || (d.shortName[0] != '\0' && name == d.shortName)) { return d.longName; } }
        return {};
    };
    auto takesValue = [&](const std::string& longName) { for (const OptionDoc& d : optionTable()) { if (longName == d.longName) { return d.argument[0] != '\0'; } } return false; };

    bool optionsEnded = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (optionsEnded || a.empty() || a[0] != '-' || a == "-") { if (a.empty()) { err("an empty argument"); } else { positional.push_back(a); } continue; }
        if (a == "--") { optionsEnded = true; continue; }
        std::string name = a, value;
        bool hasValue = false;
        if (a.rfind("--", 0) == 0) {
            const std::size_t eq = a.find('=');
            if (eq != std::string::npos) { name = a.substr(0, eq); value = a.substr(eq + 1); hasValue = true; }
        } else if (a.size() > 2) {
            // -fVALUE: a short option with its value attached
            name = a.substr(0, 2); value = a.substr(2); hasValue = true;
        }
        {
            bool passed = false;
            for (const auto& pt : cfg.passThrough) {
                if (name != pt.first) { continue; }
                passed = true;
                r.passedThrough.push_back(a);
                if (pt.second && !hasValue && i + 1 < args.size()) { r.passedThrough.push_back(args[++i]); }
                break;
            }
            if (passed) { continue; }
        }
        const std::string lname = canonicalOption(name);
        if (lname.empty()) {
            std::string m = "unknown option " + name;
            if (name.rfind("--", 0) == 0) {
                std::size_t best = 99; std::string bestName;
                for (const OptionDoc& d : optionTable()) { const std::size_t dist = detail::editDistance(name, d.longName); if (dist < best) { best = dist; bestName = d.longName; } }
                if (best <= 2) { m += " (did you mean " + bestName + "?)"; }
            }
            err(m);
            continue;
        }
        if (takesValue(lname)) {
            if (!hasValue) {
                if (i + 1 >= args.size()) { err("option " + lname + " needs a value"); continue; }
                value = args[++i];
            }
        } else if (hasValue) {
            err("option " + lname + " takes no value");
            continue;
        }

        std::string why;
        auto dup = [&](bool& seen) { if (seen) { err("option " + lname + " was given more than once"); return true; } seen = true; return false; };
        if (lname == "--help") { if (!dup(seenHelp)) { o.help = true; } }
        else if (lname == "--start") { if (!dup(seenStart)) { o.startRunning = true; } }
        else if (lname == "--freq") {
            if (dup(seenFreq)) { continue; }
            const auto v = detail::parseHz(value, &why);
            if (!v) { err("--freq '" + value + "': " + why); } else { o.frequencyHz = *v; }
        } else if (lname == "--iq-rate") {
            if (dup(seenRate)) { continue; }
            const auto v = detail::parseHz(value, &why);
            if (!v) { err("--iq-rate '" + value + "': " + why); } else { o.iqSampleRateHz = *v; }
        } else if (lname == "--iq-center") {
            if (dup(seenCenter)) { continue; }
            const auto v = detail::parseHz(value, &why);
            if (!v) { err("--iq-center '" + value + "': " + why); } else { o.iqCenterHz = *v; }
        } else if (lname == "--mode") {
            if (dup(seenMode)) { continue; }
            const std::string want = detail::lower(value);
            std::string found;
            for (const std::string& m : cfg.modes) { if (detail::lower(m) == want) { found = m; break; } }
            if (found.empty()) {
                std::string list;
                for (const std::string& m : cfg.modes) { list += (list.empty() ? "" : ", ") + m; }
                err("--mode '" + value + "' is not a mode (" + list + ")");
            } else { o.mode = found; }
        } else if (lname == "--radio") {
            if (dup(seenRadio)) { continue; }
            if (!detail::plainText(value, 128, &why)) { err("--radio: " + why); } else { o.radio = value; }
        } else if (lname == "--iq-file") {
            if (dup(seenIq)) { continue; }
            if (!detail::plainText(value, 1024, &why) || !detail::pathChars(value, &why)) { err("--iq-file: " + why); continue; }
            o.iqFile = value;
        } else if (lname == "--profile") {
            if (dup(seenProfile)) { continue; }
            if (!detail::plainText(value, 1024, &why) || !detail::pathChars(value, &why)) { err("--profile: " + why); continue; }
            o.profileDir = value;
        }
    }

    // the one bare argument
    if (!positional.empty()) {
        if (positional.size() > 1) { err("more than one file name given (" + std::to_string(positional.size()) + ")"); }
        else if (seenIq) { err("a file name was given as well as --iq-file"); }
        else {
            std::string why;
            if (!detail::plainText(positional[0], 1024, &why) || !detail::pathChars(positional[0], &why)) { err("file name '" + positional[0] + "': " + why); }
            else { o.iqFile = positional[0]; seenIq = true; }
        }
    }

    // checks on the combination and on the things named
    if (o.iqFile) {
        const std::string ext = detail::extensionOf(*o.iqFile);
        if (std::find(cfg.iqExtensions.begin(), cfg.iqExtensions.end(), ext) == cfg.iqExtensions.end()) {
            std::string list;
            for (const std::string& e : cfg.iqExtensions) { list += (list.empty() ? "" : ", ") + e; }
            err("'" + *o.iqFile + "' is not an I/Q file this program opens (" + list + ")");
        } else if (cfg.fileExists && !cfg.fileExists(*o.iqFile)) {
            err("the I/Q file '" + *o.iqFile + "' does not exist");
        }
        if (o.radio) { err("--iq-file and --radio cannot be used together (choose a file or a radio)"); }
        const bool raw = ext == ".cu8" || ext == ".cs16" || ext == ".cf32";
        if (!raw && (o.iqSampleRateHz || o.iqCenterHz)) { err("--iq-rate and --iq-center only apply to raw files (.cu8, .cs16, .cf32): the other formats carry their own"); }
    } else if (o.iqSampleRateHz || o.iqCenterHz) {
        err("--iq-rate and --iq-center need an I/Q file");
    }
    if (o.profileDir && cfg.dirExists && !cfg.dirExists(*o.profileDir)) { err("the profile folder '" + *o.profileDir + "' does not exist"); }

    r.ok = r.errors.empty();
    return r;
}

// For a main(argc, argv): converts, skipping argv[0].
inline ParseResult parseMain(int argc, const char* const* argv, const ParseConfig& cfg = {}) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) { args.emplace_back(argv[i] != nullptr ? argv[i] : ""); }
    return parse(args, cfg);
}

}  // namespace cascade::core::cli
