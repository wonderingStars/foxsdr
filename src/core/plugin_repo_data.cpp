// plugin_repo_data.cpp - the definitions of plugin_repo_data.hpp's free functions (shell retirement R11a, 2026-10-29).
//
// The bodies of PluginRepo's pure statics, moved here verbatim from plugin_repo.cpp (the names changed, nothing else): string arithmetic over a site
// address and the platform words. PluginRepo::siteLabel and the rest forward to them (plugin_repo.hpp). The small helpers below are the same few lines as
// plugin_repo.cpp's own (it keeps its copies for the rest of the file); they are ASCII-only on purpose, as a catalogue address is untrusted input.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo_data.hpp"

#include <string>

namespace cascade::core {

namespace {

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool startsWithAscii(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() < n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        if (lowerAscii(s[i]) != prefix[i]) { return false; }
    }
    return true;
}

bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string toLowerAscii(std::string s) {
    for (char& c : s) { c = lowerAscii(c); }
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) { return false; }
    for (char c : s) {
        if (c < '0' || c > '9') { return false; }
    }
    return true;
}

}  // namespace

std::string defaultPluginIndexUrl() {
    return "https://raw.githubusercontent.com/wonderingStars/foxsdr-plugins/master/"
           "index.json";
}

bool pluginIsHttpsUrl(const std::string& url) {
    return startsWithAscii(url, "https://") && url.size() > 8;
}

std::string pluginSiteHost(const std::string& url) {
    if (!pluginIsHttpsUrl(url)) { return {}; }
    const std::size_t start = 8;  // past "https://"
    std::size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) { end = url.size(); }
    const std::string authority = url.substr(start, end - start);
    if (authority.empty()) { return {}; }
    // Userinfo ("good.example@evil.example"), a backslash (a browser reads it as a
    // slash), a space, a control character or a byte >= 0x80 anywhere in the
    // authority: not a host this code will lock anything to. Refused outright
    // rather than parsed, because a parser that guesses which half is the host is
    // exactly how a host lock gets walked around.
    for (char c : authority) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '@' || c == '\\' || u <= 0x20 || u >= 0x7F) { return {}; }
    }
    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) { return {}; }
        host = authority.substr(0, close + 1);
        for (std::size_t i = 1; i < close; ++i) {
            if (!isHexDigit(authority[i]) && authority[i] != ':' && authority[i] != '.') { return {}; }
        }
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') { return {}; }
            port = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.find(':');
        host = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (colon != std::string::npos) { port = authority.substr(colon + 1); }
        if (host.empty()) { return {}; }
        for (char c : host) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            if (!ok) { return {}; }
        }
    }
    if (authority.find(':') != std::string::npos && authority.front() != '[' && port.empty()) {
        return {};  // "host:" - an empty port is a malformed authority
    }
    if (!port.empty()) {
        if (port.size() > 5 || !allDigits(port)) { return {}; }
        if (port == "443") { port.clear(); }  // the default: "host" and "host:443" are one host
    }
    host = toLowerAscii(host);
    return port.empty() ? host : host + ":" + port;
}

std::string pluginSiteLabel(const std::string& siteUrl) {
    const std::string host = pluginSiteHost(siteUrl);
    if (!host.empty()) { return host; }
    if (siteUrl.find("://") != std::string::npos) { return siteUrl; }  // not a usable address: shown as it is
    // A local file: its name, which is what tells two files apart in a list.
    const std::size_t cut = siteUrl.find_last_of("/\\");
    if (cut == std::string::npos || cut + 1 >= siteUrl.size()) { return siteUrl; }
    return siteUrl.substr(cut + 1);
}

std::string pluginSiteDisplay(const std::string& siteUrl, std::size_t maxChars) {
    std::string s = siteUrl;
    if (pluginSiteHost(siteUrl).empty() == false) {
        s = siteUrl.substr(8);  // past "https://"
    }
    if (s.size() <= maxChars) { return s; }
    if (maxChars < 8) { return s.substr(0, maxChars); }
    const std::size_t keep = maxChars - 3;
    std::size_t left = keep / 2 + keep % 2;
    std::size_t rightStart = s.size() - keep / 2;
    // Never cut inside a multi-byte character: a continuation byte is 10xxxxxx.
    while (left > 0 && (static_cast<unsigned char>(s[left]) & 0xC0) == 0x80) { --left; }
    while (rightStart < s.size() && (static_cast<unsigned char>(s[rightStart]) & 0xC0) == 0x80) {
        ++rightStart;
    }
    return s.substr(0, left) + "..." + s.substr(rightStart);
}

bool normalisePluginSiteUrl(const std::string& raw, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    std::size_t b = 0;
    std::size_t e = raw.size();
    while (b < e && (raw[b] == ' ' || raw[b] == '\t')) { ++b; }
    while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t' || raw[e - 1] == '\r' ||
                     raw[e - 1] == '\n')) {
        --e;
    }
    const std::string s = raw.substr(b, e - b);
    if (s.empty()) {
        error = "the site address is empty";
        return false;
    }
    if (s.size() > 2048) {
        error = "the site address is longer than 2048 characters";
        return false;
    }
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            error = "the site address contains a control character";
            return false;
        }
    }
    if (s.find("://") != std::string::npos) {
        if (!pluginIsHttpsUrl(s)) {
            error = "a site address must start with https:// (or be the path of a local index.json)";
            return false;
        }
        if (pluginSiteHost(s).empty()) {
            error = "the site address has no usable host name";
            return false;
        }
    }
    out = s;
    return true;
}

const char* pluginHostOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* pluginHostArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x64";
#else
    return "x86";
#endif
}

}  // namespace cascade::core
