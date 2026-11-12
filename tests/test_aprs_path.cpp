// Tests for core/aprs_path.hpp: APRS header and path parsing into hops.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/aprs_path.hpp"

#include <random>
#include <string>

#include "test_check.hpp"

namespace aprs = cascade::core::aprs;

int main() {
    aprs::Route r;

    // --- a typical RF packet gated to the Internet -----------------------------------------------------------------------------
    CHECK(aprs::parseHeader("KB1ABC-9>APRS,N0DIG*,WIDE2-1,qAR,W1IGT-10:!4903.50N/07201.75W-test", r));
    CHECK(r.source == "KB1ABC" && r.sourceSsid == 9);
    CHECK(r.destination == "APRS");
    CHECK(r.hops.size() == 4);
    CHECK(r.hops[0].kind == aprs::HopKind::Digipeater && r.hops[0].used && r.hops[0].name == "N0DIG");
    CHECK(r.hops[1].kind == aprs::HopKind::Alias && !r.hops[1].used && r.hops[1].requested == 2 && r.hops[1].remaining == 1);
    CHECK(r.hops[2].kind == aprs::HopKind::QConstruct && r.qConstruct == "qAR");
    CHECK(r.hops[3].kind == aprs::HopKind::IGate && r.igate == "W1IGT-10");
    const auto path = r.stationPath();
    CHECK(path.size() == 3 && path[0] == "KB1ABC-9" && path[1] == "N0DIG" && path[2] == "W1IGT-10");

    // --- the "*" marks the element and every one before it as used ---------------------------------------------------------------
    CHECK(aprs::parseHeader("A1B>APRS,DIGI1,DIGI2-3,DIGI3*,DIGI4", r));
    CHECK(r.hops[0].used && r.hops[1].used && r.hops[2].used && !r.hops[3].used);
    CHECK(r.hops[1].ssid == 3);
    CHECK(r.stationPath().size() == 4);   // source + three used digipeaters
    // nothing used yet
    CHECK(aprs::parseHeader("A1B>APRS,WIDE1-1,WIDE2-1", r));
    CHECK(!r.hops[0].used && !r.hops[1].used);
    CHECK(r.hops[0].remaining == 1);
    CHECK(r.stationPath().size() == 1);
    // an alias that has been consumed down to 0 and carries the mark
    CHECK(aprs::parseHeader("A1B>APRS,WIDE1-0*", r));
    CHECK(r.hops[0].used && r.hops[0].remaining == 0);

    // --- Internet-origin packets -------------------------------------------------------------------------------------------------
    CHECK(aprs::parseHeader("N0CALL>APRS,TCPIP*,qAC,T2SERVER", r));
    CHECK(r.hops[0].kind == aprs::HopKind::Marker);
    CHECK(r.qConstruct == "qAC" && r.igate == "T2SERVER");
    CHECK(aprs::parseHeader("N0CALL>APRS,qAS,server.example_1,extra", r));   // after the q construct the text is kept as is
    CHECK(r.hops[1].kind == aprs::HopKind::IGate && r.hops[2].kind == aprs::HopKind::Other);
    for (const char* q : {"qAC", "qAX", "qAU", "qAo", "qAO", "qAS", "qAr", "qAR", "qAZ", "qAI"}) {
        CHECK(aprs::parseHeader(std::string("N0CALL>APRS,") + q + ",SRV", r));
        CHECK(r.qConstruct == q);
    }
    CHECK(aprs::parseHeader("n0call>aprs,wide1-1,qar,srv", r));              // lower case is read; "qar" is not a q construct...
    CHECK(r.qConstruct.empty());                                              // ...so it is a (lower-cased -> upper) hop "QAR"? it must be a digipeater, not q
    CHECK(r.hops[1].kind == aprs::HopKind::Digipeater && r.hops[1].name == "QAR");

    // --- no path, destination only -----------------------------------------------------------------------------------------------
    CHECK(aprs::parseHeader("N0CALL>APRS", r));
    CHECK(r.hops.empty() && r.stationPath().size() == 1);
    CHECK(aprs::parseHeader("N0CALL>APRS-5", r));

    // --- refusals ------------------------------------------------------------------------------------------------------------------
    CHECK(!aprs::parseHeader("", r));
    CHECK(!aprs::parseHeader("N0CALL", r));                   // no '>'
    CHECK(!aprs::parseHeader(">APRS", r));
    CHECK(!aprs::parseHeader("N0CALL>", r));
    CHECK(!aprs::parseHeader("TOOLONGCALL>APRS", r));
    CHECK(!aprs::parseHeader("N0CALL-16>APRS", r));           // SSID above 15
    CHECK(!aprs::parseHeader("N0CALL-A>APRS", r));
    CHECK(!aprs::parseHeader("N0CALL>APRS,,WIDE1-1", r));     // empty element
    CHECK(!aprs::parseHeader("N0CALL>APRS,WIDE1-1,", r));
    CHECK(!aprs::parseHeader("N0CALL>APRS,*", r));
    CHECK(!aprs::parseHeader("N0CALL>APRS,BAD CALL", r));
    CHECK(!aprs::parseHeader("N0CALL>APRS,qAR*,X", r));       // a q construct is never "used"
    CHECK(!aprs::parseHeader("N0CALL>APRS,qAR,a,b", r) == false);   // text after q is free-form: accepted
    CHECK(!aprs::parseHeader("N0CALL>APRS,qAR,bad>x", r));
    CHECK(!aprs::parseHeader("N0CALL>APRS,qAR,bad\x01x", r));
    CHECK(r.hops.empty() && r.source.empty());                // a refusal leaves an empty route
    {
        std::string many = "N0CALL>APRS";
        for (int i = 0; i < 16; ++i) many += ",DIGI" + std::to_string(i % 10);
        CHECK(aprs::parseHeader(many, r));                    // 16 elements: at the cap
        many += ",DIGI9";
        CHECK(!aprs::parseHeader(many, r));                   // 17: refused, not truncated
        std::string huge = "N0CALL>APRS," + std::string(600, 'A');
        CHECK(!aprs::parseHeader(huge, r));                   // over 512 bytes
    }

    // --- hostile input never crashes ----------------------------------------------------------------------------------------------
    std::mt19937 rng(7);
    const std::string alphabet = "AB1-*,>:q\x01\xff WIDETCPI";
    int parsedOk = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string t;
        const int n = static_cast<int>(rng() % 60);
        for (int k = 0; k < n; ++k) t += alphabet[rng() % alphabet.size()];
        if (aprs::parseHeader(t, r)) ++parsedOk;
    }
    CHECK(parsedOk >= 0);
    std::string nul("N0CALL>APRS,W", 13);
    nul += '\0';
    nul += "X";
    CHECK(!aprs::parseHeader(nul, r));

    return testSummary("test_aprs_path");
}
