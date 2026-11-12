// aprs_path.hpp - parse the header of an APRS packet ("SRC>DEST,HOP,HOP*,qAR,IGATE") into hops a map can draw.
// Row M1b of the gaps survey. STANDALONE and PURE: no decoder is touched; the wiring lane hands the text before the ':' of a
// packet (or a plugin's path field) to parseHeader() / parsePath().
//
// WHAT IS KNOWN, AND FROM WHERE
//  * APRS-IS q constructs (qAC, qAX, qAU, qAo, qAO, qAS, qAr, qAR, qAZ, qAI), their position after the last RF digipeater and the
//    IGate or server callsign that follows them: http://www.aprs-is.net/q.aspx (read 2026-11-11). Verified.
//  * The "*" after a path element means the packet has been repeated by that element and by every element before it; the unused
//    tail still has the packet to travel. WIDEn-N / TRACEn-N: N hops remain of n requested. Written from memory of the APRS
//    protocol reference (APRS101, chapter 4); NOT re-read this session: UNVERIFIED.
//  * Callsigns: 1 to 6 letters or digits, optional "-" and a number 0 to 15 (the AX.25 address field). The APRS-IS part after the
//    q construct may carry server names with dots, underscores and longer text, which are kept as plain text.
//
// BOUNDS. The header is capped at 512 bytes and 16 path elements (an AX.25 path holds at most 8 digipeaters; more is refused,
// not truncated). Everything is refused, never guessed: a malformed header returns false and an empty route.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core::aprs {

enum class HopKind {
    Digipeater,   // a station call sign (a node a map can place)
    Alias,        // WIDEn-N, TRACEn-N, RELAY, TRACE, WIDE: a request, not a station
    Marker,       // TCPIP, TCPXX, NOGATE, RFONLY: Internet / policy markers
    QConstruct,   // qAC, qAR ...
    IGate,        // the callsign or server name that follows a q construct
    Other,        // anything after the q construct that is not the first element (server chain)
};

struct Hop {
    std::string name;      // upper-case as sent, without the "*"
    int ssid = -1;         // -1 when none
    HopKind kind = HopKind::Other;
    bool used = false;     // repeated this packet (the "*" or any element before it)
    int requested = 0;     // alias: the n of WIDEn-N
    int remaining = 0;     // alias: the N
};

struct Route {
    std::string source;
    int sourceSsid = -1;
    std::string destination;
    std::vector<Hop> hops;
    std::string qConstruct;   // "" when none
    std::string igate;        // "" when none
    // The stations a map can draw as a path, in order: source, then every USED Digipeater hop, then the IGate.
    std::vector<std::string> stationPath() const {
        std::vector<std::string> p;
        p.push_back(source + (sourceSsid >= 0 ? "-" + std::to_string(sourceSsid) : std::string()));
        for (const Hop& h : hops) {
            if (h.kind == HopKind::Digipeater && h.used)
                p.push_back(h.name + (h.ssid >= 0 ? "-" + std::to_string(h.ssid) : std::string()));
        }
        if (!igate.empty()) p.push_back(igate);
        return p;
    }
};

inline constexpr std::size_t kMaxHeaderBytes = 512;
inline constexpr std::size_t kMaxElements = 16;

namespace detail {

inline bool isAlnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

// "CALL" or "CALL-n": true when it is an AX.25-style call (after upper-casing).
inline bool splitCall(std::string_view s, std::string& call, int& ssid) {
    std::size_t dash = s.find('-');
    std::string_view c = s.substr(0, dash);
    if (c.empty() || c.size() > 6) return false;
    for (char ch : c)
        if (!isAlnum(ch)) return false;
    ssid = -1;
    if (dash != std::string_view::npos) {
        std::string_view n = s.substr(dash + 1);
        if (n.empty() || n.size() > 2) return false;
        int v = 0;
        for (char ch : n) {
            if (ch < '0' || ch > '9') return false;
            v = v * 10 + (ch - '0');
        }
        if (v > 15) return false;
        ssid = v;
    }
    call.assign(c);
    return true;
}

inline std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return o;
}

inline bool isQ(std::string_view s) {
    return s.size() == 3 && s[0] == 'q' && s[1] == 'A' &&
           (s[2] == 'C' || s[2] == 'X' || s[2] == 'U' || s[2] == 'o' || s[2] == 'O' || s[2] == 'S' || s[2] == 'r' ||
            s[2] == 'R' || s[2] == 'Z' || s[2] == 'I');
}

// WIDEn-N / TRACEn-N with n 1..7 and N 0..n.
inline bool parseAlias(std::string_view s, int& n, int& rem) {
    std::string_view rest;
    if (s.substr(0, 4) == "WIDE") rest = s.substr(4);
    else if (s.substr(0, 5) == "TRACE") rest = s.substr(5);
    else return false;
    if (rest.size() != 3 || rest[1] != '-') return false;
    if (rest[0] < '1' || rest[0] > '7' || rest[2] < '0' || rest[2] > '7') return false;
    n = rest[0] - '0';
    rem = rest[2] - '0';
    return rem <= n;
}

}  // namespace detail

// path: the comma-separated elements after the destination, e.g. "WIDE1-1,N0DIG*,WIDE2-1,qAR,W1IGT". Appends to route.
inline bool parsePath(std::string_view path, Route& route) {
    route.hops.clear();
    route.qConstruct.clear();
    route.igate.clear();
    if (path.empty()) return true;
    std::vector<std::string_view> elems;
    std::size_t pos = 0;
    while (true) {
        std::size_t c = path.find(',', pos);
        elems.push_back(path.substr(pos, c == std::string_view::npos ? std::string_view::npos : c - pos));
        if (c == std::string_view::npos) break;
        pos = c + 1;
        if (elems.size() > kMaxElements) return false;
    }
    if (elems.size() > kMaxElements) return false;
    bool afterQ = false;
    int afterQIndex = 0;
    int lastUsed = -1;   // index of the element carrying "*"
    for (std::size_t i = 0; i < elems.size(); ++i) {
        std::string_view e = elems[i];
        if (e.empty()) return false;
        bool star = false;
        if (e.back() == '*') {
            star = true;
            e.remove_suffix(1);
            if (e.empty()) return false;
        }
        Hop h;
        if (!afterQ && detail::isQ(e)) {
            if (star) return false;
            h.name = std::string(e);
            h.kind = HopKind::QConstruct;
            route.qConstruct = h.name;
            afterQ = true;
            afterQIndex = 0;
            route.hops.push_back(std::move(h));
            continue;
        }
        if (afterQ) {
            for (char ch : e) {
                if (static_cast<unsigned char>(ch) < 0x21 || static_cast<unsigned char>(ch) > 0x7e || ch == ',' || ch == ':' ||
                    ch == '>' || ch == '*')
                    return false;
            }
            h.name = std::string(e);
            h.used = true;
            if (afterQIndex == 0) {
                h.kind = HopKind::IGate;
                route.igate = h.name;
            } else {
                h.kind = HopKind::Other;
            }
            ++afterQIndex;
            route.hops.push_back(std::move(h));
            continue;
        }
        const std::string u = detail::upper(e);
        h.name = u;
        int n = 0, rem = 0;
        if (u == "TCPIP" || u == "TCPXX" || u == "NOGATE" || u == "RFONLY") {
            h.kind = HopKind::Marker;
        } else if (detail::parseAlias(u, n, rem)) {
            h.kind = HopKind::Alias;
            h.requested = n;
            h.remaining = rem;
        } else if (u == "RELAY" || u == "TRACE" || u == "WIDE") {
            h.kind = HopKind::Alias;
        } else {
            std::string call;
            int ssid = -1;
            if (!detail::splitCall(u, call, ssid)) return false;
            h.name = call;
            h.ssid = ssid;
            h.kind = HopKind::Digipeater;
        }
        if (star) lastUsed = static_cast<int>(i);
        route.hops.push_back(std::move(h));
    }
    for (int i = 0; i <= lastUsed; ++i) route.hops[static_cast<std::size_t>(i)].used = true;
    return true;
}

// header: "SRC>DEST,PATH..." (the text before the first ':' of the packet). Anything from a ':' on is ignored.
inline bool parseHeader(std::string_view header, Route& route) {
    route = Route{};
    const std::size_t colon = header.find(':');
    if (colon != std::string_view::npos) header = header.substr(0, colon);
    if (header.empty() || header.size() > kMaxHeaderBytes) return false;
    const std::size_t gt = header.find('>');
    if (gt == std::string_view::npos || gt == 0) return false;
    std::string call;
    int ssid = -1;
    if (!detail::splitCall(detail::upper(header.substr(0, gt)), call, ssid)) return false;
    std::string_view rest = header.substr(gt + 1);
    const std::size_t comma = rest.find(',');
    std::string_view dest = rest.substr(0, comma);
    if (dest.empty()) return false;
    std::string dcall;
    int dssid = -1;
    if (!detail::splitCall(detail::upper(dest), dcall, dssid)) return false;
    route.source = call;
    route.sourceSsid = ssid;
    route.destination = dest.size() ? detail::upper(dest) : std::string();
    if (comma == std::string_view::npos) return true;
    if (!parsePath(rest.substr(comma + 1), route)) {
        route = Route{};
        return false;
    }
    return true;
}

}  // namespace cascade::core::aprs
