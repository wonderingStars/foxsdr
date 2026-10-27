// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tester_usage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

bool validTesterToken(const std::string& token) {
    if (token.size() != 32) { return false; }
    for (char c : token) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) { return false; }
    }
    return true;
}

std::string extractTesterToken(const std::string& input) {
    const std::string t = trim(input);
    if (t.empty()) { return std::string(); }

    // THE BARE TOKEN. Checked first and checked exactly - a 32-character
    // string that happens to look like a URL fragment (it cannot: the
    // alphabet is 0-9a-f only) would otherwise fall through to the parser
    // below for no reason.
    if (validTesterToken(t)) { return t; }

    // THE PRIVATE PORTAL LINK: https://foxsdr.com/#t=<token> (web/portal.js's
    // manageLink, "/#t=" + token). The token lives in the fragment, so this
    // is a plain string search rather than a URL parse - there is no server
    // this could leak to at this point, the string never left the clipboard.
    //
    // "#c=" (confirmLink, a single-use confirmation secret) is deliberately
    // NOT accepted here: it opens nothing this field is for, and beta.go
    // keeps the two in separate indexes precisely so one can never be
    // presented in place of the other.
    const std::size_t hash = t.find('#');
    if (hash == std::string::npos) { return std::string(); }
    std::string frag = t.substr(hash + 1);
    // The fragment may carry more than one key ("t=...&x=...", though the
    // site never emits that); split on '&' and look for "t=" among the
    // parts, the same way URLSearchParams would.
    std::size_t at = 0;
    while (at <= frag.size()) {
        const std::size_t amp = frag.find('&', at);
        const std::string part =
            frag.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.size() > 2 && part[0] == 't' && part[1] == '=') {
            const std::string candidate = trim(part.substr(2));
            if (validTesterToken(candidate)) { return candidate; }
        }
        if (amp == std::string::npos) { break; }
        at = amp + 1;
    }
    return std::string();
}

bool validAppToken(const std::string& token) {
    if (token.size() != 40) { return false; }
    for (char c : token) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) { return false; }
    }
    return true;
}

std::string extractAppToken(const std::string& input) {
    const std::string t = trim(input);
    return validAppToken(t) ? t : std::string();
}

std::string maskTesterToken(const std::string& token) {
    if (token.size() <= 8) { return std::string(token.size(), '*'); }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

// ---------------------------------------------------------------------------
// The bounded queue
// ---------------------------------------------------------------------------

void TesterUsageQueue::push(const std::string& reportJson) {
    if (reportJson.empty()) { return; }
    items_.push_back(reportJson);
    while (items_.size() > kMax) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::removeFront() {
    if (!items_.empty()) { items_.erase(items_.begin()); }
}

void TesterUsageQueue::setItems(std::vector<std::string> items) {
    if (items.size() > kMax) {
        items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(items.size() - kMax));
    }
    items_ = std::move(items);
}

std::string tokenOfReport(const std::string& reportJson) {
    const nlohmann::json j = nlohmann::json::parse(reportJson, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return std::string(); }
    const auto it = j.find("token");
    if (it == j.end() || !it->is_string()) { return std::string(); }
    return it->get<std::string>();
}

void TesterUsageQueue::dropOthers(const std::string& token) {
    std::vector<std::string> kept;
    kept.reserve(items_.size());
    for (const std::string& item : items_) {
        if (tokenOfReport(item) == token) { kept.push_back(item); }
    }
    items_ = std::move(kept);
}

void TesterUsageQueue::rewriteToken(const std::string& newToken) {
    for (std::string& item : items_) {
        nlohmann::json j = nlohmann::json::parse(item, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) { continue; }
        j["token"] = newToken;
        item = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
}

}  // namespace cascade::core
