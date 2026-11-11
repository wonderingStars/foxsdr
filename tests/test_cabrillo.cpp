// test_cabrillo.cpp - the Cabrillo v3 writer (core/cabrillo.hpp, core/qso_log.hpp).
//
//   C1  header and trailer: first line START-OF-LOG: 3.0, last END-OF-LOG:, CR LF; the five always-present tags.
//   C2  the QSO line: columns of the template (call at 31, received call at 56, transmitter at 81), frequency, mode, UTC date and time.
//   C3  chronological order; truncation; per contest field tables (CQ WW, CQ WPX, Sweepstakes).
//   C4  refusals: bad call, mode without a code, bad exchange token, wrong field count, no frequency, negative time: skipped, counted, the
//       message never carries the call.
//   C5  header validation against the specification's value lists; nothing is written for a refused header.
//   C6  boundaries: empty log, 23:59:59, leap day, the epoch, the OPERATORS wrap at 75.
//   C7  file writing; a path that cannot be written.
//   C8  fuzz: random headers and QSOs never crash and always produce a well-formed file.
//   C9  source scan: nothing to stdout/stderr, no setting write.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/cabrillo.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::ham;
using hamops_test::unixFromCivil;

namespace {

CabrilloHeader goodHeader(const std::string& contest) {
    CabrilloHeader h;
    h.contest = contest;
    h.callsign = "W1AW";
    h.categoryOperator = "SINGLE-OP";
    h.categoryBand = "ALL";
    h.categoryPower = "LOW";
    h.categoryMode = "CW";
    h.categoryAssisted = "NON-ASSISTED";
    h.location = "CT";
    return h;
}

Qso cqww(int h, int m, const char* call, const char* zoneRcvd) {
    Qso q;
    q.unixTime = unixFromCivil(2000, 11, 26, h, m, 0);
    q.freqKHz = 14256;
    q.mode = "USB";
    q.myCall = "W1AW";
    q.call = call;
    q.sent = {"59", "05"};
    q.rcvd = {"59", zoneRcvd};
    return q;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> v;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t e = text.find("\r\n", i);
        if (e == std::string::npos) { v.push_back(text.substr(i)); break; }
        v.push_back(text.substr(i, e - i));
        i = e + 2;
    }
    return v;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ' ') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else { cur.push_back(c); }
    }
    if (!cur.empty()) { v.push_back(cur); }
    return v;
}

bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::string lastQsoLine(const std::string& text) {
    std::string line;
    for (const auto& l : lines(text)) { if (l.rfind("QSO:", 0) == 0) { line = l; } }
    return line;
}

void c1Header() {
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-CW"), {cqww(7, 11, "JT1Z", "23")});
    CHECK(r.ok);
    const auto L = lines(r.text);
    CHECK(L.size() >= 4);
    CHECK(L.front() == "START-OF-LOG: 3.0");
    CHECK(L.back() == "END-OF-LOG:");
    CHECK(r.text.size() > 2 && r.text.compare(r.text.size() - 2, 2, "\r\n") == 0);   // ends with CR LF
    CHECK(contains(r.text, "CONTEST: CQ-WW-CW\r\n"));
    CHECK(contains(r.text, "CALLSIGN: W1AW\r\n"));
    CHECK(contains(r.text, "CATEGORY-POWER: LOW\r\n"));
    CHECK(contains(r.text, "LOCATION: CT\r\n"));
    // present even when empty (notes page)
    CHECK(contains(r.text, "\r\nNAME:\r\n"));
    CHECK(contains(r.text, "\r\nADDRESS:\r\n"));
    CHECK(contains(r.text, "\r\nCLUB:\r\n"));
    CHECK(contains(r.text, "\r\nOPERATORS:\r\n"));
    CHECK(contains(r.text, "\r\nSOAPBOX:\r\n"));
    CHECK(contains(r.text, "CREATED-BY: FoxSDR\r\n"));
    // no bare LF anywhere
    for (std::size_t i = 0; i < r.text.size(); ++i) {
        if (r.text[i] == '\n') { CHECK(i > 0 && r.text[i - 1] == '\r'); }
    }
    // a different line end on request
    CabrilloResult u = writeCabrillo(goodHeader("CQ-WW-CW"), {}, "\n");
    CHECK(u.ok && u.text.find('\r') == std::string::npos);
    // claimed score, club, name, address
    CabrilloHeader h = goodHeader("CQ-WW-CW");
    h.claimedScore = 19113;
    h.club = "Test Club";
    h.name = "A Name";
    h.address = {"1 Street", "Town"};
    h.soapbox = {"fun", "more fun"};
    h.operators = {"W1AW", "@W1HQ"};
    r = writeCabrillo(h, {});
    CHECK(contains(r.text, "CLAIMED-SCORE: 19113\r\n"));
    CHECK(contains(r.text, "CLUB: Test Club\r\n"));
    CHECK(contains(r.text, "NAME: A Name\r\n"));
    CHECK(contains(r.text, "ADDRESS: 1 Street\r\nADDRESS: Town\r\n"));
    CHECK(contains(r.text, "SOAPBOX: fun\r\nSOAPBOX: more fun\r\n"));
    CHECK(contains(r.text, "OPERATORS: W1AW @W1HQ\r\n"));
}

void c2QsoLine() {
    Qso q = cqww(7, 11, "jt1z", "23");
    q.transmitter = 1;
    q.freqKHz = 3799;
    q.mode = "LSB";
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-SSB"), {q});
    CHECK(r.ok && r.qsoLines == 1);
    const std::string line = lastQsoLine(r.text);
    CHECK(!line.empty());
    CHECK(line == "QSO:  3799 PH 2000-11-26 0711 W1AW          59  05     JT1Z          59  23     1");
    // spec ruler columns (1-based): freq 6, mode 12, date 15, time 26, call 31, rst 45, exch 49, rcvd call 56, rst 70, exch 74, t 81
    CHECK(line.substr(5, 5) == " 3799");
    CHECK(line.substr(11, 2) == "PH");
    CHECK(line.substr(14, 10) == "2000-11-26");
    CHECK(line.substr(25, 4) == "0711");
    CHECK(line.substr(30, 4) == "W1AW");
    CHECK(line.substr(44, 2) == "59");
    CHECK(line.substr(48, 2) == "05");
    CHECK(line.substr(55, 4) == "JT1Z");
    CHECK(line.substr(69, 2) == "59");
    CHECK(line.substr(73, 2) == "23");
    CHECK(line.size() == 81 && line[80] == '1');
    // no transmitter: no trailing column
    q.transmitter = -1;
    r = writeCabrillo(goodHeader("CQ-WW-SSB"), {q});
    const std::string line2 = lastQsoLine(r.text);
    CHECK(line2.size() == 75 && line2.back() == '3');
    // mode codes
    struct M { const char* fox; const char* code; } modes[] = {{"CW", "CW"}, {"USB", "PH"}, {"AM", "PH"}, {"NFM", "FM"}, {"RTTY", "RY"}, {"FT8", "DG"}};
    for (const M& m : modes) { CHECK(cabrilloMode(m.fox) == m.code); }
    CHECK(cabrilloMode("WFM").empty());
    CHECK(cabrilloMode("").empty());
}

void c3OrderAndContests() {
    // given out of order, written in order; equal times keep input order
    std::vector<Qso> in = {cqww(9, 0, "AA1AA", "05"), cqww(7, 0, "BB2BB", "05"), cqww(7, 0, "CC3CC", "05"), cqww(8, 0, "DD4DD", "05")};
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-CW"), in);
    std::vector<std::string> order;
    // words: QSO: freq mo date time mycall rst zone call ...  -> index 8 is the call
    for (const auto& l : lines(r.text)) { if (l.rfind("QSO:", 0) == 0) { order.push_back(words(l)[8]); } }
    CHECK(order == (std::vector<std::string>{"BB2BB", "CC3CC", "DD4DD", "AA1AA"}));

    // truncation to the field width: zone 6 wide
    Qso t = cqww(7, 0, "JT1Z", "1234567890");
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {t});
    CHECK(r.qsoLines == 1);
    CHECK(words(lastQsoLine(r.text)).back() == "123456");

    // WPX: RST + serial
    Qso w;
    w.unixTime = unixFromCivil(2009, 5, 30, 0, 2, 0);
    w.freqKHz = 7005;
    w.mode = "CW";
    w.myCall = "AA1ZZZ";
    w.call = "S50A";
    w.sent = {"599", "1"};
    w.rcvd = {"599", "4"};
    CabrilloHeader hw = goodHeader("CQ-WPX-CW");
    hw.callsign = "AA1ZZZ";
    r = writeCabrillo(hw, {w});
    CHECK(words(lastQsoLine(r.text)) ==
          (std::vector<std::string>{"QSO:", "7005", "CW", "2009-05-30", "0002", "AA1ZZZ", "599", "1", "S50A", "599", "4"}));

    // Sweepstakes: serial, precedence, check, section; no RST
    Qso s;
    s.unixTime = unixFromCivil(2009, 11, 7, 21, 0, 0);
    s.freqKHz = 14000;
    s.mode = "CW";
    s.myCall = "W1AW";
    s.call = "K8MM";
    s.sent = {"1", "M", "38", "CT"};
    s.rcvd = {"1", "Q", "92", "MI"};
    r = writeCabrillo(goodHeader("ARRL-SS-CW"), {s});
    CHECK(words(lastQsoLine(r.text)) ==
          (std::vector<std::string>{"QSO:", "14000", "CW", "2009-11-07", "2100", "W1AW", "1", "M", "38", "CT", "K8MM", "1", "Q", "92", "MI"}));
    // the table
    CHECK(cabrilloContests().size() == 6);
    CHECK(findCabrilloContest("CQ-WW-SSB") != nullptr);
    CHECK(findCabrilloContest("NOPE") == nullptr);
}

void c4Refusals() {
    Qso good = cqww(7, 0, "JT1Z", "23");
    Qso badCall = good;  badCall.call = "J T1Z";
    Qso emptyCall = good; emptyCall.call.clear();
    Qso badMode = good;  badMode.mode = "WFM";
    Qso badTok = good;   badTok.rcvd[1] = "2X";             // a zone is a number
    Qso fewTok = good;   fewTok.sent.pop_back();
    Qso noFreq = good;   noFreq.freqKHz = 0;
    Qso negTime = good;  negTime.unixTime = -5;
    Qso badMine = good;  badMine.myCall = "W1 AW";
    Qso badRst = good;   badRst.sent[0] = "5";
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-CW"), {badCall, good, emptyCall, badMode, badTok, fewTok, noFreq, negTime, badMine, badRst});
    CHECK(r.ok);
    CHECK(r.qsoLines == 1);
    CHECK(r.skipped == 9);
    CHECK(r.problems.size() == 9);
    for (const auto& p : r.problems) {
        CHECK(!contains(p, "JT1Z") && !contains(p, "J T1Z") && !contains(p, "W1AW"));   // never the call
        CHECK(contains(p, "QSO "));
    }
    // a lower-case call is accepted and written in capitals
    Qso lower = good;
    lower.call = "jt1z";
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {lower});
    CHECK(contains(r.text, " JT1Z "));
    // when the QSO's own call is empty the header call is used
    Qso noMine = good;  noMine.myCall.clear();
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {noMine});
    CHECK(r.qsoLines == 1 && contains(r.text, " W1AW "));
}

void c5HeaderValidation() {
    auto bad = [](CabrilloHeader h) { CabrilloResult r = writeCabrillo(h, {}); return !r.ok && r.text.empty() && !r.problems.empty(); };
    CabrilloHeader h = goodHeader("CQ-WW-CW");
    CHECK(writeCabrillo(h, {}).ok);
    { CabrilloHeader x = h; x.categoryBand = "21M"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryPower = "MEDIUM"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryMode = "PH"; CHECK(bad(x)); }   // the spec says SSB, not PH
    { CabrilloHeader x = h; x.categoryOperator = "SINGLE"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryAssisted = "YES"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryStation = "HOME"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryTime = "10-HOURS"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryTransmitter = "THREE"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.categoryOverlay = "OLD"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.contest = "cq-ww-cw"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.contest = std::string(33, 'A'); CHECK(bad(x)); }
    { CabrilloHeader x = h; x.contest.clear(); CHECK(bad(x)); }
    { CabrilloHeader x = h; x.contest = "NOT-A-CONTEST"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.callsign.clear(); CHECK(bad(x)); }
    { CabrilloHeader x = h; x.callsign = "W1 AW"; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.claimedScore = -7; CHECK(bad(x)); }
    { CabrilloHeader x = h; x.name = std::string(76, 'n'); CHECK(bad(x)); }
    { CabrilloHeader x = h; x.address = std::vector<std::string>(7, "a"); CHECK(bad(x)); }
    { CabrilloHeader x = h; x.address = {std::string(46, 'a')}; CHECK(bad(x)); }
    // every listed band value is accepted
    for (const char* b : {"ALL", "160M", "80M", "40M", "20M", "15M", "10M", "6M", "4M", "2M", "222", "432", "902", "1.2G", "Light", "VHF-3-BAND", "VHF-FM-ONLY"}) {
        CabrilloHeader x = h; x.categoryBand = b; CHECK(writeCabrillo(x, {}).ok);
    }
    // a line break in free text cannot add a tag line
    CabrilloHeader x = h;
    x.club = "Club\r\nCLAIMED-SCORE: 999999";
    CabrilloResult r = writeCabrillo(x, {});
    CHECK(r.ok);
    CHECK(!contains(r.text, "\r\nCLAIMED-SCORE"));
    CHECK(contains(r.text, "CLUB: Club  CLAIMED-SCORE: 999999\r\n"));
    // problems of a refused header name no personal data (the callsign is not echoed)
    CabrilloHeader y = h; y.callsign = "W1 AW"; y.categoryBand = "21M";
    for (const auto& p : writeCabrillo(y, {}).problems) { CHECK(!contains(p, "W1 AW")); }
}

void c6Boundaries() {
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-CW"), {});
    CHECK(r.ok && r.qsoLines == 0);
    CHECK(lines(r.text).size() >= 3);
    // 23:59:59 -> 2359; midnight -> 0000
    Qso q = cqww(0, 0, "JT1Z", "23");
    q.unixTime = unixFromCivil(2024, 12, 31, 23, 59, 59);
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {q});
    CHECK(contains(r.text, " 2024-12-31 2359 "));
    q.unixTime = unixFromCivil(2025, 1, 1, 0, 0, 0);
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {q});
    CHECK(contains(r.text, " 2025-01-01 0000 "));
    // leap day and the epoch
    q.unixTime = unixFromCivil(2024, 2, 29, 12, 30, 0);
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {q});
    CHECK(contains(r.text, " 2024-02-29 1230 "));
    q.unixTime = 0;
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {q});
    CHECK(contains(r.text, " 1970-01-01 0000 "));
    CHECK(utcFromUnix(unixFromCivil(1999, 3, 6, 7, 11, 0)).year == 1999);
    CHECK(utcFromUnix(-1).year == 1969 && utcFromUnix(-1).second == 59);
    // OPERATORS wrap: no line over 75 characters, none lost
    CabrilloHeader h = goodHeader("CQ-WW-CW");
    for (int i = 0; i < 20; ++i) { h.operators.push_back("W1AB" + std::to_string(i)); }
    r = writeCabrillo(h, {});
    int opLines = 0;
    std::string joined;
    for (const auto& l : lines(r.text)) {
        if (l.rfind("OPERATORS:", 0) == 0) { ++opLines; CHECK(l.size() <= 75); joined += l.substr(10) + " "; }
    }
    CHECK(opLines >= 2);
    for (int i = 0; i < 20; ++i) { CHECK(contains(joined, " W1AB" + std::to_string(i) + " ")); }
    // frequency rounds to whole kHz; 5 wide
    Qso f = cqww(7, 0, "JT1Z", "23");
    f.freqKHz = 14025.6;
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {f});
    CHECK(contains(r.text, "QSO: 14026 "));
    f.freqKHz = 1840.2;
    r = writeCabrillo(goodHeader("CQ-WW-CW"), {f});
    CHECK(contains(r.text, "QSO:  1840 "));
}

void c7Files() {
    CabrilloResult r = writeCabrillo(goodHeader("CQ-WW-CW"), {cqww(7, 11, "JT1Z", "23")});
    const char* path = "hamops-cabrillo-test.log";
    CHECK(writeCabrilloFile(path, r.text));
    CHECK(hamops_test::readFile(path) == r.text);   // binary-exact: CR LF kept
    std::remove(path);
    CHECK(!writeCabrilloFile("no-such-folder-hamops/x/y.log", r.text));
}

void c8Fuzz() {
    hamops_test::Rng rng(77);
    const char* contests[] = {"CQ-WW-CW", "CQ-WW-SSB", "CQ-WPX-CW", "CQ-WPX-SSB", "ARRL-SS-CW", "ARRL-SS-SSB", "BOGUS", ""};
    const char* modes[] = {"CW", "USB", "LSB", "AM", "FM", "WFM", "RTTY", "FT8", "", "??"};
    const std::string chars = "ABCZ019/ -_\r\n\t{}<>*abcz";
    auto rs = [&](int maxLen) {
        std::string s;
        const int n = static_cast<int>(rng.below(static_cast<std::uint32_t>(maxLen + 1)));
        for (int i = 0; i < n; ++i) { s.push_back(chars[rng.below(static_cast<std::uint32_t>(chars.size()))]); }
        return s;
    };
    int okFiles = 0;
    for (int it = 0; it < 3000; ++it) {
        CabrilloHeader h;
        h.contest = contests[rng.below(8)];
        h.callsign = rng.below(4) == 0 ? rs(8) : "W1AW";
        h.club = rs(100);
        h.name = rs(100);
        h.soapbox = {rs(120)};
        h.address = {rs(60)};
        h.categoryBand = rng.below(3) == 0 ? rs(5) : "ALL";
        std::vector<Qso> v;
        const int n = static_cast<int>(rng.below(6));
        for (int i = 0; i < n; ++i) {
            Qso q;
            q.unixTime = static_cast<std::int64_t>(rng.next() % 4000000000ull) - (rng.below(10) == 0 ? 100 : 0);
            q.freqKHz = static_cast<double>(rng.below(60000)) / (1 + rng.below(3));
            q.mode = modes[rng.below(10)];
            q.call = rng.below(3) == 0 ? rs(10) : "K1ABC";
            const std::size_t k = rng.below(5);
            for (std::size_t j = 0; j < k; ++j) { q.sent.push_back(rs(14)); q.rcvd.push_back(rs(14)); }
            q.transmitter = static_cast<int>(rng.below(4)) - 1;
            v.push_back(q);
        }
        CabrilloResult r = writeCabrillo(h, v);
        if (!r.ok) { CHECK(r.text.empty()); continue; }
        ++okFiles;
        const auto L = lines(r.text);
        CHECK(L.front() == "START-OF-LOG: 3.0");
        CHECK(!L.empty() && L.back() == "END-OF-LOG:");
        int q = 0;
        for (const auto& l : L) {
            if (l.rfind("QSO:", 0) == 0) {
                ++q;
                CHECK(words(l).size() >= 9);
                CHECK(l.find('\t') == std::string::npos);
            }
        }
        CHECK(q == r.qsoLines);
        CHECK(r.qsoLines + r.skipped == n);
    }
    CHECK(okFiles > 200);
}

void c9SourceScan() {
    for (const char* f : {"src/core/cabrillo.hpp", "src/core/qso_log.hpp", "src/core/contest_macros.hpp", "src/core/adif_qso.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(f));
        CHECK(!src.empty());
        for (const char* bad : {"stderr", "stdout", "std::cerr", "std::cout", "settings_set", "OutputDebugString", "puts("}) {
            CHECK(src.find(bad) == std::string::npos);
        }
    }
}

}  // namespace

int main() {
    c1Header();
    c2QsoLine();
    c3OrderAndContests();
    c4Refusals();
    c5HeaderValidation();
    c6Boundaries();
    c7Files();
    c8Fuzz();
    c9SourceScan();
    return testSummary("test_cabrillo");
}
