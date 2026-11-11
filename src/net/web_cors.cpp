// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_cors.hpp"

#include <algorithm>
#include <cctype>

namespace cascade::net {
namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool validDnsName(const std::string& h) {
    if (h.empty() || h.size() > 253) { return false; }
    std::size_t start = 0;
    while (start <= h.size()) {
        std::size_t dot = h.find('.', start);
        if (dot == std::string::npos) { dot = h.size(); }
        const std::size_t len = dot - start;
        if (len == 0 || len > 63) { return false; }
        if (h[start] == '-' || h[dot - 1] == '-') { return false; }
        for (std::size_t i = start; i < dot; ++i) {
            const char c = h[i];
            if (!((c >= 'a' && c <= 'z') || isDigit(c) || c == '-')) { return false; }
        }
        if (dot == h.size()) { break; }
        start = dot + 1;
    }
    return true;
}

bool dottedQuad(const std::string& h) {
    int parts = 0;
    std::size_t i = 0;
    while (i <= h.size()) {
        std::size_t dot = h.find('.', i);
        if (dot == std::string::npos) { dot = h.size(); }
        const std::string seg = h.substr(i, dot - i);
        if (seg.empty() || seg.size() > 3) { return false; }
        for (const char c : seg) { if (!isDigit(c)) { return false; } }
        if (seg.size() > 1 && seg[0] == '0') { return false; }
        if (std::stoi(seg) > 255) { return false; }
        ++parts;
        if (dot == h.size()) { break; }
        i = dot + 1;
    }
    return parts == 4;
}

bool allDigitsDots(const std::string& h) {
    return std::all_of(h.begin(), h.end(), [](char c) { return isDigit(c) || c == '.'; });
}

bool validIpv6Literal(const std::string& h) {  // bracketed, lowercase hex and colons, at least two colons
    if (h.size() < 4 || h.front() != '[' || h.back() != ']') { return false; }
    const std::string in = h.substr(1, h.size() - 2);
    if (in.size() > 45) { return false; }
    int colons = 0;
    for (const char c : in) {
        if (c == ':') { ++colons; }
        else if (!(isDigit(c) || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return colons >= 2;
}

bool validHost(const std::string& h) {
    if (!h.empty() && h[0] == '[') { return validIpv6Literal(h); }
    if (allDigitsDots(h)) { return dottedQuad(h); }  // a numeric-looking host must be a real IPv4
    return validDnsName(h);
}

bool tokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
}

bool validToken(const std::string& t) {
    return !t.empty() && t.size() <= 64 && std::all_of(t.begin(), t.end(), tokenChar);
}

bool containsNoCase(const std::vector<std::string>& v, const std::string& s) {
    const std::string l = lower(s);
    return std::any_of(v.begin(), v.end(), [&](const std::string& e) { return lower(e) == l; });
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) { out += ", "; }
        out += s;
    }
    return out;
}

}  // namespace

bool parseCorsOrigin(const std::string& text, bool strict, std::string& canonical) {
    canonical.clear();
    if (text.empty() || text.size() > kMaxCorsOriginLength) { return false; }
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) { return false; }  // controls, space, non-ASCII
        if (ch == '\\' || ch == '@' || ch == '?' || ch == '#' || ch == ',' || ch == ';' || ch == '"' || ch == '\'' || ch == '*') { return false; }
    }
    const std::size_t sep = text.find("://");
    if (sep == std::string::npos) { return false; }
    std::string scheme = text.substr(0, sep);
    std::string rest = text.substr(sep + 3);
    if (rest.find('/') != std::string::npos) { return false; }  // no path, not even a trailing slash
    if (strict && scheme != lower(scheme)) { return false; }
    scheme = lower(scheme);
    if (scheme != "http" && scheme != "https") { return false; }
    std::string host = rest;
    std::string port;
    // Split the port: the last ':' that is not inside an IPv6 bracket.
    const std::size_t close = rest.find(']');
    const std::size_t colon = rest.rfind(':');
    if (!rest.empty() && rest[0] == '[') {
        if (close == std::string::npos) { return false; }
        host = rest.substr(0, close + 1);
        if (close + 1 < rest.size()) {
            if (rest[close + 1] != ':') { return false; }
            port = rest.substr(close + 2);
            if (port.empty()) { return false; }
        }
    } else if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (port.empty()) { return false; }
    }
    if (strict && host != lower(host)) { return false; }
    host = lower(host);
    if (!validHost(host)) { return false; }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), isDigit) || port[0] == '0') { return false; }
        const int p = std::stoi(port);
        if (p < 1 || p > 65535) { return false; }
        const bool isDefault = (scheme == "http" && p == 80) || (scheme == "https" && p == 443);
        if (isDefault) {
            if (strict) { return false; }  // a browser never writes the default port
            port.clear();
        }
    }
    canonical = scheme + "://" + host + (port.empty() ? "" : ":" + port);
    return true;
}

bool CorsPolicy::configure(const CorsConfig& cfg, std::string& error) {
    error.clear();
    if (cfg.origins.size() > kMaxCorsOrigins) { error = "too many allowed origins"; return false; }
    bool wildcard = false;
    std::vector<std::string> origins;
    for (const std::string& o : cfg.origins) {
        if (o == "*") {
            wildcard = true;
            continue;
        }
        std::string canon;
        if (!parseCorsOrigin(o, /*strict=*/false, canon)) {
            error = "an allowed origin must be exactly scheme://host or scheme://host:port with http or https (no path, no wildcard, not null)";
            return false;
        }
        if (std::find(origins.begin(), origins.end(), canon) == origins.end()) { origins.push_back(std::move(canon)); }
    }
    if (wildcard && cfg.allowCredentials) {
        error = "a wildcard origin cannot be combined with credentials";
        return false;
    }
    if (wildcard && !origins.empty()) {
        error = "a wildcard origin cannot be mixed with named origins";
        return false;
    }
    if (cfg.maxAgeSeconds < 0 || cfg.maxAgeSeconds > 86400) { error = "the preflight cache time must be 0 to 86400 seconds"; return false; }
    if (cfg.methods.size() > 16 || cfg.headers.size() > 32) { error = "too many methods or headers"; return false; }
    std::vector<std::string> methods;
    for (const std::string& m : cfg.methods) {
        if (!validToken(m) || m == "*") { error = "a method is not a valid token (or is a wildcard)"; return false; }
        std::string u = m;
        for (char& c : u) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        methods.push_back(std::move(u));
    }
    std::vector<std::string> headers;
    for (const std::string& h : cfg.headers) {
        if (!validToken(h) || h == "*") { error = "a header name is not a valid token (or is a wildcard)"; return false; }
        headers.push_back(h);
    }
    wildcard_ = wildcard;
    credentials_ = cfg.allowCredentials;
    origins_ = std::move(origins);
    methods_ = std::move(methods);
    headers_ = std::move(headers);
    maxAge_ = cfg.maxAgeSeconds;
    configured_ = true;
    return true;
}

CorsDecision CorsPolicy::evaluate(const CorsRequest& req) const {
    CorsDecision d;
    const bool varies = !wildcard_;  // the reply depends on the Origin unless it is `*`
    auto vary = [&] { if (varies) { d.headers.emplace_back("Vary", "Origin"); } };

    if (req.originValues.empty()) {
        d.verdict = CorsVerdict::NotCors;
        if (configured_) { vary(); }
        return d;
    }
    d.verdict = CorsVerdict::Denied;
    if (!configured_) { return d; }
    vary();
    if (req.originValues.size() != 1) { return d; }  // rule 3: one value only
    const std::string& raw = req.originValues[0];
    if (raw.size() > kMaxCorsHeaderValue) { return d; }
    std::string canon;
    if (!parseCorsOrigin(raw, /*strict=*/true, canon)) { return d; }  // includes "null"
    std::string allowOrigin;
    if (wildcard_) {
        allowOrigin = "*";
    } else {
        const auto it = std::find(origins_.begin(), origins_.end(), canon);
        if (it == origins_.end()) { return d; }
        allowOrigin = *it;  // the configured string, never the request's bytes
    }

    const bool preflight = req.method == "OPTIONS" && req.hasRequestMethod;
    std::vector<std::pair<std::string, std::string>> out;
    if (varies) { out.emplace_back("Vary", "Origin"); }
    out.emplace_back("Access-Control-Allow-Origin", allowOrigin);
    if (credentials_) { out.emplace_back("Access-Control-Allow-Credentials", "true"); }

    if (preflight) {
        if (req.requestMethod.size() > kMaxCorsHeaderValue || !validToken(req.requestMethod)) { return d; }
        // Methods are case-sensitive in Fetch (the configured list is upper case), so "get" is not "GET".
        if (std::find(methods_.begin(), methods_.end(), req.requestMethod) == methods_.end()) { return d; }
        std::vector<std::string> allowedNow;
        if (req.hasRequestHeaders) {
            if (req.requestHeaders.size() > kMaxCorsHeaderValue) { return d; }
            std::size_t i = 0;
            const std::string& v = req.requestHeaders;
            while (i <= v.size()) {
                std::size_t comma = v.find(',', i);
                if (comma == std::string::npos) { comma = v.size(); }
                std::string name = v.substr(i, comma - i);
                const std::size_t a = name.find_first_not_of(" \t");
                const std::size_t b = name.find_last_not_of(" \t");
                name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
                if (!name.empty()) {
                    if (!validToken(name) || !containsNoCase(headers_, name)) { return d; }
                    for (const std::string& h : headers_) {
                        if (lower(h) == lower(name) && std::find(allowedNow.begin(), allowedNow.end(), h) == allowedNow.end()) { allowedNow.push_back(h); }
                    }
                }
                if (comma == v.size()) { break; }
                i = comma + 1;
            }
        }
        out.emplace_back("Access-Control-Allow-Methods", join(methods_));
        if (!allowedNow.empty()) { out.emplace_back("Access-Control-Allow-Headers", join(allowedNow)); }
        out.emplace_back("Access-Control-Max-Age", std::to_string(maxAge_));
        d.verdict = CorsVerdict::Preflight;
    } else {
        d.verdict = CorsVerdict::Allowed;
    }
    d.headers = std::move(out);
    return d;
}

}  // namespace cascade::net
