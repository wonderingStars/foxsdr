// test_adif_qso.cpp - the ADIF 3.1.4 QSO record writer (core/adif_qso.hpp).
//
//   A1  a record: field names, lengths that equal the data length, <EOR>, date/time/band/frequency/mode; USB -> SSB + SUBMODE USB.
//   A2  contest exchange: RST_SENT/RCVD, STX/SRX, STX_STRING/SRX_STRING, CONTEST_ID.
//   A3  refusals (empty record) and boundaries: no usable call/time/frequency, a frequency in no band, non-ASCII text.
//   A4  the header.
//   A5  a re-parse of every record: each <NAME:LEN>data specifier's LEN is exactly the data's length, no name twice (spec IV.A).
//   A6  fuzz.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <map>
#include <string>
#include <vector>

#include "core/adif_qso.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::ham;
using hamops_test::unixFromCivil;

namespace {

// A reader written from the specification's data-specifier rule only: <F:L[:T]>D with D exactly L characters; returns false on any breach.
bool parseRecord(const std::string& rec, std::map<std::string, std::string>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < rec.size()) {
        const std::size_t lt = rec.find('<', i);
        if (lt == std::string::npos) { return rec.find_first_not_of(" \r\n", i) == std::string::npos; }
        const std::size_t gt = rec.find('>', lt);
        if (gt == std::string::npos) { return false; }
        std::string spec = rec.substr(lt + 1, gt - lt - 1);
        std::string up;
        for (char c : spec) { up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c)))); }
        if (up == "EOR") { i = gt + 1; return rec.find_first_not_of(" \r\n", i) == std::string::npos; }
        const std::size_t c1 = spec.find(':');
        if (c1 == std::string::npos) { return false; }
        const std::string name = spec.substr(0, c1);
        std::size_t c2 = spec.find(':', c1 + 1);
        const std::string lenStr = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        if (lenStr.empty() || lenStr.find_first_not_of("0123456789") != std::string::npos) { return false; }
        const std::size_t len = static_cast<std::size_t>(std::stoul(lenStr));
        if (gt + 1 + len > rec.size()) { return false; }
        if (out.count(name) != 0) { return false; }   // no field name twice
        out[name] = rec.substr(gt + 1, len);
        i = gt + 1 + len;
    }
    return false;   // no <EOR>
}

Qso sample() {
    Qso q;
    q.unixTime = unixFromCivil(2000, 11, 26, 7, 11, 5);
    q.freqKHz = 14256.5;
    q.mode = "USB";
    q.myCall = "w1aw";
    q.call = "jt1z";
    return q;
}

void a1Record() {
    const std::string rec = adifQsoRecord(sample());
    std::map<std::string, std::string> f;
    CHECK(parseRecord(rec, f));
    CHECK(f["STATION_CALLSIGN"] == "W1AW");
    CHECK(f["CALL"] == "JT1Z");
    CHECK(f["QSO_DATE"] == "20001126");
    CHECK(f["TIME_ON"] == "071105");
    CHECK(f["BAND"] == "20m");
    CHECK(f["FREQ"] == "14.256500");
    CHECK(f["MODE"] == "SSB");
    CHECK(f["SUBMODE"] == "USB");
    CHECK(rec.size() > 5 && rec.compare(rec.size() - 7, 7, "<EOR>\r\n") == 0);
    CHECK(rec.find("<CALL:4>JT1Z") != std::string::npos);
    Qso c = sample(); c.mode = "CW";
    CHECK(parseRecord(adifQsoRecord(c), f) && f["MODE"] == "CW" && f.count("SUBMODE") == 0);
    c.mode = "LSB"; c.freqKHz = 7100;
    CHECK(parseRecord(adifQsoRecord(c), f) && f["SUBMODE"] == "LSB" && f["BAND"] == "40m");
    c.mode = "NFM"; c.freqKHz = 145500;
    CHECK(parseRecord(adifQsoRecord(c), f) && f["MODE"] == "FM" && f["BAND"] == "2m");
    c.myCall.clear();
    CHECK(parseRecord(adifQsoRecord(c), f) && f.count("STATION_CALLSIGN") == 0);
}

void a2Contest() {
    Qso q = sample();
    q.mode = "CW";
    q.sent = {"599", "7"};
    q.rcvd = {"599", "12"};
    AdifQsoOptions o;
    o.contest = findCabrilloContest("CQ-WPX-CW");
    o.contestId = "CQ-WPX-CW";
    std::map<std::string, std::string> f;
    CHECK(parseRecord(adifQsoRecord(q, o), f));
    CHECK(f["RST_SENT"] == "599" && f["RST_RCVD"] == "599");
    CHECK(f["STX"] == "7" && f["SRX"] == "12");
    CHECK(f["STX_STRING"] == "7" && f["SRX_STRING"] == "12");
    CHECK(f["CONTEST_ID"] == "CQ-WPX-CW");
    // CQ WW: zone is not a serial -> no STX/SRX, the strings carry the zone
    Qso z = sample();
    z.sent = {"59", "05"}; z.rcvd = {"59", "23"};
    o.contest = findCabrilloContest("CQ-WW-SSB"); o.contestId = "CQ-WW-SSB";
    CHECK(parseRecord(adifQsoRecord(z, o), f));
    CHECK(f.count("STX") == 0 && f["STX_STRING"] == "05" && f["SRX_STRING"] == "23" && f["RST_SENT"] == "59");
    // Sweepstakes: serial -> STX, the rest in the string
    Qso s = sample();
    s.sent = {"1", "M", "38", "CT"}; s.rcvd = {"1", "Q", "92", "MI"};
    o.contest = findCabrilloContest("ARRL-SS-CW"); o.contestId = "ARRL-SS-CW";
    CHECK(parseRecord(adifQsoRecord(s, o), f));
    CHECK(f["STX"] == "1" && f["STX_STRING"] == "1 M 38 CT" && f["SRX_STRING"] == "1 Q 92 MI");
    // a wrong token count writes the record without exchange fields
    Qso w = sample(); w.sent = {"599"}; w.rcvd = {"599"};
    o.contest = findCabrilloContest("CQ-WPX-CW");
    CHECK(parseRecord(adifQsoRecord(w, o), f) && f.count("RST_SENT") == 0 && f.count("STX_STRING") == 0);
}

void a3Refusals() {
    Qso q = sample();
    Qso a = q; a.call = "BAD CALL";  CHECK(adifQsoRecord(a).empty());
    Qso b = q; b.call.clear();       CHECK(adifQsoRecord(b).empty());
    Qso c = q; c.unixTime = -1;      CHECK(adifQsoRecord(c).empty());
    Qso d = q; d.freqKHz = 0;        CHECK(adifQsoRecord(d).empty());
    Qso e = q; e.freqKHz = -5;       CHECK(adifQsoRecord(e).empty());
    // a frequency in no band: record written, BAND omitted, FREQ kept
    Qso g = q; g.freqKHz = 5000;     // 5 MHz is between 80 m and 60 m
    std::map<std::string, std::string> f;
    CHECK(parseRecord(adifQsoRecord(g), f) && f.count("BAND") == 0 && f["FREQ"] == "5.000000");
    // band edges
    CHECK(adifBand(14.0) == "20m" && adifBand(14.35) == "20m" && adifBand(14.3501).empty() && adifBand(13.999).empty());
    CHECK(adifBand(1.8) == "160m" && adifBand(29.7) == "10m" && adifBand(50.0) == "6m" && adifBand(144.0) == "2m" && adifBand(223.5) == "1.25m");
    // non-ASCII in the mode text cannot break the length rule
    Qso h = q; h.mode = "R\xC3\xA9";
    CHECK(parseRecord(adifQsoRecord(h), f));
    CHECK(f["MODE"].size() == 3 && f["MODE"][1] == '?');
    // an unknown mode is passed through upper-cased and clipped
    Qso m = q; m.mode = "ft8";
    CHECK(parseRecord(adifQsoRecord(m), f) && f["MODE"] == "FT8");
}

void a4Header() {
    const std::string h = adifHeader("FoxSDR", "0.99.109");
    CHECK(h.find("<ADIF_VER:5>3.1.4") != std::string::npos);
    CHECK(h.find("<PROGRAMID:6>FoxSDR") != std::string::npos);
    CHECK(h.find("<PROGRAMVERSION:8>0.99.109") != std::string::npos);
    CHECK(h.find("<EOH>") != std::string::npos);
    CHECK(h[0] != '<');   // the spec: a header begins with a character other than '<'
    const std::string hh = adifHeader("Fox\xFF" "SDR", "");
    CHECK(hh.find("<PROGRAMID:7>Fox?SDR") != std::string::npos && hh.find("PROGRAMVERSION") == std::string::npos);
}

void a6Fuzz() {
    hamops_test::Rng rng(31);
    const char* modes[] = {"CW", "USB", "LSB", "AM", "FM", "NFM", "RTTY", "FT8", "", "\xC3\xA9\x01"};
    const char* contests[] = {"CQ-WW-CW", "CQ-WPX-SSB", "ARRL-SS-CW"};
    int written = 0;
    for (int it = 0; it < 20000; ++it) {
        Qso q;
        q.unixTime = static_cast<std::int64_t>(rng.next() % 4100000000ull) - 1000;
        q.freqKHz = static_cast<double>(rng.below(300000)) / 3.0 - 100;
        q.mode = modes[rng.below(10)];
        q.call = rng.below(5) == 0 ? "X Y" : "K1ABC/P";
        q.myCall = rng.below(3) == 0 ? "" : "W1AW";
        AdifQsoOptions o;
        o.contest = findCabrilloContest(contests[rng.below(3)]);
        o.contestId = rng.below(2) ? "CQ-WW-CW" : "";
        const std::size_t k = rng.below(6);
        for (std::size_t j = 0; j < k; ++j) {
            std::string s(rng.below(15), 'a' + static_cast<char>(rng.below(26)));
            if (rng.below(3) == 0) { s += "\xC3\xA9"; }
            if (rng.below(4) == 0) { s += "<EOR>"; }
            q.sent.push_back(s); q.rcvd.push_back(s);
        }
        const std::string rec = adifQsoRecord(q, o);
        if (rec.empty()) { continue; }
        ++written;
        std::map<std::string, std::string> f;
        CHECK(parseRecord(rec, f));
        CHECK(f.count("CALL") == 1);
    }
    CHECK(written > 5000);
}

}  // namespace

int main() {
    a1Record();
    a2Contest();
    a3Refusals();
    a4Header();
    a6Fuzz();
    return testSummary("test_adif_qso");
}
