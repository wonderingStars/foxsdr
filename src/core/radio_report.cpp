// radio_report.cpp - see radio_report.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_report.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <random>
#include <regex>
#include <cstring>
#include <set>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"

namespace cascade::core::radioreport {

namespace {

bool isWordChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSlash(char c) { return c == '\\' || c == '/'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) { c = lower(c); }
    return o;
}

bool startsWithNoCase(const std::string& s, std::size_t at, const char* word) {
    for (std::size_t i = 0; word[i] != '\0'; ++i) {
        if (at + i >= s.size() || lower(s[at + i]) != word[i]) { return false; }
    }
    return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// The characters a path token cannot contain: white space, quotes and the closers that end a clause. (A "<user>" mask inside it is kept: the
// angle brackets are not terminators.)
bool pathStop(char c) {
    return isSpace(c) || c == '"' || c == '\'' || c == ',' || c == ';' || c == ')' || c == ']' || c == '}' || c == '|' || c == '*' || c == '?';
}

}  // namespace

// ---------------------------------------------------------------------------
// The text rules
// ---------------------------------------------------------------------------

bool pathInText(const std::string& s) {
    // A drive path: letter, colon, slash - where the letter does not belong to a longer word ("tcp://").
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (isAlpha(s[i]) && s[i + 1] == ':' && isSlash(s[i + 2]) && (i == 0 || !isWordChar(s[i - 1]))) { return true; }
    }
    // \\host\share (the scrub writes \\<host>\share, which is not matched)
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != '\\' || s[i + 1] != '\\') { continue; }
        std::size_t j = i + 2;
        const auto hostChar = [](char c) { return isWordChar(c) || c == '.' || c == '$' || c == '-'; };
        std::size_t h = j;
        while (h < s.size() && hostChar(s[h])) { ++h; }
        if (h > j && h + 1 < s.size() && s[h] == '\\' && (isWordChar(s[h + 1]) || s[h + 1] == '$' || s[h + 1] == '.' || s[h + 1] == '-')) { return true; }
    }
    // \Users\x, /Users/x, /home/x: anything but the scrub's <user>
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!isSlash(s[i])) { continue; }
        std::size_t after = 0;
        if (startsWithNoCase(s, i + 1, "users") && i + 6 < s.size() && isSlash(s[i + 6])) { after = i + 7; }
        else if (startsWithNoCase(s, i + 1, "home") && i + 5 < s.size() && isSlash(s[i + 5])) { after = i + 6; }
        else { continue; }
        std::size_t e = after;
        while (e < s.size() && !isSlash(s[e]) && !isSpace(s[e])) { ++e; }
        if (s.compare(after, std::min<std::size_t>(6, e - after), "<user>") != 0 || e - after < 6) { return true; }
    }
    return false;
}

bool addressInText(const std::string& s) {
    static const std::regex kQuad("[0-9]{1,3}(?:\\.[0-9]{1,3}){3}");
    for (std::sregex_iterator it(s.begin(), s.end(), kQuad), end; it != end; ++it) {
        const std::size_t i = static_cast<std::size_t>(it->position(0));
        const std::size_t e = i + static_cast<std::size_t>(it->length(0));
        const char before = i > 0 ? s[i - 1] : '\0';
        const char after = e < s.size() ? s[e] : '\0';
        if (i > 0 && (isWordChar(before) || before == '.')) { continue; }
        if (e < s.size() && isWordChar(after)) { continue; }
        if (after == '.' && e + 1 < s.size() && isDigit(s[e + 1])) { continue; }
        // the four octets
        int octet[4] = {0, 0, 0, 0};
        int n = 0;
        int cur = 0;
        for (std::size_t k = i; k < e; ++k) {
            if (s[k] == '.') { octet[n++] = cur; cur = 0; } else { cur = cur * 10 + (s[k] - '0'); }
        }
        octet[n] = cur;
        bool over = false;
        for (int o : octet) { if (o > 255) { over = true; } }
        if (over) { continue; }
        const std::string text = s.substr(i, e - i);
        if (octet[0] == 127 || text == "0.0.0.0") { continue; }
        // a four-part version is not an address
        std::size_t from = i > 16 ? i - 16 : 0;
        std::string lead = lowerCopy(s.substr(from, i - from));
        while (!lead.empty() && (isSpace(lead.back()) || lead.back() == ':' || lead.back() == '=')) { lead.pop_back(); }
        const auto endsWith = [&lead](const char* w) {
            const std::string x(w);
            return lead.size() >= x.size() && lead.compare(lead.size() - x.size(), x.size(), x) == 0;
        };
        if (endsWith("version") || endsWith("firmware") || endsWith("build") || endsWith("package")) { continue; }
        return true;
    }
    return false;
}

bool serialInText(const std::string& s, const std::string& ownSerial) {
    if (ownSerial.size() >= 4 && lowerCopy(s).find(lowerCopy(ownSerial)) != std::string::npos) { return true; }
    static const std::regex kKey("(serial(?:_number)?|s/n|_sn|\\bsn)\\b(?:\\s*(?:number|no\\.?|#))?\\s*[:=]?\\s*([^\\s,;)\"']*)",
                                 std::regex::ECMAScript | std::regex::icase);
    static const std::set<std::string> kBenign = {"port", "ports", "bus", "interface", "cable", "adapter", "line", "lines", "connection", "communication"};
    for (std::sregex_iterator it(s.begin(), s.end(), kKey), end; it != end; ++it) {
        std::string value = (*it)[2].str();
        while (!value.empty() && (value.back() == '.' || value.back() == '!' || value.back() == '?' || value.back() == ':')) { value.pop_back(); }
        if (value.empty() || value == "<stripped>") { continue; }
        if (kBenign.count(lowerCopy(value)) != 0) { continue; }
        return true;
    }
    return false;
}

bool frequencyInText(const std::string& s) {
    static const std::regex kHz("[0-9][0-9.,]*\\s?[kmg]?hz\\b", std::regex::ECMAScript | std::regex::icase);
    return std::regex_search(s, kHz);
}

std::string textCode(const std::string& s, const std::string& ownSerial) {
    if (pathInText(s)) { return "path-in-text"; }
    if (addressInText(s)) { return "address-in-text"; }
    if (serialInText(s, ownSerial)) { return "serial-in-text"; }
    if (frequencyInText(s)) { return "frequency-in-text"; }
    return std::string();
}

// ---------------------------------------------------------------------------
// The radio key
// ---------------------------------------------------------------------------

std::string modelSlug(const std::string& model) {
    std::string out;
    bool dash = false;
    for (char c : model) {
        const char l = lower(c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) {
            if (dash && !out.empty()) { out.push_back('-'); }
            dash = false;
            out.push_back(l);
        } else {
            dash = true;
        }
    }
    if (out.size() > 40) { out.resize(40); }
    while (!out.empty() && out.back() == '-') { out.pop_back(); }
    return out;
}

std::string radioKey(const std::string& model, const std::string& serial) {
    std::string key = modelSlug(model) + ":";
    if (serial.empty()) { return key + "-"; }
    for (char c : serial) { key.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c); }
    return key;
}

// ---------------------------------------------------------------------------
// Which radio
// ---------------------------------------------------------------------------

namespace {

bool serialShaped(const std::string& v) {
    if (v.empty() || v.size() > 40) { return false; }
    for (char c : v) {
        if (!(isWordChar(c) && c != '_') && c != '.' && c != '-' && c != '_') { return false; }
    }
    return true;
}

// The value of `name` in "a=1, b=2" or "a=1,b=2" (case-insensitive name, trimmed); "" when absent.
std::string kwarg(const std::string& args, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= args.size()) {
        std::size_t end = args.find(',', pos);
        if (end == std::string::npos) { end = args.size(); }
        std::string field = args.substr(pos, end - pos);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos) {
            std::string k = field.substr(0, eq);
            std::string v = field.substr(eq + 1);
            const auto trim = [](std::string& t) {
                std::size_t a = 0;
                while (a < t.size() && isSpace(t[a])) { ++a; }
                std::size_t b = t.size();
                while (b > a && isSpace(t[b - 1])) { --b; }
                t = t.substr(a, b - a);
            };
            trim(k);
            trim(v);
            if (lowerCopy(k) == name) { return v; }
        }
        if (end == args.size()) { break; }
        pos = end + 1;
    }
    return std::string();
}

}  // namespace

std::string serialFromArgs(const std::string& driverKind, const std::string& args) {
    const std::string kind = lowerCopy(driverKind);
    // These have no hardware serial in their arguments: an address, a card, a file.
    if (kind == "rtltcp" || kind == "pluto" || kind == "soundcard" || kind == "file" || kind == "siggen" || kind == "other") {
        return std::string();
    }
    const std::string v = kwarg(args, "serial");
    return serialShaped(v) ? v : std::string();
}

RadioIdentity identify(const std::string& driverKind, const std::string& args, const std::string& nativeLabel) {
    RadioIdentity id;
    id.driver = health::driverWord(driverKind);
    std::string model;
    if (lowerCopy(driverKind) == "soapy") {
        model = sanitiseDevice(args);
    } else if (!nativeLabel.empty()) {
        // the bus-reported label is "<model> (serial <serial>)": the model is what comes before the serial
        const std::size_t at = nativeLabel.find(" (serial ");
        model = at == std::string::npos ? nativeLabel : nativeLabel.substr(0, at);
    } else {
        model = nativeFamilyModel(driverKind);
    }
    model = scrubModel(model);
    if (model.empty() || model == args) { model = "other"; }
    model = scrubModel(clipUtf8(model, 32));
    if (model.empty()) { model = "other"; }
    id.model = model;
    id.serial = serialFromArgs(driverKind, args);
    id.key = radioKey(id.model, id.serial);
    return id;
}

RadioIdentity soundCardIdentity() {
    RadioIdentity id;
    id.driver = "soundcard";
    id.model = "sound card";
    id.key = radioKey(id.model, id.serial);
    return id;
}

// ---------------------------------------------------------------------------
// The scrub
// ---------------------------------------------------------------------------

std::size_t utf8Length(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) { ++n; }
    }
    return n;
}

std::string clipUtf8(const std::string& s, std::size_t maxChars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) {
            if (chars == maxChars) { return s.substr(0, i); }
            ++chars;
        }
    }
    return s;
}

namespace {

// A Linux or macOS path root that holds a person's files, devices or temporary files (R9, security review 2): /media/<user>/..., /run/media/<user>/..., /mnt/..., /root/...,
// /dev/bus/usb/..., /tmp, /var, /etc, /opt, /srv, /proc, /sys, /Volumes. It counts at `i` when the slash does not belong to a longer word ("and/or", "https://host/media").
bool unixRootAt(const std::string& s, std::size_t i) {
    if (s[i] != '/') { return false; }
    if (i > 0 && (isWordChar(s[i - 1]) || s[i - 1] == '/' || s[i - 1] == '.' || s[i - 1] == '-')) { return false; }
    static const char* const kRoots[] = {"media", "run", "mnt", "root", "dev", "tmp", "var", "etc", "opt", "srv", "proc", "sys", "volumes", "snap"};
    for (const char* r : kRoots) {
        if (!startsWithNoCase(s, i + 1, r)) { continue; }
        const std::size_t after = i + 1 + std::strlen(r);
        // "/dev/x" and "/mnt" at the end of a clause are paths; "/devices" and "/mntx" are other words
        if (after >= s.size() || isSlash(s[after]) || pathStop(s[after])) { return true; }
    }
    return false;
}

// A Windows device path or USB instance path: \\?\usb#vid_...#serial#{guid}, \\.\..., USB\VID_xxxx&PID_xxxx\<serial>, usb#vid_xxxx&pid_xxxx#<serial>.
bool devicePathAt(const std::string& s, std::size_t i) {
    if (i + 3 < s.size() && s[i] == '\\' && s[i + 1] == '\\' && (s[i + 2] == '?' || s[i + 2] == '.') && s[i + 3] == '\\') { return true; }
    if (i > 0 && isWordChar(s[i - 1])) { return false; }
    return startsWithNoCase(s, i, "usb\\vid_") || startsWithNoCase(s, i, "usb#vid_") || startsWithNoCase(s, i, "usb/vid_");
}

}  // namespace

std::string pathsToToken(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        bool path = false;
        if (devicePathAt(s, i) || unixRootAt(s, i)) {
            std::size_t j = i + 1;
            if (s[i] == '\\') { j = i + 4; }   // past the \\?\ prefix: its question mark is not the end of the path
            while (j < s.size() && !(pathStop(s[j]) && !(s[j] == '?'))) { ++j; }
            out += "<path>";
            i = j;
            continue;
        }
        if (i + 2 < s.size() && isAlpha(s[i]) && s[i + 1] == ':' && isSlash(s[i + 2]) && (i == 0 || !isWordChar(s[i - 1]))) { path = true; }
        // a network path, including one the upload scrub has already half-masked (two backslashes, <host>, then the share and the file): what follows the
        // masked host name is the user's
        else if (i + 2 < s.size() && s[i] == '\\' && s[i + 1] == '\\' && (isWordChar(s[i + 2]) || s[i + 2] == '.' || s[i + 2] == '$' || s[i + 2] == '<')) { path = true; }
        else if (isSlash(s[i]) && ((startsWithNoCase(s, i + 1, "users") && i + 6 < s.size() && isSlash(s[i + 6])) ||
                                   (startsWithNoCase(s, i + 1, "home") && i + 5 < s.size() && isSlash(s[i + 5])))) { path = true; }
        if (!path) { out.push_back(s[i]); ++i; continue; }
        std::size_t j = i + 1;
        while (j < s.size() && !pathStop(s[j])) { ++j; }
        out += "<path>";
        i = j;
    }
    return out;
}

namespace {

std::string stripSerial(const std::string& s, const std::string& serial) {
    if (serial.size() < 4) { return s; }
    const std::string low = lowerCopy(s);
    const std::string key = lowerCopy(serial);
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t at = low.find(key, pos);
        if (at == std::string::npos) { out.append(s, pos, std::string::npos); break; }
        out.append(s, pos, at - pos);
        out += "<stripped>";
        pos = at + key.size();
    }
    return out;
}

std::string stripFrequencies(const std::string& s) {
    static const std::regex kHz("[0-9][0-9.,]*\\s?[kmg]?hz\\b", std::regex::ECMAScript | std::regex::icase);
    return std::regex_replace(s, kHz, "#");
}

}  // namespace

// A NUMBER THAT LOOKS LIKE A FREQUENCY WITHOUT SAYING SO: a run of six or more digits, or a decimal with three or more decimals, that no unit explains.
// scrubUploadLine masks the numbers of a line that mentions hertz, tuning or a centre; a driver's "open at 433920000" says none of those. A sample rate
// ("2400000 S/s"), a time, a size or a count carries its unit and stays; a version ("0.99.109") has two dots and stays.
static std::string maskBareFrequencies(const std::string& s) {
    static const char* const kSafeUnits[] = {"s/s", "sps", "msps", "ms", "us", "ns", "s", "b", "kb", "mb", "gb", "bytes", "samples", "sample", "blocks", "db", "%", "x"};
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (!isDigit(s[i]) || (i > 0 && (isWordChar(s[i - 1]) || s[i - 1] == '.'))) { out.push_back(s[i]); ++i; continue; }
        std::size_t j = i;
        while (j < s.size() && isDigit(s[j])) { ++j; }
        const std::size_t intDigits = j - i;
        std::size_t decimals = 0;
        std::size_t end = j;
        if (j + 1 < s.size() && s[j] == '.' && isDigit(s[j + 1])) {
            std::size_t k = j + 1;
            while (k < s.size() && isDigit(s[k])) { ++k; }
            // a second dot makes it a version or an address, which the other rules judge
            if (k < s.size() && s[k] == '.' && k + 1 < s.size() && isDigit(s[k + 1])) {
                out.append(s, i, k - i);
                i = k;
                continue;
            }
            decimals = k - (j + 1);
            end = k;
        }
        // the unit that follows, if any
        std::size_t u = end;
        while (u < s.size() && s[u] == ' ') { ++u; }
        std::string unit;
        while (u < s.size() && (isAlpha(s[u]) || s[u] == '/' || s[u] == '%') && unit.size() < 8) { unit.push_back(lower(s[u])); ++u; }
        bool safe = false;
        for (const char* k : kSafeUnits) { if (unit == k) { safe = true; } }
        // (R9) a number near a word about tuning, with four digits or a decimal point and no unit, is a frequency in kHz or MHz written without its unit: "tuned to 14200", "freq 145.5"
        bool nearTuningWord = false;
        {
            const std::size_t from = i > 40 ? i - 40 : 0;
            const std::string window = lowerCopy(s.substr(from, i - from));
            for (const char* w : {"freq", "tun", "centre", "center", "vfo", "lo ", "channel"}) { if (window.find(w) != std::string::npos) { nearTuningWord = true; } }
        }
        const bool looksLikeFrequency = intDigits >= 6 || (decimals >= 3 && intDigits >= 1 && !(intDigits == 1 && s[i] == '0')) ||
                                        (nearTuningWord && (intDigits >= 4 || decimals >= 1) && !(intDigits == 1 && s[i] == '0' && decimals == 0));
        if (looksLikeFrequency && !safe) {
            out.push_back('#');
        } else {
            out.append(s, i, end - i);
        }
        i = end;
    }
    return out;
}

// ---------------------------------------------------------------------------
// WHAT THE SCRUB USED TO LET THROUGH (security reviews 1 and 2, L8 and R9): a MAC address, a host name in prose, a bare file name, another device's serial number and a
// frequency that carries no unit. Each is masked with a word in angle brackets, which no rule reads as data. Plain loops: no regular expression sees driver text.
// ---------------------------------------------------------------------------
namespace {

bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool nameChar(char c) { return (isWordChar(c)) || c == '.' || c == '-'; }   // a file or host name token (underscore included: isWordChar)

// aa:bb:cc:dd:ee:ff and aa-bb-cc-dd-ee-ff
std::string maskMacs(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        bool mac = false;
        if (i + 17 <= s.size() && (i == 0 || !isWordChar(s[i - 1])) && (i + 17 == s.size() || !isWordChar(s[i + 17]))) {
            const char sep = s[i + 2];
            if (sep == ':' || sep == '-') {
                mac = true;
                for (std::size_t k = 0; k < 17 && mac; ++k) {
                    mac = (k % 3 == 2) ? s[i + k] == sep : isHex(s[i + k]);
                }
            }
        }
        if (mac) { out += "<mac>"; i += 17; } else { out.push_back(s[i]); ++i; }
    }
    return out;
}

bool fileExtension(const std::string& ext) {
    static const std::set<std::string> kExt = {"iq", "wav", "cu8", "cs8", "cs16", "cf32", "cfile", "raw", "bin", "dat", "sigmf", "json", "cfg", "conf", "ini", "log", "txt", "csv",
                                               "xml", "exe", "dll", "so", "sys", "inf", "zip", "png", "jpg", "bmp", "mp3", "flac", "ogg", "gz", "tar", "bak", "tmp", "db",
                                               "sqlite", "pcap", "kml", "pdf", "lib", "dylib", "cat", "rtf", "html", "htm", "js", "py", "bat", "cmd", "ps1", "sh"};
    return kExt.count(lowerCopy(ext)) != 0;
}

bool hasLetter(const std::string& t) { for (char c : t) { if (isAlpha(c)) { return true; } } return false; }

// "name.ext" with a known extension -> <file>; "a.b.c" with a letters-only last label -> <host>
std::string maskNameTokens(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (!nameChar(s[i]) || (i > 0 && s[i - 1] == '<')) { out.push_back(s[i]); ++i; continue; }
        std::size_t j = i;
        while (j < s.size() && nameChar(s[j])) { ++j; }
        std::string tok = s.substr(i, j - i);
        // a sentence's full stop is not part of the name
        std::string tail;
        while (!tok.empty() && (tok.back() == '.' || tok.back() == '-')) { tail.insert(tail.begin(), tok.back()); tok.pop_back(); }
        const std::size_t dot = tok.rfind('.');
        std::string repl;
        if (dot != std::string::npos && dot > 0 && dot + 1 < tok.size()) {
            const std::string last = tok.substr(dot + 1);
            bool lettersOnly = true;
            for (char c : last) { if (!isAlpha(c)) { lettersOnly = false; } }
            if (fileExtension(last)) {
                repl = "<file>";
            } else if (lettersOnly && last.size() >= 2 && last.size() <= 24 && hasLetter(tok.substr(0, dot))) {
                // every label is letters, digits and hyphens, none empty
                bool labels = true;
                std::size_t from = 0;
                for (;;) {
                    const std::size_t d = tok.find('.', from);
                    const std::string label = tok.substr(from, d == std::string::npos ? std::string::npos : d - from);
                    if (label.empty()) { labels = false; }
                    for (char c : label) { if (!(isAlpha(c) || isDigit(c) || c == '-')) { labels = false; } }
                    if (d == std::string::npos) { break; }
                    from = d + 1;
                }
                if (labels) { repl = "<host>"; }
            }
        }
        if (repl.empty()) { out.append(s, i, j - i); } else { out += repl; out += tail; }
        i = j;
    }
    return out;
}

// "the host NAME", "host=NAME", "hostname: NAME": the word after the word host, whatever it looks like; and "on|to|at|from|via NAME:PORT"
std::string maskHostWords(const std::string& s) {
    static const std::set<std::string> kNotAName = {"the", "a", "an", "name", "is", "was", "not", "could", "cannot", "can", "at", "on", "for", "to", "address", "unknown", "unreachable",
                                                    "down", "failed", "refused", "did", "does", "no", "or", "and", "in", "of", "found", "resolve", "lookup", "<host>", "<mac>"};
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const bool wordStart = !isWordChar(s[i]) ? false : (i == 0 || !isWordChar(s[i - 1]));
        if (!wordStart) { out.push_back(s[i]); ++i; continue; }
        std::size_t w = i;
        while (w < s.size() && isWordChar(s[w])) { ++w; }
        const std::string word = lowerCopy(s.substr(i, w - i));
        out.append(s, i, w - i);
        i = w;
        const bool hostWord = word == "host" || word == "hostname";
        const bool joinWord = word == "on" || word == "to" || word == "at" || word == "from" || word == "via";
        if (!hostWord && !joinWord) { continue; }
        // skip the glue between the word and the name: spaces, a colon or equals sign, a quote
        std::size_t k = i;
        std::size_t glueEnd = k;
        while (glueEnd < s.size() && (s[glueEnd] == ' ' || s[glueEnd] == ':' || s[glueEnd] == '=' || s[glueEnd] == '"' || s[glueEnd] == '\'')) { ++glueEnd; }
        if (glueEnd == k) { continue; }
        std::size_t n = glueEnd;
        while (n < s.size() && nameChar(s[n])) { ++n; }
        if (n == glueEnd) { continue; }
        std::string name = s.substr(glueEnd, n - glueEnd);
        while (!name.empty() && (name.back() == '.' || name.back() == '-')) { name.pop_back(); }
        if (name.empty() || kNotAName.count(lowerCopy(name)) != 0 || !hasLetter(name)) { continue; }
        if (joinWord) {
            // only NAME:PORT counts after a join word (otherwise "on startup" would be a host)
            const std::size_t after = glueEnd + name.size();
            std::size_t p = after;
            if (p >= s.size() || s[p] != ':') { continue; }
            ++p;
            std::size_t digits = 0;
            while (p < s.size() && isDigit(s[p])) { ++p; ++digits; }
            if (digits == 0 || digits > 5) { continue; }
        }
        out.append(s, k, glueEnd - k);
        out += "<host>";
        i = glueEnd + name.size();
    }
    return out;
}

// A run of eight or more letters and digits that mixes both: another device's serial number, an instance id. (Words and plain numbers are not.)
std::string maskIdTokens(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (!(isAlpha(s[i]) || isDigit(s[i])) || (i > 0 && (s[i - 1] == '<' || s[i - 1] == '#' || s[i - 1] == '_'))) { out.push_back(s[i]); ++i; continue; }
        std::size_t j = i;
        std::size_t letters = 0, digits = 0;
        while (j < s.size() && (isAlpha(s[j]) || isDigit(s[j]))) { (isAlpha(s[j]) ? letters : digits)++; ++j; }
        const bool hex0x = j - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X');
        const bool beforeUnderscore = j < s.size() && s[j] == '_';
        if (j - i >= 8 && letters >= 1 && digits >= 2 && !hex0x && !beforeUnderscore) { out += "<id>"; } else { out.append(s, i, j - i); }
        i = j;
    }
    return out;
}

std::string maskNames(const std::string& s) {
    return maskIdTokens(maskNameTokens(maskHostWords(maskMacs(s))));
}

}  // namespace

std::string scrubText(const std::string& rawIn, const std::string& serial, std::size_t maxChars) {
    // THE LENGTH IS BOUNDED BEFORE ANY RULE RUNS (L8): a driver's sentence is a few hundred characters; the regular expressions below must never see a megabyte
    const std::string raw = clipUtf8(rawIn.size() > 2000 ? rawIn.substr(0, 2000) : rawIn, 2000);
    std::string t = maskBareFrequencies(maskMacs(scrubUploadLine(raw)));   // (a MAC first: its digit pairs are not frequencies)
    for (int pass = 0; pass < 4; ++pass) {
        t = stripSerial(t, serial);
        t = pathsToToken(t);
        t = maskNames(t);
        const std::string code = textCode(t, serial);
        if (code.empty()) { break; }
        if (code == "frequency-in-text") {
            t = stripFrequencies(t);
        } else {
            t = "withheld: " + code;
            break;
        }
    }
    // A line that still fails (it cannot, after the loop; belt and braces) is withheld whole.
    if (!textCode(t, serial).empty()) { t = "withheld: " + textCode(t, serial); }
    t = clipUtf8(t, maxChars);
    return t;
}

std::string scrubReason(const std::string& raw, const std::string& serial, std::size_t maxChars) {
    std::string t = scrubText(raw, serial, maxChars);
    // control characters cannot ride in a one-line report
    for (char& c : t) { if (c == '\n' || c == '\r' || c == '\t') { c = ' '; } }
    if (t.empty()) { t = "no reason was given"; }
    return t;
}

namespace {

const char* const kRadioPrefixes[] = {"source", "soapy", "vendor", "radio", "rtltcp", "aor", "rx888", "pluto", "mirisdr", "hackrf", "airspy",
                                      "airspyhf", "rtlsdr", "sdrplay", "hydrasdr", "usb", "patch"};

bool stampAt(const std::string& s) {
    // HH:MM:SS.mmm
    if (s.size() < 12) { return false; }
    const char* p = s.c_str();
    for (int i : {0, 1, 3, 4, 6, 7, 9, 10, 11}) { if (!isDigit(p[i])) { return false; } }
    return p[2] == ':' && p[5] == ':' && p[8] == '.';
}

}  // namespace

std::vector<std::string> radioLines(const std::vector<std::string>& ring, const std::string& serial, std::size_t maxLines) {
    std::vector<std::string> kept;
    for (const std::string& line : ring) {
        std::string stamp;
        std::string rest = line;
        if (stampAt(rest)) {
            stamp = rest.substr(0, 12);
            rest = rest.size() > 13 ? rest.substr(13) : std::string();
            // the ring's level word ("info", "warn", ...) sits between the stamp and the text
            const std::size_t sp = rest.find(' ');
            if (sp != std::string::npos && sp <= 5) {
                const std::string lvl = rest.substr(0, sp);
                if (lvl == "info" || lvl == "warn" || lvl == "error" || lvl == "debug" || lvl == "trace") { rest = rest.substr(sp + 1); }
            }
        }
        // the first word must be a radio prefix followed by ": "
        const std::size_t colon = rest.find(": ");
        if (colon == std::string::npos) { continue; }
        const std::string word = rest.substr(0, colon);
        bool radio = false;
        for (const char* p : kRadioPrefixes) { if (word == p) { radio = true; } }
        if (!radio) { continue; }
        // the time stamp is kept as the ring wrote it; the rest of the line is scrubbed on its own (its digits are not a frequency)
        std::string t = scrubText(rest, serial, stamp.empty() ? 240 : 227);
        if (!stamp.empty()) { t = stamp + " " + t; }
        if (t.empty() || !textCode(t, serial).empty()) { continue; }
        // the scrub may have withheld the whole line: that is no longer a radio line
        std::string again = t;
        if (stampAt(again) && again.size() > 13) { again = again.substr(13); }
        const std::size_t c2 = again.find(": ");
        if (c2 == std::string::npos) { continue; }
        bool still = false;
        for (const char* p : kRadioPrefixes) { if (again.substr(0, c2) == p) { still = true; } }
        if (!still) { continue; }
        kept.push_back(std::move(t));
    }
    if (kept.size() > maxLines) { kept.erase(kept.begin(), kept.end() - static_cast<std::ptrdiff_t>(maxLines)); }
    return kept;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string randomHex(std::size_t n) {
    if (n > 64) { n = 64; }
    std::string id;
    for (int attempt = 0; attempt < 3 && id.size() < n; ++attempt) {
        id = newInstallId();   // 32 hex characters from the system CSPRNG, or "" on failure
    }
    if (id.size() >= n) { return id.substr(0, n); }
    // The CSPRNG failed three times: a random message id is not a secret, so std::random_device is good enough here (and never for an install id).
    std::random_device rd;
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; ++i) { out.push_back(kHex[rd() & 0xFu]); }
    return out;
}

std::string versionText(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (isWordChar(c) && c != '_') { out.push_back(c); }
        else if (c == '.' || c == '+' || c == ' ' || c == '-') { out.push_back(c); }
    }
    if (out.size() > 48) { out.resize(48); }
    return out;
}

std::string bandName(double hz) {
    if (!(hz > 0.0)) { return "other"; }
    if (hz < 300e3) { return "lf"; }
    if (hz < 3e6) { return "mf"; }
    if (hz < 30e6) { return "hf"; }
    if (hz < 87.5e6) { return "vhf-low"; }
    if (hz < 108e6) { return "fm-broadcast"; }
    if (hz < 137e6) { return "air-band"; }
    if (hz < 300e6) { return "vhf-high"; }
    if (hz < 1e9) { return "uhf"; }
    if (hz < 2e9) { return "l-band"; }
    return "shf";
}

std::string cpuWithoutClock(const std::string& brand) {
    std::string t = brand;
    const std::size_t at = t.find('@');
    if (at != std::string::npos) { t = t.substr(0, at); }
    // collapse runs of spaces, trim
    std::string out;
    bool sp = false;
    for (char c : t) {
        if (isSpace(c)) { sp = true; continue; }
        if (sp && !out.empty()) { out.push_back(' '); }
        sp = false;
        out.push_back(c);
    }
    return out;
}

}  // namespace cascade::core::radioreport
