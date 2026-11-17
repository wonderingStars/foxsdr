// web_guard.cpp - see web_guard.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_guard.hpp"

#include <algorithm>
#include <cctype>

#include "net/web_cors.hpp"

namespace cascade::net {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool isIpv4Literal(const std::string& h) {
    int groups = 0;
    std::size_t i = 0;
    while (i <= h.size()) {
        const std::size_t dot = h.find('.', i);
        const std::string part = h.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (!allDigits(part) || part.size() > 3 || std::stoi(part) > 255) { return false; }
        ++groups;
        if (dot == std::string::npos) { break; }
        i = dot + 1;
    }
    return groups == 4;
}

bool isIpv6Literal(const std::string& h) {
    if (h.size() < 4 || h.front() != '[' || h.back() != ']') { return false; }
    bool colon = false;
    for (std::size_t i = 1; i + 1 < h.size(); ++i) {
        const char c = h[i];
        if (c == ':') { colon = true; continue; }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex && c != '.') { return false; }
    }
    return colon;
}

// The port text with the scheme's default dropped, so "example.com:80" and "example.com" over http compare equal.
std::string portFor(const std::string& scheme, const std::string& port) {
    if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443")) { return std::string(); }
    return port;
}

bool hostAllowed(const HostOriginPolicy& p, const std::string& host) {
    if (isIpLiteralHost(host) || host == "localhost") { return true; }
    return std::find(p.hostNames.begin(), p.hostNames.end(), host) != p.hostNames.end() ||
           std::find(p.machineNames.begin(), p.machineNames.end(), host) != p.machineNames.end();
}

}  // namespace

bool splitHostHeader(const std::string& value, std::string& host, std::string& port) {
    host.clear();
    port.clear();
    if (value.empty() || value.size() > 300) { return false; }
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) { return false; }
        if (ch == '/' || ch == '\\' || ch == '@' || ch == '?' || ch == '#' || ch == ',') { return false; }
    }
    std::string h = value;
    if (h[0] == '[') {
        const std::size_t close = h.find(']');
        if (close == std::string::npos) { return false; }
        if (close + 1 < h.size()) {
            if (h[close + 1] != ':') { return false; }
            port = h.substr(close + 2);
            if (!allDigits(port) || port.size() > 5) { return false; }
        }
        host = lowerAscii(h.substr(0, close + 1));
        return true;
    }
    const std::size_t colon = h.rfind(':');
    if (colon != std::string::npos) {
        port = h.substr(colon + 1);
        if (!allDigits(port) || port.size() > 5) { return false; }
        h = h.substr(0, colon);
    }
    while (!h.empty() && h.back() == '.') { h.pop_back(); }   // "example.com." is the same name
    if (h.empty()) { return false; }
    host = lowerAscii(h);
    return true;
}

bool isIpLiteralHost(const std::string& host) { return isIpv4Literal(host) || isIpv6Literal(host); }

void guardSetOrigins(HostOriginPolicy& policy, const std::vector<std::string>& listed) {
    policy.origins.clear();
    policy.anyOrigin = false;
    policy.hostNames.clear();   // the computer's own names (machineNames) are the server's and stay
    for (const std::string& o : listed) {
        if (o == "*") { policy.anyOrigin = true; continue; }
        std::string canonical;
        if (!parseCorsOrigin(o, false, canonical)) { continue; }
        policy.origins.push_back(canonical);
        std::string host, port;
        const std::size_t sep = canonical.find("://");
        if (splitHostHeader(canonical.substr(sep + 3), host, port) && !isIpLiteralHost(host)) { policy.hostNames.push_back(host); }
    }
}

std::vector<std::string> guardMachineNames(const std::string& computerName) {
    std::vector<std::string> out;
    if (computerName.empty()) { return out; }
    std::string n = lowerAscii(computerName);
    out.push_back(n);
    out.push_back(n + ".local");
    return out;
}

std::vector<std::string> guardHostNames(const std::string& computerName, const std::vector<std::string>& listedOrigins) {
    std::vector<std::string> out = guardMachineNames(computerName);
    HostOriginPolicy p;
    guardSetOrigins(p, listedOrigins);
    for (const std::string& h : p.hostNames) { out.push_back(h); }
    return out;
}

GuardVerdict checkHostOrigin(const HostOriginPolicy& policy, const GuardRequest& req) {
    // HOST.
    std::string host, hostPort;
    if (req.hostValues.size() > 1) { return GuardVerdict::ForeignHost; }
    const bool hasHost = !req.hostValues.empty();
    if (hasHost) {
        if (!splitHostHeader(req.hostValues[0], host, hostPort)) { return GuardVerdict::ForeignHost; }
        if (!hostAllowed(policy, host)) { return GuardVerdict::ForeignHost; }
    }
    // ORIGIN.
    if (req.originValues.empty()) { return GuardVerdict::Allowed; }
    if (req.originValues.size() > 1) { return GuardVerdict::ForeignOrigin; }
    std::string canonical;
    if (!parseCorsOrigin(req.originValues[0], true, canonical)) { return GuardVerdict::ForeignOrigin; }   // "null", a trailing slash, upper case ...
    const std::size_t sep = canonical.find("://");
    const std::string scheme = canonical.substr(0, sep);
    std::string oHost, oPort;
    if (!splitHostHeader(canonical.substr(sep + 3), oHost, oPort)) { return GuardVerdict::ForeignOrigin; }
    if (hasHost && oHost == host && portFor(scheme, oPort) == portFor(scheme, hostPort)) { return GuardVerdict::Allowed; }   // our own origin
    const bool readOnlyMethod = req.method == "GET" || req.method == "HEAD" || req.method == "OPTIONS";
    if (readOnlyMethod && (policy.anyOrigin || std::find(policy.origins.begin(), policy.origins.end(), canonical) != policy.origins.end())) {
        return GuardVerdict::Allowed;
    }
    return GuardVerdict::ForeignOrigin;
}

}  // namespace cascade::net
