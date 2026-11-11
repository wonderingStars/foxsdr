// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/websdr_link.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace cascade::core::websdr {

namespace {

bool badChar(unsigned char c) {
    return c <= 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '\'' || c == '`';
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string formatKHz(double f) {
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", f);
    std::string s = b;
    while (!s.empty() && s.back() == '0') { s.pop_back(); }
    if (!s.empty() && s.back() == '.') { s.pop_back(); }
    return s;
}

}  // namespace

Link buildLink(const std::string& baseUrl, double frequencyKHz, const std::string& mode) {
    Link out;
    if (!std::isfinite(frequencyKHz) || frequencyKHz <= 0.0 || frequencyKHz > 3.0e6) {
        out.error = "frequency must be above 0 and at most 3,000,000 kHz";
        return out;
    }
    for (char c : baseUrl) {
        if (badChar(static_cast<unsigned char>(c))) {
            out.error = "the address contains a space, control or quote character";
            return out;
        }
    }
    std::string rest;
    std::string scheme;
    if (baseUrl.size() >= 8 && lower(baseUrl.substr(0, 8)) == "https://") { scheme = "https://"; rest = baseUrl.substr(8); }
    else if (baseUrl.size() >= 7 && lower(baseUrl.substr(0, 7)) == "http://") { scheme = "http://"; rest = baseUrl.substr(7); }
    else {
        out.error = "only http and https addresses are accepted";
        return out;
    }
    // Drop fragment and query.
    const std::size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) { rest.erase(cut); }
    const std::size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        out.error = "the address has no host";
        return out;
    }
    if (authority.find('@') != std::string::npos) {
        out.error = "an address with a user name or password is not accepted";
        return out;
    }
    if (authority.front() == ':' || authority.front() == '.') {
        out.error = "the address has no host";
        return out;
    }
    // Port, if any, must be 1..65535 digits.
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) {
            out.error = "the port is not a number";
            return out;
        }
        unsigned v = 0;
        for (char c : port) {
            if (c < '0' || c > '9') { out.error = "the port is not a number"; return out; }
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v < 1 || v > 65535) { out.error = "the port is out of range"; return out; }
    }
    const std::string m = lower(mode);
    for (char c : m) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            out.error = "the mode must be letters and digits";
            return out;
        }
    }
    out.modeSuffixDocumented = (m == "am");
    out.url = scheme + authority + path + "?tune=" + formatKHz(frequencyKHz) + (out.modeSuffixDocumented ? "am" : "");
    out.ok = true;
    return out;
}

}  // namespace cascade::core::websdr
