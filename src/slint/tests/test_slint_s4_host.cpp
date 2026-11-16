// Screen lane S4's pure rules (src/slint/s4_logic.hpp; docs/multi-radio-records/screen-s4-wave8.md): typed whole numbers, the one-member server-configuration texts, the keyword line, the "leaves this
// computer" test and the tracker that saves a scan's hits as bookmarks.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"
#include "s4_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slints4;
using nlohmann::json;

int main() {
    // typed whole numbers: both ends are inside, one step outside is refused, nothing else is a number
    CHECK(parseWhole("1024", 1024, 65535) == 1024 && parseWhole(" 65535 ", 1024, 65535) == 65535);
    CHECK(!parseWhole("1023", 1024, 65535) && !parseWhole("65536", 1024, 65535));
    CHECK(!parseWhole("", 1, 8) && !parseWhole("  ", 1, 8) && !parseWhole("-1", 0, 8) && !parseWhole("1.5", 1, 8) && !parseWhole("1e3", 1, 5000) && !parseWhole("12a", 1, 99));
    CHECK(!parseWhole("99999999999999999999", 0, 2147483647) && parseWhole("0", 0, 3600) == 0);

    // the one-member texts the server-configuration operation takes
    CHECK(json::parse(tciBool("enabled", true)) == json::parse(R"({"tci":{"enabled":true}})"));
    CHECK(json::parse(tciBool("bindAll", false)) == json::parse(R"({"tci":{"bindAll":false}})"));
    CHECK(json::parse(tciPort(50001)) == json::parse(R"({"tci":{"port":50001}})"));
    CHECK(json::parse(udpBool("enabled", true)) == json::parse(R"({"udp":{"enabled":true}})"));
    CHECK(json::parse(udpInt("packetFrames", 960)) == json::parse(R"({"udp":{"packetFrames":960}})"));
    CHECK(json::parse(udpText("host", "192.168.1.5")) == json::parse(R"({"udp":{"host":"192.168.1.5"}})"));
    CHECK(json::parse(udpText("host", "a\"b\\c")).at("udp").at("host") == "a\"b\\c");   // quoting survives

    // who the audio goes to
    CHECK(!leavesThisComputer("127.0.0.1") && !leavesThisComputer("localhost") && !leavesThisComputer("LOCALHOST") && !leavesThisComputer("::1") && !leavesThisComputer("127.1.2.3"));
    CHECK(leavesThisComputer("192.168.1.5") && leavesThisComputer("example.org") && !leavesThisComputer("") && leavesThisComputer("10.0.0.1"));

    // the keyword line
    CHECK(keywordsToLine("mayday\nPAN PAN\n\n") == "mayday, PAN PAN");
    CHECK(lineToKeywords("mayday, PAN PAN ;fire,, ") == "mayday\nPAN PAN\nfire");
    CHECK(keywordsToLine("") == "" && lineToKeywords("") == "" && lineToKeywords(" , ; ") == "");
    CHECK(lineToKeywords(keywordsToLine("a\nb c\nd")) == "a\nb c\nd");

    // the tracker: nothing while off; a hit when the scan has just stopped on a signal; once for one frequency; not for what was held when it was turned on
    {
        AutoBookmarks t;
        CHECK(!t.update(false, FOXAPI_SCANNER_PAUSED, 162.55e6));
        CHECK(!t.update(false, FOXAPI_SCANNER_SCANNING, 162.0e6));
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 162.1e6));           // the first look after turning it on: only remembers the state
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 162.2e6));
        const auto hit = t.update(true, FOXAPI_SCANNER_PAUSED, 162.55e6);   // stopped on a signal
        CHECK(hit.has_value() && *hit == 162.55e6);
        CHECK(!t.update(true, FOXAPI_SCANNER_PAUSED, 162.55e6));            // still held: not again
        CHECK(!t.update(true, FOXAPI_SCANNER_HOLDING, 162.55e6));
        CHECK(!t.update(true, FOXAPI_SCANNER_PAUSED, 162.55e6));            // paused again out of holding: same signal, same frequency
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 162.6e6));
        CHECK(!t.update(true, FOXAPI_SCANNER_PAUSED, 162.55003e6));          // 30 Hz away: the same signal
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 162.7e6));
        const auto second = t.update(true, FOXAPI_SCANNER_PAUSED, 162.4e6); // another one
        CHECK(second.has_value() && *second == 162.4e6);
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 163.0e6) && !t.update(true, FOXAPI_SCANNER_PAUSED, 0.0));   // no frequency: nothing to save
        CHECK(t.remembered() == 2);
        t.reset();
        CHECK(t.remembered() == 0);
    }
    {
        AutoBookmarks t;
        CHECK(!t.update(true, FOXAPI_SCANNER_PAUSED, 100.0e6));             // turned on while the scan holds a signal: that one is not saved
        CHECK(!t.update(true, FOXAPI_SCANNER_PAUSED, 100.0e6));
        CHECK(!t.update(false, FOXAPI_SCANNER_SCANNING, 100.1e6));          // switched off: forgets it was on
        CHECK(!t.update(true, FOXAPI_SCANNER_SCANNING, 100.2e6));
        CHECK(t.update(true, FOXAPI_SCANNER_PAUSED, 100.3e6).has_value());
    }
    {
        // a long run of distinct hits stays bounded
        AutoBookmarks t;
        t.update(true, FOXAPI_SCANNER_SCANNING, 1e6);
        for (int i = 0; i < 5000; ++i) {
            t.update(true, FOXAPI_SCANNER_PAUSED, 1e6 + 1000.0 * i);
            t.update(true, FOXAPI_SCANNER_SCANNING, 1e6);
        }
        CHECK(t.remembered() <= 512);
    }
    return testSummary("test_slint_s4_host");
}
