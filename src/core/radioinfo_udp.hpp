// radioinfo_udp.hpp - the "RadioInfo" UDP broadcast of logging programs as a READ-ONLY rig-state source: frequency, transmit frequency, mode, split,
// the transmitting STATE. It is the way DXLab Commander and N1MM Logger+ announce their radio, and the only DXLab Commander interface whose message
// format could be opened (its TCP command set is not documented on any page that opened: see the record). Receive and read only: this file sends
// nothing and has no call that sets anything on a rig.
//
// SOURCES (opened 2026-11-11):
//   * N1MM Logger+ documentation, https://n1mmwp.hamdocs.com/appendices/external-udp-broadcasts/ : the RadioInfo XML (elements app, StationName, RadioNr,
//     Freq, TXFreq, Mode, mycall, OpCall, IsRunning, ..., IsSplit, ActiveRadioNr, IsTransmitting, ...); "Freq: Receive frequency in 10 Hz units, no
//     delimiter (e.g., 80m: 352211)"; "TXFreq: same units as Freq"; "Mode: logging mode (CW, USB, LSB, RTTY, ...)"; sent every 10 seconds or when a field
//     changes; recommended port 12060; "IsTransmitting: True only when N1MM+ initiated the transmission"; the SO2V caution (TXFreq is only meaningful in a
//     packet whose RadioNr equals ActiveRadioNr, except in split). The page states no licence ("All rights reserved" footer only).
//   * DXLab Commander help, https://dxlabsuite.com/commander/Help/NetworkService.htm : Commander "sends" a RadioInfo message to up to four configured UDP
//     address/port pairs (127.0.0.1 for the same computer; the example port 12060 is N1MM+'s default) with Freq, TXFreq, Mode, IsSplit, RadioNr (1-4), App
//     (DXLab), RadioName; on a frequency/mode/split change, when the transceiver switches between Receiving and Transmitting, and every 10 seconds. The page
//     does not give the wire format; it is taken to be the N1MM one it names (UNVERIFIED against a real Commander).
//
// PRIVACY: the broadcast also carries the station call and operator call (mycall, OpCall) and a computer name (StationName). Those are NOT parsed and NOT kept
// here; only the radio fields above are.
//
// BOUNDS / HOSTILE INPUT: datagram at most 4096 bytes; element text at most 64 characters; any markup declaration (<!DOCTYPE, <!ENTITY) or processing
// instruction other than a leading <?xml ...?> is refused; character references other than the five predefined ones are refused; nesting deeper than 3 is
// refused. No entity is ever expanded beyond the five predefined ones.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace cascade::core::rig {

inline constexpr std::size_t kRadioInfoMaxDatagram = 4096;

struct RadioInfo {
    int radioNr = 0;           // 1..4
    int activeRadioNr = 0;     // 0 = not given
    double freqHz = 0.0;       // receive; 0 = not given
    double txFreqHz = 0.0;     // transmit; 0 = not given
    std::string mode;          // as the logger names it; empty = not given
    bool split = false;
    bool transmitting = false; // IsTransmitting, read-only state
    bool running = false;
    std::string app;           // sending program's short name (N1MM, DXLab); empty = not given
};

namespace radioinfo_detail {

inline bool decodeText(const std::string& in, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') { out.push_back(in[i]); ++i; continue; }
        const std::size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 6) { return false; }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") { out.push_back('&'); }
        else if (ent == "lt") { out.push_back('<'); }
        else if (ent == "gt") { out.push_back('>'); }
        else if (ent == "quot") { out.push_back('"'); }
        else if (ent == "apos") { out.push_back('\''); }
        else { return false; }   // numeric and named references other than the five: refused
        i = semi + 1;
    }
    return true;
}

inline bool parseBool(const std::string& s, bool& out) {
    std::string l;
    for (unsigned char c : s) { l.push_back(static_cast<char>(std::tolower(c))); }
    if (l == "true") { out = true; return true; }
    if (l == "false") { out = false; return true; }
    return false;
}

inline bool parseUint(const std::string& s, std::uint64_t& out) {
    if (s.empty() || s.size() > 12) { return false; }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace radioinfo_detail

// Parses one RadioInfo datagram. False with a sentence in `error` for anything that is not a well-formed RadioInfo document with a RadioNr in 1..4 and
// a plausible frequency. Unknown elements are ignored. Never reads outside [data, data+len).
inline bool parseRadioInfoInner(const char* data, std::size_t len, RadioInfo& out, std::string& error) {
    using namespace radioinfo_detail;
    out = RadioInfo();
    if (data == nullptr || len == 0) { error = "empty datagram"; return false; }
    if (len > kRadioInfoMaxDatagram) { error = "datagram longer than 4096 bytes"; return false; }
    const std::string s(data, len);
    if (s.find('\0') != std::string::npos) { error = "NUL byte in the document"; return false; }
    std::size_t i = 0;
    auto skipWs = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) { ++i; } };
    skipWs();
    if (s.compare(i, 5, "<?xml") == 0) {
        const std::size_t e = s.find("?>", i);
        if (e == std::string::npos) { error = "the XML declaration is not closed"; return false; }
        i = e + 2;
        skipWs();
    }
    // the document: <RadioInfo> ... </RadioInfo>, children are simple <name>text</name> elements
    if (s.compare(i, 11, "<RadioInfo>") != 0) { error = "the root element is not RadioInfo"; return false; }
    i += 11;
    std::map<std::string, std::string> fields;
    bool closed = false;
    int elements = 0;
    while (i < s.size()) {
        skipWs();
        if (i >= s.size()) { break; }
        if (s[i] != '<') { error = "text outside an element"; return false; }
        if (s.compare(i, 2, "<!") == 0) { error = "markup declarations are not accepted"; return false; }
        if (s.compare(i, 2, "<?") == 0) { error = "processing instructions are not accepted"; return false; }
        if (s.compare(i, 12, "</RadioInfo>") == 0) { closed = true; i += 12; break; }
        if (++elements > 64) { error = "too many elements"; return false; }
        const std::size_t gt = s.find('>', i);
        if (gt == std::string::npos) { error = "a tag is not closed"; return false; }
        std::string name = s.substr(i + 1, gt - i - 1);
        if (!name.empty() && name.back() == '/') {   // <Element/> : empty
            name.pop_back();
            if (name.empty() || name.size() > 32) { error = "bad element name"; return false; }
            i = gt + 1;
            continue;
        }
        if (name.empty() || name.size() > 32 || name[0] == '/') { error = "bad element name"; return false; }
        for (char c : name) { if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) { error = "bad element name"; return false; } }
        const std::string closeTag = "</" + name + ">";
        const std::size_t end = s.find(closeTag, gt + 1);
        if (end == std::string::npos) { error = "an element is not closed"; return false; }
        const std::string raw = s.substr(gt + 1, end - gt - 1);
        if (raw.find('<') != std::string::npos) { error = "nested elements are not accepted"; return false; }
        if (raw.size() > 64) { error = "element text too long"; return false; }
        std::string text;
        if (!decodeText(raw, text)) { error = "an unsupported character reference"; return false; }
        fields[name] = text;
        i = end + closeTag.size();
    }
    if (!closed) { error = "the document is not closed"; return false; }
    {
        std::size_t k = i;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' || s[k] == '\n')) { ++k; }
        if (k != s.size()) { error = "data after the document"; return false; }
    }
    std::uint64_t u = 0;
    auto it = fields.find("RadioNr");
    if (it == fields.end() || !parseUint(it->second, u) || u < 1 || u > 4) { error = "RadioNr is missing or not 1..4"; return false; }
    out.radioNr = static_cast<int>(u);
    // Freq and TXFreq are in 10 Hz units; up to 6 GHz would be 600000000 units
    auto freq = [&](const char* key, double& hz) -> bool {
        auto f = fields.find(key);
        if (f == fields.end() || f->second.empty()) { return true; }
        std::uint64_t v = 0;
        if (!parseUint(f->second, v) || v > 600000000ull) { return false; }
        hz = static_cast<double>(v) * 10.0;
        return true;
    };
    if (!freq("Freq", out.freqHz) || !freq("TXFreq", out.txFreqHz)) { error = "a frequency is not a number of 10 Hz units within range"; return false; }
    if (auto m = fields.find("Mode"); m != fields.end()) {
        if (m->second.size() > 12) { error = "mode text too long"; return false; }
        for (char c : m->second) { if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '+')) { error = "mode text has odd characters"; return false; } }
        out.mode = m->second;
    }
    if (auto a = fields.find("app"); a != fields.end()) {
        if (a->second.size() > 16) { error = "app text too long"; return false; }
        for (char c : a->second) { if (!std::isalnum(static_cast<unsigned char>(c))) { error = "app text has odd characters"; return false; } }
        out.app = a->second;
    }
    bool b = false;
    for (const auto& kv : {std::make_pair("IsSplit", &out.split), std::make_pair("IsTransmitting", &out.transmitting), std::make_pair("IsRunning", &out.running)}) {
        auto f = fields.find(kv.first);
        if (f == fields.end() || f->second.empty()) { continue; }
        if (!parseBool(f->second, b)) { error = "a flag is not True or False"; return false; }
        *kv.second = b;
    }
    if (auto a = fields.find("ActiveRadioNr"); a != fields.end() && !a->second.empty()) {
        if (!parseUint(a->second, u) || u > 4) { error = "ActiveRadioNr is not 0..4"; return false; }
        out.activeRadioNr = static_cast<int>(u);
    }
    error.clear();
    return true;
}

// The public entry: on ANY failure `out` is left as a default RadioInfo (nothing half-parsed leaks out).
inline bool parseRadioInfo(const char* data, std::size_t len, RadioInfo& out, std::string& error) {
    if (parseRadioInfoInner(data, len, out, error)) { return true; }
    out = RadioInfo();
    return false;
}

// Per-radio table of the latest RadioInfo, with staleness (N1MM and Commander repeat every 10 s).
class RadioInfoTable {
public:
    explicit RadioInfoTable(std::int64_t staleAfterSec = 30) : stale_(staleAfterSec < 1 ? 1 : staleAfterSec) {}

    void update(const RadioInfo& r, std::int64_t nowUnix) {
        if (r.radioNr < 1 || r.radioNr > 4) { return; }
        info_[r.radioNr - 1] = r;
        seen_[r.radioNr - 1] = nowUnix;
        have_[r.radioNr - 1] = true;
    }
    // false when the radio has never been heard or was last heard more than staleAfterSec ago
    bool get(int radioNr, std::int64_t nowUnix, RadioInfo& out) const {
        if (radioNr < 1 || radioNr > 4 || !have_[radioNr - 1] || nowUnix - seen_[radioNr - 1] > stale_) { return false; }
        out = info_[radioNr - 1];
        return true;
    }
    // The transmit frequency to believe for a radio: TXFreq when split is on, or when the packet's radio is the active one (the SO2V caution);
    // otherwise the receive frequency. 0 when unknown.
    static double effectiveTxHz(const RadioInfo& r) {
        if (r.txFreqHz > 0 && (r.split || r.activeRadioNr == 0 || r.activeRadioNr == r.radioNr)) { return r.txFreqHz; }
        return r.freqHz;
    }

private:
    std::int64_t stale_;
    RadioInfo info_[4];
    std::int64_t seen_[4] = {0, 0, 0, 0};
    bool have_[4] = {false, false, false, false};
};

}  // namespace cascade::core::rig
