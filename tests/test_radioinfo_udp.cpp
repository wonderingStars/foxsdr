// test_radioinfo_udp.cpp - the RadioInfo UDP broadcast reader (core/radioinfo_udp.hpp, net/radioinfo_listener.hpp): the read-only rig-state source for
// DXLab Commander and N1MM Logger+. Documents are built from the structure on the N1MM page; no logger is involved.
//
//   R1  the documented example and a Commander-style document; units (Freq in 10 Hz); flags; ranges.
//   R2  refusals, each hostile shape: DOCTYPE/entity bombs, numeric references, text outside elements, nesting, unclosed, too long, too many.
//   R3  privacy: the station call, operator call and computer name are not parsed or kept.
//   R4  the table: staleness at its exact boundary, four radios, the SO2V caution of the N1MM page.
//   R5  the listener on 127.0.0.1: OFF by default, wider addresses refused, accepted and refused counted, poll bounded.
//   R6  fuzz: mutated and random documents; every accepted document satisfies the invariants.
//   R7  source scan: no send path, no thread, nothing printed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/radioinfo_udp.hpp"
#include "hamops_util.hpp"
#include "net/radioinfo_listener.hpp"
#include "test_check.hpp"

using namespace cascade::core::rig;

namespace {

// The structure of the N1MM page's RadioInfo example (values illustrative, as on the page; the calls here are made up).
const char* kExample =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<RadioInfo>\n\t<app>N1MM</app>\n\t<StationName>CW-80m</StationName>\n\t<RadioNr>1</RadioNr>\n\t<Freq>352211</Freq>\n"
    "\t<TXFreq>352211</TXFreq>\n\t<Mode>CW</Mode>\n\t<mycall>TE1ST</mycall>\n\t<OpCall>TE1ST</OpCall>\n\t<IsRunning>False</IsRunning>\n\t<FocusEntry>204626</FocusEntry>\n"
    "\t<EntryWindowHwnd>275678</EntryWindowHwnd>\n\t<Antenna>8</Antenna>\n\t<Rotors></Rotors>\n\t<FocusRadioNr>1</FocusRadioNr>\n\t<IsStereo>False</IsStereo>\n"
    "\t<IsSplit>False</IsSplit>\n\t<ActiveRadioNr>1</ActiveRadioNr>\n\t<IsTransmitting>False</IsTransmitting>\n\t<FunctionKeyCaption></FunctionKeyCaption>\n"
    "\t<RadioName></RadioName>\n\t<AuxAntSelected>-1</AuxAntSelected>\n\t<AuxAntSelectedName></AuxAntSelectedName>\n\t<IsConnected></IsConnected>\n</RadioInfo>";

bool parse(const std::string& s, RadioInfo& r, std::string& err) { return parseRadioInfo(s.data(), s.size(), r, err); }
std::string doc(const std::string& body) { return "<RadioInfo>" + body + "</RadioInfo>"; }

void r1Parsing() {
    RadioInfo r;
    std::string err;
    CHECK(parse(kExample, r, err) && err.empty());
    CHECK(r.radioNr == 1 && r.freqHz == 3522110.0 && r.txFreqHz == 3522110.0 && r.mode == "CW" && !r.split && !r.transmitting && !r.running && r.app == "N1MM" && r.activeRadioNr == 1);
    // a Commander-style document with split, radio 2, transmitting
    CHECK(parse(doc("<app>DXLab</app><RadioNr>2</RadioNr><Freq>1407400</Freq><TXFreq>1407600</TXFreq><Mode>USB</Mode><IsSplit>True</IsSplit><IsTransmitting>true</IsTransmitting>"), r, err));
    CHECK(r.radioNr == 2 && r.freqHz == 14074000.0 && r.txFreqHz == 14076000.0 && r.mode == "USB" && r.split && r.transmitting && r.app == "DXLab");
    // minimal
    CHECK(parse(doc("<RadioNr>4</RadioNr>"), r, err) && r.radioNr == 4 && r.freqHz == 0 && r.mode.empty());
    // empty Freq element is "not given"; an empty element tag is skipped
    CHECK(parse(doc("<RadioNr>1</RadioNr><Freq></Freq><Rotors/>"), r, err) && r.freqHz == 0);
    // range edges: 10 Hz units
    CHECK(parse(doc("<RadioNr>1</RadioNr><Freq>600000000</Freq>"), r, err) && r.freqHz == 6.0e9);
    CHECK(parse(doc("<RadioNr>1</RadioNr><Freq>0</Freq>"), r, err) && r.freqHz == 0);
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Freq>600000001</Freq>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Freq>-5</Freq>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Freq>14.2</Freq>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Freq>99999999999999</Freq>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><TXFreq>x</TXFreq>"), r, err));
    // radio number
    for (const char* bad : {"0", "5", "abc", "", "-1", "1.5", "01a"}) { CHECK(!parse(doc(std::string("<RadioNr>") + bad + "</RadioNr>"), r, err)); }
    CHECK(!parse(doc("<Freq>1</Freq>"), r, err));
    // flags
    CHECK(parse(doc("<RadioNr>1</RadioNr><IsSplit>TRUE</IsSplit><IsRunning>False</IsRunning>"), r, err) && r.split && !r.running);
    CHECK(!parse(doc("<RadioNr>1</RadioNr><IsSplit>yes</IsSplit>"), r, err));
    // mode and app text
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Mode>THIS-MODE-IS-LONG</Mode>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><Mode>US B</Mode>"), r, err));
    CHECK(parse(doc("<RadioNr>1</RadioNr><Mode>FT8-DATA</Mode>"), r, err) && r.mode == "FT8-DATA");
    CHECK(!parse(doc("<RadioNr>1</RadioNr><app>a b</app>"), r, err));
    CHECK(!parse(doc("<RadioNr>1</RadioNr><ActiveRadioNr>9</ActiveRadioNr>"), r, err));
    // predefined entities decode
    CHECK(parse(doc("<RadioNr>1</RadioNr><RadioName>A&amp;B</RadioName>"), r, err));
    // surrounding white space is fine; the declaration is optional
    CHECK(parse("  \r\n" + doc("<RadioNr>3</RadioNr>") + "\r\n\r\n", r, err) && r.radioNr == 3);
}

void r2Hostile() {
    RadioInfo r;
    std::string err;
    auto bad = [&](const std::string& s) { const bool ok = parse(s, r, err); return !ok && !err.empty(); };
    CHECK(bad(""));
    CHECK(!parseRadioInfo(nullptr, 5, r, err));
    CHECK(bad(std::string(5000, 'a')));
    CHECK(bad(doc("<RadioNr>1</RadioNr>") + std::string(4100, ' ')));
    CHECK(bad(std::string("<RadioInfo><RadioNr>1</RadioNr>\0</RadioInfo>", 41)));
    CHECK(bad("<Other><RadioNr>1</RadioNr></Other>"));
    CHECK(bad("<radioinfo><RadioNr>1</RadioNr></radioinfo>"));
    CHECK(bad("<RadioInfo><RadioNr>1</RadioNr>"));                                     // not closed
    CHECK(bad("<RadioInfo><RadioNr>1</RadioNr></RadioInfo> trailing"));
    CHECK(bad("<RadioInfo>text<RadioNr>1</RadioNr></RadioInfo>"));                     // text outside an element
    CHECK(bad(doc("<RadioNr>1</RadioNr><Freq>1")));                                    // unterminated
    CHECK(bad(doc("<RadioNr>1</RadioNr><Freq>1</Mode>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><A><B>1</B></A>")));                           // nested
    CHECK(bad(doc("<RadioNr>1</RadioNr><Freq")));                                      // a tag never closed
    // entity bombs and declarations
    CHECK(bad("<!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;\">]><RadioInfo><RadioNr>1</RadioNr></RadioInfo>"));
    CHECK(bad("<RadioInfo><!DOCTYPE x><RadioNr>1</RadioNr></RadioInfo>"));
    CHECK(bad("<RadioInfo><!ENTITY x \"y\"><RadioNr>1</RadioNr></RadioInfo>"));
    CHECK(bad("<RadioInfo><!-- c --><RadioNr>1</RadioNr></RadioInfo>"));
    CHECK(bad("<RadioInfo><?pi x?><RadioNr>1</RadioNr></RadioInfo>"));
    CHECK(bad(doc("<RadioNr>1</RadioNr><RadioName>&lol;&lol;&lol;</RadioName>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><RadioName>&#65;</RadioName>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><RadioName>&amp</RadioName>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><RadioName>&verylongentity;</RadioName>")));
    CHECK(bad("<?xml version=\"1.0\"<RadioInfo><RadioNr>1</RadioNr></RadioInfo>"));   // declaration never closed... closes at no "?>"
    // too many elements and element text too long
    std::string many;
    for (int i = 0; i < 70; ++i) { many += "<E" + std::to_string(i) + ">x</E" + std::to_string(i) + ">"; }
    CHECK(bad(doc("<RadioNr>1</RadioNr>" + many)));
    CHECK(bad(doc("<RadioNr>1</RadioNr><RadioName>" + std::string(65, 'a') + "</RadioName>")));
    CHECK(parse(doc("<RadioNr>1</RadioNr><RadioName>" + std::string(64, 'a') + "</RadioName>"), r, err));
    // bad element names
    CHECK(bad(doc("<RadioNr>1</RadioNr><a b>1</a b>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><>1</>")));
    CHECK(bad(doc("<RadioNr>1</RadioNr><" + std::string(33, 'a') + ">1</" + std::string(33, 'a') + ">")));
    // a closing tag of the root appearing early ends the document: the rest is trailing data
    CHECK(bad("<RadioInfo><RadioNr>1</RadioNr></RadioInfo><RadioInfo><RadioNr>2</RadioNr></RadioInfo>"));
    // a failed parse leaves a clean (default) struct
    CHECK(!parse("junk", r, err) && r.radioNr == 0 && r.freqHz == 0);
}

void r3Privacy() {
    const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile("src/core/radioinfo_udp.hpp"));
    CHECK(!src.empty());
    // the document's identity fields are in no struct and no read path of the code (strings were stripped, so a mention in a field list would show)
    for (const char* name : {"mycall", "OpCall", "StationName", "callsign", "Callsign"}) { CHECK(src.find(name) == std::string::npos); }
    RadioInfo r;
    std::string err;
    CHECK(parse(kExample, r, err));
    // nothing of the identity survives in the parsed result
    const std::string joined = r.mode + "|" + r.app;
    CHECK(joined.find("TE1ST") == std::string::npos && joined.find("CW-80m") == std::string::npos);
}

void r4Table() {
    RadioInfoTable t(30);
    RadioInfo r, out;
    r.radioNr = 2;
    r.freqHz = 7100000;
    t.update(r, 1000);
    CHECK(t.get(2, 1000, out) && out.freqHz == 7100000);
    CHECK(t.get(2, 1030, out));                      // exactly 30 s: kept
    CHECK(!t.get(2, 1031, out));                     // 31 s: stale
    CHECK(!t.get(1, 1000, out) && !t.get(0, 1000, out) && !t.get(5, 1000, out) && !t.get(-1, 1000, out));
    RadioInfo bad;
    bad.radioNr = 9;
    t.update(bad, 1000);
    bad.radioNr = 0;
    t.update(bad, 1000);
    for (int i = 1; i <= 4; ++i) { RadioInfo x; x.radioNr = i; x.freqHz = 1000.0 * i; t.update(x, 2000); }
    for (int i = 1; i <= 4; ++i) { CHECK(t.get(i, 2000, out) && out.freqHz == 1000.0 * i); }
    // a newer packet replaces the older
    r.freqHz = 7101000;
    t.update(r, 2005);
    CHECK(t.get(2, 2005, out) && out.freqHz == 7101000);
    RadioInfoTable zero(0);
    zero.update(r, 5);
    CHECK(zero.get(2, 6, out) && !zero.get(2, 7, out));       // a stale limit under 1 s is taken as 1 s
    // the SO2V caution: TXFreq is the transmit frequency only in a packet of the active radio, or in split
    RadioInfo a;
    a.radioNr = 1; a.activeRadioNr = 1; a.freqHz = 7000000; a.txFreqHz = 7000500;
    CHECK(RadioInfoTable::effectiveTxHz(a) == 7000500);       // active radio
    a.activeRadioNr = 2;
    CHECK(RadioInfoTable::effectiveTxHz(a) == 7000000);       // another radio is active: the receive frequency
    a.split = true;
    CHECK(RadioInfoTable::effectiveTxHz(a) == 7000500);       // except in split
    a.split = false; a.activeRadioNr = 0;
    CHECK(RadioInfoTable::effectiveTxHz(a) == 7000500);       // no active radio given (a Commander packet)
    a.txFreqHz = 0;
    CHECK(RadioInfoTable::effectiveTxHz(a) == 7000000);
}

void r5Listener() {
    using namespace cascade::net;
    RadioInfoListener L;
    std::string err;
    CHECK(!RadioInfoListenerConfig().enabled);
    CHECK(!L.start(RadioInfoListenerConfig(), err));
    RadioInfoListenerConfig c;
    c.enabled = true;
    c.host = "0.0.0.0";
    CHECK(!L.start(c, err));
    c.host = "10.0.0.5";
    CHECK(!L.start(c, err));
    c.host = "127.0.0.1";
    c.port = -1;
    CHECK(!L.start(c, err));
    CHECK(!L.running());
    RadioInfoTable table;
    CHECK(L.poll(table, 1) == 0);
    c.port = 0;
    CHECK(L.start(c, err) && L.running() && L.port() > 0);
    const lo::Sock tx = lo::udpOpen(lo::kLoopbackHost, -1);
    const std::string good = doc("<app>DXLab</app><RadioNr>1</RadioNr><Freq>1407400</Freq><Mode>CW</Mode>");
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), good.data(), good.size());
    const std::string junk = "not xml";
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), junk.data(), junk.size());
    const std::string huge(5000, 'x');
    lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), huge.data(), huge.size());
    int total = 0;
    for (int i = 0; i < 500 && total < 3; ++i) { total += L.poll(table, 50); if (total < 3) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); } }
    CHECK(total == 3 && L.accepted() == 1 && L.refused() == 2);
    RadioInfo out;
    CHECK(table.get(1, 50, out) && out.freqHz == 14074000 && out.mode == "CW");
    // bounded poll
    for (int i = 0; i < 10; ++i) { lo::udpSendTo(tx, lo::kLoopbackHost, L.port(), good.data(), good.size()); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(L.poll(table, 51, 3) == 3);
    int rest = 0;
    for (int i = 0; i < 200 && rest < 7; ++i) { rest += L.poll(table, 51, 64); if (rest < 7) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); } }
    CHECK(rest == 7);
    L.stop();
    CHECK(!L.running() && L.poll(table, 52) == 0);
    lo::closeSock(tx);
}

void r6Fuzz() {
    hamops_test::Rng rng(55);
    const std::string seed = kExample;
    int accepted = 0;
    for (int it = 0; it < 40000; ++it) {
        std::string s;
        const int mode = static_cast<int>(rng.below(3));
        if (mode == 0) {
            s.resize(rng.below(200));
            for (auto& c : s) { c = static_cast<char>(rng.below(256)); }
        } else {
            s = seed;
            const int muts = 1 + static_cast<int>(rng.below(2));
            for (int k = 0; k < muts && !s.empty(); ++k) {
                const std::size_t pos = rng.below(static_cast<std::uint32_t>(s.size()));
                const int op = static_cast<int>(rng.below(4));
                const char ch = "<>&/;=\"! ?0123456789abcFT"[rng.below(25)];
                if (op == 0) { s[pos] = ch; }
                else if (op == 1) { s.erase(pos, 1 + rng.below(8)); }
                else if (op == 2) { s.insert(pos, 1, ch); }
                else { s.insert(pos, "&amp;"); }
            }
            if (mode == 2) { s.resize(rng.below(static_cast<std::uint32_t>(s.size()) + 1)); }
        }
        RadioInfo r;
        std::string err;
        if (!parse(s, r, err)) { CHECK(r.radioNr == 0 && !err.empty()); continue; }
        ++accepted;
        CHECK(r.radioNr >= 1 && r.radioNr <= 4);
        CHECK(r.freqHz >= 0 && r.freqHz <= 6.0e9 && r.txFreqHz >= 0 && r.txFreqHz <= 6.0e9);
        CHECK(r.mode.size() <= 12 && r.app.size() <= 16 && r.activeRadioNr >= 0 && r.activeRadioNr <= 4);
        for (char c : r.mode) { CHECK(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '+'); }
    }
    CHECK(accepted > 200);   // most mutations hit a tag or a number and are refused; the ones in free text are accepted
}

void r7SourceScan() {
    for (const char* f : {"src/core/radioinfo_udp.hpp", "src/net/radioinfo_listener.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(f));
        CHECK(!src.empty());
        for (const char* bad : {"std::thread", "std::async", "stderr", "stdout", "std::cerr", "std::cout", "settings_set", "INADDR_ANY", "getaddrinfo", "udpSendTo", "sendto", "tcpSend", "OutputDebugString"}) {
            CHECK(src.find(bad) == std::string::npos);
        }
    }
    CHECK(hamops_test::srcFile("src/net/radioinfo_listener.hpp").find("bool enabled = false;") != std::string::npos);
}

}  // namespace

int main() {
    r1Parsing();
    r2Hostile();
    r3Privacy();
    r4Table();
    r5Listener();
    r6Fuzz();
    r7SourceScan();
    return testSummary("test_radioinfo_udp");
}
