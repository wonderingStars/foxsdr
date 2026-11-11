// web_cors.hpp - the CORS allow-list matcher for the web page's API (feat/webmodels).
//
// WHY. A page served from another origin (a dashboard, a Virtual Radar Server style front end) can read this server's replies only if the
// server answers with Access-Control-Allow-Origin naming that origin. Today the server sends no CORS header at all, so it is same-origin only.
// This model decides, from the request's headers, WHICH CORS headers the server may add: a pure function, no server needed.
//
// THE RULES come from the Fetch Standard (https://fetch.spec.whatwg.org/, opened 2026-11-11: sections 3.2 "Origin header", 4.8 CORS-preflight
// fetch, 4.10 CORS check; the text is CC-BY-licensed by the WHATWG, which this file does not copy, it only follows the rules):
//   * The Origin header is either a serialized origin (scheme "://" host [":" port]; scheme and host lowercase ASCII; the port omitted when it
//     is the scheme's default) or the literal `null`. ONE value only.
//   * The browser's CORS check passes when Access-Control-Allow-Origin is `*` and the request did not use credentials, or when it is exactly
//     the request's serialized origin. With credentials, `*` FAILS and Access-Control-Allow-Credentials must be exactly `true`.
//   * A preflight is an OPTIONS request with Access-Control-Request-Method (and Access-Control-Request-Headers when it has unsafe headers); the
//     reply needs the CORS check, an ok status, and Access-Control-Allow-Methods / -Headers listing what is asked (a `*` in those lists is not a
//     wildcard when credentials are used); Access-Control-Max-Age (default 5 s when absent; capped by the browser).
//   * `Vary: Origin` on every reply whose CORS headers depend on the Origin is HTTP caching practice (RFC 9110), not something the Fetch text
//     opened here says; it is added because a shared cache must not hand one origin's answer to another.
//
// OUR RULES ON TOP (each tested in tests/test_web_cors.cpp):
//   1. EXACT ORIGINS ONLY. A configured entry is scheme://host[:port] with an http or https scheme, a valid host (DNS name, IPv4 or a bracketed
//      IPv6 literal) and nothing else: no path, query, fragment, userinfo, no `*` inside a name (no wildcard subdomains), never `null`.
//      Entries are lower-cased and a default port is dropped, so what is compared is the browser's own serialization.
//   2. NO WILDCARD WITH CREDENTIALS. The one wildcard entry, "*", is refused when allowCredentials is set (the configure call fails; nothing
//      is stored). With a wildcard and no credentials the reply carries `Access-Control-Allow-Origin: *` and no credentials header.
//   3. A REQUEST'S Origin MUST ALREADY BE CANONICAL: upper case, a trailing slash, a default port written out, whitespace, a second value (two
//      Origin headers, or a comma list) all DENY. A browser never sends those, so only a forger or a confused client is refused.
//   4. DENIED = NO CORS HEADERS (the browser then blocks the page's read; the request itself is not stopped here, CORS is not authentication).
//      A denied preflight carries no Allow-* headers either.
//   5. The echoed origin is the CONFIGURED one, never the request's bytes, so nothing a client sent is reflected into a header.
//   6. Header names in Access-Control-Request-Headers are compared case-insensitively against the configured list and echoed in the
//      configured spelling; one that is not listed denies the preflight.
//
// SAME-SITE COOKIE NOTE. The session cookie is SameSite=Strict (web_server.cpp), so a cross-origin page's credentialed request would not carry
// it whatever this model says: allowCredentials is only useful together with a change of that cookie, which is the owner's decision (see the
// record). Without credentials a cross-origin page can read only what an unauthenticated request may.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cascade::net {

inline constexpr std::size_t kMaxCorsOrigins = 32;
inline constexpr std::size_t kMaxCorsOriginLength = 255;
inline constexpr std::size_t kMaxCorsHeaderValue = 2048;

struct CorsConfig {
    std::vector<std::string> origins;  // "https://example.com", "http://192.168.1.20:8080", or the single entry "*"
    bool allowCredentials = false;
    std::vector<std::string> methods = {"GET", "POST", "OPTIONS"};
    std::vector<std::string> headers = {"Content-Type"};
    int maxAgeSeconds = 600;           // 0..86400
};

struct CorsRequest {
    std::vector<std::string> originValues;       // every Origin header the request carried, in order
    std::string method;                          // the request's own method
    bool hasRequestMethod = false;               // Access-Control-Request-Method present
    std::string requestMethod;
    bool hasRequestHeaders = false;              // Access-Control-Request-Headers present
    std::string requestHeaders;                  // the raw value
};

enum class CorsVerdict {
    NotCors,        // no Origin header: a same-origin or non-browser request; add nothing (except Vary)
    Denied,         // an Origin that is not allowed or not valid
    Allowed,        // add `headers` to the reply
    Preflight,      // an allowed preflight: answer OPTIONS with 204 and `headers`
};

struct CorsDecision {
    CorsVerdict verdict = CorsVerdict::NotCors;
    std::vector<std::pair<std::string, std::string>> headers;  // exactly what to add to the reply
};

// Parses a serialized origin the way a browser writes it. Strict (rule 3) when `strict`; when not strict (configuration) an upper case scheme or
// host and a default port are accepted and normalised. Returns false for anything else. `canonical` is scheme://host[:port].
bool parseCorsOrigin(const std::string& text, bool strict, std::string& canonical);

class CorsPolicy {
public:
    // Validates and stores. On failure nothing changes and `error` says why (rules 1 and 2).
    bool configure(const CorsConfig& cfg, std::string& error);
    // Off until configured: an unconfigured policy denies every Origin (and adds nothing).
    bool configured() const { return configured_; }
    CorsDecision evaluate(const CorsRequest& req) const;
    const std::vector<std::string>& origins() const { return origins_; }

private:
    bool configured_ = false;
    bool wildcard_ = false;
    bool credentials_ = false;
    std::vector<std::string> origins_;  // canonical
    std::vector<std::string> methods_;
    std::vector<std::string> headers_;
    int maxAge_ = 600;
};

}  // namespace cascade::net
