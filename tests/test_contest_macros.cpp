// test_contest_macros.cpp - contest text macros (core/contest_macros.hpp): expansion to text only.
//
//   M1  tokens: MYCALL, CALL, NR (plain and zero-padded), RST, X:KEY; literal braces.
//   M2  sequences: the serial number moves only when logged() is called; a repeated expansion keeps its number.
//   M3  errors: unknown token, unterminated brace, stray close brace, too long, no session: no text is produced.
//   M4  the F-key slots; session set-up refusals; sentTokens() feeds a Cabrillo QSO of the right shape.
//   M5  boundaries: empty template, empty call, serial 99999, expansion at the output limit.
//   M6  fuzz: random templates never crash and either expand within the limit or report an error with no text.
//   M7  NO TRANSMIT PATH: the header's code (comments and strings stripped) names nothing that keys, sends or plays.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>
#include <vector>

#include "core/contest_macros.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::ham;

namespace {

ContestSession session(const char* contest = "CQ-WPX-CW") {
    ContestSession s;
    std::string err;
    const bool ok = s.begin(contest, "w1aw", err);
    CHECK(ok);
    return s;
}

void m1Tokens() {
    ContestSession s = session();
    CHECK(s.expand("{MYCALL}", "").text == "W1AW");
    CHECK(s.expand("CQ TEST {MYCALL} {MYCALL}", "").text == "CQ TEST W1AW W1AW");
    CHECK(s.expand("{CALL} 5NN {NR}", "k1abc").text == "K1ABC 5NN 1");
    CHECK(s.expand("{NR:3}", "").text == "001");
    CHECK(s.expand("{NR:1}", "").text == "1");
    CHECK(s.expand("{RST}", "").text == "599");
    CHECK(s.setValue("ZONE", "05"));
    CHECK(s.expand("{RST} {X:ZONE}", "").text == "599 05");
    CHECK(s.expand("a {{b}} c", "").text == "a {b} c");
    CHECK(s.expand("{{{MYCALL}}}", "").text == "{W1AW}");
    ContestSession ssb = session("CQ-WW-SSB");
    CHECK(ssb.expand("{RST}", "").text == "59");   // a phone contest sends 59
    ssb.setRst("57");
    CHECK(ssb.expand("{RST}", "").text == "57");
}

void m2Sequences() {
    ContestSession s = session();
    CHECK(s.nextSerial() == 1);
    CHECK(s.expand("{NR}", "A1AA").text == "1");
    CHECK(s.expand("{NR}", "A1AA").text == "1");   // asking twice does not advance: a re-sent exchange keeps its number
    s.logged();
    CHECK(s.expand("{NR}", "B2BB").text == "2");
    for (int i = 0; i < 9; ++i) { s.logged(); }
    CHECK(s.nextSerial() == 11);
    CHECK(s.expand("{NR:3}", "").text == "011");
    s.setNextSerial(250);
    CHECK(s.expand("{NR:3}", "").text == "250");
    s.setNextSerial(0);     // refused
    s.setNextSerial(100000);   // refused
    CHECK(s.nextSerial() == 250);
}

void m3Errors() {
    ContestSession s = session();
    for (const char* bad : {"{NOPE}", "{", "abc {MYCALL", "}", "{MYCALL}}", "{NR:0}", "{NR:6}", "{NR:}", "{NR:x}", "{X:MISSING}", "{}", "{ MYCALL}", "{mycall}", "{{}", "{NR:33}"}) {
        const MacroResult r = s.expand(bad, "");
        CHECK(!r.ok);
        CHECK(r.text.empty());
        CHECK(!r.error.empty());
    }
    const MacroResult tooLongTemplate = s.expand(std::string(kMacroTemplateMax + 1, 'a'), "");
    CHECK(!tooLongTemplate.ok && tooLongTemplate.text.empty());
    ContestSession none;
    const MacroResult r = none.expand("{MYCALL}", "");
    CHECK(!r.ok && r.text.empty() && !r.error.empty());
    CHECK(none.sentTokens().empty());
    // an error never repeats the template's own data
    const MacroResult e = s.expand("secret-text {BOGUS}", "");
    CHECK(e.error.find("secret-text") == std::string::npos && e.error.find("BOGUS") == std::string::npos);
}

void m4SlotsAndSession() {
    ContestSession s;
    std::string err;
    CHECK(!s.begin("NOT-A-CONTEST", "W1AW", err) && !err.empty());
    CHECK(!s.active());
    CHECK(!s.begin("CQ-WW-CW", "W1 AW", err));
    CHECK(!s.begin("CQ-WW-CW", "", err));
    CHECK(s.begin("CQ-WW-CW", "w1aw", err));
    CHECK(s.setSlot(1, "CQ TEST {MYCALL}"));
    CHECK(s.setSlot(2, "{CALL} {RST} {X:ZONE}"));
    CHECK(s.setSlot(12, "TU {MYCALL}"));
    CHECK(!s.setSlot(0, "x"));
    CHECK(!s.setSlot(13, "x"));
    CHECK(!s.setSlot(1, std::string(kMacroTemplateMax + 1, 'a')));
    CHECK(s.slot(1) == "CQ TEST {MYCALL}");
    CHECK(s.slot(13).empty());
    CHECK(s.expandSlot(1, "").text == "CQ TEST W1AW");
    CHECK(!s.expandSlot(2, "K1ABC").ok);       // ZONE not set yet
    CHECK(s.sentTokens().empty());             // and no QSO is invented from a missing value
    CHECK(s.setValue("ZONE", "5"));
    CHECK(s.expandSlot(2, "k1abc").text == "K1ABC 599 5");
    CHECK(s.expandSlot(12, "").text == "TU W1AW");
    CHECK(s.expandSlot(5, "").ok && s.expandSlot(5, "").text.empty());   // an unset slot is an empty text
    CHECK(!s.setValue("", "1") && !s.setValue("Z one", "1") && !s.setValue("ZONE", "a b") && !s.setValue("ZONE", std::string(13, '1')));
    // sentTokens -> a Cabrillo QSO that the writer accepts
    const std::vector<std::string> sent = s.sentTokens();
    CHECK(sent == (std::vector<std::string>{"599", "5"}));
    Qso q;
    q.unixTime = 1000000000;
    q.freqKHz = 14025;
    q.mode = "CW";
    q.myCall = "W1AW";
    q.call = "K1ABC";
    q.sent = sent;
    q.rcvd = {"599", "14"};
    CabrilloHeader h;
    h.contest = "CQ-WW-CW";
    h.callsign = "W1AW";
    const CabrilloResult r = writeCabrillo(h, {q});
    CHECK(r.ok && r.qsoLines == 1);
    // a Sweepstakes session needs all four values
    ContestSession ss;
    CHECK(ss.begin("ARRL-SS-CW", "W1AW", err));
    CHECK(ss.sentTokens().empty());
    ss.setValue("PREC", "A"); ss.setValue("CHECK", "99"); ss.setValue("SECT", "CT");
    CHECK(ss.sentTokens() == (std::vector<std::string>{"1", "A", "99", "CT"}));
}

void m5Boundaries() {
    ContestSession s = session();
    CHECK(s.expand("", "").ok && s.expand("", "").text.empty());
    CHECK(s.expand("{CALL}", "").text.empty());
    s.setNextSerial(99999);
    s.logged();
    CHECK(s.nextSerial() == 99999);   // saturates, does not wrap
    CHECK(s.expand("{NR:5}", "").text == "99999");
    // the output limit: 256-char template of {MYCALL} (8 chars each of "{MYCALL}" -> 4 chars) fits; a template that doubles past 512 is refused
    std::string t;
    while (t.size() + 8 <= kMacroTemplateMax) { t += "{MYCALL}"; }
    const MacroResult fit = s.expand(t, "");
    CHECK(fit.ok && fit.text.size() == (kMacroTemplateMax / 8) * 4);
    std::string long_call(30, 'A');
    std::string many;
    while (many.size() + 6 <= kMacroTemplateMax) { many += "{CALL}"; }
    const MacroResult over = s.expand(many, long_call);   // 42 * 30 = 1260 > 512
    CHECK(!over.ok && over.text.empty());
}

void m6Fuzz() {
    hamops_test::Rng rng(5);
    ContestSession s = session();
    s.setValue("ZONE", "5");
    const std::string alphabet = "{}{}{}ABCMYCALLNR:X0123456789 {{}}";
    for (int i = 0; i < 20000; ++i) {
        std::string t;
        const int n = static_cast<int>(rng.below(120));
        for (int k = 0; k < n; ++k) { t.push_back(alphabet[rng.below(static_cast<std::uint32_t>(alphabet.size()))]); }
        const MacroResult r = s.expand(t, rng.below(2) ? "K1ABC" : "");
        if (r.ok) { CHECK(r.text.size() <= kMacroOutputMax && r.error.empty()); }
        else { CHECK(r.text.empty() && !r.error.empty()); }
    }
}

void m7NoTransmitPath() {
    const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile("src/core/contest_macros.hpp"));
    CHECK(!src.empty());
    for (const char* bad : {"ptt", "PTT", "transmit", "Transmit", "send", "Send", "key(", "Key(", "keyer", "play", "audio", "socket", "engine", "fox_engine", "winsock",
                            "thread", "CreateFile", "fopen", "ofstream", "system("}) {
        CHECK(src.find(bad) == std::string::npos);
    }
    // it includes nothing but the lane's own two headers and the standard library
    const std::string raw = hamops_test::srcFile("src/core/contest_macros.hpp");
    std::size_t pos = 0;
    int ownIncludes = 0;
    while ((pos = raw.find("#include \"", pos)) != std::string::npos) {
        const std::size_t e = raw.find('"', pos + 10);
        const std::string name = raw.substr(pos + 10, e - pos - 10);
        CHECK(name == "cabrillo.hpp" || name == "qso_log.hpp");
        ++ownIncludes;
        pos = e;
    }
    CHECK(ownIncludes == 2);
}

}  // namespace

int main() {
    m1Tokens();
    m2Sequences();
    m3Errors();
    m4SlotsAndSession();
    m5Boundaries();
    m6Fuzz();
    m7NoTransmitPath();
    return testSummary("test_contest_macros");
}
