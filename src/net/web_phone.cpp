// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_phone.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace cascade::net::phone {

namespace {

constexpr char kManifest[] = R"JSON({
  "name": "FoxSDR",
  "short_name": "FoxSDR",
  "description": "Remote control and display for a FoxSDR receiver.",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "background_color": "#1f1b14",
  "theme_color": "#1f1b14",
  "icons": [
    { "src": "/icons/foxsdr-192.png", "sizes": "192x192", "type": "image/png" },
    { "src": "/icons/foxsdr-512.png", "sizes": "512x512", "type": "image/png" }
  ]
}
)JSON";

constexpr char kHead[] =
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
    "<meta name=\"theme-color\" content=\"#1f1b14\">\n"
    "<link rel=\"manifest\" href=\"/manifest.webmanifest\">\n";

// Colours are deliberately absent: the existing stylesheet's tokens govern.
constexpr char kCss[] = R"CSS(
/* PHONE ADDITIONS (served after the main stylesheet). 44 CSS px targets (W3C
   WCAG 2.2 SC 2.5.5; 2.5.8's minimum is 24) and the device's safe areas. */
@media (max-width: 600px) {
  button, select, summary,
  input[type="text"], input[type="number"], input[type="password"], input[type="range"] {
    min-height: 44px;
  }
  button, select { min-width: 44px; }
  body {
    padding-left: env(safe-area-inset-left);
    padding-right: env(safe-area-inset-right);
    padding-bottom: env(safe-area-inset-bottom);
  }
  table { display: block; max-width: 100%; overflow-x: auto; }
}
)CSS";

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

std::string manifestJson() { return kManifest; }
const char* headTags() { return kHead; }
const char* phoneCss() { return kCss; }

std::string cspWithManifest(const std::string& csp) {
    if (lower(csp).find("manifest-src") != std::string::npos) { return csp; }
    std::string out = csp;
    while (!out.empty() && (out.back() == ' ' || out.back() == ';')) { out.pop_back(); }
    if (!out.empty()) { out += "; "; }
    out += "manifest-src 'self'";
    return out;
}

Check originInstallable(const std::string& origin) {
    Check c;
    const std::string o = lower(origin);
    std::string host;
    bool https = false, http = false;
    if (o.rfind("https://", 0) == 0) { https = true; host = o.substr(8); }
    else if (o.rfind("http://", 0) == 0) { http = true; host = o.substr(7); }
    else {
        c.problems.push_back("the origin is neither http nor https");
        return c;
    }
    const std::size_t end = host.find_first_of("/?#");
    if (end != std::string::npos) { host.erase(end); }
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos) { host.erase(colon); }
    if (host.empty()) {
        c.problems.push_back("the origin has no host");
        return c;
    }
    if (https) { c.ok = true; return c; }
    if (http && (host == "localhost" || host == "127.0.0.1")) { c.ok = true; return c; }
    c.problems.push_back("plain http is installable only from localhost or 127.0.0.1; use https");
    return c;
}

Check manifestInstallable(const std::string& json) {
    Check c;
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        c.problems.push_back("the manifest is not a JSON object");
        return c;
    }
    auto hasString = [&](const char* key) { return j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty(); };
    if (!hasString("name") && !hasString("short_name")) { c.problems.push_back("name or short_name is missing"); }
    if (!hasString("start_url")) { c.problems.push_back("start_url is missing"); }
    if (!hasString("display") && !(j.contains("display_override") && j["display_override"].is_array() && !j["display_override"].empty())) {
        c.problems.push_back("display or display_override is missing");
    }
    if (j.contains("prefer_related_applications") &&
        !(j["prefer_related_applications"].is_boolean() && !j["prefer_related_applications"].get<bool>())) {
        c.problems.push_back("prefer_related_applications must be false or absent");
    }
    bool has192 = false, has512 = false;
    if (j.contains("icons") && j["icons"].is_array()) {
        for (const auto& icon : j["icons"]) {
            if (!icon.is_object() || !icon.contains("sizes") || !icon["sizes"].is_string() ||
                !icon.contains("src") || !icon["src"].is_string()) { continue; }
            const std::string sizes = icon["sizes"].get<std::string>();
            if (sizes.find("192x192") != std::string::npos) { has192 = true; }
            if (sizes.find("512x512") != std::string::npos) { has512 = true; }
        }
    }
    if (!has192) { c.problems.push_back("no 192x192 icon"); }
    if (!has512) { c.problems.push_back("no 512x512 icon"); }
    c.ok = c.problems.empty();
    return c;
}

}  // namespace cascade::net::phone
