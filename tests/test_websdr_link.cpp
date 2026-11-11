// Tests for core/websdr_link.hpp, the WebSDR link-out. No network.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/websdr_link.hpp"

#include <limits>
#include <string>

#include "test_check.hpp"

using cascade::core::websdr::Link;
using cascade::core::websdr::buildLink;

int main() {
    // The FAQ's own example (https://www.websdr.org/faq.html): 198 kHz AM.
    {
        const Link l = buildLink("http://websdr.ewi.utwente.nl:8901/", 198.0, "am");
        CHECK(l.ok);
        CHECK(l.url == "http://websdr.ewi.utwente.nl:8901/?tune=198am");
        CHECK(l.modeSuffixDocumented);
    }
    // Missing path, existing query and fragment dropped, https, upper-case scheme.
    CHECK(buildLink("http://example.org:8901", 7055.0, "am").url == "http://example.org:8901/?tune=7055am");
    CHECK(buildLink("HTTPS://example.org/sdr/?old=1#frag", 14200.5, "am").url == "https://example.org/sdr/?tune=14200.5am");
    CHECK(buildLink("http://example.org/", 3573.125, "am").url == "http://example.org/?tune=3573.125am");
    CHECK(buildLink("http://example.org/", 1000.1234, "am").url == "http://example.org/?tune=1000.123am");  // three decimals
    // A mode whose suffix is not documented: frequency only, flagged.
    {
        const Link l = buildLink("http://example.org/", 7100.0, "usb");
        CHECK(l.ok);
        CHECK(l.url == "http://example.org/?tune=7100");
        CHECK(!l.modeSuffixDocumented);
        const Link n = buildLink("http://example.org/", 7100.0, "");
        CHECK(n.ok && n.url == "http://example.org/?tune=7100" && !n.modeSuffixDocumented);
    }
    // Refusals: schemes, hosts, credentials, ports, characters, frequencies, modes.
    const char* bad[] = {"", "example.org", "ftp://example.org/", "javascript:alert(1)", "file:///c:/x", "http://",
                         "https:///path", "http://user:pw@example.org/", "http://user@example.org/", "https://example.org@evil.example/", "http://example.org:0/", "http://example.org:99999/",
                         "http://example.org:80a/", "http://example.org:/", "http://exa mple.org/", "http://example.org/\"x",
                         "http://example.org/<x>", "http://:8901/", "data:text/html,x"};
    for (const char* b : bad) {
        const Link l = buildLink(b, 198.0, "am");
        CHECK(!l.ok);
        CHECK(l.url.empty());
        CHECK(!l.error.empty());
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double f : {0.0, -5.0, nan, inf, 3.0e6 + 1.0}) { CHECK(!buildLink("http://example.org/", f, "am").ok); }
    CHECK(buildLink("http://example.org/", 3.0e6, "am").ok);
    CHECK(!buildLink("http://example.org/", 198.0, "a m").ok);
    CHECK(!buildLink("http://example.org/", 198.0, "am&x=1").ok);
    return testSummary("test_websdr_link");
}
