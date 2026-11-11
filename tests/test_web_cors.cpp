// Tests for the CORS allow-list matcher (net/web_cors.hpp). The rules come from the Fetch Standard (see the header).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "net/web_cors.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

std::string get(const Headers& h, const std::string& name) {
    for (const auto& kv : h) {
        if (kv.first == name) { return kv.second; }
    }
    return "<absent>";
}

bool has(const Headers& h, const std::string& name) { return get(h, name) != "<absent>"; }

CorsRequest simple(const std::string& origin, const std::string& method = "GET") {
    CorsRequest r;
    r.method = method;
    if (!origin.empty()) { r.originValues.push_back(origin); }
    return r;
}

CorsRequest preflight(const std::string& origin, const std::string& m, const std::string& hdrs = "") {
    CorsRequest r;
    r.method = "OPTIONS";
    r.originValues.push_back(origin);
    r.hasRequestMethod = true;
    r.requestMethod = m;
    if (!hdrs.empty()) { r.hasRequestHeaders = true; r.requestHeaders = hdrs; }
    return r;
}

CorsPolicy make(std::vector<std::string> origins, bool creds = false) {
    CorsConfig c;
    c.origins = std::move(origins);
    c.allowCredentials = creds;
    CorsPolicy p;
    std::string err;
    CHECK(p.configure(c, err));
    return p;
}

void testParseOrigin() {
    std::string c;
    CHECK(parseCorsOrigin("https://example.com", true, c) && c == "https://example.com");
    CHECK(parseCorsOrigin("http://example.com:8080", true, c) && c == "http://example.com:8080");
    CHECK(parseCorsOrigin("http://192.168.1.20:8073", true, c) && c == "http://192.168.1.20:8073");
    CHECK(parseCorsOrigin("http://[::1]:8073", true, c) && c == "http://[::1]:8073");
    CHECK(parseCorsOrigin("https://[2001:db8::1]", true, c) && c == "https://[2001:db8::1]");
    CHECK(parseCorsOrigin("https://a-b.example.co.uk", true, c));
    // strict (a request): what a browser never writes is refused
    CHECK(!parseCorsOrigin("HTTPS://example.com", true, c));            // upper-case scheme
    CHECK(!parseCorsOrigin("https://Example.com", true, c));            // upper-case host
    CHECK(!parseCorsOrigin("https://example.com:443", true, c));        // default port written out
    CHECK(!parseCorsOrigin("http://example.com:80", true, c));
    CHECK(!parseCorsOrigin("https://example.com/", true, c));           // trailing slash
    CHECK(!parseCorsOrigin("https://example.com/path", true, c));
    CHECK(!parseCorsOrigin("https://example.com ", true, c));
    CHECK(!parseCorsOrigin(" https://example.com", true, c));
    CHECK(!parseCorsOrigin("null", true, c));
    CHECK(!parseCorsOrigin("", true, c));
    CHECK(!parseCorsOrigin("https://", true, c));
    CHECK(!parseCorsOrigin("example.com", true, c));
    CHECK(!parseCorsOrigin("ftp://example.com", true, c));
    CHECK(!parseCorsOrigin("file://example.com", true, c));
    CHECK(!parseCorsOrigin("javascript://example.com", true, c));
    CHECK(!parseCorsOrigin("https://user@example.com", true, c));       // userinfo
    CHECK(!parseCorsOrigin("https://user:pw@example.com", true, c));
    CHECK(!parseCorsOrigin("https://example.com?x=1", true, c));
    CHECK(!parseCorsOrigin("https://example.com#f", true, c));
    CHECK(!parseCorsOrigin("https://example.com\\evil.com", true, c));
    CHECK(!parseCorsOrigin("https://exa mple.com", true, c));
    CHECK(!parseCorsOrigin("https://example.com,https://evil.com", true, c));   // a comma list
    CHECK(!parseCorsOrigin("https://example.com;x", true, c));
    CHECK(!parseCorsOrigin("https://*.example.com", true, c));
    CHECK(!parseCorsOrigin("https://example.com:", true, c));
    CHECK(!parseCorsOrigin("https://example.com:0", true, c));
    CHECK(!parseCorsOrigin("https://example.com:65536", true, c));
    CHECK(!parseCorsOrigin("https://example.com:08080", true, c));      // leading zero
    CHECK(!parseCorsOrigin("https://example.com:80a", true, c));
    CHECK(!parseCorsOrigin("https://example.com:-1", true, c));
    CHECK(!parseCorsOrigin("https://-bad.example.com", true, c));
    CHECK(!parseCorsOrigin("https://bad-.example.com", true, c));
    CHECK(!parseCorsOrigin("https://bad..example.com", true, c));
    CHECK(!parseCorsOrigin("https://.example.com", true, c));
    CHECK(!parseCorsOrigin("https://example.com.", true, c));
    CHECK(!parseCorsOrigin("https://caf\xC3\xA9.example.com", true, c));        // IDN must arrive as punycode
    CHECK(!parseCorsOrigin("https://exa_mple.com", true, c));
    CHECK(!parseCorsOrigin(std::string("https://example.com\0evil", 24), true, c));
    CHECK(!parseCorsOrigin("https://example.com\r\nX-Evil: 1", true, c));       // header injection
    CHECK(!parseCorsOrigin("https://999.1.1.1", true, c));                       // numeric-looking hosts must be real IPv4
    CHECK(!parseCorsOrigin("https://1.2.3", true, c));
    CHECK(!parseCorsOrigin("https://01.2.3.4", true, c));
    CHECK(!parseCorsOrigin("https://[::1", true, c));
    CHECK(!parseCorsOrigin("https://[zz::1]", true, c));
    CHECK(!parseCorsOrigin("https://[::1]x", true, c));
    CHECK(!parseCorsOrigin("https://" + std::string(64, 'a') + ".com", true, c));   // label over 63
    CHECK(!parseCorsOrigin("https://" + std::string(300, 'a'), true, c));
    CHECK(parseCorsOrigin("https://" + std::string(63, 'a') + ".com", true, c));     // the boundary
    // not strict (configuration): case folded, default port dropped
    CHECK(parseCorsOrigin("HTTPS://Example.COM", false, c) && c == "https://example.com");
    CHECK(parseCorsOrigin("https://example.com:443", false, c) && c == "https://example.com");
    CHECK(parseCorsOrigin("http://example.com:80", false, c) && c == "http://example.com");
    CHECK(parseCorsOrigin("https://example.com:80", false, c) && c == "https://example.com:80");   // 80 is not https's default
    CHECK(!parseCorsOrigin("https://example.com/", false, c));
    CHECK(!parseCorsOrigin("null", false, c));
}

void testConfigure() {
    CorsPolicy p;
    CHECK(!p.configured());
    std::string err;
    CorsConfig c;
    c.origins = {"https://a.example", "https://a.example:443", "HTTPS://A.EXAMPLE"};
    CHECK(p.configure(c, err) && p.configured());
    CHECK(p.origins().size() == 1 && p.origins()[0] == "https://a.example");        // one canonical entry
    // refusals leave the previous configuration in place
    c.origins = {"https://a.example/path"};
    CHECK(!p.configure(c, err) && !err.empty());
    c.origins = {"https://*.example.com"};
    CHECK(!p.configure(c, err));
    c.origins = {"null"};
    CHECK(!p.configure(c, err));
    c.origins = {"example.com"};
    CHECK(!p.configure(c, err));
    c.origins = {"*"};
    c.allowCredentials = true;
    CHECK(!p.configure(c, err));                                                    // rule 2: no wildcard with credentials
    CHECK(err.find("credentials") != std::string::npos);
    c.allowCredentials = false;
    c.origins = {"*", "https://a.example"};
    CHECK(!p.configure(c, err));                                                    // no mixing
    c.origins = {"https://a.example"};
    c.maxAgeSeconds = -1;
    CHECK(!p.configure(c, err));
    c.maxAgeSeconds = 86401;
    CHECK(!p.configure(c, err));
    c.maxAgeSeconds = 86400;
    CHECK(p.configure(c, err));
    c.maxAgeSeconds = 600;
    c.methods = {"GET", "*"};
    CHECK(!p.configure(c, err));
    c.methods = {"GE T"};
    CHECK(!p.configure(c, err));
    c.methods = {"GET"};
    c.headers = {"*"};
    CHECK(!p.configure(c, err));
    c.headers = {"Content Type"};
    CHECK(!p.configure(c, err));
    c.headers = {"Content-Type"};
    std::vector<std::string> many;
    for (std::size_t i = 0; i <= kMaxCorsOrigins; ++i) { many.push_back("https://h" + std::to_string(i) + ".example"); }
    c.origins = many;
    CHECK(!p.configure(c, err));
    many.pop_back();
    c.origins = many;
    CHECK(p.configure(c, err));                                                     // exactly the cap
    // after all those refusals the last successful configuration is the live one
    CHECK(p.origins().size() == kMaxCorsOrigins);
}

void testUnconfiguredDeniesEverything() {
    CorsPolicy p;
    CorsDecision d = p.evaluate(simple("https://a.example"));
    CHECK(d.verdict == CorsVerdict::Denied && d.headers.empty());
    d = p.evaluate(simple(""));
    CHECK(d.verdict == CorsVerdict::NotCors && d.headers.empty());
    d = p.evaluate(preflight("https://a.example", "GET"));
    CHECK(d.verdict == CorsVerdict::Denied && d.headers.empty());
}

void testSimpleAllowAndDeny() {
    const CorsPolicy p = make({"https://dash.example", "http://192.168.1.20:8080"});
    CorsDecision d = p.evaluate(simple("https://dash.example"));
    CHECK(d.verdict == CorsVerdict::Allowed);
    CHECK(get(d.headers, "Access-Control-Allow-Origin") == "https://dash.example");
    CHECK(get(d.headers, "Vary") == "Origin");
    CHECK(!has(d.headers, "Access-Control-Allow-Credentials"));
    d = p.evaluate(simple("http://192.168.1.20:8080", "POST"));
    CHECK(d.verdict == CorsVerdict::Allowed && get(d.headers, "Access-Control-Allow-Origin") == "http://192.168.1.20:8080");
    // not listed
    d = p.evaluate(simple("https://evil.example"));
    CHECK(d.verdict == CorsVerdict::Denied && !has(d.headers, "Access-Control-Allow-Origin"));
    CHECK(get(d.headers, "Vary") == "Origin");                                      // caches must still vary
    // same host, other scheme or port: a different origin
    CHECK(p.evaluate(simple("http://dash.example")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("https://dash.example:8443")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("http://192.168.1.20:8081")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("http://192.168.1.20")).verdict == CorsVerdict::Denied);
    // a suffix or prefix of an allowed host is not the host
    CHECK(p.evaluate(simple("https://evil-dash.example")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("https://dash.example.evil.com")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("https://x.dash.example")).verdict == CorsVerdict::Denied);   // no wildcard subdomains
    // case: a browser writes lower case; anything else is not a browser
    CHECK(p.evaluate(simple("https://DASH.example")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("HTTPS://dash.example")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("https://dash.example:443")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("https://dash.example/")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("null")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple(" https://dash.example")).verdict == CorsVerdict::Denied);
    // two Origin headers, or two values: refused even when both are allowed
    CorsRequest two;
    two.method = "GET";
    two.originValues = {"https://dash.example", "https://dash.example"};
    CHECK(p.evaluate(two).verdict == CorsVerdict::Denied);
    two.originValues = {"https://dash.example, https://evil.example"};
    CHECK(p.evaluate(two).verdict == CorsVerdict::Denied);
    // an oversized value
    CHECK(p.evaluate(simple("https://" + std::string(5000, 'a') + ".example")).verdict == CorsVerdict::Denied);
    // no Origin: not a CORS request; nothing but Vary
    d = p.evaluate(simple(""));
    CHECK(d.verdict == CorsVerdict::NotCors && !has(d.headers, "Access-Control-Allow-Origin") && get(d.headers, "Vary") == "Origin");
}

void testEchoIsTheConfiguredString() {
    // the origin written in the reply is the configured canonical one, never the request's bytes
    const CorsPolicy p = make({"https://Dash.Example:443"});
    const CorsDecision d = p.evaluate(simple("https://dash.example"));
    CHECK(d.verdict == CorsVerdict::Allowed && get(d.headers, "Access-Control-Allow-Origin") == "https://dash.example");
    for (const auto& kv : d.headers) {
        CHECK(kv.second.find('\r') == std::string::npos && kv.second.find('\n') == std::string::npos);
    }
}

void testWildcard() {
    const CorsPolicy p = make({"*"});
    CorsDecision d = p.evaluate(simple("https://anyone.example"));
    CHECK(d.verdict == CorsVerdict::Allowed && get(d.headers, "Access-Control-Allow-Origin") == "*");
    CHECK(!has(d.headers, "Access-Control-Allow-Credentials"));
    CHECK(!has(d.headers, "Vary"));                                                  // the answer does not depend on the origin
    // a malformed origin is still refused, even under the wildcard
    CHECK(p.evaluate(simple("https://anyone.example/path")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(simple("null")).verdict == CorsVerdict::Denied);
    d = p.evaluate(preflight("https://anyone.example", "POST", "content-type"));
    CHECK(d.verdict == CorsVerdict::Preflight && get(d.headers, "Access-Control-Allow-Origin") == "*");
}

void testCredentials() {
    const CorsPolicy p = make({"https://dash.example"}, true);
    const CorsDecision d = p.evaluate(simple("https://dash.example"));
    CHECK(d.verdict == CorsVerdict::Allowed);
    CHECK(get(d.headers, "Access-Control-Allow-Credentials") == "true");             // exactly "true"
    CHECK(get(d.headers, "Access-Control-Allow-Origin") == "https://dash.example");  // never "*" with credentials
    CHECK(get(d.headers, "Access-Control-Allow-Origin") != "*");
    const CorsDecision pf = p.evaluate(preflight("https://dash.example", "POST", "Content-Type"));
    CHECK(pf.verdict == CorsVerdict::Preflight && get(pf.headers, "Access-Control-Allow-Credentials") == "true");
    CHECK(get(pf.headers, "Access-Control-Allow-Origin") == "https://dash.example");
    // a denied request never gets the credentials header
    CHECK(!has(p.evaluate(simple("https://evil.example")).headers, "Access-Control-Allow-Credentials"));
}

void testPreflight() {
    const CorsPolicy p = make({"https://dash.example"});
    CorsDecision d = p.evaluate(preflight("https://dash.example", "POST", "content-type"));
    CHECK(d.verdict == CorsVerdict::Preflight);
    CHECK(get(d.headers, "Access-Control-Allow-Methods") == "GET, POST, OPTIONS");
    CHECK(get(d.headers, "Access-Control-Allow-Headers") == "Content-Type");          // configured spelling, not the request's
    CHECK(get(d.headers, "Access-Control-Max-Age") == "600");
    CHECK(get(d.headers, "Access-Control-Allow-Origin") == "https://dash.example");
    CHECK(get(d.headers, "Vary") == "Origin");
    // no Access-Control-Request-Headers: no Allow-Headers needed
    d = p.evaluate(preflight("https://dash.example", "GET"));
    CHECK(d.verdict == CorsVerdict::Preflight && !has(d.headers, "Access-Control-Allow-Headers"));
    // a method that is not configured, a lower-case method, a bad token
    d = p.evaluate(preflight("https://dash.example", "DELETE"));
    CHECK(d.verdict == CorsVerdict::Denied && !has(d.headers, "Access-Control-Allow-Methods") && !has(d.headers, "Access-Control-Allow-Origin"));
    CHECK(p.evaluate(preflight("https://dash.example", "get")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "G E T")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "GET\r\nX: y")).verdict == CorsVerdict::Denied);
    // a header that is not configured denies the whole preflight, wherever it sits in the list
    CHECK(p.evaluate(preflight("https://dash.example", "POST", "content-type, x-evil")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "POST", "x-evil, content-type")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "POST", "authorization")).verdict == CorsVerdict::Denied);
    CHECK(p.evaluate(preflight("https://dash.example", "POST", "*")).verdict == CorsVerdict::Denied);   // not a wildcard here
    CHECK(p.evaluate(preflight("https://dash.example", "POST", "content type")).verdict == CorsVerdict::Denied);
    // spacing, empty items and repeats are tolerated
    d = p.evaluate(preflight("https://dash.example", "POST", " Content-Type ,, CONTENT-TYPE "));
    CHECK(d.verdict == CorsVerdict::Preflight && get(d.headers, "Access-Control-Allow-Headers") == "Content-Type");
    // a preflight from an origin that is not allowed carries nothing
    d = p.evaluate(preflight("https://evil.example", "GET"));
    CHECK(d.verdict == CorsVerdict::Denied && !has(d.headers, "Access-Control-Allow-Methods"));
    // an OPTIONS WITHOUT Access-Control-Request-Method is an ordinary request, not a preflight
    CorsRequest plain = simple("https://dash.example", "OPTIONS");
    CHECK(p.evaluate(plain).verdict == CorsVerdict::Allowed);
    // a huge Access-Control-Request-Headers
    CHECK(p.evaluate(preflight("https://dash.example", "POST", std::string(kMaxCorsHeaderValue + 1, 'a'))).verdict == CorsVerdict::Denied);
    // configured max age, methods and headers come through
    CorsConfig c;
    c.origins = {"https://dash.example"};
    c.methods = {"get", "put"};
    c.headers = {"X-Foo", "Content-Type"};
    c.maxAgeSeconds = 0;
    CorsPolicy q;
    std::string err;
    CHECK(q.configure(c, err));
    d = q.evaluate(preflight("https://dash.example", "PUT", "x-foo,content-type"));
    CHECK(d.verdict == CorsVerdict::Preflight && get(d.headers, "Access-Control-Allow-Methods") == "GET, PUT" &&
          get(d.headers, "Access-Control-Allow-Headers") == "X-Foo, Content-Type" && get(d.headers, "Access-Control-Max-Age") == "0");
}

// FUZZ: arbitrary Origin / request-method / request-header bytes never throw, never reflect a byte the configuration did not hold, never emit a
// line break in a header value, and never allow an origin outside the list.
void testFuzz() {
    const CorsPolicy p = make({"https://dash.example", "http://10.0.0.5:8080"}, true);
    const std::vector<std::string> seeds = {"https://dash.example", "http://10.0.0.5:8080", "https://evil.example", "null", "",
                                            "content-type", "GET", "POST", "https://dash.example, https://evil.example"};
    std::mt19937_64 rng(2026);
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        auto mutate = [&](std::string s) {
            const int ops = static_cast<int>(rng() % 5);
            for (int k = 0; k < ops; ++k) {
                if (s.empty()) { s.push_back(static_cast<char>(rng())); continue; }
                const std::size_t pos = rng() % s.size();
                switch (rng() % 4) {
                    case 0: s[pos] = static_cast<char>(rng()); break;
                    case 1: s.erase(pos, 1 + rng() % 3); break;
                    case 2: s.insert(pos, 1, static_cast<char>(rng())); break;
                    default: s.resize(pos); break;
                }
            }
            return s;
        };
        CorsRequest r;
        r.method = (rng() % 3 == 0) ? "OPTIONS" : "GET";
        const int n = static_cast<int>(rng() % 3);
        for (int k = 0; k < n; ++k) { r.originValues.push_back(mutate(seeds[rng() % seeds.size()])); }
        r.hasRequestMethod = rng() % 2;
        r.requestMethod = mutate(seeds[rng() % seeds.size()]);
        r.hasRequestHeaders = rng() % 2;
        r.requestHeaders = mutate(seeds[rng() % seeds.size()]);
        CorsDecision d;
        try { d = p.evaluate(r); } catch (...) { ++bad; continue; }
        for (const auto& kv : d.headers) {
            if (kv.second.find('\r') != std::string::npos || kv.second.find('\n') != std::string::npos || kv.first.find('\n') != std::string::npos) { ++bad; }
        }
        if (d.verdict == CorsVerdict::Allowed || d.verdict == CorsVerdict::Preflight) {
            const std::string ao = get(d.headers, "Access-Control-Allow-Origin");
            if (ao != "https://dash.example" && ao != "http://10.0.0.5:8080") { ++bad; }
            // an allowed answer exists only for exactly one origin value that is itself in the list
            if (r.originValues.size() != 1 || (r.originValues[0] != ao)) { ++bad; }
            if (get(d.headers, "Access-Control-Allow-Credentials") != "true") { ++bad; }
        } else {
            if (has(d.headers, "Access-Control-Allow-Origin") || has(d.headers, "Access-Control-Allow-Credentials")) { ++bad; }
        }
    }
    CHECK(bad == 0);
}

}  // namespace

int main() {
    testParseOrigin();
    testConfigure();
    testUnconfiguredDeniesEverything();
    testSimpleAllowAndDeny();
    testEchoIsTheConfiguredString();
    testWildcard();
    testCredentials();
    testPreflight();
    testFuzz();
    return testSummary("test_web_cors");
}
