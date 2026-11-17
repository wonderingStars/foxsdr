// Tests for the web server's Host and Origin check (net/web_guard.hpp), security review 2 finding R1: the pure rules.
// The same rules over a real listener are in test_web_server.cpp (the "R1" tests).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <random>
#include <string>
#include <vector>

#include "net/web_guard.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

HostOriginPolicy basePolicy() {
    HostOriginPolicy p;
    p.machineNames = {"studio-pc", "studio-pc.local"};
    return p;
}

GuardRequest req(const std::string& host, const std::string& origin = "", const std::string& method = "GET") {
    GuardRequest r;
    r.method = method;
    if (!host.empty()) { r.hostValues.push_back(host); }
    if (!origin.empty()) { r.originValues.push_back(origin); }
    return r;
}

void testHost() {
    const HostOriginPolicy p = basePolicy();
    // what a browser sends to a page it was pointed at by an honest address
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("localhost:8073")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("LocalHost")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("[::1]:8073")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("192.168.1.20:8073")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("studio-pc:8073")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("Studio-PC.local:8073")) == GuardVerdict::Allowed);
    // DNS rebinding: a name the attacker owns that now points at this machine
    CHECK(checkHostOrigin(p, req("evil.example:8073")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("evil.example")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("127.0.0.1.evil.example:8073")) == GuardVerdict::ForeignHost);   // looks like an address, is a name
    CHECK(checkHostOrigin(p, req("localhost.evil.example")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("999.1.1.1")) == GuardVerdict::ForeignHost);
    // malformed
    CHECK(checkHostOrigin(p, req("127.0.0.1:80a")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("127.0.0.1 :80")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("evil.example@127.0.0.1")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("[::1")) == GuardVerdict::ForeignHost);
    // two Host headers
    GuardRequest two = req("127.0.0.1");
    two.hostValues.push_back("evil.example");
    CHECK(checkHostOrigin(p, two) == GuardVerdict::ForeignHost);
    // no Host header at all: an HTTP/1.0 tool, left alone as before
    CHECK(checkHostOrigin(p, req("")) == GuardVerdict::Allowed);
}

void testOrigin() {
    const HostOriginPolicy p = basePolicy();
    // no Origin from a non-browser client: allowed, as before
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "", "POST")) == GuardVerdict::Allowed);
    // the server's own origin (what its own page sends)
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "http://127.0.0.1:8073", "POST")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("localhost:8073", "http://localhost:8073", "POST")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("127.0.0.1", "http://127.0.0.1", "POST")) == GuardVerdict::Allowed);        // default port both sides
    CHECK(checkHostOrigin(p, req("127.0.0.1:443", "https://127.0.0.1", "POST")) == GuardVerdict::Allowed);   // :443 written, https
    // another web site
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://evil.example", "POST")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://evil.example", "GET")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "http://127.0.0.1:9999", "POST")) == GuardVerdict::ForeignOrigin);   // another local port is another origin
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "http://localhost:8073", "POST")) == GuardVerdict::ForeignOrigin);   // another host name is another origin
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "null", "POST")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "http://127.0.0.1:8073/", "POST")) == GuardVerdict::ForeignOrigin);   // a browser never writes the slash
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "HTTP://127.0.0.1:8073", "POST")) == GuardVerdict::ForeignOrigin);
    GuardRequest two = req("127.0.0.1:8073", "http://127.0.0.1:8073", "POST");
    two.originValues.push_back("https://evil.example");
    CHECK(checkHostOrigin(p, two) == GuardVerdict::ForeignOrigin);
    // a foreign Host wins over everything (421 before the Origin is read)
    CHECK(checkHostOrigin(p, req("evil.example", "http://evil.example", "POST")) == GuardVerdict::ForeignHost);
}

void testOwnersList() {
    HostOriginPolicy p = basePolicy();
    guardSetOrigins(p, {"https://dash.example", "http://192.168.1.50:8080"});
    // the listed origin may READ across origins ...
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://dash.example", "GET")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "http://192.168.1.50:8080", "OPTIONS")) == GuardVerdict::Allowed);
    // ... but never change anything from another origin
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://dash.example", "POST")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://dash.example", "DELETE")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://other.example", "GET")) == GuardVerdict::ForeignOrigin);
    // the listed name is also a name this page may be reached by (a dynamic-DNS address typed into the list)
    CHECK(checkHostOrigin(p, req("dash.example", "https://dash.example", "POST")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("dash.example")) == GuardVerdict::Allowed);
    // replacing the list removes the old names; the computer's own stay
    guardSetOrigins(p, {});
    CHECK(checkHostOrigin(p, req("dash.example")) == GuardVerdict::ForeignHost);
    CHECK(checkHostOrigin(p, req("studio-pc")) == GuardVerdict::Allowed);
    // the wildcard reads only
    guardSetOrigins(p, {"*"});
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://anything.example", "GET")) == GuardVerdict::Allowed);
    CHECK(checkHostOrigin(p, req("127.0.0.1:8073", "https://anything.example", "POST")) == GuardVerdict::ForeignOrigin);
    CHECK(checkHostOrigin(p, req("anything.example")) == GuardVerdict::ForeignHost);   // a wildcard is not a host name
}

// M9-api-host-rule: the program interface takes the names the PAGE takes; guardHostNames is that set, built from the same two places, and the page's own check agrees with it.
void testHostNamesAreThePages() {
    const std::vector<std::string> listed = {"https://Dash.example", "http://192.168.1.50:8080", "*", "not an origin", "http://[::1]:9", "https://ddns.example.net:8443"};
    const std::vector<std::string> names = guardHostNames("Studio-PC", listed);
    const std::vector<std::string> want = {"studio-pc", "studio-pc.local", "dash.example", "ddns.example.net"};
    CHECK(names == want);   // no IP literal, no wildcard, nothing that did not parse
    HostOriginPolicy p;
    p.machineNames = guardMachineNames("Studio-PC");
    guardSetOrigins(p, listed);
    for (const std::string& n : names) { CHECK(checkHostOrigin(p, req(n)) == GuardVerdict::Allowed); }   // the page answers each of them
    CHECK(checkHostOrigin(p, req("other.example")) == GuardVerdict::ForeignHost);
    CHECK(guardHostNames("", {}).empty());                                                                  // no name, no list: nothing
    CHECK(guardMachineNames("").empty());
}

void testSplit() {
    std::string h, port;
    CHECK(splitHostHeader("Example.COM:8073", h, port) && h == "example.com" && port == "8073");
    CHECK(splitHostHeader("example.com.", h, port) && h == "example.com" && port.empty());
    CHECK(splitHostHeader("[::1]:80", h, port) && h == "[::1]" && port == "80");
    CHECK(!splitHostHeader("", h, port));
    CHECK(!splitHostHeader("a b", h, port));
    CHECK(!splitHostHeader("a:123456", h, port));
    CHECK(isIpLiteralHost("10.0.0.1") && isIpLiteralHost("[fe80::1]") && !isIpLiteralHost("10.0.0") && !isIpLiteralHost("10.0.0.1.2") &&
          !isIpLiteralHost("example.com") && !isIpLiteralHost("[zz]"));
}

// Whatever bytes arrive, the answer is one of the three and nothing throws.
void testFuzz() {
    std::mt19937 rng(20261116);
    HostOriginPolicy p = basePolicy();
    guardSetOrigins(p, {"https://dash.example"});
    const std::string alphabet = "abcXYZ0123456789.:[]@/\\ \t,;-_%\x01\x7f\xc3\xa9";
    int bad = 0;
    for (int i = 0; i < 4000; ++i) {
        GuardRequest r;
        r.method = (i % 3 == 0) ? "POST" : "GET";
        auto make = [&]() {
            std::string s;
            const int n = static_cast<int>(rng() % 40);
            for (int k = 0; k < n; ++k) { s.push_back(alphabet[rng() % alphabet.size()]); }
            return s;
        };
        const int nh = static_cast<int>(rng() % 3), no = static_cast<int>(rng() % 3);
        for (int k = 0; k < nh; ++k) { r.hostValues.push_back(make()); }
        for (int k = 0; k < no; ++k) { r.originValues.push_back(make()); }
        const GuardVerdict v = checkHostOrigin(p, r);
        if (v != GuardVerdict::Allowed && v != GuardVerdict::ForeignHost && v != GuardVerdict::ForeignOrigin) { ++bad; }
        // a POST from an Origin that is not the request's own is never allowed
        if (v == GuardVerdict::Allowed && r.method == "POST" && !r.originValues.empty() && r.hostValues.empty()) { ++bad; }
    }
    CHECK(bad == 0);
}

}  // namespace

int main() {
    testHost();
    testOrigin();
    testOwnersList();
    testHostNamesAreThePages();
    testSplit();
    testFuzz();
    return testSummary("test_web_guard");
}
